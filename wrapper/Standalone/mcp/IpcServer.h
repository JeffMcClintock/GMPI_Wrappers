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
// The Linux counterpart of SynthEdit2/EditorIpcServer.{h,cpp} (Windows named
// pipe) and SynthEditMac/EditorIpcServerMac.h (macOS unix socket). Same
// newline framing, same JSONL responses, same discovery-by-directory-listing,
// so one MCP client drives any of them without knowing which it reached.
//
// THREADING. The listener thread never touches the plugin. It parses a line
// and hands it to MainThreadQueue, which the event loop's tick drains; see
// MainThreadQueue.h for why the tick is the only usable marshaller here.
//
// Two Linux differences from the macOS server worth naming:
//
//  * There is no SO_NOSIGPIPE. A dead peer would raise SIGPIPE and the default
//    disposition takes the app down mid-session, so every response goes out
//    through send(MSG_NOSIGNAL) rather than write(). Per-call, not per-socket,
//    which is why it is spelled at each site instead of once at accept().
//
//  * The natural home for the socket is $XDG_RUNTIME_DIR: it is already
//    per-user, already mode 0700, on tmpfs, and the session manager clears it
//    at logout so a crashed app cannot leave a corpse across sessions. macOS
//    has no equivalent and falls back to the settings folder; here that is the
//    first choice rather than the fallback.

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
/// sun_path is 108 bytes on Linux and an over-long path binds to a SILENTLY
/// TRUNCATED name - two apps would collide invisibly. So a path that does not
/// fit is refused, never truncated, and the next candidate is tried.
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

        listenFd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listenFd_ < 0)
        {
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

        if (::pipe2(wakeFd_, O_CLOEXEC | O_NONBLOCK) != 0)
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
                const int fd = ::accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
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

    /// Never blocks indefinitely on a client that stopped reading - the wake fd
    /// is in every wait. MSG_NOSIGNAL because a peer that died mid-response
    /// would otherwise raise SIGPIPE and take the app down with it.
    bool writeAll(int fd, const std::string& data)
    {
        size_t sent = 0;
        while (sent < data.size())
        {
            const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
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
