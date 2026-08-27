// Drives the REAL platform backend -- not a fake -- through the real capture
// coordinator, against ALSA devices the OS actually enumerates and opens.
//
// What this establishes that the fake-backend harness cannot: that
// IAudioBackend's contract survives contact with a real OS audio API, that
// enumeration/open/callback/teardown work on real audio threads the backend
// (not the test) creates, and that audio arriving through libasound lands in
// the right stem. Set up the virtual devices with Tools/setup_alsa_fixture.sh.
//
// What it does NOT establish: CoreAudio or WASAPI behaviour, and not real-time
// timing -- ALSA's file plugin delivers as fast as it is read, so the drift
// loop's fill error is not a wall-clock measurement here.
#include "Core/CaptureCoordinator.h"
#include "Platform/AlsaBackend.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

using namespace mma;

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr std::streamoff kDataSize = 12 + (8 + 16) + (8 + 602) + 4;
constexpr std::streamoff kAudio    = kDataSize + 4;

int failures = 0;

void check (bool ok, const char* what)
{
    std::printf ("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok) ++failures;
}

std::vector<double> decode16 (const std::string& path, uint32_t& dataBytes)
{
    std::ifstream f (path, std::ios::binary);
    if (! f.is_open()) { dataBytes = 0; return {}; }

    f.seekg (kDataSize);
    unsigned char b[4];
    f.read (reinterpret_cast<char*> (b), 4);
    dataBytes = b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);

    f.seekg (kAudio);
    std::vector<double> out;
    out.reserve (dataBytes / 2);

    for (uint32_t i = 0; i + 1 < dataBytes; i += 2)
    {
        unsigned char lo = 0, hi = 0;
        f.read (reinterpret_cast<char*> (&lo), 1);
        f.read (reinterpret_cast<char*> (&hi), 1);
        out.push_back (static_cast<int16_t> (lo | (hi << 8)) / 32768.0);
    }

    return out;
}

double toneEnergy (const std::vector<double>& x, double freq, double rate)
{
    if (x.size() < 2048) return 0.0;

    const size_t n = x.size();
    const double w = kTwoPi * freq / rate;
    const double coeff = 2.0 * std::cos (w);
    double s0 = 0, s1 = 0, s2 = 0;

    for (size_t i = 0; i < n; ++i) { s0 = x[i] + coeff * s1 - s2; s2 = s1; s1 = s0; }

    return std::sqrt (std::max (0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2)) / static_cast<double> (n);
}

} // namespace

int main (int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : "/tmp/live";
    std::filesystem::create_directories (dir);

    const double rate = 48000.0;
    const int block = 256;

    AlsaBackend backend;
    std::printf ("backend: %s\n\n", backend.getBackendName().c_str());

    // 1. Real enumeration through libasound.
    const auto inputs = backend.enumerateInputDevices();
    const auto outputs = backend.enumerateOutputDevices();

    auto hasDevice = [] (const std::vector<AudioDeviceDescriptor>& list, const std::string& id)
    {
        for (const auto& d : list)
            if (d.usbLocationId == id) return true;
        return false;
    };

    std::printf ("Enumeration (real, via snd_device_name_hint):\n");
    std::printf ("  %zu inputs, %zu outputs\n", inputs.size(), outputs.size());
    check (hasDevice (inputs, "mma_mic1"), "the OS reports mic 1");
    check (hasDevice (inputs, "mma_mic2"), "the OS reports mic 2");

    // 2. §5.4: a shared device must be refused, by name.
    const auto shared = backend.checkExclusiveModeCapability ("default", rate, block);
    std::printf ("\n§5.4 exclusive-mode gate:\n");
    check (! shared.exclusiveModeAvailable, "a shared output is refused");
    check (! shared.unavailableReason.empty(), "and the refusal names a cause");
    std::printf ("  reason: %s\n", shared.unavailableReason.c_str());

    // 3. Backend layer, measured directly. The callback delivers contiguous
    //    blocks, so accumulating them gives a clean signal whatever rate the
    //    device runs at -- which makes this the deterministic place to prove
    //    that audio from a real driver is correct and correctly routed.
    auto captureTone = [&] (const std::string& device, size_t wanted)
    {
        std::vector<float> collected;
        collected.reserve (wanted);
        std::mutex lock;

        AlsaBackend one;
        const bool opened = one.openInputStream (device, rate, block,
            [&] (const float* const* inputs, int numInputs, float* const*, int, int numSamples)
            {
                if (inputs == nullptr || numInputs < 1 || inputs[0] == nullptr)
                    return;

                std::lock_guard<std::mutex> guard (lock);

                if (collected.size() < wanted)
                    collected.insert (collected.end(), inputs[0], inputs[0] + numSamples);
            });

        if (! opened)
            return std::vector<double>{};

        for (int i = 0; i < 400; ++i)
        {
            {
                std::lock_guard<std::mutex> guard (lock);
                if (collected.size() >= wanted) break;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }

        one.closeAllStreams();

        std::lock_guard<std::mutex> guard (lock);
        return std::vector<double> (collected.begin(), collected.end());
    };

    std::printf ("\nAudio through the real driver (backend callback, contiguous):\n");

    const auto mic1 = captureTone ("mma_mic1", 48000);
    const auto mic2 = captureTone ("mma_mic2", 48000);

    const double c1_440 = toneEnergy (mic1, 440.0, rate), c1_1k = toneEnergy (mic1, 1000.0, rate);
    const double c2_440 = toneEnergy (mic2, 440.0, rate), c2_1k = toneEnergy (mic2, 1000.0, rate);

    std::printf ("  mic1: 440=%.4f 1k=%.4f   mic2: 440=%.4f 1k=%.4f  (%zu / %zu frames)\n",
                 c1_440, c1_1k, c2_440, c2_1k, mic1.size(), mic2.size());

    check (mic1.size() >= 48000 && mic2.size() >= 48000, "both devices delivered a full second");
    check (c1_440 > 0.1 && c1_440 > c1_1k * 20.0, "mic 1 delivers its own 440 Hz tone, cleanly");
    check (c2_1k  > 0.1 && c2_1k  > c2_440 * 20.0, "mic 2 delivers its own 1000 Hz tone, cleanly");

    // 4. Coordinator layer: the same real backend, driven through the real
    //    recording path. The fixture cannot rate-limit -- ALSA's file plugin
    //    delivers as fast as it is read -- so the capture ring floods and the
    //    stems carry real audio with gaps. That makes per-stem tone coherence
    //    unstable here, so this layer asserts what it can honestly support:
    //    that the whole stack runs on a real driver and writes real files.
    CaptureCoordinator coordinator (backend, rate, block);

    std::vector<CaptureChannel> mics = {
        { "mma_mic1", "Mic-440",  "01_Mic-440",  0.0f },
        { "mma_mic2", "Mic-1000", "02_Mic-1000", 0.0f },
    };

    std::printf ("\nFull stack on the real backend:\n");

    if (! coordinator.startMonitoring (mics, {}))
    {
        std::printf ("  FAIL  startMonitoring: %s\n", coordinator.getMonitorProblem().c_str());
        return 1;
    }

    check (true, "the backend opened both devices through libasound");

    if (! coordinator.startRecording (dir, 16, "2026-08-27T00:00:00Z"))
    {
        std::printf ("  FAIL  startRecording\n");
        return 1;
    }

    std::vector<float> out (static_cast<size_t> (block) * 2, 0.0f);
    float* outs[] = { out.data(), out.data() + block };

    const int blocks = static_cast<int> (rate * 2.0 / block); // ~2 seconds

    for (int i = 0; i < blocks; ++i)
    {
        coordinator.processOutputBlock (outs, 2, block);
        std::this_thread::sleep_for (std::chrono::microseconds (200));
    }

    coordinator.stopRecording();
    coordinator.stopMonitoring();

    uint32_t nA = 0, nB = 0, nM = 0;
    const auto stemA = decode16 (dir + "/01_Mic-440.wav", nA);
    const auto stemB = decode16 (dir + "/02_Mic-1000.wav", nB);
    const auto mix   = decode16 (dir + "/MIX.wav", nM);

    std::printf ("  files: A=%u B=%u MIX=%u bytes\n", nA, nB, nM);

    check (nA > 0 && nB > 0 && nM > 0, "all three files were written with audio");
    check (nA == nB && nB == nM,       "every stem and the mix are frame-locked");

    auto rms = [] (const std::vector<double>& x)
    {
        double sum = 0.0;
        for (double v : x) sum += v * v;
        return x.empty() ? 0.0 : std::sqrt (sum / static_cast<double> (x.size()));
    };

    check (rms (stemA) > 0.01 && rms (stemB) > 0.01 && rms (mix) > 0.01,
           "the stems and mix carry signal, not silence");

    std::printf ("\n%s (%d failing)\n", failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
