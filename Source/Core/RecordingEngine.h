#pragma once
#include <string>
#include <vector>
#include <optional>

namespace mma {

enum class RecordingState { Idle, Recording };

struct RecordingChannel
{
    std::string deviceUsbId;
    std::string name;
    bool live = true; // false while the mic is unplugged mid-take (§6.5: write silence, don't shrink file)
};

/// §6.5 mid-recording event orchestration. Channel layout is fixed the instant
/// recording starts and never changes mid-file; a mic that unplugs writes
/// silence into its existing channel slot rather than removing it, and a mic
/// that appears mid-take joins monitoring only, never the in-progress file.
/// This class holds that state-machine logic; the real audio callback plumbing
/// (ring buffers, SessionWriter instances, actual device I/O) is wired in by
/// App/Platform around it.
class RecordingEngine
{
public:
    /// Begins recording with the given fixed channel set (already resolved from
    /// currently-included, currently-live microphones). Returns false if already
    /// recording or the channel list is empty.
    bool start (std::vector<RecordingChannel> channels);

    /// Finalizes the take. No-op if not recording.
    void stop();

    RecordingState getState() const { return state; }
    const std::vector<RecordingChannel>& getChannels() const { return channels; }

    /// §6.5 "Microphone unplugged": marks the channel not-live so its writer
    /// substitutes silence; the channel slot and file layout are untouched.
    /// Returns true if this device was a recording channel (and thus this
    /// affects the in-progress file) as opposed to only a monitoring-only device.
    bool onMicUnplugged (const std::string& deviceUsbId);

    /// §6.5 "Microphone reconnected mid-take": resumes writing live signal to
    /// the existing channel slot. Returns true if it matched an existing
    /// recording channel.
    bool onMicReconnected (const std::string& deviceUsbId);

    /// §6.5 "New microphone plugged in mid-take": never added to the
    /// in-progress recording. Returns the exact one-line message to show,
    /// regardless of recording state (message differs when not recording).
    std::string onNewMicPluggedMidTake (const std::string& deviceUsbId, bool isCurrentlyRecording) const;

    /// True if the given device's channel is currently substituting silence.
    bool isWritingSilence (const std::string& deviceUsbId) const;

private:
    RecordingState state = RecordingState::Idle;
    std::vector<RecordingChannel> channels;

    RecordingChannel* findChannel (const std::string& deviceUsbId);
    const RecordingChannel* findChannel (const std::string& deviceUsbId) const;
};

} // namespace mma
