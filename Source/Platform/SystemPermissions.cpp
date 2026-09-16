#include "SystemPermissions.h"

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <atomic>
#include <cstring>
#include <system_error>

#if defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

namespace mma {
namespace permissions {

PermissionState fromAVAuthorizationStatus (long status) noexcept
{
    switch (status)
    {
        case 0:  return PermissionState::NotYetRequested;
        case 1:  return PermissionState::Denied;   // restricted; see the header
        case 2:  return PermissionState::Denied;
        case 3:  return PermissionState::Granted;
        default: return PermissionState::NotApplicable;
    }
}

PermissionState fromWindowsConsentValue (const std::string& value) noexcept
{
    if (value == "Allow")
        return PermissionState::Granted;

    if (value == "Deny")
        return PermissionState::Denied;

    // No value written yet means the user has never been asked, which is the
    // same position as macOS's notDetermined: the prompt comes when we open.
    return value.empty() ? PermissionState::NotYetRequested
                         : PermissionState::NotApplicable;
}

PermissionState fromWriteProbeErrno (int probeErrno) noexcept
{
    if (probeErrno == 0)
        return PermissionState::Granted;

    if (probeErrno == EACCES || probeErrno == EPERM)
        return PermissionState::Denied;

    return PermissionState::NotApplicable;
}

} // namespace permissions

PermissionState queryMicrophonePermission() noexcept
{
#if defined(__APPLE__)
    // Application.cpp is C++, not Objective-C++, and SystemThermalState.cpp
    // already reaches a small public AppKit surface through the Objective-C
    // runtime rather than converting a translation unit for one enum. This
    // follows that, calling
    //   +[AVCaptureDevice authorizationStatusForMediaType: AVMediaTypeAudio]
    // which is documented as safe to call at any time and never prompts.
    const auto deviceClass = reinterpret_cast<id> (objc_getClass ("AVCaptureDevice"));
    if (deviceClass == nullptr)
        return PermissionState::NotApplicable;

    const auto selector = sel_registerName ("authorizationStatusForMediaType:");
    const auto respondsSelector = sel_registerName ("respondsToSelector:");
    using SendBoolAndSelector = signed char (*) (id, SEL, SEL);

    if (! reinterpret_cast<SendBoolAndSelector> (objc_msgSend) (
            deviceClass, respondsSelector, selector))
        return PermissionState::NotApplicable;

    // AVMediaTypeAudio is the NSString constant @"soun". Building it here
    // avoids linking AVFoundation just to read one exported symbol.
    const auto stringClass = reinterpret_cast<id> (objc_getClass ("NSString"));
    if (stringClass == nullptr)
        return PermissionState::NotApplicable;

    using SendStringFromUtf8 = id (*) (id, SEL, const char*);
    const auto mediaType = reinterpret_cast<SendStringFromUtf8> (objc_msgSend) (
        stringClass, sel_registerName ("stringWithUTF8String:"), "soun");
    if (mediaType == nullptr)
        return PermissionState::NotApplicable;

    using SendIntegerWithObject = long (*) (id, SEL, id);
    return permissions::fromAVAuthorizationStatus (
        reinterpret_cast<SendIntegerWithObject> (objc_msgSend) (deviceClass, selector, mediaType));

#elif defined(_WIN32)
    // The CapabilityAccessManager consent store. WinRT's AppCapability API is
    // the documented route, but it is packaged-app only; a plain desktop build
    // reads the same state the Settings app writes.
    HKEY key {};
    const auto* path = "Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone";

    if (RegOpenKeyExA (HKEY_CURRENT_USER, path, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return PermissionState::NotApplicable;

    char value[32] {};
    DWORD size = sizeof (value);
    DWORD type = 0;
    const auto result = RegQueryValueExA (key, "Value", nullptr, &type,
                                          reinterpret_cast<LPBYTE> (value), &size);
    RegCloseKey (key);

    if (result != ERROR_SUCCESS || type != REG_SZ)
        return PermissionState::NotApplicable;

    return permissions::fromWindowsConsentValue (std::string (value, strnlen (value, sizeof (value))));

#else
    // Linux has no per-application microphone consent to read. NotApplicable,
    // not Granted: we have no evidence either way, and PermissionGuidance is
    // silent on both — but only one of them is honest.
    return PermissionState::NotApplicable;
#endif
}

PermissionState queryVolumeWritePermission (const std::string& destinationPath) noexcept
{
    if (destinationPath.empty())
        return PermissionState::NotApplicable;

    std::error_code ec;
    if (! std::filesystem::is_directory (destinationPath, ec) || ec)
        return PermissionState::NotApplicable;

    // A fixed name would collide between two copies of the app probing at
    // once; the point is to learn whether the OS lets us create anything here.
    static std::atomic<unsigned> counter { 0 };
    const auto probe = std::filesystem::path (destinationPath)
                     / (".sobstage-access-probe-" + std::to_string (counter.fetch_add (1)));

    errno = 0;
    std::FILE* handle = std::fopen (probe.string().c_str(), "wb");
    const int openErrno = (handle != nullptr) ? 0 : errno;

    if (handle != nullptr)
    {
        std::fclose (handle);
        std::error_code ignored;
        std::filesystem::remove (probe, ignored);
    }

    return permissions::fromWriteProbeErrno (openErrno);
}

} // namespace mma
