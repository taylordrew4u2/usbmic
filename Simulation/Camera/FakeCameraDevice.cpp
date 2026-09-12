#include "juce_video/juce_video.h"
#include <thread>

// The stub's bodies.
//
// Compiling CameraController against these guarantees that every call it makes
// exists, takes what it is given, and returns what it is used as. The device
// LIST is also settable, which lets the enumeration path -- a camera arriving,
// a camera going away, two of the same model staying apart -- be driven for
// real on a machine with no camera at all. Open success is configurable so the
// controller's retry policy is covered; viewers remain absent because they
// require a GUI message loop and are outside this simulator's scope.
namespace fakecamera {

juce::StringArray& deviceList()
{
    static juce::StringArray devices;
    return devices;
}

void setDevices (const juce::StringArray& names) { deviceList() = names; }

bool& openSucceeds()
{
    static bool succeeds = false;
    return succeeds;
}

int& openCallCount()
{
    static int count = 0;
    return count;
}

int& liveDeviceCount()
{
    static int count = 0;
    return count;
}

int& startRecordingCallCount()
{
    static int count = 0;
    return count;
}

int& stopRecordingCallCount()
{
    static int count = 0;
    return count;
}

int& activeRecordingCount()
{
    static int count = 0;
    return count;
}

juce::String& lastOpenedDeviceName()
{
    static juce::String name;
    return name;
}

void setOpenSucceeds (bool shouldSucceed) { openSucceeds() = shouldSucceed; }
void resetOpenCallCount()
{
    openCallCount() = 0;
    lastOpenedDeviceName().clear();
}
int getOpenCallCount() { return openCallCount(); }
int getLiveDeviceCount() { return liveDeviceCount(); }
juce::String getLastOpenedDeviceName() { return lastOpenedDeviceName(); }
void resetRecordingCallCounts()
{
    startRecordingCallCount() = 0;
    stopRecordingCallCount() = 0;
    activeRecordingCount() = 0;
}
int getStartRecordingCallCount() { return startRecordingCallCount(); }
int getStopRecordingCallCount() { return stopRecordingCallCount(); }
int getActiveRecordingCount() { return activeRecordingCount(); }

std::thread::id& lastEnumerationThread()
{
    static std::thread::id thread;
    return thread;
}

bool wasLastEnumerationOnThisThread()
{
    return lastEnumerationThread() == std::this_thread::get_id();
}

} // namespace fakecamera

namespace juce {

CameraDevice::CameraDevice (String deviceName)
    : name (std::move (deviceName))
{
    ++fakecamera::liveDeviceCount();
}

CameraDevice::~CameraDevice()
{
    stopRecording();
    --fakecamera::liveDeviceCount();
}

StringArray CameraDevice::getAvailableDevices()
{
    fakecamera::lastEnumerationThread() = std::this_thread::get_id();
    return fakecamera::deviceList();
}

CameraDevice* CameraDevice::openDevice (int index, int, int, int, int, bool)
{
    ++fakecamera::openCallCount();

    const auto& devices = fakecamera::deviceList();
    if (! fakecamera::openSucceeds() || ! juce::isPositiveAndBelow (index, devices.size()))
        return nullptr;

    fakecamera::lastOpenedDeviceName() = devices[index];
    return new CameraDevice (devices[index]);
}

Component* CameraDevice::createViewerComponent() { return nullptr; }

void CameraDevice::startRecordingToFile (const File& file, int)
{
    if (recording)
        return;

    recording = true;
    ++fakecamera::startRecordingCallCount();
    ++fakecamera::activeRecordingCount();
    file.replaceWithText ("fake camera recording");
}

void CameraDevice::stopRecording()
{
    if (! recording)
        return;

    recording = false;
    ++fakecamera::stopRecordingCallCount();
    --fakecamera::activeRecordingCount();
}

String CameraDevice::getFileExtension() { return ".mov"; }

} // namespace juce
