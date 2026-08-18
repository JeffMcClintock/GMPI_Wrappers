#include "MidiDriverCoreMidi.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include "../MidiStreamParser.h"

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

/// The same name with inputs()' last-resort fallback applied, for the places
/// that put it in a sentence. An endpoint with neither property readable is
/// rare but not impossible, and 'Could not open MIDI input ""' helps nobody.
std::string endpointNameForMessage(MIDIEndpointRef endpoint, int index)
{
    std::string name = endpointName(endpoint);
    if (name.empty())
        name = "MIDI input " + std::to_string(static_cast<long>(index + 1));
    return name;
}

} // namespace

/// One per connected source, handed to MIDIPortConnectSource as its connRefCon
/// and handed back to the read proc with every packet list from that source.
///
/// The parser is the portable one in ../MidiStreamParser.h, which winmm and ALSA
/// also feed - the three OS APIs fragment a stream differently but all three
/// hand the plugin the same bytes because the splitting happens in one place.
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

        // open() makes the same skip, and says why at length. The two have to
        // agree, or the tick boxes describe a different machine from the one the
        // driver connected to.
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

    // The selection test and the outcome count in one object, shared with
    // MidiDriverWin and MidiDriverAlsa - see MidiOpenTally.
    MidiOpenTally tally(inputIds);

    for (ItemCount i = 0; i < sourceCount; ++i)
    {
        const MIDIEndpointRef endpoint = MIDIGetSource(i);
        if (!endpoint)
            continue;

        // The unique id is read for every source now, not only when there is a
        // selection to match it against - the tally needs to be asked about each
        // one either way.
        //
        // And skipped on the SAME test inputs() skips on, so that the pane and
        // this loop are looking at one list. An endpoint whose unique id cannot
        // be read has nothing to persist, so inputs() cannot offer it; connect
        // it here anyway and it would be a device the user can see no tick box
        // for, playing while unconfigured and vanishing the moment they tick
        // anything else - and one the tally would count as present while the
        // pane showed nothing at all. It is a defensive branch either way:
        // CoreMIDI assigns kMIDIPropertyUniqueID to every endpoint itself.
        const std::string id = endpointId(endpoint);
        if (id.empty())
            continue;

        if (!tally.wants(id))
            continue;

        auto source = std::make_unique<Source>();
        source->driver   = this;
        source->endpoint = endpoint;

        if (MIDIPortConnectSource(port_, endpoint, source.get()) != noErr)
        {
            // One unreachable source must not cost the user the others - so the
            // loop goes on, and the tally decides later whether this mattered.
            // The display name, because this driver's id is a number - through
            // the same last-resort fallback inputs() uses, so a sentence about
            // an endpoint that names itself to neither property is not blank.
            tally.failed(endpointNameForMessage(endpoint, static_cast<int>(i)));
            continue;
        }

        sources_.push_back(source.release());
        tally.opened();
    }

    if (!tally.success())
    {
        // What counts as failure at all, and what to say about it, is
        // MidiOpenTally's decision - one story for all three platforms. The case
        // that changed here is an app nobody has configured on a machine with no
        // MIDI hardware: that is now a success, so an ordinary laptop with
        // nothing plugged in no longer puts a sentence on the settings page.
        lastError_ = tally.failure();

        close();
        return false;
    }

    // lastError_ is left EMPTY even if a source along the way refused: at least
    // one input is live, the app is playable, and a stale sentence on the
    // settings page would outlive the condition that caused it. The tally is
    // asked for its sentence only on the arm above. MidiDriverWin does the same.
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
