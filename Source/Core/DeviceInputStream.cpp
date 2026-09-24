#include "DeviceInputStream.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace mma {

static_assert (std::atomic<double>::is_always_lock_free,
               "DeviceInputStream requires lock-free double atomics on the audio thread");
static_assert (std::atomic<int64_t>::is_always_lock_free,
               "DeviceInputStream requires lock-free 64-bit atomics on the audio thread");

namespace {

int64_t steadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds> (
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::atomic<DeviceInputStream::NowNsFn> clockOverride { nullptr };
std::atomic<bool> virtualFillEnabled { true };

int64_t nowNs()
{
    const auto fn = clockOverride.load (std::memory_order_relaxed);
    return fn != nullptr ? fn() : steadyNowNs();
}

} // namespace

void DeviceInputStream::setClockForTesting (NowNsFn fn) noexcept
{
    clockOverride.store (fn, std::memory_order_relaxed);
}

void DeviceInputStream::setVirtualFillForTesting (bool enabled) noexcept
{
    virtualFillEnabled.store (enabled, std::memory_order_relaxed);
}

DeviceInputStream::DeviceInputStream (double sampleRate) noexcept
    : ring (static_cast<size_t> (kRingBlocks) * 64), compensator (sampleRate), rate (sampleRate)
{
}

void DeviceInputStream::prepare (double sampleRate, int bufferSizeSamples)
{
    const auto block = static_cast<size_t> (std::max (1, bufferSizeSamples));

    ring.reset (block * static_cast<size_t> (kRingBlocks));
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;

    // §5.4: this is monitor latency, so it is a fixed small number of blocks
    // rather than a fraction of the ring. The remaining fourteen blocks of
    // capacity are headroom the loop never intends to use: room for the
    // driver's whole ring to land at once after a late wake.
    targetFillSamples = block * static_cast<size_t> (kPreRollBlocks);

    compensator = DriftCompensator (sampleRate);
    previousSample = 0.0f;
    currentSample = 0.0f;
    phase = 0.0;
    primed = false;
    started.store (false, std::memory_order_relaxed);
    fillAverage = 0.0;
    fillAverageValid = false;
    silenceOwed = 0.0;
    pullsSinceSilence = 0;

    driftPpm.store (0.0, std::memory_order_relaxed);
    excessDrift.store (false, std::memory_order_relaxed);
    driftReportingResetEpoch.store (0, std::memory_order_relaxed);
    excessDriftSeconds = 0.0;
    observedDriftReportingResetEpoch = 0;
    underruns.store (0, std::memory_order_relaxed);
    lossEvents.store (0, std::memory_order_relaxed);

    lastPushNs.store (0, std::memory_order_relaxed);
    lastPushSamples.store (0, std::memory_order_relaxed);
    pushedSamples.store (0, std::memory_order_relaxed);
    lastPullNs.store (0, std::memory_order_relaxed);
    pulledSamples.store (0, std::memory_order_relaxed);
    resetMeasurementWindow();

    // Reset with its siblings. It was the one counter here that was not, so it
    // ran for the life of the stream while the sentence built from it -- "about
    // N seconds lost so far" -- describes the current take. Everything overrun
    // while merely monitoring, or during an earlier take, was added to this
    // take's figure.
    overrunSamples.store (0, std::memory_order_relaxed);
}

void DeviceInputStream::pushBlock (const float* samples, int numSamples) noexcept
{
    if (samples == nullptr || numSamples <= 0)
        return;

    // A full ring means the consumer is not keeping up. Dropping the newest
    // samples is the only lock-free option; the loop reacts by speeding this
    // device's playout back up. What is dropped is COUNTED: this used to
    // discard the return value, and a stalled consumer lost audio with every
    // counter on the screen still reading zero.
    const auto written = ring.write (samples, static_cast<size_t> (numSamples));

    if (written < static_cast<size_t> (numSamples))
    {
        overrunSamples.fetch_add (static_cast<uint64_t> (numSamples) - written, std::memory_order_relaxed);
        lossEvents.fetch_add (1, std::memory_order_relaxed);
    }

    // Delivered, whether or not it fit: the measurement wants the device's
    // clock, and a dropped sample was still a sample the device produced.
    pushedSamples.fetch_add (static_cast<uint64_t> (numSamples), std::memory_order_relaxed);

    // Published after the write, so a consumer that sees this stamp also sees
    // the block behind it in the ring's own release/acquire.
    lastPushSamples.store (numSamples, std::memory_order_relaxed);
    lastPushNs.store (nowNs(), std::memory_order_release);
}

bool DeviceInputStream::readOne (float& out) noexcept
{
    return ring.read (&out, 1) == 1;
}

double DeviceInputStream::virtualFillNow (size_t available) const noexcept
{
    // The ring level, read at a pull, only ever moves in whole device blocks:
    // a device 150 PPM slow lowers it by 7 samples a second, but the pull sees
    // the same number for nine seconds and then a step of 64. A loop driven by
    // that staircase either does nothing or sees a whole block of error at
    // once -- and one block, at 5 PPM per sample, was the 200 PPM clamp.
    //
    // So the pull is placed within the device's current block instead: how far
    // the device has got since its last delivery, from that delivery's
    // timestamp and its block's nominal duration, is audio the device has
    // captured but not yet handed over. The level with that fraction folded in
    // is continuous across the delivery, and it is what the loop steers.
    // Equivalently: the lowest level this pull could have seen had it landed
    // just before the next delivery, which is the cushion that actually
    // matters for an underrun.
    const auto pushNs = lastPushNs.load (std::memory_order_acquire);
    const auto pushSamples = lastPushSamples.load (std::memory_order_relaxed);

    double virtualFill = static_cast<double> (available);

    if (pushNs > 0 && pushSamples > 0 && virtualFillEnabled.load (std::memory_order_relaxed))
    {
        const double periodNs = static_cast<double> (pushSamples) * 1.0e9 / rate;
        const double elapsedNs = static_cast<double> (nowNs() - pushNs);
        const double fraction = std::clamp (elapsedNs / periodNs, 0.0, 1.0);

        virtualFill -= static_cast<double> (pushSamples) * (1.0 - fraction);
    }

    return virtualFill;
}

double DeviceInputStream::fillErrorNow (size_t available) noexcept
{
    const double virtualFill = virtualFillNow (available);

    // Smoothed a little: a delivery that lands late by a fraction of a block
    // reads as a dip until it arrives, and the loop should see the trend
    // rather than the tremor. Thirty-two blocks is 43 ms at 64/48k, nothing
    // against a loop whose slew takes thirty seconds to cross 150 PPM.
    if (! fillAverageValid)
    {
        fillAverage = virtualFill;
        fillAverageValid = true;
    }
    else
    {
        fillAverage += (virtualFill - fillAverage) * kFillSmoothing;
    }

    return fillAverage - static_cast<double> (targetFillSamples);
}

void DeviceInputStream::noteSilence (int samples) noexcept
{
    // Capped at what the ring can hold above target, which is the most late
    // audio a burst could ever leave there to be skipped.
    const double cap = static_cast<double> (ring.capacity()) - static_cast<double> (targetFillSamples);
    silenceOwed = std::min (cap, silenceOwed + static_cast<double> (samples));
    pullsSinceSilence = 0;
}

void DeviceInputStream::skipLateAudio (int numSamples) noexcept
{
    // Measured on the de-quantized level, which is what the loop holds at
    // target; the raw level sits up to a block above it just after a delivery,
    // and that block is not late audio. And never into what this pull itself
    // is about to take: a ring that is chronically short -- pulls larger than
    // the target fill -- owes silence every block, and skipping ahead of a
    // pull that will run dry anyway only makes it run dry sooner.
    const auto available = ring.availableForRead();
    const double excess = std::min (virtualFillNow (available) - static_cast<double> (targetFillSamples),
                                    static_cast<double> (available) - static_cast<double> (numSamples + 1));

    // Supplied again. Give the burst a ring's worth of pulls to land, then
    // write the silence off, skipped or not: what is still owed after that
    // is not coming.
    if (available > 0 && ++pullsSinceSilence > kRingBlocks)
    {
        silenceOwed = 0.0;
        return;
    }

    // A burst is whole blocks by nature. Anything under one is the loop's
    // own jitter around its target, and skipping it -- a few samples, with
    // the interpolator restarted each time -- put a click in every block
    // for as long as the owed silence lasted, which, since only a pull that
    // skipped nothing counted towards writing it off, was indefinitely.
    const double block = static_cast<double> (std::max (1, lastPushSamples.load (std::memory_order_relaxed)));

    if (excess >= block)
    {
        const auto skip = static_cast<size_t> (std::min (silenceOwed, excess));
        const auto skipped = ring.discard (skip);

        // A burst that lands between two of the device's own periods leaves
        // the placement above short by up to a block until the next period
        // arrives; what is still owed after that waits for it.
        silenceOwed = std::max (0.0, silenceOwed - static_cast<double> (skipped));

        // The interpolator's pair predates the gap; the level's average was
        // taken while the ring was dry. Both restart from what is there now.
        previousSample = 0.0f;
        currentSample = 0.0f;
        phase = 0.0;
        primed = false;
        fillAverageValid = false;
    }
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

    // §6.5 reconnection. Nothing consumed this stream while the channel was
    // dead, so everything here still describes the instant the microphone left:
    // a ring holding pre-gap audio, an interpolator holding the last sample
    // before it, a loop whose fill error is meaningless, and started/primed
    // both saying the stream is running.
    //
    // Resuming from that puts audio from before the gap after it -- or, when
    // the ring is dry because the old stream died with the device, holds that
    // last sample as a DC offset for the rest of the take while counting every
    // block as lost audio. Both are worse than the silence they replace, and
    // §0.1 makes the second one a false alarm about the one failure this app
    // promises not to have.
    //
    // So the channel restarts as if the stream had just opened: pre-roll again,
    // stay silent until it is buffered, and only then consume. Real-time safe --
    // a read-index store, a few scalars, no allocation (§11).
    if (restartPending.exchange (false, std::memory_order_acquire))
    {
        ring.clear();
        compensator.reset();

        previousSample = 0.0f;
        currentSample = 0.0f;
        phase = 0.0;
        primed = false;
        started.store (false, std::memory_order_relaxed);
        fillAverageValid = false;
        silenceOwed = 0.0;
        pullsSinceSilence = 0;

        driftPpm.store (0.0, std::memory_order_relaxed);
        excessDrift.store (false, std::memory_order_relaxed);
        driftReportingResetEpoch.fetch_add (1, std::memory_order_release);
    }

    // Pre-roll. The output clock starts before any device has delivered, so
    // consuming here would emit a click at the top of every take and count
    // audio as lost that had simply not arrived yet.
    if (! started.load (std::memory_order_relaxed))
    {
        if (ring.availableForRead() < targetFillSamples)
        {
            std::fill (destination, destination + numSamples, 0.0f);
            return;
        }

        started.store (true, std::memory_order_relaxed);
    }

    // Count, then stamp: a reporter that reads the stamp sees a count at most
    // one block newer than it, never older.
    pulledSamples.fetch_add (static_cast<uint64_t> (numSamples), std::memory_order_relaxed);
    lastPullNs.store (nowNs(), std::memory_order_release);

    // §3.2: fill error drives the loop. Positive means this device is producing
    // faster than this stream is being consumed, so its ratio must rise to
    // drain it.
    //
    // Every channel is steered, the clock master included. The thing this
    // stream is pulled by is the output device's callback, not any microphone,
    // so exempting one mic from correction does not make it the timebase -- it
    // just leaves that one mic uncorrected against a clock it has no
    // relationship to. Its ring then walks to one end of its travel and stays
    // there, dropping arrivals when full or holding the last sample when dry,
    // which is drift on the one channel §3.1 nominated as the reference. See
    // §3.1/§3.2 in docs/SPEC.md for why the reference is a reporting role here
    // rather than a correction one.
    //
    // Audio arriving late for a span already written as silence is skipped
    // before the level is read, so the loop never sees it as fill.
    if (silenceOwed > 0.0)
        skipLateAudio (numSamples);

    const double fillError = fillErrorNow (ring.availableForRead());

    compensator.update (fillError, numSamples);
    driftPpm.store (compensator.getPpm(), std::memory_order_relaxed);

    const double ratio = compensator.getRatio();

    if (! primed)
    {
        if (! readOne (currentSample))
        {
            std::fill (destination, destination + numSamples, 0.0f);
            underruns.fetch_add (static_cast<uint64_t> (numSamples), std::memory_order_relaxed);
            lossEvents.fetch_add (1, std::memory_order_relaxed);
            noteSilence (numSamples);
            return;
        }

        previousSample = currentSample;
        phase = 0.0;
        primed = true;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        // Advance to the pair this output sample lies between BEFORE writing
        // it, never after. Reading ahead after the last sample of a block
        // meant a block that consumed the ring exactly -- every source sample
        // used, none to spare -- ended in a failed read, and the interpolator
        // was zeroed and re-primed for a starvation that had not happened:
        // its fractional phase snapped to zero, a sub-sample step in the
        // output, once per block in a ring held one block from empty.
        while (phase >= 1.0)
        {
            previousSample = currentSample;

            if (! readOne (currentSample))
            {
                // Nothing left to interpolate towards. The source did not
                // provide the remainder of this block, so write silence for
                // exactly that missing span, count it once, and stop. Holding
                // the last non-zero sample here manufactures a DC signal that
                // was never captured; on a sustained starvation that offset is
                // both audible and unsafe in the monitor path.
                //
                // The previous version broke out of the inner loop and let the
                // outer one continue, which re-entered here on the very next
                // sample -- the ring was still dry -- and added (numSamples - i)
                // again each time. A single dry 64-sample block was reported as
                // ~2000 lost samples instead of the few dozen that were actually
                // missing. §0.1 makes any non-zero underrun the failure the user
                // is shown, so an inflated count is a false alarm about the one
                // thing this app promises not to do.
                // The next pull must not reuse the sample we just emitted.
                // Leaving the interpolator primed makes every later dry block
                // begin with that stale value before discovering the empty
                // ring, producing one click and under-counting the loss by one
                // frame per block.
                previousSample = 0.0f;
                currentSample = 0.0f;
                phase = 0.0;
                primed = false;

                const int remaining = numSamples - i;

                std::fill (destination + i, destination + numSamples, 0.0f);
                underruns.fetch_add (static_cast<uint64_t> (remaining), std::memory_order_relaxed);
                lossEvents.fetch_add (1, std::memory_order_relaxed);

                // Exactly the silence written, no more: the sample this read
                // did not get is the one the next pull primes with, and it
                // comes out where it would have.
                noteSilence (remaining);
                return;
            }

            phase -= 1.0;
        }

        destination[i] = previousSample
                         + static_cast<float> (phase) * (currentSample - previousSample);

        phase += ratio;
    }
}

void DeviceInputStream::resetMeasurementWindow() noexcept
{
    windowStart = 0;
    windowCount = 0;
    measured.store (false, std::memory_order_relaxed);
    measuredPpm.store (0.0, std::memory_order_relaxed);
    measurementSeconds.store (0.0, std::memory_order_relaxed);
    deviceRatePpm.store (0.0, std::memory_order_relaxed);
    consumerRatePpm.store (0.0, std::memory_order_relaxed);
}

namespace {

// Least-squares slope of y against x over a ring of points; false when the
// points do not spread in x. Two passes, so a 60-second window of sample
// counts near 3e6 keeps its precision.
template <typename Point, typename GetX, typename GetY>
bool slopeOf (const Point* ring, int start, int count, int capacity, GetX getX, GetY getY, double& slope)
{
    double meanX = 0.0, meanY = 0.0;
    for (int i = 0; i < count; ++i)
    {
        const auto& p = ring[(start + i) % capacity];
        meanX += getX (p);
        meanY += getY (p);
    }
    meanX /= count;
    meanY /= count;

    double sxy = 0.0, sxx = 0.0;
    for (int i = 0; i < count; ++i)
    {
        const auto& p = ring[(start + i) % capacity];
        const double dx = getX (p) - meanX;
        sxy += dx * (getY (p) - meanY);
        sxx += dx * dx;
    }

    if (sxx <= 0.0)
        return false;

    slope = sxy / sxx;
    return true;
}

} // namespace

void DeviceInputStream::tickDriftReporting (double elapsedSeconds, double referencePpm) noexcept
{
    // §3.3 judges a device against the clock master, not against the output
    // stream. Passing the master's own measurement as the reference is what
    // keeps a skewed *output* device from flagging every microphone at once:
    // that skew lands in every channel's figure equally and subtracts out here.
    const auto resetEpoch = driftReportingResetEpoch.load (std::memory_order_acquire);
    if (! channelLive.load (std::memory_order_relaxed)
        || resetEpoch != observedDriftReportingResetEpoch)
    {
        observedDriftReportingResetEpoch = resetEpoch;
        excessDriftSeconds = 0.0;
        excessDrift.store (false, std::memory_order_relaxed);
        resetMeasurementWindow();
        return;
    }

    // The measurement. Each tick records, for each side, its sample count
    // paired with the timestamp of the block that brought it there; the slope
    // of count against time is that clock's rate. The device's rate over the
    // consumer's is the figure. A count and its stamp are two atomics, so a
    // tick can see a count one block newer than its stamp: a block of noise
    // per point, white, which a fit over hundreds of points averages down --
    // a difference of two points would carry it whole, 40 PPM at a minute.
    if (! started.load (std::memory_order_relaxed))
        return;

    const auto pushNs = lastPushNs.load (std::memory_order_acquire);
    const auto pullNs = lastPullNs.load (std::memory_order_acquire);

    if (pushNs <= 0 || pullNs <= 0)
        return;

    const RatePoint point { static_cast<double> (pushNs) * 1.0e-9,
                            static_cast<double> (pushedSamples.load (std::memory_order_relaxed)),
                            static_cast<double> (pullNs) * 1.0e-9,
                            static_cast<double> (pulledSamples.load (std::memory_order_relaxed)) };

    if (windowCount == kMaxWindowPoints)
    {
        windowStart = (windowStart + 1) % kMaxWindowPoints;
        --windowCount;
    }

    window[(windowStart + windowCount) % kMaxWindowPoints] = point;
    ++windowCount;

    // Drop what has aged out of the window, keeping a little more than the
    // named length so the fit always spans it.
    while (windowCount > 2
           && point.pullSeconds - window[windowStart].pullSeconds > kMeasurementSeconds * 1.25)
    {
        windowStart = (windowStart + 1) % kMaxWindowPoints;
        --windowCount;
    }

    const double span = point.pullSeconds - window[windowStart].pullSeconds;
    measurementSeconds.store (span, std::memory_order_relaxed);

    if (windowCount >= 8 && span >= kMeasurementSeconds)
    {
        double deviceRate = 0.0, consumerRate = 0.0;

        const bool ok = slopeOf (window, windowStart, windowCount, kMaxWindowPoints,
                                 [] (const RatePoint& p) { return p.pushSeconds; },
                                 [] (const RatePoint& p) { return p.pushed; }, deviceRate)
                     && slopeOf (window, windowStart, windowCount, kMaxWindowPoints,
                                 [] (const RatePoint& p) { return p.pullSeconds; },
                                 [] (const RatePoint& p) { return p.pulled; }, consumerRate)
                     && consumerRate > 0.0;

        if (ok)
        {
            measuredPpm.store ((deviceRate / consumerRate - 1.0) * 1.0e6, std::memory_order_relaxed);
            deviceRatePpm.store ((deviceRate / rate - 1.0) * 1.0e6, std::memory_order_relaxed);
            consumerRatePpm.store ((consumerRate / rate - 1.0) * 1.0e6, std::memory_order_relaxed);
            measured.store (true, std::memory_order_relaxed);
        }
    }

    // §3.3's flag, from the measurement, once there is one. Before that there
    // is nothing honest to flag: the loop's own figure is still settling.
    if (! measured.load (std::memory_order_relaxed))
    {
        excessDriftSeconds = 0.0;
        excessDrift.store (false, std::memory_order_relaxed);
        return;
    }

    const double relativePpm = measuredPpm.load (std::memory_order_relaxed) - referencePpm;

    if (std::abs (relativePpm) > kExcessDriftThresholdPpm)
    {
        excessDriftSeconds += std::max (0.0, elapsedSeconds);
        if (excessDriftSeconds >= kExcessDriftSustainSeconds)
            excessDrift.store (true, std::memory_order_relaxed);
    }
    else
    {
        excessDriftSeconds = 0.0;
        excessDrift.store (false, std::memory_order_relaxed);
    }
}

} // namespace mma
