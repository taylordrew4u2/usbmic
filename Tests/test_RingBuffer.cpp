#include "TestFramework.h"
#include "Core/RingBuffer.h"
#include <vector>

using namespace mma;

TEST_CASE (RingBuffer_WriteThenReadRoundTrips)
{
    RingBuffer rb (16);
    std::vector<float> in = { 1, 2, 3, 4, 5 };
    size_t written = rb.write (in.data(), in.size());
    REQUIRE (written == 5);

    std::vector<float> out (5, 0.0f);
    size_t read = rb.read (out.data(), out.size());
    REQUIRE (read == 5);
    for (size_t i = 0; i < 5; ++i)
        REQUIRE_NEAR (out[i], in[i], 1e-9);
}

TEST_CASE (RingBuffer_WriteNeverExceedsAvailableCapacity)
{
    RingBuffer rb (4); // 3 usable slots (1 reserved to disambiguate full/empty)
    std::vector<float> in = { 1, 2, 3, 4, 5 };
    size_t written = rb.write (in.data(), in.size());
    REQUIRE (written <= 3);
}

TEST_CASE (RingBuffer_ReadNeverExceedsAvailableData)
{
    RingBuffer rb (16);
    std::vector<float> in = { 1, 2 };
    rb.write (in.data(), in.size());

    std::vector<float> out (10, -1.0f);
    size_t read = rb.read (out.data(), out.size());
    REQUIRE (read == 2);
}

TEST_CASE (RingBuffer_WrapsAroundCorrectly)
{
    RingBuffer rb (8); // 7 usable
    std::vector<float> a = { 1, 2, 3, 4, 5 };
    rb.write (a.data(), a.size());
    std::vector<float> drained (5, 0.0f);
    rb.read (drained.data(), 5);

    std::vector<float> b = { 6, 7, 8, 9, 10 };
    size_t written = rb.write (b.data(), b.size());
    REQUIRE (written == 5);

    std::vector<float> out (5, 0.0f);
    size_t read = rb.read (out.data(), 5);
    REQUIRE (read == 5);
    for (int i = 0; i < 5; ++i)
        REQUIRE_NEAR (out[i], b[static_cast<size_t> (i)], 1e-9);
}

TEST_CASE (RingBuffer_FillFractionReflectsOccupancy)
{
    RingBuffer rb (100);
    REQUIRE_NEAR (rb.fillFraction(), 0.0, 1e-6);

    std::vector<float> data (50, 1.0f);
    rb.write (data.data(), data.size());
    REQUIRE (rb.fillFraction() > 0.4);
    REQUIRE (rb.fillFraction() < 0.6);
}

TEST_CASE (RingBuffer_MinimumCapacityIsAtLeast30SecondsOr64MB)
{
    // Low channel count/rate: 30s dominates but is tiny, so the 64MB floor wins.
    size_t cap = RingBuffer::minimumCapacitySamples (48000.0, 1);
    size_t sixtyFourMbFloats = (64ull * 1024ull * 1024ull) / sizeof (float);
    REQUIRE (cap >= sixtyFourMbFloats);
}

TEST_CASE (RingBuffer_MinimumCapacityScalesWithChannelsAndRate)
{
    size_t cap8ch = RingBuffer::minimumCapacitySamples (48000.0, 8);
    size_t cap1ch = RingBuffer::minimumCapacitySamples (48000.0, 1);
    REQUIRE (cap8ch >= cap1ch);
}

TEST_CASE (RingBuffer_BulkWriteAndReadStraddleTheWrapPoint)
{
    // write() and read() copy in two memcpy chunks rather than looping a modulo
    // per sample. The split point is the only place that can go wrong, so drive
    // a transfer that lands squarely across it and check every sample.
    RingBuffer ring (16);

    // Push the indices most of the way round so the next transfer must wrap.
    std::vector<float> filler (12, 1.0f);
    REQUIRE (ring.write (filler.data(), 12) == 12);

    std::vector<float> drained (12, 0.0f);
    REQUIRE (ring.read (drained.data(), 12) == 12);

    std::vector<float> payload (10, 0.0f);
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<float> (i) + 0.5f;

    REQUIRE (ring.write (payload.data(), payload.size()) == payload.size());

    std::vector<float> out (10, -1.0f);
    REQUIRE (ring.read (out.data(), out.size()) == out.size());

    for (size_t i = 0; i < out.size(); ++i)
        REQUIRE_NEAR (out[i], static_cast<double> (i) + 0.5, 1e-9);

    REQUIRE (ring.availableForRead() == 0);
}

TEST_CASE (RingBuffer_RepeatedWrappedTransfersStayInOrder)
{
    // Many small transfers around the ring: the wrap arithmetic must not drift
    // or duplicate a sample over hundreds of laps.
    RingBuffer ring (7);
    float next = 0.0f;
    float expected = 0.0f;

    for (int lap = 0; lap < 500; ++lap)
    {
        std::vector<float> in { next, next + 1.0f, next + 2.0f };
        next += 3.0f;

        REQUIRE (ring.write (in.data(), in.size()) == in.size());

        std::vector<float> out (3, -1.0f);
        REQUIRE (ring.read (out.data(), out.size()) == out.size());

        for (float sample : out)
        {
            REQUIRE_NEAR (sample, expected, 1e-9);
            expected += 1.0f;
        }
    }
}
