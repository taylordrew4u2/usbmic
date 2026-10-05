#pragma once
#include <optional>
#include <string>

namespace mma {

/// The opt-in "is there a newer SobStage?" check, minus the network.
///
/// PRIVACY.md promises the app sends nothing anywhere, so this is off unless
/// the user turns it on, and even then it is one anonymous GET of the public
/// releases API at most once a day. Everything that can be decided without the
/// network lives here -- reading the release, comparing versions, deciding
/// whether a check is due -- so it can be tested on a machine with no network
/// and the App layer is left with only the request itself.
namespace UpdateCheck {

/// The one address the check ever contacts.
inline constexpr const char* kLatestReleaseUrl =
    "https://api.github.com/repos/taylordrew4u2/usbmic/releases/latest";

/// Where the Download button goes when a release's own page cannot be trusted.
inline constexpr const char* kReleasesPageUrl =
    "https://github.com/taylordrew4u2/usbmic/releases/latest";

/// At most one automatic check per this many seconds.
inline constexpr double kCheckIntervalSeconds = 24.0 * 60.0 * 60.0;

/// A version as the release tags write it: "v1.13.10", "1.13", "1.14.0-rc1".
struct Version
{
    int major = 0, minor = 0, patch = 0;

    /// Whatever followed a '-' ("rc1", "beta.2"). Empty for a real release.
    std::string preRelease;

    bool isPreRelease() const { return ! preRelease.empty(); }
};

/// Nothing back for anything that does not start with a number after an
/// optional 'v' -- "dev", an empty tag -- so a build without a real version
/// never reads as older than everything and nags about every release.
/// Missing parts are zero; build metadata after '+' is ignored.
std::optional<Version> parseVersion (const std::string& text);

/// Negative, zero or positive, like strcmp. The numbers decide first, and
/// numerically: 1.13.10 is newer than 1.13.9 although it sorts before it as
/// text. With equal numbers a release is newer than any pre-release of it, and
/// two pre-releases of the same number compare as equal -- their suffixes are
/// too freely written ("rc1", "beta", "test-build") to order honestly.
int compare (const Version& a, const Version& b);

/// True only when both parse and `candidate` is strictly newer. Anything that
/// cannot be read is "not newer": a check that cannot tell must stay quiet.
bool isNewer (const std::string& candidate, const std::string& current);

enum class Platform { Mac, Windows, Linux };

/// What this build is, for picking the download that fits it.
Platform currentPlatform();

/// What the releases API said, reduced to the parts the app uses.
struct ReleaseInfo
{
    std::string tag;       // "v1.13.18"
    std::string pageUrl;   // the release's page on github.com
    std::string assetUrl;  // this platform's download, or empty
};

/// Reads a /releases/latest response. Nothing back for anything that is not a
/// published, non-draft, non-pre-release with a readable version tag.
///
/// Every URL is checked to be on https://github.com/ before it is kept. The
/// Download button opens one in the user's browser, and a response that is not
/// what it claims -- a captive portal's page, a proxy's rewrite -- must not be
/// able to choose where that goes; a release page that fails the check falls
/// back to kReleasesPageUrl.
std::optional<ReleaseInfo> parseLatestRelease (const std::string& json, Platform platform);

/// Whether the automatic check should run now. `lastCheckSeconds` is wall-clock
/// seconds since the epoch, 0 for never. A last check in the future means the
/// clock was moved, and waiting for it to come round again could be months, so
/// that counts as due.
bool isCheckDue (bool enabled, double nowSeconds, double lastCheckSeconds);

} // namespace UpdateCheck
} // namespace mma
