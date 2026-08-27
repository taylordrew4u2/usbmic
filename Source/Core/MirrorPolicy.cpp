#include "MirrorPolicy.h"

namespace mma {

void MirrorPolicy::setEnabledByUser (bool enabled) noexcept
{
    enabledByUser = enabled;

    if (! enabled)
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

} // namespace mma
