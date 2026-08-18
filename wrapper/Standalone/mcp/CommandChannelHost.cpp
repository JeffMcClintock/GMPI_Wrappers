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

    // The two questions only a platform can answer. Where the pixels come from
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

    // The unix transports used to name the likely cause here ("no writable
    // runtime directory"). Neither IpcServer records a reason, and this line is
    // now shared with a platform where that cause does not exist, so it says
    // what is true of all three. Restoring the detail wants an
    // IpcServer::lastError() on both transports.
    shell.reportStatus(started
        ? "command channel: " + impl_->server.channelName()
        : std::string("command channel: unavailable (the endpoint could not be created)."));
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
