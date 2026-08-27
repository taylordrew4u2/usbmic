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
#include "../Core/PreflightThroughputTest.h"
#include "../Core/PortIdentity.h"
#include "../Core/OutputDeviceSelector.h"
#include "../Core/CapacityMonitor.h"
#include "../Core/BufferLadder.h"
#include "../Core/CpuPressureMonitor.h"
#include "../Core/MirrorPolicy.h"
#include "../Platform/IAudioBackend.h"
#include "../Platform/VirtualDeviceBackend.h"

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
    MonitorBus* getMonitorBus() { return monitorBus.get(); }

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

    /// §5.1 master monitor volume, 0-100. Recorded files are unaffected.
    void setMasterVolume (double volume0to100);
    double getMasterVolume() const;

    int getIncludedMicCount() const;

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
    void setMirrorEnabled (bool enabled) { mirrorPolicy.setEnabledByUser (enabled); }
    bool isMirroring() const { return mirrorPolicy.isMirroring(); }
    MirrorState getMirrorState() const { return mirrorPolicy.getState(); }

    /// §8.1: one meter per microphone plus one for the shared mix. These are
    /// owned here so they outlive any UI rebuild when mics come and go.
    Metering* getChannelMetering (int index);
    Metering* getMixMetering() { return mixMeter.get(); }
    juce::String getMicDisplayName (int index) const;

    /// §11: diagnostics export -- logs, last 5 session.json files, device
    /// inventory. Never audio.
    void exportDiagnostics (const juce::File& destinationZip);

private:
    std::unique_ptr<IAudioBackend> audioBackend;
    std::unique_ptr<VirtualDeviceBackend> virtualDeviceBackend;

    DeviceManager deviceManager;
    RecordingEngine recordingEngine;
    std::unique_ptr<MonitorBus> monitorBus;
    PortIdentityStore portIdentityStore;

    std::string selectedOutputDeviceId;
    std::string outputSelectionProblem;
    std::string rememberedOutputDeviceId;
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

    std::vector<std::unique_ptr<Metering>> channelMeters;
    std::unique_ptr<Metering> mixMeter;

    void rebuildMeters();
    void onDeviceListChanged();
    void chooseInitialDestination();
    void reselectOutputDevice();
    double bytesPerSecondOfAudio() const;
    int64_t projectedSessionBytes() const;
    std::unique_ptr<IAudioBackend> createPlatformBackend();
    std::unique_ptr<VirtualDeviceBackend> createDefaultVirtualDeviceBackend();
};

} // namespace mma
