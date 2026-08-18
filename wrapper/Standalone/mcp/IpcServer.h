#pragma once

// The command channel into the RUNNING standalone app.
//
// INCLUDE THIS ONE on every platform: it selects the transport. Windows uses a
// named pipe (IpcServerWin.h); everything else uses the unix-domain socket
// below. Both present the same gmpi::standalone::mcp::IpcServer - same start /
// stop / mainThreadQueue / channelName - so the per-platform main() differs
// only in the line that prints where it published.

#if defined(_WIN32)

#include "IpcServerWin.h"

#else

// A unix-domain-socket command channel into the RUNNING standalone app.
//
// Serves BOTH unixes: Linux and macOS. The counterpart of
// SynthEdit2/EditorIpcServer.{h,cpp} (Windows named pipe) and
// SynthEditMac/EditorIpcServerMac.h (macOS unix socket). Same newline framing,
// same JSONL responses, same discovery-by-directory-listing, so one MCP client
// drives any of them without knowing which it reached.
//
// THREADING. The listener thread never touches the plugin. It parses a line
// and hands it to MainThreadQueue, which the event loop's tick drains; see
// MainThreadQueue.h for why the tick is the only usable marshaller here.
//
// Two things differ between the two unixes, and both are handled in the compat
// shims below rather than by forking the class:
//
//  * SIGPIPE suppression. Linux has no SO_NOSIGPIPE and spells it per-call as
//    send(MSG_NOSIGNAL); macOS has no MSG_NOSIGNAL and spells it once per
//    socket as setsockopt(SO_NOSIGPIPE). Either way a peer that dies mid-
//    response must not take the app down with it, which the default
//    disposition would.
//
//  * Where the socket goes. $XDG_RUNTIME_DIR is the natural home on Linux: it
//    is already per-user, already mode 0700, on tmpfs, and the session manager
//    clears it at logout so a crashed app cannot leave a corpse across
//    sessions. macOS has no equivalent, so it lands on the /tmp fallback -
//    which is why that path is ownership-checked rather than trusted, and why
//    the MCP client's discovery lists it on every unix (the node server at the
//    repo root, <repo>/mcp/src/discover.ts - not this directory).

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "MainThreadQueue.h"

// ---------------------------------------------------------------------------
// The descriptor setup the two unixes spell differently, and the two plain
// helpers it is built out of.
//
// socketCloExec, acceptCloExec and wakePipe are the ones Linux has atomic forms
// of and macOS does not, so on macOS they do in separate steps what Linux does
// in one - and the RACE that costs is worth naming: between the create and the
// fcntl, a fork+exec on another thread leaks the descriptor into the child.
// This app forks nothing, and the alternative - declining to build on macOS
// over a hazard it does not have - is worse. The atomic form is still used
// wherever the platform has it.
//
// setCloExec and setNonBlocking are those separate steps, with no atomic form
// of their own to prefer. setNonBlocking is also reached on both platforms for
// the listening socket, which socketCloExec() does not ask non-blocking for -
// see start().
// ---------------------------------------------------------------------------

namespace gmpi
{
namespace standalone
{
namespace mcp
{
namespace compat
{

inline void setCloExec(int fd)
{
    if (fd < 0)
        return;
    const int flags = ::fcntl(fd, F_GETFD, 0);
    ::fcntl(fd, F_SETFD, (flags < 0 ? 0 : flags) | FD_CLOEXEC);
}

/// False if `fd` could not be put in non-blocking mode. Unlike FD_CLOEXEC,
/// which only matters to a child this app never spawns, a socket that stays
/// blocking is one the caller has to act on: every descriptor the listener
/// thread waits on has to be non-blocking or that thread can be parked
/// somewhere stop() cannot reach - see acceptCloExec() and start().
inline bool setNonBlocking(int fd)
{
    if (fd < 0)
        return false;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/// A SOCK_STREAM unix socket with close-on-exec set. Also the one place
/// SO_NOSIGPIPE can be applied on macOS - it is a socket option there, not a
/// send() flag - so writeAll() below needs no platform arm of its own.
inline int socketCloExec()
{
#if defined(SOCK_CLOEXEC)
    return ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    setCloExec(fd);
    return fd;
#endif
}

/// A connected client socket, close-on-exec and NON-BLOCKING.
///
/// Non-blocking is not a detail: ONE poll loop serves every client, so a
/// ::send() to a peer that stopped reading would park this thread inside the
/// kernel, where the wake fd cannot reach it and no other client is served.
/// stop() would then join a thread only that peer could release, and the app
/// would hang on exit. writeAll() waits on POLLOUT instead, which is what its
/// EAGAIN arm is for; macOS reaches it soonest, its unix-socket send buffer
/// being smaller than one large response.
///
/// `listenFd` is non-blocking for the same reason (see start()), so a negative
/// return is not necessarily a failure: POLLIN on a listening socket is not a
/// promise that a connection will still be queued by the time we ask for it,
/// and EAGAIN is that case. Both answers mean the same thing to the caller -
/// no new client this round - so neither is distinguished here.
inline int acceptCloExec(int listenFd)
{
#if defined(__linux__)
    const int fd = ::accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
    const int fd = ::accept(listenFd, nullptr, nullptr);
    setCloExec(fd);
    if (fd >= 0 && !setNonBlocking(fd))
    {
        // Refused, and reported the way a failed accept() is: a client socket
        // left blocking is the one that would park the shared thread inside
        // ::send(), so losing this one connection is what keeps the rest served.
        ::close(fd);
        return -1;
    }
#endif

#if defined(SO_NOSIGPIPE)
    // Per-socket, because macOS has no MSG_NOSIGNAL to spell it per-call.
    // Set on the ACCEPTED socket rather than the listening one: the option is
    // not inherited, and it is the accepted socket that responses go out on.
    if (fd >= 0)
    {
        const int on = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    }
#endif
    return fd;
}

/// The self-pipe stop() writes to. Non-blocking on both ends so a stop() that
/// races an earlier unread wake byte cannot block the main thread.
inline bool wakePipe(int fds[2])
{
#if defined(__linux__)
    return ::pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0;
#else
    if (::pipe(fds) != 0)
        return false;
    for (int i = 0; i < 2; ++i)
    {
        setCloExec(fds[i]);
        const int fl = ::fcntl(fds[i], F_GETFL, 0);
        ::fcntl(fds[i], F_SETFL, (fl < 0 ? 0 : fl) | O_NONBLOCK);
    }
    return true;
#endif
}

/// MSG_NOSIGNAL where it exists; 0 where SO_NOSIGPIPE did the job at accept().
#if defined(MSG_NOSIGNAL)
inline constexpr int kSendFlags = MSG_NOSIGNAL;
#else
inline constexpr int kSendFlags = 0;
#endif

} // namespace compat
} // namespace mcp
} // namespace standalone
} // namespace gmpi

namespace gmpi
{
namespace standalone
{
namespace mcp
{

/// Every published channel is `<dir>/gmpi-standalone.<pid>`. The client's
/// discovery is a directory listing plus a pid-liveness check, so there is no
/// registry, config file or port to keep in sync - exactly as SynthEdit does it.
inline constexpr std::string_view kSocketPrefix = "gmpi-standalone.";

/// Creates `dir` (and parents) mode 0700, and returns true only if what ends up
/// there is a directory we own. Refusing an unexpected owner or a symlink is
/// what makes the /tmp fallback safe to use at all.
inline bool ensurePrivateDir(const std::string& dir)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);   // ignore ec: the lstat below is the real test

    struct stat st{};
    if (::lstat(dir.c_str(), &st) != 0)
        return false;
    if (!S_ISDIR(st.st_mode))
        return false;                                // symlink or file squatting on the name
    if (st.st_uid != ::getuid())
        return false;

    ::chmod(dir.c_str(), 0700);
    return true;
}

/// Where this process publishes its channel, or empty if nowhere will have it.
///
/// sun_path is 108 bytes on Linux and 104 on macOS, and an over-long path binds
/// to a SILENTLY TRUNCATED name - two apps would collide invisibly. So a path
/// that does not fit is refused, never truncated, and the next candidate is
/// tried. sizeof(sun_path) is read from the struct rather than assumed, so the
/// shorter macOS limit needs no arm of its own.
inline std::string chooseSocketPath()
{
    const std::string leaf = std::string(kSocketPrefix) + std::to_string(static_cast<long>(::getpid()));

    std::vector<std::string> candidates;
    if (const char* override_ = ::getenv("GMPI_STANDALONE_IPC_DIR"))
    {
        candidates.emplace_back(override_);
    }
    else
    {
        if (const char* runtime = ::getenv("XDG_RUNTIME_DIR"))
            candidates.emplace_back(std::string(runtime) + "/gmpi-standalone");

        // Ordinary on a bare ssh session or a container, where the pam module
        // that creates XDG_RUNTIME_DIR never ran. World-writable ground, hence
        // the ownership check in ensurePrivateDir().
        candidates.emplace_back("/tmp/gmpi-standalone." + std::to_string(static_cast<long>(::getuid())));
    }

    sockaddr_un probe{};
    for (const auto& dir : candidates)
    {
        const std::string full = (std::filesystem::path(dir) / leaf).string();
        if (full.size() >= sizeof(probe.sun_path))
            continue;
        if (!ensurePrivateDir(dir))
            continue;
        return full;
    }
    return {};
}

class IpcServer
{
public:
    /// Handles ONE command line and returns the response line. Invoked on the
    /// main thread via the queue, so it may touch anything the app owns.
    using CommandHandler = std::function<std::string(const std::string& line)>;

    ~IpcServer() { stop(); }

    IpcServer() = default;
    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    /// Drained once per event-loop tick by the app. This is what actually runs
    /// the commands.
    MainThreadQueue& mainThreadQueue() { return queue_; }

    /// Begins listening. Reports false and leaves the app running normally when
    /// the socket cannot be created - a missing command channel must never be
    /// fatal to an app the user launched to make sound with.
    bool start(CommandHandler handler)
    {
        if (running_)
            return true;
        if (!handler)
            return false;

        socketPath_ = chooseSocketPath();
        if (socketPath_.empty())
            return false;

        // A named pipe evaporates with its process; a socket file does not, and
        // bind() over a leftover one fails EADDRINUSE even with nobody
        // listening. The name carries our own pid, so anything already here is
        // a corpse left by a previous process that happened to hold it.
        ::unlink(socketPath_.c_str());

        listenFd_ = compat::socketCloExec();
        if (listenFd_ < 0)
        {
            socketPath_.clear();
            return false;
        }

        // Non-blocking for the same reason the accepted sockets are: the ONE
        // thread that polls is also the one that accepts, and POLLIN on a
        // listening socket is a report, not a promise - a peer that connects
        // and aborts before we get there leaves the queue empty again. On a
        // blocking fd that ::accept() then parks the thread in the kernel,
        // where the wake fd cannot reach it and no existing client is served
        // either. Rare on AF_UNIX, and free to rule out.
        //
        // Refusing to start is right rather than carrying on regardless: this
        // is the reason the rest of the loop can be trusted, and the caller
        // already treats a channel that will not open as non-fatal. Nothing is
        // bound yet, so there is no socket file to unlink on the way out.
        if (!compat::setNonBlocking(listenFd_))
        {
            cleanupFds();
            socketPath_.clear();
            return false;
        }

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (socketPath_.size() >= sizeof(addr.sun_path))   // checked again: never truncate
        {
            cleanupFds();
            socketPath_.clear();
            return false;
        }
        memcpy(addr.sun_path, socketPath_.c_str(), socketPath_.size() + 1);

        // The mode is taken from the umask at bind() time - chmod afterwards
        // leaves a window, and fchmod on a socket fd does not affect the inode.
        // Without this the default umask yields srwxr-xr-x and any local user
        // could drive the app.
        const mode_t oldMask = ::umask(0077);
        const int bound = ::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::umask(oldMask);

        if (bound != 0 || ::listen(listenFd_, 4) != 0)
        {
            cleanupFds();
            ::unlink(socketPath_.c_str());
            socketPath_.clear();
            return false;
        }

        if (!compat::wakePipe(wakeFd_))
        {
            cleanupFds();
            ::unlink(socketPath_.c_str());
            socketPath_.clear();
            return false;
        }

        handler_  = std::move(handler);
        stopping_ = false;
        running_  = true;
        thread_   = std::thread([this] { threadMain(); });
        return true;
    }

    /// Idempotent. MUST be called on the MAIN thread and BEFORE the plugin,
    /// editor or drivers are torn down: the queue shutdown inside it is what
    /// guarantees no handler can still be reaching for them.
    void stop()
    {
        if (!running_ && !thread_.joinable())
            return;

        // Order matters, as on both other platforms. Refuse further model
        // access FIRST, so a listener already blocked in run() is released with
        // an error rather than waiting on a main thread that will never tick
        // again; then wake the listener; and only then join it. Signalling
        // before joining is what stops the two threads waiting on each other.
        queue_.shutdown();

        stopping_ = true;
        if (wakeFd_[1] >= 0)
        {
            const char b = 1;
            // A full pipe means an earlier wake byte is still unread, which
            // wakes the listener just as well - so a short write is success
            // here, not something to retry.
            const ssize_t written = ::write(wakeFd_[1], &b, 1);
            (void)written;
        }

        if (thread_.joinable())
            thread_.join();

        // After the join, so the fds are never yanked from under a polling
        // listener thread.
        cleanupFds();

        if (!socketPath_.empty())
        {
            ::unlink(socketPath_.c_str());
            socketPath_.clear();
        }

        running_ = false;
        handler_ = nullptr;
    }

    bool running() const { return running_; }

    /// e.g. "/run/user/1000/gmpi-standalone/gmpi-standalone.10673".
    /// Empty until start() succeeds. Named for what it IS to a caller - the
    /// address this app published - rather than for the transport, so the two
    /// implementations can be swapped without the app noticing.
    const std::string& channelName() const { return socketPath_; }

private:
    /// Connections served at once. Small: this is a command channel, not a
    /// server. The cap exists so a client that leaks connections cannot eat the
    /// app's file descriptors.
    static constexpr size_t kMaxClients = 8;

    void cleanupFds()
    {
        for (int* fd : { &wakeFd_[0], &wakeFd_[1], &listenFd_ })
        {
            if (*fd >= 0)
            {
                ::close(*fd);
                *fd = -1;
            }
        }
    }

    /// True if the fd became ready; false if stop() woke us. Every blocking
    /// wait on this thread goes through here: `stopping_` alone is not enough,
    /// because a thread parked in poll() never re-checks a flag.
    bool waitReady(int fd, short events)
    {
        pollfd p[2] = { { fd, events, 0 }, { wakeFd_[0], POLLIN, 0 } };
        while (!stopping_)
        {
            const int n = ::poll(p, 2, -1);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                return false;
            }
            if (p[1].revents)
                return false;
            if (p[0].revents)
                return true;
        }
        return false;
    }

    struct Client
    {
        int fd = -1;
        std::string inbox;
    };

    /// Accepts and serves every client from ONE poll loop.
    ///
    /// Deliberately not "serve one connection to completion, then the next":
    /// a connected-but-silent client would then hold the channel forever and
    /// every later client would hang in the listen backlog with no diagnostic.
    /// An orphaned MCP server process is an ordinary thing to find on a
    /// developer's machine, and it made SynthEdit look wedged when it was
    /// perfectly idle.
    ///
    /// Commands are still globally serialised, because dispatch happens inline
    /// on this thread: one plugin, one command at a time. What this buys is
    /// that an IDLE client costs nothing.
    void threadMain()
    {
        std::vector<Client> clients;
        std::vector<pollfd> fds;
        char buf[4096];

        while (!stopping_)
        {
            fds.clear();
            fds.push_back({ listenFd_,  POLLIN, 0 });
            fds.push_back({ wakeFd_[0], POLLIN, 0 });
            for (const auto& c : clients)
                fds.push_back({ c.fd, POLLIN, 0 });

            const int n = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), -1);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                break;
            }

            if (fds[1].revents)             // stop() woke us
                break;

            if (fds[0].revents & POLLIN)
            {
                // A negative covers both "the pending connection went away
                // before we asked" (EAGAIN - the listen fd is non-blocking) and
                // a genuine accept failure. Neither is worth abandoning the
                // clients already being served, so both just fall through to
                // the next poll.
                const int fd = compat::acceptCloExec(listenFd_);
                if (fd >= 0)
                {
                    if (clients.size() >= kMaxClients)
                        ::close(fd);        // something is leaking connections
                    else
                        clients.push_back(Client{ fd, {} });
                }
            }

            // Index by fd rather than by position: serving one client can
            // dispatch a command that takes a while, and `fds` describes a
            // moment that has passed by the time we reach the next entry.
            for (size_t i = 2; i < fds.size() && !stopping_; ++i)
            {
                if (!fds[i].revents)
                    continue;

                const int fd = fds[i].fd;
                auto it = std::find_if(clients.begin(), clients.end(),
                                       [fd](const Client& c) { return c.fd == fd; });
                if (it == clients.end())
                    continue;

                if (!serviceClient(*it, buf, sizeof(buf)))
                {
                    ::close(it->fd);
                    clients.erase(it);
                }
            }
        }

        for (const auto& c : clients)
            ::close(c.fd);
    }

    /// Reads what is available and runs any COMPLETE lines. Returns false when
    /// the client is finished with (disconnected, or errored).
    bool serviceClient(Client& client, char* buf, size_t bufSize)
    {
        // The socket is non-blocking, so a poll() that reported POLLIN can still
        // turn up nothing here: that is a quiet client, not a gone one.
        const ssize_t got = ::read(client.fd, buf, bufSize);
        if (got < 0)
            return errno == EINTR || errno == EAGAIN;   // nothing yet; keep it
        if (got == 0)
            return false;                               // client gone

        client.inbox.append(buf, static_cast<size_t>(got));

        // Newline-framed, matching SynthEditCL's `--script -` grammar and both
        // other transports, so one client implementation drives every one.
        size_t nl;
        while ((nl = client.inbox.find('\n')) != std::string::npos)
        {
            std::string line = client.inbox.substr(0, nl);
            client.inbox.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;

            // Blocks until the main thread's next tick runs it.
            std::string response = queue_.run([this, line] { return handler_(line); });
            response += '\n';
            if (!writeAll(client.fd, response))
                return false;
        }
        return true;
    }

    /// Never blocks UNINTERRUPTIBLY on a client that stopped reading, which
    /// rests on the socket being non-blocking (compat::acceptCloExec): a full
    /// send buffer surfaces here as EAGAIN, so the wait for room is a poll()
    /// that also watches the wake fd - on a blocking socket ::send would park
    /// in the kernel where stop() could never reach it.
    ///
    /// That wait carries no deadline, as the pipe's waits in IpcServerWin.h
    /// carry none: a peer that never drains holds this thread, and with it
    /// every other client, until it drains or stop() wakes us. Bounding it on
    /// the unixes alone would close a client mid-response that the pipe keeps
    /// serving, which the MCP client reports as the app having crashed (the
    /// node server at the repo root, <repo>/mcp/src/session.ts - not this
    /// directory) - a bound has to arrive on both transports or neither.
    ///
    /// A peer that died mid-response must not raise SIGPIPE and take the app
    /// down with it: on Linux that is compat::kSendFlags carrying MSG_NOSIGNAL
    /// here, on macOS it is the SO_NOSIGPIPE that compat::acceptCloExec already
    /// set on this socket.
    bool writeAll(int fd, const std::string& data)
    {
        size_t sent = 0;
        while (sent < data.size())
        {
            const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, compat::kSendFlags);
            if (n > 0)
            {
                sent += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                if (!waitReady(fd, POLLOUT))
                    return false;
                continue;
            }
            return false;   // EPIPE and friends: the client is gone
        }
        return true;
    }

    MainThreadQueue queue_;
    CommandHandler handler_;
    std::thread thread_;
    std::string socketPath_;

    int listenFd_ = -1;
    int wakeFd_[2] = { -1, -1 };   // self-pipe: the stopEvent_ analogue

    std::atomic<bool> running_{ false };
    std::atomic<bool> stopping_{ false };
};

} // namespace mcp
} // namespace standalone
} // namespace gmpi

#endif // !_WIN32
