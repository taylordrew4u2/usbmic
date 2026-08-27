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
