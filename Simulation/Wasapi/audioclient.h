#pragma once

// Stand-in for <audioclient.h>: the exclusive-mode streaming surface.

#include <windows.h>

enum AUDCLNT_SHAREMODE { AUDCLNT_SHAREMODE_SHARED = 0, AUDCLNT_SHAREMODE_EXCLUSIVE = 1 };

constexpr DWORD AUDCLNT_STREAMFLAGS_EVENTCALLBACK = 0x00040000;

constexpr DWORD AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY = 0x1;
constexpr DWORD AUDCLNT_BUFFERFLAGS_SILENT             = 0x2;
constexpr DWORD AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR    = 0x4;

// The real SDK values, so a backend that special-cases one of them by number
// behaves identically here.
constexpr HRESULT AUDCLNT_E_UNSUPPORTED_FORMAT      = static_cast<HRESULT> (0x88890008);
constexpr HRESULT AUDCLNT_E_DEVICE_IN_USE           = static_cast<HRESULT> (0x8889000A);
constexpr HRESULT AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED = static_cast<HRESULT> (0x88890019);
constexpr HRESULT AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED = static_cast<HRESULT> (0x8889000F);

struct IAudioRenderClient : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetBuffer (UINT32 frames, BYTE** data) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReleaseBuffer (UINT32 frames, DWORD flags) = 0;
};

struct IAudioCaptureClient : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetBuffer (BYTE** data, UINT32* frames, DWORD* flags,
                                                 UINT64* devicePosition, UINT64* qpcPosition) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReleaseBuffer (UINT32 frames) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNextPacketSize (UINT32* frames) = 0;
};

struct IAudioClient : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE Initialize (AUDCLNT_SHAREMODE mode, DWORD flags,
                                                  REFERENCE_TIME bufferDuration,
                                                  REFERENCE_TIME periodicity,
                                                  const WAVEFORMATEX* format,
                                                  const GUID* sessionGuid) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetBufferSize (UINT32* frames) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentPadding (UINT32* frames) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsFormatSupported (AUDCLNT_SHAREMODE mode,
                                                         const WAVEFORMATEX* format,
                                                         WAVEFORMATEX** closestMatch) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat (WAVEFORMATEX** format) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEventHandle (HANDLE event) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetService (REFIID riid, void** out) = 0;
    virtual HRESULT STDMETHODCALLTYPE Start() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
    virtual HRESULT STDMETHODCALLTYPE Reset() = 0;
};

namespace mmasim {
template <> struct SimUuid<IAudioRenderClient>  { static constexpr GUID value { 8, 0, 0, 0 }; };
template <> struct SimUuid<IAudioCaptureClient> { static constexpr GUID value { 9, 0, 0, 0 }; };
template <> struct SimUuid<IAudioClient>        { static constexpr GUID value { 10, 0, 0, 0 }; };
}
