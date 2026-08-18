#pragma once

// Splitting a raw MIDI 1.0 byte stream into individual messages.
//
// Portable, and in the portable half rather than in any one driver, because all
// three platforms hit the same wall: an OS MIDI API hands over a RUN OF BYTES,
// and the far end takes ONE COMPLETE MESSAGE per call. gmpi::midi::MidiConverter2
// decodes a status byte from byte 0 and sizes a sysex payload as size() - 2, so
// a run holding a note-off followed by a note-on loses the note-on silently, and
// a run that begins in the middle of a dump is decoded as whatever its first
// data byte happens to look like as a status.
//
// Each platform reaches that wall by a different route, which is why the answer
// could not live in one of them:
//
//   * CoreMIDI packs "one or more complete messages" into a packet, and splits a
//     long dump across packets.                    (mac/MidiDriverCoreMidi.cpp)
//   * winmm splits a dump larger than its sysex buffer across several
//     MIM_LONGDATA callbacks, so every continuation chunk begins with a DATA
//     byte and carries no 0xF0.                    (windows/MidiDriverWin.cpp)
//   * ALSA hands back one sequencer event's worth of decoded bytes, and a long
//     dump is several events.                      (linux/MidiDriverAlsa.cpp)
//
// ONE PARSER PER SOURCE, on all three. Two devices' streams interleave at
// packet, callback and event granularity respectively, so a shared parser would
// splice one device's dump around the other's notes.
//
// No allocation after construction and no locking, because this runs on whatever
// thread the OS calls a MIDI callback on - CoreMIDI's own thread, winmm's driver
// thread inside winmm's lock, ALSA's reader thread.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gmpi
{
namespace standalone
{

// The longest message this app carries end to end, and therefore the longest
// sysex dump it will reassemble.
//
// The number belongs to MidiFifo rather than to this class: StandaloneHost's
// FIFO frames every message with a ONE-BYTE length, so 255 is the longest it can
// describe. One constant, named in both places, because the two DID disagree -
// at 256 apiece, which the frame header stored as a length of zero, after which
// the reader was one byte behind the writer and every MIDI event that followed
// was garbage until the app was restarted.
//
// Raising it is a deliberate pair of changes, not an edit to this line: the FIFO
// frame header has to widen and MidiFifo::kCapacity has to grow with it, or the
// truncation simply moves one layer along. Until then a dump longer than this is
// DROPPED WHOLE - see append() for why that is the only safe way to fail.
constexpr size_t kMaxMidiMessage = 255;

// How many bytes the message introduced by `status` occupies, the status byte
// included. Zero for anything that is not a status byte, and zero for 0xF0,
// whose length is not known until its 0xF7 arrives.
//
// Shared rather than one table per platform, because getting it wrong is not a
// visible failure: winmm packs every short message into three bytes of a DWORD
// no matter how many it carries, so reading one byte too many feeds the plugin a
// trailing zero, which its own parser reads as a note-off on channel 1.
constexpr size_t midiMessageLength(uint8_t status)
{
    if (status < 0x80)
        return 0;   // a data byte, not the start of anything

    if (status < 0xF0)
    {
        // Program change and channel pressure carry one data byte; every other
        // channel message carries two.
        const uint8_t kind = status & 0xF0;
        return (kind == 0xC0 || kind == 0xD0) ? 2 : 3;
    }

    switch (status)
    {
    case 0xF0: return 0;   // sysex: variable length, terminated by 0xF7
    case 0xF1: return 2;   // MTC quarter frame
    case 0xF2: return 3;   // song position pointer
    case 0xF3: return 2;   // song select
    default:   return 1;   // tune request, the realtime bytes, and the undefined ones
    }
}

class MidiStreamParser
{
public:
    MidiStreamParser()
    {
        // Once, here, so parse() never allocates on a MIDI callback thread.
        message_.reserve(kMaxMidiMessage);
    }

    /// `emit` is called once per complete message, on the calling thread, with a
    /// buffer valid only for the duration of the call.
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
            // Not reproducible with a macOS virtual source, if you try:
            // MIDIPacketList sanitises an interleaved realtime byte on the way
            // in - 90 43 f8 64 sent through MIDIPacketListAdd arrives as
            // 90 43 78 64, with the high bit stripped. Verified by dumping the
            // raw packets. Hardware and drivers deliver realtime in packets of
            // their own, which the branch below handles either way; this is what
            // covers the one that does not.
            if (byte >= 0xF8)
            {
                const uint8_t single = byte;
                emit(&single, size_t{ 1 });
                continue;
            }

            if (byte >= 0x80)   // a status byte: whatever was in progress ends here
            {
                if (byte == 0xF7)   // end of sysex
                {
                    // Nothing is emitted for a dump that overran the cap. It was
                    // never accumulated whole, so what is in the buffer is a
                    // prefix, and a prefix is not a shorter dump.
                    if (inSysex_ && !oversize_)
                    {
                        message_.push_back(byte);   // append() kept room for it
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

                expected_ = midiMessageLength(byte);
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
                // emit these and ALSA's decoder is asked not to, but a driver
                // bridging a raw stream can, and dropping the note would be the
                // only symptom.
                if (runningStatus_ == 0)
                    continue;

                message_.push_back(runningStatus_);
                expected_ = midiMessageLength(runningStatus_);
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
    void append(uint8_t byte)
    {
        // One byte short of the cap, because the closing 0xF7 must always have
        // somewhere to go. A dump emitted without its terminator is the one
        // clearly wrong outcome: MidiConverter2 sizes the payload as size() - 2
        // and so hands the plugin the last data byte as if it were the 0xF7,
        // one byte short, with no way to tell that anything was lost.
        if (message_.size() + 1 < kMaxMidiMessage)
        {
            message_.push_back(byte);
            return;
        }

        // Past the cap the dump is abandoned WHOLE rather than clipped to fit -
        // the same answer as a dump interrupted by a new status byte above, and
        // for the same reason. The remaining bytes still have to be walked to
        // find the 0xF7, so the decision rides along in a flag until then.
        //
        // What this costs is a device that sends a bank dump getting no dump at
        // all; what it buys is that the plugin never sees a malformed one. See
        // kMaxMidiMessage above for what raising the cap would take.
        oversize_ = true;
    }

    void reset()
    {
        message_.clear();
        inSysex_ = false;
        oversize_ = false;
        expected_ = 0;
    }

    std::vector<uint8_t> message_;
    size_t expected_ = 0;
    uint8_t runningStatus_ = 0;
    bool inSysex_ = false;
    bool oversize_ = false;
};

} // namespace standalone
} // namespace gmpi
