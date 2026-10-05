#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../Core/CameraSelection.h"

namespace mma {

/// The camera door: what is connected, what it looks like right now, and what
/// will be written.
///
/// Once a camera is explicitly enabled, its live view appears on the main
/// screen and moves here while this panel is open. Framing is something you fix
/// before the take, and a picture you cannot see until Record is a picture you
/// aim afterwards.
class CameraPanel : public juce::Component
{
public:
    CameraPanel();
    ~CameraPanel() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    int getRequiredHeight() const;

    struct CameraRow
    {
        std::string id;
        juce::String displayName;
        bool enabled = false;
        bool available = true;
        bool discoveryPending = false;
        bool recordingThisTake = false;
        bool startingThisTake = false;
        /// The file this camera will write, extension included. Empty when the
        /// camera is switched off and so will not write one.
        juce::String fileName;
        uint64_t viewerRevision = 0;
        /// Empty only when a current-generation image has proved this camera
        /// live. Otherwise this is the actionable waiting/open failure shown
        /// in place of a misleading black preview.
        juce::String signalStatusText;
        CameraQuality quality = CameraQuality::Best;
        /// What the camera says it is delivering ("3840 x 2160, 30 fps").
        /// Empty until it reports, and on platforms that don't.
        juce::String activeFormatText;
    };

    /// The camera list and the state of each one. Rebuilds the rows -- and the
    /// live views -- only when the set has actually changed, so this is safe to
    /// call from the UI tick.
    void setCameras (const std::vector<CameraRow>& cameras);

    /// Empty when cameras work here. Otherwise the one sentence saying why they
    /// do not, in place of the controls.
    void setUnavailableReason (const juce::String& reason);
    /// §10.6: anything currently wrong with a camera. Empty hides the line.
    void setProblemText (const juce::String& text);
    void setPreviewQuality (PreviewQuality quality);
    /// Reflected in the panel's own wording, so what is on screen says whether
    /// the cameras are running.
    void setRecording (bool isRecording);

    /// Makes a live view for one camera. Supplied by the owner because only the
    /// controller holds the open devices; returning nullptr is normal and means
    /// that camera is not open.
    std::function<std::unique_ptr<juce::Component> (const std::string&)> makeViewer;

    /// As MainScreen::setCameraPreviewsHidden: the native previews are drawn
    /// above any card, so they are hidden, not rebuilt, while one is up.
    void setCameraPreviewsHidden (bool hidden);

    std::function<void (const std::string&, bool)> onCameraEnabledChanged;
    std::function<void (const std::string&, const juce::String&)> onCameraRenamed;
    std::function<void (PreviewQuality)> onPreviewQualityChanged;
    std::function<void (const std::string&, CameraQuality)> onCameraQualityChanged;
    std::function<void()> onCloseClicked;

private:
    struct Row
    {
        std::string id;
        std::unique_ptr<juce::ToggleButton> enabledToggle;
        std::unique_ptr<juce::TextEditor> nameEditor;
        // Owned here and destroyed with the row: a viewer outliving the camera
        // device behind it is a component drawing from freed memory.
        std::unique_ptr<juce::Component> viewer;
        std::unique_ptr<juce::Label> placeholder;
        std::unique_ptr<juce::Label> fileName;
        std::unique_ptr<juce::Label> qualityLabel;
        std::unique_ptr<juce::ComboBox> qualityCombo;
        std::unique_ptr<juce::Label> formatLabel;
    };

    juce::Label heading, explanation, problemLabel, unavailableLabel;
    juce::ToggleButton fullPreviewToggle { "Show a bigger, sharper preview" };
    juce::Label qualityNote;
    // Worded and placed like the one in Settings: the same door, closing the
    // same way.
    juce::TextButton closeButton { "< Done" };

    std::vector<Row> rows;
    bool previewsHidden = false;
    std::vector<std::string> lastCameraIds;
    std::vector<char> lastEnabled;
    std::vector<char> lastAvailable;
    std::vector<char> lastDiscoveryPending;
    std::vector<char> lastRecordingThisTake;
    std::vector<char> lastStartingThisTake;
    juce::StringArray lastFileNames;
    std::vector<uint64_t> lastViewerRevisions;
    juce::StringArray lastSignalStatusTexts;
    std::vector<int> lastQualities;
    juce::StringArray lastFormatTexts;
    PreviewQuality previewQuality = PreviewQuality::Low;
    bool recording = false;
    int recordingCameraCount = 0;
    int startingCameraCount = 0;

    int viewHeight() const;
    void rebuildRows (const std::vector<CameraRow>& cameras);
    void updateRecordingHeading();
    static juce::String formatLine (const CameraRow& camera, bool recordingNow);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CameraPanel)
};

} // namespace mma
