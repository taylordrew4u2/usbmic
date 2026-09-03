#include "FfmpegLocator.h"

namespace mma {

HostPlatform thisHostPlatform()
{
#if defined (__APPLE__)
    return HostPlatform::MacOS;
#elif defined (_WIN32)
    return HostPlatform::Windows;
#else
    return HostPlatform::Linux;
#endif
}

std::vector<std::string> ffmpegSearchPaths (HostPlatform platform)
{
    switch (platform)
    {
        case HostPlatform::MacOS:
            return {
                "/opt/homebrew/bin/ffmpeg", // Apple silicon Homebrew
                "/usr/local/bin/ffmpeg",    // Intel Homebrew, and most manual installs
                "/opt/local/bin/ffmpeg",    // MacPorts
                "ffmpeg"                    // and PATH, for a launch from a shell
            };

        case HostPlatform::Windows:
            return {
                "C:\\Program Files\\ffmpeg\\bin\\ffmpeg.exe",
                "C:\\ffmpeg\\bin\\ffmpeg.exe",
                "ffmpeg.exe"
            };

        case HostPlatform::Linux:
        default:
            return { "/usr/bin/ffmpeg", "/usr/local/bin/ffmpeg", "ffmpeg" };
    }
}

} // namespace mma
