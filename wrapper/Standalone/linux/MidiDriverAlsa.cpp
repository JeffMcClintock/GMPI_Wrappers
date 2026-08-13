#include "MidiDriverAlsa.h"

#include <algorithm>
#include <poll.h>

#include <alsa/asoundlib.h>

namespace gmpi
{
namespace standalone
{

namespace
{

// The sequencer describes ports as "client:port"; the ids carry a prefix so a
// saved setting can never be mistaken for another driver's.
std::string portId(const snd_seq_addr_t& a)
{
    return "ALSA:MIDIIN:" + std::to_string(a.client) + ":" + std::to_string(a.port);
}

constexpr unsigned kReadable = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;

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

    snd_seq_client_info_t* client{};
    snd_seq_port_info_t* port{};
    snd_seq_client_info_alloca(&client);
    snd_seq_port_info_alloca(&port);

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
            const auto* addr = snd_seq_port_info_get_addr(port);
            results.push_back({
                portId(*addr),
                std::string(snd_seq_client_info_get_name(client)) + ": " + snd_seq_port_info_get_name(port) });
        }
    }

    snd_seq_close(seq);
    return results;
}

void MidiDriverAlsa::connectInputs(const std::vector<std::string>& inputIds)
{
    const bool openAll = inputIds.empty();

    snd_seq_client_info_t* client{};
    snd_seq_port_info_t* port{};
    snd_seq_client_info_alloca(&client);
    snd_seq_port_info_alloca(&port);

    snd_seq_client_info_set_client(client, -1);
    while (snd_seq_query_next_client(seq_, client) >= 0)
    {
        const int clientId = snd_seq_client_info_get_client(client);
        if (clientId == SND_SEQ_CLIENT_SYSTEM || clientId == snd_seq_client_id(seq_))
            continue;

        snd_seq_port_info_set_client(port, clientId);
        snd_seq_port_info_set_port(port, -1);
        while (snd_seq_query_next_port(seq_, port) >= 0)
        {
            const unsigned caps = snd_seq_port_info_get_capability(port);
            if ((caps & kReadable) != kReadable || (caps & SND_SEQ_PORT_CAP_NO_EXPORT))
                continue;

            const auto* addr = snd_seq_port_info_get_addr(port);
            const auto id = portId(*addr);

            // Exact match on the id, not a substring search: client 1 port 0
            // and client 11 port 0 both contain "ALSA:MIDIIN:1", and matching
            // by substring would subscribe to a device the user unticked.
            if (openAll || std::find(inputIds.begin(), inputIds.end(), id) != inputIds.end())
                snd_seq_connect_from(seq_, inputPort_, addr->client, addr->port);
        }
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

    connectInputs(inputIds);

    // Sized for the longest sysex a controller is likely to send.
    snd_midi_event_new(4096, &decoder_);

    // Every message complete, no running status: the decoder's default elides
    // repeated status bytes, so the second note of a chord arrives as two
    // naked data bytes and anything downstream drops it.
    snd_midi_event_no_status(decoder_, 1);

    readerRun_ = true;
    reader_ = std::thread([this] { readerLoop(); });

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
                    client_->onMidiIn(bytes, static_cast<int>(n));
            }

            if (!readerRun_)
                break;
        }
    }
}

} // namespace standalone
} // namespace gmpi
