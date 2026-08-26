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

std::string RecordingEngine::onNewMicPluggedMidTake (const std::string& /*deviceUsbId*/, bool isCurrentlyRecording) const
{
    if (isCurrentlyRecording)
        return "Mic added to monitoring. It'll be recorded starting with your next take.";
    return "Mic added to monitoring.";
}

bool RecordingEngine::isWritingSilence (const std::string& deviceUsbId) const
{
    if (auto* c = findChannel (deviceUsbId))
        return ! c->live;
    return false;
}

} // namespace mma
