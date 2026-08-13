#pragma once

// The standalone app's audio and MIDI seam.
//
// Deliberately NOT SynthEdit's IO_base / IMidiDriver. Those are shaped around
// UIoManager - IO_base::Render parks the DSP thread and the process callback
// calls back into the engine - and pulling them in would make this wrapper
// depend on SynthEditLib, which nothing else in GMPI_Wrappers does.
//
// What a GMPI standalone actually needs is much smaller: enumerate devices,
// open one, and be called back with buffers. So that is all this declares, and
// the Linux/Windows/macOS drivers below it are thin.
//
// Threading contract, which every implementation must honour:
//
//   * processAudio runs on the driver's realtime thread. No allocation, no
//     locks that a non-realtime thread can hold.
//   * onMidiIn is called from the MIDI driver's own thread, NOT the audio
//     thread. Implementations of MidiCallback must therefore be safe to call
//     concurrently with processAudio - StandaloneHost does this by pushing to
//     a lock-free FIFO the audio thread drains.
//   * everything else (devices(), open(), close()) is main-thread only.

#include <cstdint>
#include <string>
#include <vector>

namespace gmpi
{
namespace standalone
{

// One entry in a device list. `id` is what gets persisted in the settings file
// and handed back to open(), `name` is what the user sees. They differ because
// a display name is not stable across reboots (index changes when a USB device
// is plugged in) and is not unique.
struct DeviceInfo
{
    std::string id;
    std::string name;
};

// Implemented by StandaloneHost. Called from the audio driver's RT thread.
struct AudioCallback
{
    virtual ~AudioCallback() = default;

    // Planar, non-interleaved, `frames` long. `inputs` may be null when the
    // driver opened output-only. Buffers are owned by the driver and valid
    // only for the duration of the call.
    virtual void processAudio(
        int frames,
        const float* const* inputs, int inChannels,
        float* const* outputs, int outChannels) = 0;

    // The graph granted a rate or block size different from what was asked
    // for, so the plugin must be restarted against the real numbers. Called
    // from a non-realtime thread before the first processAudio.
    virtual void onAudioFormatChanged(float sampleRate, int maxBlockSize) = 0;
};

class AudioDriver
{
public:
    virtual ~AudioDriver() = default;

    // Shown in the settings pane's driver list ("PipeWire", "WASAPI", ...).
    virtual const char* name() const = 0;

    // Output devices, most-useful-first. The first entry is the one an
    // unconfigured app opens, so it must be the system default.
    virtual std::vector<DeviceInfo> devices() = 0;

    // Sample rates this driver will accept. Empty means "whatever the graph
    // gives you" and the settings pane then offers no choice.
    virtual std::vector<int> sampleRates() = 0;

    // requestedBufferFrames is a HINT. Ask getBufferFrames() afterwards for
    // what was actually granted; a server-side graph (PipeWire, JACK) picks
    // its own quantum and may change it while running.
    virtual bool open(
        const std::string& deviceId,
        int requestedSampleRate,
        int requestedBufferFrames,
        int inChannels,
        int outChannels,
        AudioCallback* client) = 0;

    virtual void close() = 0;

    virtual int  getSampleRate()  const = 0;
    virtual int  getBufferFrames() const = 0;

    // Empty when the last open() succeeded.
    virtual std::string lastError() const = 0;
};

// Implemented by StandaloneHost. Called from the MIDI driver's thread.
struct MidiCallback
{
    virtual ~MidiCallback() = default;

    // Raw MIDI 1.0 bytes as they arrived. Timestamping is the host's job: a
    // standalone has no transport to align to, and every driver's clock is
    // different, so events are simply applied at the start of the next block.
    virtual void onMidiIn(const uint8_t* data, int size) = 0;
};

class MidiDriver
{
public:
    virtual ~MidiDriver() = default;

    virtual std::vector<DeviceInfo> inputs() = 0;

    // A LIST, not a choice: a standalone synth normally wants every keyboard
    // on the machine at once, and the settings pane presents tick boxes.
    // An empty list means "connect everything readable", which is what makes
    // the app play out of the box before anyone has opened settings.
    virtual bool open(const std::vector<std::string>& inputIds, MidiCallback* client) = 0;

    virtual void close() = 0;

    virtual std::string lastError() const = 0;
};

} // namespace standalone
} // namespace gmpi
