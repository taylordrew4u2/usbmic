#pragma once

#include <cstdint>
#include <cstring>

namespace mma {

/// Conversion between the engine's float samples and the fixed-point layouts a
/// device may insist on.
///
/// Exclusive-mode audio performs no format conversion: whatever the hardware
/// accepts is what the app must read and write byte for byte. Most USB
/// microphones are 16- or 24-bit PCM devices and refuse float outright, so this
/// is the common path, not an exotic fallback.
///
/// It lives in Core, free of any platform header, so the arithmetic can be
/// tested headlessly -- the backends that use it only compile on their own OS.
/// §11 applies: these run on the audio thread, so no allocation and no locking.
namespace SampleFormat {

/// Reads one interleaved sample at `index` from a raw device buffer.
/// `bytesPerSample` is the container width (2, 3 or 4); `isFloat` selects
/// IEEE-754 float32 over fixed point.
inline float read (const void* base, size_t index, int bytesPerSample, bool isFloat) noexcept
{
    const auto* p = static_cast<const unsigned char*> (base) + index * static_cast<size_t> (bytesPerSample);

    if (isFloat)
    {
        float value;
        std::memcpy (&value, p, sizeof (value));
        return value;
    }

    if (bytesPerSample == 2)
    {
        int16_t value;
        std::memcpy (&value, p, sizeof (value));
        return static_cast<float> (value) * (1.0f / 32768.0f);
    }

    if (bytesPerSample == 3)
    {
        // Packed 24-bit little-endian, sign-extended through the top byte.
        const int32_t value = (static_cast<int32_t> (static_cast<int8_t> (p[2])) << 16)
                            | (static_cast<int32_t> (p[1]) << 8)
                            | static_cast<int32_t> (p[0]);
        return static_cast<float> (value) * (1.0f / 8388608.0f);
    }

    int32_t value;
    std::memcpy (&value, p, sizeof (value));
    return static_cast<float> (value) * (1.0f / 2147483648.0f);
}

/// Writes one interleaved sample at `index` into a raw device buffer.
inline void write (void* base, size_t index, int bytesPerSample, bool isFloat, float value) noexcept
{
    auto* p = static_cast<unsigned char*> (base) + index * static_cast<size_t> (bytesPerSample);

    if (isFloat)
    {
        std::memcpy (p, &value, sizeof (value));
        return;
    }

    // Clamp before scaling. These bytes go straight to the DAC, so an integer
    // that wraps is an audible click rather than a soft clip.
    const float clamped = value > 1.0f ? 1.0f : (value < -1.0f ? -1.0f : value);

    if (bytesPerSample == 2)
    {
        const int16_t out = static_cast<int16_t> (clamped * 32767.0f);
        std::memcpy (p, &out, sizeof (out));
        return;
    }

    if (bytesPerSample == 3)
    {
        const int32_t out = static_cast<int32_t> (clamped * 8388607.0f);
        p[0] = static_cast<unsigned char> (out & 0xff);
        p[1] = static_cast<unsigned char> ((out >> 8) & 0xff);
        p[2] = static_cast<unsigned char> ((out >> 16) & 0xff);
        return;
    }

    const int32_t out = static_cast<int32_t> (clamped * 2147483520.0f);
    std::memcpy (p, &out, sizeof (out));
}

} // namespace SampleFormat
} // namespace mma
