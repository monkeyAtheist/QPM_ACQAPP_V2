#include "qacq/analysis/filters.h"

#include "qacq/analysis/analysis.h"
#include "qacq/analysis/expression.h"
#include "qacq/dsp/constants.h"
#include "qacq/dsp/fft.h"
#include "qacq/dsp/filtering.h"
#include "qacq/plot/axis.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <cmath>
#include <map>
#include <mutex>
#include <utility>

namespace qacq::analysis
{
namespace
{
using Complex = std::complex<double>;

std::string quantity(double value, std::string_view unit, int digits = 4)
{
    return plot::formatCompact(value, unit, digits);
}

std::string grouped(std::size_t value)
{
    std::string digits = std::to_string(value);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i)
    {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += "\xE2\x80\xAF"; // narrow no-break space
        out += digits[i];
    }
    return out;
}

double number(const model::Json &json, std::string_view key, double fallback, double low, double high)
{
    const model::Json &v = json[key];
    if (!v.isNumber()) return fallback;
    const double x = v.toNumber(fallback);
    if (!std::isfinite(x)) return fallback;
    return std::clamp(x, low, high);
}

// 1, 2 or 5 times a power of ten, at most `value` (value > 0).
double niceFloor(double value)
{
    if (!(value > 0.0) || !std::isfinite(value)) return 1.0;
    const double decade = std::pow(10.0, std::floor(std::log10(value)));
    for (double m : {5.0, 2.0, 1.0})
        if (m * decade <= value * (1.0 + 1e-12)) return m * decade;
    return decade;
}

bool band(dsp::FilterResponse response) noexcept
{
    return response == dsp::FilterResponse::BandPass || response == dsp::FilterResponse::BandStop;
}

std::vector<double> runningSum(const std::vector<double> &x)
{
    std::vector<double> out(x.size());
    double sum = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        sum += x[i];
        out[i] = sum;
    }
    return out;
}

// The zeros of a FIR: H(z) = z^-(N-1) Σ h[k] z^(N-1-k), the roots of the polynomial of the taps taken from
// h[0] (highest power) down. Taps at 0 at the end are as many zeros at the origin; at the start, a delay.
std::vector<Complex> firZeros(const std::vector<double> &taps)
{
    std::size_t first = 0, last = taps.size();
    while (first < last && taps[first] == 0.0) ++first;
    while (last > first && taps[last - 1] == 0.0) --last;
    std::vector<Complex> zeros(taps.size() - last, Complex(0.0, 0.0));
    if (last - first >= 2)
    {
        std::vector<double> ascending(taps.begin() + static_cast<std::ptrdiff_t>(first),
                                      taps.begin() + static_cast<std::ptrdiff_t>(last));
        std::reverse(ascending.begin(), ascending.end());
        const std::vector<Complex> roots = dsp::polynomialRoots(ascending);
        zeros.insert(zeros.end(), roots.begin(), roots.end());
    }
    return zeros;
}


// "1 kHz", "300 Hz à 3,4 kHz".
std::string edgesText(const FilterRecipe &r)
{
    if (band(r.response)) return quantity(r.frequency1Hz, "Hz") + " à " + quantity(r.frequency2Hz, "Hz");
    return quantity(r.frequency1Hz, "Hz");
}

std::string lower(std::string_view text)
{
    std::string out(text);
    if (!out.empty() && out[0] >= 'A' && out[0] <= 'Z') out[0] = static_cast<char>(out[0] - 'A' + 'a');
    return out;
}
// arg(1 − r·e^{−jω}) as a continuous function of ω: within ±π/2 for |r| < 1; for |r| > 1, from
// 1 − r e^{−jω} = −r e^{−jω} (1 − e^{jω}/r); on the unit circle (r = e^{jθ}), (θ − ω)/2 ∓ π/2: a jump of π
// at ω = θ, where the zero is.
double rootPhase(Complex r, double w)
{
    const double m = std::abs(r);
    if (m < 1.0 - 1e-9)
    {
        const Complex v = 1.0 - r * std::polar(1.0, -w);
        return std::atan2(v.imag(), v.real());
    }
    if (m > 1.0 + 1e-9)
    {
        const Complex v = 1.0 - std::polar(1.0, w) / r;
        return std::arg(-r) - w + std::atan2(v.imag(), v.real());
    }
    const double d = std::arg(r) - w;
    return 0.5 * d - (std::sin(0.5 * d) >= 0.0 ? 0.5 * dsp::kPi : -0.5 * dsp::kPi);
}

// The phase of c[0] + c[1] z⁻¹ + c[2] z⁻² on the unit circle: z^−m · c[m] · Π (1 − rᵢ z⁻¹), m the
// coefficients at 0 in front, rᵢ the roots of c[m] z^(2−m) + … + c[2].
double polynomialPhase(const std::array<double, 3> &c, double w)
{
    std::size_t m = 0;
    while (m < 3 && c[m] == 0.0) ++m;
    if (m == 3) return 0.0;
    double sum = -w * static_cast<double>(m) + (c[m] < 0.0 ? dsp::kPi : 0.0);
    if (m == 1)
    {
        sum += rootPhase(Complex(-c[2] / c[1], 0.0), w);
    }
    else if (m == 0)
    {
        const double disc = c[1] * c[1] - 4.0 * c[0] * c[2];
        if (disc >= 0.0)
        {
            // Without cancellation: q = −(c1 + sign(c1)·√disc)/2, roots q/c0 and c2/q.
            const double q = -0.5 * (c[1] + std::copysign(std::sqrt(disc), c[1]));
            const double r1 = q != 0.0 ? q / c[0] : 0.0;
            const double r2 = q != 0.0 ? c[2] / q : 0.0;
            sum += rootPhase(Complex(r1, 0.0), w) + rootPhase(Complex(r2, 0.0), w);
        }
        else
        {
            const Complex r(-c[1] / (2.0 * c[0]), std::sqrt(-disc) / (2.0 * std::abs(c[0])));
            sum += rootPhase(r, w) + rootPhase(std::conj(r), w);
        }
    }
    return sum;
}

enum class Symmetry { None, Even, Odd };

// h[k] = h[N−1−k] (even), = −h[N−1−k] (odd), to 1e-9 of the largest.
Symmetry symmetryOf(const std::vector<double> &taps)
{
    double largest = 0.0;
    for (double h : taps) largest = std::max(largest, std::abs(h));
    if (!(largest > 0.0)) return Symmetry::None;
    const double tolerance = 1e-9 * largest;
    bool even = true, odd = true;
    for (std::size_t k = 0, j = taps.size() - 1; k < j && (even || odd); ++k, --j)
    {
        even = even && std::abs(taps[k] - taps[j]) <= tolerance;
        odd = odd && std::abs(taps[k] + taps[j]) <= tolerance;
    }
    // An odd one has 0 in the middle.
    if (odd && taps.size() % 2 == 1 && std::abs(taps[taps.size() / 2]) > tolerance) odd = false;
    return even ? Symmetry::Even : odd ? Symmetry::Odd : Symmetry::None;
}
// The bands of an equiripple FIR, each transition centred on its edge, and the weights that share the
// error as the ripple and the attenuation asked do (scipy.signal.remez's weights).
struct EquirippleBands
{
    std::vector<double> edges, desired, weights;
    double deltaPass = 0.0, deltaStop = 0.0; // the errors the template allows
    std::string error;
};

EquirippleBands equirippleBands(const FilterRecipe &r)
{
    EquirippleBands b;
    const double nyquist = 0.5 * r.sampleRateHz;
    const double half = 0.5 * r.transitionHz;
    if (!(r.passbandRippleDb > 0.0) || !(r.stopbandAttenuationDb > 0.0))
    {
        b.error = "Équiondulation : l'ondulation et l'atténuation demandées doivent être positives.";
        return b;
    }
    if (!(half > 0.0))
    {
        b.error = "Équiondulation : la largeur des transitions doit être positive.";
        return b;
    }
    const double ripple = std::pow(10.0, r.passbandRippleDb / 20.0);
    const double deltaPass = (ripple - 1.0) / (ripple + 1.0);
    const double deltaStop = std::pow(10.0, -r.stopbandAttenuationDb / 20.0);
    b.deltaPass = deltaPass;
    b.deltaStop = deltaStop;
    const auto add = [&b](double low, double high, bool pass, double weight) {
        b.edges.push_back(low);
        b.edges.push_back(high);
        b.desired.push_back(pass ? 1.0 : 0.0);
        b.weights.push_back(weight);
    };
    const double stopWeight = deltaPass / deltaStop;
    const double f1 = r.frequency1Hz, f2 = r.frequency2Hz;
    switch (r.response)
    {
        case dsp::FilterResponse::LowPass:
            add(0.0, f1 - half, true, 1.0);
            add(f1 + half, nyquist, false, stopWeight);
            break;
        case dsp::FilterResponse::HighPass:
            add(0.0, f1 - half, false, stopWeight);
            add(f1 + half, nyquist, true, 1.0);
            break;
        case dsp::FilterResponse::BandPass:
            add(0.0, f1 - half, false, stopWeight);
            add(f1 + half, f2 - half, true, 1.0);
            add(f2 + half, nyquist, false, stopWeight);
            break;
        case dsp::FilterResponse::BandStop:
            add(0.0, f1 - half, true, 1.0);
            add(f1 + half, f2 - half, false, stopWeight);
            add(f2 + half, nyquist, true, 1.0);
            break;
    }
    for (std::size_t i = 0; i + 1 < b.edges.size(); i += 2)
    {
        if (!(b.edges[i + 1] > b.edges[i]) || b.edges[i] < 0.0 || b.edges[i + 1] > nyquist)
        {
            b.error = "Avec des transitions de " + quantity(r.transitionHz, "Hz") +
                      ", les bandes se chevauchent ou sortent de 0 à fs/2 : réduisez la transition ou écartez les bords.";
            return b;
        }
    }
    return b;
}

// Where an elliptic filter reaches its attenuation: the stop edge of its prototype carried through the
// transformation of the response and the bilinear warping.
std::vector<double> ellipticStopEdges(const FilterRecipe &r)
{
    const double fs = r.sampleRateHz;
    const double ws = dsp::ellipticStopbandEdge(r.order, r.passbandRippleDb, r.stopbandAttenuationDb);
    const auto warp = [fs](double f) { return 2.0 * fs * std::tan(dsp::kPi * f / fs); };
    const auto unwarp = [fs](double w) { return fs / dsp::kPi * std::atan(w / (2.0 * fs)); };
    const double w1 = warp(r.frequency1Hz);
    switch (r.response)
    {
        case dsp::FilterResponse::LowPass: return {unwarp(ws * w1)};
        case dsp::FilterResponse::HighPass: return {unwarp(w1 / ws)};
        case dsp::FilterResponse::BandPass:
        case dsp::FilterResponse::BandStop:
        {
            const double w2 = warp(r.frequency2Hz);
            const double wo2 = w1 * w2, bw = w2 - w1;
            if (r.response == dsp::FilterResponse::BandPass)
            {
                const double root = std::sqrt(ws * bw * ws * bw + 4.0 * wo2);
                return {unwarp(0.5 * (root - ws * bw)), unwarp(0.5 * (root + ws * bw))};
            }
            const double root = std::sqrt(bw * bw + 4.0 * ws * ws * wo2);
            return {unwarp((root - bw) / (2.0 * ws)), unwarp((root + bw) / (2.0 * ws))};
        }
    }
    return {};
}
} // namespace

std::complex<double> FilterDesign::response(double frequencyHz) const noexcept
{
    return gain * unitResponse(frequencyHz);
}

std::complex<double> FilterDesign::unitResponse(double frequencyHz) const noexcept
{
    const double fs = recipe.sampleRateHz;
    if (!sections.empty()) return dsp::sosResponse(sections, frequencyHz, fs);
    if (!taps.empty())
    {
        static constexpr std::array<double, 1> one{1.0};
        return dsp::transferResponse(taps, one, frequencyHz, fs);
    }
    return {0.0, 0.0};
}

std::complex<double> FilterDesign::applied(double frequencyHz) const noexcept
{
    const Complex h = response(frequencyHz);
    switch (recipe.application)
    {
        case FilterApplication::ZeroPhase: return {gain * std::norm(unitResponse(frequencyHz)), 0.0};
        case FilterApplication::Aligned:
        {
            if (taps.empty()) return h;
            const double advance =
                dsp::kTwoPi * frequencyHz / recipe.sampleRateHz * 0.5 * static_cast<double>(taps.size() - 1);
            return h * Complex(std::cos(advance), std::sin(advance));
        }
        case FilterApplication::Causal: break;
    }
    return h;
}

double FilterDesign::groupDelay(double frequencyHz) const noexcept
{
    const double fs = recipe.sampleRateHz;
    if (!sections.empty()) return dsp::sosGroupDelay(sections, frequencyHz, fs);
    if (!taps.empty())
    {
        static constexpr std::array<double, 1> one{1.0};
        return dsp::transferGroupDelay(taps, one, frequencyHz, fs);
    }
    return 0.0;
}

double FilterDesign::appliedGroupDelay(double frequencyHz) const noexcept
{
    switch (recipe.application)
    {
        case FilterApplication::ZeroPhase: return 0.0;
        case FilterApplication::Aligned: return groupDelay(frequencyHz) - alignment();
        case FilterApplication::Causal: break;
    }
    return groupDelay(frequencyHz);
}

double FilterDesign::alignment() const noexcept
{
    if (recipe.application != FilterApplication::Aligned || taps.empty()) return 0.0;
    return 0.5 * static_cast<double>(taps.size() - 1);
}

double FilterDesign::phase(double frequencyHz) const noexcept
{
    const double w = dsp::kTwoPi * frequencyHz / recipe.sampleRateHz;
    if (!sections.empty())
    {
        double sum = 0.0;
        for (const dsp::Biquad &s : sections) sum += polynomialPhase(s.b, w) - polynomialPhase(s.a, w);
        return sum;
    }
    if (taps.empty()) return 0.0;
    const Symmetry symmetry = symmetryOf(taps);
    if (symmetry != Symmetry::None)
    {
        // H = e^{−jωD}·v, v real (symmetric) or imaginary (antisymmetric): its sign is the only jump.
        const double delay = 0.5 * static_cast<double>(taps.size() - 1);
        const Complex v = response(frequencyHz) * std::polar(1.0, w * delay);
        if (symmetry == Symmetry::Even) return -w * delay + (v.real() < 0.0 ? dsp::kPi : 0.0);
        return -w * delay + (v.imag() < 0.0 ? -0.5 * dsp::kPi : 0.5 * dsp::kPi);
    }
    if (rootsKnown)
    {
        // H(z) = z^−m · h[m] · Π (1 − zᵢ z⁻¹), m the taps at 0 in front.
        std::size_t first = 0;
        while (first < taps.size() && taps[first] == 0.0) ++first;
        if (first == taps.size()) return 0.0;
        double sum = -w * static_cast<double>(first) + (taps[first] < 0.0 ? dsp::kPi : 0.0);
        for (const Complex &z : zeros) sum += rootPhase(z, w);
        return sum;
    }
    return std::arg(response(frequencyHz));
}

bool FilterDesign::phaseUnwrapped() const noexcept
{
    return !sections.empty() || (!taps.empty() && (rootsKnown || symmetryOf(taps) != Symmetry::None));
}

double FilterDesign::appliedPhase(double frequencyHz) const noexcept
{
    switch (recipe.application)
    {
        case FilterApplication::ZeroPhase: return 0.0;
        case FilterApplication::Aligned:
            return phase(frequencyHz) + dsp::kTwoPi * frequencyHz / recipe.sampleRateHz * alignment();
        case FilterApplication::Causal: break;
    }
    return phase(frequencyHz);
}

double FilterDesign::largestPoleRadius() const noexcept
{
    double r = 0.0;
    for (const Complex &p : poles) r = std::max(r, std::abs(p));
    return r;
}

bool applicationAllowed(FilterKind kind, FilterApplication application) noexcept
{
    if (kind == FilterKind::None) return application == FilterApplication::Causal; // nothing to apply otherwise
    return application != FilterApplication::Aligned || kind != FilterKind::Iir;
}

namespace
{
// Parks-McClellan over the bands of the recipe, then what it reached over them.
// Kaiser's estimate (1974) of the length an equiripple FIR needs: N − 1 ≈ (−10 log10(δp·δs) − 13)/(14,6·Δf/fs),
// Δf the transition; odd where it must be.
std::size_t equirippleLength(const EquirippleBands &b, const FilterRecipe &r)
{
    const double width = r.transitionHz / r.sampleRateHz;
    const double n = (-10.0 * std::log10(b.deltaPass * b.deltaStop) - 13.0) / (14.6 * width) + 1.0;
    auto taps = static_cast<std::size_t>(std::ceil(std::clamp(n, 3.0, 1e9)));
    const bool odd = r.response == dsp::FilterResponse::HighPass || r.response == dsp::FilterResponse::BandStop;
    if (odd && taps % 2 == 0) ++taps;
    return taps;
}

// Turned around: the attenuation (dB) of the smaller of the two errors N taps would reach, their ratio set by
// the weights (δp/δs).
double equirippleReach(const EquirippleBands &b, const FilterRecipe &r, std::size_t taps)
{
    const double width = r.transitionHz / r.sampleRateHz;
    return 14.6 * width * (static_cast<double>(taps) - 1.0) + 13.0 +
           std::abs(10.0 * std::log10(b.deltaPass / b.deltaStop));
}

void designEquiripple(FilterDesign &d)
{
    const FilterRecipe &r = d.recipe;
    const double nyquist = 0.5 * r.sampleRateHz;
    if (!(r.sampleRateHz > 0.0) || !std::isfinite(r.sampleRateHz))
    {
        d.error = "La fréquence d'échantillonnage doit être positive.";
        return;
    }
    if (!(r.frequency1Hz > 0.0 && r.frequency1Hz < nyquist))
    {
        d.error = "La fréquence de coupure doit être comprise entre 0 et fs/2.";
        return;
    }
    if (band(r.response) && !(r.frequency2Hz > r.frequency1Hz && r.frequency2Hz < nyquist))
    {
        d.error = "Les bords de bande doivent vérifier 0 < f1 < f2 < fs/2.";
        return;
    }
    if (r.taps > kMaximumEquirippleTaps)
    {
        d.error = "Parks-McClellan : " + grouped(kMaximumEquirippleTaps) +
                  " coefficients au plus (au-delà, la méthode de la fenêtre).";
        return;
    }
    if ((r.response == dsp::FilterResponse::HighPass || r.response == dsp::FilterResponse::BandStop) && r.taps % 2 == 0)
    {
        d.error = "Un passe-haut ou un coupe-bande RIF demande un nombre impair de coefficients.";
        return;
    }
    const EquirippleBands bands = equirippleBands(r);
    if (!bands.error.empty())
    {
        d.error = bands.error;
        return;
    }
    d.estimatedTaps = equirippleLength(bands, r);

    // As many taps as asked, unless they would reach far beyond the precision of the computation: then the
    // most that stay within it (as many as the template needs at least), the others zeros. Stopped by rounding
    // all the same: fewer again, a few times.
    const std::size_t parity = r.taps % 2;
    const auto sameParity = [parity](std::size_t n) { return n % 2 == parity ? n : n - 1; };
    std::size_t length = r.taps;
    if (equirippleReach(bands, r, length) > kEquirippleReachDb)
    {
        const double width = r.transitionHz / r.sampleRateHz;
        const double room = kEquirippleReachDb - 13.0 - std::abs(10.0 * std::log10(bands.deltaPass / bands.deltaStop));
        const double most = std::floor(std::max(0.0, room) / (14.6 * width)) + 1.0;
        length = sameParity(std::clamp(static_cast<std::size_t>(std::min(most, 1e9)), d.estimatedTaps + 2, r.taps));
    }
    dsp::RemezSpec spec;
    spec.sampleRateHz = r.sampleRateHz;
    spec.bandsHz = bands.edges;
    spec.desired = bands.desired;
    spec.weights = bands.weights;
    spec.maximumIterations = kEquirippleIterations;
    dsp::RemezDesign remez;
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        spec.taps = length;
        remez = dsp::designRemez(spec);
        if (remez.ok() && remez.stop != dsp::RemezStop::Rounding) break;
        const std::size_t fewer = sameParity(length * 4 / 5);
        if (fewer < std::max<std::size_t>(3, d.estimatedTaps) || fewer >= length) break;
        length = fewer;
    }
    if (!remez.ok())
    {
        d.error = remez.error;
        return;
    }
    d.iterations = remez.iterations;
    d.exchangeNote = remez.stopped;
    d.computedTaps = remez.taps.size();
    d.taps.assign(r.taps, 0.0);
    const std::size_t margin = (r.taps - remez.taps.size()) / 2;
    std::copy(remez.taps.begin(), remez.taps.end(), d.taps.begin() + static_cast<std::ptrdiff_t>(margin));

    // What it reached: |H| over each band, on 64 points or more a ripple (the taps zero-padded, one FFT).
    const std::size_t points = dsp::nextPowerOfTwo(std::max<std::size_t>(65536, 64 * d.taps.size()));
    const std::vector<std::complex<double>> spectrum = dsp::rfft(d.taps, points);
    double passHigh = 0.0, passLow = std::numeric_limits<double>::infinity(), stopHigh = 0.0;
    for (std::size_t b = 0; b < bands.desired.size(); ++b)
    {
        const auto first = static_cast<std::size_t>(std::ceil(bands.edges[2 * b] / r.sampleRateHz * static_cast<double>(points)));
        const auto last = std::min(spectrum.size() - 1,
                                   static_cast<std::size_t>(std::floor(bands.edges[2 * b + 1] / r.sampleRateHz * static_cast<double>(points))));
        for (std::size_t k = first; k <= last; ++k)
        {
            const double h = std::abs(spectrum[k]);
            if (bands.desired[b] > 0.5)
            {
                passHigh = std::max(passHigh, h);
                passLow = std::min(passLow, h);
            }
            else
            {
                stopHigh = std::max(stopHigh, h);
            }
        }
    }
    d.achievedRippleDb = passLow > 0.0 ? 20.0 * std::log10(passHigh / passLow) : std::numeric_limits<double>::infinity();
    d.achievedAttenuationDb = stopHigh > 0.0 ? -20.0 * std::log10(stopHigh) : std::numeric_limits<double>::infinity();
    if (d.achievedRippleDb > r.passbandRippleDb * 1.001 + 1e-9 || d.achievedAttenuationDb < r.stopbandAttenuationDb - 0.01)
    {
        std::string advice = d.estimatedTaps > r.taps
                                 ? "il en faudrait environ " + grouped(d.estimatedTaps) + ", ou des transitions plus larges."
                                 : "ajoutez des coefficients ou élargissez les transitions.";
        d.warnings.push_back("Le gabarit n'est pas tenu avec " + grouped(r.taps) + " coefficients et des transitions de " +
                             quantity(r.transitionHz, "Hz") + " (obtenu : " + quantity(d.achievedRippleDb, "dB") + " et " +
                             quantity(d.achievedAttenuationDb, "dB") + ") : " + advice);
    }
}
} // namespace

namespace
{
std::string_view trimmed(std::string_view text)
{
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && blank(text.front())) text.remove_prefix(1);
    while (!text.empty() && blank(text.back())) text.remove_suffix(1);
    return text;
}

bool digit(char c)
{
    return c >= '0' && c <= '9';
}

// A number and nothing else: a sign (+, - or −), digits with a decimal point or comma, an exponent.
bool plainNumber(std::string_view t)
{
    if (t.starts_with("\xE2\x88\x92")) t.remove_prefix(3);
    else if (!t.empty() && (t.front() == '-' || t.front() == '+')) t.remove_prefix(1);
    std::size_t i = 0, digits = 0;
    while (i < t.size() && digit(t[i])) ++i, ++digits;
    if (i < t.size() && (t[i] == '.' || t[i] == ',')) ++i;
    while (i < t.size() && digit(t[i])) ++i, ++digits;
    if (digits == 0) return false;
    if (i < t.size() && (t[i] == 'e' || t[i] == 'E'))
    {
        ++i;
        if (i < t.size() && (t[i] == '-' || t[i] == '+')) ++i;
        std::size_t exponent = 0;
        while (i < t.size() && digit(t[i])) ++i, ++exponent;
        if (exponent == 0) return false;
    }
    return i == t.size();
}
} // namespace

std::vector<std::string> splitCoefficients(std::string_view text)
{
    std::string_view t = trimmed(text);
    // NumPy's "array([…])" or "array([…], dtype=…)", then the brackets.
    if (t.starts_with("array(") && t.ends_with(")"))
    {
        t = trimmed(t.substr(6, t.size() - 7));
        const std::size_t close = t.rfind(']');
        if (t.starts_with("[") && close != std::string_view::npos) t = t.substr(0, close + 1);
    }
    if (t.size() >= 2 && t.front() == '[' && t.back() == ']') t = trimmed(t.substr(1, t.size() - 2));
    const bool points = t.find('.') != std::string_view::npos;
    std::vector<std::string> items;
    std::string current;
    const auto push = [&items, &current] {
        const std::string_view item = trimmed(current);
        if (!item.empty()) items.emplace_back(item);
        current.clear();
    };
    int depth = 0;
    for (std::size_t i = 0; i < t.size(); ++i)
    {
        const char c = t[i];
        if (c == '(' || c == '[') ++depth;
        else if (c == ')' || c == ']') depth = std::max(0, depth - 1);
        bool split = false;
        if (depth == 0)
        {
            if (c == ';' || c == '\n') split = true;
            else if (c == ',')
                split = points || !(i > 0 && digit(t[i - 1]) && i + 1 < t.size() && digit(t[i + 1]));
        }
        if (split) push();
        else current += c;
    }
    push();
    // In each item: plain numbers separated by spaces ("1 -2 1"); else, two commas or more: a number has one
    // decimal comma at most, so these separate ("1,2,3").
    std::vector<std::string> separated;
    for (const std::string &item : items)
    {
        std::vector<std::string> words;
        std::string_view rest = item;
        while (!rest.empty())
        {
            const std::size_t end = rest.find_first_of(" \t\r");
            words.emplace_back(rest.substr(0, end));
            rest = end == std::string_view::npos ? std::string_view{} : trimmed(rest.substr(end));
        }
        if (words.size() > 1 && std::all_of(words.begin(), words.end(), [](const std::string &w) { return plainNumber(w); }))
        {
            separated.insert(separated.end(), words.begin(), words.end());
            continue;
        }
        std::vector<std::size_t> commas;
        int level = 0;
        for (std::size_t i = 0; i < item.size(); ++i)
        {
            if (item[i] == '(' || item[i] == '[') ++level;
            else if (item[i] == ')' || item[i] == ']') level = std::max(0, level - 1);
            else if (item[i] == ',' && level == 0) commas.push_back(i);
        }
        if (commas.size() < 2)
        {
            separated.push_back(item);
            continue;
        }
        std::size_t from = 0;
        commas.push_back(item.size());
        for (std::size_t at : commas)
        {
            const std::string_view part = trimmed(std::string_view(item).substr(from, at - from));
            if (!part.empty()) separated.emplace_back(part);
            from = at + 1;
        }
    }
    items = std::move(separated);
    return items;
}

TypedCoefficients evaluateTypedCoefficients(const FilterRecipe &recipe)
{
    TypedCoefficients out;
    const double fs = recipe.sampleRateHz;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<std::string> names{"fs", "Te", "k"};
    std::vector<double> values{fs, 1.0 / fs, nan};
    const Variables variables = parseVariables(recipe.variables, names, values);
    if (!variables.error.empty())
    {
        out.error = variables.error;
        return out;
    }
    names.insert(names.end(), variables.names.begin(), variables.names.end());
    values.insert(values.end(), variables.values.begin(), variables.values.end());
    // One list: its items, or one formula of k as many times as `countText` says.
    const auto evaluate = [&](const std::string &text, const std::string &countText, const std::string &letter,
                              std::size_t limit, std::vector<double> &result, bool &ofK) {
        const std::vector<std::string> items = splitCoefficients(text);
        if (items.empty())
        {
            out.error = letter + " : aucun coefficient" + (letter == "a" ? " (« 1 » pour un RIF)." : ".");
            return false;
        }
        if (items.size() > limit)
        {
            out.error = letter + " : plus de " + grouped(limit) + " coefficients.";
            return false;
        }
        std::vector<Expression> formulas;
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            std::string why;
            formulas.push_back(Expression::parse(items[i], names, &why));
            if (!formulas.back().valid())
            {
                out.error = (items.size() > 1 ? letter + "[" + std::to_string(i) + "]" : letter) + " : " + why;
                return false;
            }
        }
        // One formula: of k, or with how many given (b[k] = 1/N, N of them).
        const std::string_view typed = trimmed(countText);
        ofK = formulas.size() == 1 && (formulas.front().uses(2) || !typed.empty());
        std::size_t count = formulas.size();
        if (ofK)
        {
            if (typed.empty())
            {
                out.error = letter + "[k] dépend de k : combien de coefficients ? (« Nombre de " + letter + " »)";
                return false;
            }
            std::string why;
            const Expression e = Expression::parse(typed, names, &why);
            if (!e.valid())
            {
                out.error = "Nombre de " + letter + " : " + why;
                return false;
            }
            const double v = e.evaluate(values);
            const double whole = std::round(v);
            if (!std::isfinite(v) || std::abs(v - whole) > 1e-9 * std::max(1.0, std::abs(v)) || whole < 1.0 ||
                whole > static_cast<double>(limit))
            {
                out.error = "Nombre de " + letter + " : " + (std::isfinite(v) ? plot::formatCompact(v, "", 6) : std::string("?")) +
                            " n'est pas un nombre entier de 1 à " + grouped(limit) + ".";
                return false;
            }
            count = static_cast<std::size_t>(whole);
        }
        result.assign(count, 0.0);
        std::vector<double> at = values;
        for (std::size_t i = 0; i < count; ++i)
        {
            at[2] = static_cast<double>(i);
            const double v = formulas[ofK ? 0 : i].evaluate(at);
            if (!std::isfinite(v))
            {
                out.error = letter + "[" + std::to_string(i) + "] n'est pas un nombre fini (une division par zéro ?).";
                return false;
            }
            result[i] = v;
        }
        return true;
    };
    if (!evaluate(recipe.numerator, recipe.numeratorCount, "b", kMaximumFirTaps, out.numerator, out.numeratorOfK)) return out;
    evaluate(recipe.denominator, recipe.denominatorCount, "a", kMaximumFirTaps, out.denominator, out.denominatorOfK);
    return out;
}

namespace
{
// Roots found in double precision refined against the polynomial (ascending powers) evaluated in long double:
// a few Aberth steps, each root corrected by P/P′ with the pull of the others taken off (so that two near roots
// do not fall onto one). Where roots are close (a low cut-off: N poles near z = 1), the evaluation in double is
// what limits them; in long double (64 bits of mantissa with GCC on x86, MinGW included), about three digits
// more. A step that would move a root farther than the last one did is not taken.
void polishRoots(const std::vector<double> &ascending, std::vector<Complex> &roots)
{
    using Wide = std::complex<long double>;
    const std::size_t n = roots.size();
    if (n == 0 || ascending.size() != n + 1) return;
    std::vector<Wide> z(roots.begin(), roots.end());
    std::vector<long double> step(n, std::numeric_limits<long double>::infinity());
    for (int iteration = 0; iteration < 4; ++iteration)
    {
        std::vector<Wide> next = z;
        for (std::size_t i = 0; i < n; ++i)
        {
            Wide p = static_cast<long double>(ascending[n]), dp = 0.0L;
            for (std::size_t k = n; k-- > 0;)
            {
                dp = dp * z[i] + p;
                p = p * z[i] + static_cast<long double>(ascending[k]);
            }
            if (p == Wide(0.0L) || dp == Wide(0.0L)) continue;
            const Wide w = p / dp;
            Wide pull = 0.0L;
            for (std::size_t j = 0; j < n; ++j)
                if (j != i && z[i] != z[j]) pull += 1.0L / (z[i] - z[j]);
            const Wide correction = w / (1.0L - w * pull);
            const long double size = std::abs(correction);
            if (!std::isfinite(static_cast<double>(size)) || !(size < step[i])) continue;
            step[i] = size;
            next[i] = z[i] - correction;
        }
        z = std::move(next);
    }
    for (std::size_t i = 0; i < n; ++i)
    {
        // Real stays real, conjugates stay conjugate: the imaginary part of a real root is rounding.
        Complex r(static_cast<double>(z[i].real()), static_cast<double>(z[i].imag()));
        if (roots[i].imag() == 0.0) r = Complex(r.real(), 0.0);
        roots[i] = r;
    }
}

// The zeros of a numerator (ascending powers, its last coefficient not 0): those at z = −1 and z = 1 divided
// out first, as many times as they are there (the numerator of a Butterworth low-pass is (1 + z⁻¹)^N, a root of
// multiplicity N that an iteration finds to ε^(1/N) only: 3e-3 at N = 6), then the others. Taken for one when
// the polynomial is no more than rounding there (1e-13 of the sum of its coefficients; a root of multiplicity
// 10 leaves less than 1e-14). Not for the poles: N of them near z = 1 (a low cut-off) leave as little, and one
// put on the unit circle would make the filter look marginally stable.
std::vector<Complex> typedZeros(std::vector<double> ascending)
{
    std::vector<Complex> roots;
    for (const double r : {-1.0, 1.0})
    {
        while (ascending.size() > 1)
        {
            // Horner: P(r), and the quotient by (z − r).
            double scale = 0.0;
            for (double c : ascending) scale += std::abs(c);
            const std::size_t n = ascending.size() - 1;
            std::vector<double> quotient(n, 0.0);
            double value = ascending[n];
            for (std::size_t i = n; i-- > 0;)
            {
                quotient[i] = value;
                value = ascending[i] + r * value;
            }
            if (!(std::abs(value) <= 1e-13 * scale)) break;
            roots.emplace_back(r, 0.0);
            ascending = std::move(quotient);
        }
    }
    if (ascending.size() > 1)
    {
        std::vector<Complex> rest = dsp::polynomialRoots(ascending);
        polishRoots(ascending, rest);
        roots.insert(roots.end(), rest.begin(), rest.end());
    }
    return roots;
}

// Coefficients typed: divided by a[0], a FIR if no a is left, else roots, then sections (and a pure delay for
// the b at 0 in front); what makes it unstable, said.
void designTyped(FilterDesign &d)
{
    const TypedCoefficients t = evaluateTypedCoefficients(d.recipe);
    d.typedNumerator = t.numerator;
    d.typedDenominator = t.denominator;
    d.numeratorOfK = t.numeratorOfK;
    d.denominatorOfK = t.denominatorOfK;
    if (!t.error.empty())
    {
        d.error = t.error;
        return;
    }
    std::vector<double> b = t.numerator, a = t.denominator;
    const double a0 = a.front();
    if (a0 == 0.0)
    {
        d.error = "a[0] = 0 : le premier coefficient du dénominateur, celui de y[n], ne peut pas être nul.";
        return;
    }
    for (double &v : b) v /= a0;
    for (double &v : a) v /= a0;
    while (b.size() > 1 && b.back() == 0.0) b.pop_back();
    while (a.size() > 1 && a.back() == 0.0) a.pop_back();
    if (std::all_of(b.begin(), b.end(), [](double v) { return v == 0.0; }))
    {
        d.error = "Tous les b sont nuls : ce filtre ne laisse rien passer.";
        return;
    }
    if (a.size() == 1)
    {
        d.taps = std::move(b);
        if (d.taps.size() <= kMaximumFirRoots)
        {
            d.zeros = firZeros(d.taps);
            d.rootsKnown = true;
        }
        return;
    }
    if (b.size() > kMaximumTypedIirCoefficients || a.size() > kMaximumTypedIirCoefficients)
    {
        d.error = "Un filtre récursif tapé a au plus " + std::to_string(kMaximumTypedIirCoefficients) + " b et " +
                  std::to_string(kMaximumTypedIirCoefficients) + " a (ordre " +
                  std::to_string(kMaximumTypedIirCoefficients - 1) + ").";
        return;
    }
    if (d.recipe.application == FilterApplication::Aligned) d.recipe.application = FilterApplication::Causal;
    // H(z) = b[m] z^−m Π (1 − zᵢ z⁻¹) / Π (1 − pⱼ z⁻¹), m the b at 0 in front: the roots of b[m] z^(M−m) + … + b[M]
    // and of z^N + a[1] z^(N−1) + … + a[N] (their last terms not 0: no root at 0 among them).
    std::size_t delay = 0;
    while (b[delay] == 0.0) ++delay;
    const std::size_t m = b.size() - 1, n = a.size() - 1;
    std::vector<double> ascendingB(b.rbegin(), b.rend() - static_cast<std::ptrdiff_t>(delay));
    std::vector<double> ascendingA(a.rbegin(), a.rend());
    dsp::Zpk zpk;
    zpk.zeros = typedZeros(ascendingB);
    zpk.poles = dsp::polynomialRoots(ascendingA);
    polishRoots(ascendingA, zpk.poles);
    zpk.gain = b[delay];
    if (b.size() <= 3)
    {
        // A biquad (or less): one section, the coefficients as typed (a double root found from them would lose
        // half its digits).
        dsp::Biquad section;
        std::copy(b.begin(), b.end(), section.b.begin());
        std::copy(a.begin(), a.end(), section.a.begin());
        d.sections = {section};
    }
    else
    {
        d.sections = dsp::zpkToSos(zpk);
        for (; delay >= 2; delay -= 2) d.sections.push_back(dsp::Biquad{{0.0, 0.0, 1.0}, {1.0, 0.0, 0.0}});
        if (delay == 1) d.sections.push_back(dsp::Biquad{{0.0, 1.0, 0.0}, {1.0, 0.0, 0.0}});
    }
    // Shown as the roots of B(z) and A(z) written in powers of z: those at the origin as well.
    d.zeros = zpk.zeros;
    d.poles = zpk.poles;
    if (n > m) d.zeros.insert(d.zeros.end(), n - m, Complex(0.0, 0.0));
    if (m > n) d.poles.insert(d.poles.end(), m - n, Complex(0.0, 0.0));
    d.rootsKnown = true;
    // Sections from roots: as accurate as the roots are. Checked against b and a evaluated in long double, on
    // 513 frequencies up to fs/2.
    const double fs = d.recipe.sampleRateHz;
    const auto typedResponse = [&b, &a](double w) {
        using Wide = std::complex<long double>;
        const Wide e = std::polar(1.0L, -static_cast<long double>(w));
        const auto horner = [&e](const std::vector<double> &c) {
            Wide v = 0.0L;
            for (std::size_t k = c.size(); k-- > 0;) v = v * e + static_cast<long double>(c[k]);
            return v;
        };
        const Wide h = horner(b) / horner(a);
        return Complex(static_cast<double>(h.real()), static_cast<double>(h.imag()));
    };
    double worst = 0.0, largest = 0.0;
    for (int i = 0; i <= 512; ++i)
    {
        const double f = 0.5 * fs * static_cast<double>(i) / 512.0;
        const Complex typed = typedResponse(dsp::kTwoPi * f / fs);
        const Complex built = dsp::sosResponse(d.sections, f, fs);
        if (!std::isfinite(std::abs(typed)) || !std::isfinite(std::abs(built))) continue;
        worst = std::max(worst, std::abs(built - typed));
        largest = std::max(largest, std::abs(typed));
    }
    if (largest > 0.0 && worst > 1e-6 * largest)
        d.warnings.push_back("Ces coefficients placent mal leurs pôles (des pôles proches, d'une coupure basse ou d'un "
                             "ordre élevé : chaque chiffre de b et a compte) : en sections du second ordre, la réponse "
                             "s'écarte jusqu'à " + plot::formatCompact(worst / largest, "", 2) +
                             " (relatif) de celle de b et a. Le filtre appliqué est celui des sections ; mieux vaut le "
                             "donner en sections (SciPy : output='sos').");
    const double radius = d.largestPoleRadius();
    if (radius > 1.0 + 1e-9)
    {
        d.warnings.push_back("Instable : un pôle à |z| = " + plot::formatFixed(radius, 5) +
                             ", hors du cercle unité ; la réponse impulsionnelle grandit sans fin (seul son début est tracé).");
        d.applicationError = "Ce filtre est instable (un pôle hors du cercle unité) : appliqué, le signal filtré grandirait "
                             "sans fin. Il n'est pas appliqué.";
    }
    else if (radius >= 1.0 - 1e-9)
    {
        d.warnings.push_back("Un pôle est sur le cercle unité : la réponse impulsionnelle ne s'éteint pas (un intégrateur, "
                             "un oscillateur) ; elle n'est tracée que sur son début.");
        if (d.recipe.application == FilterApplication::ZeroPhase)
            d.applicationError = "À phase nulle, un pôle sur le cercle unité ne s'éteint jamais (aller puis retour) : "
                                 "appliquez ce filtre en causal.";
    }
    else if (radius > 0.99999)
    {
        d.warnings.push_back("Un pôle est à " + plot::formatFixed(1.0 - radius, 6) +
                             " du cercle unité : le filtre résonne longtemps, et en virgule fixe il deviendrait instable.");
    }
}
} // namespace

namespace
{
// An IIR of a family, as the recipe has it.
void designIirInto(FilterDesign &d)
{
    const FilterRecipe &r = d.recipe;
    dsp::IirSpec spec;
    spec.family = r.family;
    spec.response = r.response;
    spec.order = r.order;
    spec.sampleRateHz = r.sampleRateHz;
    spec.frequency1Hz = r.frequency1Hz;
    spec.frequency2Hz = r.frequency2Hz;
    spec.passbandRippleDb = r.passbandRippleDb;
    spec.stopbandAttenuationDb = r.stopbandAttenuationDb;
    const dsp::IirDesign iir = dsp::designIir(spec);
    if (!iir.ok())
    {
        d.error = iir.error.empty() ? std::string("Ce filtre ne peut pas être conçu.") : iir.error;
        return;
    }
    d.sections = iir.sections;
    d.zeros = iir.digital.zeros;
    d.poles = iir.digital.poles;
    d.rootsKnown = true;
    if (r.family == dsp::FilterFamily::Elliptic) d.stopEdgesHz = ellipticStopEdges(r);
    const double radius = d.largestPoleRadius();
    if (radius > 0.99999)
        d.warnings.push_back("Un pôle est à " + plot::formatFixed(1.0 - radius, 6) +
                             " du cercle unité : le filtre résonne longtemps, et en virgule fixe il deviendrait "
                             "instable (une fréquence de coupure plus haute, ou un ordre plus petit, l'éloigne).");
}

// A FIR by the window method, its taps only (its zeros: once designed, see firRoots).
void designWindowInto(FilterDesign &d)
{
    const FilterRecipe &r = d.recipe;
    dsp::FirSpec spec;
    spec.response = r.response;
    spec.taps = r.taps;
    spec.sampleRateHz = r.sampleRateHz;
    spec.frequency1Hz = r.frequency1Hz;
    spec.frequency2Hz = r.frequency2Hz;
    spec.window = r.window;
    spec.kaiserBeta = r.kaiserBeta;
    const dsp::FirDesign fir = dsp::designFir(spec);
    if (!fir.ok())
    {
        d.error = fir.error.empty() ? std::string("Ce filtre ne peut pas être conçu.") : fir.error;
        return;
    }
    d.taps = fir.taps;
}

// The zeros of a FIR designed, up to kMaximumFirRoots taps.
void firRoots(FilterDesign &d)
{
    if (d.ok() && !d.taps.empty() && d.taps.size() <= kMaximumFirRoots)
    {
        d.zeros = firZeros(d.taps);
        d.rootsKnown = true;
    }
}

void designFromTemplate(FilterDesign &d);
} // namespace

FilterDesign designFilter(const FilterRecipe &recipe)
{
    FilterDesign d;
    d.recipe = recipe;
    if (!applicationAllowed(recipe.kind, recipe.application)) d.recipe.application = FilterApplication::Causal;
    if (!std::isfinite(d.recipe.gainDb)) d.recipe.gainDb = 0.0;
    d.recipe.gainDb = std::clamp(d.recipe.gainDb, -kMaximumFilterGainDb, kMaximumFilterGainDb);
    d.gain = std::pow(10.0, d.recipe.gainDb / 20.0);
    const FilterRecipe &r = d.recipe;
    if (r.kind == FilterKind::None)
    {
        // No filter: one coefficient, 1 (the output is the input), no zero to find.
        d.recipe.fromTemplate = false;
        d.taps = {1.0};
        d.rootsKnown = true;
        return d;
    }
    if (r.kind == FilterKind::Typed)
    {
        designTyped(d);
        return d;
    }
    if (r.fromTemplate)
    {
        designFromTemplate(d);
        return d;
    }
    if (r.kind == FilterKind::Iir)
    {
        designIirInto(d);
        return d;
    }
    if (r.taps > kMaximumFirTaps)
    {
        d.error = "Un RIF de plus de " + grouped(kMaximumFirTaps) + " coefficients n'est pas conçu ici.";
        return d;
    }
    if (r.firMethod == FirMethod::Equiripple)
        designEquiripple(d);
    else
        designWindowInto(d);
    firRoots(d);
    return d;
}

// ------------------------------------------------------------------------------------------ the template (gabarit)
dsp::FilterTemplate filterTemplate(const FilterRecipe &r)
{
    dsp::FilterTemplate t;
    t.response = r.response;
    t.sampleRateHz = r.sampleRateHz;
    t.passHz = {r.passEdge1Hz, r.passEdge2Hz};
    t.stopHz = {r.stopEdge1Hz, r.stopEdge2Hz};
    t.passbandLossDb = r.passbandRippleDb;
    t.stopbandAttenuationDb = r.stopbandAttenuationDb;
    return t;
}

bool templateEdgesValid(const FilterRecipe &r)
{
    // The edges only: Amax and Amin have their own message.
    dsp::FilterTemplate t = filterTemplate(r);
    t.passbandLossDb = 1.0;
    t.stopbandAttenuationDb = 40.0;
    return dsp::templateError(t).empty();
}

void setTemplateFromFrequencies(FilterRecipe &r)
{
    const double nyquist = 0.5 * r.sampleRateHz;
    // f times `factor`, unless that comes near fs/2: then half the way to it.
    const auto beyond = [nyquist](double f, double factor) {
        return f * factor < 0.95 * nyquist ? f * factor : f + 0.5 * (nyquist - f);
    };
    double f1 = r.frequency1Hz, f2 = r.frequency2Hz;
    if (!(f1 > 0.0 && f1 < nyquist)) f1 = std::min(1000.0, niceFloor(nyquist / 5.0));
    if (band(r.response) && !(f2 > f1 && f2 < nyquist)) f2 = beyond(f1, 2.0);
    switch (r.response)
    {
        case dsp::FilterResponse::LowPass:
            r.passEdge1Hz = f1;
            r.stopEdge1Hz = beyond(f1, 2.0);
            break;
        case dsp::FilterResponse::HighPass:
            r.passEdge1Hz = f1;
            r.stopEdge1Hz = 0.5 * f1;
            break;
        case dsp::FilterResponse::BandPass:
            r.passEdge1Hz = f1;
            r.passEdge2Hz = f2;
            r.stopEdge1Hz = 0.5 * f1;
            r.stopEdge2Hz = beyond(f2, 2.0);
            break;
        case dsp::FilterResponse::BandStop:
        {
            // The middle half of the band, in logarithm.
            const double quarter = std::pow(f2 / f1, 0.25);
            r.passEdge1Hz = f1;
            r.passEdge2Hz = f2;
            r.stopEdge1Hz = f1 * quarter;
            r.stopEdge2Hz = f2 / quarter;
            break;
        }
    }
}

namespace
{
// A FIR's pass band within 1 ± δp: (1 + δp)/(1 − δp) = 10^(Ap/20), the ripple peak to peak.
double passDeviation(double rippleDb)
{
    const double g = std::pow(10.0, rippleDb / 20.0);
    return (g - 1.0) / (g + 1.0);
}

double narrowestTransition(const dsp::FilterTemplate &t)
{
    const std::array<double, 2> &p = t.passHz, &s = t.stopHz;
    switch (t.response)
    {
        case dsp::FilterResponse::LowPass: return s[0] - p[0];
        case dsp::FilterResponse::HighPass: return p[0] - s[0];
        case dsp::FilterResponse::BandPass: return std::min(p[0] - s[0], s[1] - p[1]);
        case dsp::FilterResponse::BandStop: return std::min(s[0] - p[0], p[1] - s[1]);
    }
    return 0.0;
}

// The middles of the transitions: the cut-offs of a FIR by a window (frequency1, frequency2).
std::array<double, 2> transitionMiddles(const dsp::FilterTemplate &t)
{
    const std::array<double, 2> &p = t.passHz, &s = t.stopHz;
    switch (t.response)
    {
        case dsp::FilterResponse::LowPass:
        case dsp::FilterResponse::HighPass: return {0.5 * (p[0] + s[0]), 0.0};
        case dsp::FilterResponse::BandPass:
        case dsp::FilterResponse::BandStop: return {0.5 * (p[0] + s[0]), 0.5 * (p[1] + s[1])};
    }
    return {0.0, 0.0};
}
} // namespace

TemplateZones templateZones(const FilterRecipe &r)
{
    TemplateZones z;
    const double nyquist = 0.5 * r.sampleRateHz;
    const double p0 = r.passEdge1Hz, p1 = r.passEdge2Hz, s0 = r.stopEdge1Hz, s1 = r.stopEdge2Hz;
    switch (r.response)
    {
        case dsp::FilterResponse::LowPass:
            z.passBands = {{0.0, p0}};
            z.stopBands = {{s0, nyquist}};
            break;
        case dsp::FilterResponse::HighPass:
            z.stopBands = {{0.0, s0}};
            z.passBands = {{p0, nyquist}};
            break;
        case dsp::FilterResponse::BandPass:
            z.stopBands = {{0.0, s0}, {s1, nyquist}};
            z.passBands = {{p0, p1}};
            break;
        case dsp::FilterResponse::BandStop:
            z.passBands = {{0.0, p0}, {p1, nyquist}};
            z.stopBands = {{s0, s1}};
            break;
    }
    z.stopHigh = std::pow(10.0, -r.stopbandAttenuationDb / 20.0);
    if (r.kind == FilterKind::Fir)
    {
        const double deviation = passDeviation(r.passbandRippleDb);
        z.passLow = 1.0 - deviation;
        z.passHigh = 1.0 + deviation;
    }
    else
    {
        z.passLow = std::pow(10.0, -r.passbandRippleDb / 20.0);
        z.passHigh = 1.0;
    }
    return z;
}

namespace
{
// The least and the greatest |H| of the filter at 0 dB over a band.
struct BandLevels
{
    double least = std::numeric_limits<double>::infinity();
    double greatest = 0.0;
};

// The extreme of `magnitude` between a and b, by golden section (one there, where a grid found it).
template <typename Magnitude>
double refined(const Magnitude &magnitude, double a, double b, bool greatest)
{
    const double ratio = 0.5 * (std::sqrt(5.0) - 1.0);
    double x1 = b - ratio * (b - a), x2 = a + ratio * (b - a);
    double m1 = magnitude(x1), m2 = magnitude(x2);
    for (int i = 0; i < 60 && b - a > 1e-13 * std::max(1.0, std::abs(b)); ++i)
    {
        if (greatest ? m1 > m2 : m1 < m2)
        {
            b = x2;
            x2 = x1;
            m2 = m1;
            x1 = b - ratio * (b - a);
            m1 = magnitude(x1);
        }
        else
        {
            a = x1;
            x1 = x2;
            m1 = m2;
            x2 = a + ratio * (b - a);
            m2 = magnitude(x2);
        }
    }
    return greatest ? std::max(m1, m2) : std::min(m1, m2);
}

// Over a grid (a FIR: the bins of one FFT of its taps, `spectrum` of `points`; an IIR: 2 048 points, in logarithm
// over more than a decade), the edges of the band themselves, and the least and the greatest refined between their
// neighbours.
BandLevels bandLevels(const FilterDesign &d, double low, double high, const std::vector<Complex> &spectrum, std::size_t points)
{
    const double fs = d.recipe.sampleRateHz;
    const auto magnitude = [&d](double f) { return std::abs(d.unitResponse(f)); };
    std::vector<double> f, m;
    f.push_back(low);
    m.push_back(magnitude(low));
    if (!spectrum.empty())
    {
        const double perHz = static_cast<double>(points) / fs;
        const auto first = static_cast<std::size_t>(std::ceil(low * perHz));
        const std::size_t last = std::min(spectrum.size() - 1, static_cast<std::size_t>(std::floor(high * perHz)));
        for (std::size_t k = first; k <= last; ++k)
        {
            const double at = static_cast<double>(k) / perHz;
            if (at <= low || at >= high) continue;
            f.push_back(at);
            m.push_back(std::abs(spectrum[k]));
        }
    }
    else
    {
        constexpr std::size_t kPoints = 2048;
        const bool logarithmic = low > 0.0 && high > 20.0 * low;
        for (std::size_t i = 1; i < kPoints; ++i)
        {
            const double t = static_cast<double>(i) / static_cast<double>(kPoints);
            const double at = logarithmic ? low * std::pow(high / low, t) : low + (high - low) * t;
            f.push_back(at);
            m.push_back(magnitude(at));
        }
    }
    f.push_back(high);
    m.push_back(magnitude(high));
    std::size_t least = 0, greatest = 0;
    for (std::size_t i = 1; i < m.size(); ++i)
    {
        if (m[i] < m[least]) least = i;
        if (m[i] > m[greatest]) greatest = i;
    }
    BandLevels out;
    out.least = m[least];
    out.greatest = m[greatest];
    if (least > 0 && least + 1 < m.size()) out.least = std::min(out.least, refined(magnitude, f[least - 1], f[least + 1], false));
    if (greatest > 0 && greatest + 1 < m.size())
        out.greatest = std::max(out.greatest, refined(magnitude, f[greatest - 1], f[greatest + 1], true));
    return out;
}

} // namespace

bool templateFromDesign(const FilterDesign &d, FilterRecipe &r)
{
    if (!d.ok() || !designedKind(d.recipe.kind) || r.response != d.recipe.response) return false;
    FilterRecipe levels = r;
    levels.kind = d.recipe.kind;
    const TemplateZones z = templateZones(levels);
    const double nyquist = 0.5 * d.recipe.sampleRateHz;
    // On the grid, within the zones to 1e-9 (a ripple that reaches −Ap, a stop band that comes back to −As, stay in);
    // at the edges, the zones themselves (a template the filter holds, not one a hair harder).
    const auto gain = [&d](double f) { return std::abs(d.unitResponse(f)); };
    const auto passes = [&](double f, double slack) {
        const double g = gain(f);
        return g >= z.passLow * (1.0 - slack) && g <= z.passHigh * (1.0 + slack);
    };
    const auto stops = [&](double f, double slack) { return gain(f) <= z.stopHigh * (1.0 + slack); };
    const auto inPass = [&](double f) { return passes(f, 0.0); };
    const auto inStop = [&](double f) { return stops(f, 0.0); };
    // A grid both linear and logarithmic (down to fs/2·1e-5).
    std::vector<double> f;
    constexpr int kPoints = 4096;
    for (int i = 0; i <= kPoints; ++i)
    {
        f.push_back(nyquist * i / kPoints);
        f.push_back(nyquist * std::pow(1e-5, 1.0 - static_cast<double>(i) / kPoints));
    }
    std::sort(f.begin(), f.end());
    f.erase(std::unique(f.begin(), f.end()), f.end());
    const std::size_t n = f.size();
    std::vector<char> pass(n), stop(n);
    std::vector<double> g(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        g[i] = gain(f[i]);
        pass[i] = passes(f[i], 1e-9) ? 1 : 0;
        stop[i] = stops(f[i], 1e-9) ? 1 : 0;
    }
    // Where `test` stops holding, between a frequency where it holds and one where it does not.
    const auto boundary = [](auto &&test, double inside, double outside) {
        for (int i = 0; i < 60; ++i)
        {
            const double middle = 0.5 * (inside + outside);
            (test(middle) ? inside : outside) = middle;
        }
        return inside;
    };
    // The first index where flags fail (n: never), the last one (n: never).
    const auto firstFailing = [n](const std::vector<char> &flags) {
        for (std::size_t i = 0; i < n; ++i)
            if (!flags[i]) return i;
        return n;
    };
    const auto lastFailing = [n](const std::vector<char> &flags) {
        for (std::size_t i = n; i-- > 0;)
            if (!flags[i]) return i;
        return n;
    };
    // The band from 0 where flags hold, its end; the band up to fs/2, its start. 0: none.
    const auto fromZero = [&](const std::vector<char> &flags, auto &&test) {
        const std::size_t i = firstFailing(flags);
        return i == 0 || i == n ? 0.0 : boundary(test, f[i - 1], f[i]);
    };
    const auto toNyquist = [&](const std::vector<char> &flags, auto &&test) {
        const std::size_t j = lastFailing(flags);
        return j == n || j + 1 == n ? 0.0 : boundary(test, f[j + 1], f[j]);
    };
    // The band about index c where flags hold: its two ends. Zeros: none.
    const auto around = [&](const std::vector<char> &flags, auto &&test, std::size_t c) {
        if (!flags[c]) return std::array<double, 2>{0.0, 0.0};
        std::size_t low = c, high = c;
        while (low > 0 && flags[low - 1]) --low;
        while (high + 1 < n && flags[high + 1]) ++high;
        if (low == 0 || high + 1 == n) return std::array<double, 2>{0.0, 0.0};
        return std::array<double, 2>{boundary(test, f[low], f[low - 1]), boundary(test, f[high], f[high + 1])};
    };
    const auto extreme = [&](bool greatest) {
        std::size_t best = 0;
        for (std::size_t i = 1; i < n; ++i)
            if (greatest ? g[i] > g[best] : g[i] < g[best]) best = i;
        return best;
    };
    std::array<double, 2> p{0.0, 0.0}, s{0.0, 0.0};
    switch (r.response)
    {
        case dsp::FilterResponse::LowPass:
            p[0] = fromZero(pass, inPass);
            s[0] = toNyquist(stop, inStop);
            break;
        case dsp::FilterResponse::HighPass:
            s[0] = fromZero(stop, inStop);
            p[0] = toNyquist(pass, inPass);
            break;
        case dsp::FilterResponse::BandPass:
            s[0] = fromZero(stop, inStop);
            s[1] = toNyquist(stop, inStop);
            p = around(pass, inPass, extreme(true));
            break;
        case dsp::FilterResponse::BandStop:
            p[0] = fromZero(pass, inPass);
            p[1] = toNyquist(pass, inPass);
            s = around(stop, inStop, extreme(false));
            break;
    }
    // A FIR by a window has its cut-offs in the middle of the transitions of a template: each transition made
    // symmetric about the cut-off, as wide as the wider side asks (its deviation is the same on both sides), so
    // that the template gives this filter back.
    if (d.recipe.kind == FilterKind::Fir && d.recipe.firMethod == FirMethod::Window)
    {
        const auto symmetric = [nyquist](double &below, double cutoff, double &above) {
            const double x = std::max(cutoff - below, above - cutoff);
            below = cutoff - x;
            above = cutoff + x;
            return below > 0.0 && above < nyquist;
        };
        const double f1 = d.recipe.frequency1Hz, f2 = d.recipe.frequency2Hz;
        bool fits = false;
        switch (r.response)
        {
            case dsp::FilterResponse::LowPass: fits = symmetric(p[0], f1, s[0]); break;
            case dsp::FilterResponse::HighPass: fits = symmetric(s[0], f1, p[0]); break;
            case dsp::FilterResponse::BandPass: fits = symmetric(s[0], f1, p[0]) && symmetric(p[1], f2, s[1]); break;
            case dsp::FilterResponse::BandStop: fits = symmetric(p[0], f1, s[0]) && symmetric(s[1], f2, p[1]); break;
        }
        if (!fits) return false;
    }
    // Rounded towards an easier template: each pass band narrower, each stop band too; more digits if 3 put two
    // edges together, or if an IIR would then ask a higher order than it has (a band-stop, whose order hangs on
    // how symmetric its edges are).
    const auto rounded = [](double x, int digits, bool up) {
        if (!(x > 0.0)) return x;
        const double step = std::pow(10.0, std::floor(std::log10(x)) - (digits - 1));
        return (up ? std::ceil(x / step - 1e-9) : std::floor(x / step + 1e-9)) * step;
    };
    // Up: the edges that move into their band upwards (a low-pass's stop edge…).
    std::array<bool, 2> passUp{false, false}, stopUp{false, false};
    switch (r.response)
    {
        case dsp::FilterResponse::LowPass: stopUp = {true, false}; break;
        case dsp::FilterResponse::HighPass: passUp = {true, false}; break;
        case dsp::FilterResponse::BandPass: passUp = {true, false}; stopUp = {false, true}; break;
        case dsp::FilterResponse::BandStop: passUp = {false, true}; stopUp = {true, false}; break;
    }
    for (int digits = 3; digits <= 9; ++digits)
    {
        FilterRecipe t = r;
        t.passEdge1Hz = rounded(p[0], digits, passUp[0]);
        t.passEdge2Hz = band(r.response) ? rounded(p[1], digits, passUp[1]) : r.passEdge2Hz;
        t.stopEdge1Hz = rounded(s[0], digits, stopUp[0]);
        t.stopEdge2Hz = band(r.response) ? rounded(s[1], digits, stopUp[1]) : r.stopEdge2Hz;
        if (!templateEdgesValid(t)) continue;
        if (d.recipe.kind == FilterKind::Iir)
        {
            const dsp::IirOrder o = dsp::minimumIirOrder(d.recipe.family, filterTemplate(t));
            if (!o.ok() || o.order > d.recipe.order) continue;
        }
        r = t;
        return true;
    }
    return false;
}

TemplateMeasure measureTemplate(const FilterDesign &d)
{
    const TemplateZones z = templateZones(d.recipe);
    std::vector<Complex> spectrum;
    std::size_t points = 0;
    if (!d.taps.empty())
    {
        points = dsp::nextPowerOfTwo(std::max<std::size_t>(8192, 16 * d.taps.size()));
        spectrum = dsp::rfft(d.taps, points);
    }
    double passLeast = std::numeric_limits<double>::infinity(), passGreatest = 0.0, stopGreatest = 0.0;
    for (const std::array<double, 2> &b : z.passBands)
    {
        const BandLevels l = bandLevels(d, b[0], b[1], spectrum, points);
        passLeast = std::min(passLeast, l.least);
        passGreatest = std::max(passGreatest, l.greatest);
    }
    for (const std::array<double, 2> &b : z.stopBands)
        stopGreatest = std::max(stopGreatest, bandLevels(d, b[0], b[1], spectrum, points).greatest);
    TemplateMeasure c;
    c.lossDb = passLeast > 0.0 ? 20.0 * std::log10(passGreatest / passLeast) : std::numeric_limits<double>::infinity();
    c.attenuationDb = stopGreatest > 0.0 ? -20.0 * std::log10(stopGreatest) : std::numeric_limits<double>::infinity();
    constexpr double kTolerance = 1e-7;
    c.held = passLeast >= z.passLow * (1.0 - kTolerance) && passGreatest <= z.passHigh * (1.0 + kTolerance) &&
             stopGreatest <= z.stopHigh * (1.0 + kTolerance);
    c.excessDb = std::max({20.0 * std::log10(z.passLow / passLeast), 20.0 * std::log10(passGreatest / z.passHigh),
                           20.0 * std::log10(stopGreatest / z.stopHigh)});
    return c;
}

namespace
{

// The least n = first + step·k, at most `maximum`, for which holds(n) is true, holds taken as growing with n: from
// the estimate, galloping towards the boundary, then halving. 0 when not even the largest holds.
template <typename Holds>
std::size_t leastHolding(Holds &holds, std::size_t estimate, std::size_t first, std::size_t step, std::size_t maximum)
{
    if (maximum < first) return 0;
    const std::size_t last = (maximum - first) / step;
    const auto at = [first, step](std::size_t k) { return first + step * k; };
    const std::size_t start = estimate > first ? std::min(last, (estimate - first + step - 1) / step) : 0;
    std::size_t good = start, bad = start;
    if (holds(at(start)))
    {
        for (std::size_t jump = 1;; jump *= 2)
        {
            if (good == 0) return at(0);
            const std::size_t probe = good > jump ? good - jump : 0;
            if (!holds(at(probe)))
            {
                bad = probe;
                break;
            }
            good = probe;
        }
    }
    else
    {
        for (std::size_t jump = 1;; jump *= 2)
        {
            if (bad == last) return 0;
            const std::size_t probe = std::min(last, bad + jump);
            if (holds(at(probe)))
            {
                good = probe;
                break;
            }
            bad = probe;
        }
    }
    while (good - bad > 1)
    {
        const std::size_t middle = bad + (good - bad) / 2;
        (holds(at(middle)) ? good : bad) = middle;
    }
    return at(good);
}

// The least taps that hold: odd numbers first (only them for a high-pass or a band-stop), then the even number just
// below the odd one found, and below it while that holds.
template <typename Holds>
std::size_t leastTaps(Holds &holds, std::size_t estimate, bool oddOnly, std::size_t maximum)
{
    const std::size_t odd = leastHolding(holds, estimate, 3, 2, maximum);
    if (oddOnly || odd == 0) return odd; // (not even the most taps of odd number: one fewer will not do)
    if (odd < 5 || !holds(odd - 1)) return odd;
    return leastHolding(holds, odd - 1, 4, 2, odd - 1);
}

// The taps the last templates asked (the search designs a long FIR several times over): kept by what it depends on.
std::size_t rememberedTaps(const std::string &key, std::size_t taps = 0)
{
    static std::mutex mutex;
    static std::map<std::string, std::size_t> remembered;
    const std::lock_guard<std::mutex> lock(mutex);
    if (taps == 0)
    {
        const auto found = remembered.find(key);
        return found == remembered.end() ? 0 : found->second;
    }
    if (remembered.size() >= 64) remembered.clear();
    remembered[key] = taps;
    return taps;
}

std::string templateKey(const FilterRecipe &r)
{
    std::array<char, 512> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%s %s %s %.17g %.17g %.17g %.17g %.17g %.17g %.17g",
                  std::string(firMethodKey(r.firMethod)).c_str(), std::string(windowKey(r.window)).c_str(),
                  std::string(filterResponseKey(r.response)).c_str(), r.sampleRateHz, r.passEdge1Hz, r.passEdge2Hz,
                  r.stopEdge1Hz, r.stopEdge2Hz, r.passbandRippleDb, r.stopbandAttenuationDb);
    return buffer.data();
}

// What a window reaches by the window method (the same deviation in both bands, dB) with a transition of D·fs/N, D
// from 1 to 20: measured on scipy.signal.firwin designs of 101, 501 and 2 001 taps, the least of the three.
// Kaiser's window has its formula instead (empty).
struct WindowReach
{
    double transitions; // D
    double attenuationDb;
};

const std::vector<WindowReach> &windowReach(dsp::Window window)
{
    static const std::array<double, 21> d{1, 1.5, 2, 2.5, 3, 3.5, 4, 4.5, 5, 5.5, 6, 6.5, 7, 7.5, 8, 9, 10, 12, 14, 16, 20};
    const auto table = [](std::array<double, 21> attenuation) {
        std::vector<WindowReach> list;
        for (std::size_t i = 0; i < d.size(); ++i) list.push_back({d[i], attenuation[i]});
        return list;
    };
    static const std::vector<WindowReach> rectangular =
        table({21, 21, 21, 24, 26, 26, 26, 29, 29, 29, 29, 31, 31, 31, 31, 33, 33, 34, 35, 36, 38});
    static const std::vector<WindowReach> hann =
        table({12, 15, 20, 27, 38, 44, 44, 46, 55, 55, 55, 57, 62, 62, 62, 68, 68, 73, 77, 80, 86});
    static const std::vector<WindowReach> hamming =
        table({12, 16, 22, 29, 39, 54, 54, 56, 56, 56, 56, 56, 56, 56, 56, 56, 57, 57, 58, 59, 60});
    static const std::vector<WindowReach> blackman =
        table({10, 13, 17, 21, 26, 32, 39, 48, 59, 72, 75, 75, 75, 75, 75, 79, 79, 83, 86, 90, 95});
    static const std::vector<WindowReach> blackmanHarris =
        table({10, 12, 15, 18, 21, 25, 30, 35, 41, 48, 56, 66, 78, 95, 109, 110, 110, 110, 110, 113, 117});
    static const std::vector<WindowReach> none;
    switch (window)
    {
        case dsp::Window::Rectangular: return rectangular;
        case dsp::Window::Hann: return hann;
        case dsp::Window::Hamming: return hamming;
        case dsp::Window::Blackman: return blackman;
        case dsp::Window::BlackmanHarris: return blackmanHarris;
        case dsp::Window::FlatTop:
        case dsp::Window::Kaiser: break;
    }
    return none;
}

// Kaiser's estimate (1974) of the taps of an equiripple FIR: N − 1 ≈ (−10 log10(δp·δs) − 13)/(14,6·Δf/fs).
double equirippleEstimate(double deltaPass, double deltaStop, double transitionHz, double fs)
{
    return (-10.0 * std::log10(deltaPass * deltaStop) - 13.0) / (14.6 * transitionHz / fs) + 1.0;
}

TemplateOrders orders(const dsp::FilterTemplate &t)
{
    TemplateOrders o;
    const auto order = [&t](dsp::FilterFamily family) {
        const dsp::IirOrder found = dsp::minimumIirOrder(family, t);
        return found.ok() ? found.order : 0;
    };
    o.butterworth = order(dsp::FilterFamily::Butterworth);
    o.chebyshev = order(dsp::FilterFamily::ChebyshevI);
    o.elliptic = order(dsp::FilterFamily::Elliptic);
    o.bessel = order(dsp::FilterFamily::Bessel);
    const double deltaPass = passDeviation(t.passbandLossDb);
    const double deltaStop = std::pow(10.0, -t.stopbandAttenuationDb / 20.0);
    const double transition = narrowestTransition(t);
    const bool odd = t.response == dsp::FilterResponse::HighPass || t.response == dsp::FilterResponse::BandStop;
    const auto taps = [odd](double n) {
        auto count = static_cast<std::size_t>(std::ceil(std::clamp(n, 3.0, 1e9)));
        if (odd && count % 2 == 0) ++count;
        return count;
    };
    const double attenuation = -20.0 * std::log10(std::min(deltaPass, deltaStop));
    o.kaiserTaps = taps(static_cast<double>(dsp::estimateKaiser(attenuation, transition, t.sampleRateHz).taps));
    o.equirippleTaps = taps(equirippleEstimate(deltaPass, deltaStop, transition, t.sampleRateHz));
    return o;
}

// "la fenêtre de Hann", "la fenêtre rectangulaire".
std::string windowPhrase(dsp::Window window)
{
    if (window == dsp::Window::Rectangular) return "la fenêtre rectangulaire";
    if (window == dsp::Window::FlatTop) return "la fenêtre à sommet plat";
    return "la fenêtre de " + std::string(dsp::windowName(window));
}

std::string familyOrderText(int order, int maximum)
{
    return order > maximum ? "au-delà de " + std::to_string(maximum) : "d'ordre " + std::to_string(order);
}

// "Butterworth d'ordre 9, Chebyshev d'ordre 5, elliptique d'ordre 4, Bessel au-delà de 12".
std::string iirOrdersText(const TemplateOrders &o)
{
    std::vector<std::string> parts;
    const auto add = [&parts](std::string_view name, int order, int maximum) {
        if (order > 0) parts.push_back(std::string(name) + " " + familyOrderText(order, maximum));
    };
    add("Butterworth", o.butterworth, dsp::kMaximumIirOrder);
    add("Chebyshev", o.chebyshev, dsp::kMaximumIirOrder);
    add("elliptique", o.elliptic, dsp::kMaximumIirOrder);
    add("Bessel", o.bessel, dsp::kMaximumBesselOrder);
    std::string text;
    for (std::size_t i = 0; i < parts.size(); ++i) text += (i > 0 ? ", " : "") + parts[i];
    return text;
}

void designIirFromTemplate(FilterDesign &d, const dsp::FilterTemplate &t)
{
    FilterRecipe &r = d.recipe;
    const dsp::IirOrder o = dsp::minimumIirOrder(r.family, t);
    if (!o.ok())
    {
        d.error = o.error;
        return;
    }
    d.exactOrder = o.exactOrder;
    const int maximum = r.family == dsp::FilterFamily::Bessel ? dsp::kMaximumBesselOrder : dsp::kMaximumIirOrder;
    if (o.order > maximum)
    {
        // What does hold it.
        const TemplateOrders &a = d.templateOrders;
        std::vector<std::string> others;
        const auto other = [&](dsp::FilterFamily family, std::string_view name, int order, int most) {
            if (family != r.family && order > 0 && order <= most) others.push_back(std::string(name) + " d'ordre " + std::to_string(order));
        };
        other(dsp::FilterFamily::Elliptic, "un elliptique", a.elliptic, dsp::kMaximumIirOrder);
        if (r.family != dsp::FilterFamily::ChebyshevI && r.family != dsp::FilterFamily::ChebyshevII)
            other(dsp::FilterFamily::ChebyshevI, "un Chebyshev", a.chebyshev, dsp::kMaximumIirOrder);
        other(dsp::FilterFamily::Butterworth, "un Butterworth", a.butterworth, dsp::kMaximumIirOrder);
        other(dsp::FilterFamily::Bessel, "un Bessel", a.bessel, dsp::kMaximumBesselOrder);
        std::string advice;
        for (std::size_t i = 0; i < others.size(); ++i) advice += (i == 0 ? "" : i + 1 == others.size() ? " ou " : ", ") + others[i];
        const std::string asked = r.family == dsp::FilterFamily::Bessel || o.order > 1000
                                      ? "d'ordre supérieur à " + std::to_string(maximum)
                                      : "d'ordre " + std::to_string(o.order) + ", plus que " + std::to_string(maximum);
        d.error = "Ce gabarit demande un " + std::string(dsp::familyName(r.family)) + " " + asked + " : " +
                  (advice.empty() ? "élargissez les transitions, ou prenez un RIF (environ " + grouped(a.equirippleTaps) +
                                        " coefficients en équiondulation)."
                                  : advice + " le tient ; ou élargissez les transitions.");
        return;
    }
    r.order = o.order;
    r.frequency1Hz = o.frequency1Hz;
    if (band(r.response)) r.frequency2Hz = o.frequency2Hz;
    designIirInto(d);
}

// A FIR by a window: the cut-offs in the middle of the transitions, the least taps that hold the template (a Kaiser
// window: β from the deviation asked), searched from the estimate of the window.
void designWindowFromTemplate(FilterDesign &d, const dsp::FilterTemplate &t)
{
    FilterRecipe &r = d.recipe;
    const double fs = r.sampleRateHz;
    const double deltaPass = passDeviation(t.passbandLossDb);
    const double deltaStop = std::pow(10.0, -t.stopbandAttenuationDb / 20.0);
    // The deviation of a window design is about the same in both bands: the smaller of the two decides.
    const double attenuation = -20.0 * std::log10(std::min(deltaPass, deltaStop));
    const double transition = narrowestTransition(t);
    const std::array<double, 2> middles = transitionMiddles(t);
    r.frequency1Hz = middles[0];
    if (band(r.response)) r.frequency2Hz = middles[1];
    double estimate = 0.0;
    if (r.window == dsp::Window::Kaiser)
    {
        const dsp::KaiserEstimate k = dsp::estimateKaiser(attenuation, transition, fs);
        r.kaiserBeta = k.beta;
        estimate = static_cast<double>(k.taps);
    }
    else
    {
        const std::vector<WindowReach> &reach = windowReach(r.window);
        if (reach.empty())
        {
            d.error = "Le gabarit ne se calcule pas avec " + windowPhrase(r.window) +
                      " : prenez Kaiser, Blackman ou l'équiondulation.";
            return;
        }
        const auto found = std::find_if(reach.begin(), reach.end(),
                                        [attenuation](const WindowReach &w) { return w.attenuationDb >= attenuation; });
        if (found == reach.end())
        {
            d.error = "Avec " + windowPhrase(r.window) + ", un RIF n'atteint guère plus de " +
                      quantity(reach.back().attenuationDb, "dB") + " ; ce gabarit en demande " + quantity(attenuation, "dB") +
                      (deltaPass < deltaStop ? " (pour l'ondulation de la bande passante)" : "") +
                      " : prenez Kaiser, Blackman-Harris ou l'équiondulation.";
            return;
        }
        estimate = found->transitions * fs / transition;
    }
    const bool odd = r.response == dsp::FilterResponse::HighPass || r.response == dsp::FilterResponse::BandStop;
    // Far beyond the most taps designed: said at once (the estimate is close enough for that).
    if (estimate > 1.25 * static_cast<double>(kMaximumFirTaps))
    {
        d.error = "Ce gabarit demande environ " + grouped(static_cast<std::size_t>(estimate)) + " coefficients avec " +
                  windowPhrase(r.window) + ", plus que " + grouped(kMaximumFirTaps) +
                  " : élargissez les transitions, ou prenez l'équiondulation.";
        return;
    }
    const std::string key = templateKey(r);
    std::size_t taps = rememberedTaps(key);
    if (taps == 0)
    {
        std::map<std::size_t, bool> tried;
        auto holds = [&](std::size_t n) {
            const auto known = tried.find(n);
            if (known != tried.end()) return known->second;
            FilterDesign trial;
            trial.recipe = r;
            trial.recipe.taps = n;
            designWindowInto(trial);
            const bool ok = trial.ok() && measureTemplate(trial).held;
            tried[n] = ok;
            return ok;
        };
        taps = leastTaps(holds, static_cast<std::size_t>(std::ceil(std::clamp(estimate, 3.0, 1e9))), odd, kMaximumFirTaps);
        if (taps == 0)
        {
            d.error = "Ce gabarit demande plus de " + grouped(kMaximumFirTaps) + " coefficients avec cette fenêtre : "
                      "élargissez les transitions, ou prenez l'équiondulation.";
            return;
        }
        rememberedTaps(key, taps);
    }
    r.taps = taps;
    designWindowInto(d);
}

// Equiripple over the bands of the template themselves (weights from δp and δs): the least taps that hold it, from
// Kaiser's estimate.
void designEquirippleFromTemplate(FilterDesign &d, const dsp::FilterTemplate &t)
{
    FilterRecipe &r = d.recipe;
    const double fs = r.sampleRateHz;
    const double deltaPass = passDeviation(t.passbandLossDb);
    const double deltaStop = std::pow(10.0, -t.stopbandAttenuationDb / 20.0);
    const double transition = narrowestTransition(t);
    const std::array<double, 2> middles = transitionMiddles(t);
    r.frequency1Hz = middles[0];
    if (band(r.response)) r.frequency2Hz = middles[1];
    r.transitionHz = transition;
    // Beyond what double precision tells apart, the exchange no longer converges.
    if (-20.0 * std::log10(std::min(deltaPass, deltaStop)) > kEquirippleReachDb - 10.0)
    {
        d.error = "L'équiondulation ne tient pas plus de " + quantity(kEquirippleReachDb - 10.0, "dB") +
                  " ici (la précision des calculs) : prenez la fenêtre de Kaiser.";
        return;
    }
    // The bands in ascending order, as Parks-McClellan wants them; the stop bands weighted by δp/δs.
    dsp::RemezSpec spec;
    spec.sampleRateHz = fs;
    const double nyquist = 0.5 * fs, stopWeight = deltaPass / deltaStop;
    const auto add = [&spec](double low, double high, bool pass, double weight) {
        spec.bandsHz.push_back(low);
        spec.bandsHz.push_back(high);
        spec.desired.push_back(pass ? 1.0 : 0.0);
        spec.weights.push_back(weight);
    };
    const double p0 = r.passEdge1Hz, p1 = r.passEdge2Hz, s0 = r.stopEdge1Hz, s1 = r.stopEdge2Hz;
    switch (r.response)
    {
        case dsp::FilterResponse::LowPass:
            add(0.0, p0, true, 1.0);
            add(s0, nyquist, false, stopWeight);
            break;
        case dsp::FilterResponse::HighPass:
            add(0.0, s0, false, stopWeight);
            add(p0, nyquist, true, 1.0);
            break;
        case dsp::FilterResponse::BandPass:
            add(0.0, s0, false, stopWeight);
            add(p0, p1, true, 1.0);
            add(s1, nyquist, false, stopWeight);
            break;
        case dsp::FilterResponse::BandStop:
            add(0.0, p0, true, 1.0);
            add(s0, s1, false, stopWeight);
            add(p1, nyquist, true, 1.0);
            break;
    }
    spec.maximumIterations = kEquirippleIterations;
    const double estimate = equirippleEstimate(deltaPass, deltaStop, transition, fs);
    d.estimatedTaps = static_cast<std::size_t>(std::ceil(std::clamp(estimate, 3.0, 1e9)));
    const bool odd = r.response == dsp::FilterResponse::HighPass || r.response == dsp::FilterResponse::BandStop;
    if (estimate > 1.25 * static_cast<double>(kMaximumEquirippleTaps))
    {
        d.error = "Ce gabarit demande environ " + grouped(d.estimatedTaps) + " coefficients en équiondulation, plus que " +
                  grouped(kMaximumEquirippleTaps) + " : élargissez les transitions, ou prenez la fenêtre de Kaiser.";
        return;
    }
    const auto remez = [&spec](std::size_t n) {
        dsp::RemezSpec s = spec;
        s.taps = n;
        return dsp::designRemez(s);
    };
    const auto designed = [&](std::size_t n, const dsp::RemezDesign &e) {
        FilterDesign trial;
        trial.recipe = r;
        trial.recipe.taps = n;
        trial.taps = e.taps;
        return trial;
    };
    const std::string key = templateKey(r);
    std::size_t taps = rememberedTaps(key);
    if (taps == 0)
    {
        std::map<std::size_t, TemplateMeasure> tried;
        const auto check = [&](std::size_t n) {
            const auto known = tried.find(n);
            if (known != tried.end()) return known->second;
            const dsp::RemezDesign e = remez(n);
            TemplateMeasure c;
            c.excessDb = std::numeric_limits<double>::infinity();
            if (e.ok()) c = measureTemplate(designed(n, e));
            tried[n] = c;
            return c;
        };
        auto holds = [&](std::size_t n) { return check(n).held; };
        // A long filter takes a while to design: the estimate first corrected by what it misses (or has to
        // spare), at about 14,6·Δf/fs dB a tap (Kaiser), a few times, before the search near the boundary.
        const double perTap = 14.6 * transition / fs;
        std::size_t start = std::clamp<std::size_t>(d.estimatedTaps | 1u, 3, kMaximumEquirippleTaps);
        for (int attempt = 0; attempt < 3 && perTap > 0.0; ++attempt)
        {
            const double excess = check(start).excessDb;
            if (!std::isfinite(excess)) break;
            const double move = std::round(excess / perTap / 2.0) * 2.0;
            if (std::abs(move) < 2.0) break;
            start = static_cast<std::size_t>(std::clamp(static_cast<double>(start) + move, 3.0,
                                                        static_cast<double>(kMaximumEquirippleTaps)));
            start |= 1u;
        }
        taps = leastTaps(holds, start, odd, kMaximumEquirippleTaps);
        if (taps == 0)
        {
            d.error = "Ce gabarit demande plus de " + grouped(kMaximumEquirippleTaps) +
                      " coefficients en équiondulation (environ " + grouped(d.estimatedTaps) +
                      ") : élargissez les transitions, ou prenez la fenêtre de Kaiser.";
            return;
        }
        rememberedTaps(key, taps);
    }
    const dsp::RemezDesign e = remez(taps);
    if (!e.ok())
    {
        d.error = e.error;
        return;
    }
    r.taps = taps;
    d.taps = e.taps;
    d.iterations = e.iterations;
    d.exchangeNote = e.stopped;
    d.computedTaps = e.taps.size();
}

void designFromTemplate(FilterDesign &d)
{
    FilterRecipe &r = d.recipe;
    const dsp::FilterTemplate t = filterTemplate(r);
    d.error = dsp::templateError(t);
    if (!d.error.empty()) return;
    d.templateOrders = orders(t);
    if (r.kind == FilterKind::Iir)
        designIirFromTemplate(d, t);
    else if (r.firMethod == FirMethod::Equiripple)
        designEquirippleFromTemplate(d, t);
    else
        designWindowFromTemplate(d, t);
    if (!d.ok()) return;
    firRoots(d);
    const TemplateMeasure c = measureTemplate(d);
    d.templateLossDb = c.lossDb;
    d.templateAttenuationDb = c.attenuationDb;
    d.templateHeld = c.held;
    if (r.kind == FilterKind::Fir && r.firMethod == FirMethod::Equiripple)
    {
        d.achievedRippleDb = c.lossDb;
        d.achievedAttenuationDb = c.attenuationDb;
    }
    if (!c.held)
        d.warnings.push_back("Le gabarit n'est pas tout à fait tenu : " + quantity(c.lossDb, "dB") +
                             " dans la bande passante (au plus " + quantity(r.passbandRippleDb, "dB") + "), " +
                             quantity(c.attenuationDb, "dB") + " dans la bande coupée (au moins " +
                             quantity(r.stopbandAttenuationDb, "dB") + ").");
}
} // namespace

ImpulseResponse impulseResponse(const FilterDesign &design)
{
    ImpulseResponse out;
    if (!design.ok()) return out;
    if (design.fir())
    {
        out.impulse = design.taps;
        // A few zeros after: the end of the response shows.
        out.impulse.resize(design.taps.size() + std::max<std::size_t>(4, design.taps.size() / 10), 0.0);
    }
    else
    {
        // In blocks, an impulse then zeros, until it has died out: below 1e-9 of its peak over a whole block.
        // Unstable (coefficients typed): only while it grows by a million, 64 to 4 096 samples.
        constexpr std::size_t kBlock = 4096;
        const double radius = design.largestPoleRadius();
        const std::size_t longest =
            radius > 1.0 + 1e-9
                ? std::clamp<std::size_t>(static_cast<std::size_t>(std::ceil(std::log(1e6) / std::log(radius))), 64, kBlock)
                : kMaximumResponseLength;
        dsp::SosState state(design.sections.size(), std::array<double, 2>{0.0, 0.0});
        std::vector<double> input(kBlock, 0.0);
        input[0] = 1.0;
        input.resize(std::min(kBlock, longest));
        double peak = 0.0;
        out.settled = false;
        while (out.impulse.size() < longest)
        {
            const std::vector<double> y = dsp::sosFilter(design.sections, input, &state);
            input[0] = 0.0;
            double blockPeak = 0.0;
            for (double v : y) blockPeak = std::max(blockPeak, std::abs(v));
            peak = std::max(peak, blockPeak);
            out.impulse.insert(out.impulse.end(), y.begin(), y.end());
            if (out.impulse.size() > kBlock && blockPeak <= 1e-9 * peak)
            {
                out.settled = true;
                break;
            }
        }
        // Cut where it is no longer seen (1e-5 of the peak), with a tenth more.
        std::size_t last = 0;
        for (std::size_t i = 0; i < out.impulse.size(); ++i)
            if (std::abs(out.impulse[i]) >= 1e-5 * peak) last = i;
        if (out.settled)
        {
            const std::size_t keep = std::min(out.impulse.size(), std::max<std::size_t>(32, last + 1 + (last + 1) / 10));
            out.impulse.resize(keep);
        }
    }
    if (design.gain != 1.0)
        for (double &v : out.impulse) v *= design.gain;
    out.step = runningSum(out.impulse);
    if (design.recipe.application == FilterApplication::Causal || out.impulse.empty() || !design.applicationError.empty())
        return out;

    // As applied: an impulse in the middle of zeros long enough for the response either side, run as a record
    // (an IIR, twice its length: what was cut of its tail is then far below what is kept).
    const std::size_t half = (design.fir() ? 1 : 2) * out.impulse.size();
    std::vector<double> x(2 * half + 1, 0.0);
    x[half] = 1.0;
    const std::vector<double> y = runFilter(design, x);
    double peak = 0.0;
    for (double v : y) peak = std::max(peak, std::abs(v));
    // A FIR's response ends where its taps do; an IIR's, where it is no longer seen (1e-5 of its peak).
    const double threshold = design.fir() ? 0.0 : 1e-5 * peak;
    std::size_t first = y.size(), last = 0;
    for (std::size_t i = 0; i < y.size(); ++i)
    {
        if (std::abs(y[i]) > threshold)
        {
            first = std::min(first, i);
            last = i;
        }
    }
    if (first > last) return out;
    const std::size_t margin = std::max<std::size_t>(4, (last - first + 1) / 10);
    first = first > margin ? first - margin : 0;
    last = std::min(y.size() - 1, last + margin);
    out.appliedImpulse.assign(y.begin() + static_cast<std::ptrdiff_t>(first),
                              y.begin() + static_cast<std::ptrdiff_t>(last + 1));
    out.appliedStep = runningSum(out.appliedImpulse);
    out.appliedStart = static_cast<long>(first) - static_cast<long>(half);
    return out;
}

namespace
{
// "RIF de 8 coefficients", "RII d'ordre 2 (3 b, 3 a)", as typed (a[0] divides them, the zeros at the end left out).
std::string typedText(const FilterRecipe &r)
{
    const TypedCoefficients t = evaluateTypedCoefficients(r);
    std::string text = "Coefficients tapés";
    if (!t.error.empty() || t.denominator.empty() || t.denominator.front() == 0.0) return text;
    std::size_t nb = t.numerator.size(), na = t.denominator.size();
    while (nb > 1 && t.numerator[nb - 1] == 0.0) --nb;
    while (na > 1 && t.denominator[na - 1] == 0.0) --na;
    if (na == 1) return text + ", RIF de " + grouped(nb) + (nb > 1 ? " coefficients" : " coefficient");
    return text + ", RII d'ordre " + std::to_string(std::max(nb, na) - 1) + " (" + std::to_string(nb) + " b, " +
           std::to_string(na) + " a)";
}
} // namespace

namespace
{
// Where the template wants the pass and the stop bands: "jusqu'à 1 kHz", "de 1 kHz à 2 kHz", "jusqu'à 500 Hz et
// dès 4 kHz"…
std::pair<std::string, std::string> templatePlaces(const FilterRecipe &r)
{
    const auto hz = [](double f) { return quantity(f, "Hz"); };
    const double p0 = r.passEdge1Hz, p1 = r.passEdge2Hz, s0 = r.stopEdge1Hz, s1 = r.stopEdge2Hz;
    switch (r.response)
    {
        case dsp::FilterResponse::LowPass: return {"jusqu'à " + hz(p0), "dès " + hz(s0)};
        case dsp::FilterResponse::HighPass: return {"dès " + hz(p0), "jusqu'à " + hz(s0)};
        case dsp::FilterResponse::BandPass: return {"de " + hz(p0) + " à " + hz(p1), "jusqu'à " + hz(s0) + " et dès " + hz(s1)};
        case dsp::FilterResponse::BandStop: return {"jusqu'à " + hz(p0) + " et dès " + hz(p1), "de " + hz(s0) + " à " + hz(s1)};
    }
    return {};
}
} // namespace

std::string templateText(const FilterRecipe &r)
{
    const auto [pass, stop] = templatePlaces(r);
    return "Gabarit : au plus " + quantity(r.passbandRippleDb, "dB") +
           (r.kind == FilterKind::Fir ? " d'ondulation (crête à crête) " : " d'atténuation ") + pass + ", au moins " +
           quantity(r.stopbandAttenuationDb, "dB") + " d'atténuation " + stop + ".";
}

std::string filterText(const FilterRecipe &r)
{
    if (r.kind == FilterKind::None) return "Aucun filtre" + (r.gainDb != 0.0 ? ", gain " + filterGainText(r.gainDb) : std::string());
    if (r.kind == FilterKind::Typed)
        return typedText(r) + (r.gainDb != 0.0 ? ", gain " + filterGainText(r.gainDb) : std::string());
    const std::string gain = r.gainDb != 0.0 ? ", gain " + filterGainText(r.gainDb) : std::string();
    std::string text = std::string(dsp::responseName(r.response));
    if (r.fromTemplate)
    {
        // The order computed, then the template: what the ripple and the attenuation ask is in it.
        if (r.kind == FilterKind::Iir)
            text += " " + std::string(dsp::familyName(r.family)) + " d'ordre " + std::to_string(r.order);
        else if (r.firMethod == FirMethod::Equiripple)
            text += " RIF équiondulation de " + grouped(r.taps) + " coefficients";
        else
            text += " RIF de " + grouped(r.taps) + " coefficients (" + std::string(dsp::windowName(r.window)) +
                    (r.window == dsp::Window::Kaiser ? ", β = " + quantity(r.kaiserBeta, "", 3) : std::string()) + ")";
        const auto [pass, stop] = templatePlaces(r);
        return text + ", gabarit " + quantity(r.passbandRippleDb, "dB") + " " + pass + ", " +
               quantity(r.stopbandAttenuationDb, "dB") + " " + stop + gain;
    }
    if (r.kind == FilterKind::Iir)
    {
        text += " " + std::string(dsp::familyName(r.family)) + " d'ordre " + std::to_string(r.order);
        if (r.family == dsp::FilterFamily::ChebyshevI) text += " (" + quantity(r.passbandRippleDb, "dB") + ")";
        if (r.family == dsp::FilterFamily::ChebyshevII) text += " (" + quantity(r.stopbandAttenuationDb, "dB") + ")";
        if (r.family == dsp::FilterFamily::Elliptic)
            text += " (" + quantity(r.passbandRippleDb, "dB") + ", " + quantity(r.stopbandAttenuationDb, "dB") + ")";
    }
    else if (r.firMethod == FirMethod::Equiripple)
    {
        text += " RIF équiondulation de " + grouped(r.taps) + " coefficients (" +
                (band(r.response) ? "transitions de " : "transition de ") + quantity(r.transitionHz, "Hz") + ")";
    }
    else
    {
        text += " RIF de " + grouped(r.taps) + " coefficients (" + std::string(dsp::windowName(r.window));
        if (r.window == dsp::Window::Kaiser) text += ", β = " + quantity(r.kaiserBeta, "", 3);
        text += ")";
    }
    return text + ", " + edgesText(r) + gain;
}

std::string filterGainText(double gainDb)
{
    return (gainDb > 0.0 ? "+" : "") + quantity(gainDb, "dB");
}

std::string filterShortText(const FilterRecipe &r)
{
    if (r.kind == FilterKind::None) return "sans filtre" + (r.gainDb != 0.0 ? ", " + filterGainText(r.gainDb) : std::string());
    if (r.kind == FilterKind::Typed)
        return "coefficients tapés" + (r.gainDb != 0.0 ? ", " + filterGainText(r.gainDb) : std::string());
    std::string edges = edgesText(r);
    if (r.fromTemplate)
    {
        // The band it is for: what passes, or what a band-stop takes out.
        switch (r.response)
        {
            case dsp::FilterResponse::LowPass:
            case dsp::FilterResponse::HighPass: edges = quantity(r.passEdge1Hz, "Hz"); break;
            case dsp::FilterResponse::BandPass: edges = quantity(r.passEdge1Hz, "Hz") + " à " + quantity(r.passEdge2Hz, "Hz"); break;
            case dsp::FilterResponse::BandStop: edges = quantity(r.stopEdge1Hz, "Hz") + " à " + quantity(r.stopEdge2Hz, "Hz"); break;
        }
    }
    return lower(dsp::responseName(r.response)) + " " + edges + (r.gainDb != 0.0 ? ", " + filterGainText(r.gainDb) : std::string());
}

std::string applicationText(FilterApplication application)
{
    switch (application)
    {
        case FilterApplication::Causal: return "Causal (comme en temps réel)";
        case FilterApplication::ZeroPhase: return "Aller-retour à phase nulle (gain au carré)";
        case FilterApplication::Aligned: return "Retard du RIF retiré";
    }
    return "?";
}

std::vector<std::string> designSummary(const FilterDesign &d)
{
    std::vector<std::string> lines;
    if (!d.ok()) return lines;
    if (d.recipe.kind == FilterKind::None)
    {
        lines.push_back("Aucun filtre : la sortie est l'entrée (H(z) = 1), sans retard ni déphasage" +
                        (d.recipe.gainDb != 0.0 ? ", multipliée par le gain en sortie (" + filterGainText(d.recipe.gainDb) + ")."
                                                : std::string(".")));
        lines.push_back("Pour en concevoir un : ruban ▸ Filtre, un RII (Butterworth, Chebyshev, Bessel, elliptique), un RIF ou des "
                        "coefficients tapés ; ou un exemple (Recette ▸ Exemples). Le premier choisi part d'un passe-bas à " +
                        quantity(d.recipe.frequency1Hz, "Hz") + ".");
        return lines;
    }
    const double fs = d.recipe.sampleRateHz;
    const bool typed = d.recipe.kind == FilterKind::Typed;
    // Where it passes (coefficients typed: no response said): at 0 Hz if it passes there (within 3 dB of its
    // highest gain), else where its gain is highest, on 1 024 frequencies up to fs/2.
    const auto peakFrequency = [&d, fs] {
        double best = -1.0, at = 0.0;
        for (int i = 0; i <= 1024; ++i)
        {
            const double f = 0.5 * fs * static_cast<double>(i) / 1024.0;
            const double h = std::abs(d.unitResponse(f));
            if (std::isfinite(h) && h > best)
            {
                best = h;
                at = f;
            }
        }
        const double dc = std::abs(d.unitResponse(0.0));
        return std::isfinite(dc) && dc >= std::sqrt(0.5) * best ? 0.0 : at;
    };
    const auto delayAt = [&d, &lines, fs](double f) {
        const double delay = d.groupDelay(f);
        if (std::isfinite(delay))
            lines.push_back("Temps de propagation de groupe à " + quantity(f, "Hz") + " : " +
                            plot::formatCompact(delay, "", 4) + " échantillons (" + quantity(delay / fs, "s") + ").");
    };
    // From a template: the template, the order it asked, what it holds, what the other designs would need.
    const bool fromTemplate = d.recipe.fromTemplate && !typed;
    if (fromTemplate)
    {
        const FilterRecipe &r = d.recipe;
        lines.push_back(templateText(r));
        if (r.kind == FilterKind::Iir)
            lines.push_back("Ordre " + std::to_string(r.order) + ", le plus petit qui le tient" +
                            (d.exactOrder > 0.0 ? " (le calcul donne " + plot::formatCompact(d.exactOrder, "", 4) + ")" : std::string()) +
                            " ; fréquences de la conception : " + edgesText(r) + ".");
        else if (r.firMethod == FirMethod::Equiripple)
            lines.push_back(grouped(r.taps) + " coefficients, le plus petit nombre qui le tient en équiondulation (Parks-McClellan, " +
                            std::to_string(d.iterations) + (d.iterations > 1 ? " itérations" : " itération") +
                            " ; l'estimation de Kaiser en donnait " + grouped(d.estimatedTaps) + ").");
        else
            lines.push_back(grouped(r.taps) + " coefficients, le plus petit nombre qui le tient avec " + windowPhrase(r.window) +
                            (r.window == dsp::Window::Kaiser ? " (β = " + quantity(r.kaiserBeta, "", 4) + ")" : std::string()) +
                            " ; coupures au milieu des transitions : " + edgesText(r) + ".");
        if (d.templateHeld)
        {
            const std::string attenuation = d.templateAttenuationDb > 250.0 ? "plus de 250 dB" : quantity(d.templateAttenuationDb, "dB");
            lines.push_back("Tenu : " + quantity(d.templateLossDb, "dB") + " dans la bande passante (au plus " +
                            quantity(r.passbandRippleDb, "dB") + "), " + attenuation + " dans la bande coupée (au moins " +
                            quantity(r.stopbandAttenuationDb, "dB") + ").");
        }
        const TemplateOrders &o = d.templateOrders;
        lines.push_back("Pour ce gabarit : en RII, " + iirOrdersText(o) + " ; en RIF, environ " + grouped(o.equirippleTaps) +
                        " coefficients en équiondulation, " + grouped(o.kaiserTaps) + " avec la fenêtre de Kaiser.");
    }
    if (typed && !d.typedDenominator.empty() && d.typedDenominator.front() != 1.0)
        lines.push_back("Coefficients divisés par a[0] = " + plot::formatCompact(d.typedDenominator.front(), "", 6) +
                        " : y[n] a le coefficient 1.");
    if (d.fir() && typed && d.taps.size() == 1)
    {
        lines.push_back("Coefficients tapés : un seul, b[0] = " + plot::formatCompact(d.taps[0], "", 6) +
                        " : un gain, sans retard.");
    }
    else if (d.fir() && typed && symmetryOf(d.taps) == Symmetry::None)
    {
        lines.push_back("Coefficients tapés : RIF de " + grouped(d.taps.size()) +
                        " coefficients, non symétriques : phase non linéaire.");
        delayAt(peakFrequency());
    }
    else if (d.fir())
    {
        const double delay = 0.5 * static_cast<double>(d.taps.size() - 1);
        const char *symmetric = symmetryOf(d.taps) == Symmetry::Odd ? " coefficients antisymétriques" : " coefficients symétriques";
        lines.push_back((typed ? "Coefficients tapés : RIF de " : "") + grouped(d.taps.size()) +
                        (typed ? symmetric : " coefficients") + ", phase linéaire : retard de " +
                        plot::formatCompact(delay, "", 6) + " échantillons (" + quantity(delay / fs, "s") + ").");
        if (d.recipe.firMethod == FirMethod::Equiripple && fromTemplate)
        {
            if (!d.exchangeNote.empty())
                lines.push_back("L'échange s'est arrêté avant la fin (" + d.exchangeNote +
                                ") : ce qui est tenu ci-dessus est mesuré.");
        }
        else if (d.recipe.firMethod == FirMethod::Equiripple)
        {
            // Beyond what double precision tells apart, the attenuation is no longer a number to read.
            const std::string attenuation = d.achievedAttenuationDb > 250.0 ? "plus de 250 dB"
                                                                            : quantity(d.achievedAttenuationDb, "dB");
            lines.push_back("Équiondulation (Parks-McClellan, " + std::to_string(d.iterations) +
                            (d.iterations > 1 ? " itérations" : " itération") + ") : ondulation de " +
                            quantity(d.achievedRippleDb, "dB") + " en bande passante, atténuation de " + attenuation +
                            " en bande coupée (demandé : " + quantity(d.recipe.passbandRippleDb, "dB") + " et " +
                            quantity(d.recipe.stopbandAttenuationDb, "dB") + ").");
            if (d.estimatedTaps > 0)
                lines.push_back("Ce gabarit demande environ " + grouped(d.estimatedTaps) +
                                " coefficients (estimation de Kaiser).");
            if (d.computedTaps > 0 && d.computedTaps < d.taps.size())
                lines.push_back("Calculés sur les " + grouped(d.computedTaps) + " coefficients du milieu, les " +
                                grouped((d.taps.size() - d.computedTaps) / 2) +
                                " de chaque bout valant 0 : avec tous, l'atténuation dépasserait " +
                                quantity(kEquirippleReachDb, "dB") + ", au-delà de la précision des calculs.");
            if (!d.exchangeNote.empty())
                lines.push_back("L'échange s'est arrêté avant la fin (" + d.exchangeNote +
                                ") : l'ondulation et l'atténuation ci-dessus sont mesurées.");
        }
    }
    else if (typed)
    {
        const double radius = d.largestPoleRadius();
        const std::string stability =
            radius > 1.0 + 1e-9    ? "instable : un pôle à |z| = " + plot::formatFixed(radius, 5) + ", hors du cercle unité."
            : radius >= 1.0 - 1e-9 ? "à la limite de la stabilité : un pôle sur le cercle unité."
                                   : "stable : pôle le plus proche du cercle unité à " + plot::formatFixed(radius, 5) + ".";
        lines.push_back(typedText(d.recipe) + ", en " + std::to_string(d.sections.size()) +
                        (d.sections.size() > 1 ? " sections" : " section") + " du second ordre ; " + stability);
        if (radius < 1.0 - 1e-9) delayAt(peakFrequency());
    }
    else
    {
        const std::size_t poles = d.poles.size();
        lines.push_back(std::to_string(d.sections.size()) + (d.sections.size() > 1 ? " sections" : " section") +
                        " du second ordre (" + std::to_string(poles) + (poles > 1 ? " pôles" : " pôle") +
                        "), stable : pôle le plus proche du cercle unité à " +
                        plot::formatFixed(d.largestPoleRadius(), 5) + ".");
        // The delay where it passes: at DC for a low-pass or band-stop, at the centre of a band, at fs/2 for
        // a high-pass (approximately where its gain is highest).
        double f = 0.0;
        switch (d.recipe.response)
        {
            case dsp::FilterResponse::LowPass:
            case dsp::FilterResponse::BandStop: f = 0.0; break;
            case dsp::FilterResponse::HighPass: f = 0.45 * fs; break;
            case dsp::FilterResponse::BandPass: f = std::sqrt(d.recipe.frequency1Hz * d.recipe.frequency2Hz); break;
        }
        const double delay = d.groupDelay(f);
        if (std::isfinite(delay))
            lines.push_back("Temps de propagation de groupe à " + quantity(f, "Hz") + " : " +
                            plot::formatCompact(delay, "", 4) + " échantillons (" + quantity(delay / fs, "s") + ").");
        const std::vector<double> &e = d.stopEdgesHz;
        const std::string least = "Atténuation d'au moins " + quantity(d.recipe.stopbandAttenuationDb, "dB");
        if (e.size() == 1)
            lines.push_back(least + (d.recipe.response == dsp::FilterResponse::HighPass ? " en deçà de " : " au-delà de ") +
                            quantity(e[0], "Hz") + ".");
        else if (e.size() == 2 && d.recipe.response == dsp::FilterResponse::BandPass)
            lines.push_back(least + " en deçà de " + quantity(e[0], "Hz") + " et au-delà de " + quantity(e[1], "Hz") + ".");
        else if (e.size() == 2)
            lines.push_back(least + " de " + quantity(e[0], "Hz") + " à " + quantity(e[1], "Hz") + ".");
    }
    if (d.recipe.gainDb != 0.0)
        lines.push_back("Gain en sortie de " + filterGainText(d.recipe.gainDb) + " (×" + plot::formatCompact(d.gain, "", 4) +
                        ") : le signal filtré est multiplié par ce facteur, une fois quelle que soit l'application.");
    if (!d.applicationError.empty())
    {
        lines.push_back(d.applicationError);
        return lines;
    }
    switch (d.recipe.application)
    {
        case FilterApplication::Causal: lines.push_back("Appliqué causalement : ce retard reste dans le signal filtré."); break;
        case FilterApplication::ZeroPhase:
            lines.push_back("Appliqué aller-retour : aucun retard, mais le gain au carré (deux fois l'atténuation en dB : "
                            "−6 dB à une coupure à −3 dB).");
            lines.push_back(edgeExtensionText(d.recipe.edges));
            break;
        case FilterApplication::Aligned: lines.push_back("Appliqué en retirant le retard : le gain du RIF, sans retard."); break;
    }
    return lines;
}

namespace
{
std::vector<double> runUnit(const FilterDesign &d, std::span<const double> x)
{
    if (d.fir())
    {
        switch (d.recipe.application)
        {
            case FilterApplication::ZeroPhase: return dsp::firFiltFilt(d.taps, x, -1, d.recipe.edges);
            case FilterApplication::Aligned: return dsp::firFilterAligned(d.taps, x);
            case FilterApplication::Causal: break;
        }
        return dsp::firFilter(d.taps, x);
    }
    if (d.recipe.application == FilterApplication::ZeroPhase) return dsp::sosFiltFilt(d.sections, x, -1, d.recipe.edges);
    return dsp::sosFilter(d.sections, x);
}
} // namespace

std::vector<double> runFilter(const FilterDesign &d, std::span<const double> x)
{
    if (x.empty() || !d.ok()) return {};
    std::vector<double> y = runUnit(d, x);
    // The gain once, after the filter (forward and backward, at zero phase).
    if (d.gain != 1.0)
        for (double &v : y) v *= d.gain;
    return y;
}

FilteredSignal applyFilter(const FilterDesign &d, const model::Signal &source, const std::vector<std::size_t> &channelsIn,
                           std::size_t firstFrames, const model::Progress &progress)
{
    FilteredSignal out;
    if (!d.ok())
    {
        out.error = d.error.empty() ? std::string("Aucun filtre n'est conçu.") : d.error;
        return out;
    }
    if (!source.data || source.frameCount() == 0)
    {
        out.error = "« " + source.name + " » est vide.";
        return out;
    }
    if (!d.applicationError.empty())
    {
        out.error = d.applicationError;
        return out;
    }
    const double fs = d.recipe.sampleRateHz;
    if (!(source.sampleRateHz > 0.0) || std::abs(source.sampleRateHz - fs) > 1e-9 * fs)
    {
        out.error = "Le filtre est conçu pour fs = " + quantity(fs, "Hz", 6) + " et « " + source.name +
                    " » est échantillonné à " + quantity(source.sampleRateHz, "Hz", 6) +
                    " : il ne ferait pas ce que montrent ses réponses (choisir « fs du signal » dans le panneau Filtre).";
        return out;
    }
    std::vector<std::size_t> channels = channelsIn;
    if (channels.empty())
        for (std::size_t c = 0; c < source.channelCount(); ++c) channels.push_back(c);
    for (std::size_t c : channels)
    {
        if (c < source.channelCount()) continue;
        out.error = "Voie " + std::to_string(c + 1) + " inexistante dans « " + source.name + " ».";
        return out;
    }
    const std::size_t total = source.frameCount();
    const std::size_t n = firstFrames > 0 ? std::min(firstFrames, total) : total;

    auto data = std::make_shared<model::SampleData>();
    model::Signal &s = out.signal;
    for (std::size_t i = 0; i < channels.size(); ++i)
    {
        if (progress.stopped())
        {
            out.cancelled = true;
            out.signal = {};
            return out;
        }
        const std::size_t c = channels[i];
        data->channels.push_back(runFilter(d, source.channel(c).first(n)));
        s.channels.push_back(source.channels[c]);
        progress.report(i + 1, channels.size());
    }
    for (std::size_t k : source.data->discontinuities)
        if (k < n) data->discontinuities.push_back(k);
    // In zero phase, as far from each end as the filter rings, the result depends on what the signal did beyond
    // the record, which the extension can only guess: said when that is a tenth of the record or more (how long
    // it rings, even far longer than the record).
    if (d.recipe.application == FilterApplication::ZeroPhase)
    {
        const std::size_t ring = d.fir() ? d.taps.size() : dsp::sosRingLength(d.sections, 1e-2, std::numeric_limits<std::size_t>::max() / 16);
        if (10 * ring >= n)
        {
            std::string text = "Ce filtre met environ " + quantity(static_cast<double>(ring) / fs, "s", 2) +
                               " à se taire (sa réponse impulsionnelle retombe à 1 %)";
            if (ring >= n)
                text += ", plus que les " + quantity(static_cast<double>(n) / fs, "s", 2) + " du signal : tout le résultat";
            else if (2 * ring >= n)
                text += " : à moins de cette durée de chaque bord, soit tout le signal, le résultat";
            else
                text += " : à moins de cette durée de chaque bord, soit " +
                        std::to_string(std::lround(200.0 * static_cast<double>(ring) / static_cast<double>(n))) +
                        " % du signal, le résultat";
            text += " dépend du prolongement des bords, donc de ce que le signal faisait avant et après, que "
                    "l'enregistrement ne dit pas.";
            // Prolonged for far less time than the filter rings (SciPy's few samples, or not at all): what is seen
            // near the ends is the filter starting, a narrow band hardly started anywhere in a short record.
            const std::size_t pad = d.fir() ? dsp::firFiltFiltPad(d.taps.size(), n, d.recipe.edges)
                                            : dsp::sosFiltFiltPad(d.sections, n, d.recipe.edges);
            if (d.recipe.edges != dsp::EdgeExtension::Predicted && 2 * pad < ring)
                text += (pad == 0 ? std::string(" Les bords ne sont pas prolongés")
                                  : " Les bords ne sont prolongés que de " + grouped(pad) + " échantillons (comme SciPy)") +
                        " : la mise en route du filtre s'y voit ; « Prédits » les prolonge aussi longtemps qu'il sonne.";
            out.warnings.push_back(text);
        }
    }
    if (!data->discontinuities.empty())
        out.warnings.push_back("« " + source.name + " » a des trous (échantillons manquants) : le filtre passe par-dessus "
                               "comme si les échantillons se suivaient.");

    s.name = source.name + " · " + filterShortText(d.recipe);
    s.origin = model::Origin::Derived;
    s.representation = source.representation;
    s.sampleRateHz = source.sampleRateHz;
    s.startSeconds = source.startSeconds;
    s.data = std::move(data);
    std::string notes = filterText(d.recipe) + ", appliqué à « " + source.name + " » : " + lower(applicationText(d.recipe.application)) + ".";
    for (const std::string &line : designSummary(d)) notes += "\n" + line;
    if (n < total) notes += "\nLes " + grouped(n) + " premiers échantillons seulement.";
    s.notes = notes;

    model::Json parameters = toJson(d.recipe);
    model::Json list = model::Json::array();
    for (std::size_t c : channels) list.push_back(static_cast<unsigned long long>(c));
    parameters["channels"] = list;
    s.provenance = model::Json::object();
    s.provenance["operation"] = "filter";
    s.provenance["source"] = source.name;
    s.provenance["sourceId"] = static_cast<unsigned long long>(source.id);
    s.provenance["parameters"] = parameters;
    return out;
}

std::string_view filterKindKey(FilterKind kind) noexcept
{
    switch (kind)
    {
        case FilterKind::Fir: return "fir";
        case FilterKind::Typed: return "typed";
        case FilterKind::None: return "none";
        case FilterKind::Iir: break;
    }
    return "iir";
}

std::string_view firMethodKey(FirMethod method) noexcept
{
    return method == FirMethod::Equiripple ? "equiripple" : "window";
}

std::string_view filterFamilyKey(dsp::FilterFamily family) noexcept
{
    switch (family)
    {
        case dsp::FilterFamily::Butterworth: return "butterworth";
        case dsp::FilterFamily::ChebyshevI: return "chebyshev1";
        case dsp::FilterFamily::ChebyshevII: return "chebyshev2";
        case dsp::FilterFamily::Bessel: return "bessel";
        case dsp::FilterFamily::Elliptic: return "elliptic";
    }
    return "butterworth";
}

std::string_view filterResponseKey(dsp::FilterResponse response) noexcept
{
    switch (response)
    {
        case dsp::FilterResponse::LowPass: return "lowpass";
        case dsp::FilterResponse::HighPass: return "highpass";
        case dsp::FilterResponse::BandPass: return "bandpass";
        case dsp::FilterResponse::BandStop: return "bandstop";
    }
    return "lowpass";
}

std::string_view edgeExtensionKey(dsp::EdgeExtension extension) noexcept
{
    switch (extension)
    {
        case dsp::EdgeExtension::Predicted: return "predicted";
        case dsp::EdgeExtension::Odd: return "odd";
        case dsp::EdgeExtension::Even: return "even";
        case dsp::EdgeExtension::Constant: return "constant";
        case dsp::EdgeExtension::None: return "none";
    }
    return "predicted";
}

std::string edgeExtensionText(dsp::EdgeExtension extension)
{
    switch (extension)
    {
        case dsp::EdgeExtension::Predicted:
            return "Bords prolongés par prédiction linéaire (moindres carrés sur les échantillons proches de chaque bord), "
                   "le temps que le filtre sonne : le signal continue comme il allait, sans bouffée aux bords.";
        case dsp::EdgeExtension::Odd:
            return "Bords prolongés par symétrie impaire (2·x[0] − x[k]), comme filtfilt de SciPy : garde une tendance ; "
                   "un signal qui commence sur une crête y fait une bouffée.";
        case dsp::EdgeExtension::Even:
            return "Bords prolongés en miroir (x[k]) : garde une crête ; un signal qui commence en pente y fait une bouffée.";
        case dsp::EdgeExtension::Constant:
            return "Bords prolongés par le premier et le dernier échantillon répétés.";
        case dsp::EdgeExtension::None:
            return "Bords non prolongés : le filtre part du régime établi du premier échantillon ; le début et la fin "
                   "gardent leur transitoire.";
    }
    return {};
}

std::string_view filterApplicationKey(FilterApplication application) noexcept
{
    switch (application)
    {
        case FilterApplication::Causal: return "causal";
        case FilterApplication::ZeroPhase: return "zerophase";
        case FilterApplication::Aligned: return "aligned";
    }
    return "causal";
}

model::Json toJson(const FilterRecipe &r)
{
    model::Json j = model::Json::object();
    j["kind"] = filterKindKey(r.kind);
    j["family"] = filterFamilyKey(r.family);
    j["response"] = filterResponseKey(r.response);
    j["order"] = r.order;
    j["taps"] = static_cast<unsigned long long>(r.taps);
    j["firDesign"] = firMethodKey(r.firMethod);
    j["transition"] = r.transitionHz;
    j["window"] = windowKey(r.window);
    j["kaiserBeta"] = r.kaiserBeta;
    j["sampleRate"] = r.sampleRateHz;
    j["rateOfSignal"] = r.rateOfSignal;
    j["frequency1"] = r.frequency1Hz;
    j["frequency2"] = r.frequency2Hz;
    j["ripple"] = r.passbandRippleDb;
    j["attenuation"] = r.stopbandAttenuationDb;
    j["application"] = filterApplicationKey(r.application);
    j["edges"] = edgeExtensionKey(r.edges);
    j["gain"] = r.gainDb;
    j["b"] = r.numerator;
    j["a"] = r.denominator;
    j["bCount"] = r.numeratorCount;
    j["aCount"] = r.denominatorCount;
    j["variables"] = r.variables;
    j["fromTemplate"] = r.fromTemplate;
    j["pass1"] = r.passEdge1Hz;
    j["pass2"] = r.passEdge2Hz;
    j["stop1"] = r.stopEdge1Hz;
    j["stop2"] = r.stopEdge2Hz;
    return j;
}

FilterRecipe filterRecipeFromJson(const model::Json &j)
{
    FilterRecipe r;
    if (!j.isObject()) return r;
    const std::string kind = j["kind"].toString();
    r.kind = kind == "fir" ? FilterKind::Fir : kind == "typed" ? FilterKind::Typed : kind == "none" ? FilterKind::None : FilterKind::Iir;
    const std::string family = j["family"].toString();
    for (dsp::FilterFamily f : {dsp::FilterFamily::Butterworth, dsp::FilterFamily::ChebyshevI, dsp::FilterFamily::ChebyshevII,
                                dsp::FilterFamily::Bessel, dsp::FilterFamily::Elliptic})
        if (family == filterFamilyKey(f)) r.family = f;
    const std::string response = j["response"].toString();
    for (dsp::FilterResponse f : {dsp::FilterResponse::LowPass, dsp::FilterResponse::HighPass, dsp::FilterResponse::BandPass,
                                  dsp::FilterResponse::BandStop})
        if (response == filterResponseKey(f)) r.response = f;
    r.order = static_cast<int>(number(j, "order", r.order, 1, dsp::kMaximumIirOrder));
    r.taps = static_cast<std::size_t>(number(j, "taps", static_cast<double>(r.taps), 3, static_cast<double>(kMaximumFirTaps)));
    r.firMethod = j["firDesign"].toString() == "equiripple" ? FirMethod::Equiripple : FirMethod::Window;
    r.transitionHz = number(j, "transition", r.transitionHz, 1e-9, 1e12);
    dsp::Window w;
    if (windowFromKey(j["window"].toString(), w)) r.window = w;
    r.kaiserBeta = number(j, "kaiserBeta", r.kaiserBeta, 0.0, 50.0);
    r.sampleRateHz = number(j, "sampleRate", r.sampleRateHz, 1e-6, 1e12);
    if (j["rateOfSignal"].isBool()) r.rateOfSignal = j["rateOfSignal"].toBool(true);
    r.frequency1Hz = number(j, "frequency1", r.frequency1Hz, 0.0, 1e12);
    r.frequency2Hz = number(j, "frequency2", r.frequency2Hz, 0.0, 1e12);
    r.passbandRippleDb = number(j, "ripple", r.passbandRippleDb, 1e-3, 40.0);
    r.stopbandAttenuationDb = number(j, "attenuation", r.stopbandAttenuationDb, 1.0, 300.0);
    const std::string application = j["application"].toString();
    for (FilterApplication a : {FilterApplication::Causal, FilterApplication::ZeroPhase, FilterApplication::Aligned})
        if (application == filterApplicationKey(a)) r.application = a;
    if (!applicationAllowed(r.kind, r.application)) r.application = FilterApplication::Causal;
    const std::string edges = j["edges"].toString();
    for (dsp::EdgeExtension e : {dsp::EdgeExtension::Predicted, dsp::EdgeExtension::Odd, dsp::EdgeExtension::Even,
                                 dsp::EdgeExtension::Constant, dsp::EdgeExtension::None})
        if (edges == edgeExtensionKey(e)) r.edges = e;
    r.gainDb = number(j, "gain", r.gainDb, -kMaximumFilterGainDb, kMaximumFilterGainDb);
    // Coefficients typed: a text; a number or a list of numbers as well ("b": [1, -2, 1]), written back exactly.
    const auto shortest = [](double v) {
        std::array<char, 32> buffer{};
        const auto end = std::to_chars(buffer.data(), buffer.data() + buffer.size(), v).ptr;
        return std::string(buffer.data(), end);
    };
    const auto text = [&j, &shortest](std::string_view key, std::string fallback) {
        const model::Json &v = j[key];
        if (v.isString()) return v.toString();
        if (v.isNumber()) return shortest(v.toNumber());
        if (v.isArray())
        {
            std::string list;
            for (const model::Json &item : v.items())
                list += (list.empty() ? "" : " ; ") + (item.isNumber() ? shortest(item.toNumber()) : item.toString());
            return list;
        }
        return fallback;
    };
    r.numerator = text("b", r.numerator);
    r.denominator = text("a", r.denominator);
    r.numeratorCount = text("bCount", r.numeratorCount);
    r.denominatorCount = text("aCount", r.denominatorCount);
    r.variables = text("variables", r.variables);
    if (j["fromTemplate"].isBool()) r.fromTemplate = j["fromTemplate"].toBool(false);
    r.passEdge1Hz = number(j, "pass1", r.passEdge1Hz, 0.0, 1e12);
    r.passEdge2Hz = number(j, "pass2", r.passEdge2Hz, 0.0, 1e12);
    r.stopEdge1Hz = number(j, "stop1", r.stopEdge1Hz, 0.0, 1e12);
    r.stopEdge2Hz = number(j, "stop2", r.stopEdge2Hz, 0.0, 1e12);
    return r;
}

FilterRecipe defaultFilterRecipe(double sampleRateHz)
{
    FilterRecipe r;
    const double fs = sampleRateHz > 0.0 && std::isfinite(sampleRateHz) ? sampleRateHz : 48000.0;
    r.sampleRateHz = fs;
    r.frequency1Hz = std::min(1000.0, niceFloor(fs / 10.0));
    r.frequency2Hz = 2.0 * r.frequency1Hz;
    r.transitionHz = r.frequency1Hz / 5.0;
    return r;
}

FilterRecipe blankFilterRecipe(double sampleRateHz)
{
    FilterRecipe r = defaultFilterRecipe(sampleRateHz);
    r.kind = FilterKind::None;
    return r;
}

const std::vector<FilterExample> &filterExamples()
{
    static const std::vector<FilterExample> examples = [] {
        std::vector<FilterExample> list;
        const auto iir = [](dsp::FilterFamily family, dsp::FilterResponse response, int order, double f1, double f2) {
            FilterRecipe r;
            r.family = family;
            r.response = response;
            r.order = order;
            r.frequency1Hz = f1;
            r.frequency2Hz = f2;
            return r;
        };
        list.push_back({"Passe-bas Butterworth 1 kHz", "Le plus plat en bande passante : −3 dB à 1 kHz, −24 dB par octave au-delà (ordre 4).",
                        iir(dsp::FilterFamily::Butterworth, dsp::FilterResponse::LowPass, 4, 1000.0, 2000.0)});
        FilterRecipe mains = iir(dsp::FilterFamily::Butterworth, dsp::FilterResponse::BandStop, 2, 45.0, 55.0);
        mains.application = FilterApplication::ZeroPhase;
        list.push_back({"Coupe-bande 50 Hz (secteur)",
                        "Retire le 50 Hz du secteur (45 Hz à 55 Hz), aller-retour : sans déphaser le reste.", mains});
        FilterRecipe drift = iir(dsp::FilterFamily::Butterworth, dsp::FilterResponse::HighPass, 2, 0.5, 1.0);
        drift.application = FilterApplication::ZeroPhase;
        list.push_back({"Passe-haut 0,5 Hz (dérive)", "Retire la dérive lente de la ligne de base (capteurs, ECG), sans déphasage.", drift});
        FilterRecipe phone = iir(dsp::FilterFamily::ChebyshevI, dsp::FilterResponse::BandPass, 4, 300.0, 3400.0);
        phone.passbandRippleDb = 0.5;
        list.push_back({"Passe-bande téléphone (300 Hz à 3,4 kHz)",
                        "Chebyshev I d'ordre 4, ondulation de 0,5 dB : des flancs plus raides qu'un Butterworth du même ordre.", phone});
        FilterRecipe cauer = iir(dsp::FilterFamily::Elliptic, dsp::FilterResponse::LowPass, 5, 1000.0, 2000.0);
        cauer.passbandRippleDb = 0.5;
        cauer.stopbandAttenuationDb = 60.0;
        list.push_back({"Passe-bas elliptique 1 kHz (0,5 dB, 60 dB)",
                        "Ordre 5 seulement, et déjà 60 dB d'atténuation dès 1,8 kHz environ : une ondulation dans les deux "
                        "bandes pour le flanc le plus raide.",
                        cauer});
        FilterRecipe smooth = iir(dsp::FilterFamily::Bessel, dsp::FilterResponse::LowPass, 4, 1000.0, 2000.0);
        list.push_back({"Passe-bas Bessel 1 kHz (sans dépassement)",
                        "Temps de propagation presque constant : un carré filtré garde sa forme, sans oscillation.", smooth});
        FilterRecipe fir;
        fir.kind = FilterKind::Fir;
        fir.taps = 101;
        fir.window = dsp::Window::Hamming;
        fir.frequency1Hz = 2000.0;
        fir.frequency2Hz = 4000.0;
        fir.application = FilterApplication::Aligned;
        list.push_back({"RIF passe-bas 2 kHz (101 coefficients)",
                        "Fenêtre de Hamming : phase linéaire, retard retiré à l'application ; lobes à −53 dB.", fir});
        FilterRecipe equiripple = fir;
        equiripple.firMethod = FirMethod::Equiripple;
        equiripple.frequency1Hz = 2000.0;
        equiripple.transitionHz = 1400.0;
        equiripple.passbandRippleDb = 0.1;
        equiripple.stopbandAttenuationDb = 60.0;
        list.push_back({"RIF équiondulation 2 kHz (Parks-McClellan)",
                        "101 coefficients, transition de 1,3 kHz à 2,7 kHz : l'ondulation et l'atténuation réparties au "
                        "mieux (0,1 dB et 60 dB demandés).",
                        equiripple});
        FilterRecipe kaiser = fir;
        kaiser.response = dsp::FilterResponse::BandPass;
        kaiser.taps = 255;
        kaiser.window = dsp::Window::Kaiser;
        kaiser.kaiserBeta = 8.0;
        kaiser.frequency1Hz = 1000.0;
        kaiser.frequency2Hz = 3000.0;
        list.push_back({"RIF passe-bande Kaiser (1 kHz à 3 kHz)",
                        "255 coefficients, β = 8 : environ −80 dB hors de la bande.", kaiser});
        // From a template: the least order computed.
        FilterRecipe sharp;
        sharp.family = dsp::FilterFamily::Elliptic;
        sharp.fromTemplate = true;
        sharp.passEdge1Hz = 1000.0;
        sharp.stopEdge1Hz = 1500.0;
        sharp.passbandRippleDb = 0.5;
        sharp.stopbandAttenuationDb = 60.0;
        list.push_back({"Gabarit passe-bas : 1 kHz, 60 dB dès 1,5 kHz (elliptique)",
                        "Le gabarit seulement : au plus 0,5 dB d'atténuation jusqu'à 1 kHz, au moins 60 dB dès 1,5 kHz ; "
                        "l'ordre le plus petit qui le tient est calculé (6 en elliptique, 9 en Chebyshev, 20 en "
                        "Butterworth : le panneau Filtre le dit).",
                        sharp});
        FilterRecipe voice;
        voice.kind = FilterKind::Fir;
        voice.firMethod = FirMethod::Equiripple;
        voice.response = dsp::FilterResponse::BandPass;
        voice.application = FilterApplication::Aligned;
        voice.fromTemplate = true;
        voice.stopEdge1Hz = 600.0;
        voice.passEdge1Hz = 1000.0;
        voice.passEdge2Hz = 3000.0;
        voice.stopEdge2Hz = 3600.0;
        voice.passbandRippleDb = 0.5;
        voice.stopbandAttenuationDb = 50.0;
        list.push_back({"Gabarit RIF passe-bande 1 kHz à 3 kHz (équiondulation)",
                        "0,5 dB d'ondulation au plus de 1 kHz à 3 kHz, 50 dB d'atténuation au moins sous 600 Hz et dès "
                        "3,6 kHz : le nombre de coefficients le plus petit est cherché (238 à 48 kHz), phase linéaire.",
                        voice});
        // Coefficients typed.
        const auto typed = [](std::string b, std::string a, std::string variables, std::string bCount = {}) {
            FilterRecipe r;
            r.kind = FilterKind::Typed;
            r.numerator = std::move(b);
            r.denominator = std::move(a);
            r.variables = std::move(variables);
            r.numeratorCount = std::move(bCount);
            return r;
        };
        list.push_back({"Moyenne glissante sur 8 points (b tapés)",
                        "b[k] = 1/N pour k de 0 à N − 1, a = 1 : la moyenne des N derniers échantillons, un RIF ; "
                        "ses zéros éteignent fs/N, 2 fs/N…",
                        typed("1/N", "1", "N = 8", "N")});
        list.push_back({"Lissage exponentiel (α = 0,1)",
                        "y[n] = α x[n] + (1 − α) y[n − 1] : b = α, a = 1 ; α − 1. Un RII du premier ordre, son pôle à "
                        "1 − α ; constante de temps d'environ Te/α.",
                        typed("alpha", "1 ; alpha - 1", "alpha = 0,1")});
        list.push_back({"Encoche à 50 Hz (biquad tapé)",
                        "Deux zéros sur le cercle unité à ±f0, deux pôles juste derrière, à r = 1 − π B/fs pour une "
                        "encoche large de B à −3 dB ; g donne un gain de 1 en continu.",
                        typed("g ; -2 g cos(w0) ; g", "1 ; -2 r cos(w0) ; r^2",
                              "f0 = 50 ; B = 5 ; w0 = 2 pi f0 / fs ; r = 1 - pi B / fs ; "
                              "g = (1 - 2 r cos(w0) + r^2) / (2 - 2 cos(w0))")});
        list.push_back({"Dérivée (différence première)",
                        "y[n] = (x[n] − x[n − 1])/Te : b = fs ; −fs, la pente du signal en unités par seconde ; le gain "
                        "double à chaque octave.",
                        typed("fs ; -fs", "1", "")});
        return list;
    }();
    return examples;
}
} // namespace qacq::analysis
