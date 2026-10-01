#include "TestFramework.h"
#include "Core/PreflightScratchFile.h"

using namespace mma;

TEST_CASE (PreflightScratchFile_RecognisesEveryNameTheCheckHasEverLeftBehind)
{
    REQUIRE (PreflightScratchFile::isLeftover (".sobstage-preflight.tmp"));
    REQUIRE (PreflightScratchFile::isLeftover ("preflight.tmp"));
    REQUIRE (PreflightScratchFile::isLeftover ("preflight (2).tmp"));
    REQUIRE (PreflightScratchFile::isLeftover ("preflight (17).tmp"));
}

TEST_CASE (PreflightScratchFile_NeverTouchesAnythingElse)
{
    REQUIRE_FALSE (PreflightScratchFile::isLeftover ("MIX.wav"));
    REQUIRE_FALSE (PreflightScratchFile::isLeftover ("preflight notes.tmp"));
    REQUIRE_FALSE (PreflightScratchFile::isLeftover ("preflight ().tmp"));
    REQUIRE_FALSE (PreflightScratchFile::isLeftover ("preflight (2a).tmp"));
    REQUIRE_FALSE (PreflightScratchFile::isLeftover ("my preflight.tmp"));
    REQUIRE_FALSE (PreflightScratchFile::isLeftover ("preflight.tmp.wav"));
}
