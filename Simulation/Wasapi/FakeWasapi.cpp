#include "FakeWasapi.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>

#include "../../Source/Core/SampleFormat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace fakewasapi;

// --- Events -----------------------------------------------------------------
// Auto-reset events with a real mutex and condition variable, because the
// backend's worker is a real std::thread: the simulation exercises the actual
// cross-thread handshake rather than pretending it away.
struct FakeEvent
{
    std::mutex mutex;
    std::condition_variable cv;
    bool signalled = false;
};

// --- Endpoint state ---------------------------------------------------------
struct Endpoint
{
    EndpointSpec spec;

    std::mutex mutex;
    std::condition_variable cv;

    bool streamOpen = false;
    bool exclusive = false;
    bool started = false;
    Format negotiated {};
    int bufferFrames = 0;
    FakeEvent* clientEvent = nullptr;

    // Capture: one packet in flight, handed to the worker and acknowledged.
    std::vector<BYTE> pendingCapture;
    int pendingCaptureFrames = 0;
    bool pendingCaptureSilent = false;
    bool captureConsumed = false;

    // Render: the worker fills this, then acknowledges.
    bool renderRequested = false;
    std::vector<BYTE> renderedBytes;
    int renderedFrames = 0;
    bool renderDelivered = false;
};

struct World
{
    std::mutex mutex;
    std::map<std::string, Endpoint*> endpoints;
    std::vector<std::string> order;
    std::vector<IMMNotificationClient*> notificationClients;
    bool asioInstalled = false;
};

World& world()
{
    static World w;
    return w;
}

Endpoint* findEndpoint (const std::string& id)
{
    std::lock_guard<std::mutex> lock (world().mutex);
    auto it = world().endpoints.find (id);
    return it == world().endpoints.end() ? nullptr : it->second;
}

// Explicit element-wise conversion. The iterator-pair constructors narrow
// implicitly, which MSVC rightly warns about; the endpoint ids and device names
// here are ASCII, so a byte-for-byte widening is the whole of what is needed.
std::wstring toWide (const std::string& s)
{
    std::wstring out;
    out.reserve (s.size());

    for (char c : s)
        out.push_back (static_cast<wchar_t> (static_cast<unsigned char> (c)));

    return out;
}

std::string toNarrow (const std::wstring& s)
{
    std::string out;
    out.reserve (s.size());

    for (wchar_t c : s)
        out.push_back (static_cast<char> (c));

    return out;
}

int bytesPerSample (const Format& f) { return f.containerBits / 8; }

bool formatMatches (const Format& a, const WAVEFORMATEXTENSIBLE& w)
{
    const bool wantFloat = (w.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

    return a.channels == static_cast<int> (w.Format.nChannels)
        && a.containerBits == static_cast<int> (w.Format.wBitsPerSample)
        && a.isFloat == wantFloat
        && static_cast<int> (a.sampleRate) == static_cast<int> (w.Format.nSamplesPerSec);
}

/// Reference counting shared by every fake interface. COM lifetime bugs in the
/// backend (a missing Release, a double Release) are real bugs, so the
/// simulation counts properly rather than leaking everything.
template <typename Interface>
struct RefCounted : Interface
{
    std::atomic<ULONG> refCount { 1 };

    ULONG STDMETHODCALLTYPE AddRef() override { return refCount.fetch_add (1) + 1; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto remaining = refCount.fetch_sub (1) - 1;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface (REFIID, void** object) override
    {
        if (object == nullptr)
            return E_POINTER;

        *object = static_cast<Interface*> (this);
        AddRef();
        return S_OK;
    }

    // Virtual because Release() deletes through this type. IUnknown has no
    // virtual destructor on Windows either; the fakes need one to be deletable.
    virtual ~RefCounted() = default;
};

// --- Property store ---------------------------------------------------------
struct FakePropertyStore : RefCounted<IPropertyStore>
{
    std::wstring name;
    std::wstring storage;

    HRESULT STDMETHODCALLTYPE GetValue (const PROPERTYKEY& key, PROPVARIANT* value) override
    {
        if (value == nullptr)
            return E_POINTER;

        if (! (key == PKEY_Device_FriendlyName))
            return E_FAIL;

        storage = name;
        value->vt = 31; // VT_LPWSTR
        value->pwszVal = const_cast<LPWSTR> (storage.c_str());
        return S_OK;
    }
};

// --- Render and capture services --------------------------------------------
struct FakeRenderClient : RefCounted<IAudioRenderClient>
{
    Endpoint* endpoint = nullptr;
    std::vector<BYTE> scratch;

    HRESULT STDMETHODCALLTYPE GetBuffer (UINT32 frames, BYTE** data) override
    {
        if (data == nullptr || endpoint == nullptr)
            return E_POINTER;

        const auto bytes = static_cast<size_t> (frames)
                         * static_cast<size_t> (endpoint->negotiated.channels)
                         * static_cast<size_t> (bytesPerSample (endpoint->negotiated));

        // Filled with a poison value, not zeroes: a backend that fails to write
        // a channel then shows up as garbage rather than as plausible silence.
        scratch.assign (bytes, 0xAB);
        *data = scratch.data();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ReleaseBuffer (UINT32 frames, DWORD) override
    {
        if (endpoint == nullptr)
            return E_POINTER;

        {
            std::lock_guard<std::mutex> lock (endpoint->mutex);
            endpoint->renderedBytes = scratch;
            endpoint->renderedFrames = static_cast<int> (frames);
            endpoint->renderDelivered = true;
            endpoint->renderRequested = false;
        }

        endpoint->cv.notify_all();
        return S_OK;
    }
};

struct FakeCaptureClient : RefCounted<IAudioCaptureClient>
{
    Endpoint* endpoint = nullptr;

    HRESULT STDMETHODCALLTYPE GetNextPacketSize (UINT32* frames) override
    {
        if (frames == nullptr || endpoint == nullptr)
            return E_POINTER;

        std::lock_guard<std::mutex> lock (endpoint->mutex);
        *frames = static_cast<UINT32> (endpoint->captureConsumed ? 0 : endpoint->pendingCaptureFrames);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetBuffer (BYTE** data, UINT32* frames, DWORD* flags,
                                         UINT64*, UINT64*) override
    {
        if (data == nullptr || frames == nullptr || flags == nullptr || endpoint == nullptr)
            return E_POINTER;

        std::lock_guard<std::mutex> lock (endpoint->mutex);

        if (endpoint->captureConsumed || endpoint->pendingCaptureFrames == 0)
            return E_FAIL;

        *frames = static_cast<UINT32> (endpoint->pendingCaptureFrames);
        *flags = endpoint->pendingCaptureSilent ? AUDCLNT_BUFFERFLAGS_SILENT : 0;

        // A silent packet's contents are undefined on Windows. Handing back the
        // real pointer with the flag set is what catches a backend that reads
        // it anyway.
        *data = endpoint->pendingCapture.data();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ReleaseBuffer (UINT32) override
    {
        if (endpoint == nullptr)
            return E_POINTER;

        {
            std::lock_guard<std::mutex> lock (endpoint->mutex);
            endpoint->captureConsumed = true;
        }

        endpoint->cv.notify_all();
        return S_OK;
    }
};

// --- Audio client -----------------------------------------------------------
struct FakeAudioClient : RefCounted<IAudioClient>
{
    Endpoint* endpoint = nullptr;

    HRESULT STDMETHODCALLTYPE IsFormatSupported (AUDCLNT_SHAREMODE mode,
                                                 const WAVEFORMATEX* format,
                                                 WAVEFORMATEX** closest) override
    {
        if (closest != nullptr)
            *closest = nullptr;

        if (format == nullptr || endpoint == nullptr)
            return E_POINTER;

        // §5.4: the backend must never ask for shared mode. If it ever does,
        // fail loudly here rather than quietly succeeding.
        if (mode != AUDCLNT_SHAREMODE_EXCLUSIVE)
            return AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;

        const auto& extensible = *reinterpret_cast<const WAVEFORMATEXTENSIBLE*> (format);

        for (const auto& f : endpoint->spec.exclusiveFormats)
            if (formatMatches (f, extensible))
                return S_OK;

        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }

    HRESULT STDMETHODCALLTYPE GetMixFormat (WAVEFORMATEX** format) override
    {
        if (format == nullptr || endpoint == nullptr)
            return E_POINTER;

        // Allocated with CoTaskMemAlloc, so a missing CoTaskMemFree in the
        // backend is a real leak here too.
        auto* w = static_cast<WAVEFORMATEXTENSIBLE*> (CoTaskMemAlloc (sizeof (WAVEFORMATEXTENSIBLE)));
        std::memset (w, 0, sizeof (WAVEFORMATEXTENSIBLE));

        const auto& mix = endpoint->spec.mixFormat;
        w->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        w->Format.nChannels = static_cast<WORD> (mix.channels);
        w->Format.nSamplesPerSec = static_cast<DWORD> (mix.sampleRate);
        w->Format.wBitsPerSample = static_cast<WORD> (mix.containerBits);
        w->Format.nBlockAlign = static_cast<WORD> (mix.channels * (mix.containerBits / 8));
        w->Format.nAvgBytesPerSec = w->Format.nSamplesPerSec * w->Format.nBlockAlign;
        w->Format.cbSize = sizeof (WAVEFORMATEXTENSIBLE) - sizeof (WAVEFORMATEX);
        w->SubFormat = mix.isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;

        *format = reinterpret_cast<WAVEFORMATEX*> (w);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Initialize (AUDCLNT_SHAREMODE mode, DWORD flags,
                                          REFERENCE_TIME bufferDuration, REFERENCE_TIME,
                                          const WAVEFORMATEX* format, const GUID*) override
    {
        if (format == nullptr || endpoint == nullptr)
            return E_POINTER;

        if (mode != AUDCLNT_SHAREMODE_EXCLUSIVE)
            return AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;

        // The backend must ask for event-driven mode; polling would not hold
        // the §5.4 budget.
        if ((flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) == 0)
            return E_INVALIDARG;

        if (IsFormatSupported (mode, format, nullptr) != S_OK)
            return AUDCLNT_E_UNSUPPORTED_FORMAT;

        // Some devices accept only one period size and reject anything else,
        // reporting the size they want through GetBufferSize. The rejection is
        // a property of the size, not of the client object: a backend that
        // re-activates before retrying (which is what Windows requires after a
        // failed Initialize) must still succeed at the second attempt.
        if (endpoint->spec.alignedFrames > 0)
        {
            const auto requestedFrames = static_cast<int> (
                (static_cast<double> (bufferDuration) * format->nSamplesPerSec / 10000000.0) + 0.5);

            if (requestedFrames != endpoint->spec.alignedFrames)
            {
                std::lock_guard<std::mutex> lock (endpoint->mutex);
                endpoint->bufferFrames = endpoint->spec.alignedFrames;
                return AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED;
            }
        }

        const auto& extensible = *reinterpret_cast<const WAVEFORMATEXTENSIBLE*> (format);

        std::lock_guard<std::mutex> lock (endpoint->mutex);
        endpoint->streamOpen = true;
        endpoint->exclusive = true;
        endpoint->negotiated = Format {
            static_cast<int> (extensible.Format.nChannels),
            static_cast<int> (extensible.Format.wBitsPerSample),
            extensible.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT,
            static_cast<double> (extensible.Format.nSamplesPerSec)
        };
        endpoint->bufferFrames = endpoint->spec.alignedFrames > 0
                               ? endpoint->spec.alignedFrames : endpoint->spec.bufferFrames;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetBufferSize (UINT32* frames) override
    {
        if (frames == nullptr || endpoint == nullptr)
            return E_POINTER;

        std::lock_guard<std::mutex> lock (endpoint->mutex);

        // Before a successful Initialize this reports the size the device wants,
        // which is how the backend learns what to retry at.
        *frames = static_cast<UINT32> (endpoint->bufferFrames > 0
                                       ? endpoint->bufferFrames : endpoint->spec.bufferFrames);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCurrentPadding (UINT32* frames) override
    {
        if (frames == nullptr || endpoint == nullptr)
            return E_POINTER;

        std::lock_guard<std::mutex> lock (endpoint->mutex);

        // Zero padding means a whole period is free, which is what the harness
        // arranges when it asks for one render pass.
        *frames = endpoint->renderRequested ? 0u : static_cast<UINT32> (endpoint->bufferFrames);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetEventHandle (HANDLE event) override
    {
        if (endpoint == nullptr)
            return E_POINTER;

        std::lock_guard<std::mutex> lock (endpoint->mutex);
        endpoint->clientEvent = static_cast<FakeEvent*> (event);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetService (REFIID riid, void** out) override
    {
        if (out == nullptr || endpoint == nullptr)
            return E_POINTER;

        if (riid == __uuidof (IAudioCaptureClient))
        {
            auto* c = new FakeCaptureClient();
            c->endpoint = endpoint;
            *out = static_cast<IAudioCaptureClient*> (c);
            return S_OK;
        }

        if (riid == __uuidof (IAudioRenderClient))
        {
            auto* r = new FakeRenderClient();
            r->endpoint = endpoint;
            *out = static_cast<IAudioRenderClient*> (r);
            return S_OK;
        }

        *out = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE Start() override
    {
        if (endpoint == nullptr)
            return E_POINTER;

        std::lock_guard<std::mutex> lock (endpoint->mutex);
        endpoint->started = true;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Stop() override
    {
        if (endpoint == nullptr)
            return E_POINTER;

        std::lock_guard<std::mutex> lock (endpoint->mutex);
        endpoint->started = false;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Reset() override { return S_OK; }
};

// --- Device and enumerator --------------------------------------------------
struct FakeDevice : RefCounted<IMMDevice>
{
    std::string id;

    HRESULT STDMETHODCALLTYPE Activate (REFIID riid, DWORD, void*, void** out) override
    {
        if (out == nullptr)
            return E_POINTER;

        auto* endpoint = findEndpoint (id);

        if (endpoint == nullptr || ! endpoint->spec.allowActivate || riid != __uuidof (IAudioClient))
        {
            *out = nullptr;
            return E_FAIL;
        }

        auto* client = new FakeAudioClient();
        client->endpoint = endpoint;
        *out = static_cast<IAudioClient*> (client);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OpenPropertyStore (DWORD, IPropertyStore** out) override
    {
        if (out == nullptr)
            return E_POINTER;

        auto* endpoint = findEndpoint (id);
        if (endpoint == nullptr)
            return E_FAIL;

        auto* store = new FakePropertyStore();
        store->name = toWide (endpoint->spec.friendlyName);
        *out = store;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetId (LPWSTR* out) override
    {
        if (out == nullptr)
            return E_POINTER;

        const auto wide = toWide (id);
        const auto bytes = (wide.size() + 1) * sizeof (wchar_t);
        auto* copy = static_cast<wchar_t*> (CoTaskMemAlloc (bytes));
        std::memcpy (copy, wide.c_str(), bytes);
        *out = copy;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetState (DWORD* state) override
    {
        if (state == nullptr)
            return E_POINTER;

        *state = DEVICE_STATE_ACTIVE;
        return S_OK;
    }
};

struct FakeCollection : RefCounted<IMMDeviceCollection>
{
    std::vector<std::string> ids;

    HRESULT STDMETHODCALLTYPE GetCount (UINT* count) override
    {
        if (count == nullptr)
            return E_POINTER;

        *count = static_cast<UINT> (ids.size());
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Item (UINT index, IMMDevice** device) override
    {
        if (device == nullptr)
            return E_POINTER;

        if (index >= ids.size())
            return E_INVALIDARG;

        auto* d = new FakeDevice();
        d->id = ids[index];
        *device = d;
        return S_OK;
    }
};

struct FakeEnumerator : RefCounted<IMMDeviceEnumerator>
{
    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints (EDataFlow flow, DWORD, IMMDeviceCollection** out) override
    {
        if (out == nullptr)
            return E_POINTER;

        auto* collection = new FakeCollection();

        {
            std::lock_guard<std::mutex> lock (world().mutex);
            for (const auto& id : world().order)
            {
                auto* endpoint = world().endpoints[id];
                if (endpoint != nullptr && endpoint->spec.isCapture == (flow == eCapture))
                    collection->ids.push_back (id);
            }
        }

        *out = collection;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint (EDataFlow, ERole, IMMDevice** device) override
    {
        if (device == nullptr)
            return E_POINTER;

        *device = nullptr;
        return E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE GetDevice (LPCWSTR id, IMMDevice** device) override
    {
        if (device == nullptr || id == nullptr)
            return E_POINTER;

        const auto narrow = toNarrow (std::wstring (id));

        if (findEndpoint (narrow) == nullptr)
        {
            *device = nullptr;
            return E_FAIL;
        }

        auto* d = new FakeDevice();
        d->id = narrow;
        *device = d;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback (IMMNotificationClient* client) override
    {
        if (client == nullptr)
            return E_POINTER;

        client->AddRef(); // the enumerator holds its own reference, as on Windows
        std::lock_guard<std::mutex> lock (world().mutex);
        world().notificationClients.push_back (client);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback (IMMNotificationClient* client) override
    {
        std::lock_guard<std::mutex> lock (world().mutex);
        auto& clients = world().notificationClients;
        auto it = std::find (clients.begin(), clients.end(), client);

        if (it == clients.end())
            return E_FAIL;

        clients.erase (it);
        client->Release();
        return S_OK;
    }
};

void fireDeviceAdded (const std::string& id)
{
    std::vector<IMMNotificationClient*> snapshot;
    {
        std::lock_guard<std::mutex> lock (world().mutex);
        snapshot = world().notificationClients;
    }

    const auto wide = toWide (id);
    for (auto* c : snapshot)
        c->OnDeviceAdded (wide.c_str());
}

void fireDeviceRemoved (const std::string& id)
{
    std::vector<IMMNotificationClient*> snapshot;
    {
        std::lock_guard<std::mutex> lock (world().mutex);
        snapshot = world().notificationClients;
    }

    const auto wide = toWide (id);
    for (auto* c : snapshot)
        c->OnDeviceRemoved (wide.c_str());
}

void signalEvent (Endpoint* endpoint)
{
    FakeEvent* event = nullptr;
    {
        std::lock_guard<std::mutex> lock (endpoint->mutex);
        event = endpoint->clientEvent;
    }

    if (event == nullptr)
        return;

    {
        std::lock_guard<std::mutex> lock (event->mutex);
        event->signalled = true;
    }

    event->cv.notify_all();
}

constexpr auto kHandshakeTimeout = std::chrono::seconds (2);

} // namespace

// --- Windows API surface ----------------------------------------------------

HRESULT CoInitializeEx (void*, DWORD) { return S_OK; }
void CoUninitialize() {}

HRESULT CoCreateInstance (REFCLSID clsid, IUnknown*, DWORD, REFIID riid, void** out)
{
    if (out == nullptr)
        return E_POINTER;

    if (clsid == __uuidof (MMDeviceEnumerator) && riid == __uuidof (IMMDeviceEnumerator))
    {
        *out = static_cast<IMMDeviceEnumerator*> (new FakeEnumerator());
        return S_OK;
    }

    *out = nullptr;
    return E_NOINTERFACE;
}

void* CoTaskMemAlloc (size_t bytes) { return std::malloc (bytes); }
void CoTaskMemFree (void* p) { std::free (p); }

HANDLE CreateEventW (void*, BOOL, BOOL initialState, LPCWSTR)
{
    auto* event = new FakeEvent();
    event->signalled = initialState != 0;
    return event;
}

BOOL CloseHandle (HANDLE handle)
{
    delete static_cast<FakeEvent*> (handle);
    return 1;
}

DWORD WaitForSingleObject (HANDLE handle, DWORD milliseconds)
{
    auto* event = static_cast<FakeEvent*> (handle);
    if (event == nullptr)
        return WAIT_TIMEOUT;

    std::unique_lock<std::mutex> lock (event->mutex);

    if (! event->cv.wait_for (lock, std::chrono::milliseconds (milliseconds),
                              [event] { return event->signalled; }))
        return WAIT_TIMEOUT;

    event->signalled = false; // auto-reset, as the backend creates it
    return WAIT_OBJECT_0;
}

BOOL SetEvent (HANDLE handle)
{
    auto* event = static_cast<FakeEvent*> (handle);
    if (event == nullptr)
        return 0;

    {
        std::lock_guard<std::mutex> lock (event->mutex);
        event->signalled = true;
    }

    event->cv.notify_all();
    return 1;
}

int WideCharToMultiByte (UINT, DWORD, LPCWSTR wide, int wideLength,
                         char* out, int outBytes, const char*, BOOL*)
{
    if (wide == nullptr)
        return 0;

    const size_t length = wideLength < 0 ? std::wcslen (wide) : static_cast<size_t> (wideLength);

    if (out == nullptr || outBytes == 0)
        return static_cast<int> (length);

    const size_t toCopy = std::min (length, static_cast<size_t> (outBytes));
    for (size_t i = 0; i < toCopy; ++i)
        out[i] = static_cast<char> (wide[i]);

    return static_cast<int> (toCopy);
}

int MultiByteToWideChar (UINT, DWORD, LPCSTR narrow, int narrowLength, LPWSTR out, int outChars)
{
    if (narrow == nullptr)
        return 0;

    const size_t length = narrowLength < 0 ? std::strlen (narrow) : static_cast<size_t> (narrowLength);

    if (out == nullptr || outChars == 0)
        return static_cast<int> (length);

    const size_t toCopy = std::min (length, static_cast<size_t> (outChars));
    for (size_t i = 0; i < toCopy; ++i)
        out[i] = static_cast<wchar_t> (static_cast<unsigned char> (narrow[i]));

    return static_cast<int> (toCopy);
}

LONG RegOpenKeyExA (HKEY, LPCSTR, DWORD, DWORD, HKEY* out)
{
    std::lock_guard<std::mutex> lock (world().mutex);

    if (! world().asioInstalled)
        return ERROR_FILE_NOT_FOUND;

    if (out != nullptr)
        *out = reinterpret_cast<HKEY> (static_cast<uintptr_t> (1));

    return ERROR_SUCCESS;
}

LONG RegCloseKey (HKEY) { return ERROR_SUCCESS; }

void PropVariantInit (PROPVARIANT* v)
{
    if (v != nullptr)
        *v = PROPVARIANT { 0, nullptr };
}

HRESULT PropVariantClear (PROPVARIANT* v)
{
    if (v != nullptr)
        *v = PROPVARIANT { 0, nullptr };

    return S_OK;
}

HANDLE AvSetMmThreadCharacteristicsW (LPCWSTR, DWORD* taskIndex)
{
    if (taskIndex != nullptr)
        *taskIndex = 1;

    return reinterpret_cast<HANDLE> (static_cast<uintptr_t> (1));
}

BOOL AvRevertMmThreadCharacteristics (HANDLE) { return 1; }

// --- Harness control --------------------------------------------------------

namespace fakewasapi {

void reset()
{
    std::lock_guard<std::mutex> lock (world().mutex);

    for (auto& entry : world().endpoints)
        delete entry.second;

    world().endpoints.clear();
    world().order.clear();
    world().notificationClients.clear();
    world().asioInstalled = false;
}

void addEndpoint (const EndpointSpec& spec)
{
    {
        std::lock_guard<std::mutex> lock (world().mutex);

        auto* endpoint = new Endpoint();
        endpoint->spec = spec;
        endpoint->bufferFrames = spec.bufferFrames;
        world().endpoints[spec.id] = endpoint;
        world().order.push_back (spec.id);
    }

    fireDeviceAdded (spec.id);
}

void removeEndpoint (const std::string& id)
{
    {
        std::lock_guard<std::mutex> lock (world().mutex);
        auto it = world().endpoints.find (id);

        if (it != world().endpoints.end())
        {
            delete it->second;
            world().endpoints.erase (it);
        }

        auto& order = world().order;
        order.erase (std::remove (order.begin(), order.end(), id), order.end());
    }

    fireDeviceRemoved (id);
}

void notifyDeviceAdded (const std::string& id) { fireDeviceAdded (id); }

void setAsioDriverInstalled (bool installed)
{
    std::lock_guard<std::mutex> lock (world().mutex);
    world().asioInstalled = installed;
}

bool isRunning (const std::string& id)
{
    auto* endpoint = findEndpoint (id);
    if (endpoint == nullptr)
        return false;

    std::lock_guard<std::mutex> lock (endpoint->mutex);
    return endpoint->started;
}

Format negotiatedFormat (const std::string& id)
{
    auto* endpoint = findEndpoint (id);
    if (endpoint == nullptr)
        return Format { 0, 0, false, 0.0 };

    std::lock_guard<std::mutex> lock (endpoint->mutex);
    return endpoint->streamOpen ? endpoint->negotiated : Format { 0, 0, false, 0.0 };
}

bool openedExclusive (const std::string& id)
{
    auto* endpoint = findEndpoint (id);
    if (endpoint == nullptr)
        return false;

    std::lock_guard<std::mutex> lock (endpoint->mutex);
    return endpoint->exclusive;
}

namespace {

bool deliverCapture (const std::string& id, std::vector<BYTE> bytes, int frames, bool silent)
{
    auto* endpoint = findEndpoint (id);
    if (endpoint == nullptr)
        return false;

    {
        std::lock_guard<std::mutex> lock (endpoint->mutex);

        if (! endpoint->started)
            return false;

        endpoint->pendingCapture = std::move (bytes);
        endpoint->pendingCaptureFrames = frames;
        endpoint->pendingCaptureSilent = silent;
        endpoint->captureConsumed = false;
    }

    signalEvent (endpoint);

    std::unique_lock<std::mutex> lock (endpoint->mutex);
    const bool consumed = endpoint->cv.wait_for (lock, kHandshakeTimeout,
                                                 [endpoint] { return endpoint->captureConsumed; });

    endpoint->pendingCaptureFrames = 0;
    return consumed;
}

} // namespace

bool pushCapture (const std::string& id, const std::vector<std::vector<float>>& channels)
{
    auto* endpoint = findEndpoint (id);
    if (endpoint == nullptr || channels.empty())
        return false;

    Format format;
    {
        std::lock_guard<std::mutex> lock (endpoint->mutex);
        format = endpoint->negotiated;
    }

    if (format.containerBits == 0)
        return false;

    const int frames = static_cast<int> (channels.front().size());
    const int wireChannels = format.channels;
    const int bytes = bytesPerSample (format);

    // Encode into the negotiated wire format. The backend has to decode it back
    // to float correctly -- that is the conversion path this exercises.
    std::vector<BYTE> wire (static_cast<size_t> (frames) * wireChannels * bytes, 0);

    for (int f = 0; f < frames; ++f)
        for (int ch = 0; ch < wireChannels; ++ch)
        {
            const float value = ch < static_cast<int> (channels.size())
                              ? channels[static_cast<size_t> (ch)][static_cast<size_t> (f)] : 0.0f;

            mma::SampleFormat::write (wire.data(), static_cast<size_t> (f) * wireChannels + ch,
                                      bytes, format.isFloat, value);
        }

    return deliverCapture (id, std::move (wire), frames, false);
}

bool pushSilentCapture (const std::string& id, int frames)
{
    auto* endpoint = findEndpoint (id);
    if (endpoint == nullptr)
        return false;

    Format format;
    {
        std::lock_guard<std::mutex> lock (endpoint->mutex);
        format = endpoint->negotiated;
    }

    if (format.containerBits == 0)
        return false;

    // Deliberately non-zero: the SILENT flag means "do not read this", so a
    // backend that reads it anyway produces noise rather than silence.
    std::vector<BYTE> wire (static_cast<size_t> (frames) * format.channels * bytesPerSample (format), 0x7F);
    return deliverCapture (id, std::move (wire), frames, true);
}

bool pullRender (const std::string& id, std::vector<std::vector<float>>& out)
{
    auto* endpoint = findEndpoint (id);
    if (endpoint == nullptr)
        return false;

    Format format;
    {
        std::lock_guard<std::mutex> lock (endpoint->mutex);

        if (! endpoint->started)
            return false;

        format = endpoint->negotiated;
        endpoint->renderRequested = true;
        endpoint->renderDelivered = false;
    }

    if (format.containerBits == 0)
        return false;

    signalEvent (endpoint);

    std::unique_lock<std::mutex> lock (endpoint->mutex);

    if (! endpoint->cv.wait_for (lock, kHandshakeTimeout,
                                 [endpoint] { return endpoint->renderDelivered; }))
        return false;

    const int frames = endpoint->renderedFrames;
    const auto wire = endpoint->renderedBytes;
    lock.unlock();

    const int bytes = bytesPerSample (format);
    out.assign (static_cast<size_t> (format.channels),
                std::vector<float> (static_cast<size_t> (frames), 0.0f));

    for (int f = 0; f < frames; ++f)
        for (int ch = 0; ch < format.channels; ++ch)
            out[static_cast<size_t> (ch)][static_cast<size_t> (f)] =
                mma::SampleFormat::read (wire.data(), static_cast<size_t> (f) * format.channels + ch,
                                         bytes, format.isFloat);

    return true;
}

} // namespace fakewasapi
