#pragma once

namespace mma {

/// §9.3: "Respect `prefers-reduced-motion`": the take banner and alert card
/// flash slower (or not at all) for people who asked for less motion.
///
/// JUCE exposes no cross-platform accessor for this, so it is read from each
/// OS directly. Where the OS has no single setting to read, this returns false:
/// motion is the documented default, and guessing "reduce" for everyone would
/// silently drop an indicator §9.3 only asks to be softened for people who
/// asked for that.
bool prefersReducedMotionOnThisSystem();

} // namespace mma
