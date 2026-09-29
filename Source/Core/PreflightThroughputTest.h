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
    /// The card would not take the test file at all (read-only, full, gone).
    /// Whatever windows were measured before that are not a speed verdict.
    bool couldNotWrite = false;
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
    /// (card + mix file overhead), plus whatever the cameras are writing to the
    /// same card alongside it.
    ///
    /// The video is not a rounding error next to the audio: eight microphones at
    /// 24-bit/48k need about 4.6 MB/s, and one high-quality camera stream can
    /// ask for as much again. A card benchmarked against the audio alone can
    /// therefore pass this gate and still fail the moment a camera starts --
    /// which is precisely the mid-take degradation §6.4 exists to refuse in
    /// advance. Video is added once rather than doubled: the x2 above covers the
    /// stems plus the mix file, and there is only ever one copy of the video.
    static double requiredBytesPerSecond (int numChannels, double sampleRate, int bytesPerSample,
                                          double videoBytesPerSecond = 0.0) noexcept;

    /// Reduces a series of measured (rolling 1-second window) throughput samples,
    /// in bytes/sec, to the sustained minimum -- never the average.
    static double sustainedMinimum (const std::vector<double>& rollingWindowBytesPerSec) noexcept;

    /// Applies the pass/fail gate: sustained minimum must be >= 2x required.
    static PreflightResult evaluate (const std::vector<double>& rollingWindowBytesPerSec,
                                     int numChannels, double sampleRate, int bytesPerSample,
                                     double videoBytesPerSecond = 0.0);

    /// The same gate applied to a measurement already taken.
    ///
    /// How fast the card is belongs to the card; how fast it needs to be belongs
    /// to the take, and the take changes whenever a microphone or a camera is
    /// switched on. Keeping the two apart means the answer can be recomputed the
    /// moment someone reaches for record, instead of being frozen at whatever
    /// the rig happened to be when the 200 MB test last ran.
    static PreflightResult evaluateMeasured (double sustainedMinBytesPerSec,
                                             int numChannels, double sampleRate, int bytesPerSample,
                                             double videoBytesPerSecond = 0.0);

    /// The gate applied to a cached benchmark. A card that could not be
    /// written is refused as that, never as "too slow" -- with no windows it
    /// read 0 MB/s, and a partial run measured before the failure could even
    /// have passed.
    static PreflightResult evaluateCached (const PreflightResult& cached,
                                           int numChannels, double sampleRate, int bytesPerSample,
                                           double videoBytesPerSecond = 0.0);

    /// How long a "couldn't write" verdict is held before the card is tried
    /// again. Long enough not to hammer a missing volume every UI tick, short
    /// enough that freeing space or reinserting the card is noticed.
    static constexpr double kWriteFailureRetrySeconds = 10.0;

    /// True when a cached verdict should be thrown away and the benchmark run
    /// again. Only a write failure is transient in that way; a slow card stays
    /// slow until the user chooses the location again.
    static bool shouldRetryCached (const PreflightResult& cached, double secondsSinceVerdict,
                                   bool idle, bool workerRunning) noexcept;

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
