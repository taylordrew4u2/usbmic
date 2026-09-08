#include "Application.h"
#include "../Core/TakeCompleteness.h"
#include "../Platform/ReducedMotion.h"
#include "../Core/ClockMasterResolver.h"
#include "../Core/CombinedTakePlan.h"
#include "../Core/LoudnessMeter.h"
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
    // Before anything that could be worth recording, so every entry this
    // session shares one origin.
    appStartMs = juce::Time::getMillisecondCounterHiRes();

    // First, before anything reads a setting: the capture coordinator is built
    // with masterVolume a few lines down, and the destination is chosen below.
    loadSettings();

    // Said after loadSettings(), because that is what triggers the migration
    // and what would have come back empty if it failed.
    if (supportFolderMigrationFailed)
        noteActivity (ActivityLevel::Warning, "Settings",
                      "Couldn't move your saved settings over from the app's old name, so your "
                      "microphone names and destination may have been forgotten.");

    audioBackend = createPlatformBackend();
    virtualDeviceBackend = createDefaultVirtualDeviceBackend();
    systemAggregate = createSystemAggregateDevice();

    if (audioBackend != nullptr)
    {
        capture = std::make_unique<CaptureCoordinator> (*audioBackend, currentSampleRate,
                                                        desiredBufferSize());
        capture->getMonitorBus().setMasterVolume (masterVolume);
        captureRate = currentSampleRate;
        captureBufferSize = desiredBufferSize();

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
    else
    {
        // A build with no platform backend has no monitoring and can record
        // nothing. It used to present as an ordinary launch with an enabled
        // record button, which is the most complete silence in the app.
        noteActivity (ActivityLevel::Failed, "Sound",
                      "This build has no way to reach your sound hardware, so nothing can be "
                      "heard or recorded.");
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

    // Enumeration only. Nothing is opened here, so a rig with no camera
    // switched on turns no camera light on and spends no privacy prompt.
    //
    // A camera the user has already switched on is opened shortly afterwards by
    // the UI tick, because the main screen shows the picture beside the meters
    // and cannot show one from a closed device. That is not a new prompt:
    // switching a camera on happens behind the camera door, and the first grant
    // is spent there with the reason on screen.
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

void Application::setCameraTileScale (int step)
{
    if (step == cameraTileScale)
        return;

    cameraTileScale = step;
    saveSettings();
}

void Application::setCombineVideoAndAudio (bool shouldCombine)
{
    if (shouldCombine == combineVideoAndAudio)
        return;

    combineVideoAndAudio = shouldCombine;
    saveSettings();
}

juce::String Application::getCombineUnavailableReason()
{
    if (! combineVideoAndAudio)
        return {};

    if (takeCombiner.findFfmpeg().isNotEmpty())
        return {};

    // §10.6: named before a take rather than discovered after one. Finding out
    // that the combined file was never possible is worth knowing while there is
    // still time to install the thing, not once the recording is over.
    return "Combined video needs ffmpeg, which isn't installed. Your picture and "
           "sound will still both be recorded, as separate files. On a Mac: "
           "brew install ffmpeg.";
}

void Application::setDeliveryTarget (const juce::String& name)
{
    if (name == deliveryTarget)
        return;

    deliveryTarget = name;
    saveSettings();
}

juce::StringArray Application::getDeliveryTargetNames()
{
    juce::StringArray names;
    for (const auto& target : streamingTargets())
        names.add (juce::String (target.name));
    return names;
}

juce::String Application::getLoudnessReading() const
{
    if (capture == nullptr || capture->getLoudnessBlockCount() < kMinimumBlocksToJudge)
        return {};

    const double lufs = capture->getIntegratedLufs();

    if (lufs <= LoudnessMeter::kAbsoluteGateLufs)
        return {};

    return juce::String (lufs, 1) + " LUFS";
}

juce::String Application::getLoudnessAdvice() const
{
    if (deliveryTarget.isEmpty() || capture == nullptr)
        return {};

    const auto* target = findStreamingTarget (deliveryTarget.toStdString());

    if (target == nullptr)
        return {};

    const auto advice = adviseForTarget (*target,
                                         capture->getIntegratedLufs(),
                                         capture->getTruePeakDbtp(),
                                         capture->getLoudnessBlockCount());

    return juce::String (advice.summary);
}

void Application::openEnabledCameras()
{
    cameraController.applySelection();

    // CameraController has always set this string, and it was only ever
    // rendered by the camera panel -- which is a panel, so it is usually shut.
    // Starting a take with it shut meant "two of your cameras couldn't start
    // recording" was written to a field nobody was looking at.
    const auto problem = cameraController.getProblem();

    if (problem.isNotEmpty() && problem != reportedCameraProblem)
    {
        reportedCameraProblem = problem;
        noteActivity (ActivityLevel::Failed, "Cameras", problem);
    }
    else if (problem.isEmpty())
    {
        reportedCameraProblem.clear();
    }
}

std::vector<ChannelPlanDevice> Application::planDevices() const
{
    std::vector<ChannelPlanDevice> out;

    for (const auto& d : deviceManager.getDevices())
    {
        if (! d.included)
            continue;

        // §4: trim and the assigned name are persisted against the physical
        // port, so they follow the mic across replug rather than across slot.
        const auto persisted = portIdentityStore.get (d.identity);

        ChannelPlanDevice p;
        p.deviceKey = d.identity.key();
        p.productName = d.displayName;
        p.inputChannelCount = d.inputChannelCount;

        if (persisted.has_value())
        {
            p.assignedName = persisted->assignedName;
            p.disabledInputs = persisted->disabledInputs;
            p.inputNames = persisted->inputNames;

            // §2.4 remembers §2.1's verdict per port. Only a decision that was
            // actually made collapses a two-input device; the default of "no
            // decision yet" keeps both sides.
            p.knownDuplicateStereo = persisted->hasChannelLayoutDecision
                                  && persisted->channelLayoutIsMono;
        }

        out.push_back (std::move (p));
    }

    return out;
}

std::vector<CaptureChannel> Application::buildCaptureChannels() const
{
    // One channel per input the device presents, decided in exactly one place.
    //
    // A device with one input is one microphone. A device with several is an
    // interface with several microphones plugged into it, and each of them is a
    // person who expects their own track. Taking one channel from any device --
    // which is what this did -- silently discarded everybody but the first, and
    // if their microphone was on one of the discarded inputs they got a silent
    // recording with no explanation.
    std::vector<CaptureChannel> channels;
    int index = 0;

    for (const auto& planned : planChannels (planDevices()))
    {
        ++index;

        CaptureChannel c;
        c.deviceId = planned.deviceKey;
        c.deviceChannel = planned.deviceChannel;
        c.displayName = planned.displayName;

        // §6.2: "01_Yeti-Kitchen" -- ordinal prefix plus the sanitized name,
        // so the stems sort in channel order in any file browser.
        char prefix[4] = {};
        std::snprintf (prefix, sizeof (prefix), "%02d", index);
        c.fileName = std::string (prefix) + "_" + SessionFolderNaming::sanitizeName (c.displayName);

        for (const auto& d : deviceManager.getDevices())
            if (d.identity.key() == planned.deviceKey)
                if (const auto persisted = portIdentityStore.get (d.identity))
                    c.trimDb = persisted->trimDb;

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
    // The restart is owed, though, and the stop path pays it.
    if (capture->isRecording())
    {
        captureRestartDeferred = true;
        return;
    }

    // §2.2 can settle on a different rate once the mics are enumerated, and
    // §5.4 can move the buffer up a rung. Both are fixed at construction, so a
    // change means a new coordinator -- carrying the listening level across,
    // since the user did not ask for it to jump.
    if (audioBackend != nullptr
        && (captureRate != currentSampleRate || captureBufferSize != desiredBufferSize()))
    {
        capture->stopMonitoring();
        capture = std::make_unique<CaptureCoordinator> (*audioBackend, currentSampleRate,
                                                        desiredBufferSize());
        capture->getMonitorBus().setMasterVolume (masterVolume);

        captureRate = currentSampleRate;
        captureBufferSize = desiredBufferSize();
    }

    auto channels = buildCaptureChannels();

    if (channels.empty())
    {
        const bool wasMonitoring = capture->isMonitoring();

        capture->stopMonitoring();

        if (wasMonitoring)
        {
            noteActivity (ActivityLevel::Stopped, "Monitoring",
                          "Listening stopped -- there are no microphones switched on.");

            journalledMonitorCount = 0;
            journalledMonitorOk = false;
        }

        if (onCaptureRebuilt)
            onCaptureRebuilt();

        return;
    }

    const bool started = capture->startMonitoring (channels, selectedOutputDeviceId);

    const int micCount = static_cast<int> (channels.size());

    // Only a change is news. This runs on every rename, hot-plug and output
    // change, and an entry per call would bury everything else under a repeated
    // "2 microphones are live".
    const bool worthSaying = started != journalledMonitorOk || micCount != journalledMonitorCount;

    journalledMonitorOk = started;
    journalledMonitorCount = micCount;

    if (started)
    {
        applyClockMaster();
        driftMeasuredSeconds = 0.0;

        if (worthSaying)
            noteActivity (ActivityLevel::Started, "Monitoring",
                          juce::String (micCount)
                          + (micCount == 1 ? " microphone is live." : " microphones are live."));
    }
    else if (worthSaying)
    {
        // The sticky problem string already existed and is already shown. It is
        // recorded here as well because it is a thing that happened at a
        // moment, and the sticky string only ever says what is true now -- so a
        // monitor that failed and was then fixed left nothing behind.
        const auto why = juce::String (capture->getMonitorProblem());

        noteActivity (ActivityLevel::Failed, "Monitoring",
                      why.isNotEmpty() ? why
                                       : juce::String ("Couldn't start listening to your microphones."));
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

    // The clock master is this computer, so the aggregate is left on the
    // system's own clock rather than pinned to one microphone's crystal --
    // the same reference the in-app capture path corrects onto.
    const std::string master;

    const auto name = aggregateName.toStdString();

    // Republishing destroys the device other apps may be recording from, so it
    // happens only when something real changed.
    if (uids == publishedUids && master == publishedMaster && name == publishedNameStd)
        return;

    // The result is read, and the cache is only updated on success.
    //
    // publish() has always returned bool and the return was discarded, so a
    // failed creation cached itself as published: the guard above then matched
    // on every later call and no retry ever happened. getStatus() went on
    // saying "no microphones connected, so other apps see nothing yet", which
    // is a wrong explanation for a device that failed to be created -- the
    // user hunts for a cable while the app has already given up.
    if (! systemAggregate->publish (name, uids, master))
    {
        noteActivity (ActivityLevel::Failed, "Combined device",
                      "Couldn't make the combined device other apps record from, so they won't see "
                      "your microphones. Everything is still being recorded here.");
        return;
    }

    publishedUids = std::move (uids);
    publishedMaster = std::move (master);
    publishedNameStd = name;
}

void Application::setAggregateDeviceName (const juce::String& name)
{
    const auto trimmed = name.trim();
    aggregateName = trimmed.isEmpty() ? juce::String ("SobStage") : trimmed;
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

    // The clock master is this computer, always.
    //
    // §3.2 already corrects every microphone onto the output clock -- the
    // timebase the headphones run on, which is the machine's own. Naming one
    // microphone as "master" changed nothing about that path; it only moved
    // which crystal the drift figures were quoted against, and handed the user
    // a picker for a choice with no audible consequence. Measured against the
    // computer instead, every microphone's figure means the same thing, no
    // master can be unplugged mid-take, and there is nothing to choose.
    capture->setMasterChannel (-1);
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

    // Rates per device, kept by identity so the §2.2 vote can be taken AFTER
    // inclusion is known. Taken here, over every device the OS lists, it
    // included microphones nobody is recording -- and a MacBook's built-in mic
    // sitting at 48 kHz outvoted the interface the take actually uses, which is
    // exactly how a rig at 44.1 kHz was told to be 48.
    std::vector<EnumeratedDeviceRates> enumeratedRates;
    enumeratedRates.reserve (inputDevices.size());

    std::vector<MicDeviceState> seen;
    seen.reserve (inputDevices.size());

    int order = 0;
    for (const auto& d : inputDevices)
    {
        MicDeviceState state;
        state.identity.locationId = d.usbLocationId;
        if (! d.serialNumber.empty())
            state.identity.serial = d.serialNumber;
        state.displayName = d.name;
        state.isBuiltIn = d.isBuiltIn;
        state.inputChannelCount = std::max (1, d.maxInputChannels);

        // §2.2 prefers a rate the hardware is already on. Advertising a rate is
        // not the same as being willing to switch to it, and the switch is what
        // fails.
        enumeratedRates.push_back ({ state.identity.key(), d.supportedSampleRates, d.currentSampleRate });

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

    // §2.2: only the microphones actually being recorded get a vote. A device
    // the take does not use cannot be made to resample, cannot go out of sync,
    // and cannot be harmed by the choice -- so letting it constrain the rate
    // only ever costs the microphones that ARE being recorded.
    std::vector<std::string> includedKeys;

    for (const auto& d : deviceManager.getDevices())
        if (d.included)
            includedKeys.push_back (d.identity.key());

    auto rateCapabilities = SampleRateNegotiator::votingDevices (includedKeys, enumeratedRates);

    // Microphones are included but none of them matched what the OS listed.
    // That is a key mismatch somewhere upstream, and the honest response is to
    // let every enumerated device vote rather than let negotiate() see an empty
    // list and hand back its 48 kHz default -- which would be the very "demand
    // a rate the hardware refuses" failure this rule exists to end, arriving
    // silently through a side door.
    if (rateCapabilities.empty() && ! includedKeys.empty())
    {
        jassertfalse; // a device in the take that the OS did not list?
        int index = 0;

        for (const auto& e : enumeratedRates)
        {
            DeviceRateCapability cap;
            cap.deviceIndex = index++;
            cap.supportedRates = e.supportedRates;
            cap.currentRate = e.currentRate;
            rateCapabilities.push_back (std::move (cap));
        }
    }

    // The rate the rig is already on where they agree, else highest common,
    // capped at 48kHz. Never rejects a device.
    auto rateResult = SampleRateNegotiator::negotiate (rateCapabilities);

    // What Settings can offer: every rate any recorded microphone reports,
    // plus whatever each is running at now. Like Audio MIDI Setup, the whole
    // list, not a pre-filtered one -- the user is choosing for their hardware,
    // and a device that cannot follow is resampled by §3 or, if it refuses to
    // open, says so on the main screen by name.
    {
        std::set<uint32_t> offered;

        for (const auto& c : rateCapabilities)
        {
            for (auto rate : c.supportedRates)
                offered.insert (rate);

            if (c.currentRate != 0)
                offered.insert (c.currentRate);
        }

        availableSampleRates.assign (offered.begin(), offered.end());
    }

    // A pinned rate is honoured, full stop. The earlier rule quietly dropped a
    // pin the rig "could not reach" and fell back to automatic, which from the
    // user's side is a control that does nothing. §0.1's spirit: if the choice
    // cannot be met, say so -- the open path names the device and both rates.
    currentSampleRate = sampleRateOverride != 0 ? sampleRateOverride : rateResult.chosenRate;

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
        // Present means enumerated -- not `included`, which the Settings tick
        // box also clears. Unticking a mic mid-take used to read as an unplug
        // on the next device notification and silence that person's channel.
        std::set<std::string> present;
        for (const auto& d : deviceManager.getDevices())
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

                noteActivity (ActivityLevel::Failed, juce::String (ch.displayName),
                              "Unplugged mid-take. Its track keeps its place and is being written "
                              "as silence.");
            }
            else if (live && recordingEngine.isWritingSilence (ch.deviceId))
            {
                recordingEngine.onMicReconnected (ch.deviceId);
                midTakeDropouts.push_back ({ getElapsedRecordingSeconds(), ch.deviceId,
                                             "Microphone reconnected: its channel is live again." });

                noteActivity (ActivityLevel::Recovered, juce::String (ch.displayName),
                              "Plugged back in. Its track is live again.");
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

            // The line lasts twelve seconds. Someone who looked away for
            // fifteen used to have no way of learning that the mic they just
            // plugged in is not in the take.
            noteActivity (ActivityLevel::Warning, juce::String (d.displayName),
                          "Added to monitoring only. It'll be recorded starting with your next take.");
            break;
        }

        // §6.5 "clock master unplugged": failover per §3.3, without touching
        // the channel set.
        //
        // applyClockMaster() already resolves against the take's frozen channel
        // list and skips a candidate that is writing silence, so calling it
        // here is the whole of the failover. It runs on the status poll too;
        // doing it on the device-list notification as well is what makes the
        // switch happen when the microphone actually leaves rather than up to a
        // poll later.
        //
        // Only the reference moves. Every channel is corrected onto the output
        // stream's clock (§3.2), master included, so no channel's resampling
        // changes and the file layout is untouched (§6.5). What the move buys is
        // §3.3's figures: they are quoted relative to the master, and a master
        // that has gone silent stops updating, so leaving it there would quote
        // every surviving microphone against a frozen number.
        applyClockMaster();
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

ProofReading Application::snapshotProof() const
{
    ProofReading reading;
    reading.elapsedSeconds = getElapsedRecordingSeconds();

    if (capture != nullptr)
    {
        reading.framesAccepted = capture->getFramesAccepted();
        reading.peakArrived = capture->getPeakArrived();
    }

    if (currentSessionFolder.isNotEmpty())
        for (const auto& file : listSessionFiles (currentSessionFolder))
            reading.bytesOnDisk += static_cast<uint64_t> (std::max<int64_t> (0, file.sizeBytes));

    return reading;
}

TakeHealth Application::snapshotTakeHealth() const
{
    TakeHealth health;

    const int micCount = getIncludedMicCount();
    for (int i = 0; i < micCount; ++i)
        health.mics.push_back ({ getMicDisplayName (i).toStdString(), isMicLive (i) });

    // Only the cameras switched on are in the take, and a camera the OS has
    // stopped listing is simply absent from this list -- the watchdog treats
    // a name that was here and now is not as gone.
    const auto& cameras = cameraController.getSelection();
    for (const auto& camera : cameras.getAvailableCameras())
        if (cameras.isEnabled (camera.id))
            health.cameras.push_back ({ cameras.getDisplayName (camera.id), true });

    health.cameraProblem = cameraController.getProblem().toStdString();
    health.monitorProblem = getMonitorProblem().toStdString();

    if (capture != nullptr)
    {
        health.framesDropped = capture->getFramesDropped();
        health.samplesOverrun = capture->getOverrunSamples();
        health.outputClockLost = capture->isOutputClockLost();
        health.writerBehind = capture->getRingFillFraction() >= CapacityMonitor::kFillWarningFraction;
        health.mixOnly = capture->isMixOnly();
    }

    health.remainingSeconds = getRemainingRecordingSeconds();
    health.elapsedSeconds = getElapsedRecordingSeconds();
    return health;
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

    // Negative is "could not be determined", which is not the same as none
    // left -- it used to read as Exhausted and announce a full drive.
    const auto remaining = getRemainingRecordingSeconds();
    if (remaining < 0.0)
        return RemainingTimeWarning::None;

    return capacityMonitor.evaluateRemaining (remaining);
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

juce::String Application::nameForChannel (const std::string& identityKey, int deviceChannel) const
{
    for (const auto& d : deviceManager.getDevices())
    {
        if (d.identity.key() != identityKey)
            continue;

        const auto persisted = portIdentityStore.get (d.identity);

        // The name the user gave this port wins over the product string --
        // otherwise the skull says "Blue Yeti" while the files say "Kitchen".
        std::string base = d.displayName;
        bool knownDuplicateStereo = false;

        if (persisted.has_value())
        {
            // A name given to this particular input is who is on it, and
            // needs no socket number after it.
            const auto named = persisted->inputNames.find (deviceChannel);
            if (named != persisted->inputNames.end() && ! named->second.empty())
                return juce::String (named->second);

            if (! persisted->assignedName.empty())
                base = persisted->assignedName;

            knownDuplicateStereo = persisted->hasChannelLayoutDecision
                                && persisted->channelLayoutIsMono;
        }

        const int inputs = takeChannelsForDevice (d.inputChannelCount, knownDuplicateStereo);

        return juce::String (plannedChannelName (base, deviceChannel, inputs));
    }

    return {};
}

juce::String Application::getMicProductName (int index) const
{
    // §14.6: with four identical microphones on a desk, the name the user gave
    // a port is what identifies it -- but the hardware's own name is what tells
    // them which *kind* of thing it is.
    //
    // Resolved in channel space, not device space, for the same reason
    // getMicDisplayName is: an interface contributes several channels, and
    // after a mid-take unplug the two lists are different lengths.
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
        const auto plan = planChannels (planDevices());

        if (index < 0 || index >= static_cast<int> (plan.size()))
            return {};

        key = plan[static_cast<size_t> (index)].deviceKey;
        fallback = juce::String (plan[static_cast<size_t> (index)].displayName);
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
        const auto live = nameForChannel (ch.deviceId, ch.deviceChannel);

        // Empty means the device is no longer enumerated -- it was unplugged
        // mid-take. The name the channel opened with is the honest answer.
        return live.isNotEmpty() ? live : juce::String (ch.displayName);
    }

    // Outside a take, the same plan the take would use. Walking the device list
    // instead is what made a two-input interface show one microphone until the
    // moment recording began -- which reads as an app that cannot see the
    // second microphone at all, and was reported as exactly that.
    const auto plan = planChannels (planDevices());

    if (index < 0 || index >= static_cast<int> (plan.size()))
        return {};

    return juce::String (plan[static_cast<size_t> (index)].displayName);
}

void Application::setMicAssignedName (int index, const juce::String& name)
{
    // `index` is a strip, and a strip is one INPUT of a device -- resolved in
    // channel space, like every other strip accessor, rather than by walking
    // devices. Walking devices named the wrong port on any interface, and
    // could not name one input of it at all.
    std::string deviceKey;
    int deviceChannel = 0;

    if (capture != nullptr && capture->isRecording())
    {
        const auto& channels = capture->getChannels();
        if (index < 0 || index >= static_cast<int> (channels.size()))
            return;
        deviceKey = channels[static_cast<size_t> (index)].deviceId;
        deviceChannel = channels[static_cast<size_t> (index)].deviceChannel;
    }
    else
    {
        const auto plan = planChannels (planDevices());
        if (index < 0 || index >= static_cast<int> (plan.size()))
            return;
        deviceKey = plan[static_cast<size_t> (index)].deviceKey;
        deviceChannel = plan[static_cast<size_t> (index)].deviceChannel;
    }

    for (const auto& d : deviceManager.getDevices())
    {
        if (d.identity.key() != deviceKey)
            continue;

        auto settings = portIdentityStore.get (d.identity).value_or (PersistedDeviceSettings{});
        const auto clean = SessionFolderNaming::sanitizeName (name.toStdString());

        // One microphone is one box, so the box takes the name. On an interface
        // each socket is a person, so the socket does -- and the box's own name
        // is left alone for the other inputs.
        const bool knownDuplicateStereo = settings.hasChannelLayoutDecision && settings.channelLayoutIsMono;
        if (takeChannelsForDevice (d.inputChannelCount, knownDuplicateStereo) > 1)
        {
            if (clean.empty()) settings.inputNames.erase (deviceChannel);
            else               settings.inputNames[deviceChannel] = clean;
        }
        else
        {
            settings.assignedName = clean;
        }

        portIdentityStore.put (d.identity, settings);

        saveSettings();

        // The capture channels carry the display name into the stem filenames
        // (§6.2), so they are rebuilt -- but never mid-take, where §6.5 fixes
        // the channel list for the duration of the recording.
        requestCaptureRestart();

        return;
    }
}

void Application::requestCaptureRestart()
{
    if (capture != nullptr && capture->isRecording())
    {
        captureRestartDeferred = true;
        return;
    }

    restartCapture();
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
    {
        setDestinationFolder (target);

        noteActivity (ActivityLevel::Started, "Save location",
                      "Takes will be saved to " + target.getFullPathName() + ".");
        return;
    }

    // The click used to do nothing and say nothing: the old destination stayed,
    // the setting looked unchanged, and a card that is read-only or was pulled
    // between listing and choosing was indistinguishable from a misclick. The
    // user then recorded to somewhere they believed they had changed.
    noteActivity (ActivityLevel::Failed, "Save location",
                  "Couldn't save to " + target.getFullPathName()
                  + ", so takes are still going to " + juce::String (destinationFolder)
                  + ". Check the card is plugged in and isn't locked.");
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

            // §6.3: the mirror decision is taken HERE, before the folders are
            // made, against the free space now. It used to be evaluated after
            // the mirror folder had already been decided from the previous
            // take's state, so the first take of every launch had no backup
            // and later takes inherited the verdict of the one before.
            capacityMonitor.reset();
            mirrorPolicy.reset();
            {
                const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
                mirrorPolicy.evaluateAtArm (home.getBytesFreeOnVolume(), projectedSessionBytes());
            }

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

                    // And the user is told. This path used to return here in
                    // silence: the button un-latched, no message appeared
                    // anywhere, and the only difference between "the card is
                    // full" and "the app is broken" was that neither was said.
                    recordStartProblem = folder.isEmpty()
                        ? juce::String ("Couldn't make a folder for this take. Check the card is "
                                        "plugged in, has room, and isn't locked.")
                        : juce::String (capture->getRecordingProblem());

                    if (recordStartProblem.isEmpty())
                        recordStartProblem = "Couldn't start recording. Check the card is plugged "
                                             "in, has room, and isn't locked.";

                    noteActivity (ActivityLevel::Failed, "Recording", recordStartProblem);
                    return;
                }

                // Cleared only once a take is genuinely under way, so the
                // reason for the last failure stays on screen until it is
                // replaced by a success rather than by the next click.
                recordStartProblem.clear();
                mirrorMissingReported = false;
                backendDropsAtTakeStart = audioBackend != nullptr
                                              ? audioBackend->getFramesDroppedByBackend() : 0;

                currentSessionFolder = folder;
                currentMirrorFolder = mirror;
                sessionStartIso = now.toISO8601 (true);

                // The picture starts with the sound, into the same folder. A
                // camera the user switched on but never looked at is opened
                // here rather than being quietly left out of the take.
                openEnabledCameras();

                // recordingStartMs is the audio take's t=0. Handing it over is
                // what lets each camera record how far into the take its own
                // first frame lands -- the one number the combining step
                // cannot work out afterwards, since a camera file carries no
                // timestamp tying it to the session.
                cameraController.startRecording (juce::File (folder), recordingStartMs);

                // Same field, the other moment it is set. A camera that failed
                // to start recording is not in the take, and the take goes on
                // regardless -- so if this is not said now it is never said.
                // Only when it has changed. getProblem() now carries the
                // open-time failure as well, and that one persists across
                // takes -- so re-reading it at every take start re-logged the
                // same "Couldn't open X" for every take of the session.
                if (const auto cameraProblem = cameraController.getProblem();
                    cameraProblem.isNotEmpty() && cameraProblem != reportedCameraProblem)
                {
                    reportedCameraProblem = cameraProblem;
                    noteActivity (ActivityLevel::Failed, "Cameras", cameraProblem);
                }

                // §6.2: session.json is written at the start so a crash mid-take
                // still leaves a record of what the rig was, and rewritten on
                // stop to add the stop time and everything logged since.
                writeSessionMetadata (false);

                noteActivity (ActivityLevel::Started, "Recording",
                              "Recording started into " + juce::File (folder).getFileName()
                              + (mirror.isNotEmpty() ? ", with a backup copy." : "."));
            }

            // Each take gets its own warnings; a previous one must not leave the
            // ten-minute warning already spent.
            mirrorActiveAtStop = -1;
            midTakeDropouts.clear();
            midTakeNotice.clear();
            midTakeNoticeSeconds = 0.0;

            // §5.4: buffer size is fixed for the duration of a take.
            bufferLadder.setRecording (true);
        }
    }
    else
    {
        // Before stopRecording(), which moves the pipeline out and destroys it.
        // Read after, this is always -1 and no take could ever be reported as
        // silent -- the measurement has to be taken while the thing that made
        // it still exists.
        const float takePeak = capture != nullptr ? capture->getPeakWritten() : -1.0f;
        const float arrivedPeak = capture != nullptr ? capture->getPeakArrived() : -1.0f;
        mirrorActiveAtStop = (capture != nullptr && capture->isMirroring()) ? 1 : 0;

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

        // The combined file, if it was asked for. Started only once every input
        // is closed and complete, and run on its own thread: copying a
        // four-hour picture is minutes of work, and none of it may happen on
        // the thread drawing the meters.
        //
        // Nothing here can cost anyone the take. The inputs are finished files
        // that this only reads, and a failure leaves the folder exactly as it
        // was -- separate, complete, and playable.
        if (combineVideoAndAudio && currentSessionFolder.isNotEmpty())
        {
            // The take's own bit depth goes with it, so the combined file's
            // audio is written at the depth it was recorded at rather than
            // being quietly narrowed on the way out.
            const auto plan = buildCombinedTakePlan (CombinedVideoMode::Combined,
                                                     cameraController.getCombinedTakeInputs(),
                                                     "MIX.wav",
                                                     currentBitDepth);

            if (plan.hasWork())
                takeCombiner.start (juce::File (currentSessionFolder), plan);
        }

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

            lastTakeVerdict = judgeTakeAudio (written, takePeak, arrivedPeak);

            // Both failures mean the same thing to anyone deciding whether to
            // record it again: there is no audio in that folder.
            lastTakeHeldNoAudio = lastTakeVerdict == TakeAudioVerdict::NothingWritten
                               || lastTakeVerdict == TakeAudioVerdict::OnlySilence
                               || lastTakeVerdict == TakeAudioVerdict::DroppedByApp;
        }

        // §6.2: the take is on disk and the UI has not shown where yet. Only
        // raised when a folder was actually opened -- a start that failed
        // preflight never got one, and "saved" would be a lie.
        savedTakePending = currentSessionFolder.isNotEmpty();

        // The stop goes in the record with its outcome attached. A take that
        // ended is a thing that happened, and whether it holds audio is the one
        // fact the user most needs carried past the ten seconds the notice
        // lasts -- particularly the empty ones, which look identical on disk to
        // a folder nobody has opened yet.
        noteActivity (lastTakeHeldNoAudio ? ActivityLevel::Failed : ActivityLevel::Stopped,
                      "Recording",
                      lastTakeHeldNoAudio
                          ? juce::String ("Recording stopped, but there is no audio in "
                                          + juce::File (lastSessionFolder).getFileName()
                                          + ". Check your microphones aren't muted.")
                          : juce::String ("Recording stopped. Saved to "
                                          + juce::File (lastSessionFolder).getFileName() + "."));

        recordingEngine.stop();
        recordingStartMs = 0.0;
        bufferLadder.setRecording (false);

        currentSessionFolder.clear();
        currentMirrorFolder.clear();

        // Everything refused during the take -- a mic plugged in, an unplug,
        // a rename, a rate change -- is applied now, so the next take's plan
        // and its streams agree.
        if (captureRestartDeferred)
        {
            captureRestartDeferred = false;
            restartCapture();
        }
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
    {
        noteActivity (ActivityLevel::Failed, "Recording",
                      "Couldn't make a folder at " + folder.getFullPathName()
                      + ". Check the card is plugged in, has room, and isn't locked.");
        return {};
    }

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
    out.verdict = lastTakeVerdict;

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

    // Outside a take, the count the take WOULD produce. Counting included
    // devices instead under-counted every interface: a two-input interface
    // showed one strip and reserved disk for one track, then recorded two.
    // §6.4's remaining-time figure was wrong by the input multiplier -- double
    // on a 2-input box, quadruple on a 4-input one -- which is a promise of
    // recording time the disk cannot keep.
    int count = 0;
    for (const auto& d : planDevices())
        count += takeChannelsForDevice (d.inputChannelCount, d.knownDuplicateStereo);

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

    // The microphones have to be OPEN, not merely plugged in. A rig whose
    // streams failed to open -- the output refused low-latency mode, a mic
    // held by another app -- used to leave this button live, and pressing it
    // produced a take of empty files with the clock running. Now the button
    // says why it is off, in the words the streams gave.
    if (capture == nullptr || ! capture->isMonitoring())
    {
        const auto problem = capture != nullptr ? capture->getMonitorProblem() : std::string();
        return problem.empty() ? juce::String ("The microphones aren't open yet.")
                               : "The microphones aren't open: " + juce::String (problem);
    }

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

    // A card that will not take the test file at all is not a slow card, and
    // must not be reported as one. With no windows measured the gate below
    // reads 0 MB/s and says "this card is too slow", which sends someone
    // shopping for a faster card when the card is read-only, full, or gone.
    bool couldNotWrite = false;

    {
        // §6.4: 200 MB, written the way a take writes -- steadily, measuring the
        // sustained floor rather than a burst into the OS cache.
        juce::FileOutputStream out (testFile);

        if (! out.openedOk())
        {
            couldNotWrite = true;
        }
        else
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
                {
                    // Stopped taking writes part way. Whatever windows were
                    // measured before that describe a card that is no longer
                    // accepting audio, so they are not a verdict either.
                    couldNotWrite = true;
                    break;
                }

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

            // FileOutputStream::flush() returns void; the stream carries the
            // outcome instead. A flush that failed means the bytes counted as
            // written above never reached the card, so the windows measured
            // from them describe nothing.
            if (out.getStatus().failed())
                couldNotWrite = true;

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

        // Overridden rather than measured: this is not a speed verdict, and
        // saying so is the difference between someone checking the lock switch
        // and someone buying a card they did not need.
        if (couldNotWrite)
        {
            result.passed = false;
            result.reason = "Couldn't write to this card, so takes can't be saved here. Check it "
                             "is plugged in, has room, and isn't locked.";

            noteActivity (ActivityLevel::Failed, "Save location",
                          juce::String (result.reason));
        }

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

    // `index` is a strip, resolved in channel space like every other strip
    // accessor. Walking included devices put the trim on the wrong device on
    // any interface with more than one socket, and the slider snapped back.
    const auto deviceKey = deviceKeyForStrip (index);

    for (const auto& d : deviceManager.getDevices())
    {
        if (d.identity.key() != deviceKey)
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
    const auto deviceKey = deviceKeyForStrip (index);

    for (const auto& d : deviceManager.getDevices())
    {
        if (d.identity.key() != deviceKey)
            continue;

        if (const auto settings = portIdentityStore.get (d.identity))
            return settings->trimDb;

        return 0.0f;
    }

    return 0.0f;
}

std::string Application::deviceKeyForStrip (int index) const
{
    // Mid-take the channel list is the take's frozen one; otherwise the plan.
    if (capture != nullptr && capture->isRecording())
    {
        const auto& channels = capture->getChannels();
        return index >= 0 && index < static_cast<int> (channels.size())
             ? channels[static_cast<size_t> (index)].deviceId : std::string();
    }

    const auto plan = planChannels (planDevices());
    return index >= 0 && index < static_cast<int> (plan.size())
         ? plan[static_cast<size_t> (index)].deviceKey : std::string();
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
    {
        const auto persisted = portIdentityStore.get (d.identity);
        const bool knownDuplicateStereo = persisted.has_value()
                                       && persisted->hasChannelLayoutDecision
                                       && persisted->channelLayoutIsMono;

        MicSelection m;
        m.displayName = juce::String (d.displayName);
        m.enabled = d.userEnabled;
        m.isBuiltIn = d.isBuiltIn;
        m.channelCount = takeChannelsForDevice (d.inputChannelCount, knownDuplicateStereo);

        // One row per socket on an interface, each switchable and each showing
        // the name its person has been given, so the list reads as who is
        // being recorded rather than which boxes are plugged in.
        if (m.channelCount > 1)
        {
            for (int input = 0; input < m.channelCount; ++input)
            {
                MicSelection::Input in;
                in.index = input;
                in.label = nameForChannel (d.identity.key(), input);
                in.enabled = ! persisted.has_value()
                          || std::find (persisted->disabledInputs.begin(),
                                        persisted->disabledInputs.end(), input)
                             == persisted->disabledInputs.end();
                m.inputs.push_back (std::move (in));
            }
        }

        out.push_back (std::move (m));
    }

    return out;
}

void Application::setInputEnabled (const juce::String& displayName, int input, bool enabled)
{
    for (const auto& d : deviceManager.getDevices())
    {
        if (juce::String (d.displayName) != displayName)
            continue;

        auto settings = portIdentityStore.get (d.identity).value_or (PersistedDeviceSettings{});
        auto& off = settings.disabledInputs;
        const auto it = std::find (off.begin(), off.end(), input);
        const bool currentlyOff = it != off.end();

        if (enabled == ! currentlyOff)
            return;

        if (enabled) off.erase (it);
        else         off.push_back (input);

        portIdentityStore.put (d.identity, settings);
        saveSettings();

        // The channel set changed, so the streams are reopened -- never
        // mid-take, where §6.5 fixes the channel list for the recording.
        requestCaptureRestart();

        return;
    }
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

juce::String Application::createMirrorFolder (const juce::String& sessionFolderName) const
{
    // §6.3: the mirror lives on the internal drive, which is the whole point --
    // a card failure must not take both copies with it.
    const auto mirrorRoot = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                .getChildFile ("RECORDINGS-MIRROR");
    auto root = mirrorRoot.getChildFile (sessionFolderName);

    // §6.2 never overwrites, and that holds for the copy too. Collisions were
    // resolved against the card only, so a take restarted on a new card with
    // the same name -- the card-removal flow exactly -- reopened the mirror
    // folder it had just promised was safe and truncated every file in it.
    for (int attempt = 2; root.exists() && attempt < 1000; ++attempt)
        root = mirrorRoot.getChildFile (sessionFolderName + "_" + juce::String (attempt));

    if (root.exists() || ! root.createDirectory().wasOk())
    {
        // The caller degrades to card-only, which is right (§6.3). What it
        // could not do was tell anyone, because an empty string is not a
        // reason. pollStatusAdvice catches the resulting state and says so;
        // this puts the cause in the record.
        noteActivity (ActivityLevel::Failed, "Local backup",
                      "Couldn't make the backup folder at " + root.getFullPathName()
                      + ". This take is going to the card only.");
        return {};
    }

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
    meta.mirrorActive = sessionHasStopped && mirrorActiveAtStop >= 0
                            ? mirrorActiveAtStop == 1
                            : (capture != nullptr && capture->isMirroring());
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
                                   mirrorMissingReported
                                       ? std::string ("Local backup copy was never made: its folder or "
                                                      "files could not be opened. There is no second "
                                                      "copy of this take.")
                                       : std::string ("Local backup copy stopped: that drive stopped "
                                                      "accepting writes. The copy is incomplete from "
                                                      "this point.") });

    if (capture != nullptr)
    {
        // Loaded once, like the layout figure below. Read twice, the number
        // reported can differ from the one that passed the test.
        const auto dropped = capture->getFramesDropped();

        if (dropped > 0)
            meta.dropouts.push_back ({ getElapsedRecordingSeconds(), std::string(),
                                       "Dropped " + std::to_string (dropped)
                                           + " frames: the drive could not keep up." });
    }

    // The other end of the same loss. The count above is what the writer could
    // not put on disk; this is what never reached it -- audio the device handed
    // over and the backend could not carry. A take missing both kinds used to
    // report only one, so the record understated what was lost.
    // §0.1: audio the device delivered that did not fit the take's layout, so
    // a channel wrote silence instead. Recorded beside the other two losses.
    if (capture != nullptr)
    {
        // Loaded once. Read twice, the number reported could differ from the
        // one that passed the test above it.
        const auto missed = capture->getFramesMissedByLayout();

        if (missed > 0)
            meta.dropouts.push_back ({ getElapsedRecordingSeconds(), std::string(),
                                       "Dropped " + std::to_string (missed)
                                           + " frames that didn't fit this take's channel layout: a "
                                             "microphone delivered a different number of channels "
                                             "than it was opened with." });
    }

    // Measured from the start of THIS take. The backend's counter runs for as
    // long as its streams do, which is across takes, so writing it raw put
    // take one's losses into take three's record -- beside a card-side figure
    // on a completely different clock.
    if (audioBackend != nullptr)
    {
        const auto total = audioBackend->getFramesDroppedByBackend();

        // A total below the baseline means the streams were rebuilt mid-take
        // and the count restarted; everything since is then this take's.
        const auto thisTake = total >= backendDropsAtTakeStart ? total - backendDropsAtTakeStart : total;

        if (thisTake > 0)
            meta.dropouts.push_back ({ getElapsedRecordingSeconds(), std::string(),
                                       "Dropped " + std::to_string (thisTake)
                                           + " frames before recording: the sound hardware delivered "
                                             "more audio than it could hand over." });
    }

    // Written to the card copy and the mirror alike, so either one stands alone.
    const auto json = meta.toJsonString();

    // The result is checked. session.json is the durable record of everything
    // above -- every dropout, every buffer change, why the backup stopped --
    // and it was written with the return value discarded, so the one file that
    // explains a difficult take could fail to appear and nothing would say so.
    if (! juce::File (currentSessionFolder).getChildFile ("session.json").replaceWithText (juce::String (json)))
        noteActivity (ActivityLevel::Warning, "Recording",
                      "Couldn't write the details file for this take. The audio itself is saved.");

    // The mirror's copy is checked too. §6.3 makes each copy stand alone, and a
    // backup whose record failed to write is a folder of audio with no account
    // of how the take went -- which is the half of the pair the user reaches
    // for precisely when the card's copy is the one that went wrong.
    if (currentMirrorFolder.isNotEmpty()
        && ! juce::File (currentMirrorFolder).getChildFile ("session.json").replaceWithText (juce::String (json)))
        noteActivity (ActivityLevel::Warning, "Local backup",
                      "Couldn't write the details file into the backup copy. The backed-up audio "
                      "itself is there.");

    writeActivityLog (juce::File (currentSessionFolder));

    if (currentMirrorFolder.isNotEmpty())
        writeActivityLog (juce::File (currentMirrorFolder));
}

void Application::writeActivityLog (const juce::File& folder)
{
    // Plain text beside the take, because this one is for the person and not
    // for a parser: it is what they show someone else when a take went wrong.
    // A take that carries its own history can be explained months later; one
    // that does not is a folder of audio and a memory of something going amber.
    juce::String text;
    text << "SobStage -- what happened during this session" << juce::newLine << juce::newLine;

    // Said, rather than left as a log that mysteriously starts part way through.
    if (const auto dropped = activity.getDroppedCount(); dropped > 0)
        text << "(" << juce::String (static_cast<int> (dropped))
             << " earlier entries are not listed -- this log keeps the most recent "
             << juce::String (static_cast<int> (ActivityJournal::kMaxEntries)) << ".)"
             << juce::newLine << juce::newLine;

    // Oldest first: a log is read forwards.
    auto lines = getRecentActivityLines (static_cast<int> (ActivityJournal::kMaxEntries));

    for (int i = lines.size(); --i >= 0;)
        text << lines[i] << juce::newLine;

    // Checked like everything else here. A log that failed to write is exactly
    // the sort of thing this file exists to stop happening quietly.
    if (! folder.getChildFile ("activity.log").replaceWithText (text))
        noteActivity (ActivityLevel::Warning, "Recording",
                      "Couldn't write the activity log into " + folder.getFileName()
                      + ". The audio itself is saved.");
}

double Application::activityClockSeconds() const
{
    if (appStartMs <= 0.0)
        return 0.0;

    return (juce::Time::getMillisecondCounterHiRes() - appStartMs) / 1000.0;
}

void Application::noteActivity (ActivityLevel level, const juce::String& subject,
                                const juce::String& message) const
{
    activity.note (activityClockSeconds(), level, subject.toStdString(), message.toStdString());
}

juce::StringArray Application::getRecentActivityLines (int limit) const
{
    juce::StringArray lines;

    for (const auto& e : activity.getEntries())
    {
        if (lines.size() >= limit)
            break;

        // Minutes and seconds on the app's clock. A user comparing this against
        // a take needs an ordering and a rough distance, not a wall clock.
        const int totalSeconds = static_cast<int> (e.atSeconds);
        auto stamp = juce::String::formatted ("%d:%02d", totalSeconds / 60, totalSeconds % 60);

        auto line = stamp + "  " + juce::String (e.subject) + " -- " + juce::String (e.message);

        if (e.repeats > 1)
            line += " (x" + juce::String (e.repeats) + ")";

        lines.add (line);
    }

    return lines;
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

    // §14.4, "the sleeper failure": several microphones in omni or stereo in one
    // room produce heavy bleed, unusable stems and comb filtering, and the
    // detector for it has never been fed -- so the one piece of advice that
    // names the fix ("turn its pattern knob to the single-heart setting") could
    // not fire. Correlation is a sample-level quantity, so it comes from the
    // capture path rather than from the per-channel peaks above.
    if (capture != nullptr)
        setupAdvisor.updatePolarPattern (capture->getPolarPairCorrelation(),
                                         capture->getPolarThirdChannelPeakDb(),
                                         sinceLastCallSeconds);

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
        //
        // Recorded rather than returned. This used to return, which put it
        // ahead of the card-removal check below -- the branch that actually
        // stops the take -- so a card failing at the same moment as a drift
        // warning kept recording into a dead handle for another poll. The
        // journal carries it, and the advice line picks it up once nothing
        // more urgent is holding the line.
        for (int i = 0; i < micCount; ++i)
        {
            if (! capture->hasSustainedExcessDrift (i))
                continue;

            noteActivity (ActivityLevel::Warning, getMicDisplayName (i),
                          juce::String (getMicDisplayName (i))
                          + " can't keep steady time with the others. Try a different USB port.");
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

        // The writer's own account when it has one -- a roll-over past 3.9 GB
        // that could not create the next file is not "the card was removed",
        // and sending someone to check a cable for a card that is simply full
        // wastes the one moment they are still next to the rig.
        if (const auto why = capture->getCardWriteProblem(); ! why.empty())
            noteActivity (ActivityLevel::Failed, "Card", juce::String (why));

        noteActivity (ActivityLevel::Failed, "Card", juce::String (cardRemovalNotice.message));
        return juce::String (cardRemovalNotice.message);
    }

    // §0.1: a stream that opened and has since stopped. Taken here, on the
    // message thread, from wherever the backend's worker thread left it.
    //
    // Every backend has a loop that gives up on a dead device and exits, and
    // none of them used to tell anyone: monitoring went quiet, or a
    // microphone's track went on being written as silence for the rest of the
    // take, and the only evidence was in the file afterwards.
    if (audioBackend != nullptr)
    {
        juce::String firstFailure;

        for (const auto& failure : audioBackend->takeStreamFailures())
        {
            // The backend knows an id; the user knows a name. A backend that
            // has a better subject than either -- a report that is not about
            // one device -- supplies it.
            auto subject = failure.subject.empty() ? juce::String ("Your headphones")
                                                   : juce::String (failure.subject);

            if (failure.subject.empty() && ! failure.deviceId.empty())
            {
                subject = juce::String (failure.deviceId);

                for (const auto& d : deviceManager.getDevices())
                    if (d.identity.key() == failure.deviceId)
                        subject = juce::String (d.displayName);
            }

            const auto line = subject + " " + juce::String (failure.reason);

            noteActivity (ActivityLevel::Failed, subject, line);

            if (firstFailure.isEmpty())
                firstFailure = line;
        }

        if (firstFailure.isNotEmpty())
            return firstFailure;
    }

    // §0.1: audio lost between the device and the app. Counted by the backends
    // and, until now, reported by nobody -- the take's own dropped-frame figure
    // only ever covered what the writer could not put on disk.
    if (audioBackend != nullptr)
    {
        const auto droppedNow = audioBackend->getFramesDroppedByBackend();

        // The counters live in the stream objects, and every rename, hot-plug
        // or output change tears those down and builds new ones -- so this
        // total goes back to zero regularly. Compared against a high-water mark
        // that never reset, one noisy USB port early in a session silently
        // blinded the report for the rest of it: every later loss sat below the
        // old mark and was never mentioned again.
        if (droppedNow < reportedBackendDrops)
            reportedBackendDrops = 0;

        if (droppedNow > reportedBackendDrops)
        {
            reportedBackendDrops = droppedNow;

            const auto line = juce::String ("Your sound hardware is delivering more audio than it "
                                            "can hand over, and some has been lost. Try a different "
                                            "USB port, and close other apps using audio.");

            noteActivity (ActivityLevel::Warning, "Sound hardware", line);
            return line;
        }
    }

    // The monitor path breaking up. Not lost recording -- §6.1 keeps the two
    // apart, and the take is unaffected -- but a machine that cannot keep the
    // headphones fed is one that will start losing the recording next, and the
    // user hearing clicks deserves to know it is the machine rather than a
    // microphone. Reported on a rising count, once per rise.
    if (audioBackend != nullptr)
    {
        const auto glitchesNow = audioBackend->getOutputGlitchCount();

        if (glitchesNow < reportedOutputGlitches)
            reportedOutputGlitches = 0;

        if (glitchesNow > reportedOutputGlitches)
        {
            reportedOutputGlitches = glitchesNow;

            const auto line = juce::String ("The sound you're hearing is breaking up -- this "
                                            "machine is struggling to keep up. The recording "
                                            "itself is unaffected. Close other apps.");

            noteActivity (ActivityLevel::Warning, "Monitoring", line);
            return line;
        }
    }

    // A take that could not start. Reported here rather than only at the click,
    // because the click's own moment passes and this line is where the user
    // looks. Held until the next take starts, not until the next poll.
    if (recordStartProblem.isNotEmpty() && recordingEngine.getState() == RecordingState::Idle)
        return recordStartProblem;

    // §6.3: a mirror was asked for and this take does not have one -- either
    // its folder could not be made, or its files could not be opened. Both used
    // to leave the policy saying "Active" over a backup that did not exist, so
    // both are caught here, by comparing what was asked for against what is
    // actually being written. Said once per take.
    // Asks the pipeline directly as well as inferring it from the policy. The
    // inference alone was the whole test, which made the pipeline's own flag
    // dead code and left the report resting on a proxy: it only fires while a
    // take is running AND the policy still says Active, so a mirror that failed
    // to open in a take the policy had already given up on said nothing.
    if (capture != nullptr && capture->isRecording() && ! mirrorMissingReported
        && (capture->hasMirrorFailedToOpen()
            || (mirrorPolicy.isMirroring() && ! capture->isMirroring())))
    {
        mirrorMissingReported = true;

        // The policy is caught up too, so isMirroring() and session.json stop
        // claiming a second copy that does not exist.
        mirrorPolicy.noteWriteFailure();

        const auto line = juce::String ("There is no backup copy of this take -- that folder "
                                        "couldn't be opened. The recording itself is fine and is "
                                        "going to the card.");

        noteActivity (ActivityLevel::Failed, "Local backup", line);
        return line;
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
        const auto line = juce::String ("The local backup copy stopped -- that drive stopped "
                                        "accepting writes. The recording itself is unaffected and "
                                        "is still going to the card.");

        noteActivity (ActivityLevel::Failed, "Local backup", line);
        return line;
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

            const auto line = juce::String ("The local backup copy stopped -- this drive is low on "
                                            "space. The recording itself is unaffected.");

            noteActivity (ActivityLevel::Warning, "Local backup", line);
            return line;
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

                {
                    const auto line = juce::String ("The drive can't keep up. Still recording "
                                                    "everyone into the mixed file, but the separate "
                                                    "microphone tracks have stopped. Close other "
                                                    "apps using the disk.");

                    noteActivity (ActivityLevel::Failed, "Drive", line);
                    return line;
                }

            case WritePipelineState::FillWarning:
                {
                    const auto line = juce::String ("The drive is falling behind. Nothing has been "
                                                    "lost yet -- close any other apps using the "
                                                    "disk.");

                    noteActivity (ActivityLevel::Warning, "Drive", line);
                    return line;
                }

            case WritePipelineState::Healthy:
                break;
        }
    }

    // §6.5 next: running out of room stops the take, which outranks everything
    // else that is merely a warning.
    switch (pollCapacityWarning())
    {
        case RemainingTimeWarning::Exhausted:
            // Said AND done. This line used to claim the take had stopped
            // while the writer carried on into a full drive until a write
            // failed, which then arrived as the wrong notice.
            if (recordingEngine.getState() == RecordingState::Recording)
                toggleRecording();
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
        // A stream that ran for the whole take and carried nothing is a
        // different fault from one that never arrived, and sending someone to
        // check the drive when the problem is a muted microphone wastes the
        // one moment they are still standing next to the rig.
        // Not the user's rig. Say so plainly rather than sending them to check
        // hardware that was working the whole time.
        if (lastTakeVerdict == TakeAudioVerdict::DroppedByApp)
            return "Recording stopped, and the sound did not reach the files in "
                   + lastSessionFolder
                   + " -- your microphones were working. This is a fault in SobStage.";

        if (lastTakeVerdict == TakeAudioVerdict::OnlySilence)
            return "Recording stopped, but the files in " + lastSessionFolder
                   + " are silent -- the microphones were connected but sent no sound.";

        if (lastTakeHeldNoAudio)
            return "Recording stopped, but the files in " + lastSessionFolder
                   + " are empty -- no audio reached the drive.";

        return "Saved to " + lastSessionFolder;
    }

    // The post-take combine, which nothing had ever asked about. TakeCombiner
    // has always set a problem string and Application has always had a getter
    // for it, and no code anywhere in the app called that getter -- so a
    // combine that failed after the take reported nothing, and the user was
    // left looking for a file that was never written.
    {
        const auto combine = takeCombiner.getStatus();
        const auto problem = juce::String (combine.problem);

        if (! combine.running && problem.isNotEmpty() && problem != reportedCombineProblem)
        {
            reportedCombineProblem = problem;

            noteActivity (ActivityLevel::Warning, "Combined video", problem);
            return problem;
        }

        if (problem.isEmpty())
            reportedCombineProblem.clear();
    }

    // §10.5: hardware guidance, most serious first.
    const auto advice = getSetupAdvice();

    if (! advice.empty())
        return juce::String (advice.front().message);

    // Last, and only when nothing above had anything to say: the most serious
    // thing in the journal the user has not been shown.
    //
    // This is what closes the gap the journal was written for. Every branch
    // above holds the line for as long as its condition is true, so anything
    // that happened underneath one of them -- a mic that dropped and came back
    // during a disk warning, a backup that never opened while the drive was
    // busy -- reached the record and never reached the screen. Here it does,
    // once the loud thing has finished being loud.
    if (activityLineSeconds > 0.0)
    {
        activityLineSeconds -= sinceLastCallSeconds;
        return activityLine;
    }

    if (ActivityEntry unseen; activity.getMostSeriousUnseen (unseen))
    {
        // That one entry, not all of them. The line can only show one sentence,
        // so marking the rest seen because this one was shown would lose every
        // other piece of unread news -- which is the silence this whole thing
        // is here to remove.
        activity.markSeen (unseen.id);

        // Ordinary starts and stops are not worth interrupting a quiet screen
        // for -- they are in the panel and in the take's own log. The line is
        // for the things that need acting on.
        if (unseen.level == ActivityLevel::Warning || unseen.level == ActivityLevel::Failed)
        {
            activityLine = juce::String (unseen.message);
            activityLineSeconds = 8.0;
            return activityLine;
        }
    }

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
    // §11: a bundle missing the device inventory is the bundle that cannot
    // answer "what was plugged in", which is the first question anyone reading
    // it asks. Silently shipping one without it wastes a round trip.
    if (! tempInventory.getFile().replaceWithText (juce::JSON::toString (juce::var (summary), true)))
        noteActivity (ActivityLevel::Warning, "Diagnostics",
                      "Couldn't list the connected devices, so the diagnostics file won't include "
                      "them.");
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

    if (out.openedOk() && builder.writeToStream (out, nullptr))
    {
        noteActivity (ActivityLevel::Stopped, "Diagnostics",
                      "Saved a diagnostics file to " + destinationZip.getFullPathName() + ".");
    }
    else
    {
        // The user asked for a file and was shown nothing either way, so a
        // failed export looked exactly like a successful one -- right up until
        // they went to attach it to an email.
        noteActivity (ActivityLevel::Failed, "Diagnostics",
                      "Couldn't write the diagnostics file to "
                      + destinationZip.getFullPathName() + ". Try somewhere else.");
    }
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

    // Defaults instead of a failure is right; defaults instead of a failure
    // AND without a word is not. Everything §2.4 remembers about the rig has
    // just been replaced, and the user is about to meet an app that looks like
    // it forgot them.
    if (rememberedSettings.wasUnreadable)
        noteActivity (ActivityLevel::Warning, "Settings",
                      "Your saved settings couldn't be read, so everything is back to its "
                      "defaults -- check your microphone names and where takes are being saved.");

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
    // §7: what other apps see. A settings file written under the app's previous
    // name carries that name here, and carrying it forward would leave the
    // device in everyone's Zoom and OBS still called the old thing -- a rename
    // everywhere except the one place other software looks.
    //
    // Only the untouched default is moved. Someone who typed their own name
    // into this field meant it, and it is not ours to overwrite.
    aggregateName = rememberedSettings.aggregateName == "Multi-Mic Aggregator"
                        ? juce::String ("SobStage")
                        : juce::String (rememberedSettings.aggregateName);
    masterVolume = rememberedSettings.masterVolume;
    cameraController.setPreviewQuality (rememberedSettings.cameraPreviewFullQuality
                                            ? PreviewQuality::Full : PreviewQuality::Low);
    // The size table these index into roughly doubled at every step, because
    // the old top of the range -- 456px in a 760px window -- was not a picture
    // anyone could judge focus on. A settings file written before that carries
    // the old default, 1, which was never a choice so much as the value nobody
    // had reason to change; lifting exactly that one to the new default is what
    // stops the enlargement from reaching only people installing fresh.
    //
    // Any other remembered value was somebody moving the arrows on purpose, and
    // it is kept: the same index is a much larger picture now anyway.
    cameraTileScale = rememberedSettings.cameraTileScale == 1 ? 5
                                                             : rememberedSettings.cameraTileScale;
    combineVideoAndAudio = rememberedSettings.combineVideoAndAudio;
    deliveryTarget = juce::String (rememberedSettings.deliveryTarget);
    sampleRateOverride = rememberedSettings.sampleRateOverride;
    bufferSizeOverride = rememberedSettings.bufferSizeOverride;

    if (rememberedSettings.bitDepthOverride == 16
        || rememberedSettings.bitDepthOverride == 24
        || rememberedSettings.bitDepthOverride == 32)
        currentBitDepth = rememberedSettings.bitDepthOverride;

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

void Application::setSampleRateOverride (uint32_t rate)
{
    if (sampleRateOverride == rate)
        return;

    sampleRateOverride = rate;
    saveSettings();

    // The rate is fixed for the life of a stream (§5.4), so the streams are
    // reopened rather than nudged. handleDeviceChange re-runs §2.2 and applies
    // the new choice through exactly the path a hot-plug takes.
    onDeviceListChanged();
}

void Application::setBitDepthOverride (int bits)
{
    if (bits != 16 && bits != 24 && bits != 32)
        return;

    if (currentBitDepth == bits)
        return;

    // Bit depth is a property of the files, chosen when a take starts, so this
    // needs no stream reopened -- it simply applies to the next press of record.
    currentBitDepth = bits;
    saveSettings();
}

void Application::setBufferSizeOverride (int samples)
{
    if (samples < 0)
        return;

    if (bufferSizeOverride == samples)
        return;

    bufferSizeOverride = samples;
    saveSettings();

    // Fixed for the life of a stream (§5.4), so the streams are reopened
    // through the same path a hot-plug takes.
    onDeviceListChanged();
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
    settings.cameraTileScale = cameraTileScale;
    settings.combineVideoAndAudio = combineVideoAndAudio;
    settings.deliveryTarget = deliveryTarget.toStdString();
    settings.sampleRateOverride = sampleRateOverride;
    settings.bitDepthOverride = currentBitDepth == 24 ? 0 : currentBitDepth;
    settings.bufferSizeOverride = bufferSizeOverride;

    for (const auto& entry : portIdentityStore.all())
        settings.ports.push_back ({ entry.first, entry.second });

    for (const auto& device : deviceManager.getDevices())
        if (! device.userEnabled)
            settings.disabledMicKeys.push_back (device.identity.key());

    // And every mic the user switched off that is not plugged in right now,
    // as the cameras below already do: §2.4 port memory, for the one setting
    // that used to be forgotten the moment the mic was unplugged.
    for (const auto& key : rememberedSettings.disabledMicKeys)
    {
        bool enumerated = false;
        for (const auto& device : deviceManager.getDevices())
            if (device.identity.key() == key) { enumerated = true; break; }

        if (! enumerated)
            settings.disabledMicKeys.push_back (key);
    }

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

    // Checked, because silently failing here presents next launch as an app
    // that forgot the user's microphone names, trims and destination -- and
    // there is no moment at which that is explained.
    if (! file.replaceWithText (juce::String (settings.toJsonString())))
        noteActivity (ActivityLevel::Warning, "Settings",
                      "Couldn't save your settings, so they may not be remembered next time.");

    rememberedSettings = settings;
}

juce::File Application::getLogFile()
{
    return getSupportFolder().getChildFile ("log.txt");
}

bool Application::supportFolderMigrationFailed = false;

juce::File Application::getSupportFolder()
{
    const auto appData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
    const auto current = appData.getChildFile ("SobStage");

    // The app was called something else before, and everything §2.4 remembers
    // about a rig -- every microphone's name and trim, which ones are switched
    // off, the destination folder, the backup setting -- lives in this folder
    // under that old name. A rename that simply looked somewhere new would
    // present as every one of those being forgotten, which is precisely the
    // §10.1 failure the settings file exists to prevent: being asked the same
    // questions again.
    //
    // So the old folder is moved across, once, the first time the new name is
    // used. Moved rather than copied: two folders would then drift, and the
    // one being written to is not the one a user going looking would find.
    if (! current.isDirectory())
    {
        const auto previous = appData.getChildFile ("MultiMicAggregator");

        // The result is kept. A failed move presents as every remembered
        // microphone name, trim, disabled mic and destination folder being
        // forgotten at once -- the exact §10.1 failure this migration exists to
        // prevent -- and it used to happen without a word.
        if (previous.isDirectory() && ! previous.moveFileTo (current))
            supportFolderMigrationFailed = true;
    }

    return current;
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

void Application::clearRecoveredSessions()
{
    // The rule for "interrupted" is an empty stop timestamp, and repairing
    // the headers never wrote one -- so the same takes came back at every
    // launch. Stamped with the moment they were recovered; the audio has
    // already been repaired and is what it is.
    const auto now = juce::Time::getCurrentTime().toISO8601 (true).toStdString();

    for (const auto& session : recoveredSessions)
    {
        const auto file = juce::File (juce::String (session.folder)).getChildFile ("session.json");

        if (! file.existsAsFile())
            continue;

        try
        {
            auto meta = SessionMetadata::fromJsonString (file.loadFileAsString().toStdString());

            if (meta.stopTimestampIso.empty())
            {
                meta.stopTimestampIso = now;

                // Checked. This is the write that stops a recovered take being
                // offered again at every launch, so a card that refuses it --
                // read-only, full, the very card whose failure caused the
                // interruption -- produced the same list forever with nothing
                // explaining why dismissing it did not stick.
                if (! file.replaceWithText (juce::String (meta.toJsonString())))
                    noteActivity (ActivityLevel::Warning, "Interrupted take",
                                  file.getParentDirectory().getFileName()
                                  + " can't be marked as dealt with -- this card won't accept the "
                                    "change, so it will be offered again next time.");
            }
        }
        catch (...)
        {
            // Unreadable metadata stays as it is; the take will be offered
            // again, which beats overwriting something we could not parse.
        }
    }

    recoveredSessions.clear();
}

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
            bool metadataUnreadable = false;

            try
            {
                meta = SessionMetadata::fromJsonString (metadataFile.loadFileAsString().toStdString());
            }
            catch (...)
            {
                meta = {};
                metadataUnreadable = true;
            }

            // A take whose record cannot be read is not thereby a take that did
            // not happen, and the user should be told the record is corrupt
            // rather than left with a recovery entry that has no start time and
            // no explanation.
            //
            // Reported unconditionally. Guarding this on
            // `! sessionWasInterrupted(meta)` was a branch that could never run:
            // the catch leaves meta empty, an empty meta has no stop timestamp,
            // and no stop timestamp is exactly what sessionWasInterrupted()
            // calls interrupted -- so the condition was always false and this
            // read as coverage while doing nothing.
            if (metadataUnreadable)
                noteActivity (ActivityLevel::Warning, "Interrupted take",
                              juce::String (folder.getFileName())
                              + " has a details file this app can't read, so what it says about "
                                "that take is gone. Its audio is still in that folder.");

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

            // A repair the card would not accept. The file is still listed --
            // it is the user's audio and may well play -- but "recovered" would
            // be a promise this could not keep.
            for (const auto& f : session.files)
                if (f.repairFailed)
                    noteActivity (ActivityLevel::Warning, "Interrupted take",
                                  juce::String (f.fileName)
                                  + " couldn't be opened or repaired -- this card wouldn't accept "
                                    "the fix. Copy it somewhere else before playing it.");

            // §6.6: a take where nothing survived is not presented at all --
            // better to say nothing than to hand someone an unplayable stub.
            //
            // But §6.6 asks for the other half too: "report it as empty rather
            // than presenting an unplayable stub." Reporting it was the half
            // that never happened, so an interrupted take that lost everything
            // vanished from the recovery list with no trace -- and a folder
            // sitting on the card holding nothing playable is exactly the thing
            // someone spends an evening trying to open.
            if (session.isWorthPresenting())
            {
                recoveredSessions.push_back (std::move (session));
            }
            else
            {
                noteActivity (ActivityLevel::Failed, "Interrupted take",
                              juce::String (folder.getFileName())
                              + " was interrupted and nothing playable survived in it. There is "
                                "nothing to recover from that folder.");
            }
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
