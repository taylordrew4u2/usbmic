#pragma once
#include <cstdint>
#include <deque>

namespace mma {

/// §14.2: bus power exhaustion doesn't produce a clean error; it shows up as
/// enumeration failures and device drops. Heuristic: 2+ enumeration failures
/// or device drops within 5 minutes, while 3+ mics are attached, means "get a
/// powered hub."
class BusPowerDetector
{
public:
    static constexpr int kEventThreshold = 2;
    static constexpr double kWindowSeconds = 5.0 * 60.0;
    static constexpr int kMinMicsForConcern = 3;

    /// Record an enumeration failure or unexpected device drop at time nowSeconds
    /// (monotonic, app-relative clock), with the number of mics currently attached.
    void recordEvent (double nowSeconds, int micsAttached);

    /// True if the sustained-power-exhaustion condition currently holds.
    bool isBusPowerExhausted (double nowSeconds) const;

private:
    struct Event { double timestamp; int micsAttached; };
    std::deque<Event> events;

    void prune (double nowSeconds);
};

} // namespace mma
