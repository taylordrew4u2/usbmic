// §3.4 acceptance attempt: four dissimilar clocks, four hours, inter-channel
// drift measured at the end. Drives DeviceInputStream directly -- alignment is
// decided there, and this avoids writing 7 GB of WAV to measure it.
//
// What this CAN establish: that the §3.2 loop holds four mismatched clocks
// aligned over the full duration without accumulating error or underrunning.
// What it CANNOT: how real crystals behave. Simulated offsets are steady;
// real ones wander with temperature.
//
// The clock master is deliberately given a NON-zero offset below. It used to be
// 0.0 -- a crystal identical to the output stream's -- which is what let this
// gate pass while the master was the one channel exempt from correction: the
// exemption cost nothing only because the simulation had already assumed the
// thing the exemption needed. At a realistic 40 PPM the same gate failed by
// 8 ms after half an hour. Every channel is now corrected onto the pull clock
// (§3.2), so keep this offset non-zero: it is what makes the master's own
// crystal part of what the gate measures.
#include "Core/DeviceInputStream.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace mma;

namespace {

struct Mic {
    const char* name;
    double ppm;                  // its clock offset from nominal
    DeviceInputStream stream { 48000.0 };
    double debt = 0.0;
    long long pushed = 0;
    long long markerSourceIndex = -1; // where the impulse was injected
    long long markerOutputIndex = -1; // where it came out
    std::vector<float> in;
};

} // namespace

int main (int argc, char** argv) {
    const double rate = 48000.0;
    const int block = 64;
    const double hours = argc > 1 ? std::atof (argv[1]) : 4.0;
    const long long totalBlocks = static_cast<long long> (rate * 3600.0 * hours / block);

    // Deliberately dissimilar, and wider than USB audio devices typically are.
    std::vector<Mic> mics (4);
    const double offsets[] = { 40.0, 100.0, -80.0, 45.0 };
    const char* names[] = { "Yeti-A (master)", "Yeti-B", "Condenser-C", "Dynamic-D" };

    for (size_t i = 0; i < mics.size(); ++i) {
        mics[i].name = names[i];
        mics[i].ppm = offsets[i];
        mics[i].stream.prepare (rate, block);
        mics[i].in.resize (block + 8);
    }

    // The marker goes in at the same source instant for every mic, near the end
    // once the loops have long since settled. Where it lands in each output is
    // the inter-channel alignment.
    const long long markerBlock = totalBlocks - static_cast<long long> (rate * 30.0 / block);

    std::vector<float> out (block);
    long long outputIndex = 0;

    for (long long blk = 0; blk < totalBlocks; ++blk) {
        for (auto& m : mics) {
            m.debt += block * m.ppm * 1e-6;
            int count = block;
            if (m.debt >= 1.0) { count += 1; m.debt -= 1.0; }
            else if (m.debt <= -1.0) { count -= 1; m.debt += 1.0; }

            for (int s = 0; s < count; ++s) m.in[s] = 0.0f;

            if (blk == markerBlock) {
                m.in[0] = 1.0f;                 // impulse
                m.markerSourceIndex = m.pushed;
            }

            m.stream.pushBlock (m.in.data(), count);
            m.pushed += count;
        }

        for (auto& m : mics) {
            m.stream.pull (out.data(), block);

            if (m.markerSourceIndex >= 0 && m.markerOutputIndex < 0) {
                for (int s = 0; s < block; ++s) {
                    if (out[s] > 0.05f) { m.markerOutputIndex = outputIndex + s; break; }
                }
            }
        }

        outputIndex += block;

        if (blk % (totalBlocks / 8) == 0)
            std::printf ("  %5.1f%%  ch1 drift %+7.2f PPM  ch2 %+7.2f  ch3 %+7.2f\n",
                         100.0 * static_cast<double> (blk) / static_cast<double> (totalBlocks),
                         mics[1].stream.getDriftPpm(), mics[2].stream.getDriftPpm(),
                         mics[3].stream.getDriftPpm());
    }

    std::printf ("\n%.1f hours simulated, %lld blocks.\n\n", hours, totalBlocks);
    std::printf ("%-18s %10s %12s %14s %12s\n",
                 "device", "clock PPM", "measured", "marker out", "underruns");

    long long minMarker = -1, maxMarker = -1;
    for (auto& m : mics) {
        std::printf ("%-18s %10.1f %12.2f %14lld %12llu\n", m.name, m.ppm,
                     m.stream.getDriftPpm(), m.markerOutputIndex,
                     (unsigned long long) m.stream.getUnderrunSamples());
        if (m.markerOutputIndex < 0) continue;
        if (minMarker < 0 || m.markerOutputIndex < minMarker) minMarker = m.markerOutputIndex;
        if (m.markerOutputIndex > maxMarker) maxMarker = m.markerOutputIndex;
    }

    if (minMarker < 0) { std::printf ("\nmarker never surfaced -- inconclusive\n"); return 1; }

    const double spreadSamples = static_cast<double> (maxMarker - minMarker);
    const double spreadMs = spreadSamples / rate * 1000.0;

    std::printf ("\n§3.4: inter-channel drift after %.1f h = %.0f samples = %.3f ms (ceiling 1.000 ms)\n",
                 hours, spreadSamples, spreadMs);

    bool ok = spreadMs < 1.0;
    for (auto& m : mics) if (m.stream.getUnderrunSamples() > 0) ok = false;

    std::printf ("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
