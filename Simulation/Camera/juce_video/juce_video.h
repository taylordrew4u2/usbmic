#pragma once
// -----------------------------------------------------------------------
// A stand-in for JUCE's juce_video module, declaring exactly the
// juce::CameraDevice surface CameraController uses.
//
// JUCE implements CameraDevice on macOS and Windows only, so on every other
// machine -- including the Linux one the headless checks run on -- the camera
// path is compiled out and is therefore unverified by construction. That is
// precisely how CoreAudioBackend and WasapiAsioBackend came to be carrying
// five user-facing defects, which is why Simulation/ exists.
//
// Putting this directory ahead of JUCE on the include path lets
// Source/App/CameraController.cpp be compiled UNMODIFIED with JUCE_USE_CAMERA=1
// anywhere. The code under test is the code that ships; only the camera API
// underneath it is a stub. Every signature below is copied from
// modules/juce_video/capture/juce_CameraDevice.h, so a change on either side
// that the controller has not kept up with fails the build.
// -----------------------------------------------------------------------
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>

namespace juce {

class CameraDevice
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void imageReceived (const Image& image) = 0;
    };

    explicit CameraDevice (String deviceName = {});
    virtual ~CameraDevice();

    static StringArray getAvailableDevices();

    static CameraDevice* openDevice (int deviceIndex,
                                     int minWidth = 128, int minHeight = 64,
                                     int maxWidth = 1024, int maxHeight = 768,
                                     bool highQuality = true);

    const String& getName() const noexcept { return name; }

    std::function<void (const String&)> onErrorOccurred;
    std::function<void (const File&)> onRecordingStarted;
    std::function<void (const File&, const String&)> onRecordingFinished;

    Component* createViewerComponent();

    void addListener (Listener* listenerToAdd);
    void removeListener (Listener* listenerToRemove);

    void startRecordingToFile (const File& file, int quality = 2);
    void stopRecording();

    static String getFileExtension();

private:
    String name;
    File recordingFile;
    bool recording = false;
};

} // namespace juce

/// The fake's own controls, outside juce so nothing here shadows a real API.
namespace fakecamera {

enum class FinalizationMode
{
    ImmediateSuccess,
    DelayedSuccess,
    Never,
    ImmediateError
};

/// What CameraDevice::getAvailableDevices() will report from now on.
void setDevices (const juce::StringArray& names);
/// Pauses the next `count` discovery calls after each has captured its device
/// snapshot. These controls make a permanently slow platform enumerator
/// deterministic without changing CameraController's production source.
void pauseNextEnumerations (int count);
bool waitForPausedEnumerationCount (int count, int timeoutMilliseconds);
void releaseOnePausedEnumeration();
bool waitForNoPausedEnumerations (int timeoutMilliseconds);
void setOpenSucceeds (bool shouldSucceed);
void setViewerSucceeds (bool shouldSucceed);
/// Whether addListener immediately delivers one valid image. Enabled by
/// default so existing healthy-camera scenarios model an active source.
void setAutoFrameOnListener (bool shouldDeliver);
/// Delivers a valid image to the listeners on every current generation with
/// this OS name.
void emitFrame (const juce::String& deviceName);
int getAddListenerCallCount();
int getRemoveListenerCallCount();
void resetListenerCallCounts();
void resetOpenCallCount();
int getOpenCallCount();
void resetViewerCreateCallCount();
int getViewerCreateCallCount();
int getLiveDeviceCount();
juce::String getLastOpenedDeviceName();
void emitRuntimeError (const juce::String& deviceName, const juce::String& message);
bool wasLastEnumerationOnThisThread();
void resetRecordingCallCounts();
int getStartRecordingCallCount();
int getStopRecordingCallCount();
int getActiveRecordingCount();
void setAutoConfirmRecordingStart (bool shouldConfirm);
void setStartRecordingSucceeds (bool shouldSucceed);
int getPendingRecordingStartCount();
void completePendingRecordingStarts();
void setFinalizationMode (FinalizationMode mode);
int getPendingFinalizationCount();
void completePendingFinalizations();
std::vector<int> getOpenedDeviceIndices();

} // namespace fakecamera
