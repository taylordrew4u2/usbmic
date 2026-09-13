#include "TakeCombiner.h"
#include "../Core/FfmpegCommand.h"
#include "../Core/FfmpegLocator.h"

namespace mma {

TakeCombiner::TakeCombiner()
    : runState (std::make_shared<RunState>())
{
}

TakeCombiner::~TakeCombiner()
{
    cancel();
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
    if (isRunning() || ! plan.hasWork())
        return false;

    const auto ffmpeg = findFfmpeg();
    auto next = std::make_shared<RunState>();

    {
        const std::lock_guard<std::mutex> lock (next->statusLock);
        next->status.total = static_cast<int> (plan.jobs.size());
    }

    if (ffmpeg.isEmpty())
    {
        // §10.6: name what happened and what to do about it. Nothing has been
        // lost -- the picture and the sound are both on disk, complete -- so
        // this says that too, or the sentence reads like a failed recording.
        const std::lock_guard<std::mutex> lock (next->statusLock);
        next->status.problem = "Couldn't find ffmpeg, so the combined video wasn't made. "
                               "Your picture and sound are both saved as separate files. "
                               "Install ffmpeg (on a Mac: brew install ffmpeg) and the next "
                               "take will combine them.";
        runState = std::move (next);
        return false;
    }

    next->running.store (true, std::memory_order_release);
    {
        const std::lock_guard<std::mutex> lock (next->statusLock);
        next->status.running = true;
    }
    runState = next;

    // A removable volume or ffmpeg itself may never answer. The worker owns
    // every path and all status it can touch, so quitting merely requests
    // cancellation and never joins it. A late worker cannot refer back to a
    // destroyed TakeCombiner or overwrite a later run's status.
    try
    {
        std::thread ([next, sessionFolder, plan, ffmpeg]
        {
            TakeCombiner::run (next, sessionFolder, plan, ffmpeg);
        }).detach();
    }
    catch (...)
    {
        next->running.store (false, std::memory_order_release);
        const std::lock_guard<std::mutex> lock (next->statusLock);
        next->status.running = false;
        next->status.problem = "Couldn't start the combined-video worker. Your separate picture "
                               "and sound files are still saved.";
        return false;
    }

    return true;
}

void TakeCombiner::run (std::shared_ptr<RunState> state,
                        juce::File sessionFolder,
                        CombinedTakePlan plan,
                        juce::String ffmpeg)
{
    int failures = 0;

    // Kept from the first failure only. Two cameras failing for the same reason
    // say it once; two failing for different reasons are still one sentence,
    // and the first is the one that stopped the run being clean.
    juce::String firstFailureDetail;

    for (const auto& job : plan.jobs)
    {
        if (state->cancelling.load (std::memory_order_acquire))
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

            if (firstFailureDetail.isEmpty())
                firstFailureDetail = juce::String (job.videoFile)
                                   + " or its sound wasn't on the card to combine.";

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

        // No captured output pipe: POSIX stdio reads can block forever while
        // a wedged ffmpeg still owns the pipe. With its streams discarded we
        // can poll the child in bounded steps and honour shutdown promptly.
        bool ok = process.start (argv, 0);

        if (! ok && firstFailureDetail.isEmpty())
            firstFailureDetail = "ffmpeg wouldn't start.";

        if (ok)
        {
            bool finished = false;

            while (! state->cancelling.load (std::memory_order_acquire))
            {
                if (process.waitForProcessToFinish (100))
                {
                    finished = true;
                    break;
                }
            }

            if (state->cancelling.load (std::memory_order_acquire))
            {
                if (process.isRunning())
                    (void) process.kill();

                (void) process.waitForProcessToFinish (1000);
                output.deleteFile();
                break;
            }

            // waitForProcessToFinish's successful terminal poll is also the
            // POSIX reap. Calling isRunning() after it can ask waitpid about an
            // already-reaped child and make JUCE replace the real status.
            ok = finished && process.getExitCode() == 0;

            if (! ok && firstFailureDetail.isEmpty())
                firstFailureDetail = "ffmpeg stopped before it made a complete file.";
        }

        // A file left behind by a run that failed halfway is worse than no file
        // at all: it plays, briefly, and looks like the take.
        if (! ok)
        {
            output.deleteFile();
            ++failures;
        }

        const std::lock_guard<std::mutex> lock (state->statusLock);
        ++state->status.done;

        if (ok)
            state->status.written.add (output.getFileName());
    }

    {
        const std::lock_guard<std::mutex> lock (state->statusLock);
        state->status.running = false;

        if (failures > 0)
        {
            state->status.problem = juce::String (failures)
                                  + (failures == 1 ? " camera couldn't be combined with the sound. "
                                                   : " cameras couldn't be combined with the sound. ")
                                  + "The separate picture and sound files are all still there.";

            if (firstFailureDetail.isNotEmpty())
                state->status.problem += " (" + firstFailureDetail + ")";
        }
    }

    state->running.store (false, std::memory_order_release);
}

void TakeCombiner::cancel() noexcept
{
    if (runState != nullptr)
        runState->cancelling.store (true, std::memory_order_release);
}

bool TakeCombiner::isRunning() const
{
    return runState != nullptr
        && runState->running.load (std::memory_order_acquire);
}

TakeCombiner::Status TakeCombiner::getStatus() const
{
    const auto state = runState;

    if (state == nullptr)
        return {};

    const std::lock_guard<std::mutex> lock (state->statusLock);
    return state->status;
}

} // namespace mma
