#include "PermissionGuidance.h"

namespace mma {

bool PermissionGuidance::blocksRecording (PermissionState microphone)
{
    // NotYetRequested is not a blocker: the OS prompt appears when the stream
    // opens, and pre-emptively warning about it would be noise.
    return microphone == PermissionState::Denied;
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

} // namespace mma
