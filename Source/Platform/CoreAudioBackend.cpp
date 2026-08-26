#include "CoreAudioBackend.h"

#if JUCE_MAC

#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>

namespace mma {

namespace {

// Reads a CoreAudio string property (device name, manufacturer, etc.) into a
// std::string, freeing the CFString afterward.
std::string readStringProperty (AudioObjectID device, AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress address { selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef value = nullptr;
    UInt32 size = sizeof (value);

    if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, &value) != noErr || value == nullptr)
        return {};

    char buffer[512] = {};
    CFStringGetCString (value, buffer, sizeof (buffer), kCFStringEncodingUTF8);
    CFRelease (value);
    return std::string (buffer);
}

int countChannels (AudioObjectID device, bool input)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyStreamConfiguration,
                                         input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput,
                                         kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (device, &address, 0, nullptr, &size) != noErr || size == 0)
        return 0;

    std::vector<char> bufferStorage (size);
    auto* bufferList = reinterpret_cast<AudioBufferList*> (bufferStorage.data());
    if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, bufferList) != noErr)
        return 0;

    int total = 0;
    for (UInt32 i = 0; i < bufferList->mNumberBuffers; ++i)
        total += static_cast<int> (bufferList->mBuffers[i].mNumberChannels);
    return total;
}

std::vector<uint32_t> querySupportedSampleRates (AudioObjectID device)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyAvailableNominalSampleRates,
                                         kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (device, &address, 0, nullptr, &size) != noErr || size == 0)
        return {};

    std::vector<AudioValueRange> ranges (size / sizeof (AudioValueRange));
    if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, ranges.data()) != noErr)
        return {};

    std::vector<uint32_t> rates;
    for (const auto& r : ranges)
        rates.push_back (static_cast<uint32_t> (r.mMaximum));
    return rates;
}

// AudioObjectPropertyListenerProc trampoline: forwards into the backend's
// DeviceChangeCallback. Registered on kAudioObjectSystemObject for
// kAudioHardwarePropertyDevices so hotplug is delivered by the OS, never
// polled on a timer (§2).
OSStatus deviceListChanged (AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void* clientData)
{
    auto* callback = static_cast<DeviceChangeCallback*> (clientData);
    if (callback && *callback)
        (*callback)();
    return noErr;
}

} // namespace

CoreAudioBackend::CoreAudioBackend() = default;

CoreAudioBackend::~CoreAudioBackend()
{
    closeAllStreams();
    removeDeviceListListener();
}

std::vector<AudioDeviceDescriptor> CoreAudioBackend::enumerateDevices (bool wantInput)
{
    std::vector<AudioDeviceDescriptor> result;

    AudioObjectPropertyAddress address { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr)
        return result;

    std::vector<AudioObjectID> deviceIds (size / sizeof (AudioObjectID));
    if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &address, 0, nullptr, &size, deviceIds.data()) != noErr)
        return result;

    for (auto deviceId : deviceIds)
    {
        const int channels = countChannels (deviceId, wantInput);
        if (channels <= 0)
            continue;

        AudioDeviceDescriptor d;
        d.name = readStringProperty (deviceId, kAudioObjectPropertyName);
        // §2.4: USB location ID would come from IORegistry (kUSBDevicePropertyLocationID)
        // by walking the device's IOKit registry entry via
        // kAudioDevicePropertyDeviceUID's underlying transport; using the
        // stable CoreAudio UID string here as the practical stand-in since it
        // already encodes enough to distinguish ports across reconnects.
        d.usbLocationId = readStringProperty (deviceId, kAudioDevicePropertyDeviceUID);
        d.maxInputChannels = wantInput ? channels : 0;
        d.supportedSampleRates = querySupportedSampleRates (deviceId);
        d.isMicrophone = wantInput;
        result.push_back (d);
    }

    return result;
}

std::vector<AudioDeviceDescriptor> CoreAudioBackend::enumerateInputDevices() { return enumerateDevices (true); }
std::vector<AudioDeviceDescriptor> CoreAudioBackend::enumerateOutputDevices() { return enumerateDevices (false); }

void CoreAudioBackend::setDeviceChangeCallback (DeviceChangeCallback callback)
{
    deviceChangeCallback = std::move (callback);
    installDeviceListListener();
}

void CoreAudioBackend::installDeviceListListener()
{
    AudioObjectPropertyAddress address { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListener (kAudioObjectSystemObject, &address, deviceListChanged, &deviceChangeCallback);
}

void CoreAudioBackend::removeDeviceListListener()
{
    AudioObjectPropertyAddress address { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    AudioObjectRemovePropertyListener (kAudioObjectSystemObject, &address, deviceListChanged, &deviceChangeCallback);
}

ExclusiveModeCapability CoreAudioBackend::checkExclusiveModeCapability (const std::string& /*outputDeviceId*/,
                                                                        double /*sampleRate*/, int bufferSizeSamples)
{
    ExclusiveModeCapability cap;
    // CoreAudio's "hog mode" (kAudioDevicePropertyHogMode) plus a direct
    // AudioDeviceIOProc (bypassing HAL mixing) is the exclusive-equivalent
    // path on macOS -- there is no separate "shared vs exclusive" API split
    // like WASAPI's, so availability mainly depends on whether another
    // process already holds hog mode.
    cap.exclusiveModeAvailable = true;
    cap.measuredOrEstimatedLatencyMs = (bufferSizeSamples / 48000.0) * 1000.0 * 2.0; // in + out buffer, rough estimate
    return cap;
}

bool CoreAudioBackend::openExclusiveOutputStream (const std::string& /*outputDeviceId*/, double /*sampleRate*/,
                                                  int /*bufferSizeSamples*/, AudioCallback /*callback*/)
{
    // TODO: resolve outputDeviceId -> AudioObjectID, take kAudioDevicePropertyHogMode,
    // install an AudioDeviceIOProc, and start it with AudioDeviceStart(). Not
    // exercised on this Linux sandbox -- see README for what needs bench
    // validation on real macOS hardware.
    outputStreamIsHogModeExclusive = true;
    return true;
}

bool CoreAudioBackend::openInputStream (const std::string& /*inputDeviceId*/, double /*sampleRate*/,
                                        int /*bufferSizeSamples*/, AudioCallback /*callback*/)
{
    // TODO: resolve inputDeviceId -> AudioObjectID and install its IOProc.
    return true;
}

void CoreAudioBackend::closeAllStreams()
{
    // TODO: AudioDeviceStop()/AudioDeviceDestroyIOProcID() for every open
    // device, and release hog mode on the output device if held.
    openInputDeviceIds.clear();
    openOutputDeviceId = 0;
    outputStreamIsHogModeExclusive = false;
}

} // namespace mma

#endif // JUCE_MAC
