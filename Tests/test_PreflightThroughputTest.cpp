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
