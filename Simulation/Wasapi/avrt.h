#pragma once
#include <windows.h>

// MMCSS. Scheduling is the one thing the simulation cannot reproduce, so these
// succeed and do nothing: the backend's contract is that it asks for Pro Audio
// scheduling and copes when it does not get it, and both branches are exercised
// by the call returning a handle here and nullptr under a failure scenario.
HANDLE AvSetMmThreadCharacteristicsW (LPCWSTR taskName, DWORD* taskIndex);
BOOL AvRevertMmThreadCharacteristics (HANDLE handle);
