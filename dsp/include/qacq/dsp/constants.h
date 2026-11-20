#pragma once

// The only definition of pi in the project (the old application had 18).

#include <numbers>

namespace qacq::dsp
{
inline constexpr double kPi = std::numbers::pi;
inline constexpr double kTwoPi = 2.0 * std::numbers::pi;
} // namespace qacq::dsp
