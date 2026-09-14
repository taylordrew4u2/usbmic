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

// getpid() alone was not enough: the hog-mode property is typed pid_t, and
// CoreAudioBackend.cpp names that type in four places. MSVC has no POSIX
// pid_t of its own -- <sys/types.h> only exposes one when the non-standard
// names are enabled -- so the simulated-macOS build on Windows failed to
// compile with "syntax error: identifier 'pid_t'".
//
// int is what MSVC's own _pid_t and _getpid() are, so repeating the typedef
// is harmless even where <sys/types.h> has already made the same one: C++
// allows a typedef to be redeclared as long as it names the same type.
typedef int pid_t;

#else

#include_next <unistd.h>

#endif
