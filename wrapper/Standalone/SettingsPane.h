#pragma once

// The Audio/MIDI settings page, drawn.
//
// Same construction as SynthEdit's WaylandPreferences: a gmpi::ui::Form built
// from the shared widget set (combo boxes, tick boxes, labels), so it renders
// through whichever backend the shell is using with no toolkit anywhere.
//
// Instant apply, no OK/Cancel. The widget set has no button, and every control
// here writes exactly one setting through the back-channel the widgets already
// provide (validateAndSave). A combo commits once per SELECTION, not per
// keystroke, so choosing a sample rate restarts the device exactly as often as
// an OK button would have.
//
// The apply is DEFERRED to the next timer tick rather than done in the
// callback. validateAndSave runs inside input dispatch - underneath a popup
// menu's own event handling - and re-opening an audio device there means
// joining the driver's threads while the compositor is waiting for us to
// finish handling a click. One flag and a tick later, nothing is on the stack
// that minds.

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

    // Status line under the controls: what actually opened, or why nothing did.
    std::string status_;
};

} // namespace standalone
} // namespace gmpi
