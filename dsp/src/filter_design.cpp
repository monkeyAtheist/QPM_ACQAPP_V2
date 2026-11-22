#include "qacq/dsp/filter_design.h"

#include "qacq/dsp/constants.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>

namespace qacq::dsp
{
std::string_view familyName(FilterFamily family) noexcept
{
    switch (family)
    {
        case FilterFamily::Butterworth: return "Butterworth";
        case FilterFamily::ChebyshevI: return "Chebyshev I";
        case FilterFamily::ChebyshevII: return "Chebyshev II";
        case FilterFamily::Bessel: return "Bessel";
    }
    return "?";
}

std::string_view responseName(FilterResponse response) noexcept
{
    switch (response)
    {
        case FilterResponse::LowPass: return "Passe-bas";
        case FilterResponse::HighPass: return "Passe-haut";
        case FilterResponse::BandPass: return "Passe-bande";
        case FilterResponse::BandStop: return "Coupe-bande";
    }
    return "?";
}

namespace
{
Complex product(const std::vector<Complex> &values, Complex shift, double sign) noexcept
{
    // prod(shift + sign * v)
    Complex result(1.0, 0.0);
    for (const Complex &v : values) result *= shift + sign * v;
    return result;
}

} // namespace

// ------------------------------------------------------------------ polynomial roots
// Aberth-Ehrlich iteration on a real polynomial given in ascending powers; returns the roots.
std::vector<Complex> polynomialRoots(std::span<const double> ascending)
{
    if (ascending.size() < 2) return {};
    const std::size_t degree = ascending.size() - 1;
    std::vector<Complex> c(ascending.begin(), ascending.end());
    const Complex leading = c.back();
    for (auto &v : c) v /= leading; // monic

    auto evaluate = [&](Complex x, Complex &derivative) {
        Complex value = c[degree];
        derivative = Complex(0.0, 0.0);
        for (std::size_t i = degree; i-- > 0;)
        {
            derivative = derivative * x + value;
            value = value * x + c[i];
        }
        return value;
    };

    // Initial guesses on a circle of the Cauchy-bound radius, rotated off the real axis.
    double radius = 0.0;
    for (std::size_t i = 0; i < degree; ++i) radius = std::max(radius, std::pow(std::abs(c[i]), 1.0 / static_cast<double>(degree - i)));
    radius = std::max(radius, 1e-3);
    std::vector<Complex> roots(degree);
    for (std::size_t i = 0; i < degree; ++i)
        roots[i] = std::polar(radius, kTwoPi * (static_cast<double>(i) + 0.25) / static_cast<double>(degree) + 0.4);

    for (int iteration = 0; iteration < 500; ++iteration)
    {
        double largestStep = 0.0;
        for (std::size_t i = 0; i < degree; ++i)
        {
            Complex derivative;
            const Complex value = evaluate(roots[i], derivative);
            if (std::abs(value) == 0.0) continue;
            const Complex ratio = value / derivative;
            Complex repulsion(0.0, 0.0);
            for (std::size_t j = 0; j < degree; ++j)
                if (j != i) repulsion += 1.0 / (roots[i] - roots[j]);
            const Complex step = ratio / (1.0 - ratio * repulsion);
            roots[i] -= step;
            largestStep = std::max(largestStep, std::abs(step) / std::max(1.0, std::abs(roots[i])));
        }
        if (largestStep < 1e-15) break;
    }
    return roots;
}

namespace
{

// ------------------------------------------------------------------ analog prototypes (cut-off 1 rad/s)
Zpk butterworthPrototype(int n)
{
    Zpk zpk;
    for (int m = -n + 1; m < n; m += 2) zpk.poles.push_back(-std::exp(Complex(0.0, kPi * m / (2.0 * n))));
    zpk.gain = 1.0;
    return zpk;
}

Zpk chebyshev1Prototype(int n, double rippleDb)
{
    Zpk zpk;
    const double eps = std::sqrt(std::pow(10.0, 0.1 * rippleDb) - 1.0);
    const double mu = std::asinh(1.0 / eps) / n;
    for (int m = -n + 1; m < n; m += 2)
    {
        const double theta = kPi * m / (2.0 * n);
        zpk.poles.push_back(-std::sinh(Complex(mu, theta)));
    }
    zpk.gain = product(zpk.poles, 0.0, -1.0).real();
    if (n % 2 == 0) zpk.gain /= std::sqrt(1.0 + eps * eps);
    return zpk;
}

Zpk chebyshev2Prototype(int n, double attenuationDb)
{
    Zpk zpk;
    const double de = 1.0 / std::sqrt(std::pow(10.0, 0.1 * attenuationDb) - 1.0);
    const double mu = std::asinh(1.0 / de) / n;
    for (int m = -n + 1; m < n; m += 2)
    {
        if (n % 2 == 1 && m == 0) continue; // the odd-order prototype has one zero fewer (at infinity)
        zpk.zeros.push_back(-std::conj(Complex(0.0, 1.0) / std::sin(m * kPi / (2.0 * n))));
    }
    for (int m = -n + 1; m < n; m += 2)
    {
        const Complex base = -std::exp(Complex(0.0, kPi * m / (2.0 * n)));
        const Complex warped(std::sinh(mu) * base.real(), std::cosh(mu) * base.imag());
        zpk.poles.push_back(1.0 / warped);
    }
    zpk.gain = (product(zpk.poles, 0.0, -1.0) / product(zpk.zeros, 0.0, -1.0)).real();
    return zpk;
}

Zpk besselPrototype(int n)
{
    // Reverse Bessel polynomial theta_n(s) = sum_k (2n - k)! / (2^(n - k) k! (n - k)!) s^k.
    std::vector<double> ascending(static_cast<std::size_t>(n) + 1);
    for (int k = 0; k <= n; ++k)
    {
        double value = 1.0;
        for (int i = n - k + 1; i <= 2 * n - k; ++i) value *= i;         // (2n - k)! / (n - k)!
        for (int i = 2; i <= k; ++i) value /= i;                          // / k!
        value /= std::pow(2.0, n - k);
        ascending[static_cast<std::size_t>(k)] = value;
    }
    Zpk zpk;
    zpk.poles = polynomialRoots(ascending);
    for (auto &p : zpk.poles)
        if (std::abs(p.imag()) < 1e-12 * std::abs(p)) p = Complex(p.real(), 0.0);
    // Normalise to |H(j1)| = 1/sqrt(2) (scipy norm="mag").
    auto magnitude = [&](double w) {
        double num = 1.0, den = 1.0;
        for (const auto &p : zpk.poles)
        {
            num *= std::abs(p);
            den *= std::abs(Complex(0.0, w) - p);
        }
        return num / den;
    };
    double lo = 0.0, hi = 1.0;
    while (magnitude(hi) > std::sqrt(0.5)) hi *= 2.0;
    for (int i = 0; i < 200; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        (magnitude(mid) > std::sqrt(0.5) ? lo : hi) = mid;
    }
    const double w3 = 0.5 * (lo + hi);
    for (auto &p : zpk.poles) p /= w3;
    zpk.gain = product(zpk.poles, 0.0, -1.0).real();
    return zpk;
}

// ------------------------------------------------------------------ frequency transformations (SciPy)
int relativeDegree(const Zpk &zpk) noexcept
{
    return static_cast<int>(zpk.poles.size()) - static_cast<int>(zpk.zeros.size());
}

Zpk lowPassToLowPass(Zpk zpk, double wo)
{
    const int degree = relativeDegree(zpk);
    for (auto &z : zpk.zeros) z *= wo;
    for (auto &p : zpk.poles) p *= wo;
    zpk.gain *= std::pow(wo, degree);
    return zpk;
}

Zpk lowPassToHighPass(const Zpk &in, double wo)
{
    Zpk out;
    const int degree = relativeDegree(in);
    for (const auto &z : in.zeros) out.zeros.push_back(wo / z);
    for (const auto &p : in.poles) out.poles.push_back(wo / p);
    for (int i = 0; i < degree; ++i) out.zeros.emplace_back(0.0, 0.0);
    out.gain = in.gain * (product(in.zeros, 0.0, -1.0) / product(in.poles, 0.0, -1.0)).real();
    return out;
}

Zpk lowPassToBandPass(const Zpk &in, double wo, double bw)
{
    Zpk out;
    const int degree = relativeDegree(in);
    auto split = [&](Complex v, std::vector<Complex> &target) {
        const Complex scaled = v * (bw / 2.0);
        const Complex root = std::sqrt(scaled * scaled - wo * wo);
        target.push_back(scaled + root);
        target.push_back(scaled - root);
    };
    for (const auto &z : in.zeros) split(z, out.zeros);
    for (const auto &p : in.poles) split(p, out.poles);
    for (int i = 0; i < degree; ++i) out.zeros.emplace_back(0.0, 0.0);
    out.gain = in.gain * std::pow(bw, degree);
    return out;
}

Zpk lowPassToBandStop(const Zpk &in, double wo, double bw)
{
    Zpk out;
    const int degree = relativeDegree(in);
    auto split = [&](Complex v, std::vector<Complex> &target) {
        const Complex inverted = (bw / 2.0) / v;
        const Complex root = std::sqrt(inverted * inverted - wo * wo);
        target.push_back(inverted + root);
        target.push_back(inverted - root);
    };
    for (const auto &z : in.zeros) split(z, out.zeros);
    for (const auto &p : in.poles) split(p, out.poles);
    for (int i = 0; i < degree; ++i)
    {
        out.zeros.emplace_back(0.0, wo);
        out.zeros.emplace_back(0.0, -wo);
    }
    out.gain = in.gain * (product(in.zeros, 0.0, -1.0) / product(in.poles, 0.0, -1.0)).real();
    return out;
}

Zpk bilinear(const Zpk &analog, double fs)
{
    Zpk digital;
    const double fs2 = 2.0 * fs;
    const int degree = relativeDegree(analog);
    for (const auto &z : analog.zeros) digital.zeros.push_back((fs2 + z) / (fs2 - z));
    for (const auto &p : analog.poles) digital.poles.push_back((fs2 + p) / (fs2 - p));
    for (int i = 0; i < degree; ++i) digital.zeros.emplace_back(-1.0, 0.0);
    digital.gain = analog.gain * (product(analog.zeros, fs2, -1.0) / product(analog.poles, fs2, -1.0)).real();
    return digital;
}

// ------------------------------------------------------------------ zpk -> sections
struct RootSet
{
    std::vector<Complex> complexRoots; // one of each conjugate pair (imaginary part > 0)
    std::vector<double> realRoots;
};

RootSet splitRoots(const std::vector<Complex> &roots)
{
    RootSet set;
    std::vector<Complex> upper, lower;
    for (const auto &r : roots)
    {
        const double tolerance = 1e-9 * std::max(1.0, std::abs(r));
        if (std::abs(r.imag()) <= tolerance)
            set.realRoots.push_back(r.real());
        else if (r.imag() > 0.0)
            upper.push_back(r);
        else
            lower.push_back(r);
    }
    // Each upper root keeps its conjugate's average (removes round-off asymmetry).
    for (const auto &u : upper)
    {
        auto best = lower.end();
        double distance = std::numeric_limits<double>::infinity();
        for (auto it = lower.begin(); it != lower.end(); ++it)
        {
            const double d = std::abs(*it - std::conj(u));
            if (d < distance)
            {
                distance = d;
                best = it;
            }
        }
        if (best != lower.end())
        {
            set.complexRoots.push_back(0.5 * (u + std::conj(*best)));
            lower.erase(best);
        }
        else
        {
            set.complexRoots.push_back(u);
        }
    }
    for (const auto &l : lower) set.complexRoots.push_back(std::conj(l)); // unmatched: keep as a pair
    return set;
}

template <typename T>
std::size_t nearestIndex(const std::vector<T> &values, Complex target)
{
    std::size_t best = 0;
    double distance = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        const double d = std::abs(Complex(values[i]) - target);
        if (d < distance)
        {
            distance = d;
            best = i;
        }
    }
    return best;
}

double distanceToCircle(Complex v) noexcept
{
    return std::abs(1.0 - std::abs(v));
}

bool validFrequency(double f, double fs) noexcept
{
    return std::isfinite(f) && f > 0.0 && f < 0.5 * fs;
}
} // namespace

SosCascade zpkToSos(const Zpk &zpkIn)
{
    std::vector<Complex> zeros = zpkIn.zeros;
    std::vector<Complex> poles = zpkIn.poles;
    while (zeros.size() < poles.size()) zeros.emplace_back(0.0, 0.0);
    while (poles.size() < zeros.size()) poles.emplace_back(0.0, 0.0);
    if (poles.size() % 2 == 1)
    {
        poles.emplace_back(0.0, 0.0);
        zeros.emplace_back(0.0, 0.0);
    }
    if (poles.empty()) return {Biquad{{zpkIn.gain, 0.0, 0.0}, {1.0, 0.0, 0.0}}};

    RootSet p = splitRoots(poles);
    RootSet z = splitRoots(zeros);
    std::deque<Biquad> sections;
    const std::size_t count = poles.size() / 2;
    for (std::size_t s = 0; s < count; ++s)
    {
        // The remaining pole nearest the unit circle defines the next section (built from the last one).
        bool poleIsComplex = false;
        std::size_t poleIndex = 0;
        double best = std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < p.complexRoots.size(); ++i)
            if (distanceToCircle(p.complexRoots[i]) < best)
            {
                best = distanceToCircle(p.complexRoots[i]);
                poleIndex = i;
                poleIsComplex = true;
            }
        for (std::size_t i = 0; i < p.realRoots.size(); ++i)
            if (distanceToCircle(Complex(p.realRoots[i])) < best)
            {
                best = distanceToCircle(Complex(p.realRoots[i]));
                poleIndex = i;
                poleIsComplex = false;
            }

        Complex p1, p2;
        if (poleIsComplex)
        {
            p1 = p.complexRoots[poleIndex];
            p2 = std::conj(p1);
            p.complexRoots.erase(p.complexRoots.begin() + static_cast<std::ptrdiff_t>(poleIndex));
        }
        else
        {
            p1 = Complex(p.realRoots[poleIndex]);
            p.realRoots.erase(p.realRoots.begin() + static_cast<std::ptrdiff_t>(poleIndex));
            const std::size_t other = nearestIndex(p.realRoots, p1); // an even count of real poles is left
            p2 = Complex(p.realRoots[other]);
            p.realRoots.erase(p.realRoots.begin() + static_cast<std::ptrdiff_t>(other));
        }

        // Nearest zeros: a conjugate pair, or two real zeros.
        Complex z1, z2;
        const bool haveComplex = !z.complexRoots.empty();
        const bool haveTwoReal = z.realRoots.size() >= 2;
        double complexDistance = std::numeric_limits<double>::infinity();
        double realDistance = std::numeric_limits<double>::infinity();
        std::size_t complexIndex = 0, realIndex = 0;
        if (haveComplex)
        {
            complexIndex = nearestIndex(z.complexRoots, p1);
            complexDistance = std::abs(z.complexRoots[complexIndex] - p1);
        }
        if (haveTwoReal)
        {
            realIndex = nearestIndex(z.realRoots, p1);
            realDistance = std::abs(Complex(z.realRoots[realIndex]) - p1);
        }
        if (haveComplex && (complexDistance <= realDistance || !haveTwoReal))
        {
            z1 = z.complexRoots[complexIndex];
            z2 = std::conj(z1);
            z.complexRoots.erase(z.complexRoots.begin() + static_cast<std::ptrdiff_t>(complexIndex));
        }
        else
        {
            z1 = Complex(z.realRoots[realIndex]);
            z.realRoots.erase(z.realRoots.begin() + static_cast<std::ptrdiff_t>(realIndex));
            const std::size_t other = nearestIndex(z.realRoots, p1);
            z2 = Complex(z.realRoots[other]);
            z.realRoots.erase(z.realRoots.begin() + static_cast<std::ptrdiff_t>(other));
        }

        Biquad section;
        section.b = {1.0, -(z1 + z2).real(), (z1 * z2).real()};
        section.a = {1.0, -(p1 + p2).real(), (p1 * p2).real()};
        sections.push_front(section);
    }
    SosCascade cascade(sections.begin(), sections.end());
    for (double &b : cascade.front().b) b *= zpkIn.gain;
    return cascade;
}

IirDesign designIir(const IirSpec &spec)
{
    IirDesign design;
    const double fs = spec.sampleRateHz;
    if (!(std::isfinite(fs) && fs > 0.0))
    {
        design.error = "La fréquence d'échantillonnage doit être positive.";
        return design;
    }
    if (!validFrequency(spec.frequency1Hz, fs))
    {
        design.error = "La fréquence de coupure doit être comprise entre 0 et fs/2.";
        return design;
    }
    const bool band = spec.response == FilterResponse::BandPass || spec.response == FilterResponse::BandStop;
    if (band && !(validFrequency(spec.frequency2Hz, fs) && spec.frequency2Hz > spec.frequency1Hz))
    {
        design.error = "Les bords de bande doivent vérifier 0 < f1 < f2 < fs/2.";
        return design;
    }
    const int maximumOrder = spec.family == FilterFamily::Bessel ? kMaximumBesselOrder : kMaximumIirOrder;
    if (spec.order < 1 || spec.order > maximumOrder)
    {
        design.error = "L'ordre doit être compris entre 1 et " + std::to_string(maximumOrder) + ".";
        return design;
    }

    Zpk prototype;
    switch (spec.family)
    {
        case FilterFamily::Butterworth: prototype = butterworthPrototype(spec.order); break;
        case FilterFamily::ChebyshevI:
            if (!(spec.passbandRippleDb > 0.0))
            {
                design.error = "Chebyshev I : l'ondulation en bande passante doit être positive.";
                return design;
            }
            prototype = chebyshev1Prototype(spec.order, spec.passbandRippleDb);
            break;
        case FilterFamily::ChebyshevII:
            if (!(spec.stopbandAttenuationDb > 0.0))
            {
                design.error = "Chebyshev II : l'atténuation en bande coupée doit être positive.";
                return design;
            }
            prototype = chebyshev2Prototype(spec.order, spec.stopbandAttenuationDb);
            break;
        case FilterFamily::Bessel: prototype = besselPrototype(spec.order); break;
    }

    auto warp = [fs](double f) { return 2.0 * fs * std::tan(kPi * f / fs); };
    const double w1 = warp(spec.frequency1Hz);
    Zpk analog;
    switch (spec.response)
    {
        case FilterResponse::LowPass: analog = lowPassToLowPass(prototype, w1); break;
        case FilterResponse::HighPass: analog = lowPassToHighPass(prototype, w1); break;
        case FilterResponse::BandPass:
        case FilterResponse::BandStop:
        {
            const double w2 = warp(spec.frequency2Hz);
            const double wo = std::sqrt(w1 * w2);
            analog = spec.response == FilterResponse::BandPass ? lowPassToBandPass(prototype, wo, w2 - w1)
                                                               : lowPassToBandStop(prototype, wo, w2 - w1);
            break;
        }
    }
    design.digital = bilinear(analog, fs);
    for (const auto &pole : design.digital.poles)
    {
        if (!(std::abs(pole) < 1.0))
        {
            design.error = "Le filtre obtenu n'est pas stable (pôle sur ou hors du cercle unité).";
            return design;
        }
    }
    design.sections = zpkToSos(design.digital);
    return design;
}

namespace
{
// sum_n c_n exp(-j w n) and sum_n n c_n exp(-j w n)
void polynomialAt(std::span<const double> c, double w, Complex &value, Complex &weighted) noexcept
{
    value = Complex(0.0, 0.0);
    weighted = Complex(0.0, 0.0);
    for (std::size_t n = 0; n < c.size(); ++n)
    {
        const Complex e = std::polar(1.0, -w * static_cast<double>(n));
        value += c[n] * e;
        weighted += static_cast<double>(n) * c[n] * e;
    }
}

double polynomialDelay(std::span<const double> c, double w) noexcept
{
    Complex value, weighted;
    polynomialAt(c, w, value, weighted);
    if (std::abs(value) < 1e-300) return 0.0;
    return (weighted / value).real();
}
} // namespace

Complex transferResponse(std::span<const double> b, std::span<const double> a, double f, double fs) noexcept
{
    const double w = kTwoPi * f / fs;
    Complex nb, na, unused;
    polynomialAt(b, w, nb, unused);
    polynomialAt(a, w, na, unused);
    return nb / na;
}

Complex sosResponse(const SosCascade &sections, double f, double fs) noexcept
{
    Complex h(1.0, 0.0);
    for (const auto &s : sections) h *= transferResponse(s.b, s.a, f, fs);
    return h;
}

double transferGroupDelay(std::span<const double> b, std::span<const double> a, double f, double fs) noexcept
{
    const double w = kTwoPi * f / fs;
    return polynomialDelay(b, w) - polynomialDelay(a, w);
}

double sosGroupDelay(const SosCascade &sections, double f, double fs) noexcept
{
    double delay = 0.0;
    for (const auto &s : sections) delay += transferGroupDelay(s.b, s.a, f, fs);
    return delay;
}

FirDesign designFir(const FirSpec &spec)
{
    FirDesign design;
    const double fs = spec.sampleRateHz;
    const std::size_t n = spec.taps;
    if (!(std::isfinite(fs) && fs > 0.0))
    {
        design.error = "La fréquence d'échantillonnage doit être positive.";
        return design;
    }
    if (n < 3)
    {
        design.error = "Un FIR demande au moins 3 coefficients.";
        return design;
    }
    if (!validFrequency(spec.frequency1Hz, fs))
    {
        design.error = "La fréquence de coupure doit être comprise entre 0 et fs/2.";
        return design;
    }
    const bool band = spec.response == FilterResponse::BandPass || spec.response == FilterResponse::BandStop;
    if (band && !(validFrequency(spec.frequency2Hz, fs) && spec.frequency2Hz > spec.frequency1Hz))
    {
        design.error = "Les bords de bande doivent vérifier 0 < f1 < f2 < fs/2.";
        return design;
    }
    const bool passesNyquist = spec.response == FilterResponse::HighPass || spec.response == FilterResponse::BandStop;
    if (passesNyquist && n % 2 == 0)
    {
        design.error = "Un passe-haut ou un coupe-bande FIR demande un nombre impair de coefficients.";
        return design;
    }

    // Bands in units of the Nyquist frequency, as scipy.signal.firwin.
    const double c1 = spec.frequency1Hz / (0.5 * fs);
    const double c2 = spec.frequency2Hz / (0.5 * fs);
    std::vector<std::pair<double, double>> bands;
    switch (spec.response)
    {
        case FilterResponse::LowPass: bands = {{0.0, c1}}; break;
        case FilterResponse::HighPass: bands = {{c1, 1.0}}; break;
        case FilterResponse::BandPass: bands = {{c1, c2}}; break;
        case FilterResponse::BandStop: bands = {{0.0, c1}, {c2, 1.0}}; break;
    }
    auto sinc = [](double x) { return x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x); };
    const double alpha = 0.5 * static_cast<double>(n - 1);
    const std::vector<double> window = makeWindow(spec.window, n, WindowSymmetry::Symmetric, spec.kaiserBeta);
    design.taps.assign(n, 0.0);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double m = static_cast<double>(i) - alpha;
        double h = 0.0;
        for (const auto &[left, right] : bands) h += right * sinc(right * m) - left * sinc(left * m);
        design.taps[i] = h * window[i];
    }
    // Unit gain at DC, at Nyquist, or at the centre of the (first) pass band.
    const double scaleFrequency = bands.front().first == 0.0 ? 0.0
                                  : bands.front().second == 1.0 ? 1.0
                                                                : 0.5 * (bands.front().first + bands.front().second);
    double gain = 0.0;
    for (std::size_t i = 0; i < n; ++i)
        gain += design.taps[i] * std::cos(kPi * (static_cast<double>(i) - alpha) * scaleFrequency);
    for (double &h : design.taps) h /= gain;
    return design;
}

KaiserEstimate estimateKaiser(double attenuationDb, double transitionWidthHz, double sampleRateHz) noexcept
{
    KaiserEstimate estimate;
    const double a = std::abs(attenuationDb);
    if (a > 50.0)
        estimate.beta = 0.1102 * (a - 8.7);
    else if (a > 21.0)
        estimate.beta = 0.5842 * std::pow(a - 21.0, 0.4) + 0.07886 * (a - 21.0);
    const double width = 2.0 * transitionWidthHz / sampleRateHz; // in units of Nyquist, as scipy.kaiserord
    if (width > 0.0)
        estimate.taps = static_cast<std::size_t>(std::ceil((a - 7.95) / 2.285 / (kPi * width) + 1.0));
    return estimate;
}
} // namespace qacq::dsp
