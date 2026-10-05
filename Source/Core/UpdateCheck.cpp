#include "UpdateCheck.h"
#include "Json.h"
#include <cctype>

namespace mma {
namespace UpdateCheck {

namespace {

bool startsWith (const std::string& text, const char* prefix)
{
    return text.rfind (prefix, 0) == 0;
}

// Anything the app might open in a browser has to be on github.com, over TLS.
bool isGitHubUrl (const std::string& url)
{
    return startsWith (url, "https://github.com/");
}

// The asset each platform's release ships, by the name the release workflow
// gives it. Linux has no packaged download, so it gets the page.
const char* assetNameFor (Platform platform)
{
    switch (platform)
    {
        case Platform::Mac:     return "SobStage-macOS.dmg";
        case Platform::Windows: return "SobStage-Windows.zip";
        case Platform::Linux:   break;
    }

    return nullptr;
}

} // namespace

std::optional<Version> parseVersion (const std::string& text)
{
    size_t pos = text.find_first_not_of (" \t\r\n");
    if (pos == std::string::npos)
        return std::nullopt;

    if (text[pos] == 'v' || text[pos] == 'V')
        ++pos;

    // Up to three dot-separated numbers. Capped at nine digits each so a
    // garbage tag cannot overflow an int on its way in.
    int parts[3] = { 0, 0, 0 };
    int count = 0;

    while (count < 3 && pos < text.size() && std::isdigit (static_cast<unsigned char> (text[pos])))
    {
        const size_t start = pos;
        while (pos < text.size() && std::isdigit (static_cast<unsigned char> (text[pos])))
            ++pos;

        if (pos - start > 9)
            return std::nullopt;

        parts[count++] = std::stoi (text.substr (start, pos - start));

        if (count < 3 && pos + 1 < text.size() && text[pos] == '.'
            && std::isdigit (static_cast<unsigned char> (text[pos + 1])))
            ++pos;
        else
            break;
    }

    if (count == 0)
        return std::nullopt;

    Version v;
    v.major = parts[0];
    v.minor = parts[1];
    v.patch = parts[2];

    // Whatever is left, up to any build metadata, is a pre-release label --
    // "-rc1" as semver writes it, or "rc1" run straight on as people do.
    const size_t plus = text.find ('+', pos);
    std::string rest = text.substr (pos, plus == std::string::npos ? std::string::npos : plus - pos);
    const auto end = rest.find_last_not_of (" \t\r\n");
    rest = end == std::string::npos ? std::string() : rest.substr (0, end + 1);

    if (! rest.empty() && (rest[0] == '-' || rest[0] == '.'))
        rest.erase (0, 1);

    v.preRelease = rest;
    return v;
}

int compare (const Version& a, const Version& b)
{
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;

    if (a.isPreRelease() != b.isPreRelease())
        return a.isPreRelease() ? -1 : 1;

    return 0;
}

bool isNewer (const std::string& candidate, const std::string& current)
{
    const auto c = parseVersion (candidate);
    const auto mine = parseVersion (current);

    return c.has_value() && mine.has_value() && compare (*c, *mine) > 0;
}

Platform currentPlatform()
{
   #if defined (__APPLE__)
    return Platform::Mac;
   #elif defined (_WIN32)
    return Platform::Windows;
   #else
    return Platform::Linux;
   #endif
}

std::optional<ReleaseInfo> parseLatestRelease (const std::string& json, Platform platform)
{
    bool truncated = false;
    JsonValue root;

    try { root = JsonValue::parse (json, &truncated); }
    catch (...) { return std::nullopt; }

    // A response cut short is not trusted for any of it: the tag may be the
    // only thing that arrived, and a half-read release is not a release.
    if (truncated || root.getType() != JsonValue::Type::Object)
        return std::nullopt;

    const auto* draft = root.find ("draft");
    const auto* preRelease = root.find ("prerelease");

    // /releases/latest already skips both, but a different endpoint or a
    // proxy answering in its place might not -- and nobody should be told to
    // download a test build.
    if ((draft != nullptr && draft->asBool (false))
        || (preRelease != nullptr && preRelease->asBool (false)))
        return std::nullopt;

    ReleaseInfo info;
    if (const auto* tag = root.find ("tag_name")) info.tag = tag->asString();

    if (! parseVersion (info.tag).has_value())
        return std::nullopt;

    if (const auto* page = root.find ("html_url")) info.pageUrl = page->asString();
    if (! isGitHubUrl (info.pageUrl))
        info.pageUrl = kReleasesPageUrl;

    if (const char* wanted = assetNameFor (platform))
        if (const auto* assets = root.find ("assets"))
            for (const auto& asset : assets->asArray())
            {
                const auto* name = asset.find ("name");
                const auto* url = asset.find ("browser_download_url");

                if (name != nullptr && url != nullptr && name->asString() == wanted
                    && isGitHubUrl (url->asString()))
                {
                    info.assetUrl = url->asString();
                    break;
                }
            }

    return info;
}

bool isCheckDue (bool enabled, double nowSeconds, double lastCheckSeconds)
{
    if (! enabled)
        return false;

    if (lastCheckSeconds <= 0.0 || lastCheckSeconds > nowSeconds)
        return true;

    return nowSeconds - lastCheckSeconds >= kCheckIntervalSeconds;
}

} // namespace UpdateCheck
} // namespace mma
