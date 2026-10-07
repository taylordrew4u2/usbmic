#include "TakeFolderTracker.h"
#include "Utf8Path.h"

#include <filesystem>
#include <system_error>

#if defined (__APPLE__) || defined (__linux__)
 #include <fcntl.h>
 #include <unistd.h>
#endif

#if defined (__APPLE__)
 #include <sys/param.h>
#endif

namespace mma {

TakeFolderTracker::~TakeFolderTracker()
{
    close();
}

bool TakeFolderTracker::open (const std::string& folder)
{
    close();

    if (folder.empty())
        return false;

#if defined (__APPLE__)
    // Event-only: the descriptor exists to be asked where the folder is, and
    // must never be the thing that stops a card from ejecting.
    descriptor = ::open (folder.c_str(), O_EVTONLY | O_DIRECTORY | O_CLOEXEC);
#elif defined (__linux__)
    descriptor = ::open (folder.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
#endif

    if (descriptor < 0)
        return false;

    openedPath = folder;
    lastSystemPath.clear();
    lastResolved.clear();
    return true;
}

void TakeFolderTracker::close()
{
#if defined (__APPLE__) || defined (__linux__)
    if (descriptor >= 0)
        ::close (descriptor);
#endif

    descriptor = -1;
    openedPath.clear();
    lastSystemPath.clear();
    lastResolved.clear();
}

std::string TakeFolderTracker::currentSystemPath() const
{
    if (descriptor < 0)
        return {};

#if defined (__APPLE__) && defined (F_GETPATH)
    char buffer[MAXPATHLEN] = {};
    if (::fcntl (descriptor, F_GETPATH, buffer) != 0)
        return {};

    return std::string (buffer);
#elif defined (__linux__)
    const auto link = "/proc/self/fd/" + std::to_string (descriptor);
    char buffer[4096] = {};
    const auto length = ::readlink (link.c_str(), buffer, sizeof (buffer) - 1);
    if (length <= 0)
        return {};

    std::string path (buffer, static_cast<size_t> (length));

    // The kernel's way of saying the folder has no name any more.
    const std::string deleted = " (deleted)";
    if (path.size() >= deleted.size()
        && path.compare (path.size() - deleted.size(), deleted.size(), deleted) == 0)
        return {};

    return path;
#else
    return {};
#endif
}

std::string TakeFolderTracker::resolve() const
{
    const auto now = currentSystemPath();
    if (now.empty())
        return {};

    if (now == lastSystemPath)
        return lastResolved;

    std::string resolved = now;

    if (now == openedPath)
    {
        resolved = openedPath;
    }
    else
    {
        // Same folder reached by another spelling (a symlinked parent) is not
        // a move: keep the spelling the take was given, as WritePipeline does.
        std::error_code ec;
        if (std::filesystem::equivalent (pathFromUtf8 (openedPath), pathFromUtf8 (now), ec) && ! ec)
            resolved = openedPath;
    }

    lastSystemPath = now;
    lastResolved = resolved;
    return resolved;
}

} // namespace mma
