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

TEST_CASE (SampleRate_AnInterfaceAlreadyAt44100StaysThere)
{
    // The reported failure, exactly. A PUPGSIS mixer advertises 44.1 and 48,
    // is running at 44.1, and refuses the switch -- so demanding 48 because it
    // is "higher" produced "couldn't be opened for recording" on a rig where
    // nothing was wrong. An inaudible rate difference is not worth a take that
    // does not happen.
    DeviceRateCapability mixer;
    mixer.deviceIndex = 0;
    mixer.supportedRates = { 44100, 48000 };
    mixer.currentRate = 44100;

    REQUIRE (SampleRateNegotiator::negotiate ({ mixer }).chosenRate == 44100);
}

TEST_CASE (SampleRate_AnInterfaceAlreadyAt48000AlsoStaysThere)
{
    // The rule is "stay put", not "prefer 44.1".
    DeviceRateCapability d;
    d.deviceIndex = 0;
    d.supportedRates = { 44100, 48000 };
    d.currentRate = 48000;

    REQUIRE (SampleRateNegotiator::negotiate ({ d }).chosenRate == 48000);
}

TEST_CASE (SampleRate_ARigAlreadyAgreeingOnOneRateKeepsIt)
{
    DeviceRateCapability a, b;
    a.deviceIndex = 0; a.supportedRates = { 44100, 48000 }; a.currentRate = 44100;
    b.deviceIndex = 1; b.supportedRates = { 44100, 48000 }; b.currentRate = 44100;

    REQUIRE (SampleRateNegotiator::negotiate ({ a, b }).chosenRate == 44100);
}

TEST_CASE (SampleRate_ARigThatDisagreesFallsBackToHighestCommon)
{
    // Something has to move whatever is picked, so the old rule stands and §3
    // resamples whichever device cannot follow.
    DeviceRateCapability a, b;
    a.deviceIndex = 0; a.supportedRates = { 44100, 48000 }; a.currentRate = 44100;
    b.deviceIndex = 1; b.supportedRates = { 44100, 48000 }; b.currentRate = 48000;

    REQUIRE (SampleRateNegotiator::negotiate ({ a, b }).chosenRate == 48000);
}

TEST_CASE (SampleRate_ACurrentRateNoOtherDeviceSupportsIsIgnored)
{
    // Staying put is only free when everyone can stay. 96000 is above the cap
    // and not common, so it cannot be the answer.
    DeviceRateCapability a, b;
    a.deviceIndex = 0; a.supportedRates = { 44100, 48000, 96000 }; a.currentRate = 96000;
    b.deviceIndex = 1; b.supportedRates = { 44100, 48000 };        b.currentRate = 48000;

    REQUIRE (SampleRateNegotiator::negotiate ({ a, b }).chosenRate == 48000);
}

TEST_CASE (SampleRate_ABackendThatCannotReportTheCurrentRateBehavesAsBefore)
{
    DeviceRateCapability d;
    d.deviceIndex = 0;
    d.supportedRates = { 44100, 48000 };
    d.currentRate = 0; // unknown

    REQUIRE (SampleRateNegotiator::negotiate ({ d }).chosenRate == 48000);
}
