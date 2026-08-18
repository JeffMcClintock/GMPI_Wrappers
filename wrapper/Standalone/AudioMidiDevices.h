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
// and handed back to open(); `name` exists only to be shown to the user, and
// nothing is ever matched on it.
//
// The id therefore carries the whole burden. It has to select the same device
// after a reboot, and after other devices have been plugged in and pulled out
// around it, and it has to be unique within the list it came from. What it is
// DERIVED from is the driver's own business, and the drivers differ. Each audio
// driver has an opaque handle to hand over - a WASAPI endpoint id, a CoreAudio
// UID, a PipeWire node.name - and each also lists one entry that is no piece of
// hardware at all, a "...:default" sentinel standing for whatever the system is
// currently using, which is the one entry that survives the device behind it
// being unplugged. Among the MIDI drivers only CoreMIDI has such a handle,
// kMIDIPropertyUniqueID; the winmm index and the ALSA sequencer address both
// renumber when hardware moves, so those two drivers build the id out of the
// display name instead and add a suffix to tell duplicates apart. That is a
// perfectly good id - what is asked of one is the guarantee above, not any
// particular source.
//
// A name is held to none of it, and two identical keyboards may well produce
// two identical names - the audio drivers and CoreMIDI report whatever the
// system calls the device and leave it there. Where the id was derived from the
// name it is the disambiguated form that gets shown, so winmm and ALSA do hand
// back a "Foo #2" the user can tell from the first Foo. Either way a name is
// meant to read the way the platform's own settings app names the device, so
// that a user is choosing by words they have already seen.
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

    // The seam for a stream that renegotiates its format WHILE RUNNING. The
    // plugin's processor has to be rebuilt against the new numbers, and after
    // open() has returned this is a driver's only way to ask for that.
    //
    // Hard condition on any driver that calls it: no render callback may be in
    // flight, and none may begin, until it returns. The rebuild destroys the
    // processor object that a callback would be executing inside.
    //
    // No driver needs it yet. What a graph grants at open time is reported
    // through getSampleRate()/getBufferFrames() instead, which the host reads
    // once open() has returned, so the processor is built exactly once per
    // start. A quantum that changes mid-run is absorbed by the driver (see
    // open() below); a RATE that changes mid-run is the case this exists for.
    // The only driver that can currently observe one is PipeWire, and it just
    // updates the rate it reports, so the plugin goes on running - at the wrong
    // pitch - against the rate it started with.
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

    // requestedSampleRate and requestedBufferFrames are HINTS. What was
    // actually granted is getSampleRate()/getBufferFrames(), read once this has
    // returned; no driver announces its format through the client.
    //
    // getBufferFrames() is a MAXIMUM, and holds for the life of the stream. A
    // server-side graph (PipeWire, JACK) picks its own quantum and may change
    // it while running, so a driver on one chunks the graph's cycle down to the
    // block size it granted rather than handing over a longer one. A rate that
    // changes while running is the case nothing handles yet - see
    // AudioCallback::onAudioFormatChanged above.
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
    //
    // So this seam cannot express "connect nothing", and a user who unticks
    // every input is asking for exactly that. Callers therefore do not build
    // this list themselves: they hand a MidiInputSelection (StandaloneHost.h)
    // to StandaloneHost::startMidi, which never calls open() at all for that
    // case. Drivers need do nothing about it beyond keeping the rule above.
    virtual bool open(const std::vector<std::string>& inputIds, MidiCallback* client) = 0;

    virtual void close() = 0;

    virtual std::string lastError() const = 0;
};

} // namespace standalone
} // namespace gmpi
