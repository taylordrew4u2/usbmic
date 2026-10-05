#include "PodcastExport.h"
#include "../Core/CombinedTakePlan.h"
#include "../Core/LoudnessMeter.h"
#include "../Core/SessionFolderNaming.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <chrono>
#include <thread>

namespace mma {

namespace {

/// Samples read and written at a time. Large enough that a four-hour take is
/// not millions of calls, small enough that a cancel is noticed promptly.
constexpr int kBlockSamples = 1 << 16;

juce::String wholeLufs (double lufs)
{
    return juce::String (juce::roundToInt (lufs)) + " LUFS";
}

/// "MIX" for "MIX.wav".
juce::String stemOf (const juce::String& fileName)
{
    return fileName.containsChar ('.') ? fileName.upToLastOccurrenceOf (".", false, false)
                                       : fileName;
}

PodcastExportResult skipped (const juce::String& why)
{
    PodcastExportResult result;
    result.message = why;
    return result;
}

} // namespace

juce::String podcastCopyFileName (const juce::String& mixFileName, const std::string& targetName)
{
    return stemOf (mixFileName) + " - " + juce::String (SessionFolderNaming::sanitizeName (targetName)) + ".wav";
}

PodcastExportResult exportPodcastCopy (const juce::File& sessionFolder,
                                       const juce::String& mixFileName,
                                       const StreamingTarget& target,
                                       const std::atomic<bool>& cancel)
{
    const juce::String noCopy ("No podcast-ready copy was made: ");

    // The mix as it is on the card: the first file, then every split part that
    // follows it -- the same list the combined video is laid against.
    std::vector<std::string> namesInFolder;
    for (const auto& file : sessionFolder.findChildFiles (juce::File::findFiles, false))
        namesInFolder.push_back (file.getFileName().toStdString());

    std::vector<juce::File> parts;
    for (const auto& part : splitPartsInOrder (mixFileName.toStdString(), namesInFolder))
        parts.push_back (sessionFolder.getChildFile (juce::String::fromUTF8 (part.c_str())));

    if (parts.empty())
        return skipped (noCopy + mixFileName + " wasn't in " + sessionFolder.getFileName() + ".");

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    auto openPart = [&formats] (const juce::File& file)
    {
        return std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (file));
    };

    double rate = 0.0;
    int channels = 0;

    for (const auto& part : parts)
    {
        auto reader = openPart (part);

        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->numChannels < 1)
            return skipped (noCopy + part.getFileName() + " couldn't be read.");

        if (rate == 0.0)
        {
            rate = reader->sampleRate;
            channels = static_cast<int> (reader->numChannels);
        }
        else if (reader->sampleRate != rate || static_cast<int> (reader->numChannels) != channels)
        {
            return skipped (noCopy + "the parts of " + mixFileName + " don't match each other.");
        }
    }

    juce::AudioBuffer<float> buffer (channels, kBlockSamples);

    // Pass one: measure. The mix is mono, so the first channel is the mix; a
    // file with more is measured on that one and has the same gain applied to
    // all of them.
    LoudnessMeter meter (rate);

    for (const auto& part : parts)
    {
        auto reader = openPart (part);
        if (reader == nullptr)
            return skipped (noCopy + part.getFileName() + " couldn't be read.");

        for (juce::int64 pos = 0; pos < reader->lengthInSamples; pos += kBlockSamples)
        {
            if (cancel.load (std::memory_order_acquire))
                return skipped (noCopy + "the app was closing.");

            const auto count = static_cast<int> (juce::jmin<juce::int64> (kBlockSamples,
                                                                          reader->lengthInSamples - pos));
            if (! reader->read (&buffer, 0, count, pos, true, true))
                return skipped (noCopy + part.getFileName() + " couldn't be read to the end.");

            meter.process (buffer.getReadPointer (0), static_cast<size_t> (count));
        }
    }

    const double lufs = meter.getIntegratedLufs();
    const double truePeak = meter.getTruePeakDbtp();
    const int blocks = meter.getBlockCount();

    if (blocks < kMinimumBlocksToJudge)
        return skipped (noCopy + "the take is too short to measure how loud it is.");

    if (lufs <= LoudnessMeter::kAbsoluteGateLufs)
        return skipped (noCopy + "the mix is silent, so there's nothing to set the loudness of.");

    const auto advice = adviseForTarget (target, lufs, truePeak, blocks);

    // adviseForTarget's own test for the two cases above. Both have been
    // answered already; this is here so a change to its rules cannot turn into
    // a copy written from a figure it would not stand behind.
    if (! advice.measurable)
        return skipped (noCopy + juce::String (advice.summary));

    // Never over anything: a copy from an earlier export, or a file someone
    // put there, keeps its name and this one takes the next free one.
    const auto wanted = podcastCopyFileName (mixFileName, target.name);
    const auto stem = stemOf (wanted).toStdString();
    const auto freeStem = SessionFolderNaming::resolveCollision (stem, [&] (const std::string& candidate)
    {
        return sessionFolder.getChildFile (juce::String::fromUTF8 (candidate.c_str()) + ".wav").exists();
    });

    const auto output = sessionFolder.getChildFile (juce::String::fromUTF8 (freeStem.c_str()) + ".wav");

    // Not .wav until it is finished, so nothing that lists the folder's audio
    // -- this app's own saved-take card included -- can count it before then.
    const auto working = output.getSiblingFile (output.getFileName() + ".part");
    working.deleteFile();

    const float gain = juce::Decibels::decibelsToGain (static_cast<float> (advice.gainDb), -1000.0f);

    {
        auto stream = std::make_unique<juce::FileOutputStream> (working);

        if (stream->failedToOpen())
            return skipped (noCopy + "couldn't write to " + sessionFolder.getFileName() + ".");

        // Ownership passes to the writer only when it is made; otherwise the
        // stream is still this function's to close.
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (stream.get(), rate, static_cast<unsigned int> (channels), 24, {}, 0));

        if (writer == nullptr)
        {
            stream.reset();
            working.deleteFile();
            return skipped (noCopy + "couldn't start a WAV file in " + sessionFolder.getFileName() + ".");
        }

        stream.release();

        auto abandon = [&] (const juce::String& why)
        {
            writer.reset();
            working.deleteFile();
            return skipped (noCopy + why);
        };

        // Pass two: the same samples again, with the gain.
        for (const auto& part : parts)
        {
            auto reader = openPart (part);
            if (reader == nullptr)
                return abandon (part.getFileName() + " couldn't be read.");

            for (juce::int64 pos = 0; pos < reader->lengthInSamples; pos += kBlockSamples)
            {
                if (cancel.load (std::memory_order_acquire))
                    return abandon ("the app was closing.");

                const auto count = static_cast<int> (juce::jmin<juce::int64> (kBlockSamples,
                                                                              reader->lengthInSamples - pos));
                if (! reader->read (&buffer, 0, count, pos, true, true))
                    return abandon (part.getFileName() + " couldn't be read to the end.");

                buffer.applyGain (0, count, gain);

                if (! writer->writeFromAudioSampleBuffer (buffer, 0, count))
                    return abandon ("the card ran out of room or stopped answering while it was being written.");
            }
        }

        // Closing is what writes the header's final sizes; a writer that
        // cannot flush leaves a file that only looks finished.
        if (! writer->flush())
            return abandon ("the card ran out of room or stopped answering while it was being written.");
    }

    if (! working.moveFileTo (output))
    {
        working.deleteFile();
        return skipped (noCopy + "couldn't name the finished copy " + output.getFileName() + ".");
    }

    PodcastExportResult result;
    result.written = true;
    result.output = output;
    result.measuredLufs = lufs;
    result.measuredTruePeakDbtp = truePeak;
    result.gainDb = advice.gainDb;
    result.limitedByTruePeak = advice.limitedByTruePeak;
    result.resultingLufs = lufs + advice.gainDb;

    result.message = "Podcast-ready copy saved: " + output.getFileName()
                   + " (" + wholeLufs (result.resultingLufs) + ")";

    // Short of the target is said, not left for someone to find on a meter:
    // the copy is as loud as it can be without clipping, which is not as loud
    // as the platform asked.
    if (advice.limitedByTruePeak && result.resultingLufs < monoTargetLufs (target) - kOnTargetToleranceLu)
        result.message += ", quieter than " + juce::String (target.name) + " wants ("
                        + wholeLufs (monoTargetLufs (target)) + ") because the loudest peaks "
                          "would otherwise clip.";
    else
        result.message += ".";

    return result;
}

PodcastExporter::PodcastExporter()
    : state (std::make_shared<State>())
{
}

PodcastExporter::~PodcastExporter()
{
    cancel();
    waitUntilIdle (kShutdownWaitMs);
}

bool PodcastExporter::start (const juce::File& sessionFolder, const juce::String& mixFileName,
                             const std::string& targetName)
{
    const auto* target = findStreamingTarget (targetName);
    if (target == nullptr)
        return false;

    bool startWorker = false;

    {
        const std::lock_guard<std::mutex> lock (state->lock);

        // Quitting: nothing more may start, queued or otherwise.
        if (state->cancelling.load (std::memory_order_acquire))
            return false;

        state->queued.push_back ({ sessionFolder, mixFileName, *target });

        if (! state->running)
        {
            state->running = true;
            startWorker = true;
        }
    }

    if (! startWorker)
        return true;

    // Detached, holding its own share of the state: the worker may be stuck
    // reading a card that went away, and nothing here may wait on that.
    try
    {
        std::thread ([s = state] { run (s); }).detach();
    }
    catch (...)
    {
        const std::lock_guard<std::mutex> lock (state->lock);
        state->running = false;
        state->queued.clear();
        state->finished.push_back ({});
        state->finished.back().message = "No podcast-ready copy was made: the app couldn't start "
                                         "the work. The take itself is saved.";
        state->idle.notify_all();
    }

    return true;
}

void PodcastExporter::run (std::shared_ptr<State> state)
{
    for (;;)
    {
        Job job;

        {
            const std::lock_guard<std::mutex> lock (state->lock);

            // Decided under the same lock start() queues under, so a take is
            // either picked up by this worker or starts its own. Never neither.
            if (state->queued.empty() || state->cancelling.load (std::memory_order_acquire))
            {
                state->queued.clear();
                state->running = false;
                state->idle.notify_all();
                return;
            }

            job = std::move (state->queued.front());
            state->queued.pop_front();
        }

        auto result = exportPodcastCopy (job.sessionFolder, job.mixFileName, job.target,
                                         state->cancelling);

        const std::lock_guard<std::mutex> lock (state->lock);
        state->finished.push_back (std::move (result));
    }
}

std::vector<PodcastExportResult> PodcastExporter::collectFinished()
{
    const std::lock_guard<std::mutex> lock (state->lock);
    std::vector<PodcastExportResult> out;
    out.swap (state->finished);
    return out;
}

bool PodcastExporter::isRunning() const
{
    const std::lock_guard<std::mutex> lock (state->lock);
    return state->running;
}

void PodcastExporter::cancel() noexcept
{
    state->cancelling.store (true, std::memory_order_release);
}

bool PodcastExporter::waitUntilIdle (int timeoutMs)
{
    std::unique_lock<std::mutex> lock (state->lock);
    return state->idle.wait_for (lock, std::chrono::milliseconds (juce::jmax (0, timeoutMs)),
                                 [this] { return ! state->running; });
}

} // namespace mma
