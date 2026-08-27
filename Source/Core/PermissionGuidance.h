#pragma once
#include <string>
#include <vector>

namespace mma {

enum class PermissionKind
{
    Microphone,       // macOS microphone permission / Windows microphone privacy
    RemovableVolume,  // macOS removable-volume access, needed to write to the card
};

enum class PermissionState
{
    Granted,
    Denied,
    NotYetRequested,
    /// The OS does not gate this on the current platform, so there is nothing
    /// to ask for and nothing to warn about.
    NotApplicable,
};

struct PermissionProblem
{
    PermissionKind kind;

    /// §10.6: what is wrong, then what to do, in one sentence. §10.1 requires
    /// this be shown inside the app rather than sending the user hunting
    /// through system settings unaided.
    std::string message;

    /// True when recording and monitoring genuinely cannot work until this is
    /// granted, as opposed to a feature being degraded.
    bool blocksRecording = false;
};

/// §10.1: "Permissions are the first real obstacle." A denied microphone
/// permission looks exactly like broken hardware to a novice, so it has to be
/// named explicitly rather than presenting as silence.
class PermissionGuidance
{
public:
    /// Everything currently worth telling the user, blocking problems first.
    static std::vector<PermissionProblem> evaluate (PermissionState microphone,
                                                    PermissionState removableVolume,
                                                    bool destinationIsRemovable);

    /// True when the app cannot capture at all, which §10.4 shows next to the
    /// disabled record button.
    static bool blocksRecording (PermissionState microphone);
};

} // namespace mma
