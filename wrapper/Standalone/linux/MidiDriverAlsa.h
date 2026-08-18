#pragma once

// ALSA-sequencer MIDI input for the Linux standalone.
//
// Adapted from SynthEdit's SynthEditWayland/MidiDriverAlsa.cpp - the port
// enumeration, the non-blocking handle and the poll-based reader thread are
// its design, and the comments explaining WHY are worth keeping.
//
// The device ids are the one deliberate divergence: SynthEdit keys them on the
// sequencer address, which ALSA reassigns when hardware is replugged, so the
// ids here are built from the client and port names instead. AudioMidiDevices.h
// asks that an id still select the same device after a reboot and after other
// hardware has come and gone around it; an address survives being written to
// the settings file and read back, but by then it can point at another keyboard.
//
// An id here is that name with "ALSA:MIDIIN:" in front of it; MidiDriverWin
// persists the bare name. The prefix is the only difference between the two
// schemes - the names themselves are disambiguated by the same rule on both,
// each claimed against the names already emitted so that a device genuinely
// called "Foo #2" cannot be handed the same id as a synthesized second "Foo".
// What the prefix buys is in the .cpp, beside the line that builds the id.
//
// Only CoreMIDI has a handle of its own to persist, kMIDIPropertyUniqueID, and
// it shows the raw display name beside it - so two identical keyboards really
// do give a macOS user the same name twice. Here and on Windows the id is
// derived from the name instead, which is why the disambiguated form is the one
// the user sees and the two rows can be told apart.
//
// Input only, deliberately. The SynthEdit driver also schedules output through
// an ALSA queue, which exists because a SynthEdit document can contain a MIDI
// Out module. A standalone wrapping one GMPI plugin has nowhere for MIDI out
// to go that the user could have asked for, so the queue, the encoder and the
// whole time-sync apparatus are absent rather than present and unused.

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "../AudioMidiDevices.h"
#include "../MidiStreamParser.h"

// alsa/asoundlib.h stays out of this header (it leaks macros); the handle
// types are opaque here.
typedef struct _snd_seq snd_seq_t;
typedef struct snd_midi_event snd_midi_event_t;

namespace gmpi
{
namespace standalone
{

class MidiDriverAlsa : public MidiDriver
{
public:
    ~MidiDriverAlsa() override;

    std::vector<DeviceInfo> inputs() override;

    // An empty list connects EVERY readable port. That is what makes a
    // freshly-installed standalone play the moment a keyboard is plugged in,
    // and the settings pane distinguishes "never configured" from "configured
    // to nothing" so that turning every input off actually turns them off.
    //
    // Which of the possible outcomes counts as failure, and what lastError()
    // then says, is MidiOpenTally's to decide rather than this driver's. This
    // driver used to decide nothing at all: it returned true as soon as the
    // sequencer handle opened, so a saved selection matching no device present
    // was a success with nothing listening, and the settings pane had nothing to
    // show in the very case where winmm and CoreMIDI both had a sentence.
    //
    // FALSE HERE DOES NOT MEAN CLOSED, and this is the only driver of the three
    // where it does not: the sequencer client stays up, subscribable, with its
    // reader running, so that a user whose saved selection has gone stale can
    // still aconnect(1) a keyboard to it. The .cpp says why at the line that
    // does it. close() remains the caller's job either way, and is idempotent.
    bool open(const std::vector<std::string>& inputIds, MidiCallback* client) override;

    void close() override;

    std::string lastError() const override { return lastError_; }

private:
    void readerLoop();

    // Takes the tally rather than the id list, because the list is inside it and
    // subscribing without counting the outcome is the bug above.
    void connectInputs(MidiOpenTally& tally);

    snd_seq_t*        seq_{};
    int               inputPort_ = -1;
    snd_midi_event_t* decoder_{};   // seq event -> raw bytes

    std::thread       reader_;
    std::atomic<bool> readerRun_{ false };

    // One parser per SOURCE, matching the per-source parsers macOS keeps and the
    // per-port one Windows keeps. Every subscription feeds the same sequencer
    // input port here, so an event's source address is the only thing that
    // separates two keyboards' streams; one shared parser would splice one
    // device's dump around the other's notes.
    //
    // Keyed on client<<8|port rather than on snd_seq_addr_t, so that
    // asoundlib.h - which leaks macros, see above - stays out of this header.
    // Touched only by the reader thread, which is not the audio thread, so the
    // one-off allocation when a source is first seen is affordable here.
    std::map<uint16_t, MidiStreamParser> parsers_;

    MidiCallback* client_{};
    std::string lastError_;
};

} // namespace standalone
} // namespace gmpi
