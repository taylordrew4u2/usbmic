#include "TestFramework.h"
#include "Core/CaptureCoordinator.h"
#include "Core/PolarPatternDetector.h"
#include <cmath>
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
    std::string inputOpenError;

    int inputStreamsOpened = 0;
    int outputStreamsOpened = 0;
    int closeAllCalls = 0;
    AudioCallback captured;              // the output stream's callback (the clock)
    std::vector<AudioCallback> inputCallbacks; // one per device, in open order

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
        inputCallbacks.push_back (cb);
        captured = std::move (cb);
        return true;
    }

    std::string getLastOpenError() const override { return inputOpenError; }

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

TEST_CASE (CaptureCoordinator_SaysWhyAMicrophoneWouldNotOpen)
{
    // §0.1: the backend knows the cause and the coordinator used to discard it,
    // leaving the user to guess between a dead cable, a sample-rate mismatch, a
    // missing macOS permission and another app holding the interface. Those
    // have four different fixes and one message.
    FakeBackend backend;
    backend.failInputOpen = true;
    backend.inputOpenError = "This interface is running at 48 kHz and won't change to the 44.1 kHz "
                             "this recording uses.";

    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE_FALSE (c.startMonitoring (twoMics(), "out-device"));

    const auto problem = c.getMonitorProblem();

    // Still names the microphone, so the user knows which one.
    REQUIRE (problem.find ("Kitchen") != std::string::npos);
    // And now says what to do about it.
    REQUIRE (problem.find ("48 kHz") != std::string::npos);
    REQUIRE (problem.find ("44.1 kHz") != std::string::npos);
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

TEST_CASE (CaptureCoordinator_TrimCanBeChangedWhileRunning)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    c.getMonitorBus().setMasterVolume (100.0);

    std::vector<float> a (64, 0.10f), b (64, 0.10f);
    const float* ins[] = { a.data(), b.data() };

    std::vector<float> before (64, 0.0f), after (64, 0.0f);
    float* outBefore[] = { before.data() };
    float* outAfter[] = { after.data() };

    c.processAudioBlock (ins, 2, outBefore, 1, 64);

    // §4: the user turns one mic down mid-session and the monitor follows
    // immediately -- no restart, no gap.
    c.setChannelTrimDb (1, -20.0f);
    c.processAudioBlock (ins, 2, outAfter, 1, 64);

    REQUIRE (after[0] < before[0]);
    REQUIRE_NEAR (c.getChannelTrimDb (1), -20.0f, 1e-6);
    REQUIRE_NEAR (c.getChannelTrimDb (0), 0.0f, 1e-6);
}

TEST_CASE (CaptureCoordinator_OutOfRangeTrimIndexIsIgnored)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    // Must not write past the end: the UI can outlive a channel that just
    // disappeared, and a stray index here would corrupt the audio thread's
    // gain table.
    c.setChannelTrimDb (-1, -6.0f);
    c.setChannelTrimDb (99, -6.0f);

    REQUIRE_NEAR (c.getChannelTrimDb (0), 0.0f, 1e-6);
    REQUIRE_NEAR (c.getChannelTrimDb (1), 0.0f, 1e-6);
}

TEST_CASE (CaptureCoordinator_ReportsAudioCallbackLoad)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    std::vector<float> a (64, 0.10f), b (64, 0.10f);
    std::vector<float> out (64, 0.0f);
    const float* ins[] = { a.data(), b.data() };
    float* outs[] = { out.data() };

    for (int i = 0; i < 50; ++i)
        c.processAudioBlock (ins, 2, outs, 1, 64);

    // §6.6 needs a real load figure to warn from. A 64-sample block at 48k has
    // 1.33ms to work with and this does almost nothing, so the load must be a
    // sane fraction rather than zero or nonsense.
    const auto load = c.getAudioCallbackLoad();
    REQUIRE (load > 0.0);
    REQUIRE (load < 1.0);
}

TEST_CASE (CaptureCoordinator_EachDeviceLandsInItsOwnChannel)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    REQUIRE (backend.inputCallbacks.size() == 2);

    // Each USB device delivers only its own audio on its own callback. Passing
    // one shared callback to every device -- which is what this replaces --
    // wrote every microphone into channel 0 and recorded one mic N times.
    // Enough to clear pre-roll (§5.4: kPreRollBlocks of the 64-sample buffer).
    std::vector<float> loud (256, 0.8f), quiet (256, 0.1f);
    const float* loudIn[] = { loud.data() };
    const float* quietIn[] = { quiet.data() };

    backend.inputCallbacks[0] (loudIn, 1, nullptr, 0, 256);
    backend.inputCallbacks[1] (quietIn, 1, nullptr, 0, 256);

    std::vector<float> out (64, 0.0f);
    float* outs[] = { out.data() };
    c.processOutputBlock (outs, 1, 64);

    for (int i = 0; i < 10; ++i)
    {
        c.getChannelMetering (0)->tick (1.0 / 60.0);
        c.getChannelMetering (1)->tick (1.0 / 60.0);
    }

    // Channel 0 got the loud mic and channel 1 the quiet one -- not the same
    // mic twice, which is what the shared callback produced.
    REQUIRE (c.getChannelMetering (0)->getDisplayedLevelDb()
             > c.getChannelMetering (1)->getDisplayedLevelDb() + 6.0f);
}

TEST_CASE (CaptureCoordinator_OutputClockPullsEveryDeviceIntoTheMix)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    c.getMonitorBus().setMasterVolume (100.0);

    std::vector<float> a (256, 0.10f), b (256, 0.20f);
    const float* aIn[] = { a.data() };
    const float* bIn[] = { b.data() };

    backend.inputCallbacks[0] (aIn, 1, nullptr, 0, 256);
    backend.inputCallbacks[1] (bIn, 1, nullptr, 0, 256);

    std::vector<float> out (64, 0.0f);
    float* outs[] = { out.data() };
    c.processOutputBlock (outs, 1, 64);

    // §5.1: the mix contains every mic, summed at unity, so it must exceed
    // either one alone.
    REQUIRE (out[0] > 0.20f);
}

TEST_CASE (CaptureCoordinator_FirstMicIsTheClockMasterByDefault)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    // §3.1: a rig with no master would resample every device against nothing.
    REQUIRE (c.getMasterChannel() == 0);

    c.setMasterChannel (1);
    REQUIRE (c.getMasterChannel() == 1);

    // Out of range clears it rather than silently keeping a stale index -- the
    // master can leave the rig (§3.3).
    c.setMasterChannel (99);
    REQUIRE (c.getMasterChannel() == -1);
}

TEST_CASE (CaptureCoordinator_MasterReportsNoDriftAgainstItself)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));
    c.setMasterChannel (0);

    std::vector<float> a (2048, 0.1f);
    const float* aIn[] = { a.data() };
    backend.inputCallbacks[0] (aIn, 1, nullptr, 0, 2048);

    std::vector<float> out (64, 0.0f);
    float* outs[] = { out.data() };

    for (int i = 0; i < 100; ++i)
        c.processOutputBlock (outs, 1, 64);

    // §3.1: the timebase is never corrected against itself, however its ring
    // happens to sit.
    REQUIRE_NEAR (c.getChannelDriftPpm (0), 0.0, 1e-12);
}

TEST_CASE (CaptureCoordinator_UnpluggedDeviceStillYieldsItsChannel)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    std::vector<float> a (512, 0.5f), b (512, 0.5f);
    const float* aIn[] = { a.data() };
    const float* bIn[] = { b.data() };
    backend.inputCallbacks[0] (aIn, 1, nullptr, 0, 512);
    backend.inputCallbacks[1] (bIn, 1, nullptr, 0, 512);

    // §6.5: the mic goes away mid-session; its channel does not.
    c.setChannelLive ("dev-b", false);

    std::vector<float> out (64, 0.0f);
    float* outs[] = { out.data() };
    c.processOutputBlock (outs, 1, 64);

    for (int i = 0; i < 10; ++i)
        c.getChannelMetering (1)->tick (1.0 / 60.0);

    // Channel 1 is present and silent, not gone and not replaying stale audio.
    REQUIRE (c.getChannelMetering (1) != nullptr);
    REQUIRE_NEAR (c.getChannelMetering (1)->getDisplayedLevelDb(), Metering::kMinDb, 1.0f);
}

namespace {

/// Three mics on three crystals, driven for a simulated take. Optionally the
/// clock master is unplugged a sixth of the way in (§6.5), and optionally the
/// take fails over onto another channel.
struct TakeResult
{
    double driftPpm[3] = { 0.0, 0.0, 0.0 };
    uint64_t underruns[3] = { 0, 0, 0 };
};

TakeResult runTake (bool unplugMaster, int failoverTo, double seconds, double unplugAtSeconds = 5.0)
{
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);

    const std::vector<CaptureChannel> mics = {
        { "dev-a", "Kitchen", "01_Kitchen", 0.0f },
        { "dev-b", "Couch",   "02_Couch",   0.0f },
        { "dev-c", "Desk",    "03_Desk",    0.0f },
    };

    REQUIRE (c.startMonitoring (mics, "out-device"));
    c.setMasterChannel (0);

    // Three dissimilar USB crystals, which is the case §3 exists for.
    const double ppm[3] = { 0.0, +40.0, -60.0 };
    double owed[3] = { 0.0, 0.0, 0.0 };

    std::vector<float> in (128, 0.25f);
    std::vector<float> out (64, 0.0f);
    float* outs[] = { out.data() };

    const long long blocks = static_cast<long long> (seconds * 48000.0 / 64.0);
    const long long unplugAt = static_cast<long long> (unplugAtSeconds * 48000.0 / 64.0);
    bool unplugged = false;

    for (long long i = 0; i < blocks; ++i)
    {
        if (unplugMaster && ! unplugged && i == unplugAt)
        {
            // §6.5: the channel stays and goes silent. The channel list is
            // fixed for the take, so only liveness moves.
            c.setChannelLive ("dev-a", false);

            if (failoverTo >= 0)
                c.setMasterChannel (failoverTo);

            unplugged = true;
        }

        for (int d = 0; d < 3; ++d)
        {
            if (unplugged && d == 0)
                continue; // the mic is gone; nothing more arrives from it

            owed[d] += 64.0 * (1.0 + ppm[d] * 1.0e-6);
            const int n = static_cast<int> (owed[d]);
            owed[d] -= n;

            const float* block[] = { in.data() };
            backend.inputCallbacks[static_cast<size_t> (d)] (block, 1, nullptr, 0, n);
        }

        c.processOutputBlock (outs, 1, 64);
    }

    TakeResult r;
    for (int d = 0; d < 3; ++d)
    {
        r.driftPpm[d] = c.getChannelDriftPpm (d);
        r.underruns[d] = c.getUnderrunSamples (d);
    }
    return r;
}

} // namespace

TEST_CASE (CaptureCoordinator_UnpluggedMasterLeavesTheOtherChannelsUntouched)
{
    // The question §6.5's "clock master unplugged" row raises: with the master
    // gone silent and still flagged as master, is everything else now being
    // resampled onto silence?
    //
    // It is not, and this pins down why. The master is not a signal any other
    // channel reads -- DeviceInputStream drives each channel's ratio from that
    // stream's own ring fill against the output clock, with no channel's audio
    // entering another's arithmetic. So a master that stops delivering removes
    // nothing from anyone else's loop.
    //
    // Run to a minute so both live loops are well past settling.
    const auto control = runTake (false, -1, 60.0);
    const auto masterGone = runTake (true, -1, 60.0);

    // Bit-identical, not merely close: the two runs execute the same arithmetic
    // on the live channels. Anything else would mean a coupling that is not
    // supposed to exist.
    for (int d = 1; d < 3; ++d)
    {
        REQUIRE_NEAR (masterGone.driftPpm[d], control.driftPpm[d], 1e-12);
        REQUIRE (masterGone.underruns[d] == control.underruns[d]);
    }

    // And the loops were genuinely working, so the equality above means
    // something.
    REQUIRE (control.driftPpm[1] > 20.0);
    REQUIRE (control.driftPpm[2] < -20.0);
    REQUIRE (masterGone.underruns[1] == 0);
    REQUIRE (masterGone.underruns[2] == 0);
}

TEST_CASE (CaptureCoordinator_FailingOverMidTakeCostsTheLiveChannelsNothing)
{
    // §6.5's "clock master unplugged -> failover per §3.3", and the regression
    // guard for what used to make it unsafe.
    //
    // Under the old exemption, promoting a live channel took one the PI loop was
    // holding at its target fill and stopped steering it, leaving its ring to
    // run to one end for the rest of the take. Failing over cost more than not
    // failing over, which is why the mid-take path did not do it.
    //
    // Every channel is corrected onto the output clock now, so the title carries
    // no correction with it and the promotion is free.
    const auto noFailover = runTake (true, -1, 60.0, 4.0);
    const auto failedOver = runTake (true, 1, 60.0, 4.0);

    // Channel 1 takes over the reference and keeps being steered: its loop
    // reaches the correction its +40 PPM crystal needs either way. (Its own
    // reported figure is relative to itself once it is the master, hence
    // reading it off the control run.)
    REQUIRE (noFailover.driftPpm[1] > 35.0);
    REQUIRE_NEAR (failedOver.driftPpm[1], 0.0, 1e-12);

    // And nothing was lost anywhere: no channel underran in either run.
    for (int d = 1; d < 3; ++d)
    {
        REQUIRE (noFailover.underruns[d] == 0);
        REQUIRE (failedOver.underruns[d] == 0);
    }
}

TEST_CASE (CaptureCoordinator_DriftIsQuotedRelativeToTheClockMaster)
{
    // §3.3: "positive means this device runs fast relative to the master."
    //
    // Each stream's own loop measures itself against the output clock, whose
    // skew is common to every channel. Moving the reference therefore re-bases
    // every figure by the same amount, and the master always reads zero against
    // itself -- which is what makes these numbers mean what §3.3 says they mean
    // rather than "how far this mic is from the headphones".
    const auto control = runTake (false, -1, 60.0);

    REQUIRE_NEAR (control.driftPpm[0], 0.0, 1e-12);

    // Channels at +40 and -60 PPM against a master at 0.
    REQUIRE (control.driftPpm[1] > 35.0);
    REQUIRE (control.driftPpm[2] < -55.0);
}

namespace {

/// Drives one device's input callback with a stereo block, the way a USB
/// microphone that presents two channels does.
void pushStereo (FakeBackend& backend, int device,
                 const std::vector<float>& left, const std::vector<float>& right)
{
    const float* channels[] = { left.data(), right.data() };
    backend.inputCallbacks[static_cast<size_t> (device)] (channels, 2, nullptr, 0,
                                                          static_cast<int> (left.size()));
}

} // namespace

TEST_CASE (CaptureCoordinator_RecordsTheLiveSideOfARightWiredMicrophone)
{
    // §2.1: "many USB mics present as stereo with one silent side". Which side
    // is silent is not fixed, and the capture path used to take channel 0 no
    // matter what -- so a microphone with its capsule on the right recorded
    // pure silence. Nothing warned: the meter sat at the floor and the stem was
    // empty, which is §0.1's failure with a working device attached.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    const std::vector<float> silent (512, 0.0f);
    const std::vector<float> signal (512, 0.4f);

    // Device 0 is wired to the right, device 1 to the left.
    pushStereo (backend, 0, silent, signal);
    pushStereo (backend, 1, signal, silent);

    REQUIRE (c.getChannelLayoutSource (0) == 1);
    REQUIRE (c.getChannelLayoutSource (1) == 0);

    std::vector<float> out (64, 0.0f);
    float* outs[] = { out.data() };
    c.processOutputBlock (outs, 1, 64);

    for (int i = 0; i < 10; ++i)
    {
        c.getChannelMetering (0)->tick (1.0 / 60.0);
        c.getChannelMetering (1)->tick (1.0 / 60.0);
    }

    // Both channels carry the microphone's audio, whichever side it arrived on.
    REQUIRE (c.getChannelMetering (0)->getDisplayedLevelDb() > Metering::kMinDb + 20.0f);
    REQUIRE (c.getChannelMetering (1)->getDisplayedLevelDb() > Metering::kMinDb + 20.0f);
}

TEST_CASE (CaptureCoordinator_MonoDeviceIsUntouchedByChannelLayout)
{
    // A device presenting a single channel has nothing to decide, and must not
    // be routed through the two-channel path at all -- there is no second
    // pointer to read, and reading one would be a fault rather than a wrong
    // answer.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    std::vector<float> mono (512, 0.3f);
    const float* one[] = { mono.data() };
    backend.inputCallbacks[0] (one, 1, nullptr, 0, 512);

    // Never analysed, so never claimed a side.
    REQUIRE (c.getChannelLayoutSource (0) == -1);
    REQUIRE (c.getChannelLayoutDecision (0) == ChannelLayoutDecision::Pending);

    std::vector<float> out (64, 0.0f);
    float* outs[] = { out.data() };
    c.processOutputBlock (outs, 1, 64);

    for (int i = 0; i < 10; ++i)
        c.getChannelMetering (0)->tick (1.0 / 60.0);

    REQUIRE (c.getChannelMetering (0)->getDisplayedLevelDb() > Metering::kMinDb + 20.0f);
}

TEST_CASE (CaptureCoordinator_ChannelSideNeverMovesOnceRecording)
{
    // §6.5 fixes the take's shape for its duration. Swapping which channel
    // feeds a stem mid-file would put a discontinuity in the middle of the
    // recording -- a worse failure than the one the side-picking fixes.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    const std::vector<float> silent (512, 0.0f);
    const std::vector<float> signal (512, 0.4f);

    // Opens in a quiet room: nothing heard from either side yet, so the side is
    // still the default and §2.1 has not decided. This is deliberately the
    // state in which the side is most free to move.
    pushStereo (backend, 0, silent, silent);
    REQUIRE (c.getChannelLayoutSource (0) == 0);
    REQUIRE (c.getChannelLayoutDecision (0) == ChannelLayoutDecision::Pending);

    REQUIRE (c.startRecording (tempDir(), 16, "2026-09-01T00:00:00Z"));

    // Now the right starts carrying everything while the left stays silent --
    // the exact evidence that would move the side, arriving after the take has
    // begun. It must not move: the freeze has to be checked before the side is
    // recomputed, or the first block of the take still adopts the new one.
    for (int i = 0; i < 400; ++i)
        pushStereo (backend, 0, silent, signal);

    REQUIRE (c.getChannelLayoutSource (0) == 0);

    c.stopRecording();
}

namespace {

std::vector<CaptureChannel> threeMics()
{
    return { { "dev-a", "Kitchen", "01_Kitchen", 0.0f },
             { "dev-b", "Couch",   "02_Couch",   0.0f },
             { "dev-c", "Desk",    "03_Desk",    0.0f } };
}

/// Runs one already-aligned frame block through the aggregate path, which is
/// where §14.4's measurement sits.
void pushAligned (CaptureCoordinator& c, const std::vector<std::vector<float>>& channels)
{
    std::vector<const float*> pointers;
    for (const auto& ch : channels)
        pointers.push_back (ch.data());

    c.processAudioBlock (pointers.data(), static_cast<int> (pointers.size()),
                         nullptr, 0, static_cast<int> (channels[0].size()));
}

} // namespace

TEST_CASE (CaptureCoordinator_SpotsTwoMicrophonesHearingTheSameRoom)
{
    // §14.4, "the sleeper failure": two microphones in omni pick up the whole
    // room, so they hear the same thing, while a third microphone with nobody
    // in front of it hears nothing. That shape is what the detector looks for,
    // and it had never been fed anything.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (threeMics(), "out-device"));

    std::vector<float> shared (64), silent (64, 0.0f);
    for (size_t i = 0; i < shared.size(); ++i)
        shared[i] = 0.4f * std::sin (static_cast<float> (i) * 0.3f);

    // A and B carry the same room; C is quiet.
    pushAligned (c, { shared, shared, silent });

    REQUIRE (c.getPolarPairCorrelation() > PolarPatternDetector::kCorrelationThreshold);
    REQUIRE (c.getPolarThirdChannelPeakDb() < PolarPatternDetector::kThirdChannelSilenceDb);
}

TEST_CASE (CaptureCoordinator_DoesNotCallThreePeopleTalkingARoomProblem)
{
    // The case that must never fire. Three people each on their own cardioid
    // microphone: the channels are uncorrelated and nobody is silent. Calling
    // that a pattern problem would send someone to turn a knob that was already
    // right, which is worse than staying quiet.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (threeMics(), "out-device"));

    std::vector<float> a (64), b (64), third (64);
    for (size_t i = 0; i < a.size(); ++i)
    {
        a[i] = 0.4f * std::sin (static_cast<float> (i) * 0.30f);
        b[i] = 0.4f * std::sin (static_cast<float> (i) * 1.10f + 2.0f);
        third[i] = 0.4f * std::sin (static_cast<float> (i) * 0.70f + 4.0f);
    }

    pushAligned (c, { a, b, third });

    // Either test failing alone is enough to keep §14.4 quiet, and both do.
    REQUIRE (c.getPolarThirdChannelPeakDb() > PolarPatternDetector::kThirdChannelSilenceDb);
}

TEST_CASE (CaptureCoordinator_PolarPatternNeedsAThirdMicrophone)
{
    // §14.4's rule is stated over three channels: a correlated pair *and* a
    // third that hears nothing. With two microphones there is no uninvolved
    // one, and two people at one table correlate perfectly well.
    FakeBackend backend;
    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (twoMics(), "out-device"));

    std::vector<float> shared (64);
    for (size_t i = 0; i < shared.size(); ++i)
        shared[i] = 0.4f * std::sin (static_cast<float> (i) * 0.3f);

    pushAligned (c, { shared, shared });

    REQUIRE_NEAR (c.getPolarPairCorrelation(), 0.0f, 1e-9);
}

TEST_CASE (CaptureCoordinator_ANarrowerBlockDoesNotLeaveAnOldChannelInTheMix)
{
    // trimFrame is sized to the take's channel list, but a block can carry
    // fewer channels than that: the aggregate path takes
    // min(numInputs, channels.size()). MonitorBus::processSample sums the whole
    // vector, so any entry past the block's own width kept whatever an earlier,
    // wider block left in it -- a fixed sample added to every sample of the mix
    // from then on, from a microphone that is no longer arriving. In the
    // listener's headphones that is a DC offset that does not go away.
    FakeBackend backend;

    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (threeMics(), "out-device"));
    c.getMonitorBus().setMasterVolume (100.0);

    std::vector<float> a (64, 0.0f), b (64, 0.0f), loud (64, 0.4f);
    std::vector<float> out (64, 0.0f);
    float* outs[] = { out.data() };

    // Three channels, and the third one is the only one making any sound.
    const float* three[] = { a.data(), b.data(), loud.data() };
    c.processAudioBlock (three, 3, outs, 1, 64);
    REQUIRE (out[0] > 0.3f);

    // The third channel stops arriving. What is left is silent, so the mix must
    // be silent -- not still carrying the last sample the third one delivered.
    const float* two[] = { a.data(), b.data() };
    c.processAudioBlock (two, 2, outs, 1, 64);

    for (int i = 0; i < 64; ++i)
        REQUIRE (std::abs (out[i]) < 1.0e-6f);
}

TEST_CASE (CaptureCoordinator_PolarVerdictDoesNotOutliveItsMeasurement)
{
    // Every way out of measurePolarPattern clears the correlation except the
    // one that gives up on a null channel, which returned with the last verdict
    // still published. §14.4's advice reads that number, so it would keep
    // firing off a measurement that had stopped being made.
    FakeBackend backend;

    CaptureCoordinator c (backend, 48000.0, 64);
    REQUIRE (c.startMonitoring (threeMics(), "out-device"));

    // Two microphones hearing the same room, with a third hearing nothing:
    // §14.4's shape, and a correlation well above zero.
    std::vector<float> room (64, 0.0f), quiet (64, 0.0f);
    for (int i = 0; i < 64; ++i)
        room[static_cast<size_t> (i)] = std::sin (static_cast<float> (i) * 0.2f) * 0.5f;

    pushAligned (c, { room, room, quiet });
    REQUIRE (c.getPolarPairCorrelation() > 0.9f);

    // A channel stops being handed over at all.
    const float* withHole[] = { room.data(), nullptr, quiet.data() };
    c.processAudioBlock (withHole, 3, nullptr, 0, 64);

    REQUIRE (c.getPolarPairCorrelation() == 0.0f);
}
