#include "TestFramework.h"
#include "Core/CameraSelection.h"

using namespace mma;

namespace {

std::vector<CameraDeviceInfo> twoCameras()
{
    return { { "cam-a", "Logitech C920" }, { "cam-b", "Built-in Camera" } };
}

} // namespace

TEST_CASE (CameraSelection_firstCameraIsOnAndTheRestAreNot)
{
    CameraSelection selection;
    selection.setAvailableCameras (twoCameras());

    // §10.1's spirit without §10.1's cost: one camera works with no setup, and
    // a second one does not quietly double the bytes hitting the card.
    REQUIRE (selection.isEnabled ("cam-a"));
    REQUIRE_FALSE (selection.isEnabled ("cam-b"));
    REQUIRE (selection.getEnabledCount() == 1);
}

TEST_CASE (CameraSelection_noCamerasEnablesNothing)
{
    CameraSelection selection;
    selection.setAvailableCameras ({});

    REQUIRE (selection.getEnabledCount() == 0);
    REQUIRE (selection.buildPlans().empty());
}

TEST_CASE (CameraSelection_aCameraPluggedInLaterIsNotSwitchedOnBehindTheUser)
{
    CameraSelection selection;
    selection.setAvailableCameras ({ { "cam-a", "Logitech C920" } });
    selection.setEnabled ("cam-a", false);

    // The auto-enable is a first-run convenience, not a rule. Once the user has
    // switched everything off, plugging a camera in must not switch one back on.
    selection.setAvailableCameras (twoCameras());

    REQUIRE (selection.getEnabledCount() == 0);
}

TEST_CASE (CameraSelection_choicesSurviveAnUnplug)
{
    CameraSelection selection;
    selection.setAvailableCameras (twoCameras());
    selection.setEnabled ("cam-b", true);
    selection.setAssignedName ("cam-b", "Wide shot");

    // Unplugged...
    selection.setAvailableCameras ({ { "cam-a", "Logitech C920" } });
    REQUIRE (selection.getEnabledCount() == 1);

    // ...and back, with the answer the user already gave about it.
    selection.setAvailableCameras (twoCameras());
    REQUIRE (selection.isEnabled ("cam-b"));
    REQUIRE (selection.getDisplayName ("cam-b") == std::string ("Wide shot"));
}

TEST_CASE (CameraSelection_planNamesFollowTheSessionNamingRules)
{
    CameraSelection selection;
    selection.setAvailableCameras (twoCameras());
    selection.setEnabled ("cam-b", true);
    selection.setAssignedName ("cam-a", "Kitchen / Wide!!");

    const auto plans = selection.buildPlans();

    REQUIRE ((int) plans.size() == 2);
    // §6.2 sanitizing, and a V prefix so the pictures do not land in the middle
    // of the numbered audio stems when the folder is sorted by name.
    REQUIRE (plans[0].fileName == std::string ("V01_Kitchen-Wide"));
    REQUIRE (plans[1].fileName == std::string ("V02_Built-in-Camera"));
    REQUIRE (plans[0].deviceId == std::string ("cam-a"));
}

TEST_CASE (CameraSelection_disabledCamerasAreNotNumbered)
{
    CameraSelection selection;
    selection.setAvailableCameras (twoCameras());
    selection.setEnabled ("cam-a", false);
    selection.setEnabled ("cam-b", true);

    const auto plans = selection.buildPlans();

    // The one camera in the take is V01, not V02 -- the number counts what is
    // being recorded, not what happens to be plugged in.
    REQUIRE ((int) plans.size() == 1);
    REQUIRE (plans[0].fileName == std::string ("V01_Built-in-Camera"));
}

TEST_CASE (CameraSelection_videoCountsAgainstRemainingTime)
{
    CameraSelection selection;
    REQUIRE (selection.getEstimatedBytesPerSecond() == (int64_t) 0);

    selection.setAvailableCameras (twoCameras());
    REQUIRE (selection.getEstimatedBytesPerSecond() == CameraSelection::kEstimatedVideoBytesPerSecond);

    selection.setEnabled ("cam-b", true);
    REQUIRE (selection.getEstimatedBytesPerSecond() == 2 * CameraSelection::kEstimatedVideoBytesPerSecond);
}

TEST_CASE (CameraSelection_previewSizeNeverDrivesCaptureSize)
{
    // The only thing the preview setting decides is how tall the picture is
    // drawn. Both modes exist so the live view can be cheap; neither is
    // consulted about what gets written.
    REQUIRE (CameraSelection::previewSettingsFor (PreviewQuality::Low).maxViewHeight
                     < CameraSelection::previewSettingsFor (PreviewQuality::Full).maxViewHeight);
    REQUIRE (CameraSelection::previewSettingsFor (PreviewQuality::Low).maxViewHeight > 0);
}
