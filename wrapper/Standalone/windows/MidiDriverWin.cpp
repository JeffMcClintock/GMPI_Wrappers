#include "MidiDriverWin.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <set>
#include <utility>

#include <windows.h>
#include <mmeapi.h>

#include "helpers/unicode_conversion.h"

#include "../MidiStreamParser.h"

namespace gmpi
{
namespace standalone
{

namespace
{

// Two buffers per port, so the driver always has one to fill while we are
// handing the other on. 1 kB each: long enough for the dumps a controller sends
// unprompted, and winmm splits anything larger across several MIM_LONGDATA
// callbacks rather than failing, so a bigger buffer buys only memory.
constexpr int  kSysexBuffers     = 2;
constexpr int  kSysexBufferBytes = 1024;

// The one place ports are enumerated, so that the id a settings file holds and
// the winmm index open() passes to midiInOpen can never disagree.
//
// They WOULD disagree if inputs() built the list and open() re-derived the
// index from a position in it: a device whose caps cannot be read is skipped
// here, and from that point on list position and device index differ by one -
// which opens the wrong keyboard, silently, only on machines with a
// misbehaving driver.
struct EnumeratedPort
{
    UINT index;         // what midiInOpen wants
    std::string id;     // what the settings file holds
};

std::vector<EnumeratedPort> enumeratePorts()
{
    std::vector<EnumeratedPort> result;

    const UINT count = ::midiInGetNumDevs();
    std::set<std::string> emitted;

    for (UINT i = 0; i < count; ++i)
    {
        MIDIINCAPSW caps{};
        if (::midiInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
            continue;

        const auto base = gmpi::unicode::to_utf8(caps.szPname);

        // The id is the NAME, not the index. Indices renumber every time a
        // device is plugged in or removed, so a settings file keyed on them
        // would silently start listening to the wrong keyboard; a name
        // survives that. macOS instead has kMIDIPropertyUniqueID, which winmm
        // has no equivalent of.
        //
        // Two identical interfaces produce identical names, which is what the
        // suffix is for - deterministic given the same set of devices, and
        // degrading to "the first one with this name" when the set has changed
        // since the settings were written. Each name is claimed against the
        // ones already EMITTED, not against the raw ones: a device genuinely
        // called "Foo #2" would otherwise be handed the same id as the
        // synthesized second "Foo", and open() - which matches ids exactly -
        // would then open both of them from one saved id. MidiDriverAlsa
        // disambiguates identically, so a duplicate name reads the same on
        // both platforms.
        std::string name = base;
        for (int ordinal = 2; !emitted.insert(name).second; ++ordinal)
            name = base + " #" + std::to_string(ordinal);

        result.push_back({ i, name });
    }

    return result;
}

} // namespace

// Everything the winmm callback needs, so that it needs nothing from the driver
// object itself - which is what lets it be a free function with a C signature.
struct MidiDriverWin::Port
{
    HMIDIIN handle{};
    MidiCallback* client{};
    const std::atomic<bool>* running{};

    std::array<MIDIHDR, kSysexBuffers> headers{};
    std::array<std::array<uint8_t, kSysexBufferBytes>, kSysexBuffers> buffers{};

    // Per PORT, not per driver, exactly as macOS keeps one per connected source:
    // two keyboards' callbacks interleave, and a shared parser would splice one
    // device's dump around the other's notes.
    MidiStreamParser parser;
};

namespace
{

// The winmm callback. Runs on a driver thread, inside winmm's own lock.
//
// MSDN's list of functions that are legal here is short and does not include
// midiInAddBuffer, which the sysex path below calls. That restriction dates
// from drivers that ran this at interrupt time; on any Windows this app
// supports, re-adding the buffer from the callback is what every MIDI library
// in use does (RtMidi and PortMidi both), and the alternative - a helper thread
// whose only job is to hand buffers back - adds a thread and a queue to solve a
// problem that has not existed for twenty years. What is NOT done here is
// anything that could block: the short-message path pushes into the host's
// lock-free FIFO and returns.
void CALLBACK midiInProc(HMIDIIN, UINT message, DWORD_PTR instance,
                         DWORD_PTR param1, DWORD_PTR param2)
{
    (void)param2;   // timestamp: see MidiCallback::onMidiIn on why it is unused

    auto* const port = reinterpret_cast<MidiDriverWin::Port*>(instance);
    if (!port || !port->client || !port->running || !port->running->load(std::memory_order_acquire))
        return;

    switch (message)
    {
    case MIM_DATA:
    {
        const uint8_t packed[3] = {
            static_cast<uint8_t>(param1 & 0xFF),
            static_cast<uint8_t>((param1 >> 8) & 0xFF),
            static_cast<uint8_t>((param1 >> 16) & 0xFF),
        };

        // Straight on rather than through the parser: winmm has already done the
        // splitting for this path - one MIM_DATA is one complete short message,
        // with the status byte present even when the device used running status.
        // The length table is the parser's, though, so the two cannot drift.
        if (const size_t length = midiMessageLength(packed[0]); length > 0)
            port->client->onMidiIn(packed, static_cast<int>(length));

        break;
    }

    case MIM_LONGDATA:
    {
        auto* const header = reinterpret_cast<MIDIHDR*>(param1);
        if (!header)
            break;

        // dwBytesRecorded == 0 is a buffer being HANDED BACK by midiInReset,
        // not data. Re-adding it there would fight the shutdown that asked for
        // it and leave midiInClose failing with MIDIERR_STILLPLAYING forever.
        if (header->dwBytesRecorded == 0)
            break;

        // Through the parser, NOT handed on verbatim. A dump larger than
        // kSysexBufferBytes arrives as several MIM_LONGDATA callbacks, and only
        // the first of them begins with 0xF0 - every continuation chunk starts
        // on a data byte. Passed straight to the plugin those chunks are
        // malformed by construction: MidiConverter2 reads byte 0 as a status and
        // decodes the middle of somebody's patch dump as notes. The parser
        // rejoins them, which is also what gives Windows the same cap and the
        // same all-or-nothing termination rule as macOS and Linux.
        port->parser.parse(reinterpret_cast<const uint8_t*>(header->lpData),
                           static_cast<size_t>(header->dwBytesRecorded),
                           [port](const uint8_t* message, size_t size)
                           {
                               port->client->onMidiIn(message, static_cast<int>(size));
                           });

        header->dwBytesRecorded = 0;
        ::midiInAddBuffer(port->handle, header, sizeof(MIDIHDR));
        break;
    }

    default:
        break;   // MIM_OPEN, MIM_CLOSE, MIM_ERROR, MIM_LONGERROR
    }
}

} // namespace

MidiDriverWin::~MidiDriverWin()
{
    close();
}

std::vector<DeviceInfo> MidiDriverWin::inputs()
{
    std::vector<DeviceInfo> result;

    // Id and display name are the same string here. winmm exposes exactly one
    // name per port and it is already the one on the box; inventing a
    // prettier one would only make the settings page disagree with Device
    // Manager.
    for (const auto& port : enumeratePorts())
        result.push_back({ port.id, port.id });

    return result;
}

bool MidiDriverWin::open(const std::vector<std::string>& inputIds, MidiCallback* client)
{
    close();

    lastError_.clear();

    if (!client)
        return false;

    client_  = client;
    running_ = true;

    // The selection test and the outcome count in one object, shared with
    // MidiDriverAlsa and MidiDriverCoreMidi - see MidiOpenTally.
    MidiOpenTally tally(inputIds);

    for (const auto& candidate : enumeratePorts())
    {
        if (!tally.wants(candidate.id))
            continue;

        auto port = std::make_unique<Port>();
        port->client  = client_;
        port->running = &running_;

        const MMRESULT result = ::midiInOpen(
            &port->handle,
            candidate.index,
            reinterpret_cast<DWORD_PTR>(&midiInProc),
            reinterpret_cast<DWORD_PTR>(port.get()),
            CALLBACK_FUNCTION);

        if (result != MMSYSERR_NOERROR)
        {
            // One unavailable port (already open in a DAW, most likely) must
            // not cost the user the others - so the loop goes on, and the tally
            // decides later whether this mattered. The id doubles as the display
            // name here; see inputs().
            tally.failed(candidate.id);
            continue;
        }

        // Sysex buffers, prepared and handed to the driver before the port
        // starts. A port with none simply drops long messages; a port whose
        // buffers were added after midiInStart can drop the first one.
        for (int b = 0; b < kSysexBuffers; ++b)
        {
            MIDIHDR& header = port->headers[b];
            header.lpData         = reinterpret_cast<LPSTR>(port->buffers[b].data());
            header.dwBufferLength = kSysexBufferBytes;

            if (::midiInPrepareHeader(port->handle, &header, sizeof(MIDIHDR)) == MMSYSERR_NOERROR)
                ::midiInAddBuffer(port->handle, &header, sizeof(MIDIHDR));
        }

        ::midiInStart(port->handle);

        ports_.push_back(port.release());
        tally.opened();
    }

    if (!tally.success())
    {
        lastError_ = tally.failure();

        close();
        return false;
    }

    // lastError_ is left EMPTY even if a port along the way refused: at least
    // one input is live, the app is playable, and a stale sentence on the
    // settings page would outlive the condition that caused it. The tally is
    // asked for its sentence only on the arm above.
    return true;
}

void MidiDriverWin::close()
{
    // Before anything else: the callback can fire throughout the teardown
    // below (midiInReset exists precisely to make it fire), and from here on it
    // must hand nothing on to a client the caller may be destroying.
    running_ = false;

    for (Port* port : ports_)
    {
        if (!port)
            continue;

        if (port->handle)
        {
            ::midiInStop(port->handle);

            // Returns every buffer still with the driver, each as an
            // MIM_LONGDATA with dwBytesRecorded == 0. Without it the
            // unprepare below fails and the handle leaks.
            ::midiInReset(port->handle);

            for (auto& header : port->headers)
            {
                if (header.dwFlags & MHDR_PREPARED)
                {
                    // midiInReset is asynchronous in the sense that a buffer
                    // can still be in the driver's hands for a moment after it
                    // returns. A short bounded retry is the documented remedy;
                    // spinning forever would hang the app on a broken driver,
                    // and leaking one header is the lesser outcome.
                    for (int attempt = 0; attempt < 100; ++attempt)
                    {
                        if (::midiInUnprepareHeader(port->handle, &header, sizeof(MIDIHDR))
                            != MIDIERR_STILLPLAYING)
                        {
                            break;
                        }
                        ::Sleep(1);
                    }
                }
            }

            ::midiInClose(port->handle);
        }

        delete port;
    }

    ports_.clear();
    client_ = nullptr;
}

} // namespace standalone
} // namespace gmpi
