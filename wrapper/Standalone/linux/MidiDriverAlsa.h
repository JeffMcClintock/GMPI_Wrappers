#pragma once

// ALSA-sequencer MIDI input for the Linux standalone.
//
// Adapted from SynthEdit's SynthEditWayland/MidiDriverAlsa.cpp - the port
// enumeration, the non-blocking handle and the poll-based reader thread are
// its design, and the comments explaining WHY are worth keeping.
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
