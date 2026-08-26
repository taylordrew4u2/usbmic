#include "TestFramework.h"
#include "Core/SessionFolderNaming.h"
#include <set>
#include <cctype>

using namespace mma;

TEST_CASE (SessionFolderNaming_SanitizesDisallowedCharacters)
{
    std::string sanitized = SessionFolderNaming::sanitizeName ("My @#$ Session!!");
    for (char c : sanitized)
        REQUIRE ((std::isalnum (static_cast<unsigned char> (c)) || c == '-' || c == '_'));
}

TEST_CASE (SessionFolderNaming_CollapsesWhitespaceToSingleHyphen)
{
    std::string sanitized = SessionFolderNaming::sanitizeName ("a   b     c");
    REQUIRE (sanitized == "a-b-c");
}

TEST_CASE (SessionFolderNaming_TruncatesAt40Characters)
{
    std::string longName (100, 'a');
    std::string sanitized = SessionFolderNaming::sanitizeName (longName);
    REQUIRE (sanitized.size() == SessionFolderNaming::kMaxNameLength);
}

TEST_CASE (SessionFolderNaming_EmptyInputDefaultsToSession)
{
    std::string sanitized = SessionFolderNaming::sanitizeName ("@#$%");
    REQUIRE (sanitized == "Session");
}

TEST_CASE (SessionFolderNaming_BuildsExpectedFolderFormat)
{
    std::string folder = SessionFolderNaming::buildFolderName (2026, 8, 26, 14, 32, "Session");
    REQUIRE (folder == "2026-08-26_1432_Session");
}

TEST_CASE (SessionFolderNaming_ZeroPadsSingleDigitFields)
{
    std::string folder = SessionFolderNaming::buildFolderName (2026, 1, 5, 9, 5, "Session");
    REQUIRE (folder == "2026-01-05_0905_Session");
}

TEST_CASE (SessionFolderNaming_ResolvesCollisionWithNumberedSuffix)
{
    std::set<std::string> existing = { "2026-08-26_1432_Session", "2026-08-26_1432_Session_2" };
    auto exists = [&] (const std::string& name) { return existing.count (name) > 0; };

    std::string resolved = SessionFolderNaming::resolveCollision ("2026-08-26_1432_Session", exists);
    REQUIRE (resolved == "2026-08-26_1432_Session_3");
}

TEST_CASE (SessionFolderNaming_NoCollisionReturnsOriginalName)
{
    auto exists = [] (const std::string&) { return false; };
    std::string resolved = SessionFolderNaming::resolveCollision ("2026-08-26_1432_Session", exists);
    REQUIRE (resolved == "2026-08-26_1432_Session");
}
