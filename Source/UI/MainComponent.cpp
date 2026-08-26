#include "MainComponent.h"
#include "../App/Application.h"

namespace mma {

MainComponent::MainComponent (Application& app)
    : application (app)
{
    addAndMakeVisible (mainScreen);
    advancedPanel.setVisible (false);
    addChildComponent (advancedPanel);

    mainScreen.onRecordButtonClicked = [this] {
        application.toggleRecording();
        mainScreen.setRecording (application.getRecordingEngine().getState() == RecordingState::Recording);
    };

    mainScreen.onVolumeChanged = [this] (double volume0to100) {
        // Master monitor volume, mapped logarithmically per §5.1; MonitorBus
        // owns the actual mapping, this just forwards the UI value.
        (void) volume0to100;
    };

    mainScreen.onMuteToggled = [this] {
        if (auto* bus = application.getMonitorBus())
            bus->setGlobalMute (! bus->isGloballyMuted());
    };

    mainScreen.onAdvancedClicked = [this] { toggleAdvanced(); };

    setSize (720, 480);
}

MainComponent::~MainComponent() = default;

void MainComponent::toggleAdvanced()
{
    advancedVisible = ! advancedVisible;
    advancedPanel.setVisible (advancedVisible);
    mainScreen.setVisible (! advancedVisible);
    resized();
}

void MainComponent::resized()
{
    auto bounds = getLocalBounds();
    mainScreen.setBounds (bounds);
    advancedPanel.setBounds (bounds);
}

} // namespace mma
