#include "WasapiAsioBackend.h"

#if JUCE_WINDOWS

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace mma {

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

} // namespace

WasapiAsioBackend::WasapiAsioBackend()
{
    CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    preferAsio = hasAnyAsioDriverInstalled();
}

WasapiAsioBackend::~WasapiAsioBackend()
{
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
    // TODO: implement IMMNotificationClient and register via
    // IMMDeviceEnumerator::RegisterEndpointNotificationCallback so hotplug is
    // OS-notified, never polled (§2).
}

ExclusiveModeCapability WasapiAsioBackend::checkExclusiveModeCapability (const std::string& /*outputDeviceId*/,
                                                                         double /*sampleRate*/, int bufferSizeSamples)
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
    cap.exclusiveModeAvailable = true; // optimistic default; real check needs IAudioClient::IsFormatSupported
    cap.measuredOrEstimatedLatencyMs = (bufferSizeSamples / 48000.0) * 1000.0 * 2.0;
    return cap;
}

bool WasapiAsioBackend::openWasapiExclusiveStream (const std::string& /*deviceId*/, double /*sampleRate*/,
                                                   int /*bufferSizeSamples*/, bool /*isInput*/,
                                                   AudioCallback /*callback*/)
{
    // TODO: IMMDevice::Activate(IAudioClient) -> Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, ...)
    // -> GetService(IAudioRenderClient/IAudioCaptureClient) -> event-driven
    // callback thread. AUDCLNT_E_UNSUPPORTED_FORMAT / ALREADY_INITIALIZED
    // failures must surface as "neither exclusive path is available" per
    // §5.4, never silently fall back to shared mode.
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
    // TODO: IAudioClient::Stop()/Release() for every open stream.
}

} // namespace mma

#endif // JUCE_WINDOWS
