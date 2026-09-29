#include "MirrorPolicy.h"

namespace mma {

void MirrorPolicy::setEnabledByUser (bool enabled) noexcept
{
    enabledByUser = enabled;

    // Only a state that describes no copy at all is relabelled. Throwing an
    // Active mirror to DisabledByUser left it writing with nothing watching it:
    // evaluateDuringRecording() only judges an Active mirror, so the 1 GB stop
    // never ran again for the rest of the take.
    if (! enabled && state == MirrorState::NotStartedNoSpace)
        state = MirrorState::DisabledByUser;
}

MirrorState MirrorPolicy::evaluateAtArm (int64_t internalFreeBytes, int64_t projectedSessionBytes) noexcept
{
    if (! enabledByUser)
    {
        state = MirrorState::DisabledByUser;
        return state;
    }

    const int64_t required = kMinHeadroomBytes + projectedSessionBytes;

    state = internalFreeBytes > required ? MirrorState::Active
                                         : MirrorState::NotStartedNoSpace;
    return state;
}

MirrorState MirrorPolicy::evaluateDuringRecording (int64_t internalFreeBytes) noexcept
{
    // Only a running mirror can be stopped. A mirror that never started, or one
    // already stopped, stays as it is: restarting mid-take would leave a hole in
    // the copy and a partial mirror is not a usable one.
    if (state != MirrorState::Active)
        return state;

    if (internalFreeBytes < kStopBytes)
        state = MirrorState::StoppedLowSpace;

    return state;
}

void MirrorPolicy::reset() noexcept
{
    state = enabledByUser ? MirrorState::Active : MirrorState::DisabledByUser;
}

bool MirrorPolicy::noteWriteFailure() noexcept
{
    // Only a mirror that was actually running can stop. A failure reported
    // after it already stopped -- for space, or for an earlier failure -- is
    // not a new event and must not produce a second notice.
    if (state != MirrorState::Active)
        return false;

    state = MirrorState::StoppedWriteFailed;
    return true;
}

bool MirrorPolicy::noteStoppedByUser() noexcept
{
    if (state != MirrorState::Active)
        return false;

    state = MirrorState::StoppedByUser;
    return true;
}

} // namespace mma
