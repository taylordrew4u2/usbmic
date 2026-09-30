#include "TestFramework.h"
#include "Core/FailedOpenRetry.h"

using namespace mma;

namespace
{
    FailedOpenRetry::Situation stuck()
    {
        FailedOpenRetry::Situation s;
        s.idle = true;
        s.includedMicCount = 1;
        s.monitoring = false;
        s.permissionDenied = false;
        return s;
    }

    // Ticks at the app's poll rate until the policy fires or `limit` passes.
    // Returns the seconds it took, or -1 if it never fired.
    double secondsUntilRetry (FailedOpenRetry& policy, const FailedOpenRetry::Situation& s,
                              double limit)
    {
        const double step = 0.25;
        for (double t = step; t <= limit + 1.0e-9; t += step)
            if (policy.tick (step, s))
                return t;
        return -1.0;
    }
}

TEST_CASE (FailedOpenRetry_RetriesAStuckRigWithBackoff)
{
    // Nothing open, idle, a mic selected: the rig the Record button is
    // disabled over. Nothing else ever reopens it (a rate change in Audio
    // MIDI Setup or another app dropping hog mode fires no device-list
    // notification), so the poll has to.
    FailedOpenRetry policy;
    const auto s = stuck();

    REQUIRE (std::abs (secondsUntilRetry (policy, s, 60.0) - 3.0) < 1.0e-6);
    REQUIRE (std::abs (secondsUntilRetry (policy, s, 60.0) - 6.0) < 1.0e-6);
    REQUIRE (std::abs (secondsUntilRetry (policy, s, 60.0) - 12.0) < 1.0e-6);
    REQUIRE (std::abs (secondsUntilRetry (policy, s, 60.0) - 24.0) < 1.0e-6);
    REQUIRE (std::abs (secondsUntilRetry (policy, s, 60.0) - 30.0) < 1.0e-6);
    REQUIRE (std::abs (secondsUntilRetry (policy, s, 60.0) - 30.0) < 1.0e-6);
}

TEST_CASE (FailedOpenRetry_NeverTearsDownOrInterruptsAnything)
{
    // Monitoring up (even with one mic failed): a reopen would close every
    // working stream and the headphones with it. Never on a timer.
    {
        FailedOpenRetry policy;
        auto s = stuck();
        s.monitoring = true;
        REQUIRE (secondsUntilRetry (policy, s, 120.0) < 0.0);
    }
    // A take running, or the last take's files still finishing.
    {
        FailedOpenRetry policy;
        auto s = stuck();
        s.idle = false;
        REQUIRE (secondsUntilRetry (policy, s, 120.0) < 0.0);
    }
    // No microphone selected: there is nothing to open.
    {
        FailedOpenRetry policy;
        auto s = stuck();
        s.includedMicCount = 0;
        REQUIRE (secondsUntilRetry (policy, s, 120.0) < 0.0);
    }
    // Access denied: the permission poll owns that case and reopens on grant.
    {
        FailedOpenRetry policy;
        auto s = stuck();
        s.permissionDenied = true;
        REQUIRE (secondsUntilRetry (policy, s, 120.0) < 0.0);
    }
}

TEST_CASE (FailedOpenRetry_BackoffStartsOverOnceTheRigOpens)
{
    FailedOpenRetry policy;
    const auto s = stuck();

    REQUIRE (secondsUntilRetry (policy, s, 60.0) > 0.0); // 3 s
    REQUIRE (secondsUntilRetry (policy, s, 60.0) > 0.0); // 6 s

    auto open = s;
    open.monitoring = true;
    REQUIRE_FALSE (policy.tick (0.25, open));

    // A later failure is a new problem: retried promptly again, not after
    // the long interval the previous one had backed off to.
    REQUIRE (std::abs (secondsUntilRetry (policy, s, 60.0) - 3.0) < 1.0e-6);
}

TEST_CASE (FailedOpenRetry_ATakeInBetweenAlsoResetsTheBackoff)
{
    FailedOpenRetry policy;
    const auto s = stuck();

    REQUIRE (secondsUntilRetry (policy, s, 60.0) > 0.0);
    REQUIRE (secondsUntilRetry (policy, s, 60.0) > 0.0);
    REQUIRE (secondsUntilRetry (policy, s, 60.0) > 0.0);

    auto busy = s;
    busy.idle = false;
    REQUIRE_FALSE (policy.tick (10.0, busy));

    REQUIRE (std::abs (secondsUntilRetry (policy, s, 60.0) - 3.0) < 1.0e-6);
}
