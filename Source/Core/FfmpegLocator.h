#pragma once
#include <string>
#include <vector>

namespace mma {

enum class HostPlatform
{
    MacOS,
    Windows,
    Linux
};

/// Where to look for ffmpeg, best guess first.
///
/// PATH is not enough on macOS. A GUI app launched from Finder inherits the
/// launchd environment, not the one a login shell builds -- so Homebrew's
/// directories are missing, and an ffmpeg the user installed and can run in
/// Terminal is invisible to the app. Checking the two Homebrew prefixes
/// explicitly is what closes that gap, and the Apple-silicon prefix comes
/// first because that is what a machine bought in the last several years has.
///
/// Pure: it returns paths to try, it does not touch the file system. What is
/// actually there is the caller's business, which is what lets the ordering be
/// tested on a machine with no ffmpeg at all.
std::vector<std::string> ffmpegSearchPaths (HostPlatform platform);

/// The platform this build is for.
HostPlatform thisHostPlatform();

} // namespace mma
