#pragma once

// The portable half of the standalone app: everything that is not a window.
//
// This is the same host-side scaffolding the VST3, CLAP and AU wrappers build
// on (Hosting/processor_holder.h + Hosting/controller_holder.h), with the DAW
// replaced by an audio device and a MIDI port. The plugin cannot tell the
// difference: it still gets a Processor subtype driven by process(), an Editor
// subtype driven by a drawing host, and the same two inter-thread queues
// carrying parameter changes between them.
//
// What the platform layer supplies, and this class deliberately does not own:
//
//   * the window and its event loop
//   * the drawing host (WaylandToplevel, DxDrawingFrameHwnd, the NSView)
//   * the concrete AudioDriver / MidiDriver
//
// so that one core serves Wayland, Win32 and Cocoa, and so the same core can
// be driven headless by a test.

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "AudioMidiDevices.h"
#include "Hosting/controller_holder.h"
#include "Hosting/gmpi_factory.h"
#include "Hosting/processor_holder.h"
#include "GmpiMidi.h"

// IDrawingClient and the rest of the client/host contract live in gmpi_ui, not
// in the GMPI core: the plugin's editor is a drawing client, so even this
// window-free half of the app needs those declarations.
#include "GmpiUiDrawing.h"
#include "helpers/NativeUi.h"
#include "helpers/Timer.h"

namespace gmpi
{
namespace standalone
{

// MIDI arrives on the driver's thread and must be consumed on the audio
// thread, so it crosses a lock-free FIFO rather than a mutex: taking a lock in
// processAudio would let a MIDI thread stall the soundcard.
//
// Single producer, single consumer, byte-oriented with a length prefix per
// message. Overflow DROPS the incoming message rather than blocking or
// overwriting - a dropped note is bad, a stuck note or an audio dropout is
// worse, and a full queue only happens when audio has already stopped.
class MidiFifo
{
public:
    static constexpr int kCapacity = 8192;      // bytes; ~2700 note events
    static constexpr int kMaxMessage = 256;     // longest message accepted (sysex is truncated)

    // Producer side (MIDI thread).
    void push(const uint8_t* data, int size);

    // Consumer side (audio thread). Returns bytes written to `out`, or 0 when
    // the queue is empty. `out` must be at least kMaxMessage long.
    int pop(uint8_t* out);

private:
    uint8_t buffer_[kCapacity]{};
    std::atomic<uint32_t> writePos_{ 0 };
    std::atomic<uint32_t> readPos_{ 0 };
};

// Which MIDI inputs the USER asked for, which is not the same thing as the id
// list MidiDriver::open takes. Down at the driver an empty list means "connect
// everything readable" (see AudioMidiDevices.h) - the right answer on a machine
// nobody has configured, and the exact opposite of what someone who has just
// unticked the last input asked for. Both are an empty vector, so the choice
// travels as this instead, which can say "nothing" out loud.
//
// There is no public constructor and no conversion from a vector: a caller has
// to name which of the two it means, and StandaloneHost::startMidi is the only
// thing that turns the answer back into a list a driver can take.
class MidiInputSelection
{
public:
    // Nothing configured, so connect everything readable - what makes a fresh
    // install play the moment a keyboard is plugged in.
    static MidiInputSelection all() { return MidiInputSelection(false, {}); }

    // Exactly these inputs, and NOTHING when the list is empty. "Everything"
    // cannot be spelled here at all; that is what all() is for.
    static MidiInputSelection ids(std::vector<std::string> chosen)
    {
        return MidiInputSelection(true, std::move(chosen));
    }

    // A settings file's two halves resolved together, because neither means
    // anything without the other: keyMidiInputs is an empty list both before
    // the user has chosen and after they have chosen nothing, and only
    // keyMidiInputsSet separates them. Every shell's startup path goes through
    // here, so none of them can resolve it differently from the others.
    //
    // `configured` is false until a tick box is actually ticked or unticked -
    // SettingsPane::writeMidiInputs is the only thing that ever sets the flag,
    // and merely opening the settings page and closing it again does not reach
    // it. So a user who has only LOOKED at the page still gets everything.
    static MidiInputSelection saved(bool configured, std::vector<std::string> chosen)
    {
        return configured ? ids(std::move(chosen)) : all();
    }

    // True when the answer is "no MIDI input at all". Must be tested before
    // driverIds(), which then hands back the empty vector a driver would read
    // as "connect everything".
    bool isNone() const { return configured_ && ids_.empty(); }

    const std::vector<std::string>& driverIds() const { return ids_; }

private:
    MidiInputSelection(bool configured, std::vector<std::string> chosen)
        : configured_(configured)
        , ids_(std::move(chosen))
    {
    }

    bool configured_;
    std::vector<std::string> ids_;
};

class StandaloneHost :
      public AudioCallback
    , public MidiCallback
    , public gmpi::TimerClient
{
public:
    StandaloneHost();
    ~StandaloneHost();

    // Reads the statically-linked plugin's spec via MP_GetFactory. False when
    // the binary has no plugin in it, which is a build error rather than a
    // runtime condition, so the caller should say so and exit.
    bool init();

    const gmpi::hosting::pluginInfo* pluginInfo() const { return info_; }
    std::string pluginName() const { return info_ ? info_->name : std::string{ "GMPI Plugin" }; }

    int audioInputCount()  const { return audioInCount_; }
    int audioOutputCount() const { return audioOutCount_; }
    bool wantsMidiInput()  const { return hasMidiInPin_; }

    // --- editor ----------------------------------------------------------
    // Created in init(), before any window exists, because the window's size
    // comes from asking the editor to measure itself.

    gmpi::api::IDrawingClient* editorDrawingClient() const { return editorGraphics_.get(); }

    // Pass to the drawing frame's setFallbackHost(). The frame answers
    // IDrawingHost/IInputHost itself and forwards IEditorHost here; without it
    // the first knob drag dereferences null.
    gmpi::api::IUnknown* parameterHost();

    // The parameter store and its two notification paths, for the command
    // channel's --get-param / --set-param (mcp/CommandDispatcher.cpp).
    //
    // Handed out whole rather than wrapped in per-parameter accessors: setting
    // a value correctly means notifying the editor AND queueing to the
    // processor, and a getter/setter pair that did only the first would be an
    // inviting way to get it half right.
    gmpi::hosting::gmpi_controller_holder& controller() { return controller_; }

    // Call once the drawing client has been attached to a frame and given a
    // host. Runs the plugin's own initialisation and pushes the current value
    // of every parameter into it.
    void onEditorAttached();

    // The editor's preferred size in DIPs, clamped to what it says it can do.
    // Not const: gmpi::shared_ptr's accessors are non-const, and measure() is
    // a call INTO the plugin anyway - it is entitled to cache a layout.
    void getEditorSize(float& width, float& height);

    // --- audio / MIDI ----------------------------------------------------
    // Drivers are owned here but constructed by the platform layer, which is
    // the only thing that knows whether "audio" means PipeWire or WASAPI.

    void setAudioDriver(std::unique_ptr<AudioDriver> driver);
    void setMidiDriver(std::unique_ptr<MidiDriver> driver);

    AudioDriver* audioDriver() const { return audioDriver_.get(); }
    MidiDriver*  midiDriver()  const { return midiDriver_.get(); }

    // Open the configured devices and start processing. Safe to call again to
    // apply a settings change; it stops first. False leaves the app running
    // silently and lastError() explains why - a standalone whose window will
    // not open because the soundcard is busy is a worse outcome than a silent
    // one that lets you pick a different device.
    bool startAudio(const std::string& deviceId, int sampleRate, int bufferFrames);
    void stopAudio();

    // Connect the chosen MIDI inputs. Safe to call again to apply a settings
    // change: a running driver is closed first, so a selection of nothing
    // really does end up with nothing listening.
    //
    // True means the selection was applied, INCLUDING a deliberate selection of
    // nothing: the user asked for no MIDI input and now has none, which is a
    // success rather than a failure to connect anything.
    //
    // False means it was not applied, and there are two ways to get there:
    //
    //   * this run has no MIDI at all - no driver was set, or the plugin has no
    //     MIDI input pin. That arm returns before the close above, which strands
    //     nothing: startMidi is the only caller of MidiDriver::open, and
    //     setMidiDriver closes the outgoing driver. lastError() is left alone.
    //   * the driver could not open the inputs that were chosen, and
    //     lastError() carries what it said.
    //
    // No shell checks the result - a standalone with no keyboard attached is an
    // ordinary way to run one.
    bool startMidi(const MidiInputSelection& inputs);
    void stopMidi();

    bool isAudioRunning() const { return audioRunning_; }
    std::string lastError() const { return lastError_; }

    // --- AudioCallback (realtime thread) ---------------------------------
    void processAudio(int frames,
                      const float* const* inputs, int inChannels,
                      float* const* outputs, int outChannels) override;
    void onAudioFormatChanged(float sampleRate, int maxBlockSize) override;

    // --- MidiCallback (MIDI driver thread) -------------------------------
    void onMidiIn(const uint8_t* data, int size) override;

    // --- TimerClient (UI thread) -----------------------------------------
    // Pumps both parameter queues. The Wayland loop drives gmpi::TimerManager,
    // so this ticks with the frame rather than on a thread of its own.
    bool onTimer() override;

private:
    // Rebuilds the plugin's Processor instance for a new rate/block size, and
    // primes it with the current parameter values.
    //
    // The device stream may already be RUNNING when this is called - all three
    // drivers start it inside open() - so what makes the swap safe is that
    // processorLive_ is false for the whole rebuild and a callback arriving
    // meanwhile is turned back at the top of processAudio. Calling this while
    // the flag is true would free the processor under a callback executing
    // inside it.
    void restartProcessor(float sampleRate, int blockSize);

    gmpi::hosting::pluginInfo* info_{};

    // Processor side. `processorLive_` gates the audio callback: the pointer
    // alone is not enough, because restartProcessor swaps it while the driver
    // may still deliver one last buffer.
    gmpi::hosting::gmpi_processor processor_;
    std::atomic<bool> processorLive_{ false };

    // Controller side, matching Controller_CLAP: the holder plus the UI->DSP
    // queue and the list of parameters waiting to travel down it.
    gmpi::hosting::gmpi_controller_holder controller_;
    gmpi::hosting::interThreadQue messageQueUiToDsp_;
    gmpi::hosting::QueuedUsers pendingQueueClients_;

    gmpi::shared_ptr<gmpi::api::IEditor> editorParameters_;
    gmpi::shared_ptr<gmpi::api::IDrawingClient> editorGraphics_;

    std::unique_ptr<AudioDriver> audioDriver_;
    std::unique_ptr<MidiDriver>  midiDriver_;

    // MIDI 1.0 bytes in, MIDI 2.0 UMP events on the plugin's MIDI pin out.
    // Constructed with a sink that pushes into processor_.events, so it may
    // only be called from the audio thread.
    gmpi::midi::MidiConverter2 midiConverter_;
    MidiFifo midiFifo_;

    // Scratch buffers handed to the plugin's audio pins. Sized in
    // restartProcessor and never resized while audio runs.
    std::vector<std::vector<float>> silenceIn_;
    std::vector<float*> silenceInPtr_;

    int audioInCount_  = 0;
    int audioOutCount_ = 0;
    bool hasMidiInPin_ = false;

    bool audioRunning_ = false;
    bool midiRunning_  = false;
    std::string lastError_;

    float sampleRate_ = 44100.0f;
    int   blockSize_  = 512;
};

} // namespace standalone
} // namespace gmpi
