// §6.5, on a drive that genuinely runs out of room in the middle of a take.
//
// Both halves of this were unit-tested against fakes and neither had ever met
// a real ENOSPC. What that hid: when a write failed, SessionWriter set no
// account at all -- only the roll-over past 3.9 GB did -- so the take stopped
// under the card-removal notice, which tells the user the drive "stopped
// responding" and to check that it is plugged in properly. Someone whose card
// was simply full therefore spent the one moment they were still next to the
// rig re-seating a cable that was never loose.
//
// Application.cpp already guarded against exactly this, with a comment saying
// so; the guard could not fire because the account it looks for was never
// written.
//
//   Tools/e2e_disk_full.sh   (creates the small filesystem this needs)

#include "Core/CaptureCoordinator.h"
#include "Platform/AlsaBackend.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check (bool condition, const std::string& what)
{
    std::printf (condition ? "  PASS  %s\n" : "  FAIL  %s\n", what.c_str());
    if (! condition)
        ++failures;
}

/// Whether a WAV on disk actually describes the audio inside it.
///
/// The header's RIFF and data sizes start as placeholder zeros and are patched
/// when the file is closed. On a full card that patch used to be silently
/// discarded -- seeking to the header first flushes buffered audio, the flush
/// has nowhere to go, and the seek fails with it -- so the file kept its zeros.
/// The audio was on the card and perfectly good, and every player opened it and
/// saw an empty recording.
bool wavDescribesItsAudio (const std::string& path, int blockAlign, uint32_t& frames)
{
    frames = 0;

    std::FILE* f = std::fopen (path.c_str(), "rb");

    if (f == nullptr)
        return false;

    std::fseek (f, 0, SEEK_END);
    const auto fileBytes = static_cast<uint64_t> (std::ftell (f));

    const auto readU32 = [] (std::FILE* h) -> uint32_t
    {
        unsigned char b[4] {};

        if (std::fread (b, 1, 4, h) != 4)
            return 0;

        return static_cast<uint32_t> (b[0]) | (static_cast<uint32_t> (b[1]) << 8)
             | (static_cast<uint32_t> (b[2]) << 16) | (static_cast<uint32_t> (b[3]) << 24);
    };

    std::fseek (f, 4, SEEK_SET);
    const auto riffSize = readU32 (f);

    std::fseek (f, 12, SEEK_SET);
    uint32_t dataSize = 0;
    uint64_t dataStart = 0;
    bool found = false;

    for (;;)
    {
        unsigned char tag[4] {};

        if (std::fread (tag, 1, 4, f) != 4)
            break;

        const auto size = readU32 (f);

        if (std::memcmp (tag, "data", 4) == 0)
        {
            dataSize = size;
            dataStart = static_cast<uint64_t> (std::ftell (f));
            found = true;
            break;
        }

        std::fseek (f, static_cast<long> (size + (size & 1u)), SEEK_CUR);
    }

    std::fclose (f);

    if (! found || dataSize == 0)
        return false;

    // Every byte the header claims must really be there, or a reader walks off
    // the end of the file -- a worse failure than the one being fixed.
    if (dataStart + dataSize > fileBytes)
        return false;

    // RIFF must not describe more than the file holds either.
    if (static_cast<uint64_t> (riffSize) + 8 > fileBytes)
        return false;

    // The drive gave out mid-frame, so a chunk that is not a whole number of
    // frames is possible and is malformed.
    if (blockAlign > 0 && dataSize % static_cast<uint32_t> (blockAlign) != 0)
        return false;

    frames = blockAlign > 0 ? dataSize / static_cast<uint32_t> (blockAlign) : 0;
    return true;
}

} // namespace

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: disk_full_take <directory on a nearly-full filesystem>\n");
        return 2;
    }

    const std::string dir = argv[1];

    mma::AlsaBackend backend;
    auto devices = backend.enumerateInputDevices();

    if (devices.size() < 2)
    {
        std::printf ("  FAIL  need two fixture microphones, found %zu\n", devices.size());
        return 1;
    }

    mma::CaptureCoordinator coordinator (backend, 48000.0, 256);

    std::vector<mma::CaptureChannel> mics;

    for (size_t i = 0; i < 2; ++i)
    {
        mma::CaptureChannel c;
        c.deviceId = devices[i].usbLocationId;
        c.deviceChannel = 0;
        c.displayName = "Mic " + std::to_string (i + 1);
        c.fileName = "0" + std::to_string (i + 1) + "_Mic-" + std::to_string (i + 1);
        c.bitDepth = 24;
        mics.push_back (c);
    }

    if (! coordinator.startMonitoring (mics, {}))
    {
        std::printf ("  FAIL  startMonitoring: %s\n", coordinator.getMonitorProblem().c_str());
        return 1;
    }

    if (! coordinator.startRecording (dir, 24, "2026-09-15T00:00:00Z"))
    {
        std::printf ("  FAIL  startRecording into %s\n", dir.c_str());
        return 1;
    }

    check (! coordinator.hasCardWriteFailed(), "the take starts with the drive still writable");

    std::vector<float> out (512, 0.0f);
    float* outs[] = { out.data(), out.data() + 256 };

    // Driven until the write actually fails rather than for a fixed number of
    // blocks: how long a given filesystem takes to fill is a property of the
    // machine, and a fixed count is the timing assumption that has broken four
    // assertions in this repository already.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (60);

    while (! coordinator.hasCardWriteFailed()
           && std::chrono::steady_clock::now() < deadline)
    {
        coordinator.pullOutputBlock (outs, 2, 256);
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }

    check (coordinator.hasCardWriteFailed(),
           "the writer notices that the drive stopped accepting audio");

    const auto problem = coordinator.getCardWriteProblem();
    std::printf ("  Reported: '%s'\n", problem.c_str());

    check (! problem.empty(),
           "and gives an account of it rather than leaving the caller to guess");

    // The point of the whole exercise. Without an account the take stops under
    // the card-removal notice, whose words are "stopped responding" and "check
    // that it is plugged in properly" -- advice that cannot be followed,
    // because nothing is unplugged.
    check (problem.find ("full") != std::string::npos,
           "naming the drive as full");
    check (problem.find ("plugged in") == std::string::npos,
           "and not sending the user to check a cable that was never loose");

    // §10.6: what happened, then what to do.
    check (problem.find ("Free up space") != std::string::npos
               || problem.find ("bigger card") != std::string::npos,
           "and saying what to do about it");

    coordinator.stopRecording();
    coordinator.stopMonitoring();

    // §6.5 "finalize every open file", asked of the files rather than of the
    // app's own account of them.
    //
    // This is the half that mattered most and was never checked. The take
    // stopped, the app said every file had been closed, and two of three stems
    // carried RIFF size 0 and data size 0 -- about 2.4 seconds of real audio
    // each, on the card, that no player or DAW could reach. Saying "the drive
    // is full" while quietly making the surviving audio unopenable would have
    // been the worse of the two failures.
    for (const auto& channel : mics)
    {
        const auto path = dir + "/" + channel.fileName + ".wav";
        uint32_t frames = 0;

        // 24-bit mono stems: three bytes a frame.
        const bool describesItself = wavDescribesItsAudio (path, 3, frames);

        check (describesItself,
               channel.fileName + ".wav says how much audio it holds");
        check (describesItself && frames > 0,
               channel.fileName + ".wav offers a reader the audio that survived");

        std::printf ("  %s.wav: %u frames reachable\n", channel.fileName.c_str(), frames);
    }

    std::printf ("%s (%d failing)\n", failures == 0 ? "PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
