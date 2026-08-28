#include "FakeCoreAudio.h"

#include <CoreAudio/CoreAudio.h>

#include <unistd.h> // pid_t, and getpid() where the platform has it

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

struct FakeString
{
    std::string value;
};

struct IoProcRegistration
{
    AudioDeviceIOProc proc = nullptr;
    void* clientData = nullptr;
    bool running = false;
};

struct Device
{
    fakeca::DeviceSpec spec;

    /// The pid holding hog mode, or -1 for unowned -- the same encoding
    /// CoreAudio uses, so the backend's own comparison against getpid() is
    /// what decides whether the device is ours.
    int hogOwner = -1;

    std::vector<IoProcRegistration> procs;
};

struct Listener
{
    AudioObjectID object;
    AudioObjectPropertySelector selector;
    AudioObjectPropertyListenerProc proc;
    void* clientData;
};

struct State
{
    std::map<AudioObjectID, Device> devices;
    std::vector<AudioObjectID> order;
    std::vector<Listener> listeners;
    AudioObjectID nextId = 100;
};

State& state()
{
    static State s;
    return s;
}

Device* find (AudioObjectID id)
{
    auto it = state().devices.find (id);
    return it == state().devices.end() ? nullptr : &it->second;
}

/// Copies `bytes` from `source` into the caller's buffer, honouring CoreAudio's
/// ioSize convention: the caller states its capacity, the HAL writes what it
/// actually produced. Under-reporting here is how a real HAL truncates.
OSStatus deliver (const void* source, UInt32 bytes, UInt32* ioSize, void* outData)
{
    if (ioSize == nullptr)
        return kAudioHardwareUnspecifiedError;

    if (outData == nullptr)
    {
        *ioSize = bytes;
        return noErr;
    }

    const UInt32 toCopy = std::min (bytes, *ioSize);
    std::memcpy (outData, source, toCopy);
    *ioSize = toCopy;
    return noErr;
}

/// Builds the buffer list for one callback in the device's configured shape and
/// keeps the backing storage alive for the duration of the call.
struct BufferListStorage
{
    std::vector<char> listBytes;
    std::vector<std::vector<float>> blocks;

    AudioBufferList* build (int channels, int frames, fakeca::BufferShape shape)
    {
        const int bufferCount = (shape == fakeca::BufferShape::interleaved) ? 1 : channels;
        const int channelsPerBuffer = (shape == fakeca::BufferShape::interleaved) ? channels : 1;

        listBytes.assign (sizeof (AudioBufferList)
                          + sizeof (AudioBuffer) * static_cast<size_t> (std::max (0, bufferCount - 1)), 0);
        blocks.assign (static_cast<size_t> (bufferCount),
                       std::vector<float> (static_cast<size_t> (channelsPerBuffer) * frames, 0.0f));

        auto* list = reinterpret_cast<AudioBufferList*> (listBytes.data());
        list->mNumberBuffers = static_cast<UInt32> (bufferCount);

        for (int i = 0; i < bufferCount; ++i)
        {
            list->mBuffers[i].mNumberChannels = static_cast<UInt32> (channelsPerBuffer);
            list->mBuffers[i].mDataByteSize =
                static_cast<UInt32> (blocks[static_cast<size_t> (i)].size() * sizeof (float));
            list->mBuffers[i].mData = blocks[static_cast<size_t> (i)].data();
        }

        return list;
    }
};

void fireDeviceListListeners()
{
    AudioObjectPropertyAddress address { kAudioHardwarePropertyDevices,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };

    // Copied first: a listener is allowed to add or remove listeners.
    const auto snapshot = state().listeners;

    for (const auto& l : snapshot)
        if (l.object == kAudioObjectSystemObject && l.selector == kAudioHardwarePropertyDevices)
            l.proc (kAudioObjectSystemObject, 1, &address, l.clientData);
}

} // namespace

#if defined (_WIN32)
pid_t getpid()
{
    // Any stable non-negative value works: the backend only ever compares this
    // against the owner the hog-mode property reports back.
    return 4242;
}
#endif

// --- CoreFoundation ---------------------------------------------------------

Boolean CFStringGetCString (CFStringRef value, char* buffer, long bufferSize, CFStringEncoding)
{
    if (value == nullptr || buffer == nullptr || bufferSize <= 0)
        return 0;

    const auto* s = reinterpret_cast<const FakeString*> (value);
    const auto length = std::min (s->value.size(), static_cast<size_t> (bufferSize - 1));
    std::memcpy (buffer, s->value.data(), length);
    buffer[length] = '\0';
    return 1;
}

void CFRelease (CFStringRef value)
{
    delete reinterpret_cast<const FakeString*> (value);
}

// --- Properties -------------------------------------------------------------

OSStatus AudioObjectGetPropertyDataSize (AudioObjectID object,
                                         const AudioObjectPropertyAddress* address,
                                         UInt32, const void*, UInt32* outSize)
{
    if (address == nullptr || outSize == nullptr)
        return kAudioHardwareUnspecifiedError;

    if (object == kAudioObjectSystemObject && address->mSelector == kAudioHardwarePropertyDevices)
    {
        *outSize = static_cast<UInt32> (state().order.size() * sizeof (AudioObjectID));
        return noErr;
    }

    auto* device = find (object);
    if (device == nullptr)
        return kAudioHardwareBadObjectError;

    if (address->mSelector == kAudioDevicePropertyStreamConfiguration)
    {
        const int channels = (address->mScope == kAudioObjectPropertyScopeInput)
                           ? device->spec.inputChannels : device->spec.outputChannels;

        if (channels <= 0)
        {
            // A real HAL reports a valid, empty buffer list rather than zero
            // bytes; the backend treats either as "no channels on this scope".
            *outSize = sizeof (AudioBufferList) - sizeof (AudioBuffer);
            return noErr;
        }

        const int bufferCount = (device->spec.shape == fakeca::BufferShape::interleaved) ? 1 : channels;
        *outSize = static_cast<UInt32> (sizeof (AudioBufferList)
                                        + sizeof (AudioBuffer) * static_cast<size_t> (bufferCount - 1));
        return noErr;
    }

    if (address->mSelector == kAudioDevicePropertyAvailableNominalSampleRates)
    {
        *outSize = static_cast<UInt32> (device->spec.rateRanges.size() * sizeof (AudioValueRange));
        return noErr;
    }

    return kAudioHardwareUnknownPropertyError;
}

OSStatus AudioObjectGetPropertyData (AudioObjectID object,
                                     const AudioObjectPropertyAddress* address,
                                     UInt32, const void*, UInt32* ioSize, void* outData)
{
    if (address == nullptr || ioSize == nullptr)
        return kAudioHardwareUnspecifiedError;

    if (object == kAudioObjectSystemObject && address->mSelector == kAudioHardwarePropertyDevices)
        return deliver (state().order.data(),
                        static_cast<UInt32> (state().order.size() * sizeof (AudioObjectID)),
                        ioSize, outData);

    auto* device = find (object);
    if (device == nullptr)
        return kAudioHardwareBadObjectError;

    switch (address->mSelector)
    {
        case kAudioDevicePropertyTransportType:
        {
            // Every simulated device stands in for an attached USB microphone,
            // so nothing here reads as built in -- which is what the simulated
            // rigs mean.
            const UInt32 transport = kAudioDeviceTransportTypeUSB;
            return deliver (&transport, sizeof (transport), ioSize, outData);
        }

        case kAudioObjectPropertyName:
        case kAudioDevicePropertyDeviceUID:
        {
            auto* handle = new FakeString { address->mSelector == kAudioObjectPropertyName
                                                ? device->spec.name : device->spec.uid };
            auto ref = reinterpret_cast<CFStringRef> (handle);
            const auto status = deliver (&ref, sizeof (ref), ioSize, outData);

            if (outData == nullptr || status != noErr)
                delete handle; // nothing took ownership

            return status;
        }

        case kAudioDevicePropertyStreamConfiguration:
        {
            const int channels = (address->mScope == kAudioObjectPropertyScopeInput)
                               ? device->spec.inputChannels : device->spec.outputChannels;

            BufferListStorage storage;

            if (channels <= 0)
            {
                AudioBufferList empty {};
                empty.mNumberBuffers = 0;
                return deliver (&empty, sizeof (AudioBufferList) - sizeof (AudioBuffer), ioSize, outData);
            }

            // Frame count is irrelevant to a configuration query; only the
            // channel-per-buffer split is being reported.
            auto* list = storage.build (channels, 1, device->spec.shape);
            const auto bytes = static_cast<UInt32> (storage.listBytes.size());
            return deliver (list, bytes, ioSize, outData);
        }

        case kAudioDevicePropertyAvailableNominalSampleRates:
        {
            std::vector<AudioValueRange> ranges;
            for (const auto& r : device->spec.rateRanges)
                ranges.push_back ({ r.first, r.second });

            return deliver (ranges.data(),
                            static_cast<UInt32> (ranges.size() * sizeof (AudioValueRange)),
                            ioSize, outData);
        }

        case kAudioDevicePropertyNominalSampleRate:
        {
            const Float64 rate = device->spec.currentRate;
            return deliver (&rate, sizeof (rate), ioSize, outData);
        }

        case kAudioDevicePropertyBufferFrameSize:
        {
            const UInt32 frames = static_cast<UInt32> (device->spec.bufferFrameSize);
            return deliver (&frames, sizeof (frames), ioSize, outData);
        }

        case kAudioDevicePropertyHogMode:
        {
            // -1 means unowned. A device the harness marked unavailable reports
            // a foreign pid, which is what another app holding it looks like.
            // A device the harness marked unavailable reports a foreign pid,
            // which is what another app holding it looks like.
            constexpr int kSomeOtherProcess = 9999;
            const int owner = device->hogOwner != -1
                            ? device->hogOwner
                            : (device->spec.allowHogMode ? -1 : kSomeOtherProcess);
            return deliver (&owner, sizeof (owner), ioSize, outData);
        }

        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

OSStatus AudioObjectSetPropertyData (AudioObjectID object,
                                     const AudioObjectPropertyAddress* address,
                                     UInt32, const void*, UInt32 inSize, const void* inData)
{
    auto* device = find (object);
    if (device == nullptr || address == nullptr || inData == nullptr)
        return kAudioHardwareBadObjectError;

    switch (address->mSelector)
    {
        case kAudioDevicePropertyNominalSampleRate:
        {
            if (inSize < sizeof (Float64))
                return kAudioHardwareUnspecifiedError;

            Float64 requested;
            std::memcpy (&requested, inData, sizeof (requested));

            if (! device->spec.allowRateChange)
                return kAudioHardwareUnspecifiedError;

            const bool supported = std::any_of (device->spec.rateRanges.begin(),
                                                device->spec.rateRanges.end(),
                                                [requested] (const std::pair<double, double>& r)
                                                { return requested >= r.first && requested <= r.second; });

            if (! supported)
                return kAudioHardwareUnspecifiedError;

            device->spec.currentRate = requested;
            return noErr;
        }

        case kAudioDevicePropertyBufferFrameSize:
        {
            if (inSize < sizeof (UInt32))
                return kAudioHardwareUnspecifiedError;

            UInt32 frames;
            std::memcpy (&frames, inData, sizeof (frames));

            if (! device->spec.allowBufferSizeChange)
                return kAudioHardwareUnspecifiedError;

            device->spec.bufferFrameSize = static_cast<int> (frames);
            return noErr;
        }

        case kAudioDevicePropertyHogMode:
        {
            int owner;
            std::memcpy (&owner, inData, sizeof (owner));

            if (owner == -1)
            {
                device->hogOwner = -1;
                return noErr;
            }

            if (! device->spec.allowHogMode)
                return kAudioHardwareUnspecifiedError;

            device->hogOwner = owner;
            return noErr;
        }

        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

OSStatus AudioObjectAddPropertyListener (AudioObjectID object,
                                         const AudioObjectPropertyAddress* address,
                                         AudioObjectPropertyListenerProc listener,
                                         void* clientData)
{
    if (address == nullptr || listener == nullptr)
        return kAudioHardwareUnspecifiedError;

    state().listeners.push_back ({ object, address->mSelector, listener, clientData });
    return noErr;
}

OSStatus AudioObjectRemovePropertyListener (AudioObjectID object,
                                            const AudioObjectPropertyAddress* address,
                                            AudioObjectPropertyListenerProc listener,
                                            void* clientData)
{
    if (address == nullptr)
        return kAudioHardwareUnspecifiedError;

    auto& listeners = state().listeners;
    listeners.erase (std::remove_if (listeners.begin(), listeners.end(),
                                     [&] (const Listener& l)
                                     {
                                         return l.object == object
                                             && l.selector == address->mSelector
                                             && l.proc == listener
                                             && l.clientData == clientData;
                                     }),
                     listeners.end());
    return noErr;
}

// --- IOProcs ----------------------------------------------------------------

OSStatus AudioDeviceCreateIOProcID (AudioObjectID device, AudioDeviceIOProc proc,
                                    void* clientData, AudioDeviceIOProcID* outProcId)
{
    auto* d = find (device);
    if (d == nullptr || proc == nullptr || outProcId == nullptr)
        return kAudioHardwareBadObjectError;

    d->procs.push_back ({ proc, clientData, false });
    *outProcId = proc;
    return noErr;
}

OSStatus AudioDeviceDestroyIOProcID (AudioObjectID device, AudioDeviceIOProcID procId)
{
    auto* d = find (device);
    if (d == nullptr)
        return kAudioHardwareBadObjectError;

    auto& procs = d->procs;
    procs.erase (std::remove_if (procs.begin(), procs.end(),
                                 [procId] (const IoProcRegistration& r) { return r.proc == procId; }),
                 procs.end());
    return noErr;
}

OSStatus AudioDeviceStart (AudioObjectID device, AudioDeviceIOProcID procId)
{
    auto* d = find (device);
    if (d == nullptr)
        return kAudioHardwareBadObjectError;

    for (auto& r : d->procs)
        if (r.proc == procId)
        {
            r.running = true;
            return noErr;
        }

    return kAudioHardwareBadObjectError;
}

OSStatus AudioDeviceStop (AudioObjectID device, AudioDeviceIOProcID procId)
{
    auto* d = find (device);
    if (d == nullptr)
        return kAudioHardwareBadObjectError;

    for (auto& r : d->procs)
        if (r.proc == procId)
            r.running = false;

    return noErr;
}

// --- Harness control --------------------------------------------------------

namespace fakeca {

void reset()
{
    state().devices.clear();
    state().order.clear();
    state().listeners.clear();
    state().nextId = 100;
}

AudioObjectID addDevice (const DeviceSpec& spec)
{
    const AudioObjectID id = state().nextId++;
    state().devices[id] = Device { spec, -1, {} };
    state().order.push_back (id);
    fireDeviceListListeners();
    return id;
}

void removeDevice (AudioObjectID device)
{
    state().devices.erase (device);
    auto& order = state().order;
    order.erase (std::remove (order.begin(), order.end(), device), order.end());
    fireDeviceListListeners();
}

bool isRunning (AudioObjectID device)
{
    auto* d = find (device);
    if (d == nullptr)
        return false;

    return std::any_of (d->procs.begin(), d->procs.end(),
                        [] (const IoProcRegistration& r) { return r.running; });
}

double nominalRate (AudioObjectID device)
{
    auto* d = find (device);
    return d == nullptr ? 0.0 : d->spec.currentRate;
}

bool hogModeHeld (AudioObjectID device)
{
    auto* d = find (device);
    return d != nullptr && d->hogOwner != -1;
}

int bufferFrameSize (AudioObjectID device)
{
    auto* d = find (device);
    return d == nullptr ? 0 : d->spec.bufferFrameSize;
}

bool pumpInput (AudioObjectID device, const std::vector<std::vector<float>>& channels)
{
    auto* d = find (device);
    if (d == nullptr || channels.empty())
        return false;

    const int numChannels = static_cast<int> (channels.size());
    const int frames = static_cast<int> (channels.front().size());

    BufferListStorage storage;
    auto* list = storage.build (numChannels, frames, d->spec.shape);

    // Pack the caller's per-channel signal in the device's own shape. This is
    // the whole point: the backend must unpack whichever one it is handed.
    if (d->spec.shape == BufferShape::interleaved)
    {
        auto& block = storage.blocks[0];
        for (int f = 0; f < frames; ++f)
            for (int ch = 0; ch < numChannels; ++ch)
                block[static_cast<size_t> (f) * numChannels + ch] = channels[static_cast<size_t> (ch)][static_cast<size_t> (f)];
    }
    else
    {
        for (int ch = 0; ch < numChannels; ++ch)
            std::copy (channels[static_cast<size_t> (ch)].begin(),
                       channels[static_cast<size_t> (ch)].end(),
                       storage.blocks[static_cast<size_t> (ch)].begin());
    }

    AudioTimeStamp now {};
    bool delivered = false;

    for (auto& r : d->procs)
        if (r.running)
        {
            r.proc (device, &now, list, &now, nullptr, &now, r.clientData);
            delivered = true;
        }

    return delivered;
}

bool pumpOutput (AudioObjectID device, int frames, std::vector<std::vector<float>>& out)
{
    auto* d = find (device);
    if (d == nullptr || d->spec.outputChannels <= 0 || frames <= 0)
        return false;

    const int numChannels = d->spec.outputChannels;

    BufferListStorage storage;
    auto* list = storage.build (numChannels, frames, d->spec.shape);

    AudioTimeStamp now {};
    bool delivered = false;

    for (auto& r : d->procs)
        if (r.running)
        {
            r.proc (device, &now, nullptr, &now, list, &now, r.clientData);
            delivered = true;
        }

    if (! delivered)
        return false;

    // Unpack whatever the backend wrote back into per-channel form, so the
    // harness checks the audio rather than the pointer arithmetic.
    out.assign (static_cast<size_t> (numChannels), std::vector<float> (static_cast<size_t> (frames), 0.0f));

    if (d->spec.shape == BufferShape::interleaved)
    {
        const auto& block = storage.blocks[0];
        for (int f = 0; f < frames; ++f)
            for (int ch = 0; ch < numChannels; ++ch)
                out[static_cast<size_t> (ch)][static_cast<size_t> (f)] =
                    block[static_cast<size_t> (f) * numChannels + ch];
    }
    else
    {
        for (int ch = 0; ch < numChannels; ++ch)
            out[static_cast<size_t> (ch)] = storage.blocks[static_cast<size_t> (ch)];
    }

    return true;
}

} // namespace fakeca
