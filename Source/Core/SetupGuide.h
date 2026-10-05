#pragma once
#include <string>
#include <vector>

namespace mma {

/// The pages of the first-run setup guide, in the order they are shown.
enum class SetupGuideStep
{
    Microphones,  // plug them in
    Names,        // tap to find which is which, then name it
    Headphones,   // a pair in each microphone, volume low first
    SaveFolder,   // where the takes go
    Cameras,      // only when a camera is plugged in
    TestTake,     // ten seconds, then stop
};

/// What the guide needs to know about the rig, read fresh on every tick so a
/// microphone plugged in while the first page is up shows up on it.
struct SetupGuideRig
{
    int micCount = 0;             // microphones switched on and listed
    int switchedOffMicCount = 0;  // listed, but switched off in Settings
    int headphoneJackCount = 0;   // rows in Settings' "who hears the mix" list
    bool mixGoesToEveryMic = true; // the mix goes to every microphone's jack
    int cameraCount = 0;          // cameras the computer lists right now
    int camerasOn = 0;            // of those, the ones that will record
    bool recording = false;
    double recordingSeconds = 0.0;
    bool testTakeSaved = false;   // a take has finished since the guide opened
    std::string recordBlockedReason; // why Record is off, or empty
};

/// One page, ready to draw.
struct SetupGuidePage
{
    SetupGuideStep step = SetupGuideStep::Microphones;
    int number = 1;               // "Step 2 of 6"
    int total = 1;
    std::string title;
    std::string body;
    /// A line about how things stand right now -- "No microphones yet",
    /// "Recording: 7 seconds" -- or empty when there is nothing to add.
    std::string status;
    bool statusIsWarning = false;
    bool canGoBack = false;
    bool isLast = false;
    std::string nextLabel;        // "Next", or "Done" on the last page
};

/// §10.1: a novice reaches a working rig without knowing the words for it.
///
/// Which pages there are and what each one says depends on the rig -- no
/// camera, no camera page; no microphones, a page that says so rather than an
/// empty list -- and that branching lives here, without JUCE, so a test can
/// walk it. The UI only draws the page it is handed and reports Back, Next and
/// Skip.
///
/// The guide never holds anything up. Record works with it open, Skip is on
/// every page, and pages appear and disappear as hardware does without losing
/// the reader's place.
class SetupGuide
{
public:
    /// The pages for this rig, in order.
    static std::vector<SetupGuideStep> stepsFor (const SetupGuideRig& rig);

    /// Back to the first page.
    void restart() noexcept { current = SetupGuideStep::Microphones; }

    /// The page being shown. If the rig changed under it -- the only camera was
    /// unplugged while its page was up -- this is the next page that still
    /// exists, so the reader carries on rather than being sent back.
    SetupGuideStep currentStep (const SetupGuideRig& rig) const;

    /// Moves on. Returns false when the current page was the last one, which
    /// is the guide finishing; the caller closes it.
    bool next (const SetupGuideRig& rig);

    /// Moves back one page. Does nothing on the first.
    void back (const SetupGuideRig& rig);

    /// The current page's words for this rig.
    SetupGuidePage page (const SetupGuideRig& rig) const;

    /// Any page's words, for a test that wants to read one directly.
    static SetupGuidePage pageFor (SetupGuideStep step, const SetupGuideRig& rig);

private:
    SetupGuideStep current = SetupGuideStep::Microphones;
};

} // namespace mma
