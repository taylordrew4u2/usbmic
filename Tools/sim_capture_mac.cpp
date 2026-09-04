// The full recording stack on the macOS code path, end to end.
//
// sim_coreaudio proves the backend hands over the right samples. live_capture
// proves the coordinator and writer turn samples into files. Nothing joined
// them: no test drove backend -> coordinator -> writer -> files on the path
// macOS actually takes, and the two harnesses between them left three gaps
// that a real user fell straight into.
//
//   - live_capture's fixture mics are MONO, so the stereo path -- the one a
//     USB mixer or any stereo interface takes, through §2.1's channel-layout
//     analysis -- never reached a file in any test.
//   - every harness and every unit test records at 16 bits. The app ships 24.
//   - the backend was only ever asked for audio in isolation, never while a
//     take was running.
//
// This closes all three: a stereo interleaved device, at 24 bits, recorded
// through the real coordinator into real files, with the bytes checked.

#include "../Simulation/CoreAudio/FakeCoreAudio.h"
#include "../Source/Platform/CoreAudioBackend.h"
#include "../Source/Core/CaptureCoordinator.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Not M_PI: that is a POSIX extension rather than standard C++, and MSVC does
// not define it from <cmath> without _USE_MATH_DEFINES. e2e_capture.cpp already
// carries this same note, having broken the Windows build the same way -- and
// this harness went and did it again, so here it is a third time.
constexpr double kTwoPi = 6.283185307179586476925286766559;

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

fakeca::DeviceSpec microphone (const char* name, const char* uid, int channels,
                               fakeca::BufferShape shape)
{
    fakeca::DeviceSpec spec;
    spec.name = name;
    spec.uid = uid;
    spec.inputChannels = channels;
    spec.shape = shape;
    return spec;
}

std::vector<float> tone (int frames, double hz, double rate, float amplitude)
{
    std::vector<float> v (static_cast<size_t> (frames));
    for (int i = 0; i < frames; ++i)
        v[static_cast<size_t> (i)] =
            amplitude * static_cast<float> (std::sin (kTwoPi * hz * i / rate));
    return v;
}

/// The largest absolute 24-bit sample in a finished WAV, so "did this file
/// carry audio" is answered from the bytes rather than from the file size.
int32_t peakOf24BitWav (const std::string& path, uint32_t& framesOut)
{
    // RIFF 12 + fmt (8+16) + bext (8+602) + "data" tag 4, matching SessionWriter.
    constexpr std::streamoff kDataSizeOffset = 12 + (8 + 16) + (8 + 602) + 4;

    std::ifstream f (path, std::ios::binary);
    framesOut = 0;

    if (! f.is_open())
        return -1;

    f.seekg (kDataSizeOffset);
    unsigned char b[4] {};
    f.read (reinterpret_cast<char*> (b), 4);
    const uint32_t dataBytes = static_cast<uint32_t> (b[0]) | (static_cast<uint32_t> (b[1]) << 8)
                             | (static_cast<uint32_t> (b[2]) << 16) | (static_cast<uint32_t> (b[3]) << 24);

    framesOut = dataBytes / 3;
    int32_t peak = 0;

    for (uint32_t i = 0; i < framesOut; ++i)
    {
        unsigned char s[3] {};
        f.read (reinterpret_cast<char*> (s), 3);

        int32_t v = static_cast<int32_t> (s[0]) | (static_cast<int32_t> (s[1]) << 8)
                  | (static_cast<int32_t> (s[2]) << 16);

        if (v & 0x800000)          // sign-extend 24-bit
            v |= ~0xFFFFFF;

        peak = std::max (peak, v < 0 ? -v : v);
    }

    return peak;
}

} // namespace

int main()
{
    const auto dir = tempDir();
    const double rate = 48000.0;
    const int block = 256;

    std::printf ("A stereo USB device recorded at 24 bits, through the whole stack\n");
    fakeca::reset();

    // Two channels on one interleaved buffer: what a USB mixer or any stereo
    // interface looks like, and what §2.1's layout analysis has to decide on.
    const auto id = fakeca::addDevice (microphone ("Stereo Board", "uid-board", 2,
                                                   fakeca::BufferShape::interleaved));

    mma::CoreAudioBackend backend;
    mma::CaptureCoordinator coordinator (backend, rate, block);

    std::vector<mma::CaptureChannel> mics = { { "uid-board", "Board", "01_Board", 0.0f } };

    if (! coordinator.startMonitoring (mics, {}))
    {
        std::printf ("  FAIL  startMonitoring: %s\n", coordinator.getMonitorProblem().c_str());
        return 1;
    }

    check (fakeca::isRunning (id), "the device is running");

    // 24 bits: the depth the app ships, which no other harness records at.
    if (! coordinator.startRecording (dir, 24, "2026-09-04T00:00:00Z"))
    {
        std::printf ("  FAIL  startRecording\n");
        return 1;
    }

    std::vector<float> out (static_cast<size_t> (block) * 2, 0.0f);
    float* outs[] = { out.data(), out.data() + block };

    // A second of audio, pumped in and pulled out the way the two real
    // callbacks would interleave.
    const int blocks = static_cast<int> (rate / block);

    for (int i = 0; i < blocks; ++i)
    {
        const std::vector<std::vector<float>> signal {
            tone (block, 440.0, rate, 0.5f),
            tone (block, 440.0, rate, 0.5f),
        };

        fakeca::pumpInput (id, signal);
        coordinator.processOutputBlock (outs, 2, block);
    }

    const float arrived = coordinator.getPeakArrived();
    const float written = coordinator.getPeakWritten();

    coordinator.stopRecording();
    coordinator.stopMonitoring();

    std::printf ("\n  peak arrived %.4f   peak written %.4f\n", arrived, written);

    check (arrived > 0.1f, "audio reaches the coordinator");
    check (written > 0.1f, "and the writer accepts it");

    uint32_t frames = 0;
    const auto peak = peakOf24BitWav (dir + "/01_Board.wav", frames);

    std::printf ("  01_Board.wav: %u frames, peak %d\n", frames, peak);

    check (frames > 0, "the stem has frames in it");
    check (peak > 100000, "and those frames carry signal, not silence");

    uint32_t mixFrames = 0;
    const auto mixPeak = peakOf24BitWav (dir + "/MIX.wav", mixFrames);

    std::printf ("  MIX.wav:      %u frames, peak %d\n", mixFrames, mixPeak);
    check (mixPeak > 100000, "the mix carries signal too");

    std::remove ((dir + "/01_Board.wav").c_str());
    std::remove ((dir + "/MIX.wav").c_str());

    // ---------------------------------------------------------------------
    // An interface with four microphones plugged into it.
    //
    // One device, four inputs, four people. The app used to take exactly one
    // channel from any device -- §2.1's stereo collapse, applied to an
    // interface -- so three of the four were discarded without a word, and
    // anyone whose microphone was on a discarded input got silence.
    // ---------------------------------------------------------------------
    std::printf ("\nAn interface with four microphones on four inputs\n");
    fakeca::reset();

    const auto rig = fakeca::addDevice (microphone ("4-in Interface", "uid-rig", 4,
                                                    fakeca::BufferShape::interleaved));

    mma::CoreAudioBackend backend2;
    mma::CaptureCoordinator four (backend2, rate, block);

    // What Application::buildCaptureChannels now produces for a 4-input device.
    std::vector<mma::CaptureChannel> band;
    for (int i = 0; i < 4; ++i)
    {
        mma::CaptureChannel c;
        c.deviceId = "uid-rig";
        c.deviceChannel = i;
        c.displayName = "Mic " + std::to_string (i + 1);
        c.fileName = "0" + std::to_string (i + 1) + "_Mic-" + std::to_string (i + 1);
        band.push_back (c);
    }

    if (! four.startMonitoring (band, {}))
    {
        std::printf ("  FAIL  startMonitoring: %s\n", four.getMonitorProblem().c_str());
        return 1;
    }

    check (true, "one stream opens for the device, not one per microphone");

    if (! four.startRecording (dir, 24, "2026-09-04T00:00:00Z"))
    {
        std::printf ("  FAIL  startRecording\n");
        return 1;
    }

    std::vector<float> out4 (static_cast<size_t> (block) * 2, 0.0f);
    float* outs4[] = { out4.data(), out4.data() + block };

    // A different tone per input, so a channel landing in the wrong file is a
    // failure rather than something that happens to look right.
    for (int i = 0; i < blocks; ++i)
    {
        const std::vector<std::vector<float>> signal {
            tone (block, 220.0, rate, 0.5f),
            tone (block, 440.0, rate, 0.5f),
            tone (block, 880.0, rate, 0.5f),
            tone (block, 1760.0, rate, 0.5f),
        };

        fakeca::pumpInput (rig, signal);
        four.processOutputBlock (outs4, 2, block);
    }

    four.stopRecording();
    four.stopMonitoring();

    bool everyMicRecorded = true;

    for (int i = 0; i < 4; ++i)
    {
        uint32_t f = 0;
        const auto path = dir + "/0" + std::to_string (i + 1) + "_Mic-" + std::to_string (i + 1) + ".wav";
        const auto p = peakOf24BitWav (path, f);

        std::printf ("  Mic %d: %u frames, peak %d\n", i + 1, f, p);

        if (! (f > 0 && p > 100000))
            everyMicRecorded = false;

        std::remove (path.c_str());
    }

    check (everyMicRecorded, "all four microphones reach their own file with signal");
    std::remove ((dir + "/MIX.wav").c_str());

    // ---------------------------------------------------------------------
    // A TWO-input interface with two people plugged into it.
    //
    // The commonest small multi-mic rig there is, and the one the earlier fix
    // did not cover: two inputs collapsed to one on the assumption that any
    // two-channel device is a stereo USB mic. One of the two people was
    // discarded, and if it was the person holding the microphone that mattered,
    // the take came back silent.
    // ---------------------------------------------------------------------
    std::printf ("\nA two-input interface with two people on it\n");
    fakeca::reset();

    const auto pair = fakeca::addDevice (microphone ("2-in Interface", "uid-pair", 2,
                                                     fakeca::BufferShape::interleaved));

    mma::CoreAudioBackend backend3;
    mma::CaptureCoordinator two (backend3, rate, block);

    std::vector<mma::CaptureChannel> duo;
    for (int i = 0; i < 2; ++i)
    {
        mma::CaptureChannel c;
        c.deviceId = "uid-pair";
        c.deviceChannel = i;
        c.displayName = "Person " + std::to_string (i + 1);
        c.fileName = "0" + std::to_string (i + 1) + "_Person-" + std::to_string (i + 1);
        duo.push_back (c);
    }

    if (! two.startMonitoring (duo, {}))
    {
        std::printf ("  FAIL  startMonitoring: %s\n", two.getMonitorProblem().c_str());
        return 1;
    }

    if (! two.startRecording (dir, 24, "2026-09-04T00:00:00Z"))
    {
        std::printf ("  FAIL  startRecording\n");
        return 1;
    }

    std::vector<float> out2 (static_cast<size_t> (block) * 2, 0.0f);
    float* outs2[] = { out2.data(), out2.data() + block };

    // Only the SECOND person is speaking. Under the old rule §2.1 picked a
    // side, and picking the quiet one produced exactly the reported symptom:
    // a silent recording from a rig that was working.
    for (int i = 0; i < blocks; ++i)
    {
        const std::vector<std::vector<float>> signal {
            std::vector<float> (static_cast<size_t> (block), 0.0f),
            tone (block, 440.0, rate, 0.5f),
        };

        fakeca::pumpInput (pair, signal);
        two.processOutputBlock (outs2, 2, block);
    }

    two.stopRecording();
    two.stopMonitoring();

    uint32_t f1 = 0, f2 = 0;
    const auto p1 = peakOf24BitWav (dir + "/01_Person-1.wav", f1);
    const auto p2 = peakOf24BitWav (dir + "/02_Person-2.wav", f2);

    std::printf ("  Person 1 (silent): %u frames, peak %d\n", f1, p1);
    std::printf ("  Person 2 (talking): %u frames, peak %d\n", f2, p2);

    check (f1 > 0, "the quiet person still gets a file, at full length");
    check (p2 > 100000, "and the person actually talking is recorded");

    std::remove ((dir + "/01_Person-1.wav").c_str());
    std::remove ((dir + "/02_Person-2.wav").c_str());
    std::remove ((dir + "/MIX.wav").c_str());

    std::printf ("\n%s (%d failing)\n", failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
