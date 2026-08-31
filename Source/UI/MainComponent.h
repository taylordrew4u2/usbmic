#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "MainScreen.h"
#include "AdvancedPanel.h"
#include "CameraPanel.h"
#include "SaveLocationPrompt.h"
#include "SavedTakePanel.h"
#include "RecoveredTakesPanel.h"
#include <functional>

namespace mma {

class Application; // App/Application.h

/// Top-level component: hosts MainScreen always, and slides in AdvancedPanel
/// on demand (§10.3: "one door, one click").
class MainComponent : public juce::Component,
                      private juce::Timer
{
public:
    static constexpr int kUiRefreshHz = 60;     // §8.2 meter polling
    static constexpr int kStatusRefreshHz = 2;  // disk-backed status; see refreshStatus()

    explicit MainComponent (Application& app);
    ~MainComponent() override;

    void resized() override;

private:
    /// §8.2: the UI polls, the audio thread never pushes. Dropping a frame here
    /// is acceptable; it can never stall the audio callback.
    void timerCallback() override;
    bool keyPressed (const juce::KeyPress& key) override;
    void refreshStatus();
    /// Rebinds every Metering pointer the meters hold. Called from
    /// Application::onCaptureRebuilt in the same call stack that destroyed the
    /// old ones, so no timer can dereference a freed Metering in between.
    void rebindMeters();
    Application& application;
    MainScreen mainScreen;
    AdvancedPanel advancedPanel;

    // Both screens are taller than a small window -- Settings especially, once
    // a few microphones are listed. Without these the overflow was simply
    // clipped, with no scrollbar and no indication anything was missing.
    int lastAdvancedHeight = 0;

    juce::Viewport mainViewport;
    juce::Viewport advancedViewport;
    juce::Viewport cameraViewport;
    bool advancedVisible = false;
    bool cameraVisible = false;
    int lastCameraHeight = 0;
    int lastMicCount = -1;
    int lastAdvancedMicCount = -1;
    int framesUntilStatusRefresh = 1;

    // What the live "files are appearing" line last reported, so it is only
    // rebuilt when the folder on disk has actually changed underneath it.
    juce::String lastSavingLine;

    /// §10.1/§6.2: the record press, with the "where does this go" question in
    /// front of it the first time it is asked for a given destination.
    void beginRecording();
    /// Actually starts the take. Everything the prompt is for has already
    /// happened by the time this runs.
    void startRecordingNow();
    void showSaveLocationPrompt();
    void dismissSaveLocationPrompt();
    /// §6.2: called when a take has finished, with what landed on disk.
    void showSavedTake();
    void chooseDestinationFolder (std::function<void()> onChosen);

    /// §6.6: shown once at launch when the last run was interrupted.
    void showRecoveredTakes();

    SaveLocationPrompt saveLocationPrompt;
    SavedTakePanel savedTakePanel;
    RecoveredTakesPanel recoveredTakesPanel;
    // The folder the panel is currently showing, so "Open the folder" opens the
    // one on screen rather than whatever the app has moved on to since.
    juce::String savedTakeFolder;

    void toggleAdvanced();
    void toggleCameras();
    /// The camera list and its live views. Refreshed when the panel is open,
    /// and rebuilt only when the set of cameras or their state has moved.
    void refreshCameras();
    CameraPanel cameraPanel;

    /// §10.3 panel contents. Refreshed on the slow tick and whenever the panel
    /// is opened, so it is never showing a stale rig.
    void refreshAdvanced();
    std::unique_ptr<juce::FileChooser> folderChooser;
    void promptRenameMic (int index);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace mma
