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

TEST_CASE (SampleRate_AMicrophoneNobodyIsRecordingGetsNoVote)
{
    // The reported rig: a PUPGSIS mixer at 44.1 kHz that the take uses, and a
    // MacBook built-in microphone at 48 kHz that it does not. Letting the
    // built-in vote made the rig "disagree", which fell back to highest-common
    // 48 kHz -- on hardware locked at 44.1, so the take would not open at all.
    const std::vector<EnumeratedDeviceRates> enumerated {
        { "mixer",    { 44100, 48000 }, 44100 },
        { "built-in", { 44100, 48000 }, 48000 },
    };

    const auto voting = SampleRateNegotiator::votingDevices ({ "mixer" }, enumerated);

    REQUIRE (voting.size() == 1);
    REQUIRE (SampleRateNegotiator::negotiate (voting).chosenRate == 44100);

    // And with the built-in included, something must move whatever is picked,
    // so the old rule stands.
    const auto both = SampleRateNegotiator::votingDevices ({ "mixer", "built-in" }, enumerated);

    REQUIRE (both.size() == 2);
    REQUIRE (SampleRateNegotiator::negotiate (both).chosenRate == 48000);
}

TEST_CASE (SampleRate_AnUnknownDeviceKeyIsSkippedRatherThanGuessed)
{
    const std::vector<EnumeratedDeviceRates> enumerated { { "mixer", { 48000 }, 48000 } };

    REQUIRE (SampleRateNegotiator::votingDevices ({ "gone" }, enumerated).empty());
}

TEST_CASE (SampleRate_TheRateADeviceIsRunningAtCountsAsSupported)
{
    // An interface that advertises only 48 kHz while sitting at 44.1 is doing
    // 44.1 -- it is running at it. Taking the advertised list as the whole
    // truth ruled out the one rate guaranteed to work.
    DeviceRateCapability d;
    d.deviceIndex = 0;
    d.supportedRates = { 48000 };
    d.currentRate = 44100;

    REQUIRE (SampleRateNegotiator::negotiate ({ d }).chosenRate == 44100);
}
