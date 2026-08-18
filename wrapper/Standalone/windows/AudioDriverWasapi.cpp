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

// Resolves a device id to an endpoint, falling back to the system default.
// A saved id names hardware that may since have been unplugged, and refusing
// to make a sound because of a stale settings file is not a useful reaction.
HRESULT openEndpoint(IMMDeviceEnumerator* enumerator,
                     const std::string& deviceId,
                     EDataFlow flow,
                     IMMDevice** returnDevice)
{
    // The constant, not the virtual: this helper has no driver instance. Same
    // string either way - see the note on kDefaultDeviceId.
    if (!deviceId.empty() && deviceId != AudioDriverWasapi::kDefaultDeviceId)
    {
        const auto wide = gmpi::unicode::to_wide(deviceId);
        if (SUCCEEDED(enumerator->GetDevice(wide.c_str(), returnDevice)))
            return S_OK;
    }

    return enumerator->GetDefaultAudioEndpoint(flow, eConsole, returnDevice);
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

    // Parameters open() parked for the threads to act on. The rate keeps its
    // "0 means no preference" meaning all the way down here - see the note on
    // requestedRate in renderThread(), which is the one thing that reads it.
    std::string deviceId;
    int requestedSampleRate  = 0;
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

std::vector<int> AudioDriverWasapi::sampleRates(const std::string& deviceId)
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(enumerator.put()))))
    {
        return {};
    }

    ComPtr<IMMDevice> device;
    if (FAILED(openEndpoint(enumerator.get(), deviceId, eRender, device.put())))
        return {};

    // Activated on the CALLING thread, used on it, and released before this
    // returns - the second of the two COM-apartment cases at the top of the
    // header, and what lets this run on the UI thread's STA at all: unlike the
    // render client, nothing here outlives the call that created it.
    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(client.put()))))
    {
        return {};
    }

    WAVEFORMATEX* mixFormat{};
    if (FAILED(client->GetMixFormat(&mixFormat)) || !mixFormat)
        return {};

    // The mix rate is in the list unconditionally, because it is the rate the
    // engine is clocked at and the last rung of open()'s ladder can always
    // reach it.
    const int mixRate = static_cast<int>(mixFormat->nSamplesPerSec);
    std::vector<int> result{ mixRate };

    // The rest are PROBED rather than assumed. IsFormatSupported in shared mode
    // is the same question open() is about to ask the same endpoint, so an S_OK
    // here is a rate that will really open; S_FALSE means "no, but here is what
    // I would take instead", which for this probe is always the mix rate.
    //
    // In shared mode the engine converts word length and channel count but not
    // RATE, so on ordinary hardware every probe answers S_FALSE and this list
    // has one entry. It is written as a loop anyway because the answer belongs
    // to the endpoint: a virtual device that presents several rates is entitled
    // to say so, and hard-coding "the mix rate is the only rate" would make
    // this driver the thing that decided otherwise.
    static constexpr int kCandidates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };

    // The mix format with only the rate changed. Everything else about it is
    // known to be acceptable, so a refusal can only be about the rate - which
    // is not true of a format built from scratch, where a rejected channel mask
    // would read as a rejected rate.
    const size_t formatBytes = sizeof(WAVEFORMATEX) + mixFormat->cbSize;
    std::vector<uint8_t> probeStorage(formatBytes);

    for (const int rate : kCandidates)
    {
        if (rate == mixRate)
            continue;

        std::memcpy(probeStorage.data(), mixFormat, formatBytes);

        auto* probe = reinterpret_cast<WAVEFORMATEX*>(probeStorage.data());
        probe->nSamplesPerSec  = static_cast<DWORD>(rate);
        probe->nAvgBytesPerSec = probe->nSamplesPerSec * probe->nBlockAlign;

        WAVEFORMATEX* closest{};
        const HRESULT hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, probe, &closest);
        ::CoTaskMemFree(closest);

        if (hr == S_OK)
            result.push_back(rate);
    }

    ::CoTaskMemFree(mixFormat);

    std::sort(result.begin(), result.end());
    return result;
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
    // NOT substituted with a rate of this driver's own choosing. A 0 here means
    // the caller has no preference - a fresh install, or a device whose rates
    // could not be enumerated - and the ladder in renderThread() answers that by
    // opening at whatever the endpoint is already clocked at. Substituting 48000
    // would turn "no preference" into a request that fails on every 44100 device
    // and then falls back to the same place, one wasted Initialize later.
    w_->requestedSampleRate   = (std::max)(0, requestedSampleRate);
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

    // After the joins above, so the capture thread that writes it has finished.
    // This is the ONE place it is cleared, which is enough for the contract in
    // AudioMidiDevices.h::lastWarning because open() begins with close().
    warning_.clear();

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
        //
        // NOTHING HERE RESAMPLES. The flags used to carry
        // AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | ..._SRC_DEFAULT_QUALITY, which
        // put the shared-mode engine's converter in front of the stream so that
        // the first rung always succeeded - the plugin then ran at whatever the
        // settings file said while the hardware ran at something else, paying
        // for a converter in the middle. Without them a rate the engine is not
        // clocked at is simply refused, and the ladder walks down to a rate the
        // endpoint really has. See AudioMidiDevices.h::sampleRates.
        constexpr DWORD kStreamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

        // 0 means "no preference", which the settings pane sends when
        // sampleRates() offered nothing to choose from. The engine's own rate
        // is the answer to that, and rung 1 below is skipped rather than being
        // asked for a nonsense format.
        const int requestedRate = w_->requestedSampleRate;

        const auto wanted = makeFloatFormat(outChannels_, requestedRate);

        const WAVEFORMATEX* acceptedFormat = &wanted.Format;

        hr = requestedRate > 0
           ? tryInitialise(device.get(), &wanted.Format, kStreamFlags,
                           framesToRefTime(w_->requestedBufferFrames, requestedRate),
                           w_->renderClient)
           : E_FAIL;

        // Declared out here because acceptedFormat may end up pointing at it,
        // and it is read below the block that fills it.
        WAVEFORMATEXTENSIBLE atEngineRate{};
        WAVEFORMATEX* mixFormat{};

        if (FAILED(hr))
        {
            // Ask the endpoint what it runs at and take that instead. The
            // plugin then runs at the hardware's rate rather than the one in
            // settings - which StandaloneHost is told about, because it reads
            // getSampleRate() rather than assuming its request was honoured.
            ComPtr<IAudioClient> probe;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(probe.put()))))
            {
                probe->GetMixFormat(&mixFormat);
            }

            if (mixFormat)
            {
                const int engineRate = static_cast<int>(mixFormat->nSamplesPerSec);
                const REFERENCE_TIME engineDuration =
                    framesToRefTime(w_->requestedBufferFrames, engineRate);

                // The plugin's own channel count at the engine's rate, BEFORE
                // the mix format itself - and what that is worth is narrower
                // than it looks. Without AUTOCONVERTPCM the shared-mode engine
                // converts WORD LENGTH but not channel count, so this rung
                // succeeds only where outChannels_ already equals the mix
                // format's channel count - which is exactly where rung 3 would
                // have succeeded too. On the surround card that would make the
                // difference, this rung fails and rung 3 hands over eight
                // channels anyway.
                //
                // What it does buy, and the reason it is still here: an
                // endpoint whose mix format is NOT float32 would pass rung 3
                // and then fail the isFloat32 gate below, taking the whole open
                // with it. Asking for float32 explicitly gets the engine to
                // convert the word length, and that rescues the open whenever
                // the channel counts already match. Rare - every ordinary
                // endpoint's mix format is float32 - but it costs one
                // Initialize on a path that has already failed one.
                atEngineRate = makeFloatFormat(outChannels_, engineRate);

                hr = tryInitialise(device.get(), &atEngineRate.Format, kStreamFlags,
                                   engineDuration, w_->renderClient);
                if (SUCCEEDED(hr))
                {
                    acceptedFormat = &atEngineRate.Format;
                }
                else if (SUCCEEDED(tryInitialise(device.get(), mixFormat, kStreamFlags,
                                                 engineDuration, w_->renderClient)))
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

                {
                    // Per call rather than once for the thread, so that all
                    // three drivers apply it the same way - the other two are
                    // handed a thread they do not own and have no other option.
                    const ScopedNoDenormals noDenormals;

                    client_->processAudio(
                        static_cast<int>(frames),
                        inChannels_ > 0 ? planarInPtr_.data() : nullptr, inChannels_,
                        planarOutPtr_.data(), outChannels_);
                }

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

    // A mapping at all only when the retry ladder had to fall back to the
    // endpoint's mix format; the two rungs above it ask for the plugin's own
    // channel count, and then this is a straight interleave.
    //
    // The mapping is AudioDriver::open's - one statement, three drivers: a MONO
    // plugin fills every channel the endpoint has, anything wider fills the
    // first N and the rest are silent, and an endpoint narrower than the plugin
    // drops the extras rather than folding them down. The loop below is that
    // rule and nothing else.
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

    // Published below when the input did not open, because on this platform
    // that is an ordinary thing to happen - a desktop with no microphone, or
    // the two endpoints on different Default Formats - and the plugin then
    // hears silence with nothing anywhere to say why.
    //
    // warning_ rather than lastError_, which AudioMidiDevices.h reserves for an
    // open that FAILED, and this one has not: the plugin still plays. It also
    // goes to stderr, but stderr alone was not enough - a standalone started
    // from Explorer has no console, and the settings page was left saying
    // "Running" with every input pin silent.
    std::string whyNoInput = "Audio input is silent: no recording device is available.";

    // Whatever will not fit on the settings pane's line. Empty for every case
    // that has nothing further to add.
    const char* advice = "";

    do
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(enumerator.put()))))
        {
            whyNoInput = "Audio input is silent: the Windows Audio service could not be reached.";
            break;
        }

        // Always the default input. The settings page offers no capture choice,
        // deliberately: a standalone is for auditioning a plugin, and a second
        // device list to get wrong is not what makes that work. The output
        // device id is not reused here - it names a render endpoint.
        ComPtr<IMMDevice> device;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, device.put())))
            break;

        // Capture follows the RENDER stream's rate, not the request: the two are
        // read and written in the same callback and there is nothing between
        // them that could resample. So the input opens at activeSampleRate_ or
        // it does not open, which is what the rate test on the fallback below is
        // for - the converting flags that used to be here hid the mismatch by
        // resampling, and taking the endpoint's mix format without checking its
        // rate would replace that with input arriving at the wrong speed.
        const auto wanted = makeFloatFormat(inChannels_, activeSampleRate_);

        constexpr DWORD kStreamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

        const WAVEFORMATEX* acceptedFormat = &wanted.Format;
        const REFERENCE_TIME duration = framesToRefTime(w_->requestedBufferFrames, activeSampleRate_);

        HRESULT hr = tryInitialise(device.get(), &wanted.Format, kStreamFlags,
                                   duration, w_->captureClient);

        WAVEFORMATEX* mixFormat{};
        if (FAILED(hr))
        {
            // There IS a recording endpoint and it turned the format down, so
            // the "no recording device" default above has stopped being true.
            whyNoInput = "Audio input is silent: the recording device could not be opened.";

            ComPtr<IAudioClient> probe;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(probe.put()))))
            {
                probe->GetMixFormat(&mixFormat);
            }

            const int captureRate = mixFormat ? static_cast<int>(mixFormat->nSamplesPerSec) : 0;

            if (captureRate > 0 && captureRate != activeSampleRate_)
            {
                // THE ordinary way for this to happen, and the reason any of
                // this is reported to the user at all: the two endpoints are
                // separate devices with separate Default Formats, so a 44100
                // microphone under 48000 speakers is a normal laptop rather
                // than a broken machine. AUTOCONVERTPCM used to absorb it by
                // resampling; without it the endpoint answers
                // AUDCLNT_E_UNSUPPORTED_FORMAT and the input is simply silent.
                //
                // Both numbers, because "the rates do not match" leaves the
                // user to go and find out which two.
                whyNoInput = "Audio input is silent: recording is "
                           + std::to_string(captureRate) + " Hz, playback "
                           + std::to_string(activeSampleRate_) + " Hz.";

                // Too long for the pane's one line, so stderr only - see
                // AudioMidiDevices.h::lastWarning.
                advice = " Both are set on the endpoints' Advanced pages in Windows Sound settings.";
            }
            else if (mixFormat && SUCCEEDED(tryInitialise(device.get(), mixFormat, kStreamFlags,
                                                          duration, w_->captureClient)))
            {
                acceptedFormat = mixFormat;
                hr = S_OK;
            }
        }

        // A device that refused and a device that opened in a format this
        // driver cannot read are two different things to tell the user, and
        // only the second one has an open stream behind it.
        if (SUCCEEDED(hr) && !isFloat32(acceptedFormat))
            whyNoInput = "Audio input is silent: the recording device's sample format cannot be read.";

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

    if (!ok)
    {
        // Both destinations get the SAME sentence, so a log and the settings
        // page describing one run cannot disagree; only the advice that would
        // not fit a line is stderr's alone.
        //
        // Safe to write warning_ from here: open() is inside captureReady.wait()
        // until this thread sets the promise below, and close() joins this
        // thread before clearing it.
        warning_ = whyNoInput;

        std::fprintf(stderr, "%s The plugin's %d input(s) will be silent.%s\n",
                     whyNoInput.c_str(), inChannels_, advice);
    }

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
