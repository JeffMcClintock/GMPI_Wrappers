#pragma once

// The standalone's main-thread marshaller.
//
// This is the piece SynthEdit's live transport was missing on Linux. Windows
// has DispatcherQueue::TryEnqueue and macOS has dispatch_async, so both of
// those editors could hand a command to the thread that owns the model and
// wait for the answer (SynthEdit2/EditorIpcServer.cpp and
// SynthEditMac/EditorIpcServerMac.h). A Wayland app has no such queue: its
// main thread is sitting in poll() inside runEventLoop, and the only thing
// that reliably reaches it is the loop's own tick.
//
// So the tick IS the queue. drain() is called once per frame from the
// runEventLoop callback, which fires off a timerfd rather than off input, so
// an idle app with nobody touching it still services commands at ~60Hz.
//
// THREADING. The IPC listener thread NEVER touches the plugin, the editor or
// the controller. It parses a line, calls run(), and blocks on a future until
// the main thread has produced the response string. Everything the dispatcher
// does therefore happens on the thread that owns all of it, which is the same
// contract both other platforms already keep.
//
// WHY NOT A POLLING WAIT. The macOS version polls its future in 25ms slices
// because dispatch_async cannot report a queue that will never run the block:
// after the run loop stops, the block is silently dropped and a plain wait
// would hang forever. Here the queue is ours, so shutdown() can walk the
// pending items and fail them explicitly. That turns the wait into a single
// timed one, and the deadline below is then only about the *busy* case.

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace gmpi
{
namespace standalone
{
namespace mcp
{

class MainThreadQueue
{
public:
    // Produces the response line. Always invoked on the main thread, so it may
    // touch the document, the editor and the plugin freely.
    using Job = std::function<std::string()>;

    /// IPC thread. Blocks until the main thread has run `job`, and returns what
    /// it returned.
    ///
    /// Two ways this returns without the job having run, and both are honest
    /// about it rather than hanging: the app is shutting down, or the main
    /// thread did not get to it within kStartDeadline.
    std::string run(Job job)
    {
        auto item = std::make_shared<Item>();
        item->job = std::move(job);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_)
                return errorLine("the application is shutting down");
            queue_.push_back(item);
        }

        auto future = item->promise.get_future();

        // The deadline is on the job STARTING, never on it finishing. An idle
        // tick drains in microseconds, so seconds of latency means something is
        // sitting on the main thread - a modal dialog, or a plugin editor stuck
        // in its own loop. Meanwhile --render-audio legitimately takes a while,
        // and must not be cut off just for being slow.
        if (future.wait_for(kStartDeadline) == std::future_status::ready)
            return future.get();

        // Pending -> Abandoned, claimed by us. Whoever wins this exchange
        // decides: a command we have already reported as "not run" must never
        // quietly run afterwards, which would be worse than either outcome on
        // its own. Losing the race means it started after all, so wait it out.
        int expected = kPending;
        if (!item->state.compare_exchange_strong(expected, kAbandoned))
            return future.get();

        return R"({"ok":false,"busy":true,"error":"the application is busy - its main thread is blocked, so this command was NOT run. Retry once it is responsive."})";
    }

    /// Main thread, once per event-loop tick. Runs everything queued since the
    /// last one.
    void drain()
    {
        for (;;)
        {
            std::shared_ptr<Item> item;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (queue_.empty())
                    return;
                item = queue_.front();
                queue_.pop_front();
            }

            // Claim it, or discover run() gave up on it and moved on. In the
            // latter case the caller already has its "busy" answer and nothing
            // may touch the model on its behalf.
            int expected = kPending;
            if (!item->state.compare_exchange_strong(expected, kRunning))
                continue;

            // A throwing verb must not take the app down: the user has an
            // unsaved session in front of them, and a bad command from a
            // developer's script is not worth losing it over.
            try
            {
                item->promise.set_value(item->job());
            }
            catch (const std::exception& e)
            {
                item->promise.set_value(errorLine(std::string("exception: ") + e.what()));
            }
            catch (...)
            {
                item->promise.set_value(errorLine("unknown exception"));
            }
        }
    }

    /// Main thread, before anything the jobs touch is destroyed. Idempotent.
    ///
    /// Fails every queued item rather than dropping it, so an IPC thread parked
    /// in run() is released instead of waiting out its deadline against a main
    /// thread that will never tick again.
    void shutdown()
    {
        std::deque<std::shared_ptr<Item>> pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            pending.swap(queue_);
        }

        for (auto& item : pending)
        {
            int expected = kPending;
            if (item->state.compare_exchange_strong(expected, kRunning))
                item->promise.set_value(errorLine("the application is shutting down"));
        }
    }

private:
    static constexpr int kPending = 0, kRunning = 1, kAbandoned = 2;

    /// Generous on purpose: a false "busy" is a retryable error, whereas too
    /// short a deadline turns an ordinary slow frame into a spurious failure.
    static constexpr std::chrono::seconds kStartDeadline{ 5 };

    struct Item
    {
        Job job;
        std::promise<std::string> promise;
        std::atomic<int> state{ kPending };
    };

    static std::string errorLine(const std::string& what)
    {
        // Escaping is the dispatcher's job everywhere else, but these three
        // strings are ours and contain nothing that needs it.
        return "{\"ok\":false,\"error\":\"" + what + "\"}";
    }

    std::mutex mutex_;
    std::deque<std::shared_ptr<Item>> queue_;
    bool stopping_ = false;
};

} // namespace mcp
} // namespace standalone
} // namespace gmpi
