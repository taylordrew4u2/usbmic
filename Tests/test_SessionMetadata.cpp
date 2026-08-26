#include "TestFramework.h"
#include "Core/SessionMetadata.h"
#include "Core/Json.h"

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
