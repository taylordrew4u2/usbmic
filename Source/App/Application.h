#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <memory>
#include "../Core/DeviceManager.h"
#include "../Core/RecordingEngine.h"
#include "../Core/MonitorBus.h"
#include "../Core/MixBusLimiter.h"
#include "../Core/DriftCompensator.h"
#include "../Core/Metering.h"
#include "../Core/SessionMetadata.h"
#include "../Core/SessionFolderNaming.h"
#include "../Core/AppSettings.h"
#include "../Core/SessionRecovery.h"
#include "../Core/CardRemovalNotice.h"
#include "../Core/PreflightThroughputTest.h"
#include <atomic>
#include <functional>
#include <map>
#include <thread>
#include <mutex>
#include "../Core/PortIdentity.h"
#include "../Core/OutputDeviceSelector.h"
#include "../Core/CapacityMonitor.h"
#include "../Core/BufferLadder.h"
#include "../Core/CpuPressureMonitor.h"
#include "../Core/MirrorPolicy.h"
#include "../Core/SetupAdvisor.h"
#include "../Core/CaptureCoordinator.h"
#include "../Core/TapToNameDetector.h"
#include "CameraController.h"
#include "../Platform/IAudioBackend.h"
#include "../Platform/VirtualDeviceBackend.h"
#include "../Platform/SystemAggregateDevice.h"

namespace mma {

/// §10.1 first-run flow and overall wiring: ties DeviceManager +
/// RecordingEngine + MonitorBus + the platform audio backend + UI together.
/// This class is the composition root; it owns the long-lived engine objects
/// and drives them from OS device-change notifications and UI actions.
class Application
{
public:
    Application();
    ~Application();

    /// Called once at startup: picks the platform backend, enumerates
    /// devices, negotiates rate/depth/master, opens the monitor stream, and
    /// starts monitoring immediately (§5.1: "live from launch").
    void initialise();
    void shutdown();

    DeviceManager& getDeviceManager() { return deviceManager; }
    RecordingEngine& getRecordingEngine() { return recordingEngine; }
    /// Null only on a build with no platform audio backend.
    MonitorBus* getMonitorBus();

    /// §5.4: empty while the low-latency monitor path is healthy; otherwise the
    /// plain-language reason it isn't, which the UI must show rather than
    /// silently delivering a 40 ms mix.
    juce::String getMonitorProblem() const;

    /// §10.4: record button behavior. Starts/stops immediately, no
    /// confirmation either direction.
    void toggleRecording();

    /// §10.2 status the main screen shows by default. All plain language --
    /// no sample rates, buffers or backends leak into these.
    juce::String getDestinationFolder() const { return juce::String (destinationFolder); }
    double getElapsedRecordingSeconds() const;
    /// Remaining recording time at the current channel count and format, from
    /// free space on the destination volume. Negative if it cannot be determined.
    double getRemainingRecordingSeconds() const;
    /// Formats a duration as "2h 14m" / "14m 03s" (§6.4: hours and minutes, never bytes).
    static juce::String formatDuration (double seconds);

    /// §10.4: the record button is disabled only for these two reasons, and the
    /// reason is shown next to it. Empty string means enabled.
    juce::String getRecordDisabledReason() const;

    /// §6.4: benchmarks the destination volume by writing a 200 MB file and
    /// measuring the sustained -- never average -- throughput. Runs on its own
    /// thread; the record button stays disabled until it passes. Cached per
    /// volume for 30 days, so a card is benchmarked once and not on every launch.
    void beginPreflightForDestination();
    bool isPreflightRunning() const { return preflightRunning.load(); }

    /// §5.1 master monitor volume, 0-100. Recorded files are unaffected.
    void setMasterVolume (double volume0to100);
    double getMasterVolume() const;

    int getIncludedMicCount() const;

    /// §6.5: false while this channel's microphone is unplugged mid-take. The
    /// channel stays in the file writing silence; this is what the UI dashes
    /// its skull on. Index is into the included-mic list, as everywhere else.
    bool isMicLive (int index) const;

        /// §14.6: the mic whose tap/voice was just heard alone, or -1. The UI
    /// highlights that skull so a user can see which meter is which person --
    /// four identical USB mics enumerate with the same product string.
    int getTappedChannel() const noexcept { return tappedChannel; }

    /// §14.6 / §2.4: names a microphone. Persisted against the physical port so
    /// the name follows the mic across replug, shown on its skull, and used in
    /// its stem filename (§6.2).
    void setMicAssignedName (int index, const juce::String& name);

    /// §6.2: the user's name for the next take -- "2026-08-27_1030_<name>".
    /// Empty falls back to "Session". Sanitized by SessionFolderNaming.
    void setSessionName (const juce::String& name) { sessionName = name; }
    juce::String getSessionName() const { return sessionName; }

    /// §6.2/§10.1: everything the "where will this go?" question needs
    /// answered, worked out from the settings as they stand and without
    /// creating anything on disk. The pre-record prompt shows this, so a user
    /// knows where to look before there is anything to look for -- §6.2 calls
    /// a novice losing track of their recording a total product failure, and
    /// the cheapest place to prevent it is before the take, not after.
    struct PlannedSave
    {
        juce::String parentFolder;   // "/Users/sam/RECORDINGS"
        juce::String folderName;     // "2026-08-31_1432_Kitchen"
        juce::String fullPath;       // the two joined
        juce::String mirrorFolder;   // §6.3 second copy, empty when none will run
        juce::StringArray fileNames; // "MIX.wav", "01_Alice.wav", ..., "session.json"
    };
    PlannedSave planSave (const juce::String& proposedSessionName) const;

    /// §10.1: whether the user has been shown, and has agreed to, where their
    /// recordings go. False until the pre-record prompt has been answered for
    /// the destination currently set -- changing the destination clears it,
    /// because the answer was about somewhere else.
    bool isSaveLocationConfirmed() const;
    void confirmSaveLocation();
    /// For a user who wants the question every time rather than once a run.
    void setAskWhereToSaveEveryTime (bool ask);
    bool getAskWhereToSaveEveryTime() const { return askWhereToSaveEveryTime; }

    /// §6.2: the take in progress, and its §6.3 second copy. Empty when idle.
    juce::String getCurrentSessionFolder() const { return currentSessionFolder; }
    juce::String getCurrentMirrorFolder() const { return currentMirrorFolder; }

    /// One file in a session folder, as it stands on disk right now.
    struct SavedFile
    {
        juce::String name;
        int64_t sizeBytes = 0;
    };

    /// What is actually in `folder` at this instant, MIX first, then the stems
    /// in channel order, then session.json. Read live during a take so the
    /// files can be watched growing, and again at stop so what is shown is
    /// what was written rather than what was meant to be.
    static std::vector<SavedFile> listSessionFiles (const juce::String& folder);

    /// §6.2: "on stop, show the location and offer to open the containing
    /// folder." Set when a take finishes and consumed once by the UI that
    /// shows it, so the notice is driven by the stop rather than by the UI
    /// having to spot a state edge on a timer.
    struct SavedTake
    {
        juce::String folder;
        juce::String mirrorFolder;
        std::vector<SavedFile> files;
    };
    bool consumeSavedTake (SavedTake& out);

    /// §4 per-microphone trim, -20..+20 dB in 0.5 dB steps. Persisted against
    /// the physical port so it follows the mic across a replug, and applied
    /// live to the monitor mix and the mix file -- never to the stems.
    void setChannelTrimDb (int index, float trimDb);
    float getChannelTrimDb (int index) const;

    /// The combined device other apps see (macOS: a real CoreAudio aggregate).
    /// The name is the user's -- it is what shows up in Zoom's input list.
    void setAggregateDeviceName (const juce::String& name);
    juce::String getAggregateStatus() const;
    juce::String getAggregateDeviceName() const { return aggregateName; }

    /// §10.3 Advanced panel contents.
    double getSampleRate() const { return currentSampleRate; }
    int getBitDepth() const { return currentBitDepth; }
    double getMeasuredLatencyMs() const { return measuredLatencyMs; }
    juce::String getActiveBackendDescription() const;
    /// Display names of every candidate monitor output, and of every mic that
    /// could serve as §3.1 clock master.
    const std::vector<std::string>& getOutputDeviceNames() const;
    juce::String getClockMasterName() const;
    /// §3.2 per-device drift, one line per mic, or a plain line saying the
    /// 60-second measurement window has not elapsed yet (§3.1).
    juce::String getDriftReport() const;
    void setOutputDeviceByName (const juce::String& displayName);
    /// §3.1: an explicit clock-master choice, by display name.
    void setClockMasterByName (const juce::String& displayName);

    /// Every microphone the OS reports, in enumeration order, with whether the
    /// user currently has it selected. Includes deselected ones -- the point of
    /// the list is to let them be turned back on.
    struct MicSelection
    {
        juce::String displayName;
        bool enabled = true;
        bool isBuiltIn = false;
    };
    std::vector<MicSelection> getMicSelections() const;

    /// §10.1: somewhere to save to, chosen by pointing at it rather than by
    /// navigating a file browser to find it. One entry per mounted volume the
    /// app can actually write to, plus the home folder, so an SD card is a
    /// click rather than a path someone has to know.
    struct StorageVolume
    {
        juce::String displayName;   // "UNTITLED (SD card) - 29.8 GB free"
        juce::String path;          // where recordings would go
        bool isRemovable = false;
        bool isCurrent = false;
    };
    std::vector<StorageVolume> getStorageVolumes() const;

    /// Point the destination at one of the volumes above, by its path.
    void setDestinationByPath (const juce::String& path);

    /// Ticking or clearing a microphone in Settings. Rebuilds the audio streams
    /// only when the flag actually changed.
    void setMicEnabledByName (const juce::String& displayName, bool enabled);
    void setDestinationFolder (const juce::File& folder);

    /// §5.3 output selection result for the Advanced panel, and the plain-language
    /// line to show when nothing could be selected.
    const std::string& getSelectedOutputDeviceId() const { return selectedOutputDeviceId; }
    const std::string& getOutputSelectionProblem() const { return outputSelectionProblem; }

    /// §6.5 capacity warnings. Returns a warning the first time each threshold is
    /// crossed, so the caller can poll it every frame without re-warning.
    RemainingTimeWarning pollCapacityWarning();

    /// §5.4: report a callback overrun. Returns true when it pushed the buffer
    /// up a rung, so the caller can tell the user why latency just changed.
    bool noteCallbackOverrun();
    int getCurrentBufferSize() const { return bufferLadder.getCurrentSize(); }

    /// §5.4 requires every buffer step logged in session.json.
    const std::vector<BufferSizeChange>& getBufferSizeChanges() const { return bufferLadder.getChangeLog(); }

    /// §6.6: called with the current load so pressure is warned about before it
    /// causes dropouts rather than after.
    PerformanceWarning updatePerformance (double cpuLoad, bool thermallyThrottled);

    /// §6.3 redundant local mirror. Default on; turns most card failures from
    /// data loss into inconvenience.
    void setMirrorEnabled (bool enabled);
    bool isMirroring() const { return mirrorPolicy.isMirroring(); }
    MirrorState getMirrorState() const { return mirrorPolicy.getState(); }

    /// Fired on the message thread whenever the capture coordinator's meters
    /// have been destroyed and rebuilt (rename, hot-plug, output change, rate
    /// change). The UI must rebind every Metering pointer inside this callback
    /// -- its own timers dereference them on the very next tick.
    std::function<void()> onCaptureRebuilt;

    /// §10.5 physical setup guidance: everything currently worth telling the
    /// user about their hardware, in plain language, most serious first.
    std::vector<SetupAdvice> getSetupAdvice() const;

    /// Runs the §6.5 capacity check, the §6.6 performance check and the §10.5
    /// level-based detectors from the UI tick, and returns the single most
    /// serious thing worth saying right now -- empty when there is nothing.
    /// §10.6: what happened then what to do, never a code.
    juce::String pollStatusAdvice (double sinceLastCallSeconds);

    /// §6.5: "New microphone plugged in mid-take -- do not add to the
    /// in-progress recording. State in one line." That line, for the few
    /// seconds after it happens, or empty. RecordingEngine has always had the
    /// sentence; nothing had ever asked it for one.
    juce::String getMidTakeNotice() const { return midTakeNotice; }

        /// §14.2: report an enumeration failure or a device dropping off the bus,
    /// which is how bus-power exhaustion actually presents.
    void noteDeviceDropout();

    /// §8.1: per-block channel peaks, so silent channels are noticed.
    void updateSetupAdvisorLevels (const std::vector<float>& peaksDb, double blockSeconds);

    /// §8.1: one meter per microphone plus one for the shared mix. These are
    /// owned here so they outlive any UI rebuild when mics come and go.
    Metering* getChannelMetering (int index);
    Metering* getMixMetering();
    juce::String getMicDisplayName (int index) const;

    /// The picture side of a take: which cameras are connected, which are in,
    /// and the live views. Video only -- see CameraController for why there is
    /// no audio anywhere near it.
    CameraController& getCameraController() { return cameraController; }
    const CameraController& getCameraController() const { return cameraController; }

    /// The camera choices go through here rather than straight into the
    /// selection, so that every one of them is remembered for next launch in
    /// the one place that knows how to write them down.
    void setCameraEnabled (const std::string& id, bool enabled);
    void setCameraName (const std::string& id, const juce::String& name);
    void setCameraPreviewQuality (PreviewQuality quality);

    /// Opens the enabled cameras for viewing. Deliberately not done at launch:
    /// a camera light coming on by itself the moment an audio recorder starts
    /// is alarming, and on macOS it spends the privacy prompt before the user
    /// has asked for anything. Called when the camera panel is opened, and
    /// again at arm time so a camera switched on but never looked at still
    /// records.
    void openEnabledCameras();

    /// §6.5 "target card removed": set when a take was stopped because the
    /// destination stopped accepting writes, and consumed once by the UI that
    /// alerts about it. Empty the rest of the time.
    bool consumeCardRemovalNotice (CardRemovalNotice& out);

        /// §6.6: takes the app never got to finish -- a force-quit, a power cut, a
    /// pulled card. Found at launch on the destination volume and in the mirror
    /// folder, with their headers repaired, and presented before the main
    /// screen. Empty when the last run ended cleanly, which is the usual case.
    const std::vector<RecoveredSession>& getRecoveredSessions() const { return recoveredSessions; }
    /// Called once the user has been shown them.
    void clearRecoveredSessions() { recoveredSessions.clear(); }

    /// §11: diagnostics export -- logs, last 5 session.json files, device
    /// inventory. Never audio.
    void exportDiagnostics (const juce::File& destinationZip);

    /// §2.4/§10.1: the rig as the user last left it -- microphone names and
    /// trims, which mics are switched off, where recordings go, the mirror
    /// setting, the cameras. Beside the log, in the same folder.
    static juce::File getSettingsFile();

    /// §11: where the running log lives. Public so Main.cpp can install the
    /// logger before anything else has a chance to fail.
    static juce::File getLogFile();

private:
    std::unique_ptr<IAudioBackend> audioBackend;
    std::unique_ptr<CaptureCoordinator> capture;
    // What the live coordinator was constructed with, so restartCapture() can
    // tell a plain channel-set change from one that needs a new coordinator.
    double captureRate = 0.0;
    int captureBufferSize = 0;
    double measuredLatencyMs = 0.0;
    double driftMeasuredSeconds = 0.0; // §3.1 60-second window

    // Lifetime token for callbacks marshalled from OS threads; see initialise().
    std::shared_ptr<int> aliveToken = std::make_shared<int> (0);
    juce::String currentSessionFolder, currentMirrorFolder, sessionStartIso;
    juce::String sessionName;      // §6.2, set by the user before a take
    juce::String lastSessionFolder;
    juce::String lastMirrorFolder;
    double savedNoticeSeconds = 0.0; // how long "Saved to ..." stays on screen

    // §6.2: a take has finished and the UI has not yet shown where it went.
    bool savedTakePending = false;

    // §6.5's mid-recording row: unplugs and reconnections during a take, kept
    // until the take ends so session.json carries them. RecordingEngine has
    // always tracked this and nothing ever told it anything.
    std::vector<DropoutEntry> midTakeDropouts;
    juce::String midTakeNotice;
    double midTakeNoticeSeconds = 0.0;

        // §6.5: the take was stopped by the card going away rather than by the
    // user, and that has not been said out loud yet.
    bool cardRemovalPending = false;
    CardRemovalNotice cardRemovalNotice;

    // §10.1: the destination the user has actually been shown and accepted.
    // Compared against destinationFolder rather than being a bare flag, so
    // pointing the app at a different card asks again instead of assuming the
    // old answer covered the new place.
    std::string confirmedSaveLocation;
    bool askWhereToSaveEveryTime = false;

    // §14.6 tap-to-name. Rebuilt when the mic count changes, like the
    // fixed-width detectors in SetupAdvisor.
    CameraController cameraController;

    std::unique_ptr<TapToNameDetector> tapDetector;
    int tapDetectorChannels = 0;
    int tappedChannel = -1;

    // §6.4 preflight. Keyed by destination path so switching back to a card
    // already benchmarked does not re-run the test.
    std::atomic<bool> preflightRunning { false };
    std::atomic<bool> preflightAbort { false };
    std::map<std::string, PreflightResult> preflightResults;
    // mutable: getRecordDisabledReason() is const and must read the result.
    mutable std::mutex preflightMutex;
    std::thread preflightThread;
    std::string preflightTargetPath;
    void runPreflight (const std::string& destination, int channelCount);
    std::unique_ptr<VirtualDeviceBackend> virtualDeviceBackend;
    std::unique_ptr<SystemAggregateDevice> systemAggregate;
    juce::String aggregateName { "Multi-Mic Aggregator" };
    // What was last published, so device-change churn republishes only on a
    // real difference -- destroying a device another app is recording from is
    // justified when hardware changed, never as a side effect of a no-op.
    std::vector<std::string> publishedUids;
    std::string publishedMaster, publishedNameStd;
    void publishAggregateDevice();

    DeviceManager deviceManager;
    RecordingEngine recordingEngine;
    PortIdentityStore portIdentityStore;

    std::string selectedOutputDeviceId;
    std::string outputSelectionProblem;
    std::string rememberedOutputDeviceId;
    std::vector<std::string> outputDeviceNames; // §2: refreshed on device change only
    CapacityMonitor capacityMonitor;
    BufferLadder bufferLadder;
    CpuPressureMonitor cpuPressureMonitor;
    int outputConnectionCounter = 0;
    bool haveEnumeratedOutputsOnce = false;

    double recordingStartMs = 0.0;
    double currentSampleRate = 48000.0;
    int currentBitDepth = 24;
    // Buffer size lives in bufferLadder, which is the only thing allowed to
    // change it (§5.4). Keeping a second copy here would let the two disagree.
    std::string destinationFolder;
    MirrorPolicy mirrorPolicy;
    SetupAdvisor setupAdvisor;

    // §5.1 listening level. Owned here, not on the bus, because the coordinator
    // that owns the bus is rebuilt on a rate or buffer change.
    double masterVolume = MonitorBus::kDefaultMonitorVolume;

    /// (Re)opens the streams for the current mic set and output device. §5.1
    /// makes monitoring live from launch, and a hot-plug changes the channel
    /// set, so this runs at startup and on every device-list change.
    void restartCapture();
    /// §3.1/§3.3: pushes DeviceManager's master choice into the coordinator.
    void applyClockMaster();
    std::vector<CaptureChannel> buildCaptureChannels() const;
    /// §6.2 folder name for a take started at `now` under `name`, including the
    /// collision suffix. Resolves against the disk but creates nothing, so the
    /// pre-record prompt and the take itself agree on the answer.
    juce::String resolveSessionFolderName (juce::Time now, const juce::String& name) const;
    /// §6.2 destination folder for a new take, created on disk. Empty on failure.
    juce::String createSessionFolder (juce::Time now) const;
    /// §6.3 local backup folder for a take, created on disk. Empty on failure.
    static juce::String createMirrorFolder (const juce::String& sessionFolderName);
    /// §6.2 session.json, written at start and rewritten at stop.
    void writeSessionMetadata (bool sessionHasStopped);
    /// §11: the newest session.json files under the destination, newest first.
    juce::Array<juce::File> findRecentSessionMetadata (int maximum) const;
    void onDeviceListChanged();
    void chooseInitialDestination();

    /// Read once at launch, written whenever a remembered setting changes and
    /// again at shutdown. Saving is not debounced: the file is a few hundred
    /// bytes in the user's application-data folder, never on the card a take is
    /// being written to, so §14.3's contention worry does not apply to it.
    void loadSettings();
    void saveSettings();

    /// §6.6: walks the destination and the mirror for takes with no stop
    /// timestamp. Launch-only -- it repairs file headers, which must never
    /// happen alongside a writer that has those files open.
    void scanForInterruptedSessions();
    std::vector<RecoveredSession> recoveredSessions;

    /// Pushes the remembered per-port names and trims, and the microphones the
    /// user switched off, onto the device list as it currently stands. Runs
    /// after every enumeration, not just the first: a microphone remembered
    /// from last week is only matchable once it has actually been plugged in.
    void applyRememberedDeviceSettings();

    /// What was read at launch. Kept because devices and cameras arrive later
    /// than the file does, so it has to stay around to be matched against them.
    AppSettings rememberedSettings;
    /// Suppresses saving while the loaded settings are still being applied, so
    /// a half-applied rig cannot be written back over a complete one.
    bool applyingRememberedSettings = false;
    void reselectOutputDevice();
    double bytesPerSecondOfAudio() const;
    /// Audio plus whatever the enabled cameras are estimated to add. This, not
    /// the audio figure, is what the destination volume actually has to hold --
    /// "Room for 8h" with two cameras running would be off by an order of
    /// magnitude, and §6.5's whole point is that a novice cannot act on a
    /// surprise part-way through.
    double bytesPerSecondOfRecording() const;
    int64_t projectedSessionBytes() const;
    std::unique_ptr<IAudioBackend> createPlatformBackend();
    std::unique_ptr<VirtualDeviceBackend> createDefaultVirtualDeviceBackend();
};

} // namespace mma
