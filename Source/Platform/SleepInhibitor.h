#pragma once

#include <memory>

namespace mma {

/// §6.6: a laptop that idles to sleep mid-take ends the take. While a take is
/// recording this holds macOS power assertions that stop idle display sleep
/// and idle system sleep (IOPMAssertionCreateWithName, released with
/// IOPMAssertionRelease). Elsewhere it is a no-op.
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
