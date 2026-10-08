#pragma once
#include "Json.h"
#include <string>
#include <vector>
#include <optional>
#include <map>

namespace mma {

struct DeviceRecord
{
    std::string name;
    std::string usbId;
    float trimDb = 0.0f;

    /// Trim per physical input of an interface, keyed like the port settings.
    /// Empty for a single microphone, whose trim is trimDb.
    std::map<int, float> inputTrimDb;

    /// How this device's channels were lined up with the rest of the rig.
    /// Every stem is held back to the slowest device's figure, so a clap lands
    /// on the same frame in all of them; these say by how much, and why, so a
    /// take whose stems an editor finds offset can be explained from its own
    /// record. All in frames at the take's rate. -1 for the latency means not
    /// known: the device did not open, or this is an older session.json.
    ///   inputLatencyFrames   -- what the driver reported (device latency,
    ///                           safety offset and stream latency on macOS;
    ///                           zero where the platform does not report it)
    ///   ioBlockFrames        -- the largest block the device handed over: its
    ///                           real IO size, which a sample can wait for
    ///                           before it is delivered
    ///   alignmentDelayFrames -- what was added on top of both so this device
    ///                           lines up with the slowest one
    ///   alignmentSilenceFrames -- silence written into this device's files
    ///                           during the take to move it later, when
    ///                           another device's IO block grew mid-take. Not
    ///                           lost audio (nothing that arrived was
    ///                           skipped), but a gap in the file all the same
    int inputLatencyFrames = -1;
    int ioBlockFrames = 0;
    int alignmentDelayFrames = 0;
    int alignmentSilenceFrames = 0;
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

    /// The slowest open input's own latency, which every channel is aligned
    /// to (and the headphone figure above includes). Zero where the platform
    /// does not report input latency.
    int alignedInputLatencyFrames = 0;
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
