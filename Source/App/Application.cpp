#include "Application.h"
#include "../Core/TakeCompleteness.h"
#include "../Platform/ReducedMotion.h"
#include "../Core/ClockMasterResolver.h"
#include "../Core/SampleRateNegotiator.h"
#include "../Platform/NullBackend.h"
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <set>

#if JUCE_MAC
#include "../Platform/CoreAudioBackend.h"
#elif JUCE_WINDOWS
#include "../Platform/WasapiAsioBackend.h"
#elif defined(__linux__) && ! defined(MMA_NO_ALSA)
#include "../Platform/AlsaBackend.h"
#endif

namespace mma {

Application::Application()
    // §9.3, asked once. The setting does not change between meter repaints, and
    // the alternative -- querying the OS per strip at 60Hz -- would be absurd.
    : reducedMotionPreferred (prefersReducedMotionOnThisSystem())
{
}
Application::~Application() { shutdown(); }

std::unique_ptr<IAudioBackend> Application::createPlatformBackend()
{
#if JUCE_MAC
    return std::make_unique<CoreAudioBackend>();
#elif JUCE_WINDOWS
    return std::make_unique<WasapiAsioBackend>();
#elif defined(__linux__) && ! defined(MMA_NO_ALSA)
    return std::make_unique<AlsaBackend>();
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
    // First, before anything reads a setting: the capture coordinator is built
    // with masterVolume a few lines down, and the destination is chosen below.
    loadSettings();

    audioBackend = createPlatformBackend();
    virtualDeviceBackend = createDefaultVirtualDeviceBackend();
    systemAggregate = createSystemAggregateDevice();

    if (audioBackend != nullptr)
    {
        capture = std::make_unique<CaptureCoordinator> (*audioBackend, currentSampleRate,
                                                        bufferLadder.getCurrentSize());
        capture->getMonitorBus().setMasterVolume (masterVolume);
        captureRate = currentSampleRate;
        captureBufferSize = bufferLadder.getCurrentSize();

        // OS device-change notifications arrive on the backend's own thread --
        // a CoreAudio listener thread on macOS, the COM notification thread on
        // Windows. Everything onDeviceListChanged touches (DeviceManager, the
        // meters, the coordinator) belongs to the message thread, so the
        // callback only queues the work. The token keeps a callback that is
        // already queued at quit time from firing into a destroyed Application.
        std::weak_ptr<int> alive = aliveToken;

        audioBackend->setDeviceChangeCallback ([this, alive]
        {
            juce::MessageManager::callAsync ([this, alive]
            {
                if (alive.lock() != nullptr)
                    onDeviceListChanged();
            });
        });
        onDeviceListChanged(); // initial enumeration, per §2 "at launch"
    }

    // §10.1's default, but only when there is nothing remembered to override
    // it -- otherwise the card the user chose last time is silently replaced by
    // the home folder on every launch.
    if (destinationFolder.empty())
        chooseInitialDestination();

    // §6.4: benchmark before the user reaches for record, not at record time.
    beginPreflightForDestination();

    // §6.6: before the window opens, and only ever here. It rewrites file
    // headers, which must never happen while a writer has those files open --
    // at launch nothing does.
    scanForInterruptedSessions();

    // §5.1: monitoring is live from launch, independent of record state --
    // there is deliberately no "arm monitoring" step anywhere in this flow.
    restartCapture();

    // Enumeration only. Nothing is opened, so no camera light comes on and no
    // privacy prompt is spent, until the user opens the camera panel or arms a
    // take with a camera switched on.
    cameraController.refreshCameras();

    // A camera the user turned off last time stays off, and one they named
    // keeps its name -- applied after enumeration, since only then is there
    // anything to match the remembered answers against.
    applyingRememberedSettings = true;

    for (const auto& camera : cameraController.getSelection().getAvailableCameras())
        if (const auto* remembered = rememberedSettings.findCamera (camera.id))
        {
            cameraController.getSelection().setEnabled (camera.id, remembered->enabled);

            if (! remembered->assignedName.empty())
                cameraController.getSelection().setAssignedName (camera.id, remembered->assignedName);
        }

    applyingRememberedSettings = false;
}

void Application::setCameraEnabled (const std::string& id, bool enabled)
{
    cameraController.getSelection().setEnabled (id, enabled);
    saveSettings();
}

void Application::setCameraName (const std::string& id, const juce::String& name)
{
    // §6.2 sanitizing at the point of entry, so what is remembered is what will
    // appear in the filename rather than something that still has to be cleaned.
    cameraController.getSelection().setAssignedName (id, SessionFolderNaming::sanitizeName (name.toStdString()));
    saveSettings();
}

void Application::setCameraPreviewQuality (PreviewQuality quality)
{
    cameraController.setPreviewQuality (quality);
    saveSettings();
}

void Application::confirmSaveLocation()
{
    confirmedSaveLocation = destinationFolder;
    saveSettings();
}

void Application::setAskWhereToSaveEveryTime (bool ask)
{
    askWhereToSaveEveryTime = ask;
    saveSettings();
}

void Application::setMirrorEnabled (bool enabled)
{
    mirrorPolicy.setEnabledByUser (enabled);
    saveSettings();
}

void Application::openEnabledCameras()
{
    cameraController.applySelection();
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
        capture->stopMonitoring();
        capture = std::make_unique<CaptureCoordinator> (*audioBackend, currentSampleRate,
                                                        bufferLadder.getCurrentSize());
        capture->getMonitorBus().setMasterVolume (masterVolume);

        captureRate = currentSampleRate;
        captureBufferSize = bufferLadder.getCurrentSize();
    }

    auto channels = buildCaptureChannels();

    if (channels.empty())
    {
        capture->stopMonitoring();

        if (onCaptureRebuilt)
            onCaptureRebuilt();

        return;
    }

    const bool started = capture->startMonitoring (channels, selectedOutputDeviceId);

    if (started)
    {
        applyClockMaster();
        driftMeasuredSeconds = 0.0;
    }

    // Fired on success AND failure: either way the old meters are gone, and a
    // UI still holding pointers into them would read freed memory on its very
    // next timer tick. This runs on the message thread, in the same call
    // stack as the rebuild, so no timer can interleave.
    if (onCaptureRebuilt)
        onCaptureRebuilt();
}

void Application::publishAggregateDevice()
{
    if (systemAggregate == nullptr)
        return;

    std::vector<std::string> uids;
    for (const auto& d : deviceManager.getDevices())
        if (d.included)
            uids.push_back (d.identity.locationId); // the CoreAudio device UID on macOS

    // §3.1: same clock master as the in-app capture path, so the aggregate and
    // the app agree about whose crystal is the truth.
    std::string master;
    if (const auto* m = deviceManager.selectDefaultMaster())
        master = m->identity.locationId;

    const auto name = aggregateName.toStdString();

    // Republishing destroys the device other apps may be recording from, so it
    // happens only when something real changed.
    if (uids == publishedUids && master == publishedMaster && name == publishedNameStd)
        return;

    systemAggregate->publish (name, uids, master);
    publishedUids = std::move (uids);
    publishedMaster = std::move (master);
    publishedNameStd = name;
}

void Application::setAggregateDeviceName (const juce::String& name)
{
    const auto trimmed = name.trim();
    aggregateName = trimmed.isEmpty() ? juce::String ("Multi-Mic Aggregator") : trimmed;
    publishAggregateDevice();
    saveSettings();
}

juce::String Application::getAggregateStatus() const
{
    return systemAggregate != nullptr ? juce::String (systemAggregate->getStatus()) : juce::String();
}

void Application::applyClockMaster()
{
    if (capture == nullptr)
        return;

    // §3.1 / §3.3: DeviceManager owns *which microphone* should be the timebase
    // -- lowest measured drift, the user's override, or the next best after the
    // master leaves. What it cannot own is which channel that is, because it
    // tracks the devices the OS reports right now and a take's channel list is
    // frozen (§6.5).
    //
    // This used to count included devices to find the index, which is the same
    // number only until a microphone is unplugged mid-take. After that the
    // device list is short one entry and every index past the gap is off by
    // one, so setMasterChannel() named the wrong channel -- and the channel it
    // named could be the unplugged one, writing silence, with every other
    // microphone resampled onto it.
    std::vector<std::string> rankedIds;
    for (const auto* d : deviceManager.rankMasterCandidates())
        rankedIds.push_back (d->identity.key());

    // capture's own channel list, since that is exactly what setMasterChannel
    // indexes into. Outside a take it tracks the device list; during one it is
    // the frozen list, which is the point.
    const bool recording = capture->isRecording();
    std::vector<std::string> channelIds;
    std::vector<bool> channelLive;

    for (const auto& ch : capture->getChannels())
    {
        channelIds.push_back (ch.deviceId);
        channelLive.push_back (! recording || ! recordingEngine.isWritingSilence (ch.deviceId));
    }

    const auto resolved = resolveMasterChannel (channelIds, channelLive, rankedIds);
    capture->setMasterChannel (resolved.channelIndex);

    // §3.3: "Log the switchover timestamp in session.json." Only a change is
    // worth a line -- this runs on every status poll, and re-confirming the
    // same master is not an event.
    if (recording && resolved.deviceId != appliedMasterDeviceId)
    {
        if (! appliedMasterDeviceId.empty())
            midTakeDropouts.push_back ({ getElapsedRecordingSeconds(), resolved.deviceId,
                                         resolved.deviceId.empty()
                                             ? std::string ("Clock master lost: no live microphone can hold "
                                                            "the timebase, so the channels are free-running.")
                                             : std::string ("Clock master switched to this microphone after "
                                                            "the previous one stopped delivering audio.") });

        appliedMasterDeviceId = resolved.deviceId;
    }
    else if (! recording)
    {
        appliedMasterDeviceId = resolved.deviceId;
    }
}

MonitorBus* Application::getMonitorBus()
{
    // The coordinator owns the bus the audio callback actually runs, so this is
    // the only bus there is. A second one kept here as a "fallback" is what let
    // the volume slider write to a bus nothing was listening to.
    return capture != nullptr ? &capture->getMonitorBus() : nullptr;
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
    std::vector<MicDeviceState> seen;
    seen.reserve (inputDevices.size());

    int order = 0;
    for (const auto& d : inputDevices)
    {
        DeviceRateCapability cap;
        cap.deviceIndex = order;
        cap.supportedRates = d.supportedSampleRates;
        rateCapabilities.push_back (cap);

        MicDeviceState state;
        state.identity.locationId = d.usbLocationId;
        if (! d.serialNumber.empty())
            state.identity.serial = d.serialNumber;
        state.displayName = d.name;
        state.isBuiltIn = d.isBuiltIn;
        seen.push_back (std::move (state));
        ++order;
    }

    // Reconcile against what the OS reports rather than appending each device
    // again. macOS fires its device-list listener several times while a USB
    // microphone initialises, and this handler previously called addDevice()
    // per device per firing -- which is why one Yeti showed up five times.
    deviceManager.syncToEnumeration (seen);

    // §2.4: a microphone remembered from last week is only matchable once it is
    // actually plugged in, so this runs after every enumeration rather than
    // once at launch.
    applyRememberedDeviceSettings();

    // §2.2: highest common rate, capped at 48kHz. Never rejects a device.
    auto rateResult = SampleRateNegotiator::negotiate (rateCapabilities);
    currentSampleRate = rateResult.chosenRate;

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

    // The advisor is fed peaks per *channel* (see pollStatusAdvice), so its
    // names have to be in that same space or every piece of §10.5 advice ends
    // up addressed to the wrong person. Built from the take's channel list
    // while one is running -- taking them from the device list meant that
    // unplugging a microphone mid-take renamed everyone after it.
    for (int i = 0, n = getIncludedMicCount(); i < n; ++i)
        names.push_back (getMicDisplayName (i).toStdString());

    setupAdvisor.setChannelNames (std::move (names));
    setupAdvisor.updateControllerTopology (topology);

    // The combined device other apps see tracks the rig -- §2: on the OS
    // notification, never a timer. Safe during a take: our own capture reads
    // the per-device streams, not the aggregate.
    publishAggregateDevice();

    if (capture != nullptr && capture->isRecording())
    {
        // §6.5: mid-take, a mic that has gone away keeps its channel and writes
        // silence. Dropping or renumbering the channel would corrupt the take,
        // so the take's channel list is fixed and only its liveness moves.
        std::set<std::string> present;
        for (const auto& d : deviceManager.getDevices())
            if (d.included)
                present.insert (d.identity.key());

        std::set<std::string> takeChannels;
        for (const auto& ch : capture->getChannels())
            takeChannels.insert (ch.deviceId);

        for (const auto& ch : capture->getChannels())
        {
            const bool live = present.count (ch.deviceId) > 0;
            capture->setChannelLive (ch.deviceId, live);

            // §6.5: "Log the dropout" on an unplug, and log the reconnection
            // too. RecordingEngine has always tracked both and nothing had ever
            // told it anything, so a mic could fall out of a four-hour take and
            // leave no trace anywhere.
            if (! live && ! recordingEngine.isWritingSilence (ch.deviceId))
            {
                recordingEngine.onMicUnplugged (ch.deviceId);
                midTakeDropouts.push_back ({ getElapsedRecordingSeconds(), ch.deviceId,
                                             "Microphone unplugged: writing silence to its channel." });
            }
            else if (live && recordingEngine.isWritingSilence (ch.deviceId))
            {
                recordingEngine.onMicReconnected (ch.deviceId);
                midTakeDropouts.push_back ({ getElapsedRecordingSeconds(), ch.deviceId,
                                             "Microphone reconnected: its channel is live again." });
            }
        }

        // §6.5: a microphone plugged in during a take joins nothing -- the
        // channel list is fixed for the duration, and renumbering it would
        // corrupt the take. What the spec asks for is that the user be told,
        // in one line, rather than left believing it is being recorded.
        for (const auto& d : deviceManager.getDevices())
        {
            if (! d.included || takeChannels.count (d.identity.key()) > 0)
                continue;

            midTakeNotice = juce::String (recordingEngine.onNewMicPluggedMidTake (d.identity.key(), true));
            midTakeNoticeSeconds = 12.0;
            break;
        }

        return;
    }

    restartCapture();
}

void Application::reselectOutputDevice()
{
    if (audioBackend == nullptr)
        return;

    std::vector<OutputDeviceCandidate> candidates;
    outputDeviceNames.clear();

    for (const auto& d : audioBackend->enumerateOutputDevices())
    {
        // §5.2: a mic's own playback endpoint is never offered as a monitor
        // output, so it does not belong in the Advanced panel's list either.
        if (! d.isMicrophone)
            outputDeviceNames.push_back (d.name);

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

bool Application::isMicLive (int index) const
{
    // Outside a take every included mic is live by definition: §6.5's silence
    // only applies to a channel already fixed into a recording.
    if (capture == nullptr || ! capture->isRecording())
        return true;

    const auto& channels = capture->getChannels();

    if (index < 0 || index >= static_cast<int> (channels.size()))
        return true;

    return ! recordingEngine.isWritingSilence (channels[static_cast<size_t> (index)].deviceId);
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

Metering* Application::getChannelMetering (int index)
{
    // Same reasoning as getMonitorBus(): the meters the audio thread feeds live
    // in the coordinator, so the UI has to read those and not a second set.
    return capture != nullptr ? capture->getChannelMetering (index) : nullptr;
}

Metering* Application::getMixMetering()
{
    return capture != nullptr ? &capture->getMixMetering() : nullptr;
}

juce::String Application::nameForDevice (const std::string& identityKey,
                                         const juce::String& fallback) const
{
    for (const auto& d : deviceManager.getDevices())
    {
        if (d.identity.key() != identityKey)
            continue;

        // The name the user gave this port wins over the product string --
        // otherwise the skull says "Blue Yeti" while the files say "Kitchen".
        if (const auto persisted = portIdentityStore.get (d.identity))
            if (! persisted->assignedName.empty())
                return juce::String (persisted->assignedName);

        return juce::String (d.displayName);
    }

    return fallback;
}

juce::String Application::getMicProductName (int index) const
{
    // §14.6: with four identical microphones on a desk, the name the user gave
    // a port is what identifies it -- but the hardware's own name is what tells
    // them which *kind* of thing it is. The strip has always reserved a line
    // for it under the bold name and always drawn it empty, because
    // setDeviceName() had no callers anywhere.
    //
    // Resolved in the take's channel space while one is running, for the same
    // reason getMicDisplayName is: after a mid-take unplug the device list and
    // the channel list are different lengths.
    std::string key;
    juce::String fallback;

    if (capture != nullptr && capture->isRecording())
    {
        const auto& channels = capture->getChannels();

        if (index < 0 || index >= static_cast<int> (channels.size()))
            return {};

        key = channels[static_cast<size_t> (index)].deviceId;
        fallback = juce::String (channels[static_cast<size_t> (index)].displayName);
    }
    else
    {
        int seen = 0;
        for (const auto& d : deviceManager.getDevices())
        {
            if (! d.included)
                continue;

            if (seen == index)
            {
                key = d.identity.key();
                fallback = juce::String (d.displayName);
                break;
            }

            ++seen;
        }

        if (key.empty())
            return {};
    }

    juce::String product = fallback;

    for (const auto& d : deviceManager.getDevices())
        if (d.identity.key() == key)
            product = juce::String (d.displayName);

    // Nothing to add when the strip is already showing this exact text: an
    // unnamed microphone would otherwise print its product string twice, once
    // bold and once faint underneath.
    return product == getMicDisplayName (index) ? juce::String() : product;
}

juce::String Application::getMicDisplayName (int index) const
{
    // Mid-take, resolve through the take's frozen channel list: the device the
    // caller means is the one recording into channel `index`, which after an
    // unplug is no longer "the index-th included device". The channel keeps the
    // name it was opened with, so an unplugged microphone's strip still says
    // whose it is rather than going blank.
    if (capture != nullptr && capture->isRecording())
    {
        const auto& channels = capture->getChannels();

        if (index < 0 || index >= static_cast<int> (channels.size()))
            return {};

        const auto& ch = channels[static_cast<size_t> (index)];
        return nameForDevice (ch.deviceId, juce::String (ch.displayName));
    }

    int seen = 0;
    for (const auto& d : deviceManager.getDevices())
    {
        if (! d.included)
            continue;

        if (seen == index)
            return nameForDevice (d.identity.key(), juce::String (d.displayName));

        ++seen;
    }

    return {};
}

void Application::setMicAssignedName (int index, const juce::String& name)
{
    int seen = 0;
    for (const auto& d : deviceManager.getDevices())
    {
        if (! d.included)
            continue;

        if (seen++ != index)
            continue;

        auto settings = portIdentityStore.get (d.identity).value_or (PersistedDeviceSettings{});
        settings.assignedName = SessionFolderNaming::sanitizeName (name.toStdString());
        portIdentityStore.put (d.identity, settings);

        saveSettings();

        // The capture channels carry the display name into the stem filenames
        // (§6.2), so they are rebuilt -- but never mid-take, where §6.5 fixes
        // the channel list for the duration of the recording.
        if (capture != nullptr && ! capture->isRecording())
            restartCapture();

        return;
    }
}

std::vector<Application::StorageVolume> Application::getStorageVolumes() const
{
    std::vector<StorageVolume> volumes;
    juce::StringArray seen;

    const auto add = [&] (const juce::File& root, bool forceRemovable)
    {
        if (! root.isDirectory() || ! root.hasWriteAccess())
            return;

        const auto full = root.getFullPathName();

        if (seen.contains (full))
            return;

        seen.add (full);

        const auto freeBytes = root.getBytesFreeOnVolume();
        const bool removable = forceRemovable || root.isOnRemovableDrive();

        auto name = root.getVolumeLabel();
        if (name.isEmpty())
            name = root.getFileName();
        if (name.isEmpty())
            name = full;

        juce::String label = name;
        if (removable)
            label += " (removable)";
        if (freeBytes > 0)
            label += " - " + juce::File::descriptionOfSizeInBytes (freeBytes) + " free";

        // Recordings land in a folder on the volume rather than loose at its
        // root, which is what someone expects when they hand a card to an
        // editor and what keeps a card usable for anything else.
        const auto destination = root.getChildFile ("RECORDINGS");

        // destinationFolder is a std::string, so the comparison is made
        // explicitly rather than left to an ambiguous mixed-type operator==.
        const bool isCurrent =
            destination.getFullPathName() == juce::String (destinationFolder);

        volumes.push_back ({ label, destination.getFullPathName(), removable, isCurrent });
    };

    // Mounted volumes. On macOS every attached card and disk appears under
    // /Volumes; on Windows the roots are the drive letters; on Linux the
    // common mount points are covered by the roots plus /media and /mnt.
    const juce::File volumesDir ("/Volumes");
    if (volumesDir.isDirectory())
        for (const auto& child : volumesDir.findChildFiles (juce::File::findDirectories, false))
            add (child, false);

    for (const auto& dir : { juce::File ("/media"), juce::File ("/mnt") })
        if (dir.isDirectory())
            for (const auto& child : dir.findChildFiles (juce::File::findDirectories, false))
                add (child, true);

    juce::Array<juce::File> roots;
    juce::File::findFileSystemRoots (roots);
    for (const auto& root : roots)
        add (root, false);

    // Always last, and always present: the one destination that cannot be
    // unplugged mid-take.
    add (juce::File::getSpecialLocation (juce::File::userHomeDirectory), false);

    return volumes;
}

void Application::setDestinationByPath (const juce::String& path)
{
    const juce::File target (path);

    // Created on selection rather than at record time, so an unwritable card is
    // discovered while someone is looking at the setting, not when they press
    // record.
    if (! target.exists())
        target.createDirectory();

    if (target.isDirectory() && target.hasWriteAccess())
        setDestinationFolder (target);
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

                // §6.3: the mirror decision was made just above, at arm time.
                const auto mirror = mirrorPolicy.isMirroring()
                                        ? createMirrorFolder (juce::File (folder).getFileName())
                                        : juce::String();

                if (folder.isEmpty()
                    || ! capture->startRecording (folder.toStdString(), currentBitDepth,
                                                  now.toISO8601 (true).toStdString(),
                                                  mirror.toStdString()))
                {
                    // Nothing was written, so the engine must not claim a take.
                    recordingEngine.stop();
                    recordingStartMs = 0.0;
                    return;
                }

                currentSessionFolder = folder;
                currentMirrorFolder = mirror;
                sessionStartIso = now.toISO8601 (true);

                // The picture starts with the sound, into the same folder. A
                // camera the user switched on but never looked at is opened
                // here rather than being quietly left out of the take.
                openEnabledCameras();
                cameraController.startRecording (juce::File (folder));

                // §6.2: session.json is written at the start so a crash mid-take
                // still leaves a record of what the rig was, and rewritten on
                // stop to add the stop time and everything logged since.
                writeSessionMetadata (false);
            }

            // Each take gets its own warnings; a previous one must not leave the
            // ten-minute warning already spent.
            midTakeDropouts.clear();
            midTakeNotice.clear();
            midTakeNoticeSeconds = 0.0;

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

        // Before the folder is listed for the panel that shows what was saved,
        // so the video files are closed and their real sizes are on disk by the
        // time anyone reads them.
        cameraController.stopRecording();

        // Written after stopRecording() so the frame counts and buffer log it
        // records are the take's final ones -- and before recordingStartMs is
        // cleared, or every timestamp inside it would read as zero.
        writeSessionMetadata (true);

        // §10.6: the outcome is stated, not implied. Ten seconds is enough to
        // read without becoming furniture.
        lastSessionFolder = currentSessionFolder;
        lastMirrorFolder = currentMirrorFolder;
        savedNoticeSeconds = 10.0;

        // Judged here, against the files as finalized, so the status line and
        // the saved-take card cannot disagree. They used to: the card warned
        // that every file was empty while this line said "Saved to ..." beside
        // it, and the line is the one a user reads on their way out of the room.
        {
            std::vector<TakeFile> written;
            for (const auto& f : listSessionFiles (lastSessionFolder))
                written.push_back ({ f.name.toStdString(), f.sizeBytes });

            lastTakeHeldNoAudio = takeHoldsNoAudio (written);
        }

        // §6.2: the take is on disk and the UI has not shown where yet. Only
        // raised when a folder was actually opened -- a start that failed
        // preflight never got one, and "saved" would be a lie.
        savedTakePending = currentSessionFolder.isNotEmpty();

        recordingEngine.stop();
        recordingStartMs = 0.0;
        bufferLadder.setRecording (false);

        currentSessionFolder.clear();
        currentMirrorFolder.clear();
    }
}

juce::String Application::resolveSessionFolderName (juce::Time now, const juce::String& name) const
{
    const juce::File root (destinationFolder);

    // §6.2: the user's session name, sanitized; "Session" when they gave none.
    auto cleaned = SessionFolderNaming::sanitizeName (name.toStdString());
    if (cleaned.empty())
        cleaned = SessionFolderNaming::kDefaultName;

    const auto desired = SessionFolderNaming::buildFolderName (now.getYear(), now.getMonth() + 1,
                                                               now.getDayOfMonth(), now.getHours(),
                                                               now.getMinutes(), cleaned);

    // §6.2: never overwrite, never prompt -- collisions get _2, _3, ...
    return juce::String (SessionFolderNaming::resolveCollision (desired,
        [&root] (const std::string& candidate)
        {
            return root.getChildFile (juce::String (candidate)).exists();
        }));
}

juce::String Application::createSessionFolder (juce::Time now) const
{
    const juce::File root (destinationFolder);
    const auto folder = root.getChildFile (resolveSessionFolderName (now, sessionName));

    if (! folder.createDirectory().wasOk())
        return {};

    return folder.getFullPathName();
}

Application::PlannedSave Application::planSave (const juce::String& proposedSessionName) const
{
    PlannedSave plan;
    plan.parentFolder = juce::String (destinationFolder);
    plan.folderName = resolveSessionFolderName (juce::Time::getCurrentTime(), proposedSessionName);
    plan.fullPath = juce::File (plan.parentFolder).getChildFile (plan.folderName).getFullPathName();

    // §6.3: the mirror decision is only actually taken at arm time, against the
    // free space then. Showing where it *would* go is still worth doing -- a
    // second copy the user does not know about is a second copy they will not
    // find -- so this reports the path whenever the setting is on, and the
    // panel shown at stop reports what really happened.
    if (mirrorPolicy.isEnabledByUser())
        plan.mirrorFolder = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                .getChildFile ("RECORDINGS-MIRROR")
                                .getChildFile (plan.folderName)
                                .getFullPathName();

    // §6.1: one file per microphone plus the mix, exactly as WritePipeline
    // opens them, plus the §6.2 session.json.
    plan.fileNames.add ("MIX.wav");

    for (const auto& c : buildCaptureChannels())
        plan.fileNames.add (juce::String (c.fileName) + ".wav");

    // One file per camera in the take, in the same folder. Listed with the rest
    // because "where are my files" has one answer, not two.
    plan.fileNames.addArray (cameraController.getPlannedFileNames());

    plan.fileNames.add ("session.json");

    return plan;
}

bool Application::isSaveLocationConfirmed() const
{
    if (askWhereToSaveEveryTime)
        return false;

    return ! confirmedSaveLocation.empty() && confirmedSaveLocation == destinationFolder;
}

std::vector<Application::SavedFile> Application::listSessionFiles (const juce::String& folder)
{
    std::vector<SavedFile> files;

    if (folder.isEmpty())
        return files;

    const juce::File dir (folder);

    if (! dir.isDirectory())
        return files;

    for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findFiles))
    {
        const auto file = entry.getFile();
        files.push_back ({ file.getFileName(), file.getSize() });
    }

    // MIX first, session.json last, the stems in between in the channel order
    // their "01_", "02_" prefixes already encode. A user reading this list is
    // looking for "is everyone here", and the mix is the file they will play.
    const auto rank = [] (const juce::String& name)
    {
        if (name.startsWithIgnoreCase ("MIX")) return 0;
        if (name.equalsIgnoreCase ("session.json")) return 2;
        return 1;
    };

    std::sort (files.begin(), files.end(), [&rank] (const SavedFile& a, const SavedFile& b)
    {
        const int ra = rank (a.name), rb = rank (b.name);
        return ra != rb ? ra < rb : a.name.compareNatural (b.name) < 0;
    });

    return files;
}

bool Application::consumeCardRemovalNotice (CardRemovalNotice& out)
{
    if (! cardRemovalPending)
        return false;

    cardRemovalPending = false;
    out = cardRemovalNotice;

    return true;
}

bool Application::consumeSavedTake (SavedTake& out)
{
    if (! savedTakePending)
        return false;

    savedTakePending = false;
    out.folder = lastSessionFolder;
    out.mirrorFolder = lastMirrorFolder;
    out.files = listSessionFiles (lastSessionFolder);

    return true;
}

double Application::getElapsedRecordingSeconds() const
{
    if (recordingEngine.getState() != RecordingState::Recording || recordingStartMs <= 0.0)
        return 0.0;

    return (juce::Time::getMillisecondCounterHiRes() - recordingStartMs) / 1000.0;
}

int Application::getIncludedMicCount() const
{
    // During a take this is the count the meters, the drift reports, the
    // advisor and the skull strips are all indexed by, and every one of those
    // reads out of capture. The device list is not the same length once a
    // microphone is unplugged mid-take -- counting it there left the last
    // channel's meter unread and shifted every name past the gap onto the
    // wrong strip.
    if (capture != nullptr && capture->isRecording())
        return static_cast<int> (capture->getChannels().size());

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

double Application::bytesPerSecondOfRecording() const
{
    return bytesPerSecondOfAudio()
         + static_cast<double> (cameraController.getSelection().getEstimatedBytesPerSecond());
}

double Application::getRemainingRecordingSeconds() const
{
    const double bytesPerSecond = bytesPerSecondOfRecording();

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

    // §6.4: pre-flight blocks arming rather than degrading mid-take.
    if (preflightRunning.load())
        return "Checking this drive is fast enough...";

    {
        std::lock_guard<std::mutex> lock (preflightMutex);
        const auto it = preflightResults.find (destinationFolder);

        if (it != preflightResults.end())
        {
            // The benchmark measured the card; the gate is about this take. They
            // are applied apart so that switching a camera or a microphone on
            // re-answers the question here, rather than leaving the verdict
            // frozen at whatever the rig was when the 200 MB test last ran --
            // which would let a card pass for the audio and then fail mid-take
            // once a camera started, the exact outcome §6.4 exists to prevent.
            const auto verdict = PreflightThroughputTest::evaluateMeasured (
                it->second.sustainedMinBytesPerSec,
                std::max (1, getIncludedMicCount()),
                currentSampleRate,
                std::max (1, currentBitDepth / 8),
                static_cast<double> (cameraController.getSelection().getEstimatedBytesPerSecond()));

            if (! verdict.passed)
                return juce::String (verdict.reason);
        }
    }

    return {};
}

void Application::beginPreflightForDestination()
{
    if (preflightRunning.load() || destinationFolder.empty())
        return;

    {
        std::lock_guard<std::mutex> lock (preflightMutex);

        // §6.4: cached per volume. Re-benchmarking a card the user already
        // waited on, every launch, is exactly the friction §10.1 rules out.
        if (preflightResults.count (destinationFolder) > 0)
            return;
    }

    const int channelCount = std::max (1, getIncludedMicCount());
    const auto target = destinationFolder;

    if (preflightThread.joinable())
        preflightThread.join();

    preflightRunning.store (true);
    preflightThread = std::thread ([this, target, channelCount] { runPreflight (target, channelCount); });
}

void Application::runPreflight (const std::string& destination, int channelCount)
{
    PreflightResult result;

    const juce::File folder { juce::String (destination) };
    folder.createDirectory();

    const auto testFile = folder.getNonexistentChildFile ("preflight", ".tmp");

    std::vector<double> rollingWindows;
    const int bytesPerSample = currentBitDepth / 8;

    {
        // §6.4: 200 MB, written the way a take writes -- steadily, measuring the
        // sustained floor rather than a burst into the OS cache.
        juce::FileOutputStream out (testFile);

        if (out.openedOk())
        {
            constexpr size_t kChunkBytes = 1024 * 1024;
            const std::vector<char> chunk (kChunkBytes, 0);

            size_t written = 0;
            size_t writtenThisWindow = 0;
            auto windowStart = std::chrono::steady_clock::now();

            while (written < PreflightThroughputTest::kTestFileBytes)
            {
                // Quit must not wait for a slow card to swallow 200 MB.
                if (preflightAbort.load())
                    break;

                if (! out.write (chunk.data(), kChunkBytes))
                    break;

                written += kChunkBytes;
                writtenThisWindow += kChunkBytes;

                const auto elapsed = std::chrono::duration<double> (
                    std::chrono::steady_clock::now() - windowStart).count();

                if (elapsed >= 1.0)
                {
                    rollingWindows.push_back (static_cast<double> (writtenThisWindow) / elapsed);
                    writtenThisWindow = 0;
                    windowStart = std::chrono::steady_clock::now();
                }
            }

            out.flush();

            // A card fast enough to finish inside one window still needs a
            // sample, or the gate would see no data and fail a good drive.
            if (rollingWindows.empty() && writtenThisWindow > 0)
            {
                const auto elapsed = std::chrono::duration<double> (
                    std::chrono::steady_clock::now() - windowStart).count();

                if (elapsed > 0.0)
                    rollingWindows.push_back (static_cast<double> (writtenThisWindow) / elapsed);
            }
        }
    }

    testFile.deleteFile();

    // An aborted run proved nothing about the card; caching its verdict would
    // wrongly condemn the volume on the next launch of this session.
    if (! preflightAbort.load())
    {
        // What is kept from this is the measurement. The pass/fail and the
        // wording alongside it are a snapshot of the rig as it was during the
        // benchmark; getRecordDisabledReason() applies the gate again against
        // the rig as it is when someone actually reaches for record.
        result = PreflightThroughputTest::evaluate (rollingWindows, channelCount,
                                                    currentSampleRate, bytesPerSample);

        std::lock_guard<std::mutex> lock (preflightMutex);
        preflightResults[destination] = result;
    }

    preflightRunning.store (false);
}

void Application::setMasterVolume (double volume0to100)
{
    // Held here rather than only on the bus: §2.2 and §5.4 can both rebuild the
    // coordinator underneath us, and the listening level must not jump back to
    // the default when they do.
    masterVolume = volume0to100;

    if (auto* bus = getMonitorBus())
        bus->setMasterVolume (masterVolume);

    // Deliberately not saved here. This is the one control that moves
    // continuously while someone listens, and it is comfort rather than setup:
    // losing it costs a second to reset, where losing a trim costs the ear-work
    // that found it. It goes to disk with everything else at shutdown.
}

double Application::getMasterVolume() const
{
    return masterVolume;
}

void Application::setChannelTrimDb (int index, float trimDb)
{
    // §4: clamped to the stated range and quantised to the stated step, so the
    // value that gets persisted is one the UI can round-trip exactly.
    const auto clamped = juce::jlimit (static_cast<float> (MonitorBus::kMinTrimDb),
                                       static_cast<float> (MonitorBus::kMaxTrimDb), trimDb);
    const auto step = static_cast<float> (MonitorBus::kTrimStepDb);
    const auto quantised = std::round (clamped / step) * step;

    int seen = 0;
    for (const auto& d : deviceManager.getDevices())
    {
        if (! d.included)
            continue;

        if (seen++ != index)
            continue;

        // §4 persists trim against the physical port, not the slot, so it
        // follows the mic when it is unplugged and moved.
        auto settings = portIdentityStore.get (d.identity).value_or (PersistedDeviceSettings{});
        settings.trimDb = quantised;
        portIdentityStore.put (d.identity, settings);
        break;
    }

    if (capture != nullptr)
        capture->setChannelTrimDb (index, quantised);

    // Written on each step rather than only at quit: a trim is setup the user
    // did by ear and would have to redo, and §4 quantises it to 0.5 dB, so a
    // drag across the whole range is a few dozen writes of a small file in the
    // application-data folder -- never on the card a take is being written to.
    saveSettings();
}

float Application::getChannelTrimDb (int index) const
{
    int seen = 0;
    for (const auto& d : deviceManager.getDevices())
    {
        if (! d.included)
            continue;

        if (seen++ != index)
            continue;

        if (const auto settings = portIdentityStore.get (d.identity))
            return settings->trimDb;

        return 0.0f;
    }

    return 0.0f;
}

juce::String Application::getActiveBackendDescription() const
{
    if (virtualDeviceBackend == nullptr)
        return "None";

    const auto status = virtualDeviceBackend->getStatus();

    // §7/§10.3: say what other apps can see, not which driver model is in use.
    if (! status.reachDescription.empty())
        return juce::String (status.reachDescription);

    return "Recording and monitoring only. Other apps can't see these mics.";
}

const std::vector<std::string>& Application::getOutputDeviceNames() const
{
    // §2: enumeration happens on the OS device-change notification, never on a
    // timer. The Advanced panel repaints at 2 Hz and must read this cache
    // rather than go back to the driver each time.
    return outputDeviceNames;
}

juce::String Application::getClockMasterName() const
{
    if (const auto* master = deviceManager.selectDefaultMaster())
        return juce::String (master->displayName);

    return {};
}

juce::String Application::getDriftReport() const
{
    juce::StringArray lines;

    for (const auto& d : deviceManager.getDevices())
    {
        if (! d.included)
            continue;

        // §3.1: drift is not claimed until 60 seconds of measurement exist.
        // Showing a number before then would be showing noise.
        lines.add (juce::String (d.displayName) + ": "
                   + (d.hasDriftMeasurement
                          ? juce::String (d.measuredDriftPpm, 1) + " PPM"
                          : juce::String ("measuring...")));
    }

    if (lines.isEmpty())
        return "No microphones connected.";

    return lines.joinIntoString ("\n");
}

void Application::setOutputDeviceByName (const juce::String& displayName)
{
    if (audioBackend == nullptr)
        return;

    // A user action, not a timer, so enumerating here is what §2 allows -- and
    // it is the only way to recover the device's stable id from its name.
    for (const auto& d : audioBackend->enumerateOutputDevices())
    {
        if (juce::String (d.name) != displayName)
            continue;

        // §5.3: an explicit choice is remembered and outranks the automatic
        // priority order from then on.
        rememberedOutputDeviceId = d.usbLocationId.empty() ? d.name : d.usbLocationId;
        reselectOutputDevice();
        restartCapture();
        return;
    }
}

std::vector<Application::MicSelection> Application::getMicSelections() const
{
    std::vector<MicSelection> out;

    for (const auto& d : deviceManager.getDevices())
        out.push_back ({ juce::String (d.displayName), d.userEnabled, d.isBuiltIn });

    return out;
}

void Application::setMicEnabledByName (const juce::String& displayName, bool enabled)
{
    for (const auto& d : deviceManager.getDevices())
    {
        // Matched on display name because that is what the panel shows. Not on
        // `included`: a deselected microphone is excluded, and looking only at
        // included ones would make it impossible to tick back on.
        if (juce::String (d.displayName) != displayName)
            continue;

        if (deviceManager.setUserEnabled (d.identity.key(), enabled))
            restartCapture(); // the channel set changed, so the streams must be reopened

        saveSettings();
        return;
    }
}

void Application::setClockMasterByName (const juce::String& displayName)
{
    for (const auto& d : deviceManager.getDevices())
    {
        if (! d.included || juce::String (d.displayName) != displayName)
            continue;

        deviceManager.setPreferredMaster (d.identity.key());
        applyClockMaster();
        return;
    }
}

void Application::setDestinationFolder (const juce::File& folder)
{
    if (! folder.isDirectory())
        return;

    destinationFolder = folder.getFullPathName().toStdString();

    // §10.1: the user agreed to a place, not to a setting. Somewhere else has
    // not been agreed to, so it gets asked about before the next take.
    if (confirmedSaveLocation != destinationFolder)
        confirmedSaveLocation.clear();

    // §6.4: a new volume is an unbenchmarked volume.
    beginPreflightForDestination();

    saveSettings();
}

juce::String Application::createMirrorFolder (const juce::String& sessionFolderName)
{
    // §6.3: the mirror lives on the internal drive, which is the whole point --
    // a card failure must not take both copies with it.
    const auto root = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                          .getChildFile ("RECORDINGS-MIRROR")
                          .getChildFile (sessionFolderName);

    if (! root.createDirectory().wasOk())
        return {};

    return root.getFullPathName();
}

void Application::writeSessionMetadata (bool sessionHasStopped)
{
    if (currentSessionFolder.isEmpty())
        return;

    SessionMetadata meta;
    meta.appVersion = JUCE_STRINGIFY (JUCE_APP_VERSION);
    meta.startTimestampIso = sessionStartIso.toStdString();
    meta.stopTimestampIso = sessionHasStopped
                                ? juce::Time::getCurrentTime().toISO8601 (true).toStdString()
                                : std::string();
    meta.sampleRate = currentSampleRate;
    meta.bitDepth = currentBitDepth;
    meta.bufferSizeSamples = bufferLadder.getCurrentSize();
    meta.measuredLatencyMs = measuredLatencyMs;

    for (const auto& d : deviceManager.getDevices())
    {
        if (! d.included)
            continue;

        DeviceRecord record;
        record.name = d.displayName;
        record.usbId = d.identity.key();

        if (const auto persisted = portIdentityStore.get (d.identity))
            record.trimDb = persisted->trimDb;

        meta.devices.push_back (std::move (record));

        // §3.2: drift is only claimed once 60 seconds of measurement exist.
        if (d.hasDriftMeasurement)
            meta.driftLog.push_back ({ getElapsedRecordingSeconds(), d.identity.key(), d.measuredDriftPpm });
    }

    // §5.4 requires every buffer step logged.
    for (const auto& change : bufferLadder.getChangeLog())
        meta.bufferChanges.push_back ({ change.atSeconds, change.fromSamples, change.toSamples });

    // The cameras in this take, and their files. An editor opening the folder
    // later needs to know which picture goes with which take and that the sound
    // is not in it -- the session origin every stem carries is what lines the
    // two up, so the fact that they are separate files has to be on the record.
    {
        const auto videoNames = cameraController.getPlannedFileNames();
        const auto plans = cameraController.getSelection().buildPlans();

        for (size_t i = 0; i < plans.size() && (int) i < videoNames.size(); ++i)
            meta.videos.push_back ({ plans[i].displayName, videoNames[(int) i].toStdString(), false });
    }

    meta.mirrorEnabled = mirrorPolicy.getState() != MirrorState::DisabledByUser;
    meta.mirrorActive = capture != nullptr && capture->isMirroring();
    meta.mirrorPath = currentMirrorFolder.toStdString();

    // §6.5's mid-recording row: every unplug and reconnection during this take.
    for (const auto& entry : midTakeDropouts)
        meta.dropouts.push_back (entry);

    // §6.5: "log the exact sample position of degradation." Recorded as a
    // dropout entry rather than a new field, because that is what it is: the
    // moment the stems stopped receiving audio they should have had.
    if (const auto degradedAt = capacityMonitor.getDegradationSamplePosition(); degradedAt >= 0)
        meta.dropouts.push_back ({ getElapsedRecordingSeconds(), std::string(),
                                   "Fell back to writing the mix only at sample "
                                       + std::to_string (degradedAt) });

    // §6.3: a mirror that stopped mid-take must be visible in the record --
    // otherwise the copy looks complete and is not.
    if (mirrorPolicy.wasStoppedForSpace())
        meta.dropouts.push_back ({ getElapsedRecordingSeconds(), std::string(),
                                   "Local backup copy stopped: the internal drive ran low on space." });

    // §6.3 again, for the other way a mirror can stop. Without this line a
    // backup copy truncated by a failed drive looks exactly like a complete
    // one -- the file is simply shorter, and nothing says why.
    if (mirrorPolicy.wasStoppedForWriteFailure())
        meta.dropouts.push_back ({ getElapsedRecordingSeconds(), std::string(),
                                   "Local backup copy stopped: that drive stopped accepting writes. "
                                   "The copy is incomplete from this point." });

    if (capture != nullptr && capture->getFramesDropped() > 0)
        meta.dropouts.push_back ({ getElapsedRecordingSeconds(), std::string(),
                                   "Dropped " + std::to_string (capture->getFramesDropped())
                                       + " frames: the drive could not keep up." });

    // Written to the card copy and the mirror alike, so either one stands alone.
    const auto json = meta.toJsonString();
    juce::File (currentSessionFolder).getChildFile ("session.json").replaceWithText (juce::String (json));

    if (currentMirrorFolder.isNotEmpty())
        juce::File (currentMirrorFolder).getChildFile ("session.json").replaceWithText (juce::String (json));
}

juce::String Application::pollStatusAdvice (double sinceLastCallSeconds)
{
    // Cleared first, not inside the tap block: a capacity or performance
    // warning returns early below, and a stale index would leave one skull
    // lit indefinitely.
    tappedChannel = -1;

    // §8.1: the detectors only see anything if the per-block peaks reach them,
    // so this is where the §10.5 advice actually gets its input.
    const int micCount = getIncludedMicCount();
    std::vector<float> peaksDb;
    peaksDb.reserve (static_cast<size_t> (micCount));

    for (int i = 0; i < micCount; ++i)
    {
        auto* meter = getChannelMetering (i);
        peaksDb.push_back (meter != nullptr ? static_cast<float> (meter->getPeakHoldDb())
                                            : static_cast<float> (Metering::kMinDb));
    }

    updateSetupAdvisorLevels (peaksDb, sinceLastCallSeconds);

    // §3.2 / §3.3: the loop runs in the audio callback; reporting it does not.
    if (capture != nullptr)
    {
        capture->tickDriftReporting (sinceLastCallSeconds);
        driftMeasuredSeconds += sinceLastCallSeconds;

        // Walked over capture's channels rather than the device list: the two
        // agree only until a microphone is unplugged mid-take, and after that
        // this loop was attributing each channel's drift to whichever device
        // had shifted into its position -- so the Advanced panel showed one
        // microphone's PPM under another's name, and §3.3's 100 PPM flag could
        // land on a device that was keeping perfect time.
        {
            const auto& channels = capture->getChannels();

            for (size_t i = 0; i < channels.size(); ++i)
                deviceManager.updateMeasuredDrift (channels[i].deviceId,
                                                   capture->getChannelDriftPpm (static_cast<int> (i)),
                                                   driftMeasuredSeconds);
        }

        // §3.1: the master is re-picked as measurements arrive, so a rig that
        // started on enumeration order settles onto the steadiest clock.
        applyClockMaster();

        // §3.3: a device this far out is not drifting, it is failing.
        for (int i = 0; i < micCount; ++i)
        {
            if (! capture->hasSustainedExcessDrift (i))
                continue;

            return juce::String (getMicDisplayName (i))
                   + " can't keep steady time with the others. Try a different USB port.";
        }
    }

    // §6.5 "target card removed" outranks even running out of room: the drive
    // is not merely full, it is gone, and every further block would be written
    // into nothing. Checked before anything else because the take has to stop
    // now rather than at the end of this function.
    if (capture != nullptr && capture->hasCardWriteFailed()
        && recordingEngine.getState() == RecordingState::Recording)
    {
        // Built before the take is torn down, while the mirror's path and
        // whether it was still running are both still known.
        cardRemovalNotice = CardRemovalNotice::build (currentMirrorFolder.toStdString(),
                                                      capture->isMirroring());
        cardRemovalPending = true;

        // The ordinary stop path: it finalizes every open file (§6.5 "finalize
        // every open file"), writes session.json and raises the saved-take
        // notice, which is what shows the user whatever did survive.
        toggleRecording();

        return juce::String (cardRemovalNotice.message);
    }

    // §6.3: the mirror's destination failed -- unplugged, read-only, or dead.
    //
    // WritePipeline has always stopped mirroring on a failed write, and rightly
    // leaves the card write alone (§6.3: the mirror must never take the
    // recording down with it). What nothing ever did was *notice*, so pulling
    // the backup drive mid-take stopped the copy in silence: no message, and
    // nothing in session.json, because only the low-space stop was ever
    // recorded. The user kept a truncated backup they had every reason to
    // believe was complete.
    //
    // Checked outside the isMirroring() guard below, and deliberately so: the
    // pipeline raises this flag and stops mirroring in the same breath, on its
    // own writer thread. By the time this poll runs isMirroring() is already
    // false, so nesting the check inside that guard would make it unreachable
    // -- policy that exists and never runs, which is the whole shape of bug
    // this series keeps finding. noteWriteFailure() is the idempotence: it
    // fires only on the transition out of Active, so a mirror that never
    // started, or one already stopped for space, produces nothing.
    //
    // capture->stopMirroring() is not called here because the pipeline has
    // already done it; this only catches the policy up so session.json can say
    // why the copy is short.
    if (capture != nullptr && capture->hasMirrorWriteFailed()
        && mirrorPolicy.noteWriteFailure())
    {
        return "The local backup copy stopped -- that drive stopped accepting writes. "
               "The recording itself is unaffected and is still going to the card.";
    }

    // §6.3: the mirror is re-judged during the take, and once it stops it never
    // restarts within the same recording.
    if (capture != nullptr && capture->isMirroring())
    {
        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

        if (mirrorPolicy.evaluateDuringRecording (home.getBytesFreeOnVolume())
            == MirrorState::StoppedLowSpace)
        {
            capture->stopMirroring();
            return "The local backup copy stopped -- this drive is low on space. "
                   "The recording itself is unaffected.";
        }
    }


    // The notice's clock runs from here, not from the branch that shows it:
    // decremented where it is returned, a warning outranking it for a few
    // seconds would silently extend how long it lingers afterwards.
    if (midTakeNoticeSeconds > 0.0)
        midTakeNoticeSeconds -= sinceLastCallSeconds;

    // §6.5 buffer back-pressure, before the remaining-time warnings: the ring
    // filling up is audio about to be lost right now, where running low on
    // room is audio that will stop being recorded later.
    //
    // CapacityMonitor has always had this policy, and always had tests for it.
    // Nothing had ever called it, so the 50% warning was never shown and the
    // 90% fallback never happened -- §0.1's "never silently drop" was exactly
    // what the app did.
    if (capture != nullptr && recordingEngine.getState() == RecordingState::Recording)
    {
        switch (capacityMonitor.evaluateFill (capture->getRingFillFraction(), capture->isMirroring()))
        {
            case WritePipelineState::DegradedToMixOnly:
                if (! capture->isMixOnly())
                {
                    capture->fallBackToMixOnly();
                    // §6.5: "log the exact sample position of degradation."
                    capacityMonitor.noteDegradationAt (static_cast<long long> (capture->getFramesAccepted()));
                }

                return "The drive can't keep up. Still recording everyone into the mixed file, but the "
                       "separate microphone tracks have stopped. Close other apps using the disk.";

            case WritePipelineState::FillWarning:
                return "The drive is falling behind. Nothing has been lost yet -- close any other apps "
                       "using the disk.";

            case WritePipelineState::Healthy:
                break;
        }
    }

    // §6.5 next: running out of room stops the take, which outranks everything
    // else that is merely a warning.
    switch (pollCapacityWarning())
    {
        case RemainingTimeWarning::Exhausted:
            return "The drive is full. Recording has stopped -- free some space or choose another drive.";
        case RemainingTimeWarning::TwoMinutes:
            return "About two minutes of room left. Wrap up or switch drives now.";
        case RemainingTimeWarning::TenMinutes:
            return "About ten minutes of room left on this drive.";
        case RemainingTimeWarning::None:
            break;
    }

    // §6.6 next: warned before it causes dropouts, not after.
    const auto load = capture != nullptr ? capture->getAudioCallbackLoad() : 0.0;

    switch (updatePerformance (load, false))
    {
        case PerformanceWarning::ThermalThrottling:
            return "This machine is overheating and is about to drop audio. Close other apps.";
        case PerformanceWarning::SustainedCpuPressure:
            return "This machine is working hard. Close other apps before it starts dropping audio.";
        case PerformanceWarning::None:
            break;
    }

    // §6.5: a microphone plugged in mid-take joins nothing, and the user has to
    // be told rather than left assuming it is being recorded. Ranked here, below
    // everything that means audio is being lost or the take is about to end: it
    // is information, not a problem, and it must never sit on top of a warning
    // that the drive is failing.
    if (midTakeNoticeSeconds > 0.0 && midTakeNotice.isNotEmpty())
        return midTakeNotice;

    // §14.6: which mic was just heard alone. Not an error and not a message --
    // the UI highlights that skull, which is how a user with four identical
    // mics learns which meter is which person. Runs only outside a take, when
    // naming actually happens.
    if (capture == nullptr || ! capture->isRecording())
    {
        if (tapDetector == nullptr || tapDetectorChannels != micCount)
        {
            tapDetector = micCount > 0 ? std::make_unique<TapToNameDetector> (micCount) : nullptr;
            tapDetectorChannels = micCount;
        }

        if (tapDetector != nullptr)
        {
            const auto result = tapDetector->processBlock (peaksDb, sinceLastCallSeconds);

            if (result == TapResult::ChannelIdentified)
                tappedChannel = tapDetector->getTappedChannel();

            // Latch consumed; listen for the next tap. The meter's 2-second
            // peak hold keeps re-identifying while the sound decays, which is
            // what makes the highlight linger long enough to see.
            if (result != TapResult::Listening)
                tapDetector->reset();
        }
    }

    // §10.6: after a take, say where it went. Outranks ambient advice because
    // it answers the question the user actually has right now.
    if (savedNoticeSeconds > 0.0)
    {
        savedNoticeSeconds -= sinceLastCallSeconds;

        // §0.1: never claim audio that is not there. A take whose files hold
        // nothing but headers is where the take went, not what it saved, and
        // saying "Saved" would be the one word the user needed to be false.
        if (lastTakeHeldNoAudio)
            return "Recording stopped, but the files in " + lastSessionFolder
                   + " are empty -- no audio reached the drive.";

        return "Saved to " + lastSessionFolder;
    }

    // §10.5 last: hardware guidance, most serious first.
    const auto advice = getSetupAdvice();

    if (! advice.empty())
        return juce::String (advice.front().message);

    return {};
}

void Application::exportDiagnostics (const juce::File& destinationZip)
{
    // §11: logs + last 5 session.json + device inventory. NEVER audio -- the
    // point of a diagnostics bundle is that it can be sent to a stranger.
    juce::ZipFile::Builder builder;

    juce::Array<juce::var> deviceArray;
    for (const auto& d : deviceManager.getDevices())
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("name", juce::String (d.displayName));
        obj->setProperty ("usbId", juce::String (d.identity.key()));
        obj->setProperty ("included", d.included);
        obj->setProperty ("exclusionReason", juce::String (d.exclusionReason));
        obj->setProperty ("driftPpm", d.measuredDriftPpm);
        obj->setProperty ("driftMeasured", d.hasDriftMeasurement);
        deviceArray.add (juce::var (obj));
    }

    auto* summary = new juce::DynamicObject();
    summary->setProperty ("appVersion", JUCE_STRINGIFY (JUCE_APP_VERSION));
    summary->setProperty ("sampleRate", currentSampleRate);
    summary->setProperty ("bitDepth", currentBitDepth);
    summary->setProperty ("bufferSize", bufferLadder.getCurrentSize());
    summary->setProperty ("backend", audioBackend != nullptr
                                         ? juce::String (audioBackend->getBackendName())
                                         : juce::String ("none"));
    summary->setProperty ("outputDevice", juce::String (selectedOutputDeviceId));
    summary->setProperty ("outputProblem", juce::String (outputSelectionProblem));
    summary->setProperty ("monitorProblem", getMonitorProblem());
    summary->setProperty ("destination", juce::String (destinationFolder));
    summary->setProperty ("devices", juce::var (deviceArray));

    juce::TemporaryFile tempInventory;
    tempInventory.getFile().replaceWithText (juce::JSON::toString (juce::var (summary), true));
    builder.addFile (tempInventory.getFile(), 9, "device_inventory.json");

    // §11: the last five sessions. Newest first, because the one being asked
    // about is almost always the most recent.
    auto sessions = findRecentSessionMetadata (5);
    int index = 0;

    for (const auto& file : sessions)
        builder.addFile (file, 9, "sessions/" + juce::String (++index) + "_"
                                      + file.getParentDirectory().getFileName() + ".json");

    if (const auto log = getLogFile(); log.existsAsFile())
        builder.addFile (log, 9, "log.txt");

    juce::FileOutputStream out (destinationZip);

    if (out.openedOk())
        builder.writeToStream (out, nullptr);
}

juce::File Application::getSettingsFile()
{
    return getLogFile().getSiblingFile ("settings.json");
}

void Application::loadSettings()
{
    const auto file = getSettingsFile();

    if (! file.existsAsFile())
        return;

    // Anything unreadable comes back as defaults rather than as a failure:
    // §10.1 says the app launches to a working state, and a preferences file
    // truncated by a power cut is not a reason to refuse to start.
    rememberedSettings = AppSettings::fromJsonString (file.loadFileAsString().toStdString());

    applyingRememberedSettings = true;

    // §10.1: a remembered destination wins over the default, but only if it is
    // still there -- a card that has been unplugged since must not leave the
    // app pointed at a path that no longer exists.
    if (! rememberedSettings.destinationFolder.empty())
        if (const juce::File folder { juce::String (rememberedSettings.destinationFolder) }; folder.isDirectory())
        {
            destinationFolder = rememberedSettings.destinationFolder;
            confirmedSaveLocation = rememberedSettings.confirmedSaveLocation;
        }

    askWhereToSaveEveryTime = rememberedSettings.askWhereToSaveEveryTime;
    mirrorPolicy.setEnabledByUser (rememberedSettings.mirrorEnabled);
    aggregateName = juce::String (rememberedSettings.aggregateName);
    masterVolume = rememberedSettings.masterVolume;
    cameraController.setPreviewQuality (rememberedSettings.cameraPreviewFullQuality
                                            ? PreviewQuality::Full : PreviewQuality::Low);

    // §2.4: the names and trims go back into the store they were taken from, so
    // every path that already reads it -- stem filenames, the monitor mix, the
    // trim sliders -- picks them up without knowing a file was involved.
    for (const auto& port : rememberedSettings.ports)
    {
        PortIdentity id;
        id.locationId = port.key;
        portIdentityStore.put (id, port.settings);
    }

    applyingRememberedSettings = false;
}

void Application::applyRememberedDeviceSettings()
{
    if (rememberedSettings.ports.empty() && rememberedSettings.disabledMicKeys.empty())
        return;

    applyingRememberedSettings = true;

    for (const auto& device : deviceManager.getDevices())
    {
        const auto key = device.identity.key();

        // The store is keyed by PortIdentity::key(), which is what was written,
        // so a device only matches the entry that was actually about it.
        if (const auto* port = rememberedSettings.findPort (key); port != nullptr
                && ! portIdentityStore.contains (device.identity))
            portIdentityStore.put (device.identity, port->settings);

        if (rememberedSettings.isMicDisabled (key) && device.userEnabled)
            deviceManager.setUserEnabled (key, false);
    }

    applyingRememberedSettings = false;
}

void Application::saveSettings()
{
    // Guarded so applying a loaded file cannot write a half-applied rig back
    // over the complete one it came from.
    if (applyingRememberedSettings)
        return;

    AppSettings settings;
    settings.destinationFolder = destinationFolder;
    settings.confirmedSaveLocation = confirmedSaveLocation;
    settings.askWhereToSaveEveryTime = askWhereToSaveEveryTime;
    settings.mirrorEnabled = mirrorPolicy.isEnabledByUser();
    settings.aggregateName = aggregateName.toStdString();
    settings.masterVolume = masterVolume;
    settings.cameraPreviewFullQuality = cameraController.getPreviewQuality() == PreviewQuality::Full;

    for (const auto& entry : portIdentityStore.all())
        settings.ports.push_back ({ entry.first, entry.second });

    for (const auto& device : deviceManager.getDevices())
        if (! device.userEnabled)
            settings.disabledMicKeys.push_back (device.identity.key());

    // Every camera the user has an opinion about, not only the connected ones:
    // a camera unplugged today should come back tomorrow as it was left.
    const auto& selection = cameraController.getSelection();
    for (const auto& camera : selection.getAvailableCameras())
        settings.cameras.push_back ({ camera.id,
                                      selection.isEnabled (camera.id),
                                      selection.getDisplayName (camera.id) });

    for (const auto& remembered : rememberedSettings.cameras)
        if (settings.cameras.end() == std::find_if (settings.cameras.begin(), settings.cameras.end(),
                [&remembered] (const PersistedCamera& c) { return c.id == remembered.id; }))
            settings.cameras.push_back (remembered);

    const auto file = getSettingsFile();
    file.getParentDirectory().createDirectory();
    file.replaceWithText (juce::String (settings.toJsonString()));

    rememberedSettings = settings;
}

juce::File Application::getLogFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("MultiMicAggregator")
        .getChildFile ("log.txt");
}

namespace {
struct NewestFirst
{
    static int compareElements (const juce::File& a, const juce::File& b)
    {
        const auto ta = a.getLastModificationTime();
        const auto tb = b.getLastModificationTime();
        return ta > tb ? -1 : (ta < tb ? 1 : 0);
    }
};
} // namespace

void Application::scanForInterruptedSessions()
{
    recoveredSessions.clear();

    // §6.6 names both places a take can be: the card it was written to, and the
    // mirror, which is the copy that survives when the card is what failed.
    std::vector<juce::File> roots { juce::File (juce::String (destinationFolder)),
                                    juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                        .getChildFile ("RECORDINGS-MIRROR") };

    for (const auto& root : roots)
    {
        if (! root.isDirectory())
            continue;

        // One level down and newest first. A card can hold hundreds of takes and
        // this runs before the window opens, so it looks at the recent ones
        // rather than walking the whole volume -- an interrupted take is by
        // definition the most recent thing that happened.
        juce::Array<juce::File> folders;
        root.findChildFiles (folders, juce::File::findDirectories, false);

        NewestFirst comparator;
        folders.sort (comparator);

        constexpr int kMaxFoldersExamined = 20;
        const int examine = juce::jmin (folders.size(), kMaxFoldersExamined);

        for (int i = 0; i < examine; ++i)
        {
            const auto folder = folders[i];
            const auto metadataFile = folder.getChildFile ("session.json");

            if (! metadataFile.existsAsFile())
                continue;

            SessionMetadata meta;

            // A session.json truncated by the same power cut that interrupted
            // the take is not a reason to fail: the audio beside it is still
            // worth recovering, so an unreadable one is treated as interrupted.
            try
            {
                meta = SessionMetadata::fromJsonString (metadataFile.loadFileAsString().toStdString());
            }
            catch (...)
            {
                meta = {};
            }

            if (! SessionRecovery::sessionWasInterrupted (meta))
                continue;

            RecoveredSession session;
            session.folder = folder.getFullPathName().toStdString();
            session.startedIso = meta.startTimestampIso;

            for (const auto& entry : juce::RangedDirectoryIterator (folder, false, "*.wav", juce::File::findFiles))
                session.files.push_back (
                    SessionRecovery::repairWavFile (entry.getFile().getFullPathName().toStdString()));

            std::sort (session.files.begin(), session.files.end(),
                       [] (const RecoveredFile& a, const RecoveredFile& b) { return a.fileName < b.fileName; });

            // §6.6: a take where nothing survived is not presented at all --
            // better to say nothing than to hand someone an unplayable stub.
            if (session.isWorthPresenting())
                recoveredSessions.push_back (std::move (session));
        }
    }
}

juce::Array<juce::File> Application::findRecentSessionMetadata (int maximum) const
{
    juce::Array<juce::File> found;

    const juce::File root { juce::String (destinationFolder) };

    if (! root.isDirectory())
        return found;

    juce::Array<juce::File> folders;
    root.findChildFiles (folders, juce::File::findDirectories, false);

    // Newest first: a diagnostics bundle is nearly always about the take that
    // just went wrong.
    NewestFirst comparator;
    folders.sort (comparator);

    for (const auto& folder : folders)
    {
        if (found.size() >= maximum)
            break;

        const auto metadata = folder.getChildFile ("session.json");

        if (metadata.existsAsFile())
            found.add (metadata);
    }

    return found;
}

void Application::shutdown()
{
    // Other apps must not be left holding a combined device whose owner is gone.
    if (systemAggregate != nullptr)
        systemAggregate->remove();

    preflightAbort.store (true);

    if (preflightThread.joinable())
        preflightThread.join();

    // The rig as the user is leaving it, so tomorrow starts where today ended.
    saveSettings();

    // Finalised first: a half-written video file left open at quit is a file
    // the OS may never close a header on.
    cameraController.stopRecording();

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
