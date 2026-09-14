#include "juce_video/juce_video.h"

// The stub's bodies.
//
// Compiling CameraController against these guarantees that every call it makes
// exists, takes what it is given, and returns what it is used as. The device
// LIST is also settable, which lets the enumeration path -- a camera arriving,
// a camera going away, two of the same model staying apart -- be driven for
// real on a machine with no camera at all. Opening a device returns a live
// object and counts it, so what the controller does with a camera it has ALREADY
// opened -- the case a capture card on an HDMI input lands in every time its
// source blinks -- can be driven too. Only createViewerComponent() still
// answers nothing: a viewer needs a message manager, and that would be testing
// the harness rather than the controller.
namespace fakecamera {

juce::StringArray& deviceList()
{
    static juce::StringArray devices;
    return devices;
}

void setDevices (const juce::StringArray& names) { deviceList() = names; }

bool& openingFails()
{
    static bool fails = false;
    return fails;
}

int& opens()
{
    static int count = 0;
    return count;
}

int& live()
{
    static int count = 0;
    return count;
}

void setOpeningFails (bool fails) { openingFails() = fails; }

int openCalls() { return opens(); }
int liveDevices() { return live(); }

void resetCounters()
{
    opens() = 0;
    live() = 0;
}

} // namespace fakecamera

namespace juce {

CameraDevice::CameraDevice() { ++fakecamera::live(); }
CameraDevice::~CameraDevice() { --fakecamera::live(); }

StringArray CameraDevice::getAvailableDevices() { return fakecamera::deviceList(); }

CameraDevice* CameraDevice::openDevice (int, int, int, int, int, bool)
{
    if (fakecamera::openingFails())
        return nullptr;

    ++fakecamera::opens();
    return new CameraDevice();
}

Component* CameraDevice::createViewerComponent() { return nullptr; }

void CameraDevice::startRecordingToFile (const File&, int) {}
void CameraDevice::stopRecording() {}

String CameraDevice::getFileExtension() { return ".mov"; }

} // namespace juce
