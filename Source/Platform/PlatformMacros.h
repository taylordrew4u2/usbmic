#pragma once

// Included by every backend that guards its body on JUCE_MAC / JUCE_WINDOWS.
// An undefined macro evaluates to 0, so without this the guarded implementation
// compiles to an empty object file and only fails at link time -- which is
// exactly how both shipping platforms once ended up unable to link.
#if defined (MMA_SIMULATE_MAC) || defined (MMA_SIMULATE_WINDOWS)

// Simulation build (Simulation/): the platform shims stand in for the OS audio
// API, so the platform macro comes from the build rather than from JUCE, and
// JUCE is not on the include path at all. This is the only reason the macOS and
// Windows backends can be executed on a machine that has neither -- the backend
// sources themselves are compiled unmodified.
 #if defined (MMA_SIMULATE_MAC)
  #define JUCE_MAC 1
 #else
  #define JUCE_WINDOWS 1
 #endif

#else

#include <juce_core/juce_core.h>

#endif

// Fail loudly rather than silently: if juce_core ever stops being reached, every
// guard evaluates to 0 again and the backends vanish at link time instead.
#if ! (JUCE_MAC || JUCE_WINDOWS || JUCE_LINUX || JUCE_IOS || JUCE_ANDROID)
 #error "No JUCE platform macro is defined, so every platform guard would compile to nothing."
#endif
