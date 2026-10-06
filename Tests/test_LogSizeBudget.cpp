#include "TestFramework.h"
#include "Core/LogSizeBudget.h"

using namespace mma;

TEST_CASE (LogSizeBudget_NoTrimUntilTheCapIsPassed)
{
    LogSizeBudget budget (1000, 900);
    REQUIRE_FALSE (budget.noteWritten (100)); // exactly at the cap
    REQUIRE (budget.noteWritten (1));         // past it
}

TEST_CASE (LogSizeBudget_ALongSessionKeepsTrimmingBackUnderTheCap)
{
    // The bug: the cap held only at launch. Days of lines must keep coming
    // back under it, and a trim must buy a run of lines, not happen per line.
    const std::int64_t cap = 1000;
    LogSizeBudget budget (cap, 0);
    std::int64_t file = 0;
    int trims = 0;

    for (int line = 0; line < 10000; ++line)
    {
        file += 20;
        if (budget.noteWritten (20))
        {
            ++trims;
            file = budget.trimTarget(); // what trimFileSize leaves
            budget.resync (file);
        }

        REQUIRE (file <= cap);
    }

    // 200 000 bytes written, a quarter-cap (250) freed per trim.
    REQUIRE (trims >= 700);
    REQUIRE (trims <= 900);
}

TEST_CASE (LogSizeBudget_AFailedTrimIsNotRetriedOnEveryLine)
{
    LogSizeBudget budget (1000, 0);
    REQUIRE (budget.noteWritten (1001));

    // The trim could not shrink it.
    budget.resync (1001);
    REQUIRE_FALSE (budget.noteWritten (10));
    REQUIRE_FALSE (budget.noteWritten (200));
    REQUIRE (budget.noteWritten (100)); // another quarter-cap later
}

TEST_CASE (LogSizeBudget_AnOversizedFileAtStartIsTrimmedOnTheNextLine)
{
    // FileLogger trims at construction, but if that failed the file starts
    // over the cap; the budget still gets it back down soon.
    LogSizeBudget budget (1000, 5000);
    REQUIRE_FALSE (budget.noteWritten (100));
    REQUIRE (budget.noteWritten (200));
    REQUIRE (budget.trimTarget() == 750);
}
