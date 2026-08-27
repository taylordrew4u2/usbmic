// End-to-end drive of the real capture path with real audio, no GUI, no OS
// audio calls: two mics on deliberately mismatched clocks, recorded to disk,
// then the files are decoded and checked.
#include "Core/CaptureCoordinator.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace mma;

namespace {

// Not M_PI: that is a POSIX extension rather than standard C++, and MSVC does
// not define it from <cmath> without _USE_MATH_DEFINES. Relying on it is how
// this harness broke the Windows build.
constexpr double kTwoPi = 6.283185307179586476925286766559;

class Fake : public IAudioBackend {
public:
    std::vector<AudioCallback> inputs;
    AudioCallback output;
    std::string getBackendName() const override { return "Fake"; }
    std::vector<AudioDeviceDescriptor> enumerateInputDevices() override { return {}; }
    std::vector<AudioDeviceDescriptor> enumerateOutputDevices() override { return {}; }
    void setDeviceChangeCallback (DeviceChangeCallback) override {}
    ExclusiveModeCapability checkExclusiveModeCapability (const std::string&, double, int) override {
        ExclusiveModeCapability c; c.exclusiveModeAvailable = true; return c; }
    bool openExclusiveOutputStream (const std::string&, double, int, AudioCallback cb) override {
        output = std::move (cb); return true; }
    bool openInputStream (const std::string&, double, int, AudioCallback cb) override {
        inputs.push_back (std::move (cb)); return true; }
    void closeAllStreams() override {}
};

constexpr std::streamoff kDataSize = 12 + (8 + 16) + (8 + 602) + 4;
constexpr std::streamoff kAudio    = kDataSize + 4;

std::vector<double> decode16 (const std::string& path, uint32_t& dataBytes) {
    std::ifstream f (path, std::ios::binary);
    if (! f.is_open()) { dataBytes = 0; return {}; }
    f.seekg (kDataSize);
    unsigned char b[4]; f.read (reinterpret_cast<char*> (b), 4);
    dataBytes = b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
    f.seekg (kAudio);
    std::vector<double> out;
    out.reserve (dataBytes / 2);
    for (uint32_t i = 0; i + 1 < dataBytes; i += 2) {
        unsigned char lo = 0, hi = 0;
        f.read (reinterpret_cast<char*> (&lo), 1);
        f.read (reinterpret_cast<char*> (&hi), 1);
        out.push_back (static_cast<int16_t> (lo | (hi << 8)) / 32768.0);
    }
    return out;
}

/// Goertzel: how much energy sits at one frequency. Used to prove a stem holds
/// the tone that mic was fed and not its neighbour's.
double toneEnergy (const std::vector<double>& x, double freq, double rate) {
    if (x.size() < 512) return 0.0;
    const size_t n = x.size();
    const double w = kTwoPi * freq / rate;
    const double coeff = 2.0 * std::cos (w);
    double s0 = 0, s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; ++i) { s0 = x[i] + coeff * s1 - s2; s2 = s1; s1 = s0; }
    return std::sqrt (std::max (0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2)) / static_cast<double> (n);
}

int failures = 0;
void check (bool ok, const char* what) {
    std::printf ("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok) ++failures;
}

} // namespace

int main (int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "/tmp/e2e";
    std::filesystem::create_directories (dir); // the app creates its session folders; so does the harness
    const double rate = 48000.0;
    const int block = 64;
    const int seconds = 3;

    Fake backend;
    CaptureCoordinator c (backend, rate, block);

    std::vector<CaptureChannel> mics = {
        { "dev-a", "Kitchen", "01_Kitchen", 0.0f },
        { "dev-b", "Couch",   "02_Couch",   0.0f },
    };

    if (! c.startMonitoring (mics, "headphones")) {
        std::printf ("startMonitoring failed: %s\n", c.getMonitorProblem().c_str());
        return 1;
    }
    c.getMonitorBus().setMasterVolume (100.0);

    if (! c.startRecording (dir, 16, "2026-08-27T09:00:00Z")) {
        std::printf ("startRecording failed\n"); return 1;
    }

    // Mic A: 440 Hz. Mic B: 1000 Hz, and its clock runs ~100 PPM fast, which is
    // the kind of drift §3.2 exists to absorb.
    const int blocks = static_cast<int> (rate * seconds / block);
    double phaseA = 0, phaseB = 0;
    double bDebt = 0.0;
    std::vector<float> a (block), b (block + 4), out (block);

    for (int i = 0; i < blocks; ++i) {
        for (int s = 0; s < block; ++s) {
            a[s] = static_cast<float> (0.4 * std::sin (phaseA));
            phaseA += kTwoPi * 440.0 / rate;
        }
        const float* aIn[] = { a.data() };
        backend.inputs[0] (aIn, 1, nullptr, 0, block);

        bDebt += block * 100.0e-6;           // 100 PPM fast
        int bCount = block;
        if (bDebt >= 1.0) { bCount += 1; bDebt -= 1.0; }
        for (int s = 0; s < bCount; ++s) {
            b[s] = static_cast<float> (0.4 * std::sin (phaseB));
            phaseB += kTwoPi * 1000.0 / rate;
        }
        const float* bIn[] = { b.data() };
        backend.inputs[1] (bIn, 1, nullptr, 0, bCount);

        float* outs[] = { out.data() };
        backend.output (nullptr, 0, outs, 1, block);
    }

    c.stopRecording();
    c.stopMonitoring();

    std::printf ("\nDrove %d blocks (%d s) through the real pipeline.\n", blocks, seconds);
    std::printf ("drift: ch0=%.2f PPM (master)  ch1=%.2f PPM\n",
                 c.getChannelDriftPpm (0), c.getChannelDriftPpm (1));
    std::printf ("underruns: ch0=%llu ch1=%llu   framesDropped=%llu\n\n",
                 (unsigned long long) c.getUnderrunSamples (0),
                 (unsigned long long) c.getUnderrunSamples (1),
                 (unsigned long long) c.getFramesDropped());

    uint32_t nA = 0, nB = 0, nM = 0;
    auto stemA = decode16 (dir + "/01_Kitchen.wav", nA);
    auto stemB = decode16 (dir + "/02_Couch.wav", nB);
    auto mix   = decode16 (dir + "/MIX.wav", nM);

    const uint32_t expected = static_cast<uint32_t> (blocks) * block * 2; // 16-bit mono

    std::printf ("Files:\n");
    check (nA == expected, "stem A holds every frame the clock asked for");
    check (nB == expected, "stem B holds every frame the clock asked for");
    check (nM == expected, "MIX holds every frame the clock asked for");

    std::printf ("Content:\n");
    const double a440 = toneEnergy (stemA, 440.0, rate), a1k = toneEnergy (stemA, 1000.0, rate);
    const double b440 = toneEnergy (stemB, 440.0, rate), b1k = toneEnergy (stemB, 1000.0, rate);
    const double m440 = toneEnergy (mix,   440.0, rate), m1k = toneEnergy (mix,   1000.0, rate);
    std::printf ("  stemA 440=%.4f 1k=%.4f | stemB 440=%.4f 1k=%.4f | mix 440=%.4f 1k=%.4f\n",
                 a440, a1k, b440, b1k, m440, m1k);

    check (a440 > 0.10 && a440 > a1k * 10.0, "stem A is mic A's tone, not mic B's");
    check (b1k  > 0.10 && b1k  > b440 * 10.0, "stem B is mic B's tone, not mic A's");
    check (m440 > 0.05 && m1k > 0.05,         "MIX contains both microphones");
    check (c.getFramesDropped() == 0,          "no frames dropped (0.1)");
    check (c.getUnderrunSamples (0) == 0,      "master never underran");

    std::printf ("\n%s (%d failing)\n", failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
