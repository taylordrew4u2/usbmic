#include "TakeCompleteness.h"

#include <algorithm>
#include <cctype>

namespace mma {

namespace {

bool isMetadata (const std::string& name)
{
    // Case-insensitive suffix test: the writer produces "session.json", but a
    // folder is a folder and nothing stops one holding "SESSION.JSON".
    const std::string suffix = ".json";

    if (name.size() < suffix.size())
        return false;

    return std::equal (suffix.rbegin(), suffix.rend(), name.rbegin(),
                       [] (char a, char b)
                       {
                           return std::tolower (static_cast<unsigned char> (a))
                                  == std::tolower (static_cast<unsigned char> (b));
                       });
}

} // namespace

namespace {

/// -90 dBFS. Chosen well below any real microphone path's noise floor: a 24-bit
/// LSB sits near -138 dBFS and preamp noise in a quiet room is nowhere near
/// this low, so nothing that actually recorded can fall under it.
constexpr float kSilenceFloor = 0.0000316f;

} // namespace

TakeAudioVerdict judgeTakeAudio (const std::vector<TakeFile>& files,
                                 float peakWritten, float peakArrived)
{
    const auto base = judgeTakeAudio (files, peakWritten);

    // Audio came in and none of it reached the files. Nothing about the user's
    // hardware explains that -- the samples were here.
    if ((base == TakeAudioVerdict::OnlySilence || base == TakeAudioVerdict::NothingWritten)
        && peakArrived >= kSilenceFloor)
        return TakeAudioVerdict::DroppedByApp;

    return base;
}

TakeAudioVerdict judgeTakeAudio (const std::vector<TakeFile>& files, float peakAbs)
{
    int64_t audioBytes = 0;
    int audioFiles = 0;

    for (const auto& f : files)
    {
        if (isMetadata (f.name))
            continue;

        audioBytes += f.sizeBytes;
        ++audioFiles;
    }

    if (audioFiles == 0)
        return TakeAudioVerdict::NotJudged;

    // Nothing arrived at all: the files are headers. Reported separately from
    // silence because the causes are different and so is what the user should
    // go and check.
    if (audioBytes < static_cast<int64_t> (audioFiles) * 1024)
        return TakeAudioVerdict::NothingWritten;

    // A negative peak means no measurement was taken. Never report silence on
    // the strength of one that was not made.
    if (peakAbs >= 0.0f && peakAbs < kSilenceFloor)
        return TakeAudioVerdict::OnlySilence;

    return TakeAudioVerdict::Recorded;
}

bool takeHoldsNoAudio (const std::vector<TakeFile>& files)
{
    int64_t audioBytes = 0;
    int audioFiles = 0;

    for (const auto& f : files)
    {
        if (isMetadata (f.name))
            continue;

        audioBytes += f.sizeBytes;
        ++audioFiles;
    }

    // No audio files at all is not "an empty take" -- there is nothing to
    // judge, and saying a take is empty when the folder holds only metadata
    // would misreport a state this rule was not written for.
    return audioFiles > 0 && audioBytes < static_cast<int64_t> (audioFiles) * 1024;
}

} // namespace mma
