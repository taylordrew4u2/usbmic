#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "MainScreen.h"
#include "AdvancedPanel.h"

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
    bool advancedVisible = false;
    int lastMicCount = -1;
    int lastAdvancedMicCount = -1;
    int framesUntilStatusRefresh = 1;

    void toggleAdvanced();
    /// §10.3 panel contents. Refreshed on the slow tick and whenever the panel
    /// is opened, so it is never showing a stale rig.
    void refreshAdvanced();
    std::unique_ptr<juce::FileChooser> folderChooser;
    void promptRenameMic (int index);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace mma
