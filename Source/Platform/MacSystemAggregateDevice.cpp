#include "SystemAggregateDevice.h"
#include "PlatformMacros.h"

#if JUCE_MAC

#include <CoreAudio/CoreAudio.h>

namespace mma {

namespace {

CFStringRef makeCFString (const std::string& text)
{
    return CFStringCreateWithCString (kCFAllocatorDefault, text.c_str(), kCFStringEncodingUTF8);
}

/// One stable UID for our aggregate, so republishing replaces the device other
/// apps already selected instead of stacking a second one beside it.
constexpr const char* kOurAggregateUid = "com.multimicaggregator.combined";

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
    std::string publishedName;
    int publishedCount = 0;
};

std::unique_ptr<SystemAggregateDevice> createSystemAggregateDevice()
{
    return std::make_unique<MacSystemAggregateDevice>();
}

} // namespace mma

#endif // JUCE_MAC
