#pragma once
#include <cstdint>

namespace mma {

enum class MirrorState
{
    /// Writing a second copy alongside the card.
    Active,
    /// Never started: not enough internal room at arm time (§1, §6.3).
    NotStartedNoSpace,
    /// Turned off in Advanced.
    DisabledByUser,
    /// Was running and was stopped mid-take because internal space ran low.
    /// The card write continues (§6.3): the recording is never interrupted.
    StoppedLowSpace,
};

/// §6.3 redundant local mirror. Its whole purpose is turning most card
/// failures from data loss into inconvenience, so the rules about when it runs
/// are worth being exact about.
class MirrorPolicy
{
public:
    /// §6.3 / §1: mirror only if internal free space exceeds 2 GB plus the
    /// projected session size.
    static constexpr int64_t kMinHeadroomBytes = 2LL * 1024 * 1024 * 1024;

    /// §6.3: below 1 GB during a take, stop mirroring and keep the card write
    /// going. Lower than the start threshold on purpose -- stopping is a last
    /// resort, and re-deciding at the same number would flap.
    static constexpr int64_t kStopBytes = 1LL * 1024 * 1024 * 1024;

    void setEnabledByUser (bool enabled) noexcept;

    /// Decides at arm time whether the mirror starts.
    MirrorState evaluateAtArm (int64_t internalFreeBytes, int64_t projectedSessionBytes) noexcept;

    /// Called during a take. Once stopped it never restarts within the same
    /// recording: a mirror with a hole in the middle is not a usable copy.
    MirrorState evaluateDuringRecording (int64_t internalFreeBytes) noexcept;

    MirrorState getState() const noexcept { return state; }
    bool isMirroring() const noexcept { return state == MirrorState::Active; }

    /// True when the mirror stopped mid-take, which §6.3 requires be noted in
    /// session.json.
    bool wasStoppedForSpace() const noexcept { return state == MirrorState::StoppedLowSpace; }

    void reset() noexcept;

private:
    bool enabledByUser = true;
    MirrorState state = MirrorState::DisabledByUser;
};

} // namespace mma
