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

    // §10.3: everything behind the one door is optional, but none of it is
    // decorative -- each control below reaches the engine it names.
    advancedPanel.onTrimChanged = [this] (int index, float trimDb) {
        application.setChannelTrimDb (index, trimDb);
    };

    advancedPanel.onClockMasterChanged = [this] (const juce::String& name) {
        application.setClockMasterByName (name);
    };

    advancedPanel.onOutputDeviceChanged = [this] (const juce::String& name) {
        application.setOutputDeviceByName (name);
    };

    advancedPanel.onMirrorToggled = [this] (bool enabled) {
        application.setMirrorEnabled (enabled);
    };

    advancedPanel.onDestinationFolderClicked = [this] {
        folderChooser = std::make_unique<juce::FileChooser> ("Choose where recordings are saved",
                                                             juce::File (application.getDestinationFolder()));

        folderChooser->launchAsync (juce::FileBrowserComponent::openMode
                                        | juce::FileBrowserComponent::canSelectDirectories,
                                    [this] (const juce::FileChooser& chooser) {
                                        const auto result = chooser.getResult();

                                        if (result.isDirectory())
                                        {
                                            application.setDestinationFolder (result);
                                            refreshAdvanced();
                                        }
                                    });
    };

    advancedPanel.onDiagnosticsExportClicked = [this] {
        // §11: logs, recent session.json files and the device inventory. Never audio.
        const auto destination = juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
                                     .getNonexistentChildFile ("MultiMicAggregator-diagnostics", ".zip");
        application.exportDiagnostics (destination);
    };

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

        for (int i = 0; i < micCount; ++i)
            if (auto* skull = mainScreen.getSkullMeter (i))
                skull->setMicName (application.getMicDisplayName (i));

        lastMicCount = micCount;
    }

    // Rebound every frame rather than only on a count change: a hot-plug or a
    // sample-rate renegotiation rebuilds the meters the audio thread feeds
    // without the count moving, and a stale pointer here would outlive them.
    mainScreen.setMixMetering (application.getMixMetering());

    for (int i = 0; i < micCount; ++i)
        if (auto* skull = mainScreen.getSkullMeter (i))
            skull->setMetering (application.getChannelMetering (i));

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

        // §5.4: if the low-latency monitor path could not be obtained, say so.
        // §5.3: otherwise, if no headphone output could be chosen, say that.
        auto monitorProblem = application.getMonitorProblem();

        if (monitorProblem.isEmpty())
            monitorProblem = juce::String (application.getOutputSelectionProblem());

        mainScreen.setMonitorProblemText (monitorProblem);

        // §10.5/§6.5/§6.6 guidance. On the slow tick because it stats the
        // destination volume and runs the level detectors.
        mainScreen.setAdviceText (application.pollStatusAdvice (1.0 / kStatusRefreshHz));

        if (advancedVisible)
            refreshAdvanced();
    }
}

void MainComponent::refreshAdvanced()
{
    advancedPanel.setSampleRate (application.getSampleRate());
    advancedPanel.setBitDepth (application.getBitDepth());
    advancedPanel.setBufferSize (application.getCurrentBufferSize());
    advancedPanel.setMeasuredLatency (application.getMeasuredLatencyMs());
    advancedPanel.setActiveBackendDescription (application.getActiveBackendDescription());
    advancedPanel.setDriftReport (application.getDriftReport());
    advancedPanel.setDestinationFolderText ("Destination folder: " + application.getDestinationFolder());

    juce::StringArray outputs;
    for (const auto& name : application.getOutputDeviceNames())
        outputs.add (juce::String (name));

    advancedPanel.setOutputDevices (outputs, {});

    const int micCount = application.getIncludedMicCount();
    juce::StringArray micNames;
    for (int i = 0; i < micCount; ++i)
        micNames.add (application.getMicDisplayName (i));

    advancedPanel.setClockMasters (micNames, application.getClockMasterName());

    // Rebuilt only when the mic set changes: doing it every tick would reset a
    // slider out from under the user mid-drag.
    if (micCount != lastAdvancedMicCount)
    {
        advancedPanel.setTrimChannels (micNames, [this] (int i) { return application.getChannelTrimDb (i); });
        lastAdvancedMicCount = micCount;
    }
}

void MainComponent::toggleAdvanced()
{
    advancedVisible = ! advancedVisible;

    if (advancedVisible)
        refreshAdvanced();

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
