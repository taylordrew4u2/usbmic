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

#include <cstdio>
#include <string>
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

/// §14.6, applied to pictures: two cameras of the same model enumerate with the
/// same product string. If they collapse into one entry, the second camera is
/// silently missing from the rig -- and from anything said about it.
void twoOfTheSameModelStayApart()
{
    std::printf ("\nTwo cameras of the same model\n");

    fakecamera::setDevices ({ "HD Webcam", "HD Webcam" });
    mma::CameraController controller;
    refreshNow (controller);

    const auto& cameras = controller.getSelection().getAvailableCameras();
    check (cameras.size() == 2, "both are listed");

    if (cameras.size() == 2)
        check (cameras[0].id != cameras[1].id, "and they are told apart by id, not by name");
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

    fakecamera::setOpenSucceeds (true);
    controller.applySelection (true);
    check (fakecamera::getOpenCallCount() == 2, "an explicit action retries once");
    check (controller.getProblem().isEmpty(), "a successful retry clears the explanation");
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
    check (fakecamera::getStartRecordingCallCount() == 1
               && fakecamera::getActiveRecordingCount() == 1,
           "one camera writer is active");

    auto takeStates = controller.getTakeCameraStates();
    check (takeStates.size() == 1 && takeStates.front().recording,
           "the frozen take roster reports that writer as recording");

    fakecamera::setDevices ({});
    refreshNow (controller);
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
    refreshNow (controller);
    check (namesFrom (controller).size() == 1,
           "the remembered camera is advertised again after the take ends");
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

} // namespace

int main()
{
    std::printf ("CameraController, driven against a virtual camera layer\n");
    std::printf ("======================================================\n");

    aCameraArrivingAndLeavingMovesTheList();
    twoOfTheSameModelStayApart();
    aCameraThatComesBackKeepsItsIdentity();
    aFailedOpenWaitsForAnExplicitRetry();
    aReorderedListCannotOpenTheWrongCamera();
    topologyConsumptionOwnsOpenDeviceReconciliation();
    aRecordedCameraThatReconnectsWaitsForTheNextTake();
    aChangingSameNameGroupIsDeferredTogether();

    std::printf ("\n%s (%d checks, %d failing)\n",
                 failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", checks, failures);
    return failures == 0 ? 0 : 1;
}
