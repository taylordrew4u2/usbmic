#include "CoreAudioBackend.h"

#if JUCE_MAC

#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>
#include <unistd.h> // getpid() for hog-mode ownership
#include <algorithm>
#include <cstdio>
#include <string>
#include <cmath>
#include <vector>

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

    // Deinterleave scratch. Many USB microphones present one buffer carrying
    // N interleaved channels rather than N single-channel buffers; without
    // somewhere to unpack them the callback can only handle the second shape.
    // Sized at open time because §11 forbids allocating in the IOProc.
    std::vector<float> deinterleaveScratch;
    int maxFramesPerCallback = 0;

    // The mirror of the above for playback. An interface that presents its
    // output as one interleaved buffer needs the callback's per-channel writes
    // packed back together before the IOProc returns; without this the monitor
    // mix is silent on exactly the hardware most people plug in.
    std::vector<float> interleaveScratch;

    struct PendingInterleave
    {
        float* destination;   // the device's own interleaved buffer
        int firstChannel;     // index into interleaveScratch slices
        int channels;
        int frames;
    };

    PendingInterleave pendingInterleave[kMaxChannels] = {};
    int numPendingInterleave = 0;
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

    // A device may report discrete rates (mMinimum == mMaximum) or a
    // continuous range. Reporting only mMaximum, as this once did, hides every
    // rate a range covers -- so a device advertising 44100-96000 would look
    // like it could not do 48000, and §2.2 would negotiate around it.
    static constexpr uint32_t kCommonRates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };

    std::vector<uint32_t> rates;

    for (const auto& r : ranges)
    {
        if (r.mMinimum == r.mMaximum)
        {
            rates.push_back (static_cast<uint32_t> (r.mMaximum));
            continue;
        }

        for (auto rate : kCommonRates)
            if (static_cast<double> (rate) >= r.mMinimum && static_cast<double> (rate) <= r.mMaximum)
                rates.push_back (rate);
    }

    std::sort (rates.begin(), rates.end());
    rates.erase (std::unique (rates.begin(), rates.end()), rates.end());
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

/// The device's transport, used only to tell a built-in microphone from an
/// attached one. Unknown transports read as "not built in", which is the safe
/// answer: a device wrongly treated as attachable can still be chosen as the
/// timebase, whereas wrongly excluding one could leave a rig with no master.
UInt32 readTransportType (AudioObjectID device)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyTransportType,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    UInt32 transport = 0;
    UInt32 size = sizeof (transport);

    if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, &transport) != noErr)
        return 0;

    return transport;
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

double getNominalSampleRate (AudioObjectID device)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyNominalSampleRate,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    Float64 rate = 0.0;
    UInt32 size = sizeof (rate);

    if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, &rate) != noErr)
        return 0.0;

    return static_cast<double> (rate);
}

/// A sample rate as a person says it: "48 kHz", not "48000.000000".
std::string formatRate (double rate)
{
    const double khz = rate / 1000.0;
    char text[32] = {};

    if (std::abs (khz - std::round (khz)) < 0.01)
        std::snprintf (text, sizeof (text), "%d kHz", static_cast<int> (std::round (khz)));
    else
        std::snprintf (text, sizeof (text), "%.1f kHz", khz);

    return text;
}

bool setNominalSampleRate (AudioObjectID device, double sampleRate)
{
    // Ask first. A device another process already holds -- or one whose rate is
    // fixed in hardware -- refuses the write even when it is already running at
    // exactly the rate we want, and treating that as a failure turns a working
    // microphone into one that will not open.
    if (std::abs (getNominalSampleRate (device) - sampleRate) < 1.0)
        return true;

    AudioObjectPropertyAddress address { kAudioDevicePropertyNominalSampleRate,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    Float64 rate = sampleRate;

    if (AudioObjectSetPropertyData (device, &address, 0, nullptr, sizeof (rate), &rate) != noErr)
        return false;

    // The HAL applies the change asynchronously and may land on a neighbouring
    // rate. Confirm rather than assume: the callers negotiate one common rate
    // (§2.2) and a device quietly running at a different one is a drift source.
    return std::abs (getNominalSampleRate (device) - sampleRate) < 1.0;
}

int getBufferFrameSize (AudioObjectID device)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyBufferFrameSize,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    UInt32 value = 0;
    UInt32 size = sizeof (value);

    if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, &value) != noErr)
        return 0;

    return static_cast<int> (value);
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

            if (buffer.mData == nullptr || buffer.mNumberChannels == 0)
                continue;

            const int channelsHere = static_cast<int> (buffer.mNumberChannels);
            const int framesHere = static_cast<int> (buffer.mDataByteSize
                                                     / (sizeof (float) * buffer.mNumberChannels));

            if (channelsHere == 1)
            {
                // One channel per buffer: hand the device's own memory straight
                // through, no copy.
                stream->inputPointers[numInputChannels++] = static_cast<const float*> (buffer.mData);
                numSamples = framesHere;
                continue;
            }

            // Interleaved. Unpacking is required, and skipping it -- as this
            // once did -- means a stereo USB microphone records pure silence
            // with no error anywhere.
            if (framesHere > stream->maxFramesPerCallback
                || numInputChannels + channelsHere > CoreAudioStream::kMaxChannels)
                continue; // scratch was sized for less; dropping beats overrunning it

            const auto* source = static_cast<const float*> (buffer.mData);

            for (int ch = 0; ch < channelsHere; ++ch)
            {
                float* dest = stream->deinterleaveScratch.data()
                            + static_cast<size_t> (numInputChannels) * stream->maxFramesPerCallback;

                for (int f = 0; f < framesHere; ++f)
                    dest[f] = source[f * channelsHere + ch];

                stream->inputPointers[numInputChannels++] = dest;
            }

            numSamples = framesHere;
        }
    }

    int numOutputChannels = 0;

    if (outputData != nullptr)
    {
        for (UInt32 i = 0; i < outputData->mNumberBuffers && numOutputChannels < CoreAudioStream::kMaxChannels; ++i)
        {
            auto& buffer = outputData->mBuffers[i];

            if (buffer.mData == nullptr || buffer.mNumberChannels == 0)
                continue;

            const int channelsHere = static_cast<int> (buffer.mNumberChannels);
            const int framesHere = static_cast<int> (buffer.mDataByteSize
                                                     / (sizeof (float) * buffer.mNumberChannels));

            if (channelsHere == 1)
            {
                stream->outputPointers[numOutputChannels++] = static_cast<float*> (buffer.mData);
                numSamples = framesHere;
                continue;
            }

            // Both the scratch and the channel table must hold the whole
            // buffer: a partial take would repack with the wrong stride.
            if (framesHere > stream->maxFramesPerCallback
                || numOutputChannels + channelsHere > CoreAudioStream::kMaxChannels)
                continue;

            auto& pending = stream->pendingInterleave[stream->numPendingInterleave++];
            pending.destination = static_cast<float*> (buffer.mData);
            pending.firstChannel = numOutputChannels;
            pending.channels = 0;
            pending.frames = framesHere;

            for (int ch = 0; ch < channelsHere; ++ch)
            {
                stream->outputPointers[numOutputChannels++] = stream->interleaveScratch.data()
                    + static_cast<size_t> (pending.firstChannel + ch) * stream->maxFramesPerCallback;
                ++pending.channels;
            }

            numSamples = framesHere;
        }
    }

    if (numSamples > 0)
        stream->callback (stream->inputPointers, numInputChannels,
                          stream->outputPointers, numOutputChannels,
                          numSamples);

    // Pack whatever the callback wrote into the scratch back into the device's
    // interleaved buffers. Done after the callback so it sees a flat layout.
    for (int i = 0; i < stream->numPendingInterleave; ++i)
    {
        const auto& pending = stream->pendingInterleave[i];

        for (int ch = 0; ch < pending.channels; ++ch)
        {
            const float* source = stream->interleaveScratch.data()
                                + static_cast<size_t> (pending.firstChannel + ch) * stream->maxFramesPerCallback;

            for (int f = 0; f < pending.frames; ++f)
                pending.destination[f * pending.channels + ch] = source[f];
        }
    }

    stream->numPendingInterleave = 0;

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

        // §3.1 needs to tell the machine's own microphone apart from one the
        // user plugged in. CoreAudio enumerates the built-in first, so without
        // this it wins clock-master selection on enumeration order and the
        // Advanced panel reads "clock master: <the computer>" no matter how
        // many USB mics are attached.
        d.isBuiltIn = (readTransportType (deviceId) == kAudioDeviceTransportTypeBuiltIn);
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
    {
        lastOpenError = "This microphone is no longer connected. Unplug it and plug it back in, "
                        "then try again.";
        return false;
    }

    // Match the negotiated rate (§2.2) before the IOProc starts, so the device
    // is not still converting when audio begins.
    if (! setNominalSampleRate (device, sampleRate))
    {
        // Name both rates. "Couldn't be opened" sends the user hunting through
        // cables for a fault that is one number in a settings pane, and the
        // rate the device is actually running at is the whole answer.
        const double actual = getNominalSampleRate (device);

        lastOpenError = "This interface is running at " + formatRate (actual)
                      + " and won't change to the " + formatRate (sampleRate)
                      + " this recording uses. Set the recording to "
                      + formatRate (actual) + " in Settings, or change the interface "
                      + "to " + formatRate (sampleRate) + " in Audio MIDI Setup.";
        return false;
    }

    // §5.4 buffer ladder: the requested size is a target, and CoreAudio clamps
    // it to what the device allows. Failing to set it is not fatal -- a larger
    // buffer costs latency, and the measured figure reported at startup will
    // show it rather than the estimate hiding it.
    setBufferFrameSize (device, bufferSizeSamples);

    auto stream = std::make_unique<CoreAudioStream>();
    stream->deviceId = device;
    stream->callback = std::move (callback);
    stream->isOutput = isOutput;

    // Size the de/interleave scratch from the size the device actually granted,
    // not the size we asked for, and leave headroom: the HAL is allowed to hand
    // the IOProc a larger slice than the nominal buffer. §11 forbids allocating
    // in the callback, so anything not covered here has to be dropped there.
    const int granted = std::max (getBufferFrameSize (device), bufferSizeSamples);
    stream->maxFramesPerCallback = std::max (granted * 2, 4096);

    const size_t scratchSamples = static_cast<size_t> (CoreAudioStream::kMaxChannels)
                                * static_cast<size_t> (stream->maxFramesPerCallback);
    stream->deinterleaveScratch.assign (scratchSamples, 0.0f);
    stream->interleaveScratch.assign (scratchSamples, 0.0f);

    if (AudioDeviceCreateIOProcID (device, ioProcTrampoline, stream.get(), &stream->ioProcId) != noErr
        || stream->ioProcId == nullptr)
    {
        lastOpenError = "macOS wouldn't let this app attach to this interface. Another app is "
                        "usually holding it -- close anything else recording or streaming from "
                        "it, then try again.";
        return false;
    }

    if (AudioDeviceStart (device, stream->ioProcId) != noErr)
    {
        AudioDeviceDestroyIOProcID (device, stream->ioProcId);

        // The commonest cause by far, and the one with a fix the user can
        // actually carry out: macOS has not been told this app may listen.
        lastOpenError = "macOS wouldn't start this interface. Check that SobStage is allowed to "
                        "use the microphone in System Settings > Privacy & Security > Microphone, "
                        "and that no other app is recording from it.";
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
    lastOpenError.clear();
    outputStreamIsHogModeExclusive = takeHogMode (device);

    // §5.4: this method promises an exclusive monitor path, and the caller
    // (CaptureCoordinator) branches on that promise. Reporting success without
    // hog mode hands the user a shared output while the app believes otherwise,
    // so fail here and let checkExclusiveModeCapability name the cause.
    if (! outputStreamIsHogModeExclusive)
    {
        lastOpenError = "This sound output won't give this app exclusive use, which live monitoring needs. "
                        "Pick a different output in Advanced -- a USB or Thunderbolt interface usually works, "
                        "Bluetooth headphones usually don't.";
        return false;
    }

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
    // Cleared here, not in openStream: the output path clears it before taking
    // hog mode and sets its own reason on failure, and reporting that message
    // against a microphone would name the wrong device entirely.
    lastOpenError.clear();

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
