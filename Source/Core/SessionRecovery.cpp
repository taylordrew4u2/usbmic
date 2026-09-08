#include "SessionRecovery.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <system_error>

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
    {
        // Two different things arrive here and they need different answers.
        //
        // A path with nothing at it is nothing: no audio was lost, and calling
        // it empty is exactly right. A file that IS there and will not open for
        // writing is a recording of unknown length on a card that has gone
        // read-only -- calling that one empty sends the user away from audio
        // that may be perfectly intact.
        // std::filesystem::exists, not an ifstream open. Opening was a proxy
        // for "is there something here", and it answers differently for a
        // directory on Windows than on Linux -- so the question is asked
        // directly. An error_code overload because a path that cannot even be
        // interrogated is, for our purposes, a path with nothing at it.
        std::error_code ec;

        if (! std::filesystem::exists (path, ec) || ec)
            return result;

        // A file this app cannot even open is not a file that holds under a
        // second of audio, and reporting it as one -- which is what
        // reportedEmpty alone said -- sends the user away from a recording that
        // may be perfectly intact on a card that has gone read-only.
        //
        // reportedEmpty is cleared as well as repairFailed set, or the panel
        // goes on counting it under "empty file left alone" and a folder of
        // nothing but unopenable files still fails isWorthPresenting() and is
        // announced as one where nothing survived. Nobody knows whether
        // anything survived -- that is the whole point -- so it is listed, and
        // the per-file warning says it could not be read.
        result.reportedEmpty = false;
        result.repairFailed = true;
        return result;
    }

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

        // Checked. A card that is read-only, full or failing takes the repair
        // and drops it, and this used to report the file as repaired anyway --
        // the user then meets the same broken header in whatever they open it
        // with, having been told it was fixed.
        result.repairFailed = ! file.good();
    }

    // §6.6: under a second is a stub. Reported as empty rather than offered --
    // and left on disk rather than deleted, because silently removing something
    // off a user's card at launch is a worse mistake than listing a short file.
    result.reportedEmpty = result.seconds < kMinimumUsefulSeconds;

    return result;
}

} // namespace mma
