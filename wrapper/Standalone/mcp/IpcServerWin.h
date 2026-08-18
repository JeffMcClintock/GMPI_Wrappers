#pragma once

// A named-pipe command channel into the RUNNING standalone app.
//
// The Windows counterpart of IpcServer.h's unix socket (Linux) and of
// SynthEditMac's; include "IpcServer.h", which selects between them. Same
// newline framing, same JSONL responses, so one MCP client drives any of them
// without knowing which it reached - and the same in the literal sense, since
// the framing, the dispatch and the state around them are CommandProtocol.h's
// and this file holds only the pipe.
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
// does not hold up the others.
//
// Idle is not free, though, and ONE idle client is dearer than the rest: the
// one that arrives when the spare is the last free instance. Every other client
// costs a thread parked in a wait and nothing more. That one leaves nothing
// listening, so the listener sits in awaitFreeInstance() retrying every
// kInstanceRetryMs - a 4 Hz poll, measured at 0.39% CPU - for exactly as long
// as the client lives, which for an orphaned MCP server is indefinitely. It is
// the price of the alternative, which is what this replaced: a channel that
// died at that moment and never came back.
//
// The cap on that is the pipe's own nMaxInstances, which differs from the unix
// server's cap in one way that has to be handled rather than inherited: there,
// reaching the cap is a decision this code makes and it simply closes the
// excess fd and keeps accepting; here it is the kernel refusing to create an
// instance, which arrives as a FAILURE at exactly the moment the listener needs
// to re-arm. Treating that failure as terminal killed the channel for the rest
// of the process - see awaitFreeInstance(), which is why it does not.

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>

#include "CommandProtocol.h"
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

/// "<what>: <the system's own words> (error <n>)". The unix transport spells
/// the same idea with strerror (IpcServer.h); the shared point is that a bare
/// error NUMBER in a status line helps nobody. The number is kept alongside the
/// text anyway, because it is what a search or a colleague is asked for, and
/// because FormatMessage can decline to translate one.
inline std::string describeLastError(const std::string& what, DWORD err)
{
    char* text = nullptr;
    const DWORD chars = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&text),   // the ALLOCATE_BUFFER idiom: an out-pointer through an in-pointer parameter
        0,
        nullptr);

    std::string detail;
    if (text)
    {
        detail.assign(text, chars);
        ::LocalFree(text);
    }

    // System messages arrive punctuated and terminated - "The system cannot
    // find the file specified.\r\n" - and this is going inside a parenthesis on
    // ONE status line, which a stray CRLF would break in two.
    while (!detail.empty()
        && (detail.back() == '\r' || detail.back() == '\n'
         || detail.back() == '.'  || detail.back() == ' '))
    {
        detail.pop_back();
    }

    const std::string code = " (error " + std::to_string(err) + ")";
    return detail.empty() ? what + code
                          : what + ": " + detail + code;
}

class IpcServer
{
public:
    /// Both transports take the same handler, declared and described in
    /// CommandProtocol.h. Spelled here as well because a caller reaches for it
    /// through the server it is handing it to.
    using CommandHandler = mcp::CommandHandler;

    IpcServer() = default;
    ~IpcServer() { stop(); }

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    /// Drained once per tick by the app. This is what actually runs commands.
    MainThreadQueue& mainThreadQueue() { return core_.mainThreadQueue(); }

    /// Begins listening. Reports false and leaves the app running normally when
    /// the pipe cannot be created - a missing command channel must never be
    /// fatal to an app the user launched to make sound with.
    bool start(CommandHandler handler)
    {
        if (core_.running())
            return true;

        // Empty after a successful start, as AudioDriver and MidiDriver promise
        // of theirs: a stale reason left here would read to every later caller
        // as a failure that did not happen.
        lastError_.clear();

        if (!handler)
        {
            // The caller's own bug rather than the system's, but a bare false
            // with nothing to say is the thing this member exists to prevent.
            lastError_ = "no command handler was supplied";
            return false;
        }

        pipeName_ = L"\\\\.\\pipe\\" + std::wstring(kPipePrefix)
                  + std::to_wstring(::GetCurrentProcessId());

        // Proven before the listener starts, so a failure is reported to the
        // caller rather than discovered on a thread that has nowhere to report
        // it. The instance is not wasted - the listener takes it as its first.
        HANDLE first = createInstance();
        if (first == INVALID_HANDLE_VALUE)
        {
            lastError_ = describeLastError("CreateNamedPipeW(" + narrow(pipeName_) + ") failed",
                                           ::GetLastError());
            pipeName_.clear();
            return false;
        }

        stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual reset
        if (!stopEvent_)
        {
            lastError_ = describeLastError("could not create the stop event", ::GetLastError());
            ::CloseHandle(first);
            pipeName_.clear();
            return false;
        }

        // AUTO-reset, unlike stopEvent_: this one reports a single event that
        // has happened rather than a state the server is now in, and the
        // listener wants each wait to block again afterwards. Missing a signal
        // when two clients leave together is harmless - one retry is all it
        // takes to get an instance back, and awaitFreeInstance() retries until
        // it has one.
        slotFreed_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!slotFreed_)
        {
            lastError_ = describeLastError("could not create the slot-freed event", ::GetLastError());
            ::CloseHandle(stopEvent_);
            stopEvent_ = {};
            ::CloseHandle(first);
            pipeName_.clear();
            return false;
        }

        channelName_ = narrow(pipeName_);
        core_.arm(std::move(handler));
        listener_ = std::thread([this, first] { listenerMain(first); });
        return true;
    }

    /// Idempotent. MUST be called on the MAIN thread and BEFORE the plugin,
    /// editor or drivers are torn down: the queue shutdown inside it is what
    /// guarantees no handler can still be reaching for them.
    void stop()
    {
        if (!core_.running() && !listener_.joinable())
            return;

        // Step 1 of the shutdown order beginStop() sets out. The wake and the
        // joins below are steps 2 and 3, and swapping them is the deadlock.
        core_.beginStop();

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

        // After the client join above, not before: a client thread signals
        // slotFreed_ as it finishes, so the handle has to outlive every one of
        // them. The same argument applies to stopEvent_, which serveClient's
        // waits watch.
        if (stopEvent_)
        {
            ::CloseHandle(stopEvent_);
            stopEvent_ = {};
        }

        if (slotFreed_)
        {
            ::CloseHandle(slotFreed_);
            slotFreed_ = {};
        }

        core_.endStop();
        pipeName_.clear();
        channelName_.clear();
    }

    bool running() const { return core_.running(); }

    /// Why the last start() returned false. Empty when it succeeded, matching
    /// the contract AudioDriver::lastError and MidiDriver::lastError state in
    /// AudioMidiDevices.h, and matching the unix transport in IpcServer.h - one
    /// report line is shared by both, so the two must mean the same thing.
    std::string lastError() const { return lastError_; }

    /// e.g. "\\.\pipe\gmpi-standalone.10673". Empty until start() succeeds.
    const std::string& channelName() const { return channelName_; }

private:
    /// The concurrency this transport SUSTAINS: this many clients connected
    /// with an instance still listening for the next. NOT the most that can be
    /// connected at once, which is one higher - see below. Small either way:
    /// this is a command channel, not a server, and the cap is the pipe's own
    /// (nMaxInstances), so a client that leaks connections is refused by the
    /// kernel rather than growing this process a thread at a time.
    ///
    /// Sustained is why createInstance() asks the kernel for one MORE than
    /// this. An instance always sits unconnected waiting to accept, and it is
    /// no more a client than the unix server's listening socket is; passing
    /// this number straight through as nMaxInstances made the advertised eight
    /// sustain only seven.
    ///
    /// The client that arrives when the spare is the last free instance is
    /// where the two transports genuinely differ. The unix server accepts it
    /// and closes it again; here the kernel hands it that spare rather than
    /// refusing it, so it IS served. Which is what puts the peak one above this
    /// number - nine connected and answered at once, as it stands - with
    /// nothing listening until one of them goes, and awaitFreeInstance() is
    /// what re-arms then.
    static constexpr DWORD kMaxClients = 8;

    /// How long the listener sleeps between attempts to re-arm while
    /// createInstance() keeps failing transiently. At the instance cap it is
    /// only a backstop - slotFreed_ wakes it the instant a client goes, and
    /// this bounds the damage if that signal is ever missed; for the resource
    /// failures it is the whole retry clock. Short enough not to be noticed,
    /// long enough not to be a spin.
    static constexpr DWORD kInstanceRetryMs = 250;

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
            kMaxClients + 1,   // the spare is the one waiting to accept - see kMaxClients
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

    /// The CreateNamedPipeW failures that clear by themselves, and are
    /// therefore worth waiting out rather than ending the listener on.
    ///
    /// ERROR_PIPE_BUSY is the pipe at nMaxInstances, and every served
    /// connection that ends hands an instance back. The other two are the
    /// system briefly unable to afford one - a resource or quota exhausted, an
    /// allocation refused - and they are here for the same reason: nothing
    /// about either says the NEXT attempt cannot succeed, and a listener that
    /// ended on one would be the permanent-death bug awaitFreeInstance() exists
    /// to prevent, reached through a different error code.
    ///
    /// Everything else is terminal, and deliberately: a name held by another
    /// process, or a parameter the system rejected, will not come right on the
    /// next attempt.
    static bool isTransientInstanceError(DWORD err)
    {
        return err == ERROR_PIPE_BUSY
            || err == ERROR_NO_SYSTEM_RESOURCES
            || err == ERROR_NOT_ENOUGH_MEMORY;
    }

    /// A fresh listening instance, or INVALID_HANDLE_VALUE when the listener
    /// should end. `why` is the GetLastError() of the attempt that already
    /// failed, so the first thing this decides is whether to try again at all -
    /// see isTransientInstanceError().
    ///
    /// Ending the listener on a condition that clears by itself - which this
    /// code used to do for ERROR_PIPE_BUSY, the loop simply running out on an
    /// INVALID_HANDLE_VALUE - left the app with no command channel for the rest
    /// of its life even after every client had gone, because listener_ is
    /// created only by start() and start() early-returns while core_.running().
    /// The unix server reaches the same moment at `close(fd)` in its accept arm
    /// and just keeps polling; this is that behaviour spelled for a transport
    /// whose cap is the kernel's to enforce.
    ///
    /// Retrying cannot become a spin however long the condition lasts: each
    /// attempt is preceded by a wait, so this is bounded to one attempt per
    /// kInstanceRetryMs plus one per departing client, and a client can only
    /// depart as many times as it connected.
    HANDLE awaitFreeInstance(DWORD why)
    {
        while (isTransientInstanceError(why) && !core_.stopping())
        {
            // Timed rather than INFINITE so that slotFreed_ is an optimisation
            // and not the thing correctness rests on - and it has to be, since
            // the two resource failures are conditions no departing client will
            // ever signal. There the timeout is the ONLY wake.
            const HANDLE waits[2] = { slotFreed_, stopEvent_ };
            const DWORD signalled = ::WaitForMultipleObjects(2, waits, FALSE, kInstanceRetryMs);

            if (signalled != WAIT_OBJECT_0 && signalled != WAIT_TIMEOUT)
                break;   // stop(), or a wait that failed: retrying either is pointless

            // The accept path reaps too, but it is not reached while the
            // listener is parked here - so without this a long spell at the cap
            // would carry a thread object for every client that had departed.
            {
                std::lock_guard<std::mutex> lock(clientsMutex_);
                reapFinishedClients();
            }

            const HANDLE pipe = createInstance();
            if (pipe != INVALID_HANDLE_VALUE)
                return pipe;

            why = ::GetLastError();
        }

        return INVALID_HANDLE_VALUE;
    }

    void listenerMain(HANDLE firstInstance)
    {
        HANDLE pipe = firstInstance;

        while (!core_.stopping() && pipe != INVALID_HANDLE_VALUE)
        {
            OVERLAPPED ov{};
            ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!ov.hEvent)
                break;   // `pipe` is closed once, on the way out - see below

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

            // The ordinary shutdown path: stop() sets stopEvent_, waitOverlapped
            // reports false, and `connected` is false. `pipe` is left for the
            // single close below rather than closed here - closing it twice, as
            // this used to, hands a handle value back to the kernel while a
            // sibling thread may already have been given it for something else.
            if (!connected || core_.stopping())
                break;

            // Attempted BEFORE serving, so the window in which no instance sits
            // waiting is as short as the kernel allows: a client that calls
            // CreateFile inside it is told ERROR_PIPE_BUSY, and not every
            // client library retries that.
            HANDLE next = createInstance();

            // Read HERE, not where it is used: the hand-off below allocates,
            // locks and starts a thread, any of which may overwrite this
            // thread's last error before awaitFreeInstance() could ask for it.
            const DWORD reArmError = (next == INVALID_HANDLE_VALUE) ? ::GetLastError()
                                                                    : ERROR_SUCCESS;

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

            // Only now that the client in hand has a thread of its own. Waiting
            // for a free instance any earlier would starve the very client
            // whose arrival exhausted the pipe: the only thing that frees an
            // instance is a served connection ending, and an unserved one never
            // ends.
            if (next == INVALID_HANDLE_VALUE)
                next = awaitFreeInstance(reArmError);

            pipe = next;
        }

        // The one owner of the un-handed-off instance. Every break above leaves
        // `pipe` alive for this, and every instance handed to a client thread is
        // that thread's to close (serveClient) - so each instance is closed once
        // and by exactly one thread.
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
        // Per connection, and it has to be: a ReadFile returns whatever the
        // client happened to have written, which can stop mid-line.
        LineFramer framer;

        char buf[4096];
        bool clientGone = false;

        HANDLE readEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

        while (readEvent && !core_.stopping() && !clientGone)
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

            // A client that stopped reading mid-response is finished with, and
            // there is nothing useful to do with the rest of its inbox - which
            // is what the framer stopping at the first refused line means here.
            clientGone = !framer.feed(buf, got, [this, pipe](const std::string& line)
            {
                return writeAll(pipe, core_.respondTo(line));
            });
        }

        if (readEvent)
            ::CloseHandle(readEvent);

        // FlushFileBuffers before disconnecting: DisconnectNamedPipe discards
        // anything the client has not read yet, which would truncate the last
        // response of a client that sent a command and exited promptly.
        ::FlushFileBuffers(pipe);
        ::DisconnectNamedPipe(pipe);
        ::CloseHandle(pipe);

        // The instance goes back to the kernel's pool on that CloseHandle, and
        // nothing else would tell the listener so: it has no handle on this
        // connection and gets no callback. Without this it only finds out on
        // its next timed retry, which is a wait the user can feel.
        if (slotFreed_)
            ::SetEvent(slotFreed_);
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

    /// The queue, the handler, and the running/stopping flags - all of it
    /// shared with the unix transport (CommandProtocol.h). Everything else in
    /// this class is the pipe.
    ChannelCore core_;

    std::thread listener_;
    std::mutex clientsMutex_;
    std::vector<std::unique_ptr<ClientSlot>> clients_;

    std::wstring pipeName_;
    std::string  channelName_;

    HANDLE stopEvent_{};

    /// Set by each client thread as it exits, so a listener parked at the
    /// instance cap learns of a free slot at once rather than on a timer.
    HANDLE slotFreed_{};

    std::string lastError_;
};

} // namespace mcp
} // namespace standalone
} // namespace gmpi
