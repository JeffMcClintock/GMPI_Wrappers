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
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "AudioMidiDevices.h"
#include "MidiStreamParser.h"
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
//
// A message too LONG to frame is dropped the same way, and for a stronger
// reason: it used to be cut to kMaxMessage instead, which delivered half a sysex
// dump with no 0xF7 on it - and at 256, one byte past what a one-byte length
// prefix can hold, wrote a length of zero and left the reader permanently one
// byte behind the writer, so every event after the first big dump was garbage.
// Whole or not at all.
class MidiFifo
{
public:
    static constexpr int kCapacity = 8192;      // bytes; ~2700 note events

    // Longest message accepted; anything longer is dropped whole. The value is
    // shared with MidiStreamParser, which is what stops the drivers reassembling
    // a dump this cannot then carry - see kMaxMidiMessage for why it is 255 and
    // what raising it would take.
    static constexpr int kMaxMessage = static_cast<int>(kMaxMidiMessage);

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

    // --- the plugin's patch ----------------------------------------------
    // The same state a DAW serialises through getState/setState, in the same
    // <Preset> format the VST3, CLAP and AU wrappers hand their hosts.
    // SessionState is the only caller; nothing here knows a file exists.

    // Whether this plugin has anything worth saving. False for a plugin with no
    // parameters at all, and equally for one whose every parameter is
    // non-stateful - both are patchless, and the difference does not matter to
    // a caller deciding whether to write a file.
    //
    // Filters exactly as gmpi::hosting::writePresetXml filters, so "true" and
    // "captureState() contains a <Param>" cannot disagree.
    bool hasStatefulParameters() const;

    // The current patch. MAIN THREAD ONLY.
    //
    // Read from the CONTROLLER's store, never the processor's, and that is a
    // correctness requirement rather than a preference: processAudio calls
    // messageQueUiToDsp_.pollMessage(&processor_), so the audio thread writes
    // processor_.patchManager while the stream runs. The controller's store has
    // no audio-thread writer at all - the DSP reaches it only by queueing to
    // message_que_dsp_to_ui, which onTimer drains on this thread.
    std::string captureState() const;

    // Load a patch into every copy of it: the controller's store, the plugin's
    // own <Controller/>, and the processor's store.
    //
    // MUST be called BEFORE startAudio(). The processor's store is written
    // directly here rather than through the UI->DSP queue, which is only safe
    // while no audio callback can run; and start_processor primes each input pin
    // from that store, so a patch in place before the stream opens reaches the
    // DSP with no queue traffic at all.
    //
    // False means the text held no <Preset> element and NOTHING was changed -
    // the plugin is still at its defaults, which is the honest outcome for a
    // file that turned out not to be a patch.
    //
    // The document must have been CHECKED first - see acceptsParameterText and
    // SessionState::restore. The SDK's readers assert on a <Param> they cannot
    // use rather than returning, so a bad one gets past this return value.
    bool restoreState(const std::string& xml);

    // Would the SDK's preset readers store this `val` text for this parameter
    // id, or assert on it?
    //
    // The gate a caller reading a FILE has to put in front of restoreState.
    // GmpiParameter::setFromXml treats text of the wrong kind as a defect in
    // whoever WROTE the preset - it asserts, which in a debug build is a
    // process that stops dead with a modal dialog and no window, and in a
    // release build is a silent 0.0. Neither is an answer to a file somebody
    // hand-edited, so the reader is left exactly as the DAW wrappers have it
    // and the standalone asks this question instead.
    //
    // TRUE for an id this plugin does not have: both readers skip an id they do
    // not recognise, so nothing will look at its value. Asked of the controller's
    // store, which is the same set of ids as the processor's - both are built
    // from every entry of the same pluginInfo::parameters.
    bool acceptsParameterText(int id, const char* text) const;

    // Put every stateful parameter back to the default the plugin's own spec
    // declares - what File > Revert to Plugin Defaults does.
    //
    // Safe WHILE AUDIO RUNS, unlike restoreState, and that is the whole reason
    // it is a separate call rather than restoreState("<Preset/>"): the DSP is
    // reached through the UI->DSP queue here, exactly as a knob move reaches it,
    // instead of by writing the processor's store from under the audio thread.
    //
    // Everything the plugin owns that is NOT a stateful parameter is untouched,
    // including whatever its <Controller/> published for this run.
    //
    // Must be called from the app's tick, not from a menu callback: a menu
    // action runs from a popup's completion with the editor's own event
    // handling on the stack, which is no place to push a parameter into it.
    void revertToDefaults();

    // Called whenever a parameter is edited - by the editor, by the plugin's own
    // controller, or by the command channel. Main thread, inside the edit.
    //
    // A notification rather than a save: the callee decides when a file is
    // worth writing, and this class stays unaware that one exists.
    void setOnParameterEdited(std::function<void()> onEdited);

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
    // True also when the answer was "everything readable" and there was nothing
    // readable: a laptop nobody has configured, with no keyboard plugged into
    // it, is an ordinary way to run a standalone synth and not a failure to be
    // reported. The full rule is MidiOpenTally::success. For a NAMED selection
    // it turns on what was ASKED for rather than on what happens to be plugged
    // in, which is what stops Linux's ever-present "Midi Through" port from
    // making the same physical situation read differently there.
    //
    // The never-configured default is the one case that still varies by
    // platform, and honestly so: "everything readable" is satisfied by whatever
    // WAS readable, and on Linux Midi Through always is. So a keyboard that
    // another program is holding gets named on Windows and macOS, where it was
    // the only candidate, and passes quietly on Linux, where something else
    // genuinely did connect.
    //
    // False means it was not applied, and there are two ways to get there:
    //
    //   * this run has no MIDI at all - no driver was set, or the plugin has no
    //     MIDI input pin. That arm returns before the close above, which strands
    //     nothing: startMidi is the only caller of MidiDriver::open, and
    //     setMidiDriver closes the outgoing driver.
    //   * inputs were named and not one of them could be connected - absent, or
    //     present and refusing - and midiError() carries what the driver said.
    //
    // A false return does NOT mean the driver is closed; MidiDriverAlsa stays
    // open on purpose (see MidiDriver::open). stopMidi() is what closes one, and
    // every path to a second startMidi goes through it.
    bool startMidi(const MidiInputSelection& inputs);
    void stopMidi();

    // Whether a sound is coming out, which is not the same question as whether
    // startAudio() succeeded - and used to be answered as though it were.
    //
    // A stream can die where it stands (AudioMidiDevices.h::isStreamRunning), so
    // the driver is asked every time rather than a flag being trusted: the flag
    // below only records that THIS class started a stream and has not stopped
    // it, and after a device was unplugged that flag and the truth are different
    // things. The settings page and the command channel both used to read the
    // flag, and both went on reporting "Running: 48000 Hz" at nothing.
    bool isAudioRunning() const;

    // Why the audio stream stopped ON ITS OWN, or empty - the third channel
    // described in AudioMidiDevices.h::stoppedReason, and the only one that can
    // fill up with no call having been made.
    //
    // Empty when audio never started (lastError() is that story), empty while it
    // runs, and empty after a stopAudio() this app asked for. Non-empty is
    // therefore exactly the news the app has to go and tell somebody, which is
    // what runStandaloneApp's tick watches for.
    std::string audioStoppedReason() const;

    // Why AUDIO is not running, or empty. MIDI has its own string below.
    std::string lastError() const { return lastError_; }

    // What the running audio device could NOT give the plugin, or empty - the
    // degraded-open channel described in AudioMidiDevices.h::lastWarning. The
    // two are never both filled: this one needs an open that succeeded and that
    // one an open that did not.
    //
    // Read straight from the driver rather than copied here, because the driver
    // is what clears it: a copy would be one more thing to keep in step with
    // the device that is actually open. Nothing to say when audio is not
    // running - a driver's warning describes a stream, and there is none.
    std::string audioWarning() const;

    // Why the last startMidi could not connect what was asked for, or empty.
    //
    // Separate from lastError() because the two used to share one string, and
    // startMidi runs after startAudio: on a machine whose soundcard was busy AND
    // whose keyboard was missing, the settings page showed only the MIDI
    // sentence - the problem the user could do nothing about - in place of the
    // audio one that had actually stopped the app making a sound.
    std::string midiError() const { return midiError_; }

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
    // One place every UI-side parameter edit passes through, whatever kind it
    // is: the editor moving a knob (notifyDaw), the plugin's controller writing
    // a blob (sendNonNativeParameterToProcessor), or the command channel doing
    // either. Queues the change for the processor and then tells whoever asked.
    void onParameterChanged(gmpi::hosting::GmpiParameter* param);

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

    // The plugin's own <Controller/> subtype, if it declares one. Held for the
    // lifetime of the host: a plugin that has one typically builds its whole
    // application object here and publishes a pointer to it through a
    // parameter, so releasing it would pull the ground out from under the
    // editor. See the note at its creation in StandaloneHost::initialize.
    gmpi::shared_ptr<gmpi::api::IController> pluginController_;

    gmpi::shared_ptr<gmpi::api::IEditor> editorParameters_;
    gmpi::shared_ptr<gmpi::api::IDrawingClient> editorGraphics_;

    // What onParameterChanged tells, or an empty function when nobody is
    // listening. Cleared by the app before this object is destroyed, because
    // whatever it captures is a shorter-lived thing than the host.
    std::function<void()> onParameterEdited_;

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

    // That a stream was STARTED here and not stopped here. Whether it is still
    // running is the driver's to answer - see isAudioRunning() above.
    bool audioRunning_ = false;
    bool midiRunning_  = false;
    std::string lastError_;
    std::string midiError_;

    float sampleRate_ = 44100.0f;
    int   blockSize_  = 512;
};

} // namespace standalone
} // namespace gmpi
