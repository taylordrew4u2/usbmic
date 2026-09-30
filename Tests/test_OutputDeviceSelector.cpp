#include "TestFramework.h"
#include "Core/OutputDeviceSelector.h"

using namespace mma;

namespace {

OutputDeviceCandidate makeDevice (const std::string& id)
{
    OutputDeviceCandidate d;
    d.id = id;
    d.displayName = id;
    return d;
}

} // namespace

TEST_CASE (OutputDeviceSelector_PrefersTheRememberedDeviceWhenPresent)
{
    auto remembered = makeDevice ("remembered");
    auto jack = makeDevice ("jack");
    jack.hasPhysicalHeadphoneJack = true;
    auto fresh = makeDevice ("fresh");
    fresh.appearedAfterLaunch = true;

    auto result = OutputDeviceSelector::select ({ jack, fresh, remembered }, "remembered");

    REQUIRE (result.found);
    REQUIRE (result.id == "remembered");
    REQUIRE (result.displayName == "remembered");
    REQUIRE (result.reason == OutputSelectionReason::RememberedFromPreviousSession);
}

TEST_CASE (OutputDeviceTracker_DoesNotCallTheLaunchSnapshotNew)
{
    OutputDeviceTracker tracker;
    const auto first = tracker.observe ({ makeDevice ("built-in"), makeDevice ("amp") });

    REQUIRE_FALSE (first[0].appearedAfterLaunch);
    REQUIRE_FALSE (first[1].appearedAfterLaunch);
    REQUIRE (first[0].connectionOrder < first[1].connectionOrder);
}

TEST_CASE (OutputDeviceTracker_OnlyCallsAnActualArrivalNew)
{
    OutputDeviceTracker tracker;
    tracker.observe ({ makeDevice ("built-in"), makeDevice ("amp") });

    const auto unchanged = tracker.observe ({ makeDevice ("amp"), makeDevice ("built-in") });
    REQUIRE_FALSE (unchanged[0].appearedAfterLaunch);
    REQUIRE_FALSE (unchanged[1].appearedAfterLaunch);

    const auto withDock = tracker.observe ({ makeDevice ("built-in"), makeDevice ("amp"),
                                             makeDevice ("dock") });
    REQUIRE_FALSE (withDock[0].appearedAfterLaunch);
    REQUIRE_FALSE (withDock[1].appearedAfterLaunch);
    REQUIRE (withDock[2].appearedAfterLaunch);
}

TEST_CASE (OutputDeviceTracker_KeepsAnArrivalNewUntilThatConnectionLeaves)
{
    OutputDeviceTracker tracker;
    tracker.observe ({ makeDevice ("built-in") });

    const auto arrived = tracker.observe ({ makeDevice ("built-in"), makeDevice ("dock") });
    REQUIRE (arrived[1].appearedAfterLaunch);
    const auto arrivalOrder = arrived[1].connectionOrder;

    // An input-device notification can cause an identical output snapshot.
    // The dock is still the most recently connected output and must retain
    // priority two until it is physically removed.
    const auto unchanged = tracker.observe ({ makeDevice ("dock"), makeDevice ("built-in") });
    REQUIRE (unchanged[0].appearedAfterLaunch);
    REQUIRE (unchanged[0].connectionOrder == arrivalOrder);

    const auto selection = OutputDeviceSelector::select (unchanged, {});
    REQUIRE (selection.id == std::string ("dock"));
    REQUIRE (selection.reason == OutputSelectionReason::NewlyConnected);
}

TEST_CASE (OutputDeviceTracker_ReconnectedDeviceIsANewArrival)
{
    OutputDeviceTracker tracker;
    tracker.observe ({ makeDevice ("amp") });
    tracker.observe ({});

    const auto reconnected = tracker.observe ({ makeDevice ("amp") });
    REQUIRE (reconnected[0].appearedAfterLaunch);
}

TEST_CASE (OutputDeviceSelector_FallsPastARememberedDeviceThatIsAbsent)
{
    auto jack = makeDevice ("jack");
    jack.hasPhysicalHeadphoneJack = true;

    auto result = OutputDeviceSelector::select ({ jack }, "not-plugged-in");

    REQUIRE (result.found);
    REQUIRE (result.id == "jack");
    REQUIRE (result.reason == OutputSelectionReason::PhysicalHeadphoneJack);
}

TEST_CASE (OutputDeviceSelector_PrefersANewlyConnectedDeviceOverAHeadphoneJack)
{
    auto jack = makeDevice ("jack");
    jack.hasPhysicalHeadphoneJack = true;
    auto fresh = makeDevice ("fresh");
    fresh.appearedAfterLaunch = true;

    auto result = OutputDeviceSelector::select ({ jack, fresh }, "");

    REQUIRE (result.found);
    REQUIRE (result.id == "fresh");
    REQUIRE (result.reason == OutputSelectionReason::NewlyConnected);
}

TEST_CASE (OutputDeviceSelector_TakesTheMostRecentlyConnectedDevice)
{
    auto first = makeDevice ("first");
    first.appearedAfterLaunch = true;
    first.connectionOrder = 1;
    auto second = makeDevice ("second");
    second.appearedAfterLaunch = true;
    second.connectionOrder = 2;

    auto result = OutputDeviceSelector::select ({ first, second }, "");

    REQUIRE (result.id == "second");
}

TEST_CASE (OutputDeviceSelector_FallsBackToSystemDefault)
{
    auto plain = makeDevice ("plain");
    auto def = makeDevice ("default");
    def.isSystemDefault = true;

    auto result = OutputDeviceSelector::select ({ plain, def }, "");

    REQUIRE (result.found);
    REQUIRE (result.id == "default");
    REQUIRE (result.reason == OutputSelectionReason::SystemDefault);
}

TEST_CASE (OutputDeviceSelector_PrefersBuiltInOverAnArbitraryCaptureCardOutput)
{
    // The real failure: the mixer is both the selected input and the OS default
    // output, so feedback protection correctly excludes it. CoreAudio then
    // listed an HDMI capture-card playback endpoint before the Mac speakers.
    // Picking that arbitrary 48 kHz endpoint made a 44.1 kHz recording fail
    // before SobStage even reached the microphone streams.
    auto captureCard = makeDevice ("capture-card-audio");

    auto mixer = makeDevice ("selected-mixer");
    mixer.isAlsoSelectedInput = true;
    mixer.isSystemDefault = true;

    auto computer = makeDevice ("mac-speakers");
    computer.isBuiltIn = true;

    const auto result = OutputDeviceSelector::select ({ captureCard, mixer, computer }, "");

    REQUIRE (result.found);
    REQUIRE (result.id == "mac-speakers");
    REQUIRE (result.reason == OutputSelectionReason::BuiltInOutput);

    // The same capture card arriving later would normally outrank every
    // automatic fallback. Positive knowledge that it cannot run at this
    // recording rate excludes it before priorities are evaluated.
    captureCard.appearedAfterLaunch = true;
    captureCard.connectionOrder = 3;
    captureCard.supportsRecordingSampleRate = false;

    const auto afterHotPlug = OutputDeviceSelector::select (
        { computer, mixer, captureCard }, "");

    REQUIRE (afterHotPlug.found);
    REQUIRE (afterHotPlug.id == "mac-speakers");
    REQUIRE (afterHotPlug.reason == OutputSelectionReason::BuiltInOutput);
}

TEST_CASE (OutputDeviceSelector_NeverSelectsAMicrophonePlaybackEndpoint)
{
    // §5.2: the mic jacks carry non-defeatable analog direct monitoring, so they
    // are excluded even when they would otherwise win on every priority.
    auto mic = makeDevice ("yeti-headphone-out");
    mic.isMicrophonePlaybackEndpoint = true;
    mic.hasPhysicalHeadphoneJack = true;
    mic.isSystemDefault = true;
    mic.appearedAfterLaunch = true;

    auto result = OutputDeviceSelector::select ({ mic }, "yeti-headphone-out");

    REQUIRE_FALSE (result.found);
    REQUIRE_FALSE (result.explanation.empty());
}

TEST_CASE (OutputDeviceSelector_NeverRoutesToADeviceThatIsAlsoASelectedInput)
{
    // §5.5: output into an active input is a feedback loop by construction.
    auto loopback = makeDevice ("loopback");
    loopback.isAlsoSelectedInput = true;
    loopback.isSystemDefault = true;

    auto result = OutputDeviceSelector::select ({ loopback }, "");

    REQUIRE_FALSE (result.found);
}

TEST_CASE (OutputDeviceSelector_SkipsIneligibleDevicesButStillPicksAnEligibleOne)
{
    auto mic = makeDevice ("mic");
    mic.isMicrophonePlaybackEndpoint = true;
    mic.appearedAfterLaunch = true;
    mic.connectionOrder = 9;

    auto amp = makeDevice ("amp");
    amp.hasPhysicalHeadphoneJack = true;

    auto result = OutputDeviceSelector::select ({ mic, amp }, "");

    REQUIRE (result.found);
    REQUIRE (result.id == "amp");
}

TEST_CASE (OutputDeviceSelector_ExplainsItselfWhenThereAreNoDevicesAtAll)
{
    auto result = OutputDeviceSelector::select ({}, "");

    REQUIRE_FALSE (result.found);
    REQUIRE_FALSE (result.explanation.empty());
}

TEST_CASE (OutputDeviceSelector_EligibilityRuleIsExplicit)
{
    auto ok = makeDevice ("ok");
    REQUIRE (OutputDeviceSelector::isEligible (ok));

    auto micEndpoint = makeDevice ("mic");
    micEndpoint.isMicrophonePlaybackEndpoint = true;
    REQUIRE_FALSE (OutputDeviceSelector::isEligible (micEndpoint));

    auto alsoInput = makeDevice ("input");
    alsoInput.isAlsoSelectedInput = true;
    REQUIRE_FALSE (OutputDeviceSelector::isEligible (alsoInput));

    auto wrongRate = makeDevice ("fixed-48k-hdmi");
    wrongRate.supportsRecordingSampleRate = false;
    REQUIRE_FALSE (OutputDeviceSelector::isEligible (wrongRate));

    const auto noCompatibleOutput = OutputDeviceSelector::select ({ wrongRate }, "");
    REQUIRE_FALSE (noCompatibleOutput.found);
    REQUIRE (noCompatibleOutput.explanation.find ("recording's sample rate")
             != std::string::npos);

    // Some drivers report a stale/incomplete supported range while their
    // nominal clock is already running at the requested rate. Do not reject a
    // working output on the weaker fact.
    REQUIRE (OutputDeviceSelector::supportsRecordingRate (44100, { 48000 }, 44100));
    REQUIRE (OutputDeviceSelector::supportsRecordingRate (48000, {}, 44100));
    REQUIRE_FALSE (OutputDeviceSelector::supportsRecordingRate (48000, { 48000 }, 44100));
}

TEST_CASE (OutputDeviceSelector_MacBookHeadphoneJackBeatsTheSpeakersInEitherOrder)
{
    // Apple Silicon lists "MacBook Air Speakers" and "External Headphones" as
    // two built-in devices, both present at launch. Enumeration order must not
    // decide which one the live-mic mix plays through.
    auto speakers = makeDevice ("BuiltInSpeakerDevice");
    speakers.isBuiltIn = true;
    auto headphones = makeDevice ("BuiltInHeadphoneOutputDevice");
    headphones.isBuiltIn = true;
    headphones.hasPhysicalHeadphoneJack = true;

    for (const auto& order : { std::vector<OutputDeviceCandidate> { speakers, headphones },
                               std::vector<OutputDeviceCandidate> { headphones, speakers } })
    {
        OutputDeviceTracker tracker;
        const auto result = OutputDeviceSelector::select (tracker.observe (order), "");

        REQUIRE (result.found);
        REQUIRE (result.id == "BuiltInHeadphoneOutputDevice");
        REQUIRE (result.reason == OutputSelectionReason::PhysicalHeadphoneJack);
    }
}

TEST_CASE (OutputDeviceSelector_MacDefaultUsbAmpBeatsTheBuiltInSpeakers)
{
    auto speakers = makeDevice ("speakers");
    speakers.isBuiltIn = true;
    auto amp = makeDevice ("usb-amp");
    amp.isSystemDefault = true;

    OutputDeviceTracker tracker;
    const auto result = OutputDeviceSelector::select (tracker.observe ({ speakers, amp }), "");

    REQUIRE (result.found);
    REQUIRE (result.id == "usb-amp");
    REQUIRE (result.reason == OutputSelectionReason::SystemDefault);
}

TEST_CASE (OutputDeviceSelector_AWirelessArrivalDoesNotTakeTheMonitor)
{
    // AirPods reconnecting, or a paired Bluetooth speaker powering on, is not
    // the user plugging in headphones.
    auto amp = makeDevice ("amp");
    amp.isBuiltIn = true;
    auto airpods = makeDevice ("airpods");
    airpods.isWireless = true;

    OutputDeviceTracker tracker;
    tracker.observe ({ amp });
    const auto candidates = tracker.observe ({ amp, airpods });
    REQUIRE (candidates[1].appearedAfterLaunch);

    const auto automatic = OutputDeviceSelector::select (candidates, "");
    REQUIRE (automatic.found);
    REQUIRE (automatic.id == "amp");
    REQUIRE (automatic.reason == OutputSelectionReason::BuiltInOutput);

    // An explicit pick of the wireless device is still honoured.
    const auto explicitPick = OutputDeviceSelector::select (candidates, "airpods");
    REQUIRE (explicitPick.id == "airpods");
    REQUIRE (explicitPick.reason == OutputSelectionReason::RememberedFromPreviousSession);

    // macOS often makes the arriving headset its default; that does not move
    // the monitor off a wired output either.
    auto defaultAirpods = airpods;
    defaultAirpods.isSystemDefault = true;
    const auto asDefault = OutputDeviceSelector::select (tracker.observe ({ amp, defaultAirpods }), "");
    REQUIRE (asDefault.id == "amp");
}

TEST_CASE (OutputDeviceSelector_AWirelessArrivalDoesNotDisplaceANewlyConnectedAmp)
{
    auto speakers = makeDevice ("speakers");
    speakers.isBuiltIn = true;
    auto amp = makeDevice ("usb-amp");
    auto speaker = makeDevice ("bluetooth-speaker");
    speaker.isWireless = true;

    OutputDeviceTracker tracker;
    tracker.observe ({ speakers });
    tracker.observe ({ speakers, amp });
    const auto candidates = tracker.observe ({ speakers, amp, speaker });

    const auto result = OutputDeviceSelector::select (candidates, "");
    REQUIRE (result.id == "usb-amp");
    REQUIRE (result.reason == OutputSelectionReason::NewlyConnected);
}

TEST_CASE (OutputDeviceSelector_WirelessOnlyStillGetsAMonitor)
{
    auto airpods = makeDevice ("airpods");
    airpods.isWireless = true;
    airpods.isSystemDefault = true;

    const auto result = OutputDeviceSelector::select ({ airpods }, "");
    REQUIRE (result.found);
    REQUIRE (result.id == "airpods");
}

TEST_CASE (OutputDeviceSelector_AnUnrecordedMicsOutputIsNeverChosenAutomatically)
{
    // A Yeti switched off in Settings (or the 9th mic) is plugged in. Its
    // headphone jack shares the mic's id and just "appeared", but §5.2 keeps a
    // microphone's playback endpoint out at every priority.
    auto builtIn = makeDevice ("built-in");
    builtIn.isBuiltIn = true;

    auto amp = makeDevice ("amp");
    amp.appearedAfterLaunch = true;
    amp.connectionOrder = 2;

    auto yeti = makeDevice ("yeti");
    yeti.appearedAfterLaunch = true;
    yeti.connectionOrder = 3;
    yeti.belongsToUnrecordedMicrophone = true;
    yeti.hasPhysicalHeadphoneJack = true;
    yeti.isSystemDefault = true;

    const auto withAmp = OutputDeviceSelector::select ({ builtIn, amp, yeti }, "");
    REQUIRE (withAmp.id == "amp");
    REQUIRE (withAmp.reason == OutputSelectionReason::NewlyConnected);

    // Still pickable by hand: this is how an interface's headphone output is
    // used after unticking its inputs.
    REQUIRE (OutputDeviceSelector::isEligible (yeti));
    const auto explicitPick = OutputDeviceSelector::select ({ builtIn, amp, yeti }, "yeti");
    REQUIRE (explicitPick.id == "yeti");
    REQUIRE (explicitPick.reason == OutputSelectionReason::RememberedFromPreviousSession);

    const auto withoutAmp = OutputDeviceSelector::select ({ builtIn, yeti }, "");
    REQUIRE (withoutAmp.id == "built-in");

    const auto onlyYeti = OutputDeviceSelector::select ({ yeti }, "");
    REQUIRE_FALSE (onlyYeti.found);
    REQUIRE_FALSE (onlyYeti.explanation.empty());
}

TEST_CASE (OutputDeviceSelector_CandidateFromDescriptorCarriesEveryBackendFlag)
{
    AudioDeviceDescriptor d;
    d.name = "External Headphones";
    d.usbLocationId = "BuiltInHeadphoneOutputDevice";
    d.isBuiltIn = true;
    d.hasPhysicalHeadphoneJack = true;
    d.isSystemDefault = true;
    d.isWireless = true;
    d.currentSampleRate = 48000;
    d.supportedSampleRates = { 48000 };

    const auto c = OutputDeviceSelector::candidateFromDescriptor (d, 48000, {});
    REQUIRE (c.id == "BuiltInHeadphoneOutputDevice");
    REQUIRE (c.displayName == "External Headphones");
    REQUIRE (c.isBuiltIn);
    REQUIRE (c.hasPhysicalHeadphoneJack);
    REQUIRE (c.isSystemDefault);
    REQUIRE (c.isWireless);
    REQUIRE (c.supportsRecordingSampleRate);
    REQUIRE_FALSE (c.isMicrophonePlaybackEndpoint);
    REQUIRE_FALSE (c.isAlsoSelectedInput);
    REQUIRE_FALSE (c.belongsToUnrecordedMicrophone);

    REQUIRE_FALSE (OutputDeviceSelector::candidateFromDescriptor (d, 44100, {}).supportsRecordingSampleRate);

    // No UID: the name stands in as the id.
    AudioDeviceDescriptor unnamed;
    unnamed.name = "Speakers";
    REQUIRE (OutputDeviceSelector::candidateFromDescriptor (unnamed, 48000, {}).id == "Speakers");

    AudioDeviceDescriptor mic;
    mic.name = "Yeti";
    mic.usbLocationId = "yeti-uid";

    const auto recorded = OutputDeviceSelector::candidateFromDescriptor (mic, 48000, { { "yeti-uid", true } });
    REQUIRE (recorded.isAlsoSelectedInput);
    REQUIRE_FALSE (recorded.belongsToUnrecordedMicrophone);

    const auto switchedOff = OutputDeviceSelector::candidateFromDescriptor (mic, 48000, { { "yeti-uid", false } });
    REQUIRE_FALSE (switchedOff.isAlsoSelectedInput);
    REQUIRE (switchedOff.belongsToUnrecordedMicrophone);

    const auto unrelated = OutputDeviceSelector::candidateFromDescriptor (mic, 48000, { { "other", false } });
    REQUIRE_FALSE (unrelated.isAlsoSelectedInput);
    REQUIRE_FALSE (unrelated.belongsToUnrecordedMicrophone);

    AudioDeviceDescriptor micEndpoint;
    micEndpoint.name = "mic endpoint";
    micEndpoint.isMicrophone = true;
    REQUIRE (OutputDeviceSelector::candidateFromDescriptor (micEndpoint, 48000, {}).isMicrophonePlaybackEndpoint);
}
