#include "qacq/analysis/filters.h"

#include "qacq/analysis/analysis.h"
#include "qacq/dsp/constants.h"
#include "qacq/dsp/filtering.h"
#include "qacq/plot/axis.h"

#include <algorithm>
#include <array>
#include <cmath>
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

// Forward then backward over the record oddly extended at both ends (scipy.signal.filtfilt, padlen 3N).
std::vector<double> firFiltFilt(const std::vector<double> &taps, std::span<const double> x)
{
    const std::size_t n = x.size();
    if (n == 0) return {};
    const std::size_t pad = std::min<std::size_t>(3 * taps.size(), n - 1);
    std::vector<double> extended(n + 2 * pad);
    for (std::size_t i = 0; i < pad; ++i) extended[i] = 2.0 * x[0] - x[pad - i];
    std::copy(x.begin(), x.end(), extended.begin() + static_cast<std::ptrdiff_t>(pad));
    for (std::size_t i = 0; i < pad; ++i) extended[pad + n + i] = 2.0 * x[n - 1] - x[n - 2 - i];
    std::vector<double> y = dsp::firFilter(taps, extended);
    std::reverse(y.begin(), y.end());
    y = dsp::firFilter(taps, y);
    std::reverse(y.begin(), y.end());
    return std::vector<double>(y.begin() + static_cast<std::ptrdiff_t>(pad), y.begin() + static_cast<std::ptrdiff_t>(pad + n));
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
} // namespace

std::complex<double> FilterDesign::response(double frequencyHz) const noexcept
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
        case FilterApplication::ZeroPhase: return {std::norm(h), 0.0};
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
    return application != FilterApplication::Aligned || kind == FilterKind::Fir;
}

FilterDesign designFilter(const FilterRecipe &recipe)
{
    FilterDesign d;
    d.recipe = recipe;
    if (!applicationAllowed(recipe.kind, recipe.application)) d.recipe.application = FilterApplication::Causal;
    const FilterRecipe &r = d.recipe;
    if (r.kind == FilterKind::Iir)
    {
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
            return d;
        }
        d.sections = iir.sections;
        d.zeros = iir.digital.zeros;
        d.poles = iir.digital.poles;
        d.rootsKnown = true;
        const double radius = d.largestPoleRadius();
        if (radius > 0.99999)
            d.warnings.push_back("Un pôle est à " + plot::formatFixed(1.0 - radius, 6) +
                                 " du cercle unité : le filtre résonne longtemps, et en virgule fixe il deviendrait "
                                 "instable (une fréquence de coupure plus haute, ou un ordre plus petit, l'éloigne).");
        return d;
    }

    if (r.taps > kMaximumFirTaps)
    {
        d.error = "Un RIF de plus de " + grouped(kMaximumFirTaps) + " coefficients n'est pas conçu ici.";
        return d;
    }
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
        return d;
    }
    d.taps = fir.taps;
    if (d.taps.size() <= kMaximumFirRoots)
    {
        d.zeros = firZeros(d.taps);
        d.rootsKnown = true;
    }
    return d;
}

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
        constexpr std::size_t kBlock = 4096;
        dsp::SosState state(design.sections.size(), std::array<double, 2>{0.0, 0.0});
        std::vector<double> input(kBlock, 0.0);
        input[0] = 1.0;
        double peak = 0.0;
        out.settled = false;
        while (out.impulse.size() < kMaximumResponseLength)
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
    out.step = runningSum(out.impulse);
    if (design.recipe.application == FilterApplication::Causal || out.impulse.empty()) return out;

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

std::string filterText(const FilterRecipe &r)
{
    std::string text = std::string(dsp::responseName(r.response));
    if (r.kind == FilterKind::Iir)
    {
        text += " " + std::string(dsp::familyName(r.family)) + " d'ordre " + std::to_string(r.order);
        if (r.family == dsp::FilterFamily::ChebyshevI) text += " (" + quantity(r.passbandRippleDb, "dB") + ")";
        if (r.family == dsp::FilterFamily::ChebyshevII) text += " (" + quantity(r.stopbandAttenuationDb, "dB") + ")";
    }
    else
    {
        text += " RIF de " + grouped(r.taps) + " coefficients (" + std::string(dsp::windowName(r.window));
        if (r.window == dsp::Window::Kaiser) text += ", β = " + quantity(r.kaiserBeta, "", 3);
        text += ")";
    }
    return text + ", " + edgesText(r);
}

std::string filterShortText(const FilterRecipe &r)
{
    return lower(dsp::responseName(r.response)) + " " + edgesText(r);
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
    const double fs = d.recipe.sampleRateHz;
    if (d.fir())
    {
        const double delay = 0.5 * static_cast<double>(d.taps.size() - 1);
        lines.push_back(grouped(d.taps.size()) + " coefficients, phase linéaire : retard de " +
                        plot::formatCompact(delay, "", 6) + " échantillons (" + quantity(delay / fs, "s") + ").");
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
    }
    switch (d.recipe.application)
    {
        case FilterApplication::Causal: lines.push_back("Appliqué causalement : ce retard reste dans le signal filtré."); break;
        case FilterApplication::ZeroPhase:
            lines.push_back("Appliqué aller-retour : aucun retard, mais le gain au carré (deux fois l'atténuation en dB : "
                            "−6 dB à une coupure à −3 dB).");
            break;
        case FilterApplication::Aligned: lines.push_back("Appliqué en retirant le retard : le gain du RIF, sans retard."); break;
    }
    return lines;
}

std::vector<double> runFilter(const FilterDesign &d, std::span<const double> x)
{
    if (x.empty() || !d.ok()) return {};
    if (d.fir())
    {
        switch (d.recipe.application)
        {
            case FilterApplication::ZeroPhase: return firFiltFilt(d.taps, x);
            case FilterApplication::Aligned: return dsp::firFilterAligned(d.taps, x);
            case FilterApplication::Causal: break;
        }
        return dsp::firFilter(d.taps, x);
    }
    if (d.recipe.application == FilterApplication::ZeroPhase) return dsp::sosFiltFilt(d.sections, x);
    return dsp::sosFilter(d.sections, x);
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
    return kind == FilterKind::Fir ? "fir" : "iir";
}

std::string_view filterFamilyKey(dsp::FilterFamily family) noexcept
{
    switch (family)
    {
        case dsp::FilterFamily::Butterworth: return "butterworth";
        case dsp::FilterFamily::ChebyshevI: return "chebyshev1";
        case dsp::FilterFamily::ChebyshevII: return "chebyshev2";
        case dsp::FilterFamily::Bessel: return "bessel";
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
    j["window"] = windowKey(r.window);
    j["kaiserBeta"] = r.kaiserBeta;
    j["sampleRate"] = r.sampleRateHz;
    j["rateOfSignal"] = r.rateOfSignal;
    j["frequency1"] = r.frequency1Hz;
    j["frequency2"] = r.frequency2Hz;
    j["ripple"] = r.passbandRippleDb;
    j["attenuation"] = r.stopbandAttenuationDb;
    j["application"] = filterApplicationKey(r.application);
    return j;
}

FilterRecipe filterRecipeFromJson(const model::Json &j)
{
    FilterRecipe r;
    if (!j.isObject()) return r;
    r.kind = j["kind"].toString() == "fir" ? FilterKind::Fir : FilterKind::Iir;
    const std::string family = j["family"].toString();
    for (dsp::FilterFamily f : {dsp::FilterFamily::Butterworth, dsp::FilterFamily::ChebyshevI, dsp::FilterFamily::ChebyshevII,
                                dsp::FilterFamily::Bessel})
        if (family == filterFamilyKey(f)) r.family = f;
    const std::string response = j["response"].toString();
    for (dsp::FilterResponse f : {dsp::FilterResponse::LowPass, dsp::FilterResponse::HighPass, dsp::FilterResponse::BandPass,
                                  dsp::FilterResponse::BandStop})
        if (response == filterResponseKey(f)) r.response = f;
    r.order = static_cast<int>(number(j, "order", r.order, 1, dsp::kMaximumIirOrder));
    r.taps = static_cast<std::size_t>(number(j, "taps", static_cast<double>(r.taps), 3, static_cast<double>(kMaximumFirTaps)));
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
    return r;
}

FilterRecipe defaultFilterRecipe(double sampleRateHz)
{
    FilterRecipe r;
    const double fs = sampleRateHz > 0.0 && std::isfinite(sampleRateHz) ? sampleRateHz : 48000.0;
    r.sampleRateHz = fs;
    r.frequency1Hz = std::min(1000.0, niceFloor(fs / 10.0));
    r.frequency2Hz = 2.0 * r.frequency1Hz;
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
        FilterRecipe kaiser = fir;
        kaiser.response = dsp::FilterResponse::BandPass;
        kaiser.taps = 255;
        kaiser.window = dsp::Window::Kaiser;
        kaiser.kaiserBeta = 8.0;
        kaiser.frequency1Hz = 1000.0;
        kaiser.frequency2Hz = 3000.0;
        list.push_back({"RIF passe-bande Kaiser (1 kHz à 3 kHz)",
                        "255 coefficients, β = 8 : environ −80 dB hors de la bande.", kaiser});
        return list;
    }();
    return examples;
}
} // namespace qacq::analysis
