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
        application.setMasterVolume (volume0to100);
    };

    mainScreen.onMuteToggled = [this] {
        if (auto* bus = application.getMonitorBus())
            bus->setGlobalMute (! bus->isGloballyMuted());
    };

    mainScreen.onAdvancedClicked = [this] { toggleAdvanced(); };

    setSize (720, 480);

    // §8.1: meters and status are live from launch, not from record.
    refreshStatus();
    startTimerHz (kUiRefreshHz);
}

MainComponent::~MainComponent()
{
    stopTimer();
}

void MainComponent::timerCallback()
{
    refreshStatus();
}

void MainComponent::refreshStatus()
{
    const int micCount = application.getIncludedMicCount();

    if (micCount != lastMicCount)
    {
        // §6.5: mics come and go without a dialog and without interrupting a take.
        mainScreen.setMicCount (micCount);
        mainScreen.setMixMetering (application.getMixMetering());

        for (int i = 0; i < micCount; ++i)
        {
            if (auto* skull = mainScreen.getSkullMeter (i))
            {
                skull->setMetering (application.getChannelMetering (i));
                skull->setMicName (application.getMicDisplayName (i));
            }
        }

        lastMicCount = micCount;
    }

    // The meters tick their own ballistics as they paint, so a repaint is the poll.
    mainScreen.repaintMeters();

    const bool isRecording = application.getRecordingEngine().getState() == RecordingState::Recording;
    mainScreen.setRecording (isRecording);

    mainScreen.setElapsedTimeText (isRecording
        ? "Recording for " + Application::formatDuration (application.getElapsedRecordingSeconds())
        : juce::String());

    // Remaining time reads free space off the destination volume, so it runs at
    // kStatusRefreshHz rather than every frame -- 60 stat() calls a second on a
    // slow card is exactly the contention §14.3 warns about.
    if (--framesUntilStatusRefresh <= 0)
    {
        framesUntilStatusRefresh = kUiRefreshHz / kStatusRefreshHz;

        mainScreen.setRemainingTimeText ("Room for "
            + Application::formatDuration (application.getRemainingRecordingSeconds()));

        mainScreen.setSaveLocationText ("Saves to " + application.getDestinationFolder());

        const auto reason = application.getRecordDisabledReason();
        mainScreen.setRecordButtonEnabled (reason.isEmpty(), reason);
    }
}

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
