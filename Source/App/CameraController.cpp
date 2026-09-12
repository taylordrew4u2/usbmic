#include "CameraController.h"
#include <chrono>
#include <set>

#if JUCE_WINDOWS && JUCE_USE_CAMERA
#include <objbase.h>
#endif

namespace mma {

namespace {

/// A disposable UI owner for the one native preview component created when a
/// camera opens. Adding the native child here automatically removes it from a
/// previous host, so moving between the Cameras panel and the main screen does
/// not destroy/recreate AVFoundation's preview layer or restart its session.
class CameraViewerHost final : public juce::Component
{
public:
    explicit CameraViewerHost (std::shared_ptr<std::atomic<juce::Component*>> targetIn)
        : target (std::move (targetIn))
    {
        if (auto* viewer = target->load())
            addAndMakeVisible (*viewer);
    }

    void resized() override
    {
        if (auto* viewer = target->load(); viewer != nullptr && viewer->getParentComponent() == this)
            viewer->setBounds (getLocalBounds());
    }

private:
    std::shared_ptr<std::atomic<juce::Component*>> target;
};

} // namespace

CameraController::CameraController()
{
#if JUCE_USE_CAMERA
    discoveryThread = std::thread ([this] { runDiscoveryThread(); });
#endif
}

CameraController::~CameraController()
{
#if JUCE_USE_CAMERA
    // Close every live recording/device before waiting on OS discovery. Camera
    // enumeration can be stuck in AVFoundation/DirectShow; a slow discovery
    // must never leave a movie writer open while teardown waits for it.
    stopRecordingInternal (false);

    for (auto& [id, entry] : open)
    {
        juce::ignoreUnused (id);
        entry.viewerTarget->store (nullptr);
    }

    open.clear();

    {
        const std::lock_guard<std::mutex> guard (discoveryMutex);
        discoveryStopping = true;
    }
    discoveryCondition.notify_all();

    if (discoveryThread.joinable())
        discoveryThread.join();
#else
    stopRecording();
#endif
}

bool CameraController::isSupported() const
{
#if JUCE_USE_CAMERA
    return true;
#else
    return false;
#endif
}

juce::String CameraController::getUnavailableReason() const
{
    if (isSupported())
        return {};

    // §10.6: name what happened and what to do about it. There is nothing to
    // do about it here, so the sentence says that rather than implying the
    // user has a setting to find.
    return "This build can't use cameras. Camera recording is available in the "
           "macOS and Windows builds; the sound recording works either way.";
}

void CameraController::refreshCameras()
{
#if JUCE_USE_CAMERA
    applyPendingCameraList();
    requestDiscovery();
#else
    // Keep the unsupported build's selection consistently empty.
    selection.setAvailableCameras ({});
#endif
}

bool CameraController::applyPendingCameraList()
{
#if JUCE_USE_CAMERA
    const bool runtimeStateChanged = applyPendingRuntimeErrors();
    juce::StringArray names;
    bool hasNewDeviceList = false;
    {
        const std::lock_guard<std::mutex> guard (discoveryMutex);
        if (discoveryCompleted > discoveryApplied)
        {
            names = pendingDeviceNames;
            discoveryApplied = discoveryCompleted;
            hasNewDeviceList = true;
        }
    }

    if (hasNewDeviceList)
    {
        hasAppliedDeviceList = true;
        applyDeviceNames (names);
    }

    // Applying a topology snapshot and reconciling the devices it owns are one
    // operation. In particular, the Cameras panel may be visible, in which case
    // the outer UI deliberately does not call applySelection(). Leaving cleanup
    // to that caller kept an unplugged CameraDevice alive and allowed a same-name
    // reconnect to reuse the stale object indefinitely.
    if (hasNewDeviceList || runtimeStateChanged)
        applySelection (false);

    return hasNewDeviceList || runtimeStateChanged;
#else
    return false;
#endif
}

bool CameraController::waitForCameraRefresh (int timeoutMilliseconds)
{
#if JUCE_USE_CAMERA
    std::unique_lock<std::mutex> lock (discoveryMutex);
    const auto requested = discoveryRequested;
    if (requested == 0)
        return false;

    const bool completed = discoveryCondition.wait_for (
        lock, std::chrono::milliseconds (juce::jmax (0, timeoutMilliseconds)),
        [this, requested] { return discoveryStopping || discoveryCompleted >= requested; });
    const bool stopped = discoveryStopping;
    lock.unlock();

    return completed && ! stopped && applyPendingCameraList();
#else
    juce::ignoreUnused (timeoutMilliseconds);
    return false;
#endif
}

bool CameraController::isInitialDiscoveryPending() const noexcept
{
#if JUCE_USE_CAMERA
    if (hasAppliedDeviceList)
        return false;

    // Discovery is deliberately off-thread because AVFoundation/DirectShow
    // can hang. Give the ordinary first snapshot a short chance to protect a
    // remembered camera from an empty take, but never let one wedged driver
    // block audio recording forever. The remembered row stays visible and can
    // also be switched off during this grace period.
    constexpr double kInitialDiscoveryGraceMs = 3000.0;
    return initialDiscoveryRequestedAtMs <= 0.0
        || juce::Time::getMillisecondCounterHiRes() - initialDiscoveryRequestedAtMs
               < kInitialDiscoveryGraceMs;
#else
    return false;
#endif
}

#if JUCE_USE_CAMERA
bool CameraController::applyPendingRuntimeErrors()
{
    std::vector<RuntimeCameraError> errors;
    {
        const std::lock_guard<std::mutex> guard (runtimeErrorMailbox->mutex);
        errors.swap (runtimeErrorMailbox->pending);
    }

    bool changed = false;

    for (const auto& error : errors)
    {
        const auto entry = open.find (error.id);

        // A callback can arrive after an intentional close/reopen. Only the
        // exact CameraDevice generation which emitted it may invalidate itself.
        if (entry == open.end() || entry->second.viewerRevision != error.viewerRevision)
            continue;

        const auto name = juce::String (selection.getDisplayName (error.id));
        closeCamera (error.id);
        openFailures[error.id] = "Lost video from " + name
                               + (error.message.isNotEmpty() ? ": " + error.message : juce::String())
                               + ". Check the HDMI signal and cable, close any other app using "
                                 "the camera, then turn this camera off and back on.";
        changed = true;
    }

    return changed;
}

void CameraController::requestDiscovery()
{
    {
        const std::lock_guard<std::mutex> guard (discoveryMutex);
        if (discoveryStopping)
            return;

        if (discoveryRequested == 0)
            initialDiscoveryRequestedAtMs = juce::Time::getMillisecondCounterHiRes();

        ++discoveryRequested;
    }

    discoveryCondition.notify_all();
}

void CameraController::runDiscoveryThread()
{
   #if JUCE_WINDOWS
    const auto comResult = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const bool uninitialiseCom = SUCCEEDED (comResult);
   #endif

    std::unique_lock<std::mutex> lock (discoveryMutex);

    while (! discoveryStopping)
    {
        discoveryCondition.wait (lock, [this]
        {
            return discoveryStopping || discoveryRequested > discoveryCompleted;
        });

        if (discoveryStopping)
            break;

        const auto requested = discoveryRequested;
        lock.unlock();
        const auto names = juce::CameraDevice::getAvailableDevices();
        lock.lock();

        // A later request may have arrived while the OS was enumerating. This
        // result is still a valid snapshot; publish it, then loop immediately
        // for the newer request instead of discarding useful state.
        pendingDeviceNames = names;
        discoveryCompleted = requested;
        discoveryCondition.notify_all();
    }

    lock.unlock();

   #if JUCE_WINDOWS
    if (uninitialiseCom)
        CoUninitialize();
   #endif
}

void CameraController::applyDeviceNames (const juce::StringArray& names)
{
    // Keep the raw OS answer, not the take-filtered Selection produced below.
    // It is message-thread state and can be replayed synchronously when the
    // take ends so a camera which already reconnected is eligible immediately.
    lastAppliedDeviceNames = names;

    std::vector<CameraDeviceInfo> cameras;
    std::map<std::string, int> currentDeviceNameCounts;
    for (const auto& name : names)
        ++currentDeviceNameCounts[name.toStdString()];

    if (takeActive)
    {
        // Occurrence numbers are the only identity JUCE exposes for cameras
        // with the same OS name. If that group's count changes, there is no
        // honest way to tell which physical unit kept which id. Once ambiguous,
        // hold the whole group out until the next take rather than attach a
        // live preview to the wrong recording identity.
        for (const auto& [name, baselineCount] : takeDeviceNameCounts)
        {
            const auto current = currentDeviceNameCounts.find (name);
            const int currentCount = current != currentDeviceNameCounts.end() ? current->second : 0;

            if (currentCount != baselineCount && (baselineCount > 1 || currentCount > 1))
                ambiguousTakeDeviceNames.insert (name);
        }
    }

    std::set<std::string> previousIds;
    for (const auto& camera : selection.getAvailableCameras())
        previousIds.insert (camera.id);
    osIndexById.clear();

    // These failures mean only that JUCE's index-to-device mapping changed
    // between discovery and open. The result being applied was requested after
    // that mismatch, so it is the fresh identity/index mapping that earns one
    // automatic retry. Ordinary privacy/busy failures remain explicit-retry
    // only and are intentionally not cleared here.
    for (const auto& id : topologyRetryIds)
        openFailures.erase (id);
    topologyRetryIds.clear();

    // Whatever the OS lists, whatever it is plugged into: a USB webcam, a
    // built-in camera, a capture card presenting an HDMI feed as a camera. The
    // app does not vet the source, the same way §2 does not vet a microphone's.
    std::map<juce::String, int> seen;

    for (int i = 0; i < names.size(); ++i)
    {
        const auto name = names[i];
        const int occurrence = ++seen[name];

        // Two cameras of the same model enumerate with the same product string,
        // exactly as §14.6's four identical microphones do. The occurrence
        // keeps their choices apart for as long as they stay connected.
        const auto id = (occurrence == 1 ? name : name + " #" + juce::String (occurrence)).toStdString();

        // A camera whose writer was interrupted cannot be joined back onto the
        // same movie by JUCE. Keep it out of the live list until this take is
        // over rather than reopening a preview that looks like recording has
        // resumed when the rest of the file is in fact being lost.
        if (takeActive
            && (ambiguousTakeDeviceNames.count (name.toStdString()) > 0
                || camerasDeferredUntilTakeEnds.count (id) > 0))
            continue;

        osIndexById[id] = i;
        cameras.push_back ({ id, name.toStdString() });

        // Reconnecting is a fresh opportunity to open the camera. A failure
        // belonging to the prior connection must not suppress that one.
        if (previousIds.count (id) == 0)
            openFailures.erase (id);
    }
    selection.setAvailableCameras (std::move (cameras));
}
#endif

void CameraController::applySelection (bool retryFailures)
{
#if JUCE_USE_CAMERA
    // Names rather than a count, because "one of your cameras" helps nobody
    // standing in front of three of them. Declared inside the guard so a build
    // without camera support does not carry an unused local.
    juce::StringArray missingCameras;

    // Rebuilt from durable per-camera failures below. A periodic no-op must
    // keep the existing explanation visible even though it deliberately does
    // not spend another blocking open attempt.
    openProblem.clear();

    // Close first, so a machine that can only hold one camera open at a time
    // has the old one released before the new one is asked for.
    std::vector<std::string> toClose;

    for (const auto& entry : open)
    {
        const bool switchedOff = ! selection.isEnabled (entry.first);
        const bool disappeared = osIndexById.count (entry.first) == 0;

        // Camera membership is frozen for a take. A settings/UI change must
        // not finalize a movie halfway through merely because the camera was
        // switched off; apply it as soon as stopRecording() ends the take.
        // A real unplug is different: the device is already gone, so close it
        // now and retain the partial movie for the watchdog/combiner.
        if (takeActive && entry.second.recordingThisTake && switchedOff && ! disappeared)
            continue;

        if (switchedOff || disappeared)
            toClose.push_back (entry.first);
    }

    for (const auto& id : toClose)
        closeCamera (id);

    for (auto it = openFailures.begin(); it != openFailures.end();)
    {
        const bool stillEnabled = selection.isEnabled (it->first);
        const bool stillAvailable = osIndexById.count (it->first) > 0;
        if (! stillEnabled || ! stillAvailable)
            it = openFailures.erase (it);
        else
            ++it;
    }

    for (const auto& camera : selection.getAvailableCameras())
    {
        if (! selection.isEnabled (camera.id) || open.count (camera.id) > 0)
            continue;

        // Camera membership and files are fixed when the take starts. A device
        // that appears (or finishes a topology retry) during the take may be
        // used again afterwards, but opening only a preview now would falsely
        // imply that its picture is being added to the running recording.
        if (takeActive)
            continue;

        const auto index = osIndexById.find (camera.id);

        if (index != osIndexById.end()
            && (retryFailures || openFailures.count (camera.id) == 0))
            openCamera (camera.id, index->second, juce::String (camera.displayName));
    }

    // Selection's available list necessarily contains only the latest OS
    // snapshot. Look at its durable choices separately so a previously armed
    // capture card that vanishes remains a visible fault instead of silently
    // disappearing from the panel and from this problem line.
    if (hasAppliedDeviceList)
        for (const auto& camera : selection.getUnavailableEnabledCameras())
            missingCameras.add (juce::String (camera.displayName));

    for (const auto& [id, problem] : openFailures)
    {
        juce::ignoreUnused (id);
        if (openProblem.isNotEmpty())
            openProblem += " ";
        openProblem += problem;
    }

    // Appended, never assigned over: openCamera() may already have recorded a
    // camera that is connected and still would not open, and both facts matter.
    if (! missingCameras.isEmpty())
    {
        if (openProblem.isNotEmpty())
            openProblem += " ";

        openProblem += (missingCameras.size() == 1
                       ? "The system isn't listing " + missingCameras[0]
                             + ", so SobStage can't preview or record it."
                       : "The system isn't listing " + juce::String (missingCameras.size())
                             + " enabled cameras, so SobStage can't preview or record them.")
                 + " Reconnect the camera or HDMI capture card, check its USB cable and HDMI "
                   "signal; SobStage will rescan automatically. Sound recording is unaffected.";
    }
#endif
}

#if JUCE_USE_CAMERA
void CameraController::openCamera (const std::string& id, int osIndex,
                                   const juce::String& expectedDeviceName)
{
    const auto viewerRevision = ++viewerRevisions[id];

    // Ask JUCE/the platform for high-quality capture. The driver ultimately
    // chooses the actual format, so neither the UI nor documentation promises
    // a resolution which the device has not reported.
    //
    // highQuality=false is what JUCE calls preview mode, where the OS is free
    // to drop frames -- fine for a picture on screen, not fine for the file
    // that is the point of the exercise. Since one open device feeds both the
    // view and the recording, the safe request is high-quality capture while
    // making the *view* cheap by drawing it small, which PreviewQuality does.
    std::unique_ptr<juce::CameraDevice> device (
        juce::CameraDevice::openDevice (osIndex,
                                        640, 480,      // never settle below this
                                        8192, 8192,    // allow the platform's high-quality choice
                                        true));        // highQuality

    if (device == nullptr)
    {
        // §10.6: what happened, then what to do. The overwhelmingly common
        // cause is the OS privacy prompt having been declined, or another app
        // holding the camera.
        // Appended. Assigning meant two cameras failing in one pass reported
        // only the last -- the same enumeration-order dependence the pass-level
        // clear above was added to remove, still live one function away.
        openFailures[id] = "Couldn't open " + juce::String (selection.getDisplayName (id))
                         + ". Close any other app using it, and check this app is allowed "
                           "to use the camera in your system privacy settings.";
        return;
    }

    // JUCE's index overload enumerates the OS again inside openDevice(). A
    // hot-plug between our background snapshot and this call can therefore make
    // the same integer refer to a different camera. Never attach that picture to
    // the selected camera's id/name (or record it under that filename): discard
    // it, request a fresh mapping, and retry only after that mapping arrives.
    if (device->getName() != expectedDeviceName)
    {
        device.reset();
        openFailures[id] = "The camera list changed while opening "
                         + juce::String (selection.getDisplayName (id))
                         + ". Checking the cameras again.";
        topologyRetryIds.insert (id);
        requestDiscovery();
        return;
    }

    OpenCamera entry;
    entry.device = std::move (device);
    entry.viewerRevision = viewerRevision;
    entry.viewerTarget = std::make_shared<std::atomic<juce::Component*>> (nullptr);

    const auto mailbox = runtimeErrorMailbox;
    entry.device->onErrorOccurred = [mailbox, id, viewerRevision] (const juce::String& error)
    {
        const std::lock_guard<std::mutex> guard (mailbox->mutex);
        mailbox->pending.push_back ({ id, viewerRevision, error });
    };

    // JUCE documents that the macOS preview must be created before anything
    // starts the capture session. It is therefore part of opening the device,
    // not a transient UI object that can be created again on each screen.
    entry.nativeViewer.reset (entry.device->createViewerComponent());

    if (entry.nativeViewer == nullptr)
    {
        openFailures[id] = "Couldn't start video from "
                         + juce::String (selection.getDisplayName (id))
                         + ". Check the HDMI signal and cable, close any other app using it, "
                           "then turn this camera off and back on.";
        return;
    }

    entry.viewerTarget->store (entry.nativeViewer.get());
    entry.osIndex = osIndex;
    open[id] = std::move (entry);
    openFailures.erase (id);

    // Deliberately does NOT clear openProblem. It used to, which meant one
    // camera opening successfully erased the message about another that had
    // just failed -- with three cameras and one bad one, whether you were told
    // depended on enumeration order. The pass that starts a round of opens is
    // what clears it; see applySelection().
}

void CameraController::closeCamera (const std::string& id)
{
    const auto entry = open.find (id);

    if (entry == open.end())
        return;

    const bool wasRecordingThisTake = entry->second.recordingThisTake
                                   || recordingCameraIds.count (id) > 0;

    if (wasRecordingThisTake && entry->second.device != nullptr)
        entry->second.device->stopRecording();

    if (takeActive && wasRecordingThisTake)
        camerasDeferredUntilTakeEnds.insert (id);

    recordingCameraIds.erase (id);
    recording = ! recordingCameraIds.empty();

    entry->second.viewerTarget->store (nullptr);
    ++viewerRevisions[id];
    open.erase (entry);
}
#endif

std::unique_ptr<juce::Component> CameraController::createViewer (const std::string& deviceId)
{
#if JUCE_USE_CAMERA
    const auto entry = open.find (deviceId);

    if (entry != open.end() && entry->second.nativeViewer != nullptr)
        return std::make_unique<CameraViewerHost> (entry->second.viewerTarget);
#else
    juce::ignoreUnused (deviceId);
#endif

    return nullptr;
}

uint64_t CameraController::getViewerRevision (const std::string& deviceId) const
{
#if JUCE_USE_CAMERA
    const auto revision = viewerRevisions.find (deviceId);
    return revision != viewerRevisions.end() ? revision->second : 0;
#else
    juce::ignoreUnused (deviceId);
    return 0;
#endif
}

juce::StringArray CameraController::getPlannedFileNames() const
{
    juce::StringArray names;

#if JUCE_USE_CAMERA
    const auto extension = juce::CameraDevice::getFileExtension();
#else
    const juce::String extension { ".mov" };
#endif

    for (const auto& plan : selection.buildPlans())
        names.add (juce::String (plan.fileName) + extension);

    return names;
}

juce::String CameraController::getPlannedFileNameFor (const std::string& deviceId) const
{
#if JUCE_USE_CAMERA
    const auto extension = juce::CameraDevice::getFileExtension();
#else
    const juce::String extension { ".mov" };
#endif

    for (const auto& plan : selection.buildPlans())
        if (plan.deviceId == deviceId)
            return juce::String (plan.fileName) + extension;

    return {};
}

bool CameraController::startRecording (const juce::File& sessionFolder, double audioStartMs)
{
#if JUCE_USE_CAMERA
    if (takeActive)
        return recording;

    const auto extension = juce::CameraDevice::getFileExtension();
    int started = 0;

    takeActive = true;
    takePlans = selection.buildIntendedPlans();
    takeRecordings.clear();
    recordingCameraIds.clear();
    camerasDeferredUntilTakeEnds.clear();
    takeDeviceNameCounts.clear();
    ambiguousTakeDeviceNames.clear();

    for (const auto& camera : selection.getAvailableCameras())
        ++takeDeviceNameCounts[camera.displayName];

    for (const auto& plan : takePlans)
    {
        const auto entry = open.find (plan.deviceId);

        if (entry == open.end() || entry->second.device == nullptr)
            continue;

        // §6.2: the picture goes in the same session folder as the sound, under
        // the same naming rules, so one folder is still the whole take.
        const auto file = sessionFolder.getChildFile (juce::String (plan.fileName) + extension);
        entry->second.recordingFile = file;

        // Quality 2 is JUCE's highest. There is no setting for this and there
        // should not be: nobody wants the take they cannot redo in medium.
        entry->second.device->startRecordingToFile (file, 2);

        // Read after the call returns, not before. What matters is when the OS
        // actually began taking frames, and opening a camera file is the slow
        // part -- timing it from before the call would under-report the gap by
        // exactly the amount that matters.
        entry->second.startOffsetSeconds =
            audioStartMs > 0.0
                ? juce::jmax (0.0, (juce::Time::getMillisecondCounterHiRes() - audioStartMs) / 1000.0)
                : 0.0;

        entry->second.recordingThisTake = true;
        recordingCameraIds.insert (plan.deviceId);
        takeRecordings.push_back ({ plan.deviceId, plan.displayName,
                                    entry->second.device->getName().toStdString(), file,
                                    entry->second.startOffsetSeconds });

        ++started;
    }

    // A physically absent camera cannot join a movie after the take begins.
    // Hide only those absent-at-t=0 ids from later topology snapshots. A
    // connected camera which failed because it is busy or privacy-blocked must
    // remain listed so its precise open failure is not replaced by the false
    // "system isn't listing it" diagnosis. takeActive already prevents either
    // kind from being opened into the running take.
    for (const auto& camera : selection.getUnavailableEnabledCameras())
        camerasDeferredUntilTakeEnds.insert (camera.id);

    recording = ! recordingCameraIds.empty();

    // Its own field, and cleared each time this runs. Assigning the shared one
    // overwrote "X isn't connected any more" with the vaguer count, and never
    // clearing it left a previous take's failure standing over a clean one.
    recordProblem.clear();

    if (! takePlans.empty() && started < static_cast<int> (takePlans.size()))
        recordProblem = juce::String (static_cast<int> (takePlans.size()) - started)
                        + " of your cameras couldn't start "
                        "recording. The sound is recording either way.";

    return recording;
#else
    juce::ignoreUnused (sessionFolder, audioStartMs);
    return false;
#endif
}

std::vector<CombinedTakeInput> CameraController::getCombinedTakeInputs() const
{
    std::vector<CombinedTakeInput> inputs;

#if JUCE_USE_CAMERA
    // Retained independently of the live/open map: unplug closes and erases a
    // CameraDevice, but the movie it finalized is still a valid partial input
    // and must remain available to the post-take combiner.
    for (const auto& takeRecording : takeRecordings)
    {
        const auto& file = takeRecording.file;

        inputs.push_back ({ file.existsAsFile() ? file.getFileName().toStdString() : std::string(),
                            takeRecording.startOffsetSeconds });
    }
#endif

    return inputs;
}

std::vector<CameraController::TakeVideoRecord> CameraController::getTakeVideoRecords() const
{
    std::vector<TakeVideoRecord> videos;

#if JUCE_USE_CAMERA
    videos.reserve (takeRecordings.size());

    // Unlike takePlans, this list contains only writers for which JUCE accepted
    // startRecordingToFile(). It therefore cannot invent a file for a missing
    // or failed-open camera. An interrupted writer remains here because its
    // finalized partial movie is still a real contribution to the take.
    for (const auto& takeRecording : takeRecordings)
        videos.push_back ({ takeRecording.displayName,
                            takeRecording.file.getFileName().toStdString() });
#endif

    return videos;
}

std::vector<CameraController::TakeCameraState> CameraController::getTakeCameraStates() const
{
    std::vector<TakeCameraState> states;
    states.reserve (takePlans.size());

    for (const auto& plan : takePlans)
        states.push_back ({ plan.deviceId, plan.displayName,
                            recordingCameraIds.count (plan.deviceId) > 0 });

    return states;
}

void CameraController::stopRecording()
{
    stopRecordingInternal (true);
}

void CameraController::stopRecordingForShutdown()
{
    stopRecordingInternal (false);
}

void CameraController::stopRecordingInternal (bool reconcileForNextTake)
{
    const bool wasTakeActive = takeActive;

#if JUCE_USE_CAMERA
    for (auto& entry : open)
    {
        if (entry.second.recordingThisTake && entry.second.device != nullptr)
            entry.second.device->stopRecording();

        entry.second.recordingThisTake = false;
    }

#endif

    recordingCameraIds.clear();
    recording = false;
    takeActive = false;
    takeDeviceNameCounts.clear();
    ambiguousTakeDeviceNames.clear();

#if JUCE_USE_CAMERA
    const auto deferred = camerasDeferredUntilTakeEnds;
    camerasDeferredUntilTakeEnds.clear();

    if (wasTakeActive && reconcileForNextTake)
    {
        // applyDeviceNames deliberately hid absent-at-start or interrupted
        // cameras from the running take. Restore the latest unfiltered OS list
        // now; otherwise an immediate second Record click can freeze another
        // missing roster while an unnecessary rediscovery is still pending.
        applyDeviceNames (lastAppliedDeviceNames);

        // Only a camera that failed while this take was active earns an
        // automatic retry. Privacy/busy failures from before the take remain
        // explicit-action retries, and teardown never opens hardware.
        for (const auto& id : deferred)
            if (selection.isEnabled (id) && osIndexById.count (id) > 0)
                openFailures.erase (id);

        // Applies a safely deferred off switch and reopens only the eligible
        // in-take failures cleared immediately above.
        applySelection (false);
    }
#endif
}

} // namespace mma
