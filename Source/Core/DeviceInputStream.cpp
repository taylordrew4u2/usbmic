#include "DeviceInputStream.h"
#include <algorithm>
#include <cmath>

namespace mma {

DeviceInputStream::DeviceInputStream (double sampleRate) noexcept
    : ring (static_cast<size_t> (kRingBlocks) * 64), compensator (sampleRate)
{
}

void DeviceInputStream::prepare (double sampleRate, int bufferSizeSamples)
{
    const auto block = static_cast<size_t> (std::max (1, bufferSizeSamples));

    ring.reset (block * static_cast<size_t> (kRingBlocks));

    // §5.4: this is monitor latency, so it is a fixed small number of blocks
    // rather than a fraction of the ring. The remaining six blocks of capacity
    // are headroom the loop never intends to use.
    targetFillSamples = block * static_cast<size_t> (kPreRollBlocks);

    compensator = DriftCompensator (sampleRate);
    previousSample = 0.0f;
    currentSample = 0.0f;
    phase = 0.0;
    primed = false;
    started = false;

    driftPpm.store (0.0, std::memory_order_relaxed);
    excessDrift.store (false, std::memory_order_relaxed);
    underruns.store (0, std::memory_order_relaxed);
}

void DeviceInputStream::pushBlock (const float* samples, int numSamples) noexcept
{
    if (samples == nullptr || numSamples <= 0)
        return;

    // A full ring means the consumer is not keeping up. Dropping the newest
    // samples is the only lock-free option; the loop reacts by speeding this
    // device's playout back up.
    ring.write (samples, static_cast<size_t> (numSamples));
}

bool DeviceInputStream::readOne (float& out) noexcept
{
    return ring.read (&out, 1) == 1;
}

void DeviceInputStream::pull (float* destination, int numSamples) noexcept
{
    if (destination == nullptr || numSamples <= 0)
        return;

    if (! channelLive.load (std::memory_order_relaxed))
    {
        // §6.5: the channel survives the mic leaving, and yields silence.
        std::fill (destination, destination + numSamples, 0.0f);
        return;
    }

    // Pre-roll. The output clock starts before any device has delivered, so
    // consuming here would emit a click at the top of every take and count
    // audio as lost that had simply not arrived yet.
    if (! started)
    {
        if (ring.availableForRead() < targetFillSamples)
        {
            std::fill (destination, destination + numSamples, 0.0f);
            return;
        }

        started = true;
    }

    // §3.2: fill error drives the loop. Positive means this device is producing
    // faster than the master consumes, so its ratio must rise to drain it.
    const auto available = ring.availableForRead();
    const double fillError = static_cast<double> (available) - static_cast<double> (targetFillSamples);

    if (! isMaster)
    {
        compensator.update (fillError, numSamples);
        driftPpm.store (compensator.getPpm(), std::memory_order_relaxed);
    }

    // §3.1: the master is the timebase. Resampling it would mean correcting the
    // reference against itself.
    const double ratio = isMaster ? 1.0 : compensator.getRatio();

    if (! primed)
    {
        if (! readOne (currentSample))
        {
            std::fill (destination, destination + numSamples, 0.0f);
            underruns.fetch_add (static_cast<uint64_t> (numSamples), std::memory_order_relaxed);
            return;
        }

        previousSample = currentSample;
        phase = 0.0;
        primed = true;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        destination[i] = previousSample
                         + static_cast<float> (phase) * (currentSample - previousSample);

        phase += ratio;

        while (phase >= 1.0)
        {
            previousSample = currentSample;

            if (! readOne (currentSample))
            {
                // Nothing left to interpolate towards. Hold the last sample for
                // the remainder of the block rather than emitting a click, count
                // the shortfall exactly once, and stop.
                //
                // The previous version broke out of the inner loop and let the
                // outer one continue, which re-entered here on the very next
                // sample -- the ring was still dry -- and added (numSamples - i)
                // again each time. A single dry 64-sample block was reported as
                // ~2000 lost samples instead of the few dozen that were actually
                // missing. §0.1 makes any non-zero underrun the failure the user
                // is shown, so an inflated count is a false alarm about the one
                // thing this app promises not to do.
                currentSample = previousSample;
                phase = 0.0;

                const int remaining = numSamples - (i + 1);

                if (remaining > 0)
                {
                    std::fill (destination + i + 1, destination + numSamples, previousSample);
                    underruns.fetch_add (static_cast<uint64_t> (remaining), std::memory_order_relaxed);
                }

                return;
            }

            phase -= 1.0;
        }
    }
}

void DeviceInputStream::tickDriftReporting (double elapsedSeconds) noexcept
{
    compensator.updateSustainedDriftFlag (elapsedSeconds);
    excessDrift.store (compensator.isSustainedExcessDrift(), std::memory_order_relaxed);
}

} // namespace mma
