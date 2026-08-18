#include "CommandDispatcher.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../StandaloneHost.h"
#include "PngWriter.h"
#include "TextUtil.h"
#include "WavWriter.h"

#include "GmpiMidi.h"

namespace gmpi
{
namespace standalone
{
namespace mcp
{
namespace
{

// ---------------------------------------------------------------------------
// Small shared helpers
// ---------------------------------------------------------------------------

const char* datatypeName(gmpi::PinDatatype dt)
{
    switch (dt)
    {
    case gmpi::PinDatatype::Enum:    return "enum";
    case gmpi::PinDatatype::Bool:    return "bool";
    case gmpi::PinDatatype::Int32:   return "int32";
    case gmpi::PinDatatype::Int64:   return "int64";
    case gmpi::PinDatatype::Float32: return "float32";
    case gmpi::PinDatatype::Float64: return "float64";
    case gmpi::PinDatatype::String:  return "string";
    case gmpi::PinDatatype::Blob:    return "blob";
    case gmpi::PinDatatype::Midi:    return "midi";
    case gmpi::PinDatatype::Audio:   return "audio";
    default:                         return "unknown";
    }
}

/// One parameter as a JSON object. Reports BOTH the real and the normalised
/// value: a caller that wants "half way up" needs the latter, and one checking
/// a plugin honoured a specific Hz needs the former. Deriving one from the
/// other requires min/max, which are right here, so shipping both costs
/// nothing and removes a whole class of off-by-a-range mistake.
std::string parameterJson(gmpi::hosting::GmpiParameter& param)
{
    JsonObject obj;
    obj.num("id", param.info->id)
       .str("name", param.info->name)
       .str("datatype", datatypeName(param.info->datatype))
       .num("minimum", param.info->minimum)
       .num("maximum", param.info->maximum);

    if (gmpi::hosting::is_scalar(param.info->datatype))
    {
        obj.num("value", param.valueReal())
           .num("normalised", param.normalisedValue())
           // The plugin author's sensible state, which "whatever it happens to
           // be right now" is not: anything wanting to put the plugin back
           // into a working configuration - a test between cases, a caller
           // undoing an experiment - needs a known-good value to return to,
           // and the current one may be the degenerate result of the last
           // thing that ran.
           .num("default", atof(param.info->default_value_s.c_str()));
    }
    else
    {
        // A blob or string has no meaningful number, and emitting 0.0 would
        // read as "this parameter is at zero" rather than "ask another way".
        obj.raw("value", "null").raw("normalised", "null");
    }

    if (!param.info->enum_list.empty())
        obj.str("enumList", param.info->enum_list);

    if (param.info->is_private)
        obj.boolean("private", true);

    return obj.done();
}

/// Turns raw MIDI 1.0 bytes into the app exactly where a MIDI cable does.
///
/// onMidiIn is the MidiCallback the ALSA driver calls, so injected bytes go
/// through the same lock-free FIFO, the same MIDI 1.0 -> UMP conversion and
/// the same block-boundary timing as a real keyboard. Nothing here knows it
/// was synthetic, which is the whole point of testing this way.
std::string sendMidi(AppContext& context, std::string_view cmd, const std::vector<uint8_t>& bytes)
{
    if (!context.host)
        return errorLine(cmd, "no host");

    if (!context.host->wantsMidiInput())
        return errorLine(cmd, "this plugin has no MIDI input pin");

    // Refused up here rather than reported as sent. Sharing the cable's route
    // into the app means sharing its limit: MidiFifo::push DROPS a message
    // longer than kMaxMessage, silently, because the alternative on a MIDI
    // thread is worse (StandaloneHost.h says why it is 255 and what raising it
    // would take). onMidiIn returns void either way, so a harness injecting a
    // 300-byte sysex would otherwise get {"ok":true} for bytes that reached
    // nothing - the one answer a test channel must never give.
    //
    // It is not the only way a push can vanish: the FIFO also drops when it is
    // FULL, which takes audio stopped and thousands of bytes queued behind it,
    // and cannot be tested from this side because the read position belongs to a
    // realtime consumer. What is answerable here is answered here.
    if (bytes.size() > static_cast<size_t>(MidiFifo::kMaxMessage))
    {
        return errorLine(cmd, "message is " + std::to_string(bytes.size())
                            + " bytes; the MIDI queue carries at most "
                            + std::to_string(MidiFifo::kMaxMessage));
    }

    context.host->onMidiIn(bytes.data(), static_cast<int>(bytes.size()));

    std::string hex;
    for (const uint8_t b : bytes)
    {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02x", b);
        if (!hex.empty())
            hex += ' ';
        hex += buf;
    }

    return JsonObject().str("cmd", cmd).boolean("ok", true).str("bytes", hex).done();
}

// ---------------------------------------------------------------------------
// Verbs
// ---------------------------------------------------------------------------

std::string cmdPing(const std::vector<std::string>& args)
{
    // The framing sentinel the client writes after every batch; its echo marks
    // end-of-batch. Counting result lines instead would desync on any verb that
    // emits an unexpected number of them.
    return JsonObject()
        .str("cmd", "ping")
        .boolean("ok", true)
        .str("token", args.size() > 1 ? args[1] : std::string{})
        .done();
}

std::string cmdInfo(AppContext& context)
{
    auto* host = context.host;
    if (!host)
        return errorLine("info", "no host");

    const auto* info = host->pluginInfo();

    JsonObject obj;
    obj.str("cmd", "info").boolean("ok", true);

    if (info)
    {
        obj.str("name", info->name)
           .str("id", info->id)
           .str("vendor", info->vendorName)
           .num("parameterCount", static_cast<double>(info->parameters.size()));

        // The plugin's declared version, read by the SDK's parser - the same
        // string the VST3 factory reports to a DAW. gmpi_plugin.cmake stamps
        // this executable's VERSIONINFO resource from that same attribute but
        // by its own text search, so this is the value to trust where the two
        // disagree (gmpi::hosting::pluginInfo::version says when they can).
        //
        // Guarded because the field is in the SDK repo, which can be older than
        // this one; an older SDK simply omits the field rather than failing to
        // build.
#ifdef GMPI_HOSTING_PLUGININFO_HAS_VERSION
        obj.str("version", info->version);
#endif
    }

    // "audioRunning" is asked of the DRIVER, not of whether startAudio()
    // succeeded once (StandaloneHost::isAudioRunning): a device removed
    // mid-session leaves a stream that has stopped, and this used to go on
    // reporting true at it for the rest of the session - a harness waiting for
    // audio to fail would have waited forever.
    obj.num("audioInputs", host->audioInputCount())
       .num("audioOutputs", host->audioOutputCount())
       .boolean("midiInput", host->wantsMidiInput())
       .boolean("audioRunning", host->isAudioRunning());

    // Two errors, never merged into one field. "lastError" is audio's - it is
    // the one that decides whether this app makes a sound - and "midiError" is
    // there only when MIDI inputs were asked for and not one of them could be
    // connected (MidiOpenTally::success is the rule). An app nobody has
    // configured, on a machine with no MIDI hardware, is not one of those cases
    // and emits no "midiError" at all - so a harness can read its absence as
    // "nothing to fix" rather than as "there is no keyboard". The settings pane
    // shows the two on separate lines for the same reason.
    if (!host->lastError().empty())
        obj.str("lastError", host->lastError());

    if (!host->midiError().empty())
        obj.str("midiError", host->midiError());

    // A third field rather than a third error, because this one appears NEXT TO
    // "audioRunning": true. It is the degraded-open channel
    // (AudioMidiDevices.h::lastWarning) - audio is playing and something the
    // plugin has pins for is not working, which a harness checking only
    // lastError would read as full health, exactly as the settings page used
    // to.
    if (!host->audioWarning().empty())
        obj.str("audioWarning", host->audioWarning());

    // A fourth, and the only one that appears next to "audioRunning": false
    // WITHOUT a "lastError" beside it: the stream opened, ran, and then stopped
    // on its own (AudioMidiDevices.h::stoppedReason). Distinct from "lastError"
    // - which means no stream was ever started - because those are two different
    // things for a harness to do about it: one is a device that would not open,
    // the other a device that has gone away since.
    if (const auto stopped = host->audioStoppedReason(); !stopped.empty())
        obj.str("audioStopped", stopped);

    if (auto* driver = host->audioDriver(); driver && host->isAudioRunning())
    {
        obj.num("sampleRate", driver->getSampleRate())
           .num("bufferFrames", driver->getBufferFrames());
    }

    // Both spaces, never conflated: pointer verbs speak DIPs, screenshots are
    // measured in pixels, and under fractional scaling those differ.
    //
    // BOTH ARE ASKED OF THE WINDOW, and neither costs a paint. That is what lets
    // --info be the FIRST thing a client does: a caller has to know the scale
    // before it can place a click, and making it take a screenshot to find out
    // would mean painting the editor to answer a question about its frame - and
    // on macOS a screenshot is the only thing that ever fills the capture
    // bitmap, so deriving these from it left --info with nothing to say at all
    // until one had been taken.
    float logicalW = 0.0f, logicalH = 0.0f;
    if (context.logicalSize)
        context.logicalSize(logicalW, logicalH);

    int canvasW = 0, canvasH = 0;
    if (context.canvasSize)
        context.canvasSize(canvasW, canvasH);

    obj.num("windowWidth", logicalW)
       .num("windowHeight", logicalH)
       .num("canvasWidth", canvasW)
       .num("canvasHeight", canvasH)
       .num("editorOriginY", context.editorOriginY);

    // Reported rather than left to the caller's division, because a caller
    // dividing zero by zero gets NaN rather than an answer it can test.
    if (logicalW > 0.0f && canvasW > 0)
        obj.num("scale", static_cast<double>(canvasW) / logicalW);

    return obj.done();
}

std::string cmdListParams(AppContext& context)
{
    if (!context.host)
        return errorLine("list-params", "no host");

    auto& params = context.host->controller().patchManager.parameters;

    // Sorted by id: the map's own order is a hash order, so an unsorted dump
    // would shuffle between runs and make diffing two of them useless.
    std::vector<int> ids;
    ids.reserve(params.size());
    for (auto& entry : params)
        ids.push_back(entry.first);
    std::sort(ids.begin(), ids.end());

    std::string array = "[";
    for (const int id : ids)
    {
        if (array.size() > 1)
            array += ',';
        array += parameterJson(params[id]);
    }
    array += ']';

    return JsonObject()
        .str("cmd", "list-params")
        .boolean("ok", true)
        .num("count", static_cast<double>(ids.size()))
        .raw("parameters", array)
        .done();
}

std::string cmdGetParam(AppContext& context, const std::vector<std::string>& args)
{
    if (!context.host)
        return errorLine("get-param", "no host");
    if (args.size() < 2)
        return errorLine("get-param", "usage: --get-param <id>");

    int id = 0;
    if (!parseInt(args[1], id))
        return errorLine("get-param", "'" + args[1] + "' is not a parameter id");

    auto* param = context.host->controller().patchManager.getParameter(id);
    if (!param)
        return errorLine("get-param", "no parameter with id " + args[1]);

    return JsonObject()
        .str("cmd", "get-param")
        .boolean("ok", true)
        .raw("parameter", parameterJson(*param))
        .done();
}

std::string cmdSetParam(AppContext& context, const std::vector<std::string>& args)
{
    if (!context.host)
        return errorLine("set-param", "no host");
    if (args.size() < 3)
        return errorLine("set-param", "usage: --set-param <id> <value> [--normalised]");

    int id = 0;
    if (!parseInt(args[1], id))
        return errorLine("set-param", "'" + args[1] + "' is not a parameter id");

    double value = 0.0;
    if (!parseDouble(args[2], value))
        return errorLine("set-param", "'" + args[2] + "' is not a number");

    const bool normalised =
        std::find(args.begin() + 3, args.end(), "--normalised") != args.end() ||
        std::find(args.begin() + 3, args.end(), "--normalized") != args.end();

    auto& controller = context.host->controller();

    auto* existing = controller.patchManager.getParameter(id);
    if (!existing)
        return errorLine("set-param", "no parameter with id " + std::to_string(id));

    if (!gmpi::hosting::is_scalar(existing->info->datatype))
        return errorLine("set-param", "parameter " + std::to_string(id) + " is not numeric");

    auto* changed = normalised
        ? controller.patchManager.setParameterNormalised(id, value)
        : controller.patchManager.setParameterReal(id, value);

    // Null means the store short-circuited an unchanged write. Reporting that
    // honestly (rather than a bare ok) is what tells a caller its value was
    // already in place versus silently rejected - same contract SynthEdit's
    // --rename reports with `changed:false`.
    if (changed)
    {
        // BOTH halves, or the result is a lie in one direction: notifyGui moves
        // the knob the user is looking at, notifyDaw queues the value for the
        // processor so the sound actually changes. Doing only the first gives a
        // GUI that disagrees with what is being heard.
        controller.notifyGui(changed);
        controller.notifyDaw(changed);
    }

    return JsonObject()
        .str("cmd", "set-param")
        .boolean("ok", true)
        .boolean("changed", changed != nullptr)
        .raw("parameter", parameterJson(*existing))
        .done();
}

std::string cmdScreenshot(AppContext& context, const std::vector<std::string>& args)
{
    if (args.size() < 2)
        return errorLine("screenshot", "usage: --screenshot <path.png>");
    if (!context.framePixels)
        return errorLine("screenshot", "this build cannot read its own window");

    const uint8_t* pixels = nullptr;
    int w = 0, h = 0, stride = 0;

    // forceRedraw: a screenshot taken right after --set-param must show the new
    // value. Without it the buffer still holds the frame drawn before the
    // command arrived, and the picture silently contradicts the parameter dump
    // sitting next to it in the transcript.
    if (!context.framePixels(true, pixels, w, h, stride))
        return errorLine("screenshot", "no frame has been drawn yet");

    std::string error;
    if (!writePng(args[1], pixels, w, h, stride, error))
        return errorLine("screenshot", error);

    return JsonObject()
        .str("cmd", "screenshot")
        .boolean("ok", true)
        .str("path", args[1])
        .num("width", w)
        .num("height", h)
        .done();
}

// --- pointer input ---------------------------------------------------------

constexpr int32_t kContactFlags =
      static_cast<int32_t>(gmpi::api::PointerFlags::InContact)
    | static_cast<int32_t>(gmpi::api::PointerFlags::Primary)
    | static_cast<int32_t>(gmpi::api::PointerFlags::Confidence)
    | static_cast<int32_t>(gmpi::api::PointerFlags::FirstButton);

constexpr int32_t kHoverFlags =
      static_cast<int32_t>(gmpi::api::PointerFlags::Primary)
    | static_cast<int32_t>(gmpi::api::PointerFlags::Confidence);

enum class PointerAction { Down, Move, Up, Hover };

std::string cmdPointer(AppContext& context, PointerAction action,
                       std::string_view cmd, const std::vector<std::string>& args)
{
    if (args.size() < 2)
        return errorLine(cmd, std::string("usage: ") + std::string(cmd) + " <x,y>");
    if (!context.inputClient)
        return errorLine(cmd, "this build has no input path");

    float x = 0.0f, y = 0.0f;
    if (!parsePoint(args[1], x, y))
        return errorLine(cmd, "'" + args[1] + "' is not an x,y coordinate");

    auto* client = context.inputClient();
    if (!client)
        return errorLine(cmd, "no window is attached");

    const gmpi::drawing::Point point{ x, y };
    const int32_t flags = (action == PointerAction::Hover) ? kHoverFlags : kContactFlags;

    switch (action)
    {
    case PointerAction::Down:  client->onPointerDown(point, flags); break;
    case PointerAction::Up:    client->onPointerUp  (point, flags); break;
    case PointerAction::Move:
    case PointerAction::Hover: client->onPointerMove(point, flags); break;
    }

    return JsonObject().str("cmd", cmd).boolean("ok", true)
        .num("x", x).num("y", y).done();
}

std::string cmdDrag(AppContext& context, const std::vector<std::string>& args)
{
    if (args.size() < 3)
        return errorLine("drag", "usage: --drag <x1,y1> <x2,y2> [--steps N]");
    if (!context.inputClient)
        return errorLine("drag", "this build has no input path");

    float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
    if (!parsePoint(args[1], x1, y1))
        return errorLine("drag", "'" + args[1] + "' is not an x,y coordinate");
    if (!parsePoint(args[2], x2, y2))
        return errorLine("drag", "'" + args[2] + "' is not an x,y coordinate");

    int steps = 10;
    for (size_t i = 3; i + 1 < args.size(); ++i)
    {
        if (args[i] == "--steps" && !parseInt(args[i + 1], steps))
            return errorLine("drag", "'" + args[i + 1] + "' is not a step count");
    }
    steps = std::clamp(steps, 1, 1000);

    auto* client = context.inputClient();
    if (!client)
        return errorLine("drag", "no window is attached");

    // The whole gesture runs inside ONE dispatched job, so it costs one visit
    // to the main thread rather than one per step. A drag issued as separate
    // commands would take steps * tick milliseconds and, worse, let the app
    // repaint mid-gesture in a way no real mouse ever produces.
    client->onPointerDown({ x1, y1 }, kContactFlags);

    for (int i = 1; i <= steps; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        client->onPointerMove({ x1 + (x2 - x1) * t, y1 + (y2 - y1) * t }, kContactFlags);
    }

    client->onPointerUp({ x2, y2 }, kContactFlags);

    return JsonObject().str("cmd", "drag").boolean("ok", true)
        .num("fromX", x1).num("fromY", y1)
        .num("toX", x2).num("toY", y2)
        .num("steps", steps)
        .done();
}

// --- MIDI ------------------------------------------------------------------

/// Channel is 1-16 as a musician counts them, not 0-15 as the wire encodes
/// them. Everything a user reads off a keyboard or a DAW is 1-based, and a
/// tool that silently disagreed by one would produce bugs that look like the
/// plugin's fault.
bool parseChannel(const std::string& s, uint8_t& out)
{
    int channel = 0;
    if (!parseInt(s, channel) || channel < 1 || channel > 16)
        return false;

    out = static_cast<uint8_t>(channel - 1);
    return true;
}

bool parseByte(const std::string& s, int lo, int hi, uint8_t& out)
{
    int v = 0;
    if (!parseInt(s, v) || v < lo || v > hi)
        return false;

    out = static_cast<uint8_t>(v);
    return true;
}

std::string cmdNoteOn(AppContext& context, const std::vector<std::string>& args)
{
    if (args.size() < 3)
        return errorLine("note-on", "usage: --note-on <channel 1-16> <note 0-127> [velocity 1-127]");

    uint8_t channel = 0, note = 0, velocity = 100;
    if (!parseChannel(args[1], channel))
        return errorLine("note-on", "channel must be 1-16");
    if (!parseByte(args[2], 0, 127, note))
        return errorLine("note-on", "note must be 0-127");
    if (args.size() > 3 && !parseByte(args[3], 1, 127, velocity))
        return errorLine("note-on", "velocity must be 1-127 (0 would be a note-off)");

    return sendMidi(context, "note-on",
                    { static_cast<uint8_t>(0x90 | channel), note, velocity });
}

std::string cmdNoteOff(AppContext& context, const std::vector<std::string>& args)
{
    if (args.size() < 3)
        return errorLine("note-off", "usage: --note-off <channel 1-16> <note 0-127> [velocity 0-127]");

    uint8_t channel = 0, note = 0, velocity = 0;
    if (!parseChannel(args[1], channel))
        return errorLine("note-off", "channel must be 1-16");
    if (!parseByte(args[2], 0, 127, note))
        return errorLine("note-off", "note must be 0-127");
    if (args.size() > 3 && !parseByte(args[3], 0, 127, velocity))
        return errorLine("note-off", "velocity must be 0-127");

    return sendMidi(context, "note-off",
                    { static_cast<uint8_t>(0x80 | channel), note, velocity });
}

std::string cmdControlChange(AppContext& context, const std::vector<std::string>& args)
{
    if (args.size() < 4)
        return errorLine("cc", "usage: --cc <channel 1-16> <controller 0-127> <value 0-127>");

    uint8_t channel = 0, controller = 0, value = 0;
    if (!parseChannel(args[1], channel))
        return errorLine("cc", "channel must be 1-16");
    if (!parseByte(args[2], 0, 127, controller))
        return errorLine("cc", "controller must be 0-127");
    if (!parseByte(args[3], 0, 127, value))
        return errorLine("cc", "value must be 0-127");

    return sendMidi(context, "cc",
                    { static_cast<uint8_t>(0xB0 | channel), controller, value });
}

std::string cmdAllNotesOff(AppContext& context)
{
    if (!context.host)
        return errorLine("all-notes-off", "no host");
    if (!context.host->wantsMidiInput())
        return errorLine("all-notes-off", "this plugin has no MIDI input pin");

    // Every channel, because a stuck note is exactly the situation where you do
    // not know which channel it came in on. CC 123 rather than 128 individual
    // note-offs: a plugin that ignores the former is worth knowing about, and
    // the caller can fall back to explicit note-offs.
    for (uint8_t channel = 0; channel < 16; ++channel)
    {
        const uint8_t bytes[3] = { static_cast<uint8_t>(0xB0 | channel), 123, 0 };
        context.host->onMidiIn(bytes, 3);
    }

    return JsonObject().str("cmd", "all-notes-off").boolean("ok", true).done();
}

std::string cmdMidiRaw(AppContext& context, const std::vector<std::string>& args)
{
    if (args.size() < 2)
        return errorLine("midi", "usage: --midi <hex bytes, e.g. 90 3c 64>");

    std::vector<uint8_t> bytes;
    for (size_t i = 1; i < args.size(); ++i)
    {
        char* end = nullptr;
        const long v = std::strtol(args[i].c_str(), &end, 16);
        if (end != args[i].c_str() + args[i].size() || v < 0 || v > 255)
            return errorLine("midi", "'" + args[i] + "' is not a hex byte");
        bytes.push_back(static_cast<uint8_t>(v));
    }

    if (bytes.empty())
        return errorLine("midi", "no bytes given");

    return sendMidi(context, "midi", bytes);
}

// --- offline render --------------------------------------------------------

/// Renders the plugin to a WAV faster than realtime, on a processor of its own.
///
/// A SEPARATE processor instance, deliberately - not the one the audio device
/// is driving. Two reasons, and the second is the one that matters:
///
///   * the live processor is owned by the audio thread while a device is open,
///     so driving it from here would be a data race on the plugin's own state;
///   * a render must not perturb what the user is listening to. Reusing the
///     live processor would mean stopping audio, and a render that silences
///     the app for its duration cannot be used to check something *while* it
///     plays.
///
/// The cost is that the render starts from the plugin's initial state plus the
/// current parameter values, rather than from whatever the live voice happens
/// to be doing mid-note. That is the right trade for a verification tool:
/// deterministic and repeatable beats a snapshot nobody can reproduce.
std::string cmdRenderAudio(AppContext& context, const std::vector<std::string>& args)
{
    if (!context.host)
        return errorLine("render-audio", "no host");
    if (args.size() < 2)
        return errorLine("render-audio", "usage: --render-audio <path.wav> [--seconds S] [--rate R] [--note N] [--velocity V] [--hold S] [--format int16|float32]");

    const auto* info = context.host->pluginInfo();
    if (!info)
        return errorLine("render-audio", "no plugin");

    const std::string path = args[1];

    double seconds = 2.0;
    double hold    = -1.0;      // negative: derive from `seconds` once it is known
    int sampleRate = 48000;
    int blockSize  = 512;
    int note       = -1;        // negative: render without playing anything
    int velocity   = 100;
    int channel    = 1;
    WavFormat format = WavFormat::Int16;

    // What to feed the plugin's audio INPUTS. Silence by default, which is
    // right for an instrument and useless for an effect: gain times silence is
    // silence, so a render of a filter or a compressor came back
    // silent:true and told you nothing at all.
    enum class InputSignal { Silence, Tone, Noise } inputSignal = InputSignal::Silence;
    double inputFreq  = 440.0;
    double inputLevel = 0.5;    // amplitude, not dB: -6 dBFS, enough headroom
                                // that a plugin with gain above 1 still shows
                                // its shape before it clips.

    for (size_t i = 2; i < args.size(); ++i)
    {
        const std::string& flag = args[i];
        const bool hasValue = (i + 1 < args.size());
        const std::string value = hasValue ? args[i + 1] : std::string{};

        auto needNumber = [&](auto& target, auto parse) -> bool
        {
            if (!hasValue || !parse(value, target))
                return false;
            ++i;
            return true;
        };

        if (flag == "--seconds")
        {
            if (!needNumber(seconds, [](const std::string& s, double& d) { return parseDouble(s, d); }))
                return errorLine("render-audio", "--seconds needs a number");
        }
        else if (flag == "--hold")
        {
            if (!needNumber(hold, [](const std::string& s, double& d) { return parseDouble(s, d); }))
                return errorLine("render-audio", "--hold needs a number");
        }
        else if (flag == "--rate")
        {
            if (!needNumber(sampleRate, [](const std::string& s, int& v) { return parseInt(s, v); }))
                return errorLine("render-audio", "--rate needs a number");
        }
        else if (flag == "--block")
        {
            if (!needNumber(blockSize, [](const std::string& s, int& v) { return parseInt(s, v); }))
                return errorLine("render-audio", "--block needs a number");
        }
        else if (flag == "--note")
        {
            if (!needNumber(note, [](const std::string& s, int& v) { return parseInt(s, v); }))
                return errorLine("render-audio", "--note needs a number");
        }
        else if (flag == "--velocity")
        {
            if (!needNumber(velocity, [](const std::string& s, int& v) { return parseInt(s, v); }))
                return errorLine("render-audio", "--velocity needs a number");
        }
        else if (flag == "--channel")
        {
            if (!needNumber(channel, [](const std::string& s, int& v) { return parseInt(s, v); }))
                return errorLine("render-audio", "--channel needs a number");
        }
        else if (flag == "--input-tone")
        {
            inputSignal = InputSignal::Tone;
            if (!needNumber(inputFreq, [](const std::string& s, double& d) { return parseDouble(s, d); }))
                return errorLine("render-audio", "--input-tone needs a frequency in Hz");
        }
        else if (flag == "--input-noise")
        {
            inputSignal = InputSignal::Noise;
        }
        else if (flag == "--input-level")
        {
            if (!needNumber(inputLevel, [](const std::string& s, double& d) { return parseDouble(s, d); }))
                return errorLine("render-audio", "--input-level needs a number");
        }
        else if (flag == "--format")
        {
            if (!hasValue)
                return errorLine("render-audio", "--format needs int16 or float32");
            if (value == "float32")      format = WavFormat::Float32;
            else if (value == "int16")   format = WavFormat::Int16;
            else return errorLine("render-audio", "--format must be int16 or float32");
            ++i;
        }
        else
        {
            return errorLine("render-audio", "unknown option '" + flag + "'");
        }
    }

    // Bounds that keep an honest typo from becoming a hung app or a full disk.
    // A 4-minute ceiling is far past anything a test needs and still finishes.
    if (!(seconds > 0.0) || seconds > 240.0)
        return errorLine("render-audio", "--seconds must be between 0 and 240");
    if (sampleRate < 8000 || sampleRate > 384000)
        return errorLine("render-audio", "--rate must be between 8000 and 384000");
    if (blockSize < 1 || blockSize > 65536)
        return errorLine("render-audio", "--block must be between 1 and 65536");
    if (note >= 0 && (note > 127))
        return errorLine("render-audio", "--note must be 0-127");
    if (velocity < 1 || velocity > 127)
        return errorLine("render-audio", "--velocity must be 1-127");
    if (channel < 1 || channel > 16)
        return errorLine("render-audio", "--channel must be 1-16");

    if (!(inputLevel >= 0.0) || inputLevel > 1.0)
        return errorLine("render-audio", "--input-level must be between 0 and 1");
    if (inputSignal == InputSignal::Tone && !(inputFreq > 0.0 && inputFreq < sampleRate * 0.5))
        return errorLine("render-audio", "--input-tone must be above 0 and below half the sample rate");

    const int outChannels = context.host->audioOutputCount();
    if (outChannels <= 0)
        return errorLine("render-audio", "this plugin has no audio outputs");

    // Refused rather than quietly ignored: a caller who asked for a test tone
    // and got back silent:true would reasonably conclude the plugin ate it,
    // when in fact there was nowhere to put it.
    if (inputSignal != InputSignal::Silence && context.host->audioInputCount() <= 0)
        return errorLine("render-audio", "this plugin has no audio inputs, so there is nothing to feed a test signal into");

    if (hold < 0.0)
        hold = seconds * 0.5;   // half on, half releasing: shows the tail too

    // Half-open note handling: a note that never ends is a legitimate request
    // (a drone, a held pad), so a hold at or past the end simply omits the
    // note-off rather than being rejected.
    const int64_t totalFrames = static_cast<int64_t>(seconds * sampleRate);
    const int64_t noteOnFrame  = 0;
    const int64_t noteOffFrame = static_cast<int64_t>(hold * sampleRate);

    // --- build an independent processor -----------------------------------
    gmpi::hosting::gmpi_processor processor;
    processor.init(*info);

    if (!processor.start_processor(&processor, *info, blockSize, static_cast<float>(sampleRate)))
        return errorLine("render-audio", "the plugin's audio processor failed to start");

    // --- buffers -----------------------------------------------------------
    std::vector<std::vector<float>> outputs(static_cast<size_t>(outChannels),
                                            std::vector<float>(static_cast<size_t>(blockSize), 0.0f));
    std::vector<std::vector<float>> captured(static_cast<size_t>(outChannels));
    for (auto& channelData : captured)
        channelData.reserve(static_cast<size_t>(totalFrames));

    const int inChannels = context.host->audioInputCount();
    std::vector<std::vector<float>> inputs(static_cast<size_t>(std::max(0, inChannels)),
                                           std::vector<float>(static_cast<size_t>(blockSize), 0.0f));

    // Fills the input pins for one block. Silence leaves the buffers alone -
    // they start zeroed and nothing writes to them.
    //
    // Both generators are DETERMINISTIC, which is the whole value of rendering
    // offline: the noise uses a fixed-seed LCG rather than rand(), so two runs
    // of the same command produce byte-identical files and a regression is a
    // real change rather than a different roll. Phase is carried across blocks
    // so the tone has no discontinuity at block boundaries - a click there
    // would show up as broadband content and quietly ruin any spectral check.
    double tonePhase = 0.0;
    uint32_t noiseState = 0x12345678u;
    const double phaseStep = 2.0 * 3.14159265358979323846 * inputFreq / sampleRate;

    auto fillInputs = [&](int frames)
    {
        if (inputSignal == InputSignal::Silence || inputs.empty())
            return;

        for (int i = 0; i < frames; ++i)
        {
            float sample = 0.0f;

            if (inputSignal == InputSignal::Tone)
            {
                sample = static_cast<float>(std::sin(tonePhase) * inputLevel);
                tonePhase += phaseStep;
                if (tonePhase > 2.0 * 3.14159265358979323846)
                    tonePhase -= 2.0 * 3.14159265358979323846;
            }
            else
            {
                noiseState = noiseState * 1664525u + 1013904223u;   // Numerical Recipes LCG
                const double unit = static_cast<double>(noiseState >> 8) / 16777216.0;   // [0,1)
                sample = static_cast<float>((unit * 2.0 - 1.0) * inputLevel);
            }

            // Same signal to every input channel. A stereo effect fed a
            // correlated pair is the ordinary case for a level test; anything
            // needing decorrelated channels wants a WAV input, which this is
            // not pretending to be.
            for (auto& channelData : inputs)
                channelData[static_cast<size_t>(i)] = sample;
        }
    };

    // MIDI 1.0 bytes -> UMP events on the processor's MIDI pin, the same
    // conversion the live path uses. The sample offset is within the block.
    int currentBlockOffset = 0;
    gmpi::midi::MidiConverter2 converter(
        [&processor, &currentBlockOffset](const gmpi::midi2::message_view msg, int /*offset*/)
        {
            if (processor.MidiInputPinIdx < 0 || msg.size() > 8)
                return;

            gmpi::api::Event event
            {
                {},
                currentBlockOffset,
                gmpi::api::EventType::Midi,
                processor.MidiInputPinIdx,
                static_cast<int32_t>(msg.size()),
                {}
            };
            std::copy(msg.begin(), msg.begin() + msg.size(), reinterpret_cast<uint8_t*>(&event.data_));
            processor.events.push(event);
        });

    const bool wantsNote = (note >= 0) && (processor.MidiInputPinIdx >= 0);

    // Point the plugin's pins at our buffers. Same pin-order rule as
    // StandaloneHost::processAudio, so a plugin cannot tell the two apart.
    // Done once: the buffers never move, and setBuffer persists across
    // process() calls.
    {
        int inIdx = 0, outIdx = 0;
        for (const auto& pin : info->dspPins)
        {
            if (pin.datatype != gmpi::PinDatatype::Audio)
                continue;

            if (pin.direction == gmpi::PinDirection::In)
            {
                if (static_cast<size_t>(inIdx) < inputs.size())
                    processor.processor->setBuffer(pin.id, inputs[inIdx].data());
                ++inIdx;
            }
            else
            {
                if (static_cast<size_t>(outIdx) < outputs.size())
                    processor.processor->setBuffer(pin.id, outputs[outIdx].data());
                ++outIdx;
            }
        }
    }

    // Runs one block and throws it away. Used to settle the plugin before the
    // recorded part begins.
    auto discardBlock = [&]
    {
        for (auto& channelData : outputs)
            std::fill(channelData.begin(), channelData.end(), 0.0f);

        processor.processor->process(blockSize, processor.events.head());
        processor.events.clear();
    };

    // ORDER HERE IS LOAD-BEARING, and getting it wrong is silent.
    //
    // start_processor has already queued a GraphStart plus a PinSet for EVERY
    // input pin carrying the plugin's DEFAULT value - all stamped timeDelta 0.
    // EventQue::push inserts with lower_bound, so among equal timestamps the
    // most recently pushed lands FIRST. Priming straight after
    // start_processor therefore put our values ahead of those defaults, and
    // the defaults overwrote every one of them.
    //
    // The symptom was a render that ignored the parameters completely while
    // looking perfectly healthy: correct duration, plausible level, a note
    // audibly playing - and the identical peak whether the volume was 1 or 0.
    //
    // So: let the defaults land in a block of their own, THEN prime, THEN let
    // that land too, and only then start the note. Sequencing whole blocks
    // rather than juggling timestamps means this does not depend on the
    // queue's tie-breaking, which is an implementation detail no caller should
    // have to know.
    discardBlock();

    // Prime with what the GUI currently shows, so a render verifies the state
    // the user is actually looking at rather than the plugin's defaults.
    //
    // Counted and reported: "the render ignored my parameter" and "the
    // parameter does not affect the sound" look identical from outside, and
    // they call for very different responses from whoever is looking.
    int primed = 0;
    for (auto& entry : context.host->controller().patchManager.parameters)
    {
        auto& param = entry.second;
        if (!gmpi::hosting::is_scalar(param.info->datatype))
            continue;

        const size_t before = processor.events.size();
        processor.setParameterNormalizedFromDaw(*info, 0, entry.first, param.normalisedValue());
        if (processor.events.size() != before)
            ++primed;
    }

    // Applies the priming, so a voice that latches its parameters at note-on
    // (SawDemo does) starts on the values that were asked for.
    discardBlock();

    // --- render ------------------------------------------------------------
    for (int64_t frame = 0; frame < totalFrames; frame += blockSize)
    {
        const int frames = static_cast<int>(std::min<int64_t>(blockSize, totalFrames - frame));

        if (wantsNote)
        {
            const uint8_t ch = static_cast<uint8_t>(channel - 1);

            if (noteOnFrame >= frame && noteOnFrame < frame + frames)
            {
                currentBlockOffset = static_cast<int>(noteOnFrame - frame);
                const uint8_t bytes[3] = { static_cast<uint8_t>(0x90 | ch),
                                           static_cast<uint8_t>(note),
                                           static_cast<uint8_t>(velocity) };
                converter.processMidi({ bytes, 3 }, currentBlockOffset);
            }
            if (noteOffFrame >= frame && noteOffFrame < frame + frames)
            {
                currentBlockOffset = static_cast<int>(noteOffFrame - frame);
                const uint8_t bytes[3] = { static_cast<uint8_t>(0x80 | ch),
                                           static_cast<uint8_t>(note), 0 };
                converter.processMidi({ bytes, 3 }, currentBlockOffset);
            }
        }

        fillInputs(frames);

        // Stale output is worse than silence here: a plugin that writes nothing
        // this block (a synth with no active voice) would otherwise repeat the
        // previous block forever and render as a loud buzz.
        for (auto& channelData : outputs)
            std::fill(channelData.begin(), channelData.begin() + frames, 0.0f);

        processor.processor->process(frames, processor.events.head());
        processor.events.clear();

        for (size_t ch = 0; ch < captured.size(); ++ch)
            captured[ch].insert(captured[ch].end(), outputs[ch].begin(), outputs[ch].begin() + frames);
    }

    const AudioStats stats = measure(captured);

    std::string error;
    if (!writeWav(path, captured, sampleRate, format, error))
        return errorLine("render-audio", error);

    // peak/rms in the response, so "did it make a sound" is answered without
    // the caller having to open the WAV at all - which is the question almost
    // every render is really asking.
    return JsonObject()
        .str("cmd", "render-audio")
        .boolean("ok", true)
        .str("path", path)
        .num("frames", static_cast<double>(totalFrames))
        .num("channels", outChannels)
        .num("sampleRate", sampleRate)
        .num("seconds", seconds)
        .num("peak", stats.peak)
        .num("rms", stats.rms)
        .num("clippedSamples", static_cast<double>(stats.clippedSamples))
        .num("parametersPrimed", primed)
        .str("input", inputSignal == InputSignal::Tone  ? "tone"
                    : inputSignal == InputSignal::Noise ? "noise"
                                                        : "silence")
        // Echoed so a gain check is arithmetic rather than guesswork: feed
        // 0.5, read the peak back, and the ratio IS the gain the plugin
        // applied.
        .num("inputLevel", inputSignal == InputSignal::Silence ? 0.0 : inputLevel)
        .num("inputChannels", inChannels)
        .boolean("silent", stats.peak < 1e-6)
        .boolean("notePlayed", wantsNote)
        .done();
}

} // namespace

// ---------------------------------------------------------------------------

std::string dispatchCommand(AppContext& context, const std::string& line)
{
    const auto args = tokenize(line);
    if (args.empty())
        return errorLine("", "empty command");

    const std::string& verb = args[0];

    if (verb == "--ping")          return cmdPing(args);
    if (verb == "--info")          return cmdInfo(context);
    if (verb == "--list-params")   return cmdListParams(context);
    if (verb == "--get-param")     return cmdGetParam(context, args);
    if (verb == "--set-param")     return cmdSetParam(context, args);
    if (verb == "--screenshot")    return cmdScreenshot(context, args);

    if (verb == "--pointer-down")  return cmdPointer(context, PointerAction::Down,  "pointer-down", args);
    if (verb == "--pointer-move")  return cmdPointer(context, PointerAction::Move,  "pointer-move", args);
    if (verb == "--pointer-up")    return cmdPointer(context, PointerAction::Up,    "pointer-up",   args);
    if (verb == "--hover")         return cmdPointer(context, PointerAction::Hover, "hover",        args);
    if (verb == "--drag")          return cmdDrag(context, args);

    if (verb == "--note-on")       return cmdNoteOn(context, args);
    if (verb == "--note-off")      return cmdNoteOff(context, args);
    if (verb == "--cc")            return cmdControlChange(context, args);
    if (verb == "--all-notes-off") return cmdAllNotesOff(context);
    if (verb == "--midi")          return cmdMidiRaw(context, args);

    if (verb == "--render-audio")  return cmdRenderAudio(context, args);

    return errorLine("", "unknown command '" + verb + "'");
}

} // namespace mcp
} // namespace standalone
} // namespace gmpi
