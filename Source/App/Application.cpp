#include "Application.h"
#include "../Core/SampleRateNegotiator.h"
#include "../Platform/NullBackend.h"

#if JUCE_MAC
#include "../Platform/CoreAudioBackend.h"
#elif JUCE_WINDOWS
#include "../Platform/WasapiAsioBackend.h"
#endif

namespace mma {

Application::Application() = default;
Application::~Application() { shutdown(); }

std::unique_ptr<IAudioBackend> Application::createPlatformBackend()
{
#if JUCE_MAC
    return std::make_unique<CoreAudioBackend>();
#elif JUCE_WINDOWS
    return std::make_unique<WasapiAsioBackend>();
#else
    return nullptr; // unsupported platform for real audio I/O; Core/ logic still runs
#endif
}

std::unique_ptr<VirtualDeviceBackend> Application::createDefaultVirtualDeviceBackend()
{
    // §7: backend A (none) is always available and ships immediately. B/C/D
    // are progressively richer but each gated on work outside this repo
    // (ASIO COM registration, driver licensing, EV cert + Partner Center).
    return std::make_unique<NullBackend>();
}

void Application::initialise()
{
    audioBackend = createPlatformBackend();
    virtualDeviceBackend = createDefaultVirtualDeviceBackend();

    monitorBus = std::make_unique<MonitorBus> (currentSampleRate);

    if (audioBackend != nullptr)
    {
        audioBackend->setDeviceChangeCallback ([this] { onDeviceListChanged(); });
        onDeviceListChanged(); // initial enumeration, per §2 "at launch"
    }

    chooseInitialDestination();

    // §5.1: monitoring is live from launch, independent of record state --
    // there is deliberately no "arm monitoring" step anywhere in this flow.
}

void Application::onDeviceListChanged()
{
    if (audioBackend == nullptr)
        return;

    auto inputDevices = audioBackend->enumerateInputDevices();

    std::vector<DeviceRateCapability> rateCapabilities;
    int order = 0;
    for (const auto& d : inputDevices)
    {
        DeviceRateCapability cap;
        cap.deviceIndex = order;
        cap.supportedRates = d.supportedSampleRates;
        rateCapabilities.push_back (cap);

        MicDeviceState state;
        state.identity.locationId = d.usbLocationId;
        state.displayName = d.name;
        state.enumerationOrder = order;
        deviceManager.addDevice (state);
        ++order;
    }

    // §2.2: highest common rate, capped at 48kHz. Never rejects a device.
    auto rateResult = SampleRateNegotiator::negotiate (rateCapabilities);
    currentSampleRate = rateResult.chosenRate;
}

void Application::chooseInitialDestination()
{
    // §10.1: destination defaults to a connected external card; falls back to
    // ~/RECORDINGS, stated in one line. Real removable-volume enumeration is
    // platform-specific (DiskArbitration on macOS, WM_DEVICECHANGE volume
    // notifications on Windows) and lives in the backend; here we fall
    // through to the always-available default so the app never blocks.
    destinationFolder = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                            .getChildFile ("RECORDINGS")
                            .getFullPathName()
                            .toStdString();
}

void Application::toggleRecording()
{
    if (recordingEngine.getState() == RecordingState::Idle)
    {
        std::vector<RecordingChannel> channels;
        for (const auto& d : deviceManager.getDevices())
        {
            if (! d.included)
                continue;
            RecordingChannel c;
            c.deviceUsbId = d.identity.key();
            c.name = d.displayName;
            channels.push_back (c);
        }
        recordingEngine.start (std::move (channels));
    }
    else
    {
        recordingEngine.stop();
    }
}

void Application::exportDiagnostics (const juce::File& destinationZip)
{
    // §11: logs + last 5 session.json + device inventory. NEVER audio.
    juce::ZipFile::Builder builder;
    // TODO: add real log file(s) and the last 5 session.json paths once the
    // App layer tracks recent session folders; device inventory below is
    // real and complete.

    juce::var deviceInventory;
    juce::Array<juce::var> deviceArray;
    for (const auto& d : deviceManager.getDevices())
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("name", juce::String (d.displayName));
        obj->setProperty ("usbId", juce::String (d.identity.key()));
        obj->setProperty ("included", d.included);
        deviceArray.add (juce::var (obj));
    }
    deviceInventory = deviceArray;

    juce::TemporaryFile tempInventory;
    tempInventory.getFile().replaceWithText (juce::JSON::toString (deviceInventory));
    builder.addFile (tempInventory.getFile(), 9, "device_inventory.json");

    juce::FileOutputStream out (destinationZip);
    if (out.openedOk())
        builder.writeToStream (out, nullptr);
}

void Application::shutdown()
{
    if (recordingEngine.getState() == RecordingState::Recording)
        recordingEngine.stop();
    if (audioBackend != nullptr)
        audioBackend->closeAllStreams();
}

} // namespace mma
