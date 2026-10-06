#pragma once

// The handful of HAL property reads both CoreAudioBackend.cpp and
// MacSystemAggregateDevice.cpp need. Each file kept its own copy, and the two
// had already drifted -- one read a channel count through a byte vector, not
// aligned for an AudioBufferList's pointers, the other through 8-byte words.
//
// Include only from code already inside JUCE_MAC; under MMA_SIMULATE_MAC the
// stand-in headers answer these calls.

#include <CoreAudio/CoreAudio.h>

#include <cstdint>
#include <string>
#include <vector>

namespace mma::macaudio {

/// A CoreAudio string property (name, UID, ...) as UTF-8, or empty when the
/// device will not say. The CFString is released here.
inline std::string readStringProperty (AudioObjectID device, AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress address { selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef value = nullptr;
    UInt32 size = sizeof (value);

    if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, &value) != noErr || value == nullptr)
        return {};

    char buffer[512] = {};
    const bool ok = CFStringGetCString (value, buffer, sizeof (buffer), kCFStringEncodingUTF8);
    CFRelease (value);
    return ok ? std::string (buffer) : std::string();
}

/// Resolves a device UID (the stable identifier §2.4 stores) to a live
/// AudioObjectID. Returns kAudioObjectUnknown when the device is not present,
/// which is the normal case after an unplug.
///
/// Scanned rather than translated: the list read is the one call every macOS
/// release answers alike.
inline AudioObjectID findDeviceByUID (const std::string& uid)
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

/// Channels on one scope of a device, summed over its streams. 0 when the
/// device has none there or cannot be asked.
inline int countChannels (AudioObjectID device, bool input)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyStreamConfiguration,
                                         input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput,
                                         kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (device, &address, 0, nullptr, &size) != noErr || size == 0)
        return 0;

    // 8-byte words: an AudioBufferList holds pointers, so a byte vector is
    // not aligned for it.
    std::vector<uint64_t> storage ((size + sizeof (uint64_t) - 1) / sizeof (uint64_t));
    auto* list = reinterpret_cast<AudioBufferList*> (storage.data());
    if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, list) != noErr)
        return 0;

    int channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
        channels += static_cast<int> (list->mBuffers[i].mNumberChannels);
    return channels;
}

/// An aggregate device's ACTIVE sub-devices -- only one that is present
/// brings channels -- in the order the HAL lays those channels out. Empty
/// when the aggregate has none yet or cannot be asked.
inline std::vector<AudioObjectID> readActiveSubDevices (AudioObjectID aggregate)
{
    AudioObjectPropertyAddress address { kAudioAggregateDevicePropertyActiveSubDeviceList,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (aggregate, &address, 0, nullptr, &size) != noErr || size == 0)
        return {};

    std::vector<AudioObjectID> subDevices (size / sizeof (AudioObjectID));
    if (AudioObjectGetPropertyData (aggregate, &address, 0, nullptr, &size, subDevices.data()) != noErr)
        return {};

    subDevices.resize (size / sizeof (AudioObjectID));
    return subDevices;
}

} // namespace mma::macaudio
