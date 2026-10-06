#pragma once
#include <cstdint>

namespace mma {

/// When the app's own log has grown far enough to be trimmed again.
///
/// The log was capped only at launch. A SobStage left open for days -- a
/// venue machine that is never quit -- kept appending until the next launch,
/// and Export diagnostics zipped whatever size it had reached. The logger now
/// asks this after every line, which is a couple of additions: no stat, no
/// file read. Only once the running total passes the cap does it trim the file
/// (to trimTarget(), so one trim buys a quarter of the cap's worth of lines
/// before the next) and resync() with the size it actually found.
///
/// Pure bookkeeping, so it is testable without a file system.
class LogSizeBudget
{
public:
    LogSizeBudget (std::int64_t maxBytes, std::int64_t currentBytes) noexcept;

    /// `bytes` were just appended. True when the file should be trimmed now.
    bool noteWritten (std::int64_t bytes) noexcept;

    /// What to trim the file down to: three quarters of the cap.
    std::int64_t trimTarget() const noexcept;

    /// The size the file really has after a trim (or a trim that failed). A
    /// trim that could not shrink it waits for another quarter-cap of lines
    /// rather than being retried on every line.
    void resync (std::int64_t actualBytes) noexcept;

    std::int64_t estimatedBytes() const noexcept { return estimate; }

private:
    std::int64_t maxBytes;
    std::int64_t estimate;
    std::int64_t nextTrimAt;
};

} // namespace mma
