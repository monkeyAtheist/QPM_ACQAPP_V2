#include "qacq/dsp/filtering.h"

#include "qacq/dsp/fft.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace qacq::dsp
{
std::vector<double> sosFilter(const SosCascade &sections, std::span<const double> x, SosState *state)
{
    std::vector<double> y(x.begin(), x.end());
    if (state && state->size() != sections.size()) state->assign(sections.size(), {0.0, 0.0});
    for (std::size_t s = 0; s < sections.size(); ++s)
    {
        const auto &b = sections[s].b;
        const auto &a = sections[s].a;
        double s0 = state ? (*state)[s][0] : 0.0;
        double s1 = state ? (*state)[s][1] : 0.0;
        for (double &v : y)
        {
            const double in = v;
            const double out = b[0] * in + s0;
            s0 = b[1] * in - a[1] * out + s1;
            s1 = b[2] * in - a[2] * out;
            v = out;
        }
        if (state) (*state)[s] = {s0, s1};
    }
    return y;
}

SosState sosStepState(const SosCascade &sections)
{
    SosState state(sections.size(), {0.0, 0.0});
    double level = 1.0; // input of the current section for a unit step
    for (std::size_t s = 0; s < sections.size(); ++s)
    {
        const auto &b = sections[s].b;
        const auto &a = sections[s].a;
        const double den = 1.0 + a[1] + a[2];
        if (std::abs(den) < 1e-300) break; // pole at z = 1: no steady state
        const double gain = (b[0] + b[1] + b[2]) / den;
        const double s1 = (b[2] - a[2] * gain) * level;
        const double s0 = (b[1] - a[1] * gain) * level + s1;
        state[s] = {s0, s1};
        level *= gain;
    }
    return state;
}

std::vector<double> sosFiltFilt(const SosCascade &sections, std::span<const double> x, long padLength)
{
    if (x.empty() || sections.empty()) return std::vector<double>(x.begin(), x.end());
    std::size_t edge = 0;
    if (padLength < 0)
    {
        std::size_t trivialB = 0, trivialA = 0;
        for (const auto &s : sections)
        {
            trivialB += s.b[2] == 0.0 ? 1 : 0;
            trivialA += s.a[2] == 0.0 ? 1 : 0;
        }
        const std::size_t taps = 2 * sections.size() + 1 - std::min(trivialB, trivialA);
        edge = 3 * taps;
    }
    else
    {
        edge = static_cast<std::size_t>(padLength);
    }
    edge = std::min(edge, x.size() - 1);

    // Odd extension: 2 x[0] - x[edge..1] | x | 2 x[n-1] - x[n-2..n-1-edge].
    const std::size_t n = x.size();
    std::vector<double> extended;
    extended.reserve(n + 2 * edge);
    for (std::size_t i = edge; i >= 1; --i) extended.push_back(2.0 * x[0] - x[i]);
    extended.insert(extended.end(), x.begin(), x.end());
    for (std::size_t i = 1; i <= edge; ++i) extended.push_back(2.0 * x[n - 1] - x[n - 1 - i]);

    const SosState unit = sosStepState(sections);
    auto scaled = [&unit](double level) {
        SosState s;
        s.reserve(unit.size());
        for (const auto &u : unit) s.push_back({u[0] * level, u[1] * level});
        return s;
    };
    SosState forwardState = scaled(extended.front());
    std::vector<double> forward = sosFilter(sections, extended, &forwardState);
    std::reverse(forward.begin(), forward.end());
    SosState backwardState = scaled(forward.front());
    std::vector<double> backward = sosFilter(sections, forward, &backwardState);
    std::reverse(backward.begin(), backward.end());
    return std::vector<double>(backward.begin() + static_cast<std::ptrdiff_t>(edge),
                               backward.begin() + static_cast<std::ptrdiff_t>(edge + n));
}

std::vector<double> lfilter(std::span<const double> b, std::span<const double> a, std::span<const double> x)
{
    if (b.empty() || a.empty() || a[0] == 0.0) throw std::invalid_argument("lfilter: empty b or a, or a[0] == 0");
    const std::size_t order = std::max(b.size(), a.size()) - 1;
    std::vector<double> bn(order + 1, 0.0), an(order + 1, 0.0);
    for (std::size_t i = 0; i < b.size(); ++i) bn[i] = b[i] / a[0];
    for (std::size_t i = 0; i < a.size(); ++i) an[i] = a[i] / a[0];
    std::vector<double> state(order + 1, 0.0);
    std::vector<double> y(x.size());
    for (std::size_t n = 0; n < x.size(); ++n)
    {
        const double in = x[n];
        const double out = bn[0] * in + state[0];
        for (std::size_t i = 0; i + 1 < order + 1; ++i) state[i] = bn[i + 1] * in - an[i + 1] * out + state[i + 1];
        y[n] = out;
    }
    return y;
}

std::vector<double> convolve(std::span<const double> a, std::span<const double> b)
{
    if (a.empty() || b.empty()) return {};
    const std::span<const double> longer = a.size() >= b.size() ? a : b;
    const std::span<const double> shorter = a.size() >= b.size() ? b : a;
    const std::size_t total = longer.size() + shorter.size() - 1;
    std::vector<double> y(total, 0.0);

    if (shorter.size() <= 64)
    {
        for (std::size_t i = 0; i < longer.size(); ++i)
        {
            const double v = longer[i];
            for (std::size_t k = 0; k < shorter.size(); ++k) y[i + k] += v * shorter[k];
        }
        return y;
    }

    // Overlap-add with blocks of the longer sequence.
    const std::size_t fftSize = nextPowerOfTwo(std::max<std::size_t>(8 * shorter.size(), 4096));
    const std::size_t block = fftSize - shorter.size() + 1;
    const FftPlan plan(fftSize);
    std::vector<Complex> kernel(fftSize, Complex(0.0, 0.0));
    for (std::size_t k = 0; k < shorter.size(); ++k) kernel[k] = Complex(shorter[k], 0.0);
    plan.forward(kernel);
    std::vector<Complex> work(fftSize);
    for (std::size_t start = 0; start < longer.size(); start += block)
    {
        const std::size_t count = std::min(block, longer.size() - start);
        std::fill(work.begin(), work.end(), Complex(0.0, 0.0));
        for (std::size_t i = 0; i < count; ++i) work[i] = Complex(longer[start + i], 0.0);
        plan.forward(work);
        for (std::size_t k = 0; k < fftSize; ++k) work[k] *= kernel[k];
        plan.inverse(work);
        const std::size_t produced = std::min(fftSize, total - start);
        for (std::size_t i = 0; i < produced; ++i) y[start + i] += work[i].real();
    }
    return y;
}

std::vector<double> firFilter(std::span<const double> taps, std::span<const double> x)
{
    std::vector<double> y = convolve(taps, x);
    y.resize(x.size());
    return y;
}

std::vector<double> firFilterAligned(std::span<const double> taps, std::span<const double> x)
{
    if (taps.empty() || x.empty()) return std::vector<double>(x.size(), 0.0);
    const std::vector<double> full = convolve(taps, x);
    const std::size_t delay = (taps.size() - 1) / 2;
    return std::vector<double>(full.begin() + static_cast<std::ptrdiff_t>(delay),
                               full.begin() + static_cast<std::ptrdiff_t>(delay + x.size()));
}
} // namespace qacq::dsp
