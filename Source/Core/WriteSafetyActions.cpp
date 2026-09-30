#include "WriteSafetyActions.h"

namespace mma {

WriteSafetyDecision decideWriteSafetyActions (const WriteSafetyInputs& in,
                                              CapacityMonitor& capacity,
                                              MirrorPolicy& mirror) noexcept
{
    WriteSafetyDecision decision;

    // Once the stems are shed they stay shed, so a second fallback would only
    // move the logged degradation position.
    if (in.recording && ! in.alreadyMixOnly
        && capacity.evaluateFill (in.ringFillFraction) == WritePipelineState::DegradedToMixOnly)
        decision.fallBackToMixOnly = true;

    // A free-space figure the OS declined to report is not an empty disk.
    // evaluateDuringRecording reports StoppedLowSpace again for a copy that
    // already stopped, so only an Active policy may ask for the stop -- once.
    if (in.mirroring && in.mirrorFreeBytes >= 0 && mirror.isMirroring()
        && mirror.evaluateDuringRecording (in.mirrorFreeBytes) == MirrorState::StoppedLowSpace)
        decision.stopMirroring = true;

    return decision;
}

} // namespace mma
