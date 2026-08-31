#pragma once

namespace mma {

/// §9.3: "Respect `prefers-reduced-motion`: glow and pulse off, fill and
/// numbers still live."
///
/// SkullMeterComponent has always had the gate -- the clip glow is drawn only
/// when reducedMotion is false -- and nothing ever set it, so the flag sat at
/// its default and the glow always drew. This is the missing half.
///
/// JUCE exposes no cross-platform accessor for this, so it is read from each
/// OS directly. Where the OS has no single setting to read, this returns false:
/// motion is the documented default, and guessing "reduce" for everyone would
/// silently drop an indicator §9.3 only asks to be softened for people who
/// asked for that.
bool prefersReducedMotionOnThisSystem();

} // namespace mma
