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

namespace
{
constexpr std::size_t kMaximumPad = std::size_t{1} << 20;
constexpr std::size_t kPredictionWindow = 4096;

// The default pad of SciPy for odd, even and constant: 3 × the length of the filter, within the record.
std::size_t scipyPad(std::size_t taps, std::size_t n, long padLength)
{
    const std::size_t pad = padLength < 0 ? 3 * taps : static_cast<std::size_t>(padLength);
    return std::min(pad, n - 1);
}
} // namespace

namespace
{
// x[t] ≈ Σ c[j]·x[t − 1 − j], j < p, by least squares: exact for a sum of sinusoids, which Burg's method is
// not (it biases their frequencies: off by a tenth within a hundred samples on three of them). Over the forward
// and the backward predictions (the modified covariance method) for a steady signal: less sensitive to noise,
// its sinusoids stay on the unit circle; over the forward ones alone (the covariance method) for a signal that
// dies out or grows, which the backward predictions would see the other way round (a ring that fades would go
// on at its last level). A ridge of 1e-12 of the mean diagonal for a record that p terms over-describe.
// False: too short, or nothing to solve.
bool leastSquaresPredictor(const std::vector<double> &x, std::size_t p, bool bothWays, std::vector<double> &c)
{
    const std::size_t n = x.size();
    if (p == 0 || n < p + 2) return false;
    std::vector<double> m(p * p, 0.0), v(p, 0.0);
    const auto row = [&](std::size_t t, bool forward) {
        const double target = x[t];
        for (std::size_t i = 0; i < p; ++i)
        {
            const double xi = forward ? x[t - 1 - i] : x[t + 1 + i];
            v[i] += xi * target;
            for (std::size_t j = 0; j <= i; ++j) m[i * p + j] += xi * (forward ? x[t - 1 - j] : x[t + 1 + j]);
        }
    };
    for (std::size_t t = p; t < n; ++t) row(t, true);
    if (bothWays)
        for (std::size_t t = 0; t + p < n; ++t) row(t, false);
    double trace = 0.0;
    for (std::size_t i = 0; i < p; ++i) trace += m[i * p + i];
    if (!(trace > 0.0)) return false;
    const double ridge = 1e-12 * trace / static_cast<double>(p);
    // Cholesky of the (symmetric, lower half filled) matrix plus the ridge, then the two triangular solves.
    for (std::size_t i = 0; i < p; ++i) m[i * p + i] += ridge;
    for (std::size_t j = 0; j < p; ++j)
    {
        double d = m[j * p + j];
        for (std::size_t k = 0; k < j; ++k) d -= m[j * p + k] * m[j * p + k];
        if (!(d > 0.0)) return false;
        d = std::sqrt(d);
        m[j * p + j] = d;
        for (std::size_t i = j + 1; i < p; ++i)
        {
            double s = m[i * p + j];
            for (std::size_t k = 0; k < j; ++k) s -= m[i * p + k] * m[j * p + k];
            m[i * p + j] = s / d;
        }
    }
    c.assign(p, 0.0);
    for (std::size_t i = 0; i < p; ++i)
    {
        double s = v[i];
        for (std::size_t k = 0; k < i; ++k) s -= m[i * p + k] * c[k];
        c[i] = s / m[i * p + i];
    }
    for (std::size_t i = p; i-- > 0;)
    {
        double s = c[i];
        for (std::size_t k = i + 1; k < p; ++k) s -= m[k * p + i] * c[k];
        c[i] = s / m[i * p + i];
    }
    for (double value : c)
        if (!std::isfinite(value)) return false;
    return true;
}

// Whether the model neither grows nor would: the roots of z^p − c[0] z^(p−1) − … − c[p−1] within the unit
// circle (to 1e-6: a sinusoid's are on it).
bool stableModel(const std::vector<double> &c)
{
    std::vector<double> ascending(c.size() + 1, 1.0);
    for (std::size_t j = 0; j < c.size(); ++j) ascending[c.size() - 1 - j] = -c[j];
    for (const Complex &root : polynomialRoots(ascending))
        if (!(std::abs(root) <= 1.0 + 1e-6)) return false;
    return true;
}

// Burg's method: the prediction polynomial a (a[0] = 1, x[t] ≈ −Σ a[j] x[t − j]), stable by construction
// (each reflection coefficient |k| < 1); it stops where the residual is no more than rounding.
std::vector<double> burg(const std::vector<double> &r, std::size_t stages)
{
    const std::size_t n = r.size();
    std::vector<double> a{1.0};
    double energy = 0.0;
    for (double v : r) energy += v * v;
    if (!(energy > 0.0)) return a;
    std::vector<double> f(r), b(r), previous;
    const double floor = 1e-14 * energy / static_cast<double>(n);
    for (std::size_t m = 1; m <= stages && m < n; ++m)
    {
        double num = 0.0, den = 0.0;
        for (std::size_t i = m; i < n; ++i)
        {
            num += f[i] * b[i - 1];
            den += f[i] * f[i] + b[i - 1] * b[i - 1];
        }
        if (!(den > 2.0 * floor * static_cast<double>(n - m))) break;
        const double k = -2.0 * num / den;
        previous = a;
        a.push_back(0.0);
        for (std::size_t i = 1; i < m; ++i) a[i] = previous[i] + k * previous[m - i];
        a[m] = k;
        for (std::size_t i = n - 1; i >= m; --i)
        {
            const double fi = f[i], bi = b[i - 1];
            f[i] = fi + k * bi;
            b[i] = bi + k * fi;
            if (i == m) break;
        }
    }
    return a;
}
} // namespace

std::vector<double> predictLinear(std::span<const double> history, std::size_t count, int order)
{
    const std::size_t n = history.size();
    std::vector<double> out(count, 0.0);
    if (count == 0 || n == 0) return out;
    // The samples as they are (a constant part is one more term, a root at z = 1): a record at rest near its
    // end goes on at rest, whatever its middle holds.
    const std::vector<double> r(history.begin(), history.end());
    double swing = 0.0;
    for (double v : r) swing = std::max(swing, std::abs(v));
    if (!(swing > 0.0)) return out;
    const double bound = 4.0 * swing;
    // The model run on from the last samples; false if it leaves the bound (it would grow).
    // A prediction that fades out goes on at zero: no denormals (a hundred times slower) over a long prolongation.
    const double faded = 1e-30 * swing;
    const auto run = [&r, n, count, bound, faded, &out](const std::vector<double> &c) {
        const std::size_t q = c.size();
        std::vector<double> line(r.end() - static_cast<std::ptrdiff_t>(std::min(q, n)), r.end());
        line.insert(line.begin(), q - std::min(q, n), 0.0);
        line.reserve(q + count);
        std::size_t silent = 0;
        for (std::size_t t = 0; t < count && silent < q; ++t)
        {
            double v = 0.0;
            const std::size_t at = line.size();
            for (std::size_t j = 0; j < q; ++j) v += c[j] * line[at - 1 - j];
            if (!std::isfinite(v) || std::abs(v) > bound) return false;
            if (std::abs(v) < faded) v = 0.0;
            silent = v == 0.0 ? silent + 1 : 0;
            line.push_back(v);
        }
        line.resize(q + count, 0.0);
        std::copy(line.begin() + static_cast<std::ptrdiff_t>(q), line.end(), out.begin());
        return true;
    };
    const std::size_t p = std::min(static_cast<std::size_t>(std::max(order, 0)), n / 3);
    // Steady if the energy of the history's second half is within a factor of 2 of the first's.
    double first = 0.0, second = 0.0;
    for (std::size_t i = 0; i < n; ++i) (2 * i < n ? first : second) += r[i] * r[i];
    const bool steady = second <= 2.0 * first && first <= 2.0 * second;
    std::vector<double> c;
    for (const bool bothWays : {steady, !steady})
        if (leastSquaresPredictor(r, p, bothWays, c) && stableModel(c) && run(c)) return out;
    // Burg's model, stable; damped should rounding make it grow all the same.
    const std::vector<double> a = burg(r, std::min(static_cast<std::size_t>(std::max(order, 0)), n / 2));
    if (a.size() > 1)
    {
        for (const double damping : {1.0, 0.999, 0.99})
        {
            c.assign(a.size() - 1, 0.0);
            double scale = 1.0;
            for (std::size_t j = 1; j < a.size(); ++j) c[j - 1] = -a[j] * (scale *= damping);
            if (run(c)) return out;
        }
    }
    std::fill(out.begin(), out.end(), r.back());
    return out;
}

std::size_t sosRingLength(const SosCascade &sections, double tolerance, std::size_t maximum)
{
    double radius = 0.0;
    for (const Biquad &s : sections)
    {
        // The poles of 1 + a1 z^-1 + a2 z^-2: a complex pair of modulus √a2, or two real roots.
        const double a1 = s.a[1], a2 = s.a[2];
        const double disc = a1 * a1 - 4.0 * a2;
        if (disc < 0.0)
            radius = std::max(radius, std::sqrt(std::abs(a2)));
        else
            radius = std::max({radius, std::abs(0.5 * (-a1 + std::sqrt(disc))), std::abs(0.5 * (-a1 - std::sqrt(disc)))});
    }
    const std::size_t floor = 2 * sections.size() + 1;
    if (!(radius > 1e-12)) return std::min(floor, maximum);
    if (!(radius < 1.0)) return maximum;
    const double n = std::ceil(std::log(tolerance) / std::log(radius));
    if (!(n < static_cast<double>(maximum))) return maximum;
    return std::max(floor, static_cast<std::size_t>(n));
}

std::vector<double> extendEdges(std::span<const double> x, std::size_t pad, EdgeExtension extension)
{
    const std::size_t n = x.size();
    if (n == 0 || pad == 0 || extension == EdgeExtension::None) return std::vector<double>(x.begin(), x.end());
    std::vector<double> e;
    e.reserve(n + 2 * pad);
    switch (extension)
    {
        case EdgeExtension::Odd:
        case EdgeExtension::Even:
        {
            const std::size_t p = std::min(pad, n - 1);
            const bool odd = extension == EdgeExtension::Odd;
            for (std::size_t i = p; i >= 1; --i) e.push_back(odd ? 2.0 * x[0] - x[i] : x[i]);
            e.insert(e.end(), x.begin(), x.end());
            for (std::size_t i = 1; i <= p; ++i) e.push_back(odd ? 2.0 * x[n - 1] - x[n - 1 - i] : x[n - 1 - i]);
            return e;
        }
        case EdgeExtension::Constant:
            e.assign(pad, x[0]);
            e.insert(e.end(), x.begin(), x.end());
            e.insert(e.end(), pad, x[n - 1]);
            return e;
        case EdgeExtension::Predicted:
        {
            const std::size_t window = std::min(n, kPredictionWindow);
            // Before the start: the record read backwards, predicted, put back in order.
            std::vector<double> reversed(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(window));
            std::reverse(reversed.begin(), reversed.end());
            std::vector<double> head = predictLinear(reversed, pad);
            std::reverse(head.begin(), head.end());
            const std::vector<double> tail = predictLinear(x.subspan(n - window), pad);
            e = std::move(head);
            e.insert(e.end(), x.begin(), x.end());
            e.insert(e.end(), tail.begin(), tail.end());
            return e;
        }
        case EdgeExtension::None: break;
    }
    return std::vector<double>(x.begin(), x.end());
}

namespace
{
// The length of the cascade as scipy.signal.sosfiltfilt counts it for its padding: 2 × sections + 1, less the
// sections of the first order.
std::size_t sosTaps(const SosCascade &sections)
{
    std::size_t trivialB = 0, trivialA = 0;
    for (const auto &s : sections)
    {
        trivialB += s.b[2] == 0.0 ? 1 : 0;
        trivialA += s.a[2] == 0.0 ? 1 : 0;
    }
    return 2 * sections.size() + 1 - std::min(trivialB, trivialA);
}
} // namespace

std::size_t sosFiltFiltPad(const SosCascade &sections, std::size_t n, EdgeExtension extension)
{
    if (n == 0 || sections.empty()) return 0;
    const std::size_t taps = sosTaps(sections);
    switch (extension)
    {
        case EdgeExtension::Odd:
        case EdgeExtension::Even:
        case EdgeExtension::Constant: return scipyPad(taps, n, -1);
        case EdgeExtension::None: return 0;
        case EdgeExtension::Predicted:
            return std::min(kMaximumPad, std::max(3 * taps, sosRingLength(sections, 1e-6, kMaximumPad)));
    }
    return 0;
}

std::size_t firFiltFiltPad(std::size_t taps, std::size_t n, EdgeExtension extension)
{
    if (n == 0 || taps == 0) return 0;
    switch (extension)
    {
        case EdgeExtension::Odd:
        case EdgeExtension::Even:
        case EdgeExtension::Constant: return scipyPad(taps, n, -1);
        case EdgeExtension::None: return 0;
        case EdgeExtension::Predicted: return std::min(kMaximumPad, taps);
    }
    return 0;
}

std::vector<double> sosFiltFilt(const SosCascade &sections, std::span<const double> x, long padLength,
                                EdgeExtension extension)
{
    if (x.empty() || sections.empty()) return std::vector<double>(x.begin(), x.end());
    const std::size_t n = x.size();
    std::size_t edge = 0;
    if (padLength < 0)
        edge = sosFiltFiltPad(sections, n, extension);
    else
        switch (extension)
        {
            case EdgeExtension::Odd:
            case EdgeExtension::Even: edge = scipyPad(sosTaps(sections), n, padLength); break;
            case EdgeExtension::Constant:
            case EdgeExtension::Predicted: edge = static_cast<std::size_t>(padLength); break;
            case EdgeExtension::None: edge = 0; break;
        }
    const std::vector<double> extended = extendEdges(x, edge, extension);
    edge = (extended.size() - n) / 2;

    // Started in the steady state of the first sample (scipy.signal.sosfilt_zi), forward then backward.
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

std::vector<double> firFiltFilt(std::span<const double> taps, std::span<const double> x, long padLength,
                                EdgeExtension extension)
{
    const std::size_t n = x.size();
    if (n == 0 || taps.empty()) return std::vector<double>(x.begin(), x.end());
    std::size_t edge = 0;
    if (padLength < 0)
        edge = firFiltFiltPad(taps.size(), n, extension);
    else
        switch (extension)
        {
            case EdgeExtension::Odd:
            case EdgeExtension::Even: edge = scipyPad(taps.size(), n, padLength); break;
            case EdgeExtension::Constant:
            case EdgeExtension::Predicted: edge = static_cast<std::size_t>(padLength); break;
            case EdgeExtension::None: edge = 0; break;
        }
    const std::vector<double> extended = extendEdges(x, edge, extension);
    edge = (extended.size() - n) / 2;
    // Each pass started in the steady state of its first sample (scipy.signal.lfilter_zi): N − 1 copies of it
    // before, their outputs dropped.
    const std::size_t lead = taps.size() - 1;
    const auto pass = [&taps, lead](const std::vector<double> &v) {
        std::vector<double> led(lead, v.front());
        led.insert(led.end(), v.begin(), v.end());
        std::vector<double> y = firFilter(taps, led);
        y.erase(y.begin(), y.begin() + static_cast<std::ptrdiff_t>(lead));
        return y;
    };
    std::vector<double> y = pass(extended);
    std::reverse(y.begin(), y.end());
    y = pass(y);
    std::reverse(y.begin(), y.end());
    return std::vector<double>(y.begin() + static_cast<std::ptrdiff_t>(edge), y.begin() + static_cast<std::ptrdiff_t>(edge + n));
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
