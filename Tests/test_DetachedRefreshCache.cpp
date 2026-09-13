#include "TestFramework.h"
#include "Core/DetachedRefreshCache.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace mma;

namespace {

struct BlockingScan
{
    std::vector<int> run()
    {
        std::unique_lock<std::mutex> lock (mutex);
        ++calls;
        entered = true;
        condition.notify_all();
        condition.wait (lock, [this] { return released; });
        finished = true;
        condition.notify_all();
        return { 99 };
    }

    bool waitUntilEntered()
    {
        std::unique_lock<std::mutex> lock (mutex);
        return condition.wait_for (lock, std::chrono::seconds (2),
                                   [this] { return entered; });
    }

    void release()
    {
        const std::lock_guard<std::mutex> guard (mutex);
        released = true;
        condition.notify_all();
    }

    bool waitUntilFinished()
    {
        std::unique_lock<std::mutex> lock (mutex);
        return condition.wait_for (lock, std::chrono::seconds (2),
                                   [this] { return finished; });
    }

    int callCount()
    {
        const std::lock_guard<std::mutex> guard (mutex);
        return calls;
    }

    std::mutex mutex;
    std::condition_variable condition;
    int calls = 0;
    bool entered = false;
    bool released = false;
    bool finished = false;
};

} // namespace

TEST_CASE (DetachedRefreshCache_ReturnsFallbackAndNeverJoinsAStuckScan)
{
    auto scan = std::make_shared<BlockingScan>();
    auto cache = std::make_unique<DetachedRefreshCache<std::vector<int>>> (
        std::vector<int> { 7 });

    const auto firstStarted = std::chrono::steady_clock::now();
    const auto first = cache->getAndRefresh (std::chrono::seconds (2), [scan]
    {
        return scan->run();
    });
    const auto firstElapsed = std::chrono::steady_clock::now() - firstStarted;
    const bool entered = scan->waitUntilEntered();

    // Calls made while the OS probe is stuck keep returning the safe snapshot
    // and must not create a growing collection of blocked workers.
    bool keptFallback = true;
    for (int i = 0; i < 8; ++i)
        keptFallback = keptFallback
                    && cache->getAndRefresh (std::chrono::milliseconds (0), [scan]
                       {
                           return scan->run();
                       }) == std::vector<int> { 7 };

    const auto destroyStarted = std::chrono::steady_clock::now();
    cache.reset();
    const auto destroyElapsed = std::chrono::steady_clock::now() - destroyStarted;

    const int callsWhileBlocked = scan->callCount();
    scan->release();
    const bool finished = scan->waitUntilFinished();

    REQUIRE (first == std::vector<int> { 7 });
    REQUIRE (firstElapsed < std::chrono::seconds (1));
    REQUIRE (entered);
    REQUIRE (keptFallback);
    REQUIRE (callsWhileBlocked == 1);
    REQUIRE (destroyElapsed < std::chrono::seconds (1));
    REQUIRE (finished);
}

TEST_CASE (DetachedRefreshCache_PublishesCompletedScanAndHonoursRefreshInterval)
{
    DetachedRefreshCache<std::vector<int>> cache (std::vector<int> { 1 });
    auto scan = std::make_shared<BlockingScan>();

    const auto first = cache.getAndRefresh (std::chrono::hours (1), [scan]
    {
        return scan->run();
    });
    const bool entered = scan->waitUntilEntered();
    scan->release();
    const bool finished = scan->waitUntilFinished();

    // The scanner signals just before the cache publishes. Give that final
    // handoff a bounded chance to complete; the one-hour interval then means
    // these reads cannot launch another scan.
    std::vector<int> published;
    for (int attempt = 0; attempt < 100 && published != std::vector<int> { 99 }; ++attempt)
    {
        published = cache.getAndRefresh (std::chrono::hours (1), [scan]
        {
            return scan->run();
        });
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }

    REQUIRE (first == std::vector<int> { 1 });
    REQUIRE (entered);
    REQUIRE (finished);
    REQUIRE (published == std::vector<int> { 99 });
    REQUIRE (scan->callCount() == 1);
}

TEST_CASE (DetachedResultTask_OwnsAStuckWorkerAndNeverJoinsItsDestructor)
{
    auto scan = std::make_shared<BlockingScan>();
    auto task = std::make_unique<DetachedResultTask<std::vector<int>>>();

    REQUIRE (task->start ([scan] (const std::atomic<bool>&)
    {
        return scan->run();
    }));
    REQUIRE (scan->waitUntilEntered());

    const auto destroyStarted = std::chrono::steady_clock::now();
    task.reset();
    const auto destroyElapsed = std::chrono::steady_clock::now() - destroyStarted;

    REQUIRE (destroyElapsed < std::chrono::seconds (1));
    scan->release();
    REQUIRE (scan->waitUntilFinished());
}

TEST_CASE (DetachedResultTask_AllowsOnlyOneWorkerAndPublishesOnTheOwner)
{
    DetachedResultTask<std::vector<int>> task;
    auto scan = std::make_shared<BlockingScan>();

    REQUIRE (task.start ([scan] (const std::atomic<bool>&)
    {
        return scan->run();
    }));
    REQUIRE (scan->waitUntilEntered());
    REQUIRE_FALSE (task.start ([] (const std::atomic<bool>&)
    {
        return std::vector<int> { 100 };
    }));

    scan->release();
    REQUIRE (scan->waitUntilFinished());

    std::optional<std::vector<int>> result;
    for (int attempt = 0; attempt < 100 && ! result.has_value(); ++attempt)
    {
        result = task.takeResult();
        if (! result.has_value())
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }

    REQUIRE (result == std::optional<std::vector<int>> ({ 99 }));
    REQUIRE_FALSE (task.isRunning());
    REQUIRE_FALSE (task.takeResult().has_value());
}

TEST_CASE (DetachedResultTask_AbandonLetsANewOwnerProceedAndDropsTheLateOldResult)
{
    DetachedResultTask<std::vector<int>> task;
    auto stuck = std::make_shared<BlockingScan>();

    REQUIRE (task.start ([stuck] (const std::atomic<bool>&)
    {
        return stuck->run();
    }));
    REQUIRE (stuck->waitUntilEntered());

    task.abandon();
    REQUIRE (task.start ([] (const std::atomic<bool>&)
    {
        return std::vector<int> { 42 };
    }));

    std::optional<std::vector<int>> replacement;
    for (int attempt = 0; attempt < 100 && ! replacement.has_value(); ++attempt)
    {
        replacement = task.takeResult();
        if (! replacement.has_value())
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }

    REQUIRE (replacement == std::optional<std::vector<int>> ({ 42 }));

    stuck->release();
    REQUIRE (stuck->waitUntilFinished());
    std::this_thread::sleep_for (std::chrono::milliseconds (2));
    REQUIRE_FALSE (task.takeResult().has_value());
}

TEST_CASE (DetachedResultTask_AReleasedOldRecoveryCannotTouchATakeCreatedAfterTheNewGateSettles)
{
    DetachedResultTask<std::vector<int>> recovery;
    auto oldEnumeration = std::make_shared<BlockingScan>();
    std::atomic<bool> currentTakeExists { false };
    std::atomic<bool> oldScanRepairedCurrentTake { false };
    std::atomic<bool> oldWorkerFinished { false };

    REQUIRE (recovery.start (
        [oldEnumeration, &currentTakeExists, &oldScanRepairedCurrentTake,
         &oldWorkerFinished]
        (const std::atomic<bool>& cancelled)
        {
            (void) oldEnumeration->run();

            // This models the cancellation check between a filesystem call
            // returning and recovery mutating a candidate. The current take is
            // created only after the replacement destination's gate settles.
            if (! cancelled.load (std::memory_order_acquire)
                && currentTakeExists.load (std::memory_order_acquire))
                oldScanRepairedCurrentTake.store (true, std::memory_order_release);

            oldWorkerFinished.store (true, std::memory_order_release);
            return std::vector<int> { 1 };
        }));
    REQUIRE (oldEnumeration->waitUntilEntered());

    recovery.abandon();
    REQUIRE (recovery.start ([] (const std::atomic<bool>&)
    {
        return std::vector<int> { 2 };
    }));

    std::optional<std::vector<int>> replacement;
    for (int attempt = 0; attempt < 100 && ! replacement.has_value(); ++attempt)
    {
        replacement = recovery.takeResult();
        if (! replacement.has_value())
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    REQUIRE (replacement == std::optional<std::vector<int>> ({ 2 }));

    currentTakeExists.store (true, std::memory_order_release);
    oldEnumeration->release();
    REQUIRE (oldEnumeration->waitUntilFinished());

    for (int attempt = 0;
         attempt < 2000 && ! oldWorkerFinished.load (std::memory_order_acquire);
         ++attempt)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));

    REQUIRE (oldWorkerFinished.load (std::memory_order_acquire));
    REQUIRE_FALSE (oldScanRepairedCurrentTake.load (std::memory_order_acquire));
    REQUIRE_FALSE (recovery.takeResult().has_value());
}

TEST_CASE (DetachedPathMutationGate_SerializesEachRootAndOutlivesItsHandle)
{
    auto gate = std::make_unique<DetachedPathMutationGate>();
    auto first = gate->tryAcquire ({ "/card/RECORDINGS", "/card/RECORDINGS" });

    REQUIRE (first != nullptr);
    REQUIRE (gate->isActive ("/card/RECORDINGS"));
    REQUIRE (gate->tryAcquire ({ "/card/RECORDINGS" }) == nullptr);

    auto independent = gate->tryAcquire ({ "/Users/test/RECORDINGS-MIRROR" });
    REQUIRE (independent != nullptr);

    // Leases own the bookkeeping they need. Destroying the Application-side
    // handle never joins a worker or leaves its eventual lease release unsafe.
    gate.reset();
    first.reset();
    independent.reset();
    REQUIRE (true);
}

TEST_CASE (DetachedPathMutationGate_AbandonedWorkerPoisonsOnlyItsOwnRootUntilItReturns)
{
    DetachedPathMutationGate gate;
    DetachedResultTask<std::vector<int>> recovery;
    auto stuck = std::make_shared<BlockingScan>();
    auto oldLease = gate.tryAcquire ({ "/card/RECORDINGS" });

    REQUIRE (oldLease != nullptr);
    REQUIRE (recovery.start (
        [stuck, oldLease = std::move (oldLease)] (const std::atomic<bool>&)
        {
            (void) oldLease;
            return stuck->run();
        }));
    REQUIRE (stuck->waitUntilEntered());

    recovery.abandon();

    // The result owner has moved on, but the old worker can still return from
    // its syscall and mutate. Its original root therefore remains poisoned;
    // an unrelated destination remains usable immediately.
    REQUIRE (gate.tryAcquire ({ "/card/RECORDINGS" }) == nullptr);
    auto healthy = gate.tryAcquire ({ "/other-card/RECORDINGS" });
    REQUIRE (healthy != nullptr);
    healthy.reset();

    stuck->release();
    REQUIRE (stuck->waitUntilFinished());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (2);
    while (gate.isActive ("/card/RECORDINGS")
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));

    REQUIRE_FALSE (gate.isActive ("/card/RECORDINGS"));
    REQUIRE (gate.tryAcquire ({ "/card/RECORDINGS" }) != nullptr);
}

TEST_CASE (DetachedPathMutationGate_SerializesAncestorAndDescendantRootsLexically)
{
    DetachedPathMutationGate gate;
    auto take = gate.tryAcquire ({ "/card/RECORDINGS/show/take-1/" });

    REQUIRE (take != nullptr);
    REQUIRE (gate.isActive ("/card/RECORDINGS"));
    REQUIRE (gate.isActive ("/card/RECORDINGS/show/take-1/video.mov"));
    REQUIRE (gate.tryAcquire ({ "/card/RECORDINGS" }) == nullptr);
    REQUIRE (gate.tryAcquire ({ "/card/RECORDINGS/show/../show/take-1" }) == nullptr);

    // A textual prefix without a path-component boundary is a sibling, not an
    // ancestor. Independent cards/folders must remain usable in parallel.
    auto sibling = gate.tryAcquire ({ "/card/RECORDINGS-ARCHIVE" });
    REQUIRE (sibling != nullptr);
}

TEST_CASE (DetachedPathMutationGate_NormalizesSeparatorsDotsAndTrailingSlashes)
{
    DetachedPathMutationGate gate;
    auto first = gate.tryAcquire ({ "/Volumes//CARD/./RECORDINGS/" });

    REQUIRE (first != nullptr);
    REQUIRE (gate.tryAcquire ({ "/Volumes/CARD/RECORDINGS/take" }) == nullptr);

   #if defined(_WIN32) || defined(__APPLE__)
    REQUIRE (gate.tryAcquire ({ "/volumes/card/recordings" }) == nullptr);
   #endif

    DetachedPathMutationGate windowsGate;
    auto windows = windowsGate.tryAcquire ({ "E:\\RECORDINGS\\show" });
    REQUIRE (windows != nullptr);
    REQUIRE (windowsGate.tryAcquire ({ "e:/RECORDINGS" }) == nullptr);
}

TEST_CASE (DetachedResultTask_PollCannotObserveFinishedWithoutItsPublishedResult)
{
    DetachedResultTask<int> task;
    auto release = std::make_shared<std::atomic<bool>> (false);
    auto entered = std::make_shared<std::atomic<bool>> (false);

    REQUIRE (task.start ([release, entered] (const std::atomic<bool>&)
    {
        entered->store (true, std::memory_order_release);
        while (! release->load (std::memory_order_acquire))
            std::this_thread::yield();
        return 42;
    }));

    while (! entered->load (std::memory_order_acquire))
        std::this_thread::yield();

    auto running = task.poll();
    REQUIRE (running.running);
    REQUIRE_FALSE (running.result.has_value());

    release->store (true, std::memory_order_release);

    DetachedResultTask<int>::PollResult finished;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (2);
    do
    {
        finished = task.poll();
        if (! finished.running)
            break;
        std::this_thread::yield();
    }
    while (std::chrono::steady_clock::now() < deadline);

    REQUIRE_FALSE (finished.running);
    REQUIRE (finished.result == std::optional<int> (42));
}

TEST_CASE (DetachedPathMutationGate_CopiedHandleSharesWorkerOwnedLeases)
{
    DetachedPathMutationGate applicationGate;
    auto workerGate = applicationGate;
    auto workerLease = workerGate.tryAcquire ({ "/card/RECORDINGS" });

    REQUIRE (workerLease != nullptr);
    REQUIRE (applicationGate.isActive ("/card/RECORDINGS/take"));
    REQUIRE (applicationGate.tryAcquire ({ "/card/RECORDINGS" }) == nullptr);

    workerLease.reset();
    REQUIRE_FALSE (applicationGate.isActive ("/card/RECORDINGS"));
}
