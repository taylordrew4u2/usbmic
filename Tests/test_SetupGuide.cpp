#include "TestFramework.h"
#include "Core/SetupGuide.h"
#include <algorithm>
#include <cctype>

using namespace mma;

namespace {

SetupGuideRig twoMicsNoCamera()
{
    SetupGuideRig rig;
    rig.micCount = 2;
    rig.headphoneJackCount = 2;
    return rig;
}

bool contains (const std::string& text, const std::string& phrase)
{
    return text.find (phrase) != std::string::npos;
}

std::string lower (std::string text)
{
    std::transform (text.begin(), text.end(), text.begin(),
                    [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return text;
}

} // namespace

TEST_CASE (SetupGuide_SixPagesWithACameraFiveWithout)
{
    auto rig = twoMicsNoCamera();
    const auto without = SetupGuide::stepsFor (rig);

    REQUIRE (without.size() == 5u);
    REQUIRE (without.front() == SetupGuideStep::Microphones);
    REQUIRE (without.back() == SetupGuideStep::TestTake);
    REQUIRE (std::find (without.begin(), without.end(), SetupGuideStep::Cameras) == without.end());

    rig.cameraCount = 1;
    const auto with = SetupGuide::stepsFor (rig);

    REQUIRE (with.size() == 6u);
    REQUIRE (with[4] == SetupGuideStep::Cameras);
    REQUIRE (with[5] == SetupGuideStep::TestTake);
}

TEST_CASE (SetupGuide_NextWalksEveryPageThenFinishes)
{
    SetupGuide guide;
    auto rig = twoMicsNoCamera();
    rig.cameraCount = 2;
    rig.camerasOn = 2;

    std::vector<SetupGuideStep> seen { guide.currentStep (rig) };

    while (guide.next (rig))
        seen.push_back (guide.currentStep (rig));

    REQUIRE (seen == SetupGuide::stepsFor (rig));

    // Next on the last page is the guide finishing, and it stays put rather
    // than wrapping round to the first page.
    REQUIRE (guide.currentStep (rig) == SetupGuideStep::TestTake);
    REQUIRE_FALSE (guide.next (rig));
    REQUIRE (guide.currentStep (rig) == SetupGuideStep::TestTake);
}

TEST_CASE (SetupGuide_BackStopsAtTheFirstPage)
{
    SetupGuide guide;
    const auto rig = twoMicsNoCamera();

    guide.back (rig);
    REQUIRE (guide.currentStep (rig) == SetupGuideStep::Microphones);

    guide.next (rig);
    guide.next (rig);
    REQUIRE (guide.currentStep (rig) == SetupGuideStep::Headphones);

    guide.back (rig);
    REQUIRE (guide.currentStep (rig) == SetupGuideStep::Names);

    guide.restart();
    REQUIRE (guide.currentStep (rig) == SetupGuideStep::Microphones);
}

TEST_CASE (SetupGuide_BackFromTheTestTakeSkipsAMissingCameraPage)
{
    SetupGuide guide;
    const auto rig = twoMicsNoCamera();

    while (guide.next (rig)) {}

    guide.back (rig);
    REQUIRE (guide.currentStep (rig) == SetupGuideStep::SaveFolder);
}

TEST_CASE (SetupGuide_ACameraUnpluggedOnItsOwnPageMovesTheReaderOn)
{
    // The reader is on the camera page when the only camera goes away. They
    // carry on from the next page rather than being thrown back to the start.
    SetupGuide guide;
    auto rig = twoMicsNoCamera();
    rig.cameraCount = 1;
    rig.camerasOn = 1;

    for (int i = 0; i < 4; ++i)
        guide.next (rig);
    REQUIRE (guide.currentStep (rig) == SetupGuideStep::Cameras);

    rig.cameraCount = 0;
    rig.camerasOn = 0;
    REQUIRE (guide.currentStep (rig) == SetupGuideStep::TestTake);

    const auto page = guide.page (rig);
    REQUIRE (page.step == SetupGuideStep::TestTake);
    REQUIRE (page.number == 5);
    REQUIRE (page.total == 5);
    REQUIRE (page.isLast);

    // And plugged back in, the page they were on is there again.
    rig.cameraCount = 1;
    REQUIRE (guide.currentStep (rig) == SetupGuideStep::Cameras);
}

TEST_CASE (SetupGuide_PageNumbersAndButtons)
{
    const auto rig = twoMicsNoCamera();

    const auto first = SetupGuide::pageFor (SetupGuideStep::Microphones, rig);
    REQUIRE (first.number == 1);
    REQUIRE (first.total == 5);
    REQUIRE_FALSE (first.canGoBack);
    REQUIRE_FALSE (first.isLast);
    REQUIRE (first.nextLabel == "Next");

    const auto last = SetupGuide::pageFor (SetupGuideStep::TestTake, rig);
    REQUIRE (last.number == 5);
    REQUIRE (last.canGoBack);
    REQUIRE (last.isLast);
    REQUIRE (last.nextLabel == "Done");
}

TEST_CASE (SetupGuide_NoMicrophonesSaysSoInsteadOfAnEmptyList)
{
    SetupGuideRig rig;

    const auto mics = SetupGuide::pageFor (SetupGuideStep::Microphones, rig);
    REQUIRE (mics.statusIsWarning);
    REQUIRE (contains (mics.status, "No microphones yet"));

    const auto names = SetupGuide::pageFor (SetupGuideStep::Names, rig);
    REQUIRE (names.statusIsWarning);
    REQUIRE (contains (names.status, "Plug in a microphone first"));

    const auto headphones = SetupGuide::pageFor (SetupGuideStep::Headphones, rig);
    REQUIRE_FALSE (headphones.statusIsWarning);
    REQUIRE_FALSE (headphones.status.empty());
}

TEST_CASE (SetupGuide_CountsMicrophonesAndPointsAtSwitchedOffOnes)
{
    auto rig = twoMicsNoCamera();

    auto page = SetupGuide::pageFor (SetupGuideStep::Microphones, rig);
    REQUIRE (page.status == "Found 2 microphones.");
    REQUIRE_FALSE (page.statusIsWarning);

    rig.micCount = 1;
    rig.switchedOffMicCount = 1;
    page = SetupGuide::pageFor (SetupGuideStep::Microphones, rig);
    REQUIRE (contains (page.status, "Found 1 microphone."));
    REQUIRE (contains (page.status, "1 more is switched off"));
    REQUIRE (contains (page.status, "Settings"));
    REQUIRE (page.statusIsWarning);
}

TEST_CASE (SetupGuide_TapToNameIsExplainedOnlyWhenThereIsMoreThanOneMic)
{
    auto rig = twoMicsNoCamera();
    REQUIRE (contains (SetupGuide::pageFor (SetupGuideStep::Names, rig).body, "tap the top"));

    rig.micCount = 1;
    const auto one = SetupGuide::pageFor (SetupGuideStep::Names, rig);
    REQUIRE_FALSE (contains (one.body, "tap the top"));
    REQUIRE (contains (one.body, "Name"));
}

TEST_CASE (SetupGuide_HeadphonesSayVolumeDownFirstEitherWay)
{
    auto rig = twoMicsNoCamera();

    for (const bool everyMic : { true, false })
    {
        rig.mixGoesToEveryMic = everyMic;
        const auto page = SetupGuide::pageFor (SetupGuideStep::Headphones, rig);
        REQUIRE (contains (page.body, "volume"));
        REQUIRE (contains (page.body, "down before"));
    }

    rig.mixGoesToEveryMic = false;
    REQUIRE (contains (SetupGuide::pageFor (SetupGuideStep::Headphones, rig).body, "Settings"));

    // Microphones plugged in, but none with a socket to play into.
    rig.mixGoesToEveryMic = true;
    rig.headphoneJackCount = 0;
    const auto noJacks = SetupGuide::pageFor (SetupGuideStep::Headphones, rig);
    REQUIRE (noJacks.statusIsWarning);
    REQUIRE (contains (noJacks.status, "Output device"));
}

TEST_CASE (SetupGuide_CameraPageSaysHowManyWillRecord)
{
    auto rig = twoMicsNoCamera();
    rig.cameraCount = 2;
    rig.camerasOn = 2;
    REQUIRE (SetupGuide::pageFor (SetupGuideStep::Cameras, rig).status == "Found 2 cameras. All of them will record.");

    rig.camerasOn = 1;
    REQUIRE (SetupGuide::pageFor (SetupGuideStep::Cameras, rig).status == "Found 2 cameras. 1 will record.");

    rig.cameraCount = 1;
    rig.camerasOn = 0;
    REQUIRE (SetupGuide::pageFor (SetupGuideStep::Cameras, rig).status == "Found 1 camera. It is switched off.");

    REQUIRE (contains (SetupGuide::pageFor (SetupGuideStep::Cameras, rig).body, "Cameras"));
}

TEST_CASE (SetupGuide_TestTakeFollowsTheTake)
{
    auto rig = twoMicsNoCamera();

    auto page = SetupGuide::pageFor (SetupGuideStep::TestTake, rig);
    REQUIRE (page.status.empty());
    REQUIRE (contains (page.body, "Start recording"));
    REQUIRE (contains (page.body, "ten seconds"));

    rig.recording = true;
    rig.recordingSeconds = 4.7;
    page = SetupGuide::pageFor (SetupGuideStep::TestTake, rig);
    REQUIRE (page.status == "Recording: 4 seconds.");

    rig.recordingSeconds = 1.2;
    REQUIRE (SetupGuide::pageFor (SetupGuideStep::TestTake, rig).status == "Recording: 1 second.");

    rig.recordingSeconds = 11.0;
    REQUIRE (contains (SetupGuide::pageFor (SetupGuideStep::TestTake, rig).status, "That's enough"));

    rig.recording = false;
    rig.testTakeSaved = true;
    page = SetupGuide::pageFor (SetupGuideStep::TestTake, rig);
    REQUIRE (contains (page.status, "saved"));
    REQUIRE_FALSE (page.statusIsWarning);

    // Record is off for a reason: the guide repeats it rather than leaving
    // the reader pressing a button that does nothing.
    rig.testTakeSaved = false;
    rig.recordBlockedReason = "Checking the drive is fast enough.";
    page = SetupGuide::pageFor (SetupGuideStep::TestTake, rig);
    REQUIRE (page.statusIsWarning);
    REQUIRE (contains (page.status, "Checking the drive is fast enough."));
}

TEST_CASE (SetupGuide_EveryPageIsWrittenInPlainLanguage)
{
    // §10.1: this guide is for someone who has never set up a recording. The
    // words the rest of the app keeps behind the Settings door stay there.
    const char* jargon[] = { "aggregate", "buffer", "sample rate", "latency", "dbfs",
                             "channel", "stem", "monitor", "input", "interface", "device driver" };

    SetupGuideRig rigs[4];
    rigs[1] = twoMicsNoCamera();
    rigs[2] = twoMicsNoCamera();
    rigs[2].cameraCount = 1;
    rigs[2].mixGoesToEveryMic = false;
    rigs[3] = twoMicsNoCamera();
    rigs[3].recording = true;
    rigs[3].recordingSeconds = 12.0;

    for (const auto& rig : rigs)
        for (const auto step : SetupGuide::stepsFor (rig))
        {
            const auto page = SetupGuide::pageFor (step, rig);
            REQUIRE (! page.title.empty());
            REQUIRE (page.body.size() > 60);

            const auto text = lower (page.title + " " + page.body + " " + page.status);
            for (const auto* word : jargon)
                REQUIRE_FALSE (contains (text, word));
        }
}
