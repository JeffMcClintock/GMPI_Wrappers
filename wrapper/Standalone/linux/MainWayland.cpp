// The GMPI standalone app on Linux/Wayland.
//
// No toolkit and no X11: the window, input, menus and dialogs come from
// gmpi_ui's Wayland backend and everything is drawn by the CPU backend. The
// plugin sees exactly what it sees in a DAW - a Processor driven by process()
// and an Editor attached to a drawing host - with an audio device and a MIDI
// port standing in for the host.
//
// Wayland-only, deliberately. A build that silently fell back to XWayland
// would hide the problems this target exists to find, and X11 hosts are the
// VST3 wrapper's job, not this one's.
//
// Two things this file owns that a plugin never does:
//
//   * the event loop. Neither timer framework has a native source here, so
//     the loop's tick drives them - parameter queues, editor animation, and
//     the deferred settings apply all hang off it.
//   * the connection. In a plugin both belong to the host; that is why the
//     backend takes them as constructor arguments rather than creating them.

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <memory>
#include <span>
#include <string>

#include "AudioDriverPipeWire.h"
#include "MidiDriverAlsa.h"

#include "../AppLayout.h"
#include "../MenuBarView.h"
#include "../SettingsPane.h"
#include "../StandaloneHost.h"
#include "../StandaloneSettings.h"
#include "../mcp/CommandDispatcher.h"
#include "../mcp/IpcServer.h"

#include "GmpiUiDrawing.h"
#include "backends/DrawingFrameWayland.h"
#include "helpers/CpuTextEngine.h"
#include "helpers/DecodeImage.h"
#include "helpers/FontProvider.h"
#include "helpers/Timer.h"

namespace
{

// The CPU backend deliberately ships no font or shaping code, so the app wires
// in the three pieces every gmpi_ui host does: fontconfig to find faces,
// freetype to rasterise, harfbuzz to shape. Unlike SynthEdit this does NOT
// bundle a face - a standalone has no reference images to match, and the
// desktop's own UI font is the right thing to look like.
gmpi::drawing::TextFormat wireTextStack(gmpi::wayland::WaylandToplevel& frame,
                                        gmpi::drawing::Factory& facade)
{
    static gmpi::drawing::CpuTextEngine textEngine{ gmpi::drawing::findFont };

    textEngine.imageDecoder             = gmpi::drawing::decodeImageMemory;
    frame.drawingFactory().imageDecoder = gmpi::drawing::decodeImageFile;
    frame.drawingFactory().textEngine   = &textEngine;

    *gmpi::drawing::AccessPtr::put(facade) = &frame.drawingFactory();

    const std::string_view family{ "sans-serif" };
    return facade.createTextFormat(14.0f, std::span{ &family, 1 });
}

// SIGTERM/SIGINT must run the normal shutdown, not the default instant kill.
// The session manager sends SIGTERM at logout, and some compositors crash when
// a client vanishes without disconnecting cleanly - the workaround roundtrip
// lives in ~Connection, which a default-handled signal never reaches.
//
// A handler may only touch lock-free atomics; the loop's tick notices the flag
// and closes the frame from safe context.
std::atomic<bool> terminationRequested{ false };

void onTerminationSignal(int)
{
    terminationRequested = true;
}

} // namespace

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    using namespace gmpi::standalone;

    StandaloneHost host;
    if (!host.init())
    {
        fprintf(stderr, "This binary contains no GMPI plugin (MP_GetFactory returned nothing).\n");
        return 1;
    }

    Settings settings(host.pluginName());
    settings.load();

    gmpi::wayland::Connection connection;
    if (!connection.open())
    {
        fprintf(stderr, "%s: no Wayland display. This build is Wayland-only;\n"
                        "under X11 or a remote session there is nothing to connect to.\n",
                host.pluginName().c_str());
        return 1;
    }

    gmpi::wayland::WaylandToplevel frame(connection);

    gmpi::drawing::Factory facade;
    auto font = wireTextStack(frame, facade);
    if (!font)
        fprintf(stderr, "No usable font was found; text will not draw.\n");

    frame.setMenuFont(gmpi::drawing::AccessPtr::get(font));

    // The plugin's parameter pins take their host from a queryInterface for
    // IEditorHost during setHost. The frame answers the drawing and input
    // interfaces itself and forwards the rest here; without this the first
    // knob drag dereferences null.
    frame.setFallbackHost(host.parameterHost());

    // Size the window from the editor, plus the strip the menu bar occupies.
    float editorWidth = 0.0f;
    float editorHeight = 0.0f;
    host.getEditorSize(editorWidth, editorHeight);

    const int windowWidth  = static_cast<int>(std::ceil(editorWidth));
    const int windowHeight = static_cast<int>(std::ceil(editorHeight + MenuBarView::kHeight));

    if (!frame.create(host.pluginName().c_str(), "com.gmpi.standalone", windowWidth, windowHeight))
    {
        fprintf(stderr, "Could not create a window.\n");
        return 1;
    }

    // A window narrower than the menu bar's titles is not useful, and the
    // client cannot refuse a configure - only the compositor can stop the drag.
    frame.setMinimumSize(320, static_cast<int>(MenuBarView::kHeight) + 80);

    host.setAudioDriver(std::make_unique<AudioDriverPipeWire>());
    if (host.wantsMidiInput())
        host.setMidiDriver(std::make_unique<MidiDriverAlsa>());

    // --- the window's contents ---------------------------------------------
    // Refcounted objects, so each is owned by a shared_ptr rather than by the
    // stack: the frame and the layout keep BORROWED pointers (attachClient
    // does not addRef), and the layout's children outlive nothing.

    gmpi::shared_ptr<AppLayout> layout(new AppLayout());
    gmpi::shared_ptr<MenuBarView> menuBar(new MenuBarView());
    gmpi::shared_ptr<SettingsPane> settingsPane(new SettingsPane(host, settings));

    menuBar->setFont(gmpi::drawing::AccessPtr::get(font));

    layout->setMenuBarHeight(MenuBarView::kHeight);
    layout->setMenuBar(static_cast<gmpi::api::IDrawingClient*>(menuBar.get()));

    const int pageEditor   = layout->addPage(host.editorDrawingClient());
    const int pageSettings = layout->addPage(static_cast<gmpi::api::IDrawingClient*>(settingsPane.get()));

    // --- menus --------------------------------------------------------------
    // Modelled on JUCE's standalone shell, which is what anyone reaching for
    // this has used before: a File menu that quits and an Options menu that
    // reaches the device settings.
    {
        std::vector<MenuBarView::Menu> menus;

        menus.push_back({ "File", {
            { "Quit", [&frame] { frame.close(); } },
        } });

        menus.push_back({ "Options", {
            {
                "Audio/MIDI Settings",
                [&]
                {
                    // Re-read the device lists on the way in: keyboards and
                    // sinks come and go while the app runs.
                    settingsPane->reload();
                    layout->showPage(pageSettings);
                },
                {},
                [&] { return layout->currentPage() == pageSettings; }
            },
            {
                "Plugin Editor",
                [&] { layout->showPage(pageEditor); },
                [&host] { return host.editorDrawingClient() != nullptr; },
                [&] { return layout->currentPage() == pageEditor; }
            },
            {},   // separator
            { "Quit", [&frame] { frame.close(); } },
        } });

        menuBar->setMenus(std::move(menus));
    }

    frame.attachClient(static_cast<gmpi::api::IDrawingClient*>(layout.get()));

    // AFTER attachClient: the plugin's editor is initialised against a host it
    // now has, and initUi pushes every current parameter value into it.
    host.onEditorAttached();

    // --- devices ------------------------------------------------------------
    {
        const auto deviceId = settings.getString(
            Settings::keyAudioDevice, AudioDriverPipeWire::defaultDeviceId());
        const int sampleRate  = settings.getInt(Settings::keySampleRate, 48000);
        const int bufferFrames = settings.getInt(Settings::keyBufferFrames, 512);

        if (!host.startAudio(deviceId, sampleRate, bufferFrames))
        {
            // Not fatal. A standalone that refuses to open its window because
            // the soundcard is busy has removed the only UI that could pick a
            // different one.
            fprintf(stderr, "Audio: %s\n", host.lastError().c_str());

            // Open ON the settings page instead of the plugin's editor, the
            // way JUCE's standalone shell does. Showing a synth GUI that
            // cannot make a sound, with no indication of why, is the worst of
            // the available options; the page names the failure and offers the
            // device list that fixes it.
            settingsPane->reload();
            layout->showPage(pageSettings);
        }

        if (host.wantsMidiInput())
        {
            // An empty list means "connect everything readable", which is what
            // makes a fresh install play the moment a keyboard is plugged in.
            // Only a user who has actually visited the settings page gets the
            // narrower list.
            const auto midiInputs = settings.getBool(Settings::keyMidiInputsSet, false)
                                  ? settings.getStringList(Settings::keyMidiInputs)
                                  : std::vector<std::string>{};

            host.startMidi(midiInputs);
        }
    }

    // --- command channel ----------------------------------------------------
    // A unix socket naming this process, so a test harness or an MCP server can
    // drive the very plugin instance the user is looking at. Failing to open it
    // is not fatal: someone launched this app to make a sound with, and a
    // missing debug channel must never be the reason it will not start.
    gmpi::standalone::mcp::IpcServer ipcServer;
    gmpi::standalone::mcp::AppContext ipcContext;
    {
        ipcContext.host = &host;

        // Pointer coordinates are window-relative, matching the screenshot, so
        // "find the knob in the PNG, then click it" needs no arithmetic. This
        // is what a caller adds to convert a plugin-relative coordinate.
        ipcContext.editorOriginY = MenuBarView::kHeight;

        ipcContext.framePixels = [&frame](bool forceRedraw,
                                          const uint8_t*& pixels, int& w, int& h, int& stride)
        {
            // A screenshot must show the effect of the command before it, so it
            // renders rather than reading a frame that predates the change.
            // present() early-outs before the first configure, so this is safe
            // even if a client connects while the window is still opening.
            if (forceRedraw)
                frame.present();

            const auto& buffer = frame.frameBuffer();
            if (!buffer.pixels())
                return false;

            pixels = buffer.pixels();
            w      = buffer.width();
            h      = buffer.height();
            stride = buffer.stride();
            return true;
        };

        ipcContext.logicalSize = [&frame](float& w, float& h)
        {
            w = static_cast<float>(frame.logicalWidth());
            h = static_cast<float>(frame.logicalHeight());
        };

        // Input enters at the layout, which is what the frame has attached and
        // therefore exactly where the seat delivers a real mouse - so the menu
        // bar, the page switch and the plugin's own widgets all see synthetic
        // events on the same path, with the same capture bookkeeping.
        ipcContext.inputClient = [&layout]() -> gmpi::api::IInputClient*
        {
            gmpi::api::IInputClient* client{};
            layout->queryInterface(&gmpi::api::IInputClient::guid,
                                   reinterpret_cast<void**>(&client));

            // queryInterface addRefs. The layout outlives every command, so the
            // reference is dropped here rather than making each caller own one.
            if (client)
                client->release();

            return client;
        };

        const bool started = ipcServer.start(
            [&ipcContext](const std::string& line)
            {
                return gmpi::standalone::mcp::dispatchCommand(ipcContext, line);
            });

        // Printed rather than silent: it is how you find the socket to point
        // socat at, and its absence is the first thing to check when a client
        // reports no running apps.
        if (started)
            fprintf(stderr, "command channel: %s\n", ipcServer.socketPath().c_str());
        else
            fprintf(stderr, "command channel: unavailable (no writable runtime directory).\n");
    }

    std::signal(SIGTERM, onTerminationSignal);
    std::signal(SIGINT,  onTerminationSignal);

    frame.runEventLoop(16, [&](int elapsedMs)
    {
        if (terminationRequested)
        {
            frame.close();
            return;
        }

        // Commands from the socket. This is the ONLY point at which they run:
        // the listener thread never touches the plugin, it just parks here
        // until we get to it (mcp/MainThreadQueue.h explains why the tick has
        // to be the marshaller on Wayland).
        //
        // BEFORE the timer pump, not after: a --set-param queues the value for
        // the processor, and the pump is what delivers it. Draining first means
        // a parameter set now is audible this tick rather than the next one.
        ipcServer.mainThreadQueue().drain();

        // gmpi_ui's timers have no native source on Linux, so the loop is the
        // source. This is what drives the host's parameter queues and the
        // form widgets' animation.
        gmpi::TimerManager::instance()->pump(elapsedMs);

        // Lets the visible page service its DSP->GUI queue once per frame,
        // rather than polling it from the audio thread's side.
        layout->preGraphicsRedraw();

        // Device changes the user asked for while we were inside input
        // dispatch. Re-opening an audio device there would mean joining the
        // driver's threads with a click still on the stack.
        settingsPane->pumpDeferred();
    });

    // FIRST, and on this thread: stop() refuses further model access before it
    // joins the listener, so no command can still be reaching for the host,
    // the editor or the drivers that the next three lines tear down.
    ipcServer.stop();

    // Stop the audio and MIDI threads before anything they touch goes away.
    host.stopMidi();
    host.stopAudio();

    frame.detachClient();

    return 0;
}
