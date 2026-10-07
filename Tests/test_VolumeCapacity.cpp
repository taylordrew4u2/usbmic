#include "TestFramework.h"
#include "Platform/VolumeCapacity.h"

#include <filesystem>

using namespace mma;

// APFS's plain free figure leaves out purgeable space macOS frees the moment a
// write needs it, so a Mac showing 50 GB available in Finder could be told its
// drive was full. The "important usage" figure counts that space and wins
// whenever the system gives one.
TEST_CASE (VolumeCapacity_PurgeableSpaceCountsWhenTheSystemReportsIt)
{
    constexpr uint64_t basic = 300ull * 1000 * 1000;
    constexpr int64_t important = 50ll * 1000 * 1000 * 1000;

    REQUIRE (volumecapacity::choose (basic, important) == static_cast<uint64_t> (important));
}

TEST_CASE (VolumeCapacity_FallsBackWhenTheImportantUsageFigureIsMissingOrZero)
{
    constexpr uint64_t basic = 300ull * 1000 * 1000;

    REQUIRE (volumecapacity::choose (basic, std::nullopt) == basic);
    REQUIRE (volumecapacity::choose (basic, int64_t { 0 }) == basic);
    REQUIRE (volumecapacity::choose (basic, int64_t { -1 }) == basic);
}

TEST_CASE (VolumeCapacity_OffTheMacItIsExactlyTheFilesystemFigure)
{
    const auto here = std::filesystem::temp_directory_path();
    const auto available = availableBytesForRecording (here.string());
    REQUIRE (available.has_value());

#if ! defined (__APPLE__)
    // Free space moves under a running system; the two reads are moments
    // apart, so allow a little drift rather than demanding identity.
    const auto direct = static_cast<uint64_t> (std::filesystem::space (here).available);
    const auto drift = *available > direct ? *available - direct : direct - *available;
    REQUIRE (drift < 64ull * 1024 * 1024);
#endif

    REQUIRE_FALSE (availableBytesForRecording (
        (here / "sobstage-no-such-folder" / "deeper").string()).has_value());
}

// The backup folder is judged before it exists, so a path that is not there
// yet must answer for the folder it will be made in.
TEST_CASE (VolumeCapacity_AFolderNotYetMadeIsOnItsParentsDisk)
{
    const auto here = std::filesystem::temp_directory_path();
    const auto existing = volumeIdentityOf (here.string());
    const auto notYet = volumeIdentityOf ((here / "sobstage-not-made-yet" / "RECORDINGS-MIRROR").string());

    REQUIRE_FALSE (existing.device.empty());
    REQUIRE (notYet.device == existing.device);
    REQUIRE (mirrorplacement::sharesDisk (existing, notYet));
}

#if defined (__linux__)
// /proc is its own filesystem on every Linux, so it is a dependable "some
// other volume" for the negative case.
TEST_CASE (VolumeCapacity_TwoFilesystemsAreTwoDisks)
{
    const auto temp = volumeIdentityOf (std::filesystem::temp_directory_path().string());
    const auto proc = volumeIdentityOf ("/proc");

    REQUIRE_FALSE (proc.device.empty());
    REQUIRE_FALSE (mirrorplacement::sharesDisk (temp, proc));
}
#endif
