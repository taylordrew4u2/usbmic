#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../Core/CameraSelection.h"

namespace mma {

/// The camera door: what is connected, what it looks like right now, and what
/// will be written.
///
/// Live from the moment the panel opens, for the same reason §5.1 makes the
/// sound live from launch: framing is something you fix before the take, and a
/// picture you cannot see until you press record is a picture you aim at
/// afterwards.
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
        /// The file this camera will write, extension included. Empty when the
        /// camera is switched off and so will not write one.
        juce::String fileName;
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

    std::function<void (const std::string&, bool)> onCameraEnabledChanged;
    std::function<void (const std::string&, const juce::String&)> onCameraRenamed;
    std::function<void (PreviewQuality)> onPreviewQualityChanged;
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
    };

    juce::Label heading, explanation, problemLabel, unavailableLabel;
    juce::ToggleButton fullPreviewToggle { "Show a bigger, sharper preview" };
    juce::Label qualityNote;
    // Worded and placed like the one in Settings: the same door, closing the
    // same way.
    juce::TextButton closeButton { "< Done" };

    std::vector<Row> rows;
    std::vector<std::string> lastCameraIds;
    std::vector<char> lastEnabled;
    juce::StringArray lastFileNames;
    PreviewQuality previewQuality = PreviewQuality::Low;
    bool recording = false;

    int viewHeight() const;
    void rebuildRows (const std::vector<CameraRow>& cameras);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CameraPanel)
};

} // namespace mma
