#include "SystemAggregateDevice.h"
#include "PlatformMacros.h"

#if JUCE_MAC

#include "MacCoreAudioQueries.h"

#include <chrono>
#include <thread>
#include <vector>

namespace mma {

namespace {

CFStringRef makeCFString (const std::string& text)
{
    return CFStringCreateWithCString (kCFAllocatorDefault, text.c_str(), kCFStringEncodingUTF8);
}

using macaudio::countChannels;
using macaudio::findDeviceByUID;
using macaudio::readActiveSubDevices;
using macaudio::readStringProperty;

/// How long publish() waits for a new aggregate to list its sub-devices. The
/// HAL builds it asynchronously, and for a moment after the create returns
/// its active list can be empty.
constexpr int kLayoutSettleTimeoutMs = 500;
constexpr int kLayoutSettlePollMs = 10;

// A public aggregate outlives the process that made it. One left behind by a
// crash, a force-quit or a power cut holds our fixed UID, so the next launch's
// create was refused ("Couldn't make the combined device") and the stale one,
// listing last session's microphones, stayed in every app's input list. Ours
// by UID, so it is ours to take away.
void destroyLeftoverAggregate()
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const auto stale = findDeviceByUID (kOurAggregateUid);
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

        // The caller reads getOutputLayout() straight after this to decide
        // whether the combined device can carry the headphone mix. Read too
        // early the list was empty, so it could not, and monitoring settled on
        // some other output. Waited for here, briefly and off the audio
        // thread; a sub-device that is slower still is caught by the caller's
        // re-check.
        waitForCombinedLayout ([this] { return getOutputLayout(); }, deviceUids,
                               kLayoutSettleTimeoutMs / kLayoutSettlePollMs,
                               [] { std::this_thread::sleep_for (std::chrono::milliseconds (kLayoutSettlePollMs)); });

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
        for (auto sub : readActiveSubDevices (aggregateId))
            layout.push_back ({ readStringProperty (sub, kAudioDevicePropertyDeviceUID), countChannels (sub, false) });

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
