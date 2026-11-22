#include "qacq/dsp/filter_design.h"

#include "qacq/dsp/constants.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <initializer_list>
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
        case FilterFamily::Elliptic: return "elliptique";
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

    // p(x), p'(x), and Σ|c_i||x|^i: the size of what rounding leaves in p(x).
    auto evaluate = [&](Complex x, Complex &derivative, double &scale) {
        Complex value = c[degree];
        derivative = Complex(0.0, 0.0);
        const double ax = std::abs(x);
        scale = std::abs(c[degree]);
        for (std::size_t i = degree; i-- > 0;)
        {
            derivative = derivative * x + value;
            value = value * x + c[i];
            scale = scale * ax + std::abs(c[i]);
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

    // Aberth's iteration. A root is left alone once found: its step below 1e-15 of it, or p as small there as
    // rounding allows (a root of a long FIR deep in its stop band, or a multiple one, wanders at that level for
    // ever: before, every root was moved until all had stopped, up to 500 times, 1,4 s for a FIR of 511 taps).
    std::vector<char> found(degree, 0);
    constexpr double kEpsilon = 2.220446049250313e-16;
    for (int iteration = 0; iteration < 500; ++iteration)
    {
        bool moving = false;
        for (std::size_t i = 0; i < degree; ++i)
        {
            if (found[i] != 0) continue;
            Complex derivative;
            double scale = 0.0;
            const Complex value = evaluate(roots[i], derivative, scale);
            if (std::abs(value) <= 4.0 * kEpsilon * scale)
            {
                found[i] = 1;
                continue;
            }
            const Complex ratio = value / derivative;
            Complex repulsion(0.0, 0.0);
            for (std::size_t j = 0; j < degree; ++j)
                if (j != i) repulsion += 1.0 / (roots[i] - roots[j]);
            const Complex step = ratio / (1.0 - ratio * repulsion);
            roots[i] -= step;
            if (std::abs(step) / std::max(1.0, std::abs(roots[i])) < 1e-15)
                found[i] = 1;
            else
                moving = true;
        }
        if (!moving) break;
    }
    return roots;
}

// ------------------------------------------------------------------ elliptic functions
namespace
{
double agm(double a, double b) noexcept
{
    for (int i = 0; i < 64 && std::abs(a - b) > 1e-16 * a; ++i)
    {
        const double next = 0.5 * (a + b);
        b = std::sqrt(a * b);
        a = next;
    }
    return 0.5 * (a + b);
}

// The degree equation n K'(m)/K(m) = K'(m1)/K(m1), solved for m by nomes (scipy _ellipdeg).
double ellipticDegree(int n, double m1) noexcept
{
    const double q1 = std::exp(-kPi * ellipticKComplement(m1) / ellipticK(m1));
    const double q = std::pow(q1, 1.0 / n);
    double num = 0.0, den = 0.0;
    for (int k = 0; k <= 7; ++k) num += std::pow(q, k * (k + 1));
    for (int k = 1; k <= 8; ++k) den += std::pow(q, k * k);
    den = 1.0 + 2.0 * den;
    return 16.0 * q * std::pow(num / den, 4);
}

// z such that sn(z, m) = w, w complex, by descending Landen transformations (scipy _arc_jac_sn).
Complex arcJacobiSn(Complex w, double m)
{
    const auto complement = [](Complex x) { return std::sqrt((1.0 - x) * (1.0 + x)); };
    const double k = std::sqrt(m);
    if (k >= 1.0) return std::atanh(w);
    std::vector<double> ks{k};
    for (int i = 0; i < 20 && ks.back() != 0.0; ++i)
    {
        const double kp = std::sqrt((1.0 - ks.back()) * (1.0 + ks.back()));
        ks.push_back((1.0 - kp) / (1.0 + kp));
    }
    double big = kPi / 2.0;
    for (std::size_t i = 1; i < ks.size(); ++i) big *= 1.0 + ks[i];
    Complex wn = w;
    for (std::size_t i = 0; i + 1 < ks.size(); ++i)
        wn = 2.0 * wn / ((1.0 + ks[i + 1]) * (1.0 + complement(ks[i] * wn)));
    return big * (2.0 / kPi) * std::asin(wn);
}
} // namespace

double ellipticK(double m) noexcept
{
    if (!(m < 1.0)) return std::numeric_limits<double>::infinity();
    return kPi / (2.0 * agm(1.0, std::sqrt(1.0 - m)));
}

double ellipticKComplement(double p) noexcept
{
    if (!(p > 0.0)) return std::numeric_limits<double>::infinity();
    return kPi / (2.0 * agm(1.0, std::sqrt(p)));
}

JacobiValues jacobiElliptic(double u, double m) noexcept
{
    JacobiValues v;
    if (m < 1e-9)
    {
        const double t = std::sin(u), b = std::cos(u);
        const double ai = 0.25 * m * (u - t * b);
        v.sn = t - ai * b;
        v.cn = b + ai * t;
        v.dn = 1.0 - 0.5 * m * t * t;
        return v;
    }
    if (m >= 0.9999999999)
    {
        double ai = 0.25 * (1.0 - m);
        const double b = std::cosh(u), t = std::tanh(u);
        const double phi = 1.0 / b;
        const double twon = b * std::sinh(u);
        v.sn = t + ai * (twon - u) / (b * b);
        ai *= t * phi;
        v.cn = phi - ai * (twon - u);
        v.dn = phi + ai * (twon + u);
        return v;
    }
    // Arithmetic-geometric mean scale (DLMF 22.20(ii)), then the backward recurrence.
    std::array<double, 16> a{}, c{};
    a[0] = 1.0;
    double b = std::sqrt(1.0 - m);
    c[0] = std::sqrt(m);
    double twon = 1.0;
    std::size_t i = 0;
    while (std::abs(c[i] / a[i]) > std::numeric_limits<double>::epsilon() && i + 1 < a.size())
    {
        const double ai = a[i];
        ++i;
        c[i] = 0.5 * (ai - b);
        const double t = std::sqrt(ai * b);
        a[i] = 0.5 * (ai + b);
        b = t;
        twon *= 2.0;
    }
    double phi = twon * a[i] * u;
    double previous = phi;
    do
    {
        const double t = c[i] * std::sin(phi) / a[i];
        previous = phi;
        phi = 0.5 * (std::asin(t) + phi);
    } while (--i);
    v.sn = std::sin(phi);
    v.cn = std::cos(phi);
    const double dn = v.cn / std::cos(phi - previous);
    v.dn = std::abs(dn) < 0.1 ? std::sqrt(1.0 - m * v.sn * v.sn) : dn;
    return v;
}

Zpk ellipticPrototype(int n, double rippleDb, double attenuationDb)
{
    Zpk zpk;
    const double epsSquared = std::expm1(0.1 * rippleDb * std::log(10.0));
    if (n == 1)
    {
        const double p = -std::sqrt(1.0 / epsSquared);
        zpk.poles.emplace_back(p, 0.0);
        zpk.gain = -p;
        return zpk;
    }
    const double eps = std::sqrt(epsSquared);
    const double m1 = epsSquared / std::expm1(0.1 * attenuationDb * std::log(10.0));
    const double k1 = ellipticK(m1);
    const double m = ellipticDegree(n, m1);
    const double bigK = ellipticK(m);
    // r: sc⁻¹(1/ε, 1 − m1) (scipy _arc_jac_sc1).
    const double r = arcJacobiSn(Complex(0.0, 1.0 / eps), m1).imag();
    const double v0 = bigK * r / (n * k1);
    const JacobiValues sv = jacobiElliptic(v0, 1.0 - m);
    constexpr double kEpsilon = 2e-16;
    for (int j = 1 - n % 2; j < n; j += 2)
    {
        const JacobiValues e = jacobiElliptic(j * bigK / n, m);
        if (std::abs(e.sn) > kEpsilon)
        {
            const Complex z(0.0, 1.0 / (std::sqrt(m) * e.sn));
            zpk.zeros.push_back(z);
            zpk.zeros.push_back(std::conj(z));
        }
        const Complex p = -(Complex(e.cn * e.dn * sv.sn * sv.cn, e.sn * sv.dn)) / (1.0 - std::pow(e.dn * sv.sn, 2));
        if (n % 2 == 1 && j == 0)
        {
            zpk.poles.emplace_back(p.real(), 0.0); // the real pole of an odd order
            continue;
        }
        zpk.poles.push_back(p);
        zpk.poles.push_back(std::conj(p));
    }
    zpk.gain = (product(zpk.poles, 0.0, -1.0) / product(zpk.zeros, 0.0, -1.0)).real();
    if (n % 2 == 0) zpk.gain /= std::sqrt(1.0 + epsSquared);
    return zpk;
}

double ellipticStopbandEdge(int n, double rippleDb, double attenuationDb)
{
    // The selectivity k = 1/ωs solves the degree equation: m = k².
    const double epsSquared = std::expm1(0.1 * rippleDb * std::log(10.0));
    const double m1 = epsSquared / std::expm1(0.1 * attenuationDb * std::log(10.0));
    if (n == 1) return std::sqrt(1.0 / m1); // first order: no ripple, |H|² = 1/(1 + ε² ω²)
    return 1.0 / std::sqrt(ellipticDegree(n, m1));
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
        case FilterFamily::Elliptic:
            if (!(spec.passbandRippleDb > 0.0) || !(spec.stopbandAttenuationDb > spec.passbandRippleDb))
            {
                design.error = "Elliptique : il faut une ondulation positive et une atténuation plus grande qu'elle.";
                return design;
            }
            prototype = ellipticPrototype(spec.order, spec.passbandRippleDb, spec.stopbandAttenuationDb);
            break;
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

// ------------------------------------------------------------------ the least order for a template
std::string templateError(const FilterTemplate &t)
{
    const double fs = t.sampleRateHz;
    if (!(std::isfinite(fs) && fs > 0.0)) return "La fréquence d'échantillonnage doit être positive.";
    if (!(t.passbandLossDb > 0.0) || !(t.stopbandAttenuationDb > t.passbandLossDb) || !std::isfinite(t.stopbandAttenuationDb))
        return "Gabarit : il faut une atténuation maximale Amax positive dans la bande passante, et une atténuation "
               "minimale Amin plus grande qu'elle dans la bande coupée.";
    const double nyquist = 0.5 * fs;
    const auto ascending = [nyquist](std::initializer_list<double> edges) {
        double previous = 0.0;
        for (const double f : edges)
        {
            if (!(f > previous) || !std::isfinite(f)) return false;
            previous = f;
        }
        return previous < nyquist;
    };
    const std::array<double, 2> &p = t.passHz;
    const std::array<double, 2> &s = t.stopHz;
    switch (t.response)
    {
        case FilterResponse::LowPass:
            if (!ascending({p[0], s[0]}))
                return "Gabarit d'un passe-bas : il faut 0 < fp < fa < fs/2 (la bande passante finit avant que la bande "
                       "coupée commence).";
            break;
        case FilterResponse::HighPass:
            if (!ascending({s[0], p[0]}))
                return "Gabarit d'un passe-haut : il faut 0 < fa < fp < fs/2 (la bande coupée finit avant que la bande "
                       "passante commence).";
            break;
        case FilterResponse::BandPass:
            if (!ascending({s[0], p[0], p[1], s[1]}))
                return "Gabarit d'un passe-bande : il faut 0 < fa₁ < fp₁ < fp₂ < fa₂ < fs/2 (coupée, passante, coupée).";
            break;
        case FilterResponse::BandStop:
            if (!ascending({p[0], s[0], s[1], p[1]}))
                return "Gabarit d'un coupe-bande : il faut 0 < fp₁ < fa₁ < fa₂ < fp₂ < fs/2 (passante, coupée, passante).";
            break;
    }
    return {};
}

namespace
{
double sign(double v) noexcept
{
    return v > 0.0 ? 1.0 : v < 0.0 ? -1.0 : 0.0;
}

// scipy.optimize.fminbound(f, x1, x2) with its defaults (xtol 1e-5, 500 evaluations): Brent's bounded
// minimisation, step for step as SciPy's _minimize_scalar_bounded, so that a band-stop's edges move as there.
template <typename F>
double boundedMinimum(F &&f, double x1, double x2)
{
    constexpr double xatol = 1e-5;
    constexpr int maximumEvaluations = 500;
    const double sqrtEps = std::sqrt(2.2e-16);
    const double goldenMean = 0.5 * (3.0 - std::sqrt(5.0));
    double a = x1, b = x2;
    double fulc = a + goldenMean * (b - a);
    double nfc = fulc, xf = fulc;
    double rat = 0.0, e = 0.0;
    double x = xf;
    double fx = f(x);
    int evaluations = 1;
    double ffulc = fx, fnfc = fx;
    double xm = 0.5 * (a + b);
    double tol1 = sqrtEps * std::abs(xf) + xatol / 3.0;
    double tol2 = 2.0 * tol1;
    while (std::abs(xf - xm) > (tol2 - 0.5 * (b - a)))
    {
        bool golden = true;
        // A parabola through the three best points, if it falls well inside.
        if (std::abs(e) > tol1)
        {
            golden = false;
            double r = (xf - nfc) * (fx - ffulc);
            double q = (xf - fulc) * (fx - fnfc);
            double p = (xf - fulc) * q - (xf - nfc) * r;
            q = 2.0 * (q - r);
            if (q > 0.0) p = -p;
            q = std::abs(q);
            r = e;
            e = rat;
            if (std::abs(p) < std::abs(0.5 * q * r) && p > q * (a - xf) && p < q * (b - xf))
            {
                rat = (p + 0.0) / q;
                x = xf + rat;
                if ((x - a) < tol2 || (b - x) < tol2)
                {
                    const double si = sign(xm - xf) + ((xm - xf) == 0.0 ? 1.0 : 0.0);
                    rat = tol1 * si;
                }
            }
            else
            {
                golden = true;
            }
        }
        if (golden)
        {
            e = xf >= xm ? a - xf : b - xf;
            rat = goldenMean * e;
        }
        const double si = sign(rat) + (rat == 0.0 ? 1.0 : 0.0);
        x = xf + si * std::max(std::abs(rat), tol1);
        const double fu = f(x);
        ++evaluations;
        if (fu <= fx)
        {
            if (x >= xf)
                a = xf;
            else
                b = xf;
            fulc = nfc;
            ffulc = fnfc;
            nfc = xf;
            fnfc = fx;
            xf = x;
            fx = fu;
        }
        else
        {
            if (x < xf)
                a = x;
            else
                b = x;
            if (fu <= fnfc || nfc == xf)
            {
                fulc = nfc;
                ffulc = fnfc;
                nfc = x;
                fnfc = fu;
            }
            else if (fu <= ffulc || fulc == xf || fulc == nfc)
            {
                fulc = x;
                ffulc = fu;
            }
        }
        xm = 0.5 * (a + b);
        tol1 = sqrtEps * std::abs(xf) + xatol / 3.0;
        tol2 = 2.0 * tol1;
        if (evaluations >= maximumEvaluations) break;
    }
    return xf;
}

enum class OrderFormula
{
    Butterworth,
    Chebyshev,
    Elliptic
};

// The order a template asks of the normalised low-pass whose stop band starts at nat (its pass band ending at 1), as a
// real number: the formulas of buttord, cheb1ord and cheb2ord, and of SciPy's band_stop_obj for an elliptic filter.
double orderFor(double nat, double gpass, double gstop, OrderFormula formula)
{
    switch (formula)
    {
        case OrderFormula::Butterworth:
        {
            const double gs = std::pow(10.0, 0.1 * std::abs(gstop));
            const double gp = std::pow(10.0, 0.1 * std::abs(gpass));
            return std::log10((gs - 1.0) / (gp - 1.0)) / (2.0 * std::log10(nat));
        }
        case OrderFormula::Chebyshev:
        {
            const double gs = std::pow(10.0, 0.1 * std::abs(gstop));
            const double gp = std::pow(10.0, 0.1 * std::abs(gpass));
            return std::acosh(std::sqrt((gs - 1.0) / (gp - 1.0))) / std::acosh(nat);
        }
        case OrderFormula::Elliptic:
        {
            const double gs = std::pow(10.0, 0.1 * gstop);
            const double gp = std::pow(10.0, 0.1 * gpass);
            const double arg1 = std::sqrt((gp - 1.0) / (gs - 1.0));
            const double arg0 = 1.0 / nat;
            return ellipticK(arg0 * arg0) * ellipticK(1.0 - arg1 * arg1) /
                   (ellipticK(1.0 - arg0 * arg0) * ellipticK(arg1 * arg1));
        }
    }
    return 0.0;
}

// nat of a band-stop: the least of its two stop edges, its pass band (prewarped) mapped to ±1.
double bandStopNat(const std::array<double, 2> &passb, const std::array<double, 2> &stopb) noexcept
{
    double nat = std::numeric_limits<double>::infinity();
    for (const double s : stopb) nat = std::min(nat, std::abs((s * (passb[0] - passb[1])) / (s * s - passb[0] * passb[1])));
    return nat;
}

// |H(jω)|² of an all-pole analog prototype of gain 1 at DC; where it falls to `level` (< 1), by bisection (a
// Bessel's magnitude falls steadily).
double allPoleMagnitudeSquared(const std::vector<Complex> &poles, double w) noexcept
{
    double ratio = 1.0;
    for (const Complex &p : poles) ratio *= std::norm(p) / std::norm(Complex(0.0, w) - p);
    return ratio;
}

double fallsTo(const std::vector<Complex> &poles, double level) noexcept
{
    double low = 0.0, high = 1.0;
    while (allPoleMagnitudeSquared(poles, high) > level && high < 1e15) high *= 2.0;
    for (int i = 0; i < 200 && high - low > 1e-15 * high; ++i)
    {
        const double middle = 0.5 * (low + high);
        (allPoleMagnitudeSquared(poles, middle) > level ? low : high) = middle;
    }
    return 0.5 * (low + high);
}
} // namespace

IirOrder minimumIirOrder(FilterFamily family, const FilterTemplate &t)
{
    IirOrder out;
    out.error = templateError(t);
    if (!out.error.empty()) return out;
    const double fs = t.sampleRateHz;
    const bool band = t.response == FilterResponse::BandPass || t.response == FilterResponse::BandStop;
    const std::size_t count = band ? 2 : 1;
    // As SciPy: in units of fs/2, then prewarped (tan(π w/2), the bilinear transform's frequencies).
    std::array<double, 2> passb{0.0, 0.0}, stopb{0.0, 0.0};
    for (std::size_t i = 0; i < count; ++i)
    {
        passb[i] = std::tan(kPi * (2.0 * t.passHz[i] / fs) / 2.0);
        stopb[i] = std::tan(kPi * (2.0 * t.stopHz[i] / fs) / 2.0);
    }
    const double gpass = t.passbandLossDb, gstop = t.stopbandAttenuationDb;
    const OrderFormula formula = family == FilterFamily::ChebyshevI || family == FilterFamily::ChebyshevII ? OrderFormula::Chebyshev
                                 : family == FilterFamily::Elliptic                                       ? OrderFormula::Elliptic
                                                                                                          : OrderFormula::Butterworth;
    const double gs = std::pow(10.0, 0.1 * std::abs(gstop));
    const double gp = std::pow(10.0, 0.1 * std::abs(gpass));
    // The order a normalised low-pass stopping from nat (its pass band ending at 1) needs; inverseEdge (W0): the
    // frequency where the prototype (−3 dB at 1 for Butterworth and Bessel) loses Ap, inverted. Bessel: from its
    // magnitude, the ratio of the frequencies where it loses As and Ap, order by order (computed once).
    struct Found
    {
        int order = 0; // 0: cannot be held
        double exact = 0.0;
        double inverseEdge = 1.0;
    };
    std::vector<std::array<double, 2>> bessel; // per order: (ratio, wp)
    const auto orderAt = [&](double nat) {
        Found found;
        if (!(nat > 1.0) || !std::isfinite(nat)) return found;
        if (family == FilterFamily::Bessel)
        {
            if (bessel.empty())
            {
                const double passLevel = std::pow(10.0, -0.1 * gpass), stopLevel = std::pow(10.0, -0.1 * gstop);
                for (int n = 1; n <= kMaximumBesselOrder; ++n)
                {
                    const std::vector<Complex> poles = besselPrototype(n).poles;
                    const double wp = fallsTo(poles, passLevel);
                    bessel.push_back({fallsTo(poles, stopLevel) / wp, wp});
                }
            }
            found.order = kMaximumBesselOrder + 1;
            for (std::size_t n = 0; n < bessel.size(); ++n)
            {
                if (bessel[n][0] <= nat * (1.0 + 1e-12))
                {
                    found.order = static_cast<int>(n) + 1;
                    found.inverseEdge = 1.0 / bessel[n][1];
                    break;
                }
            }
            return found;
        }
        double n = 0.0;
        if (family == FilterFamily::Elliptic)
        {
            // ellipord: K and K' of both moduli, the complements accurate when the parameter is tiny.
            const double arg1Squared = std::expm1(std::log(10.0) * (0.1 * gpass)) / std::expm1(std::log(10.0) * (0.1 * gstop));
            const double arg0 = 1.0 / nat;
            const double m0 = arg0 * arg0;
            n = ellipticK(m0) * ellipticKComplement(arg1Squared) / (ellipticKComplement(m0) * ellipticK(arg1Squared));
        }
        else
        {
            n = orderFor(nat, gpass, gstop, formula);
        }
        if (!(n > 0.0) || !std::isfinite(n)) return found;
        found.exact = n;
        // Rounded up; an order within 1e-9 of an integer is that integer (the template then held to the rounding).
        found.order = n > 1000.0 ? 1001 : std::max(1, static_cast<int>(std::ceil(n - 1e-9)));
        if (family == FilterFamily::Butterworth) found.inverseEdge = std::pow(gp - 1.0, -1.0 / (2.0 * found.order));
        return found;
    };
    // nat: where the stop band starts once the pass band is mapped to a low-pass ending at 1 (SciPy's _find_nat_freq).
    double nat = 0.0;
    switch (t.response)
    {
        case FilterResponse::LowPass: nat = stopb[0] / passb[0]; break;
        case FilterResponse::HighPass: nat = passb[0] / stopb[0]; break;
        case FilterResponse::BandStop:
        {
            // Each pass band edge moved towards the stop band where that lowers the order, the other edge where it was.
            const std::array<double, 2> original = passb;
            const auto order = [&](std::size_t index, double w) {
                std::array<double, 2> moved = original;
                moved[index] = w;
                return orderFor(bandStopNat(moved, stopb), gpass, gstop, formula);
            };
            const double w0 = boundedMinimum([&](double w) { return order(0, w); }, original[0], stopb[0] - 1e-12);
            const double w1 = boundedMinimum([&](double w) { return order(1, w); }, stopb[1] + 1e-12, original[1]);
            // SciPy's search stops within 1e-5 of tan(π f/fs): at low frequencies, well inside the band even where the
            // best is its edge (a filter designed there, then asked again its order, would get one more). So an edge left
            // where it was, or both, when that asks a lower order.
            const std::array<std::array<double, 2>, 4> choices{{{w0, w1}, {original[0], w1}, {w0, original[1]}, original}};
            passb = choices[0];
            int least = orderAt(bandStopNat(passb, stopb)).order;
            for (std::size_t i = 1; i < choices.size(); ++i)
            {
                const int o = orderAt(bandStopNat(choices[i], stopb)).order;
                if (o > 0 && (least == 0 || o < least))
                {
                    least = o;
                    passb = choices[i];
                }
            }
            nat = bandStopNat(passb, stopb);
            break;
        }
        case FilterResponse::BandPass:
            nat = std::numeric_limits<double>::infinity();
            for (const double s : stopb)
                nat = std::min(nat, std::abs((s * s - passb[0] * passb[1]) / (s * (passb[0] - passb[1]))));
            break;
    }
    if (!(nat > 1.0) || !std::isfinite(nat))
    {
        out.error = "Ce gabarit ne peut pas être tenu : la bande coupée touche la bande passante.";
        return out;
    }
    const Found found = orderAt(nat);
    if (found.order == 0)
    {
        out.error = "Ce gabarit ne peut pas être tenu.";
        return out;
    }
    out.order = found.order;
    out.exactOrder = found.exact;
    const double inverseEdge = found.inverseEdge;
    if (family == FilterFamily::Bessel && out.order > kMaximumBesselOrder) return out;

    // The frequencies of the design, prewarped (SciPy's expressions), then back to Hz.
    std::array<double, 2> wn = passb;
    const auto sortedMagnitudes = [](double a, double b) {
        a = std::abs(a);
        b = std::abs(b);
        return a <= b ? std::array<double, 2>{a, b} : std::array<double, 2>{b, a};
    };
    if (family == FilterFamily::ChebyshevII)
    {
        // Where the stop band starts (cheby2's frequency) once −Ap falls on the pass band edges.
        const double v = std::acosh(std::sqrt((gs - 1.0) / (gp - 1.0)));
        const double nf = 1.0 / std::cosh(1.0 / out.order * v);
        const double d = passb[1] - passb[0];
        switch (t.response)
        {
            case FilterResponse::LowPass: wn[0] = passb[0] / nf; break;
            case FilterResponse::HighPass: wn[0] = passb[0] * nf; break;
            case FilterResponse::BandStop:
            {
                const double nat0 = nf / 2.0 * (passb[0] - passb[1]) + std::sqrt(nf * nf * (d * d) / 4.0 + passb[1] * passb[0]);
                wn = {nat0, passb[1] * passb[0] / nat0};
                break;
            }
            case FilterResponse::BandPass:
            {
                const double nat0 = 1.0 / (2.0 * nf) * (passb[0] - passb[1]) + std::sqrt(d * d / (4.0 * nf * nf) + passb[1] * passb[0]);
                wn = {nat0, passb[0] * passb[1] / nat0};
                break;
            }
        }
    }
    else if (family == FilterFamily::Butterworth || family == FilterFamily::Bessel)
    {
        // The −3 dB frequencies, the pass band edges at −Ap (buttord).
        const double w0 = inverseEdge;
        const double d = passb[1] - passb[0];
        switch (t.response)
        {
            case FilterResponse::LowPass: wn[0] = w0 * passb[0]; break;
            case FilterResponse::HighPass: wn[0] = passb[0] / w0; break;
            case FilterResponse::BandStop:
            {
                const double discr = std::sqrt(d * d + 4.0 * w0 * w0 * passb[0] * passb[1]);
                wn = sortedMagnitudes((d + discr) / (2.0 * w0), (d - discr) / (2.0 * w0));
                break;
            }
            case FilterResponse::BandPass:
            {
                const double root = std::sqrt(w0 * w0 / 4.0 * (d * d) + passb[0] * passb[1]);
                wn = sortedMagnitudes(w0 * d / 2.0 + root, -w0 * d / 2.0 + root);
                break;
            }
        }
    }
    // (Chebyshev I and elliptic: the pass band edges themselves.)
    const auto unwarped = [fs](double w) { return std::atan(w) * 2.0 / kPi * fs / 2.0; };
    out.frequency1Hz = unwarped(wn[0]);
    if (band) out.frequency2Hz = unwarped(wn[1]);
    return out;
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
        design.error = "Un RIF demande au moins 3 coefficients.";
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
        design.error = "Un passe-haut ou un coupe-bande RIF demande un nombre impair de coefficients.";
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
    // (Below about 8 dB the formula no longer holds: at least one tap.)
    if (width > 0.0)
        estimate.taps = static_cast<std::size_t>(std::ceil(std::clamp((a - 7.95) / 2.285 / (kPi * width) + 1.0, 1.0, 1e9)));
    return estimate;
}
} // namespace qacq::dsp
