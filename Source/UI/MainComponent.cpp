#include "MainComponent.h"
#include "../App/Application.h"

namespace mma {

MainComponent::MainComponent (Application& app)
    : application (app)
{
    addAndMakeVisible (mainScreen);
    // Owned by this component, not by the viewports: `false` here means the
    // viewport does not take ownership and will not delete them.
    mainViewport.setViewedComponent (&mainScreen, false);
    mainViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (mainViewport);

    advancedViewport.setViewedComponent (&advancedPanel, false);
    advancedViewport.setScrollBarsShown (true, false);
    advancedViewport.setVisible (false);
    addChildComponent (advancedViewport);

    mainScreen.onRecordButtonClicked = [this] {
        // §6.2: whatever is in the name box when record is pressed names the take.
        application.setSessionName (mainScreen.getSessionName());
        application.toggleRecording();
        mainScreen.setRecording (application.getRecordingEngine().getState() == RecordingState::Recording);
    };

    mainScreen.onVolumeChanged = [this] (double volume0to100) {
        // Master monitor volume, mapped logarithmically per §5.1; MonitorBus
        // owns the actual mapping, this just forwards the UI value.
        application.setMasterVolume (volume0to100);
    };

    mainScreen.onMuteToggled = [this] {
        auto* bus = application.getMonitorBus();

        if (bus == nullptr)
            return;

        // §5: the runaway cut stays muted until the user says otherwise. The
        // mute button is that path -- without this, a runaway cut is a dead
        // end the user can only escape by restarting the app.
        if (bus->isRunawayMuted())
        {
            bus->manuallyUnmute();
            bus->setGlobalMute (false);
            return;
        }

        bus->setGlobalMute (! bus->isGloballyMuted());
    };

    mainScreen.onMicNameClicked = [this] (int index) { promptRenameMic (index); };

    // §5.1: spacebar is the instant mute, so the window has to take keys.
    setWantsKeyboardFocus (true);

    // A capture rebuild destroys the Metering objects the skull meters point
    // at, and each skull's own timer dereferences its pointer on the next
    // tick. Rebinding inside the rebuild's call stack closes that window.
    application.onCaptureRebuilt = [this] { rebindMeters(); };

    mainScreen.onAdvancedClicked = [this] { toggleAdvanced(); };

    // §10.3: everything behind the one door is optional, but none of it is
    // decorative -- each control below reaches the engine it names.
    advancedPanel.onTrimChanged = [this] (int index, float trimDb) {
        application.setChannelTrimDb (index, trimDb);
    };

    advancedPanel.onClockMasterChanged = [this] (const juce::String& name) {
        application.setClockMasterByName (name);
    };

    advancedPanel.onAggregateNameChanged = [this] (const juce::String& name) {
        application.setAggregateDeviceName (name);
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

    advancedPanel.onStorageVolumeChosen = [this] (const juce::String& path) {
        application.setDestinationByPath (path);
        refreshAdvanced();
    };

    advancedPanel.onCloseClicked = [this] { toggleAdvanced(); };

    advancedPanel.onMicEnabledChanged = [this] (const juce::String& name, bool enabled) {
        application.setMicEnabledByName (name, enabled);
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
    // The window is torn down before Application::shutdown(), and a queued
    // device-change can still fire in between -- it must not call into a
    // destroyed component.
    application.onCaptureRebuilt = nullptr;
    stopTimer();
}

void MainComponent::timerCallback()
{
    refreshStatus();
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    // §5.1: spacebar mutes and unmutes the monitor instantly. A focused text
    // field never reaches here -- it consumes its own keys -- so typing a
    // space into the session name does not silence the room.
    if (key == juce::KeyPress::spaceKey)
    {
        if (mainScreen.onMuteToggled)
            mainScreen.onMuteToggled();

        return true;
    }

    return false;
}

void MainComponent::promptRenameMic (int index)
{
    // §14.6: click the skull that lit up when you tapped the mic, type who it
    // is. The name follows the physical port across replug (§2.4).
    auto* window = new juce::AlertWindow ("Name this microphone",
                                          "The name goes on its skull and into its recording's filename.",
                                          juce::MessageBoxIconType::QuestionIcon, this);
    window->addTextEditor ("name", application.getMicDisplayName (index));
    window->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    window->enterModalState (true,
        juce::ModalCallbackFunction::create ([this, window, index] (int result)
        {
            if (result == 1)
                application.setMicAssignedName (index, window->getTextEditorContents ("name"));
        }),
        true); // delete the window when dismissed
}

void MainComponent::refreshStatus()
{
    const int micCount = application.getIncludedMicCount();

    if (micCount != lastMicCount)
    {
        // §6.5: mics come and go without a dialog and without interrupting a take.
        mainScreen.setMicCount (micCount);

        lastMicCount = micCount;
    }

    // Belt and braces on top of onCaptureRebuilt: rebinding every frame means
    // even a rebuild path that forgets the callback cannot leave a stale
    // pointer alive for more than one tick.
    rebindMeters();

    // §14.6: light the skull of whoever was just heard alone.
    mainScreen.setHighlightedMic (application.getTappedChannel());

    // The mute button reflects the bus, including a §5 runaway cut it must
    // be able to undo.
    if (auto* bus = application.getMonitorBus())
        mainScreen.setMuteState (bus->isMuted(), bus->isRunawayMuted());

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
        // A §5 runaway cut outranks both: the user is sitting in silence and
        // the line has to say why and what to press.
        auto monitorProblem = application.getMonitorProblem();

        if (auto* bus = application.getMonitorBus())
            if (bus->isRunawayMuted())
                monitorProblem = "Sound was cut to protect your ears from feedback. Press Unmute to bring it back.";

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

void MainComponent::rebindMeters()
{
    mainScreen.setMixMetering (application.getMixMetering());

    const int micCount = application.getIncludedMicCount();

    // Size the strip set BEFORE binding anything into it.
    //
    // Rebuilding the capture destroys every Metering object the existing strips
    // point at -- CaptureCoordinator::startMonitoring clears and recreates the
    // lot on every call. Binding only the first micCount strips therefore left
    // any surplus strip from a larger set holding a pointer into freed memory,
    // and the strips repaint at 60 Hz, so it was read long before the next
    // timer tick could have tidied up.
    //
    // That window was survivable while the channel count only shrank on an
    // unplug. Adding a checkbox to deselect a microphone made it reachable on
    // demand, and it crashed.
    if (mainScreen.getMicCount() != micCount)
        mainScreen.setMicCount (micCount);

    for (int i = 0; i < micCount; ++i)
    {
        if (auto* skull = mainScreen.getSkullMeter (i))
        {
            skull->setMetering (application.getChannelMetering (i));
            skull->setMicName (application.getMicDisplayName (i));
        }
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
    advancedPanel.setAggregateStatus (application.getAggregateStatus());
    advancedPanel.setAggregateName (application.getAggregateDeviceName());
    advancedPanel.setDestinationFolderText ("Destination folder: " + application.getDestinationFolder());

    juce::StringArray outputs;
    for (const auto& name : application.getOutputDeviceNames())
        outputs.add (juce::String (name));

    advancedPanel.setOutputDevices (outputs, {});

    const int micCount = application.getIncludedMicCount();
    juce::StringArray micNames;
    for (int i = 0; i < micCount; ++i)
        micNames.add (application.getMicDisplayName (i));

    std::vector<std::pair<juce::String, bool>> micSelections;
    for (const auto& m : application.getMicSelections())
        micSelections.push_back ({ m.displayName, m.enabled });
    advancedPanel.setMicSelections (micSelections);

    // Adding or removing a microphone changes how tall the panel needs to be,
    // and only this component can resize it inside its viewport. Guarded on the
    // height actually changing, because refreshAdvanced runs on the UI tick and
    // relaying out the whole panel twice a second would be churn for nothing.
    const int requiredHeight = advancedPanel.getRequiredHeight();

    if (requiredHeight != lastAdvancedHeight)
    {
        lastAdvancedHeight = requiredHeight;
        resized();
    }

    std::vector<AdvancedPanel::VolumeChoice> volumes;
    for (const auto& v : application.getStorageVolumes())
        volumes.push_back ({ v.displayName, v.path, v.isCurrent });
    advancedPanel.setStorageVolumes (volumes);

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

    advancedViewport.setVisible (advancedVisible);
    mainViewport.setVisible (! advancedVisible);

    // Back to the top on entry, so opening Settings never starts halfway down
    // wherever it was last left.
    if (advancedVisible)
        advancedViewport.setViewPosition (0, 0);

    resized();
}

void MainComponent::resized()
{
    auto bounds = getLocalBounds();

    mainViewport.setBounds (bounds);
    advancedViewport.setBounds (bounds);

    // Each screen is laid out at least as tall as its content needs, and at
    // least as tall as the window -- so a short window scrolls and a tall one
    // does not leave the content floating in a strip at the top. The width
    // excludes the scrollbar when one is showing, or the content would sit
    // underneath it.
    const auto fit = [] (juce::Viewport& viewport, juce::Component& content, int requiredHeight)
    {
        const int width = viewport.getWidth()
                        - (viewport.isVerticalScrollBarShown() ? viewport.getScrollBarThickness() : 0);

        content.setSize (juce::jmax (1, width),
                         juce::jmax (viewport.getHeight(), requiredHeight));
    };

    fit (mainViewport, mainScreen, mainScreen.getRequiredHeight());
    fit (advancedViewport, advancedPanel, advancedPanel.getRequiredHeight());
}

} // namespace mma
