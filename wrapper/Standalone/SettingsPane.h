#pragma once

// The Audio/MIDI settings page, drawn.
//
// Same construction as SynthEdit's WaylandPreferences: a gmpi::ui::Form built
// from the shared widget set (combo boxes, tick boxes, labels), so it renders
// through whichever backend the shell is using with no toolkit anywhere.
//
// Instant apply, no OK/Cancel. Every control here writes exactly one setting
// through the back-channel the widgets already provide (validateAndSave), and
// a combo commits once per SELECTION rather than per keystroke - so choosing a
// sample rate restarts the device exactly as often as an OK button would have.
//
// The one button on the page is Close, and it commits nothing: closing the
// window instead of pressing it loses nothing either. It is here because a
// settings screen you dismiss is the shape people expect, and the alternative -
// a second menu item to switch back - made the menu a radio pair, which is not
// how anyone looks for the way out of a settings page.
//
// The apply is DEFERRED to the next timer tick rather than done in the
// callback. validateAndSave runs inside input dispatch - underneath a popup
// menu's own event handling - and re-opening an audio device there means
// joining the driver's threads while the compositor is waiting for us to
// finish handling a click. One flag and a tick later, nothing is on the stack
// that minds.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "AudioMidiDevices.h"
#include "GmpiUiDrawing.h"
#include "GmpiApiCommon.h"
#include "GmpiSdkCommon.h"
#include "experimental/forms.h"
#include "experimental/theme.h"
#include "helpers/NativeUi.h"

namespace gmpi
{
namespace standalone
{

class StandaloneHost;
class Settings;

class SettingsPane : public gmpi::ui::Form
{
public:
    SettingsPane(StandaloneHost& host, Settings& settings);
    ~SettingsPane();

    // Re-read the device lists from the drivers. Call each time the page is
    // shown: devices come and go while the app runs (plug in a keyboard) and a
    // list captured at startup goes stale within minutes.
    void reload();

    // Call once per frame from the app's tick. Applies whatever the user
    // changed since the last tick; does nothing when nothing changed.
    void pumpDeferred();

    // What the page's Close button does. Set by the shell, because only it
    // knows what "closed" means - here, showing the plugin's editor again.
    //
    // Leave it unset and no Close button is drawn at all, which is the right
    // answer for a plugin with no GUI: there is no other page to return to,
    // and a button that dismisses onto nothing is worse than no button.
    //
    // Set it BEFORE the page is first shown. The form is rebuilt from Body()
    // whenever it is marked dirty, and whether the button exists is decided
    // there.
    void setOnClose(std::function<void()> onClose) { onClose_ = std::move(onClose); }

    void Body() override;

    // Form's base arrange/render neither lay out nor clear - a Form subclass
    // owns that. Without these the page has zero bounds and paints black.
    gmpi::ReturnCode arrange(const gmpi::drawing::Rect* finalRect) override;
    gmpi::ReturnCode measure(const gmpi::drawing::Size*, gmpi::drawing::Size*) override
    { return gmpi::ReturnCode::Ok; }
    gmpi::ReturnCode render(gmpi::drawing::api::IDeviceContext* dc) override;

private:
    void applyAudio();
    void applyMidi();
    void writeMidiInputs();

    StandaloneHost& host_;
    Settings& settings_;

    std::function<void()> onClose_;

    bool formIsDirty_ = true;
    gmpi::ui::ThemeMode lastRenderedTheme_ = gmpi::ui::ThemeMode::Dark;

    // What pumpDeferred has to do, set from the widget callbacks.
    bool audioRestartPending_ = false;
    bool midiRestartPending_  = false;

    // Widget state. gmpi_forms::State is what the widgets bind to, so these
    // ARE this page's model: reload() fills them and each widget's
    // validateAndSave writes its one setting straight through.
    gmpi_forms::State<std::string> deviceList_;
    gmpi_forms::State<int32_t>     deviceIdx_;
    gmpi_forms::State<std::string> sampleRateList_;
    gmpi_forms::State<int32_t>     sampleRateIdx_;
    gmpi_forms::State<std::string> bufferList_;
    gmpi_forms::State<int32_t>     bufferIdx_;

    // One tick box per MIDI input, so several can be enabled at once - the
    // driver takes a list, not a choice.
    struct MidiInput
    {
        std::string id;
        std::string name;
        std::unique_ptr<gmpi_forms::State<bool>> enabled;
    };
    std::vector<MidiInput> midiInputs_;

    // Parallel to the combo boxes, which carry indices only.
    std::vector<std::string> deviceIds_;
    std::vector<int> sampleRates_;

    // Which device sampleRates_ was asked about. The rate combo carries an
    // INDEX into that list, so the two only mean anything together: move the
    // device combo and the index suddenly points into the previous device's
    // rates. applyAudio checks this before believing it.
    std::string sampleRatesDeviceId_;

    // Status line under the audio controls: what actually opened, or why nothing
    // did.
    std::string status_;

    // The line under THAT: what the device that did open could not give the
    // plugin (AudioMidiDevices.h::lastWarning), or empty. Separate from status_
    // because they are both true at once - "Running: 48000 Hz, 512 frames" with
    // every input pin silent is exactly the state this exists to show, and
    // status_ on its own reported full health for it.
    std::string audioWarning_;

    // The same for MIDI, and separate because the two failures are unrelated and
    // the audio one is the one that stops the app making a sound. It sits under
    // the tick boxes it is about, and is empty in the ordinary case - including
    // on a machine with no MIDI hardware, where an app nobody has configured
    // asked for whatever was readable and got it. It fills only when inputs were
    // named and not one of them could be connected.
    std::string midiStatus_;
};

} // namespace standalone
} // namespace gmpi
