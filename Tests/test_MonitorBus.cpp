#include "TestFramework.h"
#include "Core/MonitorBus.h"
#include <cmath>

using namespace mma;

TEST_CASE (MonitorBus_SumsChannelsAtUnity)
{
    MonitorBus bus (48000.0);
    float out = bus.processSample ({ 0.1f, 0.1f, 0.1f });
    REQUIRE_NEAR (out, 0.3, 1e-5);
}

TEST_CASE (MonitorBus_LimitsAboveMinus3dBFS)
{
    MonitorBus bus (48000.0);
    float out = bus.processSample ({ 0.9f, 0.9f, 0.9f }); // sums to 2.7, way over ceiling
    const float ceiling = std::pow (10.0f, MonitorBus::kLimiterCeilingDb / 20.0f);
    REQUIRE_NEAR (std::abs (out), ceiling, 1e-4);
}

TEST_CASE (MonitorBus_GlobalMuteSilencesOutput)
{
    MonitorBus bus (48000.0);
    bus.setGlobalMute (true);
    float out = bus.processSample ({ 0.5f });
    REQUIRE_NEAR (out, 0.0, 1e-9);
    REQUIRE (bus.isMuted());
}

TEST_CASE (MonitorBus_RunawayCutEngagesAfter500msOfContinuousLimiting)
{
    MonitorBus bus (48000.0);
    const int samplesFor500ms = static_cast<int> (48000.0 * 0.5);
    for (int i = 0; i < samplesFor500ms + 10; ++i)
        bus.processSample ({ 5.0f }); // sustained massive overload

    REQUIRE (bus.isRunawayMuted());
    REQUIRE (bus.isMuted());
}

TEST_CASE (MonitorBus_RunawayCutRequiresManualUnmute)
{
    MonitorBus bus (48000.0);
    const int samplesFor500ms = static_cast<int> (48000.0 * 0.5);
    for (int i = 0; i < samplesFor500ms + 10; ++i)
        bus.processSample ({ 5.0f });
    REQUIRE (bus.isRunawayMuted());

    // Even quiet input afterward doesn't clear it automatically.
    bus.processSample ({ 0.0f });
    REQUIRE (bus.isRunawayMuted());

    bus.manuallyUnmute();
    REQUIRE_FALSE (bus.isRunawayMuted());
}

TEST_CASE (MonitorBus_BriefLimitingDoesNotTriggerRunawayCut)
{
    MonitorBus bus (48000.0);
    for (int i = 0; i < 100; ++i) // ~2ms, well under 500ms
        bus.processSample ({ 5.0f });

    REQUIRE_FALSE (bus.isRunawayMuted());
}

TEST_CASE (MonitorBus_VolumeMappingIsMonotonicAndLogarithmic)
{
    float g0 = MonitorBus::monitorVolumeToLinearGain (0.0);
    float g50 = MonitorBus::monitorVolumeToLinearGain (50.0);
    float g70 = MonitorBus::monitorVolumeToLinearGain (MonitorBus::kDefaultMonitorVolume);
    float g100 = MonitorBus::monitorVolumeToLinearGain (100.0);

    REQUIRE_NEAR (g0, 0.0, 1e-9);
    REQUIRE (g50 < g70);
    REQUIRE (g70 < g100);
    REQUIRE_NEAR (g100, 1.0, 1e-4);
}

TEST_CASE (MonitorBus_TrimRangeClampedToPlusMinus20dB)
{
    float gainAt20 = MonitorBus::trimDbToLinearGain (20.0f);
    float gainAt100 = MonitorBus::trimDbToLinearGain (100.0f); // out of range, should clamp
    REQUIRE_NEAR (gainAt20, gainAt100, 1e-4);
}

TEST_CASE (MonitorBus_FeedbackDetectionTriggersOnSustainedNarrowbandRise)
{
    MonitorBus bus (48000.0);
    // Baseline band level close to broadband peak, then a >10dB rise over a
    // continuous 500ms window while staying within 6dB of the broadband peak.
    REQUIRE_FALSE (bus.processFeedbackCandidate (-40.0, -40.0, 0.0));
    REQUIRE_FALSE (bus.processFeedbackCandidate (-35.0, -35.0, 0.1));
    REQUIRE_FALSE (bus.processFeedbackCandidate (-30.0, -30.0, 0.1));
    REQUIRE_FALSE (bus.processFeedbackCandidate (-27.0, -27.0, 0.1));
    REQUIRE_FALSE (bus.processFeedbackCandidate (-25.0, -25.0, 0.1));
    bool triggered = bus.processFeedbackCandidate (-20.0, -20.0, 0.1);

    REQUIRE (triggered);
    REQUIRE (bus.isRunawayMuted());
}

TEST_CASE (MonitorBus_FeedbackDetectionDoesNotTriggerWhenFarFromBroadbandPeak)
{
    MonitorBus bus (48000.0);
    bool triggered = false;
    for (int i = 0; i < 10; ++i)
        triggered = bus.processFeedbackCandidate (-40.0 + i * 3.0, -5.0, 0.1); // always >6dB below peak

    REQUIRE_FALSE (triggered);
    REQUIRE_FALSE (bus.isRunawayMuted());
}
