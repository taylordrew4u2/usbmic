#pragma once

// Included by every backend that guards its body on JUCE_MAC / JUCE_WINDOWS.
// An undefined macro evaluates to 0, so without this the guarded implementation
// compiles to an empty object file and only fails at link time -- which is
// exactly how both shipping platforms once ended up unable to link.
#include <juce_core/juce_core.h>

// Fail loudly rather than silently: if juce_core ever stops being reached, every
// guard evaluates to 0 again and the backends vanish at link time instead.
#if ! (JUCE_MAC || JUCE_WINDOWS || JUCE_LINUX || JUCE_IOS || JUCE_ANDROID)
 #error "No JUCE platform macro is defined, so every platform guard would compile to nothing."
#endif
