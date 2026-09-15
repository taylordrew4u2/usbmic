// Headphones that stop accepting audio in the middle of a take, on Linux.
//
// §0.1: losing the monitor mix must never cost the recording. sim_capture_mac
// and sim_capture_win each make this claim on their own platform; Linux -- the
// platform whose CI opens a real device -- could not make it at all, because
// nothing had ever run the playback half of AlsaBackend's worker loop. The
// existing harness passes no output device, so snd_pcm_writei, the recovered
// -EPIPE path that counts outputGlitches, and the -ENODEV path that gives up
// and reports were all unreachable from any test.
//
// They are reachable now: the shim that already replaces snd_pcm_readi for one
// named PCM does the same for snd_pcm_writei under MMA_SHIM_STREAM=playback,
// so the monitor output dies exactly as a pulled headphone cable makes it die
// while every other call in the path stays the shipping code.
//
//   Tools/alsa_monitor_loss.sh
//
// What this covers and what it does not, said plainly: the capture half runs
// through the real CaptureCoordinator and the real writer, and the monitor
// output is opened on the backend directly. It is opened directly because the
// coordinator reaches the output through checkExclusiveModeCapability, which
// refuses a user-defined .asoundrc alias -- correctly, since such a name says
// nothing about what it wraps. So this is the backend's claim, which is where
// the two streams actually live: they are independent PCMs on independent
// threads, and the question is whether one dying disturbs the other.

#include "Core/CaptureCoordinator.h"
#include "Platform/AlsaBackend.h"

#include <atomic>
#include <chrono>
#include <cmath>
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

std::string tempDir()
{
    for (const char* var : { "MMA_TEST_TMPDIR", "TMPDIR", "TMP", "TEMP" })
        if (const char* d = std::getenv (var); d != nullptr && *d != '\0')
            return std::string (d);

    return "/tmp";
}

/// Frames in a 24-bit WAV, the largest sample in it, and one past the last
/// non-zero sample. The chunk list is walked rather than searched: the header
/// carries a 602-byte bext chunk and the bytes "data" occur inside it.
///
/// Measured from the audio, never from a fraction of the file or a count of
/// callbacks. Under this harness the file is not as long as the audio put into
/// it -- the writer's ring drops what it cannot take in time -- and every
/// assertion in this repository that was written against a fraction or a
/// callback count held on a fast machine and broke on a loaded CI runner.
bool inspect24BitWav (const std::string& path, uint32_t& frames, int32_t& peak,
                      uint32_t& lastSounding)
{
    frames = 0;
    peak = 0;
    lastSounding = 0;

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

        const int32_t magnitude = v < 0 ? -v : v;

        if (magnitude > peak)
            peak = magnitude;

        if (magnitude > 0)
            lastSounding = i + 1;
    }

    std::fclose (f);
    return true;
}

} // namespace

int main()
{
    const auto dir = tempDir();
    const char* dyingOutput = std::getenv ("MMA_SHIM_DEVICE");
    const bool outputDies = dyingOutput != nullptr;

    std::printf ("%s\n", outputDies
                             ? "The headphones stop accepting audio part way through a take"
                             : "Control: the same take, with the headphones left alone");

    mma::AlsaBackend backend;

    auto devices = backend.enumerateInputDevices();

    if (devices.empty())
    {
        std::printf ("  FAIL  need a fixture microphone, found none -- "
                     "run Tools/setup_alsa_fixture.sh\n");
        return 1;
    }

    mma::CaptureCoordinator coordinator (backend, 48000.0, 256);

    mma::CaptureChannel mic;
    mic.deviceId = devices[0].usbLocationId;
    mic.deviceChannel = 0;
    mic.displayName = "Singer";
    mic.fileName = "01_Singer";
    mic.bitDepth = 24;

    if (! coordinator.startMonitoring ({ mic }, {}))
    {
        std::printf ("  FAIL  startMonitoring: %s\n", coordinator.getMonitorProblem().c_str());
        return 1;
    }

    // The monitor output, opened on the backend so the playback worker really
    // runs. Its callback fills a tone: writing silence would leave the -EPIPE
    // branch counting glitches on buffers nobody could tell apart.
    std::atomic<long> monitorCallbacks { 0 };

    const bool outputOpened = backend.openExclusiveOutputStream (
        "mma_out", 48000.0, 256,
        [&monitorCallbacks] (const float* const*, int, float* const* outputs,
                             int numOutputs, int numFrames)
        {
            monitorCallbacks.fetch_add (1, std::memory_order_relaxed);

            for (int ch = 0; ch < numOutputs; ++ch)
                for (int f = 0; f < numFrames; ++f)
                    outputs[ch][f] = 0.25f * std::sin (0.05f * static_cast<float> (f));
        });

    check (outputOpened, "the headphones open and start taking audio");

    if (! outputOpened)
    {
        std::printf ("  (%s)\n", backend.getLastOpenError().c_str());
        coordinator.stopMonitoring();
        return 1;
    }

    if (! coordinator.startRecording (dir, 24, "2026-09-15T00:00:00Z"))
    {
        std::printf ("  FAIL  startRecording\n");
        return 1;
    }

    // Real seconds, because the whole point is headphones that go PART WAY
    // THROUGH a take: the shim lets the first stretch of writes succeed and
    // fails everything after it.
    std::vector<float> out (512, 0.0f);
    float* outs[] = { out.data(), out.data() + 256 };

    const auto drive = [&] (std::chrono::milliseconds span)
    {
        const auto until = std::chrono::steady_clock::now() + span;

        while (std::chrono::steady_clock::now() < until)
        {
            coordinator.pullOutputBlock (outs, 2, 256);
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }
    };

    // Sampled either side of the loss, which is the only thing that tells a
    // surviving take from a stopped one.
    //
    // This harness first asked only that the stem sounded to its own end, and
    // that is internal consistency, not survival: a take that stopped dead at
    // the unplug satisfies it just as well, and a deliberately broken run
    // proved it did. Frame counts cannot stand in for wall clock here either --
    // the ALSA file plugin free-runs at thousands of times real time, so six
    // seconds of take is not 288,000 frames -- so the measurement is a
    // comparison rather than a number: it kept going, or it did not.
    drive (std::chrono::seconds (3));

    const auto framesAtLoss = coordinator.getFramesAcceptedCount();
    const auto monitorAtLoss = monitorCallbacks.load();

    drive (std::chrono::seconds (3));

    const auto framesRecorded = coordinator.getFramesAcceptedCount();
    const auto monitorAtEnd = monitorCallbacks.load();

    coordinator.stopRecording();

    const auto reported = backend.takeStreamFailures();
    const auto glitches = backend.getOutputGlitchCount();
    const auto dropped = backend.getFramesDroppedByBackend();

    coordinator.stopMonitoring();

    uint32_t frames = 0, sounding = 0;
    int32_t peak = 0;
    const auto path = dir + "/01_Singer.wav";

    check (inspect24BitWav (path, frames, peak, sounding), "the stem exists");

    std::printf ("  01_Singer.wav: %u frames, peak %d, sounds to frame %u\n",
                 frames, peak, sounding);
    std::printf ("  monitor callbacks %ld, output glitches %llu, frames dropped %llu\n",
                 monitorCallbacks.load(), (unsigned long long) glitches,
                 (unsigned long long) dropped);

    // The claim, on either run: the take is untouched by what the monitor
    // output did. A quarter of a second of slack at the end, because the
    // writer's final flush leaves a short silent tail.
    constexpr uint32_t kQuarterSecond = 12000;

    check (peak > 100000, "the microphone records real audio");
    check (sounding + kQuarterSecond >= frames, "right through to the end of the take");
    check (framesRecorded > framesAtLoss,
           "and the take went on accepting frames after the monitor's fate was decided");

    // Losing the monitor mix is not lost recording, and this is the counter
    // that once said otherwise: output xruns were added to framesDropped, which
    // feeds a sentence about audio lost before recording and a permanent line
    // in the take's own record. A take that recorded perfectly carried a
    // lasting claim that it had not.
    check (dropped == 0, "and nothing the monitor did was counted as lost recording");

    if (outputDies)
    {
        check (! reported.empty(), "the headphones going is reported rather than left silent");

        bool saidHeadphones = false;

        for (const auto& failure : reported)
        {
            std::printf ("  Reported: id='%s' %s\n", failure.deviceId.c_str(),
                         failure.reason.c_str());

            // Empty is correct here and is what Application.cpp expects: with no
            // id it names the subject "Your headphones", because the monitor
            // output is not one of the user's microphones.
            if (failure.deviceId.empty())
                saidHeadphones = true;

            // §0.1 again, from the other side: the app stops a take only for a
            // sample-rate change, and headphones are never that. A monitor
            // failure that asked for a stop would end takes on an unplugged
            // cable.
            //
            // Said plainly, because a green assertion that cannot go red is
            // worse than no assertion: StreamFailureSink::note takes no kind,
            // so every ALSA report is `unknown` and this cannot fail as the
            // code stands today. It is a guard on the day someone gives note()
            // a kind and passes the wrong one -- checked by temporarily
            // defaulting StreamFailure::kind to sampleRateChanged, which does
            // turn this line red.
            check (! mma::streamFailureRequiresRecordingStop (failure.kind),
                   "and does not ask for the take to be stopped");
        }

        check (saidHeadphones, "as a report about the monitor rather than about a microphone");

        // Without this the run above would pass just as well against headphones
        // that never failed, and would be making no claim at all.
        check (monitorAtEnd == monitorAtLoss,
               "and the headphones really had stopped taking audio by then");
    }
    else
    {
        check (reported.empty(), "with nothing reported as failing");
        check (glitches == 0, "and no monitor glitches on a healthy output");
        check (monitorAtEnd > monitorAtLoss,
               "while the headphones went on being fed for the whole take");
    }

    std::remove (path.c_str());
    std::remove ((dir + "/MIX.wav").c_str());

    std::printf ("%s (%d failing)\n", failures == 0 ? "PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
