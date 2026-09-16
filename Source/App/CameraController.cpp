#include "CameraController.h"
#include <algorithm>
#include <chrono>
#include <set>

#if JUCE_WINDOWS && JUCE_USE_CAMERA
#include <objbase.h>
#endif

namespace mma {

namespace {

#if JUCE_USE_CAMERA
constexpr double kCameraSignalTimeoutMs = 5000.0;
constexpr double kCameraFinalizationTimeoutMs = 15000.0;

#if JUCE_MAC || JUCE_WINDOWS || defined(SOBSTAGE_CAMERA_SIMULATION)
constexpr bool kCameraBackendReportsFinalization = true;
#else
constexpr bool kCameraBackendReportsFinalization = false;
#endif

/// CameraDevice::Listener may run on a platform capture thread. It publishes
/// one tiny generation-tagged fact and never reaches back into the controller
/// or UI. That makes an already-running callback harmless after a close/reopen:
/// the message-thread drain rejects it unless the exact device generation is
/// still current.
class FrameHeartbeatListener final : public juce::CameraDevice::Listener
{
public:
    explicit FrameHeartbeatListener (std::function<void()> publishIn)
        : publish (std::move (publishIn))
    {
    }

    void imageReceived (const juce::Image& image) override
    {
        if (image.isValid())
            publish();
    }

private:
    std::function<void()> publish;
};
#endif

/// A disposable UI owner for the one native preview component created when a
/// camera opens. Adding the native child here automatically removes it from a
/// previous host, so moving between the Cameras panel and the main screen does
/// not destroy/recreate AVFoundation's preview layer or restart its session.
#if JUCE_USE_CAMERA
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
#endif

} // namespace

CameraController::CameraController() = default;

CameraController::~CameraController()
{
#if JUCE_USE_CAMERA
    // Suppress a late platform result before touching message-thread-owned
    // camera state. Enumeration itself is deliberately not joined: an OS
    // camera service can remain wedged forever, and the worker owns no part of
    // this controller which could become dangling after destruction.
    const auto discovery = discoveryState;
    {
        const std::lock_guard<std::mutex> guard (discovery->mutex);
        discovery->cancelled = true;
        discovery->pendingDeviceNames.clear();
    }
    discovery->condition.notify_all();

    stopRecordingInternal (false);

    for (auto& [id, entry] : open)
    {
        juce::ignoreUnused (id);
        if (entry.device != nullptr && entry.frameListener != nullptr)
            entry.device->removeListener (entry.frameListener.get());
        entry.viewerTarget->store (nullptr);
    }

    open.clear();
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
    requestDiscovery (true);
#else
    // Keep the unsupported build's selection consistently empty.
    selection.setAvailableCameras ({});
#endif
}

void CameraController::refreshCamerasIfIdle()
{
#if JUCE_USE_CAMERA
    applyPendingCameraList();
    requestDiscovery (false);
#else
    // Keep the unsupported build's selection consistently empty.
    selection.setAvailableCameras ({});
#endif
}

bool CameraController::applyPendingCameraList()
{
#if JUCE_USE_CAMERA
    const bool runtimeStateChanged = applyPendingRuntimeEvents();
    const bool signalStateChanged = applySignalTimeouts();
    bool finalizationStateChanged = applyRecordingFinalizationTimeout();
    finalizationStateChanged = finishRecordingFinalizationIfReady()
                            || finalizationStateChanged;
    juce::StringArray names;
    bool hasNewDeviceList = false;
    const auto discovery = discoveryState;
    {
        const std::lock_guard<std::mutex> guard (discovery->mutex);
        if (! discovery->cancelled && discovery->completed > discoveryApplied)
        {
            names = discovery->pendingDeviceNames;
            discoveryApplied = discovery->completed;
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
    if (hasNewDeviceList || runtimeStateChanged || signalStateChanged)
        applySelection (false);

    return hasNewDeviceList || runtimeStateChanged || signalStateChanged
        || finalizationStateChanged;
#else
    return false;
#endif
}

bool CameraController::waitForCameraRefresh (int timeoutMilliseconds)
{
#if JUCE_USE_CAMERA
    const auto discovery = discoveryState;
    std::unique_lock<std::mutex> lock (discovery->mutex);
    const auto requested = discovery->requested;
    if (requested == 0)
        return false;

    const bool signalled = discovery->condition.wait_for (
        lock, std::chrono::milliseconds (juce::jmax (0, timeoutMilliseconds)),
        [discovery, requested]
        {
            return discovery->cancelled
                || discovery->completed >= requested
                || ! discovery->workerRunning;
        });
    const bool completed = discovery->completed >= requested;
    const bool cancelled = discovery->cancelled;
    lock.unlock();

    return signalled && completed && ! cancelled && applyPendingCameraList();
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
bool CameraController::applyPendingRuntimeEvents()
{
    std::vector<RuntimeCameraError> errors;
    std::vector<FrameNotification> frames;
    std::vector<RecordingStartedNotification> recordingsStarted;
    std::vector<RecordingFinishedNotification> recordingsFinished;
    {
        const std::lock_guard<std::mutex> guard (runtimeErrorMailbox->mutex);
        errors.swap (runtimeErrorMailbox->pending);
        frames.swap (runtimeErrorMailbox->frames);
        recordingsStarted.swap (runtimeErrorMailbox->recordingsStarted);
        recordingsFinished.swap (runtimeErrorMailbox->recordingsFinished);
    }

    bool changed = false;

    for (const auto& started : recordingsStarted)
    {
        const auto take = std::find_if (takeRecordings.begin(), takeRecordings.end(),
            [&] (const TakeRecording& recording)
            {
                return recording.deviceId == started.id
                    && recording.file == started.file;
            });

        if (take == takeRecordings.end() || started.takeGeneration != takeGeneration
            || take->finalizationComplete || take->started)
            continue;

        take->started = true;
        take->startOffsetSeconds = started.startOffsetSeconds;
        startingCameraIds.erase (started.id);

        // didStart may legitimately arrive after Stop was pressed. It still
        // proves that this movie must be awaited and can be used if didFinish
        // succeeds, but it must not put the stopped take back into REC.
        if (takeActive)
        {
            recordingCameraIds.insert (started.id);
            recording = true;
        }

        const auto entry = open.find (started.id);
        if (entry != open.end() && entry->second.viewerRevision == started.viewerRevision)
        {
            entry->second.startingThisTake = false;
            entry->second.recordingThisTake = takeActive;
        }

        changed = true;
    }

    for (const auto& frame : frames)
    {
        const auto entry = open.find (frame.id);

        // A frame from the device generation which just closed must never make
        // a same-id replacement look live. This is especially important for
        // capture cards, whose product string and therefore app id rarely
        // changes across a USB reconnect.
        if (entry == open.end() || entry->second.viewerRevision != frame.viewerRevision)
            continue;

        const bool becameLive = ! entry->second.firstFrameReceived
                             || entry->second.signalTimedOut;
        entry->second.firstFrameReceived = true;
        entry->second.signalTimedOut = false;
        entry->second.lastFrameAtMs = signalClockMs();
        openFailures.erase (frame.id);

        // Keep this generation's listener attached. SobStage's pinned-JUCE
        // patch throttles the proof capture to two frames per second, which is
        // cheap enough to detect HDMI loss/recovery without the old unbounded
        // still-photo loop contending with the movie writer.
        if (becameLive)
        {
            // Device generation stays fixed for callback validation. The public
            // revision is only the UI cache token for placeholder/live changes.
            ++viewerRevisions[frame.id];
            changed = true;
        }
    }

    for (const auto& finished : recordingsFinished)
    {
        const auto take = std::find_if (takeRecordings.begin(), takeRecordings.end(),
            [&] (const TakeRecording& recording)
            {
                return recording.deviceId == finished.id
                    && recording.file == finished.file;
            });

        if (take == takeRecordings.end() || finished.takeGeneration != takeGeneration
            || take->finalizationComplete)
            continue;

        take->finalizationComplete = true;
        take->finalizationError = finished.error;

        // didFinish is required even for a start which AVFoundation rejected.
        // A backend may provide no NSError in that edge case; the missing
        // didStart is itself enough evidence that no usable movie began.
        if (! take->started)
        {
            if (take->finalizationError.isEmpty())
                take->finalizationError = "The camera ended before it confirmed recording started";

            recordProblem = "Couldn't start recording video from "
                          + juce::String (take->displayName) + ": "
                          + take->finalizationError + ".";
        }

        const auto openEntry = open.find (finished.id);
        if (openEntry != open.end()
            && openEntry->second.viewerRevision == finished.viewerRevision)
        {
            openEntry->second.recordingThisTake = false;
            openEntry->second.startingThisTake = false;
            openEntry->second.device->onRecordingStarted = nullptr;
            openEntry->second.device->onRecordingFinished = nullptr;
        }

        finalizingDevices.erase (
            std::remove_if (finalizingDevices.begin(), finalizingDevices.end(),
                [&] (const FinalizingDevice& device)
                {
                    return device.id == finished.id
                        && device.viewerRevision == finished.viewerRevision
                        && device.takeGeneration == finished.takeGeneration;
                }),
            finalizingDevices.end());

        recordingCameraIds.erase (finished.id);
        startingCameraIds.erase (finished.id);
        recording = ! recordingCameraIds.empty();
        changed = true;
    }

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
                               + ". Check the camera or capture-device signal and cables, close any "
                                 "other app using it, then turn this camera off and back on.";
        changed = true;
    }

    return changed;
}

double CameraController::signalClockMs() const noexcept
{
    auto now = juce::Time::getMillisecondCounterHiRes();
#if defined(SOBSTAGE_CAMERA_SIMULATION)
    now += signalClockOffsetForTesting;
#endif
    return now;
}

bool CameraController::applySignalTimeouts()
{
    bool changed = false;
    const auto now = signalClockMs();

    for (auto& [id, entry] : open)
    {
        const auto lastProofAt = entry.firstFrameReceived ? entry.lastFrameAtMs
                                                          : entry.openedAtMs;

        if (entry.signalTimedOut || now - lastProofAt < kCameraSignalTimeoutMs)
            continue;

        entry.signalTimedOut = true;
        openFailures[id] = "No video frames arrived from "
                         + juce::String (selection.getDisplayName (id))
                         + ". Check that the camera or capture device is on. For HDMI, check its "
                           "output plus the capture card's HDMI and USB cables. Then turn this "
                           "camera off and back on.";
        ++viewerRevisions[id];
        changed = true;
    }

    return changed;
}

bool CameraController::applyRecordingFinalizationTimeout()
{
    if (recordingFinalizationState != RecordingFinalizationState::Waiting
        || signalClockMs() < recordingFinalizationDeadlineMs)
        return false;

    juce::StringArray timedOutNames;

    std::vector<std::string> timedOutIds;

    for (auto& recording : takeRecordings)
    {
        if (recording.finalizationComplete)
            continue;

        recording.finalizationComplete = true;
        recording.finalizationError = "The camera did not finish writing its movie within 15 seconds.";
        timedOutNames.add (juce::String (recording.displayName));
        timedOutIds.push_back (recording.deviceId);

        const auto entry = open.find (recording.deviceId);
        if (entry != open.end())
        {
            entry->second.device->onRecordingFinished = nullptr;
            entry->second.device->onRecordingStarted = nullptr;
            entry->second.recordingThisTake = false;
            entry->second.startingThisTake = false;
        }

        // A writer which missed its required completion callback is not a
        // safe device generation to reuse. Keep this durable until the user
        // explicitly retries the camera; periodic discovery/reconciliation
        // must not silently open it for the next take.
        openFailures[recording.deviceId] =
            "Video file finalization timed out for "
            + juce::String (recording.displayName)
            + ". Turn this camera off and back on before recording with it again.";
        finalizationRetryRequiredIds.insert (recording.deviceId);
    }

    if (! timedOutNames.isEmpty())
        recordingFinalizationProblem = timedOutNames.size() == 1
            ? timedOutNames[0] + " did not finish its video file. The audio is safe; do not use that movie."
            : juce::String (timedOutNames.size())
                + " cameras did not finish their video files. The audio is safe; do not use those movies.";

    finalizingDevices.clear();

    // A backend that missed its required completion callback is not safe to
    // reuse for another file. Closing is asynchronous in the pinned JUCE patch,
    // so this remains bounded even when the driver itself is wedged.
    for (const auto& id : timedOutIds)
        closeCamera (id);

    return true;
}

bool CameraController::finishRecordingFinalizationIfReady()
{
    if (recordingFinalizationState != RecordingFinalizationState::Waiting)
        return false;

    if (std::any_of (takeRecordings.begin(), takeRecordings.end(),
                     [] (const TakeRecording& recording) { return ! recording.finalizationComplete; }))
        return false;

    const bool failed = std::any_of (takeRecordings.begin(), takeRecordings.end(),
        [] (const TakeRecording& recording) { return recording.finalizationError.isNotEmpty(); });

    if (failed && recordingFinalizationProblem.isEmpty())
    {
        juce::StringArray failedNames;
        for (const auto& recording : takeRecordings)
            if (recording.finalizationError.isNotEmpty())
                failedNames.add (juce::String (recording.displayName));

        recordingFinalizationProblem = failedNames.size() == 1
            ? failedNames[0] + " could not finish its video file. The audio is safe; do not use that movie."
            : juce::String (failedNames.size())
                + " cameras could not finish their video files. The audio is safe; do not use those movies.";
    }

    recordingFinalizationState = failed ? RecordingFinalizationState::Failed
                                        : RecordingFinalizationState::Succeeded;
    finalizingDevices.clear();

    if (reconcileWhenFinalized)
        reconcileAfterTake();

    reconcileWhenFinalized = false;
    return true;
}

void CameraController::requestDiscovery (bool supersedePending)
{
    const auto discovery = discoveryState;
    bool launchWorker = false;

    {
        const std::lock_guard<std::mutex> guard (discovery->mutex);
        if (discovery->cancelled)
            return;

        // The status timer is observation, not a new user request. Let the
        // current OS call finish so a merely slow enumerator can eventually
        // publish; explicit actions are allowed to invalidate that snapshot.
        if (discovery->workerRunning && ! supersedePending)
            return;

        if (discovery->requested == 0)
            initialDiscoveryRequestedAtMs = juce::Time::getMillisecondCounterHiRes();

        ++discovery->requested;
        if (! discovery->workerRunning)
        {
            discovery->workerRunning = true;
            launchWorker = true;
        }
    }

    if (! launchWorker)
        return;

    try
    {
        std::thread ([discovery]
        {
            runDiscoveryWorker (discovery);
        }).detach();
    }
    catch (...)
    {
        // Thread construction can fail under severe resource pressure. Leave
        // the request pending and reopen the one-flight gate so a later UI
        // refresh can retry without blocking this caller.
        const std::lock_guard<std::mutex> guard (discovery->mutex);
        discovery->workerRunning = false;
        discovery->condition.notify_all();
    }
}

void CameraController::runDiscoveryWorker (std::shared_ptr<DiscoveryState> discovery)
{
   #if JUCE_WINDOWS
    const auto comResult = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const bool uninitialiseCom = SUCCEEDED (comResult);
   #endif

    for (;;)
    {
        uint64_t requested = 0;
        {
            const std::lock_guard<std::mutex> guard (discovery->mutex);
            if (discovery->cancelled)
            {
                discovery->workerRunning = false;
                discovery->condition.notify_all();
                break;
            }

            requested = discovery->requested;
        }

        juce::StringArray names;
        bool succeeded = false;
        try
        {
            names = juce::CameraDevice::getAvailableDevices();
            succeeded = true;
        }
        catch (...)
        {
            // A platform exception must not escape a detached worker. Keep the
            // previous applied snapshot and allow a later refresh to retry.
        }

        bool finished = false;
        {
            const std::lock_guard<std::mutex> guard (discovery->mutex);

            if (discovery->cancelled || ! succeeded)
            {
                discovery->workerRunning = false;
                finished = true;
            }
            else if (discovery->requested == requested)
            {
                // Only the newest requested generation may become visible.
                // If another refresh arrived while the OS was blocked, this
                // snapshot is stale and the loop immediately enumerates again.
                discovery->pendingDeviceNames = std::move (names);
                discovery->completed = requested;
                discovery->workerRunning = false;
                finished = true;
            }

            discovery->condition.notify_all();
        }

        if (finished)
            break;
    }

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
        if (previousIds.count (id) == 0
            && finalizationRetryRequiredIds.count (id) == 0)
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

    if (retryFailures)
        for (const auto& camera : selection.getAvailableCameras())
            if (selection.isEnabled (camera.id))
                finalizationRetryRequiredIds.erase (camera.id);

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
        {
            if (! stillEnabled)
                finalizationRetryRequiredIds.erase (it->first);
            it = openFailures.erase (it);
        }
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
            && (retryFailures
                || (openFailures.count (camera.id) == 0
                    && finalizationRetryRequiredIds.count (camera.id) == 0)))
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
                           "to use cameras and capture devices in your system privacy settings.";
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
    entry.openedAtMs = signalClockMs();

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
                         + ". Check the camera or capture-device signal and cables, close any other app using it, "
                           "then turn this camera off and back on.";
        return;
    }

    entry.viewerTarget->store (entry.nativeViewer.get());
    entry.osIndex = osIndex;
    open[id] = std::move (entry);
    openFailures.erase (id);
    finalizationRetryRequiredIds.erase (id);

    // A native preview component proves only that JUCE created a view. Capture
    // cards with no HDMI signal commonly satisfy that condition while drawing
    // a permanent black rectangle. Listen for an actual image from this exact
    // open generation and keep the UI in its explicit waiting state until one
    // arrives. The native viewer is deliberately created first: JUCE's macOS
    // backend requires that ordering before addListener starts the session.
    const auto frameMailbox = runtimeErrorMailbox;
    auto& stored = open.at (id);
    stored.frameListener = std::make_unique<FrameHeartbeatListener> (
        [frameMailbox, id, viewerRevision]
        {
            const std::lock_guard<std::mutex> guard (frameMailbox->mutex);
            frameMailbox->frames.push_back ({ id, viewerRevision });
        });
    stored.device->addListener (stored.frameListener.get());

    // Some camera backends can synchronously deliver their first frame while
    // the listener is being attached. Consume that proof before returning so a
    // working camera does not flash a false waiting state for one UI tick.
    applyPendingRuntimeEvents();

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
                                   || entry->second.startingThisTake
                                   || recordingCameraIds.count (id) > 0
                                   || startingCameraIds.count (id) > 0;

    if (wasRecordingThisTake && entry->second.device != nullptr)
        entry->second.device->stopRecording();

    if (entry->second.device != nullptr && entry->second.frameListener != nullptr)
        entry->second.device->removeListener (entry->second.frameListener.get());

    if (takeActive && wasRecordingThisTake)
        camerasDeferredUntilTakeEnds.insert (id);

    recordingCameraIds.erase (id);
    startingCameraIds.erase (id);
    recording = ! recordingCameraIds.empty();

    entry->second.viewerTarget->store (nullptr);
    ++viewerRevisions[id];

    if (takeActive && wasRecordingThisTake && kCameraBackendReportsFinalization
        && entry->second.device != nullptr)
        finalizingDevices.push_back ({ id, entry->second.viewerRevision, takeGeneration,
                                       std::move (entry->second.device) });

    open.erase (entry);
}
#endif

std::unique_ptr<juce::Component> CameraController::createViewer (const std::string& deviceId)
{
#if JUCE_USE_CAMERA
    const auto entry = open.find (deviceId);

    if (entry != open.end() && entry->second.nativeViewer != nullptr
        && entry->second.firstFrameReceived && ! entry->second.signalTimedOut)
        return std::make_unique<CameraViewerHost> (entry->second.viewerTarget);
#else
    juce::ignoreUnused (deviceId);
#endif

    return nullptr;
}

CameraController::SignalState CameraController::getSignalState (const std::string& deviceId) const
{
#if JUCE_USE_CAMERA
    const auto entry = open.find (deviceId);

    if (entry == open.end())
        return SignalState::NotOpen;

    if (entry->second.signalTimedOut)
        return SignalState::TimedOut;

    return entry->second.firstFrameReceived ? SignalState::Live : SignalState::Waiting;
#else
    juce::ignoreUnused (deviceId);
    return SignalState::NotOpen;
#endif
}

juce::String CameraController::getSignalStatusText (const std::string& deviceId) const
{
    const auto displayName = juce::String (selection.getDisplayName (deviceId));

    switch (getSignalState (deviceId))
    {
        case SignalState::Waiting:
            return "Waiting for video from " + displayName
                 + ". Turn the camera on; if it uses HDMI, check the HDMI and USB cables.";

        case SignalState::TimedOut:
            return "No video signal from " + displayName
                 + ". Check the camera or capture device; for HDMI, check the output and both cables, "
                   "then turn this camera off and back on.";

        case SignalState::NotOpen:
            return "No picture from " + displayName
                 + ". Close any other app using it, then turn this camera off and back on.";

        case SignalState::Live:
            break;
    }

    return {};
}

#if defined(SOBSTAGE_CAMERA_SIMULATION)
void CameraController::advanceSignalClockForTesting (double milliseconds)
{
#if JUCE_USE_CAMERA
    signalClockOffsetForTesting += juce::jmax (0.0, milliseconds);
    if (applySignalTimeouts())
        applySelection (false);
#else
    juce::ignoreUnused (milliseconds);
#endif
}
#endif

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

    if (recordingFinalizationState == RecordingFinalizationState::Waiting)
    {
        recordProblem = "The last camera files are still finishing. Wait before starting another take.";
        return false;
    }

    // A frame may have arrived since the last UI poll. Consume that bounded
    // mailbox now so the frozen take roster uses the freshest proof without
    // ever entering a platform camera call after audio has begun.
    const bool cameraStateChanged = applyPendingRuntimeEvents() || applySignalTimeouts();
    if (cameraStateChanged)
        applySelection (false);

    const auto extension = juce::CameraDevice::getFileExtension();
    int startRequests = 0;

    ++takeGeneration;
    takeActive = true;
    takePlans = selection.buildIntendedPlans();
    takeRecordings.clear();
    recordingCameraIds.clear();
    startingCameraIds.clear();
    finalizingDevices.clear();
    camerasDeferredUntilTakeEnds.clear();
    takeDeviceNameCounts.clear();
    ambiguousTakeDeviceNames.clear();
    recordingFinalizationState = RecordingFinalizationState::Idle;
    recordingFinalizationProblem.clear();
    reconcileWhenFinalized = false;

    // Its own field, and cleared each time this runs. A callback-backed start
    // failure below must remain visible rather than being erased after the
    // mailbox is drained.
    recordProblem.clear();

    for (const auto& camera : selection.getAvailableCameras())
        ++takeDeviceNameCounts[camera.displayName];

    // Collected so the problem below can NAME the cameras that did not start.
    // It used to say "2 of your cameras couldn't start recording" -- a count,
    // to someone looking at a rig of four, with the roster of display names
    // sitting right here in takePlans. Knowing which two is the whole
    // difference between checking one cable and checking all of them.
    juce::StringArray couldNotStart;

    for (const auto& plan : takePlans)
    {
        const auto entry = open.find (plan.deviceId);

        if (entry == open.end() || entry->second.device == nullptr
            || ! entry->second.firstFrameReceived || entry->second.signalTimedOut)
        {
            couldNotStart.add (juce::String (plan.displayName));
            continue;
        }

        // §6.2: the picture goes in the same session folder as the sound, under
        // the same naming rules, so one folder is still the whole take.
        const auto file = sessionFolder.getChildFile (juce::String (plan.fileName) + extension);
        entry->second.recordingFile = file;

        const auto mailbox = runtimeErrorMailbox;
        const auto viewerRevision = entry->second.viewerRevision;
        const auto generation = takeGeneration;
        entry->second.device->onRecordingStarted =
            [mailbox, id = plan.deviceId, viewerRevision, generation, audioStartMs]
            (const juce::File& startedFile)
            {
                const auto offset = audioStartMs > 0.0
                    ? juce::jmax (0.0,
                        (juce::Time::getMillisecondCounterHiRes() - audioStartMs) / 1000.0)
                    : 0.0;
                const std::lock_guard<std::mutex> guard (mailbox->mutex);
                mailbox->recordingsStarted.push_back (
                    { id, viewerRevision, generation, startedFile, offset });
            };
        entry->second.device->onRecordingFinished =
            [mailbox, id = plan.deviceId, viewerRevision, generation]
            (const juce::File& finishedFile, const juce::String& error)
            {
                const std::lock_guard<std::mutex> guard (mailbox->mutex);
                mailbox->recordingsFinished.push_back (
                    { id, viewerRevision, generation, finishedFile, error });
            };

        entry->second.startingThisTake = true;
        entry->second.recordingThisTake = false;
        startingCameraIds.insert (plan.deviceId);
        takeRecordings.push_back ({ plan.deviceId, plan.displayName,
                                    entry->second.device->getName().toStdString(), file,
                                    0.0, false, false, {} });

        // Quality 2 is JUCE's highest. There is no setting for this and there
        // should not be: nobody wants the take they cannot redo in medium.
        entry->second.device->startRecordingToFile (file, 2);

        ++startRequests;
    }

    // A physically absent camera cannot join a movie after the take begins.
    // Hide only those absent-at-t=0 ids from later topology snapshots. A
    // connected camera which failed because it is busy or privacy-blocked must
    // remain listed so its precise open failure is not replaced by the false
    // "system isn't listing it" diagnosis. takeActive already prevents either
    // kind from being opened into the running take.
    for (const auto& camera : selection.getUnavailableEnabledCameras())
        camerasDeferredUntilTakeEnds.insert (camera.id);

    // REC is confirmed only by AVFoundation's didStart callback. A submitted
    // request remains STARTING until that generation-scoped callback arrives.
    recording = false;
    applyPendingRuntimeEvents();

    if (! takePlans.empty() && startRequests < static_cast<int> (takePlans.size()))
    {
        // Named, in the same voice as the finalization failure above. The
        // count alone sent someone round every camera on the rig.
        recordProblem = (couldNotStart.size() == 1
                             ? couldNotStart[0] + " couldn't start recording."
                             : couldNotStart.joinIntoString (", ")
                                   + " couldn't start recording.")
                      + " The sound is recording either way.";

        // Belt and braces: if the roster and the request count ever disagree
        // -- a plan that reached startRecordingToFile and still did not count
        // -- fall back to the number rather than naming the wrong cameras.
        if (couldNotStart.isEmpty())
            recordProblem = juce::String (static_cast<int> (takePlans.size()) - startRequests)
                            + " of your cameras couldn't start "
                            "recording. The sound is recording either way.";
    }

    return startRequests > 0;
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

        if (takeRecording.started && takeRecording.finalizationComplete
            && takeRecording.finalizationError.isEmpty())
            inputs.push_back ({ file.getFileName().toStdString(),
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
        if (takeRecording.started && takeRecording.finalizationComplete
            && takeRecording.finalizationError.isEmpty())
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
                            recordingCameraIds.count (plan.deviceId) > 0,
                            startingCameraIds.count (plan.deviceId) > 0 });

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

bool CameraController::pollRecordingFinalization()
{
    return applyPendingCameraList();
}

void CameraController::stopRecordingInternal (bool reconcileForNextTake)
{
    const bool wasTakeActive = takeActive;

#if JUCE_USE_CAMERA
    for (auto& entry : open)
    {
        if ((entry.second.recordingThisTake || entry.second.startingThisTake)
            && entry.second.device != nullptr)
            entry.second.device->stopRecording();

        entry.second.recordingThisTake = false;
        entry.second.startingThisTake = false;
    }

    // The simulator can deliver both start and finish synchronously. Drain its
    // generation-tagged facts before deciding whether any file is still owed.
    applyPendingRuntimeEvents();
#endif

    recordingCameraIds.clear();
    startingCameraIds.clear();
    recording = false;
    takeActive = false;
    takeDeviceNameCounts.clear();
    ambiguousTakeDeviceNames.clear();

#if JUCE_USE_CAMERA
    if (! wasTakeActive)
        return;

    reconcileWhenFinalized = reconcileForNextTake;
    recordingFinalizationState = RecordingFinalizationState::Waiting;
    recordingFinalizationDeadlineMs = signalClockMs() + kCameraFinalizationTimeoutMs;

    if (! kCameraBackendReportsFinalization)
    {
        // Unpatched/unsupported JUCE camera backends expose no completion
        // callback. Their legacy stop contract is the only available boundary;
        // the patched macOS and Windows backends never enter this fallback.
        for (auto& recording : takeRecordings)
            recording.finalizationComplete = true;

        for (auto& [id, entry] : open)
        {
            juce::ignoreUnused (id);
            entry.device->onRecordingStarted = nullptr;
            entry.device->onRecordingFinished = nullptr;
            entry.recordingThisTake = false;
            entry.startingThisTake = false;
        }
    }

    finishRecordingFinalizationIfReady();
#else
    juce::ignoreUnused (wasTakeActive, reconcileForNextTake);
#endif
}

#if JUCE_USE_CAMERA
void CameraController::reconcileAfterTake()
{
    const auto deferred = camerasDeferredUntilTakeEnds;
    camerasDeferredUntilTakeEnds.clear();

    // applyDeviceNames deliberately hid absent-at-start or interrupted cameras
    // from the running take. Restore the latest unfiltered OS list only after
    // every movie is complete, so a reconnect cannot reuse its writer early.
    applyDeviceNames (lastAppliedDeviceNames);

    for (const auto& id : deferred)
    {
        const bool failedToFinalize = std::any_of (
            takeRecordings.begin(), takeRecordings.end(),
            [&] (const TakeRecording& recording)
            {
                return recording.deviceId == id
                    && recording.finalizationError.isNotEmpty();
            });

        if (! failedToFinalize && selection.isEnabled (id) && osIndexById.count (id) > 0)
            openFailures.erase (id);
    }

    applySelection (false);
}
#endif

} // namespace mma
