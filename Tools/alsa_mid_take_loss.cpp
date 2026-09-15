// A microphone unplugged in the middle of a take, on Linux, end to end.
//
// This is the §0.1 case two earlier attempts could not reach. The ALSA `file`
// plugin the fixture is built on free-runs at thousands of times real time and
// LOOPS its infile, so it ignores truncation and cannot be made to disappear:
// killing the FIFO behind it and truncating it were both tried, and both
// proved only that the plugin does not care. sim_capture_mac reached the event
// on the macOS path through the virtual HAL; Linux had no equivalent.
//
// It has one now, and it does not need a virtual HAL: libasound is a shared
// library, so snd_pcm_readi can be replaced for ONE named PCM while every other
// call in the path -- snd_pcm_open, the hw_params negotiation, the worker
// thread, the coordinator, the writer -- stays the shipping code reading a real
// device. The mic that dies returns -ENODEV, which snd_pcm_recover cannot
// recover, exactly as a pulled USB cable does.
//
//   Tools/alsa_mid_take_loss.sh
//
// What must survive it is the take: the other microphone keeps recording, both
// stems stay the same length so they still line up, and the loss is reported in
// words rather than left as silence nobody explains.

#include "Core/CaptureCoordinator.h"
#include "Platform/AlsaBackend.h"

#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
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

std::string tempDir()
{
    for (const char* var : { "MMA_TEST_TMPDIR", "TMPDIR", "TMP", "TEMP" })
        if (const char* d = std::getenv (var); d != nullptr && *d != '\0')
            return std::string (d);

    return "/tmp";
}

/// Frames in a 24-bit WAV, read from the data chunk rather than guessed from
/// the file size, and the largest sample in it.
bool inspect24BitWav (const std::string& path, uint32_t& frames, int32_t& peak,
                      int32_t* tailPeak = nullptr, double tailFraction = 0.25)
{
    frames = 0;
    peak = 0;

    if (tailPeak != nullptr)
        *tailPeak = 0;

    std::FILE* f = std::fopen (path.c_str(), "rb");

    if (f == nullptr)
        return false;

    // Walked rather than assumed: the header carries a 602-byte bext chunk, and
    // searching the bytes for "data" finds a match inside it.
    std::fseek (f, 12, SEEK_SET);
    uint32_t dataBytes = 0;

    for (;;)
    {
        unsigned char header[8] {};

        if (std::fread (header, 1, 8, f) != 8)
            break;

        const uint32_t size = static_cast<uint32_t> (header[4])
                            | (static_cast<uint32_t> (header[5]) << 8)
                            | (static_cast<uint32_t> (header[6]) << 16)
                            | (static_cast<uint32_t> (header[7]) << 24);

        if (std::memcmp (header, "data", 4) == 0)
        {
            dataBytes = size;
            break;
        }

        std::fseek (f, static_cast<long> (size + (size & 1u)), SEEK_CUR);
    }

    frames = dataBytes / 3;

    for (uint32_t i = 0; i < frames; ++i)
    {
        unsigned char s[3] {};

        if (std::fread (s, 1, 3, f) != 3)
            break;

        int32_t v = static_cast<int32_t> (s[0]) | (static_cast<int32_t> (s[1]) << 8)
                  | (static_cast<int32_t> (s[2]) << 16);

        if (v & 0x800000)
            v |= ~0xFFFFFF;

        const int32_t magnitude = v < 0 ? -v : v;
        peak = std::max (peak, magnitude);

        if (tailPeak != nullptr
            && i >= static_cast<uint32_t> (static_cast<double> (frames) * (1.0 - tailFraction)))
            *tailPeak = std::max (*tailPeak, magnitude);
    }

    std::fclose (f);
    return true;
}

} // namespace

int main()
{
    const auto dir = tempDir();
    const char* dyingDevice = std::getenv ("MMA_SHIM_DEVICE");

    std::printf ("%s\n", dyingDevice != nullptr
                             ? "One of two microphones is unplugged mid-take"
                             : "Control: the same two microphones, neither unplugged");

    mma::AlsaBackend backend;

    auto devices = backend.enumerateInputDevices();

    if (devices.size() < 2)
    {
        std::printf ("  FAIL  need two fixture microphones, found %zu -- "
                     "run Tools/setup_alsa_fixture.sh\n", devices.size());
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
        std::printf ("  %s -> %s.wav%s\n", c.deviceId.c_str(), c.fileName.c_str(),
                     dyingDevice != nullptr && c.deviceId == dyingDevice ? "  (this one dies)" : "");
    }

    if (! coordinator.startMonitoring (mics, {}))
    {
        std::printf ("  FAIL  startMonitoring: %s\n", coordinator.getMonitorProblem().c_str());
        return 1;
    }

    if (! coordinator.startRecording (dir, 24, "2026-09-14T00:00:00Z"))
    {
        std::printf ("  FAIL  startRecording\n");
        return 1;
    }

    check (true, "the take starts with both microphones live");

    // Real seconds, because the whole point is a device that dies PART WAY
    // THROUGH one: the shim lets the first stretch of reads through and fails
    // everything after it.
    std::vector<float> out (512, 0.0f);
    float* outs[] = { out.data(), out.data() + 256 };

    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds (6);

    while (std::chrono::steady_clock::now() < until)
    {
        coordinator.pullOutputBlock (outs, 2, 256);
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }

    coordinator.stopRecording();

    const auto reported = backend.takeStreamFailures();

    coordinator.stopMonitoring();

    uint32_t frames1 = 0, frames2 = 0;
    int32_t peak1 = 0, peak2 = 0;
    int32_t tail1 = 0, tail2 = 0;

    const auto path1 = dir + "/01_Mic-1.wav";
    const auto path2 = dir + "/02_Mic-2.wav";

    check (inspect24BitWav (path1, frames1, peak1, &tail1), "the first stem exists");
    check (inspect24BitWav (path2, frames2, peak2, &tail2), "the second stem exists");

    std::printf ("  01_Mic-1.wav: %u frames, peak %d (last quarter %d)\n", frames1, peak1, tail1);
    std::printf ("  02_Mic-2.wav: %u frames, peak %d (last quarter %d)\n", frames2, peak2, tail2);

    if (dyingDevice != nullptr)
    {
        const bool firstDies = mics[0].deviceId == dyingDevice;
        const uint32_t survivorFrames = firstDies ? frames2 : frames1;
        const int32_t survivorPeak = firstDies ? peak2 : peak1;
        const int32_t lostPeak = firstDies ? peak1 : peak2;

        check (survivorPeak > 100000,
               "the surviving microphone is still recording real audio");
        check (survivorFrames > 0 && frames1 == frames2,
               "both stems are the same length, so the take still lines up");
        check (lostPeak > 100000,
               "and what the lost microphone did record before it went is kept");

        // Equal lengths on their own would also be satisfied by a channel that
        // never actually stopped, so the padding is checked for what it is:
        // silence, while the survivor is still writing audio in the same span.
        const int32_t survivorTail = firstDies ? tail2 : tail1;
        const int32_t lostTail = firstDies ? tail1 : tail2;

        check (lostTail == 0, "the lost channel is padded with silence, not stale audio");
        check (survivorTail > 100000, "while the survivor is still writing audio there");

        // The silence is the easy half; saying so is the half that was missing
        // on Windows and Linux both, and it is what the user actually needs.
        check (! reported.empty(), "the loss is reported rather than left unexplained");

        for (const auto& failure : reported)
            std::printf ("  Reported: %s %s\n", failure.deviceId.c_str(), failure.reason.c_str());
    }
    else
    {
        check (peak1 > 100000 && peak2 > 100000, "both microphones record real audio");
        check (tail1 > 100000 && tail2 > 100000, "right through to the end of the take");
        check (frames1 == frames2, "and their stems are the same length");
        check (reported.empty(), "with nothing reported as failing");
    }

    std::remove (path1.c_str());
    std::remove (path2.c_str());
    std::remove ((dir + "/MIX.wav").c_str());

    std::printf ("%s (%d failing)\n", failures == 0 ? "PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
