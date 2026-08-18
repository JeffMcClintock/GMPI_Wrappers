#include "CommandChannelHost.h"

#if GMPI_STANDALONE_COMMAND_CHANNEL

#include <cstdint>
#include <string>

// The portable half first, and the transport last: IpcServer.h is what brings
// <windows.h> into this translation unit, and nothing above it wants to be
// compiled underneath that.
#include "../AppLayout.h"
#include "../MenuBarView.h"
#include "../StandaloneApp.h"
#include "../StandaloneHost.h"

#include "CommandDispatcher.h"
#include "IpcServer.h"

namespace gmpi
{
namespace standalone
{
namespace mcp
{

struct CommandChannelHost::Impl
{
    IpcServer  server;
    AppContext context;
};

CommandChannelHost::CommandChannelHost() : impl_(std::make_unique<Impl>()) {}

CommandChannelHost::~CommandChannelHost() = default;

void CommandChannelHost::start(StandaloneHost& host, AppLayout& layout, PlatformShell& shell)
{
    auto& context = impl_->context;

    context.host = &host;

    // Pointer coordinates are window-relative, matching the screenshot, so
    // "find the knob in the PNG, then click it" needs no arithmetic. This is
    // what a caller adds to convert a plugin-relative coordinate.
    context.editorOriginY = MenuBarView::kHeight;

    // The three questions only a platform can answer. Where the pixels come from
    // differs completely between the three - an shm buffer, a swap-chain
    // readback, a second draw of an NSView - and none of that is visible from
    // here, which is the point of routing them through the shell.
    context.framePixels = [&shell](bool forceRedraw,
                                   const uint8_t*& pixels, int& w, int& h, int& stride)
    {
        return shell.framePixels(forceRedraw, pixels, w, h, stride);
    };

    context.logicalSize = [&shell](float& w, float& h)
    {
        shell.logicalSize(w, h);
    };

    // The window's two measurements are asked as two questions, never derived
    // from a frame: a shell whose capture bitmap is empty until a screenshot has
    // been taken still knows perfectly well how big its window is.
    context.canvasSize = [&shell](int& w, int& h)
    {
        shell.canvasSize(w, h);
    };

    // Input enters at the layout, which is what the frame has attached and
    // therefore exactly where a real mouse arrives - so the menu bar, the page
    // switch and the plugin's own widgets all see synthetic events on the same
    // path, with the same capture bookkeeping.
    context.inputClient = [&layout]() -> gmpi::api::IInputClient*
    {
        gmpi::api::IInputClient* client{};
        layout.queryInterface(&gmpi::api::IInputClient::guid,
                              reinterpret_cast<void**>(&client));

        // queryInterface addRefs. The layout outlives every command, so the
        // reference is dropped here rather than making each caller own one.
        if (client)
            client->release();

        return client;
    };

    const bool started = impl_->server.start(
        [this](const std::string& line)
        {
            return dispatchCommand(impl_->context, line);
        });

    // The reason comes from the transport rather than from here. This line is
    // shared by a unix socket and a named pipe, whose failures have nothing in
    // common - a runtime directory that is unset or unwritable on one, a pipe
    // the system refused on the other - so the only honest thing this file can
    // say by itself is that it did not work.
    if (started)
    {
        shell.reportStatus("command channel: " + impl_->server.channelName());
    }
    else
    {
        const std::string reason = impl_->server.lastError();
        shell.reportStatus("command channel: unavailable"
            + (reason.empty() ? std::string(".") : " (" + reason + ")."));
    }
}

void CommandChannelHost::drain()
{
    impl_->server.mainThreadQueue().drain();
}

void CommandChannelHost::stop()
{
    impl_->server.stop();
}

} // namespace mcp
} // namespace standalone
} // namespace gmpi

#endif // GMPI_STANDALONE_COMMAND_CHANNEL
