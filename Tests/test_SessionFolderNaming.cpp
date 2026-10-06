#include "TestFramework.h"
#include "Core/SessionFolderNaming.h"
#include "Core/PortIdentity.h"
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

// A name the user gives a microphone or camera is an override: clearing it (or
// typing only symbols) must hand the device its own name back, not rename it
// "Session" -- which then became the strip, the stem file and the .mov.
TEST_CASE (SessionFolderNaming_OverrideNameWithNothingUsableIsEmpty)
{
    REQUIRE (SessionFolderNaming::sanitizeNameOrEmpty ("").empty());
    REQUIRE (SessionFolderNaming::sanitizeNameOrEmpty ("   ").empty());
    REQUIRE (SessionFolderNaming::sanitizeNameOrEmpty ("!!!").empty());
    REQUIRE (SessionFolderNaming::sanitizeNameOrEmpty (" - _ ").empty());
    REQUIRE (SessionFolderNaming::sanitizeNameOrEmpty ("  Alex Kim  ") == "Alex-Kim");
    REQUIRE (SessionFolderNaming::sanitizeNameOrEmpty ("Sam!") == "Sam");
}

// Letters beyond ASCII used to be dropped byte by byte: "Müller" became
// "Mller", and a name written entirely in another script became "Session".
TEST_CASE (SessionFolderNaming_KeepsLettersInEveryScript)
{
    REQUIRE (SessionFolderNaming::sanitizeName ("M\xC3\xBCller") == "M\xC3\xBCller");
    REQUIRE (SessionFolderNaming::sanitizeName ("Jos\xC3\xA9 Garc\xC3\xAD" "a") == "Jos\xC3\xA9-Garc\xC3\xAD" "a");
    REQUIRE (SessionFolderNaming::sanitizeName ("\xE5\xB1\xB1\xE7\x94\xB0") == "\xE5\xB1\xB1\xE7\x94\xB0");   // 山田
    REQUIRE (SessionFolderNaming::sanitizeName ("\xCE\x91\xCE\xB8\xCE\xB7\xCE\xBD\xCE\xAC") == "\xCE\x91\xCE\xB8\xCE\xB7\xCE\xBD\xCE\xAC"); // Αθηνά

    // A mic or input name in another script is a name, not "no name".
    REQUIRE (SessionFolderNaming::sanitizeNameOrEmpty ("  \xE5\xB1\xB1\xE7\x94\xB0  ") == "\xE5\xB1\xB1\xE7\x94\xB0");
}

// What Windows, FAT and Finder object to never survives, in either script.
TEST_CASE (SessionFolderNaming_StripsWhatFileSystemsReject)
{
    REQUIRE (SessionFolderNaming::sanitizeName ("a/b\\c:d*e?f\"g<h>i|j") == "abcdefghij");
    REQUIRE (SessionFolderNaming::sanitizeName (".hidden..") == "hidden");
    REQUIRE (SessionFolderNaming::sanitizeName (std::string ("tab\there\x01")) == "tabhere");

    // Invisible direction and format marks: a right-to-left override would
    // make Finder show a different name from the real one.
    REQUIRE (SessionFolderNaming::sanitizeName ("ab\xE2\x80\xAE" "cd\xE2\x80\x8B" "e") == "abcde");

    // Unicode spaces collapse like ASCII ones, and a name never starts with one.
    REQUIRE (SessionFolderNaming::sanitizeName ("\xC2\xA0Show\xE3\x80\x80One") == "Show-One");
}

// The same name has to give the same bytes whichever form it arrived in, or
// "Müller" typed and "Müller" from a decomposed macOS file name would be two
// folders that look identical.
TEST_CASE (SessionFolderNaming_ComposesToNfc)
{
    const std::string composed = "M\xC3\xBCller";      // U+00FC
    const std::string decomposed = "Mu\xCC\x88ller";   // u + U+0308
    REQUIRE (SessionFolderNaming::sanitizeName (decomposed) == composed);

    // Two marks stacked in canonical order: e + U+0323 + U+0302 -> U+1EC7.
    REQUIRE (SessionFolderNaming::sanitizeName ("e\xCC\xA3\xCC\x82") == "\xE1\xBB\x87");

    // Hangul jamo compose to the syllable: U+1112 U+1161 U+11AB -> U+D55C (한).
    REQUIRE (SessionFolderNaming::sanitizeName ("\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB") == "\xED\x95\x9C");
}

// Forty characters, not forty bytes, and never half of one.
TEST_CASE (SessionFolderNaming_TruncatesByCharacterNotByte)
{
    std::string longName;
    for (int i = 0; i < 60; ++i)
        longName += "\xC3\xBC"; // ü

    const auto sanitized = SessionFolderNaming::sanitizeName (longName);
    REQUIRE (sanitized.size() == SessionFolderNaming::kMaxNameLength * 2);

    // Malformed UTF-8 is dropped rather than copied into a file name.
    REQUIRE (SessionFolderNaming::sanitizeName (std::string ("ok\xC3") + "\xFF" + "go") == "okgo");
}

TEST_CASE (PortIdentity_ClearingAnInputNameRemovesTheOverride)
{
    PersistedDeviceSettings settings;
    settings.setNameForInput (1, true, "Alex");
    REQUIRE (settings.inputNames.at (1) == "Alex");

    settings.setNameForInput (1, true, "");
    REQUIRE (settings.inputNames.count (1) == 0);

    settings.setNameForInput (1, true, "Sam");
    settings.setNameForInput (1, true, "!!!");
    REQUIRE (settings.inputNames.count (1) == 0);
}

TEST_CASE (PortIdentity_ClearingASingleMicNameRemovesTheOverride)
{
    PersistedDeviceSettings settings;
    settings.setNameForInput (0, false, "  Host  ");
    REQUIRE (settings.assignedName == "Host");

    settings.setNameForInput (0, false, "   ");
    REQUIRE (settings.assignedName.empty());
}
