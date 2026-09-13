// Drives CameraController through JUCE's Windows-style camera lifecycle:
// DirectShow creates/removes its file capture filter synchronously. SobStage's
// pinned JUCE patch publishes start/finish callbacks around those real results,
// so the same truthful controller path can run on Windows and AVFoundation.

#include "../Simulation/Camera/juce_video/juce_video.h"
#include "../Source/App/CameraController.h"

#include <cstdio>

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    int checks = 0;
    int failures = 0;

    const auto check = [&checks, &failures] (bool condition, const char* label)
    {
        ++checks;
        std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", label);
        failures += condition ? 0 : 1;
    };

    std::printf ("CameraController synchronous DirectShow lifecycle callbacks\n");
    std::printf ("==========================================================\n");

    fakecamera::setDevices ({ "DirectShow Camera" });
    fakecamera::setOpenSucceeds (true);
    fakecamera::setViewerSucceeds (true);
    fakecamera::setAutoFrameOnListener (true);
    fakecamera::resetRecordingCallCounts();

    // The fake publishes both results inline, matching the patched DirectShow
    // backend rather than AVFoundation's later delegate queue.
    fakecamera::setAutoConfirmRecordingStart (true);
    fakecamera::setFinalizationMode (fakecamera::FinalizationMode::ImmediateSuccess);

    mma::CameraController controller;
    controller.refreshCameras();
    check (controller.waitForCameraRefresh (2000), "camera discovery completes");
    controller.getSelection().setEnabled ("DirectShow Camera", true);
    controller.applySelection (true);

    const auto takeFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-sync-camera", {}, false);
    check (takeFolder.createDirectory().wasOk(), "a take folder is available");
    check (controller.startRecording (takeFolder), "the synchronous camera starts");

    const auto states = controller.getTakeCameraStates();
    check (controller.isRecording() && states.size() == 1
               && states.front().recording && ! states.front().starting,
           "the successful DirectShow callback promotes the camera to REC");

    controller.stopRecording();
    check (controller.getRecordingFinalizationState()
               == mma::CameraController::RecordingFinalizationState::Succeeded,
           "the synchronous finish callback completes Stop immediately");
    check (! controller.isFinalizingRecording(),
           "the next take is not held after the real graph has closed");
    check (controller.getTakeVideoRecords().size() == 1
               && controller.getCombinedTakeInputs().size() == 1,
           "the synchronously finalized movie remains usable");

    takeFolder.deleteRecursively();

    std::printf ("\n%s (%d checks, %d failing)\n",
                 failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", checks, failures);
    return failures == 0 ? 0 : 1;
}
