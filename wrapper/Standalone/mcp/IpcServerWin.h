#pragma once

// A named-pipe command channel into the RUNNING standalone app.
//
// The Windows counterpart of IpcServer.h's unix socket (Linux) and of
// SynthEditMac's; include "IpcServer.h", which selects between them. Same
// newline framing, same JSONL responses, so one MCP client drives any of them
// without knowing which it reached.
//
// THREADING, unchanged from the other two: no thread here ever touches the
// plugin. A line is parsed on a pipe thread and handed to MainThreadQueue,
// which the app's tick drains; see MainThreadQueue.h.
//
// Three Windows differences from the unix-socket server worth naming:
//
//  * There is no path, so there is no directory to make private and no stale
//    file to unlink. A named pipe is a kernel object that evaporates with its
//    last handle, which removes the whole class of "a corpse from a previous
//    run is holding the name" problems the unix version has to handle.
//
//  * Discovery is an enumeration of \\.\pipe\ rather than a directory listing,
//    which is the same shape of operation - the client still finds apps by
//    looking for the "gmpi-standalone." prefix and checking the pid is alive.
//
//  * Access control comes from the pipe's default security descriptor (the
//    creating user gets full control; Everyone gets read, which is not enough
//    to send a command) plus PIPE_REJECT_REMOTE_CLIENTS. The unix version has
//    to build the same guarantee out of a 0700 directory and an ownership
//    check, because a socket sits in the filesystem where anyone can reach it.
//
// CONCURRENCY. One listener thread plus one thread per connected client, and
// the listener re-arms a fresh pipe instance the moment it hands one off. That
// matters for the reason the Linux server's poll loop exists: serving one
// connection to completion would let a connected-but-silent client hold the
// channel forever, and an orphaned MCP server process is an ordinary thing to
// find on a developer's machine. Commands are still globally serialised -
// every one of them goes through MainThreadQueue, which runs them one at a
// time on the app's thread - so what this buys is only that an IDLE client
// costs nothing.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>

#include "MainThreadQueue.h"

namespace gmpi
{
namespace standalone
{
namespace mcp
{

/// Every published channel is `\\.\pipe\gmpi-standalone.<pid>`. Deliberately
/// the same leaf the unix server uses for its socket file, so the client's
/// prefix match and pid-liveness check are one implementation.
inline constexpr std::wstring_view kPipePrefix = L"gmpi-standalone.";

class IpcServer
{
public:
    /// Handles ONE command line and returns the response line. Invoked on the
    /// main thread via the queue, so it may touch anything the app owns.
    using CommandHandler = std::function<std::string(const std::string& line)>;

    IpcServer() = default;
    ~IpcServer() { stop(); }

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    /// Drained once per tick by the app. This is what actually runs commands.
    MainThreadQueue& mainThreadQueue() { return queue_; }

    /// Begins listening. Reports false and leaves the app running normally when
    /// the pipe cannot be created - a missing command channel must never be
    /// fatal to an app the user launched to make sound with.
    bool start(CommandHandler handler)
    {
        if (running_)
            return true;
        if (!handler)
            return false;

        pipeName_ = L"\\\\.\\pipe\\" + std::wstring(kPipePrefix)
                  + std::to_wstring(::GetCurrentProcessId());

        // Proven before the listener starts, so a failure is reported to the
        // caller rather than discovered on a thread that has nowhere to report
        // it. The instance is not wasted - the listener takes it as its first.
        HANDLE first = createInstance();
        if (first == INVALID_HANDLE_VALUE)
        {
            pipeName_.clear();
            return false;
        }

        stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual reset
        if (!stopEvent_)
        {
            ::CloseHandle(first);
            pipeName_.clear();
            return false;
        }

        channelName_ = narrow(pipeName_);
        handler_  = std::move(handler);
        stopping_ = false;
        running_  = true;
        listener_ = std::thread([this, first] { listenerMain(first); });
        return true;
    }

    /// Idempotent. MUST be called on the MAIN thread and BEFORE the plugin,
    /// editor or drivers are torn down: the queue shutdown inside it is what
    /// guarantees no handler can still be reaching for them.
    void stop()
    {
        if (!running_ && !listener_.joinable())
            return;

        // Order matters, as on the other platforms. Refuse further model access
        // FIRST, so a client thread already blocked in run() is released with
        // an error rather than waiting on a main thread that will never tick
        // again; then wake every thread; and only then join. Signalling before
        // joining is what stops the two sides waiting on each other.
        queue_.shutdown();

        stopping_ = true;
        if (stopEvent_)
            ::SetEvent(stopEvent_);

        if (listener_.joinable())
            listener_.join();

        // The listener has stopped spawning by now, so this list is stable.
        // Every client thread is joined, not detached: that join is what proves
        // no handler is still in flight when this returns, which is the one
        // guarantee the caller tears the plugin down on.
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            for (auto& slot : clients_)
            {
                if (slot->thread.joinable())
                    slot->thread.join();
            }
            clients_.clear();
        }

        if (stopEvent_)
        {
            ::CloseHandle(stopEvent_);
            stopEvent_ = {};
        }

        running_ = false;
        handler_ = nullptr;
        pipeName_.clear();
        channelName_.clear();
    }

    bool running() const { return running_; }

    /// e.g. "\\.\pipe\gmpi-standalone.10673". Empty until start() succeeds.
    const std::string& channelName() const { return channelName_; }

private:
    /// Connections served at once. Small: this is a command channel, not a
    /// server. The cap is enforced by the pipe itself (nMaxInstances), so a
    /// client that leaks connections is refused by the kernel rather than
    /// growing this process a thread at a time.
    static constexpr DWORD kMaxClients = 8;

    static std::string narrow(const std::wstring& text)
    {
        if (text.empty())
            return {};

        const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                               nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(size), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                              out.data(), size, nullptr, nullptr);
        return out;
    }

    HANDLE createInstance() const
    {
        // FILE_FLAG_OVERLAPPED throughout: every blocking wait in this file is
        // a WaitForMultipleObjects that also watches stopEvent_. A synchronous
        // ReadFile could only be unstuck with CancelSynchronousIo, which needs
        // the target thread's handle and races its own completion.
        //
        // PIPE_REJECT_REMOTE_CLIENTS: this is a channel into a desktop app's
        // running plugin, and nothing about it should be reachable over SMB.
        return ::CreateNamedPipeW(
            pipeName_.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            kMaxClients,
            4096,    // out buffer
            4096,    // in buffer
            0,       // default timeout, unused: nothing here calls WaitNamedPipe
            nullptr);   // default DACL - see the header comment on access control
    }

    /// Waits for one overlapped operation, or for shutdown. Returns false when
    /// stop() woke us or the operation failed; on the shutdown path the I/O is
    /// cancelled and drained, so `ov` is safe to destroy afterwards.
    bool waitOverlapped(HANDLE pipe, OVERLAPPED& ov, DWORD& transferred)
    {
        const HANDLE waits[2] = { ov.hEvent, stopEvent_ };
        const DWORD signalled = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);

        if (signalled != WAIT_OBJECT_0)
        {
            // Shutdown (or a failed wait). The pending operation still refers
            // to `ov`, which is about to go out of scope, so it has to be
            // cancelled AND drained - CancelIoEx only requests cancellation.
            ::CancelIoEx(pipe, &ov);
            ::GetOverlappedResult(pipe, &ov, &transferred, TRUE);
            return false;
        }

        return ::GetOverlappedResult(pipe, &ov, &transferred, FALSE) != FALSE;
    }

    void listenerMain(HANDLE firstInstance)
    {
        HANDLE pipe = firstInstance;

        while (!stopping_ && pipe != INVALID_HANDLE_VALUE)
        {
            OVERLAPPED ov{};
            ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!ov.hEvent)
            {
                ::CloseHandle(pipe);
                break;
            }

            bool connected = false;

            if (::ConnectNamedPipe(pipe, &ov))
            {
                // Documented never to succeed synchronously for an overlapped
                // pipe, but treated as success rather than assumed away.
                connected = true;
            }
            else
            {
                switch (::GetLastError())
                {
                case ERROR_IO_PENDING:
                {
                    DWORD ignored = 0;
                    connected = waitOverlapped(pipe, ov, ignored);
                    break;
                }

                // A client connected in the window between CreateNamedPipe and
                // ConnectNamedPipe. There is no pending operation and no event
                // will ever fire; the connection is simply already made.
                case ERROR_PIPE_CONNECTED:
                    connected = true;
                    break;

                default:
                    connected = false;
                    break;
                }
            }

            ::CloseHandle(ov.hEvent);

            if (!connected || stopping_)
            {
                ::CloseHandle(pipe);
                break;
            }

            // Re-arm BEFORE serving, so the channel is never unlistened. If a
            // new instance cannot be created (the cap is reached) this one is
            // still served and the loop ends - which is the right shape: the
            // cap exists to bound the damage, not to drop the client in hand.
            HANDLE next = createInstance();

            {
                std::lock_guard<std::mutex> lock(clientsMutex_);

                // Threads that have finished are joined here rather than only
                // in stop(), so a long-running app that has seen a hundred
                // clients is not carrying a hundred thread objects.
                reapFinishedClients();

                auto slot = std::make_unique<ClientSlot>();
                ClientSlot* const raw = slot.get();

                // `finished` is set as the thread's LAST act, so a slot the
                // reaper sees flagged is a thread that is about to return -
                // join() on it does not block. Nothing else may set it.
                raw->thread = std::thread([this, pipe, raw]
                {
                    serveClient(pipe);
                    raw->finished = true;
                });

                clients_.push_back(std::move(slot));
            }

            pipe = next;
        }

        if (pipe != INVALID_HANDLE_VALUE)
            ::CloseHandle(pipe);
    }

    /// Caller holds clientsMutex_.
    void reapFinishedClients()
    {
        for (size_t i = 0; i < clients_.size();)
        {
            if (clients_[i]->finished)
            {
                if (clients_[i]->thread.joinable())
                    clients_[i]->thread.join();
                clients_.erase(clients_.begin() + static_cast<ptrdiff_t>(i));
                continue;
            }
            ++i;
        }
    }

    /// Reads lines from one client until it disconnects or we are stopping.
    void serveClient(HANDLE pipe)
    {
        std::string inbox;
        char buf[4096];
        bool clientGone = false;

        HANDLE readEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

        while (readEvent && !stopping_ && !clientGone)
        {
            OVERLAPPED ov{};
            ov.hEvent = readEvent;
            ::ResetEvent(readEvent);

            DWORD got = 0;
            const BOOL ok = ::ReadFile(pipe, buf, static_cast<DWORD>(sizeof(buf)), &got, &ov);

            if (!ok)
            {
                if (::GetLastError() != ERROR_IO_PENDING)
                    break;                                  // broken pipe, or worse
                if (!waitOverlapped(pipe, ov, got))
                    break;                                  // client gone, or shutdown
            }

            if (got == 0)
                break;                                      // client closed its end

            inbox.append(buf, got);

            // Newline-framed, matching SynthEditCL's `--script -` grammar and
            // every other transport, so one client implementation drives all.
            size_t nl;
            while (!clientGone && (nl = inbox.find('\n')) != std::string::npos)
            {
                std::string line = inbox.substr(0, nl);
                inbox.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty())
                    continue;

                // Blocks until the main thread's next tick runs it.
                std::string response = queue_.run([this, line] { return handler_(line); });
                response += '\n';

                // A client that stopped reading mid-response is finished with;
                // there is nothing useful to do with the rest of its inbox.
                clientGone = !writeAll(pipe, response);
            }
        }

        if (readEvent)
            ::CloseHandle(readEvent);

        // FlushFileBuffers before disconnecting: DisconnectNamedPipe discards
        // anything the client has not read yet, which would truncate the last
        // response of a client that sent a command and exited promptly.
        ::FlushFileBuffers(pipe);
        ::DisconnectNamedPipe(pipe);
        ::CloseHandle(pipe);
    }

    bool writeAll(HANDLE pipe, const std::string& data)
    {
        HANDLE writeEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!writeEvent)
            return false;

        bool ok = true;
        size_t sent = 0;

        while (sent < data.size())
        {
            OVERLAPPED ov{};
            ov.hEvent = writeEvent;
            ::ResetEvent(writeEvent);

            DWORD written = 0;
            const BOOL started = ::WriteFile(pipe, data.data() + sent,
                                             static_cast<DWORD>(data.size() - sent), &written, &ov);

            if (!started)
            {
                if (::GetLastError() != ERROR_IO_PENDING)
                {
                    ok = false;
                    break;
                }
                if (!waitOverlapped(pipe, ov, written))
                {
                    ok = false;
                    break;
                }
            }

            if (written == 0)
            {
                ok = false;
                break;
            }

            sent += written;
        }

        ::CloseHandle(writeEvent);
        return ok;
    }

    /// One connected client. Held by unique_ptr because the thread body keeps a
    /// raw pointer to its own slot, which a vector reallocation would otherwise
    /// invalidate under it.
    struct ClientSlot
    {
        std::thread thread;
        std::atomic<bool> finished{ false };
    };

    MainThreadQueue queue_;
    CommandHandler handler_;

    std::thread listener_;
    std::mutex clientsMutex_;
    std::vector<std::unique_ptr<ClientSlot>> clients_;

    std::wstring pipeName_;
    std::string  channelName_;

    HANDLE stopEvent_{};

    std::atomic<bool> running_{ false };
    std::atomic<bool> stopping_{ false };
};

} // namespace mcp
} // namespace standalone
} // namespace gmpi
