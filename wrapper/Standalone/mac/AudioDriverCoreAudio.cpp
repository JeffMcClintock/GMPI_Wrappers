#include "AudioDriverCoreAudio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace gmpi
{
namespace standalone
{

namespace
{

// kAudioObjectPropertyElementMain spelled as its value.
//
// The constant itself is macOS 12, and its pre-12 name
// (kAudioObjectPropertyElementMaster) is deprecated FROM 12 - so naming either
// one ties this file to a deployment target. Both have always been 0, and this
// wrapper is consumed by projects that set their own target (GMPI-plugins asks
// for 10.15, GMPI_Wrappers' own root asks for 12).
constexpr AudioObjectPropertyElement kElementMain = 0;

// CoreAudio's property API is four calls with the same three-field address in
// front of each, so the address is spelled once here rather than at every site.
AudioObjectPropertyAddress addr(AudioObjectPropertySelector selector,
                                AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal,
                                AudioObjectPropertyElement element = kElementMain)
{
    return { selector, scope, element };
}

template <typename T>
bool getProperty(AudioObjectID object, const AudioObjectPropertyAddress& address, T& out)
{
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &out) == noErr;
}

/// A variable-length property (a device list, a stream configuration, a set of
/// sample-rate ranges) read into a byte block sized by the API itself. Empty
/// when the property does not exist, which is an ordinary answer rather than an
/// error - a device with no input streams simply has no input configuration.
std::vector<uint8_t> getPropertyBlock(AudioObjectID object, const AudioObjectPropertyAddress& address)
{
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(object, &address, 0, nullptr, &size) != noErr || size == 0)
        return {};

    std::vector<uint8_t> block(size);
    if (AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, block.data()) != noErr)
        return {};

    block.resize(size);
    return block;
}

std::string toUtf8(CFStringRef s)
{
    if (!s)
        return {};

    // The fast path returns an interior pointer and is null when the string is
    // not already UTF-8, which is why the copy below exists rather than being
    // the only path.
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

/// Channels the device offers in one direction. The stream configuration is an
/// AudioBufferList whose buffers describe the hardware's own layout, so this is
/// what "has an input" actually means - not whether the device exists.
int channelCount(AudioDeviceID device, AudioObjectPropertyScope scope)
{
    const auto block = getPropertyBlock(
        device, addr(kAudioDevicePropertyStreamConfiguration, scope));
    if (block.empty())
        return 0;

    const auto* list = reinterpret_cast<const AudioBufferList*>(block.data());

    int channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
        channels += static_cast<int>(list->mBuffers[i].mNumberChannels);

    return channels;
}

std::string deviceUid(AudioDeviceID device)
{
    CFStringRef uid{};
    if (!getProperty(device, addr(kAudioDevicePropertyDeviceUID), uid) || !uid)
        return {};

    const std::string result = toUtf8(uid);
    CFRelease(uid);
    return result;
}

std::string deviceName(AudioDeviceID device)
{
    // The name a user recognises, which is not always the same as
    // kAudioObjectPropertyName: an aggregate or a virtual device often carries
    // a better one here.
    CFStringRef name{};
    if (getProperty(device, addr(kAudioObjectPropertyName), name) && name)
    {
        const std::string result = toUtf8(name);
        CFRelease(name);
        if (!result.empty())
            return result;
    }
    return {};
}

/// Non-interleaved float32, which is CoreAudio's canonical format AND the
/// planar layout AudioCallback::processAudio is defined in - so nothing between
/// the plugin and the device has to convert anything.
AudioStreamBasicDescription planarFloat32(double sampleRate, int channels)
{
    AudioStreamBasicDescription asbd{};
    asbd.mSampleRate       = sampleRate;
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = kAudioFormatFlagIsFloat
                           | kAudioFormatFlagIsPacked
                           | kAudioFormatFlagIsNonInterleaved;
    asbd.mFramesPerPacket  = 1;
    asbd.mChannelsPerFrame = static_cast<UInt32>(channels);
    asbd.mBitsPerChannel   = 32;
    // Non-interleaved, so these two describe ONE channel's sample rather than a
    // whole frame. Getting this wrong is the classic way to end up with audio
    // at half speed in one ear.
    asbd.mBytesPerFrame    = sizeof(float);
    asbd.mBytesPerPacket   = sizeof(float);
    return asbd;
}

// AUHAL's two buses. Named, because "0" and "1" mean opposite things depending
// on which scope they are paired with and the pairing is the part people get
// wrong: the OUTPUT bus's INPUT scope is what we supply to the speakers, and
// the INPUT bus's OUTPUT scope is what the microphone hands us.
constexpr AudioUnitElement kOutputBus = 0;
constexpr AudioUnitElement kInputBus  = 1;

} // namespace

AudioDriverCoreAudio::~AudioDriverCoreAudio()
{
    close();
}

// --- enumeration ---------------------------------------------------------

std::vector<DeviceInfo> AudioDriverCoreAudio::devices()
{
    std::vector<DeviceInfo> result;

    // The system default first, because the first entry is what an unconfigured
    // app opens. A UID is stable but names a specific piece of hardware;
    // "whatever macOS is using" survives unplugging it.
    result.push_back({ defaultDeviceId(), "System Default" });

    const auto block = getPropertyBlock(kAudioObjectSystemObject,
                                        addr(kAudioHardwarePropertyDevices));
    if (block.empty())
        return result;

    const auto* ids = reinterpret_cast<const AudioDeviceID*>(block.data());
    const size_t count = block.size() / sizeof(AudioDeviceID);

    for (size_t i = 0; i < count; ++i)
    {
        const AudioDeviceID device = ids[i];

        // Output-capable only. An input-only device in the OUTPUT list is a
        // device the app cannot make a sound through, and offering it is how a
        // user ends up choosing one and hearing nothing.
        if (channelCount(device, kAudioObjectPropertyScopeOutput) < 1)
            continue;

        const std::string uid = deviceUid(device);
        if (uid.empty())
            continue;   // nothing stable to persist, so nothing to offer

        std::string name = deviceName(device);
        if (name.empty())
            name = uid;

        // Duplex devices are marked, because whether the effect being
        // auditioned will hear anything depends on it - see the header.
        if (channelCount(device, kAudioObjectPropertyScopeInput) > 0)
            name += " (in/out)";

        result.push_back({ uid, name });
    }

    return result;
}

std::vector<int> AudioDriverCoreAudio::supportedRates(AudioDeviceID device)
{
    const auto block = getPropertyBlock(
        device, addr(kAudioDevicePropertyAvailableNominalSampleRates));
    if (block.empty())
        return {};

    const auto* ranges = reinterpret_cast<const AudioValueRange*>(block.data());
    const size_t count = block.size() / sizeof(AudioValueRange);

    std::vector<int> rates;

    for (size_t i = 0; i < count; ++i)
    {
        // Nearly every device publishes its rates as degenerate ranges - one
        // entry per rate, minimum equal to maximum - and those go straight in.
        if (std::abs(ranges[i].mMaximum - ranges[i].mMinimum) < 1.0)
        {
            const int rate = static_cast<int>(ranges[i].mMinimum + 0.5);
            if (rate > 0)
                rates.push_back(rate);
            continue;
        }

        // A genuine RANGE, which a few devices (and most aggregates) report.
        // The hardware will take any rate inside it, so what to offer is a
        // question of taste rather than of capability: the rates a plugin's
        // presets were made at, and nothing in between.
        static constexpr int kUsualRates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };

        for (const int candidate : kUsualRates)
        {
            if (candidate >= ranges[i].mMinimum - 1.0 && candidate <= ranges[i].mMaximum + 1.0)
                rates.push_back(candidate);
        }
    }

    std::sort(rates.begin(), rates.end());
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
    return rates;
}

std::vector<int> AudioDriverCoreAudio::sampleRates(const std::string& deviceId)
{
    // What the DEVICE advertises, which on this platform has always been the
    // question open() asks too: setDeviceSampleRate reclocks the hardware for a
    // rate on this list and declines to touch it for anything else. So the list
    // and the behaviour have one source, and a rate offered here is one the user
    // will really get.
    //
    // This used to be the flat { 44100 ... 192000 } the other two shells also
    // carried, with a comment saying so. It was the one part of this file that
    // did not follow the hardware.
    const AudioDeviceID device = resolveDevice(deviceId);
    if (device == kAudioObjectUnknown)
        return {};

    return supportedRates(device);
}

// --- device selection ----------------------------------------------------

AudioDeviceID AudioDriverCoreAudio::resolveDevice(const std::string& deviceId) const
{
    AudioDeviceID defaultDevice = kAudioObjectUnknown;
    getProperty(kAudioObjectSystemObject,
                addr(kAudioHardwarePropertyDefaultOutputDevice), defaultDevice);

    if (deviceId.empty() || deviceId == defaultDeviceId())
        return defaultDevice;

    CFStringRef uid = CFStringCreateWithCString(nullptr, deviceId.c_str(), kCFStringEncodingUTF8);
    if (!uid)
        return defaultDevice;

    AudioDeviceID device = kAudioObjectUnknown;
    AudioValueTranslation translation{ &uid, sizeof(uid), &device, sizeof(device) };

    const auto address = addr(kAudioHardwarePropertyDeviceForUID);
    UInt32 size = sizeof(translation);
    const OSStatus status = AudioObjectGetPropertyData(
        kAudioObjectSystemObject, &address, 0, nullptr, &size, &translation);

    CFRelease(uid);

    // A saved device that has been unplugged falls back to the default rather
    // than refusing to open. The user gets sound and a device list to correct
    // it from; a silent app with an error about hardware that is no longer on
    // the desk helps nobody.
    if (status != noErr || device == kAudioObjectUnknown)
        return defaultDevice;

    return device;
}

double AudioDriverCoreAudio::setDeviceSampleRate(AudioDeviceID device, double rate)
{
    const auto rateAddress = addr(kAudioDevicePropertyNominalSampleRate);

    double current = 0.0;
    getProperty(device, rateAddress, current);

    // Zero is "no preference" - the settings pane sends it when sampleRates()
    // offered nothing to choose from - and leaving the device where it is is
    // exactly the right answer to that.
    if (rate <= 0.0 || std::abs(current - rate) < 1.0)
        return current;

    // Only ask for a rate the device advertises. Setting an unsupported one
    // succeeds on some drivers and then quietly does nothing, which is worse
    // than not asking, because everything downstream believes the new number.
    //
    // The same list sampleRates() published, so what the settings pane offered
    // and what is honoured here cannot disagree.
    const auto supported = supportedRates(device);
    const int wanted = static_cast<int>(rate + 0.5);

    if (std::find(supported.begin(), supported.end(), wanted) == supported.end())
        return current;

    if (AudioObjectSetPropertyData(device, &rateAddress, 0, nullptr, sizeof(rate), &rate) != noErr)
        return current;

    // The set is ASYNCHRONOUS: the HAL reclocks the device and the property
    // only reads back the new value once it has. Polling rather than listening
    // because this runs on the main thread during open(), with nothing else to
    // do until the answer arrives - and a listener would need the run loop that
    // has not been entered yet at startup.
    //
    // Two seconds, which is well past what any device takes and short enough
    // that a driver that will never answer does not hang the app on launch.
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        getProperty(device, rateAddress, current);
        if (std::abs(current - rate) < 1.0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return current;
}

int AudioDriverCoreAudio::setDeviceBufferFrames(AudioDeviceID device, int frames)
{
    const auto sizeAddress = addr(kAudioDevicePropertyBufferFrameSize);

    AudioValueRange range{ 0, 0 };
    if (getProperty(device, addr(kAudioDevicePropertyBufferFrameSizeRange), range)
        && range.mMaximum >= range.mMinimum && range.mMaximum > 0)
    {
        frames = std::clamp(frames,
                            static_cast<int>(range.mMinimum),
                            static_cast<int>(range.mMaximum));
    }

    UInt32 request = static_cast<UInt32>(std::max(1, frames));
    AudioObjectSetPropertyData(device, &sizeAddress, 0, nullptr, sizeof(request), &request);

    // Read back rather than assume. The device is entitled to grant a different
    // size, and the settings page's "Running: 48000 Hz, 512 frames" line is
    // only worth printing if it says what is actually happening.
    UInt32 granted = request;
    getProperty(device, sizeAddress, granted);

    return static_cast<int>(granted);
}

// --- open / close --------------------------------------------------------

bool AudioDriverCoreAudio::open(const std::string& deviceId,
                                int requestedSampleRate,
                                int requestedBufferFrames,
                                int inChannels,
                                int outChannels,
                                AudioCallback* client)
{
    close();

    lastError_.clear();

    if (!client || outChannels < 1)
    {
        lastError_ = "nothing to play (the plugin has no audio outputs)";
        return false;
    }

    device_ = resolveDevice(deviceId);
    if (device_ == kAudioObjectUnknown)
    {
        lastError_ = "no audio output device is available";
        return false;
    }

    client_       = client;
    outChannels_  = outChannels;
    inChannels_   = std::max(0, inChannels);

    // Input only when the plugin wants it AND this device has it. See the
    // header: one AUHAL is one clock domain, and the alternative to declining
    // here is a second device with a ring buffer between the two - which is
    // what an aggregate device already is, done properly, by the OS.
    const int deviceInChannels = channelCount(device_, kAudioObjectPropertyScopeInput);
    inputOpen_ = inChannels_ > 0 && deviceInChannels > 0;
    if (inChannels_ > 0 && !inputOpen_)
    {
        // NOT lastError_: that is documented as empty after a successful open,
        // and StandaloneHost only reads it on failure - so a note left there
        // would be both a broken contract and information nobody sees.
        //
        // warning_ is the channel for exactly this, and the settings page draws
        // it. Worth reporting at all because it is the ordinary case for an
        // effect on a modern Mac, where the speakers and the microphone are
        // separate devices: the plugin runs, and hears silence, and the reason
        // is not otherwise visible anywhere.
        //
        // stderr as well, for whoever is reading a log - and the remedy goes
        // only there, being too long for the pane's one line (see
        // AudioMidiDevices.h::lastWarning).
        warning_ = "Audio input is silent: this output device has no audio input.";

        std::fprintf(stderr,
                     "%s The plugin's %d input(s) will be silent. Choose a duplex device (one "
                     "marked \"in/out\") on the Audio/MIDI settings page to feed it.\n",
                     warning_.c_str(), inChannels_);
    }

    const double sampleRate = setDeviceSampleRate(device_, static_cast<double>(requestedSampleRate));
    activeBufferFrames_ = setDeviceBufferFrames(device_, requestedBufferFrames);
    activeSampleRate_   = static_cast<int>(sampleRate + 0.5);

    // --- the unit --------------------------------------------------------

    AudioComponentDescription description{};
    description.componentType         = kAudioUnitType_Output;
    description.componentSubType      = kAudioUnitSubType_HALOutput;
    description.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent component = AudioComponentFindNext(nullptr, &description);
    if (!component || AudioComponentInstanceNew(component, &unit_) != noErr)
    {
        unit_ = {};
        lastError_ = "could not create the CoreAudio output unit";
        return false;
    }

    const UInt32 enable  = 1;
    const UInt32 disable = 0;

    if (AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output,
                             kOutputBus, &enable, sizeof(enable)) != noErr)
    {
        teardown();
        lastError_ = "the device would not enable audio output";
        return false;
    }

    AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input,
                         kInputBus, inputOpen_ ? &enable : &disable, sizeof(UInt32));

    // AFTER EnableIO and BEFORE the formats: the current device is what decides
    // which formats are legal, and changing it later resets them.
    if (AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global,
                             0, &device_, sizeof(device_)) != noErr)
    {
        teardown();
        lastError_ = "the selected audio device could not be opened (in use?)";
        return false;
    }

    // --- output format ---------------------------------------------------
    // Asked for at the plugin's own channel count first. AUHAL maps that onto
    // whatever the hardware has, so this succeeds on an 8-channel interface as
    // readily as on a stereo one; the fallback below is for the device that
    // refuses, which in practice means a mono output.
    deviceOutChannels_ = outChannels_;
    {
        auto format = planarFloat32(sampleRate, deviceOutChannels_);
        if (AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                                 kOutputBus, &format, sizeof(format)) != noErr)
        {
            const int hardwareChannels = std::max(1, channelCount(device_, kAudioObjectPropertyScopeOutput));

            deviceOutChannels_ = hardwareChannels;
            format = planarFloat32(sampleRate, deviceOutChannels_);

            if (AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                                     kOutputBus, &format, sizeof(format)) != noErr)
            {
                teardown();
                lastError_ = "the device would not accept 32-bit float audio";
                return false;
            }
        }
    }

    // What the unit ended up running at, which can differ from the rate in the
    // settings file: setDeviceSampleRate declines to ask for anything the device
    // does not advertise, and returns the rate the device is on instead. That is
    // the number the stream format above was built from, so AUHAL has nothing to
    // convert - and it is what the plugin's processor must be built against.
    {
        AudioStreamBasicDescription actual{};
        UInt32 size = sizeof(actual);
        if (AudioUnitGetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                                 kOutputBus, &actual, &size) == noErr && actual.mSampleRate > 0.0)
        {
            activeSampleRate_ = static_cast<int>(actual.mSampleRate + 0.5);
            deviceOutChannels_ = static_cast<int>(actual.mChannelsPerFrame);
        }
    }

    // --- input format ----------------------------------------------------

    if (inputOpen_)
    {
        // Never more than the device has. Asking a mono input for two channels
        // fails the format outright, and the plugin's second input is better
        // silent than absent.
        activeInChannels_ = std::min(inChannels_, deviceInChannels);
        auto format = planarFloat32(static_cast<double>(activeSampleRate_), activeInChannels_);

        if (AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
                                 kInputBus, &format, sizeof(format)) != noErr)
        {
            // Output still works, so this is a degraded open rather than a
            // failed one - the same call the header's duplex note describes,
            // and reported the same way as the no-input case above.
            inputOpen_ = false;
            activeInChannels_ = 0;
            AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input,
                                 kInputBus, &disable, sizeof(disable));

            warning_ = "Audio input is silent: the device would not open its input.";

            std::fprintf(stderr, "%s The plugin's input(s) will be silent.\n", warning_.c_str());
        }
    }

    // --- scratch ---------------------------------------------------------
    // Everything the callbacks touch is sized here, on the main thread, and
    // never resized while running: allocation on the audio thread is the one
    // thing AudioMidiDevices.h's threading contract rules out outright.

    const size_t frames = static_cast<size_t>(std::max(1, activeBufferFrames_));

    planarOut_.assign(static_cast<size_t>(outChannels_), std::vector<float>(frames, 0.0f));
    planarOutPtr_.resize(static_cast<size_t>(outChannels_));
    for (size_t ch = 0; ch < planarOut_.size(); ++ch)
        planarOutPtr_[ch] = planarOut_[ch].data();

    planarIn_.assign(static_cast<size_t>(std::max(0, inChannels_)), std::vector<float>(frames, 0.0f));
    planarInPtr_.resize(planarIn_.size());
    for (size_t ch = 0; ch < planarIn_.size(); ++ch)
        planarInPtr_[ch] = planarIn_[ch].data();

    if (inputOpen_)
    {
        // An AudioBufferList is a flexible array member, so it is built in a
        // byte block rather than declared. Pointed straight at planarIn_, which
        // is what makes AudioUnitRender a copy into the plugin's own buffers
        // rather than into somewhere they must then be copied from.
        //
        // Sized to activeInChannels_, NOT to the plugin's count: AudioUnitRender
        // rejects a list whose buffer count disagrees with the stream format,
        // so on a device narrower than the plugin every cycle would fail and
        // the input would be silent with nothing to say why. The plugin's
        // remaining buffers keep the zeros they were assigned and are never
        // written again.
        const size_t buffers = static_cast<size_t>(activeInChannels_);
        inputListStorage_.assign(sizeof(AudioBufferList) + sizeof(AudioBuffer) * (buffers > 0 ? buffers - 1 : 0), 0);

        auto* list = reinterpret_cast<AudioBufferList*>(inputListStorage_.data());
        list->mNumberBuffers = static_cast<UInt32>(buffers);
        for (size_t ch = 0; ch < buffers; ++ch)
        {
            list->mBuffers[ch].mNumberChannels = 1;
            list->mBuffers[ch].mDataByteSize   = static_cast<UInt32>(frames * sizeof(float));
            list->mBuffers[ch].mData           = planarIn_[ch].data();
        }
    }

    // The unit will never be asked for more than this in one call, and it
    // refuses to initialise if it might be. Set from the granted buffer size
    // rather than the requested one, for the same reason the scratch is.
    {
        UInt32 maxFrames = static_cast<UInt32>(activeBufferFrames_);
        AudioUnitSetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
                             0, &maxFrames, sizeof(maxFrames));
    }

    // --- callbacks -------------------------------------------------------

    {
        AURenderCallbackStruct callback{ &AudioDriverCoreAudio::renderProc, this };
        if (AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input,
                                 kOutputBus, &callback, sizeof(callback)) != noErr)
        {
            teardown();
            lastError_ = "the output unit would not take a render callback";
            return false;
        }
    }

    if (inputOpen_)
    {
        AURenderCallbackStruct callback{ &AudioDriverCoreAudio::inputProc, this };
        AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global,
                             0, &callback, sizeof(callback));
    }

    if (AudioUnitInitialize(unit_) != noErr)
    {
        teardown();
        lastError_ = "the audio device could not be initialised (in use by another app?)";
        return false;
    }

    // NOT client_->onAudioFormatChanged: what the device GRANTED is reported by
    // getSampleRate()/getBufferFrames(), which StandaloneHost reads once this
    // has returned. That is the contract the WASAPI and PipeWire drivers are
    // built on - neither notifies at all - and it is what makes the plugin's
    // processor get built exactly once per start. Announcing the format from
    // here would build it a second time, on return, while the callbacks started
    // below were already executing inside the first.

    streamRunning_ = true;

    if (AudioOutputUnitStart(unit_) != noErr)
    {
        streamRunning_ = false;
        teardown();
        lastError_ = "the audio device would not start";
        return false;
    }

    return true;
}

void AudioDriverCoreAudio::close()
{
    // Above the early return, because this is the one place it is cleared and
    // AudioMidiDevices.h::lastWarning promises a closed driver has nothing to
    // say - whether or not there was a unit to tear down.
    warning_.clear();

    if (!unit_)
    {
        client_ = nullptr;
        return;
    }

    // Stop BEFORE anything is released. AudioOutputUnitStop returns only once
    // the IOProc is off the device, so after this line no callback can still be
    // running and the scratch below it is safe to drop.
    streamRunning_ = false;
    AudioOutputUnitStop(unit_);

    teardown();
}

void AudioDriverCoreAudio::teardown()
{
    if (unit_)
    {
        AudioUnitUninitialize(unit_);
        AudioComponentInstanceDispose(unit_);
        unit_ = {};
    }

    device_ = kAudioObjectUnknown;
    client_ = nullptr;
    inputOpen_ = false;
    activeInChannels_ = 0;
    inputFresh_ = false;

    planarOut_.clear();
    planarOutPtr_.clear();
    planarIn_.clear();
    planarInPtr_.clear();
    inputListStorage_.clear();
}

// --- the realtime thread -------------------------------------------------

OSStatus AudioDriverCoreAudio::inputProc(void* refCon,
                                         AudioUnitRenderActionFlags* flags,
                                         const AudioTimeStamp* timeStamp,
                                         UInt32 busNumber,
                                         UInt32 frames,
                                         AudioBufferList* /*data*/)
{
    auto* self = static_cast<AudioDriverCoreAudio*>(refCon);

    if (!self->streamRunning_ || !self->inputOpen_ || self->inputListStorage_.empty())
        return noErr;

    auto* list = reinterpret_cast<AudioBufferList*>(self->inputListStorage_.data());

    // The buffer list is sized for the granted block; a shorter cycle must not
    // leave the tail of the previous one in place, which is what resetting the
    // byte size per call prevents.
    const UInt32 bytes = frames * static_cast<UInt32>(sizeof(float));
    const UInt32 capacity = static_cast<UInt32>(self->activeBufferFrames_) * static_cast<UInt32>(sizeof(float));
    if (bytes > capacity)
        return noErr;   // more than we sized for; drop rather than overrun

    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
        list->mBuffers[i].mDataByteSize = bytes;

    // Input is PULLED, not pushed: this callback only says that a block is
    // ready, and AudioUnitRender is what copies it - straight into planarIn_,
    // which the buffer list already points at.
    const OSStatus status = AudioUnitRender(self->unit_, flags, timeStamp, busNumber, frames, list);

    self->inputFresh_.store(status == noErr, std::memory_order_relaxed);

    return noErr;
}

OSStatus AudioDriverCoreAudio::renderProc(void* refCon,
                                          AudioUnitRenderActionFlags* flags,
                                          const AudioTimeStamp* /*timeStamp*/,
                                          UInt32 /*busNumber*/,
                                          UInt32 frames,
                                          AudioBufferList* data)
{
    auto* self = static_cast<AudioDriverCoreAudio*>(refCon);

    // Silence, and say so: the flag is what lets the HAL skip work downstream
    // rather than mixing a buffer of zeros it did not need to.
    auto emitSilence = [&]
    {
        for (UInt32 i = 0; i < data->mNumberBuffers; ++i)
            std::memset(data->mBuffers[i].mData, 0, data->mBuffers[i].mDataByteSize);
        if (flags)
            *flags |= kAudioUnitRenderAction_OutputIsSilence;
        return noErr;
    };

    if (!self->streamRunning_ || !self->client_ || !data)
        return emitSilence();

    // A cycle longer than what was granted would run off the end of the input
    // scratch. Refusing it is the only safe answer; it does not happen once the
    // maximum frames per slice is set, and this is the assertion that says so.
    if (static_cast<int>(frames) > self->activeBufferFrames_)
        return emitSilence();

    // Input the input callback did not deliver this cycle is silence, not the
    // previous block: a gap is honest, a loop is not. Cleared here rather than
    // in inputProc so that a cycle with no input callback at all is covered too.
    //
    // Only the channels the device actually fills are cleared - the ones past
    // activeInChannels_ have been zero since open() and nothing ever writes
    // them, so clearing them every cycle would be work for no effect.
    const bool haveInput = self->inputOpen_
                        && self->inputFresh_.exchange(false, std::memory_order_relaxed);

    if (!haveInput)
    {
        for (int ch = 0; ch < self->activeInChannels_; ++ch)
            std::memset(self->planarIn_[static_cast<size_t>(ch)].data(), 0,
                        static_cast<size_t>(frames) * sizeof(float));
    }

    const float* const* inputs = self->planarInPtr_.empty() ? nullptr : self->planarInPtr_.data();

    // Covers both of the paths below, and is why they are not each wrapped:
    // this is CoreAudio's own IOProc thread and the guard has to come off again
    // before returning to it. See AudioMidiDevices.h.
    const ScopedNoDenormals noDenormals;

    // The fast path, and the ordinary one: the unit took the plugin's own
    // channel count, so the plugin renders straight into CoreAudio's buffers
    // and nothing is copied at all.
    if (self->deviceOutChannels_ == self->outChannels_
        && data->mNumberBuffers == static_cast<UInt32>(self->outChannels_))
    {
        for (int ch = 0; ch < self->outChannels_; ++ch)
            self->planarOutPtr_[static_cast<size_t>(ch)] = static_cast<float*>(data->mBuffers[ch].mData);

        self->client_->processAudio(static_cast<int>(frames),
                                    inputs, static_cast<int>(self->planarIn_.size()),
                                    self->planarOutPtr_.data(), self->outChannels_);
        return noErr;
    }

    // The mismatch path: render into our own planar scratch, then lay it out
    // the way the device wants.
    for (int ch = 0; ch < self->outChannels_; ++ch)
        self->planarOutPtr_[static_cast<size_t>(ch)] = self->planarOut_[static_cast<size_t>(ch)].data();

    self->client_->processAudio(static_cast<int>(frames),
                                inputs, static_cast<int>(self->planarIn_.size()),
                                self->planarOutPtr_.data(), self->outChannels_);

    self->mapOut(data, static_cast<int>(frames));
    return noErr;
}

void AudioDriverCoreAudio::mapOut(AudioBufferList* data, int frames) const
{
    // The channel mapping is AudioDriver::open's - one statement, three
    // drivers: a MONO plugin fills every channel the device has, anything wider
    // fills the first N and the rest are silent, and a device narrower than the
    // plugin drops the extras rather than folding them down. The loop below is
    // that rule and nothing else.
    const size_t bytes = static_cast<size_t>(frames) * sizeof(float);

    for (UInt32 ch = 0; ch < data->mNumberBuffers; ++ch)
    {
        auto* destination = static_cast<float*>(data->mBuffers[ch].mData);
        if (!destination)
            continue;

        const float* source = nullptr;
        if (static_cast<int>(ch) < outChannels_)
            source = planarOut_[ch].data();
        else if (outChannels_ == 1)
            source = planarOut_[0].data();

        if (source)
            std::memcpy(destination, source, bytes);
        else
            std::memset(destination, 0, bytes);
    }
}

} // namespace standalone
} // namespace gmpi
