#pragma once

#include <cstdint>

#include "CapacityMonitor.h"
#include "MirrorPolicy.h"

namespace mma {

/// The two mid-take write protections that must act on every status poll,
/// whatever warning is on the advice line (§6.3, §6.5).
///
/// Both used to live beneath a run of warning branches that return as soon as
/// their counter rises -- stream overloads, output glitches, layout misses,
/// backend drops -- and on a loaded machine one of those rises in almost
/// every poll. The ring then climbed past 90% with the mix-only fallback
/// never reached, and overflowed into whole-block drops from every stem and
/// the mix; and the backup copy went on filling the computer's disk past the
/// 1 GB stop. Deciding them here, from plain inputs, lets the poll run them
/// before any warning and keeps the decision testable off the device.
struct WriteSafetyInputs
{
    bool recording = false;
    bool alreadyMixOnly = false;
    double ringFillFraction = 0.0;
    bool mirroring = false;
    /// Free bytes on the mirror's volume; negative when the OS would not say.
    int64_t mirrorFreeBytes = -1;
};

struct WriteSafetyDecision
{
    /// Shed the stems now and keep the mix moving (§6.5, 90% fill).
    bool fallBackToMixOnly = false;
    /// Stop the backup copy now; the policy has already moved to
    /// StoppedLowSpace (§6.3, below 1 GB).
    bool stopMirroring = false;
};

/// Applies the policies and says which actions the caller must take. The
/// mirror policy is advanced as a side effect; the capacity monitor's fill
/// check is stateless, so calling this on every poll latches nothing that a
/// later warning depends on.
WriteSafetyDecision decideWriteSafetyActions (const WriteSafetyInputs& in,
                                              CapacityMonitor& capacity,
                                              MirrorPolicy& mirror) noexcept;

} // namespace mma
