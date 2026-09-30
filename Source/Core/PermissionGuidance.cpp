#include "PermissionGuidance.h"

namespace mma {

bool PermissionGuidance::blocksRecording (PermissionState microphone)
{
    // NotYetRequested is not a blocker: the OS prompt appears when the stream
    // opens, and pre-emptively warning about it would be noise.
    return microphone == PermissionState::Denied;
}

std::string PermissionGuidance::pendingPromptReason (PermissionState microphone,
                                                    bool osPromptsWhenStreamOpens)
{
    if (! osPromptsWhenStreamOpens || microphone != PermissionState::NotYetRequested)
        return {};

    return "macOS is asking whether SobStage may use your microphones. Click Allow in that message; "
           "Record turns on a moment later. If no message is showing, turn SobStage on in "
           "System Settings > Privacy & Security > Microphone.";
}

std::string PermissionGuidance::grantArrivedMessage (bool takeRunning)
{
    if (takeRunning)
        return "Microphone access was allowed during this take, so this take is silent. "
               "Stop and press Record again.";

    return "Microphone access is allowed now.";
}

std::vector<PermissionProblem> PermissionGuidance::evaluate (PermissionState microphone,
                                                             PermissionState removableVolume,
                                                             bool destinationIsRemovable)
{
    std::vector<PermissionProblem> problems;

    // Microphone first: denied, the app captures nothing at all, and to a
    // novice that is indistinguishable from broken hardware.
    if (microphone == PermissionState::Denied)
        problems.push_back ({ PermissionKind::Microphone,
                              "This app isn't allowed to use your microphones yet. "
                              "Turn on microphone access for it in your computer's privacy settings, then reopen the app.",
                              true });

    // Only worth raising when the card is actually where the recording goes.
    if (destinationIsRemovable && removableVolume == PermissionState::Denied)
        problems.push_back ({ PermissionKind::RemovableVolume,
                              "This app isn't allowed to save to your memory card yet. "
                              "Allow it access to removable volumes in your computer's privacy settings, "
                              "or choose a different place to save.",
                              false });

    return problems;
}

PermissionRefresh PermissionRefresh::decide (PermissionState previous,
                                             PermissionState current) noexcept
{
    PermissionRefresh refresh;
    refresh.changed = (previous != current);

    // Only a real grant arriving from a real non-grant reopens the streams.
    // NotApplicable carries no evidence either way, so leaving it is not a
    // grant and must not tear down streams that are already working.
    refresh.restartCapture = refresh.changed
                          && current == PermissionState::Granted
                          && (previous == PermissionState::NotYetRequested
                              || previous == PermissionState::Denied);
    return refresh;
}

} // namespace mma
