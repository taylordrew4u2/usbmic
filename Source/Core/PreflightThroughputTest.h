#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <optional>

namespace mma {

struct PreflightResult
{
    bool passed = false;
    double sustainedMinBytesPerSec = 0.0;
    double requiredBytesPerSec = 0.0;
    std::string reason; // human-readable reason when !passed
};

/// §6.4 pre-flight throughput test. This class contains only the pure
/// calculations (required-rate math, rolling-minimum reduction, gate logic,
/// remaining-time formatting, cache-expiry check); actual file I/O to write
/// the 200MB test file belongs to the caller (SessionWriter/RecordingEngine),
/// keeping this class trivially testable without touching a real filesystem.
class PreflightThroughputTest
{
public:
    static constexpr size_t kTestFileBytes = 200ull * 1024ull * 1024ull;
    static constexpr double kRequiredMultiplier = 2.0; // must sustain >= 2x required rate
    static constexpr double kCacheExpiryDays = 30.0;

    /// Required sustained throughput per §6.4: channels * sampleRate * bytesPerSample * 2
    /// (card + mix file overhead).
    static double requiredBytesPerSecond (int numChannels, double sampleRate, int bytesPerSample) noexcept;

    /// Reduces a series of measured (rolling 1-second window) throughput samples,
    /// in bytes/sec, to the sustained minimum -- never the average.
    static double sustainedMinimum (const std::vector<double>& rollingWindowBytesPerSec) noexcept;

    /// Applies the pass/fail gate: sustained minimum must be >= 2x required.
    static PreflightResult evaluate (const std::vector<double>& rollingWindowBytesPerSec,
                                     int numChannels, double sampleRate, int bytesPerSample);

    /// Formats remaining free space as recording time in "Xh Ym" form, per §6.4
    /// ("remaining recording time in hours and minutes, not bytes").
    static std::string formatRemainingTime (uint64_t freeBytes, double requiredBytesPerSecPerFile) noexcept;

    /// True if a cached pass/fail result for this volume, recorded cacheAgeDays ago,
    /// has expired and preflight must be re-run.
    static bool isCacheExpired (double cacheAgeDays) noexcept { return cacheAgeDays >= kCacheExpiryDays; }

    enum class FilesystemKind { Unknown, ExFAT, NTFS, APFS, HFSPlus, FAT32, Other };

    /// True if the filesystem is one the OS cannot write large/long files to
    /// reliably (FAT32's 4GB file-size ceiling is exactly the case in §6.1/§6.4).
    static bool needsReformat (FilesystemKind kind) noexcept { return kind == FilesystemKind::FAT32; }
};

} // namespace mma
