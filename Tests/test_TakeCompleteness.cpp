#include "TestFramework.h"
#include "../Source/Core/TakeCompleteness.h"

using namespace mma;

TEST_CASE (TakeCompleteness_HeadersOnlyCountsAsNoAudio)
{
    // What a take looks like when the card was pulled before any audio landed:
    // every stem finalized, every one of them a bare WAV header.
    REQUIRE (takeHoldsNoAudio ({ { "MIX.wav", 44 },
                                 { "01_Kitchen.wav", 44 },
                                 { "02_Sofa.wav", 44 },
                                 { "session.json", 900 } }));
}

TEST_CASE (TakeCompleteness_ARealTakeIsNotEmpty)
{
    REQUIRE_FALSE (takeHoldsNoAudio ({ { "MIX.wav", 5'000'000 },
                                       { "01_Kitchen.wav", 5'000'000 },
                                       { "session.json", 900 } }));
}

TEST_CASE (TakeCompleteness_SessionJsonAloneCannotMakeATakeLookRecorded)
{
    // The metadata is a few hundred bytes whatever happened. Counting it would
    // push a folder of empty stems over the threshold and call it saved.
    REQUIRE (takeHoldsNoAudio ({ { "MIX.wav", 44 }, { "session.json", 4000 } }));
}

TEST_CASE (TakeCompleteness_MetadataSuffixIsMatchedRegardlessOfCase)
{
    REQUIRE (takeHoldsNoAudio ({ { "MIX.wav", 44 }, { "SESSION.JSON", 4000 } }));
}

TEST_CASE (TakeCompleteness_AFolderWithNoAudioFilesIsNotJudged)
{
    // Nothing to judge: there are no stems, so "this take is empty" would be
    // reporting on a state this rule was not written for.
    REQUIRE_FALSE (takeHoldsNoAudio ({ { "session.json", 900 } }));
    REQUIRE_FALSE (takeHoldsNoAudio ({}));
}

TEST_CASE (TakeCompleteness_ThresholdIsPerFileNotForTheWholeTake)
{
    // Eight stems holding a header each sum to more than 1KB. Judging the
    // total against a single kilobyte would call that a real recording.
    REQUIRE (takeHoldsNoAudio ({ { "1.wav", 200 }, { "2.wav", 200 }, { "3.wav", 200 },
                                 { "4.wav", 200 }, { "5.wav", 200 }, { "6.wav", 200 },
                                 { "7.wav", 200 }, { "8.wav", 200 } }));

    // And one genuinely-recorded file among them still clears the per-file bar
    // on average, which is the case the panel has always accepted as recorded.
    REQUIRE_FALSE (takeHoldsNoAudio ({ { "1.wav", 44 }, { "2.wav", 5'000'000 } }));
}

TEST_CASE (TakeCompleteness_FullLengthFilesOfPureSilenceAreNotARecording)
{
    // The case the byte-count rule cannot see, and the one a user actually
    // hits: the device was there, the stream ran for the whole take, and every
    // sample of it was zero. A mic muted at its own switch, an interface
    // delivering a dead channel, a board whose USB audio never carried signal.
    //
    // Every file is megabytes. takeHoldsNoAudio looks at size alone and calls
    // that a recording, so the app said "Saved." and handed over silence --
    // which is §0.1's one unacceptable failure, audio lost without a word.
    const std::vector<TakeFile> fullLength { { "MIX.wav", 5'000'000 },
                                             { "01_Kitchen.wav", 5'000'000 },
                                             { "session.json", 900 } };

    // The rule this replaces, on the same folder: it sees megabytes and calls
    // it recorded. That is the blind spot, kept here so it cannot come back.
    REQUIRE_FALSE (takeHoldsNoAudio (fullLength));

    REQUIRE (judgeTakeAudio (fullLength, 0.0f) == TakeAudioVerdict::OnlySilence);

    // Signal anywhere in the take clears it, however quiet.
    REQUIRE (judgeTakeAudio (fullLength, 0.01f) == TakeAudioVerdict::Recorded);
}

TEST_CASE (TakeCompleteness_AQuietTakeIsStillARecording)
{
    // A whisper at the far end of a room is not silence, and calling it silence
    // would put a warning on a take that is perfectly good. The bar is digital
    // silence -- below -90 dBFS, which no microphone path reaches.
    const std::vector<TakeFile> files { { "MIX.wav", 5'000'000 } };

    REQUIRE (judgeTakeAudio (files, 0.001f) == TakeAudioVerdict::Recorded);   // -60 dBFS
    REQUIRE (judgeTakeAudio (files, 0.0001f) == TakeAudioVerdict::Recorded);  // -80 dBFS
}

TEST_CASE (TakeCompleteness_HeaderOnlyFilesReportNothingWrittenNotSilence)
{
    // Two different failures with two different causes, so they must not share
    // a message: nothing arrived at all, versus a stream that ran and was flat.
    REQUIRE (judgeTakeAudio ({ { "MIX.wav", 44 }, { "01.wav", 44 } }, 0.0f)
             == TakeAudioVerdict::NothingWritten);
}

TEST_CASE (TakeCompleteness_AnUnmeasuredPeakNeverReportsSilence)
{
    // A negative peak means nobody measured one. Reporting silence on the
    // strength of a measurement that was never taken would be a guess.
    REQUIRE (judgeTakeAudio ({ { "MIX.wav", 5'000'000 } }, -1.0f)
             == TakeAudioVerdict::Recorded);
}
