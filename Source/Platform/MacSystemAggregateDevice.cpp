#include "SystemAggregateDevice.h"
#include "PlatformMacros.h"

#if JUCE_MAC

#include <CoreAudio/CoreAudio.h>
#include <cstdint>
#include <vector>

namespace mma {

namespace {

CFStringRef makeCFString (const std::string& text)
{
    return CFStringCreateWithCString (kCFAllocatorDefault, text.c_str(), kCFStringEncodingUTF8);
}

std::string readUid (AudioObjectID device)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyDeviceUID,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    CFStringRef value = nullptr;
    UInt32 size = sizeof (value);

    if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, &value) != noErr || value == nullptr)
        return {};

    char buffer[512] = {};
    const bool ok = CFStringGetCString (value, buffer, sizeof (buffer), kCFStringEncodingUTF8);
    CFRelease (value);
    return ok ? std::string (buffer) : std::string();
}

// Scanned rather than translated, the same way the capture backend finds a
// device: the list read is the one call every macOS release answers alike.
AudioObjectID findDeviceWithUid (const std::string& uid)
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
        if (readUid (device) == uid)
            return device;

    return kAudioObjectUnknown;
}

int countOutputChannels (AudioObjectID device)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyStreamConfiguration,
                                         kAudioObjectPropertyScopeOutput,
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

// A public aggregate outlives the process that made it. One left behind by a
// crash, a force-quit or a power cut holds our fixed UID, so the next launch's
// create was refused ("Couldn't make the combined device") and the stale one,
// listing last session's microphones, stayed in every app's input list. Ours
// by UID, so it is ours to take away.
void destroyLeftoverAggregate()
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const auto stale = findDeviceWithUid (kOurAggregateUid);
        if (stale == kAudioObjectUnknown)
            return;

        if (AudioHardwareDestroyAggregateDevice (stale) != noErr)
            return;
    }
}

} // namespace

class MacSystemAggregateDevice : public SystemAggregateDevice
{
public:
    ~MacSystemAggregateDevice() override { remove(); }

    bool publish (const std::string& name,
                  const std::vector<std::string>& deviceUids,
                  const std::string& masterUid) override
    {
        remove();

        // One left behind by an earlier run that crashed is cleared the first
        // time this run publishes, and only then: after that, a device with
        // our UID that is not ours belongs to another running copy (a second
        // user via Fast User Switching), and destroying it on every republish
        // and at quit would pull its input out from under the apps using it.
        if (! clearedLeftoverFromEarlierRun)
        {
            destroyLeftoverAggregate();
            clearedLeftoverFromEarlierRun = true;
        }

        publishedName = name;
        publishedCount = static_cast<int> (deviceUids.size());

        if (deviceUids.empty())
            return true;

        CFMutableArrayRef subDevices = CFArrayCreateMutable (kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);

        for (const auto& uid : deviceUids)
        {
            CFMutableDictionaryRef sub = CFDictionaryCreateMutable (kCFAllocatorDefault, 0,
                                                                    &kCFTypeDictionaryKeyCallBacks,
                                                                    &kCFTypeDictionaryValueCallBacks);
            CFStringRef cfUid = makeCFString (uid);
            CFDictionarySetValue (sub, CFSTR (kAudioSubDeviceUIDKey), cfUid);
            CFRelease (cfUid);

            // §3: the master defines the timebase; the HAL resamples everyone
            // else onto it. Same rule the in-app capture path follows.
            const int drift = (uid == masterUid) ? 0 : 1;
            CFNumberRef cfDrift = CFNumberCreate (kCFAllocatorDefault, kCFNumberIntType, &drift);
            CFDictionarySetValue (sub, CFSTR (kAudioSubDeviceDriftCompensationKey), cfDrift);
            CFRelease (cfDrift);

            CFArrayAppendValue (subDevices, sub);
            CFRelease (sub);
        }

        CFMutableDictionaryRef description = CFDictionaryCreateMutable (kCFAllocatorDefault, 0,
                                                                        &kCFTypeDictionaryKeyCallBacks,
                                                                        &kCFTypeDictionaryValueCallBacks);

        CFStringRef cfName = makeCFString (name.empty() ? "SobStage" : name);
        CFStringRef cfAggregateUid = makeCFString (kOurAggregateUid);
        CFDictionarySetValue (description, CFSTR (kAudioAggregateDeviceNameKey), cfName);
        CFDictionarySetValue (description, CFSTR (kAudioAggregateDeviceUIDKey), cfAggregateUid);
        CFDictionarySetValue (description, CFSTR (kAudioAggregateDeviceSubDeviceListKey), subDevices);
        CFRelease (cfName);
        CFRelease (cfAggregateUid);
        CFRelease (subDevices);

        if (! masterUid.empty())
        {
            CFStringRef cfMaster = makeCFString (masterUid);
            CFDictionarySetValue (description, CFSTR (kAudioAggregateDeviceMainSubDeviceKey), cfMaster);
            CFRelease (cfMaster);
        }

        // Deliberately NOT marked private: the whole point is that other apps
        // see it. (kAudioAggregateDeviceIsPrivateKey absent == public.)
        const OSStatus err = AudioHardwareCreateAggregateDevice (description, &aggregateId);
        CFRelease (description);

        if (err != noErr)
        {
            aggregateId = kAudioObjectUnknown;
            publishedCount = 0;
            return false;
        }

        return true;
    }

    void remove() override
    {
        if (aggregateId != kAudioObjectUnknown)
        {
            AudioHardwareDestroyAggregateDevice (aggregateId);
            aggregateId = kAudioObjectUnknown;
        }

        publishedCount = 0;
    }

    bool isPublished() const override { return aggregateId != kAudioObjectUnknown; }

    std::vector<CombinedDeviceOutputs> getOutputLayout() const override
    {
        std::vector<CombinedDeviceOutputs> layout;

        if (aggregateId == kAudioObjectUnknown)
            return layout;

        // The ACTIVE list, because only a sub-device that is present brings
        // channels -- and in the order the HAL lays those channels out.
        AudioObjectPropertyAddress address { kAudioAggregateDevicePropertyActiveSubDeviceList,
                                             kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain };
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize (aggregateId, &address, 0, nullptr, &size) != noErr || size == 0)
            return layout;

        std::vector<AudioObjectID> subDevices (size / sizeof (AudioObjectID));
        if (AudioObjectGetPropertyData (aggregateId, &address, 0, nullptr, &size, subDevices.data()) != noErr)
            return layout;

        for (auto sub : subDevices)
            layout.push_back ({ readUid (sub), countOutputChannels (sub) });

        return layout;
    }

    std::string getStatus() const override
    {
        if (publishedCount <= 0)
            return "No microphones connected, so other apps see nothing yet.";

        return "Other apps see one input device called \"" + publishedName + "\" ("
               + std::to_string (publishedCount)
               + (publishedCount == 1 ? " microphone)." : " microphones).");
    }

private:
    AudioObjectID aggregateId = kAudioObjectUnknown;
    bool clearedLeftoverFromEarlierRun = false;
    std::string publishedName;
    int publishedCount = 0;
};

std::unique_ptr<SystemAggregateDevice> createSystemAggregateDevice()
{
    return std::make_unique<MacSystemAggregateDevice>();
}

} // namespace mma

#endif // JUCE_MAC
