// The §3.2 drift loop on real, independent clocks, as a time series.
//
// Runs the real CaptureCoordinator on the real ALSA backend against the
// fixture's virtual microphones, which Tools/alsa_readi_shim.cpp paces to
// their own clocks when preloaded with MMA_SIM_REALTIME=1. Every interval it
// prints one line per channel: ring fill, the PI loop's own PPM, the PPM the
// app would report (relative to the clock master), and what that ring has
// dropped or run dry. Alongside: how fast the consumer is actually pulling
// against the wall clock, which is the one number the loop cannot see.
//
//   MMA_SIM_REALTIME=1 MMA_SIM_PPM="mma_mic1=150,mma_mic2=-150,mma_out=0"
//   LD_PRELOAD=build-app/libalsa_readi_shim.so
//     ./build-app/clock_trace [seconds] [block] [output-device|-] [interval-ms] [dir]
//
// This is for reading, not for pass/fail; Tools/e2e_realtime_mics.sh is the
// gate. It exists because a gate that says "700 samples were lost" cannot
// say which ring lost them, when, or what the loop was doing at the time.
#include "Core/CaptureCoordinator.h"
#include "Platform/AlsaBackend.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace mma;

int main (int argc, char** argv)
{
    const double seconds = argc > 1 ? std::atof (argv[1]) : 120.0;
    const int block = argc > 2 ? std::atoi (argv[2]) : 64;
    const std::string outputArg = argc > 3 ? argv[3] : "-";
    const int intervalMs = argc > 4 ? std::atoi (argv[4]) : 250;
    const std::string dir = argc > 5 ? argv[5] : "/tmp/clock-trace";
    const std::string outputDevice = outputArg == "-" ? std::string() : outputArg;

    const double rate = 48000.0;
    std::filesystem::create_directories (dir);

    AlsaBackend backend;
    CaptureCoordinator coordinator (backend, rate, block);

    std::vector<CaptureChannel> mics = {
        { "mma_mic1", "Mic-440",  "01_Mic-440",  0.0f },
        { "mma_mic2", "Mic-1000", "02_Mic-1000", 0.0f },
    };

    if (const char* extra = std::getenv ("MMA_TRACE_THIRD_MIC"); extra != nullptr && *extra != '\0')
        mics.push_back ({ extra, "Third", "03_Third", 0.0f });

    if (! coordinator.startMonitoring (mics, outputDevice))
    {
        std::printf ("startMonitoring failed: %s\n", coordinator.getMonitorProblem().c_str());
        return 1;
    }

    std::printf ("# block=%d output=%s software_clock=%s channels=%zu master=%d\n",
                 block, outputDevice.empty() ? "none" : outputDevice.c_str(),
                 coordinator.hasOutputStream() ? "standby" : "driving",
                 mics.size(), coordinator.getMasterChannel());

    if (! coordinator.startRecording (dir, 16, "2026-09-24T00:00:00Z"))
    {
        std::printf ("startRecording failed: %s\n", coordinator.getRecordingProblem().c_str());
        return 1;
    }

    std::printf ("# t  accepted  consumer_ppm  | ch fill loop_ppm measured_ppm underrun overrun | ... | clock diagnostics\n");

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    auto lastTick = t0;

    // The consumer's rate is measured from a point after every ring has
    // pre-rolled, so start-up silence is not read as a slow clock.
    bool baselined = false;
    uint64_t baseAccepted = 0;
    clock::time_point baseTime {};

    while (true)
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (intervalMs));

        const auto now = clock::now();
        const double t = std::chrono::duration<double> (now - t0).count();
        const double sinceTick = std::chrono::duration<double> (now - lastTick).count();
        lastTick = now;

        coordinator.tickDriftReporting (sinceTick);

        const auto accepted = coordinator.getFramesAccepted();

        if (! baselined && t >= 2.0)
        {
            baselined = true;
            baseAccepted = accepted;
            baseTime = now;
        }

        double consumerPpm = 0.0;

        if (baselined)
        {
            const double elapsed = std::chrono::duration<double> (now - baseTime).count();
            if (elapsed > 0.5)
                consumerPpm = (static_cast<double> (accepted - baseAccepted) / (elapsed * rate) - 1.0) * 1.0e6;
        }

        std::printf ("%7.2f %10llu %+8.1f", t, static_cast<unsigned long long> (accepted), consumerPpm);

        for (int ch = 0; ch < static_cast<int> (mics.size()); ++ch)
        {
            const auto seams = coordinator.getChannelSeams (ch);
            std::printf (" | %d %.3f %+7.1f %+7.1f %6llu %6llu p%llu h%llu s%llu", ch,
                         coordinator.getChannelFillFraction (ch),
                         coordinator.getChannelRawDriftPpm (ch),
                         coordinator.hasChannelDriftMeasurement (ch) ? coordinator.getChannelMeasuredDriftPpm (ch) : 0.0,
                         static_cast<unsigned long long> (coordinator.getUnderrunSamples (ch)),
                         static_cast<unsigned long long> (coordinator.getChannelOverrunSamples (ch)),
                         static_cast<unsigned long long> (seams.primes),
                         static_cast<unsigned long long> (seams.holds),
                         static_cast<unsigned long long> (seams.skips));
        }

        const auto diag = coordinator.getClockDiagnostics();
        std::printf (" | clock late_max=%.2fms catchup=%llu pull_max=%.2fms\n",
                     diag.maxWakeLateUs / 1000.0,
                     static_cast<unsigned long long> (diag.catchUpTicks),
                     diag.maxPullUs / 1000.0);
        std::fflush (stdout);

        if (t >= seconds)
            break;
    }

    coordinator.stopRecording();
    coordinator.stopMonitoring();

    std::printf ("# done: overrun_total=%llu frames_dropped_by_writer=%llu\n",
                 static_cast<unsigned long long> (coordinator.getOverrunSamples()),
                 static_cast<unsigned long long> (coordinator.getFramesDropped()));
    return 0;
}
