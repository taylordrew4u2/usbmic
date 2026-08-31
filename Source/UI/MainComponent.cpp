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

    cameraViewport.setViewedComponent (&cameraPanel, false);
    cameraViewport.setScrollBarsShown (true, false);
    cameraViewport.setVisible (false);
    addChildComponent (cameraViewport);

    mainScreen.onRecordButtonClicked = [this] { beginRecording(); };

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
    mainScreen.onCamerasClicked = [this] { toggleCameras(); };

    // The live views come from the controller, which is the only thing holding
    // the open devices -- the panel never opens a camera itself.
    cameraPanel.makeViewer = [this] (const std::string& id) {
        return application.getCameraController().createViewer (id);
    };

    cameraPanel.onCameraEnabledChanged = [this] (const std::string& id, bool enabled) {
        application.getCameraController().getSelection().setEnabled (id, enabled);
        // Opening or releasing the camera immediately is what makes the toggle
        // mean something: the view appears or goes when it is clicked, not when
        // the next take starts.
        application.openEnabledCameras();
        refreshCameras();
    };

    cameraPanel.onCameraRenamed = [this] (const std::string& id, const juce::String& name) {
        application.getCameraController().getSelection().setAssignedName (id, name.toStdString());
    };

    cameraPanel.onPreviewQualityChanged = [this] (PreviewQuality quality) {
        application.getCameraController().setPreviewQuality (quality);
    };

    cameraPanel.onCloseClicked = [this] { toggleCameras(); };

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
        chooseDestinationFolder ([this] { refreshAdvanced(); });
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

    // §10.1/§6.2: the question asked before the first take, and the answer
    // given after every one. Children of this component rather than
    // AlertWindows so they arrive in the app's own palette and spacing, and
    // addChildComponent (not addAndMakeVisible) so neither is up until it is
    // wanted.
    saveLocationPrompt.folderNameFor = [this] (const juce::String& name) {
        return application.planSave (name).folderName;
    };
    saveLocationPrompt.onStart = [this] {
        application.setAskWhereToSaveEveryTime (saveLocationPrompt.getAskEveryTime());
        // §10.1: the answer is about the folder they were just shown, so it is
        // recorded against that folder and every later press goes straight
        // through -- §10.4's "no confirmation" holds from here on.
        application.confirmSaveLocation();
        mainScreen.setSessionName (saveLocationPrompt.getSessionName());
        dismissSaveLocationPrompt();
        startRecordingNow();
    };
    saveLocationPrompt.onChooseFolder = [this] {
        // The card is rebuilt from the main screen's name field, so a name
        // typed into the card goes back there first -- otherwise picking a
        // folder silently throws away what the user had just typed.
        mainScreen.setSessionName (saveLocationPrompt.getSessionName());

        // Re-stated against the folder they just picked, rather than dismissed:
        // the whole card was the answer to "where does this go", and the answer
        // has just changed.
        chooseDestinationFolder ([this] { showSaveLocationPrompt(); });
    };
    saveLocationPrompt.onCancel = [this] {
        // Backing out of the card is not backing out of the name: it lands in
        // the box on the main screen, where the user can see it and where the
        // next press will pick it up.
        mainScreen.setSessionName (saveLocationPrompt.getSessionName());
        dismissSaveLocationPrompt();
    };
    addChildComponent (saveLocationPrompt);

    savedTakePanel.onOpenFolder = [this] {
        // §6.2: the offer to open the containing folder. Reveals the session
        // folder in the OS file browser rather than opening the files, which
        // is the difference between "here is your recording" and a media
        // player nobody asked for.
        juce::File (savedTakeFolder).revealToUser();
    };
    savedTakePanel.onDone = [this] {
        savedTakePanel.setVisible (false);
        grabKeyboardFocus();
    };
    addChildComponent (savedTakePanel);

    // Tall enough that the whole main screen -- monitor volume, mute and the
    // Settings button included -- is on screen at launch. At 480 the bottom
    // row sat below the fold, so the one door into Settings was reachable
    // only by scrolling. The viewport still scrolls on shorter displays.
    setSize (720, mainScreen.getRequiredHeight() + 24);

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
    // A card is up and owns the keyboard. Muting the room from behind one would
    // be a change the user cannot see the cause of.
    if (saveLocationPrompt.isVisible() || savedTakePanel.isVisible())
        return false;

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

void MainComponent::beginRecording()
{
    // §6.2: whatever is in the name box when record is pressed names the take.
    application.setSessionName (mainScreen.getSessionName());

    const bool stopping = application.getRecordingEngine().getState() == RecordingState::Recording;

    // §10.4: stopping is never confirmed and never delayed. Only the first
    // start against a destination the user has not been shown asks anything,
    // and it asks before a file exists rather than after one is lost.
    if (! stopping && ! application.isSaveLocationConfirmed())
    {
        showSaveLocationPrompt();
        return;
    }

    startRecordingNow();
}

void MainComponent::startRecordingNow()
{
    const bool stopping = application.getRecordingEngine().getState() == RecordingState::Recording;

    // §6.4: arming is blocked until the destination has passed its throughput
    // benchmark, and refusing before the take starts is the whole point --
    // "never degrade mid-take". The record button is disabled for this, but the
    // prompt's own start button is a second way in, and picking a new folder
    // from the prompt is exactly what sets a fresh benchmark running.
    if (! stopping && application.getRecordDisabledReason().isNotEmpty())
        return;

    application.setSessionName (mainScreen.getSessionName());
    application.toggleRecording();
    mainScreen.setRecording (application.getRecordingEngine().getState() == RecordingState::Recording);

    // §6.2: a stop that wrote a take raises the notice; showing it here rather
    // than waiting for the next timer tick keeps the panel attached to the
    // press that caused it.
    showSavedTake();
}

void MainComponent::showSaveLocationPrompt()
{
    const auto plan = application.planSave (mainScreen.getSessionName());

    saveLocationPrompt.setSessionName (mainScreen.getSessionName());
    saveLocationPrompt.setAskEveryTime (application.getAskWhereToSaveEveryTime());
    saveLocationPrompt.setPlan (plan.parentFolder, plan.folderName, plan.mirrorFolder, plan.fileNames,
                                application.getCameraController().getSelection().getEnabledCount());

    saveLocationPrompt.setBlockedReason (application.getRecordDisabledReason());
    saveLocationPrompt.setBounds (getLocalBounds());
    saveLocationPrompt.setVisible (true);
    saveLocationPrompt.toFront (true);
    saveLocationPrompt.prepareToShow();
}

void MainComponent::dismissSaveLocationPrompt()
{
    saveLocationPrompt.setVisible (false);
    grabKeyboardFocus();
}

void MainComponent::showSavedTake()
{
    Application::SavedTake take;

    if (! application.consumeSavedTake (take))
        return;

    savedTakeFolder = take.folder;

    std::vector<SavedTakePanel::FileRow> rows;
    rows.reserve (take.files.size());

    for (const auto& file : take.files)
        rows.push_back ({ file.name, file.sizeBytes });

    savedTakePanel.setTake (take.folder, take.mirrorFolder, rows);
    savedTakePanel.setBounds (getLocalBounds());
    savedTakePanel.setVisible (true);
    savedTakePanel.toFront (true);
    savedTakePanel.prepareToShow();
}

void MainComponent::chooseDestinationFolder (std::function<void()> onChosen)
{
    folderChooser = std::make_unique<juce::FileChooser> ("Choose where recordings are saved",
                                                         juce::File (application.getDestinationFolder()));

    folderChooser->launchAsync (juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectDirectories,
                                [this, onChosen] (const juce::FileChooser& chooser) {
                                    const auto result = chooser.getResult();

                                    if (result.isDirectory())
                                        application.setDestinationFolder (result);

                                    if (onChosen)
                                        onChosen();
                                });
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

        // Idle: where the next take will go. Recording: the folder this one is
        // actually in, which is the specific thing a user needs and the parent
        // folder only implies.
        mainScreen.setSaveLocationText (isRecording
            ? "Saving into " + application.getCurrentSessionFolder()
            : "Saves to " + application.getDestinationFolder());

        // §6.2/§10.6: the files themselves, growing. Read off the folder rather
        // than assumed from the channel count, so what is on screen is what is
        // on the disk. On the slow tick because it stats the destination
        // volume, which §14.3 warns is exactly where contention shows up.
        juce::String savingLine;

        if (isRecording)
        {
            const auto files = Application::listSessionFiles (application.getCurrentSessionFolder());
            int64_t total = 0;

            for (const auto& file : files)
                total += file.sizeBytes;

            if (! files.empty())
                savingLine = "Writing " + juce::String ((int) files.size()) + " files -- "
                           + juce::File::descriptionOfSizeInBytes (total) + " so far";
        }

        if (savingLine != lastSavingLine)
        {
            lastSavingLine = savingLine;
            mainScreen.setFilesBeingSavedText (savingLine);
        }

        // A take can also end without the record button: §6.5 stops one when
        // the card fills or is pulled. The notice belongs to the stop, not to
        // the press, so it is picked up here too.
        showSavedTake();

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

        mainScreen.setCameraCount (application.getCameraController().getSelection().getEnabledCount());

        // The benchmark behind this finishes on its own thread, so a prompt
        // that was blocked when it opened has to notice when it stops being.
        if (saveLocationPrompt.isVisible())
            saveLocationPrompt.setBlockedReason (application.getRecordDisabledReason());

        if (advancedVisible)
            refreshAdvanced();

        if (cameraVisible)
            refreshCameras();
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

void MainComponent::toggleCameras()
{
    cameraVisible = ! cameraVisible;

    if (cameraVisible)
    {
        // Opening the door is the user asking to see the cameras, which is the
        // moment to actually open them -- and on macOS the moment to spend the
        // privacy prompt, with the reason on screen behind it.
        advancedVisible = false;
        application.getCameraController().refreshCameras();
        application.openEnabledCameras();
        refreshCameras();
    }

    cameraViewport.setVisible (cameraVisible);
    advancedViewport.setVisible (advancedVisible);
    mainViewport.setVisible (! cameraVisible && ! advancedVisible);

    if (cameraVisible)
        cameraViewport.setViewPosition (0, 0);

    resized();
}

void MainComponent::refreshCameras()
{
    auto& controller = application.getCameraController();

    cameraPanel.setUnavailableReason (controller.getUnavailableReason());
    cameraPanel.setProblemText (controller.getProblem());
    cameraPanel.setPreviewQuality (controller.getPreviewQuality());
    cameraPanel.setRecording (application.getRecordingEngine().getState() == RecordingState::Recording
                                  && controller.isRecording());

    std::vector<CameraPanel::CameraRow> cameras;

    for (const auto& camera : controller.getSelection().getAvailableCameras())
        cameras.push_back ({ camera.id,
                             juce::String (controller.getSelection().getDisplayName (camera.id)),
                             controller.getSelection().isEnabled (camera.id) });

    cameraPanel.setCameras (cameras);

    const int requiredHeight = cameraPanel.getRequiredHeight();

    if (requiredHeight != lastCameraHeight)
    {
        lastCameraHeight = requiredHeight;
        resized();
    }
}

void MainComponent::toggleAdvanced()
{
    advancedVisible = ! advancedVisible;

    if (advancedVisible)
    {
        // §10.3 says one door at a time. Two panels stacked over the main
        // screen would leave whichever was underneath unreachable but alive,
        // still running its live views.
        cameraVisible = false;
        refreshAdvanced();
    }

    advancedViewport.setVisible (advancedVisible);
    cameraViewport.setVisible (cameraVisible);
    mainViewport.setVisible (! advancedVisible && ! cameraVisible);

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
    cameraViewport.setBounds (bounds);

    // The modal cards cover whichever screen is underneath, so they follow the
    // window rather than the viewport they happen to be over.
    saveLocationPrompt.setBounds (bounds);
    savedTakePanel.setBounds (bounds);

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
    fit (cameraViewport, cameraPanel, cameraPanel.getRequiredHeight());
}

} // namespace mma
