#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mma {

/// One camera the OS is offering us, whatever it is plugged into. §2 treats
/// every microphone the same whether it is USB, built in or on a capture card,
/// and cameras get the same deal: the app takes what the OS lists and does not
/// second-guess where it came from.
struct CameraDeviceInfo
{
    std::string id;          // stable for as long as the camera is connected
    std::string displayName; // "Logitech C920"
};

/// The live view and the recorded file answer two different questions.
///
/// The file requests the platform's high-quality capture mode; the OS/driver
/// ultimately chooses its format, and nothing on screen may lower that request.
/// The view only has to be good enough to aim by, and drawing a 4K frame sixty
/// times a second to check someone is in shot is exactly the CPU §6.6 warns
/// about spending before it starts dropping audio. So this picks how big the
/// picture is *drawn*, never how big it is captured or written.
enum class PreviewQuality
{
    Low,  // small, cheap, always on
    Full  // as large as the panel allows, for a quick check of focus and framing
};

struct PreviewSettings
{
    int maxViewHeight = 0; // the tallest the live picture is drawn
};

/// One camera's part in a take: §6.2 naming, applied to pictures.
struct CameraPlan
{
    std::string deviceId;
    std::string displayName;
    /// "V01_Kitchen-Cam" -- no extension, since the container is the platform's
    /// choice. The "V" keeps video out of the middle of the audio stems when a
    /// file browser sorts the folder by name, and the number keeps two cameras
    /// with the same product string apart.
    std::string fileName;
};

/// Which cameras are in a take, what they are called, and what they will write.
///
/// Kept apart from the platform camera API for the same reason DeviceManager is
/// kept apart from the audio backends: this is all the behaviour worth being
/// sure about, and none of it needs a camera to be plugged into the machine
/// running the tests.
class CameraSelection
{
public:
    /// A rough figure for how much disk a high-quality camera stream can eat, used
    /// only to keep the §6.4 remaining-time estimate honest once video is in
    /// the take. Deliberately pessimistic: telling someone they have less room
    /// than they do costs them nothing, and the reverse costs them the end of
    /// their recording.
    static constexpr int64_t kEstimatedVideoBytesPerSecond = 4 * 1000 * 1000; // ~32 Mbit/s

    /// Replaces the list of connected cameras, keeping every choice already
    /// made about a camera that is still there. A camera that comes back after
    /// being unplugged comes back switched on if that is how it was left.
    void setAvailableCameras (std::vector<CameraDeviceInfo> cameras);
    const std::vector<CameraDeviceInfo>& getAvailableCameras() const { return available; }

    /// Remembered, enabled cameras which the OS is not currently listing.
    /// These remain visible in the Cameras panel so an unplugged capture card
    /// cannot silently disappear from the rig the user thinks is armed.
    std::vector<CameraDeviceInfo> getUnavailableEnabledCameras() const;

    /// Every camera with remembered state, connected or not. Settings use
    /// this instead of copying an old persisted row back verbatim, so turning
    /// off a currently disconnected capture card is saved correctly.
    std::vector<CameraDeviceInfo> getKnownCameras() const;

    /// Cameras are opt-in. Enumeration alone must not open a camera, illuminate
    /// its privacy light, raise a permission prompt, or add gigabytes to a take.
    void setEnabled (const std::string& id, bool enabled);
    bool isEnabled (const std::string& id) const;
    /// Enabled cameras in the latest completed OS snapshot. Remembered missing
    /// cameras stay armed and visible through getUnavailableEnabledCameras(),
    /// but are not counted as files the next take can actually create.
    int getEnabledCount() const;

    /// The name that goes on the picture's file, like §14.6 for microphones.
    void setAssignedName (const std::string& id, const std::string& name);
    std::string getDisplayName (const std::string& id) const;

    /// Enabled cameras the OS is currently listing, numbered and named. This
    /// is the honest pre-take file promise shown in the save-location card.
    std::vector<CameraPlan> buildPlans() const;

    /// The complete armed roster at the instant a take starts. Available
    /// cameras come first in OS order; remembered missing cameras follow. The
    /// controller freezes this broader list for the watchdog, but it must not
    /// be presented as a promise that every listed file will exist.
    std::vector<CameraPlan> buildIntendedPlans() const;

    /// What a take with this many cameras adds to the bytes-per-second the
    /// §6.4 remaining-time figure is worked out from.
    int64_t getEstimatedBytesPerSecond() const;

    static PreviewSettings previewSettingsFor (PreviewQuality quality);

private:
    struct Choice
    {
        bool enabled = false;
        std::string assignedName;
        std::string lastDisplayName;
    };

    std::vector<CameraDeviceInfo> available;
    std::map<std::string, Choice> choices; // by id, so unplugging forgets nothing
};

} // namespace mma
