#include "juce_video/juce_video.h"

// The stub's bodies. Nothing here is exercised at runtime -- the simulation
// target is compile-only, because standing up JUCE's message manager and a
// Component on a headless box tests the harness rather than the controller.
// What it does guarantee is that every call CameraController makes exists,
// takes what it is given, and returns what it is used as.
namespace juce {

CameraDevice::~CameraDevice() = default;

StringArray CameraDevice::getAvailableDevices() { return {}; }

CameraDevice* CameraDevice::openDevice (int, int, int, int, int, bool) { return nullptr; }

Component* CameraDevice::createViewerComponent() { return nullptr; }

void CameraDevice::startRecordingToFile (const File&, int) {}
void CameraDevice::stopRecording() {}

String CameraDevice::getFileExtension() { return ".mov"; }

} // namespace juce
