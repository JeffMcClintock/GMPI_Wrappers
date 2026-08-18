#pragma once

// WASAPI audio for the Windows standalone.
//
// Shared mode, event-driven. Shared rather than exclusive because a standalone
// is something you leave open next to a DAW and a browser: taking exclusive
// ownership of the soundcard would silence everything else on the machine, to
// buy a latency figure nobody auditioning a plugin is measuring. Event-driven
// rather than polled because it is the only shared-mode arrangement where the
// engine tells us when it wants audio, instead of us guessing with a sleep.
//
// SHARED MODE ALSO FIXES THE SAMPLE RATE, and this driver accepts that rather
// than working around it. The engine is clocked at the endpoint's mix format -
// the "Default Format" on the endpoint's Advanced page in Sound settings - and
// the only way a shared-mode client reaches another rate is to ask the engine
// to resample, which is what AudioMidiDevices.h rules out. So sampleRates()
// reports what the endpoint will genuinely take (normally the mix rate, and
// nothing else), and a user who wants 44100 changes it in Sound settings, where
// it is one setting rather than a converter in every application.
//
// COM APARTMENTS. Every WASAPI object this class touches is created ON the
// thread that uses it, and no object is ever touched from an apartment other
// than the one it was created in. For the STREAM that means each device thread
// activates its own client and is an MTA of its own: activating an IAudioClient
// on the UI thread (an STA, because the window needs OLE) and then calling it
// from the render thread is a cross-apartment call that either marshals -
// putting the message loop in the path of every audio buffer - or misbehaves.
// So open() starts the thread and waits for it to report back, rather than
// doing the work itself and handing the result over.
//
// devices() and sampleRates() are the exceptions to the THREAD and not to the
// rule: both are main-thread-only per AudioMidiDevices.h, so they run in the
// app's STA, and both activate, use and release everything they touch inside
// the one call. Nothing they create outlives the call that made it, so there is
// no second apartment for it to be reached from.
//
// Duplex is two streams with a ring buffer between them, matching
// AudioDriverPipeWire: WASAPI's render and capture clients are separate objects
// with separate clocks and separate event handles, and only the RENDER side
// drives the plugin. Capture opens only when the plugin has audio input pins,
// so a synth still runs on a machine with no microphone.

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../AudioMidiDevices.h"

namespace gmpi
{
namespace standalone
{

class AudioDriverWasapi : public AudioDriver
{
public:
    AudioDriverWasapi();
    ~AudioDriverWasapi() override;

    const char* name() const override { return "WASAPI"; }

    // Unlike an endpoint id this stays meaningful when the user changes their
    // default device or unplugs the interface the saved id named.
    //
    // Spelled as a constant as well as the virtual, which the CoreAudio and
    // PipeWire drivers have no reason to do: openEndpoint() in the .cpp is a
    // free helper in an anonymous namespace with no driver to ask, and handing
    // it one would mean threading `this` through a function whose whole job is
    // to turn an id into an IMMDevice. Everything outside this file's own
    // helpers asks the virtual.
    static constexpr const char* kDefaultDeviceId = "wasapi:default";
    const char* defaultDeviceId() const override { return kDefaultDeviceId; }

    std::vector<DeviceInfo> devices() override;
    std::vector<int> sampleRates(const std::string& deviceId) override;

    bool open(const std::string& deviceId,
              int requestedSampleRate,
              int requestedBufferFrames,
              int inChannels,
              int outChannels,
              AudioCallback* client) override;

    void close() override;

    int getSampleRate()   const override { return activeSampleRate_; }
    int getBufferFrames() const override { return activeBufferFrames_; }

    std::string lastError() const override { return lastError_; }
    std::string lastWarning() const override { return warning_; }

private:
    // WASAPI's headers stay out of this one. Everything COM lives in the .cpp,
    // in a pimpl the two threads share.
    struct Wasapi;

    void renderThread();
    void captureThread();

    // Planar plugin output -> interleaved device buffer, applying whatever
    // channel mapping the negotiated format forced on us. Separate from the
    // render loop because the fallback path (device would not take the plugin's
    // channel count) is the only reason it is not a memcpy.
    void interleaveOut(float* deviceBuffer, int frames) const;

    // Capture -> render hand-off. One writer (the capture thread), one reader
    // (the render thread). Underruns feed silence rather than stale audio - a
    // gap is honest, a loop is not.
    void ringWrite(const float* interleaved, uint32_t frames, int srcChannels);
    void ringRead(float* const* planarOut, int outChannels, uint32_t frames);

    std::unique_ptr<Wasapi> w_;

    AudioCallback* client_{};

    std::thread renderThread_;
    std::thread captureThread_;

    std::vector<float> ring_;
    // Atomic because it is the PUBLICATION of ring_ itself: the capture thread
    // sizes the buffer while the render loop is already running, and a
    // release-store here paired with an acquire-load in ringRead is what makes
    // that safe. Zero means "no capture buffer yet" and the reader emits
    // silence without touching ring_.
    std::atomic<uint32_t> ringFrames_{ 0 };
    std::atomic<uint32_t> ringWritePos_{ 0 };
    std::atomic<uint32_t> ringReadPos_{ 0 };
    int captureChannels_ = 0;
    std::atomic<bool> captureRunning_{ false };

    // Planar scratch the plugin renders into. The wire format is interleaved
    // float32, which every shared-mode endpoint accepts.
    std::vector<std::vector<float>> planarOut_;
    std::vector<float*> planarOutPtr_;
    std::vector<std::vector<float>> planarIn_;
    std::vector<float*> planarInPtr_;

    int outChannels_ = 2;   // what the PLUGIN produces
    int inChannels_  = 0;
    int deviceOutChannels_ = 2;  // what the DEVICE accepted; usually the same

    int activeSampleRate_   = 48000;
    int activeBufferFrames_ = 512;

    std::atomic<bool> streamRunning_{ false };
    std::string lastError_;

    // AudioMidiDevices.h::lastWarning - written by the CAPTURE thread, which is
    // the only thing that can discover a degraded open, and read by the main
    // thread once open() has returned. The captureReady promise between them is
    // what orders the two: open() waits on it, so the string is complete before
    // anything else can look at it. close() joins that thread before clearing.
    std::string warning_;
};

} // namespace standalone
} // namespace gmpi
