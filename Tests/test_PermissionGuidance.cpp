#include "TestFramework.h"
#include "Core/PermissionGuidance.h"

using namespace mma;

namespace {

bool mentions (const std::vector<PermissionProblem>& problems, PermissionKind kind)
{
    for (const auto& p : problems)
        if (p.kind == kind)
            return true;
    return false;
}

} // namespace

TEST_CASE (PermissionGuidance_QuietWhenEverythingIsGranted)
{
    const auto problems = PermissionGuidance::evaluate (PermissionState::Granted,
                                                        PermissionState::Granted, true);
    REQUIRE (problems.empty());
}

TEST_CASE (PermissionGuidance_DeniedMicrophoneIsReportedAndBlocks)
{
    const auto problems = PermissionGuidance::evaluate (PermissionState::Denied,
                                                        PermissionState::Granted, false);

    REQUIRE (problems.size() == 1);
    REQUIRE (problems[0].kind == PermissionKind::Microphone);
    // §10.1: denied microphone access looks exactly like broken hardware, so it
    // has to block and say why.
    REQUIRE (problems[0].blocksRecording);
    REQUIRE (PermissionGuidance::blocksRecording (PermissionState::Denied));
}

TEST_CASE (PermissionGuidance_NotYetRequestedDoesNotWarn)
{
    // The OS prompt appears when the stream opens; warning first would be noise.
    const auto problems = PermissionGuidance::evaluate (PermissionState::NotYetRequested,
                                                        PermissionState::NotYetRequested, true);
    REQUIRE (problems.empty());
    REQUIRE_FALSE (PermissionGuidance::blocksRecording (PermissionState::NotYetRequested));
}

TEST_CASE (PermissionGuidance_UnansweredMacPromptHoldsRecord)
{
    // macOS raises the prompt when the input streams open and feeds them
    // silence until it is answered: a take started then records nothing.
    REQUIRE_FALSE (PermissionGuidance::pendingPromptReason (PermissionState::NotYetRequested, true).empty());

    // Every other state is someone else's business: Denied is evaluate()'s.
    REQUIRE (PermissionGuidance::pendingPromptReason (PermissionState::Granted, true).empty());
    REQUIRE (PermissionGuidance::pendingPromptReason (PermissionState::Denied, true).empty());
    REQUIRE (PermissionGuidance::pendingPromptReason (PermissionState::NotApplicable, true).empty());

    // Windows (an empty consent value) has no prompt-on-open silence.
    REQUIRE (PermissionGuidance::pendingPromptReason (PermissionState::NotYetRequested, false).empty());
}

TEST_CASE (PermissionGuidance_LiveSoundReleasesTheMacPromptGate)
{
    // Seen on macOS 27: the status call said "not yet asked" while both mics
    // were metering live sound, and Record stayed locked with no way past it.
    // A pending prompt feeds exact zeros, so real sound means access works.
    REQUIRE (PermissionGuidance::pendingPromptReason (PermissionState::NotYetRequested, true, true).empty());
    REQUIRE_FALSE (PermissionGuidance::pendingPromptReason (PermissionState::NotYetRequested, true, false).empty());
}

TEST_CASE (PermissionGuidance_GrantMidTakeIsNotAnAllClear)
{
    // The reopen a grant needs waits for Stop, so the running take stays
    // silent; the journal must say to restart it rather than "allowed now".
    const auto midTake = PermissionGuidance::grantArrivedMessage (true);
    REQUIRE (midTake.find ("Stop and press Record again") != std::string::npos);
    REQUIRE (PermissionGuidance::grantArrivedMessage (false) == "Microphone access is allowed now.");
}

TEST_CASE (PermissionGuidance_NotApplicableIsSilent)
{
    // Windows does not gate removable volumes the way macOS does.
    const auto problems = PermissionGuidance::evaluate (PermissionState::Granted,
                                                        PermissionState::NotApplicable, true);
    REQUIRE (problems.empty());
}

TEST_CASE (PermissionGuidance_VolumeAccessOnlyMattersWhenSavingToRemovable)
{
    // Saving to the internal drive: card access is irrelevant, so saying
    // anything would just be one more thing to ignore.
    const auto internalDest = PermissionGuidance::evaluate (PermissionState::Granted,
                                                            PermissionState::Denied, false);
    REQUIRE (internalDest.empty());

    const auto cardDest = PermissionGuidance::evaluate (PermissionState::Granted,
                                                        PermissionState::Denied, true);
    REQUIRE (mentions (cardDest, PermissionKind::RemovableVolume));
}

TEST_CASE (PermissionGuidance_VolumeAccessDoesNotBlockRecording)
{
    const auto problems = PermissionGuidance::evaluate (PermissionState::Granted,
                                                        PermissionState::Denied, true);
    REQUIRE (problems.size() == 1);
    // Recording can still go somewhere else, so this is not a hard block.
    REQUIRE_FALSE (problems[0].blocksRecording);
}

TEST_CASE (PermissionGuidance_MicrophoneIsReportedBeforeVolume)
{
    const auto problems = PermissionGuidance::evaluate (PermissionState::Denied,
                                                        PermissionState::Denied, true);
    REQUIRE (problems.size() == 2);
    // The blocking one leads.
    REQUIRE (problems.front().kind == PermissionKind::Microphone);
}

TEST_CASE (PermissionGuidance_MessagesSayWhatToDo)
{
    const auto problems = PermissionGuidance::evaluate (PermissionState::Denied,
                                                        PermissionState::Denied, true);

    for (const auto& p : problems)
    {
        // §10.6: name what happened, then what to do. No codes, no apologies.
        REQUIRE_FALSE (p.message.empty());
        REQUIRE (p.message.find ("settings") != std::string::npos);
        REQUIRE (p.message.find ("0x") == std::string::npos);
    }
}

// --- PermissionRefresh: re-sampling the microphone answer while running ---

TEST_CASE (PermissionRefresh_UnchangedAnswerDoesNothing)
{
    for (auto s : { PermissionState::Granted, PermissionState::Denied,
                    PermissionState::NotYetRequested, PermissionState::NotApplicable })
    {
        const auto r = PermissionRefresh::decide (s, s);
        REQUIRE_FALSE (r.changed);
        REQUIRE_FALSE (r.restartCapture);
    }
}

TEST_CASE (PermissionRefresh_DontAllowOnFirstPromptIsNoticed)
{
    // The launch-time sample was NotYetRequested; the user then clicked
    // "Don't Allow". Record must stop being offered.
    const auto r = PermissionRefresh::decide (PermissionState::NotYetRequested,
                                              PermissionState::Denied);
    REQUIRE (r.changed);
    REQUIRE_FALSE (r.restartCapture);
    REQUIRE (PermissionGuidance::blocksRecording (PermissionState::Denied));
}

TEST_CASE (PermissionRefresh_RevokedWhileRunningIsNoticed)
{
    const auto r = PermissionRefresh::decide (PermissionState::Granted, PermissionState::Denied);
    REQUIRE (r.changed);
    REQUIRE_FALSE (r.restartCapture);
}

TEST_CASE (PermissionRefresh_AllowOnFirstPromptReopensTheStreamsOnce)
{
    // Streams opened while the prompt was up deliver silence; they must be
    // reopened now that access exists.
    const auto r = PermissionRefresh::decide (PermissionState::NotYetRequested,
                                              PermissionState::Granted);
    REQUIRE (r.changed);
    REQUIRE (r.restartCapture);
}

TEST_CASE (PermissionRefresh_GrantAfterDenialReopensTheStreams)
{
    const auto r = PermissionRefresh::decide (PermissionState::Denied, PermissionState::Granted);
    REQUIRE (r.changed);
    REQUIRE (r.restartCapture);
}

TEST_CASE (PermissionRefresh_PlatformWithoutConsentNeverRestarts)
{
    // NotApplicable is "no evidence either way" (Linux, or a Mac where the
    // query class is missing); moving to or from it is recorded but is not a
    // grant and must not tear down working streams.
    auto r = PermissionRefresh::decide (PermissionState::NotApplicable, PermissionState::Granted);
    REQUIRE (r.changed);
    REQUIRE_FALSE (r.restartCapture);

    r = PermissionRefresh::decide (PermissionState::Granted, PermissionState::NotApplicable);
    REQUIRE (r.changed);
    REQUIRE_FALSE (r.restartCapture);
}
