#pragma once

#include <memory>

#include "../Core/BatteryRecordingNotice.h"

namespace mma {

/// §6.6: a laptop that idles to sleep mid-take ends the take. While a take is
/// recording this holds macOS power assertions that stop idle display sleep,
/// idle system sleep and -- on AC power, on most Macs -- the sleep a closed
/// lid would otherwise cause (IOPMAssertionCreateWithName, released with
/// IOPMAssertionRelease). On battery macOS sleeps on lid close regardless;
/// BatteryRecordingNotice tells the performer. Elsewhere it is a no-op.
///
/// setHeld() is idempotent and the destructor releases, so the owner can
/// simply mirror "is a take recording?" into it from any tick and still never
/// leak an assertion on quit. The OS calls sit behind Backend so the
/// bookkeeping is unit-tested on every platform.
class SleepInhibitor
{
public:
    class Backend
    {
    public:
        virtual ~Backend() = default;
        /// True when the OS is now holding the machine awake.
        virtual bool acquire() = 0;
        /// Only called after a successful acquire().
        virtual void release() = 0;
    };

    /// The platform's backend: IOKit power assertions on macOS, a no-op
    /// (acquire() returns false) anywhere else.
    static std::unique_ptr<Backend> createPlatformBackend();

    /// What is powering the computer now (IOPSGetProvidingPowerSourceType on
    /// macOS). Unknown anywhere else, and whenever the OS gives no answer.
    static PowerSource queryPowerSource();

    SleepInhibitor();
    explicit SleepInhibitor (std::unique_ptr<Backend> backend);
    ~SleepInhibitor();

    SleepInhibitor (const SleepInhibitor&) = delete;
    SleepInhibitor& operator= (const SleepInhibitor&) = delete;

    /// Acquire when `shouldHold` and not already held; release when not
    /// `shouldHold` and held. A failed acquire is retried on the next call.
    void setHeld (bool shouldHold);

    /// Whether an assertion is currently held.
    bool isHeld() const noexcept { return held; }

private:
    std::unique_ptr<Backend> backend;
    bool held = false;
};

} // namespace mma
