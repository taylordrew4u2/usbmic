#include "SessionWriter.h"
#include <algorithm>
#include <cstring>
#include <cmath>

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <fcntl.h>
 #include <unistd.h>
#endif

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
    // Only the depths the packer below actually writes. 32 used to be
    // accepted here, given a 32-bit header, and then packed as 24-bit --
    // every file unreadable, no error anywhere.
    if (bitDepthIn != 16 && bitDepthIn != 24 && bitDepthIn != 32)
        return false;

    basePathNoExt = basePath;
    sampleRate = sampleRateIn;
    numChannels = numChannelsIn;
    bitDepth = bitDepthIn;
    originTimestamp = originTimestampIso;
    splitIndex = 0;
    splitSuffixActive = false;
    writeProblem.clear();
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
        // BWF TimeReferenceLow/High (offset 338/342) is the sample offset from
        // the shared session origin. The first file starts at zero; a split
        // continuation starts after every frame already written, so a DAW lays
        // the pieces end to end instead of stacking every _NNN file at t=0.
        const uint64_t timeReference = totalFramesWritten;
        for (int byte = 0; byte < 8; ++byte)
            bext[static_cast<size_t> (338 + byte)] =
                static_cast<char> ((timeReference >> (byte * 8)) & 0xff);
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
        if (dataBytesWrittenToCurrentFile + frameBytes > autoSplitBytes)
        {
            if (! rewriteHeaderSizes())
            {
                writeProblem = "Couldn't finish " + currentFilePath
                             + " before starting the next file. Recording has stopped to protect the take.";
                return false;
            }

            splitIndex = std::max (1, splitIndex + 1);
            splitSuffixActive = true;

            if (! openNewFile (splitIndex))
            {
                // The caller turns a false into "the card stopped accepting
                // writes", which is true but not what happened: the take ran
                // past 3.9 GB and the next file could not be created. Same
                // outcome, different thing to check.
                writeProblem = "Couldn't start the next file after " + currentFilePath
                             + ". The card may be full, or may not allow files this large.";
                return false;
            }
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
            else if (bitDepth == 32)
            {
                // Integer PCM, full scale. Computed in double: float cannot
                // hold 2^31 - 1 exactly and would round past it.
                const int32_t v = static_cast<int32_t> (std::llround (static_cast<double> (s) * 2147483647.0));
                const auto u = static_cast<uint32_t> (v);
                char b[4] = { static_cast<char> (u & 0xFF), static_cast<char> ((u >> 8) & 0xFF),
                              static_cast<char> ((u >> 16) & 0xFF), static_cast<char> ((u >> 24) & 0xFF) };
                file.write (b, 4);
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

bool SessionWriter::rewriteHeaderSizes()
{
    if (! file.is_open())
        return false;

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

    return file.good() && syncCurrentFileToStorage();
}

bool SessionWriter::syncCurrentFileToStorage()
{
    // std::fstream::flush() only reaches the operating-system cache. §6.6's
    // periodic header rewrite is specifically the crash/power-loss boundary,
    // so ask the OS to push those bytes to the device as well. This function is
    // called by the writer/timer path, never by an audio callback.
#if defined (_WIN32)
    const HANDLE handle = CreateFileA (currentFilePath.c_str(), GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;

    const bool ok = FlushFileBuffers (handle) != 0;
    CloseHandle (handle);
    return ok;
#else
    const int descriptor = ::open (currentFilePath.c_str(), O_RDWR);
    if (descriptor < 0)
        return false;

    bool ok = false;
 #if defined (__APPLE__) && defined (F_FULLFSYNC)
    ok = ::fcntl (descriptor, F_FULLFSYNC) == 0;
    if (! ok)
        ok = ::fsync (descriptor) == 0;
 #else
    ok = ::fsync (descriptor) == 0;
 #endif
    ::close (descriptor);
    return ok;
#endif
}

bool SessionWriter::tick (double dtSeconds)
{
    secondsSinceLastHeaderRewrite += dtSeconds;

    if (secondsSinceLastHeaderRewrite < kHeaderRewriteIntervalSeconds)
        return true;

    secondsSinceLastHeaderRewrite = 0.0;
    return rewriteHeaderSizes();
}

bool SessionWriter::close()
{
    if (! file.is_open())
        return false;

    // Taken before close(), because closing clears the stream state that says
    // whether the final header actually landed.
    const bool headerLanded = rewriteHeaderSizes();

    file.close();

    // file.good() is false after a successful close on some implementations,
    // so the close itself is judged by fail(), not good().
    return headerLanded && ! file.fail();
}

} // namespace mma
