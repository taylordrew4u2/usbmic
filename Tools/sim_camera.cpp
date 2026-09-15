// Runs the real CameraController against a virtual camera layer.
//
// JUCE implements CameraDevice on macOS and Windows only, so on Linux the whole
// camera path is #if'd out and nothing has ever exercised it. This compiles
// CameraController.cpp unmodified with JUCE_USE_CAMERA=1 against
// Simulation/Camera's stand-in juce_video, and DRIVES it: cameras arriving,
// cameras going away, and two of the same model staying apart.
//
// The stand-in also records open attempts. It deliberately does not create a
// viewer component, but that is enough to verify the controller's failed-open
// retry policy without needing a GUI message loop.

#include "../Simulation/Camera/juce_video/juce_video.h"
#include "../Source/App/CameraController.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

int checks = 0;
int failures = 0;

void check (bool condition, const std::string& what)
{
    ++checks;

    if (! condition)
        ++failures;

    std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", what.c_str());
}

std::vector<std::string> namesFrom (const mma::CameraController& controller)
{
    std::vector<std::string> out;

    for (const auto& cam : controller.getSelection().getAvailableCameras())
        out.push_back (cam.displayName);

    return out;
}

void refreshNow (mma::CameraController& controller)
{
    controller.refreshCameras();
    check (controller.waitForCameraRefresh (2000),
           "camera discovery completes and publishes its result");
}

/// The list follows the OS, in both directions. Everything the app says about a
/// camera arriving or leaving is a diff of this list, so a list that does not
/// move is a rig change nobody can be told about.
void aCameraArrivingAndLeavingMovesTheList()
{
    std::printf ("\nA camera plugged in, then pulled out\n");

    fakecamera::setDevices ({});
    mma::CameraController controller;
    refreshNow (controller);
    check (namesFrom (controller).empty(), "no cameras to begin with");
    check (! fakecamera::wasLastEnumerationOnThisThread(),
           "camera discovery runs off the caller/message thread");

    fakecamera::setDevices ({ "Logitech C920" });
    refreshNow (controller);

    const auto after = namesFrom (controller);
    check (after.size() == 1, "the camera appears once it is listed");
    check (! after.empty() && after.front() == "Logitech C920", "and is called what the OS calls it");

    fakecamera::setDevices ({});
    refreshNow (controller);
    check (namesFrom (controller).empty(), "and is gone once the OS stops listing it");
}

/// AVFoundation and DirectShow enumeration are unbounded platform calls. A
/// driver which never returns must not turn ordinary app shutdown into an
/// equally unbounded join, and its eventual result must remain detached from a
/// later controller instance.
void aStuckDiscoveryCannotHoldControllerLifetime()
{
    std::printf ("\nA camera discovery call stuck while the controller is destroyed\n");

    fakecamera::setDevices ({ "Late Camera" });
    fakecamera::pauseNextEnumerations (1);

    auto controller = std::make_unique<mma::CameraController>();
    controller->refreshCameras();
    check (fakecamera::waitForPausedEnumerationCount (1, 2000),
           "the simulated platform discovery is held inside its worker");

    // The delayed release keeps this test finite if a blocking join is ever
    // reintroduced. Correct teardown returns long before the safety release.
    std::thread safetyRelease ([]
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (750));
        fakecamera::releaseOnePausedEnumeration();
    });

    const auto before = std::chrono::steady_clock::now();
    controller.reset();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - before);

    check (elapsed < std::chrono::milliseconds (250),
           "controller destruction never waits for the stuck discovery call");
    safetyRelease.join();
    check (fakecamera::waitForNoPausedEnumerations (2000),
           "the late platform result is released after its owner is gone");

    fakecamera::setDevices ({ "Replacement Camera" });
    mma::CameraController replacement;
    refreshNow (replacement);
    check (namesFrom (replacement) == std::vector<std::string> { "Replacement Camera" },
           "a late result from the destroyed owner cannot enter a replacement controller");
}

/// A refresh requested while an earlier OS call is blocked supersedes that
/// call. Hold the replacement call too so the message thread gets a reliable
/// chance to prove the stale first snapshot was never published.
void aSupersededDiscoveryCannotPublishItsStaleSnapshot()
{
    std::printf ("\nA newer refresh supersedes a blocked camera snapshot\n");

    fakecamera::setDevices ({ "Old Camera" });
    fakecamera::pauseNextEnumerations (2);
    mma::CameraController controller;
    controller.refreshCameras();
    check (fakecamera::waitForPausedEnumerationCount (1, 2000),
           "the old snapshot is paused after being captured");

    fakecamera::setDevices ({ "New Camera" });
    controller.refreshCameras();
    fakecamera::releaseOnePausedEnumeration();
    check (fakecamera::waitForPausedEnumerationCount (2, 2000),
           "the worker discards the old generation and starts the replacement scan");

    check (! controller.applyPendingCameraList() && namesFrom (controller).empty(),
           "the superseded old snapshot is never exposed while the fresh scan is pending");

    fakecamera::releaseOnePausedEnumeration();
    check (controller.waitForCameraRefresh (2000),
           "the newest discovery generation completes and is published");
    check (namesFrom (controller) == std::vector<std::string> { "New Camera" },
           "only the newest camera snapshot reaches controller state");
    check (fakecamera::waitForNoPausedEnumerations (2000),
           "no simulated camera discovery remains paused");
}

/// The UI asks for topology every two seconds. Those observational polls must
/// not continually supersede an OS enumeration which is slow but will return,
/// or a scan taking just over two seconds could leave the camera list empty
/// forever despite completing successfully each time.
void periodicPollingCannotStarveASlowDiscovery()
{
    std::printf ("\nPeriodic polling while one camera discovery is slow\n");

    fakecamera::setDevices ({ "Slow Camera" });
    fakecamera::pauseNextEnumerations (1);
    mma::CameraController controller;
    controller.refreshCameras();
    check (fakecamera::waitForPausedEnumerationCount (1, 2000),
           "the slow camera snapshot remains in flight");

    for (int i = 0; i < 8; ++i)
        controller.refreshCamerasIfIdle();

    fakecamera::releaseOnePausedEnumeration();
    check (controller.waitForCameraRefresh (2000),
           "periodic polls do not invalidate the in-flight generation");
    check (namesFrom (controller) == std::vector<std::string> { "Slow Camera" },
           "the slow successful snapshot is eventually visible");
    check (fakecamera::waitForNoPausedEnumerations (2000),
           "the slow simulated enumeration is fully released");
}

/// §14.6, applied to pictures: two cameras of the same model enumerate with the
/// same product string. If they collapse into one entry, the second camera is
/// silently missing from the rig -- and from anything said about it.
void twoOfTheSameModelStayApart()
{
    std::printf ("\nTwo cameras of the same model\n");

    fakecamera::setDevices ({ "HD Webcam", "HD Webcam" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetOpenCallCount();
    mma::CameraController controller;
    refreshNow (controller);

    const auto& cameras = controller.getSelection().getAvailableCameras();
    check (cameras.size() == 2, "both are listed");

    if (cameras.size() == 2)
    {
        check (cameras[0].id != cameras[1].id, "and they are told apart by id, not by name");

        controller.getSelection().setEnabled (cameras[0].id, true);
        controller.getSelection().setEnabled (cameras[1].id, true);
        controller.applySelection (true);
        check (fakecamera::getOpenedDeviceIndices() == std::vector<int> ({ 0, 1 }),
               "each duplicate id opens its own OS occurrence instead of the first matching name");
    }
}

/// A camera that is unplugged and plugged back in is the same camera. Its id
/// has to come back the same, or every remembered choice about it is lost.
void aCameraThatComesBackKeepsItsIdentity()
{
    std::printf ("\nA camera unplugged and plugged back in\n");

    fakecamera::setDevices ({ "Studio Cam" });
    mma::CameraController controller;
    refreshNow (controller);

    const auto before = controller.getSelection().getAvailableCameras();
    check (before.size() == 1, "listed to begin with");

    fakecamera::setDevices ({});
    refreshNow (controller);

    fakecamera::setDevices ({ "Studio Cam" });
    refreshNow (controller);

    const auto after = controller.getSelection().getAvailableCameras();
    check (after.size() == 1, "listed again after coming back");

    if (! before.empty() && ! after.empty())
        check (before.front().id == after.front().id, "and is the same camera, by id");
}

/// A privacy denial or a camera held by another app can make openDevice block
/// and fail. The status timer still applies the selection twice a second; that
/// timer must retain the explanation without retrying the OS call forever.
void aFailedOpenWaitsForAnExplicitRetry()
{
    std::printf ("\nA camera that will not open\n");

    fakecamera::setDevices ({ "Busy Camera" });
    fakecamera::setOpenSucceeds (false);
    fakecamera::resetOpenCallCount();

    mma::CameraController controller;
    refreshNow (controller);
    const auto& cameras = controller.getSelection().getAvailableCameras();
    check (cameras.size() == 1, "the busy camera is listed");

    if (cameras.empty())
        return;

    controller.getSelection().setEnabled (cameras.front().id, true);
    controller.applySelection();
    check (fakecamera::getOpenCallCount() == 1, "the first selection attempts one open");
    check (controller.getProblem().isNotEmpty(), "the failed open remains explained");

    controller.applySelection();
    controller.applySelection();
    check (fakecamera::getOpenCallCount() == 1,
           "periodic selection refreshes do not retry the failed OS call");
    check (controller.getProblem().isNotEmpty(), "the explanation survives those refreshes");

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-busy", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a busy-camera take folder is available");
    check (! controller.startRecording (takeFolder),
           "a connected camera which failed to open is not claimed as recording");
    check (controller.getTakePlans().size() == 1
               && controller.getTakeVideoRecords().empty(),
           "the watchdog retains it but the manifest invents no movie");

    refreshNow (controller);
    check (namesFrom (controller).size() == 1,
           "a topology refresh keeps the connected failed camera listed during the take");
    check (controller.getProblem().containsIgnoreCase ("Couldn't open Busy Camera")
               && ! controller.getProblem().containsIgnoreCase ("system isn't listing"),
           "its busy/privacy error is not replaced by a false disconnected diagnosis");

    controller.stopRecording();

    fakecamera::setOpenSucceeds (true);
    controller.applySelection (true);
    check (fakecamera::getOpenCallCount() == 2, "an explicit action retries once");
    check (! controller.getProblem().containsIgnoreCase ("Couldn't open Busy Camera"),
           "a successful retry clears the open failure while retaining the prior take result");
    check (controller.startRecording (takeFolder),
           "the recovered camera can join the next take");
    check (controller.getProblem().isEmpty(),
           "the successful next take replaces the prior failed-start result");
    controller.stopRecording();

    takeFolder.deleteRecursively();
}

/// JUCE accepts an array index, then enumerates the OS again inside openDevice.
/// If the list moves in between, blindly trusting that index records the wrong
/// camera under the selected camera's name. The controller must reject the
/// mismatched instance and retry only after its worker publishes a fresh map.
void aReorderedListCannotOpenTheWrongCamera()
{
    std::printf ("\nA camera list that reorders between discovery and open\n");

    fakecamera::setDevices ({ "Wide Camera", "Close Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::resetOpenCallCount();

    mma::CameraController controller;
    refreshNow (controller);

    const auto cameras = controller.getSelection().getAvailableCameras();
    check (cameras.size() == 2, "both cameras are in the discovered snapshot");
    if (cameras.size() != 2)
        return;

    controller.getSelection().setEnabled ("Wide Camera", true);

    // Do not refresh the controller yet: this is the exact race window between
    // its completed background snapshot and JUCE's internal re-enumeration.
    fakecamera::setDevices ({ "Close Camera", "Wide Camera" });
    controller.applySelection (true);

    check (fakecamera::getOpenCallCount() == 1, "the stale index is attempted once");
    check (fakecamera::getLastOpenedDeviceName() == "Close Camera",
           "the fake exposes that JUCE's index now names the other camera");
    check (fakecamera::getLiveDeviceCount() == 0,
           "the mismatched camera is immediately discarded rather than retained");
    check (controller.getProblem().isNotEmpty(), "the topology retry is visible while pending");

    check (controller.waitForCameraRefresh (2000),
           "the mismatch automatically requests and applies a fresh discovery");
    check (fakecamera::getOpenCallCount() == 2, "the selected camera is retried once with the fresh map");
    check (fakecamera::getLastOpenedDeviceName() == "Wide Camera",
           "the retry opens the camera the user actually selected");
    check (fakecamera::getLiveDeviceCount() == 1, "only that selected camera remains open");
    check (controller.getProblem().isEmpty(), "the transient topology problem clears after recovery");
}

/// Pending topology is consumed while the camera drawer is visible too. That
/// path has no outer applySelection call, so the controller itself must release
/// an unplugged device and must not reuse it when the same camera comes back.
void topologyConsumptionOwnsOpenDeviceReconciliation()
{
    std::printf ("\nUnplug and reconnect while only topology refreshes run\n");

    fakecamera::setDevices ({ "Panel Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::resetOpenCallCount();

    {
        mma::CameraController controller;
        refreshNow (controller);
        controller.getSelection().setEnabled ("Panel Camera", true);
        controller.applySelection (true);

        check (fakecamera::getLiveDeviceCount() == 1, "the enabled camera begins open");

        fakecamera::setDevices ({});
        refreshNow (controller);
        check (fakecamera::getLiveDeviceCount() == 0,
               "consuming the unplug snapshot closes the device without an outer selection pass");

        fakecamera::setDevices ({ "Panel Camera" });
        refreshNow (controller);
        check (fakecamera::getOpenCallCount() == 2, "the remembered enabled camera is opened afresh");
        check (fakecamera::getLiveDeviceCount() == 1,
               "the reconnect owns one new device rather than the stale pre-unplug object");
    }

    check (fakecamera::getLiveDeviceCount() == 0, "controller teardown releases the reconnected device");
}

/// A capture card remembered as enabled must not disappear just because the
/// latest OS snapshot omits it. The UI needs an unavailable row and a concrete
/// reason before the user trusts a take that cannot contain that picture.
void anEnabledMissingCaptureCardStaysVisibleAsAProblem()
{
    std::printf ("\nA remembered capture card missing from the OS list\n");

    fakecamera::setDevices ({});
    mma::CameraController controller;
    controller.getSelection().setEnabled ("USB2 Video", true);
    controller.getSelection().setAssignedName ("USB2 Video", "HDMI wide");
    refreshNow (controller);

    const auto missing = controller.getSelection().getUnavailableEnabledCameras();
    check (missing.size() == 1, "the enabled missing camera remains in controller state");
    check (! missing.empty() && missing.front().displayName == "HDMI wide",
           "its remembered name remains available to the UI");
    check (controller.getProblem().containsIgnoreCase ("system isn't listing HDMI wide"),
           "the problem names the missing camera and the OS boundary");
    check (controller.getProblem().containsIgnoreCase ("HDMI signal"),
           "the recovery text covers a capture card's video source");
    check (fakecamera::getLiveDeviceCount() == 0,
           "a missing camera never creates a phantom open device");
}

/// Remembered choices are loaded synchronously but camera discovery is not.
/// The empty pre-discovery state must not be presented as an unplug or allowed
/// to become an empty frozen take plan.
void aRememberedCameraWaitsForTheFirstSnapshot()
{
    std::printf ("\nA remembered camera before the first OS snapshot\n");

    fakecamera::setDevices ({});
    fakecamera::setOpenSucceeds (true);

    mma::CameraController controller;
    controller.getSelection().setEnabled ("USB2 Video", true);
    controller.applySelection (true);

    check (controller.isInitialDiscoveryPending(),
           "the controller distinguishes pending discovery from an empty snapshot");
    check (controller.getProblem().isEmpty(),
           "a remembered camera is not falsely called missing before discovery");

    refreshNow (controller);
    check (! controller.isInitialDiscoveryPending(),
           "the first completed OS snapshot ends the pending state");
    check (controller.getProblem().containsIgnoreCase ("system isn't listing USB2 Video"),
           "an actually empty snapshot reports the remembered camera as missing");
}

/// Audio recording must never wait forever on a platform camera enumeration.
/// Starting before the first snapshot keeps the remembered camera in the
/// frozen plan as missing, and a late first snapshot is held for the next take.
void aTakeBeforeFirstDiscoveryKeepsTheCameraPlanHonest()
{
    std::printf ("\nA take starts before the first camera snapshot\n");

    fakecamera::setDevices ({ "Slow HDMI" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::resetOpenCallCount();
    fakecamera::resetRecordingCallCounts();

    mma::CameraController controller;
    controller.getSelection().setEnabled ("Slow HDMI", true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-first-scan", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a pre-discovery take folder is available");
    check (! controller.startRecording (takeFolder),
           "the missing pre-snapshot writer is not claimed as recording");
    check (controller.getTakePlans().size() == 1,
           "the remembered camera is retained in the frozen take plan");
    check (controller.getTakeVideoRecords().empty(),
           "the take manifest cannot claim a movie which never started");
    const auto states = controller.getTakeCameraStates();
    check (states.size() == 1 && ! states.front().recording,
           "the frozen roster reports the remembered camera as missing");

    refreshNow (controller);
    check (namesFrom (controller).empty()
               && fakecamera::getOpenCallCount() == 0
               && fakecamera::getStartRecordingCallCount() == 0,
           "the late first snapshot cannot join the active take");

    controller.stopRecording();
    check (namesFrom (controller).size() == 1
               && fakecamera::getOpenCallCount() == 1
               && fakecamera::getLiveDeviceCount() == 1,
           "the camera from the delayed snapshot opens immediately for the next take");

    takeFolder.deleteRecursively();
}

/// A camera absent at t=0 is part of the intended/frozen roster, but JUCE
/// cannot append it when it arrives later. Keep it unavailable for this take,
/// record the missing writer, and open it only for the next one.
void aCameraMissingAtTakeStartCannotJoinMidTake()
{
    std::printf ("\nA camera missing at take start arrives mid-take\n");

    fakecamera::setDevices ({});
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::resetOpenCallCount();
    fakecamera::resetRecordingCallCounts();

    mma::CameraController controller;
    controller.getSelection().setEnabled ("Late HDMI", true);
    refreshNow (controller);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-late-arrival", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a late-arrival take folder is available");
    check (! controller.startRecording (takeFolder),
           "the controller does not claim the missing camera started");
    check (controller.getTakePlans().size() == 1,
           "the missing enabled camera remains in the frozen take plan");
    check (controller.getTakeVideoRecords().empty(),
           "the missing camera is not written as a fictional movie in session metadata");

    auto states = controller.getTakeCameraStates();
    check (states.size() == 1 && ! states.front().recording,
           "the watchdog sees the expected camera as not recording");

    fakecamera::setDevices ({ "Late HDMI" });
    refreshNow (controller);
    check (namesFrom (controller).empty() && fakecamera::getLiveDeviceCount() == 0,
           "a mid-take arrival is not advertised or opened as part of this take");
    check (fakecamera::getStartRecordingCallCount() == 0,
           "the late camera never creates a misleading partial writer");

    controller.stopRecording();
    check (namesFrom (controller).size() == 1
               && fakecamera::getOpenCallCount() == 1
               && fakecamera::getLiveDeviceCount() == 1,
           "the remembered camera opens before an immediate next take");
    check (controller.startRecording (takeFolder)
               && fakecamera::getStartRecordingCallCount() == 1,
           "an immediate second take records the camera without waiting for another scan");
    controller.stopRecording();

    takeFolder.deleteRecursively();
}

/// AVFoundation's preview layer is tied to the capture session which created
/// it. Screen changes move one persistent layer between disposable hosts; they
/// must never ask JUCE to create a second preview for the running session.
void oneNativeViewerMovesBetweenScreens()
{
    std::printf ("\nOne native preview moves between UI hosts\n");

    fakecamera::setDevices ({ "HDMI Capture" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::resetViewerCreateCallCount();

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("HDMI Capture", true);
    controller.applySelection (true);

    check (fakecamera::getViewerCreateCallCount() == 1,
           "opening creates the native preview exactly once");

    auto mainHost = controller.createViewer ("HDMI Capture");
    check (mainHost != nullptr && mainHost->getNumChildComponents() == 1,
           "the first screen hosts that native preview");

    auto panelHost = controller.createViewer ("HDMI Capture");
    check (panelHost != nullptr && panelHost->getNumChildComponents() == 1,
           "the next screen reparents the same preview");
    check (mainHost != nullptr && mainHost->getNumChildComponents() == 0,
           "the previous host no longer retains the preview");
    check (fakecamera::getViewerCreateCallCount() == 1,
           "moving screens never asks JUCE for another preview layer");
}

/// A CameraDevice pointer is not proof that a preview was created. A failed
/// native layer must remain retryable, and its revision must invalidate the
/// same-id placeholder cached by both camera screens.
void aFailedViewerCanRecoverWithTheSameId()
{
    std::printf ("\nA failed native preview recovers under the same camera id\n");

    fakecamera::setDevices ({ "Capture Card" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (false);
    fakecamera::resetViewerCreateCallCount();

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Capture Card", true);
    controller.applySelection (true);

    const auto failedRevision = controller.getViewerRevision ("Capture Card");
    check (controller.createViewer ("Capture Card") == nullptr,
           "a missing native preview is never treated as a usable camera");
    check (controller.getProblem().isNotEmpty(), "the missing preview is explained");
    check (fakecamera::getViewerCreateCallCount() == 1,
           "the failed open made one native preview attempt");

    fakecamera::setViewerSucceeds (true);
    controller.applySelection (true);

    check (controller.getViewerRevision ("Capture Card") > failedRevision,
           "the successful same-id retry changes the UI cache revision");
    check (controller.createViewer ("Capture Card") != nullptr,
           "the retry now supplies a live preview host");
    check (fakecamera::getViewerCreateCallCount() == 2,
           "the explicit retry makes exactly one fresh native preview");
    check (controller.getProblem().isEmpty(), "successful preview recovery clears the problem");
}

/// A CameraDevice and native viewer can both exist while a capture card has no
/// HDMI input. That black handle is not a recordable camera until this exact
/// open generation has delivered a valid image callback.
void anOpenedCaptureCardWithoutAFrameIsNotRecordable()
{
    std::printf ("\nAn opened capture card with no video frames\n");

    fakecamera::setDevices ({ "Signal-less HDMI" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (false);
    fakecamera::resetRecordingCallCounts();

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Signal-less HDMI", true);
    controller.applySelection (true);

    check (controller.getSignalState ("Signal-less HDMI")
               == mma::CameraController::SignalState::Waiting,
           "a non-null device/viewer begins in waiting, not live");
    check (controller.createViewer ("Signal-less HDMI") == nullptr,
           "the black native viewer is hidden until an image proves it live");
    check (controller.getSignalStatusText ("Signal-less HDMI")
               .containsIgnoreCase ("Waiting for video"),
           "the waiting tile names the missing video signal plainly");

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-no-frame", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a no-frame take folder is available");
    check (! controller.startRecording (takeFolder),
           "a camera without first-frame proof is not reported as recording");
    check (fakecamera::getStartRecordingCallCount() == 0,
           "no movie writer is started for the unproved source");

    const auto states = controller.getTakeCameraStates();
    check (states.size() == 1 && ! states.front().recording,
           "the frozen take roster exposes that omission to the UI/watchdog");

    controller.stopRecording();
    takeFolder.deleteRecursively();
    fakecamera::setAutoFrameOnListener (true);
}

/// No-frame timeout is actionable, but not terminal. Some HDMI sources become
/// valid only after a resolution/HDCP handshake; their first late frame should
/// promote the same open generation without a destructive reopen.
void aLateFirstFrameRecoversAfterTheSignalTimeout()
{
    std::printf ("\nA late HDMI frame after the waiting timeout\n");

    fakecamera::setDevices ({ "Late Signal HDMI" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (false);
    fakecamera::resetOpenCallCount();
    fakecamera::resetListenerCallCounts();
    fakecamera::resetRecordingCallCounts();

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Late Signal HDMI", true);
    controller.applySelection (true);

    const auto waitingRevision = controller.getViewerRevision ("Late Signal HDMI");
    controller.advanceSignalClockForTesting (5001.0);
    check (controller.getSignalState ("Late Signal HDMI")
               == mma::CameraController::SignalState::TimedOut,
           "five seconds without a frame becomes a signal timeout");
    check (controller.getProblem().containsIgnoreCase ("For HDMI")
               && controller.getProblem().containsIgnoreCase ("USB cables"),
           "the timeout explains the camera and capture-card checks to make");
    check (controller.getViewerRevision ("Late Signal HDMI") > waitingRevision,
           "the timeout invalidates the cached waiting tile");

    fakecamera::emitFrame ("Late Signal HDMI");
    check (controller.applyPendingCameraList(),
           "a late frame is consumed without reopening the camera");
    check (controller.getSignalState ("Late Signal HDMI")
               == mma::CameraController::SignalState::Live,
           "the late current-generation frame proves the source live");
    check (controller.createViewer ("Late Signal HDMI") != nullptr,
           "the native preview is exposed only after that proof");
    check (controller.getProblem().isEmpty(),
           "the recovered signal clears its timeout explanation");
    check (fakecamera::getOpenCallCount() == 1,
           "late-signal recovery does not churn the platform device");
    check (fakecamera::getAddListenerCallCount() == 1
               && fakecamera::getRemoveListenerCallCount() == 0,
           "the throttled proof listener remains attached to detect later signal loss");

    controller.advanceSignalClockForTesting (5001.0);
    check (controller.getSignalState ("Late Signal HDMI")
               == mma::CameraController::SignalState::TimedOut,
           "an already-live source becomes SIGNAL LOST when its heartbeat stops");
    fakecamera::emitFrame ("Late Signal HDMI");
    controller.applyPendingCameraList();
    check (controller.getSignalState ("Late Signal HDMI")
               == mma::CameraController::SignalState::Live,
           "a later heartbeat restores that same source without reopening it");

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-late-frame", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a recovered-signal take folder is available");
    check (controller.startRecording (takeFolder)
               && fakecamera::getStartRecordingCallCount() == 1,
           "the now-proved camera is eligible for the next frozen take");
    controller.stopRecording();
    takeFolder.deleteRecursively();
    fakecamera::setAutoFrameOnListener (true);
}

/// A callback already queued when an identical capture card reconnects belongs
/// to the old platform object. Product-name identity must not let that event
/// certify the replacement generation.
void aStaleGenerationFrameCannotCertifyAReopenedCamera()
{
    std::printf ("\nA stale first frame after a same-id camera reopen\n");

    fakecamera::setDevices ({ "Same-id HDMI" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (false);
    fakecamera::resetOpenCallCount();

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Same-id HDMI", true);
    controller.applySelection (true);
    fakecamera::emitFrame ("Same-id HDMI"); // queued for generation one

    controller.getSelection().setEnabled ("Same-id HDMI", false);
    controller.applySelection (true);
    controller.getSelection().setEnabled ("Same-id HDMI", true);
    controller.applySelection (true); // generation two, still without a frame

    check (fakecamera::getOpenCallCount() == 2,
           "the same app id now owns a fresh platform camera generation");
    controller.applyPendingCameraList();
    check (controller.getSignalState ("Same-id HDMI")
               == mma::CameraController::SignalState::Waiting,
           "the queued old-generation frame cannot certify the replacement");
    check (controller.createViewer ("Same-id HDMI") == nullptr,
           "the replacement remains a waiting tile rather than a false preview");

    fakecamera::emitFrame ("Same-id HDMI");
    check (controller.applyPendingCameraList()
               && controller.getSignalState ("Same-id HDMI")
                      == mma::CameraController::SignalState::Live,
           "a frame from the replacement generation certifies only that generation");

    fakecamera::setAutoFrameOnListener (true);
}

/// JUCE can return a non-null CameraDevice and only later report that its input
/// or capture session failed. The callback is drained on the message-thread
/// poll, invalidates the viewer, and leaves an explicit-retry failure behind.
void aRuntimeCameraErrorInvalidatesThePreview()
{
    std::printf ("\nA runtime camera error invalidates its preview\n");

    fakecamera::setDevices ({ "Flaky HDMI" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Flaky HDMI", true);
    controller.applySelection (true);

    const auto liveRevision = controller.getViewerRevision ("Flaky HDMI");
    check (controller.createViewer ("Flaky HDMI") != nullptr,
           "the non-null device initially has a live preview host");

    fakecamera::emitRuntimeError ("Flaky HDMI", "capture input stopped");
    check (controller.applyPendingCameraList(),
           "the UI poll consumes a runtime error even without a topology update");
    check (fakecamera::getLiveDeviceCount() == 0,
           "the failed CameraDevice is closed instead of remaining black forever");
    check (controller.createViewer ("Flaky HDMI") == nullptr,
           "the invalid preview is no longer advertised as live");
    check (controller.getViewerRevision ("Flaky HDMI") > liveRevision,
           "runtime failure invalidates same-id UI caches");
    check (controller.getProblem().containsIgnoreCase ("capture input stopped"),
           "the platform's runtime reason is surfaced to the user");

    controller.applySelection (true);
    check (fakecamera::getLiveDeviceCount() == 1,
           "an explicit action can reopen the camera after the runtime failure");
}

/// Selection changes are for the next take. Applying one while a writer is
/// active must not finalize that movie early; the close happens immediately
/// after the take ends instead.
void switchingOffARecordingCameraWaitsForTheTakeToEnd()
{
    std::printf ("\nSwitching off a camera during a take is deferred\n");

    fakecamera::setDevices ({ "HDMI Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::resetRecordingCallCounts();

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("HDMI Camera", true);
    controller.applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-frozen-roster", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a temporary take folder is available");
    check (controller.startRecording (takeFolder), "the HDMI camera starts recording");

    controller.getSelection().setEnabled ("HDMI Camera", false);
    controller.applySelection (true);
    check (fakecamera::getActiveRecordingCount() == 1
               && fakecamera::getStopRecordingCallCount() == 0,
           "a mid-take setting change does not truncate the movie");

    controller.stopRecording();
    check (fakecamera::getActiveRecordingCount() == 0
               && fakecamera::getStopRecordingCallCount() == 1,
           "the writer is finalized exactly once when the take ends");
    check (fakecamera::getLiveDeviceCount() == 0,
           "the deferred off setting is applied after finalization");

    takeFolder.deleteRecursively();
}

/// Runtime failure suppresses same-take reopening, but once the take ends an
/// enabled device which is still in the OS snapshot gets one fresh preview.
void aRuntimeFailureRetriesAfterTheTakeEnds()
{
    std::printf ("\nA runtime camera failure retries after the take\n");

    fakecamera::setDevices ({ "Recovering HDMI" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::resetOpenCallCount();
    fakecamera::resetRecordingCallCounts();

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Recovering HDMI", true);
    controller.applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-runtime-retry", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a runtime-retry take folder is available");
    check (controller.startRecording (takeFolder), "the recovering camera starts recording");

    fakecamera::emitRuntimeError ("Recovering HDMI", "capture input stopped");
    check (controller.applyPendingCameraList(), "the in-take runtime error is consumed");
    controller.applyPendingCameraList(); // drain the simulator's synchronous didFinish
    check (fakecamera::getLiveDeviceCount() == 0,
           "the failed camera stays closed for the rest of the take");

    controller.stopRecording();
    check (fakecamera::getOpenCallCount() == 2 && fakecamera::getLiveDeviceCount() == 1,
           "the same listed camera is reopened automatically for the next take");
    check (controller.getProblem().isEmpty(),
           "a successful post-take retry clears the runtime failure");

    takeFolder.deleteRecursively();
}

/// JUCE cannot append a reconnected camera to the movie it finalized when the
/// device vanished. Reopening that camera's preview during the same take would
/// look like recovery while silently omitting every later frame. Keep the
/// camera absent, retain its partial file, and reopen the remembered preview
/// only after the take ends.
void aRecordedCameraThatReconnectsWaitsForTheNextTake()
{
    std::printf ("\nA recorded camera unplugged and replugged during a take\n");

    fakecamera::setDevices ({ "Recording Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::resetOpenCallCount();
    fakecamera::resetRecordingCallCounts();

    mma::CameraController controller;
    refreshNow (controller);

    const auto cameras = controller.getSelection().getAvailableCameras();
    check (cameras.size() == 1, "the recording camera is discovered");
    if (cameras.empty())
        return;

    controller.getSelection().setEnabled (cameras.front().id, true);
    controller.applySelection (true);
    check (fakecamera::getLiveDeviceCount() == 1, "the selected preview is open before the take");

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-continuity", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a temporary take folder is available");

    check (controller.startRecording (takeFolder), "the camera starts recording");
    check (controller.getTakeVideoRecords().empty(),
           "session metadata cannot claim the movie before its writer finalizes");
    check (fakecamera::getStartRecordingCallCount() == 1
               && fakecamera::getActiveRecordingCount() == 1,
           "one camera writer is active");

    auto takeStates = controller.getTakeCameraStates();
    check (takeStates.size() == 1 && takeStates.front().recording,
           "the frozen take roster reports that writer as recording");

    fakecamera::setDevices ({});
    refreshNow (controller);
    controller.applyPendingCameraList(); // consume the partial movie's didFinish
    check (fakecamera::getLiveDeviceCount() == 0, "unplug closes the camera device");
    check (! controller.isRecording(), "the controller no longer claims camera recording is live");

    takeStates = controller.getTakeCameraStates();
    check (takeStates.size() == 1 && ! takeStates.front().recording,
           "the lost camera remains in the take roster as absent");

    const auto partialInputs = controller.getCombinedTakeInputs();
    check (partialInputs.size() == 1 && ! partialInputs.front().videoFile.empty(),
           "the finalized partial movie remains available to the combiner");

    fakecamera::setDevices ({ "Recording Camera" });
    refreshNow (controller);
    check (namesFrom (controller).empty(),
           "a same-take reconnect is not advertised as an available live camera");
    check (fakecamera::getLiveDeviceCount() == 0,
           "the reconnected camera preview stays closed for this take");
    check (fakecamera::getOpenCallCount() == 1,
           "the reconnect does not make a second OS open attempt during the take");
    check (fakecamera::getStartRecordingCallCount() == 1
               && fakecamera::getActiveRecordingCount() == 0,
           "the reconnect is never falsely counted as resumed recording");

    takeStates = controller.getTakeCameraStates();
    check (takeStates.size() == 1 && ! takeStates.front().recording,
           "the watchdog-facing state stays lost after reconnect");

    controller.applySelection (true);
    check (fakecamera::getOpenCallCount() == 1 && fakecamera::getLiveDeviceCount() == 0,
           "even an explicit selection refresh cannot reopen it mid-take");

    controller.stopRecording();
    check (namesFrom (controller).size() == 1,
           "the remembered camera is advertised immediately after the take ends");
    check (fakecamera::getOpenCallCount() == 2 && fakecamera::getLiveDeviceCount() == 1,
           "its remembered preview reopens for the next take");
    check (fakecamera::getStartRecordingCallCount() == 1
               && fakecamera::getActiveRecordingCount() == 0,
           "reopening the preview does not retroactively restart recording");

    takeFolder.deleteRecursively();
}

/// JUCE identifies two same-model cameras only by occurrence in the current
/// device list. Once that count changes, no occurrence can be safely mapped to
/// the physical camera that owned it at take start. Both writers must stop and
/// the whole ambiguous group must wait for the next take.
void aChangingSameNameGroupIsDeferredTogether()
{
    std::printf ("\nTwo same-name recording cameras become ambiguous\n");

    fakecamera::setDevices ({ "Twin Camera", "Twin Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::resetOpenCallCount();
    fakecamera::resetRecordingCallCounts();

    mma::CameraController controller;
    refreshNow (controller);

    const auto cameras = controller.getSelection().getAvailableCameras();
    check (cameras.size() == 2, "both same-name cameras are discovered");
    if (cameras.size() != 2)
        return;

    for (const auto& camera : cameras)
        controller.getSelection().setEnabled (camera.id, true);

    controller.applySelection (true);
    check (fakecamera::getOpenCallCount() == 2 && fakecamera::getLiveDeviceCount() == 2,
           "both selected previews are open before the take");

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-ambiguity", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a second temporary take folder is available");
    check (controller.startRecording (takeFolder), "both same-name cameras start recording");
    check (fakecamera::getStartRecordingCallCount() == 2
               && fakecamera::getActiveRecordingCount() == 2,
           "both same-name camera writers are active");

    auto takeStates = controller.getTakeCameraStates();
    check (takeStates.size() == 2
               && takeStates[0].recording && takeStates[1].recording,
           "the frozen roster initially reports both writers");

    fakecamera::setDevices ({ "Twin Camera" });
    refreshNow (controller);
    controller.applyPendingCameraList(); // consume both synchronous didFinish callbacks
    check (namesFrom (controller).empty(),
           "a changed same-name count hides the entire ambiguous group");
    check (fakecamera::getLiveDeviceCount() == 0
               && fakecamera::getActiveRecordingCount() == 0,
           "both ambiguous camera devices and writers are closed");
    check (fakecamera::getStopRecordingCallCount() == 2,
           "each interrupted same-name movie is finalized exactly once");

    takeStates = controller.getTakeCameraStates();
    check (takeStates.size() == 2
               && ! takeStates[0].recording && ! takeStates[1].recording,
           "both watchdog-facing camera states remain lost");

    fakecamera::setDevices ({ "Twin Camera", "Twin Camera" });
    refreshNow (controller);
    check (namesFrom (controller).empty() && fakecamera::getOpenCallCount() == 2,
           "restoring the count cannot undo same-take identity ambiguity");

    controller.stopRecording();
    refreshNow (controller);
    check (namesFrom (controller).size() == 2,
           "the same-name group is advertised again after the take");
    check (fakecamera::getOpenCallCount() == 4 && fakecamera::getLiveDeviceCount() == 2,
           "both remembered previews reopen for the next take");
    check (fakecamera::getStartRecordingCallCount() == 2
               && fakecamera::getActiveRecordingCount() == 0,
           "neither reopened preview is misreported as recording");

    takeFolder.deleteRecursively();
}

/// Submitting startRecordingToFile is not proof that AVFoundation accepted the
/// movie. The UI stays STARTING until didStart, and the alignment offset is
/// measured at that callback rather than at the request call.
void recordingTruthWaitsForTheStartCallback()
{
    std::printf ("\nRecording state waits for the camera start callback\n");

    fakecamera::setDevices ({ "Slow-start Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetRecordingCallCounts();
    fakecamera::setAutoConfirmRecordingStart (false);

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Slow-start Camera", true);
    controller.applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-slow-start", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a slow-start take folder is available");

    const auto audioStart = juce::Time::getMillisecondCounterHiRes() - 250.0;
    check (controller.startRecording (takeFolder, audioStart),
           "the camera start request is submitted");
    auto states = controller.getTakeCameraStates();
    check (! controller.isRecording() && states.size() == 1
               && states.front().starting && ! states.front().recording,
           "submitted but unconfirmed video reads STARTING, never REC");
    check (fakecamera::getPendingRecordingStartCount() == 1,
           "the simulator retains the delayed didStart fact");

    fakecamera::completePendingRecordingStarts();
    controller.applyPendingCameraList();
    states = controller.getTakeCameraStates();
    check (controller.isRecording() && states.size() == 1
               && ! states.front().starting && states.front().recording,
           "only didStart promotes the matching take generation to REC");

    controller.stopRecording();
    const auto inputs = controller.getCombinedTakeInputs();
    check (inputs.size() == 1 && inputs.front().videoStartOffsetSeconds >= 0.20,
           "camera alignment is measured at didStart, after the delayed request");

    takeFolder.deleteRecursively();
    fakecamera::setAutoConfirmRecordingStart (true);
}

/// stopRecording must return immediately while AVFoundation closes the movie.
/// No manifest/combiner input is exposed and no second take is allowed until
/// the generation- and file-matched didFinish arrives.
void delayedFinalizationBlocksClaimsAndTheNextTake()
{
    std::printf ("\nA delayed camera movie finalization\n");

    fakecamera::setDevices ({ "Finishing Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetRecordingCallCounts();
    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::DelayedSuccess);

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Finishing Camera", true);
    controller.applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-delayed-finish", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a delayed-finalization folder is available");
    check (controller.startRecording (takeFolder), "the delayed writer starts");

    const auto before = std::chrono::steady_clock::now();
    controller.stopRecording();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - before);

    check (elapsed < std::chrono::milliseconds (250),
           "Stop returns without waiting for AVFoundation's movie finalizer");
    check (controller.getRecordingFinalizationState()
               == mma::CameraController::RecordingFinalizationState::Waiting,
           "the controller explicitly waits for didFinish");
    check (controller.getTakeVideoRecords().empty()
               && controller.getCombinedTakeInputs().empty(),
           "metadata and combining cannot consume an unfinished movie");
    check (! controller.startRecording (takeFolder),
           "a rapid second take is rejected while the previous file is finishing");

    fakecamera::completePendingFinalizations();
    controller.pollRecordingFinalization();
    check (controller.getRecordingFinalizationState()
               == mma::CameraController::RecordingFinalizationState::Succeeded,
           "the matching didFinish releases the finalization gate");
    check (controller.getTakeVideoRecords().size() == 1
               && controller.getCombinedTakeInputs().size() == 1,
           "only the finalized movie becomes a manifest and combiner input");

    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::ImmediateSuccess);
    check (controller.startRecording (takeFolder),
           "a new take may start once the prior generation is complete");
    controller.stopRecording();
    takeFolder.deleteRecursively();
}

/// A backend can report didFinish without ever reporting didStart. Even an
/// empty platform error cannot turn that into a successful zero-frame movie.
void finishWithoutStartIsAStartFailure()
{
    std::printf ("\nA camera finishes without ever confirming start\n");

    fakecamera::setDevices ({ "Rejected Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetRecordingCallCounts();
    fakecamera::setAutoConfirmRecordingStart (false);
    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::ImmediateSuccess);

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Rejected Camera", true);
    controller.applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-no-didstart", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a rejected-start folder is available");
    check (controller.startRecording (takeFolder), "the rejected start request is submitted");
    controller.stopRecording();

    check (controller.getRecordingFinalizationState()
               == mma::CameraController::RecordingFinalizationState::Failed,
           "didFinish without didStart fails closed even with no NSError");
    check (controller.getProblem().containsIgnoreCase ("confirmed recording started"),
           "the missing start confirmation is explained");
    check (controller.getTakeVideoRecords().empty()
               && controller.getCombinedTakeInputs().empty(),
           "the unconfirmed movie is never claimed as saved or combinable");

    takeFolder.deleteRecursively();
    fakecamera::setAutoConfirmRecordingStart (true);
}

/// DirectShow can fail while creating its file-capture graph even though
/// JUCE's public startRecordingToFile method returns void. The patched backend
/// publishes that rejection instead of letting the controller synthesize REC.
void aSynchronousWriterStartFailureNeverClaimsAFile()
{
    std::printf ("\nA synchronous camera writer fails to create its file graph\n");

    fakecamera::setDevices ({ "Failing DirectShow Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetRecordingCallCounts();
    fakecamera::setStartRecordingSucceeds (false);

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Failing DirectShow Camera", true);
    controller.applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-sync-start-fail", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a synchronous-failure folder is available");
    check (controller.startRecording (takeFolder), "the camera writer request is submitted");

    const auto states = controller.getTakeCameraStates();
    check (! controller.isRecording() && states.size() == 1
               && ! states.front().recording && ! states.front().starting,
           "a rejected synchronous writer never displays REC or STARTING");
    check (controller.getProblem().containsIgnoreCase ("start recording video"),
           "the backend's start rejection is explained immediately");

    controller.stopRecording();
    check (controller.getRecordingFinalizationState()
               == mma::CameraController::RecordingFinalizationState::Failed,
           "the rejected writer remains failed at take finalization");
    check (controller.getTakeVideoRecords().empty()
               && controller.getCombinedTakeInputs().empty(),
           "no metadata or combine input is invented for the absent file");

    takeFolder.deleteRecursively();
    fakecamera::setStartRecordingSucceeds (true);
}

/// A writer that fails its finalization IMMEDIATELY, which is the common shape
/// of the failure and the one nothing here had ever produced.
///
/// FinalizationMode::ImmediateError existed in the camera stand-in and no
/// scenario used it. That matters beyond coverage: when every didFinish has
/// already arrived by the time stopRecording returns, isFinalizingRecording()
/// is false, and Application's stop took its SYNCHRONOUS branch -- which never
/// read getRecordingFinalizationProblem(). So the one sentence warning that a
/// movie is unusable was produced and dropped precisely when it was true, and
/// the app said "Saved to ..." over the top of it.
///
/// This pins the state that made that possible: finalization already finished,
/// and a problem waiting to be read. Application now reads it on both paths.
void anImmediateFinalizationErrorIsReadyBeforeTheStopReturns()
{
    std::printf ("\nA camera writer which fails its finalization at once\n");

    fakecamera::setDevices ({ "Failing Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetOpenCallCount();
    fakecamera::resetRecordingCallCounts();
    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::ImmediateError);

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Failing Camera", true);
    controller.applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-immediate-error", {}, false);
    check (takeFolder.createDirectory().wasOk(), "an immediate-error folder is available");
    check (controller.startRecording (takeFolder), "the writer begins");

    controller.stopRecording();

    // The two halves of the hazard, asserted together. Either alone is
    // harmless; it is the combination that the old stop path threw away.
    check (! controller.isFinalizingRecording(),
           "finalization is already over when stopRecording returns");
    check (controller.getRecordingFinalizationState()
               == mma::CameraController::RecordingFinalizationState::Failed,
           "and it ended in failure");
    check (controller.getRecordingFinalizationProblem().isNotEmpty(),
           "with a problem waiting to be read, on the path that used to drop it");
    check (controller.getRecordingFinalizationProblem().containsIgnoreCase ("do not use"),
           "and it tells the user not to trust the movie");
    check (controller.getTakeVideoRecords().empty(),
           "and no movie is claimed for the take");

    takeFolder.deleteRecursively();
    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::ImmediateSuccess);
}

/// A missing didFinish is bounded. Once timed out, the unsafe device generation
/// stays closed through periodic discovery and only an explicit retry can open
/// it again.
void aNeverFinishingWriterFailsClosedAndDoesNotAutoReopen()
{
    std::printf ("\nA camera writer which never finishes\n");

    fakecamera::setDevices ({ "Wedged Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetOpenCallCount();
    fakecamera::resetRecordingCallCounts();
    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::Never);

    mma::CameraController controller;
    refreshNow (controller);
    controller.getSelection().setEnabled ("Wedged Camera", true);
    controller.applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-never-finish", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a never-finalization folder is available");
    check (controller.startRecording (takeFolder), "the wedged writer begins");
    controller.stopRecording();
    check (controller.isFinalizingRecording()
               && fakecamera::getPendingFinalizationCount() == 1,
           "the missing callback leaves one bounded finalizer pending");

    controller.advanceSignalClockForTesting (15001.0);
    controller.pollRecordingFinalization();
    check (controller.getRecordingFinalizationState()
               == mma::CameraController::RecordingFinalizationState::Failed,
           "the 15-second deadline ends in an explicit failed state");
    check (controller.getRecordingFinalizationProblem().containsIgnoreCase ("did not finish")
               && controller.getTakeVideoRecords().empty(),
           "the audio-safe/video-unsafe outcome is reported without a movie claim");
    check (fakecamera::getOpenCallCount() == 1 && fakecamera::getLiveDeviceCount() == 0,
           "the timed-out platform device is closed and not immediately reopened");

    refreshNow (controller);
    check (fakecamera::getOpenCallCount() == 1 && fakecamera::getLiveDeviceCount() == 0,
           "periodic topology refresh cannot reuse the unsafe generation");

    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::ImmediateSuccess);
    controller.applySelection (true);
    check (fakecamera::getOpenCallCount() == 2 && fakecamera::getLiveDeviceCount() == 1,
           "an explicit retry can open a fresh camera generation");

    takeFolder.deleteRecursively();
}

/// Destruction cannot join a platform movie callback. The fake retains the
/// delayed callback by raw device pointer, so this also proves teardown removes
/// it rather than allowing a late use-after-free.
void shutdownDuringFinalizationIsBoundedAndLifetimeSafe()
{
    std::printf ("\nShutdown while a camera movie is still finalizing\n");

    fakecamera::setDevices ({ "Shutdown Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetRecordingCallCounts();
    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::DelayedSuccess);

    auto controller = std::make_unique<mma::CameraController>();
    refreshNow (*controller);
    controller->getSelection().setEnabled ("Shutdown Camera", true);
    controller->applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-camera-shutdown-finish", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a shutdown-finalization folder is available");
    check (controller->startRecording (takeFolder), "the shutdown writer starts");
    controller->stopRecordingForShutdown();
    check (controller->isFinalizingRecording(), "shutdown observes the unfinished movie");

    const auto before = std::chrono::steady_clock::now();
    controller.reset();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - before);
    check (elapsed < std::chrono::milliseconds (250),
           "controller destruction never joins a delayed movie callback");
    check (fakecamera::getPendingFinalizationCount() == 0,
           "device destruction removes the callback target before it can fire late");

    fakecamera::completePendingFinalizations();
    takeFolder.deleteRecursively();
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("CameraController, driven against a virtual camera layer\n");
    std::printf ("======================================================\n");

    aCameraArrivingAndLeavingMovesTheList();
    aStuckDiscoveryCannotHoldControllerLifetime();
    aSupersededDiscoveryCannotPublishItsStaleSnapshot();
    periodicPollingCannotStarveASlowDiscovery();
    twoOfTheSameModelStayApart();
    aCameraThatComesBackKeepsItsIdentity();
    aFailedOpenWaitsForAnExplicitRetry();
    aReorderedListCannotOpenTheWrongCamera();
    topologyConsumptionOwnsOpenDeviceReconciliation();
    anEnabledMissingCaptureCardStaysVisibleAsAProblem();
    aRememberedCameraWaitsForTheFirstSnapshot();
    aTakeBeforeFirstDiscoveryKeepsTheCameraPlanHonest();
    aCameraMissingAtTakeStartCannotJoinMidTake();
    oneNativeViewerMovesBetweenScreens();
    aFailedViewerCanRecoverWithTheSameId();
    anOpenedCaptureCardWithoutAFrameIsNotRecordable();
    aLateFirstFrameRecoversAfterTheSignalTimeout();
    aStaleGenerationFrameCannotCertifyAReopenedCamera();
    aRuntimeCameraErrorInvalidatesThePreview();
    switchingOffARecordingCameraWaitsForTheTakeToEnd();
    aRuntimeFailureRetriesAfterTheTakeEnds();
    aRecordedCameraThatReconnectsWaitsForTheNextTake();
    aChangingSameNameGroupIsDeferredTogether();
    recordingTruthWaitsForTheStartCallback();
    delayedFinalizationBlocksClaimsAndTheNextTake();
    finishWithoutStartIsAStartFailure();
    aSynchronousWriterStartFailureNeverClaimsAFile();
    anImmediateFinalizationErrorIsReadyBeforeTheStopReturns();
    aNeverFinishingWriterFailsClosedAndDoesNotAutoReopen();
    shutdownDuringFinalizationIsBoundedAndLifetimeSafe();

    std::printf ("\n%s (%d checks, %d failing)\n",
                 failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", checks, failures);
    return failures == 0 ? 0 : 1;
}
