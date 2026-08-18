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
//   * every call into processAudio is wrapped in a ScopedNoDenormals (below).
//   * onMidiIn is called from the MIDI driver's own thread, NOT the audio
//     thread. Implementations of MidiCallback must therefore be safe to call
//     concurrently with processAudio - StandaloneHost does this by pushing to
//     a lock-free FIFO the audio thread drains.
//   * everything else (devices(), open(), close()) is main-thread only.

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Which of ScopedNoDenormals' two implementations applies. The INSTRUCTION SET
// decides this, not the operating system, which is why the guard can live in
// the portable half of the app at all: one x86 branch and one AArch64 branch
// between them cover all three shells.
#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__) \
    || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
  #define GMPI_STANDALONE_DENORMALS_MXCSR 1
  #include <xmmintrin.h>
#elif (defined(__aarch64__) || defined(__arm64__)) && !defined(_MSC_VER)
  #define GMPI_STANDALONE_DENORMALS_FPCR 1
#endif

namespace gmpi
{
namespace standalone
{

// Denormals off for the duration of one call into the plugin.
//
// A plugin with a decaying tail - a reverb, a filter with feedback - keeps
// multiplying its own state by something under one, and once those numbers fall
// below the smallest normal float the CPU drops into microcoded arithmetic that
// can cost many times more for the same block. Setting flush-to-zero on the
// audio thread is what a host normally does about that, so a plugin author does
// not meet it in the DAW they test in; a standalone that skipped it would be the
// one place their plugin stutters, with nothing on screen to explain why.
//
// Constructed by each driver around its call into AudioCallback::processAudio,
// and RESTORING on the way out: two of the three realtime threads belong to
// somebody else - CoreAudio's IOProc thread, PipeWire's graph thread, which
// also runs other clients living in this process - and leaving the control word
// changed under them would be altering arithmetic they never asked about.
class ScopedNoDenormals
{
public:
    ScopedNoDenormals(const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;

#if defined(GMPI_STANDALONE_DENORMALS_MXCSR)
    ScopedNoDenormals()
        : saved_(_mm_getcsr())
    {
        // 0x8000 flushes a denormal RESULT to zero, 0x0040 reads a denormal
        // OPERAND as zero. Both: a feedback loop reads back what it just wrote,
        // so either one alone leaves the other half of the loop slow.
        _mm_setcsr(saved_ | 0x8000u | 0x0040u);
    }

    ~ScopedNoDenormals() { _mm_setcsr(saved_); }

private:
    unsigned int saved_;
#elif defined(GMPI_STANDALONE_DENORMALS_FPCR)
    ScopedNoDenormals()
    {
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(saved_));
        // FZ, bit 24. AArch64 has no separate operand control: FZ covers both
        // the operands and the result.
        __asm__ __volatile__("msr fpcr, %0" : : "r"(saved_ | (1ull << 24)));
    }

    ~ScopedNoDenormals() { __asm__ __volatile__("msr fpcr, %0" : : "r"(saved_)); }

private:
    unsigned long long saved_;
#else
    // Some other instruction set, or MSVC on ARM64, where the control register
    // is reachable only through intrinsics this header does not want to know
    // about. Nothing is set and nothing is restored, so a plugin with a decaying
    // tail costs what it costs; correctness is unaffected either way.
    //
    // Both of these are written out rather than defaulted so that a driver's
    // `ScopedNoDenormals guard;` is not a trivially-destructible unused local
    // here and a warning on the one platform where the guard does nothing.
    ScopedNoDenormals() {}
    ~ScopedNoDenormals() {}
#endif
};

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

    // The id of this driver's "...:default" sentinel - the entry devices()
    // lists first, and what an app with nothing saved opens.
    //
    // Asked of the DRIVER because the driver is what invents the string, and
    // what tests incoming ids against it: each open() below decides "system
    // default" by comparing the id it was handed to this one. Two spellings and
    // a fresh install silently opens the wrong thing, or nothing.
    //
    // PlatformShell used to answer this on the driver's behalf. That worked
    // only for as long as whoever wrote a shell also wrote its driver and
    // happened to keep the two in step; there was nothing to catch a platform
    // that answered one string here and matched another over there. A new
    // platform now cannot get it inconsistent, because there is only one answer
    // to give.
    //
    // An EMPTY id means the same thing, and every driver must accept it: that
    // is what a settings file written before this key existed contains.
    //
    // const char* like name() above - always a literal, never computed. The
    // settings layer deals in std::string and converts.
    virtual const char* defaultDeviceId() const = 0;

    // Output devices, most-useful-first. The first entry is the one an
    // unconfigured app opens, so it must be the system default.
    //
    // The name of that first entry is "System Default" on every platform. It is
    // the one device name the app writes rather than reads, so it is the one
    // that can drift, and it did: two shells said that and the third said
    // "Default output (system)" for the identical thing.
    virtual std::vector<DeviceInfo> devices() = 0;

    // The rates THIS DEVICE will actually run at, ascending. `deviceId` is one
    // of devices()' ids, and an empty one means what it means to open(): the
    // system default.
    //
    // THE APP DOES NOT RESAMPLE, and this list is where that shows. A
    // standalone exists to audition a plugin, and a sample rate converter
    // between the plugin and the soundcard costs CPU and puts a filter nobody
    // asked for into the thing being judged. So the plugin runs at a rate the
    // hardware is already clocked at, and the settings pane offers exactly
    // those: a rate the user cannot actually get is not a choice, it is a way
    // to end up somewhere else without being told.
    //
    // Worth saying plainly because all three drivers used to answer the same
    // fixed { 44100, 48000, 88200, 96000, 176400, 192000 } whatever was plugged
    // in, and two of them made that list true by switching a converter on -
    // WASAPI's AUTOCONVERTPCM, PipeWire's adapter resampling into the graph.
    // A wish-list and a converter are two halves of the same mistake.
    //
    // EMPTY is a legitimate answer: the driver cannot say what the device will
    // take. The settings pane then offers no rate control at all and passes 0
    // to open(), which leaves the device on whatever it is already running -
    // the same outcome as a wish-list, without the pretence of a choice.
    virtual std::vector<int> sampleRates(const std::string& deviceId) = 0;

    // requestedSampleRate and requestedBufferFrames are HINTS. What was
    // actually granted is getSampleRate()/getBufferFrames(), read once this has
    // returned; no driver announces its format through the client.
    //
    // requestedSampleRate is one of sampleRates() above, or 0 for "no
    // preference". NO DRIVER CONVERTS TO REACH IT: where the device will not
    // run at that rate the stream opens at one it will, and getSampleRate()
    // says which. That is the whole of the policy at this end - a driver that
    // finds itself reaching for a resampler has misread it.
    //
    // getBufferFrames() is a MAXIMUM, and holds for the life of the stream. A
    // server-side graph (PipeWire, JACK) picks its own quantum and may change
    // it while running, so a driver on one chunks the graph's cycle down to the
    // block size it granted rather than handing over a longer one. A rate that
    // changes while running is the case nothing handles yet - see
    // AudioCallback::onAudioFormatChanged above.
    //
    // inChannels/outChannels are the PLUGIN's pin counts, and the device's own
    // count is frequently something else. One rule, all three drivers:
    //
    //   device WIDER - a MONO plugin goes to every device channel, because
    //     "play my synth" on a stereo card means both speakers and a synth
    //     heard only on the left is a fault report waiting to happen. A plugin
    //     with two or more channels goes to the first N and the rest are
    //     silent: a surround card should not get the front pair repeated
    //     behind the listener.
    //   device NARROWER - the plugin's extra channels are DROPPED, not folded
    //     down. Only a mono output reaches this, and summing a stereo plugin
    //     would change the level of everything being auditioned.
    //
    // Written here because it had been written three times and one of the three
    // did the opposite - silence on the extra channels, under a comment
    // rejecting the mono case explicitly.
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

    // Empty when the last open() succeeded, and that is a rule rather than a
    // description: StandaloneHost reads this only on a false return, and the
    // settings pane's status line shows either this string or "Running", never
    // both. A note left here by a successful open is therefore invisible, and
    // an audio failure LATER in the session would be read next to it as though
    // the two were one sentence.
    //
    // A DEGRADED open is still a success and does not belong here - no capture
    // device, an input the plugin will hear silence from. lastWarning() below
    // is where that goes; one driver used to write the sentence here and return
    // true anyway, which is what this paragraph exists to stop.
    virtual std::string lastError() const = 0;

    // What a SUCCESSFUL open could not give the plugin, or empty. The other
    // half of the paragraph above: open() returned true, the app is making a
    // sound, and something the plugin has pins for is not working.
    //
    // Every driver's case so far is audio INPUT - no capture endpoint at all,
    // an output device with no input streams, a recording endpoint clocked
    // where the playback endpoint is not - and all three used to say so on
    // stderr alone. A standalone launched from a file manager has no console to
    // say it to: the settings page reported "Running: 48000 Hz, 512 frames"
    // while every input pin was fed silence, which is the one degraded state
    // with no other way to reach the user. So it is reported here as well, and
    // the pane draws it under the audio controls the way it already draws the
    // MIDI one.
    //
    // ONE SHORT SENTENCE, and a SPECIFIC one - "the recording device is at
    // 44100 Hz and playback at 48000 Hz" is something the user can act on,
    // "input unavailable" is not. The pane has a single line for it, so a
    // driver with advice too long to fit prints that advice on stderr after the
    // sentence, and nowhere else.
    //
    // Cleared by close(), which every open() begins with - so what is read here
    // always belongs to the stream now running, and a stopped device has
    // nothing to say.
    virtual std::string lastWarning() const = 0;

    // Whether the stream a successful open() started is still running.
    //
    // The one question on this interface whose answer changes with nobody having
    // called anything. A stream can end where it stands - the interface
    // unplugged, the audio service restarted, the server dropping the client -
    // and each driver notices on a thread of its own: WASAPI's render thread,
    // PipeWire's loop thread, a CoreAudio property listener. The main thread is
    // not involved in the discovery, so it does not know until it asks.
    //
    // Which is why this is a flag to be POLLED rather than a callback to be
    // fired. The discovery happens on, or beside, a realtime thread that is in
    // the middle of dying, and calling from there into the host - which would
    // then want the settings page, the drivers, the plugin's processor - is how
    // a teardown race becomes a use-after-free. So an implementation does the
    // smallest thing such a thread can safely do: clear an atomic, and point
    // stoppedReason() at a string literal. StandaloneApp's tick asks once a
    // frame, on the main thread, where acting on the answer is safe.
    //
    // False before the first open(), after an open() that FAILED, and after
    // close(). True from a successful open() until the stream ends, whichever of
    // the two ways it ends.
    //
    // NO DRIVER REOPENS ANYTHING. One that notices its device vanish must not go
    // looking for another: this app deliberately does not follow a mid-session
    // change of default device, and quietly playing out of a different speaker
    // is a worse answer than saying that the stream stopped. Reporting is the
    // whole job here.
    virtual bool isStreamRunning() const = 0;

    // Why the stream stopped by itself, or empty. The third of this interface's
    // three sentences, and the three do not overlap:
    //
    //   lastError()   - open() returned false. There is no stream.
    //   lastWarning() - open() returned true and something the plugin has pins
    //                   for is not working. There IS a stream, and it is running.
    //   this          - open() returned true, the stream ran, and then it ended
    //                   with nobody having called close().
    //
    // Its own channel rather than a second use of lastWarning() because of where
    // the two are read: a warning is drawn BESIDE "Running: 48000 Hz, 512
    // frames" and qualifies it, and this REPLACES that line, being the news that
    // the line has stopped being true. A dead stream is not a degraded one.
    //
    // ONE SHORT SENTENCE in the same voice as lastWarning's, naming what
    // happened rather than the call that failed - "Audio stopped: the device was
    // removed, or the audio service restarted." A driver with more to say prints
    // the rest on stderr, as the warnings do.
    //
    // Empty while the stream runs, empty before it ever ran, and empty after a
    // close() the app asked for: a stop the app performed itself is not news to
    // it. Cleared by close(), which every open() begins with, so what is read
    // here always belongs to the stream that has just stopped.
    virtual std::string stoppedReason() const = 0;
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
    //
    // Nor does an implementation decide for itself what to make of the list, or
    // what to return when nothing came of it: MidiOpenTally below is the one
    // answer to both, and every driver runs its open loop through it.
    //
    // A FALSE RETURN DOES NOT PROMISE THE DRIVER IS CLOSED. Two of the three
    // always close on the way out; MidiDriverAlsa does on the paths where the
    // sequencer itself would not open, and deliberately does NOT when the
    // subscriptions were the problem, because by then it has a routable
    // sequencer client that another program can still wire a keyboard into - its
    // open() says why at length. So close() must be safe after a failed open,
    // and callers must actually call it: StandaloneHost::startMidi calls
    // stopMidi() before every attempt, and the shells call it again on the way
    // down.
    virtual bool open(const std::vector<std::string>& inputIds, MidiCallback* client) = 0;

    // Safe at any point: before any open(), after a failed one, and twice in a
    // row. Every driver's open() begins with it, which is what stops the
    // paragraph above from leaking one.
    virtual void close() = 0;

    // Empty when the last open() succeeded. When the open LOOP came to nothing,
    // what it says comes from MidiOpenTally::failure(), so that the settings
    // pane describes the same situation in the same words on all three
    // platforms. A driver that could not get as far as that loop - no ALSA
    // sequencer, no CoreMIDI client - speaks for itself, in its own words.
    virtual std::string lastError() const = 0;
};

// What open() above makes of a selection: which inputs to connect, whether the
// attempt as a whole succeeded, and what to say when it did not. One
// implementation, driven by all three drivers.
//
// Here rather than in each of them because it HAD been written three times over
// and the three had already drifted. winmm chose its "nothing opened" sentence
// on whether any devices EXISTED and CoreMIDI on whether the user had SELECTED
// any, so a machine with no MIDI hardware and a stale saved selection got two
// different explanations from the two. ALSA counted nothing at all -
// snd_seq_connect_from returned into the void and open() reported success as
// long as the sequencer handle had opened - so the one platform that can
// silently subscribe to nothing was also the one that never said so.
//
// The selection test and the counting are ONE object rather than two, because
// two is what ALSA had: it tested the selection and counted nothing. A driver
// that has to call wants() anyway gets the count of what it was offered for
// free. What became of each yes it still has to report, and opened()/failed()
// below are the only two ways to do that.
class MidiOpenTally
{
public:
    // The selection by value: open() is not a realtime path, and a few strings
    // are a small price for a tally that cannot outlive the vector it reads.
    explicit MidiOpenTally(std::vector<std::string> inputIds)
        : selection_(std::move(inputIds))
    {
    }

    // Ask once per input the driver enumerated - EVERY one of them, including
    // the ones about to be skipped, because this is also what counts them, and
    // "was there anything here at all" is what success() falls back on when
    // nothing connected. True means connect it.
    //
    // An empty selection wants everything, which is the empty-list contract
    // above. A selection of NOTHING never arrives here: StandaloneHost::startMidi
    // answers that one by not calling open() at all (see MidiInputSelection),
    // which is what lets an empty list keep its other meaning down here.
    //
    // Matched exactly, never as a substring. Two of the three platforms derive
    // an id from the device name, and a name is a prefix of its own
    // disambiguated duplicate - "Novation SL: MIDI 1" sits inside "Novation SL:
    // MIDI 1 #2" - so a substring match connects a device the user unticked.
    bool wants(const std::string& deviceId)
    {
        ++available_;

        return selection_.empty()
            || std::find(selection_.begin(), selection_.end(), deviceId) != selection_.end();
    }

    // Exactly ONE of the next two per input wants() said yes to. failure() below
    // tells "nothing was even attempted" from "everything attempted refused"
    // purely by which of them were called, so an input dropped without saying
    // which way it went would make it describe the wrong situation.

    // Once the port is open and delivering - not merely once wants() said yes.
    // "Could not be opened" and "is not present" are different sentences, and
    // only the driver knows which of the two happened.
    void opened() { ++opened_; }

    // ... and the other outcome, named rather than skipped in silence: a port
    // another application is holding is the one thing here a user can go and
    // free. `deviceName` is what to call it in front of the user - the display
    // name where the driver's id is not one, since CoreMIDI's is a number.
    //
    // Only the FIRST is kept. Every port on a machine refusing for one reason is
    // one thing that went wrong rather than a list, and the settings pane has a
    // line for it, not a paragraph.
    void failed(const std::string& deviceName)
    {
        if (firstFailure_.empty())
            firstFailure_ = "Could not open MIDI input \"" + deviceName + "\".";
    }

    // What open() returns: whether the driver did what it was ASKED, which is a
    // different question from whether anything ended up connected.
    //
    // Asked to "connect everything readable" - the empty selection, which is
    // what an app nobody has configured asks for - the request is satisfied by
    // whatever happens to be there, including nothing. A machine with no MIDI
    // inputs at all is an ordinary way to run a standalone synth: a laptop with
    // no keyboard plugged in, with nothing for the user to act on, and the
    // command channel can inject MIDI regardless. Only inputs that exist and
    // then refuse are a failure of that request.
    //
    // Asked to "connect exactly these", failure is getting none of them, and the
    // reason does not change the answer: absent, and present but refusing, are
    // equally a device the user chose and did not get. That is the case worth a
    // sentence, because it is the one the user can act on.
    //
    // What does NOT enter into it is anything ELSE on the machine, and that part
    // has to be said out loud, because available_ is the number that differs
    // most between platforms. A bare Windows box enumerates no MIDI inputs
    // (midiInGetNumDevs() == 0), and so does a bare Mac (the IAC driver is off
    // until someone turns it on) - but a bare desktop Linux has ALSA's "Midi
    // Through", snd-seq-dummy at client 14, wherever that module is loaded,
    // which on a desktop distribution is everywhere. It is READ|SUBS_READ and
    // not NO_EXPORT, so enumeration lists it. This predicate used to be
    // `opened_ > 0 || available_ == 0`, and under it one physical event - the
    // user unplugging the single keyboard they had ticked - was a silent success
    // on Windows and macOS and a reported failure on Linux. Asking what was
    // SELECTED rather than what is plugged in is what makes the three agree.
    bool success() const
    {
        if (opened_ > 0)
            return true;

        // Nothing was connected, so this is a success only if nothing was really
        // asked for: "everything readable", on a machine with no MIDI inputs to
        // read. (An empty selection wants every enumerated input, so available_
        // == 0 also means nothing was skipped and nothing refused - there was
        // simply nothing there.)
        return selection_.empty() && available_ == 0;
    }

    // What lastError() should say when success() is false. There are exactly two
    // situations to be in, because opened_ is zero and an input was therefore
    // either attempted and refused, or never attempted at all.
    //
    //   * Attempted and refused - all of them, or success() would be true. The
    //     driver named the first through failed(), and a named port beats any
    //     summary of the same fact. This is why there is no summary here: two
    //     carefully worded ones used to sit in this function ("No MIDI input
    //     could be opened.", "None of the selected MIDI inputs could be
    //     opened.") and neither could ever reach a screen, because all three
    //     drivers name the port that refused and the callers preferred the name.
    //
    //   * Never attempted, which is what an empty firstFailure_ means: the
    //     selection named inputs and not one of them is on this machine. No
    //     driver can say anything more specific - it opened no port - so this
    //     sentence is all there is. It cannot be reached with an EMPTY
    //     selection, which is what makes the word "selected" honest: an empty
    //     one wants every input there is, so attempting none of them would mean
    //     available_ == 0, which success() has already called a success.
    std::string failure() const
    {
        if (!firstFailure_.empty())
            return firstFailure_;

        return "None of the selected MIDI inputs are present.";
    }

private:
    std::vector<std::string> selection_;

    // The sentence failed() built for the first input that refused, empty until
    // one does. Doubles as the record of whether anything was attempted at all -
    // see failure().
    std::string firstFailure_;

    int available_ = 0;   // inputs the driver enumerated
    int opened_    = 0;   // ... of those, the ones that opened
};

} // namespace standalone
} // namespace gmpi
