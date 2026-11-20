#include "qacq/dsp/window.h"

#include "qacq/dsp/constants.h"

#include <algorithm>
#include <cmath>

namespace qacq::dsp
{
std::string_view windowName(Window window) noexcept
{
    switch (window)
    {
        case Window::Rectangular: return "Rectangulaire";
        case Window::Hann: return "Hann";
        case Window::Hamming: return "Hamming";
        case Window::Blackman: return "Blackman";
        case Window::BlackmanHarris: return "Blackman-Harris";
        case Window::FlatTop: return "Flat top";
        case Window::Kaiser: return "Kaiser";
    }
    return "?";
}

double besselI0(double x) noexcept
{
    // sum_k ((x/2)^k / k!)^2 ; every term is positive, so the series is stable.
    const double q = 0.25 * x * x;
    double term = 1.0;
    double sum = 1.0;
    for (int k = 1; k < 500; ++k)
    {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

namespace
{
// Generalized cosine window: w = sum_i (-1)^i a_i cos(2 pi i n / M).
double cosineSum(std::span<const double> a, double phase) noexcept
{
    double w = 0.0;
    double sign = 1.0;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        w += sign * a[i] * std::cos(static_cast<double>(i) * phase);
        sign = -sign;
    }
    return w;
}
} // namespace

std::vector<double> makeWindow(Window window, std::size_t n, WindowSymmetry symmetry, double kaiserBeta)
{
    std::vector<double> w(n, 1.0);
    if (n <= 1 || window == Window::Rectangular) return w;

    // Periodic: the symmetric window of length n + 1 without its last point.
    const double m = symmetry == WindowSymmetry::Periodic ? static_cast<double>(n) : static_cast<double>(n - 1);

    static constexpr double hann[] = {0.5, 0.5};
    static constexpr double hamming[] = {0.54, 0.46};
    static constexpr double blackman[] = {0.42, 0.5, 0.08};
    static constexpr double blackmanHarris[] = {0.35875, 0.48829, 0.14128, 0.01168};
    static constexpr double flatTop[] = {0.21557895, 0.41663158, 0.277263158, 0.083578947, 0.006947368};

    std::span<const double> coefficients;
    switch (window)
    {
        case Window::Hann: coefficients = hann; break;
        case Window::Hamming: coefficients = hamming; break;
        case Window::Blackman: coefficients = blackman; break;
        case Window::BlackmanHarris: coefficients = blackmanHarris; break;
        case Window::FlatTop: coefficients = flatTop; break;
        case Window::Kaiser:
        {
            const double i0Beta = besselI0(kaiserBeta);
            for (std::size_t i = 0; i < n; ++i)
            {
                const double r = 2.0 * static_cast<double>(i) / m - 1.0;
                const double arg = kaiserBeta * std::sqrt(std::max(0.0, 1.0 - r * r));
                w[i] = besselI0(arg) / i0Beta;
            }
            return w;
        }
        case Window::Rectangular: return w;
    }
    for (std::size_t i = 0; i < n; ++i) w[i] = cosineSum(coefficients, kTwoPi * static_cast<double>(i) / m);
    return w;
}

double coherentGain(std::span<const double> window) noexcept
{
    if (window.empty()) return 0.0;
    double sum = 0.0;
    for (double v : window) sum += v;
    return sum / static_cast<double>(window.size());
}

double equivalentNoiseBandwidthBins(std::span<const double> window) noexcept
{
    double sum = 0.0;
    double squares = 0.0;
    for (double v : window)
    {
        sum += v;
        squares += v * v;
    }
    return sum != 0.0 ? static_cast<double>(window.size()) * squares / (sum * sum) : 0.0;
}
} // namespace qacq::dsp
