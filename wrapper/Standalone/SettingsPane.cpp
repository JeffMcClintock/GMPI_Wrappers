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
    // Asked of the DEVICE chosen above rather than of the driver in general.
    // The app does not resample (AudioMidiDevices.h::sampleRates), so a rate the
    // selected device will not run at is not a choice, and this list used to be
    // the same six numbers whatever was plugged in. An empty answer means the
    // driver cannot say; that, and a list with only one rate on it, are both
    // handled in Body() by drawing a line of text instead of a control.
    {
        const auto selectedDevice = (size_t(deviceIdx_.get()) < deviceIds_.size())
                                  ? deviceIds_[size_t(deviceIdx_.get())]
                                  : std::string{};

        sampleRates_ = audio ? audio->sampleRates(selectedDevice) : std::vector<int>{};
        sampleRatesDeviceId_ = selectedDevice;

        // 0, not a number of this page's own choosing: absent means the user
        // has never applied a rate, and the fallback below - what is actually
        // coming out of the speakers - is the honest thing to show them. A
        // default of 48000 here preselected a rate nobody had asked for, on a
        // device that may well be clocked somewhere else.
        const int savedRate = settings_.getInt(Settings::keySampleRate, 0);

        // What is coming out of the speakers, which is what the combo falls back
        // to when the saved rate is not on offer - the user moved to a device
        // clocked elsewhere, or changed it in the OS. Landing on index 0 instead
        // would show a rate that is not the one being heard.
        const int runningRate = (audio && host_.isAudioRunning()) ? audio->getSampleRate() : 0;

        std::vector<std::string> names;
        int savedIndex   = -1;
        int runningIndex = -1;

        for (size_t i = 0; i < sampleRates_.size(); ++i)
        {
            names.push_back(std::to_string(sampleRates_[i]));

            if (sampleRates_[i] == savedRate)
                savedIndex = int(i);
            if (sampleRates_[i] == runningRate)
                runningIndex = int(i);
        }

        // The SETTING is deliberately not rewritten here. A device that cannot
        // do 44100 today may be back tomorrow, and silently overwriting the
        // preference on the way past would lose it. It is overwritten only when
        // the user next applies something, which is them committing to what the
        // page is showing them.
        sampleRateList_ = joined(names);
        sampleRateIdx_  = savedIndex >= 0 ? savedIndex : (runningIndex >= 0 ? runningIndex : 0);
    }

    // --- buffer size --------------------------------------------------------
    {
        const int savedFrames = settings_.getInt(Settings::keyBufferFrames, 512);

        // Only to turn frames into milliseconds for the labels. Zero when
        // nothing is offered and nothing is running, and describeBuffer then
        // prints the frame count alone rather than a duration derived from a
        // rate this page made up.
        const int rate = !sampleRates_.empty()
                       ? sampleRates_[size_t(std::clamp(sampleRateIdx_.get(), 0, int(sampleRates_.size()) - 1))]
                       : ((audio && host_.isAudioRunning()) ? audio->getSampleRate() : 0);

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

    // A DEGRADED open, which the line above cannot express: it says "Running"
    // and means it. Empty unless the driver has something to report, and empty
    // whenever audio is not running at all.
    audioWarning_ = host_.audioWarning();

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

    // A rate only where the page offered a CHOICE of one, which is what a combo
    // box being drawn at all means (Body()). Zero - AudioDriver::open's "no
    // preference" - covers both the cases that got a line of text instead: no
    // rates to offer, and exactly one, which is the rate the device is already
    // clocked at. Asking for that rate and asking for nothing land in the same
    // place, and only the second avoids writing a preference the user never
    // expressed; on PipeWire that is the difference between leaving the graph
    // alone and asking the daemon to reclock it at every launch.
    //
    // The saved preference is likewise left alone rather than overwritten with a
    // placeholder - see the setInt below.
    int rate = (sampleRates_.size() > 1 && size_t(sampleRateIdx_.get()) < sampleRates_.size())
             ? sampleRates_[size_t(sampleRateIdx_.get())]
             : 0;

    if (rate > 0 && deviceId != sampleRatesDeviceId_)
    {
        // The device combo moved since reload() built sampleRates_, so the rate
        // above was read out of the PREVIOUS device's list. Keep it only if the
        // device being opened really has it, and ask for nothing otherwise -
        // rather than persisting one device's rate as the preference and, on
        // PipeWire, asking the daemon to reclock the whole graph to it.
        //
        // reload() at the end of this function rebuilds the list for the device
        // that did open, so this is the one moment the two can disagree.
        auto* audio = host_.audioDriver();
        const auto rates = audio ? audio->sampleRates(deviceId) : std::vector<int>{};

        if (std::find(rates.begin(), rates.end(), rate) == rates.end())
            rate = 0;
    }

    const int frames = (size_t(bufferIdx_.get()) < std::size(kBufferFrames))
                     ? kBufferFrames[size_t(bufferIdx_.get())]
                     : 512;

    settings_.setString(Settings::keyAudioDevice, deviceId);
    if (rate > 0)
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
        const auto control = row("Sample rate");

        if (sampleRates_.size() < 2)
        {
            // NOTHING TO CHOOSE BETWEEN, which covers two situations and rules
            // out a combo box for both. Empty means the driver could not
            // enumerate the device's rates; one entry is the ordinary answer on
            // Windows, where a shared-mode endpoint runs at its mix rate and
            // that is the only rate it has. A combo with one option looks
            // selectable and is not, and an empty one does nothing at all when
            // clicked - neither is a control. One that offered the app's own
            // guesses would be offering rates nobody has checked the hardware
            // will take.
            auto* audio = host_.audioDriver();
            const int running = (audio && host_.isAudioRunning()) ? audio->getSampleRate() : 0;

            // Label copies the text it is given, so a temporary is safe here.
            Label _(!sampleRates_.empty()
                        ? std::to_string(sampleRates_.front()) + " Hz (the device's only rate)"
                        : (running > 0
                               ? std::to_string(running) + " Hz (chosen by the audio system)"
                               : std::string("Chosen by the audio system")),
                    control);
        }
        else
        {
            ComboBox c(control);
            c.view->enum_list.setSource(&sampleRateList_);
            c.view->enum_value.setSource(&sampleRateIdx_);
            c.view->validateAndSave = [this](int32_t i)
            {
                sampleRateIdx_ = i;
                audioRestartPending_ = true;
            };
        }
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

    // What opened but is not working, on its own line under the one that says
    // it is running - the MIDI section further down has a line of its own for
    // the same kind of thing. Only when there is something to say: a device
    // that gave the plugin everything it asked for leaves this empty, and an
    // empty line where faults appear would read as one.
    //
    // Wider than the controls above it. Those are clamped so that a combo box
    // is not 1200 pixels of target to aim at; a sentence is not aimed at, and
    // the longest of these names two sample rates.
    if (!audioWarning_.empty())
    {
        Label _(audioWarning_, { left, y, bounds.right - 16.0f, y + kRowH });
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
