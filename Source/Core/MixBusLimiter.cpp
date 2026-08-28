#include "MixBusLimiter.h"
#include <algorithm>
#include <cmath>

namespace mma {

namespace {

// Computed once at static-initialisation time rather than on every sample. The
// writer thread runs this over every frame of every take, and a std::pow whose
// operands never change had no business being inside that loop.
const float kCeilingLinear = std::pow (10.0f, MixBusLimiter::kCeilingDb / 20.0f);

} // namespace

float MixBusLimiter::processSample (float sumInputSample) noexcept
{
    if (std::abs (sumInputSample) > kCeilingLinear)
        return std::copysign (kCeilingLinear, sumInputSample);
    return sumInputSample;
}

} // namespace mma
