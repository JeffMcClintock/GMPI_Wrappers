#pragma once

// MIDI input for the Windows standalone, through winmm.
//
// winmm rather than WinRT MIDI (Windows.Devices.Midi), for two reasons that
// both come down to what a standalone is for. The WinRT API is asynchronous
// from the first call and needs an initialised apartment plus a completion
// pump, which is a lot of machinery to put between a keyboard and a synth; and
// it cannot see the class-compliant ports that MIDI-over-USB devices present
// unless the user has paired them. midiIn* sees every port the driver stack
// knows about, synchronously, in about forty lines.
//
// Input only, matching MidiDriverAlsa. A standalone wrapping one GMPI plugin
// has nowhere for MIDI output to go that the user could have asked for, so the
// output half is absent rather than present and unused.

#include <atomic>
#include <string>
#include <vector>

#include "../AudioMidiDevices.h"

namespace gmpi
{
namespace standalone
{

class MidiDriverWin : public MidiDriver
{
public:
    ~MidiDriverWin() override;

    std::vector<DeviceInfo> inputs() override;

    // An empty list opens EVERY port. That is what makes a freshly-installed
    // standalone play the moment a keyboard is plugged in, and the settings
    // pane distinguishes "never configured" from "configured to nothing" so
    // that turning every input off actually turns them off.
    //
    // Which of the possible outcomes counts as failure, and what lastError()
    // then says, is MidiOpenTally's to decide rather than this driver's. Its
    // rule keys on what was ASKED for rather than on what is plugged in, and
    // the case that changes here is the empty list above: on a machine with no
    // MIDI hardware at all that is now a SUCCESS with nothing open, where this
    // driver used to call it a failed open and put a sentence on the settings
    // page in the one situation where nothing was wrong.
    bool open(const std::vector<std::string>& inputIds, MidiCallback* client) override;

    // Closes on the way out of a failed open() too, unlike MidiDriverAlsa: a
    // winmm handle is not a thing another program can route into, so there is
    // nothing to be gained by keeping one.
    void close() override;

    std::string lastError() const override { return lastError_; }

    // One open winmm port: the sysex buffers it owns, and the parser that
    // rejoins them. midiInAddBuffer hands the driver memory it writes into and
    // returns to us later, so the headers have to outlive every callback the
    // port can still make - which is why they live here rather than on the stack
    // of whatever opened it. The parser is per-port for a different reason, and
    // the .cpp says which.
    //
    // Public only as a NAME. The definition is in the .cpp, where the winmm
    // callback - a free function with a C signature, which could not otherwise
    // reach a private nested type - needs it. Nothing outside can do anything
    // with an incomplete type, so this concedes no encapsulation; the
    // alternative was pulling <windows.h> into this header to spell the
    // callback's signature, which every other driver header here avoids.
    struct Port;

private:
    std::vector<Port*> ports_;
    MidiCallback* client_{};

    // Read by the callback thread, written by close(). Closing a winmm port is
    // a three-step dance (reset, unprepare, close) during which the driver can
    // still deliver the buffers it is returning, so the callback needs a way to
    // know it must stop handing them on. Each Port holds a pointer to this.
    std::atomic<bool> running_{ false };

    std::string lastError_;
};

} // namespace standalone
} // namespace gmpi
