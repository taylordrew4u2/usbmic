#include "TestFramework.h"
#include "Platform/SleepInhibitor.h"

#include <memory>

using namespace mma;

namespace {

struct Counts
{
    int acquires = 0;
    int releases = 0;
    bool acquireSucceeds = true;
    bool heldByOs = false;
};

class FakeBackend final : public SleepInhibitor::Backend
{
public:
    explicit FakeBackend (Counts& c) : counts (c) {}

    bool acquire() override
    {
        ++counts.acquires;
        counts.heldByOs = counts.acquireSucceeds;
        return counts.acquireSucceeds;
    }

    void release() override
    {
        ++counts.releases;
        counts.heldByOs = false;
    }

private:
    Counts& counts;
};

} // namespace

TEST_CASE (SleepInhibitor_HeldWhileRecordingReleasedOnStop)
{
    Counts counts;
    SleepInhibitor inhibitor (std::make_unique<FakeBackend> (counts));

    REQUIRE_FALSE (inhibitor.isHeld());

    inhibitor.setHeld (true);   // take starts
    REQUIRE (inhibitor.isHeld());
    REQUIRE (counts.heldByOs);

    inhibitor.setHeld (false);  // take stops
    REQUIRE_FALSE (inhibitor.isHeld());
    REQUIRE_FALSE (counts.heldByOs);
    REQUIRE (counts.acquires == 1);
    REQUIRE (counts.releases == 1);
}

TEST_CASE (SleepInhibitor_RepeatedTicksDoNotStackAssertions)
{
    // The app mirrors "is recording" into it from its status tick, so the
    // same answer arrives many times; it must not acquire or release again.
    Counts counts;
    SleepInhibitor inhibitor (std::make_unique<FakeBackend> (counts));

    for (int i = 0; i < 5; ++i)
        inhibitor.setHeld (true);
    for (int i = 0; i < 5; ++i)
        inhibitor.setHeld (false);

    REQUIRE (counts.acquires == 1);
    REQUIRE (counts.releases == 1);
}

TEST_CASE (SleepInhibitor_QuitMidTakeReleases)
{
    Counts counts;
    {
        SleepInhibitor inhibitor (std::make_unique<FakeBackend> (counts));
        inhibitor.setHeld (true);
    }
    REQUIRE (counts.releases == 1);
    REQUIRE_FALSE (counts.heldByOs);
}

TEST_CASE (SleepInhibitor_FailedAcquireIsRetriedAndNeverReleased)
{
    Counts counts;
    counts.acquireSucceeds = false;
    {
        SleepInhibitor inhibitor (std::make_unique<FakeBackend> (counts));
        inhibitor.setHeld (true);
        REQUIRE_FALSE (inhibitor.isHeld());

        counts.acquireSucceeds = true;
        inhibitor.setHeld (true);
        REQUIRE (inhibitor.isHeld());
        REQUIRE (counts.acquires == 2);
    }
    REQUIRE (counts.releases == 1);
}

TEST_CASE (SleepInhibitor_PlatformBackendIsSafeEverywhere)
{
    // Off the Mac this is a no-op; on the Mac it takes and drops a real
    // assertion. Either way it must not throw or leave anything held.
    SleepInhibitor inhibitor;
    inhibitor.setHeld (true);
#if ! defined(__APPLE__)
    REQUIRE_FALSE (inhibitor.isHeld());
#endif
    inhibitor.setHeld (false);
    REQUIRE_FALSE (inhibitor.isHeld());
}
