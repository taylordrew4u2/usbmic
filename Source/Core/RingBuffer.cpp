#include "RingBuffer.h"
#include <algorithm>
#include <cstring>

namespace mma {

RingBuffer::RingBuffer (size_t capacitySamples)
    : buffer (std::max<size_t> (capacitySamples, 1))
{
}

void RingBuffer::reset (size_t capacitySamples)
{
    buffer.assign (std::max<size_t> (capacitySamples, 1), 0.0f);
    writeIndex.store (0, std::memory_order_relaxed);
    readIndex.store (0, std::memory_order_relaxed);
}

size_t RingBuffer::availableForRead() const noexcept
{
    const size_t w = writeIndex.load (std::memory_order_acquire);
    const size_t r = readIndex.load (std::memory_order_relaxed);
    return (w >= r) ? (w - r) : (buffer.size() - r + w);
}

size_t RingBuffer::availableForWrite() const noexcept
{
    // Reserve one slot to disambiguate full vs empty.
    return buffer.size() - 1 - availableForRead();
}

size_t RingBuffer::write (const float* data, size_t numSamples) noexcept
{
    const size_t capacity = buffer.size();
    const size_t toWrite = std::min (numSamples, availableForWrite());
    size_t w = writeIndex.load (std::memory_order_relaxed);

    for (size_t i = 0; i < toWrite; ++i)
    {
        buffer[w] = data[i];
        w = (w + 1) % capacity;
    }

    writeIndex.store (w, std::memory_order_release);
    return toWrite;
}

size_t RingBuffer::read (float* data, size_t numSamples) noexcept
{
    const size_t capacity = buffer.size();
    const size_t toRead = std::min (numSamples, availableForRead());
    size_t r = readIndex.load (std::memory_order_relaxed);

    for (size_t i = 0; i < toRead; ++i)
    {
        data[i] = buffer[r];
        r = (r + 1) % capacity;
    }

    readIndex.store (r, std::memory_order_release);
    return toRead;
}

double RingBuffer::fillFraction() const noexcept
{
    if (buffer.size() <= 1)
        return 0.0;
    return static_cast<double> (availableForRead()) / static_cast<double> (buffer.size() - 1);
}

size_t RingBuffer::minimumCapacitySamples (double sampleRate, int numChannels) noexcept
{
    const size_t thirtySeconds = static_cast<size_t> (sampleRate * 30.0 * std::max (1, numChannels));
    const size_t sixtyFourMbInFloats = (64ull * 1024ull * 1024ull) / sizeof (float);
    return std::max (thirtySeconds, sixtyFourMbInFloats);
}

} // namespace mma
