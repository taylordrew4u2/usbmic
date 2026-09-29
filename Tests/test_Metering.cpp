#include "TestFramework.h"
#include "Core/Metering.h"
#include <vector>

using namespace mma;

TEST_CASE (Metering_StartsAtMinDb)
{
    Metering m (48000.0);
    REQUIRE_NEAR (m.getDisplayedLevelDb(), Metering::kMinDb, 1e-6);
}

TEST_CASE (Metering_RisesTowardFullScaleQuicklyOnAttack)
{
    Metering m (48000.0);
    m.pushBlockStats (1.0f, 128);
    // 10ms attack time-constant: after ~50ms (5 time constants) should be very close to 0dBFS.
    float level = 0.0f;
    for (int i = 0; i < 50; ++i)
        level = m.tick (0.001);
    REQUIRE (level > -1.0f);
}

TEST_CASE (Metering_DecaysSlowlyAfterSignalStops)
{
    Metering m (48000.0);
    m.pushBlockStats (1.0f, 128);
    for (int i = 0; i < 50; ++i)
        m.tick (0.001);
    REQUIRE (m.getDisplayedLevelDb() > -1.0f);

    m.pushBlockStats (0.0f, 128);
    float levelAfter10ms = m.tick (0.010);
    // 1.5s decay time-constant: 10ms in, level should barely have moved.
    REQUIRE (levelAfter10ms < 0.0f);
    REQUIRE (levelAfter10ms > -3.0f);
}

TEST_CASE (Metering_ClipLatchesAtThreeConsecutiveSamplesAboveThreshold)
{
    Metering m (48000.0);
    std::vector<float> samples = { 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f };
    m.processAudioBlock (samples.data(), static_cast<int> (samples.size()));
    REQUIRE (m.isClipped());
    REQUIRE (m.getClipCount() == 1);
}

TEST_CASE (Metering_DoesNotClipOnTwoConsecutiveSamples)
{
    Metering m (48000.0);
    std::vector<float> samples = { 1.0f, 1.0f, 0.0f, 0.0f };
    m.processAudioBlock (samples.data(), static_cast<int> (samples.size()));
    REQUIRE_FALSE (m.isClipped());
}

TEST_CASE (Metering_AcknowledgeClipClearsLatch)
{
    Metering m (48000.0);
    std::vector<float> samples = { 1.0f, 1.0f, 1.0f };
    m.processAudioBlock (samples.data(), static_cast<int> (samples.size()));
    REQUIRE (m.isClipped());
    m.acknowledgeClip();
    REQUIRE_FALSE (m.isClipped());
}

TEST_CASE (Metering_PeakHoldStaysUpThenDecaysAfterTwoSeconds)
{
    Metering m (48000.0);
    m.pushBlockStats (1.0f, 128); // peak at 0dBFS
    m.tick (0.001);
    REQUIRE_NEAR (m.getPeakHoldDb(), 0.0, 0.5);

    m.pushBlockStats (0.0f, 128);
    for (int i = 0; i < 190; ++i) // 1.9s, still within the 2s hold
        m.tick (0.010);
    REQUIRE_NEAR (m.getPeakHoldDb(), 0.0, 0.5);

    for (int i = 0; i < 200; ++i) // past the hold, now decaying at 20dB/s
        m.tick (0.010);
    REQUIRE (m.getPeakHoldDb() < 0.0f);
}

TEST_CASE (Metering_ShortPeakBetweenUiTicksStillReachesPeakHold)
{
    // At 64-sample buffers the audio thread delivers ~12 blocks per 60Hz UI
    // tick. A loud transient confined to one of them must still register.
    Metering m (48000.0);
    std::vector<float> loud (64, 0.0f);
    loud[10] = 1.0f;
    std::vector<float> quiet (64, 0.001f);

    m.processAudioBlock (loud.data(), static_cast<int> (loud.size()));
    for (int i = 0; i < 11; ++i)
        m.processAudioBlock (quiet.data(), static_cast<int> (quiet.size()));
    m.tick (1.0 / 60.0);

    REQUIRE (m.getPeakHoldDb() > -1.5f);
}

TEST_CASE (Metering_PeakConsumedByTickIsNotReportedAgain)
{
    // The max is taken since the last UI read, not for all time: once a tick
    // has seen the transient, later quiet blocks must be what the next tick sees.
    Metering m (48000.0);
    m.pushBlockStats (1.0f, 64);
    m.pushBlockStats (0.0f, 64);
    m.tick (1.0 / 60.0);
    m.pushBlockStats (0.0f, 64);
    for (int i = 0; i < 600; ++i) // 10s: hold expires and decays to the floor
        m.tick (1.0 / 60.0);
    REQUIRE (m.getPeakHoldDb() < -50.0f);
}
