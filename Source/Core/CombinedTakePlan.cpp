#include "CombinedTakePlan.h"
#include <algorithm>

namespace mma {

namespace {

std::string extensionOf (const std::string& fileName)
{
    const auto dot = fileName.find_last_of ('.');

    if (dot == std::string::npos)
        return {};

    std::string ext = fileName.substr (dot);
    std::transform (ext.begin(), ext.end(), ext.begin(),
                    [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return ext;
}

std::string stemOf (const std::string& fileName)
{
    const auto dot = fileName.find_last_of ('.');
    return dot == std::string::npos ? fileName : fileName.substr (0, dot);
}

} // namespace

std::string combinedFileNameFor (const std::string& videoFileName)
{
    if (videoFileName.empty())
        return {};

    // The picture is copied, never re-encoded, so the container has to be one
    // that can legally carry whatever codec the camera chose. mp4 cannot hold
    // VC-1, which is what Windows writes; Matroska can hold anything.
    const auto container = extensionOf (videoFileName) == ".mov" ? ".mp4" : ".mkv";

    return stemOf (videoFileName) + "_with-sound" + container;
}

CombinedTakePlan buildCombinedTakePlan (CombinedVideoMode mode,
                                        const std::vector<CombinedTakeInput>& cameras,
                                        const std::string& mixFileName)
{
    CombinedTakePlan plan;

    if (mode == CombinedVideoMode::Off)
        return plan; // not asked for; not a problem

    if (mixFileName.empty())
    {
        plan.problem = "The take has no mix file, so there is no sound to put with the picture.";
        return plan;
    }

    if (cameras.empty())
    {
        plan.problem = "No cameras were recording, so there is only sound to save.";
        return plan;
    }

    for (const auto& camera : cameras)
    {
        // A camera that never started wrote no file. Skipping it is the whole
        // of the handling: one camera failing must not cost the others theirs.
        if (camera.videoFile.empty())
            continue;

        CombinedTakeJob job;
        job.videoFile = camera.videoFile;
        job.audioFile = mixFileName;
        job.outputFile = combinedFileNameFor (camera.videoFile);

        // Never negative. A camera cannot start before the audio it is being
        // laid against -- the stems are opened first -- and a negative lead
        // would ask the muxer to trim time off the front of a file that does
        // not have it, which silently produces a file shorter than the take.
        job.audioLeadSeconds = std::max (0.0, camera.videoStartOffsetSeconds);

        plan.jobs.push_back (std::move (job));
    }

    if (plan.jobs.empty())
        plan.problem = "None of the cameras wrote a file, so there is nothing to combine.";

    return plan;
}

} // namespace mma
