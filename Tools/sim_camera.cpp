// Runs the real CameraController against a virtual camera layer.
//
// JUCE implements CameraDevice on macOS and Windows only, so on Linux the whole
// camera path is #if'd out and nothing has ever exercised it. This compiles
// CameraController.cpp unmodified with JUCE_USE_CAMERA=1 against
// Simulation/Camera's stand-in juce_video, and DRIVES it: cameras arriving,
// cameras going away, and two of the same model staying apart.
//
// Opening is covered too: the fake hands back a live object and counts it, so
// a camera that is already open when the OS stops listing it -- where a capture
// card on an HDMI input lands every time its source blinks -- can be driven.
// Only the viewer component is left alone: that needs a message manager, and
// would test the harness rather than the controller.

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

/// The list follows the OS, in both directions. Everything the app says about a
/// camera arriving or leaving is a diff of this list, so a list that does not
/// move is a rig change nobody can be told about.
void aCameraArrivingAndLeavingMovesTheList()
{
    std::printf ("\nA camera plugged in, then pulled out\n");

    fakecamera::setDevices ({});
    mma::CameraController controller;
    controller.refreshCameras();
    check (namesFrom (controller).empty(), "no cameras to begin with");

    fakecamera::setDevices ({ "Logitech C920" });
    controller.refreshCameras();

    const auto after = namesFrom (controller);
    check (after.size() == 1, "the camera appears once it is listed");
    check (! after.empty() && after.front() == "Logitech C920", "and is called what the OS calls it");

    fakecamera::setDevices ({});
    controller.refreshCameras();
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
    controller.refreshCameras();

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
    controller.refreshCameras();

    const auto before = controller.getSelection().getAvailableCameras();
    check (before.size() == 1, "listed to begin with");

    fakecamera::setDevices ({});
    controller.refreshCameras();

    fakecamera::setDevices ({ "Studio Cam" });
    controller.refreshCameras();

    const auto after = controller.getSelection().getAvailableCameras();
    check (after.size() == 1, "listed again after coming back");

    if (! before.empty() && ! after.empty())
        check (before.front().id == after.front().id, "and is the same camera, by id");
}

/// The HDMI case, and the one that only an app restart used to fix.
///
/// A capture card is listed as a camera whether or not anything is feeding its
/// input. Power the source down, switch the input, change its resolution, reseat
/// the cable, and the card drops off the bus and comes back seconds later -- an
/// ordinary evening for anyone shooting into one. The camera has to be open
/// again when it returns.
void aCameraThatBlinksIsOpenedAgainWhenItReturns()
{
    std::printf ("\nA camera that drops off the bus and comes back\n");

    fakecamera::setOpeningFails (false);
    fakecamera::resetCounters();
    fakecamera::setDevices ({ "Cam Link 4K" });

    mma::CameraController controller;
    controller.refreshCameras();

    const auto cameras = controller.getSelection().getAvailableCameras();
    check (cameras.size() == 1, "the capture card is listed");

    if (cameras.empty())
        return;

    const auto id = cameras.front().id;
    controller.getSelection().setEnabled (id, true);
    controller.applySelection();
    check (fakecamera::openCalls() == 1, "and is opened once switched on");
    check (fakecamera::liveDevices() == 1, "and is held open");

    // The HDMI source goes away. The card stops being listed.
    fakecamera::setDevices ({});
    controller.refreshCameras();
    controller.applySelection();
    check (fakecamera::liveDevices() == 0,
           "the handle is let go when the OS stops listing the camera");
    check (! controller.getProblem().isEmpty(),
           "and the take is told the camera went away");

    // And it comes back.
    fakecamera::setDevices ({ "Cam Link 4K" });
    controller.refreshCameras();
    controller.applySelection();
    check (fakecamera::openCalls() == 2, "it is opened again when it comes back");
    check (fakecamera::liveDevices() == 1, "and there is exactly one handle to it");
    check (controller.getProblem().isEmpty(), "and nothing is still complaining about it");
}

} // namespace

int main()
{
    std::printf ("CameraController, driven against a virtual camera layer\n");
    std::printf ("======================================================\n");

    aCameraArrivingAndLeavingMovesTheList();
    twoOfTheSameModelStayApart();
    aCameraThatComesBackKeepsItsIdentity();
    aCameraThatBlinksIsOpenedAgainWhenItReturns();

    std::printf ("\n%s (%d checks, %d failing)\n",
                 failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", checks, failures);
    return failures == 0 ? 0 : 1;
}
