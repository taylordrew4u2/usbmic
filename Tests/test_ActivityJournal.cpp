#include "TestFramework.h"
#include "Core/ActivityJournal.h"

using namespace mma;

TEST_CASE (ActivityJournal_AStartAndAStopAreBothRecorded)
{
    // The user's ask, and §10.6's: a take beginning and a take ending are both
    // things that happened, and both belong in the record. An app that only
    // logs failures cannot answer "did it actually start?"
    ActivityJournal journal;
    journal.note (0.0, ActivityLevel::Started, "Recording", "Recording started.");
    journal.note (12.5, ActivityLevel::Stopped, "Recording", "Recording stopped. 3 files saved.");

    const auto entries = journal.getEntries();
    REQUIRE (entries.size() == 2u);

    // Newest first, so a UI showing the top of the list shows the latest news.
    REQUIRE (entries[0].level == ActivityLevel::Stopped);
    REQUIRE (entries[1].level == ActivityLevel::Started);
    REQUIRE (entries[0].atSeconds == 12.5);
}

TEST_CASE (ActivityJournal_TheMostSeriousUnseenThingIsWhatSurfaces)
{
    ActivityJournal journal;
    journal.note (0.0, ActivityLevel::Started, "Monitoring", "You can hear your mics.");
    journal.note (1.0, ActivityLevel::Warning, "Drive", "The drive is falling behind.");
    journal.note (2.0, ActivityLevel::Stopped, "Camera", "Camera switched off.");

    ActivityEntry top;
    REQUIRE (journal.getMostSeriousUnseen (top));

    // Not the newest -- the most serious. A camera being switched off must
    // never push a disk warning out of the one line the user is reading.
    REQUIRE (top.level == ActivityLevel::Warning);
    REQUIRE (top.subject == std::string ("Drive"));
}

TEST_CASE (ActivityJournal_AFailureOutranksAWarningEvenWhenItCameFirst)
{
    ActivityJournal journal;
    journal.note (0.0, ActivityLevel::Failed, "Local backup", "The backup copy stopped.");
    journal.note (1.0, ActivityLevel::Warning, "Drive", "The drive is falling behind.");

    ActivityEntry top;
    REQUIRE (journal.getMostSeriousUnseen (top));
    REQUIRE (top.level == ActivityLevel::Failed);
    REQUIRE (top.subject == std::string ("Local backup"));
}

TEST_CASE (ActivityJournal_TwoFailuresReportTheLatestNotTheFirst)
{
    // Ties break toward the newest so an ongoing problem reports its current
    // form. Told about the first failure forever, the user acts on stale news.
    ActivityJournal journal;
    journal.note (0.0, ActivityLevel::Failed, "Kitchen", "Kitchen mic stopped responding.");
    journal.note (5.0, ActivityLevel::Failed, "Hallway", "Hallway mic stopped responding.");

    ActivityEntry top;
    REQUIRE (journal.getMostSeriousUnseen (top));
    REQUIRE (top.subject == std::string ("Hallway"));
}

TEST_CASE (ActivityJournal_NothingUnseenReportsNothingRatherThanAStaleEntry)
{
    ActivityJournal journal;
    journal.note (0.0, ActivityLevel::Warning, "Drive", "The drive is falling behind.");
    journal.markAllSeen();

    ActivityEntry top;
    REQUIRE_FALSE (journal.getMostSeriousUnseen (top));

    // Seen is not deleted: the entry is still there to look back at.
    REQUIRE (journal.size() == 1u);
    REQUIRE (journal.getUnseenCount() == 0u);
}

TEST_CASE (ActivityJournal_ARepeatCollapsesInsteadOfFloodingTheList)
{
    // A mic on a failing cable can drop several times a second. Six hundred
    // identical lines is the same as no lines -- nobody reads it.
    ActivityJournal journal;

    for (int i = 0; i < 40; ++i)
        journal.note (static_cast<double> (i) * 0.25, ActivityLevel::Failed,
                      "Kitchen", "Kitchen mic stopped responding.");

    REQUIRE (journal.size() == 1u);

    const auto entries = journal.getEntries();
    REQUIRE (entries[0].repeats == 40);

    // The timestamp tracks the latest occurrence, so "when did this last
    // happen" has an answer.
    REQUIRE (entries[0].atSeconds > 9.0);
}

TEST_CASE (ActivityJournal_ARepeatAfterTheWindowIsItsOwnEntry)
{
    // Two dropouts an hour apart are two events, not one event with a count.
    ActivityJournal journal;
    journal.note (0.0, ActivityLevel::Failed, "Kitchen", "Kitchen mic stopped responding.");
    journal.note (ActivityJournal::kRepeatWindowSeconds + 1.0, ActivityLevel::Failed,
                  "Kitchen", "Kitchen mic stopped responding.");

    REQUIRE (journal.size() == 2u);
}

TEST_CASE (ActivityJournal_AStillHappeningProblemBecomesUnseenAgain)
{
    // Dismissing a message must not silence the thing it was about. If it is
    // still going on, the next occurrence is news again.
    ActivityJournal journal;
    journal.note (0.0, ActivityLevel::Failed, "Kitchen", "Kitchen mic stopped responding.");
    journal.markAllSeen();
    REQUIRE (journal.getUnseenCount() == 0u);

    journal.note (1.0, ActivityLevel::Failed, "Kitchen", "Kitchen mic stopped responding.");
    REQUIRE (journal.getUnseenCount() == 1u);
}

TEST_CASE (ActivityJournal_ADifferentMessageIsNeverCollapsedIntoAnother)
{
    ActivityJournal journal;
    journal.note (0.0, ActivityLevel::Failed, "Kitchen", "Kitchen mic stopped responding.");
    journal.note (0.1, ActivityLevel::Failed, "Hallway", "Hallway mic stopped responding.");
    journal.note (0.2, ActivityLevel::Recovered, "Kitchen", "Kitchen mic is back.");

    REQUIRE (journal.size() == 3u);
}

TEST_CASE (ActivityJournal_TheBoundHoldsOverALongSession)
{
    ActivityJournal journal;

    for (int i = 0; i < 5000; ++i)
        journal.note (static_cast<double> (i) * 30.0, ActivityLevel::Started,
                      "Camera", "Camera " + std::to_string (i) + " switched on.");

    REQUIRE (journal.size() == ActivityJournal::kMaxEntries);

    // The newest survived -- a bound that drops the latest news is worse than
    // no journal at all.
    const auto entries = journal.getEntries();
    REQUIRE (entries[0].message.find ("4999") != std::string::npos);
}

TEST_CASE (ActivityJournal_AnUnseenFailureIsNotDroppedToMakeRoomForChatter)
{
    // The bound must never be the reason a failure goes unreported. An unseen
    // failure survives a flood of ordinary starts and stops.
    ActivityJournal journal;
    journal.note (0.0, ActivityLevel::Failed, "Card", "The recording card stopped accepting writes.");

    for (int i = 0; i < 1000; ++i)
        journal.note (static_cast<double> (i + 1), ActivityLevel::Started,
                      "Camera", "Camera " + std::to_string (i) + " switched on.");

    const auto entries = journal.getEntries();

    bool failureSurvived = false;
    for (const auto& e : entries)
        if (e.subject == "Card")
            failureSurvived = true;

    REQUIRE (failureSurvived);
    REQUIRE (journal.size() == ActivityJournal::kMaxEntries);
}

TEST_CASE (ActivityJournal_AnAcknowledgedFailureIsDroppableAgain)
{
    // Pinning is about unread news, not about keeping every failure forever --
    // otherwise a long session of seen failures fills the bound and the newest
    // entries stop being recorded.
    ActivityJournal journal;

    for (int i = 0; i < 300; ++i)
        journal.note (static_cast<double> (i), ActivityLevel::Failed,
                      "Kitchen", "Dropout " + std::to_string (i) + ".");

    journal.markAllSeen();
    journal.note (10000.0, ActivityLevel::Started, "Recording", "Recording started.");

    REQUIRE (journal.size() == ActivityJournal::kMaxEntries);
    REQUIRE (journal.getEntries()[0].subject == std::string ("Recording"));
}

TEST_CASE (ActivityJournal_TheJsonIsOldestFirstAndCarriesEveryField)
{
    ActivityJournal journal;
    journal.note (0.0, ActivityLevel::Started, "Recording", "Recording started.");
    journal.note (61.25, ActivityLevel::Failed, "Local backup", "The backup copy stopped.");

    const auto json = journal.toJson();

    // A log is read forwards, so session.json gets it in the order it happened.
    const auto started = json.find ("Recording started.");
    const auto stopped = json.find ("The backup copy stopped.");
    REQUIRE (started != std::string::npos);
    REQUIRE (stopped != std::string::npos);
    REQUIRE (started < stopped);

    REQUIRE (json.find ("\"level\":\"failed\"") != std::string::npos);
    REQUIRE (json.find ("\"subject\":\"Local backup\"") != std::string::npos);
    REQUIRE (json.find ("\"at\":61.2") != std::string::npos || json.find ("\"at\":61.3") != std::string::npos);
    REQUIRE (json.find ("\"repeats\":1") != std::string::npos);
}

TEST_CASE (ActivityJournal_AQuoteInADeviceNameCannotBreakTheJson)
{
    // Device names come from the OS, so they are not ours to trust. A name with
    // a quote in it used to be the kind of thing that produced a session.json
    // nothing could read -- which loses the whole record, not one line of it.
    ActivityJournal journal;
    journal.note (0.0, ActivityLevel::Started, "Mic", "\"Sam's\" mic \\ started.\nSecond line.");

    const auto json = journal.toJson();
    REQUIRE (json.find ("\\\"Sam's\\\"") != std::string::npos);
    REQUIRE (json.find ("\\\\") != std::string::npos);
    REQUIRE (json.find ("\\n") != std::string::npos);

    // No raw newline survived into the file.
    REQUIRE (json.find ('\n') == std::string::npos);
}

TEST_CASE (ActivityJournal_AnEmptyJournalIsStillValidJson)
{
    ActivityJournal journal;
    REQUIRE (journal.toJson() == std::string ("[]"));
    REQUIRE (journal.size() == 0u);
    REQUIRE (journal.getUnseenCount() == 0u);
}

TEST_CASE (ActivityJournal_ClearingLeavesNothingBehind)
{
    ActivityJournal journal;
    journal.note (0.0, ActivityLevel::Failed, "Card", "The card stopped accepting writes.");
    journal.clear();

    REQUIRE (journal.size() == 0u);

    ActivityEntry top;
    REQUIRE_FALSE (journal.getMostSeriousUnseen (top));
}
