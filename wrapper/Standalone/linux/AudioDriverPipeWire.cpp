#include "AudioDriverPipeWire.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/json.h>

// PW_KEY_TARGET_OBJECT is how a stream asks to land on a particular node, and
// it arrived in 0.3.44. Older headers only have node.target, which the server
// still honours. Defining the newer name against the older key keeps one code
// path rather than two.
#ifndef PW_KEY_TARGET_OBJECT
#define PW_KEY_TARGET_OBJECT "target.object"
#endif

namespace gmpi
{
namespace standalone
{

struct AudioDriverPipeWire::Pw
{
    pw_thread_loop* loop{};
    pw_stream*      stream{};    // playback: drives the plugin
    pw_stream*      capture{};   // input, when the plugin has audio in pins
};

// pw_stream_events wants C function pointers whose signatures use PipeWire's
// enums, so they cannot be declared in the header without dragging PipeWire in.
struct PwGlue
{
    static void process(void* self)       { static_cast<AudioDriverPipeWire*>(self)->onProcess(); }
    static void captureProcess(void* self){ static_cast<AudioDriverPipeWire*>(self)->onCaptureProcess(); }

    static void stateChanged(void* self, pw_stream_state, pw_stream_state newState, const char* error)
    {
        static_cast<AudioDriverPipeWire*>(self)->onStateChanged(static_cast<int>(newState), error);
    }

    static void paramChanged(void* self, uint32_t id, const spa_pod* param)
    {
        static_cast<AudioDriverPipeWire*>(self)->onParamChanged(id, param);
    }
};

namespace
{

// pw_init is process-wide and refcounted; one static pairs it with pw_deinit.
struct PwLibrary
{
    PwLibrary()  { pw_init(nullptr, nullptr); }
    ~PwLibrary() { pw_deinit(); }
};

// Any pw_stream_* call from outside the loop thread must hold the thread-loop
// lock. Taking it also waits out an in-flight process callback, which is
// exactly the guarantee close() owes its caller.
struct LoopLock
{
    explicit LoopLock(pw_thread_loop* l) : loop(l) { if (loop) pw_thread_loop_lock(loop); }
    ~LoopLock() { if (loop) pw_thread_loop_unlock(loop); }
    pw_thread_loop* loop;
};

const pw_stream_events streamEvents = []
{
    pw_stream_events e{};
    e.version       = PW_VERSION_STREAM_EVENTS;
    e.process       = PwGlue::process;
    e.state_changed = PwGlue::stateChanged;
    e.param_changed = PwGlue::paramChanged;
    return e;
}();

// Capture reports no state or format changes of its own: the playback stream
// owns the plugin's rate, and a capture that fails to negotiate simply leaves
// the ring empty (silence) rather than stopping the app.
const pw_stream_events captureEvents = []
{
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.process = PwGlue::captureProcess;
    return e;
}();

// --- device enumeration ----------------------------------------------------
// A one-shot registry walk on its own connection, run whenever the settings
// pane opens. It is NOT kept alive between calls: holding a core connection
// for the life of the app to answer a question asked twice a session costs a
// thread and a socket, and a list cached across a device being plugged in is
// worse than no list.

struct RegistryScan
{
    std::vector<DeviceInfo> sinks;
    std::vector<DeviceInfo> sources;
    pw_main_loop* loop{};
    int pending = 0;         // sync id we are waiting for
    spa_hook coreHook{};
    spa_hook registryHook{};
};

void onRegistryGlobal(void* data, uint32_t /*id*/, uint32_t /*permissions*/,
                      const char* type, uint32_t /*version*/, const spa_dict* props)
{
    auto* scan = static_cast<RegistryScan*>(data);

    if (!props || !type || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
        return;

    const char* mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!mediaClass)
        return;

    const bool isSink   = std::strcmp(mediaClass, "Audio/Sink") == 0;
    const bool isSource = std::strcmp(mediaClass, "Audio/Source") == 0;
    if (!isSink && !isSource)
        return;

    // node.name is the routing key (what PW_KEY_TARGET_OBJECT matches) and is
    // stable across restarts; node.description is what the desktop shows.
    // Without a node.name there is nothing to persist, so skip the node.
    const char* nodeName = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (!nodeName)
        return;

    const char* description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);

    DeviceInfo info{ nodeName, description ? description : nodeName };
    (isSink ? scan->sinks : scan->sources).push_back(std::move(info));
}

const pw_registry_events registryEvents = []
{
    pw_registry_events e{};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global  = onRegistryGlobal;
    return e;
}();

void onCoreDone(void* data, uint32_t id, int seq)
{
    auto* scan = static_cast<RegistryScan*>(data);

    // The registry emits every existing global before answering our sync, so
    // "our sync came back" means "the walk is complete".
    if (id == PW_ID_CORE && seq == scan->pending)
        pw_main_loop_quit(scan->loop);
}

const pw_core_events coreEvents = []
{
    pw_core_events e{};
    e.version = PW_VERSION_CORE_EVENTS;
    e.done    = onCoreDone;
    return e;
}();

// Fills `sinks` and `sources`. Returns false when there is no daemon, which
// the caller reports as "PipeWire is not running" rather than as an empty list
// - an empty list looks like a machine with no soundcard.
bool scanRegistry(std::vector<DeviceInfo>& sinks, std::vector<DeviceInfo>& sources)
{
    static PwLibrary library;

    RegistryScan scan;
    scan.loop = pw_main_loop_new(nullptr);
    if (!scan.loop)
        return false;

    pw_context* context = pw_context_new(pw_main_loop_get_loop(scan.loop), nullptr, 0);
    if (!context)
    {
        pw_main_loop_destroy(scan.loop);
        return false;
    }

    pw_core* core = pw_context_connect(context, nullptr, 0);
    if (!core)
    {
        pw_context_destroy(context);
        pw_main_loop_destroy(scan.loop);
        return false;
    }

    pw_registry* registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

    pw_core_add_listener(core, &scan.coreHook, &coreEvents, &scan);
    pw_registry_add_listener(registry, &scan.registryHook, &registryEvents, &scan);

    scan.pending = pw_core_sync(core, PW_ID_CORE, 0);

    pw_main_loop_run(scan.loop);

    spa_hook_remove(&scan.registryHook);
    spa_hook_remove(&scan.coreHook);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(scan.loop);

    sinks  = std::move(scan.sinks);
    sources = std::move(scan.sources);
    return true;
}

} // namespace

AudioDriverPipeWire::AudioDriverPipeWire() : pw_(std::make_unique<Pw>())
{
}

AudioDriverPipeWire::~AudioDriverPipeWire()
{
    close();
    teardownLoop();
}

std::vector<DeviceInfo> AudioDriverPipeWire::devices()
{
    std::vector<DeviceInfo> out;

    // Always first, always present. Routing is the desktop's job and its mixer
    // can move a running stream anywhere; picking a named sink is the
    // exception, not the default.
    out.push_back({ defaultDeviceId(), "Default output (system)" });

    std::vector<DeviceInfo> sinks;
    std::vector<DeviceInfo> sources;
    if (!scanRegistry(sinks, sources))
        return out;

    out.insert(out.end(), sinks.begin(), sinks.end());
    return out;
}

std::vector<int> AudioDriverPipeWire::sampleRates()
{
    // PipeWire resamples between the stream's rate and the graph's, so every
    // one of these works whatever the daemon is clocked at. Offering them is
    // about the plugin's own rate, not the hardware's.
    return { 44100, 48000, 88200, 96000, 176400, 192000 };
}

bool AudioDriverPipeWire::ensureLoop()
{
    static PwLibrary library;

    if (pw_->loop)
        return true;

    pw_->loop = pw_thread_loop_new("gmpi audio", nullptr);
    if (!pw_->loop)
    {
        lastError_ = "PipeWire: could not create a thread loop";
        return false;
    }

    if (pw_thread_loop_start(pw_->loop) != 0)
    {
        pw_thread_loop_destroy(pw_->loop);
        pw_->loop = nullptr;
        lastError_ = "PipeWire: could not start the audio thread";
        return false;
    }

    return true;
}

void AudioDriverPipeWire::teardownStreams()
{
    if (!pw_->stream && !pw_->capture)
        return;

    LoopLock lock(pw_->loop);

    if (pw_->capture)
    {
        pw_stream_disconnect(pw_->capture);
        pw_stream_destroy(pw_->capture);
        pw_->capture = nullptr;
    }
    if (pw_->stream)
    {
        pw_stream_disconnect(pw_->stream);
        pw_stream_destroy(pw_->stream);
        pw_->stream = nullptr;
    }
}

void AudioDriverPipeWire::teardownLoop()
{
    if (!pw_->loop)
        return;

    pw_thread_loop_stop(pw_->loop);
    pw_thread_loop_destroy(pw_->loop);
    pw_->loop = nullptr;
}

// --- stream callbacks -------------------------------------------------------
// process() runs on the loop's RT thread; the others run on the loop thread
// with its lock held.

void AudioDriverPipeWire::onStateChanged(int newState, const char* error)
{
    if (newState == PW_STREAM_STATE_ERROR && streamRunning_)
        lastError_ = std::string("PipeWire: ") + (error ? error : "stream error");

    // open() may be sitting in a timed wait for negotiation to land.
    pw_thread_loop_signal(pw_->loop, false);
}

void AudioDriverPipeWire::onParamChanged(uint32_t id, const spa_pod* param)
{
    if (id != SPA_PARAM_Format || !param)
        return;

    // What was granted, not what was asked for. The adapter normally gives a
    // stream exactly the format it requested (resampling behind the scenes),
    // but the plugin's clock must follow reality.
    spa_audio_info_raw info{};
    if (spa_format_audio_raw_parse(param, &info) >= 0 && info.rate > 0 && info.channels > 0)
    {
        activeSampleRate_ = static_cast<int>(info.rate);
        outChannels_      = static_cast<int>(info.channels);
    }
}

void AudioDriverPipeWire::ringWrite(const float* interleaved, uint32_t frames, int srcChannels)
{
    if (ringFrames_ == 0 || !interleaved || captureChannels_ < 1)
        return;

    const auto write = ringWritePos_.load(std::memory_order_relaxed);
    const auto read  = ringReadPos_.load(std::memory_order_acquire);

    // Drop the OLDEST on overrun by advancing the reader, not by refusing to
    // write: a reader that has stopped must not freeze the input at the moment
    // it stalled.
    const uint32_t used = write - read;
    if (used + frames > ringFrames_)
        ringReadPos_.store(write + frames - ringFrames_, std::memory_order_release);

    for (uint32_t i = 0; i < frames; ++i)
    {
        const uint32_t slot = (write + i) % ringFrames_;
        for (int ch = 0; ch < captureChannels_; ++ch)
        {
            ring_[size_t(slot) * captureChannels_ + ch] =
                ch < srcChannels ? interleaved[size_t(i) * srcChannels + ch] : 0.0f;
        }
    }

    ringWritePos_.store(write + frames, std::memory_order_release);
}

void AudioDriverPipeWire::ringRead(float* const* planarOut, int outChannels, uint32_t frames)
{
    const auto read  = ringReadPos_.load(std::memory_order_relaxed);
    const auto write = ringWritePos_.load(std::memory_order_acquire);
    const uint32_t available = write - read;

    for (uint32_t i = 0; i < frames; ++i)
    {
        const bool have = ringFrames_ > 0 && i < available;
        const uint32_t slot = have ? (read + i) % ringFrames_ : 0;

        for (int ch = 0; ch < outChannels; ++ch)
        {
            planarOut[ch][i] = (have && ch < captureChannels_)
                             ? ring_[size_t(slot) * captureChannels_ + ch]
                             : 0.0f;
        }
    }

    ringReadPos_.store(read + (std::min)(available, frames), std::memory_order_release);
}

void AudioDriverPipeWire::onCaptureProcess()
{
    pw_buffer* b = pw_stream_dequeue_buffer(pw_->capture);
    if (!b)
        return;

    auto& data = b->buffer->datas[0];
    if (const auto* in = static_cast<const float*>(data.data); in && data.chunk && captureRunning_)
    {
        const uint32_t stride = sizeof(float) * uint32_t(captureChannels_);
        if (stride > 0)
            ringWrite(in, data.chunk->size / stride, captureChannels_);
    }

    pw_stream_queue_buffer(pw_->capture, b);
}

void AudioDriverPipeWire::onProcess()
{
    pw_buffer* b = pw_stream_dequeue_buffer(pw_->stream);
    if (!b)
        return;

    auto& data = b->buffer->datas[0];
    auto* out = static_cast<float*>(data.data);
    const uint32_t stride = sizeof(float) * static_cast<uint32_t>(outChannels_);

    uint32_t frames = stride ? data.maxsize / stride : 0;

    // pw_buffer::requested - how many frames the graph wants THIS cycle - only
    // exists from 0.3.49. Without it the buffer's full capacity is the only
    // number available, which is what every client did before the field
    // landed: correct, just less efficient when the quantum is small.
#if PW_CHECK_VERSION(0, 3, 49)
    if (b->requested > 0)
        frames = (std::min)(frames, static_cast<uint32_t>(b->requested));
#endif

    if (!out || !client_ || !streamRunning_ || activeBufferFrames_ < 1)
    {
        if (out)
            std::memset(out, 0, frames * stride);
    }
    else
    {
        // Chunk to the block size the plugin was started with: the quantum is
        // the server's to choose, can change while running, and can exceed
        // anything that was requested.
        for (uint32_t done = 0; done < frames;)
        {
            const int todo = (std::min)(static_cast<int>(frames - done), activeBufferFrames_);

            if (inChannels_ > 0)
                ringRead(planarInPtr_.data(), inChannels_, uint32_t(todo));

            client_->processAudio(
                todo,
                inChannels_ > 0 ? planarInPtr_.data() : nullptr, inChannels_,
                planarOutPtr_.data(), static_cast<int>(planarOutPtr_.size()));

            // Plugin renders planar; the wire format is interleaved F32.
            for (int ch = 0; ch < outChannels_; ++ch)
            {
                float* dst = out + size_t(done) * outChannels_ + ch;

                if (static_cast<size_t>(ch) < planarOutPtr_.size())
                {
                    const float* src = planarOutPtr_[ch];
                    for (int i = 0; i < todo; ++i)
                        dst[size_t(i) * outChannels_] = src[i];
                }
                else
                {
                    // The device has more channels than the plugin drives (a
                    // mono plugin on a stereo sink). Silence, not a copy of
                    // channel 0 - the settings pane says what the plugin is.
                    for (int i = 0; i < todo; ++i)
                        dst[size_t(i) * outChannels_] = 0.0f;
                }
            }

            done += todo;
        }
    }

    data.chunk->offset = 0;
    data.chunk->stride = static_cast<int32_t>(stride);
    data.chunk->size   = frames * stride;
    pw_stream_queue_buffer(pw_->stream, b);
}

bool AudioDriverPipeWire::openCapture(int rate, const std::string& deviceId)
{
    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE,     "Production",
        PW_KEY_APP_NAME,       "GMPI Standalone",
        PW_KEY_NODE_NAME,      "GMPI Standalone capture",
        nullptr);

    // Capture is never pinned to the playback device's node - they are
    // different nodes - so the input always follows the desktop's default
    // source. Choosing an input device is a separate setting this app does not
    // offer yet; letting the desktop route it is the honest behaviour.
    (void)deviceId;

    pw_->capture = pw_stream_new_simple(
        pw_thread_loop_get_loop(pw_->loop), "GMPI Standalone capture", props, &captureEvents, this);
    if (!pw_->capture)
        return false;

    uint8_t podBuffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
    spa_audio_info_raw info{};
    info.format   = SPA_AUDIO_FORMAT_F32;
    info.channels = uint32_t(captureChannels_);
    info.rate     = uint32_t(rate);
    const spa_pod* params[1] = { spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info) };

    // No INACTIVE flag and no negotiation wait: capture is optional, so it must
    // never delay or fail the playback path. If it cannot link, the ring stays
    // empty and the plugin hears silence.
    if (pw_stream_connect(
            pw_->capture,
            PW_DIRECTION_INPUT,
            PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
            params, 1) < 0)
    {
        pw_stream_destroy(pw_->capture);
        pw_->capture = nullptr;
        return false;
    }

    return true;
}

bool AudioDriverPipeWire::open(
    const std::string& deviceId,
    int requestedSampleRate,
    int requestedBufferFrames,
    int inChannels,
    int outChannels,
    AudioCallback* client)
{
    lastError_.clear();

    close();

    if (!ensureLoop())
        return false;

    client_ = client;

    // A plugin with no audio outputs still needs a stream to be clocked by;
    // give it a stereo sink and let it write nothing.
    outChannels_ = (std::max)(1, outChannels);
    inChannels_  = (std::max)(0, inChannels);

    const int rate = requestedSampleRate > 0 ? requestedSampleRate : 48000;
    activeSampleRate_ = rate;

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE,     "Production",
        PW_KEY_APP_NAME,       "GMPI Standalone",
        PW_KEY_NODE_NAME,      "GMPI Standalone",
        nullptr);

    // A named sink, or nothing at all for "default". Setting target.object to
    // an empty string is NOT the same as leaving it unset - the server reads
    // that as "link to the node called ''" and the stream never connects.
    if (!deviceId.empty() && deviceId != defaultDeviceId())
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, deviceId.c_str());

    // The buffer preference is a request the graph may quantise or override;
    // frames-at-our-rate is the documented form.
    if (requestedBufferFrames > 0)
        pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d", requestedBufferFrames, rate);

    {
        LoopLock lock(pw_->loop);

        pw_->stream = pw_stream_new_simple(
            pw_thread_loop_get_loop(pw_->loop), "GMPI Standalone", props, &streamEvents, this);
        if (!pw_->stream)
        {
            lastError_ = "PipeWire: could not create a stream";
            return false;
        }

        uint8_t podBuffer[1024];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
        spa_audio_info_raw info{};
        info.format   = SPA_AUDIO_FORMAT_F32;
        info.channels = static_cast<uint32_t>(outChannels_);
        info.rate     = static_cast<uint32_t>(rate);
        const spa_pod* params[1] = { spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info) };

        // INACTIVE: negotiate now, run at the end of open(). Errors (no
        // daemon, no sink) surface here, where the app can still offer a
        // different device, rather than mid-run.
        if (pw_stream_connect(
                pw_->stream,
                PW_DIRECTION_OUTPUT,
                PW_ID_ANY,
                static_cast<pw_stream_flags>(
                    PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_INACTIVE),
                params, 1) < 0)
        {
            lastError_ = "PipeWire: stream connect failed";
            return false;
        }

        // Wait for negotiation: CONNECTING -> PAUSED on success, ERROR when
        // there is no daemon or nothing to link to. State changes signal the
        // loop; the timeout is only a backstop against a missed signal.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;)
        {
            const char* error = nullptr;
            const auto state = pw_stream_get_state(pw_->stream, &error);

            if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING)
                break;

            if (state == PW_STREAM_STATE_ERROR)
            {
                lastError_ = std::string("PipeWire: ") + (error ? error : "stream error");
                return false;
            }

            if (std::chrono::steady_clock::now() >= deadline)
            {
                lastError_ = "PipeWire: stream negotiation timed out (is the daemon running?)";
                return false;
            }

            pw_thread_loop_timed_wait(pw_->loop, 1);
        }
    }

    // The block size the plugin is started with. With no request, the graph's
    // quantum is unknown until the first process callback, so allocate a
    // generous default and let onProcess chunk into it.
    activeBufferFrames_ = requestedBufferFrames > 0 ? (std::max)(16, requestedBufferFrames) : 512;

    planarOut_.assign(static_cast<size_t>(outChannels_), std::vector<float>(static_cast<size_t>(activeBufferFrames_), 0.0f));
    planarOutPtr_.clear();
    for (auto& buffer : planarOut_)
        planarOutPtr_.push_back(buffer.data());

    if (inChannels_ > 0)
    {
        captureChannels_ = inChannels_;

        planarIn_.assign(static_cast<size_t>(inChannels_), std::vector<float>(static_cast<size_t>(activeBufferFrames_), 0.0f));
        planarInPtr_.clear();
        for (auto& buffer : planarIn_)
            planarInPtr_.push_back(buffer.data());

        // Four quanta of slack. The two streams normally run in the same graph
        // cycle, so one would nearly do; the extra absorbs a late capture
        // callback without dropping audio.
        ringFrames_ = uint32_t(activeBufferFrames_) * 4;
        ring_.assign(size_t(ringFrames_) * size_t(captureChannels_), 0.0f);
        ringWritePos_ = 0;
        ringReadPos_  = 0;

        LoopLock lock(pw_->loop);
        if (!openCapture(activeSampleRate_, deviceId))
        {
            // Not fatal, and deliberately not an error the caller sees: a synth
            // with an unused audio-in pin must still play on a machine with no
            // microphone.
            lastError_ = "PipeWire: no audio input available (playing anyway)";
        }
    }

    streamRunning_ = true;

    {
        LoopLock lock(pw_->loop);
        pw_stream_set_active(pw_->stream, true);

        if (pw_->capture)
        {
            captureRunning_ = true;
            pw_stream_set_active(pw_->capture, true);
        }
    }

    return true;
}

void AudioDriverPipeWire::close()
{
    streamRunning_  = false;
    captureRunning_ = false;

    if (pw_->stream || pw_->capture)
    {
        // Blocks until any in-flight process callback has finished, which is
        // what makes it safe for the caller to free the plugin afterwards.
        LoopLock lock(pw_->loop);
        if (pw_->stream)  pw_stream_set_active(pw_->stream, false);
        if (pw_->capture) pw_stream_set_active(pw_->capture, false);
    }

    teardownStreams();

    client_ = nullptr;
}

} // namespace standalone
} // namespace gmpi
