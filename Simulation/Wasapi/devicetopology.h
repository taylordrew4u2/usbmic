#pragma once

// The two device-topology interfaces the backend uses to get from an opaque
// WASAPI endpoint to the physical Plug and Play audio filter behind it.

#include <mmdeviceapi.h>

struct IConnector : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetDeviceIdConnectedTo (LPWSTR* deviceId) = 0;
};

struct IDeviceTopology : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetConnector (UINT index, IConnector** connector) = 0;
};

namespace mmasim {
template <> struct SimUuid<IConnector>      { static constexpr GUID value { 8, 0, 0, 0 }; };
template <> struct SimUuid<IDeviceTopology> { static constexpr GUID value { 9, 0, 0, 0 }; };
}
