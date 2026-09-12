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
    {
        auto& choice = choices[camera.id];
        choice.lastDisplayName = camera.displayName;
    }

    // Discovery is not consent. A newly seen camera stays off until the user
    // deliberately enables it in the camera panel; otherwise a fresh launch
    // can light the built-in camera and raise a privacy prompt before the user
    // has asked SobStage to use video at all.
}

std::vector<CameraDeviceInfo> CameraSelection::getUnavailableEnabledCameras() const
{
    std::vector<CameraDeviceInfo> unavailable;

    for (const auto& [id, choice] : choices)
    {
        if (! choice.enabled)
            continue;

        bool isAvailable = false;
        for (const auto& camera : available)
            if (camera.id == id)
            {
                isAvailable = true;
                break;
            }

        if (! isAvailable)
            unavailable.push_back ({ id, getDisplayName (id) });
    }

    return unavailable;
}

std::vector<CameraDeviceInfo> CameraSelection::getKnownCameras() const
{
    std::vector<CameraDeviceInfo> known;
    known.reserve (choices.size());

    for (const auto& entry : choices)
        known.push_back ({ entry.first, getDisplayName (entry.first) });

    return known;
}

void CameraSelection::setEnabled (const std::string& id, bool enabled)
{
    choices[id].enabled = enabled;

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

    if (camera != available.end())
        return camera->displayName;

    if (choice != choices.end() && ! choice->second.lastDisplayName.empty())
        return choice->second.lastDisplayName;

    // An old settings entry may predate lastDisplayName. Its id is still much
    // more useful than a blank label when explaining which device is missing.
    return id;
}

std::vector<CameraPlan> CameraSelection::buildPlans() const
{
    std::vector<CameraPlan> plans;
    std::vector<std::string> enabledIds;

    // Connected cameras keep the OS order the operator sees. A remembered
    // missing camera is not a file this plan can honestly promise.
    for (const auto& camera : available)
        if (isEnabled (camera.id))
            enabledIds.push_back (camera.id);

    int index = 0;

    for (const auto& id : enabledIds)
    {
        ++index;

        CameraPlan plan;
        plan.deviceId = id;
        plan.displayName = getDisplayName (id);

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

std::vector<CameraPlan> CameraSelection::buildIntendedPlans() const
{
    auto plans = buildPlans();
    std::vector<std::string> includedIds;
    includedIds.reserve (plans.size());

    for (const auto& plan : plans)
        includedIds.push_back (plan.deviceId);

    for (const auto& [id, choice] : choices)
    {
        if (! choice.enabled
            || std::find (includedIds.begin(), includedIds.end(), id) != includedIds.end())
            continue;

        CameraPlan plan;
        plan.deviceId = id;
        plan.displayName = getDisplayName (id);

        const auto number = std::to_string (plans.size() + 1);
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
