#pragma once

// Values of a sampled signal between its samples: a Kaiser-windowed sinc, tabulated.
//
// With the defaults (8 samples on each side, beta 9, 2048 steps per sample interpolated linearly) a signal
// band-limited to |f| < 0.3 fs comes back between its samples to about 2e-5 of its amplitude (1e-2 at 0.4 fs:
// the window's transition), and each set of weights sums to 1 (a constant stays constant). White noise keeps
// 93 % of its power (what lies above 0.3 fs is cut). Outside the record the samples count as 0.

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace qacq::dsp
{
class SincInterpolator
{
public:
    explicit SincInterpolator(int halfWidth = 8, int steps = 2048, double beta = 9.0);

    // x at a position counted in samples (0: the first one, 2.5: half-way between the third and the fourth).
    double at(std::span<const double> x, double position) const noexcept;
    // A complex signal held as its two parts.
    std::complex<double> at(std::span<const double> re, std::span<const double> im, double position) const noexcept;
    std::complex<double> at(std::span<const std::complex<double>> x, double position) const noexcept;

    int halfWidth() const noexcept { return m_halfWidth; }

private:
    // The weights of samples floor(position) - halfWidth + 1 ... floor(position) + halfWidth.
    const double *weights(double fraction, double *scratch) const noexcept;

    int m_halfWidth;
    int m_steps;
    std::vector<double> m_table; // (steps + 1) rows of 2 halfWidth weights
};

// A shared interpolator with the defaults (built once).
const SincInterpolator &sincInterpolator();
} // namespace qacq::dsp
