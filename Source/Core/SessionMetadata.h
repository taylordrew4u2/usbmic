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

    /// How this device's stems were lined up with the rest of the rig. The
    /// headphones hear every microphone as early as its own device allows;
    /// the writer holds each device's stems back to the slowest device's
    /// figure so a clap lands on the same frame in all of them (and once in
    /// the mix), and these say by how much and why -- enough to redo it in an
    /// editor if the take's stemsAligned is false. All in frames at the
    /// take's rate. -1 for the latency means not known: the device did not
    /// open, or this is an older session.json.
    ///   inputLatencyFrames   -- what the driver reported (device latency,
    ///                           safety offset and stream latency on macOS;
    ///                           zero where the platform does not report it)
    ///   ioBlockFrames        -- the largest block the device handed over: its
    ///                           real IO size, which a sample can wait for
    ///                           before it is delivered
    ///   alignmentDelayFrames -- how far this device's audio sits later in its
    ///                           stems than it arrived, so it lines up with the
    ///                           slowest one: the slowest device's latency
    ///                           plus block, less this one's. Silence of this
    ///                           length opens each of its stems.
    ///   alignmentSilenceFrames -- silence written into this device's stems
    ///                           during the take to hold it back further,
    ///                           when another device's IO block grew mid-take.
    ///                           Not lost audio (nothing that arrived was
    ///                           skipped), but a gap in the file all the same
    ///   alignmentDroppedFrames -- samples taken out of this device's stems
    ///                           during the take to bring it forward, when its
    ///                           own IO block grew: mostly the silence its own
    ///                           stream put in as it moved
    ///   ioShiftFrames        -- silence this device's own stream left in its
    ///                           stems during the take: its IO block grew by
    ///                           less than its ring's cushion, so the stream
    ///                           moved later by writing that much silence
    ///                           (no audio lost, so not in dropouts), and the
    ///                           writer could not take it back out because
    ///                           the device was, or became, the slowest. It
    ///                           sits at the same frame as the silence every
    ///                           other stem was given for that growth. A
    ///                           growth past the cushion is a gap counted in
    ///                           dropouts instead
    int inputLatencyFrames = -1;
    int ioBlockFrames = 0;
    int alignmentDelayFrames = 0;
    int alignmentSilenceFrames = 0;
    int alignmentDroppedFrames = 0;
    int ioShiftFrames = 0;
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

    /// The slowest open input's own latency, which the stems are lined up to
    /// (with that device's IO block). The headphone figure above does not
    /// include it: the headphones never wait for alignment. Zero where the
    /// platform does not report input latency.
    int alignedInputLatencyFrames = 0;

    /// Whether the stems and MIX.wav were lined up exactly as each device's
    /// alignmentDelayFrames says. False when the writer could not (an offset
    /// past its bound, a change that landed late) -- or for an older
    /// session.json, whose stems were lined up on the monitor path or not at
    /// all; then each device's offset is what to slide its stems by.
    bool stemsAligned = false;
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
