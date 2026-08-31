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

namespace juce {

class CameraDevice
{
public:
    virtual ~CameraDevice();

    static StringArray getAvailableDevices();

    static CameraDevice* openDevice (int deviceIndex,
                                     int minWidth = 128, int minHeight = 64,
                                     int maxWidth = 1024, int maxHeight = 768,
                                     bool highQuality = true);

    Component* createViewerComponent();

    void startRecordingToFile (const File& file, int quality = 2);
    void stopRecording();

    static String getFileExtension();
};

} // namespace juce
