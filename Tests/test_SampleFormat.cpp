#include "TestFramework.h"
#include "Core/SampleFormat.h"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace mma;

namespace {

constexpr int kFloat = 4;

void writeThenRead (int bytes, bool isFloat, float value, float tolerance)
{
    unsigned char buffer[16] = {};
    SampleFormat::write (buffer, 1, bytes, isFloat, value);
    const float roundTrip = SampleFormat::read (buffer, 1, bytes, isFloat);
    REQUIRE (std::fabs (roundTrip - value) <= tolerance);
}

} // namespace

// Every fixed-point layout has to survive a round trip within its own
// quantisation step. A sign or scaling error here is inaudible in a unit test
// that only checks positive values, so both polarities and the rails are
// covered explicitly.
TEST_CASE (SampleFormat_RoundTripsEveryLayoutBothPolarities)
{
    const float values[] = { 0.0f, 0.5f, -0.5f, 0.999f, -0.999f, 1.0f, -1.0f, 0.001f, -0.001f };

    for (float v : values)
    {
        writeThenRead (kFloat, true, v, 0.0f);          // float is exact
        writeThenRead (4, false, v, 1.0e-6f);
        writeThenRead (3, false, v, 1.0e-6f);
        // Two LSBs, not one: writing scales by 32767 to keep full scale off
        // the wrap point while reading scales by 32768, so the round trip can
        // lose a step to that asymmetry as well as one to truncation.
        writeThenRead (2, false, v, 2.0f / 32768.0f);
    }
}

// The 24-bit path is hand-packed byte by byte, which is where sign extension
// goes wrong: a negative sample read as a large positive one is full-scale
// noise, not a quiet artefact.
TEST_CASE (SampleFormat_SignExtends24BitNegatives)
{
    unsigned char minusHalf[3] = { 0x00, 0x00, 0xC0 }; // -0x400000 little-endian
    REQUIRE_NEAR (SampleFormat::read (minusHalf, 0, 3, false), -0.5f, 1.0e-6f);

    unsigned char plusHalf[3] = { 0x00, 0x00, 0x40 };  // +0x400000
    REQUIRE_NEAR (SampleFormat::read (plusHalf, 0, 3, false), 0.5f, 1.0e-6f);

    unsigned char minusOne[3] = { 0xFF, 0xFF, 0xFF };  // -1 LSB
    REQUIRE (SampleFormat::read (minusOne, 0, 3, false) < 0.0f);
}

// Known bit patterns, not just round trips: a consistently wrong scale factor
// would round-trip perfectly and still be wrong against the hardware.
TEST_CASE (SampleFormat_MatchesKnownBitPatterns)
{
    int16_t half16 = 16384;
    REQUIRE_NEAR (SampleFormat::read (&half16, 0, 2, false), 0.5f, 1.0e-6f);

    int32_t half32 = 1073741824; // 2^30
    REQUIRE_NEAR (SampleFormat::read (&half32, 0, 4, false), 0.5f, 1.0e-6f);

    int16_t minimum = -32768;
    REQUIRE_NEAR (SampleFormat::read (&minimum, 0, 2, false), -1.0f, 1.0e-6f);
}

// §5: a runaway sum must not wrap. Clamping is what keeps an over-range block
// a clipped block instead of full-scale square-wave noise.
TEST_CASE (SampleFormat_ClampsOutOfRangeInsteadOfWrapping)
{
    for (int bytes : { 2, 3, 4 })
    {
        unsigned char buffer[8] = {};

        SampleFormat::write (buffer, 0, bytes, false, 4.0f);
        REQUIRE (SampleFormat::read (buffer, 0, bytes, false) > 0.99f);

        SampleFormat::write (buffer, 0, bytes, false, -4.0f);
        REQUIRE (SampleFormat::read (buffer, 0, bytes, false) < -0.99f);
    }
}

// The backends index a single interleaved device buffer by frame*channels+ch.
// This is the arithmetic that decides whether microphone 2 lands in stem 2 or
// in silence.
TEST_CASE (SampleFormat_InterleavedIndexingKeepsChannelsSeparate)
{
    constexpr int channels = 3;
    constexpr int frames = 8;
    std::vector<unsigned char> wire (static_cast<size_t> (channels) * frames * 2);

    for (int f = 0; f < frames; ++f)
        for (int ch = 0; ch < channels; ++ch)
            SampleFormat::write (wire.data(), static_cast<size_t> (f) * channels + ch, 2, false,
                                 0.1f * static_cast<float> (ch + 1));

    for (int f = 0; f < frames; ++f)
        for (int ch = 0; ch < channels; ++ch)
            REQUIRE_NEAR (SampleFormat::read (wire.data(), static_cast<size_t> (f) * channels + ch, 2, false),
                          0.1f * static_cast<float> (ch + 1), 1.0e-3f);
}
