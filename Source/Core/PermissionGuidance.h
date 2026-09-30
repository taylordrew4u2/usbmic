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

/// What a fresh sample of the microphone permission means for a running app.
///
/// The answer used to be read once at launch. A user who clicked "Don't
/// Allow" on the first-run prompt (or revoked access later) was left with a
/// live Record button over microphones the OS was feeding zeros: a silent
/// show. Re-sampling is cheap and never prompts; this decides what to do with
/// the new answer, so the decision is unit-tested off the Mac.
struct PermissionRefresh
{
    /// The stored answer is stale: store the new one, re-journal the
    /// permission problems and let the record gate re-read it.
    bool changed = false;

    /// Access has just been granted where it was not before, so the streams
    /// opened while it was pending (or denied) carry silence and must be
    /// reopened once.
    bool restartCapture = false;

    static PermissionRefresh decide (PermissionState previous, PermissionState current) noexcept;
};

} // namespace mma
