#include "LogSizeBudget.h"
#include <algorithm>

namespace mma {

LogSizeBudget::LogSizeBudget (std::int64_t maxBytesToKeep, std::int64_t currentBytes) noexcept
    : maxBytes (std::max<std::int64_t> (maxBytesToKeep, 1)),
      estimate (std::max<std::int64_t> (currentBytes, 0)),
      nextTrimAt (maxBytes)
{
    resync (estimate);
}

bool LogSizeBudget::noteWritten (std::int64_t bytes) noexcept
{
    estimate += std::max<std::int64_t> (bytes, 0);
    return estimate > nextTrimAt;
}

std::int64_t LogSizeBudget::trimTarget() const noexcept
{
    return maxBytes - maxBytes / 4;
}

void LogSizeBudget::resync (std::int64_t actualBytes) noexcept
{
    estimate = std::max<std::int64_t> (actualBytes, 0);

    // Normally the cap. If the file is already over it (the trim failed, or a
    // huge line arrived), the next attempt waits for another quarter-cap.
    nextTrimAt = estimate < maxBytes ? maxBytes
                                     : estimate + (maxBytes - trimTarget());
}

} // namespace mma
