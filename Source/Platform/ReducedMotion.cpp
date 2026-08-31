#include "ReducedMotion.h"
#include "PlatformMacros.h"

#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#elif JUCE_WINDOWS
 #include <windows.h>
#endif

namespace mma {

bool prefersReducedMotionOnThisSystem()
{
#if JUCE_MAC
    // Accessibility > Display > Reduce motion. Read through CoreFoundation
    // rather than NSWorkspace's accessibilityDisplayShouldReduceMotion so this
    // stays a plain .cpp, matching the other platform sources here.
    CFPropertyListRef value = CFPreferencesCopyAppValue (CFSTR ("reduceMotion"),
                                                         CFSTR ("com.apple.universalaccess"));

    if (value == nullptr)
        return false; // never set: the default is motion on.

    bool reduce = false;

    // Guard the type: a preference file can hold anything, and
    // CFBooleanGetValue on a non-boolean is undefined behaviour.
    if (CFGetTypeID (value) == CFBooleanGetTypeID())
        reduce = CFBooleanGetValue (static_cast<CFBooleanRef> (value));

    CFRelease (value);
    return reduce;

#elif JUCE_WINDOWS
    // Ease of Access > Display > "Show animations in Windows". The setting is
    // phrased the other way round -- it reports whether animation is *enabled*
    // -- so it is inverted here.
    BOOL animationsEnabled = TRUE;

    if (! SystemParametersInfoW (SPI_GETCLIENTAREAANIMATION, 0, &animationsEnabled, 0))
        return false; // could not ask: assume motion is fine rather than removing it.

    return ! animationsEnabled;

#else
    // Linux has no single setting every desktop agrees on -- GNOME's
    // enable-animations, KDE's own, and neither is readable without a
    // desktop-specific dependency this app does not otherwise carry. Reporting
    // "no preference" is the honest answer, and matches how §14.3 treats
    // controller topology the OS does not expose.
    return false;
#endif
}

} // namespace mma
