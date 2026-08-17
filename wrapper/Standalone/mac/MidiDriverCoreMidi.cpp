#include "MidiDriverCoreMidi.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace gmpi
{
namespace standalone
{

namespace
{

std::string toUtf8(CFStringRef s)
{
    if (!s)
        return {};

    if (const char* direct = CFStringGetCStringPtr(s, kCFStringEncodingUTF8))
        return direct;

    const CFIndex length = CFStringGetLength(s);
    const CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;

    std::string out(static_cast<size_t>(maxBytes), '\0');
    if (!CFStringGetCString(s, out.data(), maxBytes, kCFStringEncodingUTF8))
        return {};

    out.resize(std::strlen(out.c_str()));
    return out;
}

std::string endpointString(MIDIEndpointRef endpoint, CFStringRef property)
{
    CFStringRef value{};
    if (MIDIObjectGetStringProperty(endpoint, property, &value) != noErr || !value)
        return {};

    const std::string result = toUtf8(value);
    CFRelease(value);
    return result;
}

/// The id persisted in the settings file. kMIDIPropertyUniqueID rather than the
/// source INDEX, which changes the moment another device is plugged in, and
/// rather than the name, which is not unique - two identical keyboards present
/// two ports with the same display name.
std::string endpointId(MIDIEndpointRef endpoint)
{
    SInt32 uniqueId = 0;
    if (MIDIObjectGetIntegerProperty(endpoint, kMIDIPropertyUniqueID, &uniqueId) != noErr)
        return {};

    return std::to_string(static_cast<long>(uniqueId));
}

/// What the user sees. kMIDIPropertyDisplayName is the one macOS itself shows
/// in Audio MIDI Setup - "Keystation 49 Port 1" rather than the bare port name -
/// so it is the one that matches what a user is looking at when they choose.
std::string endpointName(MIDIEndpointRef endpoint)
{
    std::string name = endpointString(endpoint, kMIDIPropertyDisplayName);
    if (name.empty())
        name = endpointString(endpoint, kMIDIPropertyName);
    return name;
}

/// Splits a CoreMIDI packet's bytes into individual MIDI 1.0 messages.
///
/// Needed because the far end takes ONE message per call: MidiConverter2 decodes
/// a status byte from byte 0 and converts that message, so handing it a packet
/// holding a note-off followed by a note-on would silently drop the second.
/// CoreMIDI packs "one or more complete messages" into a packet, so this is the
/// ordinary case rather than an edge one.
///
/// Sysex is the exception CoreMIDI documents: a long dump arrives split across
/// packets, so it accumulates here until its 0xF7. One parser per connected
/// source, because two devices' streams interleave at packet granularity and a
/// shared parser would splice one device's sysex around the other's notes.
class MidiStreamParser
{
public:
    MidiStreamParser()
    {
        // Once, here, so parse() never allocates on CoreMIDI's thread.
        message_.reserve(kMaxSysex);
    }

    /// `emit` is called once per complete message, on CoreMIDI's thread.
    template <typename Emit>
    void parse(const uint8_t* data, size_t size, Emit&& emit)
    {
        for (size_t i = 0; i < size; ++i)
        {
            const uint8_t byte = data[i];

            // System real time (0xF8..0xFF) may appear ANYWHERE, including in
            // the middle of another message or a sysex dump, and does not
            // disturb it. Emitted on the spot and the partial message resumes.
            //
            // Not reproducible with a virtual source, if you try: MIDIPacketList
            // sanitises an interleaved realtime byte on the way in - 90 43 f8 64
            // sent through MIDIPacketListAdd arrives as 90 43 78 64, with the
            // high bit stripped. Verified by dumping the raw packets. Hardware
            // and drivers deliver realtime in packets of their own, which the
            // branch below handles either way; this is what covers the one that
            // does not.
            if (byte >= 0xF8)
            {
                const uint8_t single = byte;
                emit(&single, 1);
                continue;
            }

            if (byte >= 0x80)   // a status byte: whatever was in progress ends here
            {
                if (byte == 0xF7)   // end of sysex
                {
                    if (inSysex_)
                    {
                        append(byte);
                        emit(message_.data(), message_.size());
                    }
                    reset();
                    continue;
                }

                // An unterminated sysex followed by a new status byte is a
                // dropped dump, not a message to emit: the far end would decode
                // the truncation as MIDI. Discarded rather than guessed at.
                reset();

                message_.push_back(byte);

                if (byte == 0xF0)
                {
                    inSysex_ = true;
                    expected_ = 0;
                    runningStatus_ = 0;   // sysex cancels running status
                    continue;
                }

                expected_ = messageLength(byte);
                runningStatus_ = (byte < 0xF0) ? byte : 0;   // system messages cancel it

                if (expected_ == 1)
                {
                    emit(message_.data(), message_.size());
                    reset();
                }
                continue;
            }

            // A data byte.
            if (inSysex_)
            {
                append(byte);
                continue;
            }

            if (message_.empty())
            {
                // Running status: a data byte with no status in front of it
                // repeats the last channel status. CoreMIDI is not supposed to
                // emit these, but a driver bridging a raw stream can, and
                // dropping the note would be the only symptom.
                if (runningStatus_ == 0)
                    continue;

                message_.push_back(runningStatus_);
                expected_ = messageLength(runningStatus_);
            }

            message_.push_back(byte);

            if (message_.size() >= expected_)
            {
                emit(message_.data(), message_.size());
                message_.clear();   // NOT reset(): running status survives
            }
        }
    }

private:
    static size_t messageLength(uint8_t status)
    {
        if (status < 0xF0)
        {
            // Program change and channel pressure carry one data byte; every
            // other channel message carries two.
            const uint8_t kind = status & 0xF0;
            return (kind == 0xC0 || kind == 0xD0) ? 2 : 3;
        }

        switch (status)
        {
        case 0xF1: return 2;   // MTC quarter frame
        case 0xF2: return 3;   // song position pointer
        case 0xF3: return 2;   // song select
        default:   return 1;   // tune request, and the undefined ones
        }
    }

    void append(uint8_t byte)
    {
        // A dump longer than this is truncated rather than grown without
        // bound: MidiFifo::kMaxMessage is 256 and truncates anyway, and a
        // device stuck mid-sysex must not be able to allocate the app to death
        // on a thread that may not allocate at all.
        if (message_.size() < kMaxSysex)
            message_.push_back(byte);
    }

    void reset()
    {
        message_.clear();
        inSysex_ = false;
        expected_ = 0;
    }

    static constexpr size_t kMaxSysex = 256;

    std::vector<uint8_t> message_;
    size_t expected_ = 0;
    uint8_t runningStatus_ = 0;
    bool inSysex_ = false;
};

} // namespace

/// One per connected source, handed to MIDIPortConnectSource as its connRefCon
/// and handed back to the read proc with every packet list from that source.
struct MidiDriverCoreMidi::Source
{
    MidiDriverCoreMidi* driver{};
    MIDIEndpointRef endpoint{};
    MidiStreamParser parser;
};

MidiDriverCoreMidi::~MidiDriverCoreMidi()
{
    close();
}

std::vector<DeviceInfo> MidiDriverCoreMidi::inputs()
{
    std::vector<DeviceInfo> result;

    const ItemCount count = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < count; ++i)
    {
        const MIDIEndpointRef source = MIDIGetSource(i);
        if (!source)
            continue;

        const std::string id = endpointId(source);
        if (id.empty())
            continue;   // nothing stable to persist, so nothing to offer

        std::string name = endpointName(source);
        if (name.empty())
            name = "MIDI input " + std::to_string(static_cast<long>(i + 1));

        result.push_back({ id, name });
    }

    return result;
}

bool MidiDriverCoreMidi::open(const std::vector<std::string>& inputIds, MidiCallback* client)
{
    close();

    lastError_.clear();

    if (!client)
        return false;

    callback_ = client;

    // The client's name shows up in Audio MIDI Setup, so it is worth being the
    // app rather than "client 1".
    if (MIDIClientCreate(CFSTR("GMPI Standalone"), nullptr, nullptr, &client_) != noErr)
    {
        client_ = {};
        callback_ = nullptr;
        lastError_ = "could not create a CoreMIDI client";
        return false;
    }

    // The deprecated call, deliberately - see the header. MIDIInputPortCreate
    // delivers MIDI 1.0 BYTES, which is what MidiCallback::onMidiIn is defined
    // in and what gmpi::midi::MidiConverter2 converts from; the replacement
    // hands over UMP words that would have to be converted back to bytes to
    // reach the same seam.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const OSStatus portStatus =
        MIDIInputPortCreate(client_, CFSTR("Input"), &MidiDriverCoreMidi::readProc, this, &port_);
#pragma clang diagnostic pop

    if (portStatus != noErr)
    {
        port_ = {};
        MIDIClientDispose(client_);
        client_ = {};
        callback_ = nullptr;
        lastError_ = "could not open a CoreMIDI input port";
        return false;
    }

    const ItemCount sourceCount = MIDIGetNumberOfSources();

    running_ = true;

    for (ItemCount i = 0; i < sourceCount; ++i)
    {
        const MIDIEndpointRef endpoint = MIDIGetSource(i);
        if (!endpoint)
            continue;

        // An empty list means "connect everything readable", which is what
        // makes a fresh install play the moment a keyboard is plugged in. A
        // non-empty one is honoured exactly, including a saved id whose device
        // is no longer present - which simply matches nothing.
        if (!inputIds.empty())
        {
            const std::string id = endpointId(endpoint);
            if (std::find(inputIds.begin(), inputIds.end(), id) == inputIds.end())
                continue;
        }

        auto source = std::make_unique<Source>();
        source->driver   = this;
        source->endpoint = endpoint;

        if (MIDIPortConnectSource(port_, endpoint, source.get()) != noErr)
            continue;   // one unreachable port must not cost the others

        sources_.push_back(source.release());
    }

    if (sources_.empty())
    {
        // False, matching MidiDriverWin: "connected to nothing" is reported as
        // a failed open on every platform, so the settings page has one story
        // to tell. It is not fatal to the app - a machine with no keyboard
        // attached is an ordinary way to run a standalone, and the command
        // channel can inject MIDI regardless.
        lastError_ = inputIds.empty() ? "No MIDI input devices were found."
                                      : "None of the selected MIDI inputs could be opened.";

        close();
        return false;
    }

    return true;
}

void MidiDriverCoreMidi::close()
{
    // FIRST: a packet list already in flight must not reach a callback that is
    // about to go away. The read proc checks this before touching anything.
    running_ = false;

    if (port_)
    {
        for (const auto* source : sources_)
            MIDIPortDisconnectSource(port_, source->endpoint);

        MIDIPortDispose(port_);
        port_ = {};
    }

    // After the disconnect and the dispose, so CoreMIDI can no longer be
    // holding any of these addresses.
    for (auto* source : sources_)
        delete source;
    sources_.clear();

    if (client_)
    {
        MIDIClientDispose(client_);
        client_ = {};
    }

    callback_ = nullptr;
}

void MidiDriverCoreMidi::readProc(const MIDIPacketList* packets, void* /*refCon*/, void* connRefCon)
{
    auto* state = static_cast<Source*>(connRefCon);
    if (!state || !packets)
        return;

    auto* driver = state->driver;
    if (!driver || !driver->running_ || !driver->callback_)
        return;

    const MIDIPacket* packet = &packets->packet[0];

    for (UInt32 i = 0; i < packets->numPackets; ++i)
    {
        // packet->length may exceed the 256 bytes MIDIPacket declares - the
        // storage is variable-length and MIDIPacketNext is the only way to
        // walk it - so the whole run is handed to the parser and split there.
        state->parser.parse(packet->data, packet->length,
                            [driver](const uint8_t* message, size_t size)
                            {
                                driver->callback_->onMidiIn(message, static_cast<int>(size));
                            });

        packet = MIDIPacketNext(packet);
    }
}

} // namespace standalone
} // namespace gmpi
