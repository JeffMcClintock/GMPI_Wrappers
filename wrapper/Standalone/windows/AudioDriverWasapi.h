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
// COM APARTMENTS. Every WASAPI object this class touches is created ON the
// thread that uses it, and that thread is an MTA of its own. Activating an
// IAudioClient on the UI thread (an STA, because the window needs OLE) and then
// calling it from the render thread is a cross-apartment call that either
// marshals - putting the message loop in the path of every audio buffer - or
// misbehaves. So open() starts the thread and waits for it to report back,
// rather than doing the work itself and handing the result over.
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

    // The id of the "follow the system default" entry. An unconfigured app
    // opens this, and unlike an endpoint id it stays meaningful when the user
    // changes their default device or unplugs the interface it named.
    static const char* defaultDeviceId() { return "wasapi:default"; }

    const char* name() const override { return "WASAPI"; }

    std::vector<DeviceInfo> devices() override;
    std::vector<int> sampleRates() override;

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
};

} // namespace standalone
} // namespace gmpi
