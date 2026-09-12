#pragma once

// Minimal Configuration Manager surface used by the WASAPI simulator.

#include <devpkey.h>

using CONFIGRET = ULONG;
using DEVINST = ULONG;
using PDEVINST = DEVINST*;
using DEVINSTID_W = WCHAR*;
using PBYTE = BYTE*;
using PULONG = ULONG*;

constexpr CONFIGRET CR_SUCCESS = 0;
constexpr CONFIGRET CR_FAILURE = 1;
constexpr CONFIGRET CR_NO_SUCH_DEVNODE = 2;

constexpr ULONG CM_LOCATE_DEVNODE_NORMAL = 0;
constexpr ULONG CM_DEVCAP_REMOVABLE = 0x00000004;
constexpr ULONG CM_REMOVAL_POLICY_EXPECT_NO_REMOVAL = 1;
constexpr ULONG CM_REMOVAL_POLICY_EXPECT_ORDERLY_REMOVAL = 2;
constexpr ULONG CM_REMOVAL_POLICY_EXPECT_SURPRISE_REMOVAL = 3;

CONFIGRET CM_Locate_DevNodeW (PDEVINST node, DEVINSTID_W deviceId, ULONG flags);
CONFIGRET CM_Get_Parent (PDEVINST parent, DEVINST node, ULONG flags);
CONFIGRET CM_Get_DevNode_PropertyW (DEVINST node, const DEVPROPKEY* key,
                                    DEVPROPTYPE* type, PBYTE buffer,
                                    PULONG bufferSize, ULONG flags);
