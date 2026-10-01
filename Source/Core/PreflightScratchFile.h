#pragma once
#include <cctype>
#include <string>

namespace mma {

/// The drive-speed check's scratch file: one fixed, hidden name per folder.
///
/// It used to take a fresh name every run ("preflight.tmp", "preflight (2).tmp"
/// ...) and was deleted only when the check finished. A quit, crash or power
/// cut during the check left up to 200 MB beside the takes, and the next run
/// picked a new name, so they piled up and ate into "Room for ...".
struct PreflightScratchFile
{
    static constexpr const char* kName = ".sobstage-preflight.tmp";

    /// True for a scratch file an earlier run of the check left behind:
    /// the fixed name, or one of the names older versions generated --
    /// "preflight.tmp" or "preflight (N).tmp" exactly. Anything else in the
    /// folder is somebody's file and is never touched.
    static bool isLeftover (const std::string& fileName)
    {
        if (fileName == kName || fileName == "preflight.tmp")
            return true;

        const std::string prefix = "preflight (";
        const std::string suffix = ").tmp";

        if (fileName.size() <= prefix.size() + suffix.size()
            || fileName.compare (0, prefix.size(), prefix) != 0
            || fileName.compare (fileName.size() - suffix.size(), suffix.size(), suffix) != 0)
            return false;

        const auto digits = fileName.substr (prefix.size(),
                                             fileName.size() - prefix.size() - suffix.size());
        for (char c : digits)
            if (! std::isdigit (static_cast<unsigned char> (c)))
                return false;

        return true;
    }
};

} // namespace mma
