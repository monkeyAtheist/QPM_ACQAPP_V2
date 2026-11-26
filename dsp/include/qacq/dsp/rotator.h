#pragma once

// exp(j (2 pi ratio i + phase)) for i = 0, 1, 2...: the carrier of a modulator, the oscillator of a mixer.
// Every kBlock samples the value is computed from the fraction of ratio * i (nothing builds up over a long
// record), and in between from the one before, by one complex multiplication: a cosine and a sine every 64
// samples rather than at every one. As close to the true value as the angle written at every sample, whose
// fraction is only known to the rounding of ratio * i (2.2e-16 of that many turns); for a ratio written exactly
// in binary, within 1e-14. Where the mathematical library is slow (an x86 program emulated on an ARM processor:
// about a microsecond a cosine), the carriers were most of the time of a link.

#include "qacq/dsp/constants.h"

#include <cmath>
#include <complex>
#include <cstddef>

namespace qacq::dsp
{
// v turned by a step of unit modulus (v * step, written out: no check for infinities).
inline std::complex<double> turned(std::complex<double> v, std::complex<double> step) noexcept
{
    return {v.real() * step.real() - v.imag() * step.imag(), v.real() * step.imag() + v.imag() * step.real()};
}

class Rotator
{
public:
    static constexpr std::size_t kBlock = 64;

    // ratio: cycles per sample (f / fs, any sign); amplitude: its modulus.
    explicit Rotator(double ratio, double phaseRadians = 0.0, double amplitude = 1.0) noexcept
        : m_ratio(ratio), m_phase(phaseRadians), m_amplitude(amplitude)
    {
        const double angle = kTwoPi * (ratio - std::floor(ratio));
        m_step = {std::cos(angle), std::sin(angle)};
    }

    // The value at sample i, from its angle.
    std::complex<double> at(std::size_t i) const noexcept
    {
        const double turns = m_ratio * static_cast<double>(i);
        const double angle = kTwoPi * (turns - std::floor(turns)) + m_phase;
        return {m_amplitude * std::cos(angle), m_amplitude * std::sin(angle)};
    }

    // The value at the sample it is at (0 at first), then one sample on.
    std::complex<double> next() noexcept
    {
        if (m_index % kBlock == 0) m_value = at(m_index);
        const std::complex<double> value = m_value;
        m_value = turned(m_value, m_step);
        ++m_index;
        return value;
    }

private:
    double m_ratio, m_phase, m_amplitude;
    std::complex<double> m_step, m_value;
    std::size_t m_index = 0;
};
} // namespace qacq::dsp
