#include "juce_video/juce_video.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

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

struct EnumerationControl
{
    std::mutex mutex;
    std::condition_variable condition;
    juce::StringArray devices;
    std::thread::id lastThread;
    int pausesRemaining = 0;
    int releasePermits = 0;
    int pausedStarted = 0;
    int pausedActive = 0;
};

EnumerationControl& enumerationControl()
{
    static EnumerationControl control;
    return control;
}

void setDevices (const juce::StringArray& names)
{
    auto& control = enumerationControl();
    const std::lock_guard<std::mutex> guard (control.mutex);
    control.devices = names;
}

void pauseNextEnumerations (int count)
{
    auto& control = enumerationControl();
    const std::lock_guard<std::mutex> guard (control.mutex);
    jassert (control.pausedActive == 0);
    control.pausesRemaining = juce::jmax (0, count);
    control.releasePermits = 0;
    control.pausedStarted = 0;
}

bool waitForPausedEnumerationCount (int count, int timeoutMilliseconds)
{
    auto& control = enumerationControl();
    std::unique_lock<std::mutex> lock (control.mutex);
    return control.condition.wait_for (
        lock, std::chrono::milliseconds (juce::jmax (0, timeoutMilliseconds)),
        [&control, count] { return control.pausedStarted >= count; });
}

void releaseOnePausedEnumeration()
{
    auto& control = enumerationControl();
    {
        const std::lock_guard<std::mutex> guard (control.mutex);
        ++control.releasePermits;
    }
    control.condition.notify_all();
}

bool waitForNoPausedEnumerations (int timeoutMilliseconds)
{
    auto& control = enumerationControl();
    std::unique_lock<std::mutex> lock (control.mutex);
    return control.condition.wait_for (
        lock, std::chrono::milliseconds (juce::jmax (0, timeoutMilliseconds)),
        [&control] { return control.pausedActive == 0; });
}

bool& openSucceeds()
{
    static bool succeeds = false;
    return succeeds;
}

bool& viewerSucceeds()
{
    static bool succeeds = true;
    return succeeds;
}

bool& autoFrameOnListener()
{
    static bool shouldDeliver = true;
    return shouldDeliver;
}

int& addListenerCallCount()
{
    static int count = 0;
    return count;
}

int& removeListenerCallCount()
{
    static int count = 0;
    return count;
}

std::map<juce::CameraDevice*, std::vector<juce::CameraDevice::Listener*>>& listenersByDevice()
{
    static std::map<juce::CameraDevice*, std::vector<juce::CameraDevice::Listener*>> listeners;
    return listeners;
}

int& openCallCount()
{
    static int count = 0;
    return count;
}

int& viewerCreateCallCount()
{
    static int count = 0;
    return count;
}

std::vector<juce::CameraDevice*>& liveDevices()
{
    static std::vector<juce::CameraDevice*> devices;
    return devices;
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

bool& autoConfirmRecordingStart()
{
    static bool shouldConfirm = true;
    return shouldConfirm;
}

bool& startRecordingSucceeds()
{
    static bool succeeds = true;
    return succeeds;
}

struct PendingRecordingStart
{
    juce::CameraDevice* device = nullptr;
    juce::File file;
};

std::vector<PendingRecordingStart>& pendingRecordingStarts()
{
    static std::vector<PendingRecordingStart> pending;
    return pending;
}

FinalizationMode& finalizationMode()
{
    static FinalizationMode mode = FinalizationMode::ImmediateSuccess;
    return mode;
}

struct PendingFinalization
{
    juce::CameraDevice* device = nullptr;
    juce::File file;
    bool deliverable = true;
};

std::vector<PendingFinalization>& pendingFinalizations()
{
    static std::vector<PendingFinalization> pending;
    return pending;
}

std::vector<int>& openedDeviceIndices()
{
    static std::vector<int> indices;
    return indices;
}

juce::String& lastOpenedDeviceName()
{
    static juce::String name;
    return name;
}

void setOpenSucceeds (bool shouldSucceed) { openSucceeds() = shouldSucceed; }
void setViewerSucceeds (bool shouldSucceed) { viewerSucceeds() = shouldSucceed; }
void setAutoFrameOnListener (bool shouldDeliver) { autoFrameOnListener() = shouldDeliver; }
int getAddListenerCallCount() { return addListenerCallCount(); }
int getRemoveListenerCallCount() { return removeListenerCallCount(); }
void resetListenerCallCounts()
{
    addListenerCallCount() = 0;
    removeListenerCallCount() = 0;
}
void resetOpenCallCount()
{
    openCallCount() = 0;
    lastOpenedDeviceName().clear();
    openedDeviceIndices().clear();
}
int getOpenCallCount() { return openCallCount(); }
void resetViewerCreateCallCount() { viewerCreateCallCount() = 0; }
int getViewerCreateCallCount() { return viewerCreateCallCount(); }
int getLiveDeviceCount() { return liveDeviceCount(); }
juce::String getLastOpenedDeviceName() { return lastOpenedDeviceName(); }
void resetRecordingCallCounts()
{
    startRecordingCallCount() = 0;
    stopRecordingCallCount() = 0;
    activeRecordingCount() = 0;
    autoConfirmRecordingStart() = true;
    startRecordingSucceeds() = true;
    pendingRecordingStarts().clear();
    pendingFinalizations().clear();
    finalizationMode() = FinalizationMode::ImmediateSuccess;
}
int getStartRecordingCallCount() { return startRecordingCallCount(); }
int getStopRecordingCallCount() { return stopRecordingCallCount(); }
int getActiveRecordingCount() { return activeRecordingCount(); }
void setAutoConfirmRecordingStart (bool shouldConfirm)
{
    autoConfirmRecordingStart() = shouldConfirm;
}
void setStartRecordingSucceeds (bool shouldSucceed)
{
    startRecordingSucceeds() = shouldSucceed;
}
int getPendingRecordingStartCount()
{
    return static_cast<int> (pendingRecordingStarts().size());
}
void completePendingRecordingStarts()
{
    auto pending = std::move (pendingRecordingStarts());
    pendingRecordingStarts().clear();

    for (const auto& item : pending)
        if (item.device != nullptr && item.device->onRecordingStarted)
            item.device->onRecordingStarted (item.file);
}
void setFinalizationMode (FinalizationMode mode) { finalizationMode() = mode; }
int getPendingFinalizationCount() { return static_cast<int> (pendingFinalizations().size()); }
std::vector<int> getOpenedDeviceIndices() { return openedDeviceIndices(); }

void completePendingFinalizations()
{
    auto pending = std::move (pendingFinalizations());
    pendingFinalizations().clear();

    for (const auto& item : pending)
        if (item.deliverable && item.device != nullptr && item.device->onRecordingFinished)
            item.device->onRecordingFinished (item.file, {});
        else if (! item.deliverable)
            pendingFinalizations().push_back (item);
}

void emitRuntimeError (const juce::String& deviceName, const juce::String& message)
{
    // Copy because the callback may cause the controller to close the device
    // when it next drains its mailbox.
    const auto devices = liveDevices();
    for (auto* device : devices)
        if (device != nullptr && device->getName() == deviceName && device->onErrorOccurred)
            device->onErrorOccurred (message);
}

void emitFrame (const juce::String& deviceName)
{
    const juce::Image image (juce::Image::RGB, 4, 4, true);

    // Copy both collections because a controller may consume the resulting
    // mailbox and remove a listener immediately afterwards.
    const auto devices = liveDevices();
    for (auto* device : devices)
    {
        if (device == nullptr || device->getName() != deviceName)
            continue;

        const auto found = listenersByDevice().find (device);
        if (found == listenersByDevice().end())
            continue;

        const auto listeners = found->second;
        for (auto* listener : listeners)
            if (listener != nullptr)
                listener->imageReceived (image);
    }
}

bool wasLastEnumerationOnThisThread()
{
    auto& control = enumerationControl();
    const std::lock_guard<std::mutex> guard (control.mutex);
    return control.lastThread == std::this_thread::get_id();
}

} // namespace fakecamera

namespace juce {

CameraDevice::CameraDevice (String deviceName)
    : name (std::move (deviceName))
{
    ++fakecamera::liveDeviceCount();
    fakecamera::liveDevices().push_back (this);
}

CameraDevice::~CameraDevice()
{
    stopRecording();
    auto& pendingStarts = fakecamera::pendingRecordingStarts();
    pendingStarts.erase (std::remove_if (pendingStarts.begin(), pendingStarts.end(),
                                        [this] (const auto& item) { return item.device == this; }),
                         pendingStarts.end());
    auto& pending = fakecamera::pendingFinalizations();
    pending.erase (std::remove_if (pending.begin(), pending.end(),
                                  [this] (const auto& item) { return item.device == this; }),
                   pending.end());
    fakecamera::listenersByDevice().erase (this);
    auto& devices = fakecamera::liveDevices();
    devices.erase (std::remove (devices.begin(), devices.end(), this), devices.end());
    --fakecamera::liveDeviceCount();
}

StringArray CameraDevice::getAvailableDevices()
{
    auto& control = fakecamera::enumerationControl();
    std::unique_lock<std::mutex> lock (control.mutex);
    control.lastThread = std::this_thread::get_id();
    auto devices = control.devices;

    if (control.pausesRemaining > 0)
    {
        --control.pausesRemaining;
        ++control.pausedStarted;
        ++control.pausedActive;
        control.condition.notify_all();
        control.condition.wait (lock, [&control] { return control.releasePermits > 0; });
        --control.releasePermits;
        --control.pausedActive;
        control.condition.notify_all();
    }

    return devices;
}

CameraDevice* CameraDevice::openDevice (int index, int, int, int, int, bool)
{
    ++fakecamera::openCallCount();
    fakecamera::openedDeviceIndices().push_back (index);

    juce::String selectedDevice;
    {
        auto& control = fakecamera::enumerationControl();
        const std::lock_guard<std::mutex> guard (control.mutex);
        if (! juce::isPositiveAndBelow (index, control.devices.size()))
            return nullptr;
        selectedDevice = control.devices[index];
    }

    if (! fakecamera::openSucceeds())
        return nullptr;

    fakecamera::lastOpenedDeviceName() = selectedDevice;
    return new CameraDevice (selectedDevice);
}

Component* CameraDevice::createViewerComponent()
{
    ++fakecamera::viewerCreateCallCount();
    return fakecamera::viewerSucceeds() ? new Component() : nullptr;
}

void CameraDevice::addListener (Listener* listenerToAdd)
{
    ++fakecamera::addListenerCallCount();
    if (listenerToAdd == nullptr)
        return;

    auto& listeners = fakecamera::listenersByDevice()[this];
    if (std::find (listeners.begin(), listeners.end(), listenerToAdd) == listeners.end())
        listeners.push_back (listenerToAdd);

    if (fakecamera::autoFrameOnListener())
        listenerToAdd->imageReceived (Image (Image::RGB, 4, 4, true));
}

void CameraDevice::removeListener (Listener* listenerToRemove)
{
    ++fakecamera::removeListenerCallCount();
    const auto found = fakecamera::listenersByDevice().find (this);
    if (found == fakecamera::listenersByDevice().end())
        return;

    auto& listeners = found->second;
    listeners.erase (std::remove (listeners.begin(), listeners.end(), listenerToRemove),
                     listeners.end());
}

void CameraDevice::startRecordingToFile (const File& file, int)
{
    if (recording)
        return;

    recordingFile = file;
    ++fakecamera::startRecordingCallCount();

    if (! fakecamera::startRecordingSucceeds())
    {
        if (onRecordingFinished)
            onRecordingFinished (file, "simulated camera writer start failure");
        return;
    }

    recording = true;
    ++fakecamera::activeRecordingCount();
    file.replaceWithText ("fake camera recording");

    if (fakecamera::autoConfirmRecordingStart() && onRecordingStarted)
        onRecordingStarted (file);
    else if (! fakecamera::autoConfirmRecordingStart())
        fakecamera::pendingRecordingStarts().push_back ({ this, file });
}

void CameraDevice::stopRecording()
{
    if (! recording)
        return;

    recording = false;
    ++fakecamera::stopRecordingCallCount();
    --fakecamera::activeRecordingCount();

    auto& pendingStarts = fakecamera::pendingRecordingStarts();
    pendingStarts.erase (std::remove_if (pendingStarts.begin(), pendingStarts.end(),
                                        [this] (const auto& item) { return item.device == this; }),
                         pendingStarts.end());

    switch (fakecamera::finalizationMode())
    {
        case fakecamera::FinalizationMode::ImmediateSuccess:
            if (onRecordingFinished)
                onRecordingFinished (recordingFile, {});
            break;

        case fakecamera::FinalizationMode::DelayedSuccess:
            fakecamera::pendingFinalizations().push_back ({ this, recordingFile, true });
            break;

        case fakecamera::FinalizationMode::Never:
            fakecamera::pendingFinalizations().push_back ({ this, recordingFile, false });
            break;

        case fakecamera::FinalizationMode::ImmediateError:
            if (onRecordingFinished)
                onRecordingFinished (recordingFile, "simulated finalization failure");
            break;
    }
}

String CameraDevice::getFileExtension() { return ".mov"; }

} // namespace juce
