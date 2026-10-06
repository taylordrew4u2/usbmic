#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace mma {

namespace volumecapacity {

/// The figure to use from the two the system can give.
///
/// `basicAvailable` is statfs's f_bavail (std::filesystem::space().available).
/// On APFS that leaves out purgeable space -- iCloud "Optimize Mac storage"
/// copies, caches, local snapshots -- which macOS frees the moment a write
/// needs it, so a Mac whose Finder shows 50 GB available can report a few
/// hundred megabytes here. `importantUsage` is the volume's "available for
/// important usage" capacity, which counts that space; it is used when the
/// system gave one (present and non-zero), and the basic figure otherwise.
uint64_t choose (uint64_t basicAvailable, std::optional<int64_t> importantUsage) noexcept;

} // namespace volumecapacity

/// Bytes a recording can still use on the volume holding `path` (an existing
/// file or folder, UTF-8), or empty when the system cannot say.
///
/// macOS: kCFURLVolumeAvailableCapacityForImportantUsageKey, falling back to
/// std::filesystem::space when that is unavailable or zero. Elsewhere exactly
/// std::filesystem::space().available. May block on a disappearing volume the
/// same way std::filesystem::space does, so callers keep it off the message
/// thread wherever they already kept that off it.
std::optional<uint64_t> availableBytesForRecording (const std::string& path) noexcept;

} // namespace mma
