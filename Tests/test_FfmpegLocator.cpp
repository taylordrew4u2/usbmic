#include "TestFramework.h"
#include "Core/FfmpegLocator.h"

using namespace mma;

namespace {

bool hasSeparator (const std::string& path)
{
    return path.find ('/') != std::string::npos || path.find ('\\') != std::string::npos;
}

} // namespace

TEST_CASE (FfmpegLocator_MacChecksBothHomebrewPrefixesBeforePath)
{
    // A Finder launch does not inherit a login shell's PATH, so an ffmpeg the
    // user installed with Homebrew is only found by its absolute location.
    const auto paths = ffmpegSearchPaths (HostPlatform::MacOS);
    REQUIRE (paths.size() >= 3);
    REQUIRE (paths[0] == "/opt/homebrew/bin/ffmpeg");
    REQUIRE (paths[1] == "/usr/local/bin/ffmpeg");
    REQUIRE (paths.back() == "ffmpeg");
}

TEST_CASE (FfmpegLocator_EveryPlatformEndsWithABareNameForPath)
{
    // TakeCombiner checks anything with a separator on disk without running
    // it, and runs a bare name to let PATH answer. Exactly one bare name, last,
    // keeps that from spawning a process for every candidate.
    for (auto platform : { HostPlatform::MacOS, HostPlatform::Windows, HostPlatform::Linux })
    {
        const auto paths = ffmpegSearchPaths (platform);
        REQUIRE (! paths.empty());
        REQUIRE (! hasSeparator (paths.back()));

        for (size_t i = 0; i + 1 < paths.size(); ++i)
            REQUIRE (hasSeparator (paths[i]));
    }
}

TEST_CASE (FfmpegLocator_WindowsLooksForTheExe)
{
    for (const auto& path : ffmpegSearchPaths (HostPlatform::Windows))
        REQUIRE (path.size() > 4 && path.substr (path.size() - 4) == ".exe");
}

TEST_CASE (FfmpegLocator_KnowsWhichPlatformItWasBuiltFor)
{
#if defined (__APPLE__)
    REQUIRE (thisHostPlatform() == HostPlatform::MacOS);
#elif defined (_WIN32)
    REQUIRE (thisHostPlatform() == HostPlatform::Windows);
#else
    REQUIRE (thisHostPlatform() == HostPlatform::Linux);
#endif
}
