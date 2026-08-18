#include "SettingsPane.h"

#include <algorithm>

#include "StandaloneHost.h"
#include "StandaloneSettings.h"
#include "experimental/builders.h"

namespace gmpi
{
namespace standalone
{

namespace
{

// Block sizes in FRAMES, not milliseconds. A standalone's user is choosing
// against a plugin's block size and a graph's quantum, both of which are
// powers of two; offering "20 ms" would round to one of these anyway and hide
// which one.
constexpr int kBufferFrames[] = { 64, 128, 256, 512, 1024, 2048 };

// Combo boxes take their options as one comma-separated string, so a device
// name containing a comma would split into two entries. Nothing here can quote
// or escape, so the comma is replaced - the id is what gets persisted, and it
// is untouched.
std::string joined(const std::vector<std::string>& items)
{
    std::string out;
    for (const auto& s : items)
    {
        if (!out.empty())
            out += ',';

        for (const char c : s)
            out += (c == ',') ? ' ' : c;
    }
    return out;
}

std::string describeBuffer(int frames, int sampleRate)
{
    auto text = std::to_string(frames) + " frames";

    if (sampleRate > 0)
        text += " (" + std::to_string(1000 * frames / sampleRate) + " ms)";

    return text;
}

} // namespace

SettingsPane::SettingsPane(StandaloneHost& host, Settings& settings)
    : host_(host)
    , settings_(settings)
{
    reload();
}

SettingsPane::~SettingsPane()
{
    // Form's destructor asserts the display and mouse lists are empty: the
    // visuals point at the State members above, which are destroyed first.
    clear();
}

void SettingsPane::reload()
{
    auto* audio = host_.audioDriver();
    auto* midi  = host_.midiDriver();

    // --- audio device -------------------------------------------------------
    {
        deviceIds_.clear();
        std::vector<std::string> names;

        const auto savedId = settings_.getString(Settings::keyAudioDevice);

        int index = 0;
        int selected = 0;
        if (audio)
        {
            for (const auto& device : audio->devices())
            {
                names.push_back(device.name);
                deviceIds_.push_back(device.id);

                if (device.id == savedId)
                    selected = index;

                ++index;
            }
        }

        if (names.empty())
            names.push_back("<no audio driver>");

        deviceList_ = joined(names);
        deviceIdx_  = selected;
    }

    // --- sample rate --------------------------------------------------------
    {
        sampleRates_ = audio ? audio->sampleRates() : std::vector<int>{ 44100, 48000 };
        if (sampleRates_.empty())
            sampleRates_ = { 44100, 48000 };

        const int savedRate = settings_.getInt(Settings::keySampleRate, 48000);

        std::vector<std::string> names;
        int selected = 0;
        for (size_t i = 0; i < sampleRates_.size(); ++i)
        {
            names.push_back(std::to_string(sampleRates_[i]));
            if (sampleRates_[i] == savedRate)
                selected = int(i);
        }

        sampleRateList_ = joined(names);
        sampleRateIdx_  = selected;
    }

    // --- buffer size --------------------------------------------------------
    {
        const int savedFrames = settings_.getInt(Settings::keyBufferFrames, 512);
        const int rate = sampleRates_.empty() ? 48000 : sampleRates_[size_t(std::clamp(sampleRateIdx_.get(), 0, int(sampleRates_.size()) - 1))];

        std::vector<std::string> names;
        int selected = 0;
        for (size_t i = 0; i < std::size(kBufferFrames); ++i)
        {
            names.push_back(describeBuffer(kBufferFrames[i], rate));
            if (kBufferFrames[i] == savedFrames)
                selected = int(i);
        }

        bufferList_ = joined(names);
        bufferIdx_  = selected;
    }

    // --- MIDI inputs --------------------------------------------------------
    {
        midiInputs_.clear();

        // "Never configured" and "configured to nothing" are different: the
        // first connects everything (so a fresh install plays), the second
        // honours the user having turned every input off.
        const bool configured = settings_.getBool(Settings::keyMidiInputsSet, false);
        const auto enabledIds = settings_.getStringList(Settings::keyMidiInputs);

        if (midi)
        {
            for (const auto& device : midi->inputs())
            {
                const bool on = configured
                    ? std::find(enabledIds.begin(), enabledIds.end(), device.id) != enabledIds.end()
                    : true;

                midiInputs_.push_back({ device.id, device.name,
                                        std::make_unique<gmpi_forms::State<bool>>(on) });
            }
        }

        midiStatus_ = host_.midiError();
    }

    // --- status -------------------------------------------------------------
    if (host_.isAudioRunning() && audio)
    {
        status_ = "Running: " + std::to_string(audio->getSampleRate()) + " Hz, "
                + std::to_string(audio->getBufferFrames()) + " frames";
    }
    else
    {
        status_ = host_.lastError().empty() ? "Audio is not running." : host_.lastError();
    }

    formIsDirty_ = true;
    redraw();
}

void SettingsPane::pumpDeferred()
{
    if (audioRestartPending_)
    {
        audioRestartPending_ = false;
        applyAudio();
    }

    if (midiRestartPending_)
    {
        midiRestartPending_ = false;
        applyMidi();
    }
}

void SettingsPane::applyAudio()
{
    const auto deviceId = (size_t(deviceIdx_.get()) < deviceIds_.size())
                        ? deviceIds_[size_t(deviceIdx_.get())]
                        : std::string{};

    const int rate = (size_t(sampleRateIdx_.get()) < sampleRates_.size())
                   ? sampleRates_[size_t(sampleRateIdx_.get())]
                   : 48000;

    const int frames = (size_t(bufferIdx_.get()) < std::size(kBufferFrames))
                     ? kBufferFrames[size_t(bufferIdx_.get())]
                     : 512;

    settings_.setString(Settings::keyAudioDevice, deviceId);
    settings_.setInt(Settings::keySampleRate, rate);
    settings_.setInt(Settings::keyBufferFrames, frames);
    settings_.save();

    host_.startAudio(deviceId, rate, frames);

    // reload() rather than just repainting: the granted rate can differ from
    // the requested one, and the buffer labels quote milliseconds computed
    // from it.
    reload();
}

void SettingsPane::applyMidi()
{
    writeMidiInputs();

    std::vector<std::string> enabled;
    for (const auto& in : midiInputs_)
    {
        if (in.enabled->get())
            enabled.push_back(in.id);
    }

    // A selection, not a bare list: writeMidiInputs has just recorded that the
    // user HAS chosen, so an empty list here means they chose nothing - which
    // is the one thing a driver's own id list cannot say.
    host_.startMidi(MidiInputSelection::ids(std::move(enabled)));

    // Just the status line, not the reload() applyAudio does: the tick boxes
    // were built from the same device list a moment ago and rebuilding them here
    // would destroy the State objects the visuals are still bound to. Whether
    // the line is there at all changes the layout, so the form is rebuilt.
    midiStatus_ = host_.midiError();
    formIsDirty_ = true;
    redraw();
}

void SettingsPane::writeMidiInputs()
{
    std::vector<std::string> enabled;
    for (const auto& in : midiInputs_)
    {
        if (in.enabled->get())
            enabled.push_back(in.id);
    }

    settings_.setStringList(Settings::keyMidiInputs, enabled);
    settings_.setBool(Settings::keyMidiInputsSet, true);
    settings_.save();
}

gmpi::ReturnCode SettingsPane::arrange(const gmpi::drawing::Rect* finalRect)
{
    bounds = *finalRect;
    formIsDirty_ = true;
    Invalidate({});
    return gmpi::ReturnCode::Ok;
}

gmpi::ReturnCode SettingsPane::render(gmpi::drawing::api::IDeviceContext* dc)
{
    // A theme change repaints every colour, so it needs the same full rebuild
    // a layout change does - and consumeThemeChanged must be called here,
    // because Form::DoUpdates would otherwise swallow the flag first.
    gmpi::ui::consumeThemeChanged();

    const auto mode = gmpi::ui::themeModeStorage();
    if (mode != lastRenderedTheme_)
        formIsDirty_ = true;

    if (formIsDirty_)
    {
        formIsDirty_ = false;
        lastRenderedTheme_ = mode;
        renderVisuals();
    }

    gmpi::drawing::Graphics g(dc);
    g.clear(gmpi::ui::currentTheme().controlBackground);

    return Form::render(dc);
}

void SettingsPane::Body()
{
    using namespace gmpi::ui;

    constexpr float kRowH = 26.0f;
    constexpr float kGap  = 6.0f;

    float y = bounds.top + 12.0f;
    const float left  = bounds.left + 16.0f;

    // Controls do not stretch to the window's width: a combo box 1200 pixels
    // wide is not more usable, only harder to aim at. Label above control, as
    // in SynthEdit's preferences - a label column wide enough for "Output
    // device" leaves a narrow window nothing for the combo itself.
    const float right = (std::min)(bounds.right - 16.0f, left + 420.0f);

    auto row = [&](std::string_view label) -> gmpi::drawing::Rect
    {
        Label _(label, { left, y, right, y + kRowH });
        y += kRowH;
        const gmpi::drawing::Rect control{ left, y, right, y + kRowH };
        y += kRowH + kGap;
        return control;
    };

    auto heading = [&](std::string_view text)
    {
        y += 10.0f;
        Label _(text, { left, y, right, y + kRowH });
        y += kRowH + 2.0f;
    };

    heading("AUDIO");
    {
        ComboBox c(row("Output device"));
        c.view->enum_list.setSource(&deviceList_);
        c.view->enum_value.setSource(&deviceIdx_);
        c.view->validateAndSave = [this](int32_t i)
        {
            deviceIdx_ = i;
            audioRestartPending_ = true;
        };
    }
    {
        ComboBox c(row("Sample rate"));
        c.view->enum_list.setSource(&sampleRateList_);
        c.view->enum_value.setSource(&sampleRateIdx_);
        c.view->validateAndSave = [this](int32_t i)
        {
            sampleRateIdx_ = i;
            audioRestartPending_ = true;
        };
    }
    {
        ComboBox c(row("Buffer size"));
        c.view->enum_list.setSource(&bufferList_);
        c.view->enum_value.setSource(&bufferIdx_);
        c.view->validateAndSave = [this](int32_t i)
        {
            bufferIdx_ = i;
            audioRestartPending_ = true;
        };
    }

    {
        Label _(status_, { left, y, right, y + kRowH });
        y += kRowH + kGap;
    }

    // Only when the plugin has a MIDI input pin. A gain plugin's settings page
    // listing MIDI keyboards would imply they do something.
    if (host_.wantsMidiInput())
    {
        heading("MIDI INPUT");

        if (midiInputs_.empty())
        {
            Label _("No MIDI inputs found", { left, y, right, y + kRowH });
            y += kRowH + kGap;
        }
        else
        {
            // Tick boxes, not a combo: the driver takes a LIST, and a studio
            // with a keyboard and a control surface wants both at once.
            for (auto& in : midiInputs_)
            {
                ToggleSwitch sw(in.name, *in.enabled);
                sw.view->setBounds({ left, y, right, y + kRowH });
                sw.view->validateAndSave = [this, &in](bool on)
                {
                    *in.enabled = on;
                    midiRestartPending_ = true;
                };
                y += kRowH;
            }
            y += kGap;
        }

        // Only when there is something wrong to say, which under
        // MidiOpenTally's rule means inputs were NAMED and not one of them could
        // be connected. A machine with no keyboard on it says nothing here.
        //
        // Not because the tick list above is empty - it is on Windows and macOS,
        // and it is not on Linux, where ALSA offers a software "Midi Through:
        // Midi Through Port-0" whether any hardware exists or not. The reason is
        // that nothing went wrong: an app nobody has configured asked for
        // whatever was readable and got it. Putting that on the line where
        // errors appear would read as a fault on an ordinary configuration.
        if (!midiStatus_.empty())
        {
            Label _(midiStatus_, { left, y, right, y + kRowH });
            y += kRowH + kGap;
        }
    }

    // Close, bottom right - where a dialog's dismiss button goes, and the
    // shape a settings screen is expected to have. There is no OK/Cancel pair
    // to sit beside it: every control here has already applied itself (see the
    // note at the top of the header), so this button dismisses the page rather
    // than committing anything, and nothing is lost by closing the window
    // instead of pressing it.
    //
    // Only when the shell gave us somewhere to go back to. A plugin with no
    // GUI has no other page, and a Close button that closes onto nothing would
    // be a dead control.
    if (onClose_)
    {
        constexpr float kButtonW = 90.0f;

        y += kGap;
        Button close("Close", { right - kButtonW, y, right, y + kRowH });
        close.view->onClick = [this] { onClose_(); };
        y += kRowH + kGap;
    }
}

} // namespace standalone
} // namespace gmpi
