#pragma once

// Stand-in for <mmdeviceapi.h>: the endpoint enumeration and hotplug surface.

#include <windows.h>

enum EDataFlow { eRender = 0, eCapture = 1, eAll = 2 };
enum ERole { eConsole = 0, eMultimedia = 1, eCommunications = 2 };

constexpr DWORD DEVICE_STATE_ACTIVE     = 0x00000001;
constexpr DWORD DEVICE_STATE_DISABLED   = 0x00000002;
constexpr DWORD DEVICE_STATE_NOTPRESENT = 0x00000004;
constexpr DWORD DEVICE_STATE_UNPLUGGED  = 0x00000008;

struct IPropertyStore : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetValue (const PROPERTYKEY& key, PROPVARIANT* value) = 0;
};

struct IMMDevice : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE Activate (REFIID riid, DWORD context, void* params, void** out) = 0;
    virtual HRESULT STDMETHODCALLTYPE OpenPropertyStore (DWORD access, IPropertyStore** out) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetId (LPWSTR* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetState (DWORD* state) = 0;
};

struct IMMDeviceCollection : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetCount (UINT* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE Item (UINT index, IMMDevice** device) = 0;
};

/// The hotplug interface the backend implements. §2 requires the OS to push
/// device changes; the simulation pushes them the same way, so a backend that
/// went back to polling would show up here as a callback that never fires.
struct IMMNotificationClient : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE OnDeviceStateChanged (LPCWSTR id, DWORD newState) = 0;
    virtual HRESULT STDMETHODCALLTYPE OnDeviceAdded (LPCWSTR id) = 0;
    virtual HRESULT STDMETHODCALLTYPE OnDeviceRemoved (LPCWSTR id) = 0;
    virtual HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged (EDataFlow flow, ERole role, LPCWSTR id) = 0;
    virtual HRESULT STDMETHODCALLTYPE OnPropertyValueChanged (LPCWSTR id, const PROPERTYKEY key) = 0;
};

struct IMMDeviceEnumerator : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE EnumAudioEndpoints (EDataFlow flow, DWORD stateMask,
                                                          IMMDeviceCollection** collection) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint (EDataFlow flow, ERole role,
                                                               IMMDevice** device) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDevice (LPCWSTR id, IMMDevice** device) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback (IMMNotificationClient* client) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback (IMMNotificationClient* client) = 0;
};

/// The coclass the backend asks CoCreateInstance for. Incomplete by design --
/// it names a CLSID, never an instance.
class MMDeviceEnumerator;

namespace mmasim {
template <> struct SimUuid<IPropertyStore>        { static constexpr GUID value { 2, 0, 0, 0 }; };
template <> struct SimUuid<IMMDevice>             { static constexpr GUID value { 3, 0, 0, 0 }; };
template <> struct SimUuid<IMMDeviceCollection>   { static constexpr GUID value { 4, 0, 0, 0 }; };
template <> struct SimUuid<IMMNotificationClient> { static constexpr GUID value { 5, 0, 0, 0 }; };
template <> struct SimUuid<IMMDeviceEnumerator>   { static constexpr GUID value { 6, 0, 0, 0 }; };
template <> struct SimUuid<MMDeviceEnumerator>    { static constexpr GUID value { 7, 0, 0, 0 }; };
}
