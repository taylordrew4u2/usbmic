#include "CameraSelection.h"
#include "SessionFolderNaming.h"
#include <algorithm>
#include <string>

namespace mma {

void CameraSelection::setAvailableCameras (std::vector<CameraDeviceInfo> cameras)
{
    available = std::move (cameras);

    // Every camera keeps an entry whether it is on or off, so turning one off
    // and unplugging it does not turn it back on when it returns.
    for (const auto& camera : available)
        choices.emplace (camera.id, Choice {});

    // The first camera ever seen is switched on, so plugging a webcam in and
    // looking at the screen is the whole setup. Only once: a second camera
    // appearing later is an addition the user makes, not one the app makes on
    // their behalf and their disk.
    if (! haveAutoEnabledOne && ! available.empty())
    {
        choices[available.front().id].enabled = true;
        haveAutoEnabledOne = true;
    }
}

void CameraSelection::setEnabled (const std::string& id, bool enabled)
{
    choices[id].enabled = enabled;

    // A user who turns a camera on by hand has made the decision this flag
    // exists to avoid making for them, so it must not be made again later.
    if (enabled)
        haveAutoEnabledOne = true;
}

bool CameraSelection::isEnabled (const std::string& id) const
{
    const auto it = choices.find (id);
    return it != choices.end() && it->second.enabled;
}

int CameraSelection::getEnabledCount() const
{
    int count = 0;

    for (const auto& camera : available)
        if (isEnabled (camera.id))
            ++count;

    return count;
}

void CameraSelection::setAssignedName (const std::string& id, const std::string& name)
{
    choices[id].assignedName = name;
}

std::string CameraSelection::getDisplayName (const std::string& id) const
{
    const auto choice = choices.find (id);

    if (choice != choices.end() && ! choice->second.assignedName.empty())
        return choice->second.assignedName;

    const auto camera = std::find_if (available.begin(), available.end(),
                                      [&id] (const CameraDeviceInfo& c) { return c.id == id; });

    return camera != available.end() ? camera->displayName : std::string();
}

std::vector<CameraPlan> CameraSelection::buildPlans() const
{
    std::vector<CameraPlan> plans;
    int index = 0;

    for (const auto& camera : available)
    {
        if (! isEnabled (camera.id))
            continue;

        ++index;

        CameraPlan plan;
        plan.deviceId = camera.id;
        plan.displayName = getDisplayName (camera.id);

        // §6.2's rules, unchanged: sanitized, numbered, sorting in the order
        // the cameras are listed rather than by whatever the OS calls them.
        // Padded by hand rather than with snprintf into a fixed buffer, which
        // cannot be sized so that the compiler stops warning about a truncation
        // the loop bound already rules out.
        const auto number = std::to_string (index);
        plan.fileName = "V" + (number.size() < 2 ? "0" + number : number)
                      + "_" + SessionFolderNaming::sanitizeName (plan.displayName);

        plans.push_back (std::move (plan));
    }

    return plans;
}

int64_t CameraSelection::getEstimatedBytesPerSecond() const
{
    return static_cast<int64_t> (getEnabledCount()) * kEstimatedVideoBytesPerSecond;
}

PreviewSettings CameraSelection::previewSettingsFor (PreviewQuality quality)
{
    // Low is 180 lines: enough to see who is in shot and whether the lens cap
    // is on, and a fraction of the pixels of the picture being recorded. Full
    // is 540, which is a real check of focus and framing without ever being
    // what determines what is written.
    return quality == PreviewQuality::Full ? PreviewSettings { 540 }
                                           : PreviewSettings { 180 };
}

} // namespace mma
