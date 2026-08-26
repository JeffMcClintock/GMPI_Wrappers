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
//
// TWO DEADLINES, NOT ONE, AND THE SECOND ONE EXISTS BECAUSE OF A REAL WEDGE.
// kStartDeadline covers "the main thread never got to it". It cannot cover
// "the main thread got to it and never came back", and that is not a
// hypothetical: on macOS a --pointer-down over the menu bar opens a native
// NSMenu, which runs a NESTED MODAL RUN LOOP INSIDE the job. The item is
// already kRunning by then, so the old code fell through to an unbounded
// future.get() - and because every transport dispatches inline on its single
// listener thread ("one plugin, one command at a time", IpcServer.h), that
// one blocked command took the WHOLE CHANNEL with it. Measured: the click
// never answered, a fresh connection's --info never answered either, and only
// kill -9 recovered. BACKLOG E43.
//
// So a started job is also on a clock - kProgressDeadline - and the clock is a
// HEARTBEAT rather than a total. A job that is genuinely working says so by
// calling heartbeat(), which is one line in --render-audio's block loop and
// nothing anywhere else. That is deliberately the opposite way round from a
// list of "verbs that may block": a list of verbs ages silently as verbs are
// added, whereas an unheard-of new verb that blocks forever simply gets the
// bounded answer, which is the safe direction to be wrong in.
//
// WHAT THE SECOND DEADLINE MUST NOT CLAIM. A job past its start is running and
// nothing can take it back, so the answer says so: "it started, it has not
// finished". Reusing the kStartDeadline wording - "this command was NOT run" -
// would be a lie, and the one kind of lie this channel cannot afford, because
// a caller reads it as licence to retry an edit that is about to land anyway.

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

    /// Generous on purpose: a false "busy" is a retryable error, whereas too
    /// short a deadline turns an ordinary slow frame into a spurious failure.
    static constexpr std::chrono::seconds kStartDeadline{ 5 };

    /// How long a STARTED job may go without a heartbeat before the caller is
    /// told so and released. Four times kStartDeadline, for the same reason
    /// that one is generous: every ordinary verb finishes in microseconds, so
    /// anything approaching this is already pathological. It bounds the wedge,
    /// it is not a performance budget.
    static constexpr std::chrono::seconds kProgressDeadline{ 20 };

    /// IPC thread. Blocks until the main thread has run `job`, and returns what
    /// it returned.
    ///
    /// Three ways this returns without an answer from the job, and every one of
    /// them is honest about which it is rather than hanging: the app is
    /// shutting down; the main thread did not START it within kStartDeadline;
    /// or it started and then stopped making progress for kProgressDeadline.
    /// The last is a different sentence from the second on purpose - see the
    /// header.
    std::string run(Job job)
    {
        auto item = std::make_shared<Item>();
        item->job = std::move(job);

        // Seeded here, and drain() restamps it the moment the job actually
        // starts. Both, because the two stores straddle a race: the waiter can
        // reach awaitProgress in the sliver between drain()'s state CAS and its
        // own stamp, and a zero beat there would read as "stalled since the
        // epoch" and answer immediately. Seeded, the worst case is a deadline
        // measured from the enqueue instead of from the start, which is still
        // bounded and still generous.
        item->beat.store(nowTicks(), std::memory_order_release);

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
        // its own. Losing the race means it started after all, so wait it out -
        // but only while it is still making progress.
        int expected = kPending;
        if (!item->state.compare_exchange_strong(expected, kAbandoned))
            return awaitProgress(*item, future);

        return R"({"ok":false,"busy":true,"error":"the application is busy - its main thread is blocked, so this command was NOT run. Retry once it is responsive."})";
    }

    /// Main thread, from INSIDE a running job: "still working, do not give up
    /// on me". Anything that can legitimately hold the main thread for longer
    /// than kProgressDeadline calls this as it goes; --render-audio's block
    /// loop is the only such thing today.
    ///
    /// A no-op off the main thread, and a no-op when no job is running, so it
    /// is safe to call from shared code that a plugin might also reach by some
    /// other path.
    static void heartbeat()
    {
        if (Item* item = currentItem_)
            item->beat.store(nowTicks(), std::memory_order_release);
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

            // The first beat, and it has to be taken HERE rather than when the
            // item was queued: kProgressDeadline is about how long the job has
            // been silent, and time spent waiting in the queue is already
            // kStartDeadline's business.
            item->beat.store(nowTicks(), std::memory_order_release);

            // Published for heartbeat(). Restored on every path out, including
            // a throw, because a stale pointer here would let the NEXT job's
            // waiter be held open by a dead item.
            CurrentJobScope scope(item.get());

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

    using Clock = std::chrono::steady_clock;

    struct Item
    {
        Job job;
        std::promise<std::string> promise;
        std::atomic<int> state{ kPending };

        /// steady_clock ticks at the last sign of life from this job. Written
        /// by the main thread (drain, then heartbeat), read by the waiter.
        std::atomic<Clock::rep> beat{ 0 };
    };

    static Clock::rep nowTicks() { return Clock::now().time_since_epoch().count(); }

    /// The item the main thread is inside right now, for heartbeat(). Thread
    /// local, so an IPC thread calling heartbeat() by mistake finds nothing and
    /// does nothing, rather than beating on somebody else's job.
    static inline thread_local Item* currentItem_ = nullptr;

    struct CurrentJobScope
    {
        explicit CurrentJobScope(Item* item) { currentItem_ = item; }
        ~CurrentJobScope() { currentItem_ = nullptr; }
        CurrentJobScope(const CurrentJobScope&) = delete;
        CurrentJobScope& operator=(const CurrentJobScope&) = delete;
    };

    /// IPC thread, for a job that has already STARTED. Returns its answer if it
    /// produces one, or a bounded "it started and stalled" line if it goes
    /// kProgressDeadline without a heartbeat.
    ///
    /// The wait is re-armed off the LAST BEAT rather than off entry, so a job
    /// that beats every block is waited on indefinitely and one that beats
    /// never is given exactly one deadline. Re-reading the beat after the
    /// timeout is what tells the two apart: unchanged means genuinely stalled.
    static std::string awaitProgress(Item& item, std::future<std::string>& future)
    {
        for (;;)
        {
            const Clock::rep beat = item.beat.load(std::memory_order_acquire);
            const Clock::time_point deadline =
                Clock::time_point(Clock::duration(beat)) + kProgressDeadline;

            if (future.wait_until(deadline) == std::future_status::ready)
                return future.get();

            if (item.beat.load(std::memory_order_acquire) == beat)
                return stalledLine();
        }
    }

    /// Deliberately NOT the kStartDeadline wording. This command IS running;
    /// what the caller has lost is the answer, not the effect.
    static std::string stalledLine()
    {
        return R"({"ok":false,"busy":true,"started":true,"error":"this command STARTED and has not finished - the main thread is inside it, which on macOS usually means a native menu or modal dialog it opened. It was NOT cancelled and may still complete. Nothing else can run until it does."})";
    }

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
