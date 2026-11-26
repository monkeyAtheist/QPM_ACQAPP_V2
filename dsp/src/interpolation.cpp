#include "qacq/dsp/interpolation.h"

#include "qacq/dsp/constants.h"

#include <algorithm>
#include <cmath>

namespace qacq::dsp
{
namespace
{
constexpr int kMaximumHalfWidth = 32;

double besselI0(double x) noexcept
{
    double sum = 1.0, term = 1.0;
    const double q = 0.25 * x * x;
    for (int k = 1; k < 500; ++k)
    {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < 1e-17 * sum) break;
    }
    return sum;
}
} // namespace

SincInterpolator::SincInterpolator(int halfWidth, int steps, double beta)
    : m_halfWidth(std::clamp(halfWidth, 1, kMaximumHalfWidth)), m_steps(std::max(1, steps))
{
    const int taps = 2 * m_halfWidth;
    m_table.assign(static_cast<std::size_t>(m_steps + 1) * static_cast<std::size_t>(taps), 0.0);
    const double i0Beta = besselI0(beta);
    for (int s = 0; s <= m_steps; ++s)
    {
        const double mu = static_cast<double>(s) / static_cast<double>(m_steps);
        double *row = &m_table[static_cast<std::size_t>(s) * static_cast<std::size_t>(taps)];
        double sum = 0.0;
        for (int j = 0; j < taps; ++j)
        {
            // Sample floor(position) - halfWidth + 1 + j lies x samples before the position.
            const double x = mu + static_cast<double>(m_halfWidth - 1 - j);
            const double r = x / static_cast<double>(m_halfWidth);
            double w = 0.0;
            if (std::abs(r) < 1.0)
            {
                const double window = besselI0(beta * std::sqrt(1.0 - r * r)) / i0Beta;
                const double sinc = std::abs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
                w = sinc * window;
            }
            row[j] = w;
            sum += w;
        }
        for (int j = 0; j < taps; ++j) row[j] /= sum; // a constant stays constant
    }
}

const double *SincInterpolator::weights(double fraction, double *scratch) const noexcept
{
    const double s = std::clamp(fraction, 0.0, 1.0) * static_cast<double>(m_steps);
    const int i = std::min(m_steps - 1, static_cast<int>(s));
    const double t = s - static_cast<double>(i);
    const int taps = 2 * m_halfWidth;
    const double *a = &m_table[static_cast<std::size_t>(i) * static_cast<std::size_t>(taps)];
    const double *b = a + taps;
    for (int j = 0; j < taps; ++j) scratch[j] = a[j] + t * (b[j] - a[j]);
    return scratch;
}

double SincInterpolator::at(std::span<const double> x, double position) const noexcept
{
    const auto n = static_cast<double>(x.size());
    if (x.empty() || !(position > -static_cast<double>(m_halfWidth) && position < n + static_cast<double>(m_halfWidth))) return 0.0;
    const double base = std::floor(position);
    double scratch[2 * kMaximumHalfWidth];
    const double *w = weights(position - base, scratch);
    const long long first = static_cast<long long>(base) - m_halfWidth + 1;
    const auto size = static_cast<long long>(x.size());
    double sum = 0.0;
    for (int j = 0; j < 2 * m_halfWidth; ++j)
    {
        const long long k = first + j;
        if (k >= 0 && k < size) sum += w[j] * x[static_cast<std::size_t>(k)];
    }
    return sum;
}

std::complex<double> SincInterpolator::at(std::span<const double> re, std::span<const double> im, double position) const noexcept
{
    const std::size_t count = std::min(re.size(), im.size());
    const auto n = static_cast<double>(count);
    if (count == 0 || !(position > -static_cast<double>(m_halfWidth) && position < n + static_cast<double>(m_halfWidth))) return {};
    const double base = std::floor(position);
    double scratch[2 * kMaximumHalfWidth];
    const double *w = weights(position - base, scratch);
    const long long first = static_cast<long long>(base) - m_halfWidth + 1;
    const auto size = static_cast<long long>(count);
    double sr = 0.0, si = 0.0;
    for (int j = 0; j < 2 * m_halfWidth; ++j)
    {
        const long long k = first + j;
        if (k < 0 || k >= size) continue;
        sr += w[j] * re[static_cast<std::size_t>(k)];
        si += w[j] * im[static_cast<std::size_t>(k)];
    }
    return {sr, si};
}

std::complex<double> SincInterpolator::at(std::span<const std::complex<double>> x, double position) const noexcept
{
    const auto n = static_cast<double>(x.size());
    if (x.empty() || !(position > -static_cast<double>(m_halfWidth) && position < n + static_cast<double>(m_halfWidth))) return {};
    const double base = std::floor(position);
    double scratch[2 * kMaximumHalfWidth];
    const double *w = weights(position - base, scratch);
    const long long first = static_cast<long long>(base) - m_halfWidth + 1;
    const auto size = static_cast<long long>(x.size());
    std::complex<double> sum;
    for (int j = 0; j < 2 * m_halfWidth; ++j)
    {
        const long long k = first + j;
        if (k >= 0 && k < size) sum += w[j] * x[static_cast<std::size_t>(k)];
    }
    return sum;
}

const SincInterpolator &sincInterpolator()
{
    static const SincInterpolator shared;
    return shared;
}
} // namespace qacq::dsp
