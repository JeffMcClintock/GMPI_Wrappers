#include "MidiDriverAlsa.h"

#include <algorithm>
#include <set>
#include <poll.h>

#include <alsa/asoundlib.h>

namespace gmpi
{
namespace standalone
{

namespace
{

constexpr unsigned kReadable = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;

// The one place ports are enumerated, so that the id a settings file holds and
// the sequencer address subscribed to are derived side by side, from one query
// of one port. MidiDriverWin is built the same way and says why: two walks
// deriving an id independently is how the id in the settings file and the
// device actually opened drift apart.
//
// The two callers do pass different handles, so the client each walk skips as
// its own differs - the throwaway handle in inputs(), seq_ in connectInputs().
// The lists still match because neither of those clients owns a readable port:
// the throwaway creates no port at all, and seq_'s only port is write-only.
// Give this client a readable port and they would stop matching.
struct EnumeratedPort
{
    snd_seq_addr_t addr;   // what snd_seq_connect_from wants
    std::string    id;     // what the settings file holds
    std::string    name;   // what the user sees
};

// The handle is a parameter because enumeration happens against two of them:
// inputs() must work before open() and after close(), when seq_ is null, so it
// opens a throwaway handle of its own, and connectInputs() must walk the same
// list through the handle it is subscribing.
std::vector<EnumeratedPort> enumeratePorts(snd_seq_t* seq)
{
    std::vector<EnumeratedPort> result;
    if (!seq)
        return result;

    snd_seq_client_info_t* client{};
    snd_seq_port_info_t* port{};
    snd_seq_client_info_alloca(&client);
    snd_seq_port_info_alloca(&port);

    std::set<std::string> emitted;

    snd_seq_client_info_set_client(client, -1);
    while (snd_seq_query_next_client(seq, client) >= 0)
    {
        const int clientId = snd_seq_client_info_get_client(client);
        if (clientId == SND_SEQ_CLIENT_SYSTEM || clientId == snd_seq_client_id(seq))
            continue;

        snd_seq_port_info_set_client(port, clientId);
        snd_seq_port_info_set_port(port, -1);
        while (snd_seq_query_next_port(seq, port) >= 0)
        {
            const unsigned caps = snd_seq_port_info_get_capability(port);
            if ((caps & kReadable) != kReadable || (caps & SND_SEQ_PORT_CAP_NO_EXPORT))
                continue;

            // A port that can be read FROM is one of our INPUT choices: the
            // capability names are from the port's point of view, not ours.
            const std::string base = std::string(snd_seq_client_info_get_name(client))
                                   + ": " + snd_seq_port_info_get_name(port);

            // The id is the NAME, not the sequencer address. Client numbers are
            // handed out in card and connection order, so they shift whenever a
            // device is added, removed, or simply enumerates differently at
            // boot, and a settings file keyed on them would silently start
            // listening to the wrong keyboard; the names survive that. This is
            // MidiDriverWin's scheme; macOS instead has kMIDIPropertyUniqueID,
            // which ALSA has no equivalent of. The prefix is what keeps a saved
            // id from being mistaken for another driver's.
            //
            // Not every name is stable, though: ALSA calls a client "Client-<n>"
            // until its owner calls snd_seq_set_client_name, so an application
            // that publishes a readable port without naming itself still lands
            // on an id built from a number that moves. Hardware always carries
            // a real name, and so does any application that bothered to set
            // one; for the rest there is nothing better here to key on.
            //
            // Two identical interfaces produce identical names, which is what
            // the suffix is for - deterministic given the same set of devices,
            // and degrading to "the first one with this name" when the set has
            // changed since the settings were written. Each name is claimed
            // against the ones already EMITTED, not against the raw ones: a
            // device genuinely called "Foo #2" would otherwise be handed the
            // same id as the synthesized second "Foo", and connectInputs would
            // subscribe to both of them from one saved id.
            std::string name = base;
            for (int ordinal = 2; !emitted.insert(name).second; ++ordinal)
                name = base + " #" + std::to_string(ordinal);

            result.push_back({ *snd_seq_port_info_get_addr(port), "ALSA:MIDIIN:" + name, name });
        }
    }

    return result;
}

} // namespace

MidiDriverAlsa::~MidiDriverAlsa()
{
    close();
}

std::vector<DeviceInfo> MidiDriverAlsa::inputs()
{
    std::vector<DeviceInfo> results;

    // A throwaway connection: enumeration must work before open() and after
    // close(), so it cannot rely on seq_ being open.
    snd_seq_t* seq{};
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0)
        return results;

    for (const auto& port : enumeratePorts(seq))
        results.push_back({ port.id, port.name });

    snd_seq_close(seq);
    return results;
}

void MidiDriverAlsa::connectInputs(MidiOpenTally& tally)
{
    for (const auto& port : enumeratePorts(seq_))
    {
        // The selection test, and the count of what it was offered, both belong
        // to the tally - which is shared with MidiDriverWin and
        // MidiDriverCoreMidi so that the three cannot answer differently.
        if (!tally.wants(port.id))
            continue;

        // The subscription's result, which this driver used to discard. That is
        // what left Linux reporting a success whatever happened: a saved id that
        // matches nothing, or a port already subscribed exclusively elsewhere,
        // both ended with an open sequencer, no keyboard, and nothing said.
        if (snd_seq_connect_from(seq_, inputPort_, port.addr.client, port.addr.port) < 0)
        {
            // One unreachable port must not cost the user the others, exactly as
            // on the other two - so the loop goes on, and the tally decides later
            // whether this mattered. The name rather than the id, which carries
            // an "ALSA:MIDIIN:" prefix no user needs to read.
            tally.failed(port.name);
            continue;
        }

        tally.opened();
    }
}

bool MidiDriverAlsa::open(const std::vector<std::string>& inputIds, MidiCallback* client)
{
    close();

    lastError_.clear();
    client_ = client;

    if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_INPUT, 0) < 0)
    {
        seq_ = nullptr;
        lastError_ = "ALSA MIDI: could not open the sequencer";
        return false;
    }

    snd_seq_set_client_name(seq_, "GMPI Standalone");

    // Non-blocking, and not optional: with a blocking handle the reader's
    // drain loop parks INSIDE snd_seq_event_input once the queue empties
    // (kernel-side, snd_seq_fifo_cell_out), where readerRun_ is never checked
    // again. close() would then join a thread that never returns and the app
    // would hang on exit. poll() is the only place this thread waits.
    snd_seq_nonblock(seq_, 1);

    inputPort_ = snd_seq_create_simple_port(seq_, "input",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

    if (inputPort_ < 0)
    {
        lastError_ = "ALSA MIDI: could not create the input port";
        snd_seq_close(seq_);
        seq_ = nullptr;
        return false;
    }

    MidiOpenTally tally(inputIds);
    connectInputs(tally);

    // Sized for the longest sysex a controller is likely to send.
    snd_midi_event_new(4096, &decoder_);

    // Every message complete, no running status: the decoder's default elides
    // repeated status bytes, so the second note of a chord arrives as two
    // naked data bytes and anything downstream drops it.
    snd_midi_event_no_status(decoder_, 1);

    readerRun_ = true;
    reader_ = std::thread([this] { readerLoop(); });

    // The client, its port, the decoder and the reader are all up BEFORE the
    // verdict below, and stay up whichever way it goes. That is the one place
    // this driver parts company with MidiDriverWin and MidiDriverCoreMidi, which
    // both close() on the way out of a failed open.
    //
    // What is being kept is an escape hatch rather than a formality. "GMPI
    // Standalone" is a sequencer client with a writable, subscribable port, so
    // it shows up in aconnect(1) and in every patchbay on the machine, and a
    // user whose saved selection has gone stale - the exact situation the tally
    // is about to report - can wire a keyboard to it by hand without touching
    // the app. Closing it here would take away the one route they had left. The
    // reader thread has to stay with it, or a subscription made from outside
    // would deliver into a queue nobody drains.
    //
    // Safe to leave open because a false return promises nothing about the
    // driver's state (see MidiDriver::open) and close() is idempotent: open()
    // itself begins with one, so a second attempt takes this down before
    // building anything, and StandaloneHost::stopMidi takes it down at shutdown.
    if (!tally.success())
    {
        lastError_ = tally.failure();
        return false;
    }

    // lastError_ is left EMPTY even if a port along the way refused: at least one
    // input is subscribed, the app is playable, and a stale sentence on the
    // settings page would outlive the condition that caused it. The tally is
    // asked for its sentence only on the arm above, as on the other two.
    return true;
}

void MidiDriverAlsa::close()
{
    readerRun_ = false;

    if (reader_.joinable())
        reader_.join();     // poll() has a timeout, so this returns promptly

    if (seq_)
    {
        snd_seq_close(seq_);   // ports die with the client
        seq_ = nullptr;
    }
    inputPort_ = -1;

    if (decoder_)
    {
        snd_midi_event_free(decoder_);
        decoder_ = nullptr;
    }

    // After the reader has been joined, so nothing is mid-parse. A dump that was
    // still arriving when the user changed devices must not resume into the next
    // session's stream.
    parsers_.clear();

    client_ = nullptr;
}

void MidiDriverAlsa::readerLoop()
{
    // Own thread rather than the app's poll loop: MIDI must keep flowing while
    // the UI thread is busy in a menu grab or a long repaint. The callback
    // pushes to a lock-free FIFO, so this never blocks the audio thread.
    const int fdCount = snd_seq_poll_descriptors_count(seq_, POLLIN);
    std::vector<pollfd> fds(static_cast<size_t>((std::max)(1, fdCount)));
    snd_seq_poll_descriptors(seq_, fds.data(), static_cast<unsigned>(fds.size()), POLLIN);

    unsigned char bytes[4096];

    while (readerRun_)
    {
        if (poll(fds.data(), fds.size(), 100) <= 0)
            continue;

        snd_seq_event_t* ev{};
        while (snd_seq_event_input(seq_, &ev) >= 0 && ev)
        {
            if (client_ && decoder_)
            {
                const long n = snd_midi_event_decode(decoder_, bytes, sizeof(bytes), ev);
                if (n > 0)
                {
                    // Through the parser rather than straight on, and NOT only
                    // because of sysex: ONE SEQUENCER EVENT IS NOT ONE MIDI
                    // MESSAGE, in either direction.
                    //
                    // Several event types decode to a RUN of complete messages.
                    // Measured against libasound 1.2.6.1, with the no_status
                    // setting above: SND_SEQ_EVENT_CONTROL14 comes back as SIX
                    // bytes - controller 1 at 0x2000 decodes to "b0 01 40 b0 21
                    // 00", the MSB controller and its LSB partner, two CC
                    // messages - and NONREGPARAM and REGPARAM as TWELVE, the
                    // four CCs that spell an NRPN or an RPN out on the wire
                    // (99/98/6/38 and 101/100/6/38). Handed on whole, the far
                    // end decodes byte 0 and converts that one message, so a
                    // 14-bit controller lost its LSB and an NRPN arrived as one
                    // CC of four. Splitting the run here is what fixes that, and
                    // it is ordinary traffic rather than an edge case.
                    //
                    // Ordinary from SOFTWARE, to be exact. These are plain
                    // members of the sequencer's event set and any client can
                    // compose one - snd_seq_ev_set_control14 does it in a line -
                    // but the encoder that carries a hardware keyboard's bytes
                    // in never builds them: snd_midi_event_encode_byte returns
                    // CONTROLLER for every CC of a 14-bit pair or an NRPN, one
                    // event each (measured the same way). So it is the other
                    // applications on the sequencer this covers, not a
                    // keyboard's own cable - and it is a hazard the other two
                    // platforms do not have at all, because winmm and CoreMIDI
                    // only ever hand over wire bytes.
                    //
                    // And a long dump arrives the other way round, as a RUN OF
                    // EVENTS: the decoder hands back one event's worth of bytes
                    // at a time, so every chunk after the first begins on a data
                    // byte with no 0xF0 in front of it, exactly as winmm's
                    // continuation buffers do. The parser rejoins those, and
                    // gives Linux the same cap and the same all-or-nothing
                    // termination rule as the other two platforms.
                    const uint16_t source =
                        static_cast<uint16_t>((ev->source.client << 8) | ev->source.port);

                    parsers_[source].parse(bytes, static_cast<size_t>(n),
                        [this](const uint8_t* message, size_t size)
                        {
                            client_->onMidiIn(message, static_cast<int>(size));
                        });
                }
            }

            if (!readerRun_)
                break;
        }
    }
}

} // namespace standalone
} // namespace gmpi
