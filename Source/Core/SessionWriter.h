#pragma once
#include <string>
#include <fstream>
#include <vector>
#include <cstdint>

namespace mma {

/// §6.1/§6.3/§6.6: a single BWF-tagged WAV (or RF64) file writer with
/// auto-split at 3.9GB and 5-second header rewrites for crash safety.
/// Real file I/O -- exercised by Tests/ against tmp files on this Linux
/// sandbox, since none of it needs real audio hardware.
class SessionWriter
{
public:
    static constexpr uint64_t kAutoSplitBytes = 3900ull * 1000ull * 1000ull; // 3.9 GB
    static constexpr double kHeaderRewriteIntervalSeconds = 5.0;

    SessionWriter() = default;
    ~SessionWriter();

    /// basePath is the file path WITHOUT the _NNN split suffix or extension, e.g.
    /// ".../01_Yeti-Kitchen"; ".wav" is appended, and "_001" etc. once a second
    /// file is needed. originTimestampIso is the shared BWF session-origin
    /// timestamp stamped into every stem and the mix file.
    bool open (const std::string& basePath, double sampleRate, int numChannels, int bitDepth,
              const std::string& originTimestampIso);

    /// Writes one block of interleaved float samples in [-1, 1]. Converts to the
    /// configured bit depth, auto-splitting to a new numbered file when the
    /// current file would exceed kAutoSplitBytes. No throwing; returns false on
    /// an unrecoverable write failure (caller handles per §6.5 "target card removed").
    bool writeInterleaved (const float* interleaved, size_t numFrames);

    /// Call periodically (e.g. from a timer, not the audio thread) with elapsed
    /// wall-clock seconds; rewrites the RIFF/data-chunk-size header fields every
    /// kHeaderRewriteIntervalSeconds so a crash mid-take leaves a playable file.
    void tick (double dtSeconds);

    /// Finalizes the current file (writes a final correct header) and closes it.
    void close();

    uint64_t getTotalFramesWritten() const { return totalFramesWritten; }
    int getSplitFileCount() const { return splitIndex; }
    std::string getCurrentFilePath() const { return currentFilePath; }

private:
    std::string basePathNoExt;
    std::string originTimestamp;
    double sampleRate = 48000.0;
    int numChannels = 1;
    int bitDepth = 24;
    int splitIndex = 0; // 0 = no split suffix yet; becomes 1 ("_001") on first split
    bool splitSuffixActive = false;

    std::fstream file;
    std::string currentFilePath;
    uint64_t dataBytesWrittenToCurrentFile = 0;
    uint64_t totalFramesWritten = 0;
    double secondsSinceLastHeaderRewrite = 0.0;

    // Byte offsets of header fields we rewrite in place.
    std::streampos riffSizeFieldPos {};
    std::streampos dataSizeFieldPos {};

    std::string makePathForSplit (int index) const;
    bool openNewFile (int index);
    void writeHeaderPlaceholder();
    void rewriteHeaderSizes();
    int bytesPerSample() const { return bitDepth / 8; }
};

} // namespace mma
