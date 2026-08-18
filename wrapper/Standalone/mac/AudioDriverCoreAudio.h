#pragma once

// CoreAudio for the macOS standalone.
//
// One AUHAL (kAudioUnitSubType_HALOutput), which is the same unit every macOS
// host uses to reach a soundcard directly. The plugin's buffers are handed to
// it as non-interleaved float32, which is CoreAudio's canonical format, so
// nothing between here and the device has to interleave or convert.
//
// NOR IS THE RATE CONVERTED. This platform lets an application reclock the
// hardware, so open() asks the device to run at the rate the user chose and
// builds the unit's stream format from whatever the device ended up on - which
// leaves AUHAL's converter with nothing to do. sampleRates() offers exactly the
// rates the device advertises, so the two agree by construction. The other two
// shells reach the same place by a different road; the shared statement of it
// is AudioMidiDevices.h::sampleRates.
//
// DUPLEX IS ONE DEVICE, and that is the difference from the other two shells.
// AudioDriverWasapi and AudioDriverPipeWire open a render stream and a capture
// stream with a ring buffer between them, because on those platforms the two
// halves are separate objects with separate clocks whatever device they name.
// A single AUHAL with input enabled has no such problem: input and output are
// delivered by ONE HAL IOProc, off ONE clock, so there is nothing to reconcile
// and no ring buffer to underrun - but it only works when the chosen device
// has both, which is what an audio interface is and what a modern Mac's
// built-in audio is not (speakers and microphone are separate devices there).
//
// So: input opens when the plugin has input pins AND the selected device has
// input streams, and otherwise the plugin is rendered with silent inputs and
// the settings page says so. That covers every interface an effect would
// actually be auditioned through. Aggregating two devices into one clock
// domain is a real feature, and it belongs behind a real aggregate device
// rather than a ring buffer bolted on here - see the note in open().
//
// THREADING. The render and input callbacks run on CoreAudio's own realtime
// thread; everything else on this class is main-thread only, matching the
// contract in AudioMidiDevices.h. The one thing crossing between them is the
// input scratch, written by the input callback and read by the render callback
// on the SAME thread in the same IO cycle, so it needs no synchronisation.

#include <atomic>
#include <string>
#include <vector>

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include "../AudioMidiDevices.h"

namespace gmpi
{
namespace standalone
{

class AudioDriverCoreAudio : public AudioDriver
{
public:
    AudioDriverCoreAudio() = default;
    ~AudioDriverCoreAudio() override;

    const char* name() const override { return "CoreAudio"; }

    // Unlike a device UID this stays meaningful when the user changes their
    // default output or unplugs the interface the saved id named. resolveDevice
    // is the other half of it - see AudioDriver::defaultDeviceId.
    const char* defaultDeviceId() const override { return "coreaudio:default"; }

    std::vector<DeviceInfo> devices() override;
    std::vector<int> sampleRates(const std::string& deviceId) override;

    bool open(const std::string& deviceId,
              int requestedSampleRate,
              int requestedBufferFrames,
              int inChannels,
              int outChannels,
              AudioCallback* client) override;

    void close() override;

    int getSampleRate()   const override { return activeSampleRate_; }
    int getBufferFrames() const override { return activeBufferFrames_; }

    std::string lastError() const override { return lastError_; }
    std::string lastWarning() const override { return warning_; }

    bool isStreamRunning() const override { return streamRunning_; }

    std::string stoppedReason() const override
    {
        const char* const why = stopReason_.load();
        return why ? std::string(why) : std::string{};
    }

private:
    // --- realtime thread -------------------------------------------------
    static OSStatus renderProc(void* refCon,
                               AudioUnitRenderActionFlags* flags,
                               const AudioTimeStamp* timeStamp,
                               UInt32 busNumber,
                               UInt32 frames,
                               AudioBufferList* data);

    static OSStatus inputProc(void* refCon,
                              AudioUnitRenderActionFlags* flags,
                              const AudioTimeStamp* timeStamp,
                              UInt32 busNumber,
                              UInt32 frames,
                              AudioBufferList* data);

    // --- a HAL notification thread ---------------------------------------

    // kAudioDevicePropertyDeviceIsAlive on the open device, which is how this
    // platform says an interface has been unplugged. Nothing else here notices:
    // AudioOutputUnitStart succeeds and then the IOProc simply stops being
    // called, so without this listener the app would go on reporting a running
    // stream at a device that is no longer on the desk.
    //
    // NOT the realtime thread, and not the main one either - the HAL delivers
    // notifications on a thread of its own choosing. So this does what
    // AudioMidiDevices.h::isStreamRunning asks of it and no more: two atomic
    // stores, no allocation, no call back into anything.
    static OSStatus deviceAliveProc(AudioObjectID object,
                                    UInt32 addressCount,
                                    const AudioObjectPropertyAddress* addresses,
                                    void* clientData);

    // --- main thread -----------------------------------------------------
    // Resolves a settings-file id ("coreaudio:default", or a device UID) to a
    // live AudioDeviceID. kAudioObjectUnknown when nothing answers to it, which
    // is the ordinary case for a saved device that has been unplugged.
    AudioDeviceID resolveDevice(const std::string& deviceId) const;

    // Every rate the device advertises, which is what sampleRates() answers
    // with and what setDeviceSampleRate tests a request against. Empty when the
    // device does not publish the property, which is not an error - it means
    // nothing can be said about it and nothing should be offered.
    static std::vector<int> supportedRates(AudioDeviceID device);

    // Asks the DEVICE to run at `rate` - the hardware really reclocks, which is
    // why this driver never needed a converter and the other two have just
    // stopped using theirs. CoreAudio applies it asynchronously, so this polls
    // for it to land. Returns the rate actually in effect, which is the
    // device's existing one when it declined or when `rate` was 0.
    double setDeviceSampleRate(AudioDeviceID device, double rate);

    // Clamped to kAudioDevicePropertyBufferFrameSizeRange, then read back:
    // the device grants its own size the way a WASAPI shared-mode engine does,
    // and the settings page reports what was granted rather than what was
    // asked for.
    int setDeviceBufferFrames(AudioDeviceID device, int frames);

    // Planar plugin output -> the device's channel layout, for the one case
    // that is not a straight write: the unit would not take the plugin's own
    // channel count and we fell back to the device's. Same policy as
    // AudioDriverWasapi::interleaveOut, deliberately - see the comment there.
    void mapOut(AudioBufferList* data, int frames) const;

    void teardown();

    AudioUnit unit_{};
    AudioDeviceID device_ = kAudioObjectUnknown;

    AudioCallback* client_{};

    int outChannels_ = 2;         // what the PLUGIN produces
    int deviceOutChannels_ = 2;   // what the UNIT accepted; usually the same
    int inChannels_  = 0;         // what the plugin CONSUMES
    // What the device actually delivers, which can be narrower - a mono input
    // feeding a stereo effect. The plugin still gets inChannels_ buffers; the
    // ones past this are left silent, matching AudioDriverWasapi::ringRead
    // rather than spreading one channel across several.
    int activeInChannels_ = 0;
    bool inputOpen_  = false;

    int activeSampleRate_   = 48000;
    int activeBufferFrames_ = 512;

    // Planar scratch, sized in open() and never resized while running.
    //
    // The output side is used ONLY on the channel-mismatch path: when the unit
    // took the plugin's own channel count - which is nearly always - the plugin
    // renders straight into the AudioBufferList CoreAudio supplied and there is
    // no copy at all.
    std::vector<std::vector<float>> planarOut_;
    std::vector<float*> planarOutPtr_;

    std::vector<std::vector<float>> planarIn_;
    std::vector<float*> planarInPtr_;
    std::vector<uint8_t> inputListStorage_;   // an AudioBufferList with inChannels_ buffers

    // Cleared each cycle by the render callback once it has consumed them, so
    // a cycle that delivered no input feeds silence rather than the previous
    // block again - a gap is honest, a loop is not.
    std::atomic<bool> inputFresh_{ false };

    std::atomic<bool> streamRunning_{ false };
    std::string lastError_;

    // Whether deviceAliveProc is registered on device_, so that teardown()
    // removes exactly the listeners open() added. A listener left behind on a
    // driver that has been closed is a callback into a dead object.
    bool aliveListener_ = false;

    // AudioDriver::stoppedReason, and a string LITERAL rather than a
    // std::string: it is written from a HAL notification thread while the main
    // thread may be reading it, and a pointer to static text is what can cross
    // that boundary without a lock. Null means nothing to report. Stored before
    // streamRunning_ is lowered, so a reader that sees the stream stopped also
    // sees why.
    std::atomic<const char*> stopReason_{ nullptr };

    // AudioMidiDevices.h::lastWarning. Written on the main thread inside open()
    // - the only thread that decides whether the input opened - and cleared by
    // close(), which every open() begins with.
    std::string warning_;
};

} // namespace standalone
} // namespace gmpi
