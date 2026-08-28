#include "WasapiAsioBackend.h"

#if JUCE_WINDOWS

#include "../Core/SampleFormat.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <avrt.h>
#include <thread>
#include <atomic>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace mma {

/// §2 hotplug on Windows: the OS tells us, we never poll. This is the
/// counterpart to the CoreAudio property listener on macOS -- without it a mic
/// plugged in after launch is simply never noticed, because nothing else in the
/// app ever re-enumerates on its own.
///
/// Deliberately minimal: every notification funnels to the same callback, which
/// re-runs enumeration. Distinguishing "added" from "state changed" here would
/// duplicate logic DeviceManager already owns.
class DeviceNotificationClient : public IMMNotificationClient
{
public:
    explicit DeviceNotificationClient (DeviceChangeCallback* target) : callback (target) {}

    // IUnknown. Reference counted because the enumerator holds a reference for
    // as long as the registration lives.
    ULONG STDMETHODCALLTYPE AddRef() override { return refCount.fetch_add (1) + 1; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto remaining = refCount.fetch_sub (1) - 1;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void** object) override
    {
        if (object == nullptr)
            return E_POINTER;

        if (riid == __uuidof (IUnknown) || riid == __uuidof (IMMNotificationClient))
        {
            *object = static_cast<IMMNotificationClient*> (this);
            AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded (LPCWSTR) override { return fire(); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved (LPCWSTR) override { return fire(); }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged (LPCWSTR, DWORD) override { return fire(); }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged (EDataFlow, ERole, LPCWSTR) override { return fire(); }

    // Property changes fire constantly (volume, mute, jack presence) and none
    // of them alter the device list, so re-enumerating on them would be the
    // polling §2 rules out, just triggered by a different clock.
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged (LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    HRESULT fire()
    {
        // These arrive on an OS-owned thread. The callback only marks the
        // device list dirty for the message thread to pick up; it must not do
        // enumeration work here.
        if (callback != nullptr && *callback)
            (*callback)();

        return S_OK;
    }

    DeviceChangeCallback* callback;
    std::atomic<ULONG> refCount { 1 };
};

struct WasapiStream
{
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    ComPtr<IAudioCaptureClient> capture;

    HANDLE readyEvent = nullptr;
    UINT32 bufferFrames = 0;
    int channels = 0;
    bool isInput = false;

    // The format the device actually accepted. Exclusive mode does no
    // conversion, so whatever this ends up being is what the worker has to
    // read and write byte for byte.
    int bytesPerSample = 4;
    bool sampleIsFloat = true;

    AudioCallback callback;
    std::thread worker;
    std::atomic<bool> running { false };

    // Pre-allocated deinterleave scratch and channel pointers. §11 forbids
    // allocation on the audio thread, so these are sized once at open time.
    std::vector<float> scratch;
    std::vector<float*> channelPointers;

    ~WasapiStream()
    {
        if (readyEvent != nullptr)
            CloseHandle (readyEvent);
    }
};

namespace {

std::string wideToUtf8 (const std::wstring& wide)
{
    if (wide.empty())
        return {};
    int sizeNeeded = WideCharToMultiByte (CP_UTF8, 0, wide.data(), static_cast<int> (wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result (static_cast<size_t> (sizeNeeded), 0);
    WideCharToMultiByte (CP_UTF8, 0, wide.data(), static_cast<int> (wide.size()), result.data(), sizeNeeded, nullptr, nullptr);
    return result;
}

/// Resolves the endpoint id §2.4 stores back to a live IMMDevice. Fails when
/// the device is gone, which is the normal case after an unplug.
bool resolveDevice (const std::string& deviceId, ComPtr<IMMDevice>& out)
{
    ComPtr<IMMDeviceEnumerator> enumerator;

    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof (IMMDeviceEnumerator),
                                  reinterpret_cast<void**> (enumerator.GetAddressOf()))))
        return false;

    const int wideLength = MultiByteToWideChar (CP_UTF8, 0, deviceId.data(),
                                                static_cast<int> (deviceId.size()), nullptr, 0);
    std::wstring wide (static_cast<size_t> (wideLength), 0);
    MultiByteToWideChar (CP_UTF8, 0, deviceId.data(), static_cast<int> (deviceId.size()),
                         wide.data(), wideLength);

    return SUCCEEDED (enumerator->GetDevice (wide.c_str(), out.GetAddressOf()));
}

/// 32-bit float, the format the engine works in throughout. Exclusive mode
/// requires an exact match rather than letting a mixer convert.
WAVEFORMATEXTENSIBLE makeFloat32Format (double sampleRate, int channels)
{
    WAVEFORMATEXTENSIBLE format {};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = static_cast<WORD> (channels);
    format.Format.nSamplesPerSec = static_cast<DWORD> (sampleRate);
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = static_cast<WORD> (channels * 4);
    format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof (WAVEFORMATEXTENSIBLE) - sizeof (WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER
                                         : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return format;
}

/// Fixed-point PCM in the same layout. Most USB microphones and many consumer
/// interfaces expose only 16- or 24-bit PCM in exclusive mode and refuse float
/// outright, so this is not an exotic fallback -- it is the common case.
WAVEFORMATEXTENSIBLE makePcmFormat (double sampleRate, int channels, int containerBits, int validBits)
{
    WAVEFORMATEXTENSIBLE format {};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = static_cast<WORD> (channels);
    format.Format.nSamplesPerSec = static_cast<DWORD> (sampleRate);
    format.Format.wBitsPerSample = static_cast<WORD> (containerBits);
    format.Format.nBlockAlign = static_cast<WORD> (channels * (containerBits / 8));
    format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof (WAVEFORMATEXTENSIBLE) - sizeof (WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = static_cast<WORD> (validBits);
    format.dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER
                                         : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    return format;
}

/// The audio worker. Waits on the client's event and services one buffer per
/// wake. §11: no allocation, locking, logging or file I/O in here.
void runStreamThread (WasapiStream* stream)
{
    // Exclusive-mode buffers are small; without Pro Audio scheduling the OS
    // will not wake this thread reliably enough to hold the §5.4 budget.
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW (L"Pro Audio", &taskIndex);

    while (stream->running.load (std::memory_order_acquire))
    {
        if (WaitForSingleObject (stream->readyEvent, 2000) != WAIT_OBJECT_0)
            continue;

        const int channels = stream->channels;

        for (int ch = 0; ch < channels; ++ch)
            stream->channelPointers[static_cast<size_t> (ch)] =
                stream->scratch.data() + static_cast<size_t> (ch) * stream->bufferFrames;

        if (stream->isInput)
        {
            UINT32 packetFrames = 0;

            while (SUCCEEDED (stream->capture->GetNextPacketSize (&packetFrames)) && packetFrames > 0)
            {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;

                if (FAILED (stream->capture->GetBuffer (&data, &frames, &flags, nullptr, nullptr)))
                    break;

                if (frames > stream->bufferFrames)
                    frames = stream->bufferFrames;

                // AUDCLNT_BUFFERFLAGS_SILENT means the buffer contents are
                // undefined and must be treated as silence rather than read.
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr)
                {
                    std::memset (stream->scratch.data(), 0, stream->scratch.size() * sizeof (float));
                }
                else
                {
                    const int bytes = stream->bytesPerSample;
                    const bool isFloat = stream->sampleIsFloat;

                    for (UINT32 f = 0; f < frames; ++f)
                        for (int ch = 0; ch < channels; ++ch)
                            stream->channelPointers[static_cast<size_t> (ch)][f] =
                                SampleFormat::read (data, static_cast<size_t> (f) * channels + ch, bytes, isFloat);
                }

                stream->callback (stream->channelPointers.data(), channels,
                                  nullptr, 0, static_cast<int> (frames));

                stream->capture->ReleaseBuffer (frames);
            }
        }
        else
        {
            UINT32 padding = 0;
            if (FAILED (stream->client->GetCurrentPadding (&padding)))
                continue;

            const UINT32 frames = stream->bufferFrames - padding;
            if (frames == 0)
                continue;

            BYTE* data = nullptr;
            if (FAILED (stream->render->GetBuffer (frames, &data)) || data == nullptr)
                continue;

            std::memset (stream->scratch.data(), 0, stream->scratch.size() * sizeof (float));

            stream->callback (nullptr, 0,
                              stream->channelPointers.data(), channels,
                              static_cast<int> (frames));

            const int bytes = stream->bytesPerSample;
            const bool isFloat = stream->sampleIsFloat;

            for (UINT32 f = 0; f < frames; ++f)
                for (int ch = 0; ch < channels; ++ch)
                    SampleFormat::write (data, static_cast<size_t> (f) * channels + ch, bytes, isFloat,
                                 stream->channelPointers[static_cast<size_t> (ch)][f]);

            stream->render->ReleaseBuffer (frames, 0);
        }
    }

    if (task != nullptr)
        AvRevertMmThreadCharacteristics (task);
}

} // namespace

WasapiAsioBackend::WasapiAsioBackend()
{
    CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    preferAsio = hasAnyAsioDriverInstalled();
}

WasapiAsioBackend::~WasapiAsioBackend()
{
    unregisterNotificationClient();
    closeAllStreams();
    CoUninitialize();
}

bool WasapiAsioBackend::hasAnyAsioDriverInstalled() const
{
    // §7 backend B / ASIO preference: an ASIO driver is "installed" if it has
    // a CLSID registered under HKEY_LOCAL_MACHINE\SOFTWARE\ASIO. JUCE's ASIO
    // AudioIODeviceType already does this enumeration robustly; this is a
    // lightweight presence check used only to decide ASIO-vs-WASAPI preference.
    HKEY key;
    const bool present = RegOpenKeyExA (HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ, &key) == ERROR_SUCCESS;
    if (present)
        RegCloseKey (key);
    return present;
}

std::vector<AudioDeviceDescriptor> WasapiAsioBackend::enumerateWasapiDevices (bool wantInput) const
{
    std::vector<AudioDeviceDescriptor> result;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof (IMMDeviceEnumerator), &enumerator)))
        return result;

    ComPtr<IMMDeviceCollection> collection;
    const EDataFlow flow = wantInput ? eCapture : eRender;
    if (FAILED (enumerator->EnumAudioEndpoints (flow, DEVICE_STATE_ACTIVE, &collection)))
        return result;

    UINT count = 0;
    collection->GetCount (&count);

    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> device;
        if (FAILED (collection->Item (i, &device)))
            continue;

        LPWSTR idWide = nullptr;
        device->GetId (&idWide);
        std::string id = idWide ? wideToUtf8 (idWide) : std::string();
        if (idWide) CoTaskMemFree (idWide);

        ComPtr<IPropertyStore> props;
        std::string friendlyName;
        if (SUCCEEDED (device->OpenPropertyStore (STGM_READ, &props)))
        {
            PROPVARIANT nameProp;
            PropVariantInit (&nameProp);
            if (SUCCEEDED (props->GetValue (PKEY_Device_FriendlyName, &nameProp)) && nameProp.pwszVal)
                friendlyName = wideToUtf8 (nameProp.pwszVal);
            PropVariantClear (&nameProp);
        }

        AudioDeviceDescriptor d;
        d.name = friendlyName;
        d.usbLocationId = id; // WASAPI endpoint IDs are stable per-port identifiers already
        d.isMicrophone = wantInput;
        d.hasPhysicalHeadphoneJack = ! wantInput; // refined by jack-presence property when available
        result.push_back (d);
    }

    return result;
}

std::vector<AudioDeviceDescriptor> WasapiAsioBackend::enumerateAsioDevices() const
{
    // TODO: delegate to juce::AudioIODeviceType("ASIO") for driver
    // enumeration + channel/rate queries -- ASIO driver COM activation isn't
    // reimplemented here, JUCE's ASIO wrapper already does this correctly.
    return {};
}

std::vector<AudioDeviceDescriptor> WasapiAsioBackend::enumerateInputDevices()
{
    auto devices = enumerateWasapiDevices (true);
    auto asioDevices = enumerateAsioDevices();
    devices.insert (devices.end(), asioDevices.begin(), asioDevices.end());
    return devices;
}

std::vector<AudioDeviceDescriptor> WasapiAsioBackend::enumerateOutputDevices()
{
    return enumerateWasapiDevices (false);
}

void WasapiAsioBackend::setDeviceChangeCallback (DeviceChangeCallback callback)
{
    deviceChangeCallback = std::move (callback);

    unregisterNotificationClient();

    if (! deviceChangeCallback)
        return;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS (&enumerator))))
        return;

    auto* client = new DeviceNotificationClient (&deviceChangeCallback);

    if (SUCCEEDED (enumerator->RegisterEndpointNotificationCallback (client)))
    {
        notificationClient = client;   // the enumerator holds its own reference
    }
    else
    {
        client->Release();
    }
}

void WasapiAsioBackend::unregisterNotificationClient()
{
    if (notificationClient == nullptr)
        return;

    auto* client = static_cast<DeviceNotificationClient*> (notificationClient);

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (SUCCEEDED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     IID_PPV_ARGS (&enumerator))))
        enumerator->UnregisterEndpointNotificationCallback (client);

    // Must outlive the registration: the OS can be mid-notification on another
    // thread when this runs, and releasing our reference is the only safe way
    // to hand ownership back to the reference count.
    client->Release();
    notificationClient = nullptr;
}

ExclusiveModeCapability WasapiAsioBackend::checkExclusiveModeCapability (const std::string& outputDeviceId,
                                                                         double sampleRate, int bufferSizeSamples)
{
    ExclusiveModeCapability cap;

    if (preferAsio)
    {
        cap.exclusiveModeAvailable = true;
        cap.measuredOrEstimatedLatencyMs = (bufferSizeSamples / 48000.0) * 1000.0 * 2.0;
        return cap;
    }

    // WASAPI: only AUDCLNT_SHAREMODE_EXCLUSIVE qualifies. IAudioClient::
    // IsFormatSupported against AUDCLNT_SHAREMODE_EXCLUSIVE would be the
    // real check here; shared mode is never accepted as a substitute (§5.4).
    ComPtr<IMMDevice> device;

    if (! resolveDevice (outputDeviceId, device) || device == nullptr)
    {
        cap.unavailableReason = "That sound output isn't connected any more.";
        return cap;
    }

    ComPtr<IAudioClient> client;

    if (FAILED (device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**> (client.GetAddressOf()))))
    {
        cap.unavailableReason = "Windows wouldn't give this app direct access to your headphones.";
        return cap;
    }

    const auto format = makeFloat32Format (sampleRate, 2);

    if (FAILED (client->IsFormatSupported (AUDCLNT_SHAREMODE_EXCLUSIVE,
                                           reinterpret_cast<const WAVEFORMATEX*> (&format),
                                           nullptr)))
    {
        // §5.4: shared mode is never the fallback. Name the cause instead.
        cap.unavailableReason = "Your headphones won't accept direct low-latency audio. In Windows sound settings, "
                                "turn on exclusive mode for this device, or use a different output.";
        return cap;
    }

    cap.exclusiveModeAvailable = true;
    cap.measuredOrEstimatedLatencyMs = (bufferSizeSamples / sampleRate) * 1000.0 * 2.0;
    return cap;
}

bool WasapiAsioBackend::openWasapiExclusiveStream (const std::string& deviceId, double sampleRate,
                                                   int bufferSizeSamples, bool isInput,
                                                   AudioCallback callback)
{
    if (! callback)
        return false;

    ComPtr<IMMDevice> device;
    if (! resolveDevice (deviceId, device) || device == nullptr)
        return false;

    lastOpenError.clear();

    auto stream = std::make_unique<WasapiStream>();
    stream->callback = std::move (callback);
    stream->isInput = isInput;

    if (FAILED (device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**> (stream->client.GetAddressOf()))))
        return false;

    // §5.4: exclusive mode or nothing -- falling back to shared would deliver
    // 40-100 ms and the product fails at that latency. But exclusive mode also
    // performs no conversion, so the format has to be one the hardware speaks
    // natively. Offering only float32 (as this once did) means most USB
    // microphones, which are 16- or 24-bit PCM devices, simply refuse to open.
    // Try the engine's own format first, then descend through the fixed-point
    // layouts, and try the device's native channel count before giving up.
    WAVEFORMATEXTENSIBLE format {};
    bool formatFound = false;

    int channelCandidates[3] = { isInput ? 1 : 2, isInput ? 2 : 1, 0 };
    int numChannelCandidates = 2;

    // The device's own mix format names the channel count it prefers; if it is
    // neither 1 nor 2 it would otherwise never be tried.
    {
        WAVEFORMATEX* mixFormat = nullptr;

        if (SUCCEEDED (stream->client->GetMixFormat (&mixFormat)) && mixFormat != nullptr)
        {
            const int mixChannels = static_cast<int> (mixFormat->nChannels);

            if (mixChannels > 0 && mixChannels != channelCandidates[0] && mixChannels != channelCandidates[1])
                channelCandidates[numChannelCandidates++] = mixChannels;

            CoTaskMemFree (mixFormat);
        }
    }

    for (int c = 0; c < numChannelCandidates && ! formatFound; ++c)
    {
        const int channels = channelCandidates[c];

        // Container bits, valid bits; 0 valid bits marks the float candidate.
        const int layouts[5][2] = { { 32, 0 }, { 32, 32 }, { 32, 24 }, { 24, 24 }, { 16, 16 } };

        for (const auto& layout : layouts)
        {
            const auto candidate = layout[1] == 0
                ? makeFloat32Format (sampleRate, channels)
                : makePcmFormat (sampleRate, channels, layout[0], layout[1]);

            if (SUCCEEDED (stream->client->IsFormatSupported (AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                              reinterpret_cast<const WAVEFORMATEX*> (&candidate),
                                                              nullptr)))
            {
                format = candidate;
                formatFound = true;
                break;
            }
        }
    }

    if (! formatFound)
    {
        lastOpenError = "This device won't accept a low-latency connection at " + std::to_string ((int) sampleRate)
                      + " Hz. Try a different sample rate, or a different device, in Advanced.";
        return false;
    }

    stream->bytesPerSample = format.Format.wBitsPerSample / 8;
    stream->sampleIsFloat = (format.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

    // Exclusive mode wants the buffer expressed as a duration in 100 ns units.
    const REFERENCE_TIME duration =
        static_cast<REFERENCE_TIME> ((10000.0 * 1000.0 / sampleRate) * bufferSizeSamples + 0.5);

    HRESULT hr = stream->client->Initialize (AUDCLNT_SHAREMODE_EXCLUSIVE,
                                             AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                             duration, duration,
                                             reinterpret_cast<const WAVEFORMATEX*> (&format),
                                             nullptr);

    // The device can reject the period and name the one it wants; retry once at
    // that size rather than giving up on exclusive mode.
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
    {
        UINT32 alignedFrames = 0;
        if (SUCCEEDED (stream->client->GetBufferSize (&alignedFrames)) && alignedFrames > 0)
        {
            const REFERENCE_TIME aligned =
                static_cast<REFERENCE_TIME> ((10000.0 * 1000.0 / sampleRate) * alignedFrames + 0.5);

            stream->client.Reset();

            if (FAILED (device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr,
                                          reinterpret_cast<void**> (stream->client.GetAddressOf()))))
                return false;

            hr = stream->client->Initialize (AUDCLNT_SHAREMODE_EXCLUSIVE,
                                             AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                             aligned, aligned,
                                             reinterpret_cast<const WAVEFORMATEX*> (&format),
                                             nullptr);
        }
    }

    if (FAILED (hr))
        return false;

    stream->readyEvent = CreateEventW (nullptr, FALSE, FALSE, nullptr);
    if (stream->readyEvent == nullptr || FAILED (stream->client->SetEventHandle (stream->readyEvent)))
        return false;

    if (FAILED (stream->client->GetBufferSize (&stream->bufferFrames)))
        return false;

    if (isInput)
    {
        if (FAILED (stream->client->GetService (__uuidof (IAudioCaptureClient),
                                                reinterpret_cast<void**> (stream->capture.GetAddressOf()))))
            return false;
    }
    else
    {
        if (FAILED (stream->client->GetService (__uuidof (IAudioRenderClient),
                                                reinterpret_cast<void**> (stream->render.GetAddressOf()))))
            return false;
    }

    stream->channels = format.Format.nChannels;

    // Deinterleave scratch, allocated here so the audio thread never does (§11).
    stream->scratch.assign (static_cast<size_t> (stream->channels) * stream->bufferFrames, 0.0f);
    stream->channelPointers.resize (stream->channels);

    if (FAILED (stream->client->Start()))
        return false;

    stream->running = true;
    auto* raw = stream.get();
    stream->worker = std::thread ([raw] { runStreamThread (raw); });

    openStreams.push_back (std::move (stream));
    return true;
}

bool WasapiAsioBackend::openExclusiveOutputStream (const std::string& outputDeviceId, double sampleRate,
                                                   int bufferSizeSamples, AudioCallback callback)
{
    return openWasapiExclusiveStream (outputDeviceId, sampleRate, bufferSizeSamples, false, std::move (callback));
}

bool WasapiAsioBackend::openInputStream (const std::string& inputDeviceId, double sampleRate,
                                         int bufferSizeSamples, AudioCallback callback)
{
    return openWasapiExclusiveStream (inputDeviceId, sampleRate, bufferSizeSamples, true, std::move (callback));
}

void WasapiAsioBackend::closeAllStreams()
{
    for (auto& stream : openStreams)
    {
        stream->running.store (false, std::memory_order_release);

        // Wake the worker immediately rather than letting it sit out its
        // timeout, so stopping is not perceptibly slow.
        if (stream->readyEvent != nullptr)
            SetEvent (stream->readyEvent);

        if (stream->worker.joinable())
            stream->worker.join();

        if (stream->client != nullptr)
            stream->client->Stop();
    }

    openStreams.clear();
}

} // namespace mma

#endif // JUCE_WINDOWS
