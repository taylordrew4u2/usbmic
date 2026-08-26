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
