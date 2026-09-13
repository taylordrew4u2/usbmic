#include "App/TakeCombiner.h"
#include <chrono>
#include <cstring>
#include <cstdio>
#include <memory>
#include <thread>

namespace {

int fail (const char* message)
{
    std::fprintf (stderr, "FAIL  %s\n", message);
    return 1;
}

} // namespace

int main (int argc, char** argv)
{
    // TakeCombiner invokes this executable as its stand-in for ffmpeg. A real
    // wedged muxer never exits; the parent must still be able to destroy the
    // combiner immediately and its detached worker must kill this child.
    if (argc > 1)
    {
        for (int index = 1; index < argc; ++index)
        {
            if (std::strstr (argv[index], "success-input") != nullptr)
            {
                if (auto* complete = std::fopen (argv[argc - 1], "wb"))
                {
                    std::fputs ("complete", complete);
                    std::fclose (complete);
                    return 0;
                }

                return 8;
            }

            if (std::strstr (argv[index], "fail-input") != nullptr)
            {
                if (auto* partial = std::fopen (argv[argc - 1], "wb"))
                {
                    std::fputs ("partial", partial);
                    std::fclose (partial);
                }

                return 7;
            }
        }

        for (;;)
            std::this_thread::sleep_for (std::chrono::seconds (1));
    }

    // argv[0] may be relative when this probe is launched directly. Resolve it
    // against the current directory instead of tripping JUCE's absolute-path
    // assertion in a successful test run.
    const auto executable = juce::File::getCurrentWorkingDirectory()
                                .getChildFile (juce::String::fromUTF8 (argv[0]))
                                .getFullPathName();
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getNonexistentChildFile ("sobstage-combiner-probe", {}, false);

    if (! root.createDirectory().wasOk())
        return fail ("could not create the temporary take folder");

    struct Cleanup
    {
        juce::File root;
        ~Cleanup() { root.deleteRecursively(); }
    } cleanup { root };

    const auto video = root.getChildFile ("V01_Camera.mov");
    const auto audio = root.getChildFile ("MIX.wav");

    if (! video.replaceWithText ("video") || ! audio.replaceWithText ("audio"))
        return fail ("could not make the fake take inputs");

    mma::CombinedTakePlan plan;
    plan.jobs.push_back ({ "V01_Camera.mov", "MIX.wav",
                           "V01_Camera_with-sound.mov", 24, 0.0 });

    auto combiner = std::make_unique<mma::TakeCombiner>();
    combiner->setFfmpegOverride (executable);

    if (! combiner->start (root, plan) || ! combiner->isRunning())
        return fail ("the simulated combine did not start");

    // Give the worker enough time to enter its infinite child. This is not a
    // timing assertion; only destruction below is timed.
    std::this_thread::sleep_for (std::chrono::milliseconds (250));

    const auto before = std::chrono::steady_clock::now();
    combiner.reset();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - before);

    if (elapsed > std::chrono::milliseconds (100))
        return fail ("destroying the combiner waited for the wedged child");

    // The detached owner gets a bounded poll in which to observe cancellation,
    // kill the child and discard any incomplete output.
    std::this_thread::sleep_for (std::chrono::milliseconds (350));

    if (root.getChildFile ("V01_Camera_with-sound.mov").exists())
        return fail ("a cancelled combine left an output file behind");

    const auto failingVideo = root.getChildFile ("fail-input.mov");
    if (! failingVideo.replaceWithText ("video"))
        return fail ("could not make the failing fake input");

    mma::CombinedTakePlan failingPlan;
    failingPlan.jobs.push_back ({ "fail-input.mov", "MIX.wav",
                                  "failed-output.mov", 24, 0.0 });

    mma::TakeCombiner failingCombiner;
    failingCombiner.setFfmpegOverride (executable);

    if (! failingCombiner.start (root, failingPlan))
        return fail ("the failing simulated combine did not start");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (2);
    while (failingCombiner.isRunning() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));

    if (failingCombiner.isRunning())
        return fail ("the failing simulated combine did not finish");

    const auto failedStatus = failingCombiner.getStatus();
    if (failedStatus.problem.isEmpty())
        return fail ("a non-zero ffmpeg exit was reported as success");

    if (! failedStatus.written.isEmpty())
        return fail ("a non-zero ffmpeg exit was added to written outputs");

    if (root.getChildFile ("failed-output.mov").exists())
        return fail ("a non-zero ffmpeg exit left its partial output behind");

    const auto successfulVideo = root.getChildFile ("success-input.mov");
    if (! successfulVideo.replaceWithText ("video"))
        return fail ("could not make the successful fake input");

    mma::CombinedTakePlan successfulPlan;
    successfulPlan.jobs.push_back ({ "success-input.mov", "MIX.wav",
                                     "successful-output.mov", 24, 0.0 });

    mma::TakeCombiner successfulCombiner;
    successfulCombiner.setFfmpegOverride (executable);

    if (! successfulCombiner.start (root, successfulPlan))
        return fail ("the successful simulated combine did not start");

    const auto successDeadline = std::chrono::steady_clock::now() + std::chrono::seconds (2);
    while (successfulCombiner.isRunning() && std::chrono::steady_clock::now() < successDeadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));

    if (successfulCombiner.isRunning())
        return fail ("the successful simulated combine did not finish");

    const auto successfulStatus = successfulCombiner.getStatus();
    if (successfulStatus.problem.isNotEmpty())
        return fail ("a zero ffmpeg exit was reported as a failure");

    if (! successfulStatus.written.contains ("successful-output.mov"))
        return fail ("a zero ffmpeg exit was not added to written outputs");

    if (! root.getChildFile ("successful-output.mov").existsAsFile())
        return fail ("a zero ffmpeg exit lost its completed output");

    std::printf ("ALL CHECKS PASSED (11 checks, 0 failing)\n");
    return 0;
}
