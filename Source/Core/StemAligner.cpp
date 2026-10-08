#include "StemAligner.h"
#include <algorithm>

namespace mma {

void StemOffsetQueue::clear() noexcept
{
    head.store (0, std::memory_order_relaxed);
    tail.store (0, std::memory_order_relaxed);
}

bool StemOffsetQueue::push (const StemOffsetEvent& event) noexcept
{
    const auto h = head.load (std::memory_order_relaxed);
    const auto t = tail.load (std::memory_order_acquire);

    if (h - t >= kCapacity)
        return false;

    slots[h % kCapacity] = event;
    head.store (h + 1, std::memory_order_release);
    return true;
}

bool StemOffsetQueue::peek (StemOffsetEvent& event) const noexcept
{
    const auto t = tail.load (std::memory_order_relaxed);
    const auto h = head.load (std::memory_order_acquire);

    if (t == h)
        return false;

    event = slots[t % kCapacity];
    return true;
}

void StemOffsetQueue::pop() noexcept
{
    tail.store (tail.load (std::memory_order_relaxed) + 1, std::memory_order_release);
}

void StemAligner::prepare (int numChannels)
{
    lines.clear();

    for (int ch = 0; ch < std::max (0, numChannels); ++ch)
    {
        auto line = std::make_unique<Line>();

        // One more than the longest offset: a sample is put in before the
        // oldest one is taken out.
        line->buffer.assign (static_cast<size_t> (kMaxOffsetSamples) + 1, 0.0f);
        lines.push_back (std::move (line));
    }

    exact.store (true, std::memory_order_relaxed);
}

void StemAligner::setOffset (int channel, int samples) noexcept
{
    if (channel < 0 || channel >= static_cast<int> (lines.size()))
        return;

    auto& line = *lines[static_cast<size_t> (channel)];

    if (samples < 0 || samples > kMaxOffsetSamples)
        exact.store (false, std::memory_order_relaxed);

    const int wanted = std::clamp (samples, 0, kMaxOffsetSamples);
    const int current = line.offset.load (std::memory_order_relaxed);

    if (wanted == current)
        return;

    if (wanted > current)
    {
        // Held back further. First by no longer taking out silence still to
        // arrive (a shorter offset not yet fully applied), then by writing
        // silence now while the channel's audio waits behind it. Before its
        // first sample, that is the take's starting alignment rather than a
        // change to it.
        auto longer = static_cast<size_t> (wanted - current);

        const auto unskipped = std::min (longer, line.skipOwed);
        line.skipOwed -= unskipped;
        longer -= unskipped;

        (line.started ? line.silenceOwed : line.paddingOwed) += longer;
    }
    else
    {
        // Brought forward: first by not writing silence still owed. Then by
        // taking out the move's own silence -- a channel brought forward is
        // one whose stream has just moved later for its device's larger
        // block, and that move put exactly this much silence in its stream:
        // the run of it that has already arrived (the newest samples
        // waiting, while they are silent), and then as much of it as is
        // still arriving (process(), until a sample that is not silent
        // arrives). Audio is taken out only for what neither finds: the
        // newest samples waiting.
        auto shorter = static_cast<size_t> (current - wanted);

        const auto unwritten = std::min (shorter, line.silenceOwed);
        line.silenceOwed -= unwritten;
        shorter -= unwritten;

        const auto unpadded = std::min (shorter, line.paddingOwed);
        line.paddingOwed -= unpadded;
        shorter -= unpadded;

        const size_t capacity = line.buffer.size();
        size_t silentTail = 0;

        while (silentTail < shorter && silentTail < line.size
               && line.buffer[(line.head + line.size - 1 - silentTail) % capacity] == 0.0f)
            ++silentTail;

        if (silentTail > 0)
        {
            line.size -= silentTail;
            line.dropped.fetch_add (silentTail, std::memory_order_relaxed);
            shorter -= silentTail;
        }

        line.skipOwed += shorter;
    }

    line.offset.store (wanted, std::memory_order_relaxed);
}

void StemAligner::dropNewest (Line& line, size_t count) noexcept
{
    const auto removed = std::min (count, line.size);
    line.size -= removed;

    if (removed > 0)
        line.dropped.fetch_add (removed, std::memory_order_relaxed);
}

void StemAligner::process (int channel, float* samples, size_t frames) noexcept
{
    if (channel < 0 || channel >= static_cast<int> (lines.size()) || samples == nullptr || frames == 0)
        return;

    auto& line = *lines[static_cast<size_t> (channel)];

    if (! line.started)
    {
        line.started = true;
        line.startOffset.store (line.offset.load (std::memory_order_relaxed), std::memory_order_relaxed);
    }

    // Nothing held back and nothing owed: the channel passes straight
    // through, which is every channel of a rig whose devices agree.
    if (line.size == 0 && line.paddingOwed == 0 && line.silenceOwed == 0 && line.skipOwed == 0)
        return;

    const size_t capacity = line.buffer.size();
    uint64_t silenceWritten = 0;
    uint64_t silenceSkipped = 0;

    for (size_t f = 0; f < frames; ++f)
    {
        const float in = samples[f];

        if (line.skipOwed > 0)
        {
            if (in == 0.0f)
            {
                // The rest of the move's silence, arriving: not kept. The
                // channel comes forward by a sample; what goes out is the
                // oldest sample waiting.
                --line.skipOwed;
                ++silenceSkipped;
                samples[f] = line.buffer[line.head];
                line.head = (line.head + 1) % capacity;
                --line.size;
                continue;
            }

            // Audio, and the move is not all accounted for: the rest comes
            // out of the newest samples waiting.
            dropNewest (line, line.skipOwed);
            line.skipOwed = 0;
        }

        line.buffer[(line.head + line.size) % capacity] = in;
        ++line.size;

        if (line.paddingOwed > 0)
        {
            samples[f] = 0.0f;
            --line.paddingOwed;
        }
        else if (line.silenceOwed > 0)
        {
            samples[f] = 0.0f;
            --line.silenceOwed;
            ++silenceWritten;
        }
        else
        {
            samples[f] = line.buffer[line.head];
            line.head = (line.head + 1) % capacity;
            --line.size;
        }
    }

    if (silenceWritten > 0)
        line.silenceInserted.fetch_add (silenceWritten, std::memory_order_relaxed);

    if (silenceSkipped > 0)
        line.dropped.fetch_add (silenceSkipped, std::memory_order_relaxed);
}

int StemAligner::getOffset (int channel) const noexcept
{
    return channel >= 0 && channel < static_cast<int> (lines.size())
               ? lines[static_cast<size_t> (channel)]->offset.load (std::memory_order_relaxed) : 0;
}

int StemAligner::getStartOffset (int channel) const noexcept
{
    return channel >= 0 && channel < static_cast<int> (lines.size())
               ? lines[static_cast<size_t> (channel)]->startOffset.load (std::memory_order_relaxed) : 0;
}

uint64_t StemAligner::getSilenceInserted (int channel) const noexcept
{
    return channel >= 0 && channel < static_cast<int> (lines.size())
               ? lines[static_cast<size_t> (channel)]->silenceInserted.load (std::memory_order_relaxed) : 0;
}

uint64_t StemAligner::getSamplesDropped (int channel) const noexcept
{
    return channel >= 0 && channel < static_cast<int> (lines.size())
               ? lines[static_cast<size_t> (channel)]->dropped.load (std::memory_order_relaxed) : 0;
}

} // namespace mma
