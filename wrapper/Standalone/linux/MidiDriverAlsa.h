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
#include <string>
#include <thread>
#include <vector>

#include "../AudioMidiDevices.h"

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
    bool open(const std::vector<std::string>& inputIds, MidiCallback* client) override;

    void close() override;

    std::string lastError() const override { return lastError_; }

private:
    void readerLoop();
    void connectInputs(const std::vector<std::string>& inputIds);

    snd_seq_t*        seq_{};
    int               inputPort_ = -1;
    snd_midi_event_t* decoder_{};   // seq event -> raw bytes

    std::thread       reader_;
    std::atomic<bool> readerRun_{ false };

    MidiCallback* client_{};
    std::string lastError_;
};

} // namespace standalone
} // namespace gmpi
