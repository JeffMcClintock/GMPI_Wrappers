#pragma once

// MIDI input for the macOS standalone, through CoreMIDI.
//
// Input only, matching MidiDriverAlsa and MidiDriverWin. A standalone wrapping
// one GMPI plugin has nowhere for MIDI output to go that the user could have
// asked for, so the output half is absent rather than present and unused.
//
// THE OLD API ON PURPOSE. CoreMIDI has two input paths since macOS 11:
// MIDIInputPortCreateWithProtocol, which delivers MIDI 2.0 UMP words, and the
// deprecated MIDIInputPortCreate, which delivers MIDI 1.0 bytes in a
// MIDIPacketList. What the far end wants is bytes - StandaloneHost::onMidiIn
// takes "raw MIDI 1.0 bytes as they arrived" and gmpi::midi::MidiConverter2 is
// what turns them into the UMP events the plugin's MIDI pin carries. Taking the
// UMP path here would mean converting 1.0 to UMP in CoreMIDI and back to bytes
// for the converter to convert to UMP again. So the deprecated call is the
// RIGHT one for this seam, and the deprecation warning is suppressed at the
// call site with this note rather than repo-wide.
//
// THREADING. CoreMIDI delivers on a thread of its own, high-priority but not
// the audio thread, which is exactly the arrangement AudioMidiDevices.h
// describes: MidiCallback::onMidiIn is called there, and StandaloneHost pushes
// into a lock-free FIFO the audio thread drains.

#include <atomic>
#include <string>
#include <vector>

#include <CoreMIDI/CoreMIDI.h>

#include "../AudioMidiDevices.h"

namespace gmpi
{
namespace standalone
{

class MidiDriverCoreMidi : public MidiDriver
{
public:
    MidiDriverCoreMidi() = default;
    ~MidiDriverCoreMidi() override;

    std::vector<DeviceInfo> inputs() override;

    // An empty list connects EVERY source. That is what makes a freshly-
    // installed standalone play the moment a keyboard is plugged in, and the
    // settings pane distinguishes "never configured" from "configured to
    // nothing" so that turning every input off actually turns them off.
    //
    // Which of the possible outcomes counts as failure, and what lastError()
    // then says, is MidiOpenTally's to decide rather than this driver's. Its
    // rule keys on what was ASKED for rather than on what is plugged in, and
    // the case that changes here is the empty list above: on a machine with no
    // MIDI hardware at all that is now a SUCCESS with nothing connected, where
    // this driver used to call it a failed open and put a sentence on the
    // settings page in the one situation where nothing was wrong.
    //
    // The sources this connects are exactly the ones inputs() lists, which is
    // not automatic - see the skip in open().
    bool open(const std::vector<std::string>& inputIds, MidiCallback* client) override;

    // Closes on the way out of a failed open() too, unlike MidiDriverAlsa: a
    // CoreMIDI input port is not published to other applications - only a
    // virtual DESTINATION would be, and this driver creates none - so there is
    // nothing to be gained by keeping one.
    void close() override;

    std::string lastError() const override { return lastError_; }

    // One connected source, plus the running-message parser its stream needs -
    // CoreMIDI hands a packet's bytes over as a run that may hold several
    // messages, and a long sysex arrives split across packets, so the split has
    // to be per-source state rather than per-call.
    //
    // Public only as a NAME, exactly as MidiDriverWin::Port is. The definition
    // is in the .cpp, where the read proc - a free function with a C signature -
    // needs it; nothing outside can do anything with an incomplete type, so
    // this concedes no encapsulation.
    struct Source;

private:
    static void readProc(const MIDIPacketList* packets, void* refCon, void* connRefCon);

    MIDIClientRef client_{};
    MIDIPortRef   port_{};

    // CoreMIDI holds each Source's ADDRESS for the life of its connection, so
    // these are owned by pointer and deleted in close(), after the disconnect.
    std::vector<Source*> sources_;

    MidiCallback* callback_{};

    // Read by CoreMIDI's thread, written by close(). Disconnecting a source
    // does not retract packets already in flight, so the read proc needs a way
    // to know it must stop handing them on before the callback goes away.
    std::atomic<bool> running_{ false };

    std::string lastError_;
};

} // namespace standalone
} // namespace gmpi
