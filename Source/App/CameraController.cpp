#include "CameraController.h"
#include <chrono>
#include <set>

#if JUCE_WINDOWS && JUCE_USE_CAMERA
#include <objbase.h>
#endif

namespace mma {

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
    stopRecording();
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
    juce::StringArray names;
    {
        const std::lock_guard<std::mutex> guard (discoveryMutex);
        if (discoveryCompleted <= discoveryApplied)
            return false;

        names = pendingDeviceNames;
        discoveryApplied = discoveryCompleted;
    }

    applyDeviceNames (names);

    // Applying a topology snapshot and reconciling the devices it owns are one
    // operation. In particular, the Cameras panel may be visible, in which case
    // the outer UI deliberately does not call applySelection(). Leaving cleanup
    // to that caller kept an unplugged CameraDevice alive and allowed a same-name
    // reconnect to reuse the stale object indefinitely.
    applySelection (false);
    return true;
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

#if JUCE_USE_CAMERA
void CameraController::requestDiscovery()
{
    {
        const std::lock_guard<std::mutex> guard (discoveryMutex);
        if (discoveryStopping)
            return;

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
        if (! selection.isEnabled (entry.first) || osIndexById.count (entry.first) == 0)
            toClose.push_back (entry.first);

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

        if (index != osIndexById.end())
        {
            if (retryFailures || openFailures.count (camera.id) == 0)
                openCamera (camera.id, index->second, juce::String (camera.displayName));
        }
        else
        {
            // A camera the user switched on that the OS is no longer offering.
            // It was skipped with no else at all, so it was simply absent from
            // the take -- and a camera you deliberately enabled and then do not
            // find in the folder is the kind of absence nobody thinks to check
            // for until the edit.
            missingCameras.add (juce::String (selection.getDisplayName (camera.id)));
        }
    }


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
                       ? missingCameras[0] + " isn't connected any more, so it isn't in this take."
                       : juce::String (missingCameras.size())
                             + " of your cameras aren't connected any more, so they aren't in this "
                               "take.")
                 + " The sound is recording either way.";
    }
#endif
}

#if JUCE_USE_CAMERA
void CameraController::openCamera (const std::string& id, int osIndex,
                                   const juce::String& expectedDeviceName)
{
    // Always opened at the best the camera can do, and never at anything less.
    //
    // highQuality=false is what JUCE calls preview mode, where the OS is free
    // to drop frames -- fine for a picture on screen, not fine for the file
    // that is the point of the exercise. Since one open device feeds both the
    // view and the recording, the only safe answer is to capture at full
    // quality always and make the *view* cheap by drawing it small, which is
    // what PreviewQuality does.
    std::unique_ptr<juce::CameraDevice> device (
        juce::CameraDevice::openDevice (osIndex,
                                        640, 480,      // never settle below this
                                        8192, 8192,    // and take the best on offer
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

    open.erase (entry);
}
#endif

std::unique_ptr<juce::Component> CameraController::createViewer (const std::string& deviceId)
{
#if JUCE_USE_CAMERA
    const auto entry = open.find (deviceId);

    if (entry != open.end() && entry->second.device != nullptr)
        return std::unique_ptr<juce::Component> (entry->second.device->createViewerComponent());
#else
    juce::ignoreUnused (deviceId);
#endif

    return nullptr;
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

juce::StringArray CameraController::getTakePlannedFileNames() const
{
    juce::StringArray names;

#if JUCE_USE_CAMERA
    const auto extension = juce::CameraDevice::getFileExtension();
#else
    const juce::String extension { ".mov" };
#endif

    for (const auto& plan : takePlans)
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
    takePlans = selection.buildPlans();
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

std::vector<CameraController::TakeCameraState> CameraController::getTakeCameraStates() const
{
    std::vector<TakeCameraState> states;
    states.reserve (takeRecordings.size());

    for (const auto& takeRecording : takeRecordings)
        states.push_back ({ takeRecording.deviceId, takeRecording.displayName,
                            recordingCameraIds.count (takeRecording.deviceId) > 0 });

    return states;
}

void CameraController::stopRecording()
{
#if JUCE_USE_CAMERA
    for (auto& entry : open)
    {
        if (entry.second.recordingThisTake && entry.second.device != nullptr)
            entry.second.device->stopRecording();

        entry.second.recordingThisTake = false;
    }

    camerasDeferredUntilTakeEnds.clear();
#endif

    recordingCameraIds.clear();
    recording = false;
    takeActive = false;
    takeDeviceNameCounts.clear();
    ambiguousTakeDeviceNames.clear();
}

} // namespace mma
