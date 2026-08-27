#include "CoreAudioBackend.h"

#if JUCE_MAC

#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>
#include <unistd.h> // getpid() for hog-mode ownership

namespace mma {

struct CoreAudioStream
{
    AudioObjectID deviceId = kAudioObjectUnknown;
    AudioDeviceIOProcID ioProcId = nullptr;
    AudioCallback callback;
    bool isOutput = false;

    // Channel pointer scratch, sized once at open time. §11 forbids allocation
    // inside the callback, so the IOProc only ever fills these.
    static constexpr int kMaxChannels = 64;
    const float* inputPointers[kMaxChannels] = {};
    float* outputPointers[kMaxChannels] = {};
};

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

/// Resolves a device UID (the stable identifier §2.4 stores) to a live
/// AudioObjectID. Returns kAudioObjectUnknown when the device is not present,
/// which is the normal case after an unplug.
AudioObjectID findDeviceByUID (const std::string& uid)
{
    AudioObjectPropertyAddress address { kAudioHardwarePropertyDevices,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr)
        return kAudioObjectUnknown;

    std::vector<AudioObjectID> devices (size / sizeof (AudioObjectID));
    if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &address, 0, nullptr, &size, devices.data()) != noErr)
        return kAudioObjectUnknown;

    for (auto device : devices)
        if (readStringProperty (device, kAudioDevicePropertyDeviceUID) == uid)
            return device;

    return kAudioObjectUnknown;
}

bool setNominalSampleRate (AudioObjectID device, double sampleRate)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyNominalSampleRate,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    Float64 rate = sampleRate;
    return AudioObjectSetPropertyData (device, &address, 0, nullptr, sizeof (rate), &rate) == noErr;
}

bool setBufferFrameSize (AudioObjectID device, int frames)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyBufferFrameSize,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    UInt32 value = static_cast<UInt32> (frames);
    return AudioObjectSetPropertyData (device, &address, 0, nullptr, sizeof (value), &value) == noErr;
}

bool setHogMode (AudioObjectID device, pid_t owner)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyHogMode,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    pid_t value = owner;
    return AudioObjectSetPropertyData (device, &address, 0, nullptr, sizeof (value), &value) == noErr;
}

bool takeHogMode (AudioObjectID device)
{
    return setHogMode (device, getpid());
}

void releaseHogMode (AudioObjectID device)
{
    setHogMode (device, -1);
}

/// The real-time IOProc. §11: no allocation, locking, logging or file I/O here.
/// It only unpacks the buffer lists into pre-sized pointer arrays and forwards.
OSStatus ioProcTrampoline (AudioObjectID /*device*/,
                           const AudioTimeStamp* /*now*/,
                           const AudioBufferList* inputData,
                           const AudioTimeStamp* /*inputTime*/,
                           AudioBufferList* outputData,
                           const AudioTimeStamp* /*outputTime*/,
                           void* clientData)
{
    auto* stream = static_cast<CoreAudioStream*> (clientData);

    if (stream == nullptr || ! stream->callback)
        return noErr;

    int numInputChannels = 0;
    int numSamples = 0;

    if (inputData != nullptr)
    {
        for (UInt32 i = 0; i < inputData->mNumberBuffers && numInputChannels < CoreAudioStream::kMaxChannels; ++i)
        {
            const auto& buffer = inputData->mBuffers[i];

            // Deinterleaved (one channel per buffer) is what CoreAudio gives for
            // USB class-compliant devices; anything else is not handled here.
            if (buffer.mNumberChannels != 1)
                continue;

            stream->inputPointers[numInputChannels++] = static_cast<const float*> (buffer.mData);
            numSamples = static_cast<int> (buffer.mDataByteSize / sizeof (float));
        }
    }

    int numOutputChannels = 0;

    if (outputData != nullptr)
    {
        for (UInt32 i = 0; i < outputData->mNumberBuffers && numOutputChannels < CoreAudioStream::kMaxChannels; ++i)
        {
            auto& buffer = outputData->mBuffers[i];

            if (buffer.mNumberChannels != 1)
                continue;

            stream->outputPointers[numOutputChannels++] = static_cast<float*> (buffer.mData);
            numSamples = static_cast<int> (buffer.mDataByteSize / sizeof (float));
        }
    }

    if (numSamples > 0)
        stream->callback (stream->inputPointers, numInputChannels,
                          stream->outputPointers, numOutputChannels,
                          numSamples);

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

ExclusiveModeCapability CoreAudioBackend::checkExclusiveModeCapability (const std::string& outputDeviceId,
                                                                        double sampleRate, int bufferSizeSamples)
{
    ExclusiveModeCapability cap;
    // CoreAudio's "hog mode" (kAudioDevicePropertyHogMode) plus a direct
    // AudioDeviceIOProc (bypassing HAL mixing) is the exclusive-equivalent
    // path on macOS -- there is no separate "shared vs exclusive" API split
    // like WASAPI's, so availability mainly depends on whether another
    // process already holds hog mode.
    const AudioObjectID device = findDeviceByUID (outputDeviceId);

    if (device == kAudioObjectUnknown)
    {
        cap.unavailableReason = "That sound output isn't connected any more.";
        return cap;
    }

    // Hog mode is unavailable when another process already holds it. §5.4 wants
    // the cause named rather than a silently degraded mix.
    AudioObjectPropertyAddress address { kAudioDevicePropertyHogMode,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    pid_t owner = -1;
    UInt32 size = sizeof (owner);

    if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, &owner) == noErr
        && owner != -1 && owner != getpid())
    {
        cap.unavailableReason = "Another app has taken exclusive control of your headphones. Quit it, then reopen this app.";
        return cap;
    }

    cap.exclusiveModeAvailable = true;
    cap.measuredOrEstimatedLatencyMs = (bufferSizeSamples / sampleRate) * 1000.0 * 2.0; // in + out buffer
    return cap;
}

bool CoreAudioBackend::openStream (const std::string& deviceId, double sampleRate, int bufferSizeSamples,
                                   AudioCallback callback, bool isOutput)
{
    const AudioObjectID device = findDeviceByUID (deviceId);

    if (device == kAudioObjectUnknown || ! callback)
        return false;

    // Match the negotiated rate (§2.2) before the IOProc starts, so the device
    // is not still converting when audio begins.
    if (! setNominalSampleRate (device, sampleRate))
        return false;

    // §5.4 buffer ladder: the requested size is a target, and CoreAudio clamps
    // it to what the device allows. Failing to set it is not fatal -- a larger
    // buffer costs latency, and the measured figure reported at startup will
    // show it rather than the estimate hiding it.
    setBufferFrameSize (device, bufferSizeSamples);

    auto stream = std::make_unique<CoreAudioStream>();
    stream->deviceId = device;
    stream->callback = std::move (callback);
    stream->isOutput = isOutput;

    if (AudioDeviceCreateIOProcID (device, ioProcTrampoline, stream.get(), &stream->ioProcId) != noErr
        || stream->ioProcId == nullptr)
        return false;

    if (AudioDeviceStart (device, stream->ioProcId) != noErr)
    {
        AudioDeviceDestroyIOProcID (device, stream->ioProcId);
        return false;
    }

    openStreams.push_back (std::move (stream));
    return true;
}

bool CoreAudioBackend::openExclusiveOutputStream (const std::string& outputDeviceId, double sampleRate,
                                                  int bufferSizeSamples, AudioCallback callback)
{
    const AudioObjectID device = findDeviceByUID (outputDeviceId);

    if (device == kAudioObjectUnknown)
        return false;

    // Hog mode is the exclusive-equivalent on macOS: it stops the HAL mixing
    // other processes into this device. §5.4 requires the monitor path be
    // exclusive, and §5.4 also requires naming the cause when it is not -- so
    // record whether we actually got it rather than assuming we did.
    outputStreamIsHogModeExclusive = takeHogMode (device);

    if (! openStream (outputDeviceId, sampleRate, bufferSizeSamples, std::move (callback), true))
    {
        if (outputStreamIsHogModeExclusive)
            releaseHogMode (device);

        outputStreamIsHogModeExclusive = false;
        return false;
    }

    openOutputDeviceId = device;
    return true;
}

bool CoreAudioBackend::openInputStream (const std::string& inputDeviceId, double sampleRate,
                                        int bufferSizeSamples, AudioCallback callback)
{
    return openStream (inputDeviceId, sampleRate, bufferSizeSamples, std::move (callback), false);
}

void CoreAudioBackend::closeAllStreams()
{
    for (auto& stream : openStreams)
    {
        if (stream->ioProcId != nullptr)
        {
            AudioDeviceStop (stream->deviceId, stream->ioProcId);
            AudioDeviceDestroyIOProcID (stream->deviceId, stream->ioProcId);
            stream->ioProcId = nullptr;
        }
    }

    openStreams.clear();

    if (outputStreamIsHogModeExclusive && openOutputDeviceId != kAudioObjectUnknown)
        releaseHogMode (openOutputDeviceId);

    openOutputDeviceId = 0;
    outputStreamIsHogModeExclusive = false;
}

} // namespace mma

#endif // JUCE_MAC
