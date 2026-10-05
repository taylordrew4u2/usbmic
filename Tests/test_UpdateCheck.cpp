#include "TestFramework.h"
#include "Core/UpdateCheck.h"
#include "Core/AppSettings.h"

using namespace mma;

namespace {

// Trimmed from a real /releases/latest response: the fields the app reads,
// plus enough of the rest (nested author, escaped body, numbers) to prove the
// parser walks past what it does not use.
const char* kRelease = R"({
  "url": "https://api.github.com/repos/taylordrew4u2/usbmic/releases/1",
  "html_url": "https://github.com/taylordrew4u2/usbmic/releases/tag/v1.13.18",
  "id": 123456789,
  "author": { "login": "taylordrew4u2", "id": 42, "site_admin": false },
  "tag_name": "v1.13.18",
  "name": "SobStage 1.13.18",
  "draft": false,
  "prerelease": false,
  "assets": [
    { "name": "SobStage-Windows.zip", "size": 1000,
      "browser_download_url": "https://github.com/taylordrew4u2/usbmic/releases/download/v1.13.18/SobStage-Windows.zip" },
    { "name": "SobStage-macOS.dmg", "size": 2000,
      "browser_download_url": "https://github.com/taylordrew4u2/usbmic/releases/download/v1.13.18/SobStage-macOS.dmg" }
  ],
  "body": "Fixes \"the thing\".\r\n\r\n- one\n- two — three"
})";

} // namespace

TEST_CASE (UpdateCheck_VersionsCompareNumericallyNotAsText)
{
    // The case that motivates this file: as text, "1.13.10" sorts before
    // "1.13.9", and a string compare would never offer the tenth patch.
    REQUIRE (UpdateCheck::isNewer ("v1.13.10", "1.13.9"));
    REQUIRE_FALSE (UpdateCheck::isNewer ("v1.13.9", "1.13.10"));

    REQUIRE (UpdateCheck::isNewer ("v1.14.0", "1.13.17"));
    REQUIRE (UpdateCheck::isNewer ("v2.0.0", "1.99.99"));
    REQUIRE_FALSE (UpdateCheck::isNewer ("v1.13.17", "1.13.17"));
    REQUIRE_FALSE (UpdateCheck::isNewer ("v1.13.16", "1.13.17"));
}

TEST_CASE (UpdateCheck_TheLeadingVIsOptionalAndMissingPartsAreZero)
{
    REQUIRE_FALSE (UpdateCheck::isNewer ("1.13.17", "v1.13.17"));
    REQUIRE_FALSE (UpdateCheck::isNewer ("V1.14", "1.14.0"));
    REQUIRE (UpdateCheck::isNewer ("2", "1.99.99"));

    const auto v = UpdateCheck::parseVersion ("  v1.13.18\n");
    REQUIRE (v.has_value());
    REQUIRE (v->major == 1);
    REQUIRE (v->minor == 13);
    REQUIRE (v->patch == 18);
    REQUIRE_FALSE (v->isPreRelease());
}

TEST_CASE (UpdateCheck_APreReleaseIsOlderThanItsReleaseButNotThanTheOneBefore)
{
    // Someone running a release candidate is offered the real release...
    REQUIRE (UpdateCheck::isNewer ("v1.14.0", "1.14.0-rc1"));
    // ...is never offered their own number back as a downgrade...
    REQUIRE_FALSE (UpdateCheck::isNewer ("v1.14.0-rc2", "1.14.0"));
    // ...and a pre-release of the next number is still ahead of today's.
    REQUIRE (UpdateCheck::isNewer ("v1.14.0-beta", "1.13.17"));

    // Suffixes are not ordered: "rc2" over "rc1" would be a guess about how
    // somebody chose to spell a label, so two of the same number tie.
    REQUIRE_FALSE (UpdateCheck::isNewer ("v1.14.0-rc2", "1.14.0-rc1"));

    // Build metadata is not a pre-release.
    const auto v = UpdateCheck::parseVersion ("1.13.18+mac.arm64");
    REQUIRE (v.has_value());
    REQUIRE_FALSE (v->isPreRelease());

    const auto rc = UpdateCheck::parseVersion ("v1.14.0-rc.1+build5");
    REQUIRE (rc.has_value());
    REQUIRE (rc->preRelease == std::string ("rc.1"));
}

TEST_CASE (UpdateCheck_AnythingUnreadableIsNeverNewer)
{
    // A development build reports "dev". It must not read as older than every
    // release and nag about each one.
    REQUIRE_FALSE (UpdateCheck::parseVersion ("dev").has_value());
    REQUIRE_FALSE (UpdateCheck::parseVersion ("").has_value());
    REQUIRE_FALSE (UpdateCheck::parseVersion ("v").has_value());
    REQUIRE_FALSE (UpdateCheck::parseVersion ("latest").has_value());
    REQUIRE_FALSE (UpdateCheck::parseVersion ("v99999999999.0.0").has_value());

    REQUIRE_FALSE (UpdateCheck::isNewer ("v1.14.0", "dev"));
    REQUIRE_FALSE (UpdateCheck::isNewer ("nightly", "1.13.17"));
}

TEST_CASE (UpdateCheck_AReleaseIsReadWithThisPlatformsDownload)
{
    const auto mac = UpdateCheck::parseLatestRelease (kRelease, UpdateCheck::Platform::Mac);
    REQUIRE (mac.has_value());
    REQUIRE (mac->tag == std::string ("v1.13.18"));
    REQUIRE (mac->pageUrl == std::string ("https://github.com/taylordrew4u2/usbmic/releases/tag/v1.13.18"));
    REQUIRE (mac->assetUrl == std::string ("https://github.com/taylordrew4u2/usbmic/releases/download/v1.13.18/SobStage-macOS.dmg"));

    const auto win = UpdateCheck::parseLatestRelease (kRelease, UpdateCheck::Platform::Windows);
    REQUIRE (win.has_value());
    REQUIRE (win->assetUrl == std::string ("https://github.com/taylordrew4u2/usbmic/releases/download/v1.13.18/SobStage-Windows.zip"));

    // No packaged Linux download: the page is the answer.
    const auto other = UpdateCheck::parseLatestRelease (kRelease, UpdateCheck::Platform::Linux);
    REQUIRE (other.has_value());
    REQUIRE (other->assetUrl.empty());
    REQUIRE_FALSE (other->pageUrl.empty());
}

TEST_CASE (UpdateCheck_DraftsPreReleasesAndJunkAreNotReleases)
{
    REQUIRE_FALSE (UpdateCheck::parseLatestRelease (
        R"({"tag_name":"v1.14.0","html_url":"https://github.com/x","draft":true})",
        UpdateCheck::Platform::Mac).has_value());
    REQUIRE_FALSE (UpdateCheck::parseLatestRelease (
        R"({"tag_name":"v1.14.0-rc1","html_url":"https://github.com/x","prerelease":true})",
        UpdateCheck::Platform::Mac).has_value());

    // GitHub's own error shape, a rate limit, a captive portal's HTML page,
    // and nothing at all.
    REQUIRE_FALSE (UpdateCheck::parseLatestRelease (
        R"({"message":"Not Found","documentation_url":"https://docs.github.com"})",
        UpdateCheck::Platform::Mac).has_value());
    REQUIRE_FALSE (UpdateCheck::parseLatestRelease (
        "<html><body>Sign in to the hotel wifi</body></html>",
        UpdateCheck::Platform::Mac).has_value());
    REQUIRE_FALSE (UpdateCheck::parseLatestRelease ("", UpdateCheck::Platform::Mac).has_value());
    REQUIRE_FALSE (UpdateCheck::parseLatestRelease ("[]", UpdateCheck::Platform::Mac).has_value());
}

TEST_CASE (UpdateCheck_AResponseCutShortIsNotTrusted)
{
    // The tag arrived; the rest did not. A half-read release is not one.
    const std::string full (kRelease);
    const auto cut = full.substr (0, full.find ("\"draft\""));

    REQUIRE_FALSE (UpdateCheck::parseLatestRelease (cut, UpdateCheck::Platform::Mac).has_value());
}

TEST_CASE (UpdateCheck_OnlyGitHubAddressesAreKeptForTheBrowser)
{
    // The Download button opens this. A response that is not what it claims
    // must not get to pick where the user's browser goes.
    const auto info = UpdateCheck::parseLatestRelease (R"({
        "tag_name": "v1.14.0",
        "html_url": "https://example.com/totally-sobstage",
        "assets": [ { "name": "SobStage-macOS.dmg",
                      "browser_download_url": "http://github.com/plain-http.dmg" } ]
    })", UpdateCheck::Platform::Mac);

    REQUIRE (info.has_value());
    REQUIRE (info->pageUrl == std::string (UpdateCheck::kReleasesPageUrl));
    REQUIRE (info->assetUrl.empty());
}

TEST_CASE (UpdateCheck_TheAutomaticCheckRunsAtMostOnceADay)
{
    const double now = 1790000000.0;
    const double day = UpdateCheck::kCheckIntervalSeconds;

    // Off means off, however long it has been.
    REQUIRE_FALSE (UpdateCheck::isCheckDue (false, now, 0.0));
    REQUIRE_FALSE (UpdateCheck::isCheckDue (false, now, now - 10 * day));

    REQUIRE (UpdateCheck::isCheckDue (true, now, 0.0));            // never checked
    REQUIRE_FALSE (UpdateCheck::isCheckDue (true, now, now - 60));  // a minute ago
    REQUIRE_FALSE (UpdateCheck::isCheckDue (true, now, now - day + 1));
    REQUIRE (UpdateCheck::isCheckDue (true, now, now - day));

    // A last check in the future is a clock that moved; do not wait for it.
    REQUIRE (UpdateCheck::isCheckDue (true, now, now + 30 * day));
}

TEST_CASE (UpdateCheck_TheSettingIsOffByDefaultAndRemembered)
{
    // PRIVACY.md: nothing leaves the machine unless the user asks.
    REQUIRE_FALSE (AppSettings{}.checkForUpdates);
    REQUIRE (AppSettings::fromJsonString ("{\"mirrorEnabled\": true}").checkForUpdates == false);

    AppSettings s;
    s.checkForUpdates = true;
    s.lastUpdateCheckSeconds = 1790000123.75;

    const auto restored = AppSettings::fromJsonString (s.toJsonString());
    REQUIRE (restored.checkForUpdates);

    // Whole seconds, exactly: the number writer rounds fractions to six
    // significant figures, which would move a timestamp by minutes.
    REQUIRE (restored.lastUpdateCheckSeconds == 1790000123.0);
}
