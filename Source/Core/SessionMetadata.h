#pragma once
#include "Json.h"
#include <string>
#include <vector>
#include <optional>

namespace mma {

struct DeviceRecord
{
    std::string name;
    std::string usbId;
    float trimDb = 0.0f;
};

struct DriftLogEntry
{
    double timestampSeconds = 0.0;
    std::string deviceUsbId;
    double drift_ppm = 0.0;
};

struct BufferChangeEntry
{
    double timestampSeconds = 0.0;
    int oldBufferSize = 0;
    int newBufferSize = 0;
};

struct DropoutEntry
{
    double timestampSeconds = 0.0;
    std::string deviceUsbId;
    std::string description;
};

struct FailoverEntry
{
    double timestampSeconds = 0.0;
    std::string oldMasterUsbId;
    std::string newMasterUsbId;
};

/// One camera's contribution to a take. §6.2's session.json is what a DAW or an
/// editor is read by later, and a video file with no sound track needs the
/// session origin beside it to line up against the stems -- which is exactly
/// what session.json already carries for the audio.
struct VideoRecord
{
    std::string cameraName;
    std::string fileName;      // "V01_Kitchen-Cam.mov"
    bool hasAudioTrack = false; // always false: the sound is the WAVs, deliberately
};

/// §6.2 session.json schema. Pure data + JSON (de)serialization, no file I/O
/// here -- SessionWriter owns when/where this gets written to disk.
struct SessionMetadata
{
    std::string appVersion;
    std::string startTimestampIso;
    std::string stopTimestampIso; // empty until the session is stopped
    double sampleRate = 48000.0;
    int bitDepth = 24;
    int bufferSizeSamples = 64;
    double measuredLatencyMs = 0.0;
    std::vector<DeviceRecord> devices;
    std::vector<DriftLogEntry> driftLog;
    std::vector<BufferChangeEntry> bufferChanges;
    std::vector<DropoutEntry> dropouts;
    std::vector<FailoverEntry> failovers;
    std::vector<VideoRecord> videos;
    bool mirrorEnabled = true;
    bool mirrorActive = true;
    std::string mirrorPath;

    JsonValue toJson() const;
    static SessionMetadata fromJson (const JsonValue& v);

    std::string toJsonString() const { return toJson().dump (2); }
    static SessionMetadata fromJsonString (const std::string& s) { return fromJson (JsonValue::parse (s)); }
};

} // namespace mma
