#include "SessionRecovery.h"
#include <algorithm>
#include <array>
#include <fstream>

namespace mma {

namespace {

uint32_t readU32LE (const std::array<char, 4>& b)
{
    return static_cast<uint32_t> (static_cast<unsigned char> (b[0]))
         | (static_cast<uint32_t> (static_cast<unsigned char> (b[1])) << 8)
         | (static_cast<uint32_t> (static_cast<unsigned char> (b[2])) << 16)
         | (static_cast<uint32_t> (static_cast<unsigned char> (b[3])) << 24);
}

bool readTag (std::fstream& f, std::array<char, 4>& out)
{
    f.read (out.data(), 4);
    return f.gcount() == 4;
}

bool readU32 (std::fstream& f, uint32_t& out)
{
    std::array<char, 4> b {};
    if (! readTag (f, b))
        return false;

    out = readU32LE (b);
    return true;
}

void writeU32LE (std::fstream& f, std::streampos at, uint32_t value)
{
    const char bytes[4] = {
        static_cast<char> (value & 0xFF),
        static_cast<char> ((value >> 8) & 0xFF),
        static_cast<char> ((value >> 16) & 0xFF),
        static_cast<char> ((value >> 24) & 0xFF)
    };

    f.seekp (at);
    f.write (bytes, 4);
}

bool tagIs (const std::array<char, 4>& tag, const char* expected)
{
    return std::equal (tag.begin(), tag.end(), expected);
}

} // namespace

bool SessionRecovery::sessionWasInterrupted (const SessionMetadata& meta)
{
    return meta.stopTimestampIso.empty();
}

int RecoveredSession::keptFileCount() const
{
    return static_cast<int> (std::count_if (files.begin(), files.end(),
                                            [] (const RecoveredFile& f) { return ! f.reportedEmpty; }));
}

int RecoveredSession::emptyFileCount() const
{
    return static_cast<int> (std::count_if (files.begin(), files.end(),
                                            [] (const RecoveredFile& f) { return f.reportedEmpty; }));
}

double RecoveredSession::longestSeconds() const
{
    double longest = 0.0;

    for (const auto& f : files)
        if (! f.reportedEmpty)
            longest = std::max (longest, f.seconds);

    return longest;
}

RecoveredFile SessionRecovery::repairWavFile (const std::string& path)
{
    RecoveredFile result;

    // The name only, so the caller can show it without a path down the side of
    // a panel. The separator check covers both platforms' folders.
    const auto slash = path.find_last_of ("/\\");
    result.fileName = slash == std::string::npos ? path : path.substr (slash + 1);
    result.reportedEmpty = true; // until proven otherwise

    std::fstream file (path, std::ios::in | std::ios::out | std::ios::binary);

    if (! file.is_open())
        return result;

    file.seekg (0, std::ios::end);
    const auto fileSize = static_cast<uint64_t> (file.tellg());
    file.seekg (0, std::ios::beg);

    std::array<char, 4> tag {};
    uint32_t riffSize = 0;
    std::array<char, 4> waveTag {};

    if (! readTag (file, tag) || ! tagIs (tag, "RIFF")
        || ! readU32 (file, riffSize)
        || ! readTag (file, waveTag) || ! tagIs (waveTag, "WAVE"))
        return result;

    const std::streampos riffSizeFieldPos = 4;

    // Walk the chunks rather than assuming where data starts. The writer puts a
    // bext chunk between fmt and data (§6.1), and assuming a fixed offset would
    // break the moment that chunk changed size.
    uint32_t channels = 0, sampleRate = 0, bitsPerSample = 0;
    std::streampos dataSizeFieldPos = 0;
    uint64_t dataStart = 0;
    uint32_t declaredDataSize = 0;
    bool foundData = false;

    while (file && static_cast<uint64_t> (file.tellg()) + 8 <= fileSize)
    {
        std::array<char, 4> chunkTag {};
        uint32_t chunkSize = 0;

        if (! readTag (file, chunkTag) || ! readU32 (file, chunkSize))
            break;

        if (tagIs (chunkTag, "fmt "))
        {
            std::array<char, 4> field {};
            file.read (field.data(), 2); // audio format, unused
            file.read (field.data(), 2);
            channels = static_cast<uint32_t> (static_cast<unsigned char> (field[0]))
                     | (static_cast<uint32_t> (static_cast<unsigned char> (field[1])) << 8);
            readU32 (file, sampleRate);
            uint32_t byteRate = 0;
            readU32 (file, byteRate);
            file.read (field.data(), 2); // block align, recomputed below
            file.read (field.data(), 2);
            bitsPerSample = static_cast<uint32_t> (static_cast<unsigned char> (field[0]))
                          | (static_cast<uint32_t> (static_cast<unsigned char> (field[1])) << 8);

            // Skip any remainder of an extended fmt chunk.
            file.seekg (static_cast<std::streamoff> (8 + chunkSize) - 24, std::ios::cur);
        }
        else if (tagIs (chunkTag, "data"))
        {
            dataSizeFieldPos = static_cast<std::streamoff> (file.tellg()) - 4;
            dataStart = static_cast<uint64_t> (file.tellg());
            declaredDataSize = chunkSize;
            foundData = true;
            break;
        }
        else
        {
            // Chunks are word-aligned, so an odd size carries a pad byte.
            file.seekg (static_cast<std::streamoff> (chunkSize + (chunkSize & 1)), std::ios::cur);
        }
    }

    if (! foundData || channels == 0 || sampleRate == 0 || bitsPerSample == 0)
        return result;

    const uint32_t blockAlign = channels * (bitsPerSample / 8);

    if (blockAlign == 0 || fileSize < dataStart)
        return result;

    // What is actually there, as opposed to what the header last admitted to.
    const uint64_t actualDataBytes = fileSize - dataStart;
    const uint64_t wholeFrameBytes = (actualDataBytes / blockAlign) * blockAlign;

    result.frames = wholeFrameBytes / blockAlign;
    result.seconds = static_cast<double> (result.frames) / static_cast<double> (sampleRate);
    result.headerWasStale = declaredDataSize != wholeFrameBytes;

    if (result.headerWasStale)
    {
        writeU32LE (file, dataSizeFieldPos, static_cast<uint32_t> (wholeFrameBytes));
        // RIFF size counts everything after the size field itself.
        writeU32LE (file, riffSizeFieldPos, static_cast<uint32_t> (dataStart + wholeFrameBytes - 8));
        file.flush();
    }

    // §6.6: under a second is a stub. Reported as empty rather than offered --
    // and left on disk rather than deleted, because silently removing something
    // off a user's card at launch is a worse mistake than listing a short file.
    result.reportedEmpty = result.seconds < kMinimumUsefulSeconds;

    return result;
}

} // namespace mma
