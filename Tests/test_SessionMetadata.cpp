#include "TestFramework.h"
#include "Core/SessionMetadata.h"
#include "Core/Json.h"
#include "Core/TakeStopSnapshot.h"

using namespace mma;

TEST_CASE (SessionMetadata_RoundTripsThroughJson)
{
    SessionMetadata m;
    m.appVersion = "0.1.0";
    m.startTimestampIso = "2026-08-26T14:32:00Z";
    m.sampleRate = 48000.0;
    m.bitDepth = 24;
    m.bufferSizeSamples = 64;
    m.measuredLatencyMs = 5.2;
    m.mirrorEnabled = true;
    m.mirrorActive = true;
    m.mirrorPath = "/home/user/RECORDINGS/mirror";

    DeviceRecord d;
    d.name = "Yeti-Kitchen";
    d.usbId = "usb-1-2";
    d.trimDb = 3.5f;
    m.devices.push_back (d);

    DriftLogEntry drift;
    drift.timestampSeconds = 10.0;
    drift.deviceUsbId = "usb-1-2";
    drift.drift_ppm = 42.5;
    m.driftLog.push_back (drift);

    BufferChangeEntry buf;
    buf.timestampSeconds = 5.0;
    buf.oldBufferSize = 64;
    buf.newBufferSize = 128;
    m.bufferChanges.push_back (buf);

    DropoutEntry dropout;
    dropout.timestampSeconds = 20.0;
    dropout.deviceUsbId = "usb-1-2";
    dropout.description = "unplugged";
    m.dropouts.push_back (dropout);

    FailoverEntry failover;
    failover.timestampSeconds = 30.0;
    failover.oldMasterUsbId = "usb-1-2";
    failover.newMasterUsbId = "usb-1-3";
    m.failovers.push_back (failover);

    std::string json = m.toJsonString();
    SessionMetadata roundTripped = SessionMetadata::fromJsonString (json);

    REQUIRE (roundTripped.appVersion == m.appVersion);
    REQUIRE (roundTripped.startTimestampIso == m.startTimestampIso);
    REQUIRE_NEAR (roundTripped.sampleRate, m.sampleRate, 1e-6);
    REQUIRE (roundTripped.bitDepth == m.bitDepth);
    REQUIRE (roundTripped.bufferSizeSamples == m.bufferSizeSamples);
    REQUIRE (roundTripped.devices.size() == 1);
    REQUIRE (roundTripped.devices[0].name == "Yeti-Kitchen");
    REQUIRE_NEAR (roundTripped.devices[0].trimDb, 3.5, 1e-4);
    REQUIRE (roundTripped.driftLog.size() == 1);
    REQUIRE_NEAR (roundTripped.driftLog[0].drift_ppm, 42.5, 1e-4);
    REQUIRE (roundTripped.bufferChanges.size() == 1);
    REQUIRE (roundTripped.dropouts.size() == 1);
    REQUIRE (roundTripped.failovers.size() == 1);
    REQUIRE (roundTripped.failovers[0].newMasterUsbId == "usb-1-3");
    REQUIRE (roundTripped.mirrorPath == m.mirrorPath);
}

TEST_CASE (SessionMetadata_RecordsEachInterfaceInputsOwnTrim)
{
    // session.json says what trim each stem's mix was made with. On an
    // interface that is one value per input, not one for the box.
    SessionMetadata m;
    DeviceRecord d;
    d.name = "Scarlett 4i4";
    d.usbId = "usb-3|SN123";
    d.inputTrimDb[0] = 6.0f;
    d.inputTrimDb[1] = -1.5f;
    m.devices.push_back (d);

    const auto back = SessionMetadata::fromJsonString (m.toJsonString());

    REQUIRE (back.devices.size() == 1);
    REQUIRE (back.devices[0].inputTrimDb.size() == 2);
    REQUIRE_NEAR (back.devices[0].inputTrimDb.at (0), 6.0, 1e-4);
    REQUIRE_NEAR (back.devices[0].inputTrimDb.at (1), -1.5, 1e-4);
}

TEST_CASE (SessionMetadata_EmptySessionRoundTrips)
{
    SessionMetadata m;
    std::string json = m.toJsonString();
    SessionMetadata roundTripped = SessionMetadata::fromJsonString (json);
    REQUIRE (roundTripped.devices.empty());
    REQUIRE (roundTripped.driftLog.empty());
}

TEST_CASE (JsonValue_ParsesNestedObjectsAndArrays)
{
    std::string text = R"({"a": 1, "b": [1, 2, 3], "c": {"d": "hello"}, "e": true, "f": null})";
    JsonValue v = JsonValue::parse (text);
    REQUIRE_NEAR (v.find ("a")->asDouble(), 1.0, 1e-9);
    REQUIRE (v.find ("b")->asArray().size() == 3);
    REQUIRE (v.find ("c")->find ("d")->asString() == "hello");
    REQUIRE (v.find ("e")->asBool() == true);
    REQUIRE (v.find ("f")->isNull());
}

TEST_CASE (JsonValue_EscapesSpecialCharactersInStrings)
{
    JsonValue v = JsonValue::makeObject();
    v["text"] = JsonValue (std::string ("line1\nline2\"quoted\""));
    std::string dumped = v.dump (0);
    JsonValue reparsed = JsonValue::parse (dumped);
    REQUIRE (reparsed.find ("text")->asString() == "line1\nline2\"quoted\"");
}

TEST_CASE (SessionMetadata_videoFilesSurviveARoundTrip)
{
    SessionMetadata meta;
    meta.appVersion = "0.5.0";
    meta.videos.push_back ({ "Kitchen Cam", "V01_Kitchen-Cam.mov", false });
    meta.videos.push_back ({ "Wide", "V02_Wide.mov", false });

    const auto restored = SessionMetadata::fromJsonString (meta.toJsonString());

    REQUIRE (restored.videos.size() == 2u);
    REQUIRE (restored.videos[0].cameraName == std::string ("Kitchen Cam"));
    REQUIRE (restored.videos[1].fileName == std::string ("V02_Wide.mov"));

    // The picture never carries the sound: that is the whole arrangement, and
    // an editor reading this file is entitled to be told so rather than having
    // to open the video to find out.
    REQUIRE_FALSE (restored.videos[0].hasAudioTrack);
    REQUIRE_FALSE (restored.videos[1].hasAudioTrack);
}

TEST_CASE (SessionMetadata_aTakeWithNoCamerasRecordsNoVideos)
{
    SessionMetadata meta;
    const auto restored = SessionMetadata::fromJsonString (meta.toJsonString());
    REQUIRE (restored.videos.empty());
}

TEST_CASE (TakeStopSnapshot_TheStopTimeRecordDescribesTheMomentStopWasPressed)
{
    // With a camera still finishing its movie the stop-time session.json is
    // written later, from the camera's completion. By then the engine has
    // stopped, so the live clock reads zero -- every drift and dropout entry was
    // stamped 0.0 s -- and the counters have run on through monitoring, or
    // restarted with a rebuilt coordinator.
    TakeFigures atStop;
    atStop.elapsedSeconds = 1234.5;
    atStop.stopTimestampIso = "2026-09-29T21:00:00Z";
    atStop.bufferSizeSamples = 256;
    atStop.framesDropped = 11;
    atStop.overrunSamples = 22;
    atStop.underrunSamples = 33;
    atStop.framesMissedByLayout = 44;
    atStop.backendFramesDropped = 55;

    TakeFigures later;
    later.elapsedSeconds = 0.0;
    later.stopTimestampIso = "2026-09-29T21:00:19Z";
    later.bufferSizeSamples = 512;

    TakeStopSnapshot snapshot;
    snapshot.capture (atStop);

    const auto figures = snapshot.resolve (true, later);
    REQUIRE (figures.elapsedSeconds == 1234.5);
    REQUIRE (figures.stopTimestampIso == "2026-09-29T21:00:00Z");
    REQUIRE (figures.bufferSizeSamples == 256);
    REQUIRE (figures.framesDropped == 11u);
    REQUIRE (figures.overrunSamples == 22u);
    REQUIRE (figures.underrunSamples == 33u);
    REQUIRE (figures.framesMissedByLayout == 44u);
    REQUIRE (figures.backendFramesDropped == 55u);
}

TEST_CASE (TakeStopSnapshot_TheStartTimeRecordAndANewTakeUseLiveFigures)
{
    // The write at the start of a take has no stop to describe, and a take
    // must never be recorded with the previous take's stop.
    TakeFigures atStop;
    atStop.elapsedSeconds = 99.0;
    atStop.stopTimestampIso = "2026-09-29T21:00:00Z";

    TakeFigures live;
    live.elapsedSeconds = 0.25;
    live.bufferSizeSamples = 128;

    TakeStopSnapshot snapshot;
    snapshot.capture (atStop);
    REQUIRE (snapshot.resolve (false, live).elapsedSeconds == 0.25);
    REQUIRE (snapshot.resolve (false, live).stopTimestampIso.empty());

    snapshot.clear();
    REQUIRE_FALSE (snapshot.hasSnapshot());
    REQUIRE (snapshot.resolve (true, live).elapsedSeconds == 0.25);
    REQUIRE (snapshot.resolve (true, live).bufferSizeSamples == 128);
}
