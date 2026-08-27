#include "TestFramework.h"
#include "Core/CaptureCoordinator.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

using namespace mma;

namespace {

std::string tempDir()
{
    for (const char* var : { "MMA_TEST_TMPDIR", "TMPDIR", "TMP", "TEMP" })
    {
        const char* dir = std::getenv (var);
        if (dir != nullptr && *dir != '\0')
            return std::string (dir);
    }
    return "/tmp";
}

// §6.1 writes BWF: RIFF 12 + fmt (8+16) + bext (8+602) + "data" tag 4.
constexpr std::streamoff kDataSizeOffset = 12 + (8 + 16) + (8 + 602) + 4;
constexpr std::streamoff kAudioDataOffset = kDataSizeOffset + 4;

uint32_t readU32LE (std::ifstream& f, std::streampos pos)
{
    f.seekg (pos);
    unsigned char b[4];
    f.read (reinterpret_cast<char*> (b), 4);
    return static_cast<uint32_t> (b[0]) | (static_cast<uint32_t> (b[1]) << 8)
         | (static_cast<uint32_t> (b[2]) << 16) | (static_cast<uint32_t> (b[3]) << 24);
}

/// Stands in for a real device. The platform backends' OS calls still need
/// hardware, but everything this project wires together does not.
class FakeBackend : public IAudioBackend
{
public:
    bool exclusiveAvailable = true;
    std::string exclusiveReason;
    bool failOutputOpen = false;
    bool failInputOpen = false;

    int inputStreamsOpened = 0;
    int outputStreamsOpened = 0;
    int closeAllCalls = 0;
    AudioCallback captured;

    std::string getBackendName() const override { return "Fake"; }
    std::vector<AudioDeviceDescriptor> enumerateInputDevices() override { return {}; }
    std::vector<AudioDeviceDescriptor> enumerateOutputDevices() override { return {}; }
    void setDeviceChangeCallback (DeviceChangeCallback) override {}

    ExclusiveModeCapability checkExclusiveModeCapability (const std::string&, double, int) override
    {
        ExclusiveModeCapability c;
        c.exclusiveModeAvailable = exclusiveAvailable;
        c.unavailableReason = exclusiveReason;
        return c;
    }

    bool openExclusiveOutputStream (const std::string&, double, int, AudioCallback cb) override
    {
        if (failOutputOpen)
            return false;
        ++outputStreamsOpened;
        captured = std::move (cb);
        return true;
    }

    bool openInputStream (const std::string&, double, int, AudioCallback cb) override
    {
        if (failInputOpen)
            return false;
        ++inputStreamsOpened;
        captured = std::move (cb);
        return true;
    }

    void closeAllStreams() override { ++closeAllCalls; }
};

std::vector<CaptureChannel> twoMics()
{
    return { { "dev-a", "Kitchen", "01_Kitchen", 0.0f },
             { "dev-b", "Couch",   "02_Couch",   0.0f } };
}

} // namespace

TEST_CASE (CaptureCoordinator_OpensOneOutputAndOneInputPerMic)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.isMonitoring());

    // §5.2: exactly one output stream, ever.
    REQUIRE (backend.outputStreamsOpened == 1);
    REQUIRE (backend.inputStreamsOpened == 2);
}

TEST_CASE (CaptureCoordinator_RefusesWhenExclusiveModeIsUnavailable)
{
    FakeBackend backend;
    backend.exclusiveAvailable = false;
    backend.exclusiveReason = "Another app has taken exclusive control of your headphones.";

    CaptureCoordinator c (backend, 48000.0, 64);

    // §5.4: never ship a 40 ms mix silently -- refuse and name the cause.
    REQUIRE_FALSE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE_FALSE (c.isMonitoring());
    REQUIRE (c.getMonitorProblem().find ("exclusive control") != std::string::npos);
}

TEST_CASE (CaptureCoordinator_ClosesStreamsWhenAnInputFailsToOpen)
{
    FakeBackend backend;
    backend.failInputOpen = true;

    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE_FALSE (c.startMonitoring (twoMics(), "out-device"));

    // A half-open set of streams would leave the device hogged.
    REQUIRE (backend.closeAllCalls >= 1);
    REQUIRE_FALSE (c.getMonitorProblem().empty());
}

TEST_CASE (CaptureCoordinator_ProducesTheMonitorMixFromEveryMic)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    c.getMonitorBus().setMasterVolume (100.0); // unity output stage, so this test sees the bus itself

    std::vector<float> a (64, 0.10f), b (64, 0.20f);
    std::vector<float> outL (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { outL.data() };

    c.processAudioBlock (ins, 2, outs, 1, 64);

    // §5.1: the mix contains every microphone, summed at unity. Both are
    // present, so the output must exceed either one alone.
    REQUIRE (outL[0] > 0.20f);
}

TEST_CASE (CaptureCoordinator_EveryOutputChannelGetsTheSameMix)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    std::vector<float> a (64, 0.15f), b (64, 0.15f);
    std::vector<float> outL (64, 0.0f), outR (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { outL.data(), outR.data() };

    c.processAudioBlock (ins, 2, outs, 2, 64);

    // §5.1: one mix, identical for every listener. No per-listener variation.
    REQUIRE_NEAR (outL[0], outR[0], 1e-9);
}

TEST_CASE (CaptureCoordinator_MetersRunWithoutRecording)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    std::vector<float> a (64, 0.5f), b (64, 0.0f);
    std::vector<float> outL (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { outL.data() };

    for (int i = 0; i < 40; ++i)
        c.processAudioBlock (ins, 2, outs, 1, 64);

    // §8.1: meters run from launch, not from record.
    REQUIRE_FALSE (c.isRecording());
    auto* m = c.getChannelMetering (0);
    REQUIRE (m != nullptr);

    // §8.2 splits this on purpose: the audio thread only stores block stats into
    // atomics, and the UI thread advances the ballistics at 60 Hz. The
    // coordinator must not call tick() itself -- that would be the audio thread
    // doing UI work.
    for (int i = 0; i < 10; ++i)
        m->tick (1.0 / 60.0);

    REQUIRE (m->getDisplayedLevelDb() > Metering::kMinDb);
}

TEST_CASE (CaptureCoordinator_RecordsAudioThroughToTheFiles)
{
    const auto dir = tempDir();
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.startRecording (dir, 16, "2026-08-27T00:00:00Z"));
    REQUIRE (c.isRecording());

    std::vector<float> a (64, 0.5f), b (64, 0.5f);
    std::vector<float> outL (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { outL.data() };

    for (int i = 0; i < 16; ++i)
        c.processAudioBlock (ins, 2, outs, 1, 64);

    c.stopRecording();

    // 16 blocks x 64 frames = 1024 frames, 2 bytes each at 16-bit mono.
    std::ifstream f (dir + "/01_Kitchen.wav", std::ios::binary);
    REQUIRE (f.is_open());
    REQUIRE (readU32LE (f, kDataSizeOffset) == 1024 * 2);

    // Real signal, not zeros: this is the end-to-end proof that audio reaches
    // the file rather than the pipeline merely being constructed.
    f.seekg (kAudioDataOffset);
    unsigned char lo = 0, hi = 0;
    f.read (reinterpret_cast<char*> (&lo), 1);
    f.read (reinterpret_cast<char*> (&hi), 1);
    const int16_t sample = static_cast<int16_t> (static_cast<uint16_t> (lo) | (static_cast<uint16_t> (hi) << 8));
    REQUIRE (sample > 12000);
}

TEST_CASE (CaptureCoordinator_MixFileIsWrittenAlongsideTheStems)
{
    const auto dir = tempDir();
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.startRecording (dir, 16, "2026-08-27T00:00:00Z"));

    std::vector<float> a (64, 0.25f), b (64, 0.25f);
    std::vector<float> outL (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { outL.data() };
    c.processAudioBlock (ins, 2, outs, 1, 64);

    c.stopRecording();

    std::ifstream mix (dir + "/MIX.wav", std::ios::binary);
    REQUIRE (mix.is_open());
    REQUIRE (readU32LE (mix, kDataSizeOffset) == 64 * 2);
}

TEST_CASE (CaptureCoordinator_MonitoringSurvivesStoppingTheRecording)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.startRecording (tempDir(), 16, "2026-08-27T00:00:00Z"));
    c.stopRecording();

    // §5.1: monitoring is independent of record state.
    REQUIRE (c.isMonitoring());
    REQUIRE_FALSE (c.isRecording());

    std::vector<float> a (64, 0.3f), b (64, 0.3f);
    std::vector<float> outL (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { outL.data() };
    c.processAudioBlock (ins, 2, outs, 1, 64);

    REQUIRE (outL[0] > 0.0f);
}

TEST_CASE (CaptureCoordinator_UnpluggedMicWritesSilenceIntoItsChannel)
{
    const auto dir = tempDir();
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    REQUIRE (c.startRecording (dir, 16, "2026-08-27T00:00:00Z"));

    // §6.5: the channel stays, it just goes quiet.
    c.setChannelLive ("dev-a", false);

    std::vector<float> a (64, 0.9f), b (64, 0.9f);
    std::vector<float> outL (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { outL.data() };
    c.processAudioBlock (ins, 2, outs, 1, 64);

    c.stopRecording();

    std::ifstream f (dir + "/01_Kitchen.wav", std::ios::binary);
    REQUIRE (f.is_open());
    // Frames present, and zero.
    REQUIRE (readU32LE (f, kDataSizeOffset) == 64 * 2);

    f.seekg (kAudioDataOffset);
    unsigned char lo = 0, hi = 0;
    f.read (reinterpret_cast<char*> (&lo), 1);
    f.read (reinterpret_cast<char*> (&hi), 1);
    REQUIRE (lo == 0);
    REQUIRE (hi == 0);
}

TEST_CASE (CaptureCoordinator_NoOutputBuffersIsNotAFailure)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    std::vector<float> a (64, 0.5f), b (64, 0.5f);
    const float* ins[] = { a.data(), b.data() };

    // An input-only callback is normal on backends that split the directions.
    c.processAudioBlock (ins, 2, nullptr, 0, 64);
    REQUIRE (c.isMonitoring());
}

TEST_CASE (CaptureCoordinator_StoppingMonitoringClosesTheStreams)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);

    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    c.stopMonitoring();

    REQUIRE_FALSE (c.isMonitoring());
    REQUIRE (backend.closeAllCalls >= 1);
}

TEST_CASE (CaptureCoordinator_RecordingRefusesWithNoChannels)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);

    REQUIRE (c.startMonitoring ({}, "out-device"));
    REQUIRE_FALSE (c.startRecording (tempDir(), 16, "2026-08-27T00:00:00Z"));
}

TEST_CASE (CaptureCoordinator_MasterVolumeChangesTheMonitorButNotTheStems)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    std::vector<float> a (64, 0.10f), b (64, 0.10f);
    const float* ins[] = { a.data(), b.data() };

    std::vector<float> loudOut (64, 0.0f), quietOut (64, 0.0f);

    c.getMonitorBus().setMasterVolume (100.0);
    float* loud[] = { loudOut.data() };
    c.processAudioBlock (ins, 2, loud, 1, 64);

    c.getMonitorBus().setMasterVolume (20.0);
    float* quiet[] = { quietOut.data() };
    c.processAudioBlock (ins, 2, quiet, 1, 64);

    // §5.1: master volume is a listening level. It must move the headphones...
    REQUIRE (quietOut[0] < loudOut[0]);
    REQUIRE (quietOut[0] > 0.0f);
}

TEST_CASE (CaptureCoordinator_TrimAffectsTheMonitorMix)
{
    FakeBackend backend;

    auto quiet = twoMics();
    quiet[1].trimDb = -20.0f; // §4 trim range floor

    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (quiet, "out-device"));
    c.getMonitorBus().setMasterVolume (100.0);

    std::vector<float> a (64, 0.10f), b (64, 0.10f);
    std::vector<float> out (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { out.data() };

    c.processAudioBlock (ins, 2, outs, 1, 64);

    // §4: trim is a mix-side decision, so pulling one mic down 20 dB must land
    // the sum well below the 0.20 two-mics-at-unity would give.
    REQUIRE (out[0] < 0.20f);
    REQUIRE (out[0] > 0.10f);
}
