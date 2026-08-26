#include "TestFramework.h"
#include "Core/SampleRateNegotiator.h"

using namespace mma;

TEST_CASE (SampleRateNegotiator_ChoosesHighestCommonRateCappedAt48k)
{
    std::vector<DeviceRateCapability> devices = {
        { 0, { 44100, 48000, 96000 } },
        { 1, { 48000, 96000 } },
        { 2, { 44100, 48000 } },
    };

    auto result = SampleRateNegotiator::negotiate (devices);
    REQUIRE (result.chosenRate == 48000);
    REQUIRE (result.devicesNeedingResample.empty());
}

TEST_CASE (SampleRateNegotiator_CapsAt48kEvenIfAllSupportHigher)
{
    std::vector<DeviceRateCapability> devices = {
        { 0, { 48000, 96000, 192000 } },
        { 1, { 48000, 96000 } },
    };

    auto result = SampleRateNegotiator::negotiate (devices);
    REQUIRE (result.chosenRate == 48000);
}

TEST_CASE (SampleRateNegotiator_NeverRejectsADeviceForRateMismatch)
{
    std::vector<DeviceRateCapability> devices = {
        { 0, { 44100 } },
        { 1, { 48000 } },
    };

    auto result = SampleRateNegotiator::negotiate (devices);
    // No common rate under 48k; a rate is still chosen, and mismatched devices
    // are flagged for resampling rather than excluded.
    REQUIRE (result.chosenRate > 0);
    REQUIRE_FALSE (result.devicesNeedingResample.empty());
}

TEST_CASE (SampleRateNegotiator_FlagsDevicesNeedingResample)
{
    // Device 1 tops out at 96k with no 48k support; device 0 can do 48k.
    // Highest common rate is 44100 -- wait, no common rate under the cap
    // between {48000,96000} and {44100} -- so a rate is still chosen and the
    // mismatched device is flagged, never excluded.
    std::vector<DeviceRateCapability> devices = {
        { 0, { 48000, 96000 } },
        { 1, { 44100 } },
    };

    auto result = SampleRateNegotiator::negotiate (devices);
    REQUIRE (result.chosenRate > 0);
    REQUIRE (result.devicesNeedingResample.size() == 1);
    REQUIRE (result.devicesNeedingResample[0] == 1);
}

TEST_CASE (SampleRateNegotiator_EmptyDeviceListDefaultsToCap)
{
    std::vector<DeviceRateCapability> devices;
    auto result = SampleRateNegotiator::negotiate (devices);
    REQUIRE (result.chosenRate == SampleRateNegotiator::kRateCap);
}
