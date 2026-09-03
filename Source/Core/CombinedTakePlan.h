#pragma once
#include <string>
#include <vector>

namespace mma {

/// Whether a take also produces one file per camera carrying the sound.
///
/// The picture and the sound are written separately and always have been, for
/// a reason worth keeping: the platform camera capture is video-only, the
/// microphones are the sound, and one clean track per person is the whole
/// point of the rig. A file with both in it is a *third* thing -- something to
/// review, send, or upload without an editor -- not a replacement for either.
///
/// So this never removes anything. The stems, the mix and the silent video all
/// stay exactly where they were; when this is on, a combined file appears
/// beside them.
enum class CombinedVideoMode
{
    Off,      ///< Today's behaviour: picture and sound, side by side.
    Combined  ///< Also write one video-with-sound file per camera.
};

/// One camera's part in the combining step.
struct CombinedTakeJob
{
    std::string videoFile;   ///< "V01_Kitchen-Cam.mov", relative to the session folder
    std::string audioFile;   ///< "MIX.wav", the summed mix with trims and the limiter
    std::string outputFile;  ///< "V01_Kitchen-Cam_with-sound.mp4"

    /// How far into the audio the video's first frame lands.
    ///
    /// The sound starts first: the writer thread and the stem files are opened
    /// before any camera is asked to record, so a camera's file begins some
    /// tens or hundreds of milliseconds into the take. Laying the two on top of
    /// each other without accounting for that puts the picture ahead of the
    /// sound by exactly that much -- small enough to look like nothing and
    /// large enough to look wrong, which is the worst size for a sync error.
    ///
    /// Trimming this much off the front of the audio is what lines them up.
    double audioLeadSeconds = 0.0;
};

/// Why no combined file can be made, in the app's voice. Empty means it can.
struct CombinedTakePlan
{
    std::vector<CombinedTakeJob> jobs;
    std::string problem;

    bool hasWork() const { return ! jobs.empty(); }
};

/// What one camera contributed to the take, as the plan needs to see it.
struct CombinedTakeInput
{
    std::string videoFile;          ///< file name with extension, or empty if it wrote nothing
    double videoStartOffsetSeconds; ///< how long after the audio's t=0 this camera began
};

/// §6.2 naming, applied to the combined file: the video's own name with a
/// suffix, so the folder still sorts the way it did and the origin of each
/// file is readable without opening it.
///
/// The container is chosen by what the camera actually wrote, because the
/// picture is copied rather than re-encoded -- re-encoding a take would cost
/// quality nobody asked to spend and minutes nobody asked to wait.
///
///   .mov  ->  .mp4   (what macOS writes; H.264 moves between the two intact)
///   other ->  .mkv   (Windows writes .wmv, which mp4 cannot legally carry;
///                     Matroska takes any codec, so the copy still holds)
std::string combinedFileNameFor (const std::string& videoFileName);

/// Builds the plan for a finished take.
///
/// `mixFileName` is the audio to lay under the picture -- the mix rather than a
/// stem, since that is the one already carrying the user's trims and the mix
/// bus limiter, and the one that sounds like the room rather than like one
/// person in it.
///
/// A camera that wrote nothing is skipped rather than failing the plan: one
/// camera failing to start must not cost the others their combined file. When
/// every camera is skipped the plan says so in one sentence.
///
/// Pure: no file system, no process, no platform. What can actually be run is
/// the muxer's business; what *should* be run is this.
CombinedTakePlan buildCombinedTakePlan (CombinedVideoMode mode,
                                        const std::vector<CombinedTakeInput>& cameras,
                                        const std::string& mixFileName);

} // namespace mma
