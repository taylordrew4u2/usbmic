#include "OutputDeviceSelector.h"
#include <algorithm>
#include <unordered_set>

namespace mma {

std::vector<OutputDeviceCandidate> OutputDeviceTracker::observe (
    std::vector<OutputDeviceCandidate> snapshot)
{
    std::unordered_set<std::string> present;
    present.reserve (snapshot.size());

    for (auto& candidate : snapshot)
    {
        present.insert (candidate.id);

        const auto existing = connected.find (candidate.id);
        if (existing != connected.end())
        {
            // "Newly connected" describes this connection, not just the one
            // enumeration pass that first saw it. Keep the priority until the
            // device is removed; an unrelated input notification on the next
            // tick must not demote the headphones the user just plugged in.
            candidate.appearedAfterLaunch = existing->second.appearedAfterLaunch;
            candidate.connectionOrder = existing->second.order;
            continue;
        }

        const int order = ++nextConnectionOrder;
        const bool appearedAfterLaunch = haveBaseline;
        connected[candidate.id] = { order, appearedAfterLaunch };
        candidate.appearedAfterLaunch = appearedAfterLaunch;
        candidate.connectionOrder = order;
    }

    for (auto it = connected.begin(); it != connected.end();)
        if (present.count (it->first) == 0)
            it = connected.erase (it);
        else
            ++it;

    haveBaseline = true;
    return snapshot;
}

bool OutputDeviceSelector::isEligible (const OutputDeviceCandidate& candidate)
{
    return candidate.supportsRecordingSampleRate
        && ! candidate.isMicrophonePlaybackEndpoint
        && ! candidate.isAlsoSelectedInput;
}

bool OutputDeviceSelector::supportsRecordingRate (
    uint32_t currentRate,
    const std::vector<uint32_t>& supportedRates,
    uint32_t recordingRate)
{
    return recordingRate == 0
        || currentRate == recordingRate
        || supportedRates.empty()
        || std::find (supportedRates.begin(), supportedRates.end(), recordingRate)
               != supportedRates.end();
}

OutputSelection OutputDeviceSelector::select (const std::vector<OutputDeviceCandidate>& candidates,
                                              const std::string& rememberedId,
                                              const std::string& currentId)
{
    OutputSelection result;

    std::vector<const OutputDeviceCandidate*> eligible;
    for (const auto& c : candidates)
        if (isEligible (c))
            eligible.push_back (&c);

    if (eligible.empty())
    {
        const bool hasRateMismatch = std::any_of (
            candidates.begin(), candidates.end(),
            [] (const OutputDeviceCandidate& c)
            {
                return ! c.supportsRecordingSampleRate
                    && ! c.isMicrophonePlaybackEndpoint
                    && ! c.isAlsoSelectedInput;
            });

        result.explanation = candidates.empty()
            ? "No headphones found. Plug headphones into the computer, or into a headphone amp connected to it."
            : hasRateMismatch
                ? "None of the safe sound outputs can run at this recording's sample rate. Connect a compatible headphone output or choose a recording rate it supports."
                : "The only safe sound outputs are microphones or devices being recorded. Plug headphones into the computer or a headphone amp instead, or you'll hear yourself twice.";
        return result;
    }

    auto pick = [&result] (const OutputDeviceCandidate* c, OutputSelectionReason reason)
    {
        result.found = true;
        result.id = c->id;
        result.displayName = c->displayName;
        result.reason = reason;
    };

    // 1. What the user chose before, if it is present.
    if (! rememberedId.empty())
    {
        auto it = std::find_if (eligible.begin(), eligible.end(),
                                [&] (const OutputDeviceCandidate* c) { return c->id == rememberedId; });

        if (it != eligible.end())
        {
            pick (*it, OutputSelectionReason::RememberedFromPreviousSession);
            return result;
        }
    }

    // Everything below is automatic. A listed-but-unrecorded microphone's own
    // output is never chosen here (§5.2), and a wireless output is only taken
    // when nothing wired is left: AirPods reconnecting or a Bluetooth speaker
    // powering on is not the user plugging in monitoring headphones.
    std::vector<const OutputDeviceCandidate*> wired, wireless;
    for (auto* c : eligible)
        if (! c->belongsToUnrecordedMicrophone)
            (c->isWireless ? wireless : wired).push_back (c);

    // The one automatic route to a listed-but-unrecorded microphone's output:
    // the user made it the macOS default, which on a Mac is how headphones on
    // an interface whose inputs are switched off in SobStage are chosen. It
    // must have been there at launch -- macOS can make a just-plugged USB mic
    // the default on its own -- and is only reached when nothing already in
    // use, nothing just plugged in and no headphone jack applies.
    const OutputDeviceCandidate* unrecordedDefault = nullptr;
    for (auto* c : eligible)
        if (c->belongsToUnrecordedMicrophone && c->isSystemDefault && ! c->isWireless
            && ! c->appearedAfterLaunch)
            unrecordedDefault = c;

    if (wired.empty() && wireless.empty())
    {
        if (unrecordedDefault != nullptr)
        {
            pick (unrecordedDefault, OutputSelectionReason::SystemDefault);
            return result;
        }

        result.explanation = "The only sound outputs belong to microphones that aren't being recorded. Plug headphones into the computer or a headphone amp, or choose one in Settings.";
        return result;
    }

    const auto& automatic = wired.empty() ? wireless : wired;

    // 2. Something plugged in after launch, most recent first.
    const OutputDeviceCandidate* newest = nullptr;
    for (auto* c : automatic)
        if (c->appearedAfterLaunch && (newest == nullptr || c->connectionOrder > newest->connectionOrder))
            newest = c;

    if (newest != nullptr)
    {
        pick (newest, OutputSelectionReason::NewlyConnected);
        return result;
    }

    // 3. Anything with a real headphone jack.
    auto jack = std::find_if (automatic.begin(), automatic.end(),
                              [] (const OutputDeviceCandidate* c) { return c->hasPhysicalHeadphoneJack; });

    if (jack != automatic.end())
    {
        pick (*jack, OutputSelectionReason::PhysicalHeadphoneJack);
        return result;
    }

    // Nothing new and no headphone jack: keep the output already in use while
    // it is still an automatic candidate. Priorities 4 and 5 are only a
    // starting point. macOS moves its default output on its own -- onto a USB
    // mic that was just plugged in, or off a device this app holds in hog mode
    // -- and following it on the next device-list pass would move the monitor
    // mix from the performers' amp to the room speakers mid-show.
    if (! currentId.empty())
    {
        auto current = std::find_if (automatic.begin(), automatic.end(),
                                     [&] (const OutputDeviceCandidate* c) { return c->id == currentId; });

        if (current != automatic.end())
        {
            pick (*current, OutputSelectionReason::CurrentOutput);
            return result;
        }
    }

    // 4. Whatever the OS considers default.
    auto def = std::find_if (automatic.begin(), automatic.end(),
                             [] (const OutputDeviceCandidate* c) { return c->isSystemDefault; });

    if (def != automatic.end())
    {
        pick (*def, OutputSelectionReason::SystemDefault);
        return result;
    }

    if (unrecordedDefault != nullptr && ! wired.empty())
    {
        pick (unrecordedDefault, OutputSelectionReason::SystemDefault);
        return result;
    }

    // The system default can itself be excluded because it is also a selected
    // input. Prefer the computer's own output next. Falling straight to the
    // enumeration-first USB endpoint selected HDMI capture-card audio on a rig
    // pinned to 44.1 kHz; that unused 48 kHz output then prevented every input
    // stream from opening and left a sample-rate warning on screen forever.
    auto builtIn = std::find_if (automatic.begin(), automatic.end(),
                                 [] (const OutputDeviceCandidate* c) { return c->isBuiltIn; });

    if (builtIn != automatic.end())
    {
        pick (*builtIn, OutputSelectionReason::BuiltInOutput);
        return result;
    }

    // Eligible devices exist but none matched a stated or safe fallback
    // priority. Take the first rather than leaving the room without a monitor
    // mix (§5.1: live from launch).
    pick (automatic.front(), OutputSelectionReason::SystemDefault);
    return result;
}

std::string OutputDeviceSelector::currentIdToKeep (const std::vector<OutputDeviceCandidate>& candidates,
                                                   const std::string& currentId,
                                                   bool currentWasHeadphoneJack)
{
    if (! currentWasHeadphoneJack || currentId.empty())
        return currentId;

    for (const auto& c : candidates)
        if (c.id == currentId && ! c.hasPhysicalHeadphoneJack)
            return {};

    return currentId;
}

OutputDeviceCandidate OutputDeviceSelector::candidateFromDescriptor (
    const AudioDeviceDescriptor& d,
    uint32_t recordingRate,
    const std::vector<KnownMicrophone>& microphones)
{
    OutputDeviceCandidate c;
    c.id = d.usbLocationId.empty() ? d.name : d.usbLocationId;
    c.displayName = d.name;
    c.hasPhysicalHeadphoneJack = d.hasPhysicalHeadphoneJack;
    c.isBuiltIn = d.isBuiltIn;
    c.isSystemDefault = d.isSystemDefault;
    c.isWireless = d.isWireless;

    // The monitor output shares the recording clock. A fixed-48 kHz HDMI
    // capture-card endpoint cannot serve a 44.1 kHz T12S take and must not
    // displace a compatible Mac output merely because it hot-plugged most
    // recently. An empty capability list means the backend cannot say, so
    // keep it eligible and let the open path report any real refusal.
    c.supportsRecordingSampleRate = supportsRecordingRate (
        d.currentSampleRate, d.supportedSampleRates, recordingRate);

    // §5.2: a microphone's own playback endpoint is never a monitor output.
    c.isMicrophonePlaybackEndpoint = d.isMicrophone;

    // §5.5: refuse to route output to a device that is also a capture device.
    // A duplex mic that is listed but not recorded (switched off, or past the
    // cap) shares the same id on CoreAudio; keep it out of automatic choice.
    for (const auto& mic : microphones)
        if (! d.usbLocationId.empty() && mic.locationId == d.usbLocationId)
        {
            if (mic.included)
                c.isAlsoSelectedInput = true;
            else
                c.belongsToUnrecordedMicrophone = true;
        }

    return c;
}

} // namespace mma
