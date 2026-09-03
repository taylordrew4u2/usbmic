#include "TestFramework.h"
#include "Core/CombinedTakePlan.h"
#include "Core/FfmpegCommand.h"
#include <algorithm>
#include <string>
#include <vector>

using namespace mma;

namespace {

std::vector<CombinedTakeInput> twoCameras()
{
    return { { "V01_Kitchen-Cam.mov", 0.25 },
             { "V02_Couch-Cam.mov",   0.40 } };
}

/// The value ffmpeg would see for a named flag, or empty when the flag is
/// absent. Written as a search rather than an index so a test says what it
/// means about the command instead of encoding the order of every other flag.
std::string valueAfter (const std::vector<std::string>& args, const std::string& flag)
{
    const auto it = std::find (args.begin(), args.end(), flag);

    if (it == args.end() || std::next (it) == args.end())
        return {};

    return *std::next (it);
}

bool contains (const std::vector<std::string>& args, const std::string& value)
{
    return std::find (args.begin(), args.end(), value) != args.end();
}

int indexOf (const std::vector<std::string>& args, const std::string& value)
{
    const auto it = std::find (args.begin(), args.end(), value);
    return it == args.end() ? -1 : static_cast<int> (std::distance (args.begin(), it));
}

} // namespace

TEST_CASE (CombinedTake_OffProducesNothingAndIsNotAProblem)
{
    const auto plan = buildCombinedTakePlan (CombinedVideoMode::Off, twoCameras(), "MIX.wav");

    REQUIRE_FALSE (plan.hasWork());

    // Not asking for a combined file is not a failure to make one, and a
    // problem string here would put a complaint on screen after every take by
    // everyone who left the setting alone.
    REQUIRE (plan.problem.empty());
}

TEST_CASE (CombinedTake_OneJobPerCameraThatWroteSomething)
{
    const auto plan = buildCombinedTakePlan (CombinedVideoMode::Combined, twoCameras(), "MIX.wav");

    REQUIRE (plan.problem.empty());
    REQUIRE (plan.jobs.size() == 2);

    REQUIRE (plan.jobs[0].videoFile == "V01_Kitchen-Cam.mov");
    REQUIRE (plan.jobs[0].audioFile == "MIX.wav");
    REQUIRE (plan.jobs[1].videoFile == "V02_Couch-Cam.mov");
}

TEST_CASE (CombinedTake_NamesTheOutputAfterTheVideoItCameFrom)
{
    // §6.2: the folder still sorts the way it did, and every file says where it
    // came from without being opened.
    REQUIRE (combinedFileNameFor ("V01_Kitchen-Cam.mov") == "V01_Kitchen-Cam_with-sound.mp4");
}

TEST_CASE (CombinedTake_ContainerFollowsWhatTheCameraActuallyWrote)
{
    // The picture is copied rather than re-encoded, so the container has to be
    // able to carry the codec that is already there. mp4 cannot legally hold
    // the VC-1 that Windows writes; Matroska holds anything. Picking mp4 for
    // both would produce a file that either fails to write or fails to play,
    // on the platform that cannot be tested from here.
    REQUIRE (combinedFileNameFor ("V01_Kitchen-Cam.mov") == "V01_Kitchen-Cam_with-sound.mp4");
    REQUIRE (combinedFileNameFor ("V01_Kitchen-Cam.wmv") == "V01_Kitchen-Cam_with-sound.mkv");

    // Case is the OS's business, not ours.
    REQUIRE (combinedFileNameFor ("V01_Kitchen-Cam.MOV") == "V01_Kitchen-Cam_with-sound.mp4");
}

TEST_CASE (CombinedTake_ACameraThatWroteNothingIsSkippedNotFatal)
{
    std::vector<CombinedTakeInput> cameras = { { "", 0.0 },
                                               { "V02_Couch-Cam.mov", 0.4 } };

    const auto plan = buildCombinedTakePlan (CombinedVideoMode::Combined, cameras, "MIX.wav");

    // One camera failing to start must not cost the others their combined file.
    REQUIRE (plan.jobs.size() == 1);
    REQUIRE (plan.jobs[0].videoFile == "V02_Couch-Cam.mov");
    REQUIRE (plan.problem.empty());
}

TEST_CASE (CombinedTake_EveryCameraFailingIsSaidOutLoud)
{
    std::vector<CombinedTakeInput> cameras = { { "", 0.0 }, { "", 0.0 } };

    const auto plan = buildCombinedTakePlan (CombinedVideoMode::Combined, cameras, "MIX.wav");

    REQUIRE_FALSE (plan.hasWork());
    REQUIRE_FALSE (plan.problem.empty());
}

TEST_CASE (CombinedTake_AudioOnlyTakeSaysSoRatherThanFailingSilently)
{
    const auto plan = buildCombinedTakePlan (CombinedVideoMode::Combined, {}, "MIX.wav");

    REQUIRE_FALSE (plan.hasWork());
    REQUIRE_FALSE (plan.problem.empty());
}

TEST_CASE (CombinedTake_TheAudioIsTrimmedByHowLateTheCameraStarted)
{
    // The sound starts first -- the stem files and the writer thread are opened
    // before any camera is asked to record -- so a camera's file begins some
    // way into the take. Laying them on top of each other without accounting
    // for that puts the picture ahead of the sound by exactly that much.
    const auto plan = buildCombinedTakePlan (CombinedVideoMode::Combined, twoCameras(), "MIX.wav");

    REQUIRE (plan.jobs[0].audioLeadSeconds == 0.25);
    REQUIRE (plan.jobs[1].audioLeadSeconds == 0.40);
}

TEST_CASE (CombinedTake_ANegativeOffsetIsNotTrustedIntoTheCommand)
{
    // A camera cannot start before the audio it is laid against. If a clock
    // reading ever says otherwise, trimming a negative amount would ask the
    // muxer to seek before the start of the file -- which quietly produces
    // something shorter than the take rather than refusing.
    std::vector<CombinedTakeInput> cameras = { { "V01_Kitchen-Cam.mov", -3.0 } };

    const auto plan = buildCombinedTakePlan (CombinedVideoMode::Combined, cameras, "MIX.wav");

    REQUIRE (plan.jobs.size() == 1);
    REQUIRE (plan.jobs[0].audioLeadSeconds == 0.0);
}

TEST_CASE (FfmpegCommand_CopiesThePictureAndEncodesTheSound)
{
    const auto args = buildFfmpegArguments ("/usr/bin/ffmpeg", "/take/V01.mov",
                                            "/take/MIX.wav", "/take/V01_with-sound.mp4", 0.25);

    REQUIRE (args.front() == "/usr/bin/ffmpeg");

    // The picture is the one thing that cannot be recaptured, so it is copied.
    REQUIRE (valueAfter (args, "-c:v") == "copy");
    REQUIRE (valueAfter (args, "-c:a") == "aac");

    // Both streams named explicitly: ffmpeg's default mapping picks one stream
    // per type across all inputs by a rule that is not this one.
    REQUIRE (contains (args, "0:v:0"));
    REQUIRE (contains (args, "1:a:0"));

    REQUIRE (args.back() == "/take/V01_with-sound.mp4");
}

TEST_CASE (FfmpegCommand_SeeksTheInputRatherThanDecodingAndDiscarding)
{
    const auto args = buildFfmpegArguments ("ffmpeg", "/take/V01.mov", "/take/MIX.wav",
                                            "/take/out.mp4", 0.25);

    const int ss = indexOf (args, "-ss");
    REQUIRE (ss > 0);
    REQUIRE (args[static_cast<size_t> (ss) + 1] == "0.250");

    // -ss must come *before* the audio input it applies to. After it, the same
    // trim decodes the whole file to throw the front away -- minutes of work on
    // a four-hour take, for a quarter of a second of audio.
    const int audioInput = indexOf (args, "/take/MIX.wav");
    REQUIRE (audioInput > ss);
}

TEST_CASE (FfmpegCommand_NoTrimWhenThereIsNothingToTrim)
{
    const auto args = buildFfmpegArguments ("ffmpeg", "/v.mov", "/a.wav", "/o.mp4", 0.0);

    // A "-ss 0" is harmless but says the take needed correcting when it did not.
    REQUIRE (indexOf (args, "-ss") == -1);
}

TEST_CASE (FfmpegCommand_NeverWaitsOnStdin)
{
    // ffmpeg asked to overwrite reads from a stdin that is not attached to
    // anything in a GUI app, and hangs there for as long as the app is running.
    const auto args = buildFfmpegArguments ("ffmpeg", "/v.mov", "/a.wav", "/o.mp4", 1.0);

    REQUIRE (contains (args, "-nostdin"));
    REQUIRE (contains (args, "-y"));
}

TEST_CASE (FfmpegCommand_PathsWithSpacesStayOneArgumentEach)
{
    // The whole reason this is a vector and not a command string: a session
    // folder can hold anything a person can type, and through a shell those
    // characters stop being part of a path.
    const std::string video = "/Users/sam/My Takes/Sunday's session/V01 Kitchen.mov";
    const std::string output = "/Users/sam/My Takes/Sunday's session/V01 Kitchen_with-sound.mp4";

    const auto args = buildFfmpegArguments ("ffmpeg", video, "/a.wav", output, 0.0);

    REQUIRE (contains (args, video));
    REQUIRE (args.back() == output);
}

TEST_CASE (FfmpegCommand_SecondsAlwaysUseADotWhateverTheLocale)
{
    // ffmpeg reads "0,250" as 0. It does not complain, so the take would come
    // back looking correct and sounding a quarter-second out -- on the machines
    // of everyone whose locale writes numbers with a comma.
    REQUIRE (formatSecondsForFfmpeg (0.25) == "0.250");
    REQUIRE (formatSecondsForFfmpeg (1.5) == "1.500");
    REQUIRE (formatSecondsForFfmpeg (12.0) == "12.000");

    REQUIRE (formatSecondsForFfmpeg (0.0) == "0.000");
    REQUIRE (formatSecondsForFfmpeg (-1.0) == "0.000");
}
