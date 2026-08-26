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

    double currentSampleRate = 48000.0;
    int currentBitDepth = 24;
    int currentBufferSize = 64;
    std::string destinationFolder;
    bool mirrorEnabled = true;

    void onDeviceListChanged();
    void chooseInitialDestination();
    std::unique_ptr<IAudioBackend> createPlatformBackend();
    std::unique_ptr<VirtualDeviceBackend> createDefaultVirtualDeviceBackend();
};

} // namespace mma
