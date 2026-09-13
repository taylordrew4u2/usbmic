#pragma once

// CoreAudioBackend.cpp includes <unistd.h> for getpid(), which the hog-mode
// property takes as its owner. macOS has it; MSVC does not, so a simulated-macOS
// build on Windows needs it supplied.
//
// Everywhere else the real header is what is wanted -- the shim sits earlier on
// the include path, so it has to hand back through rather than replace it.

#if defined (_WIN32)
#include <process.h>
#define getpid _getpid

#else

#include_next <unistd.h>

#endif
