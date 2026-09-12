#include "SystemThermalState.h"

#if defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace mma {

SystemThermalState getSystemThermalState() noexcept
{
#if defined(__APPLE__)
    // Application.cpp is C++, not Objective-C++. Calling the small public
    // NSProcessInfo surface through the Objective-C runtime keeps this probe
    // independent of the audio callback and avoids converting the whole app
    // translation unit merely to read one enum.
    const auto processInfoClass = reinterpret_cast<id> (objc_getClass ("NSProcessInfo"));
    if (processInfoClass == nullptr)
        return SystemThermalState::Unknown;

    const auto processInfoSelector = sel_registerName ("processInfo");
    const auto thermalSelector = sel_registerName ("thermalState");
    using SendObject = id (*) (id, SEL);
    using SendInteger = long (*) (id, SEL);

    const auto processInfo = reinterpret_cast<SendObject> (objc_msgSend) (
        processInfoClass, processInfoSelector);
    if (processInfo == nullptr)
        return SystemThermalState::Unknown;

    const auto respondsSelector = sel_registerName ("respondsToSelector:");
    using SendBoolAndSelector = signed char (*) (id, SEL, SEL);
    if (! reinterpret_cast<SendBoolAndSelector> (objc_msgSend) (
            processInfo, respondsSelector, thermalSelector))
        return SystemThermalState::Unknown;

    switch (reinterpret_cast<SendInteger> (objc_msgSend) (processInfo, thermalSelector))
    {
        case 0:  return SystemThermalState::Nominal;
        case 1:  return SystemThermalState::Fair;
        case 2:  return SystemThermalState::Serious;
        case 3:  return SystemThermalState::Critical;
        default: return SystemThermalState::Unknown;
    }
#else
    // Windows and Linux expose several vendor- and machine-specific thermal
    // sources but no dependable process-level throttle state equivalent to
    // NSProcessInfo. Unknown is safer than inventing a healthy reading.
    return SystemThermalState::Unknown;
#endif
}

} // namespace mma
