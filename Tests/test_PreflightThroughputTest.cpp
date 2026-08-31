#include "TestFramework.h"
#include "Core/PreflightThroughputTest.h"

using namespace mma;

TEST_CASE (PreflightThroughputTest_RequiredRateFormula)
{
    // 4 channels, 48kHz, 3 bytes/sample (24-bit), x2 overhead.
    double required = PreflightThroughputTest::requiredBytesPerSecond (4, 48000.0, 3);
    REQUIRE_NEAR (required, 4.0 * 48000.0 * 3.0 * 2.0, 1e-6);
}

TEST_CASE (PreflightThroughputTest_SustainedMinimumIsMinNotAverage)
{
    std::vector<double> samples = { 100.0, 50.0, 90.0, 200.0 };
    REQUIRE_NEAR (PreflightThroughputTest::sustainedMinimum (samples), 50.0, 1e-9);
}

TEST_CASE (PreflightThroughputTest_PassesWhenSustainedIsAtLeastTwiceRequired)
{
    double required = PreflightThroughputTest::requiredBytesPerSecond (2, 48000.0, 2);
    std::vector<double> samples = { required * 2.5, required * 3.0 };
    auto result = PreflightThroughputTest::evaluate (samples, 2, 48000.0, 2);
    REQUIRE (result.passed);
}

TEST_CASE (PreflightThroughputTest_BlocksWhenUnderTwiceRequired)
{
    double required = PreflightThroughputTest::requiredBytesPerSecond (2, 48000.0, 2);
    std::vector<double> samples = { required * 1.5 };
    auto result = PreflightThroughputTest::evaluate (samples, 2, 48000.0, 2);
    REQUIRE_FALSE (result.passed);
    REQUIRE_FALSE (result.reason.empty());
}

TEST_CASE (PreflightThroughputTest_BlocksExactlyAtBoundaryJustBelow)
{
    double required = PreflightThroughputTest::requiredBytesPerSecond (1, 48000.0, 2);
    std::vector<double> samples = { required * 1.999 };
    auto result = PreflightThroughputTest::evaluate (samples, 1, 48000.0, 2);
    REQUIRE_FALSE (result.passed);
}

TEST_CASE (PreflightThroughputTest_PassesExactlyAtTwiceRequired)
{
    double required = PreflightThroughputTest::requiredBytesPerSecond (1, 48000.0, 2);
    std::vector<double> samples = { required * 2.0 };
    auto result = PreflightThroughputTest::evaluate (samples, 1, 48000.0, 2);
    REQUIRE (result.passed);
}

TEST_CASE (PreflightThroughputTest_FormatsRemainingTimeAsHoursMinutes)
{
    // 1 GB/s required, 3.5 hours of free space worth of bytes.
    double bytesPerSec = 1024.0 * 1024.0 * 1024.0;
    uint64_t freeBytes = static_cast<uint64_t> (bytesPerSec * 3600.0 * 3.5);
    std::string formatted = PreflightThroughputTest::formatRemainingTime (freeBytes, bytesPerSec);
    REQUIRE (formatted == "3h 30m");
}

TEST_CASE (PreflightThroughputTest_CacheExpiresAfter30Days)
{
    REQUIRE_FALSE (PreflightThroughputTest::isCacheExpired (29.9));
    REQUIRE (PreflightThroughputTest::isCacheExpired (30.0));
    REQUIRE (PreflightThroughputTest::isCacheExpired (31.0));
}

TEST_CASE (PreflightThroughputTest_FlagsFat32ForReformat)
{
    REQUIRE (PreflightThroughputTest::needsReformat (PreflightThroughputTest::FilesystemKind::FAT32));
    REQUIRE_FALSE (PreflightThroughputTest::needsReformat (PreflightThroughputTest::FilesystemKind::ExFAT));
}

TEST_CASE (PreflightThroughputTest_VideoAddsToTheRequiredRate)
{
    // Eight microphones, 24-bit, 48k: stems plus the mix file.
    const double audioOnly = PreflightThroughputTest::requiredBytesPerSecond (8, 48000.0, 3);
    REQUIRE (audioOnly == 8.0 * 48000.0 * 3.0 * 2.0);

    // A camera writes once, so it is added rather than doubled -- the x2 above
    // is the stems plus the mix, and there is only one copy of the video.
    const double withVideo = PreflightThroughputTest::requiredBytesPerSecond (8, 48000.0, 3, 4'000'000.0);
    REQUIRE (withVideo == audioOnly + 4'000'000.0);

    // The default keeps every existing caller on the audio-only figure.
    REQUIRE (PreflightThroughputTest::requiredBytesPerSecond (8, 48000.0, 3, 0.0) == audioOnly);
}

TEST_CASE (PreflightThroughputTest_ACardThatPassesForAudioCanFailOnceACameraIsOn)
{
    // This is the whole point of the change. The card is genuinely fast enough
    // for eight microphones and genuinely not fast enough for eight microphones
    // plus a camera, and §6.4 says that has to be refused before the take rather
    // than discovered during it.
    const double audioGate = PreflightThroughputTest::requiredBytesPerSecond (8, 48000.0, 3)
                           * PreflightThroughputTest::kRequiredMultiplier;
    const double measured = audioGate * 1.1;

    const auto audioOnly = PreflightThroughputTest::evaluateMeasured (measured, 8, 48000.0, 3);
    REQUIRE (audioOnly.passed);

    const auto withCamera = PreflightThroughputTest::evaluateMeasured (measured, 8, 48000.0, 3, 4'000'000.0);
    REQUIRE_FALSE (withCamera.passed);

    // §10.6: the way out is named, because a number alone does not tell someone
    // that the camera is what put them over.
    REQUIRE (withCamera.reason.find ("cameras off") != std::string::npos);
}

TEST_CASE (PreflightThroughputTest_ACardTooSlowForTheAudioAloneIsNotBlamedOnTheCamera)
{
    // Turning the cameras off would not save this one, so it must not be offered
    // as a fix -- §10.6 forbids advice that does not work.
    const auto result = PreflightThroughputTest::evaluateMeasured (1024.0, 8, 48000.0, 3, 4'000'000.0);

    REQUIRE_FALSE (result.passed);
    REQUIRE (result.reason.find ("cameras off") == std::string::npos);
}

TEST_CASE (PreflightThroughputTest_EvaluateAgreesWithTheMeasuredForm)
{
    // evaluate() is now the windowed front end of evaluateMeasured(); the two
    // must not drift apart.
    const std::vector<double> windows { 30e6, 24e6, 41e6 };

    const auto windowed = PreflightThroughputTest::evaluate (windows, 4, 48000.0, 3, 4'000'000.0);
    const auto measured = PreflightThroughputTest::evaluateMeasured (24e6, 4, 48000.0, 3, 4'000'000.0);

    REQUIRE (windowed.passed == measured.passed);
    REQUIRE (windowed.requiredBytesPerSec == measured.requiredBytesPerSec);
    REQUIRE (windowed.sustainedMinBytesPerSec == measured.sustainedMinBytesPerSec);
}
