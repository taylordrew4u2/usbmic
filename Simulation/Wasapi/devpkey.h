#pragma once

// Minimal Unified Device Property Model surface used by the WASAPI simulator.

#include <windows.h>

using DEVPROPKEY = PROPERTYKEY;
using DEVPROPTYPE = ULONG;

constexpr DEVPROPTYPE DEVPROP_TYPE_EMPTY  = 0x00000000;
constexpr DEVPROPTYPE DEVPROP_TYPE_INT32  = 0x00000006;
constexpr DEVPROPTYPE DEVPROP_TYPE_UINT32 = 0x00000007;

constexpr DEVPROPKEY DEVPKEY_Device_Capabilities {
    { 0xa45c254e, 0xdf1c, 0x4efd, 0x8020 }, 15
};
constexpr DEVPROPKEY DEVPKEY_Device_RemovalPolicy {
    { 0x4340a6c5, 0x93fa, 0x4706, 0x972c }, 2
};
