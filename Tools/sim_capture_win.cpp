// The full recording stack on the Windows code path, end to end.
//
// sim_wasapi proves the backend hands over the right samples. live_capture and
// e2e_capture prove the coordinator and writer turn samples into files. Nothing
// joined them on Windows -- and that is not a hypothetical gap, it is the exact
// one that let "a stereo device recording at 24 bits" reach a user untested on
// macOS, which is why sim_capture_mac was written. The same reasoning applied
// to Windows the whole time and nobody applied it: of every harness in this
// repository, not one put WasapiAsioBackend and CaptureCoordinator in the same
// process.
//
// So this is sim_capture_mac's argument on the other platform. The backend is
// the shipping backend, compiled unmodified; the coordinator and the writer are
// the shipping ones; only the operating system underneath is fake. What comes
// out are real WAV files, and the bytes are checked.

#include "../Simulation/Wasapi/FakeWasapi.h"
#include "../Source/Platform/WasapiAsioBackend.h"
#include "../Source/Core/CaptureCoordinator.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// Not M_PI: a POSIX extension MSVC does not define from <cmath> without
// _USE_MATH_DEFINES. Three harnesses in this repository have broken the Windows
// build on exactly this, so it is spelled out here too.
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

fakewasapi::EndpointSpec microphone (const std::string& id, const std::string& name,
                                     int channels, int bits)
{
    fakewasapi::EndpointSpec spec;
    spec.id = id;
    spec.friendlyName = name;
    spec.isCapture = true;
    spec.exclusiveFormats = { fakewasapi::Format::pcm (channels, bits, 48000.0) };
    spec.mixFormat = spec.exclusiveFormats.front();
    return spec;
}

fakewasapi::EndpointSpec headphones (const std::string& id, const std::string& name,
                                     int channels, int bits)
{
    fakewasapi::EndpointSpec spec;
    spec.id = id;
    spec.friendlyName = name;
    spec.isCapture = false;
    spec.exclusiveFormats = { fakewasapi::Format::pcm (channels, bits, 48000.0) };
    spec.mixFormat = spec.exclusiveFormats.front();
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

/// Frames and peak of a finished 24-bit WAV, walking the chunks rather than
/// assuming an offset: "data" also occurs inside the 602-byte bext chunk, and
/// searching the bytes for it finds that one first. (Asked how I know.)
bool inspect24BitWav (const std::string& path, uint32_t& frames, int32_t& peak)
{
    frames = 0;
    peak = 0;

    std::FILE* f = std::fopen (path.c_str(), "rb");

    if (f == nullptr)
        return false;

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

        peak = std::max (peak, v < 0 ? -v : v);
    }

    std::fclose (f);
    return true;
}

} // namespace

int main()
{
    const auto dir = tempDir();
    const double rate = 48000.0;
    const int block = 256;

    // -----------------------------------------------------------------------
    // A stereo USB device at 24 bits, through the whole stack.
    //
    // Deliberately the same case sim_capture_mac opens with, because it is the
    // one that reached a user untested: a stereo interface, at the depth the
    // app actually ships, recorded to files nobody had ever read back on this
    // platform.
    // -----------------------------------------------------------------------
    std::printf ("A stereo USB device recorded at 24 bits, through the whole stack\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (microphone ("mic-board", "Stereo Board", 2, 24));

    mma::WasapiAsioBackend backend;
    mma::CaptureCoordinator coordinator (backend, rate, block);

    std::vector<mma::CaptureChannel> mics;

    for (int i = 0; i < 2; ++i)
    {
        mma::CaptureChannel c;
        c.deviceId = "mic-board";
        c.deviceChannel = i;
        c.displayName = "Side " + std::to_string (i + 1);
        c.fileName = "0" + std::to_string (i + 1) + "_Side-" + std::to_string (i + 1);
        c.bitDepth = 24;
        mics.push_back (c);
    }

    if (! coordinator.startMonitoring (mics, {}))
    {
        std::printf ("  FAIL  startMonitoring: %s\n", coordinator.getMonitorProblem().c_str());
        return 1;
    }

    check (fakewasapi::isRunning ("mic-board"), "the device is running");
    check (fakewasapi::openedExclusive ("mic-board"),
           "and it was opened in exclusive mode, as §5.4 requires");

    if (! coordinator.startRecording (dir, 24, "2026-09-15T00:00:00Z"))
    {
        std::printf ("  FAIL  startRecording\n");
        return 1;
    }

    std::vector<float> out (static_cast<size_t> (block) * 2, 0.0f);
    float* outs[] = { out.data(), out.data() + block };

    // Two different amplitudes, because a stereo device is the case where a
    // fan-out that collapses both sides to one is invisible if both sides
    // carry the same thing. §2.1 is exactly about not doing that.
    const int blocks = 96;

    for (int i = 0; i < blocks; ++i)
    {
        const std::vector<std::vector<float>> signal {
            tone (block, 440.0, rate, 0.2f),
            tone (block, 440.0, rate, 0.4f),
        };

        fakewasapi::pushCapture ("mic-board", signal);
        coordinator.pullOutputBlock (outs, 2, block);
    }

    coordinator.stopRecording();
    coordinator.stopMonitoring();

    uint32_t framesL = 0, framesR = 0;
    int32_t peakL = 0, peakR = 0;

    const auto pathL = dir + "/01_Side-1.wav";
    const auto pathR = dir + "/02_Side-2.wav";

    check (inspect24BitWav (pathL, framesL, peakL), "the first side has a file");
    check (inspect24BitWav (pathR, framesR, peakR), "the second side has a file");

    std::printf ("  01_Side-1.wav: %u frames, peak %d\n", framesL, peakL);
    std::printf ("  02_Side-2.wav: %u frames, peak %d\n", framesR, peakR);

    check (framesL > 0 && framesL == framesR,
           "both sides are the same length, so the take lines up");

    // 24-bit full scale is 8388607, so 0.2 lands near 1677721 and 0.4 near
    // 3355442. Checked apart rather than merely non-zero: two sides that both
    // came out at one amplitude would be §2.1's collapse wearing a disguise.
    const int32_t slack = 168000;
    check (std::abs (peakL - 1677721) < slack, "the quieter side is recorded at its own level");
    check (std::abs (peakR - 3355442) < slack, "and the louder side at its own");

    std::remove (pathL.c_str());
    std::remove (pathR.c_str());
    std::remove ((dir + "/MIX.wav").c_str());

    // -----------------------------------------------------------------------
    // A four-input interface, one performer per input.
    //
    // This is #94's case -- "a mixer or interface gave you input one and
    // nothing else" -- asked of the third platform. That bug was ALSA's, and
    // CoreAudio was shown clean; WASAPI had never been asked at all, because
    // until this harness existed there was nothing on Windows that could carry
    // an interface's inputs all the way to files.
    //
    // Four distinct amplitudes again. Four identical tones would be satisfied
    // by a fan-out that gave everyone input four, which is the same failure
    // #94 fixed wearing better clothes.
    // -----------------------------------------------------------------------
    std::printf ("\nA four-input interface with four people on it\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (microphone ("mic-rig", "4-in Interface", 4, 24));

    mma::WasapiAsioBackend backend2;
    mma::CaptureCoordinator rig (backend2, rate, block);

    std::vector<mma::CaptureChannel> four;

    for (int i = 0; i < 4; ++i)
    {
        mma::CaptureChannel c;
        c.deviceId = "mic-rig";
        c.deviceChannel = i;
        c.displayName = "Person " + std::to_string (i + 1);
        c.fileName = "0" + std::to_string (i + 1) + "_Person-" + std::to_string (i + 1);
        c.bitDepth = 24;
        four.push_back (c);
    }

    if (! rig.startMonitoring (four, {}))
    {
        std::printf ("  FAIL  startMonitoring: %s\n", rig.getMonitorProblem().c_str());
        return 1;
    }

    check (fakewasapi::negotiatedFormat ("mic-rig").channels == 4,
           "all four inputs were negotiated, not one");

    if (! rig.startRecording (dir, 24, "2026-09-15T00:00:00Z"))
    {
        std::printf ("  FAIL  startRecording\n");
        return 1;
    }

    std::vector<float> outRig (static_cast<size_t> (block) * 2, 0.0f);
    float* outsRig[] = { outRig.data(), outRig.data() + block };

    const float amplitudes[] = { 0.1f, 0.2f, 0.3f, 0.4f };

    for (int i = 0; i < blocks; ++i)
    {
        std::vector<std::vector<float>> signal;

        for (const float amplitude : amplitudes)
            signal.push_back (tone (block, 440.0, rate, amplitude));

        fakewasapi::pushCapture ("mic-rig", signal);
        rig.pullOutputBlock (outsRig, 2, block);
    }

    rig.stopRecording();
    rig.stopMonitoring();

    bool everyoneLanded = true;

    for (int i = 0; i < 4; ++i)
    {
        const auto path = dir + "/0" + std::to_string (i + 1) + "_Person-"
                        + std::to_string (i + 1) + ".wav";

        uint32_t frames = 0;
        int32_t peak = 0;
        const bool read = inspect24BitWav (path, frames, peak);

        const int32_t expected = static_cast<int32_t> (amplitudes[static_cast<size_t> (i)] * 8388607.0f);
        const bool right = read && frames > 0 && std::abs (peak - expected) < 84000;

        std::printf ("  0%d_Person-%d.wav: %u frames, peak %d (expected about %d)%s\n",
                     i + 1, i + 1, frames, peak, expected, right ? "" : "   <-- wrong input");

        if (! right)
            everyoneLanded = false;

        std::remove (path.c_str());
    }

    check (everyoneLanded,
           "each person reaches their own file, at their own level, in the right order");

    std::remove ((dir + "/MIX.wav").c_str());

    // -----------------------------------------------------------------------
    // The headphones are unplugged mid-take, on Windows this time.
    //
    // The take is clocked by the output callback on every platform, so losing
    // the headphones stops the clock on every platform. The coordinator's
    // software clock is what picks it up, and it is shared -- but the half that
    // is NOT shared is the backend: WASAPI's render thread has to die and let
    // go rather than wedge, and until this harness existed nothing joined that
    // to a real take.
    // -----------------------------------------------------------------------
    std::printf ("\nThe headphones are unplugged in the middle of a take\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (microphone ("mic-singer", "Singer", 1, 24));
    fakewasapi::addEndpoint (headphones ("out-cans", "Headphones", 2, 24));

    mma::WasapiAsioBackend backend3;
    mma::CaptureCoordinator monitored (backend3, rate, block);

    std::vector<mma::CaptureChannel> singerOnly;
    {
        mma::CaptureChannel c;
        c.deviceId = "mic-singer";
        c.deviceChannel = 0;
        c.displayName = "Singer";
        c.fileName = "01_Singer";
        c.bitDepth = 24;
        singerOnly.push_back (c);
    }

    if (! monitored.startMonitoring (singerOnly, "out-cans"))
    {
        std::printf ("  FAIL  startMonitoring: %s\n", monitored.getMonitorProblem().c_str());
        return 1;
    }

    check (monitored.hasOutputStream(), "the headphones are open and clocking the take");

    if (! monitored.startRecording (dir, 24, "2026-09-15T00:00:00Z"))
    {
        std::printf ("  FAIL  startRecording\n");
        return 1;
    }

    // The OUTPUT pulls, as it does in the app.
    std::vector<std::vector<float>> rendered;

    for (int i = 0; i < 24; ++i)
    {
        fakewasapi::pushCapture ("mic-singer", { tone (block, 440.0, rate, 0.4f) });
        fakewasapi::pullRender ("out-cans", rendered);
    }

    const auto framesBefore = monitored.getFramesAccepted();
    check (framesBefore > 0, "the take is advancing while the headphones are there");

    fakewasapi::removeEndpoint ("out-cans");

    // Only the microphone from here. Nothing pulls the output, because there
    // is no output; if the take depends on it, this is where it dies.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (900);

    while (std::chrono::steady_clock::now() < until)
    {
        fakewasapi::pushCapture ("mic-singer", { tone (block, 440.0, rate, 0.4f) });
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }

    const auto framesAfter = monitored.getFramesAccepted();

    monitored.stopRecording();
    monitored.stopMonitoring();

    std::printf ("  accepted %llu frames before the unplug, %llu after\n",
                 (unsigned long long) framesBefore, (unsigned long long) framesAfter);

    check (framesAfter > framesBefore,
           "the take keeps advancing once the headphones are gone");

    uint32_t singerFrames = 0;
    int32_t singerPeak = 0;
    const auto singerPath = dir + "/01_Singer.wav";

    check (inspect24BitWav (singerPath, singerFrames, singerPeak),
           "and the singer still has a file");

    std::printf ("  01_Singer.wav: %u frames, peak %d\n", singerFrames, singerPeak);
    check (singerPeak > 100000, "with their audio in it, not silence");

    std::remove (singerPath.c_str());
    std::remove ((dir + "/MIX.wav").c_str());

    std::printf ("\n%s (%d failing)\n", failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
