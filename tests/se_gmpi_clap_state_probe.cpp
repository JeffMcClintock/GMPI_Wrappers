/* se_gmpi_clap_state_probe -- the CLAP twin of se_gmpi_state_probe.cpp: which of a SynthEdit
 * export's parameters survive a CLAP state save, in a bare host with no DAW and no window.
 *
 * Stage 3 of TideSynth/docs/state-management-consolidation-plan.md moved the export's panel
 * controller (MpController) into the plug-in's own <Controller/>, so every GMPI wrapper gets
 * the engine preset for free. This probe is how that claim is measured on a second wrapper,
 * with the same reader as the VST3 probe (se_gmpi_state_analysis.h) so the two are quoted
 * side by side. Hosting shape follows TideSynth/tests/e79_clap_headless_probe.c.
 *
 *   se_gmpi_clap_state_probe <plugin.clap> [options]
 *
 *   --restore <preset.xml>   state->load this chunk (raw XML; the CLAP wrapper reads the
 *                            whole stream) before saving.
 *   --save <out.xml>         write the saved chunk here.
 *   --process <N>            run N blocks of audio between restore and save (default 8;
 *                            0 skips activation).
 *   --resources <dir>        the Resources folder holding parameters.se.xml, when it is
 *                            not <bundle dir>/Contents/Resources.
 *
 * Build (linux): g++ -std=c++17 -O1 -I <clap-include> se_gmpi_clap_state_probe.cpp -o se_gmpi_clap_state_probe -ldl
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dlfcn.h>
#include <sys/stat.h>
#include <clap/clap.h>
#include "se_gmpi_state_analysis.h"

using namespace se_state_analysis;

static int failures = 0;
static void check(const char* what, bool ok)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

/* ---- the smallest host a plugin will accept -------------------------------------------- */
static const void* host_get_extension(const clap_host_t*, const char*) { return nullptr; }
static void host_noop(const clap_host_t*) {}
static int restartRequested = 0;
static void host_request_restart(const clap_host_t*) { restartRequested = 1; }

static clap_host_t host = {
    CLAP_VERSION_INIT, nullptr, "se-gmpi-clap-state-probe", "SynthEdit", "", "1.0",
    host_get_extension, host_request_restart, host_noop, host_noop,
};

/* ---- streams ----------------------------------------------------------------------------- */
struct Sink { std::string bytes; };
static int64_t sink_write(const clap_ostream_t* s, const void* buffer, uint64_t size)
{
    static_cast<Sink*>(s->ctx)->bytes.append(static_cast<const char*>(buffer), static_cast<size_t>(size));
    return static_cast<int64_t>(size);
}
struct Source { const std::string* buf; size_t pos = 0; };
static int64_t source_read(const clap_istream_t* s, void* buffer, uint64_t size)
{
    auto* src = static_cast<Source*>(s->ctx);
    const size_t left = src->buf->size() - src->pos;
    const size_t n = size < left ? static_cast<size_t>(size) : left;
    memcpy(buffer, src->buf->data() + src->pos, n);
    src->pos += n;
    return static_cast<int64_t>(n);
}

/* ---- empty event lists ------------------------------------------------------------------- */
static uint32_t in_size(const clap_input_events_t*) { return 0; }
static const clap_event_header_t* in_get(const clap_input_events_t*, uint32_t) { return nullptr; }
static bool out_try_push(const clap_output_events_t*, const clap_event_header_t*) { return true; }

int main(int argc, char** argv)
{
    const char* bundle = nullptr;
    const char* restorePath = nullptr;
    const char* savePath = nullptr;
    const char* resourcesDir = nullptr;
    int blocks = 8;
    const uint32_t blockSize = 256;
    const double sampleRate = 48000.0;

    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--restore") && i + 1 < argc) restorePath = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) savePath = argv[++i];
        else if (!strcmp(argv[i], "--resources") && i + 1 < argc) resourcesDir = argv[++i];
        else if (!strcmp(argv[i], "--process") && i + 1 < argc) blocks = atoi(argv[++i]);
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
        else bundle = argv[i];
    }
    if (!bundle)
    {
        fprintf(stderr, "usage: se_gmpi_clap_state_probe <plugin.clap> [--restore preset.xml] [--save out.xml] [--process N] [--resources dir]\n");
        return 2;
    }
    printf("se_gmpi_clap_state_probe: %s\n", bundle);

    /* On Linux a .clap is the shared object itself. Its Resources sit beside it the way the
     * VST3 bundle lays them out: <dir>/Contents/Resources, with the binary in
     * <dir>/Contents/x86_64-linux/<name>.clap - which is also accepted as the argument. */
    std::string modulePath(bundle);
    std::string dir(bundle);
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    {
        struct stat st;
        if (stat(bundle, &st) == 0 && S_ISDIR(st.st_mode))
        {
            std::string leaf = dir.substr(dir.find_last_of('/') + 1);
            if (const auto dot = leaf.find_last_of('.'); dot != std::string::npos) leaf = leaf.substr(0, dot);
            modulePath = dir + "/Contents/x86_64-linux/" + leaf + ".clap";
        }
        else
        {
            /* <dir>/Contents/x86_64-linux/<name>.clap -> <dir> */
            const auto a = dir.rfind("/Contents/");
            if (a != std::string::npos) dir = dir.substr(0, a);
        }
    }
    const std::string resources = resourcesDir ? resourcesDir : dir + "/Contents/Resources";
    printf("      module -> %s\n      resources -> %s\n", modulePath.c_str(), resources.c_str());

    void* lib = dlopen(modulePath.c_str(), RTLD_LOCAL | RTLD_NOW);
    check("the plug-in binary loads", lib != nullptr);
    if (!lib) { fprintf(stderr, "  dlerror: %s\n", dlerror()); return 1; }

    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(lib, "clap_entry"));
    check("clap_entry is exported", entry != nullptr);
    if (!entry) return 1;
    check("clap_entry->init succeeds", entry->init(modulePath.c_str()));

    auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    check("the plugin factory is offered", factory != nullptr);
    if (!factory) return 1;
    const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, 0);
    check("descriptor 0 exists", desc != nullptr);
    if (!desc) return 1;
    printf("      plugin: %s (%s)\n", desc->name, desc->id);

    const clap_plugin_t* plug = factory->create_plugin(factory, &host, desc->id);
    check("create_plugin returns an instance", plug != nullptr);
    if (!plug) return 1;
    check("plugin->init succeeds", plug->init(plug));

    auto* state = static_cast<const clap_plugin_state_t*>(plug->get_extension(plug, CLAP_EXT_STATE));
    check("the plugin offers clap.state", state != nullptr);
    if (!state) return 1;

    std::string restoreXml;
    if (restorePath)
    {
        check("the restore preset reads", slurp(restorePath, restoreXml) && !restoreXml.empty());
        Source src{ &restoreXml };
        clap_istream_t is{ &src, source_read };
        check("state->load accepts the preset", state->load(plug, &is));
        printf("      restore chunk: %zu bytes from %s\n", restoreXml.size(), restorePath);
    }
    else
        printf("      no restore: the plug-in saves its bundled defaults\n");

    FrequencyEstimate freq;
    if (blocks > 0)
    {
        check("activate succeeds", plug->activate(plug, sampleRate, 1, blockSize));
        check("start_processing succeeds", plug->start_processing(plug));
        if (restartRequested)
        {
            restartRequested = 0;
            plug->stop_processing(plug);
            plug->deactivate(plug);
            check("re-activate after request_restart", plug->activate(plug, sampleRate, 1, blockSize));
            check("re-start_processing after request_restart", plug->start_processing(plug));
        }

        uint32_t channels = 2;
        if (auto* ports = static_cast<const clap_plugin_audio_ports_t*>(plug->get_extension(plug, CLAP_EXT_AUDIO_PORTS)); ports)
        {
            clap_audio_port_info_t info{};
            if (ports->count(plug, false) > 0 && ports->get(plug, 0, false, &info))
                channels = info.channel_count;
        }
        std::vector<std::vector<float>> store(channels, std::vector<float>(blockSize, 0.0f));
        std::vector<float*> chans(channels);
        for (uint32_t c = 0; c < channels; ++c) chans[c] = store[c].data();
        clap_audio_buffer_t outBus{};
        outBus.data32 = chans.data();
        outBus.channel_count = channels;

        clap_input_events_t inEv{ nullptr, in_size, in_get };
        clap_output_events_t outEv{ nullptr, out_try_push };

        double peak = 0.0;
        int processFailures = 0;
        int64_t frames = 0;
        for (int b = 0; b < blocks; ++b)
        {
            for (auto& ch : store) memset(ch.data(), 0, ch.size() * sizeof(float));
            clap_process_t p{};
            p.steady_time = frames;
            p.frames_count = blockSize;
            p.audio_outputs = &outBus;
            p.audio_outputs_count = 1;
            p.in_events = &inEv;
            p.out_events = &outEv;
            if (plug->process(plug, &p) == CLAP_PROCESS_ERROR) ++processFailures;
            for (uint32_t c = 0; c < channels; ++c)
                for (uint32_t i = 0; i < blockSize; ++i) { const double a = fabs((double)chans[c][i]); if (a > peak) peak = a; }
            if (b >= blocks / 2) freq.feed(chans[0], static_cast<int>(blockSize));
            frames += blockSize;
        }
        check("no block returned CLAP_PROCESS_ERROR", processFailures == 0);
        printf("      %d blocks: audio out channels %u, peak %.4f (%.1f dBFS)\n", blocks, channels, peak, peak > 0 ? 20.0 * log10(peak) : -1000.0);
        freq.print(sampleRate);

        plug->stop_processing(plug);
        plug->deactivate(plug);
    }

    Sink sink;
    clap_ostream_t os{ &sink, sink_write };
    check("state->save succeeds", state->save(plug, &os));
    std::string chunk = sink.bytes;
    if (const auto nul = chunk.find('\0'); nul != std::string::npos) chunk.resize(nul); /* the CLAP wrapper writes a terminating NUL */
    if (savePath)
    {
        if (FILE* f = fopen(savePath, "wb")) { fwrite(chunk.data(), 1, chunk.size(), f); fclose(f); printf("      saved chunk to %s\n", savePath); }
        else check("the --save path is writable", false);
    }

    report(chunk, resources, restoreXml);

    plug->destroy(plug);
    entry->deinit();
    dlclose(lib);

    printf("\n%s\n", failures ? "SOME PROBE CHECKS FAILED" : "all probe checks passed");
    return failures ? 1 : 0;
}
