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

    explicit CameraDevice (String deviceName = {}, String deviceIdentifier = {});
    virtual ~CameraDevice();

    static StringArray getAvailableDevices();
    /// SobStage's patch: parallel to getAvailableDevices(); macOS gives each
    /// AVCaptureDevice's uniqueID, other platforms an empty string per device.
    static StringArray getAvailableDeviceIds();
    static void getAvailableDevicesWithIds (StringArray& names, StringArray& identifiers);

    static CameraDevice* openDevice (int deviceIndex,
                                     int minWidth = 128, int minHeight = 64,
                                     int maxWidth = 1024, int maxHeight = 768,
                                     bool highQuality = true);

    const String& getName() const noexcept { return name; }
    String getDeviceIdentifier() const { return identifier; }

    std::function<void (const String&)> onErrorOccurred;
    /// The second argument is the backend's estimate of the movie's first
    /// frame on Time::getMillisecondCounterHiRes(), or 0 when it has none.
    std::function<void (const File&, double)> onRecordingStarted;
    std::function<void (const File&, const String&)> onRecordingFinished;
    std::function<void (int, int, double)> onFormatChanged;

    Component* createViewerComponent();

    void addListener (Listener* listenerToAdd);
    void removeListener (Listener* listenerToRemove);

    void startRecordingToFile (const File& file, int quality = 2);
    void stopRecording();

    static String getFileExtension();

private:
    String name;
    String identifier;
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

/// What CameraDevice::getAvailableDevices() will report from now on. The
/// devices have no stable identifiers, as on Windows.
void setDevices (const juce::StringArray& names);
/// The same, with a stable identifier per device (macOS's uniqueID),
/// index for index. An empty identifier means that device has none.
void setDevices (const juce::StringArray& names, const juce::StringArray& identifiers);
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
/// A grey picture, or (black = true) the all-black frames a capture dongle
/// keeps sending with no HDMI signal or an HDCP-protected source.
void emitFrame (const juce::String& deviceName, bool black = false);
int getAddListenerCallCount();
int getRemoveListenerCallCount();
void resetListenerCallCounts();
void resetOpenCallCount();
/// The maxWidth/maxHeight passed to the most recent openDevice().
int getLastOpenMaxHeight();
/// Reports the format the named camera "settled on", as the macOS backend does
/// once its session starts.
void emitFormat (const juce::String& deviceName, int width, int height, double fps);
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
/// Confirms every delayed start. `firstFrameMs` is what the backend reports
/// as the movie's first frame (0: no estimate, as on Windows).
void completePendingRecordingStarts (double firstFrameMs = 0.0);
void setFinalizationMode (FinalizationMode mode);
int getPendingFinalizationCount();
void completePendingFinalizations();
/// From now on the start/finish callbacks name the movie inside `folder`
/// rather than where it was started -- a backend reporting where the file is
/// after its take folder was renamed or moved mid-take. A default-constructed
/// File restores the path the recording was started with.
/// resetRecordingCallCounts() also restores it.
void setReportedRecordingFolder (const juce::File& folder);
std::vector<int> getOpenedDeviceIndices();

} // namespace fakecamera
