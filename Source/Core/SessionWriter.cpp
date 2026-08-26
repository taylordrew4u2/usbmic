#include "SessionWriter.h"
#include <algorithm>
#include <cstring>
#include <cmath>

namespace mma {

namespace {

void writeU32LE (std::ostream& out, uint32_t v)
{
    char b[4] = { static_cast<char> (v & 0xFF), static_cast<char> ((v >> 8) & 0xFF),
                  static_cast<char> ((v >> 16) & 0xFF), static_cast<char> ((v >> 24) & 0xFF) };
    out.write (b, 4);
}

void writeU16LE (std::ostream& out, uint16_t v)
{
    char b[2] = { static_cast<char> (v & 0xFF), static_cast<char> ((v >> 8) & 0xFF) };
    out.write (b, 2);
}

void writeTag (std::ostream& out, const char* tag) { out.write (tag, 4); }

} // namespace

SessionWriter::~SessionWriter()
{
    close();
}

std::string SessionWriter::makePathForSplit (int index) const
{
    if (index <= 0)
        return basePathNoExt + ".wav";

    char suffix[16];
    std::snprintf (suffix, sizeof (suffix), "_%03d", index);
    return basePathNoExt + suffix + ".wav";
}

bool SessionWriter::open (const std::string& basePath, double sampleRateIn, int numChannelsIn, int bitDepthIn,
                          const std::string& originTimestampIso)
{
    basePathNoExt = basePath;
    sampleRate = sampleRateIn;
    numChannels = numChannelsIn;
    bitDepth = bitDepthIn;
    originTimestamp = originTimestampIso;
    splitIndex = 0;
    splitSuffixActive = false;
    totalFramesWritten = 0;
    secondsSinceLastHeaderRewrite = 0.0;

    return openNewFile (0);
}

bool SessionWriter::openNewFile (int index)
{
    if (file.is_open())
        file.close();

    currentFilePath = makePathForSplit (index);
    file.open (currentFilePath, std::ios::binary | std::ios::out | std::ios::in | std::ios::trunc);
    if (! file.is_open())
    {
        // Retry with out-only (some platforms need the file to exist before in|out trunc works).
        file.open (currentFilePath, std::ios::binary | std::ios::out | std::ios::trunc);
        file.close();
        file.open (currentFilePath, std::ios::binary | std::ios::out | std::ios::in);
        if (! file.is_open())
            return false;
    }

    dataBytesWrittenToCurrentFile = 0;
    writeHeaderPlaceholder();
    return true;
}

void SessionWriter::writeHeaderPlaceholder()
{
    // RIFF header.
    writeTag (file, "RIFF");
    riffSizeFieldPos = file.tellp();
    writeU32LE (file, 0); // patched later
    writeTag (file, "WAVE");

    // fmt chunk.
    writeTag (file, "fmt ");
    writeU32LE (file, 16);
    writeU16LE (file, 1); // PCM
    writeU16LE (file, static_cast<uint16_t> (numChannels));
    writeU32LE (file, static_cast<uint32_t> (sampleRate));
    const uint32_t byteRate = static_cast<uint32_t> (sampleRate) * static_cast<uint32_t> (numChannels) * static_cast<uint32_t> (bytesPerSample());
    writeU32LE (file, byteRate);
    writeU16LE (file, static_cast<uint16_t> (numChannels * bytesPerSample()));
    writeU16LE (file, static_cast<uint16_t> (bitDepth));

    // bext (Broadcast WAVE) chunk: minimal fixed-size 602-byte body, per EBU
    // Tech 3285, carrying the origination date/time so a DAW can align stems
    // without manual nudging (§6.1).
    writeTag (file, "bext");
    writeU32LE (file, 602);
    {
        std::vector<char> bext (602, 0);
        // Description[256], Originator[32], OriginatorReference[32] left blank/zero.
        // OriginationDate[10] "YYYY-MM-DD" at offset 320, OriginationTime[8] "HH:MM:SS" at offset 330.
        std::string date, time;
        if (originTimestamp.size() >= 19)
        {
            date = originTimestamp.substr (0, 10);
            time = originTimestamp.substr (11, 8);
        }
        std::memcpy (bext.data() + 320, date.data(), std::min<size_t> (date.size(), 10));
        std::memcpy (bext.data() + 330, time.data(), std::min<size_t> (time.size(), 8));
        // TimeReferenceLow/High (offset 338/342) left at 0: this file's origin IS t=0.
        // Version (offset 346) = 1.
        bext[346] = 1;
        file.write (bext.data(), static_cast<std::streamsize> (bext.size()));
    }

    // data chunk.
    writeTag (file, "data");
    dataSizeFieldPos = file.tellp();
    writeU32LE (file, 0); // patched as data is written and on close
}

bool SessionWriter::writeInterleaved (const float* interleaved, size_t numFrames)
{
    if (! file.is_open())
        return false;

    const size_t bps = static_cast<size_t> (bytesPerSample());
    const size_t frameBytes = bps * static_cast<size_t> (numChannels);

    size_t frameStart = 0;
    while (frameStart < numFrames)
    {
        // Auto-split at 3.9GB (§6.1) before writing would push us over.
        if (dataBytesWrittenToCurrentFile + frameBytes > kAutoSplitBytes)
        {
            rewriteHeaderSizes();
            splitIndex = std::max (1, splitIndex + 1);
            splitSuffixActive = true;
            if (! openNewFile (splitIndex))
                return false;
        }

        const float* frame = interleaved + frameStart * static_cast<size_t> (numChannels);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float s = std::clamp (frame[ch], -1.0f, 1.0f);
            if (bitDepth == 16)
            {
                int16_t v = static_cast<int16_t> (std::lround (s * 32767.0f));
                char b[2] = { static_cast<char> (v & 0xFF), static_cast<char> ((v >> 8) & 0xFF) };
                file.write (b, 2);
            }
            else // 24-bit
            {
                int32_t v = static_cast<int32_t> (std::lround (s * 8388607.0f));
                char b[3] = { static_cast<char> (v & 0xFF), static_cast<char> ((v >> 8) & 0xFF),
                              static_cast<char> ((v >> 16) & 0xFF) };
                file.write (b, 3);
            }
        }

        dataBytesWrittenToCurrentFile += frameBytes;
        ++totalFramesWritten;
        ++frameStart;
    }

    return file.good();
}

void SessionWriter::rewriteHeaderSizes()
{
    if (! file.is_open())
        return;

    const auto currentPos = file.tellp();
    file.seekp (0, std::ios::end);
    const auto fileEnd = file.tellp();
    file.seekp (currentPos);

    const uint32_t dataSize = static_cast<uint32_t> (std::min<uint64_t> (dataBytesWrittenToCurrentFile, 0xFFFFFFFFull));
    const uint64_t totalFileBytes = static_cast<uint64_t> (fileEnd);
    const uint32_t riffSize = static_cast<uint32_t> (std::min<uint64_t> (totalFileBytes >= 8 ? totalFileBytes - 8 : 0, 0xFFFFFFFFull));

    file.seekp (riffSizeFieldPos);
    writeU32LE (file, riffSize);
    file.seekp (dataSizeFieldPos);
    writeU32LE (file, dataSize);

    file.seekp (currentPos);
    file.flush();
}

void SessionWriter::tick (double dtSeconds)
{
    secondsSinceLastHeaderRewrite += dtSeconds;
    if (secondsSinceLastHeaderRewrite >= kHeaderRewriteIntervalSeconds)
    {
        rewriteHeaderSizes();
        secondsSinceLastHeaderRewrite = 0.0;
    }
}

void SessionWriter::close()
{
    if (! file.is_open())
        return;
    rewriteHeaderSizes();
    file.close();
}

} // namespace mma
