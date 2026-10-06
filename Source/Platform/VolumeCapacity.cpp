#include "VolumeCapacity.h"
#include "../Core/Utf8Path.h"

#include <filesystem>
#include <system_error>

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

} // namespace mma
