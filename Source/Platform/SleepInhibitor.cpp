#include "SleepInhibitor.h"

#include <utility>

#if defined(__APPLE__)
 // Plain C API: no Objective-C needed, so this stays a .cpp file.
 #include <IOKit/pwr_mgt/IOPMLib.h>
#endif

namespace mma {

namespace {

#if defined(__APPLE__)

// IOPMAssertionID is a uint32_t; 0 is never a valid assertion (the SDK's
// kIOPMNullAssertionID), spelled here to depend on as little as possible.
constexpr IOPMAssertionID kNoAssertion = 0;

class MacPowerAssertions final : public SleepInhibitor::Backend
{
public:
    bool acquire() override
    {
        const CFStringRef name = CFSTR ("SobStage recording");

        IOPMAssertionID display = kNoAssertion;
        if (IOPMAssertionCreateWithName (kIOPMAssertionTypePreventUserIdleDisplaySleep,
                                         kIOPMAssertionLevelOn, name, &display)
            != kIOReturnSuccess)
            return false;

        IOPMAssertionID system = kNoAssertion;
        if (IOPMAssertionCreateWithName (kIOPMAssertionTypePreventUserIdleSystemSleep,
                                         kIOPMAssertionLevelOn, name, &system)
            != kIOReturnSuccess)
        {
            IOPMAssertionRelease (display);
            return false;
        }

        displayAssertion = display;
        systemAssertion = system;
        return true;
    }

    void release() override
    {
        if (displayAssertion != kNoAssertion)
            IOPMAssertionRelease (displayAssertion);
        if (systemAssertion != kNoAssertion)
            IOPMAssertionRelease (systemAssertion);

        displayAssertion = kNoAssertion;
        systemAssertion = kNoAssertion;
    }

private:
    IOPMAssertionID displayAssertion = kNoAssertion;
    IOPMAssertionID systemAssertion = kNoAssertion;
};

#else

class NoSleepInhibition final : public SleepInhibitor::Backend
{
public:
    bool acquire() override { return false; }
    void release() override {}
};

#endif

} // namespace

std::unique_ptr<SleepInhibitor::Backend> SleepInhibitor::createPlatformBackend()
{
#if defined(__APPLE__)
    return std::make_unique<MacPowerAssertions>();
#else
    return std::make_unique<NoSleepInhibition>();
#endif
}

SleepInhibitor::SleepInhibitor() : SleepInhibitor (createPlatformBackend()) {}

SleepInhibitor::SleepInhibitor (std::unique_ptr<Backend> b) : backend (std::move (b)) {}

SleepInhibitor::~SleepInhibitor()
{
    setHeld (false);
}

void SleepInhibitor::setHeld (bool shouldHold)
{
    if (backend == nullptr || shouldHold == held)
        return;

    if (shouldHold)
    {
        held = backend->acquire();
    }
    else
    {
        backend->release();
        held = false;
    }
}

} // namespace mma
