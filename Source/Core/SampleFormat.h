#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

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

/// §2.3: the depth one stem should be WRITTEN at, given what its device can do.
///
///   "Follows device capability per channel. Do not upconvert -- it adds file
///    size and no information. Where a device supports multiple depths, choose
///    the highest, capped at 24-bit."
///
/// The cap is the spec's, not a limitation: 24 bits is past the point where
/// more carries anything a listener or an editor can use, and a 32-bit
/// container costs a third more card for it. A device that offers only depths
/// above the cap therefore lands on the cap.
///
/// An empty list means the backend cannot say -- not that the device is
/// limited -- so it yields the fallback rather than a guess. Only depths this
/// app can actually write are considered; a device advertising something
/// exotic falls back rather than producing a file nothing can open.
///
/// Note what this does NOT change: a device reporting the usual {16, 24, 32}
/// lands on 24, exactly what the app wrote before §2.3 was implemented. The
/// depth only moves for hardware that genuinely cannot do 24 -- which is the
/// whole point, and is the Blue Yeti of §14.1.
inline int chooseRecordingBitDepth (const std::vector<int>& supportedBitDepths,
                                    int fallbackDepth = 24) noexcept
{
    constexpr int kHighestUsefulDepth = 24;

    int best = 0;

    for (const int depth : supportedBitDepths)
    {
        // 16 and 24 are what SessionWriter can lay down. Anything else is
        // either below what this app offers or above the cap.
        if (depth != 16 && depth != kHighestUsefulDepth)
            continue;

        if (depth > best)
            best = depth;
    }

    if (best > 0)
        return best;

    // Nothing usable was named. Either the backend said nothing at all, or it
    // named only depths outside what this app writes -- a 32-bit-only device,
    // for instance, which the cap sends here too.
    return supportedBitDepths.empty() ? fallbackDepth : kHighestUsefulDepth;
}

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
        // Assembled unsigned and sign-extended by arithmetic, not by shifting
        // a negative value left -- which is undefined before C++20 and what
        // UBSan flagged here.
        const uint32_t raw = (static_cast<uint32_t> (p[2]) << 16)
                           | (static_cast<uint32_t> (p[1]) << 8)
                           | static_cast<uint32_t> (p[0]);
        const int32_t value = static_cast<int32_t> (raw << 8) / 256;
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
