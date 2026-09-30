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
    // Mid-take nothing opens its stream, so it is not in the headphones either,
    // and the line must not say it is.
    REQUIRE (message == "Mic plugged in. It isn't in this take or the headphones yet. It'll be recorded starting with your next take.");
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

// §6.5 unplug and replug mid-take. A device that leaves the device list takes
// its stream with it: on macOS the replugged mic is a new AudioObjectID, and
// the take's only IOProc is still bound to the destroyed one. Nothing may
// reopen a stream mid-take, so the channel stays silent until the next take --
// and must never be announced as recording again.
TEST_CASE (RecordingEngine_ReplugMidTakeStaysSilentAndIsNotAnnouncedAsLive)
{
    RecordingEngine engine;
    engine.start (twoChannels());

    REQUIRE (engine.onDeviceListSeen ("usb-1", false, false) == MidTakeMicChange::Unplugged);
    REQUIRE (engine.isWritingSilence ("usb-1"));

    // Still gone on a later notification: nothing new to say.
    REQUIRE (engine.onDeviceListSeen ("usb-1", false, false) == MidTakeMicChange::None);

    // Back in the list, same key. It must not be called live.
    REQUIRE (engine.onDeviceListSeen ("usb-1", true, false) == MidTakeMicChange::BackNextTake);
    REQUIRE (engine.isWritingSilence ("usb-1"));

    // Told once, however many times the list is re-read.
    REQUIRE (engine.onDeviceListSeen ("usb-1", true, false) == MidTakeMicChange::None);
    REQUIRE (engine.isWritingSilence ("usb-1"));

    // The other mic is untouched throughout.
    REQUIRE (engine.onDeviceListSeen ("usb-2", true, false) == MidTakeMicChange::None);
    REQUIRE_FALSE (engine.isWritingSilence ("usb-2"));
}

TEST_CASE (RecordingEngine_ReplugIsLiveAgainFromTheNextTake)
{
    RecordingEngine engine;
    engine.start (twoChannels());
    engine.onDeviceListSeen ("usb-1", false, false);
    engine.onDeviceListSeen ("usb-1", true, false);
    engine.stop();

    // Stop pays the deferred restart, which reopens the mic on its new device.
    engine.start (twoChannels());
    REQUIRE_FALSE (engine.isWritingSilence ("usb-1"));
    REQUIRE (engine.onDeviceListSeen ("usb-1", true, false) == MidTakeMicChange::None);
}

TEST_CASE (RecordingEngine_DeadStreamStillListedIsSilencedOnce)
{
    RecordingEngine engine;
    engine.start (twoChannels());
    REQUIRE (engine.onDeviceListSeen ("usb-2", true, true) == MidTakeMicChange::Unplugged);
    REQUIRE (engine.isWritingSilence ("usb-2"));
    REQUIRE (engine.onDeviceListSeen ("usb-2", true, true) == MidTakeMicChange::None);

    // It then leaves the list and comes back: still no claim that it is live.
    REQUIRE (engine.onDeviceListSeen ("usb-2", false, false) == MidTakeMicChange::None);
    REQUIRE (engine.onDeviceListSeen ("usb-2", true, false) == MidTakeMicChange::BackNextTake);
    REQUIRE (engine.isWritingSilence ("usb-2"));
}

TEST_CASE (RecordingEngine_DeviceListOutsideATakeChangesNothing)
{
    RecordingEngine engine;
    REQUIRE (engine.onDeviceListSeen ("usb-1", false, false) == MidTakeMicChange::None);
}
