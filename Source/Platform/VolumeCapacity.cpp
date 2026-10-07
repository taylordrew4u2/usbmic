#include "VolumeCapacity.h"
#include "../Core/Utf8Path.h"

#include <cctype>
#include <filesystem>
#include <system_error>

#if ! defined(_WIN32)
 #include <cerrno>
 #include <sys/stat.h>
#endif

#if defined(__APPLE__)
 #include <sys/mount.h>
 #include <sys/param.h>
#endif

#if defined(__APPLE__)
 // Plain C API: no Objective-C needed, so this stays a .cpp file. mma_core
 // already links CoreFoundation for SleepInhibitor.
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace mma {

namespace volumecapacity {

uint64_t choose (uint64_t basicAvailable, std::optional<int64_t> importantUsage) noexcept
{
    if (importantUsage.has_value() && *importantUsage > 0)
        return static_cast<uint64_t> (*importantUsage);

    return basicAvailable;
}

} // namespace volumecapacity

namespace {

#if defined(__APPLE__)
std::optional<int64_t> importantUsageCapacity (const std::string& path) noexcept
{
    const CFURLRef url = CFURLCreateFromFileSystemRepresentation (
        kCFAllocatorDefault, reinterpret_cast<const UInt8*> (path.c_str()),
        static_cast<CFIndex> (path.size()), false);

    if (url == nullptr)
        return std::nullopt;

    CFTypeRef value = nullptr;
    const Boolean found = CFURLCopyResourcePropertyForKey (
        url, kCFURLVolumeAvailableCapacityForImportantUsageKey, &value, nullptr);
    CFRelease (url);

    std::optional<int64_t> result;

    if (found && value != nullptr && CFGetTypeID (value) == CFNumberGetTypeID())
    {
        SInt64 bytes = 0;
        if (CFNumberGetValue (static_cast<CFNumberRef> (value), kCFNumberSInt64Type, &bytes))
            result = static_cast<int64_t> (bytes);
    }

    if (value != nullptr)
        CFRelease (value);

    return result;
}
#endif

} // namespace

std::optional<uint64_t> availableBytesForRecording (const std::string& path) noexcept
{
    try
    {
        std::error_code error;
        const auto space = std::filesystem::space (pathFromUtf8 (path), error);

        if (error)
            return std::nullopt;

        const auto basic = static_cast<uint64_t> (space.available);

       #if defined(__APPLE__)
        return volumecapacity::choose (basic, importantUsageCapacity (path));
       #else
        return basic;
       #endif
    }
    catch (...)
    {
        return std::nullopt;
    }
}

VolumeIdentity volumeIdentityOf (const std::string& path) noexcept
{
    VolumeIdentity identity;

    try
    {
        std::error_code error;
        auto p = std::filesystem::absolute (pathFromUtf8 (path), error);
        if (error || p.empty())
            return identity;

       #if defined(_WIN32)
        // A drive letter is a volume. Nothing finer is asked: Windows cannot
        // record to a folder that is not on some drive root anyway.
        auto root = utf8FromPath (p.root_name());
        for (auto& c : root)
            c = static_cast<char> (std::toupper (static_cast<unsigned char> (c)));
        if (! root.empty())
            identity.device = "root:" + root;
       #else
        // The backup folder does not exist before its first take: walk up to
        // the nearest folder that does, which is where it will be created.
        for (int depth = 0; depth < 128 && ! p.empty(); ++depth)
        {
            struct stat info {};
            if (::stat (p.c_str(), &info) == 0)
            {
                identity.device = "dev:" + std::to_string (static_cast<unsigned long long> (info.st_dev));

               #if defined(__APPLE__)
                struct statfs fsInfo {};
                if (::statfs (p.c_str(), &fsInfo) == 0)
                    identity.physicalDisk = mirrorplacement::apfsContainerFromMount (fsInfo.f_fstypename,
                                                                                    fsInfo.f_mntfromname);
               #endif
                return identity;
            }

            // Only "not there yet" is worth climbing past. A permission or
            // I/O error says nothing about which disk this is.
            if (errno != ENOENT && errno != ENOTDIR)
                return identity;

            const auto parent = p.parent_path();
            if (parent == p)
                break;
            p = parent;
        }
       #endif
    }
    catch (...)
    {
        return {};
    }

    return identity;
}

} // namespace mma
