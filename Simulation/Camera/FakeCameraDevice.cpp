#include "juce_video/juce_video.h"

// The stub's bodies.
//
// Compiling CameraController against these guarantees that every call it makes
// exists, takes what it is given, and returns what it is used as. The device
// LIST is also settable, which lets the enumeration path -- a camera arriving,
// a camera going away, two of the same model staying apart -- be driven for
// real on a machine with no camera at all. Opening a device still returns
// nothing: a viewer component needs a message manager, and that would be
// testing the harness rather than the controller.
namespace fakecamera {

juce::StringArray& deviceList()
{
    static juce::StringArray devices;
    return devices;
}

void setDevices (const juce::StringArray& names) { deviceList() = names; }

} // namespace fakecamera

namespace juce {

CameraDevice::~CameraDevice() = default;

StringArray CameraDevice::getAvailableDevices() { return fakecamera::deviceList(); }

CameraDevice* CameraDevice::openDevice (int, int, int, int, int, bool) { return nullptr; }

Component* CameraDevice::createViewerComponent() { return nullptr; }

void CameraDevice::startRecordingToFile (const File&, int) {}
void CameraDevice::stopRecording() {}

String CameraDevice::getFileExtension() { return ".mov"; }

} // namespace juce
