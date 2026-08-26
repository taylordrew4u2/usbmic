#include "TestFramework.h"
#include "Core/RecordingEngine.h"

using namespace mma;

static std::vector<RecordingChannel> twoChannels()
{
    return { { "usb-1", "Mic1", true }, { "usb-2", "Mic2", true } };
}

TEST_CASE (RecordingEngine_StartsWithFixedChannelSet)
{
    RecordingEngine engine;
    REQUIRE (engine.start (twoChannels()));
    REQUIRE (engine.getState() == RecordingState::Recording);
    REQUIRE (engine.getChannels().size() == 2);
}

TEST_CASE (RecordingEngine_CannotStartTwice)
{
    RecordingEngine engine;
    REQUIRE (engine.start (twoChannels()));
    REQUIRE_FALSE (engine.start (twoChannels()));
}

TEST_CASE (RecordingEngine_CannotStartWithNoChannels)
{
    RecordingEngine engine;
    REQUIRE_FALSE (engine.start ({}));
}

TEST_CASE (RecordingEngine_UnplugWritesSilenceWithoutRemovingChannel)
{
    RecordingEngine engine;
    engine.start (twoChannels());
    REQUIRE (engine.onMicUnplugged ("usb-1"));
    REQUIRE (engine.getChannels().size() == 2); // channel count never changes mid-file
    REQUIRE (engine.isWritingSilence ("usb-1"));
    REQUIRE_FALSE (engine.isWritingSilence ("usb-2"));
}

TEST_CASE (RecordingEngine_ReconnectResumesLiveSignalOnSameChannel)
{
    RecordingEngine engine;
    engine.start (twoChannels());
    engine.onMicUnplugged ("usb-1");
    REQUIRE (engine.onMicReconnected ("usb-1"));
    REQUIRE_FALSE (engine.isWritingSilence ("usb-1"));
}

TEST_CASE (RecordingEngine_NewMicMidTakeNeverJoinsRecording)
{
    RecordingEngine engine;
    engine.start (twoChannels());
    std::string message = engine.onNewMicPluggedMidTake ("usb-3", true);
    REQUIRE (engine.getChannels().size() == 2); // still not part of the file
    REQUIRE (message == "Mic added to monitoring. It'll be recorded starting with your next take.");
}

TEST_CASE (RecordingEngine_UnpluggingUnknownDeviceIsANoop)
{
    RecordingEngine engine;
    engine.start (twoChannels());
    REQUIRE_FALSE (engine.onMicUnplugged ("usb-nonexistent"));
}

TEST_CASE (RecordingEngine_StopResetsState)
{
    RecordingEngine engine;
    engine.start (twoChannels());
    engine.stop();
    REQUIRE (engine.getState() == RecordingState::Idle);
    REQUIRE (engine.getChannels().empty());
}

TEST_CASE (RecordingEngine_EventsAreNoopsWhenNotRecording)
{
    RecordingEngine engine;
    REQUIRE_FALSE (engine.onMicUnplugged ("usb-1"));
    REQUIRE_FALSE (engine.onMicReconnected ("usb-1"));
}
