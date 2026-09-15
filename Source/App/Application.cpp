#include "Application.h"
#include "../Platform/SystemPermissions.h"
#include "../Core/TakeCompleteness.h"
#include "../Platform/ReducedMotion.h"
#include "../Platform/SystemThermalState.h"
#include "../Core/ClockMasterResolver.h"
#include "../Core/CombinedTakePlan.h"
#include "../Core/LoudnessMeter.h"
#include "../Core/SampleRateNegotiator.h"
#include "../Platform/NullBackend.h"
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <set>
#include <thread>

#if JUCE_MAC || defined (__linux__)
#include <dirent.h>
#endif

#if JUCE_MAC
#include "../Platform/CoreAudioBackend.h"
#elif JUCE_WINDOWS
#include "../Platform/WasapiAsioBackend.h"
#elif defined(__linux__) && ! defined(MMA_NO_ALSA)
#include "../Platform/AlsaBackend.h"
#endif

namespace mma {

namespace {

std::vector<std::string> mutationAliasesForRoots (const std::vector<std::string>& roots)
{
    std::vector<std::string> aliases;
    aliases.reserve (roots.size() * 2);

    for (const auto& root : roots)
    {
        if (root.empty())
            continue;

        aliases.push_back (root);

        // weakly_canonical resolves every existing symlink/alias prefix while
        // still accepting a take folder which has not been created yet. This
        // may enter a stale removable volume, so this helper is called only by
        // detached workers, never by the message thread.
        std::error_code error;
        const auto canonical = std::filesystem::weakly_canonical (
            std::filesystem::u8path (root), error);

        if (! error)
        {
            const auto resolved = canonical.u8string();
            if (! resolved.empty())
                aliases.push_back (resolved);
        }
    }

    return aliases;
}

DetachedPathMutationGate::LeasePtr waitForMutationLease (
    DetachedPathMutationGate gate,
    const std::vector<std::string>& roots,
    const std::atomic<bool>& cancelled)
{
    const auto aliases = mutationAliasesForRoots (roots);

    while (! cancelled.load (std::memory_order_acquire))
    {
        if (auto lease = gate.tryAcquire (aliases))
            return lease;

        // A prior detached syscall may never return. This worker is disposable
        // too, so polling here never blocks the message thread and selecting a
        // genuinely different root can abandon it immediately.
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }

    return {};
}

// The version CMake stamped in. JUCE_APP_VERSION is only defined for builds
// that go through JuceHeader.h, so stringifying it here wrote the literal
// token "JUCE_APP_VERSION" into every take's session.json and every
// diagnostics bundle -- the two places support would look to find out which
// build produced a recording.
juce::String appVersionString()
{
   #if defined (SOBSTAGE_VERSION_STRING)
    return SOBSTAGE_VERSION_STRING;
   #else
    return "dev";
   #endif
}

// Writes text and then checks the file actually holds it. On a full drive
// juce::File::replaceWithText can report success having left a truncated or
// empty file behind -- seen for real: a take whose card filled up ended with a
// zero-byte session.json and nothing said so. Everything durable this app
// writes goes through here, so a write that did not survive is a write the
// user hears about.
bool replaceWithTextChecked (const juce::File& file, const juce::String& text)
{
    // nullptr line endings: write the text verbatim. juce::File::replaceWithText
    // defaults to turning every "\n" into "\r\n", which makes the file bigger
    // than the string it came from -- so the size check below called every
    // healthy multi-line write a failure. A false alarm on every take is the
    // same disservice as the silence this check exists to end.
    if (! file.replaceWithText (text, false, false, nullptr))
        return false;

    return file.getSize() == static_cast<juce::int64> (text.getNumBytesAsUTF8());
}

std::vector<juce::File> directChildDirectories (const juce::File& root)
{
    std::vector<juce::File> result;

   #if JUCE_MAC || defined (__linux__)
    // JUCE's macOS File::findChildFiles path goes through
    // NSAllDescendantPathsEnumerator, which has been observed waiting forever
    // when /Volumes contains a disappearing capture disk. These are flat mount
    // roots, so use the native non-recursive directory API directly. The
    // caller validates each candidate on its background worker.
    const auto path = root.getFullPathName();
    std::unique_ptr<DIR, decltype (&::closedir)> directory (
        ::opendir (path.toRawUTF8()), &::closedir);

    if (directory == nullptr)
        return result;

    while (const auto* entry = ::readdir (directory.get()))
    {
        const juce::String name = juce::String::fromUTF8 (entry->d_name);

        if (name == "." || name == "..")
            continue;

       #if defined (DT_DIR) && defined (DT_LNK) && defined (DT_UNKNOWN)
        if (entry->d_type != DT_DIR
            && entry->d_type != DT_LNK
            && entry->d_type != DT_UNKNOWN)
            continue;
       #endif

        result.push_back (root.getChildFile (name));
    }

   #else
    for (const auto& child : root.findChildFiles (juce::File::findDirectories, false))
        result.push_back (child);
   #endif

    return result;
}

Application::StorageVolume homeStorageVolumeFallback()
{
    const auto destination = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                 .getChildFile ("RECORDINGS");
    return { "Home folder", destination.getFullPathName(), false, false };
}

juce::String baseSessionFolderName (juce::Time now, const juce::String& name)
{
    auto cleaned = SessionFolderNaming::sanitizeName (name.toStdString());
    if (cleaned.empty())
        cleaned = SessionFolderNaming::kDefaultName;

    return juce::String (SessionFolderNaming::buildFolderName (
        now.getYear(), now.getMonth() + 1, now.getDayOfMonth(),
        now.getHours(), now.getMinutes(), cleaned));
}

} // namespace

Application::Application()
    // §9.3, asked once. The setting does not change between meter repaints, and
    // the alternative -- querying the OS per strip at 60Hz -- would be absurd.
    : reducedMotionPreferred (prefersReducedMotionOnThisSystem())
{
}
Application::~Application() { shutdown(); }

std::weak_ptr<int> Application::getAliveToken() const
{
    const std::lock_guard<std::mutex> guard (aliveTokenMutex);
    return aliveToken;
}

void Application::invalidateAliveToken()
{
    const std::lock_guard<std::mutex> guard (aliveTokenMutex);
    aliveToken.reset();
}

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

    // §10.1: asked BEFORE the backend enumerates, because on macOS a denial is
    // what makes enumeration come back empty. Without this the app told a user
    // with a microphone plugged in to "plug in a USB microphone" -- advice for
    // a problem they did not have, about the one thing they had already done.
    microphonePermission = queryMicrophonePermission();

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
        const auto alive = getAliveToken();

        audioBackend->setDeviceChangeCallback ([this, alive]
        {
            juce::MessageManager::callAsync ([this, alive]
            {
                if (alive.lock() != nullptr)
                    onDeviceListChanged();
            });
        });
        onDeviceListChanged(); // initial enumeration, per §2 "at launch"

        // Said once, at launch, if the backend cannot watch the rig at all.
        // The watch is set up above and its two failure paths used to return in
        // silence -- leaving an app that never notices a microphone arriving,
        // and never reports one pulled out mid-take (§6.5), while looking
        // exactly like one that has nothing to report.
        if (const auto hotplug = audioBackend->getHotplugProblem(); ! hotplug.empty())
            noteActivity (ActivityLevel::Warning, "Microphones", juce::String (hotplug));
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

    // A remembered destination bypasses chooseInitialDestination(), but the
    // Settings panel will still need the volume list. This only schedules the
    // mount/free-space scan; it immediately returns the home-folder fallback,
    // so a sick removable volume cannot hold the window closed at launch.
    (void) getStorageVolumes();

    // Start the filesystem worker before the window asks for its first status
    // line. Submitting this request performs no filesystem work here.
    filesystemStatusProbe.setRequest (currentFilesystemProbeRequest());

    // §6.4: benchmark before the user reaches for record, not at record time.
    beginPreflightForDestination();

    // §6.6: schedule the one-shot recovery work before the window opens.
    // The walks and repairs run on detached, worker-owned state; arming stays
    // gated until the roots this take would use have finished.
    scanForInterruptedSessions();

    // §5.1 monitoring was already opened by the initial
    // onDeviceListChanged() above. Reopening the identical CoreAudio IOProcs
    // here immediately after startup made some USB drivers stall in their HAL
    // teardown/recreate path before the window could appear.

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

void Application::openEnabledCameras (bool retryFailures)
{
    cameraController.applySelection (retryFailures);

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
            p.hasChannelLayoutDecision = persisted->hasChannelLayoutDecision;

            // §2.4 remembers §2.1's verdict per port. Only a decision that was
            // actually made collapses a two-input device; the default of "no
            // decision yet" keeps both sides.
            p.knownDuplicateStereo = persisted->hasChannelLayoutDecision
                                  && persisted->channelLayoutIsMono;
            p.monoSourceChannel = persisted->channelLayoutMonoSource;
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
        c.collapseStereoPair = planned.collapseStereoPair;
        c.analyzeStereoPair = planned.analyzeStereoPair;
        c.monoSourceChannel = planned.monoSourceChannel;
        c.displayName = planned.displayName;

        // §6.2: "01_Yeti-Kitchen" -- ordinal prefix plus the sanitized name,
        // so the stems sort in channel order in any file browser.
        char prefix[4] = {};
        std::snprintf (prefix, sizeof (prefix), "%02d", index);
        c.fileName = std::string (prefix) + "_" + SessionFolderNaming::sanitizeName (c.displayName);

        for (const auto& d : deviceManager.getDevices())
        {
            if (d.identity.key() != planned.deviceKey)
                continue;

            if (const auto persisted = portIdentityStore.get (d.identity))
                c.trimDb = persisted->trimDb;

            // §2.3: this channel is written at its own device's depth. A rig
            // can hold a 16-bit microphone and a 24-bit interface at once and
            // there is no single answer that serves both -- 24 pads the first,
            // 16 discards from the second.
            //
            // currentBitDepth is the fallback, so a backend that reports
            // nothing keeps exactly today's behaviour, and a user who has set
            // the depth by hand keeps their choice.
            c.bitDepth = SampleFormat::chooseRecordingBitDepth (d.supportedBitDepths,
                                                                currentBitDepth);
        }

        channels.push_back (std::move (c));
    }

    return channels;
}

void Application::applyChannelLayoutDecisions()
{
    if (capture == nullptr || capture->isRecording())
        return;

    bool changed = false;
    const auto& captureChannels = capture->getChannels();

    for (size_t i = 0; i < captureChannels.size(); ++i)
    {
        const auto& channel = captureChannels[i];
        const bool learningLayout = channel.analyzeStereoPair;
        const bool refreshingCollapsedSource = channel.collapseStereoPair;
        if (! learningLayout && ! refreshingCollapsedSource)
            continue;

        // The sixty-second all-silent Mono fallback is intentionally not a
        // port classification. Persisting it would permanently hide input 2
        // of an interface that merely opened in a quiet room.
        if (! capture->isChannelLayoutDecisionPersistable (static_cast<int> (i)))
            continue;

        const auto decision = capture->getChannelLayoutDecision (static_cast<int> (i));

        for (const auto& device : deviceManager.getDevices())
        {
            if (device.identity.key() != channel.deviceId)
                continue;

            auto settings = portIdentityStore.get (device.identity).value_or (
                PersistedDeviceSettings {});

            if (learningLayout && ! settings.hasChannelLayoutDecision)
            {
                // Another UI action may already have saved a verdict between
                // the callback publishing it and this slow tick. Never
                // overwrite an explicit answer with a stale observation.
                settings.hasChannelLayoutDecision = true;
                settings.channelLayoutIsMono = decision == ChannelLayoutDecision::Mono;
                const int source = capture->getChannelLayoutSource (static_cast<int> (i));
                settings.channelLayoutMonoSource = decision == ChannelLayoutDecision::Mono
                                                 && source == 1 ? 1 : 0;
                portIdentityStore.put (device.identity, settings);
                changed = true;
            }
            else if (refreshingCollapsedSource
                     && settings.hasChannelLayoutDecision
                     && settings.channelLayoutIsMono
                     && decision == ChannelLayoutDecision::Mono)
            {
                // A known mono device can reveal that its live capsule is on
                // the opposite side this connection. Keep that valid idle
                // re-evaluation across the next launch as well as the next
                // take; silence alone never reaches this persistable branch.
                const int source = capture->getChannelLayoutSource (static_cast<int> (i)) == 1
                                     ? 1 : 0;
                if (settings.channelLayoutMonoSource != source)
                {
                    settings.channelLayoutMonoSource = source;
                    portIdentityStore.put (device.identity, settings);
                    changed = true;
                }
            }

            break;
        }
    }

    if (! changed)
        return;

    // Disk I/O and stream reconstruction stay on the message thread. The
    // analyzer callback itself performs only arithmetic and atomic stores.
    saveSettings();
    restartCapture();
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
            journalledMonitorFailedCount = 0;
            journalledMonitorOk = false;
        }

        if (onCaptureRebuilt)
            onCaptureRebuilt();

        return;
    }

    const bool started = capture->startMonitoring (channels, selectedOutputDeviceId);

    // §5.4: what the monitor path actually costs, taken from the backend that
    // opened it.
    //
    // This member was declared and never assigned by anything, so the Advanced
    // panel reported monitoring latency as "0.0 ms" and every take's
    // session.json recorded 0.0 as a permanent fact about how the take was
    // made. Zero is not a small latency; it is an impossible one. Every backend
    // had worked the figure out all along and CaptureCoordinator dropped it.
    measuredLatencyMs = capture->getMonitoringLatencyMs();

    // The microphones that actually OPENED, not the ones that were selected.
    //
    // A device that refuses to open no longer takes the whole rig down with it,
    // so "started" is now true in cases where part of the rig is missing --
    // and counting the selection here then announced "3 microphones are live"
    // for a rig with a dead one. A count that is wrong in the user's favour is
    // worse than no count.
    const auto& failedToOpen = capture->getDevicesThatFailedToOpen();

    int liveCount = 0;
    for (const auto& ch : channels)
        if (std::find (failedToOpen.begin(), failedToOpen.end(), ch.deviceId) == failedToOpen.end())
            ++liveCount;

    const int failedCount = static_cast<int> (failedToOpen.size());

    // Only a change is news. This runs on every rename, hot-plug and output
    // change, and an entry per call would bury everything else under a repeated
    // "2 microphones are live". The failure count is part of "a change": a
    // microphone dropping out of a rig that still has the same number selected
    // is exactly the event worth saying.
    const bool worthSaying = started != journalledMonitorOk
                          || liveCount != journalledMonitorCount
                          || failedCount != journalledMonitorFailedCount;

    journalledMonitorOk = started;
    journalledMonitorCount = liveCount;
    journalledMonitorFailedCount = failedCount;

    if (started)
    {
        applyClockMaster();
        driftMeasuredSeconds = 0.0;

        if (worthSaying)
            noteActivity (ActivityLevel::Started, "Monitoring",
                          juce::String (liveCount)
                          + (liveCount == 1 ? " microphone is live." : " microphones are live."));

        // §0.1: a microphone that would not open is no longer fatal, and that
        // is precisely why it has to be said out loud here. Before, the failure
        // reached the journal because it killed monitoring outright and landed
        // in the [failed] branch below; carrying on without it must not also
        // mean carrying on quietly.
        if (failedCount > 0 && worthSaying)
        {
            noteActivity (ActivityLevel::Warning, "Microphones",
                          juce::String (capture->getMonitorProblem()));

            // §14.2 names this exact event first: "bus power exhaustion doesn't
            // produce a clean error; it shows up as ENUMERATION FAILURES and
            // device drops". BusPowerDetector has always been able to count
            // them and nothing ever handed it one -- Application::
            // noteDeviceDropout() was written, wired to the advisor, and called
            // from nowhere in the app, so "use a powered hub" could not be
            // reached however many microphones dropped off a shared port.
            //
            // Gated on worthSaying with the message above, so a rebuild that
            // changes nothing does not stack events into the five-minute
            // window and invent a power problem out of one bad cable.
            for (int i = 0; i < failedCount; ++i)
                noteDeviceDropout();
        }
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

    // A platform with no combined device is not a failure to report. publish()
    // answers false there by definition, and once its result started being
    // read, every Windows and Linux launch filed "Couldn't make the combined
    // device" as a FAILURE -- about a feature that platform has never had.
    // What other apps see is already explained accurately, in the Advanced
    // panel, by getStatus().
    if (! systemAggregate->isSupported())
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

        // §2.3: carried through rather than dropped here, which is where it was
        // being dropped. The field existed on the descriptor, one backend
        // filled it, and nothing downstream ever saw it.
        state.supportedBitDepths = d.supportedBitDepths;

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

    // Which microphone arrived, and which one left. Run after the remembered
    // settings so each is called by the name the user gave it.
    announceDeviceChanges (seen);

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

                // The other half of §14.2's heuristic: a "device drop". Already
                // edge-triggered by the isWritingSilence guard above, so one
                // unplug counts once however many times the device list is
                // re-read while it is gone.
                noteDeviceDropout();
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

    std::vector<OutputDeviceCandidate> snapshot;
    outputDeviceNames.clear();
    outputDeviceIdByLabel.clear();

    for (const auto& d : audioBackend->enumerateOutputDevices())
    {
        OutputDeviceCandidate c;
        c.id = d.usbLocationId.empty() ? d.name : d.usbLocationId;
        c.displayName = d.name;
        c.hasPhysicalHeadphoneJack = d.hasPhysicalHeadphoneJack;
        c.isBuiltIn = d.isBuiltIn;

        // The monitor output shares the recording clock. A fixed-48 kHz HDMI
        // capture-card endpoint cannot serve a 44.1 kHz T12S take and must not
        // displace a compatible Mac output merely because it hot-plugged most
        // recently. An empty capability list means the backend cannot say, so
        // keep it eligible and let the open path report any real refusal.
        const auto recordingRate = static_cast<uint32_t> (currentSampleRate + 0.5);
        c.supportsRecordingSampleRate = OutputDeviceSelector::supportsRecordingRate (
            d.currentSampleRate, d.supportedSampleRates, recordingRate);

        // §5.2: a microphone's own playback endpoint is never a monitor output.
        c.isMicrophonePlaybackEndpoint = d.isMicrophone;

        // §5.5: refuse to route output to a device that is also a capture device.
        for (const auto& mic : deviceManager.getDevices())
            if (mic.included && ! d.usbLocationId.empty() && mic.identity.locationId == d.usbLocationId)
                c.isAlsoSelectedInput = true;

        snapshot.push_back (std::move (c));
    }

    // A later snapshot is not itself an arrival. The tracker remembers what
    // was present before and marks only ids that actually appeared, preserving
    // §5.3's newly-connected priority across unrelated device notifications.
    auto candidates = outputDeviceTracker.observe (std::move (snapshot));

    // Labels shown in Settings map back to stable ids without asking the OS to
    // enumerate again on click. Duplicate product names are numbered rather
    // than made indistinguishable in the picker.
    std::map<std::string, int> nameTotals, nameOccurrence;
    for (const auto& c : candidates)
        if (OutputDeviceSelector::isEligible (c))
            ++nameTotals[c.displayName];

    for (const auto& c : candidates)
    {
        if (! OutputDeviceSelector::isEligible (c))
            continue;

        auto label = c.displayName;
        if (nameTotals[c.displayName] > 1)
            label += " (" + std::to_string (++nameOccurrence[c.displayName]) + ")";

        outputDeviceNames.push_back (label);
        outputDeviceIdByLabel[label] = c.id;
    }

    // Headphones arriving or leaving is a change to the rig and was said only
    // when it left the user with nothing to listen on at all. A microphone's
    // own playback endpoint is skipped: it is the same physical thing the
    // microphone list already announced, and saying it twice under two names
    // is noise rather than news.
    {
        std::map<std::string, std::string> outputs;

        for (const auto& c : candidates)
            if (! c.isMicrophonePlaybackEndpoint)
                outputs[c.id] = c.displayName;

        announceOutputChanges (outputs);
    }

    const auto selection = OutputDeviceSelector::select (candidates, rememberedOutputDeviceId);
    selectedOutputDeviceId = selection.id;
    selectedOutputDeviceName.clear();

    for (const auto& [label, id] : outputDeviceIdByLabel)
        if (id == selectedOutputDeviceId)
        {
            selectedOutputDeviceName = label;
            break;
        }

    outputSelectionProblem = selection.explanation;

    // Into the record, not just onto the screen. Having nothing to listen on
    // is a fact about the session -- someone singing to a rig that cannot play
    // them back is the thing they will ask about afterwards -- and it was shown
    // in the panel and written down nowhere. Only on a change, so a session
    // with no headphones does not repeat itself.
    if (outputSelectionProblem != reportedOutputProblem)
    {
        reportedOutputProblem = outputSelectionProblem;

        if (! outputSelectionProblem.empty())
            noteActivity (ActivityLevel::Warning, "Monitoring",
                          juce::String (outputSelectionProblem));
        else
            noteActivity (ActivityLevel::Recovered, "Monitoring",
                          "You can hear yourself again.");
    }
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
    {
        bool snapshotAvailable = false;
        for (const auto& file : getCurrentSessionFiles (&snapshotAvailable))
            reading.bytesOnDisk += static_cast<uint64_t> (std::max<int64_t> (0, file.sizeBytes));
        reading.diskObservationAvailable = snapshotAvailable;
    }

    return reading;
}

TakeHealth Application::snapshotTakeHealth() const
{
    TakeHealth health;

    const int micCount = getIncludedMicCount();
    for (int i = 0; i < micCount; ++i)
        health.mics.push_back ({ getMicDisplayName (i).toStdString(), isMicLive (i) });

    // The controller freezes the cameras that actually began this take. An
    // unplugged writer stays in that roster as absent; a preview cannot be
    // reopened and mistaken for resumed recording during the same take.
    for (const auto& camera : cameraController.getTakeCameraStates())
        health.cameras.push_back ({ camera.displayName, camera.recording });

    health.cameraProblem = cameraController.getProblem().toStdString();
    health.monitorProblem = getMonitorProblem().toStdString();

    if (capture != nullptr)
    {
        health.framesDropped = capture->getFramesDropped();
        // The take's own overruns, not the monitoring session's. TakeWatchdog
        // divides this by the sample rate and says "about N seconds lost so
        // far" about the CURRENT take, so a session-lifetime number made that
        // sentence describe audio lost before the take began.
        // The worst single channel, not the sum. TakeWatchdog turns this into
        // "about N seconds lost so far", and four rings overflowing together
        // for a second lose a second of recording, not four.
        health.samplesOverrun = capture->getWorstChannelOverrunThisTake();
        health.sampleRate = currentSampleRate;
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

Application::MicRenameTarget Application::getMicRenameTarget (int index) const
{
    // `index` is a strip, and a strip is one INPUT of a device -- resolved in
    // channel space, like every other strip accessor, rather than by walking
    // devices. Walking devices named the wrong port on any interface, and
    // could not name one input of it at all.
    MicRenameTarget target;

    if (capture != nullptr && capture->isRecording())
    {
        const auto& channels = capture->getChannels();
        if (index < 0 || index >= static_cast<int> (channels.size()))
            return target;
        target.deviceKey = channels[static_cast<size_t> (index)].deviceId;
        target.deviceChannel = channels[static_cast<size_t> (index)].deviceChannel;
    }
    else
    {
        const auto plan = planChannels (planDevices());
        if (index < 0 || index >= static_cast<int> (plan.size()))
            return target;
        target.deviceKey = plan[static_cast<size_t> (index)].deviceKey;
        target.deviceChannel = plan[static_cast<size_t> (index)].deviceChannel;
    }

    return target;
}

void Application::setMicAssignedName (int index, const juce::String& name)
{
    setMicAssignedName (getMicRenameTarget (index), name);
}

void Application::setMicAssignedName (const MicRenameTarget& target, const juce::String& name)
{
    if (! target.isValid())
        return;

    for (const auto& d : deviceManager.getDevices())
    {
        if (d.identity.key() != target.deviceKey)
            continue;

        auto settings = portIdentityStore.get (d.identity).value_or (PersistedDeviceSettings{});
        const auto clean = SessionFolderNaming::sanitizeName (name.toStdString());

        // One microphone is one box, so the box takes the name. On an interface
        // each socket is a person, so the socket does -- and the box's own name
        // is left alone for the other inputs.
        const bool knownDuplicateStereo = settings.hasChannelLayoutDecision && settings.channelLayoutIsMono;
        if (takeChannelsForDevice (d.inputChannelCount, knownDuplicateStereo) > 1)
        {
            if (clean.empty()) settings.inputNames.erase (target.deviceChannel);
            else               settings.inputNames[target.deviceChannel] = clean;
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

std::vector<Application::StorageVolume> Application::scanStorageVolumes()
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

        volumes.push_back ({ label, destination.getFullPathName(), removable, false });
    };

    // Mounted volumes. On macOS every attached card and disk appears under
    // /Volumes; on Windows the roots are the drive letters; on Linux the
    // common mount points are covered by the roots plus /media and /mnt.
    const juce::File volumesDir ("/Volumes");
    for (const auto& child : directChildDirectories (volumesDir))
        add (child, false);

    for (const auto& dir : { juce::File ("/media"), juce::File ("/mnt") })
        for (const auto& child : directChildDirectories (dir))
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

std::vector<Application::StorageVolume> Application::getStorageVolumes() const
{
    auto result = storageVolumeCache.getAndRefresh (std::chrono::milliseconds (2000), []
    {
        return scanStorageVolumes();
    });

    // This is deliberately constructed without asking the filesystem whether
    // it exists, is writable or has free space. The very first call therefore
    // has a useful answer even while the detached full scan is blocked in the
    // OS on a stale mount. A completed scan normally replaces it immediately.
    if (result.empty())
        result.push_back (homeStorageVolumeFallback());

    // The selected marker is cheap application state rather than part of the
    // filesystem scan, so it remains current while the volume list is cached.
    for (auto& volume : result)
        volume.isCurrent = volume.path == juce::String (destinationFolder);

    return result;
}

void Application::setDestinationByPath (const juce::String& path)
{
    const juce::File target (path);

    if (path.trim().isEmpty())
        return;

    // The selected mount may disappear while the Settings row is being
    // clicked. Do not ask that path anything on the message thread: accepting
    // the inert string schedules detached recovery and write-speed checks,
    // and their fail-closed result decides whether Record can arm.
    setDestinationFolder (target);

    if (recordingEngine.getState() != RecordingState::Recording)
        noteActivity (ActivityLevel::Started, "Save location",
                      "Checking " + target.getFullPathName()
                      + " before it can be used for a take.");
}

void Application::chooseInitialDestination()
{
    // §10.1: a first launch must not wait for a stale card mount. The first
    // getStorageVolumes() result is therefore the immediate ~/RECORDINGS
    // fallback while full platform discovery continues off the message thread.
    // Settings picks up connected removable volumes from the completed cache.
    for (const auto& volume : getStorageVolumes())
    {
        if (! volume.isRemovable)
            continue;

        const juce::File recordings (volume.path);
        if ((! recordings.exists() && recordings.createDirectory().wasOk())
            || (recordings.isDirectory() && recordings.hasWriteAccess()))
        {
            destinationFolder = recordings.getFullPathName().toStdString();
            return;
        }
    }

    destinationFolder = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                            .getChildFile ("RECORDINGS")
                            .getFullPathName()
                            .toStdString();
}

void Application::toggleRecording()
{
    if (recordingEngine.getState() == RecordingState::Idle)
    {
        // Keep the safety gates in the composition root as well as on the UI
        // button. A keyboard/action callback must not be able to create a take
        // while a recovery worker could still enumerate and repair that same
        // destination or mirror.
        if (const auto blocked = getRecordDisabledReason(); blocked.isNotEmpty())
        {
            recordStartProblem = blocked;
            return;
        }

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

                // The coordinator zeroes the counter itself when a take begins,
                // so the watermark has to follow it down or the first take's
                // losses would silence every later one.
                reportedLayoutMisses = 0;

                currentSessionFolder = folder;
                currentMirrorFolder = mirror;
                sessionStartIso = now.toISO8601 (true);

                // Do not attempt a fresh platform camera open after the audio
                // writer has started. JUCE's desktop open is synchronous and a
                // broken capture-card driver can wait inside it indefinitely;
                // doing that here would leave a take running behind a frozen
                // interface. Enabled previews are opened before arming by the
                // normal camera/UI reconciliation. Any camera which is not
                // already open is reported below while the sound take remains
                // controllable and complete.

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

            // §0.1: a stem that is silent for the whole take has to say why.
            //
            // A microphone that would not open is no longer allowed to stop the
            // rest of the rig recording, so its track is written as silence
            // beside working ones -- and silence nobody warned about is the
            // whole failure this app is built against. "Never opened" is a
            // different answer from "unplugged part way through", so it is
            // recorded at zero seconds, before any of the mid-take entries.
            if (capture != nullptr)
            {
                for (const auto& deviceId : capture->getDevicesThatFailedToOpen())
                    midTakeDropouts.push_back ({ 0.0, deviceId,
                                                 "This microphone could not be opened when the take "
                                                 "started, so its track is silent for the whole "
                                                 "take. The other microphones recorded normally." });
            }

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

        // AVFoundation's stopRecording is intentionally asynchronous. Keep
        // all claims which consume or describe the movie files together, and
        // run them only after each camera's didFinish callback (or the
        // controller's bounded fail-closed timeout) has been observed.
        auto completeStoppedTake = [this, takePeak, arrivedPeak]
        {

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
        // Whether the app ended this take, and why. A take stopped because the
        // card went away or the drive filled up used to write the same
        // "Recording stopped. Saved to X." a user-initiated stop writes -- so
        // reading the log afterwards, a take the app killed was
        // indistinguishable from one someone chose to end. The reason lived on
        // the advice line for a few seconds and nowhere else.
        const auto because = stopReason.isEmpty() ? juce::String (".")
                                                  : " -- " + stopReason + ".";
        const auto endedBy = stopReason.isEmpty() ? juce::String ("Recording stopped")
                                                  : juce::String ("Recording was stopped");

        noteActivity (stopReason.isNotEmpty() || lastTakeHeldNoAudio ? ActivityLevel::Failed
                                                                    : ActivityLevel::Stopped,
                      "Recording",
                      lastTakeHeldNoAudio
                          ? endedBy + because + " There is no audio in "
                                + juce::File (lastSessionFolder).getFileName()
                                + ". Check your microphones aren't muted."
                          : endedBy + because + " Saved to "
                                + juce::File (lastSessionFolder).getFileName() + ".");

        // One take, one reason. Cleared here so the next stop cannot inherit
        // this one's.
        stopReason.clear();

        // Written AGAIN, now that the stop itself is in the journal. The first
        // write happens inside writeSessionMetadata above, before this note
        // exists -- so the log filed beside every take ended mid-recording and
        // never said the take stopped, let alone why. A take the app killed
        // because the drive filled or the card left is exactly the one whose
        // log has to carry the reason, and that was the one it dropped.
        writeActivityLog (juce::File (currentSessionFolder));

        if (currentMirrorFolder.isNotEmpty())
            writeActivityLog (juce::File (currentMirrorFolder));

        recordingEngine.stop();
        recordingStartMs = 0.0;
        bufferLadder.setRecording (false);

        currentSessionFolder.clear();
        currentMirrorFolder.clear();

        // A destination the user chose mid-take, applied now that applying it
        // cannot move a running take's files out from under it.
        //
        // Placed here, AFTER recordingEngine.stop(), and that is the whole
        // point: run before it, the state is still Recording, so the apply fell
        // into its own deferral branch, re-armed the pending value and
        // re-emitted the promise -- forever. The user was told at every stop
        // that their new folder took effect from the next take, and it never
        // did, for the life of the process. applyDestinationFolder is used
        // rather than setDestinationFolder so that cannot happen again: it does
        // not consult the recording state at all.
        if (pendingDestinationFolder.isNotEmpty())
        {
            const juce::File chosen (pendingDestinationFolder);
            pendingDestinationFolder.clear();

            // Never stat a removable path on the message thread. The detached
            // recovery and preflight workers now validate this requested root;
            // recording remains fail-closed until both have answered.
            applyDestinationFolder (chosen);

            noteActivity (ActivityLevel::Started, "Save location",
                          "Checking " + chosen.getFullPathName()
                          + " before it can be used for the next take.");
        }

        // Everything refused during the take -- a mic plugged in, an unplug,
        // a rename, a rate change -- is applied now, so the next take's plan
        // and its streams agree.
        if (captureRestartDeferred)
        {
            captureRestartDeferred = false;
            restartCapture();
        }

        // A camera unplugged while recording is deliberately held absent for
        // the rest of that take because JUCE cannot append its reconnect to the
        // finalized movie. Discovery resumes only after metadata and the
        // combined-take inputs have captured the finished take, so a remembered
        // preview may safely return for the next one.
        cameraController.refreshCameras();
        };

        if (cameraController.isFinalizingRecording())
        {
            pendingStoppedTakeCompletion = std::move (completeStoppedTake);

            // Sound has already been drained above. Drop the global REC state
            // immediately so Stop stays responsive, while retaining the take
            // paths and t=0 until the movie completion closes its metadata.
            recordingEngine.stop();
            bufferLadder.setRecording (false);
            return;
        }

        completeStoppedTake();
    }
}

bool Application::pollCameraFinalization()
{
    cameraController.pollRecordingFinalization();

    if (pendingStoppedTakeCompletion == nullptr
        || cameraController.isFinalizingRecording())
        return pendingStoppedTakeCompletion == nullptr;

    if (const auto problem = cameraController.getRecordingFinalizationProblem();
        problem.isNotEmpty())
        noteActivity (ActivityLevel::Failed, "Cameras", problem);

    // Clear the member before invoking it: completion refreshes cameras and
    // may synchronously publish callbacks, but can never execute this take a
    // second time through re-entrancy.
    auto completion = std::move (pendingStoppedTakeCompletion);
    pendingStoppedTakeCompletion = {};
    completion();
    return true;
}

bool Application::prepareToQuit()
{
    if (recordingEngine.getState() == RecordingState::Recording)
    {
        if (stopReason.isEmpty())
            stopReason = "the app was asked to quit";
        toggleRecording();
    }

    pollCameraFinalization();
    return pendingStoppedTakeCompletion == nullptr
        && ! cameraController.isFinalizingRecording();
}

juce::String Application::resolveSessionFolderName (juce::Time now, const juce::String& name) const
{
    const juce::File root (destinationFolder);
    const auto desired = baseSessionFolderName (now, name).toStdString();

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
    // This is a preview, not the creation step. Asking whether every candidate
    // exists can wedge the prompt on a stale removable mount before the async
    // recovery/preflight gates get a chance to explain the problem. At arm
    // time createSessionFolder() performs the authoritative collision check
    // and adds _2, _3, ... rather than overwriting an existing take.
    plan.folderName = baseSessionFolderName (juce::Time::getCurrentTime(), proposedSessionName);
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

std::vector<Application::SavedFile> Application::getCurrentSessionFiles (bool* snapshotAvailable) const
{
    if (snapshotAvailable != nullptr)
        *snapshotAvailable = false;

    const auto request = currentFilesystemProbeRequest();
    filesystemStatusProbe.setRequest (request);
    const auto snapshot = filesystemStatusProbe.getSnapshot();

    if (! snapshot.ready || snapshot.request != request || ! snapshot.filesObservationReady)
        return {};

    if (snapshotAvailable != nullptr)
        *snapshotAvailable = true;

    std::vector<SavedFile> files;
    files.reserve (snapshot.files.size());
    for (const auto& file : snapshot.files)
        files.push_back ({ juce::String (file.name), file.sizeBytes });
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

FilesystemStatusProbe::Request Application::currentFilesystemProbeRequest() const
{
    FilesystemStatusProbe::Request request;
    const bool isRecording = recordingEngine.getState() == RecordingState::Recording;

    // While a take is running, measure the folder actually being written, not
    // a destination setting that may have changed for the next take.
    request.destinationPath = (isRecording && currentSessionFolder.isNotEmpty()
                                ? currentSessionFolder
                                : juce::String (destinationFolder)).toStdString();
    request.sessionFolder = isRecording ? currentSessionFolder.toStdString() : std::string {};
    request.bytesPerSecond = bytesPerSecondOfRecording();

    if (capture != nullptr && capture->isMirroring())
        request.mirrorPath = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                 .getFullPathName().toStdString();

    return request;
}

int64_t Application::getMirrorFreeBytes() const
{
    const auto request = currentFilesystemProbeRequest();
    filesystemStatusProbe.setRequest (request);
    const auto snapshot = filesystemStatusProbe.getSnapshot();
    return snapshot.ready && snapshot.request == request ? snapshot.mirrorFreeBytes : -1;
}

double Application::getRemainingRecordingSeconds() const
{
    const auto request = currentFilesystemProbeRequest();
    if (request.bytesPerSecond <= 0.0)
        return -1.0;

    filesystemStatusProbe.setRequest (request);
    const auto snapshot = filesystemStatusProbe.getSnapshot();
    return snapshot.ready && snapshot.request == request ? snapshot.remainingSeconds : -1.0;
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
    // Nothing disables the button while a take is running, because the button
    // IS the stop control and it is the only thing in the app that can end a
    // take.
    //
    // Every reason below is about whether it is sensible to START. Applied
    // during a take they take the stop away: unplug every microphone mid-take
    // and the no-microphone message disabled the one control that could
    // have ended the recording, leaving the clock running with no way out but
    // quitting the app. A user who cannot stop their own take has been failed
    // more completely than by any silence.
    if (recordingEngine.getState() == RecordingState::Recording)
        return {};

    if (pendingStoppedTakeCompletion != nullptr
        || cameraController.isFinalizingRecording())
        return "Finishing the camera files from the last take. Record will be ready when they are safely closed.";

    // Ahead of the microphone count on purpose. A denied microphone permission
    // is invisible to enumeration: the count is zero for the same reason it
    // would be with nothing plugged in, and the two need different fixes.
    for (const auto& problem : PermissionGuidance::evaluate (microphonePermission,
                                                             destinationWritePermission,
                                                             ! destinationFolder.empty()))
        if (problem.blocksRecording)
            return juce::String (problem.message);

    if (getIncludedMicCount() == 0)
        return "Plug in a USB microphone or audio interface first.";

    // The microphones have to be OPEN, not merely plugged in. A rig whose
    // recording-input streams failed to open -- for example, a mic held by
    // another app -- used to leave this button live, and pressing it produced
    // a take of empty files with the clock running. Output failure falls back
    // to input-only recording and is shown separately as a monitoring warning.
    if (capture == nullptr || ! capture->isMonitoring())
    {
        const auto problem = capture != nullptr ? capture->getMonitorProblem() : std::string();
        return problem.empty() ? juce::String ("The microphones aren't open yet.")
                               : "The microphones aren't open: " + juce::String (problem);
    }

    // Recovery can repair WAV headers. Do not let a new writer create files
    // under either active write root until the corresponding one-shot scan has
    // finished. In particular, a mirror scan that was blocked in the OS must
    // never resume later and mistake this process's current take for a crashed
    // one. Finished results are consumed here, on the message thread, before
    // the button can become enabled.
    if (const auto recoveryReason = recoveryBlockingReason(); recoveryReason.isNotEmpty())
        return recoveryReason;

    // §6.4 blocks arming on a drive that is too SLOW. A drive with no room at
    // all was not checked here at all: the button stayed live, the take started
    // and was stopped by the capacity check a moment later. Refusing before the
    // take is the same principle, and it is the answer to "what tells the user
    // when the drive is full and nothing is recording".
    if (getRemainingRecordingSeconds() == 0.0)
        return "This drive is full. Free some space, or choose another drive.";

    publishCompletedPreflight();
    startPreflightIfNeeded();

    // §6.4: pre-flight blocks arming rather than degrading mid-take.
    const bool preflightRunning = publishCompletedPreflight();
    if (preflightTaskDestination == destinationFolder && preflightRunning)
        return "Checking this drive is fast enough...";

    const auto it = preflightResults.find (destinationFolder);

    // There is no safe third state between "benchmark is running" and "a
    // verdict exists". In particular, a completion which lands between two
    // separate result/running reads must not open a one-frame window where a
    // keyboard action can arm without ever evaluating the drive.
    if (it == preflightResults.end())
        return recoveryMutationGate.isActive (destinationFolder)
            ? juce::String ("A previous check is still finishing on this save location. "
                            "Choose another location, or wait for the drive to respond.")
            : juce::String ("Couldn't confirm this drive is fast enough yet. Choose another "
                            "location, or reconnect this drive and try again.");

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

    return {};
}

void Application::beginPreflightForDestination()
{
    publishCompletedPreflight();
    startPreflightIfNeeded();
}

bool Application::isPreflightRunning() const
{
    const bool running = publishCompletedPreflight();
    return preflightTaskDestination == destinationFolder && running;
}

bool Application::publishCompletedPreflight() const
{
    auto snapshot = preflightTask.poll();
    auto completed = std::move (snapshot.result);
    if (! completed.has_value())
        return snapshot.running;

    preflightResults[completed->destination] = completed->result;

    if (completed->couldNotWrite)
        noteActivity (ActivityLevel::Failed, "Save location",
                      juce::String (completed->result.reason));

    return snapshot.running;
}

void Application::startPreflightIfNeeded() const
{
    if (destinationFolder.empty() || preflightResults.count (destinationFolder) > 0)
        return;

    const auto target = destinationFolder;

    if (preflightTaskDestination == target)
    {
        if (publishCompletedPreflight())
            return;

        if (preflightResults.count (target) > 0)
            return;

        // A completed worker always publishes a result. Reaching this branch
        // means its callable threw (or its thread could not be created). Hold a
        // safe failed verdict instead of launching a new worker on every UI
        // tick or silently allowing recording without a benchmark.
        PreflightResult failed;
        failed.passed = false;
        failed.reason = "Couldn't check whether this drive is fast enough. Choose another drive, or reconnect this one and try again.";
        preflightResults[target] = failed;
        noteActivity (ActivityLevel::Failed, "Save location", juce::String (failed.reason));
        return;
    }

    // A syscall against the previous destination may never return. Abandoning
    // replaces only the shared result state; the old worker keeps ownership of
    // its files and cancellation flag, while the new safe destination starts
    // immediately and cannot receive a late old result.
    if (! preflightTaskDestination.empty())
        preflightTask.abandon();

    preflightTaskDestination = target;
    const int channelCount = std::max (1, getIncludedMicCount());
    const double sampleRate = currentSampleRate;
    const int bytesPerSample = std::max (1, currentBitDepth / 8);
    const auto mutationGate = recoveryMutationGate;

    const bool launched = preflightTask.start (
        [target, channelCount, sampleRate, bytesPerSample,
         mutationGate]
        (const std::atomic<bool>& cancelled) mutable
        {
            // Preflight creates, flushes and removes a 200 MB file.
            // Cancellation suppresses publication but cannot pull a wedged
            // syscall off the kernel. Resolve aliases and acquire the shared
            // recovery/preflight lease on this detached worker, so reselecting
            // the same physical volume under another spelling cannot race it.
            auto mutationLease = waitForMutationLease (
                mutationGate, { target }, cancelled);
            if (mutationLease == nullptr)
                return PreflightBackgroundResult { target, {}, false };

            auto result = Application::runPreflight (target, channelCount, sampleRate,
                                                     bytesPerSample, cancelled);
            // Release the path before DetachedResultTask publishes completion.
            // A caller which atomically sees running=false plus this result can
            // therefore authorize the measured root immediately and safely.
            mutationLease.reset();
            return result;
        });

    if (! launched)
    {
        PreflightResult failed;
        failed.passed = false;
        failed.reason = "Couldn't start the drive speed check. Choose another drive, or restart SobStage and try again.";
        preflightResults[target] = failed;
        noteActivity (ActivityLevel::Failed, "Save location", juce::String (failed.reason));
    }
}

Application::PreflightBackgroundResult Application::runPreflight (
    std::string destination, int channelCount, double sampleRate,
    int bytesPerSample, const std::atomic<bool>& cancelled)
{
    PreflightBackgroundResult completed;
    completed.destination = std::move (destination);
    PreflightResult result;

    const juce::File folder { juce::String (completed.destination) };
    folder.createDirectory();

    if (cancelled.load (std::memory_order_acquire))
        return completed;

    const auto testFile = folder.getNonexistentChildFile ("preflight", ".tmp");

    std::vector<double> rollingWindows;

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
                if (cancelled.load (std::memory_order_acquire))
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

                const auto elapsedBeforeFlush = std::chrono::duration<double> (
                    std::chrono::steady_clock::now() - windowStart).count();

                if (elapsedBeforeFlush >= 1.0)
                {
                    // FileOutputStream::flush() reaches the platform durability
                    // primitive (fsync/FlushFileBuffers). Include that latency in
                    // every rolling window so a large OS cache cannot make a slow
                    // card look safe for a long recording.
                    out.flush();

                    if (out.getStatus().failed())
                    {
                        couldNotWrite = true;
                        break;
                    }

                    const auto durableElapsed = std::chrono::duration<double> (
                        std::chrono::steady_clock::now() - windowStart).count();
                    rollingWindows.push_back (static_cast<double> (writtenThisWindow) / durableElapsed);
                    writtenThisWindow = 0;
                    windowStart = std::chrono::steady_clock::now();
                }
            }

            if (! cancelled.load (std::memory_order_acquire))
                out.flush();

            // FileOutputStream::flush() returns void; the stream carries the
            // outcome instead. A flush that failed means the bytes counted as
            // written above never reached the card, so the windows measured
            // from them describe nothing.
            if (! cancelled.load (std::memory_order_acquire) && out.getStatus().failed())
                couldNotWrite = true;

            // Include the final partial window too. This gives a fast card its
            // only sample and makes the last durability flush part of the
            // sustained-floor verdict for every other card.
            if (! cancelled.load (std::memory_order_acquire)
                && ! couldNotWrite && writtenThisWindow > 0)
            {
                const auto elapsed = std::chrono::duration<double> (
                    std::chrono::steady_clock::now() - windowStart).count();

                if (elapsed > 0.0)
                    rollingWindows.push_back (static_cast<double> (writtenThisWindow) / elapsed);
            }
        }
    }

    testFile.deleteFile();

    // An abandoned run proved nothing. DetachedResultTask suppresses this
    // return after cancellation, but avoiding the calculation makes that
    // ownership contract explicit and keeps the worker independent of the
    // Application that launched it.
    if (cancelled.load (std::memory_order_acquire))
        return completed;

    // What is kept from this is the measurement. The pass/fail and the wording
    // alongside it are a snapshot of the rig as it was during the benchmark;
    // getRecordDisabledReason() reapplies the gate to the current rig.
    result = PreflightThroughputTest::evaluate (rollingWindows, channelCount,
                                                sampleRate, bytesPerSample);

    if (couldNotWrite)
    {
        result.passed = false;
        result.reason = "Couldn't write to this card, so takes can't be saved here. Check it "
                         "is plugged in, has room, and isn't locked.";
    }

    completed.result = std::move (result);
    completed.couldNotWrite = couldNotWrite;
    return completed;
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

    const auto found = outputDeviceIdByLabel.find (displayName.toStdString());
    if (found == outputDeviceIdByLabel.end())
        return;

    // §5.3: an explicit choice is remembered and outranks the automatic
    // priority order from then on, including after the app is relaunched.
    rememberedOutputDeviceId = found->second;
    saveSettings();
    reselectOutputDevice();
    restartCapture();
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
    if (folder.getFullPathName().trim().isEmpty())
        return;

    // §6.5 fixes where a take is going for its duration, and nothing in the UI
    // stopped this being changed in the middle of one. The take kept writing to
    // the folder it opened with, so the setting and the recording disagreed --
    // and the change also cleared the agreed save location and kicked off a
    // 200 MB benchmark write while a take was running, neither of which was
    // announced.
    //
    // Deferred rather than refused, the same way a mic change mid-take is
    // (requestCaptureRestart): the user asked for it, so it happens, at the
    // first moment it can happen without touching a take.
    if (recordingEngine.getState() == RecordingState::Recording)
    {
        pendingDestinationFolder = folder.getFullPathName();

        noteActivity (ActivityLevel::Warning, "Save location",
                      "This take is still being written to where it started. Takes will be saved "
                      "to " + folder.getFullPathName() + " from the next one.");
        return;
    }

    applyDestinationFolder (folder);
}

void Application::applyDestinationFolder (const juce::File& folder)
{
    destinationFolder = folder.getFullPathName().toStdString();

    // §10.1: the user agreed to a place, not to a setting. Somewhere else has
    // not been agreed to, so it gets asked about before the next take.
    if (confirmedSaveLocation != destinationFolder)
        confirmedSaveLocation.clear();

    // Re-selecting the same mount path can still mean a different physical
    // card now occupies it. Retire that path's previous safety fact so the new
    // medium gets its own recovery scan; a stuck old worker keeps only its old
    // cancelled shared state and is never joined.
    if (destinationRecoveryRoot == destinationFolder)
    {
        destinationRecoveryTask.abandon();
        destinationRecoveryRoot.clear();
        destinationRecoveryStatus = RecoveryScanStatus::NotStarted;
    }

    // Replace a possibly stuck old-volume scan with a fresh shared state. The
    // old worker owns its old state until its syscall returns, so it cannot
    // publish against this destination or hold the new one behind its gate.
    startDestinationRecoveryScan();

    // §6.4: selecting a volume is a fresh claim about what is mounted at this
    // path now. A different card can later reuse the same /Volumes name, so a
    // throughput pass cached for the previous occupant must not arm it. A
    // worker already stuck on this exact path is abandoned, never joined.
    preflightResults.erase (destinationFolder);
    if (preflightTaskDestination == destinationFolder)
    {
        preflightTask.abandon();
        preflightTaskDestination.clear();
    }

    beginPreflightForDestination();

    // §10.4. There is no query API for this on macOS; the only truthful answer
    // comes from trying, so it is asked here -- once, when the location
    // changes -- rather than anywhere near arming or the audio path.
    destinationWritePermission = queryVolumeWritePermission (destinationFolder);
    journalledPermissionProblems = false;

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
    meta.appVersion = appVersionString().toStdString();
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

    // Only camera writers which actually started belong in session.json. The
    // watchdog separately keeps the complete intended roster so it can report
    // a missing capture card, but inventing that card's movie filename here
    // would make an editor look for a file which never existed.
    for (const auto& video : cameraController.getTakeVideoRecords())
        meta.videos.push_back ({ video.displayName, video.fileName, false });

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
    // over and the backend could not carry.
    // §0.1: audio this app received and threw away because the ring was full.
    // The take's record carried the writer's drops, the layout's and the
    // backend's, and not this one -- so the one loss the app inflicts on itself
    // was the one the record did not mention.
    if (capture != nullptr)
    {
        const auto overrun = capture->getOverrunSamplesThisTake();

        if (overrun > 0)
            meta.dropouts.push_back ({ getElapsedRecordingSeconds(), std::string(),
                                       "Dropped " + std::to_string (overrun)
                                           + " samples: audio arrived faster than it could be "
                                             "taken away, so the buffer overflowed." });
    }

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
    if (! replaceWithTextChecked (juce::File (currentSessionFolder).getChildFile ("session.json"), juce::String (json)))
        noteActivity (ActivityLevel::Warning, "Recording",
                      "Couldn't write the details file for this take. The audio itself is saved.");

    // The mirror's copy is checked too. §6.3 makes each copy stand alone, and a
    // backup whose record failed to write is a folder of audio with no account
    // of how the take went -- which is the half of the pair the user reaches
    // for precisely when the card's copy is the one that went wrong.
    if (currentMirrorFolder.isNotEmpty()
        && ! replaceWithTextChecked (juce::File (currentMirrorFolder).getChildFile ("session.json"), juce::String (json)))
        noteActivity (ActivityLevel::Warning, "Local backup",
                      "Couldn't write the details file into the backup copy. The backed-up audio "
                      "itself is there.");

    writeActivityLog (juce::File (currentSessionFolder));

    if (currentMirrorFolder.isNotEmpty())
        writeActivityLog (juce::File (currentMirrorFolder));
}

void Application::writeActivityLog (const juce::File& folder) const
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
    if (! replaceWithTextChecked (folder.getChildFile ("activity.log"), text))
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

void Application::announceArrivalsAndDepartures (const std::map<std::string, std::string>& current,
                                                 std::map<std::string, std::string>& known,
                                                 bool& seeded,
                                                 const std::set<std::string>& saidElsewhere) const
{
    // The first enumeration is the rig as the user set it up, not a series of
    // arrivals. "N microphones are live" covers the launch.
    if (! seeded)
    {
        seeded = true;
        known = current;
        return;
    }

    for (const auto& [key, name] : current)
        if (known.find (key) == known.end())
            noteActivity (ActivityLevel::Started, juce::String (name),
                          juce::String (name) + " is connected.", true);

    for (const auto& [key, name] : known)
        if (current.find (key) == current.end() && saidElsewhere.count (key) == 0)
            noteActivity (ActivityLevel::Warning, juce::String (name),
                          juce::String (name) + " was unplugged.", true);

    known = current;
}

void Application::announceDeviceChanges (const std::vector<MicDeviceState>& seen) const
{
    std::map<std::string, std::string> current;

    for (const auto& d : seen)
        current[d.identity.key()] = d.displayName;

    // Whether a take is running decides only ONE thing here: a microphone that
    // is part of the take gets §6.5's own sentence when it goes, which says
    // what happens to its track, so saying "unplugged" beside it would be the
    // same news twice. Everything else is announced either way -- a
    // microphone arriving or leaving between takes is exactly as much a fact
    // about the rig as one arriving or leaving during one.
    std::set<std::string> takeChannels;

    if (capture != nullptr && capture->isRecording())
        for (const auto& ch : capture->getChannels())
            takeChannels.insert (ch.deviceId);

    announceArrivalsAndDepartures (current, knownDeviceNames, haveEnumeratedDevicesOnce, takeChannels);
}

void Application::announceOutputChanges (const std::map<std::string, std::string>& current) const
{
    announceArrivalsAndDepartures (current, knownOutputNames, haveAnnouncedOutputsOnce, {});
}

void Application::announceCameraChanges() const
{
    std::map<std::string, std::string> current;

    for (const auto& cam : cameraController.getSelection().getAvailableCameras())
        current[cam.id] = cam.displayName;

    // A camera that began the running take gets TakeWatchdog's own sentence
    // when it goes, so it is not also announced here. Use the frozen take
    // roster rather than the live OS list: the lost camera is absent from that
    // list at exactly the moment duplicate suppression matters.
    std::set<std::string> saidByTheWatchdog;

    if (recordingEngine.getState() == RecordingState::Recording)
        for (const auto& cam : cameraController.getTakeCameraStates())
            saidByTheWatchdog.insert (cam.id);

    announceArrivalsAndDepartures (current, knownCameraNames, haveAnnouncedCamerasOnce,
                                   saidByTheWatchdog);
}

void Application::noteActivity (ActivityLevel level, const juce::String& subject,
                                const juce::String& message, bool onTheLine) const
{
    // A repeat that only bumps a count is not news for the two append-only
    // places below. Without this, one failure repeating twice a second wrote
    // its sentence to log.txt every time -- 207 identical lines in 40 seconds
    // when a destination went away -- and rewrote the take's log just as often.
    if (! activity.note (activityClockSeconds(), level, subject.toStdString(), message.toStdString(),
                         onTheLine))
        return;

    // Into the app's own log as well, which until now held one line per launch
    // -- "Starting up." -- and nothing else, for the whole life of the app. It
    // is the file the diagnostics bundle ships to whoever is helping, and the
    // only account that survives when the take folder does not: pull the card
    // mid-take and the take's own log goes with it, leaving the one file that
    // was still readable saying nothing at all.
    juce::Logger::writeToLog (juce::Time::getCurrentTime().toString (true, true)
                              + "  [" + juce::String (ActivityJournal::levelName (level)) + "] "
                              + subject + ": " + message);

    // Straight to disk while a take is running. The journal lives in memory,
    // and the only writes were at the start of the take -- before the "started"
    // note even existed -- and at the stop. So a take that ended in a crash or
    // a power cut, the take this log exists for, was recovered next to a log
    // that said nothing had happened: not that recording started, not the
    // drive-space warning that came ten minutes before the end.
    flushActivityLogToTake();
}

void Application::flushActivityLogToTake() const
{
    // The message thread owns the take's folder paths, while some device
    // notifications can record activity from OS threads. Reading those
    // juce::Strings there, and writing the same two files from two threads at
    // once, is a race; the journal itself is locked, so the entry is already
    // safely recorded and only the file write is deferred.
    if (! juce::MessageManager::existsAndIsCurrentThread())
    {
        const auto alive = getAliveToken();

        juce::MessageManager::callAsync ([this, alive]
        {
            if (alive.lock() != nullptr)
                flushActivityLogToTake();
        });

        return;
    }

    // writeActivityLog reports its own failure through noteActivity, which
    // lands back here. Once is a report; twice is a loop.
    if (writingActivityLog || currentSessionFolder.isEmpty())
        return;

    writingActivityLog = true;
    writeActivityLog (juce::File (currentSessionFolder));

    if (currentMirrorFolder.isNotEmpty())
        writeActivityLog (juce::File (currentMirrorFolder));

    writingActivityLog = false;
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
    // A fresh two-channel device is monitored as two exact physical inputs
    // until its analyzer reaches a verdict. Applying that answer here keeps
    // persistence and stream rebuilding off the audio thread; the helper also
    // refuses to do either while a take is running.
    applyChannelLayoutDecisions();

    // Cleared first, not inside the tap block: a capacity or performance
    // warning returns early below, and a stale index would leave one skull
    // lit indefinitely.
    tappedChannel = -1;

    // §10.6: the blocking problems already gate the record button, but a
    // save-location denial does not block anything -- it just means the take
    // will fail later -- so it has to reach the user some other way. The
    // journal is that way, and it also puts the reason in the take's own log.
    if (! journalledPermissionProblems)
    {
        const auto permissionProblems =
            PermissionGuidance::evaluate (microphonePermission, destinationWritePermission,
                                          ! destinationFolder.empty());

        journalledPermissionProblems = true;

        for (const auto& problem : permissionProblems)
            noteActivity (problem.blocksRecording ? ActivityLevel::Failed : ActivityLevel::Warning,
                          problem.kind == PermissionKind::Microphone ? "Microphones"
                                                                     : "Save location",
                          juce::String (problem.message));
    }

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
        const auto& driftChannels = capture->getChannels();

        for (int i = 0; i < micCount; ++i)
        {
            if (! capture->hasSustainedExcessDrift (i))
                continue;

            // Not about a microphone that has been unplugged. Its stream is
            // gone, so whatever the drift figure still says is about a device
            // that is not there -- and "try a different USB port" is advice
            // for a microphone the user has already pulled out. Seen for real:
            // a mic unplugged mid-take went on filing sync warnings under its
            // own name for the rest of the take.
            if (static_cast<size_t> (i) < driftChannels.size()
                && recordingEngine.isWritingSilence (driftChannels[static_cast<size_t> (i)].deviceId))
                continue;

            noteActivity (ActivityLevel::Warning, getMicDisplayName (i),
                          juce::String (getMicDisplayName (i))
                          + " can't keep steady time with the others. Try a different USB port.");
        }

        // §10.5: a channel that stays silent while the others are working is
        // the single most common failure on a live rig -- usually the mute
        // switch on the microphone itself -- and it reached the screen and
        // nowhere else. A take where someone's microphone was muted right
        // through it left nothing in the record to say so, which is the same
        // silence-nobody-warned-about this app exists to prevent. noteActivity
        // already collapses the repeat, so this can run on the timer beside the
        // drift check above.
        for (const auto& advice : getSetupAdvice())
        {
            // Two of the advisor's findings are about something being WRONG
            // with the rig for the whole take rather than about how to set it
            // up better, and both reached the screen and nowhere else. A take
            // recorded on an underpowered hub, or with someone's microphone
            // muted right through it, should carry the reason in its own
            // record; the rest of the advice is guidance and would only be
            // noise in a log.
            if (advice.issue != SetupIssue::SilentChannel
                && advice.issue != SetupIssue::BusPowerExhausted)
                continue;

            const auto who = advice.channelIndex >= 0
                           ? juce::String (getMicDisplayName (advice.channelIndex))
                           : juce::String ("Microphones");

            noteActivity (ActivityLevel::Warning, who, juce::String (advice.message));
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
        stopReason = "the card stopped accepting writes";
        toggleRecording();

        // The writer's own account when it has one -- a roll-over past 3.9 GB
        // that could not create the next file, or a drive that is simply full
        // -- is not "the card was removed", and sending someone to check a
        // cable for a card that is simply full wastes the one moment they are
        // still next to the rig.
        //
        // It now LEADS rather than being journalled underneath the removal
        // notice and then contradicted by it. The headline is the sentence the
        // user acts on, and it went on saying the drive had stopped responding
        // and to check it was plugged in properly even when the writer knew
        // perfectly well that the drive was merely full. The guard above was
        // written for exactly this and could not fire, because an ordinary
        // failed write left no account at all -- only the roll-over path set
        // one.
        auto headline = juce::String (cardRemovalNotice.message);

        if (const auto why = capture->getCardWriteProblem(); ! why.empty())
        {
            headline = juce::String (why);

            // §6.5: pointing at the surviving copy is the whole point of the
            // notice's second sentence, so it outlives the swap.
            if (cardRemovalNotice.aCompleteCopySurvives)
                headline += " A complete backup copy is safe on this computer: "
                          + juce::String (cardRemovalNotice.survivingFolder);
        }

        noteActivity (ActivityLevel::Failed, "Card", headline);
        return headline;
    }

    // §6.5: the drive is full, so the take stops -- here, before any of the
    // warning branches below.
    //
    // The stop used to sit at the bottom, beneath five branches that return as
    // soon as their condition holds and keep holding it: backend drops, layout
    // losses, monitor glitches, mirror failures, ring fill. A rig with any one
    // of those persisting therefore never reached the capacity check, and the
    // take was NOT stopped on a full drive -- which is precisely the failure
    // the "said AND done" fix was written to prevent. The card-removal stop was
    // already hoisted for the same reason; this one was left behind.
    //
    // Only the stop is hoisted, and it is decided from the raw remaining figure
    // rather than from CapacityMonitor -- because evaluateRemaining LATCHES.
    // Evaluating it here and reusing the answer below swapped one bug for its
    // mirror image: the latch would be spent on every poll, including the ones
    // where a branch further down returns first, so a ring sitting at 50% fill
    // -- which returns on every single poll -- would permanently swallow the
    // ten- and two-minute warnings. Silencing the two warnings whose whole job
    // is to give the user time to act, inside a change about not being silent.
    //
    // The stop does not need the latch: it needs to know whether the room ran
    // out, which getRemainingRecordingSeconds answers as often as it is asked.
    // The latching call stays at the bottom, where its answer is delivered.
    if (recordingEngine.getState() == RecordingState::Recording)
    {
        const auto remaining = getRemainingRecordingSeconds();

        // Zero means the volume was asked and has nothing left. Negative means
        // it could not be asked, which is not the same thing -- reading that as
        // empty would stop a take over a drive whose free space the OS declined
        // to report. Same test CapacityMonitor applies, on a figure that can
        // now actually take the value.
        const bool roomRanOut = remaining == 0.0;

        if (roomRanOut)
        {
            stopReason = "the drive ran out of room";
            toggleRecording();

            const auto line = juce::String ("The drive is full. Recording has stopped -- free some "
                                            "space or choose another drive.");

            noteActivity (ActivityLevel::Failed, "Drive", line);
            return line;
        }
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
        juce::String firstWarning;

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

            auto line = subject + " " + juce::String (failure.reason);
            const bool stopForSafety = streamFailureRequiresRecordingStop (failure.kind)
                                    && recordingEngine.getState() == RecordingState::Recording;

            if (stopForSafety)
                line += " Recording stopped before its file format could become untrue.";

            if (streamFailureIsWarning (failure.kind))
            {
                noteActivity (ActivityLevel::Warning, subject, line);
                if (firstWarning.isEmpty())
                    firstWarning = line;
            }
            else
            {
                // Journal the hardware failure while the take folder is still
                // active, then use the ordinary stop/finalize path. This puts
                // both the cause and the stop in activity.log and ensures a
                // changed CoreAudio rate cannot leave a WAV header describing
                // samples captured under another clock.
                noteActivity (ActivityLevel::Failed, subject, line);
                if (firstFailure.isEmpty())
                    firstFailure = line;
            }

            if (stopForSafety)
            {
                stopReason = "an audio device changed sample rate";
                toggleRecording();
            }
        }

        if (firstFailure.isNotEmpty())
            return firstFailure;
        if (firstWarning.isNotEmpty())
            return firstWarning;
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

    // §0.1: a microphone that changed its channel count mid-take. Reported
    // live, like the drive-side and backend-side losses -- this one only ever
    // reached session.json, so audio was being lost during a take with nothing
    // on screen saying so, which is the silence this whole thing is about.
    if (capture != nullptr && capture->isRecording())
    {
        const auto missed = capture->getFramesMissedByLayout();

        // The same backwards guard its siblings carry. The counter is only
        // zeroed by startRecording today, in the same call that zeroes this --
        // but being the one member of the family without the guard is how the
        // next person introduces the bug the others already had.
        if (missed < reportedLayoutMisses)
            reportedLayoutMisses = 0;

        if (missed > reportedLayoutMisses)
        {
            reportedLayoutMisses = missed;

            // Worded for what every path here has in common -- a device is
            // delivering audio this take cannot fit -- rather than asserting
            // the channel-count diagnosis, which is true of one of the four
            // ways this counter rises and wrong advice for the other three.
            const auto line = juce::String ("A microphone is sending audio this take can't fit, so "
                                            "some of it isn't being recorded. Unplug it and plug it "
                                            "back in after this take.");

            noteActivity (ActivityLevel::Failed, "Recording", line);
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
        // The mirror lives on the computer's own disk, so "that drive stopped
        // accepting writes" is almost always "that disk is full" -- and the
        // difference is the one thing the user can act on. The generic sentence
        // stays for anything the writer could not account for.
        //
        // Worded here rather than borrowed from the writer: the writer's
        // sentence is written for the card and says recording has stopped and
        // every file has been closed, which is true of the card and alarming
        // nonsense about a backup, whose failure stops no recording at all.
        const auto line = capture->mirrorRanOutOfSpace()
            ? juce::String ("The local backup copy stopped -- this computer's disk is full. "
                            "The recording itself is unaffected and is still going to the "
                            "card. Free up space to have a backup again.")
            : juce::String ("The local backup copy stopped -- that drive stopped accepting "
                            "writes. The recording itself is unaffected and is still going "
                            "to the card.");

        noteActivity (ActivityLevel::Failed, "Local backup", line);
        return line;
    }

    // §6.3: the mirror is re-judged during the take, and once it stops it never
    // restarts within the same recording.
    if (capture != nullptr && capture->isMirroring())
    {
        const auto freeBytes = getMirrorFreeBytes();

        if (freeBytes >= 0 && mirrorPolicy.evaluateDuringRecording (freeBytes)
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
        // The mirror is fed by this same ring and writer thread. It protects a
        // take from one destination failing, not from this queue overflowing,
        // so its presence must never suppress the 90% mix-only safety step.
        switch (capacityMonitor.evaluateFill (capture->getRingFillFraction()))
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
            // Kept only so the switch stays exhaustive. A running take that is
            // out of room is stopped and returned from at the top of this
            // function, and pollCapacityWarning returns None when no take is
            // running -- so nothing reaches here. A full drive with no take
            // running is the record button's job, not this line's.
            break;
        case RemainingTimeWarning::TwoMinutes:
            return "About two minutes of room left. Wrap up or switch drives now.";
        case RemainingTimeWarning::TenMinutes:
            return "About ten minutes of room left on this drive.";
        case RemainingTimeWarning::None:
            break;
    }

    // §6.6 next: warned before it causes dropouts, not after.
    const auto load = capture != nullptr ? capture->getAudioCallbackLoad() : 0.0;

    switch (updatePerformance (load, isSystemThermallyThrottled()))
    {
        case PerformanceWarning::ThermalThrottling:
        {
            const auto line = juce::String (
                "This machine is overheating and is about to drop audio. Close other apps.");
            noteActivity (ActivityLevel::Warning, "Performance", line);
            return line;
        }
        case PerformanceWarning::SustainedCpuPressure:
        {
            const auto line = juce::String (
                "This machine is working hard. Close other apps before it starts dropping audio.");
            noteActivity (ActivityLevel::Warning, "Performance", line);
            return line;
        }
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
        // for the things that need acting on, plus the few things the journal
        // marks as belonging on the screen: a microphone arriving is not a
        // warning, and the user still wants it confirmed the moment it lands.
        if (unseen.onTheLine || unseen.level == ActivityLevel::Warning
            || unseen.level == ActivityLevel::Failed)
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
    summary->setProperty ("appVersion", appVersionString());
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
    if (! replaceWithTextChecked (tempInventory.getFile(), juce::JSON::toString (juce::var (summary), true)))
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

    // §10.1: preserve the user's remembered destination without touching it
    // on the launch thread. Even a seemingly harmless isDirectory() can wait
    // indefinitely on a stale removable or network volume. The detached
    // recovery/preflight/status workers will establish its current condition,
    // and the user can switch to the always-available default immediately.
    if (! rememberedSettings.destinationFolder.empty())
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
    rememberedOutputDeviceId = rememberedSettings.rememberedOutputDeviceId;
    cameraController.setPreviewQuality (rememberedSettings.cameraPreviewFullQuality
                                            ? PreviewQuality::Full : PreviewQuality::Low);

    // Camera discovery now runs away from the message thread, so the first OS
    // snapshot may land after initialise() has returned. Seed choices by id
    // now; CameraSelection intentionally keeps choices for unavailable cameras
    // and applies them when that id appears.
    for (const auto& camera : rememberedSettings.cameras)
    {
        cameraController.getSelection().setEnabled (camera.id, camera.enabled);
        if (! camera.assignedName.empty())
            cameraController.getSelection().setAssignedName (camera.id, camera.assignedName);
    }
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
    settings.rememberedOutputDeviceId = rememberedOutputDeviceId;
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

    // Every known camera, not only the connected ones: a camera unplugged
    // today should come back tomorrow as it was left, while changing the
    // toggle on its unavailable row must replace (not resurrect) old state.
    const auto& selection = cameraController.getSelection();
    for (const auto& camera : selection.getKnownCameras())
        settings.cameras.push_back ({ camera.id,
                                      selection.isEnabled (camera.id),
                                      selection.getDisplayName (camera.id) });

    const auto file = getSettingsFile();
    file.getParentDirectory().createDirectory();

    // Checked, because silently failing here presents next launch as an app
    // that forgot the user's microphone names, trims and destination -- and
    // there is no moment at which that is explained.
    if (! replaceWithTextChecked (file, juce::String (settings.toJsonString())))
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
    publishCompletedRecoveryAcknowledgement();

    std::vector<std::string> sessionFolders;
    sessionFolders.reserve (recoveredSessions.size());

    for (const auto& session : recoveredSessions)
        sessionFolders.push_back (session.folder);

    std::vector<std::string> mutationRoots;
    mutationRoots.reserve (sessionFolders.size());
    for (const auto& folder : sessionFolders)
        mutationRoots.push_back (
            juce::File (juce::String (folder)).getParentDirectory()
                .getFullPathName().toStdString());

    // Dismiss the card before touching any path it names. A recovered take may
    // live on the same removable volume whose disappearance interrupted it;
    // even existsAsFile() or loadFileAsString() can then wait indefinitely.
    recoveredSessions.clear();

    if (sessionFolders.empty())
        return;

    // The rule for "interrupted" is an empty stop timestamp, and repairing
    // the headers never wrote one -- so the same takes came back at every
    // launch. The worker stamps them with this acknowledgement time. It owns
    // every folder string and never captures Application; a stuck OS call can
    // therefore outlive this object without holding up the UI or shutdown.
    const auto recoveredAt = juce::Time::getCurrentTime().toISO8601 (true).toStdString();
    const auto mutationGate = recoveryMutationGate;
    const bool launched = recoveryAcknowledgementTask.start (
        [sessionFolders = std::move (sessionFolders), recoveredAt,
         mutationRoots = std::move (mutationRoots), mutationGate]
        (const std::atomic<bool>& cancelled) mutable
        {
            auto mutationLease = waitForMutationLease (
                mutationGate, mutationRoots, cancelled);
            if (mutationLease == nullptr)
                return RecoveryAcknowledgementResult {};

            auto result = Application::runRecoveryAcknowledgement (
                std::move (sessionFolders), recoveredAt, cancelled);
            mutationLease.reset();
            return result;
        });

    if (! launched)
        noteActivity (ActivityLevel::Warning, "Interrupted take",
                      "Couldn't mark the recovered takes as dealt with. No recording was "
                      "changed, so they may be offered again next time SobStage opens.");
}

Application::RecoveryAcknowledgementResult Application::runRecoveryAcknowledgement (
    std::vector<std::string> sessionFolders, std::string recoveredAt,
    const std::atomic<bool>& cancelled)
{
    RecoveryAcknowledgementResult result;

    const auto wasCancelled = [&cancelled]
    {
        return cancelled.load (std::memory_order_acquire);
    };

    for (const auto& folderPath : sessionFolders)
    {
        if (wasCancelled())
            break;

        const auto file = juce::File (juce::String (folderPath)).getChildFile ("session.json");

        // Every filesystem call stays on this detached worker. Check
        // cancellation after each one so a worker released from a stale mount
        // after shutdown cannot proceed to the next read or mutate a file.
        if (! file.existsAsFile() || wasCancelled())
            continue;

        try
        {
            const auto contents = file.loadFileAsString().toStdString();

            if (wasCancelled())
                break;

            auto meta = SessionMetadata::fromJsonString (contents);

            if (meta.stopTimestampIso.empty())
            {
                meta.stopTimestampIso = recoveredAt;

                if (wasCancelled())
                    break;

                // Checked. This is the write that stops a recovered take being
                // offered again at every launch. A read-only/full/disappearing
                // card leaves the metadata untouched and the take is safely
                // offered again next launch.
                if (! replaceWithTextChecked (file, juce::String (meta.toJsonString()))
                    && ! wasCancelled())
                {
                    result.activity.push_back (
                        { ActivityLevel::Warning,
                          "Interrupted take",
                          (file.getParentDirectory().getFileName()
                           + " can't be marked as dealt with -- this card won't accept the "
                             "change, so it will be offered again next time.").toStdString() });
                }
            }
        }
        catch (...)
        {
            // Unreadable metadata stays as it is and is offered again. Do not
            // risk replacing a file whose contents could not be understood.
        }
    }

    return result;
}

void Application::publishCompletedRecoveryAcknowledgement() const
{
    auto completed = recoveryAcknowledgementTask.takeResult();

    if (! completed.has_value())
        return;

    // Only the message thread calls this publisher. The detached worker owns
    // plain result values and never reaches into the journal or Application.
    for (const auto& entry : completed->activity)
        noteActivity (entry.level, juce::String (entry.subject), juce::String (entry.message));
}

void Application::scanForInterruptedSessions()
{
    recoveredSessions.clear();

    // Separate tasks matter here. A destination can be replaced while its OS
    // call is stuck, and mirroring can be switched off to escape a stuck local
    // mirror probe. Neither root is allowed to hold the other behind one
    // worker, and neither detached worker ever captures Application.
    startDestinationRecoveryScan();
    startMirrorRecoveryScan();
}

void Application::startDestinationRecoveryScan()
{
    // A shared destination/mirror root may currently be represented by the
    // destination task alone. Consume a just-finished result before changing
    // either root so that safety fact can follow the mirror when the roots
    // split again.
    publishCompletedRecoveryScans();

    if (destinationFolder.empty())
    {
        destinationRecoveryStatus = RecoveryScanStatus::Failed;
        return;
    }

    const auto target = juce::File (juce::String (destinationFolder))
                            .getFullPathName().toStdString();

    if (target == destinationRecoveryRoot)
        return;

    const bool destinationOwnedSharedRoot = ! destinationRecoveryRoot.empty()
        && destinationRecoveryRoot == mirrorRecoveryRoot
        && destinationRecoveryStatus != RecoveryScanStatus::NotStarted
        && mirrorRecoveryStatus == RecoveryScanStatus::NotStarted;

    if (destinationOwnedSharedRoot)
    {
        // A finished scan proves the same root safe (or unsafe) for the mirror.
        // A running scan is about to be abandoned, so the mirror must launch
        // its own replacement below rather than waiting on a task it no longer
        // owns.
        mirrorRecoveryStatus = destinationRecoveryStatus == RecoveryScanStatus::Running
            ? RecoveryScanStatus::NotStarted
            : destinationRecoveryStatus;
    }

    if (! destinationRecoveryRoot.empty())
        destinationRecoveryTask.abandon();

    destinationRecoveryRoot = target;
    destinationRecoveryStatus = RecoveryScanStatus::NotStarted;

    // If the user's destination is the mirror root itself, the existing scan
    // already establishes the same safety fact. Do not race two header repairs
    // over one directory.
    if (target == mirrorRecoveryRoot
        && (mirrorRecoveryStatus != RecoveryScanStatus::NotStarted
            || mirrorRecoveryTask.isRunning()))
        return;

    const auto mutationGate = recoveryMutationGate;
    const bool launched = destinationRecoveryTask.start (
        [target, mutationGate] (const std::atomic<bool>& cancelled) mutable
        {
            auto mutationLease = waitForMutationLease (
                mutationGate, { target }, cancelled);
            if (mutationLease == nullptr)
                return RecoveryBackgroundResult { target, true, {}, {} };

            auto result = Application::runRecoveryScan (target, true, cancelled);
            mutationLease.reset();
            return result;
        });

    if (! launched)
    {
        destinationRecoveryStatus = RecoveryScanStatus::Failed;
        noteActivity (ActivityLevel::Warning, "Interrupted take",
                      "Couldn't start the interrupted-take check for this save location. "
                      "No files were changed; restart SobStage before recording there.");
    }
    else
    {
        destinationRecoveryStatus = RecoveryScanStatus::Running;
    }

    // If changing the destination split a once-shared root while its only scan
    // was still running, immediately give the mirror an independent task. The
    // old detached worker is cancelled and cannot publish, but is never joined.
    if (! mirrorRecoveryRoot.empty()
        && mirrorRecoveryRoot != destinationRecoveryRoot
        && mirrorRecoveryStatus == RecoveryScanStatus::NotStarted)
        startMirrorRecoveryScan();
}

void Application::startMirrorRecoveryScan()
{
    const auto target = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                            .getChildFile ("RECORDINGS-MIRROR")
                            .getFullPathName().toStdString();

    if (target == mirrorRecoveryRoot)
    {
        if (mirrorRecoveryStatus != RecoveryScanStatus::NotStarted
            || target == destinationRecoveryRoot)
            return;
    }
    else
    {
        if (! mirrorRecoveryRoot.empty())
            mirrorRecoveryTask.abandon();

        mirrorRecoveryRoot = target;
        mirrorRecoveryStatus = RecoveryScanStatus::NotStarted;
    }

    if (target == destinationRecoveryRoot)
        return;

    const auto mutationGate = recoveryMutationGate;
    const bool launched = mirrorRecoveryTask.start (
        [target, mutationGate] (const std::atomic<bool>& cancelled) mutable
        {
            auto mutationLease = waitForMutationLease (
                mutationGate, { target }, cancelled);
            if (mutationLease == nullptr)
                return RecoveryBackgroundResult { target, false, {}, {} };

            auto result = Application::runRecoveryScan (target, false, cancelled);
            mutationLease.reset();
            return result;
        });

    if (! launched)
    {
        mirrorRecoveryStatus = RecoveryScanStatus::Failed;
        noteActivity (ActivityLevel::Warning, "Interrupted take",
                      "Couldn't start the interrupted-take check for the local backup folder. "
                      "No files were changed; restart SobStage before using local backup.");
    }
    else
    {
        mirrorRecoveryStatus = RecoveryScanStatus::Running;
    }
}

Application::RecoveryBackgroundResult Application::runRecoveryScan (
    std::string rootPath, bool isDestinationCopy,
    const std::atomic<bool>& cancelled)
{
    RecoveryBackgroundResult result;
    result.root = std::move (rootPath);
    result.isDestinationCopy = isDestinationCopy;

    const auto wasCancelled = [&cancelled]
    {
        return cancelled.load (std::memory_order_acquire);
    };

    const auto report = [&result] (ActivityLevel level, juce::String message)
    {
        result.activity.push_back ({ level, "Interrupted take", message.toStdString() });
    };

    if (wasCancelled())
        return result;

    const juce::File root { juce::String (result.root) };
    if (! root.isDirectory() || wasCancelled())
        return result;

    // One level down and newest first. A card can hold hundreds of takes; an
    // interrupted take is, by definition, among the most recent events.
    juce::Array<juce::File> folders;
    root.findChildFiles (folders, juce::File::findDirectories, false);

    if (wasCancelled())
        return result;

    NewestFirst comparator;
    folders.sort (comparator);

    if (wasCancelled())
        return result;

    constexpr int kMaxFoldersExamined = 20;
    const int examine = juce::jmin (folders.size(), kMaxFoldersExamined);

    for (int i = 0; i < examine && ! wasCancelled(); ++i)
    {
        const auto folder = folders[i];

        const auto metadataFile = folder.getChildFile ("session.json");

        if (! metadataFile.existsAsFile() || wasCancelled())
            continue;

        SessionMetadata meta;

        // A session.json truncated by the same power cut that interrupted the
        // take is not a reason to fail: the audio beside it is still worth
        // recovering, so an unreadable one is treated as interrupted.
        bool metadataUnreadable = false;

        try
        {
            const auto text = metadataFile.loadFileAsString().toStdString();

            if (wasCancelled())
                return result;

            const auto parsed = JsonValue::parse (text);

            // The parser is lenient by design and hardly ever throws: a file
            // cut short mid-key comes back as one dangling key with nothing
            // under it. Judge it by whether a field carried a value.
            if (parsed.getType() != JsonValue::Type::Object
                || parsed.getValuedMemberCount() == 0)
            {
                meta = {};
                metadataUnreadable = true;
            }
            else
            {
                meta = SessionMetadata::fromJson (parsed);
            }
        }
        catch (...)
        {
            meta = {};
            metadataUnreadable = true;
        }

        if (wasCancelled())
            return result;

        if (metadataUnreadable)
            report (ActivityLevel::Warning,
                    folder.getFileName()
                    + " has a details file this app can't read, so what it says about "
                      "that take is gone. Its audio is still in that folder.");

        if (! SessionRecovery::sessionWasInterrupted (meta))
            continue;

        RecoveredSession session;
        session.folder = folder.getFullPathName().toStdString();
        session.startedIso = meta.startTimestampIso;

        for (const auto& entry : juce::RangedDirectoryIterator (
                 folder, false, "*.wav", juce::File::findFiles))
        {
            if (wasCancelled())
                return result;

            session.files.push_back (SessionRecovery::repairWavFile (
                entry.getFile().getFullPathName().toStdString()));

            if (wasCancelled())
                return result;
        }

        std::sort (session.files.begin(), session.files.end(),
                   [] (const RecoveredFile& a, const RecoveredFile& b)
                   {
                       return a.fileName < b.fileName;
                   });

        for (const auto& file : session.files)
            if (file.repairFailed)
                report (ActivityLevel::Warning,
                        juce::String (file.fileName)
                        + " couldn't be opened or repaired -- this card wouldn't accept "
                          "the fix. Copy it somewhere else before playing it.");

        if (session.isWorthPresenting())
        {
            report (ActivityLevel::Recovered,
                    folder.getFileName()
                    + " was interrupted, and its "
                    + juce::String (static_cast<int> (session.files.size()))
                    + (session.files.size() == 1 ? " file has" : " files have")
                    + " been repaired and can be played.");
            result.sessions.push_back (std::move (session));
        }
        else
        {
            report (ActivityLevel::Failed,
                    folder.getFileName()
                    + " was interrupted and nothing playable survived in it. There is "
                      "nothing to recover from that folder.");
        }
    }

    return result;
}

void Application::publishCompletedRecoveryScans() const
{
    const auto publish = [this] (DetachedResultTask<RecoveryBackgroundResult>& task,
                                 const std::string& expectedRoot,
                                 RecoveryScanStatus& status,
                                 const juce::String& failureMessage)
    {
        if (status != RecoveryScanStatus::Running)
            return;

        auto completed = task.takeResult();

        // The worker publishes its result and clears `running` under the same
        // mutex. If the first read arrived just before that publication and
        // the second sees the worker finished, one final result read closes
        // that narrow race before a missing result is treated as failure.
        if (! completed.has_value())
        {
            if (task.isRunning())
                return;

            completed = task.takeResult();

            if (! completed.has_value())
            {
                status = RecoveryScanStatus::Failed;
                noteActivity (ActivityLevel::Warning, "Interrupted take", failureMessage);
                return;
            }
        }

        if (completed->root != expectedRoot)
        {
            status = RecoveryScanStatus::Failed;
            noteActivity (ActivityLevel::Warning, "Interrupted take", failureMessage);
            return;
        }

        status = RecoveryScanStatus::Succeeded;

        for (const auto& entry : completed->activity)
            noteActivity (entry.level, juce::String (entry.subject),
                          juce::String (entry.message));

        for (auto& session : completed->sessions)
        {
            const auto name = juce::File (juce::String (session.folder)).getFileName();
            const auto existing = std::find_if (
                recoveredSessions.begin(), recoveredSessions.end(),
                [&name] (const RecoveredSession& candidate)
                {
                    return juce::File (juce::String (candidate.folder)).getFileName() == name;
                });

            if (existing == recoveredSessions.end())
                recoveredSessions.push_back (std::move (session));
            else if (completed->isDestinationCopy)
                *existing = std::move (session); // the user's primary copy wins
        }
    };

    publish (mirrorRecoveryTask, mirrorRecoveryRoot, mirrorRecoveryStatus,
             "The interrupted-take check for the local backup folder stopped unexpectedly. "
             "No files were changed; turn off local backup or restart SobStage before recording.");
    publish (destinationRecoveryTask, destinationRecoveryRoot, destinationRecoveryStatus,
             "The interrupted-take check for this save location stopped unexpectedly. "
             "No files were changed; choose another save location or restart SobStage before recording there.");
}

juce::String Application::recoveryBlockingReason() const
{
    publishCompletedRecoveryScans();

    const auto hasNonPreflightMutation = [this] (const std::string& root)
    {
        if (! recoveryMutationGate.isActive (root))
            return false;

        // The speed benchmark deliberately shares this mutation gate with
        // recovery. Once recovery itself has succeeded, that current worker
        // is reported by the preflight-specific status below rather than as a
        // misleading interrupted-take scan. An abandoned preflight has a fresh
        // task state (not running) and therefore still blocks here until its
        // old worker and lease really return.
        return ! (preflightTaskDestination == root && preflightTask.isRunning());
    };

    const bool rootsAreShared = ! destinationRecoveryRoot.empty()
        && destinationRecoveryRoot == mirrorRecoveryRoot;

    const auto sharedStatus = [&]
    {
        // Exactly one task is launched when the save location and local backup
        // are the same directory. Whichever side owns that task establishes
        // the safety fact for both roots.
        if (destinationRecoveryStatus != RecoveryScanStatus::NotStarted)
            return destinationRecoveryStatus;

        return mirrorRecoveryStatus;
    };

    const auto destinationStatus = rootsAreShared ? sharedStatus()
                                                   : destinationRecoveryStatus;

    if (destinationStatus == RecoveryScanStatus::Failed)
        return "Couldn't check this save location for interrupted takes. Choose another save "
               "location or restart SobStage before recording here.";

    if (destinationStatus != RecoveryScanStatus::Succeeded)
        return "Checking this save location for an interrupted take...";

    if (hasNonPreflightMutation (destinationRecoveryRoot))
        return "Waiting for earlier work on this save location to finish. Choose another "
               "location if this drive is no longer responding.";

    if (! mirrorPolicy.isEnabledByUser())
        return {};

    const auto enabledMirrorStatus = rootsAreShared ? sharedStatus()
                                                    : mirrorRecoveryStatus;

    if (enabledMirrorStatus == RecoveryScanStatus::Failed)
        return "Couldn't check the local backup folder for interrupted takes. Turn off local "
               "backup or restart SobStage before recording.";

    if (enabledMirrorStatus != RecoveryScanStatus::Succeeded)
        return "Checking the local backup folder for an interrupted take...";

    if (hasNonPreflightMutation (mirrorRecoveryRoot))
        return "Waiting for earlier work on the local backup folder to finish. Turn off local "
               "backup if that drive is no longer responding.";

    return {};
}

bool Application::isRecoveryScanPending() const
{
    publishCompletedRecoveryAcknowledgement();
    publishCompletedRecoveryScans();
    return destinationRecoveryTask.isRunning() || mirrorRecoveryTask.isRunning();
}

const std::vector<RecoveredSession>& Application::getRecoveredSessions() const
{
    publishCompletedRecoveryAcknowledgement();
    publishCompletedRecoveryScans();
    return recoveredSessions;
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
    if (shutdownStarted)
        return;

    shutdownStarted = true;

    // A device/preflight callback may already be queued on the message thread.
    // Make it harmless before any owned subsystem begins disappearing. This
    // also covers the explicit JUCE shutdown followed by ~Application's second
    // call into this function.
    invalidateAliveToken();

    // Combining is post-take convenience work. Ask its detached worker and
    // child process to stop before any hardware or filesystem finalizer gets a
    // chance to stall; this request never joins the worker.
    takeCombiner.cancel();

    // Finalize user media before auxiliary teardown. The detached storage
    // workers never join here; their cancellation flags only suppress late
    // publication, and worker-owned state survives any OS call that does not
    // return. Finalization itself performs filesystem I/O and is not
    // cancellable: force-terminating while either writer is blocked can still
    // leave an incomplete file, which no in-process ordering can prevent.
    // Camera first: CaptureCoordinator::stopRecording drains and joins a writer
    // that may itself be stuck in removable-volume I/O. Putting the movie stop
    // behind that join could leave its header open forever. JUCE camera teardown
    // is message-thread-affine, so these two finalizers cannot safely be raced.
    cameraController.stopRecordingForShutdown();

    if (capture != nullptr)
        capture->stopRecording();

    // The writer is drained above while its callbacks still have a valid rig.
    // Now stop the clock/streams that can enter those callbacks.
    if (capture != nullptr)
        capture->stopMonitoring();

    if (recordingEngine.getState() == RecordingState::Recording)
        recordingEngine.stop();
    if (audioBackend != nullptr)
        audioBackend->closeAllStreams();

    // Other apps must not be left holding a combined device whose owner is gone.
    if (systemAggregate != nullptr)
        systemAggregate->remove();

    preflightTask.cancel();
    destinationRecoveryTask.cancel();
    mirrorRecoveryTask.cancel();
    recoveryAcknowledgementTask.cancel();
    filesystemStatusProbe.stop();

    // The rig as the user is leaving it, so tomorrow starts where today ended.
    saveSettings();
}

} // namespace mma
