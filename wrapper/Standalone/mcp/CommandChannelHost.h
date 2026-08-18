#pragma once

// The command channel, as the app's one startup sequence sees it.
//
// This class exists for a BUILD reason rather than a design one, and the reason
// is worth stating plainly. StandaloneApp.cpp is in the PORTABLE source list,
// and IpcServer.h selects a per-platform transport: on Windows that is a named
// pipe, and IpcServerWin.h pulls a bare <windows.h> in behind it - no NOMINMAX,
// no lean-and-mean. A portable translation unit cannot include that and stay
// portable. So the handful of lines that touch the concrete server live here
// instead, in a file the CMakeLists appends per arm beside the transport
// itself, and the app's sequence never names mcp::IpcServer at all.
//
// It earns its keep twice over. The AppContext wiring and the "say where it
// published" line are then compiled only in a build that HAS a channel, so the
// sequence carries no #if of its own: with the channel off every method below
// is an inline no-op that costs nothing and says nothing, and runStandaloneApp
// reads exactly the same either way. A conditional in the one remaining copy of
// the startup sequence is precisely the sort of thing this change exists to
// remove.

#include <memory>

#include "../CommandChannel.h"

namespace gmpi
{
namespace standalone
{

class AppLayout;
class PlatformShell;
class StandaloneHost;

namespace mcp
{

class CommandChannelHost
{
public:
#if GMPI_STANDALONE_COMMAND_CHANNEL

    CommandChannelHost();

    // Out of line, along with the constructor, because Impl is only complete in
    // the .cpp - which is the whole point of holding it behind a pointer.
    ~CommandChannelHost();

    CommandChannelHost(const CommandChannelHost&) = delete;
    CommandChannelHost& operator=(const CommandChannelHost&) = delete;

    // Wires the dispatcher's view of the app, begins listening, and reports
    // through the shell where it published - or that it could not.
    //
    // Reported rather than silent, on EVERY platform: it is how you find the
    // endpoint to point socat at, and its absence is the first thing to check
    // when a client reports no running apps. The Windows shell used to discard
    // this result entirely, which is why README.md's claim that "the app prints
    // its address at startup" was true on only two of the three.
    //
    // Failing to open the endpoint is never fatal: someone launched this app to
    // make a sound with, and a missing debug channel must not be the reason it
    // will not start.
    void start(StandaloneHost& host, AppLayout& layout, PlatformShell& shell);

    // Runs every command that arrived since the last tick, ON THE APP'S THREAD.
    // The listener thread never touches the plugin; it parks a line here and
    // this is the only place one is ever executed.
    void drain();

    // Refuses further model access and joins the transport's threads. Called
    // FIRST in the teardown, so nothing can still be reaching for the host, the
    // editor or the drivers that the lines after it tear down.
    void stop();

private:
    // The server and the AppContext whose lambdas reach back into the shell.
    // Named nowhere in this header, which is the constraint the file is for.
    struct Impl;
    std::unique_ptr<Impl> impl_;

#else

    // No mcp/ implementation is compiled in this build - this header is the
    // only piece of the directory that survives - so this is the whole class.
    void start(StandaloneHost&, AppLayout&, PlatformShell&) {}
    void drain() {}
    void stop() {}

#endif
};

} // namespace mcp
} // namespace standalone
} // namespace gmpi
