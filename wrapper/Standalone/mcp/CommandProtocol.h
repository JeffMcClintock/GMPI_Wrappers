#pragma once

// The command channel's WIRE FORMAT, and the state a transport keeps around it.
//
// ONE copy, shared by the unix socket in IpcServer.h and the named pipe in
// IpcServerWin.h. Those two were the same framing loop written twice - append,
// find a newline, strip a '\r', skip an empty line, dispatch, terminate the
// answer - which is a format free to drift on one platform and not the other,
// and then a client that works against only one of them.
//
// NO PLATFORM HEADER MAY REACH THIS FILE. It sits between two transports that
// pull in mutually exclusive worlds - a bare <windows.h> on one side, sockets
// and <poll.h> on the other - and it is included by both, so anything it
// dragged in would arrive in an arm with no business seeing it. The standard
// library and MainThreadQueue.h, and nothing else.
//
// The format is not ours alone to change: the MCP client at
// <repo>/mcp/src/session.ts is the external consumer of every rule below, and
// SynthEditCL's `--script -` grammar is where they come from - so one client
// implementation drives a standalone, an editor, or the command-line tool
// without knowing which it reached.

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>

#include "MainThreadQueue.h"

namespace gmpi
{
namespace standalone
{
namespace mcp
{

/// Handles ONE command line and returns the response line. Invoked on the main
/// thread via MainThreadQueue, so it may touch anything the app owns.
using CommandHandler = std::function<std::string(const std::string& line)>;

/// One connection's half-received bytes, and the rule for turning them into
/// command lines.
///
/// PER CONNECTION, not per server. A read returns whatever happened to have
/// arrived, which can stop in the middle of a line or carry several at once, so
/// every peer needs its own place to keep the tail. The unix transport holds
/// one of these per Client in its single poll loop; the pipe transport has one
/// on each client thread's stack.
class LineFramer
{
public:
    /// Appends `count` bytes and runs `dispatch` on every COMPLETE line they
    /// finish, in arrival order. Returns false once `dispatch` has - which both
    /// transports mean as "this peer is finished with", so the lines still in
    /// the inbox are abandoned along with it rather than answered into a socket
    /// nobody is reading.
    ///
    /// The three rules a client depends on, and together they are the whole
    /// format:
    ///
    ///  * '\n' ends a line, and only '\n'. Anything after the last one is kept
    ///    here until the newline that finishes it arrives, however many reads
    ///    later - a 20KB command line spans several.
    ///
    ///  * ONE trailing '\r' is dropped, so a client writing CRLF reaches the
    ///    same server as one writing LF. Only the last one: a '\r' anywhere
    ///    else in the line is a byte of the command.
    ///
    ///  * An empty line is skipped SILENTLY - no response, not even an error.
    ///    A blank costs nothing to send: it is what a hand-typed socat session
    ///    produces on a stray return, and what a caller with an empty string in
    ///    its batch produces. An answer to it would cost something, because
    ///    session.ts pairs answers to a sentinel rather than counting them, so
    ///    a line nobody asked for is not ignored there - it is reported as a
    ///    result.
    template <class Dispatch>
    bool feed(const char* bytes, size_t count, Dispatch&& dispatch)
    {
        inbox_.append(bytes, count);

        size_t nl;
        while ((nl = inbox_.find('\n')) != std::string::npos)
        {
            std::string line = inbox_.substr(0, nl);
            inbox_.erase(0, nl + 1);

            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (line.empty())
                continue;

            if (!dispatch(line))
                return false;
        }

        return true;
    }

private:
    std::string inbox_;
};

/// What a transport keeps that is neither socket nor pipe: the marshaller onto
/// the main thread, the handler it marshals to, and the two flags start() and
/// stop() move.
///
/// COMPOSED, not inherited. A base class owning start() and stop() outright
/// looked tempting and is not: most of both is platform work in a platform
/// order - a socket bound before it listens, a pipe instance created before the
/// thread that waits on it exists - and a base would have to hand every one of
/// those steps back through a hook, which is the same code with an indirection
/// added. What the two genuinely share is the state below, the three lines that
/// arm it, the shutdown ORDER, and the wire format above.
class ChannelCore
{
public:
    /// Drained once per event-loop tick by the app. This is what actually runs
    /// the commands.
    MainThreadQueue& mainThreadQueue() { return queue_; }

    bool running() const { return running_; }

    /// Set for the whole of stop(), and read by every wait on every transport
    /// thread. Atomic because those threads read it while the main thread sets
    /// it - and not sufficient on its own, because a thread parked in poll() or
    /// WaitForMultipleObjects never re-checks a flag. That is what each
    /// transport's own wake handle is for.
    bool stopping() const { return stopping_; }

    /// Runs one command line on the main thread and returns the RESPONSE LINE:
    /// the handler's answer with the terminator appended, ready to go out on
    /// the wire exactly as it stands.
    ///
    /// Called on a transport thread, and blocks there until the app's next tick
    /// runs it. MainThreadQueue is what makes that safe, and what fails the
    /// wait rather than hanging it when there will be no next tick.
    std::string respondTo(const std::string& line)
    {
        std::string response = queue_.run([this, line] { return handler_(line); });
        response += '\n';
        return response;
    }

    /// The last thing start() does, once the transport is up and only if it is:
    /// from here a listener thread may run, and it will find a handler to call
    /// and a clear `stopping_` the moment it looks. Reached only from a stopped
    /// server, since both transports return early while running().
    void arm(CommandHandler handler)
    {
        handler_  = std::move(handler);
        stopping_ = false;
        running_  = true;
    }

    /// The FIRST thing stop() does, before it wakes or joins anything. The
    /// order is load-bearing, and is the same on all three platforms:
    ///
    ///   1. refuse further model access (here), so a transport thread already
    ///      blocked in MainThreadQueue::run is released with an error instead
    ///      of waiting on a main thread that will never tick again;
    ///   2. wake the threads, through whatever handle the transport owns;
    ///   3. only then join them.
    ///
    /// Signalling before joining is what stops the two sides waiting on each
    /// other. Joining first is not a tidier shutdown, it is the deadlock.
    void beginStop()
    {
        queue_.shutdown();
        stopping_ = true;
    }

    /// The last thing stop() does, after every transport thread is joined.
    ///
    /// The handler is released HERE rather than alongside the shutdown above
    /// because it is what those threads were calling: dropping it while one
    /// could still reach it would free the app's lambda out from under a
    /// command in flight.
    void endStop()
    {
        running_ = false;
        handler_ = nullptr;
    }

private:
    MainThreadQueue queue_;
    CommandHandler  handler_;

    std::atomic<bool> running_{ false };
    std::atomic<bool> stopping_{ false };
};

} // namespace mcp
} // namespace standalone
} // namespace gmpi
