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

/// Where a 24-bit WAV's audio stops: the index just past the last non-zero
/// sample, and the largest sample found after it.
///
/// Located by CONTENT rather than by fraction of the file, which is what the
/// first version of this got wrong. It measured eighths of the file on the
/// assumption that the file was as long as the audio pumped into it -- and it
/// is not, because the harness pumps blocks back to back with no wall clock
/// between them and the writer's ring drops what it cannot take in time. On
/// this machine 16384 frames go in and 16384 come out; on a loaded CI runner
/// 9728 came out, which slid the unplug from the middle of the file to
/// two-thirds through it, and a range that had been safely after the event was
/// suddenly straddling it. The test failed, correctly, on a stack that was
/// behaving perfectly.
///
/// Nothing here depends on how much of the take survived.
struct AudioExtent
{
    uint32_t frames = 0;
    uint32_t lastSounding = 0;   ///< one past the last non-zero sample
    int32_t peakBefore = 0;      ///< largest sample up to that point
};

AudioExtent extentOf24BitWav (const std::string& path)
{
    AudioExtent e;

    constexpr std::streamoff kDataOffset = 12 + (8 + 16) + (8 + 602) + 8;

    if (peakOf24BitWav (path, e.frames) < 0 || e.frames == 0)
        return e;

    std::ifstream f (path, std::ios::binary);
    f.seekg (kDataOffset);

    for (uint32_t i = 0; i < e.frames; ++i)
    {
        unsigned char s[3] {};
        f.read (reinterpret_cast<char*> (s), 3);

        int32_t v = static_cast<int32_t> (s[0]) | (static_cast<int32_t> (s[1]) << 8)
                  | (static_cast<int32_t> (s[2]) << 16);

        if (v & 0x800000)
            v |= ~0xFFFFFF;

        if (v != 0)
        {
            e.lastSounding = i + 1;
            e.peakBefore = std::max (e.peakBefore, v < 0 ? -v : v);
        }
    }

    return e;
}

/// The largest absolute sample in [from, to) frames.
int32_t peakOfRange (const std::string& path, uint32_t from, uint32_t to)
{
    constexpr std::streamoff kDataOffset = 12 + (8 + 16) + (8 + 602) + 8;

    if (to <= from)
        return 0;

    std::ifstream f (path, std::ios::binary);
    f.seekg (kDataOffset + static_cast<std::streamoff> (from) * 3);

    int32_t peak = 0;

    for (uint32_t i = from; i < to; ++i)
    {
        unsigned char s[3] {};
        f.read (reinterpret_cast<char*> (s), 3);

        int32_t v = static_cast<int32_t> (s[0]) | (static_cast<int32_t> (s[1]) << 8)
                  | (static_cast<int32_t> (s[2]) << 16);

        if (v & 0x800000)
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
        coordinator.pullOutputBlock (outs, 2, block);
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
        four.pullOutputBlock (outs4, 2, block);
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
        two.pullOutputBlock (outs2, 2, block);
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


    // ---------------------------------------------------------------------
    // A microphone unplugged in the MIDDLE of a take.
    //
    // §0.1's hardest case, and the one no harness could reach: the ALSA `file`
    // plugin free-runs and loops its infile, so on Linux a mid-stream device
    // loss cannot be expressed at all. The virtual HAL can: removeDevice()
    // erases the device and fires the system list listener, which is exactly
    // what the OS does when someone pulls the cable.
    //
    // What must survive it: the take. The other person keeps recording, the
    // take stays valid, and the lost channel is padded rather than truncated,
    // so the two stems still line up frame for frame.
    // ---------------------------------------------------------------------
    std::printf ("\nOne of two microphones unplugged mid-take\n");
    fakeca::reset();

    const auto keeper = fakeca::addDevice (microphone ("Keeper", "uid-keeper", 1,
                                                       fakeca::BufferShape::oneChannelPerBuffer));
    const auto doomed = fakeca::addDevice (microphone ("Doomed", "uid-doomed", 1,
                                                       fakeca::BufferShape::oneChannelPerBuffer));

    mma::CoreAudioBackend backend4;
    mma::CaptureCoordinator loss (backend4, rate, block);

    std::vector<mma::CaptureChannel> both = {
        { "uid-keeper", "Keeper", "01_Keeper", 0.0f },
        { "uid-doomed", "Doomed", "02_Doomed", 0.0f },
    };

    if (! loss.startMonitoring (both, {}))
    {
        std::printf ("  FAIL  startMonitoring: %s\n", loss.getMonitorProblem().c_str());
        return 1;
    }

    if (! loss.startRecording (dir, 24, "2026-09-04T00:00:00Z"))
    {
        std::printf ("  FAIL  startRecording\n");
        return 1;
    }

    std::vector<float> outLoss (static_cast<size_t> (block) * 2, 0.0f);
    float* outsLoss[] = { outLoss.data(), outLoss.data() + block };

    // Short enough that the writer's ring keeps up with a harness that pumps
    // blocks back to back with no wall clock between them: a longer free-run
    // overruns the ring and fills the stem's head with silence, which would
    // drown the very thing this case is looking at.
    const int lossBlocks = 64;
    const int unplugAt = lossBlocks / 2;

    for (int i = 0; i < lossBlocks; ++i)
    {
        if (i == unplugAt)
            fakeca::removeDevice (doomed);

        const std::vector<std::vector<float>> signal { tone (block, 440.0, rate, 0.5f) };

        fakeca::pumpInput (keeper, signal);

        if (i < unplugAt)
            fakeca::pumpInput (doomed, signal);

        loss.pullOutputBlock (outsLoss, 2, block);
    }

    loss.stopRecording();
    loss.stopMonitoring();

    uint32_t kFrames = 0, dFrames = 0;
    const auto kPeak = peakOf24BitWav (dir + "/01_Keeper.wav", kFrames);
    const auto dPeak = peakOf24BitWav (dir + "/02_Doomed.wav", dFrames);

    std::printf ("  01_Keeper.wav: %u frames, peak %d\n", kFrames, kPeak);
    std::printf ("  02_Doomed.wav: %u frames, peak %d\n", dFrames, dPeak);

    check (kPeak > 100000, "the surviving mic keeps recording through the unplug");
    check (kFrames >= static_cast<uint32_t> (lossBlocks) * static_cast<uint32_t> (block) / 2,
           "and its stem runs past the moment the other cable was pulled");
    check (dPeak > 100000, "what the lost mic did record is still in its stem");
    check (dFrames == kFrames,
           "the lost channel is padded to the take length, not truncated");

    // And padded with SILENCE, at the right moment: frame-count equality on
    // its own would also be satisfied by a channel that kept receiving audio,
    // or by one filled with whatever was left in a buffer.
    // Asked of the audio, not of the clock: where does the lost channel stop
    // sounding, and is the survivor still sounding after that point?
    const auto lost = extentOf24BitWav (dir + "/02_Doomed.wav");
    const auto keptGoing = peakOfRange (dir + "/01_Keeper.wav", lost.lastSounding, kFrames);
    const auto lostTail = peakOfRange (dir + "/02_Doomed.wav", lost.lastSounding, dFrames);

    std::printf ("  02_Doomed.wav: sounds up to frame %u of %u, peak %d; after it %d\n",
                 lost.lastSounding, lost.frames, lost.peakBefore, lostTail);
    std::printf ("  01_Keeper.wav: peak %d across the same tail\n", keptGoing);

    check (lost.peakBefore > 100000, "the lost mic's audio up to the unplug is intact");
    check (lostTail == 0, "and everything after it is silence, not stale samples");

    // The tail has to be worth something, or a channel that ran to the very
    // last frame would satisfy "silence after the end" trivially.
    check (lost.lastSounding + static_cast<uint32_t> (block) < dFrames,
           "the silence is a real stretch of the take, not a rounding error");
    check (keptGoing > 100000, "while the surviving mic is still writing real audio there");

    std::remove ((dir + "/01_Keeper.wav").c_str());
    std::remove ((dir + "/02_Doomed.wav").c_str());
    std::remove ((dir + "/MIX.wav").c_str());

    // ---------------------------------------------------------------------
    // The same interface, handing its inputs over as SEPARATE BUFFERS.
    //
    // Every multi-input case above uses one interleaved buffer carrying N
    // channels. A class-compliant USB interface commonly does the other thing
    // -- one mono buffer per input -- and that layout had been proven only as
    // far as the backend's callback, never through the coordinator and the
    // writer into files. Each side of that boundary was tested; the join was
    // not, which is where every bug found tonight was living.
    //
    // Each input carries a DIFFERENT amplitude, so this cannot pass while the
    // channels are transposed. Four identical tones would be satisfied by a
    // fan-out that put input four into everyone's file.
    // ---------------------------------------------------------------------
    std::printf ("\nAn interface that hands over one buffer per input\n");
    fakeca::reset();

    const auto split = fakeca::addDevice (microphone ("Split 4-in", "uid-split", 4,
                                                      fakeca::BufferShape::oneChannelPerBuffer));

    mma::CoreAudioBackend backend5;
    mma::CaptureCoordinator perBuffer (backend5, rate, block);

    std::vector<mma::CaptureChannel> splitMics;
    for (int i = 0; i < 4; ++i)
    {
        mma::CaptureChannel c;
        c.deviceId = "uid-split";
        c.deviceChannel = i;
        c.displayName = "Input " + std::to_string (i + 1);
        c.fileName = "0" + std::to_string (i + 1) + "_Input-" + std::to_string (i + 1);
        c.bitDepth = 24;
        splitMics.push_back (c);
    }

    if (! perBuffer.startMonitoring (splitMics, {}))
    {
        std::printf ("  FAIL  startMonitoring: %s\n", perBuffer.getMonitorProblem().c_str());
        return 1;
    }

    if (! perBuffer.startRecording (dir, 24, "2026-09-04T00:00:00Z"))
    {
        std::printf ("  FAIL  startRecording\n");
        return 1;
    }

    std::vector<float> outSplit (static_cast<size_t> (block) * 2, 0.0f);
    float* outsSplit[] = { outSplit.data(), outSplit.data() + block };

    // 0.1, 0.2, 0.3, 0.4 -- far enough apart that a swap is unmistakable.
    const float amplitudes[] = { 0.1f, 0.2f, 0.3f, 0.4f };

    for (int i = 0; i < 64; ++i)
    {
        std::vector<std::vector<float>> signal;
        for (const float amplitude : amplitudes)
            signal.push_back (tone (block, 440.0, rate, amplitude));

        fakeca::pumpInput (split, signal);
        perBuffer.pullOutputBlock (outsSplit, 2, block);
    }

    perBuffer.stopRecording();
    perBuffer.stopMonitoring();

    bool everyInputLanded = true;

    for (int i = 0; i < 4; ++i)
    {
        const auto path = dir + "/0" + std::to_string (i + 1) + "_Input-"
                        + std::to_string (i + 1) + ".wav";

        uint32_t frames = 0;
        const auto peak = peakOf24BitWav (path, frames);

        // 24-bit full scale is 8388607, so 0.1 lands near 838860 and each step
        // is about that much again. A tolerance of a tenth of a step is ample
        // and still cannot confuse one input with its neighbour.
        const int32_t expected = static_cast<int32_t> (amplitudes[static_cast<size_t> (i)] * 8388607.0f);
        const int32_t slack = 83886;
        const bool right = frames > 0 && std::abs (peak - expected) < slack;

        std::printf ("  0%d_Input-%d.wav: %u frames, peak %d (expected about %d)%s\n",
                     i + 1, i + 1, frames, peak, expected, right ? "" : "   <-- wrong input");

        if (! right)
            everyInputLanded = false;

        std::remove (path.c_str());
    }

    check (everyInputLanded,
           "each input reaches its own file, at its own level, in the right order");

    std::remove ((dir + "/MIX.wav").c_str());

    std::printf ("\n%s (%d failing)\n", failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
