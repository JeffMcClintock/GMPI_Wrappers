#include "AudioDriverWasapi.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <future>

#include <windows.h>

// mmreg.h explicitly: it carries WAVEFORMATEXTENSIBLE and the SPEAKER_ masks,
// and it normally arrives via mmsystem.h - which WIN32_LEAN_AND_MEAN (set by
// gmpi_ui's headers) excludes.
#include <mmreg.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <propidl.h>

#include "helpers/unicode_conversion.h"

namespace gmpi
{
namespace standalone
{

namespace
{

// PKEY_Device_FriendlyName and KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out.
//
// Both live in headers (<functiondiscoverykeys_devpkey.h>, <ksmedia.h>) that
// only DEFINE their constants when INITGUID is set before the include - and
// setting INITGUID in a translation unit emits a definition for every GUID in
// every header it reaches, which turns into duplicate symbols the moment a
// second file in the same library does the same. Two literals cost less than
// that, and neither value can change: they are on the wire.
const PROPERTYKEY kPkeyDeviceFriendlyName =
    { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

const GUID kSubtypeIeeeFloat =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

// Older SDKs predate the shared-mode resamplers. Declaring them here rather
// than #ifdef-ing every use keeps the retry ladder readable.
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

// Minimal owning COM pointer. <wrl/client.h> would do, but it is a large header
// for one behaviour, and this file's whole COM surface is eight interfaces held
// in two structs.
template <typename T>
class ComPtr
{
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { reset(); }

    T** put()
    {
        reset();
        return &p_;
    }
    T*  get()  const { return p_; }
    T*  operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

    void reset()
    {
        if (p_)
        {
            p_->Release();
            p_ = nullptr;
        }
    }

    // Ownership transfer, for handing a locally-built interface to a caller.
    // Neither touches the reference count: the point is that exactly one
    // ComPtr holds the reference that was already taken.
    T* detach()
    {
        T* const p = p_;
        p_ = nullptr;
        return p;
    }
    void attach(T* p)
    {
        reset();
        p_ = p;
    }

private:
    T* p_{};
};

// The usual desktop layouts. A mask that does not match the channel count is
// rejected by some drivers, so anything unusual falls back to "the first N
// speakers", which every endpoint accepts.
DWORD defaultChannelMask(int channels)
{
    switch (channels)
    {
    case 1:  return SPEAKER_FRONT_CENTER;
    case 2:  return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    default: return channels > 0 && channels < 32 ? (1u << channels) - 1u : 0u;
    }
}

WAVEFORMATEXTENSIBLE makeFloatFormat(int channels, int sampleRate)
{
    WAVEFORMATEXTENSIBLE f{};

    f.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels       = static_cast<WORD>(channels);
    f.Format.nSamplesPerSec  = static_cast<DWORD>(sampleRate);
    f.Format.wBitsPerSample  = 32;
    f.Format.nBlockAlign     = static_cast<WORD>(channels * sizeof(float));
    f.Format.nAvgBytesPerSec = f.Format.nSamplesPerSec * f.Format.nBlockAlign;
    f.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask               = defaultChannelMask(channels);
    f.SubFormat                   = kSubtypeIeeeFloat;

    return f;
}

// True when the endpoint agreed to hand us plain 32-bit float. Every fallback
// below negotiates towards this, and the render loop assumes it - a device that
// insisted on 24-bit packed would need a converter this driver does not have.
bool isFloat32(const WAVEFORMATEX* f)
{
    if (!f || f->wBitsPerSample != 32)
        return false;

    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return true;

    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22)
    {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        return ext->SubFormat == kSubtypeIeeeFloat;
    }

    return false;
}

// WASAPI measures durations in 100ns units. Rounded UP, because a duration that
// lands just short of the requested frame count gets a buffer one frame smaller
// than asked for, every time.
REFERENCE_TIME framesToRefTime(int frames, int sampleRate)
{
    if (sampleRate <= 0)
        return 0;

    return static_cast<REFERENCE_TIME>(
        (10000000.0 * frames) / sampleRate + 0.5);
}

std::string describeHresult(const char* what, HRESULT hr)
{
    char buffer[160];

    const char* detail = nullptr;
    switch (hr)
    {
    case AUDCLNT_E_DEVICE_IN_USE:        detail = "the device is in use by another application in exclusive mode"; break;
    case AUDCLNT_E_UNSUPPORTED_FORMAT:   detail = "the device does not support the requested format"; break;
    case AUDCLNT_E_DEVICE_INVALIDATED:   detail = "the device was removed or reconfigured"; break;
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED: detail = "the audio endpoint could not be created"; break;
    case AUDCLNT_E_SERVICE_NOT_RUNNING:  detail = "the Windows Audio service is not running"; break;
    case E_ACCESSDENIED:                 detail = "access to the device was denied"; break;
    default: break;
    }

    if (detail)
        snprintf(buffer, sizeof(buffer), "%s: %s.", what, detail);
    else
        snprintf(buffer, sizeof(buffer), "%s (HRESULT 0x%08lX).", what, static_cast<unsigned long>(hr));

    return buffer;
}

} // namespace

// Everything COM. Shared by the two device threads, but never CONCURRENTLY:
// the render half is touched only by the render thread and the capture half
// only by the capture thread, from creation to release.
struct AudioDriverWasapi::Wasapi
{
    // Render.
    ComPtr<IAudioClient>       renderClient;
    ComPtr<IAudioRenderClient> render;
    HANDLE renderEvent{};

    // Capture.
    ComPtr<IAudioClient>        captureClient;
    ComPtr<IAudioCaptureClient> capture;
    HANDLE captureEvent{};

    // Set once, watched by both loops. Manual-reset: a single SetEvent has to
    // release every thread waiting on it, and stay released until the next open.
    HANDLE stopEvent{};

    // The handshake that lets open() report a failure the device threads
    // discovered. Reset per open, so a re-open is not answered by the previous
    // attempt's result.
    std::promise<bool> renderReady;
    std::promise<bool> captureReady;

    // Parameters open() parked for the threads to act on.
    std::string deviceId;
    int requestedSampleRate  = 48000;
    int requestedBufferFrames = 512;

    ~Wasapi()
    {
        if (renderEvent)  ::CloseHandle(renderEvent);
        if (captureEvent) ::CloseHandle(captureEvent);
        if (stopEvent)    ::CloseHandle(stopEvent);
    }
};

AudioDriverWasapi::AudioDriverWasapi() = default;

AudioDriverWasapi::~AudioDriverWasapi()
{
    close();
}

// --- enumeration ---------------------------------------------------------
//
// Main-thread only, per AudioMidiDevices.h. Uses the caller's apartment, which
// is the app's STA - fine for IMMDeviceEnumerator, which is agile, and never
// for IAudioClient, which the device threads activate for themselves.

std::vector<DeviceInfo> AudioDriverWasapi::devices()
{
    std::vector<DeviceInfo> result;

    // The system default first, because the first entry is what an
    // unconfigured app opens. An endpoint id is stable but a specific piece of
    // hardware; "whatever Windows is using" survives unplugging it.
    result.push_back({ defaultDeviceId(), "System Default" });

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(enumerator.put()))))
    {
        return result;
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.put())))
        return result;

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.put())))
            continue;

        LPWSTR id{};
        if (FAILED(device->GetId(&id)))
            continue;

        std::string name;
        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, properties.put())))
        {
            PROPVARIANT value;
            PropVariantInit(&value);

            if (SUCCEEDED(properties->GetValue(kPkeyDeviceFriendlyName, &value))
                && value.vt == VT_LPWSTR && value.pwszVal)
            {
                name = gmpi::unicode::to_utf8(value.pwszVal);
            }

            PropVariantClear(&value);
        }

        // An endpoint with no friendly name is still selectable - showing its
        // id is ugly, but silently omitting the only device on the machine
        // would be worse.
        result.push_back({ gmpi::unicode::to_utf8(id), name.empty() ? "Unnamed device" : name });

        ::CoTaskMemFree(id);
    }

    return result;
}

std::vector<int> AudioDriverWasapi::sampleRates()
{
    // What the shared-mode resampler will accept, not what the hardware runs
    // at. In shared mode the engine owns the clock: asking for 44100 on a
    // device running at 48000 inserts a converter rather than reclocking it,
    // which is exactly what a standalone auditioning a plugin wants - and the
    // alternative (offering only the device's own rate) would mean a plugin
    // could never be heard at the rate its presets were made at.
    return { 44100, 48000, 88200, 96000, 176400, 192000 };
}

// --- open / close --------------------------------------------------------

bool AudioDriverWasapi::open(const std::string& deviceId,
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
        lastError_ = "Nothing to play: the plugin has no audio output pins.";
        return false;
    }

    client_       = client;
    outChannels_  = outChannels;
    inChannels_   = (std::max)(0, inChannels);

    w_ = std::make_unique<Wasapi>();
    w_->deviceId              = deviceId;
    w_->requestedSampleRate   = requestedSampleRate > 0 ? requestedSampleRate : 48000;
    w_->requestedBufferFrames = requestedBufferFrames > 0 ? requestedBufferFrames : 512;

    w_->stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!w_->stopEvent)
    {
        lastError_ = "Could not create the audio shutdown event.";
        close();
        return false;
    }

    // Render FIRST, and this order is not negotiable: the capture stream opens
    // at whatever rate the render stream was granted, because the two are read
    // and written in the same callback and nothing here resamples between them.
    auto renderReady = w_->renderReady.get_future();
    renderThread_ = std::thread(&AudioDriverWasapi::renderThread, this);

    if (!renderReady.get())
    {
        close();
        return false;
    }

    // Capture second, and its failure is NOT fatal: an effect that cannot hear
    // anything is still worth showing, and the settings page is where the user
    // finds out why. The render loop is already running by now and will read
    // silence until ringFrames_ is published - see ringRead.
    if (inChannels_ > 0)
    {
        auto captureReady = w_->captureReady.get_future();
        captureThread_ = std::thread(&AudioDriverWasapi::captureThread, this);
        captureReady.wait();
    }

    streamRunning_ = true;
    return true;
}

void AudioDriverWasapi::close()
{
    if (w_ && w_->stopEvent)
        ::SetEvent(w_->stopEvent);

    // Joined, not detached. AudioDriver::close() promises to return only once
    // no callback can still be running, because StandaloneHost tears the
    // plugin's processor down the moment it does.
    if (renderThread_.joinable())
        renderThread_.join();
    if (captureThread_.joinable())
        captureThread_.join();

    streamRunning_  = false;
    captureRunning_ = false;

    w_.reset();
    client_ = nullptr;

    ring_.clear();
    ringFrames_ = 0;
    ringWritePos_ = 0;
    ringReadPos_  = 0;
    captureChannels_ = 0;

    planarOut_.clear();
    planarOutPtr_.clear();
    planarIn_.clear();
    planarInPtr_.clear();
}

// --- the render thread ---------------------------------------------------

namespace
{

// Resolves a device id to an endpoint, falling back to the system default.
// A saved id names hardware that may since have been unplugged, and refusing
// to make a sound because of a stale settings file is not a useful reaction.
HRESULT openEndpoint(IMMDeviceEnumerator* enumerator,
                     const std::string& deviceId,
                     EDataFlow flow,
                     IMMDevice** returnDevice)
{
    if (!deviceId.empty() && deviceId != AudioDriverWasapi::defaultDeviceId())
    {
        const auto wide = gmpi::unicode::to_wide(deviceId);
        if (SUCCEEDED(enumerator->GetDevice(wide.c_str(), returnDevice)))
            return S_OK;
    }

    return enumerator->GetDefaultAudioEndpoint(flow, eConsole, returnDevice);
}

// One Activate + Initialize attempt. The client is re-activated per attempt
// because a failed Initialize leaves an IAudioClient unusable - MSDN is
// explicit that it must be released and a fresh one obtained, and reusing it
// produces failures that look like the format was at fault when it was not.
HRESULT tryInitialise(IMMDevice* device,
                      const WAVEFORMATEX* format,
                      DWORD flags,
                      REFERENCE_TIME bufferDuration,
                      ComPtr<IAudioClient>& returnClient)
{
    ComPtr<IAudioClient> client;
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(client.put()));
    if (FAILED(hr))
        return hr;

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, bufferDuration, 0, format, nullptr);
    if (FAILED(hr))
        return hr;

    // Ownership moves only on success, so a caller stepping down the ladder
    // never has to remember to clear a half-initialised client.
    returnClient.attach(client.detach());
    return S_OK;
}

} // namespace

void AudioDriverWasapi::renderThread()
{
    // MTA. The audio engine calls nothing back into us through COM, and an STA
    // here would need a message pump this thread must never run.
    const HRESULT comInit = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    bool ok = false;
    UINT32 bufferFrameCount = 0;

    do
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(enumerator.put()))))
        {
            lastError_ = "The Windows Audio service could not be reached.";
            break;
        }

        ComPtr<IMMDevice> device;
        HRESULT hr = openEndpoint(enumerator.get(), w_->deviceId, eRender, device.put());
        if (FAILED(hr))
        {
            lastError_ = describeHresult("No audio output device is available", hr);
            break;
        }

        // The retry ladder. Each rung asks for less than the one above it, and
        // the LAST rung is the endpoint's own mix format - which by definition
        // it accepts, so reaching the bottom means the device is unusable
        // rather than fussy.
        const auto wanted = makeFloatFormat(outChannels_, w_->requestedSampleRate);
        const REFERENCE_TIME duration =
            framesToRefTime(w_->requestedBufferFrames, w_->requestedSampleRate);

        constexpr DWORD convertingFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                        | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                        | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

        const WAVEFORMATEX* acceptedFormat = &wanted.Format;

        hr = tryInitialise(device.get(), &wanted.Format, convertingFlags, duration, w_->renderClient);

        WAVEFORMATEX* mixFormat{};
        if (FAILED(hr))
        {
            // Ask the endpoint what it runs at and take that instead. This is
            // where a device that refuses the shared-mode converters (some
            // pro interfaces do) ends up, and the price is that the plugin
            // runs at the hardware's rate rather than the one in settings -
            // which StandaloneHost is told about, because it reads
            // getSampleRate() rather than assuming its request was honoured.
            ComPtr<IAudioClient> probe;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(probe.put()))))
            {
                probe->GetMixFormat(&mixFormat);
            }

            if (mixFormat)
            {
                const HRESULT mixHr = tryInitialise(device.get(), mixFormat,
                                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                    duration, w_->renderClient);
                if (SUCCEEDED(mixHr))
                {
                    acceptedFormat = mixFormat;
                    hr = S_OK;
                }
            }
        }

        if (FAILED(hr))
        {
            lastError_ = describeHresult("The audio device could not be opened", hr);
            ::CoTaskMemFree(mixFormat);
            break;
        }

        if (!isFloat32(acceptedFormat))
        {
            // Every shared-mode endpoint's mix format is float32; the engine
            // converts to the hardware's word length behind us. Reaching here
            // means an assumption this driver is built on has stopped holding,
            // and playing 32-bit floats into an integer buffer would be noise
            // at full scale into someone's headphones.
            lastError_ = "The audio device offered a sample format this application cannot write.";
            ::CoTaskMemFree(mixFormat);
            break;
        }

        deviceOutChannels_  = acceptedFormat->nChannels;
        activeSampleRate_   = static_cast<int>(acceptedFormat->nSamplesPerSec);

        ::CoTaskMemFree(mixFormat);

        if (FAILED(w_->renderClient->GetBufferSize(&bufferFrameCount)) || bufferFrameCount == 0)
        {
            lastError_ = "The audio device reported an unusable buffer size.";
            break;
        }

        activeBufferFrames_ = static_cast<int>(bufferFrameCount);

        if (FAILED(w_->renderClient->GetService(__uuidof(IAudioRenderClient),
                                                reinterpret_cast<void**>(w_->render.put()))))
        {
            lastError_ = "The audio device would not provide a render client.";
            break;
        }

        w_->renderEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!w_->renderEvent || FAILED(w_->renderClient->SetEventHandle(w_->renderEvent)))
        {
            lastError_ = "The audio device would not accept an event handle.";
            break;
        }

        // Scratch the plugin renders into. Allocated HERE, on the thread, and
        // before the handshake below: open() is blocked until then, so there
        // is no other thread to race with, and once the loop starts nothing
        // may allocate again.
        planarOut_.assign(static_cast<size_t>(outChannels_),
                          std::vector<float>(bufferFrameCount, 0.0f));
        planarOutPtr_.clear();
        for (auto& channel : planarOut_)
            planarOutPtr_.push_back(channel.data());

        if (inChannels_ > 0)
        {
            planarIn_.assign(static_cast<size_t>(inChannels_),
                             std::vector<float>(bufferFrameCount, 0.0f));
            planarInPtr_.clear();
            for (auto& channel : planarIn_)
                planarInPtr_.push_back(channel.data());
        }

        ok = true;
    }
    while (false);

    w_->renderReady.set_value(ok);

    if (ok)
    {
        // Pro Audio scheduling. Without it this is an ordinary thread and the
        // scheduler will happily preempt it for a browser tab, which is heard
        // as a click. Failure is not fatal - it means the MMCSS service is off,
        // and glitchy audio still beats none.
        DWORD taskIndex = 0;
        const HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

        // Prime with silence before Start(). An event-driven stream whose first
        // buffer is never released stalls, and the engine's first event fires
        // immediately - so there is no window in which to fill it afterwards.
        BYTE* data{};
        if (SUCCEEDED(w_->render->GetBuffer(bufferFrameCount, &data)))
            w_->render->ReleaseBuffer(bufferFrameCount, AUDCLNT_BUFFERFLAGS_SILENT);

        if (SUCCEEDED(w_->renderClient->Start()))
        {
            const HANDLE waitOn[2] = { w_->stopEvent, w_->renderEvent };

            for (;;)
            {
                // stopEvent is index 0: WaitForMultipleObjects reports the
                // LOWEST signalled index, so a shutdown racing an audio event
                // is seen as a shutdown rather than one more buffer.
                const DWORD signalled = ::WaitForMultipleObjects(2, waitOn, FALSE, 2000);

                if (signalled == WAIT_OBJECT_0 || signalled == WAIT_FAILED)
                    break;

                // A timeout means the engine has stopped asking - the device
                // was removed, or the service restarted. Either way this stream
                // is finished; the user reopens from the settings page.
                if (signalled == WAIT_TIMEOUT)
                    break;

                UINT32 padding = 0;
                if (FAILED(w_->renderClient->GetCurrentPadding(&padding)))
                    break;

                const UINT32 frames = bufferFrameCount > padding
                                    ? bufferFrameCount - padding : 0;
                if (frames == 0)
                    continue;

                if (FAILED(w_->render->GetBuffer(frames, &data)))
                    break;

                if (inChannels_ > 0)
                    ringRead(planarInPtr_.data(), inChannels_, frames);

                client_->processAudio(
                    static_cast<int>(frames),
                    inChannels_ > 0 ? planarInPtr_.data() : nullptr, inChannels_,
                    planarOutPtr_.data(), outChannels_);

                interleaveOut(reinterpret_cast<float*>(data), static_cast<int>(frames));

                if (FAILED(w_->render->ReleaseBuffer(frames, 0)))
                    break;
            }

            w_->renderClient->Stop();
        }

        if (mmcss)
            ::AvRevertMmThreadCharacteristics(mmcss);
    }

    // Released on this thread, in the apartment they were created in.
    w_->render.reset();
    w_->renderClient.reset();

    if (SUCCEEDED(comInit))
        ::CoUninitialize();
}

void AudioDriverWasapi::interleaveOut(float* deviceBuffer, int frames) const
{
    const int deviceChannels = deviceOutChannels_;

    // Only reached when the retry ladder had to fall back to the endpoint's mix
    // format; the first rung asks for the plugin's own channel count, and then
    // this is a straight interleave. The two mismatches are handled thus:
    //
    //   device WIDER than the plugin - a mono plugin is spread across every
    //     channel (which is what "play my synth" means on a stereo card), but
    //     anything wider goes on the first N with the rest left silent: a
    //     surround card should not get the front pair repeated behind the
    //     listener.
    //   device NARROWER - the loop simply runs out, so the plugin's extra
    //     channels are dropped rather than folded down. Only a mono endpoint
    //     does this, and a stereo plugin summed to mono would change the level
    //     of everything the user is auditioning.
    for (int ch = 0; ch < deviceChannels; ++ch)
    {
        const float* source = nullptr;
        if (ch < outChannels_)
            source = planarOut_[static_cast<size_t>(ch)].data();
        else if (outChannels_ == 1)
            source = planarOut_[0].data();

        if (source)
        {
            for (int i = 0; i < frames; ++i)
                deviceBuffer[static_cast<size_t>(i) * deviceChannels + ch] = source[i];
        }
        else
        {
            for (int i = 0; i < frames; ++i)
                deviceBuffer[static_cast<size_t>(i) * deviceChannels + ch] = 0.0f;
        }
    }
}

// --- the capture thread --------------------------------------------------

void AudioDriverWasapi::captureThread()
{
    const HRESULT comInit = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    bool ok = false;
    UINT32 captureBufferFrames = 0;

    do
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(enumerator.put()))))
        {
            break;
        }

        // Always the default input. The settings page offers no capture choice,
        // deliberately: a standalone is for auditioning a plugin, and a second
        // device list to get wrong is not what makes that work. The output
        // device id is not reused here - it names a render endpoint.
        ComPtr<IMMDevice> device;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, device.put())))
            break;

        // Capture follows the RENDER stream's rate, not the request: the two
        // are read and written in the same callback, and a rate mismatch would
        // need a resampler between them.
        const auto wanted = makeFloatFormat(inChannels_, activeSampleRate_);

        constexpr DWORD convertingFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                        | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                        | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

        const WAVEFORMATEX* acceptedFormat = &wanted.Format;
        const REFERENCE_TIME duration = framesToRefTime(w_->requestedBufferFrames, activeSampleRate_);

        HRESULT hr = tryInitialise(device.get(), &wanted.Format, convertingFlags,
                                   duration, w_->captureClient);

        WAVEFORMATEX* mixFormat{};
        if (FAILED(hr))
        {
            ComPtr<IAudioClient> probe;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(probe.put()))))
            {
                probe->GetMixFormat(&mixFormat);
            }

            if (mixFormat && SUCCEEDED(tryInitialise(device.get(), mixFormat,
                                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                     duration, w_->captureClient)))
            {
                acceptedFormat = mixFormat;
                hr = S_OK;
            }
        }

        if (FAILED(hr) || !isFloat32(acceptedFormat))
        {
            ::CoTaskMemFree(mixFormat);
            break;
        }

        captureChannels_ = acceptedFormat->nChannels;
        ::CoTaskMemFree(mixFormat);

        if (FAILED(w_->captureClient->GetBufferSize(&captureBufferFrames)) || captureBufferFrames == 0)
            break;

        if (FAILED(w_->captureClient->GetService(__uuidof(IAudioCaptureClient),
                                                 reinterpret_cast<void**>(w_->capture.put()))))
        {
            break;
        }

        w_->captureEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!w_->captureEvent || FAILED(w_->captureClient->SetEventHandle(w_->captureEvent)))
            break;

        // Four device buffers of slack. The two streams are event-driven off
        // separate clocks, so one will always be slightly ahead; enough room
        // to absorb that drift is the difference between an occasional gap and
        // a continuous one.
        //
        // ringFrames_ is stored LAST, with release ordering, and it is the
        // only thing ringRead consults before touching ring_. That is what
        // makes this allocation safe to perform while the render loop is
        // already spinning: until the store lands the reader sees zero and
        // emits silence, and once it does, the assign above happened-before.
        ring_.assign(static_cast<size_t>(captureBufferFrames) * 4 * captureChannels_, 0.0f);
        ringWritePos_ = 0;
        ringReadPos_  = 0;
        ringFrames_.store(captureBufferFrames * 4, std::memory_order_release);

        ok = true;
    }
    while (false);

    // Signalled whatever happened. Capture is optional, so open() does not read
    // this result - but it does WAIT for it, and a thread that never answered
    // would hang the app on startup instead of merely failing to record.
    w_->captureReady.set_value(ok);

    if (ok)
    {
        DWORD taskIndex = 0;
        const HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

        if (SUCCEEDED(w_->captureClient->Start()))
        {
            captureRunning_ = true;

            const HANDLE waitOn[2] = { w_->stopEvent, w_->captureEvent };

            for (;;)
            {
                const DWORD signalled = ::WaitForMultipleObjects(2, waitOn, FALSE, 2000);

                if (signalled != WAIT_OBJECT_0 + 1)
                    break;   // stop, timeout or failure - all end the stream

                // One event can cover several packets, and GetNextPacketSize
                // returning 0 is the only reliable "drained" signal.
                UINT32 packetFrames = 0;
                while (SUCCEEDED(w_->capture->GetNextPacketSize(&packetFrames)) && packetFrames > 0)
                {
                    BYTE* data{};
                    UINT32 frames = 0;
                    DWORD flags = 0;

                    if (FAILED(w_->capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                        break;

                    // AUDCLNT_BUFFERFLAGS_SILENT means the pointer is not
                    // required to hold anything - writing what it points at
                    // would be reading uninitialised memory.
                    if (data && frames > 0 && !(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                        ringWrite(reinterpret_cast<const float*>(data), frames, captureChannels_);

                    w_->capture->ReleaseBuffer(frames);
                }
            }

            captureRunning_ = false;
            w_->captureClient->Stop();
        }

        if (mmcss)
            ::AvRevertMmThreadCharacteristics(mmcss);
    }

    w_->capture.reset();
    w_->captureClient.reset();

    if (SUCCEEDED(comInit))
        ::CoUninitialize();
}

// --- capture -> render ring ----------------------------------------------
//
// Identical in shape to AudioDriverPipeWire's, and for the same reasons: one
// writer, one reader, both on device threads, nothing resized while either runs.

void AudioDriverWasapi::ringWrite(const float* interleaved, uint32_t frames, int srcChannels)
{
    // Relaxed: this thread is the one that published it.
    const uint32_t ringFrames = ringFrames_.load(std::memory_order_relaxed);

    if (ringFrames == 0 || !interleaved || captureChannels_ < 1)
        return;

    const auto write = ringWritePos_.load(std::memory_order_relaxed);
    const auto read  = ringReadPos_.load(std::memory_order_acquire);

    // Drop the OLDEST on overrun by advancing the reader, not by refusing to
    // write: a reader that has stopped must not freeze the input at the moment
    // it stalled.
    const uint32_t used = write - read;
    if (used + frames > ringFrames)
        ringReadPos_.store(write + frames - ringFrames, std::memory_order_release);

    for (uint32_t i = 0; i < frames; ++i)
    {
        const uint32_t slot = (write + i) % ringFrames;
        for (int ch = 0; ch < captureChannels_; ++ch)
        {
            ring_[size_t(slot) * captureChannels_ + ch] =
                ch < srcChannels ? interleaved[size_t(i) * srcChannels + ch] : 0.0f;
        }
    }

    ringWritePos_.store(write + frames, std::memory_order_release);
}

void AudioDriverWasapi::ringRead(float* const* planarOut, int outChannels, uint32_t frames)
{
    // Acquire, and read ONCE. This is the gate on ring_ itself: a zero here
    // means the capture thread has not finished sizing the buffer (or there is
    // no capture at all), and touching ring_ then would race its assign.
    const uint32_t ringFrames = ringFrames_.load(std::memory_order_acquire);

    if (ringFrames == 0)
    {
        for (int ch = 0; ch < outChannels; ++ch)
            std::memset(planarOut[ch], 0, sizeof(float) * static_cast<size_t>(frames));
        return;
    }

    const int captureChannels = captureChannels_;

    const auto read  = ringReadPos_.load(std::memory_order_relaxed);
    const auto write = ringWritePos_.load(std::memory_order_acquire);
    const uint32_t available = write - read;

    for (uint32_t i = 0; i < frames; ++i)
    {
        const bool have = i < available;
        const uint32_t slot = have ? (read + i) % ringFrames : 0;

        for (int ch = 0; ch < outChannels; ++ch)
        {
            planarOut[ch][i] = (have && ch < captureChannels)
                             ? ring_[size_t(slot) * captureChannels + ch]
                             : 0.0f;
        }
    }

    ringReadPos_.store(read + (std::min)(available, frames), std::memory_order_release);
}

} // namespace standalone
} // namespace gmpi
