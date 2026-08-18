#pragma once

// PipeWire audio for the Linux standalone.
//
// Adapted from SynthEdit's SynthEditWayland/IO_PipeWire.cpp, which is the
// proven version of this - the stream setup, the negotiation wait, the
// capture ring buffer and the quantum chunking are all its design. What
// changed is the far end: instead of driving UIoManager it calls an
// AudioCallback, and instead of IO_base's driver list it enumerates the
// graph's real sinks so the settings pane has something to offer.
//
// Duplex is two streams, not one pw_filter: pw_stream is one-directional, so
// capture is a second stream with its own callback handing frames over through
// a ring buffer. Only the PLAYBACK callback drives the plugin. Capture opens
// only when the plugin has audio input pins, because a machine with no capture
// device must still be able to play a synth.

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "../AudioMidiDevices.h"

// At GLOBAL scope, deliberately. Written as `const struct spa_pod*` inside the
// class below it would declare a DIFFERENT spa_pod in the enclosing namespace,
// and the .cpp's handlers would then not match PipeWire's own type.
struct spa_pod;

namespace gmpi
{
namespace standalone
{

class AudioDriverPipeWire : public AudioDriver
{
public:
    AudioDriverPipeWire();
    ~AudioDriverPipeWire() override;

    const char* name() const override { return "PipeWire"; }

    // The "let the desktop decide" entry. It stays valid across reboots and
    // device changes, which a node name does not; open() answers it by leaving
    // PW_KEY_TARGET_OBJECT unset - see AudioDriver::defaultDeviceId.
    const char* defaultDeviceId() const override { return "pipewire:default"; }

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
    // PipeWire types stay out of this header. Pw owns the loop and streams;
    // PwGlue (in the .cpp) adapts pw_stream_events' C callbacks - whose
    // signatures use PipeWire's enums - onto the private handlers below.
    struct Pw;
    friend struct PwGlue;

    void onProcess();
    void onCaptureProcess();
    void onStateChanged(int newState, const char* error);
    void onParamChanged(uint32_t id, const ::spa_pod* param);

    bool ensureLoop();
    bool openCapture(int rate, const std::string& deviceId);
    void teardownStreams();
    void teardownLoop();

    // Capture -> playback hand-off. One writer (the capture callback), one
    // reader (the playback callback), both on PipeWire threads: indices are
    // atomic and the buffer is never resized while either runs. Underruns feed
    // silence rather than stale audio - a click is honest, a loop is not.
    void ringWrite(const float* interleaved, uint32_t frames, int srcChannels);
    void ringRead(float* const* planarOut, int outChannels, uint32_t frames);

    std::unique_ptr<Pw> pw_;

    AudioCallback* client_{};

    std::vector<float> ring_;
    uint32_t ringFrames_ = 0;
    std::atomic<uint32_t> ringWritePos_{ 0 };
    std::atomic<uint32_t> ringReadPos_{ 0 };
    int captureChannels_ = 0;
    std::atomic<bool> captureRunning_{ false };

    // Planar scratch the plugin renders into; the wire format is interleaved
    // F32, which every PipeWire adapter takes without negotiation detours.
    std::vector<std::vector<float>> planarOut_;
    std::vector<float*> planarOutPtr_;
    std::vector<std::vector<float>> planarIn_;
    std::vector<float*> planarInPtr_;

    int outChannels_ = 2;
    int inChannels_  = 0;

    int activeSampleRate_  = 48000;
    int activeBufferFrames_ = 512;

    std::atomic<bool> streamRunning_{ false };
    std::string lastError_;
};

} // namespace standalone
} // namespace gmpi
