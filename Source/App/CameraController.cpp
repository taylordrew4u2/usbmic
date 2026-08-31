#include "CameraController.h"

namespace mma {

CameraController::CameraController() = default;
CameraController::~CameraController()
{
    stopRecording();

#if JUCE_USE_CAMERA
    open.clear();
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
    std::vector<CameraDeviceInfo> cameras;
    osIndexById.clear();

#if JUCE_USE_CAMERA
    const auto names = juce::CameraDevice::getAvailableDevices();

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

        osIndexById[id] = i;
        cameras.push_back ({ id, name.toStdString() });
    }
#endif

    selection.setAvailableCameras (std::move (cameras));
}

void CameraController::applySelection()
{
#if JUCE_USE_CAMERA
    // Close first, so a machine that can only hold one camera open at a time
    // has the old one released before the new one is asked for.
    std::vector<std::string> toClose;

    for (const auto& entry : open)
        if (! selection.isEnabled (entry.first))
            toClose.push_back (entry.first);

    for (const auto& id : toClose)
        closeCamera (id);

    for (const auto& camera : selection.getAvailableCameras())
    {
        if (! selection.isEnabled (camera.id) || open.count (camera.id) > 0)
            continue;

        const auto index = osIndexById.find (camera.id);

        if (index != osIndexById.end())
            openCamera (camera.id, index->second);
    }
#endif
}

#if JUCE_USE_CAMERA
void CameraController::openCamera (const std::string& id, int osIndex)
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
        problem = "Couldn't open " + juce::String (selection.getDisplayName (id))
                + ". Close any other app using it, and check this app is allowed "
                  "to use the camera in your system privacy settings.";
        return;
    }

    OpenCamera entry;
    entry.device = std::move (device);
    entry.osIndex = osIndex;
    open[id] = std::move (entry);

    problem.clear();
}

void CameraController::closeCamera (const std::string& id)
{
    const auto entry = open.find (id);

    if (entry == open.end())
        return;

    if (recording && entry->second.device != nullptr)
        entry->second.device->stopRecording();

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

bool CameraController::startRecording (const juce::File& sessionFolder)
{
#if JUCE_USE_CAMERA
    if (recording)
        return true;

    const auto extension = juce::CameraDevice::getFileExtension();
    int started = 0;
    int wanted = 0;

    for (const auto& plan : selection.buildPlans())
    {
        ++wanted;
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
        ++started;
    }

    recording = started > 0;

    if (wanted > 0 && started < wanted)
        problem = juce::String (wanted - started) + " of your cameras couldn't start recording. "
                  "The sound is recording either way.";

    return recording;
#else
    juce::ignoreUnused (sessionFolder);
    return false;
#endif
}

void CameraController::stopRecording()
{
#if JUCE_USE_CAMERA
    if (! recording)
        return;

    for (auto& entry : open)
        if (entry.second.device != nullptr)
            entry.second.device->stopRecording();
#endif

    recording = false;
}

} // namespace mma
