#include "CameraPanel.h"
#include "AppLookAndFeel.h"

namespace mma {

namespace {
    constexpr int kMargin = 16;
    constexpr int kRowHeight = 30;
    constexpr int kViewGap = 10;
}

CameraPanel::CameraPanel()
{
    heading.setText ("Cameras", juce::dontSendNotification);
    heading.setFont (juce::Font (20.0f, juce::Font::bold));
    heading.setColour (juce::Label::textColourId, AppLookAndFeel::bone);
    addAndMakeVisible (heading);

    // The whole shape of the feature in one sentence, in the plain language
    // §10.2 requires: no codecs, no containers, no bitrates.
    explanation.setText ("Turn on any camera that's plugged in and you'll see it live. "
                         "Press record and it saves alongside the sound -- the picture and "
                         "the sound are separate files, so the video has no sound of its own "
                         "and your microphone tracks stay untouched.",
                         juce::dontSendNotification);
    explanation.setFont (juce::Font (13.0f));
    explanation.setColour (juce::Label::textColourId, AppLookAndFeel::secondary);
    explanation.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (explanation);

    fullPreviewToggle.onClick = [this] {
        const auto quality = fullPreviewToggle.getToggleState() ? PreviewQuality::Full
                                                                : PreviewQuality::Low;
        setPreviewQuality (quality);

        if (onPreviewQualityChanged)
            onPreviewQualityChanged (quality);
    };
    addAndMakeVisible (fullPreviewToggle);

    // Said out loud, because it is the question this toggle raises and the
    // wrong answer would make someone record a worse take to save some CPU.
    qualityNote.setText ("Recording is always at the camera's best quality. This only "
                         "changes the picture on this screen.", juce::dontSendNotification);
    qualityNote.setFont (juce::Font (12.0f));
    qualityNote.setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
    qualityNote.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (qualityNote);

    problemLabel.setFont (juce::Font (13.0f));
    problemLabel.setColour (juce::Label::textColourId, AppLookAndFeel::warning);
    problemLabel.setMinimumHorizontalScale (1.0f);
    addChildComponent (problemLabel);

    unavailableLabel.setFont (juce::Font (13.0f));
    unavailableLabel.setColour (juce::Label::textColourId, AppLookAndFeel::secondary);
    unavailableLabel.setMinimumHorizontalScale (1.0f);
    addChildComponent (unavailableLabel);

    closeButton.onClick = [this] { if (onCloseClicked) onCloseClicked(); };
    addAndMakeVisible (closeButton);
}

CameraPanel::~CameraPanel() = default;

int CameraPanel::viewHeight() const
{
    return CameraSelection::previewSettingsFor (previewQuality).maxViewHeight;
}

void CameraPanel::setPreviewQuality (PreviewQuality quality)
{
    if (previewQuality == quality)
        return;

    previewQuality = quality;
    fullPreviewToggle.setToggleState (quality == PreviewQuality::Full, juce::dontSendNotification);
    resized();
}

void CameraPanel::setRecording (bool isRecording)
{
    if (recording == isRecording)
        return;

    recording = isRecording;

    // §9.3: never colour alone. The heading says it as well as showing it.
    heading.setText (recording ? "Cameras -- recording" : "Cameras", juce::dontSendNotification);
    heading.setColour (juce::Label::textColourId,
                       recording ? AppLookAndFeel::danger : AppLookAndFeel::bone);
}

void CameraPanel::setUnavailableReason (const juce::String& reason)
{
    const bool unavailable = reason.isNotEmpty();

    if (unavailable == unavailableLabel.isVisible() && unavailableLabel.getText() == reason)
        return;

    unavailableLabel.setText (reason, juce::dontSendNotification);
    unavailableLabel.setVisible (unavailable);

    // The controls are not disabled, they are gone: a row of dead toggles
    // invites someone to keep clicking them looking for the one that works.
    fullPreviewToggle.setVisible (! unavailable);
    qualityNote.setVisible (! unavailable);

    resized();
}

void CameraPanel::setProblemText (const juce::String& text)
{
    if (problemLabel.getText() == text)
        return;

    problemLabel.setText (text, juce::dontSendNotification);
    problemLabel.setVisible (text.isNotEmpty());
    resized();
}

void CameraPanel::setCameras (const std::vector<CameraRow>& cameras)
{
    // Rebuilding tears down every live view, which is a visible flicker and a
    // real cost, so it happens only when the set or its state has moved.
    std::vector<std::string> ids;
    std::vector<char> enabled;

    for (const auto& camera : cameras)
    {
        ids.push_back (camera.id);
        enabled.push_back (camera.enabled ? 1 : 0);
    }

    if (ids == lastCameraIds && enabled == lastEnabled)
        return;

    lastCameraIds = std::move (ids);
    lastEnabled = std::move (enabled);
    rebuildRows (cameras);
}

void CameraPanel::rebuildRows (const std::vector<CameraRow>& cameras)
{
    rows.clear();

    for (const auto& camera : cameras)
    {
        Row row;
        row.id = camera.id;

        row.enabledToggle = std::make_unique<juce::ToggleButton> ("Record this camera");
        row.enabledToggle->setToggleState (camera.enabled, juce::dontSendNotification);
        row.enabledToggle->onClick = [this, id = camera.id, button = row.enabledToggle.get()] {
            if (onCameraEnabledChanged)
                onCameraEnabledChanged (id, button->getToggleState());
        };
        addAndMakeVisible (*row.enabledToggle);

        // §14.6 for cameras: the name goes on the file, so "which one is the
        // wide shot" is answerable from a file browser a week later.
        row.nameEditor = std::make_unique<juce::TextEditor>();
        row.nameEditor->setText (camera.displayName, juce::dontSendNotification);
        row.nameEditor->setTextToShowWhenEmpty ("Name this camera", AppLookAndFeel::tertiary);
        row.nameEditor->onFocusLost = [this, id = camera.id, editor = row.nameEditor.get()] {
            if (onCameraRenamed)
                onCameraRenamed (id, editor->getText());
        };
        row.nameEditor->onReturnKey = [this, id = camera.id, editor = row.nameEditor.get()] {
            if (onCameraRenamed)
                onCameraRenamed (id, editor->getText());
        };
        addAndMakeVisible (*row.nameEditor);

        if (camera.enabled && makeViewer)
            row.viewer = makeViewer (camera.id);

        if (row.viewer != nullptr)
        {
            addAndMakeVisible (*row.viewer);
        }
        else
        {
            // A camera that is off, or one that would not open, gets a well
            // that says which -- never an empty rectangle.
            row.placeholder = std::make_unique<juce::Label>();
            row.placeholder->setText (camera.enabled
                                          ? "This camera didn't open. Check that nothing else is using it."
                                          : "Switched off. Turn it on to see it live.",
                                      juce::dontSendNotification);
            row.placeholder->setJustificationType (juce::Justification::centred);
            row.placeholder->setFont (juce::Font (12.0f));
            row.placeholder->setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
            addAndMakeVisible (*row.placeholder);
        }

        rows.push_back (std::move (row));
    }

    resized();
}

int CameraPanel::getRequiredHeight() const
{
    // The way back, the heading, and the paragraph under it.
    int height = kMargin + 30 + 10 + 30 + 8 + 56 + 16;

    if (unavailableLabel.isVisible())
        return height + 40 + kMargin;

    height += kRowHeight + 4 + 34 + 16; // the preview toggle and its note

    if (problemLabel.isVisible())
        height += 40;

    for (size_t i = 0; i < rows.size(); ++i)
        height += kRowHeight + 6 + viewHeight() + kViewGap;

    if (rows.empty())
        height += 60;

    return height + kMargin;
}

void CameraPanel::paint (juce::Graphics& g)
{
    g.fillAll (AppLookAndFeel::background);

    // The same card treatment the rest of the app uses, so a live view sits in
    // a well rather than floating on the background.
    g.setColour (AppLookAndFeel::surface);

    for (const auto& row : rows)
    {
        const auto* view = row.viewer != nullptr ? row.viewer.get()
                                                 : static_cast<juce::Component*> (row.placeholder.get());

        if (view != nullptr)
            g.fillRoundedRectangle (view->getBounds().toFloat().expanded (4.0f), 8.0f);
    }
}

void CameraPanel::resized()
{
    auto area = getLocalBounds().reduced (kMargin);

    // The way back goes first, in the same place and with the same wording as
    // the one in Settings. Two doors off the main screen that close in
    // different corners is two things to learn instead of one.
    closeButton.setBounds (area.removeFromTop (30).removeFromLeft (110));
    area.removeFromTop (10);

    heading.setBounds (area.removeFromTop (30));
    area.removeFromTop (8);
    explanation.setBounds (area.removeFromTop (56));
    area.removeFromTop (16);

    if (unavailableLabel.isVisible())
    {
        unavailableLabel.setBounds (area.removeFromTop (40));
        return;
    }

    fullPreviewToggle.setBounds (area.removeFromTop (kRowHeight));
    area.removeFromTop (4);
    qualityNote.setBounds (area.removeFromTop (34));
    area.removeFromTop (16);

    if (problemLabel.isVisible())
        problemLabel.setBounds (area.removeFromTop (40));

    for (auto& row : rows)
    {
        auto header = area.removeFromTop (kRowHeight);
        row.nameEditor->setBounds (header.removeFromLeft (juce::jmin (240, header.getWidth() / 2)));
        header.removeFromLeft (12);
        row.enabledToggle->setBounds (header);

        area.removeFromTop (6);
        auto view = area.removeFromTop (viewHeight());

        // 16:9 inside whatever space the panel has, centred, so a camera is
        // never stretched to the panel's shape.
        const int wanted = juce::jmin (view.getWidth(), view.getHeight() * 16 / 9);
        view = view.withSizeKeepingCentre (wanted, view.getHeight());

        if (row.viewer != nullptr)
            row.viewer->setBounds (view);
        else if (row.placeholder != nullptr)
            row.placeholder->setBounds (view);

        area.removeFromTop (kViewGap);
    }
}

} // namespace mma
