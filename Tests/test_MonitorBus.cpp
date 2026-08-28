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

TEST_CASE (MonitorBus_DefaultMasterVolumeIsSeventy)
{
    MonitorBus bus (48000.0);
    REQUIRE_NEAR (bus.getMasterVolume(), MonitorBus::kDefaultMonitorVolume, 1e-9);
}

TEST_CASE (MonitorBus_MasterVolumeIsClampedToZeroHundred)
{
    MonitorBus bus (48000.0);

    bus.setMasterVolume (250.0);
    REQUIRE_NEAR (bus.getMasterVolume(), 100.0, 1e-9);

    bus.setMasterVolume (-40.0);
    REQUIRE_NEAR (bus.getMasterVolume(), 0.0, 1e-9);
}

TEST_CASE (MonitorBus_MasterVolumeNeverAffectsTheBusItself)
{
    // §5.4: nothing on the monitor bus but summing, trim and the safety limiter.
    // Changing the listening level must not move the summed value or the ceiling.
    MonitorBus loud (48000.0), quiet (48000.0);
    loud.setMasterVolume (100.0);
    quiet.setMasterVolume (5.0);

    const std::vector<float> in { 0.2f, 0.1f };
    REQUIRE_NEAR (loud.processSample (in), quiet.processSample (in), 1e-9);
}

TEST_CASE (MonitorBus_MasterVolumeScalesTheOutputStageMonotonically)
{
    MonitorBus bus (48000.0);

    bus.setMasterVolume (100.0);
    const float atFull = bus.applyMasterVolume (0.5f);

    bus.setMasterVolume (50.0);
    const float atHalf = bus.applyMasterVolume (0.5f);

    bus.setMasterVolume (0.0);
    const float atZero = bus.applyMasterVolume (0.5f);

    REQUIRE (atFull > atHalf);
    REQUIRE (atHalf > atZero);
    REQUIRE_NEAR (atZero, 0.0f, 1e-6);
}

TEST_CASE (MonitorBus_MasterVolumeAtFullIsUnityGain)
{
    MonitorBus bus (48000.0);
    bus.setMasterVolume (100.0);
    REQUIRE_NEAR (bus.applyMasterVolume (0.5f), 0.5f, 1e-4);
}

TEST_CASE (MonitorBus_SeparateBurstsDoNotCombineIntoARunawayCut)
{
    // Regression: kLimiterReleaseSeconds was declared and never used. Instead of
    // clearing 1 ms after the last clip, the engagement accumulator drained one
    // sample-time per quiet sample -- so it took as long to unwind as it took to
    // build. An engagement that ended long ago kept most of its credit, and the
    // next unrelated burst resumed from there.
    //
    // Two 300 ms bursts 10 ms apart is the case: neither is anywhere near the
    // §5 threshold of 500 ms *continuous* engagement, but the old accumulator
    // only shed 10 ms in the gap and crossed 500 ms partway through the second
    // burst -- cutting the monitor and forcing a manual unmute, mid-take, over
    // two ordinary loud moments.
    MonitorBus bus (48000.0);

    const int burstSamples = static_cast<int> (48000.0 * 0.3);   // 300 ms, under the threshold
    const int gapSamples   = static_cast<int> (48000.0 * 0.01);  // 10 ms, well over the 1 ms release

    for (int i = 0; i < burstSamples; ++i)
        bus.processSample ({ 5.0f });

    REQUIRE_FALSE (bus.isRunawayMuted());

    for (int i = 0; i < gapSamples; ++i)
        bus.processSample ({ 0.0f });

    for (int i = 0; i < burstSamples; ++i)
        bus.processSample ({ 5.0f });

    // The gap ended the first engagement outright, so the second burst is judged
    // on its own 300 ms and must not cut.
    REQUIRE_FALSE (bus.isRunawayMuted());
}

TEST_CASE (MonitorBus_ClippingSurvivesGapsShorterThanTheRelease)
{
    // The flip side: a gap shorter than the 1 ms release is part of the same
    // engagement, so genuinely sustained overload still cuts even though it is
    // not clipping on literally every sample.
    MonitorBus bus (48000.0);

    // 10 clipped samples, then 4 quiet ones (~83 us, well inside the 1 ms
    // release), repeated well past 500 ms of accumulated engagement.
    const int cycles = 60000;
    for (int c = 0; c < cycles && ! bus.isRunawayMuted(); ++c)
    {
        for (int i = 0; i < 10; ++i)
            bus.processSample ({ 5.0f });
        for (int i = 0; i < 4; ++i)
            bus.processSample ({ 0.0f });
    }

    REQUIRE (bus.isRunawayMuted());
}

TEST_CASE (MonitorBus_MasterVolumeGainTracksTheSetting)
{
    // applyMasterVolume now reads a cached gain rather than recomputing a
    // std::pow per sample; the cache must follow every setMasterVolume call.
    MonitorBus bus (48000.0);

    bus.setMasterVolume (100.0);
    REQUIRE_NEAR (bus.applyMasterVolume (1.0f), 1.0f, 1e-4);

    bus.setMasterVolume (0.0);
    REQUIRE_NEAR (bus.applyMasterVolume (1.0f), 0.0f, 1e-6);

    bus.setMasterVolume (70.0);
    REQUIRE_NEAR (bus.applyMasterVolume (1.0f),
                  MonitorBus::monitorVolumeToLinearGain (70.0), 1e-6);
}
