#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "MainScreen.h"
#include "AdvancedPanel.h"

namespace mma {

class Application; // App/Application.h

/// Top-level component: hosts MainScreen always, and slides in AdvancedPanel
/// on demand (§10.3: "one door, one click").
class MainComponent : public juce::Component
{
public:
    explicit MainComponent (Application& app);
    ~MainComponent() override;

    void resized() override;

private:
    Application& application;
    MainScreen mainScreen;
    AdvancedPanel advancedPanel;
    bool advancedVisible = false;

    void toggleAdvanced();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace mma
