#include "AudioDriverPipeWire.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

// Likewise for the rate request. Spelled as a fraction ("1/48000"), and it is a
// request to reclock the GRAPH rather than a property of our stream - see the
// note at the top of the header.
#ifndef PW_KEY_NODE_RATE
#define PW_KEY_NODE_RATE "node.rate"
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

// Capture reports no state or format changes of its own: the rate belongs to
// the graph and the playback stream is what reads it back, so there is nothing
// here a second listener could learn. A capture that fails to negotiate simply
// leaves the ring empty (silence) rather than stopping the app.
const pw_stream_events captureEvents = []
{
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.process = PwGlue::captureProcess;
    return e;
}();

// --- asking the daemon what it has -----------------------------------------
// A one-shot registry walk on its own connection, run whenever the settings
// pane opens and once more per open(). It is NOT kept alive between calls:
// holding a core connection for the life of the app to answer a question asked
// twice a session costs a thread and a socket, and a list cached across a
// device being plugged in is worse than no list.

// Everything one walk brings back. The clock half comes from the CORE's own
// properties rather than from any node: the graph has one rate and one quantum,
// the daemon publishes both, and they are what a stream will actually be run at
// no matter which sink it lands on.
struct GraphInfo
{
    std::vector<DeviceInfo> sinks;
    std::vector<DeviceInfo> sources;

    int rate         = 0;   // default.clock.rate - what the graph is running at
    std::vector<int> allowedRates;  // default.clock.allowed-rates, if configured

    int quantum      = 0;   // default.clock.quantum - the graph's usual cycle
    int maxQuantum   = 0;   // default.clock.max-quantum - the biggest it can be
};

struct RegistryScan
{
    GraphInfo* out{};
    pw_main_loop* loop{};
    int pending = 0;         // sync id we are waiting for
    spa_hook coreHook{};
    spa_hook registryHook{};
};

// A property the daemon writes as a decimal integer.
int readIntProp(const spa_dict* props, const char* key)
{
    const char* value = props ? spa_dict_lookup(props, key) : nullptr;
    if (!value)
        return 0;

    const long parsed = std::strtol(value, nullptr, 10);
    return (parsed > 0 && parsed < 100000000) ? static_cast<int>(parsed) : 0;
}

// "[ 44100, 48000 ]", as the daemon writes default.clock.allowed-rates.
//
// Every run of digits in the string, which for a flat array of integers is the
// whole of the grammar - brackets and commas carry nothing this needs. Scanned
// by hand rather than handed to spa_json because this file is compiled against
// whatever PipeWire the distribution ships, and the spa_json spelling for
// "iterate an array of ints" is not the same across the releases the standalone
// is expected to build on.
std::vector<int> parseRateArray(const char* text)
{
    std::vector<int> rates;

    for (const char* p = text; p && *p; )
    {
        if (*p < '0' || *p > '9')
        {
            ++p;
            continue;
        }

        long long value = 0;
        while (*p >= '0' && *p <= '9')
        {
            if (value < 100000000)          // no overflow on a malformed value
                value = value * 10 + (*p - '0');
            ++p;
        }

        if (value > 0 && value <= 768000)
            rates.push_back(static_cast<int>(value));
    }

    std::sort(rates.begin(), rates.end());
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
    return rates;
}

void onCoreInfo(void* data, const pw_core_info* info)
{
    auto* scan = static_cast<RegistryScan*>(data);
    if (!info || !info->props)
        return;

    scan->out->rate       = readIntProp(info->props, "default.clock.rate");
    scan->out->quantum    = readIntProp(info->props, "default.clock.quantum");
    scan->out->maxQuantum = readIntProp(info->props, "default.clock.max-quantum");

    if (const char* allowed = spa_dict_lookup(info->props, "default.clock.allowed-rates"))
        scan->out->allowedRates = parseRateArray(allowed);
}

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
    (isSink ? scan->out->sinks : scan->out->sources).push_back(std::move(info));
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
    e.info    = onCoreInfo;
    e.done    = onCoreDone;
    return e;
}();

// Fills `graph`. False means there was no daemon to ask - told apart from "a
// daemon with nothing in it" so that devices() can still offer the default
// entry, sampleRates() can say it knows nothing rather than inventing a list,
// and open() can go on to fail with the message that names the daemon.
bool queryGraph(GraphInfo& graph)
{
    static PwLibrary library;

    RegistryScan scan;
    scan.out  = &graph;
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
    // exception, not the default. Named "System Default" because that is what
    // the other two shells call the same entry - it used to read "Default
    // output (system)" here, which is the same thing said differently on the
    // one page all three share.
    out.push_back({ defaultDeviceId(), "System Default" });

    GraphInfo graph;
    if (!queryGraph(graph))
        return out;

    out.insert(out.end(), graph.sinks.begin(), graph.sinks.end());
    return out;
}

std::vector<int> AudioDriverPipeWire::sampleRates(const std::string& deviceId)
{
    // The GRAPH's rates, not the sink's, and the device id is unused for
    // exactly that reason: every stream in a PipeWire graph runs at one rate,
    // whichever sink it is linked to, and the daemon publishes both what that
    // rate is and which others it will switch to.
    //
    //   default.clock.allowed-rates - the set the administrator permitted, and
    //     the only rates a node.rate request can actually reach. Those are the
    //     real choices, so those are what the settings pane offers.
    //   default.clock.rate alone - no allowed-rates configured, which is a
    //     graph that will not reclock. One entry: the rate you are going to get.
    //
    // This used to be the same six-rate wish-list the other two shells carried,
    // made true by letting the adapter resample into the graph. Every rate on
    // it "worked" and none of them was the hardware's.
    (void)deviceId;

    GraphInfo graph;
    if (!queryGraph(graph))
        return {};   // no daemon: nothing is known, and open() will say so

    if (!graph.allowedRates.empty())
        return graph.allowedRates;

    if (graph.rate > 0)
        return { graph.rate };

    // A daemon that published neither. Empty is the honest answer - see
    // AudioMidiDevices.h::sampleRates - and open() is then asked for no rate at
    // all, which is what this driver would rather be asked for anyway.
    return {};
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

    // What was granted, which is the whole point of not naming a rate in the
    // format we offered: the graph fills it in, and this is where the app finds
    // out what it is. getSampleRate() reports this, StandaloneHost builds the
    // plugin's processor against it, and nothing anywhere assumes the request
    // was honoured.
    //
    // Into deviceOutChannels_, NOT outChannels_: this is what the SINK takes,
    // and the plugin's own count is what open() was handed. Writing the sink's
    // count over the plugin's is how a mono plugin on a stereo sink used to end
    // up rendering two channels and being heard on the left speaker only.
    spa_audio_info_raw info{};
    if (spa_format_audio_raw_parse(param, &info) >= 0 && info.rate > 0 && info.channels > 0)
    {
        activeSampleRate_  = static_cast<int>(info.rate);
        deviceOutChannels_ = static_cast<int>(info.channels);
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

    // The SINK's channel count: this is the wire, not the plugin.
    const int deviceChannels = deviceOutChannels_;
    const uint32_t stride = sizeof(float) * static_cast<uint32_t>(deviceChannels);

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

            {
                // See AudioMidiDevices.h. Around the call rather than set once
                // for the thread, because this thread is PipeWire's and runs
                // every stream this process has.
                const ScopedNoDenormals noDenormals;

                client_->processAudio(
                    todo,
                    inChannels_ > 0 ? planarInPtr_.data() : nullptr, inChannels_,
                    planarOutPtr_.data(), outChannels_);
            }

            // Plugin renders planar; the wire format is interleaved F32. The
            // channel mapping is AudioDriver::open's, the same one the WASAPI
            // and CoreAudio drivers apply: a MONO plugin fills every channel of
            // the sink, anything wider fills the first N and the rest are
            // silent. This used to write silence for a mono plugin too, under a
            // comment rejecting that spread outright - and a mono synth on an
            // ordinary stereo desktop was then audible only on the left.
            for (int ch = 0; ch < deviceChannels; ++ch)
            {
                float* dst = out + size_t(done) * deviceChannels + ch;

                const float* src = nullptr;
                if (ch < outChannels_)
                    src = planarOutPtr_[static_cast<size_t>(ch)];
                else if (outChannels_ == 1)
                    src = planarOutPtr_[0];

                if (src)
                {
                    for (int i = 0; i < todo; ++i)
                        dst[size_t(i) * deviceChannels] = src[i];
                }
                else
                {
                    for (int i = 0; i < todo; ++i)
                        dst[size_t(i) * deviceChannels] = 0.0f;
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

bool AudioDriverPipeWire::openCapture(const std::string& deviceId)
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
    // No rate, for the same reason as the playback stream: a rate left out of
    // the format is the graph's, arriving unconverted. It cannot disagree with
    // what playback got - one graph, one clock - which is what makes the ring
    // buffer between the two a matter of timing rather than of pitch.
    info.rate     = 0;
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
    deviceOutChannels_ = outChannels_;   // until negotiation says otherwise

    // What the daemon is clocked at and how large a cycle it will hand over.
    // Asked before the stream is built because both numbers shape what is asked
    // for below. The return value is not tested: nothing here can be done about
    // a missing daemon, and the stream connect that follows is where one is
    // reported.
    GraphInfo graph;
    queryGraph(graph);

    // A starting value for getSampleRate(), replaced by onParamChanged with
    // what the graph actually granted - which is the number that matters and
    // the only one the plugin's processor is ever built against. This one is
    // read solely in the corner where the negotiated format could not be
    // parsed, so it descends from the graph's own rate to the request to a bare
    // 48000, each rung being what is still known at that point.
    activeSampleRate_ = graph.rate > 0 ? graph.rate
                      : (requestedSampleRate > 0 ? requestedSampleRate : 48000);

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

    // ASK for the rate here, and only here. This is a request to the daemon to
    // reclock the whole graph, which it grants for a rate in
    // default.clock.allowed-rates and ignores otherwise - so it can move the
    // hardware but can never produce a resampler, which is the distinction the
    // whole policy turns on. 0 means the settings pane had nothing to offer
    // (sampleRates() came back empty) and the graph is left exactly as it is.
    //
    // The negotiated FORMAT below deliberately names no rate at all. Those two
    // facts together are what makes the plugin run at the hardware's rate: we
    // may ask the graph to move, and then we take whatever it settled on.
    if (requestedSampleRate > 0)
        pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%d", requestedSampleRate);

    // The buffer preference is a request the graph may quantise or override;
    // frames-at-our-rate is the documented form. Expressed against the rate we
    // are asking for, or the graph's current one when we are asking for none -
    // a latency fraction is only a duration, so the denominator has to be the
    // rate the numerator was counted at.
    if (requestedBufferFrames > 0)
    {
        pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d", requestedBufferFrames,
                           requestedSampleRate > 0 ? requestedSampleRate : activeSampleRate_);
    }

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
        // NO RATE. spa_format_audio_raw_build leaves the field out of the pod
        // entirely when it is zero, and a format with no rate in it is how a
        // pw_stream says "whatever the graph is running at" - which is the one
        // request the adapter can satisfy without putting a resampler in front
        // of us. Naming a rate here is what made this driver resample; the ask
        // now lives in node.rate above, where the answer is yes or no rather
        // than yes-with-a-converter.
        info.rate     = 0;
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

    // The block size the plugin is started with, and the MAXIMUM it will ever
    // be handed - onProcess chunks a longer cycle down to it, so this is a
    // promise the driver keeps rather than one the graph makes.
    //
    // Bounded by the daemon's own default.clock.max-quantum, which is the
    // largest cycle the graph can ever run: past that there is nothing to
    // report but buffers no cycle will fill, and this number is what the
    // settings pane prints as fact and what the plugin's maxBlockSize is built
    // from. With no request at all it is the graph's usual quantum, which is a
    // real number read from the server rather than the flat 512 that used to be
    // invented here.
    {
        int blockFrames = requestedBufferFrames > 0
                        ? requestedBufferFrames
                        : (graph.quantum > 0 ? graph.quantum : 512);

        if (graph.maxQuantum > 0)
            blockFrames = (std::min)(blockFrames, graph.maxQuantum);

        // A floor, because everything below is sized from it and a graph
        // configured with a tiny max-quantum should not turn into a scratch
        // buffer of four frames.
        activeBufferFrames_ = (std::max)(16, blockFrames);
    }

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
        if (!openCapture(deviceId))
        {
            // A DEGRADED open, not a failed one: a synth with an unused
            // audio-in pin must still play on a machine with no microphone.
            //
            // So warning_, and NOT lastError_, which used to be written here on
            // the way to returning true. AudioMidiDevices.h reserves that string
            // for an open that failed - StandaloneHost reads it only on a false
            // return and the settings pane shows either it or "Running", never
            // both - so the sentence was invisible where it was, and left behind
            // to be read next to an unrelated audio failure later in the
            // session. warning_ is the channel for a degraded open, and the
            // settings page has a line of its own for it.
            //
            // stderr as well, for whoever is reading a log. This is the same
            // place and the same shape as the note AudioDriverCoreAudio prints
            // for the identical situation.
            warning_ = "Audio input is silent: no audio input could be opened.";

            std::fprintf(stderr, "%s The plugin's %d input(s) will be silent.\n",
                         warning_.c_str(), inChannels_);
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

    // The one place it is cleared - AudioMidiDevices.h::lastWarning promises a
    // closed driver has nothing to say, and open() begins here.
    warning_.clear();

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
