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
