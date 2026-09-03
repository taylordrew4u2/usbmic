#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <map>
#include <memory>
#include <string>
#include "../Core/CameraSelection.h"
#include "../Core/CombinedTakePlan.h"
#include <vector>

#if JUCE_USE_CAMERA
#include <juce_video/juce_video.h>
#endif

namespace mma {

/// The picture half of a take.
///
/// There is no audio path in this class at all, and that is the point. The
/// microphones are the sound: a camera's own microphone folded into a take
/// would put a room mic nobody asked for into a recording whose whole purpose
/// is one clean track per person. On macOS and Windows -- the two desktop
/// targets §11 names -- the platform camera capture JUCE drives is video-only,
/// so the files this writes carry no sound track of any kind. The sound lives
/// beside them in the WAVs, two separate files, aligned by the shared session
/// start §6.1 stamps into every stem.
///
/// What gets written is always the best the camera can give. The live view is
/// the only thing the preview setting touches -- see PreviewQuality.
class CameraController
{
public:
    CameraController();
    ~CameraController();

    /// False on a build or an OS where the app cannot open a camera at all.
    /// Everything else then becomes a no-op and getUnavailableReason() says why
    /// in one sentence (§10.6), rather than the panel simply staying empty.
    bool isSupported() const;
    juce::String getUnavailableReason() const;

    /// Re-reads whatever the OS is offering. Called at launch and whenever the
    /// user opens the camera panel -- cameras do not announce themselves the
    /// way §2 audio devices do, so this is the refresh.
    void refreshCameras();

    CameraSelection& getSelection() { return selection; }
    const CameraSelection& getSelection() const { return selection; }

    /// Opens the enabled cameras for viewing, and closes the ones that are no
    /// longer enabled. §5.1 makes the sound live from launch rather than from
    /// record, and a picture is worth even less after the fact: a camera you
    /// cannot see until you press record is a camera you aim afterwards.
    void applySelection();

    /// A live view of one open camera, or nullptr when it is not open. The
    /// caller owns what comes back.
    std::unique_ptr<juce::Component> createViewer (const std::string& deviceId);

    /// §6.2: one file per camera, in the session folder next to the audio.
    /// Returns false only when nothing could be started at all.
    ///
    /// `audioStartMs` is the high-resolution millisecond counter read as the
    /// audio take began. The sound is always started first -- the stem files
    /// and the writer thread are opened before any camera is asked to record --
    /// so each camera's file begins some way into the take, and how far in is
    /// the one number the combining step cannot work out for itself. A camera
    /// gives no timestamp for its first frame, so this is measured rather than
    /// assumed: the counter is read again the moment the OS accepts the start.
    bool startRecording (const juce::File& sessionFolder, double audioStartMs = 0.0);
    void stopRecording();
    bool isRecording() const { return recording; }

    /// What each camera contributed to the take just finished: the file it
    /// wrote and how late it started. Empty when nothing recorded.
    std::vector<CombinedTakeInput> getCombinedTakeInputs() const;

    /// The file names this take is writing, extension included, for the panels
    /// that tell the user what to expect and what they got.
    juce::StringArray getPlannedFileNames() const;

    /// The file one camera will write, extension included, or empty when that
    /// camera is not in the take.
    juce::String getPlannedFileNameFor (const std::string& deviceId) const;

    /// §10.6: whatever is currently wrong with the cameras, in plain language.
    /// Empty when nothing is.
    juce::String getProblem() const { return problem; }

    void setPreviewQuality (PreviewQuality quality) { previewQuality = quality; }
    PreviewQuality getPreviewQuality() const { return previewQuality; }

private:
    CameraSelection selection;
    PreviewQuality previewQuality = PreviewQuality::Low;
    juce::String problem;
    bool recording = false;

#if JUCE_USE_CAMERA
    struct OpenCamera
    {
        std::unique_ptr<juce::CameraDevice> device;
        int osIndex = -1;
        juce::File recordingFile;

        /// Seconds after the audio's t=0 that this camera's first frame lands.
        double startOffsetSeconds = 0.0;
    };

    // Keyed by the id CameraSelection uses, so the two never have to agree on
    // an ordering -- the OS list reorders on a hot-plug and the choices must not
    // follow it onto a different camera.
    std::map<std::string, OpenCamera> open;
    void openCamera (const std::string& id, int osIndex);
    void closeCamera (const std::string& id);
#endif

    // The OS list index for each id, refreshed with the list itself.
    std::map<std::string, int> osIndexById;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CameraController)
};

} // namespace mma
