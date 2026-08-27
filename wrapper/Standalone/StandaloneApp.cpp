#include "StandaloneApp.h"

#include <atomic>
#include <cmath>
#include <climits>                     // INT_MIN -- E32's "no saved position"
#include <csignal>
#include <string>
#include <vector>

#include "AppLayout.h"
#include "MenuBarView.h"
#include "SessionState.h"
#include "SettingsPane.h"
#include "StandaloneHost.h"
#include "StandaloneSettings.h"

#include "mcp/CommandChannelHost.h"

namespace gmpi
{
namespace standalone
{

PlatformShell::~PlatformShell() = default;

void PlatformShell::reportFatal(const std::string& message)
{
    reportStatus(message);
    showFatalAlert(message);
}

void PlatformShell::showFatalAlert(const std::string&)
{
}

namespace
{

// BACKLOG E32 -- the persisted window geometry.
//
// "window." rather than a bare name because Settings is one flat key=value file
// shared with the device selection, and its header promises unknown keys are
// preserved on save. A prefix keeps a later "window.maximised" (the row's third
// trap) from colliding with anything.
constexpr const char* kWindowWidthKey  = "window.width";
constexpr const char* kWindowHeightKey = "window.height";

// BACKLOG E32, the position half. Screen PIXELS, not DIPs -- see
// PlatformShell::windowPosition for why the two halves use different units.
constexpr const char* kWindowXKey = "window.x";
constexpr const char* kWindowYKey = "window.y";

// "no saved position", because 0,0 is a POSITION -- the top-left of the primary
// monitor, and on a multi-monitor desktop a perfectly ordinary place to leave a
// window. A missing key cannot be spelled as a value in range, so it is spelled
// as one that cannot be.
constexpr int kNoSavedPosition = INT_MIN;

// One bound, stated once, used by both the read and the write -- so a size this
// build refuses to restore is also a size it refuses to save, and the file
// cannot accumulate values that are silently ignored forever.
//
// The lower bound is deliberately below setMinimumClientSize's 320: this only
// has to reject nonsense (0 from a shell that could not report, or a negative
// from a corrupted file). Clamping to the real minimum is the shell's job and
// it already does it, on every path, including the ones that never read a file.
//
// The upper bound is a sanity rail, not a screen-size check: this code cannot
// know the display geometry at the point it runs, and a window wider than any
// monitor is still a window the user can drag and resize. It exists to stop a
// garbled file producing something the compositor has to fight.
bool isUsableWindowSize(int dips)
{
    return dips >= 64 && dips <= 16384;
}

// SIGTERM/SIGINT must run the normal shutdown, not the default instant kill.
// The session manager sends SIGTERM at logout and some compositors crash when a
// client vanishes without disconnecting - the workaround roundtrip lives in
// ~Connection, which a default-handled signal never reaches. Ctrl-C in the
// terminal the app was launched from is the ordinary case on both unixes.
//
// Windows raises neither at a GUI app, and MSVC documents SIGINT as unsupported
// there, so the handlers are not installed on that platform at all. Its real
// analogue is session end, and it lives where it belongs -
// WM_QUERYENDSESSION/WM_ENDSESSION in windows/ToplevelWindow.cpp, which routes
// to the same requestClose() the tick below calls. The FLAG and the tick's
// check of it stay portable: one tick, one shape, on all three.
//
// A handler may only touch lock-free atomics; the tick notices the flag and
// closes the window from safe context.
std::atomic<bool> terminationRequested{ false };

#if !defined(_WIN32)
void onTerminationSignal(int)
{
    terminationRequested = true;
}
#endif

} // namespace

namespace
{
int    gArgc = 0;
char** gArgv = nullptr;
}

int    standaloneArgc() { return gArgc; }
char** standaloneArgv() { return gArgv; }

namespace
{
std::function<std::vector<DivertedDialog>()> gDialogDrain;
}

void setDialogDrain(std::function<std::vector<DivertedDialog>()> drain)
{
    gDialogDrain = std::move(drain);
}

std::vector<DivertedDialog> drainDivertedDialogs()
{
    // No drain installed is not an error: a plugin that raises no dialogs, or one
    // not built against EditorLib at all, simply has nothing to report.
    return gDialogDrain ? gDialogDrain() : std::vector<DivertedDialog>{};
}

int runStandaloneApp(PlatformShell& shell, int argc, char** argv)
{
    // FIRST, before the host or the plugin exist: the controller reads this
    // during its own initialize(), which runs well inside the setup below.
    gArgc = argc;
    gArgv = argv;

    StandaloneHost host;
    if (!host.init())
    {
        shell.reportFatal("This binary contains no GMPI plugin (MP_GetFactory returned nothing).");
        return 1;
    }

    Settings settings(host.pluginName());
    settings.load();

    // --- the plugin's patch --------------------------------------------------
    // What the DAW does for the VST3 and CLAP builds of this same plugin. It
    // costs the shells nothing: no PlatformShell method was added for it, and
    // nothing below this line needs to know a file exists. All three platforms
    // therefore get it by running the same sequence they already ran.
    //
    // HERE, before the window is sized. An editor's preferred size can depend
    // on its patch, so measuring on the defaults and restoring afterwards would
    // fit the window to a patch the user never had. Before startAudio() too,
    // which is a harder requirement - see SessionState::restore.
    //
    // And before onEditorAttached(), which is what makes it invisible to the
    // editor: initUi pushes the CURRENT value of every parameter into the
    // plugin's GUI, and by then those are the restored ones. Nothing has to
    // notify the editor of a restore, because from where it sits nothing was
    // ever restored.
    SessionState session(host, settings);
    session.restore(shell);

    host.setOnParameterEdited([&session] { session.markDirty(); });

    // Size the window from the editor, plus the strip the menu bar occupies.
    float editorWidth = 0.0f;
    float editorHeight = 0.0f;
    host.getEditorSize(editorWidth, editorHeight);

    int windowWidth  = static_cast<int>(std::ceil(editorWidth));
    int windowHeight = static_cast<int>(std::ceil(editorHeight + MenuBarView::kHeight));

    // BACKLOG E32 -- reopen at the size the user left, not the editor's default.
    //
    // IN DIPs, NOT PIXELS, and that is the whole reason these are stored rather
    // than the canvas size: a window moved to a monitor of a different scale
    // must reopen the same LOGICAL size. The shell already does that arithmetic
    // (canvasSize multiplies by the scale); saving pixels would bake one
    // monitor's scale into the file and reopen wrong on the other.
    //
    // WHY SIZE AND NOT POSITION. Position is per-shell and Linux can never have
    // it: xdg-shell has no set-position, so a Wayland client cannot place its
    // own window. That is a property of the protocol, not a gap to close later.
    // Size is portable, so it lives here; whatever Windows and macOS do about
    // position belongs in their own shells.
    //
    // A saved size is a REQUEST. Every shell may be handed something else by the
    // compositor or the window manager, and setMinimumClientSize below still
    // applies -- so this cannot reopen a window too small to use even if the
    // file says so.
    {
        const int savedWidth  = settings.getInt(kWindowWidthKey,  0);
        const int savedHeight = settings.getInt(kWindowHeightKey, 0);

        // Both, or neither. A half-written pair is likelier to be a truncated
        // file than a deliberate one, and one restored dimension against one
        // default is a shape the user never chose.
        if (isUsableWindowSize(savedWidth) && isUsableWindowSize(savedHeight))
        {
            windowWidth  = savedWidth;
            windowHeight = savedHeight;
        }
    }

    if (!shell.createWindow(host.pluginName(), windowWidth, windowHeight))
    {
        const auto why = shell.lastError();
        shell.reportFatal(why.empty() ? "Could not create the application window." : why);
        return 1;
    }

    // BACKLOG E32 -- and reopen WHERE the user left it, on the shells that can.
    //
    // AFTER createWindow rather than as an argument to it, for two reasons.
    // The window is created hidden on every shell that supports this and only
    // shown when the event loop starts, so there is no visible jump to avoid;
    // and passing a position down would mean changing a pure virtual that all
    // three shells implement, to carry something two of them would ignore.
    //
    // NO CLAMPING HERE ON PURPOSE. Only the shell can see the monitors, so the
    // shell decides what "on screen" means -- see setWindowPosition.
    //
    // Both, or neither, for the same reason the size half pairs its two: half a
    // position is a coordinate the user never chose.
    {
        const int savedX = settings.getInt(kWindowXKey, kNoSavedPosition);
        const int savedY = settings.getInt(kWindowYKey, kNoSavedPosition);

        if (savedX != kNoSavedPosition && savedY != kNoSavedPosition)
            shell.setWindowPosition(savedX, savedY);
    }

    // A window narrower than the menu bar's titles is not useful.
    shell.setMinimumClientSize(320, static_cast<int>(MenuBarView::kHeight) + 80);

    host.setAudioDriver(shell.createAudioDriver());
    if (host.wantsMidiInput())
        host.setMidiDriver(shell.createMidiDriver());

    // --- the window's contents ----------------------------------------------
    // Refcounted objects, so each is owned by a shared_ptr rather than by this
    // stack frame.
    //
    // All three shells used to claim here that the frame and the layout kept
    // BORROWED pointers because "attachClient does not addRef", and that was
    // wrong twice over: AppLayout::attach assigns every child into a
    // gmpi::shared_ptr, and so does the frame on two platforms of three
    // (DxDrawingFrameBase::attachClient and DrawingFrameCommon both
    // queryInterface into shared_ptr members - only the Wayland frame really
    // does borrow, from a raw client_).
    //
    // It matters now, because the shell owns the frame and the shell outlives
    // this function. What makes that safe in either direction is that
    // closeWindow() is the LAST teardown step and is called explicitly rather
    // than left to the shell's destructor: it drops the frame's references -
    // or clears Wayland's borrowed pointer - while the layout is still alive,
    // and these shared_ptrs are released immediately afterwards as this frame
    // unwinds.
    gmpi::shared_ptr<AppLayout> layout(new AppLayout());
    gmpi::shared_ptr<MenuBarView> menuBar(new MenuBarView());
    gmpi::shared_ptr<SettingsPane> settingsPane(new SettingsPane(host, settings));

    menuBar->setFont(shell.menuFont());

    layout->setMenuBarHeight(MenuBarView::kHeight);
    layout->setMenuBar(static_cast<gmpi::api::IDrawingClient*>(menuBar.get()));

    const int pageEditor   = layout->addPage(host.editorDrawingClient());
    const int pageSettings = layout->addPage(static_cast<gmpi::api::IDrawingClient*>(settingsPane.get()));

    // The way OUT of the settings page. A menu opens it; its own Close button
    // dismisses it, the way a settings screen anywhere else does. Only offered
    // when there is a plugin editor to go back to.
    if (host.editorDrawingClient())
        settingsPane->setOnClose([&] { layout->showPage(pageEditor); });

    // What File > Revert to Plugin Defaults has asked for, applied by the tick.
    // Declared before the menus so the item can capture it, and beside them
    // rather than inside SessionState because it is a request the USER made, not
    // a fact about the file.
    bool revertPending = false;

    // --- menus ---------------------------------------------------------------
    // Modelled on JUCE's standalone shell, which is what anyone reaching for
    // this has used before: a File menu that quits and an Options menu that
    // reaches the device settings.
    {
        std::vector<MenuBarView::Menu> menus;

        menus.push_back({ "File", {
            {
                // The counterweight to a session that is always restored. With
                // no way back, a patch you dislike is one you are stuck with -
                // and unlike a DAW there is no project to close without saving.
                //
                // DEFERRED to the tick, like every device change on the settings
                // page and for the same reason: this runs from a popup menu's
                // own completion, with the editor's event handling underneath
                // it, and pushing a parameter into the editor from there means
                // reentering a view that is still handling a click.
                "Revert to Plugin Defaults",
                [&] { revertPending = true; },
                // Greyed out for a plugin with no patch, rather than offered and
                // silently doing nothing.
                [&] { return host.hasStatefulParameters(); }
            },
            {},   // separator
            { "Quit", [&shell] { shell.requestClose(); } },
        } });

        menus.push_back({ "Options", {
            {
                // No "Plugin Editor" item beside it. Two items that switch
                // between two pages is a radio pair, and nobody looks in a menu
                // for the way out of a settings screen - they look for a button
                // on the screen itself, which is where it now is.
                "Audio/MIDI Settings...",
                [&]
                {
                    // Re-read the device lists on the way in: keyboards and
                    // endpoints come and go while the app runs.
                    settingsPane->reload();
                    layout->showPage(pageSettings);
                },
                {},
                [&] { return layout->currentPage() == pageSettings; }
            },
            {},   // separator
            { "Quit", [&shell] { shell.requestClose(); } },
        } });

        menuBar->setMenus(std::move(menus));
    }

    if (!shell.attachClient(static_cast<gmpi::api::IDrawingClient*>(layout.get()),
                            host.parameterHost()))
    {
        const auto why = shell.lastError();
        shell.reportFatal(why.empty() ? "Could not create the editor view." : why);
        return 1;
    }

    // AFTER attaching: the plugin's editor is initialised against a host it now
    // has, and initUi pushes every current parameter value into it.
    host.onEditorAttached();

    // BEFORE the device is opened, and macOS is why - see
    // PlatformShell::showAndPaint.
    shell.showAndPaint();

    // --- devices -------------------------------------------------------------
    {
        // The driver names its own sentinel, so the fallback here always agrees
        // with what open() will test for. Null only if a shell chose to build
        // without audio at all, which startAudio below reports.
        const auto* audio = host.audioDriver();

        const auto deviceId = settings.getString(
            Settings::keyAudioDevice, audio ? audio->defaultDeviceId() : "");
        // 0 is "no preference" (AudioDriver::open), and it is what an app with
        // no settings file has to ask for. A default of 48000 here was not a
        // user's choice, it was this line's, and every driver acts on a rate:
        // WASAPI spends a failed Initialize on it before falling back, CoreAudio
        // reclocks the hardware to it, and PipeWire turns it into node.rate -
        // asking the daemon to move the WHOLE graph, every other client in the
        // session with it, on behalf of a preference nobody expressed. Only a
        // user who has applied a rate on the settings page writes this key, and
        // only then does anything get asked of the device.
        const int sampleRate   = settings.getInt(Settings::keySampleRate, 0);
        const int bufferFrames = settings.getInt(Settings::keyBufferFrames, 512);

        if (!host.startAudio(deviceId, sampleRate, bufferFrames))
        {
            // Not fatal, and not a message box either. A standalone that refuses
            // to open its window because the soundcard is busy has removed the
            // only UI that could pick a different one - so say why for whoever
            // is reading a log, then open ON the settings page instead of the
            // plugin's editor, where the failure is named next to the device
            // list that fixes it.
            shell.reportStatus("Audio: " + host.lastError());

            settingsPane->reload();
            layout->showPage(pageSettings);
        }

        if (host.wantsMidiInput())
        {
            // The flag and the list are resolved together, in one place all
            // three shells share. With nothing saved, every readable input is
            // connected, so a fresh install plays the moment a keyboard is
            // plugged in. Only actually ticking or unticking an input saves a
            // list - opening the settings page and closing it again does not -
            // and from then on that list is honoured exactly, including an empty
            // one, which means no MIDI input at all rather than all of them.
            if (!host.startMidi(MidiInputSelection::saved(
                    settings.getBool(Settings::keyMidiInputsSet, false),
                    settings.getStringList(Settings::keyMidiInputs))))
            {
                // Said, but not acted on - no page switch, unlike the audio
                // failure above. What is left is an app that still runs and
                // still makes a sound, just without the keyboard.
                //
                // By MidiOpenTally's rule this fires only when something was
                // asked for and none of it arrived: a saved selection whose
                // devices have been unplugged or are held by another program,
                // or - with nothing saved, where the request is "everything
                // readable" - every readable input on the machine refusing at
                // once. Nothing saved and nothing plugged in is a success and
                // stays quiet, on all three platforms, whether or not the
                // platform lists a software port of its own.
                shell.reportStatus("MIDI: " + host.midiError());
            }
        }
    }

    // --- command channel -----------------------------------------------------
    // An IPC endpoint naming this process, so a test harness or an MCP server
    // can drive the very plugin instance the user is looking at. A named pipe on
    // Windows, a unix socket on both the others; none of that is visible here,
    // and neither is whether this build has a channel at all - see
    // mcp/CommandChannelHost.h.
    mcp::CommandChannelHost commandChannel;
    commandChannel.start(host, *layout, shell);

#if !defined(_WIN32)
    std::signal(SIGTERM, onTerminationSignal);
    std::signal(SIGINT,  onTerminationSignal);
#endif

    const auto services = shell.backendServices();

    // What the tick below compares against to notice a stream that has stopped
    // on its own. Seeded from the state startAudio left behind, so an app that
    // never got a device is not reported as having lost one.
    bool audioWasRunning = host.isAudioRunning();

    const int exitCode = shell.runEventLoop([&](int elapsedMs)
    {
        if (terminationRequested.load(std::memory_order_relaxed))
        {
            shell.requestClose();
            return;
        }

        // Commands from the endpoint, BEFORE the timer pump rather than after:
        // a --set-param queues the value for the processor and the pump is what
        // delivers it, so draining first means a parameter set now is audible
        // this tick rather than the next one.
        commandChannel.drain();

        // Only where gmpi_ui has no native timer source. This is what drives the
        // host's parameter queues and the form widgets' animation.
        if (services.timerSource == PlatformShell::TimerSource::appTick)
            gmpi::TimerManager::instance()->pump(elapsedMs);

        // Lets the visible page service its DSP->GUI queue once per frame,
        // rather than polling it from the audio thread's side. Skipped where the
        // frame's own render timer already does it, which would otherwise
        // service the queue twice per frame.
        if (services.redrawClient == PlatformShell::RedrawClientDriver::appTick)
            layout->preGraphicsRedraw();

        // Device changes the user asked for while we were inside input dispatch.
        // Re-opening an audio device there would mean joining the driver's
        // threads with a click still on the stack.
        settingsPane->pumpDeferred();

        // A revert the user asked for while we were inside a menu's completion.
        // BEFORE the save below, so the patch it throws away is kept aside first
        // and the debounce it starts is the one that writes the defaults.
        if (revertPending)
        {
            revertPending = false;

            session.keepCurrentAside(shell);
            host.revertToDefaults();
        }

        // The patch, once the edits have stopped for a moment.
        //
        // Debounced rather than saved at quit alone, because a standalone gets
        // killed - a crash, a `kill -9`, a machine that reboots for an update -
        // and an hour of tweaking lost to that is the same complaint as an hour
        // lost to quitting.
        //
        // The debounce is what keeps the write out here in the tick, which is
        // the other half of why it is a debounce: an edit is delivered from
        // inside input dispatch, and a knob being dragged would otherwise put a
        // disk write on the stack under the editor, once per frame.
        session.pump(elapsedMs, shell);

        // A stream that stopped without being asked to: the interface unplugged,
        // the audio service restarted, the server dropping the client. The
        // driver discovered it on a thread that could do nothing about it and
        // left a flag (AudioMidiDevices.h::isStreamRunning); this is the main
        // thread noticing, which is the first moment anything may be said or
        // done about it.
        //
        // AFTER pumpDeferred, so that a device the user has just applied is
        // already open and this compares against what they now have.
        //
        // Reported and no more. Nothing reopens a device here - see the same
        // note on isStreamRunning - and the page is NOT switched to as it is on
        // a failed startup: the plugin's editor is on screen and the user is
        // using it, and a settings page appearing over the top of a knob being
        // dragged is its own fault report. Startup could switch because there
        // was nothing to interrupt.
        const bool audioIsRunning = host.isAudioRunning();

        if (audioWasRunning && !audioIsRunning)
        {
            // Empty when it was the app's own doing - the user applied a device
            // that would not open, and applyAudio has already put lastError() on
            // the page. Only a death has something to announce.
            if (const auto why = host.audioStoppedReason(); !why.empty())
            {
                // No "Audio: " prefix, unlike the startup report above: the
                // driver's sentence already names itself, and this one goes out
                // whole so a log and the settings page say the same words.
                shell.reportStatus(why);

                // The page's status line is built by reload(), so without this
                // it would go on saying "Running" for as long as it was left
                // open - which is the whole of the fault this path exists for.
                settingsPane->reload();
            }
        }

        audioWasRunning = audioIsRunning;
    });

    // --- teardown ------------------------------------------------------------
    // FIRST, and on this thread: stop() refuses further model access before it
    // joins its threads, so no command can still be reaching for the host, the
    // editor or the drivers the next lines tear down.
    commandChannel.stop();

    // Then the patch, while everything it reads is still standing.
    //
    // AFTER stop(), so that it is inside the fence the line above just put up
    // rather than one statement in front of it. Nothing races either way - the
    // dispatcher only ever runs from drain(), on this thread, and the loop it
    // was called from has already returned - but "no command can still be
    // reaching for the model" is a sentence that should be true of every line
    // below it, including this one.
    //
    // BEFORE shell.closeWindow(), which is the only line below that this one
    // actually has to come before. closeWindow detaches the frame's client,
    // which hands setHost(nullptr) down through AppLayout to the plugin's own
    // editor, and ~StandaloneHost then unRegisterGui's it and does the same
    // again. Those are the teardown's calls INTO the plugin, and the very next
    // comment is about a plugin that writes a pin from inside one. A patch
    // captured after them is a patch captured across the plugin being taken
    // apart.
    //
    // The two driver stops in between demand nothing of their own, and this
    // comment used to say they did - that stopping them was the last chance at
    // "the values only the DSP authored". It is not. captureState reads the
    // CONTROLLER's store, deliberately and never the processor's (StandaloneHost
    // says why), and closing an audio device does not touch it. Nor can a DSP
    // value still be on its way: the only drain of message_que_dsp_to_ui is
    // StandaloneHost::onTimer, which the tick called and this teardown does not,
    // so whatever the last tick brought across is already all there is.
    //
    // What catches those values is that this save happens at ALL. They reach the
    // controller's store without ever marking the session dirty - a meter would
    // otherwise keep it permanently so - which is why the debounce never wrote
    // them and why this call is unconditional.
    session.saveNow(shell);

    // BACKLOG E32 -- the window's own geometry, saved beside the device
    // selection rather than in the plugin's patch. SessionState.h states the
    // rule this follows: "Window size and position are the shell's business,
    // not the plugin's, and are not kept here."
    //
    // BEFORE closeWindow(), because logicalSize() reads the live frame and the
    // shells destroy it in there. Measured on Wayland: after closeWindow() the
    // call returns 0x0, which would persist as a size the next launch rejects --
    // silently reverting to the default and looking like the feature never
    // worked.
    {
        float finalWidth = 0.0f;
        float finalHeight = 0.0f;
        shell.logicalSize(finalWidth, finalHeight);

        const int w = static_cast<int>(finalWidth + 0.5f);
        const int h = static_cast<int>(finalHeight + 0.5f);

        // Only write a size worth restoring. A shell that could not report one
        // must leave whatever is already in the file alone, so an unrelated
        // failure at teardown does not erase a good saved size.
        if (isUsableWindowSize(w) && isUsableWindowSize(h))
        {
            settings.setInt(kWindowWidthKey, w);
            settings.setInt(kWindowHeightKey, h);
            settings.save();
        }

        // BACKLOG E32, the position half. BEFORE closeWindow() for the same
        // reason the size is: the shell reads its live window, and closeWindow
        // destroys it. A shell that cannot answer leaves whatever is in the
        // file alone -- so Linux, which never can, neither writes nor erases,
        // and a config carried between machines keeps a position the box that
        // wrote it can still use.
        int finalX = 0;
        int finalY = 0;
        if (shell.windowPosition(finalX, finalY))
        {
            settings.setInt(kWindowXKey, finalX);
            settings.setInt(kWindowYKey, finalY);
            settings.save();
        }
    }

    // `session` is destroyed BEFORE `host`, being declared after it, and the
    // callback installed on the host captures `&session`. ~StandaloneHost calls
    // into the plugin (unRegisterGui, then setHost(nullptr) on the editor), and
    // a plugin that writes a pin from there would reach that callback with the
    // object already gone. Cut it here rather than reason about what a plugin
    // does in its destructor.
    host.setOnParameterEdited({});

    // Then the audio and MIDI threads, before anything they touch goes away.
    // close() on either driver returns only once no callback can still be
    // running.
    host.stopMidi();
    host.stopAudio();

    // Then the window, which is what releases the editor. The three shells used
    // to disagree about this line: Windows destroyed the frame from inside
    // WM_CLOSE, with both those threads still running, and then detached a
    // client from a frame that had already gone.
    shell.closeWindow();

    // settingsPane, menuBar and layout are released as this frame unwinds, then
    // session, then settings, then host - which keeps SettingsPane's references
    // to the last two, and SessionState's to the host, valid for the whole of
    // their lives.
    return exitCode;
}

} // namespace standalone
} // namespace gmpi
