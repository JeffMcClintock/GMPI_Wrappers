// main_thread_queue_deadline_probe - does the command channel's second
// deadline actually bound a job that blocks AFTER it starts, and does a
// heartbeat actually keep a long job alive?
//
// WHY THIS EXISTS. BACKLOG E43: on macOS a --pointer-down over TIDE's menu bar
// opens a native NSMenu, whose nested modal run loop runs INSIDE the job. The
// job is already running by then, so MainThreadQueue::run fell through to an
// unbounded future.get(), and because every transport dispatches inline on one
// listener thread the whole channel went with it - measured, twice, and only
// kill -9 recovered.
//
// The fix is measurable end to end against the real app for the FAILING half:
// click File, get a bounded answer, watch a second connection still answer.
// The RESCUE half is not, and that is the gap this probe fills. The only verb
// that legitimately outlives kProgressDeadline is --render-audio, and TIDE's
// default rack renders its 240-second maximum in under half a second on this
// box - so on the app there is no way to watch the heartbeat save anything.
// A hook nobody has watched work is not a hook.
//
// It drives the real class, unmodified, with no GMPI, no plugin, no window and
// no build system:
//
//     c++ -std=c++17 -O1 -o /tmp/mtq_probe \
//         tests/main_thread_queue_deadline_probe.cpp
//     /tmp/mtq_probe
//
// It takes about a minute, because two of the three cases have to outlast a
// real 20-second deadline. It is opt-in for that reason and is not wired into
// CI; -DGMPI_WRAPPERS_BUILD_TESTS=ON builds it if you want it built.
//
// The sleeps are sized off MainThreadQueue's own published constants rather
// than off the numbers 5 and 20, so changing a deadline cannot leave this
// probe quietly measuring the wrong thing.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "../wrapper/Standalone/mcp/MainThreadQueue.h"

using gmpi::standalone::mcp::MainThreadQueue;
using Clock = std::chrono::steady_clock;

namespace
{

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        ++failures;
}

double secondsSince(Clock::time_point t0)
{
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

/// Stands in for the app's event loop: drain() once per tick, forever, until
/// told to stop. Running it on its own thread is what makes heartbeat()'s
/// thread_local mean what it means in the app - the job's thread and the
/// waiter's thread are different, exactly as they are in the standalone.
class FakeMainThread
{
public:
    explicit FakeMainThread(MainThreadQueue& queue)
        : thread_([this, &queue]
          {
              while (!stop_.load(std::memory_order_acquire))
              {
                  queue.drain();
                  std::this_thread::sleep_for(std::chrono::milliseconds(5));
              }
          })
    {
    }

    ~FakeMainThread()
    {
        stop_.store(true, std::memory_order_release);
        thread_.join();
    }

private:
    std::atomic<bool> stop_{ false };
    std::thread       thread_;
};

bool stalled(const std::string& answer)
{
    return answer.find("\"started\":true") != std::string::npos;
}

bool notRun(const std::string& answer)
{
    return answer.find("\"busy\":true") != std::string::npos && !stalled(answer);
}

} // namespace

int main()
{
    const auto start    = MainThreadQueue::kStartDeadline;
    const auto progress = MainThreadQueue::kProgressDeadline;

    std::printf("kStartDeadline=%llds  kProgressDeadline=%llds\n\n",
                static_cast<long long>(start.count()),
                static_cast<long long>(progress.count()));

    // --- 1. control: an ordinary job answers, and answers fast -------------
    //
    // Without this the two timing cases below mean nothing: a queue that
    // never runs anything would "pass" case 2 for entirely the wrong reason.
    {
        MainThreadQueue queue;
        FakeMainThread  loop(queue);

        const auto t0 = Clock::now();
        const std::string answer = queue.run([] { return std::string("{\"ok\":true}"); });
        const double took = secondsSince(t0);

        check(answer == "{\"ok\":true}", "control: a fast job returns its own answer");
        check(took < 1.0, "control: and returns it in under a second");
        std::printf("      %.2fs %s\n\n", took, answer.c_str());
    }

    // --- 2. a job that starts and then stops responding --------------------
    //
    // The E43 shape, with a sleep standing in for the modal run loop. Before
    // the fix this waited forever.
    {
        MainThreadQueue    queue;
        FakeMainThread     loop(queue);
        std::atomic<bool>  jobFinished{ false };

        const auto blockFor = progress + std::chrono::seconds(6);

        const auto t0 = Clock::now();
        const std::string answer = queue.run([&]
        {
            std::this_thread::sleep_for(blockFor);
            jobFinished.store(true, std::memory_order_release);
            return std::string("{\"ok\":true,\"late\":true}");
        });
        const double took = secondsSince(t0);

        check(stalled(answer), "stall: a job that blocks past the deadline is reported as STARTED");
        check(!notRun(answer),
              "stall: and NOT as \"was NOT run\", which would be a lie about a running job");
        check(took < std::chrono::duration<double>(progress).count() + 3.0,
              "stall: the caller is released at about the progress deadline");
        check(took > std::chrono::duration<double>(progress).count() - 1.0,
              "stall: and not before it");
        check(!jobFinished.load(std::memory_order_acquire),
              "stall: the job really was still running when the caller was released");
        std::printf("      %.2fs %s\n", took, answer.c_str());

        // The answer said "may still complete". Show that it does, rather than
        // leaving the claim in a string.
        std::this_thread::sleep_for(blockFor);
        check(jobFinished.load(std::memory_order_acquire),
              "stall: and it completed afterwards, as the answer said it might");
        std::printf("\n");
    }

    // --- 3. the rescue: a long job that heartbeats is waited for -----------
    //
    // Same duration as case 2, so the ONLY variable is the heartbeat.
    {
        MainThreadQueue queue;
        FakeMainThread  loop(queue);

        const auto blockFor = progress + std::chrono::seconds(6);

        const auto t0 = Clock::now();
        const std::string answer = queue.run([&]
        {
            const auto until = Clock::now() + blockFor;
            while (Clock::now() < until)
            {
                MainThreadQueue::heartbeat();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            return std::string("{\"ok\":true,\"long\":true}");
        });
        const double took = secondsSince(t0);

        check(answer == "{\"ok\":true,\"long\":true}",
              "heartbeat: a beating job outlives the deadline and returns its own answer");
        check(!stalled(answer), "heartbeat: and is never reported as stalled");
        check(took > std::chrono::duration<double>(progress).count(),
              "heartbeat: the run genuinely lasted longer than the deadline it survived");
        std::printf("      %.2fs %s\n\n", took, answer.c_str());
    }

    std::printf("%s  %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
