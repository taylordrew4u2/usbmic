#include "RecordingEngine.h"
#include <algorithm>

namespace mma {

bool RecordingEngine::start (std::vector<RecordingChannel> channelsIn)
{
    if (state == RecordingState::Recording || channelsIn.empty())
        return false;

    channels = std::move (channelsIn);
    for (auto& c : channels)
        c.live = true;
    state = RecordingState::Recording;
    return true;
}

void RecordingEngine::stop()
{
    state = RecordingState::Idle;
    channels.clear();
}

RecordingChannel* RecordingEngine::findChannel (const std::string& deviceUsbId)
{
    auto it = std::find_if (channels.begin(), channels.end(), [&] (const RecordingChannel& c) {
        return c.deviceUsbId == deviceUsbId;
    });
    return it == channels.end() ? nullptr : &(*it);
}

const RecordingChannel* RecordingEngine::findChannel (const std::string& deviceUsbId) const
{
    auto it = std::find_if (channels.begin(), channels.end(), [&] (const RecordingChannel& c) {
        return c.deviceUsbId == deviceUsbId;
    });
    return it == channels.end() ? nullptr : &(*it);
}

bool RecordingEngine::onMicUnplugged (const std::string& deviceUsbId)
{
    if (state != RecordingState::Recording)
        return false;
    if (auto* c = findChannel (deviceUsbId))
    {
        c->live = false;
        return true;
    }
    return false;
}

bool RecordingEngine::onMicReconnected (const std::string& deviceUsbId)
{
    if (state != RecordingState::Recording)
        return false;
    if (auto* c = findChannel (deviceUsbId))
    {
        c->live = true;
        return true;
    }
    return false;
}

MidTakeMicChange RecordingEngine::onDeviceListSeen (const std::string& deviceUsbId, bool listed, bool streamDead)
{
    if (state != RecordingState::Recording)
        return MidTakeMicChange::None;

    auto* c = findChannel (deviceUsbId);
    if (c == nullptr)
        return MidTakeMicChange::None;

    // Either way its stream is gone for good: a dead IOProc is never revived,
    // and one on a device that left the list is bound to an object the OS has
    // destroyed. Only the deferred rebuild at Stop opens a new one.
    if (! listed || streamDead)
        c->streamLost = true;

    const bool live = listed && ! streamDead && ! c->streamLost;
    if (! live && c->live)
    {
        c->live = false;
        return MidTakeMicChange::Unplugged;
    }
    if (live && ! c->live)
    {
        c->live = true;
        return MidTakeMicChange::Reconnected;
    }
    if (listed && ! streamDead && c->streamLost && ! c->backAnnounced)
    {
        c->backAnnounced = true;
        return MidTakeMicChange::BackNextTake;
    }
    return MidTakeMicChange::None;
}

std::string RecordingEngine::onNewMicPluggedMidTake (const std::string& /*deviceUsbId*/, bool isCurrentlyRecording) const
{
    if (isCurrentlyRecording)
        // Nothing opens a stream mid-take -- the rebuild waits for Stop -- so
        // it is not in the headphones yet either.
        return "Mic plugged in. It isn't in this take or the headphones yet. It'll be recorded starting with your next take.";
    return "Mic added to monitoring.";
}

bool RecordingEngine::isWritingSilence (const std::string& deviceUsbId) const
{
    if (auto* c = findChannel (deviceUsbId))
        return ! c->live;
    return false;
}

} // namespace mma
