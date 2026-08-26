#include "MixBusLimiter.h"
#include <algorithm>
#include <cmath>

namespace mma {

float MixBusLimiter::processSample (float sumInputSample) noexcept
{
    const float ceilingLinear = std::pow (10.0f, kCeilingDb / 20.0f);
    if (std::abs (sumInputSample) > ceilingLinear)
        return std::copysign (ceilingLinear, sumInputSample);
    return sumInputSample;
}

} // namespace mma
