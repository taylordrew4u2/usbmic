#include "TakeCombiner.h"
#include "../Core/FfmpegCommand.h"
#include "../Core/FfmpegLocator.h"

namespace mma {

TakeCombiner::TakeCombiner() = default;

TakeCombiner::~TakeCombiner()
{
    waitForCompletion();
}

juce::String TakeCombiner::findFfmpeg()
{
    if (ffmpegOverride.isNotEmpty())
        return ffmpegOverride;

    if (haveResolved)
        return resolvedFfmpeg;

    haveResolved = true;
    resolvedFfmpeg = {};

    for (const auto& candidate : ffmpegSearchPaths (thisHostPlatform()))
    {
        const juce::String path (candidate);

        // A bare name is for PATH to answer, and the only honest way to ask is
        // to run it. Anything with a separator in it is a real location and can
        // be checked without spawning anything.
        if (path.contains ("/") || path.contains ("\\"))
        {
            const juce::File file (path);

            if (file.existsAsFile())
            {
                resolvedFfmpeg = path;
                return resolvedFfmpeg;
            }

            continue;
        }

        juce::ChildProcess probe;

        if (probe.start (juce::StringArray { path, "-version" })
            && probe.waitForProcessToFinish (4000)
            && probe.getExitCode() == 0)
        {
            resolvedFfmpeg = path;
            return resolvedFfmpeg;
        }
    }

    return resolvedFfmpeg;
}

bool TakeCombiner::start (const juce::File& sessionFolder, const CombinedTakePlan& plan)
{
    if (running.load() || ! plan.hasWork())
        return false;

    const auto ffmpeg = findFfmpeg();

    {
        const std::lock_guard<std::mutex> lock (statusLock);
        status = {};
        status.total = static_cast<int> (plan.jobs.size());
    }

    if (ffmpeg.isEmpty())
    {
        // §10.6: name what happened and what to do about it. Nothing has been
        // lost -- the picture and the sound are both on disk, complete -- so
        // this says that too, or the sentence reads like a failed recording.
        const std::lock_guard<std::mutex> lock (statusLock);
        status.problem = "Couldn't find ffmpeg, so the combined video wasn't made. "
                         "Your picture and sound are both saved as separate files. "
                         "Install ffmpeg (on a Mac: brew install ffmpeg) and the next "
                         "take will combine them.";
        return false;
    }

    waitForCompletion();

    running.store (true);
    cancelling.store (false);

    worker = std::make_unique<std::thread> ([this, sessionFolder, plan, ffmpeg]
                                            { run (sessionFolder, plan, ffmpeg); });
    return true;
}

void TakeCombiner::run (juce::File sessionFolder, CombinedTakePlan plan, juce::String ffmpeg)
{
    {
        const std::lock_guard<std::mutex> lock (statusLock);
        status.running = true;
    }

    int failures = 0;

    for (const auto& job : plan.jobs)
    {
        if (cancelling.load())
            break;

        const auto video = sessionFolder.getChildFile (juce::String (job.videoFile));
        const auto audio = sessionFolder.getChildFile (juce::String (job.audioFile));
        const auto output = sessionFolder.getChildFile (juce::String (job.outputFile));

        // Checked here rather than in the plan, because the plan is built from
        // what the take intended to write and this runs against what is
        // actually on the card -- which a pulled card or a full disk can make
        // two different things.
        if (! video.existsAsFile() || ! audio.existsAsFile())
        {
            ++failures;
            continue;
        }

        const auto args = buildFfmpegArguments (ffmpeg.toStdString(),
                                                video.getFullPathName().toStdString(),
                                                audio.getFullPathName().toStdString(),
                                                output.getFullPathName().toStdString(),
                                                job.audioLeadSeconds,
                                                job.audioBitDepth);

        juce::StringArray argv;
        for (const auto& arg : args)
            argv.add (juce::String (arg));

        juce::ChildProcess process;
        bool ok = process.start (argv, juce::ChildProcess::wantStdErr);

        if (ok)
        {
            // Read the output as it comes rather than after: a pipe nobody
            // drains fills, and ffmpeg then blocks writing to it forever, which
            // presents as a combine that never finishes.
            juce::String errorText = process.readAllProcessOutput();
            ok = process.waitForProcessToFinish (-1) && process.getExitCode() == 0;
            juce::ignoreUnused (errorText);
        }

        // A file left behind by a run that failed halfway is worse than no file
        // at all: it plays, briefly, and looks like the take.
        if (! ok)
        {
            output.deleteFile();
            ++failures;
        }

        const std::lock_guard<std::mutex> lock (statusLock);
        ++status.done;

        if (ok)
            status.written.add (output.getFileName());
    }

    {
        const std::lock_guard<std::mutex> lock (statusLock);
        status.running = false;

        if (failures > 0)
            status.problem = juce::String (failures)
                           + (failures == 1 ? " camera couldn't be combined with the sound. "
                                            : " cameras couldn't be combined with the sound. ")
                           + "The separate picture and sound files are all still there.";
    }

    running.store (false);
}

void TakeCombiner::waitForCompletion()
{
    if (worker != nullptr)
    {
        if (worker->joinable())
            worker->join();

        worker.reset();
    }
}

TakeCombiner::Status TakeCombiner::getStatus() const
{
    const std::lock_guard<std::mutex> lock (statusLock);
    return status;
}

} // namespace mma
