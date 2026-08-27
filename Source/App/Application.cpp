#include "Application.h"
#include "../Core/SampleRateNegotiator.h"
#include "../Platform/NullBackend.h"
#include <cstdio>
#include <set>

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
    mixMeter = std::make_unique<Metering> (currentSampleRate);
    rebuildMeters();

    if (audioBackend != nullptr)
    {
        capture = std::make_unique<CaptureCoordinator> (*audioBackend, currentSampleRate,
                                                        bufferLadder.getCurrentSize());
        captureRate = currentSampleRate;
        captureBufferSize = bufferLadder.getCurrentSize();

        audioBackend->setDeviceChangeCallback ([this] { onDeviceListChanged(); });
        onDeviceListChanged(); // initial enumeration, per §2 "at launch"
    }

    chooseInitialDestination();

    // §5.1: monitoring is live from launch, independent of record state --
    // there is deliberately no "arm monitoring" step anywhere in this flow.
    restartCapture();
}

std::vector<CaptureChannel> Application::buildCaptureChannels() const
{
    std::vector<CaptureChannel> channels;
    int index = 0;

    for (const auto& d : deviceManager.getDevices())
    {
        if (! d.included)
            continue;

        ++index;

        // §4: trim and the assigned name are persisted against the physical
        // port, so they follow the mic across replug rather than across slot.
        const auto persisted = portIdentityStore.get (d.identity);

        CaptureChannel c;
        c.deviceId = d.identity.key();
        c.displayName = (persisted.has_value() && ! persisted->assignedName.empty())
                            ? persisted->assignedName : d.displayName;

        // §6.2: "01_Yeti-Kitchen" -- ordinal prefix plus the sanitized name, so
        // the stems sort in channel order in any file browser.
        char prefix[4] = {};
        std::snprintf (prefix, sizeof (prefix), "%02d", index);
        c.fileName = std::string (prefix) + "_" + SessionFolderNaming::sanitizeName (c.displayName);
        c.trimDb = persisted.has_value() ? persisted->trimDb : 0.0f;

        channels.push_back (std::move (c));
    }

    return channels;
}

void Application::restartCapture()
{
    if (capture == nullptr)
        return;

    // §5.4 fixes the buffer size for the duration of a take, and reopening the
    // streams would tear down the writer mid-file. A device change during a
    // recording is handled by §6.5 instead: the channel stays and goes silent.
    if (capture->isRecording())
        return;

    // §2.2 can settle on a different rate once the mics are enumerated, and
    // §5.4 can move the buffer up a rung. Both are fixed at construction, so a
    // change means a new coordinator -- carrying the listening level across,
    // since the user did not ask for it to jump.
    if (audioBackend != nullptr
        && (captureRate != currentSampleRate || captureBufferSize != bufferLadder.getCurrentSize()))
    {
        const auto volume = capture->getMonitorBus().getMasterVolume();

        capture->stopMonitoring();
        capture = std::make_unique<CaptureCoordinator> (*audioBackend, currentSampleRate,
                                                        bufferLadder.getCurrentSize());
        capture->getMonitorBus().setMasterVolume (volume);

        captureRate = currentSampleRate;
        captureBufferSize = bufferLadder.getCurrentSize();
    }

    auto channels = buildCaptureChannels();

    if (channels.empty())
    {
        capture->stopMonitoring();
        return;
    }

    capture->startMonitoring (channels, selectedOutputDeviceId);
}

MonitorBus* Application::getMonitorBus()
{
    // The coordinator owns the bus the audio callback actually runs, so the UI
    // must reach that one. monitorBus is the fallback for a build with no
    // platform backend, where there is no callback to own it.
    return capture != nullptr ? &capture->getMonitorBus() : monitorBus.get();
}

juce::String Application::getMonitorProblem() const
{
    if (capture == nullptr)
        return {};

    return juce::String (capture->getMonitorProblem());
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

    rebuildMeters();

    // A device change can add or remove an output too, so §5.3 is re-run here
    // rather than only at launch (§6.5: output device disappears -> re-select).
    reselectOutputDevice();

    // §10.5 guidance follows the device list: names so advice can say "Kitchen"
    // rather than "channel 2", and topology so §14.3 contention is re-judged
    // when the card reader moves.
    std::vector<std::string> names;
    std::vector<ControllerContentionDetector::DeviceControllerInfo> topology;

    for (const auto& d : deviceManager.getDevices())
    {
        if (! d.included)
            continue;

        names.push_back (d.displayName);

        ControllerContentionDetector::DeviceControllerInfo info;
        info.deviceId = d.identity.key();
        // controllerId is left empty: no backend reports USB host-controller
        // topology yet, and §14.3 only judges co-location "where the OS exposes
        // controller topology". The detector treats unknown as unjudgeable and
        // stays silent, which is right -- guessing would warn people whose card
        // reader is fine.
        info.isMicrophone = true;
        topology.push_back (std::move (info));
    }

    setupAdvisor.setChannelNames (std::move (names));
    setupAdvisor.updateControllerTopology (topology);

    if (capture != nullptr && capture->isRecording())
    {
        // §6.5: mid-take, a mic that has gone away keeps its channel and writes
        // silence. Dropping or renumbering the channel would corrupt the take,
        // so the take's channel list is fixed and only its liveness moves.
        std::set<std::string> present;
        for (const auto& d : deviceManager.getDevices())
            if (d.included)
                present.insert (d.identity.key());

        for (const auto& ch : capture->getChannels())
            capture->setChannelLive (ch.deviceId, present.count (ch.deviceId) > 0);

        return;
    }

    restartCapture();
}

void Application::reselectOutputDevice()
{
    if (audioBackend == nullptr)
        return;

    std::vector<OutputDeviceCandidate> candidates;

    for (const auto& d : audioBackend->enumerateOutputDevices())
    {
        OutputDeviceCandidate c;
        c.id = d.usbLocationId.empty() ? d.name : d.usbLocationId;
        c.displayName = d.name;
        c.hasPhysicalHeadphoneJack = d.hasPhysicalHeadphoneJack;

        // §5.2: a microphone's own playback endpoint is never a monitor output.
        c.isMicrophonePlaybackEndpoint = d.isMicrophone;

        // §5.5: refuse to route output to a device that is also a capture device.
        for (const auto& mic : deviceManager.getDevices())
            if (mic.included && ! d.usbLocationId.empty() && mic.identity.locationId == d.usbLocationId)
                c.isAlsoSelectedInput = true;

        // Anything seen after the first enumeration is something the user just
        // plugged in (§5.3 priority 2).
        c.appearedAfterLaunch = haveEnumeratedOutputsOnce;
        c.connectionOrder = ++outputConnectionCounter;

        candidates.push_back (std::move (c));
    }

    haveEnumeratedOutputsOnce = true;

    const auto selection = OutputDeviceSelector::select (candidates, rememberedOutputDeviceId);
    selectedOutputDeviceId = selection.id;
    outputSelectionProblem = selection.explanation;
}

bool Application::noteCallbackOverrun()
{
    // §5.4 requires every step logged. The ladder keeps that log itself and
    // getBufferSizeChanges() hands it to whoever writes session.json, so the
    // history is not duplicated into a second place that could disagree.
    return bufferLadder.noteOverrun (juce::Time::getMillisecondCounterHiRes() / 1000.0);
}

PerformanceWarning Application::updatePerformance (double cpuLoad, bool thermallyThrottled)
{
    return cpuPressureMonitor.update (cpuLoad, thermallyThrottled,
                                      juce::Time::getMillisecondCounterHiRes() / 1000.0);
}

std::vector<SetupAdvice> Application::getSetupAdvice() const
{
    return setupAdvisor.getActiveAdvice (juce::Time::getMillisecondCounterHiRes() / 1000.0);
}

void Application::noteDeviceDropout()
{
    setupAdvisor.noteDeviceDropout (juce::Time::getMillisecondCounterHiRes() / 1000.0,
                                    getIncludedMicCount());
}

void Application::updateSetupAdvisorLevels (const std::vector<float>& peaksDb, double blockSeconds)
{
    setupAdvisor.updateChannelLevels (peaksDb, blockSeconds);
}

RemainingTimeWarning Application::pollCapacityWarning()
{
    if (recordingEngine.getState() != RecordingState::Recording)
        return RemainingTimeWarning::None;

    return capacityMonitor.evaluateRemaining (getRemainingRecordingSeconds());
}

void Application::rebuildMeters()
{
    const auto needed = static_cast<size_t> (getIncludedMicCount());

    // Keep existing meters so ballistics and clip latches survive a hot-plug.
    while (channelMeters.size() > needed)
        channelMeters.pop_back();

    while (channelMeters.size() < needed)
        channelMeters.push_back (std::make_unique<Metering> (currentSampleRate));
}

Metering* Application::getChannelMetering (int index)
{
    // Same reasoning as getMonitorBus(): the meters the audio thread feeds live
    // in the coordinator, so the UI has to read those and not a second set.
    if (capture != nullptr)
        return capture->getChannelMetering (index);

    if (index < 0 || static_cast<size_t> (index) >= channelMeters.size())
        return nullptr;

    return channelMeters[static_cast<size_t> (index)].get();
}

Metering* Application::getMixMetering()
{
    return capture != nullptr ? &capture->getMixMetering() : mixMeter.get();
}

juce::String Application::getMicDisplayName (int index) const
{
    int seen = 0;
    for (const auto& d : deviceManager.getDevices())
    {
        if (! d.included)
            continue;

        if (seen == index)
            return juce::String (d.displayName);

        ++seen;
    }

    return {};
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
        if (recordingEngine.start (std::move (channels)))
        {
            recordingStartMs = juce::Time::getMillisecondCounterHiRes();

            // §6: this is what actually opens the stem files and starts the
            // writer thread. Without it the record button only changes state.
            if (capture != nullptr)
            {
                const auto now = juce::Time::getCurrentTime();
                const auto folder = createSessionFolder (now);

                if (folder.isEmpty()
                    || ! capture->startRecording (folder.toStdString(), currentBitDepth,
                                                  now.toISO8601 (true).toStdString()))
                {
                    // Nothing was written, so the engine must not claim a take.
                    recordingEngine.stop();
                    recordingStartMs = 0.0;
                    return;
                }
            }

            // Each take gets its own warnings; a previous one must not leave the
            // ten-minute warning already spent.
            capacityMonitor.reset();
            mirrorPolicy.reset();

            // §6.3: the mirror only starts when the internal drive has room for
            // the whole projected session plus headroom.
            const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
            mirrorPolicy.evaluateAtArm (home.getBytesFreeOnVolume(), projectedSessionBytes());

            // §5.4: buffer size is fixed for the duration of a take.
            bufferLadder.setRecording (true);
        }
    }
    else
    {
        // §6.1: stop the writer first so every buffered frame reaches the files
        // before the engine reports the take finished.
        if (capture != nullptr)
            capture->stopRecording();

        recordingEngine.stop();
        recordingStartMs = 0.0;
        bufferLadder.setRecording (false);
    }
}

juce::String Application::createSessionFolder (juce::Time now) const
{
    const juce::File root (destinationFolder);

    // §6.2: never overwrite, never prompt -- collisions get _2, _3, ...
    const auto desired = SessionFolderNaming::buildFolderName (now.getYear(), now.getMonth() + 1,
                                                               now.getDayOfMonth(), now.getHours(),
                                                               now.getMinutes(),
                                                               SessionFolderNaming::kDefaultName);

    const auto resolved = SessionFolderNaming::resolveCollision (desired,
        [&root] (const std::string& candidate)
        {
            return root.getChildFile (juce::String (candidate)).exists();
        });

    const auto folder = root.getChildFile (juce::String (resolved));

    if (! folder.createDirectory().wasOk())
        return {};

    return folder.getFullPathName();
}

double Application::getElapsedRecordingSeconds() const
{
    if (recordingEngine.getState() != RecordingState::Recording || recordingStartMs <= 0.0)
        return 0.0;

    return (juce::Time::getMillisecondCounterHiRes() - recordingStartMs) / 1000.0;
}

int Application::getIncludedMicCount() const
{
    int count = 0;
    for (const auto& d : deviceManager.getDevices())
        if (d.included)
            ++count;

    return count;
}

double Application::bytesPerSecondOfAudio() const
{
    const int channels = std::max (1, getIncludedMicCount());
    const int bytesPerSample = std::max (1, currentBitDepth / 8);

    // Stems plus the mix file, matching the pre-flight required-rate figure (§6.4).
    return static_cast<double> (channels) * currentSampleRate
         * static_cast<double> (bytesPerSample) * 2.0;
}

int64_t Application::projectedSessionBytes() const
{
    // §6.3 needs a size to reserve for the mirror before the take starts, and
    // nothing knows how long the user will record. An hour is the planning
    // figure: long enough that a typical session fits, short enough that the
    // mirror is not refused on a drive that could hold it.
    constexpr double kProjectedSessionSeconds = 60.0 * 60.0;
    return static_cast<int64_t> (bytesPerSecondOfAudio() * kProjectedSessionSeconds);
}

double Application::getRemainingRecordingSeconds() const
{
    const double bytesPerSecond = bytesPerSecondOfAudio();

    if (bytesPerSecond <= 0.0)
        return -1.0;

    juce::File destination (destinationFolder);
    auto probe = destination;
    while (! probe.exists() && probe.getParentDirectory() != probe)
        probe = probe.getParentDirectory();

    const auto freeBytes = probe.getBytesFreeOnVolume();
    if (freeBytes <= 0)
        return -1.0;

    return static_cast<double> (freeBytes) / bytesPerSecond;
}

juce::String Application::formatDuration (double seconds)
{
    if (seconds < 0.0)
        return "--";

    const auto total = static_cast<int64_t> (seconds);
    const auto hours = total / 3600;
    const auto minutes = (total % 3600) / 60;
    const auto secs = total % 60;

    if (hours > 0)
        return juce::String (hours) + "h " + juce::String (minutes).paddedLeft ('0', 2) + "m";

    return juce::String (minutes) + "m " + juce::String (secs).paddedLeft ('0', 2) + "s";
}

juce::String Application::getRecordDisabledReason() const
{
    if (getIncludedMicCount() == 0)
        return "Plug in a microphone first.";

    // §6.4: pre-flight blocks arming rather than degrading mid-take. Until a
    // volume has been benchmarked, nothing else disables the button.
    return {};
}

void Application::setMasterVolume (double volume0to100)
{
    if (monitorBus != nullptr)
        monitorBus->setMasterVolume (volume0to100);
}

double Application::getMasterVolume() const
{
    return monitorBus != nullptr ? monitorBus->getMasterVolume()
                                 : MonitorBus::kDefaultMonitorVolume;
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
    // Ordered: drain the writer, then close the streams the callback runs on.
    if (capture != nullptr)
    {
        capture->stopRecording();
        capture->stopMonitoring();
    }

    if (recordingEngine.getState() == RecordingState::Recording)
        recordingEngine.stop();
    if (audioBackend != nullptr)
        audioBackend->closeAllStreams();
}

} // namespace mma
