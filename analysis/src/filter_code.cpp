#include "qacq/analysis/filter_code.h"

#include "qacq/dsp/constants.h"
#include "qacq/dsp/filtering.h"
#include "qacq/plot/axis.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
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

std::string decibels(double value)
{
    return quantity(value, "dB", 3);
}

// A gain in dB, to the hundredth.
std::string gainDecibels(double value)
{
    return std::abs(value) < 0.005 ? std::string("0 dB") : quantity(value, "dB", 3);
}

// The response of one section (b0, b1, b2, a1, a2; a0 = 1) at ω.
Complex sectionResponse(const std::array<double, 5> &c, double w) noexcept
{
    const Complex z1 = std::polar(1.0, -w), z2 = z1 * z1;
    return (c[0] + c[1] * z1 + c[2] * z2) / (1.0 + c[3] * z1 + c[4] * z2);
}

Complex sectionResponse(const dsp::Biquad &s, double w) noexcept
{
    return sectionResponse(std::array<double, 5>{s.b[0], s.b[1], s.b[2], s.a[1], s.a[2]}, w);
}

// The pole of z² + a1 z + a2 farthest from the origin.
double sectionPoleRadius(double a1, double a2) noexcept
{
    const double disc = a1 * a1 - 4.0 * a2;
    if (disc < 0.0) return std::sqrt(std::abs(a2));
    const double root = std::sqrt(disc);
    return std::max(std::abs(0.5 * (-a1 + root)), std::abs(0.5 * (-a1 - root)));
}

// h[k] = h[N−1−k] (1), = −h[N−1−k] (−1), to 1e-9 of the largest; 0 otherwise.
int tapSymmetry(const std::vector<double> &taps)
{
    double largest = 0.0;
    for (double h : taps) largest = std::max(largest, std::abs(h));
    if (!(largest > 0.0) || taps.size() < 2) return 0;
    const double tolerance = 1e-9 * largest;
    bool even = true, odd = true;
    for (std::size_t k = 0, j = taps.size() - 1; k < j; ++k, --j)
    {
        even = even && std::abs(taps[k] - taps[j]) <= tolerance;
        odd = odd && std::abs(taps[k] + taps[j]) <= tolerance;
    }
    if (taps.size() % 2 == 1) odd = odd && std::abs(taps[taps.size() / 2]) <= tolerance;
    return even ? 1 : odd ? -1 : 0;
}

// The deterministic noise of the tests (a linear congruential generator), uniform in [−1, 1).
struct Noise
{
    std::uint32_t state = 0x2545F491u;
    double next() noexcept
    {
        state = 1664525u * state + 1013904223u;
        return static_cast<double>(state) / 2147483648.0 - 1.0;
    }
};

// Fixed point: the largest integer, the right shift and its rounding.
constexpr std::int64_t fullScale(int bits) noexcept
{
    return std::int64_t{1} << (bits - 1);
}

std::int64_t saturate(std::int64_t v, int bits, std::size_t *count) noexcept
{
    const std::int64_t top = fullScale(bits) - 1, bottom = -fullScale(bits);
    if (v > top)
    {
        if (count) ++*count;
        return top;
    }
    if (v < bottom)
    {
        if (count) ++*count;
        return bottom;
    }
    return v;
}

std::int64_t toFixed(double x, int bits) noexcept
{
    const double scaled = x * static_cast<double>(fullScale(bits));
    if (!(scaled < static_cast<double>(fullScale(bits) - 1))) return fullScale(bits) - 1;
    if (!(scaled > -static_cast<double>(fullScale(bits)))) return -fullScale(bits);
    return std::llround(scaled);
}

// (acc + 2^(d−1)) >> d: rounded to the nearest, halves up (the arithmetic shift of every compiler for
// microcontrollers, and of C++20).
std::int64_t roundShift(std::int64_t acc, int shift) noexcept
{
    return (acc + (std::int64_t{1} << (shift - 1))) >> shift;
}

// The coefficients of a section (or the taps of a FIR) as integers: the smallest left part m (the format is
// Q(bits − 1 − m)) such that every integer fits and the accumulator cannot overflow, whatever the samples
// (at most 2^(bits−1) in magnitude): Σ|c|·2^(bits−1) + 2^(d−1) ≤ 2^(accumulatorBits−1) − 1, d = bits − 1 − m
// the right shift. False if none does (d ≥ 1).
// A coefficient that equals another in the design is made from that one once rounded: a numerator
// b0 (1 − 2 cos θ z⁻¹ + z⁻²), b2 = b0, keeps its zeros on the unit circle (a notch stays a notch, a low-pass
// stops fs/2). Not b1 = ±2 b0 as well: the gain of a section of small b would be off by more.
struct Tie
{
    std::size_t index;
    std::size_t source;
    std::int64_t factor;
};

std::vector<Tie> sectionTies(const std::vector<double> &b)
{
    std::vector<Tie> ties;
    const double size = std::max({std::abs(b[0]), std::abs(b[1]), std::abs(b[2])});
    const double tolerance = 1e-12 * size;
    if (!(size > 0.0)) return ties;
    if (std::abs(b[2] - b[0]) <= tolerance) ties.push_back({2, 0, 1});
    return ties;
}

bool quantize(const std::vector<double> &values, int bits, int accumulatorBits, std::vector<std::int64_t> &integers,
              int &shift, const std::vector<Tie> &ties = {})
{
    const int f = bits - 1;
    const std::uint64_t accumulatorTop = accumulatorBits == 32 ? std::uint64_t{0x7FFFFFFF} : std::uint64_t{0x7FFFFFFFFFFFFFFF};
    for (int m = 0; m < f; ++m)
    {
        const int d = f - m;
        const double scale = std::ldexp(1.0, d);
        integers.assign(values.size(), 0);
        bool fits = true;
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < values.size() && fits; ++i)
        {
            const double v = values[i] * scale;
            if (!(std::abs(v) <= static_cast<double>(fullScale(bits) - 1) - 0.5))
            {
                fits = false;
                break;
            }
            integers[i] = std::llround(v);
        }
        if (!fits) continue;
        for (const Tie &t : ties) integers[t.index] = t.factor * integers[t.source];
        for (std::int64_t c : integers)
        {
            if (c > fullScale(bits) - 1 || c < -(fullScale(bits) - 1)) fits = false;
            sum += static_cast<std::uint64_t>(c < 0 ? -c : c);
        }
        if (!fits) continue;
        const std::uint64_t limit = (accumulatorTop - (std::uint64_t{1} << (d - 1))) >> f;
        if (sum > limit) continue;
        shift = d;
        return true;
    }
    return false;
}

// The impulse response of the cascade through its first sections: Σ|h| at each section's output.
std::vector<double> cumulativeSums(const dsp::SosCascade &sections)
{
    const std::size_t length = std::max<std::size_t>(64, dsp::sosRingLength(sections, 1e-12, std::size_t{1} << 22));
    std::vector<double> sums(sections.size(), 0.0);
    std::vector<std::array<double, 2>> state(sections.size(), std::array<double, 2>{0.0, 0.0});
    for (std::size_t n = 0; n < length; ++n)
    {
        double x = n == 0 ? 1.0 : 0.0;
        for (std::size_t s = 0; s < sections.size(); ++s)
        {
            const dsp::Biquad &c = sections[s];
            const double y = c.b[0] * x + state[s][0];
            state[s][0] = c.b[1] * x - c.a[1] * y + state[s][1];
            state[s][1] = c.b[2] * x - c.a[2] * y;
            sums[s] += std::abs(y);
            x = y;
        }
    }
    return sums;
}

// The largest |Π of the first s sections| over the frequencies, for each s: on a grid of 4 096 frequencies
// and at the angle of every pole (where narrow peaks are), refined around the best by golden-section search.
std::vector<double> cumulativePeaks(const dsp::SosCascade &sections, double fs)
{
    const std::size_t n = sections.size();
    const auto cumulative = [&sections](std::size_t s, double w) {
        Complex h(1.0, 0.0);
        for (std::size_t i = 0; i <= s; ++i) h *= sectionResponse(sections[i], w);
        return std::abs(h);
    };
    std::vector<double> frequencies;
    constexpr int kGrid = 4096;
    for (int i = 0; i <= kGrid; ++i) frequencies.push_back(dsp::kPi * static_cast<double>(i) / kGrid);
    for (const dsp::Biquad &c : sections)
    {
        const double disc = c.a[1] * c.a[1] - 4.0 * c.a[2];
        if (disc < 0.0 && c.a[2] > 0.0)
            frequencies.push_back(std::acos(std::clamp(-c.a[1] / (2.0 * std::sqrt(c.a[2])), -1.0, 1.0)));
    }
    std::vector<double> peaks(n, 0.0);
    for (std::size_t s = 0; s < n; ++s)
    {
        double best = -1.0, at = 0.0;
        for (double w : frequencies)
        {
            const double h = cumulative(s, w);
            if (std::isfinite(h) && h > best)
            {
                best = h;
                at = w;
            }
        }
        // Golden section over a grid step on each side.
        double lo = std::max(0.0, at - dsp::kPi / kGrid), hi = std::min(dsp::kPi, at + dsp::kPi / kGrid);
        const double ratio = 0.5 * (std::sqrt(5.0) - 1.0);
        for (int i = 0; i < 60; ++i)
        {
            const double a = hi - ratio * (hi - lo), b = lo + ratio * (hi - lo);
            if (cumulative(s, a) > cumulative(s, b)) hi = b;
            else lo = a;
        }
        peaks[s] = std::max(best, cumulative(s, 0.5 * (lo + hi)));
    }
    (void)fs;
    return peaks;
}

const char *const kCWords[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum", "extern",
    "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return", "short", "signed",
    "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while",
    // C++, for the header included there (extern "C").
    "and", "bool", "catch", "class", "delete", "false", "friend", "namespace", "new", "not", "operator", "or",
    "private", "protected", "public", "template", "this", "throw", "true", "try", "typename", "using", "virtual",
    "main"};
} // namespace

bool validCodeName(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 31) return false;
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (!letter(name[0])) return false;
    for (char c : name)
        if (!letter(c) && !(c >= '0' && c <= '9')) return false;
    // Reserved: _ then a capital, or two _.
    if (name[0] == '_' && name.size() > 1 && (name[1] == '_' || (name[1] >= 'A' && name[1] <= 'Z'))) return false;
    for (const char *word : kCWords)
        if (name == word) return false;
    return true;
}

std::string_view codeFormatKey(CodeFormat format) noexcept
{
    switch (format)
    {
        case CodeFormat::Float32: return "float";
        case CodeFormat::Float64: return "double";
        case CodeFormat::Q15: return "q15";
        case CodeFormat::Q31: return "q31";
    }
    return "float";
}

std::string_view codeScalingKey(CodeScaling scaling) noexcept
{
    return scaling == CodeScaling::Peak ? "peak" : "sum";
}

bool codeFormatFromKey(std::string_view key, CodeFormat &format) noexcept
{
    for (CodeFormat f : {CodeFormat::Float32, CodeFormat::Float64, CodeFormat::Q15, CodeFormat::Q31})
        if (key == codeFormatKey(f))
        {
            format = f;
            return true;
        }
    return false;
}

bool codeScalingFromKey(std::string_view key, CodeScaling &scaling) noexcept
{
    for (CodeScaling s : {CodeScaling::Sum, CodeScaling::Peak})
        if (key == codeScalingKey(s))
        {
            scaling = s;
            return true;
        }
    return false;
}

std::string codeFormatText(CodeFormat format)
{
    switch (format)
    {
        case CodeFormat::Float32: return "Flottant simple précision (float)";
        case CodeFormat::Float64: return "Flottant double précision (double)";
        case CodeFormat::Q15: return "Virgule fixe Q15 (int16_t)";
        case CodeFormat::Q31: return "Virgule fixe Q31 (int32_t)";
    }
    return {};
}

// ---- Running it as the board does ----

namespace
{
struct RunCounts
{
    std::size_t internal = 0; // saturated between sections
    std::size_t output = 0;   // saturated at the output
};

template <typename Real>
std::vector<double> runFloatSections(const EmbeddedFilter &e, std::span<const double> x)
{
    std::vector<std::array<Real, 5>> c;
    for (const auto &s : e.sections)
        c.push_back({static_cast<Real>(s[0]), static_cast<Real>(s[1]), static_cast<Real>(s[2]), static_cast<Real>(s[3]),
                     static_cast<Real>(s[4])});
    const Real gain = static_cast<Real>(e.outputGain);
    std::vector<std::array<Real, 2>> z(c.size(), std::array<Real, 2>{Real(0), Real(0)});
    std::vector<double> out(x.size());
    for (std::size_t n = 0; n < x.size(); ++n)
    {
        Real v = static_cast<Real>(x[n]);
        for (std::size_t k = 0; k < c.size(); ++k)
        {
            // As written in the C: y = c0 x + z0; z0 = c1 x − c3 y + z1; z1 = c2 x − c4 y.
            const Real y = c[k][0] * v + z[k][0];
            z[k][0] = c[k][1] * v - c[k][3] * y + z[k][1];
            z[k][1] = c[k][2] * v - c[k][4] * y;
            v = y;
        }
        if (e.outputGain != 1.0) v = v * gain;
        out[n] = static_cast<double>(v);
    }
    return out;
}

// The taps as the code stores them: all of them, or the first half (and the middle) when they are folded.
std::size_t storedTaps(const EmbeddedFilter &e)
{
    return e.symmetry != 0 ? (e.taps.size() + 1) / 2 : e.taps.size();
}

template <typename Real>
std::vector<double> runFloatTaps(const EmbeddedFilter &e, std::span<const double> x)
{
    const std::size_t n = e.taps.size();
    std::vector<Real> h(n);
    for (std::size_t k = 0; k < n; ++k) h[k] = static_cast<Real>(e.taps[k]);
    const Real gain = static_cast<Real>(e.outputGain);
    // The doubled line: the last N samples are w[0..N−1], w[k] = x[n − k].
    std::vector<Real> line(2 * n, Real(0));
    std::size_t pos = 0;
    std::vector<double> out(x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        pos = pos == 0 ? n - 1 : pos - 1;
        line[pos] = static_cast<Real>(x[i]);
        line[pos + n] = line[pos];
        const Real *w = line.data() + pos;
        Real acc = Real(0);
        if (e.symmetry == 0)
        {
            for (std::size_t k = 0; k < n; ++k) acc += h[k] * w[k];
        }
        else
        {
            for (std::size_t k = 0; k < n / 2; ++k)
                acc += h[k] * (e.symmetry > 0 ? w[k] + w[n - 1 - k] : w[k] - w[n - 1 - k]);
            if (n % 2 == 1 && e.symmetry > 0) acc += h[n / 2] * w[n / 2];
        }
        if (e.outputGain != 1.0) acc = acc * gain;
        out[i] = static_cast<double>(acc);
    }
    return out;
}

std::vector<double> runFixedSections(const EmbeddedFilter &e, std::span<const double> x, RunCounts &counts)
{
    const int bits = e.bits;
    const double unit = static_cast<double>(fullScale(bits));
    struct State
    {
        std::int64_t x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    };
    std::vector<State> state(e.sectionIntegers.size());
    std::vector<double> out(x.size());
    const std::size_t last = e.sectionIntegers.size() - 1;
    for (std::size_t n = 0; n < x.size(); ++n)
    {
        std::int64_t v = toFixed(x[n], bits);
        for (std::size_t k = 0; k < e.sectionIntegers.size(); ++k)
        {
            const auto &c = e.sectionIntegers[k];
            State &s = state[k];
            const std::int64_t acc = c[0] * v + c[1] * s.x1 + c[2] * s.x2 - c[3] * s.y1 - c[4] * s.y2;
            const std::int64_t y =
                saturate(roundShift(acc, e.sectionShifts[k]), bits, k == last && !e.gainStage ? &counts.output : &counts.internal);
            s.x2 = s.x1;
            s.x1 = v;
            s.y2 = s.y1;
            s.y1 = y;
            v = y;
        }
        if (e.gainStage) v = saturate(roundShift(e.gainInteger * v, e.gainShift), bits, &counts.output);
        out[n] = static_cast<double>(v) / unit;
    }
    return out;
}

std::vector<double> runFixedTaps(const EmbeddedFilter &e, std::span<const double> x, RunCounts &counts)
{
    const int bits = e.bits;
    const double unit = static_cast<double>(fullScale(bits));
    const std::size_t n = e.taps.size();
    const std::vector<std::int64_t> &h = e.tapIntegers;
    std::vector<std::int64_t> line(2 * n, 0);
    std::size_t pos = 0;
    std::vector<double> out(x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        pos = pos == 0 ? n - 1 : pos - 1;
        line[pos] = toFixed(x[i], bits);
        line[pos + n] = line[pos];
        const std::int64_t *w = line.data() + pos;
        std::int64_t acc = 0;
        if (e.symmetry == 0)
        {
            for (std::size_t k = 0; k < n; ++k) acc += h[k] * w[k];
        }
        else
        {
            for (std::size_t k = 0; k < n / 2; ++k) acc += h[k] * (e.symmetry > 0 ? w[k] + w[n - 1 - k] : w[k] - w[n - 1 - k]);
            if (n % 2 == 1 && e.symmetry > 0) acc += h[n / 2] * w[n / 2];
        }
        out[i] = static_cast<double>(saturate(roundShift(acc, e.tapShift), bits, &counts.output)) / unit;
    }
    return out;
}

std::vector<double> run(const EmbeddedFilter &e, std::span<const double> x, RunCounts &counts)
{
    if (!e.ok() || x.empty()) return {};
    if (e.fixed) return e.fir ? runFixedTaps(e, x, counts) : runFixedSections(e, x, counts);
    if (e.bits == 32) return e.fir ? runFloatTaps<float>(e, x) : runFloatSections<float>(e, x);
    return e.fir ? runFloatTaps<double>(e, x) : runFloatSections<double>(e, x);
}
} // namespace

std::vector<double> runEmbedded(const EmbeddedFilter &filter, std::span<const double> x, std::size_t *saturated)
{
    RunCounts counts;
    std::vector<double> y = run(filter, x, counts);
    if (saturated) *saturated += counts.internal + counts.output;
    return y;
}

std::complex<double> embeddedResponse(const EmbeddedFilter &e, double frequencyHz) noexcept
{
    const double w = dsp::kTwoPi * frequencyHz / e.sampleRateHz;
    Complex h(1.0, 0.0);
    if (e.fir)
    {
        h = Complex(0.0, 0.0);
        for (std::size_t k = 0; k < e.taps.size(); ++k) h += e.taps[k] * std::polar(1.0, -w * static_cast<double>(k));
    }
    else
    {
        for (const auto &s : e.sections) h *= sectionResponse(s, w);
    }
    return h * e.outputGain;
}

// ---- Rounded, scaled, analysed ----

EmbeddedFilter embedFilter(const FilterDesign &design, const CodeOptions &options)
{
    EmbeddedFilter e;
    e.options = options;
    e.sampleRateHz = design.recipe.sampleRateHz;
    e.fixed = options.format == CodeFormat::Q15 || options.format == CodeFormat::Q31;
    e.bits = options.format == CodeFormat::Q15 ? 16 : options.format == CodeFormat::Float64 ? 64 : 32;
    if (!validCodeName(options.name))
    {
        e.error = "« " + options.name + " » ne peut pas nommer le code : une lettre ou _, puis des lettres, des chiffres "
                  "ou _ (31 au plus), pas un mot du C.";
        return e;
    }
    if (!design.ok())
    {
        e.error = design.error.empty() ? std::string("Aucun filtre n'est conçu.") : design.error;
        return e;
    }
    e.text = filterText(design.recipe);
    e.fir = design.fir();
    const double radius = design.largestPoleRadius();
    if (!e.fir && radius > 1.0 + 1e-9)
    {
        e.error = "Ce filtre est instable (un pôle hors du cercle unité) : pas de code pour lui.";
        return e;
    }
    if (!e.fir && radius >= 1.0 - 1e-9 && e.fixed)
    {
        e.error = "Un pôle est sur le cercle unité (un intégrateur, un oscillateur) : en virgule fixe, sa sortie finirait "
                  "par saturer, et aucune mise à l'échelle n'y fait rien. Prenez du flottant.";
        return e;
    }
    if (e.fir && design.taps.size() > kMaximumCodeTaps)
    {
        e.error = "Un RIF de plus de " + std::to_string(kMaximumCodeTaps) +
                  " coefficients ne tient pas dans un microcontrôleur courant : pas de code pour lui.";
        return e;
    }
    const double fs = e.sampleRateHz;
    const double g = design.gain;
    const char *formatName = e.fixed ? (e.bits == 16 ? "Q15" : "Q31") : (e.bits == 32 ? "float" : "double");

    if (e.fir)
    {
        // Folded where symmetric: the two halves made equal first, so that rounding keeps them so.
        e.symmetry = tapSymmetry(design.taps);
        std::vector<double> h = design.taps;
        const std::size_t n = h.size();
        if (e.symmetry != 0)
        {
            for (std::size_t k = 0, j = n - 1; k < j; ++k, --j)
            {
                const double v = e.symmetry > 0 ? 0.5 * (h[k] + h[j]) : 0.5 * (h[k] - h[j]);
                h[k] = v;
                h[j] = e.symmetry > 0 ? v : -v;
            }
            if (e.symmetry < 0 && n % 2 == 1) h[n / 2] = 0.0;
        }
        if (e.fixed)
        {
            for (double &v : h) v *= g;
            e.accumulatorBits = 64;
            if (e.bits == 16)
            {
                // A 32-bit accumulator if it holds the taps in the format they have with 64 bits.
                std::vector<std::int64_t> wide, narrow;
                int wideShift = 0, narrowShift = 0;
                if (quantize(h, 16, 64, wide, wideShift) && quantize(h, 16, 32, narrow, narrowShift) && narrowShift == wideShift)
                    e.accumulatorBits = 32;
            }
            if (!quantize(h, e.bits, e.accumulatorBits, e.tapIntegers, e.tapShift))
            {
                double largest = 0.0, sum = 0.0;
                for (double v : h)
                {
                    largest = std::max(largest, std::abs(v));
                    sum += std::abs(v);
                }
                e.error = std::string("Les coefficients, gain compris, sont trop grands pour ") + formatName + " (jusqu'à " +
                          plot::formatCompact(largest, "", 4) + ", somme des valeurs absolues " +
                          plot::formatCompact(sum, "", 4) + ") : prenez " + (e.bits == 16 ? "Q31 ou du flottant." : "du flottant.");
                return e;
            }
            e.taps.resize(n);
            for (std::size_t k = 0; k < n; ++k) e.taps[k] = std::ldexp(static_cast<double>(e.tapIntegers[k]), -e.tapShift);
            e.outputGain = 1.0;
        }
        else
        {
            e.taps.resize(n);
            for (std::size_t k = 0; k < n; ++k)
                e.taps[k] = e.bits == 32 ? static_cast<double>(static_cast<float>(h[k])) : h[k];
            e.outputGain = e.bits == 32 ? static_cast<double>(static_cast<float>(g)) : g;
        }
    }
    else
    {
        const dsp::SosCascade &sections = design.sections;
        if (e.fixed)
        {
            // Scaled so that each section's output stays under 0,99 of full scale (the last one: the filter's own
            // gain, as long as what it passes stays within full scale; the rest of it in a stage of its own).
            const std::vector<double> norms =
                options.scaling == CodeScaling::Sum ? cumulativeSums(sections) : cumulativePeaks(sections, fs);
            constexpr double kTarget = 0.99;
            const double lastScale = g * norms.back() <= 1.0 + 1e-9 ? g : kTarget / norms.back();
            std::vector<double> scales;
            double previous = 1.0;
            for (std::size_t s = 0; s < sections.size(); ++s)
            {
                const double cumulative = s + 1 == sections.size() ? lastScale : kTarget / norms[s];
                if (!(std::isfinite(cumulative) && cumulative > 0.0))
                {
                    e.error = "La mise à l'échelle des sections échoue (une section qui ne laisse rien passer) : prenez du "
                              "flottant.";
                    return e;
                }
                scales.push_back(cumulative / previous);
                previous = cumulative;
            }
            // Each section rounded, for an accumulator of the width given; false if one does not fit.
            const auto round = [&](int accumulatorBits, std::size_t &failed) {
                e.sectionScales.clear();
                e.sectionShifts.clear();
                e.sectionIntegers.clear();
                e.sections.clear();
                for (std::size_t s = 0; s < sections.size(); ++s)
                {
                    const dsp::Biquad &c = sections[s];
                    const double k = scales[s];
                    const std::vector<double> values{k * c.b[0], k * c.b[1], k * c.b[2], c.a[1], c.a[2]};
                    std::vector<std::int64_t> integers;
                    int shift = 0;
                    if (!quantize(values, e.bits, accumulatorBits, integers, shift, sectionTies({c.b[0], c.b[1], c.b[2]})))
                    {
                        failed = s;
                        return false;
                    }
                    e.sectionScales.push_back(k);
                    e.sectionShifts.push_back(shift);
                    e.sectionIntegers.push_back({integers[0], integers[1], integers[2], integers[3], integers[4]});
                    std::array<double, 5> stored{};
                    for (std::size_t i = 0; i < 5; ++i) stored[i] = std::ldexp(static_cast<double>(integers[i]), -shift);
                    e.sections.push_back(stored);
                }
                return true;
            };
            std::size_t failed = 0;
            if (!round(64, failed))
            {
                e.error = "Les coefficients de la section " + std::to_string(failed + 1) + ", mis à l'échelle, sont trop "
                          "grands pour " + formatName + " : prenez du flottant.";
                return e;
            }
            e.accumulatorBits = 64;
            if (e.bits == 16)
            {
                // A 32-bit accumulator if every section keeps the format it has with 64 bits.
                const std::vector<int> wide = e.sectionShifts;
                if (round(32, failed) && e.sectionShifts == wide) e.accumulatorBits = 32;
                else round(64, failed);
            }
            e.outputGain = 1.0;
            if (g > lastScale * (1.0 + 1e-12))
            {
                std::vector<std::int64_t> integers;
                int shift = 0;
                if (!quantize({g / lastScale}, e.bits, 2 * e.bits, integers, shift))
                {
                    e.error = std::string("Le gain en sortie est trop grand pour ") + formatName + ".";
                    return e;
                }
                e.gainStage = true;
                e.gainInteger = integers[0];
                e.gainShift = shift;
                e.outputGain = std::ldexp(static_cast<double>(e.gainInteger), -shift);
            }
            // The gain where the filter passes most, as designed: the b of the last section corrected for what
            // rounding took from the others, in the same format (its a, its poles untouched), twice; not at all if
            // they would no longer fit.
            double peak = 0.0, at = 0.0;
            for (int i = 0; i <= 512; ++i)
            {
                const double f = 0.5 * fs * static_cast<double>(i) / 512.0;
                const double h = std::abs(design.response(f));
                if (std::isfinite(h) && h > peak * (1.0 + 1e-9))
                {
                    peak = h;
                    at = f;
                }
            }
            const std::uint64_t accumulatorTop =
                e.accumulatorBits == 32 ? std::uint64_t{0x7FFFFFFF} : std::uint64_t{0x7FFFFFFFFFFFFFFF};
            for (int pass = 0; pass < 2 && peak > 0.0; ++pass)
            {
                const double got = std::abs(embeddedResponse(e, at));
                if (!(got > 0.0) || !std::isfinite(got)) break;
                const std::size_t last = e.sections.size() - 1;
                const double k = e.sectionScales[last] * peak / got;
                const dsp::Biquad &c = sections[last];
                const int shift = e.sectionShifts[last];
                std::array<std::int64_t, 5> integers = e.sectionIntegers[last];
                for (std::size_t i = 0; i < 3; ++i) integers[i] = std::llround(std::ldexp(k * c.b[i], shift));
                for (const Tie &t : sectionTies({c.b[0], c.b[1], c.b[2]})) integers[t.index] = t.factor * integers[t.source];
                std::uint64_t sum = 0;
                bool fits = true;
                for (std::int64_t v : integers)
                {
                    fits = fits && v <= fullScale(e.bits) - 1 && v >= -(fullScale(e.bits) - 1);
                    sum += static_cast<std::uint64_t>(v < 0 ? -v : v);
                }
                if (!fits || sum > (accumulatorTop - (std::uint64_t{1} << (shift - 1))) >> (e.bits - 1)) break;
                e.sectionScales[last] = k;
                e.sectionIntegers[last] = integers;
                for (std::size_t i = 0; i < 5; ++i) e.sections[last][i] = std::ldexp(static_cast<double>(integers[i]), -shift);
            }
        }
        else
        {
            for (const dsp::Biquad &c : sections)
            {
                std::array<double, 5> v{c.b[0], c.b[1], c.b[2], c.a[1], c.a[2]};
                if (e.bits == 32)
                    for (double &x : v) x = static_cast<double>(static_cast<float>(x));
                e.sections.push_back(v);
            }
            e.outputGain = e.bits == 32 ? static_cast<double>(static_cast<float>(g)) : g;
        }
        for (const auto &s : e.sections) e.largestPoleRadius = std::max(e.largestPoleRadius, sectionPoleRadius(s[3], s[4]));
        if (e.largestPoleRadius > 1.0 + 1e-12 || (e.fixed && e.largestPoleRadius >= 1.0))
        {
            e.error = std::string("Arrondis en ") + formatName + ", les coefficients mettent un pôle à |z| = " +
                      plot::formatFixed(e.largestPoleRadius, 6) + ", hors du cercle unité : le filtre deviendrait instable. " +
                      (e.fixed ? "Prenez Q31 ou du flottant." : "Prenez double.");
            e.sections.clear();
            return e;
        }
    }

    // ---- What rounding does ----
    // The responses on 2 048 frequencies up to fs/2.
    {
        constexpr int kCount = 2048;
        std::vector<double> designed(kCount + 1), embedded(kCount + 1);
        double largest = 0.0, worst = 0.0;
        for (int i = 0; i <= kCount; ++i)
        {
            const double f = 0.5 * fs * static_cast<double>(i) / kCount;
            const Complex h = design.response(f);
            const Complex q = embeddedResponse(e, f);
            designed[static_cast<std::size_t>(i)] = std::abs(h);
            embedded[static_cast<std::size_t>(i)] = std::abs(q);
            if (std::isfinite(std::abs(h))) largest = std::max(largest, std::abs(h));
            if (std::isfinite(std::abs(q - h))) worst = std::max(worst, std::abs(q - h));
        }
        e.peakGain = largest;
        for (std::size_t i = 0; i < designed.size(); ++i)
            if (designed[i] >= (1.0 - 1e-9) * largest)
            {
                e.peakFrequencyHz = 0.5 * fs * static_cast<double>(i) / kCount;
                break;
            }
        e.deviationDb = largest > 0.0 && worst > 0.0 ? 20.0 * std::log10(worst / largest) : -400.0;
        // Where the design is 60 dB or more below its largest gain: the highest the rounded response gets above it
        // (above −120 dB, and twice the design at least), and the design there.
        for (std::size_t i = 0; i < designed.size(); ++i)
        {
            if (designed[i] >= 0.5 * largest && embedded[i] > 0.0)
                e.passbandDeviationDb = std::max(e.passbandDeviationDb, std::abs(20.0 * std::log10(embedded[i] / designed[i])));
            if (designed[i] <= 1e-3 * largest && embedded[i] > 2.0 * designed[i] && embedded[i] > 1e-6 * largest &&
                embedded[i] / largest > e.stopbandEmbedded)
            {
                e.stopbandEmbedded = embedded[i] / largest;
                e.stopbandDesigned = designed[i] / largest;
            }
        }
    }
    if (e.fir)
    {
        // The zeros of a FIR do not make it unstable; the pole radius stays 0.
        e.largestPoleRadius = 0.0;
    }

    // Computing noise: white noise, at most half of full scale at the output, against double precision on the
    // same input (rounded to the format, in fixed point).
    {
        const std::size_t length =
            e.fir ? std::clamp<std::size_t>((std::size_t{1} << 26) / std::max<std::size_t>(1, e.taps.size()), 2048, 16384) : 16384;
        const double amplitude = 0.5 / std::max(1.0, e.peakGain);
        Noise noise;
        std::vector<double> x(length);
        for (double &v : x)
        {
            v = amplitude * noise.next();
            if (e.fixed) v = static_cast<double>(toFixed(v, e.bits)) / static_cast<double>(fullScale(e.bits));
        }
        std::vector<double> reference = e.fir ? dsp::firFilter(design.taps, x) : dsp::sosFilter(design.sections, x);
        for (double &v : reference) v *= g;
        RunCounts counts;
        const std::vector<double> y = run(e, x, counts);
        double signal = 0.0, error = 0.0;
        for (std::size_t i = 0; i < y.size(); ++i)
        {
            signal += reference[i] * reference[i];
            error += (y[i] - reference[i]) * (y[i] - reference[i]);
        }
        e.snrDb = !(error > 0.0) ? 400.0 : signal > 0.0 ? 10.0 * std::log10(signal / error) : -400.0;
    }

    // A sine at full scale (0,99) where the gain is largest: what saturates.
    if (e.fixed)
    {
        const std::size_t ring =
            e.fir ? e.taps.size() : dsp::sosRingLength(design.sections, 1e-3, std::size_t{1} << 18);
        const std::size_t length = std::clamp<std::size_t>(4 * ring, 4096, 65536);
        std::vector<double> sine(length);
        for (std::size_t i = 0; i < length; ++i)
            sine[i] = 0.99 * std::sin(dsp::kTwoPi * e.peakFrequencyHz / fs * static_cast<double>(i) + 0.3);
        RunCounts counts;
        run(e, sine, counts);
        e.saturations = counts.internal + counts.output;
        if (counts.internal > 0)
            e.warnings.push_back(std::to_string(counts.internal) + " échantillons ont saturé entre deux sections pour un "
                                 "sinus pleine échelle à " + quantity(e.peakFrequencyHz, "Hz") +
                                 " : prenez la mise à l'échelle « somme ».");
    }

    // ---- Said ----
    const std::size_t count = e.fir ? e.taps.size() : e.sections.size();
    if (e.fir)
    {
        const std::string fold = e.symmetry > 0 ? " symétriques, repliés (moitié moins de multiplications)"
                                 : e.symmetry < 0 ? " antisymétriques, repliés (moitié moins de multiplications)"
                                                  : "";
        if (e.fixed)
            e.notes.push_back(std::string("Virgule fixe ") + formatName + " : échantillons et coefficients " +
                              (e.bits == 16 ? "int16_t" : "int32_t") + " (−1 à 1 = " +
                              (e.bits == 16 ? "−32 768 à 32 767" : "−2 147 483 648 à 2 147 483 647") + "), accumulateur " +
                              (e.accumulatorBits == 32 ? "int32_t" : "int64_t") + " sans débordement possible ; RIF de " +
                              std::to_string(count) + " coefficients" + fold + ", en Q" + std::to_string(e.tapShift) +
                              (g != 1.0 ? ", gain en sortie compris" : "") + ".");
        else
            e.notes.push_back(codeFormatText(options.format) + " : RIF de " + std::to_string(count) + " coefficients" + fold +
                              (g != 1.0 ? ", puis le gain en sortie" : "") + ".");
    }
    else
    {
        std::string formats;
        if (e.fixed)
        {
            for (std::size_t s = 0; s < e.sectionShifts.size(); ++s)
                formats += (s > 0 ? ", Q" : "Q") + std::to_string(e.sectionShifts[s]);
            e.notes.push_back(std::string("Virgule fixe ") + formatName + " : échantillons et coefficients " +
                              (e.bits == 16 ? "int16_t" : "int32_t") + " (−1 à 1 = " +
                              (e.bits == 16 ? "−32 768 à 32 767" : "−2 147 483 648 à 2 147 483 647") + "), accumulateur " +
                              (e.accumulatorBits == 32 ? "int32_t" : "int64_t") + " sans débordement possible ; " +
                              std::to_string(count) + (count > 1 ? " sections" : " section") + " du second ordre en forme directe I" +
                              (count > 1 ? ", chacune dans son format (" + formats + ")." : ", en " + formats + "."));
            std::string factors;
            for (std::size_t s = 0; s < e.sectionScales.size(); ++s)
                factors += (s > 0 ? ", " : "") + plot::formatCompact(e.sectionScales[s], "", 3);
            e.notes.push_back(std::string(options.scaling == CodeScaling::Sum
                                              ? "Mise à l'échelle « somme » (L1) : rien ne peut déborder entre les sections, quelle que soit l'entrée"
                                              : "Mise à l'échelle « crête » (L∞) : un sinus pleine échelle reste sous 0,99 de la pleine échelle à la sortie de chaque section") +
                              " ; facteurs sur les b : " + factors +
                              (e.gainStage ? " ; puis le reste du gain en sortie, ×" + plot::formatCompact(e.outputGain, "", 4) +
                                                 " en Q" + std::to_string(e.gainShift)
                                           : std::string()) +
                              ".");
        }
        else
        {
            e.notes.push_back(codeFormatText(options.format) + " : " + std::to_string(count) +
                              (count > 1 ? " sections" : " section") + " du second ordre en forme directe II transposée" +
                              (g != 1.0 ? ", puis le gain en sortie" : "") + ".");
        }
    }
    // The response of the numbers stored.
    std::string response = "Coefficients arrondis : réponse à " +
                           (e.passbandDeviationDb < 1e-6 ? std::string("moins de 1e-6 dB") : decibels(e.passbandDeviationDb)) +
                           " près là où le gain est à moins de 6 dB de son maximum ; écart au pire à " +
                           (e.deviationDb < -300.0 ? std::string("moins de −300 dB") : decibels(e.deviationDb)) +
                           " sous le maximum.";
    e.notes.push_back(response);
    if (e.stopbandEmbedded > 0.0)
        e.notes.push_back("Dans la bande coupée, l'arrondi remonte le gain jusqu'à " +
                          decibels(20.0 * std::log10(e.stopbandEmbedded)) + " sous le maximum (conçu là : " +
                          (e.stopbandDesigned > 0.0 ? decibels(20.0 * std::log10(e.stopbandDesigned)) : std::string("rien")) +
                          ").");
    if (!e.fir)
        e.notes.push_back("Pôle le plus proche du cercle unité : " + plot::formatFixed(e.largestPoleRadius, 6) +
                          " (conçu : " + plot::formatFixed(radius, 6) + ").");
    e.notes.push_back("Bruit de calcul : rapport signal sur bruit de " +
                      (e.snrDb >= 300.0 ? std::string("plus de 300 dB") : decibels(e.snrDb)) +
                      " sur un bruit blanc, comparé au calcul en double précision.");
    if (e.fixed)
    {
        if (e.peakGain > 1.0001)
            e.notes.push_back("Gain crête de " + plot::formatCompact(e.peakGain, "", 3) + " (" +
                              gainDecibels(20.0 * std::log10(e.peakGain)) + ") à " + quantity(e.peakFrequencyHz, "Hz") +
                              " : une entrée au-delà de " + plot::formatCompact(1.0 / e.peakGain, "", 3) +
                              " de la pleine échelle y sature la sortie.");
        else
            e.notes.push_back("Gain crête de " + plot::formatCompact(e.peakGain, "", 3) +
                              " : une entrée pleine échelle ne sature pas la sortie.");
    }
    // What is too little.
    const double floorSnr = e.fixed ? (e.bits == 16 ? 40.0 : 80.0) : (e.bits == 32 ? 80.0 : 200.0);
    const std::string better = e.fixed ? (e.bits == 16 ? "Q31 ou du flottant" : "du flottant") : "double";
    if (e.snrDb < floorSnr)
        e.warnings.push_back("Rapport signal sur bruit de " + decibels(e.snrDb) + " seulement : en " + formatName +
                             ", ce filtre perd trop de précision" +
                             (e.fir ? std::string() : std::string(" (des pôles près du cercle unité, une coupure basse)")) +
                             ". Prenez " + better + ".");
    if (e.passbandDeviationDb > 0.1)
        e.warnings.push_back("Arrondis en " + std::string(formatName) + ", les coefficients changent la réponse de " +
                             decibels(e.passbandDeviationDb) + " là où le filtre passe. Prenez " + better + ".");
    return e;
}

// ---- The C ----

namespace
{
std::string upper(std::string_view name)
{
    std::string out(name);
    for (char &c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}

// Shortest digits that read back the same value; a literal of C (a point or an exponent; f for a float).
std::string floatLiteral(double v, bool single)
{
    std::array<char, 48> buffer{};
    const auto end = single ? std::to_chars(buffer.data(), buffer.data() + buffer.size(), static_cast<float>(v)).ptr
                            : std::to_chars(buffer.data(), buffer.data() + buffer.size(), v).ptr;
    std::string text(buffer.data(), end);
    if (text.find_first_of(".eE") == std::string::npos) text += ".0";
    return single ? text + "f" : text;
}

// An integer literal; the most negative one written so that no constant overflows.
std::string integerLiteral(std::int64_t v, int bits)
{
    if (v == -fullScale(bits)) return "(-" + std::to_string(fullScale(bits) - 1) + " - 1)";
    return std::to_string(v);
}

// Values joined, a few per line, indented.
std::string joined(const std::vector<std::string> &items, std::size_t perLine, const std::string &indent)
{
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        if (i % perLine == 0) out += indent;
        out += items[i];
        if (i + 1 < items.size()) out += (i % perLine == perLine - 1) ? ",\n" : ", ";
    }
    return out;
}

// A comment block: each line after " * ", wrapped at 100 columns.
std::string commentLines(const std::vector<std::string> &lines)
{
    std::string out;
    for (const std::string &line : lines)
    {
        if (line.empty())
        {
            out += " *\n";
            continue;
        }
        std::string rest = line;
        bool first = true;
        while (!rest.empty())
        {
            const std::size_t room = first ? 96 : 94;
            std::size_t cut = rest.size();
            // Measured in bytes: an accented letter takes two, the lines a little shorter than counted.
            if (rest.size() > room)
            {
                // Not before a French : ; ! ? » (they keep their space before), nor after «.
                cut = rest.rfind(' ', room);
                while (cut != std::string::npos && cut > 0 &&
                       (std::string_view(":;!?").find(rest[cut + 1]) != std::string_view::npos ||
                        rest.compare(cut + 1, 2, "\xC2\xBB") == 0 || (cut >= 2 && rest.compare(cut - 2, 2, "\xC2\xAB") == 0)))
                    cut = rest.rfind(' ', cut - 1);
                if (cut == std::string::npos || cut == 0) cut = room;
            }
            out += (first ? " * " : " *   ") + rest.substr(0, cut) + "\n";
            rest = cut < rest.size() ? rest.substr(cut + (rest[cut] == ' ' ? 1 : 0)) : std::string();
            first = false;
        }
    }
    return out;
}
} // namespace

GeneratedCode generateCode(const EmbeddedFilter &e, const FilterDesign &design)
{
    GeneratedCode out;
    if (!e.ok())
    {
        out.error = e.error.empty() ? std::string("Pas de filtre à écrire en C.") : e.error;
        return out;
    }
    const std::string n = e.options.name;
    const std::string N = upper(n);
    out.headerName = n + ".h";
    out.sourceName = n + ".c";
    const bool single = !e.fixed && e.bits == 32;
    const std::string sample = e.fixed ? (e.bits == 16 ? "int16_t" : "int32_t") : (single ? "float" : "double");
    const std::string coefficient = sample;
    const std::string accumulator = e.fixed ? (e.accumulatorBits == 32 ? "int32_t" : "int64_t") : sample;
    const std::string zero = e.fixed ? "0" : (single ? "0.0f" : "0.0");
    const std::size_t taps = e.taps.size();
    const std::size_t sections = e.sections.size();

    // The self-test: an impulse, then a sine and noise, within half of full scale over the largest gain; the
    // outputs as simulated here.
    std::vector<double> testInput(kCodeTestLength);
    {
        Noise noise;
        const double amplitude = 0.5 / std::max(1.0, e.peakGain);
        for (std::size_t i = 0; i < kCodeTestLength; ++i)
        {
            const double v = i == 0 ? amplitude : amplitude * (0.6 * std::sin(0.7 * static_cast<double>(i)) + 0.4 * noise.next());
            testInput[i] = e.fixed ? static_cast<double>(toFixed(v, e.bits)) / static_cast<double>(fullScale(e.bits))
                           : single ? static_cast<double>(static_cast<float>(v))
                                    : v;
        }
    }
    const std::vector<double> testOutput = runEmbedded(e, testInput);
    // Then an impulse, followed by zeros for as long as the filter answers (every tap of a FIR, the tail of an
    // IIR, a limit cycle of fixed point included): Σ (n + 1)·y[n], exact in fixed point.
    const std::size_t impulseLength =
        e.fir ? taps + 1
              : std::clamp<std::size_t>(dsp::sosRingLength(design.sections, 1e-6, 4096), 64, 4096);
    const double impulseLevel = [&e, single] {
        const double v = 0.5 / std::max(1.0, e.peakGain);
        return e.fixed ? static_cast<double>(toFixed(v, e.bits)) / static_cast<double>(fullScale(e.bits))
               : single ? static_cast<double>(static_cast<float>(v))
                        : v;
    }();
    std::vector<double> impulse(impulseLength, 0.0);
    impulse[0] = impulseLevel;
    const std::vector<double> impulseOutput = runEmbedded(e, impulse);
    std::int64_t weightedInteger = 0;
    double weighted = 0.0, weightedSize = 0.0;
    for (std::size_t i = 0; i < impulseOutput.size(); ++i)
    {
        const double w = static_cast<double>(i + 1);
        if (e.fixed) weightedInteger += static_cast<std::int64_t>(i + 1) * toFixed(impulseOutput[i], e.bits);
        weighted += w * impulseOutput[i];
        weightedSize += w * std::abs(impulseOutput[i]);
    }
    const auto literals = [&e, single](const std::vector<double> &values) {
        std::vector<std::string> items;
        for (double v : values)
            items.push_back(e.fixed ? integerLiteral(toFixed(v, e.bits), e.bits) : floatLiteral(v, single));
        return items;
    };
    double largestOutput = 0.0;
    for (double v : testOutput) largestOutput = std::max(largestOutput, std::abs(v));

    // ---- The header ----
    std::vector<std::string> about;
    about.push_back(out.headerName + " : " + e.text + ", fs = " + quantity(e.sampleRateHz, "Hz", 6) + ".");
    about.push_back("Écrit par QTacquisitionApp (module Filtres), en C99 pour microcontrôleur.");
    about.push_back("");
    for (const std::string &note : e.notes) about.push_back(note);
    for (std::string warning : e.warnings)
    {
        if (!warning.empty() && warning[0] >= 'A' && warning[0] <= 'Z') warning[0] = static_cast<char>(warning[0] - 'A' + 'a');
        about.push_back("Attention : " + warning);
    }
    if (design.recipe.application != FilterApplication::Causal)
        about.push_back("Dans l'application, il est appliqué " +
                        std::string(design.recipe.application == FilterApplication::ZeroPhase ? "à phase nulle (aller puis retour)"
                                                                                               : "sans son retard") +
                        " ; un microcontrôleur le passe en causal, échantillon après échantillon : sa phase et son "
                        "retard restent.");
    about.push_back("");
    about.push_back("Utilisation :");
    about.push_back("    static " + n + "_t filtre;");
    about.push_back("    " + n + "_init(&filtre);");
    about.push_back("    y = " + n + "_echantillon(&filtre, x);       à chaque échantillon (interruption de l'ADC...)");
    about.push_back("    " + n + "_bloc(&filtre, entree, sortie, n);  ou sur un bloc");
    if (e.fixed)
        about.push_back(std::string("Échantillons en Q") + (e.bits == 16 ? "15" : "31") + " : la valeur v (−1 ≤ v < 1) s'écrit v × " +
                        (e.bits == 16 ? "32 768" : "2 147 483 648") + " ; d'un ADC de 12 bits (0 à 4 095) : x = (" + sample +
                        ")(mesure - 2048) * " + (e.bits == 16 ? "16" : "1048576") + ".");
    about.push_back(n + "_autotest() rend 0 si ce microcontrôleur et son compilateur donnent ce que donne l'application " +
                    (e.fixed ? "(au bit près)" : "(à l'arrondi près)") + ", sinon le nombre d'écarts.");
    if (e.fixed)
        about.push_back("Le décalage à droite d'un entier négatif est supposé arithmétique (celui de tous les compilateurs "
                        "pour microcontrôleur).");

    std::string h;
    h += "/*\n" + commentLines(about) + " */\n";
    h += "#ifndef " + N + "_H\n#define " + N + "_H\n\n#include <stdint.h>\n\n";
    h += "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";
    h += "/* La fréquence d'échantillonnage pour laquelle il est conçu, en Hz. */\n";
    h += "#define " + N + "_FE " + floatLiteral(e.sampleRateHz, false) + "\n";
    if (e.fir)
    {
        h += "/* Nombre de coefficients. */\n#define " + N + "_N " + std::to_string(taps) + "\n\n";
        h += "typedef struct\n{\n    " + sample + " ligne[2 * " + N + "_N]; /* chaque échantillon deux fois : les N derniers se suivent */\n";
        h += "    int position;\n} " + n + "_t;\n\n";
    }
    else
    {
        h += "/* Nombre de sections du second ordre. */\n#define " + N + "_SECTIONS " + std::to_string(sections) + "\n\n";
        if (e.fixed)
        {
            h += "typedef struct\n{\n    " + sample + " x1, x2, y1, y2; /* x[n-1], x[n-2], y[n-1], y[n-2] */\n} " + n +
                 "_section_t;\n\n";
            h += "typedef struct\n{\n    " + n + "_section_t s[" + N + "_SECTIONS];\n} " + n + "_t;\n\n";
        }
        else
        {
            h += "typedef struct\n{\n    " + sample + " z[" + N + "_SECTIONS][2]; /* état de chaque section */\n} " + n +
                 "_t;\n\n";
        }
    }
    h += "void " + n + "_init(" + n + "_t *f);\n";
    h += sample + " " + n + "_echantillon(" + n + "_t *f, " + sample + " x);\n";
    h += "void " + n + "_bloc(" + n + "_t *f, const " + sample + " *x, " + sample + " *y, int n);\n";
    h += "int " + n + "_autotest(void);\n\n";
    h += "#ifdef __cplusplus\n}\n#endif\n\n#endif /* " + N + "_H */\n";
    out.header = h;

    // ---- The source ----
    std::string c;
    c += "/* " + out.sourceName + " : voir " + out.headerName + ". */\n#include \"" + out.headerName + "\"\n\n";
    if (e.fixed)
    {
        const std::string top = e.bits == 16 ? "INT16_MAX" : "INT32_MAX";
        const std::string bottom = e.bits == 16 ? "INT16_MIN" : "INT32_MIN";
        c += "/* La valeur ramenée dans ce que " + sample + " représente. */\n";
        c += "static " + sample + " " + n + "_sature(" + accumulator + " v)\n{\n";
        c += "    if (v > " + top + ")\n        return " + top + ";\n";
        c += "    if (v < " + bottom + ")\n        return " + bottom + ";\n";
        c += "    return (" + sample + ")v;\n}\n\n";
    }
    if (e.fir)
    {
        const std::size_t stored = storedTaps(e);
        std::vector<std::string> items;
        for (std::size_t k = 0; k < stored; ++k)
            items.push_back(e.fixed ? integerLiteral(e.tapIntegers[k], e.bits) : floatLiteral(e.taps[k], single));
        if (e.symmetry != 0)
            c += "/* h[0] … h[" + std::to_string(stored - 1) + "] ; les autres s'en déduisent : h[N - 1 - k] = " +
                 (e.symmetry > 0 ? "h[k]" : "-h[k]") + ". */\n";
        else
            c += "/* h[0] … h[N - 1] : y[n] = h[0] x[n] + h[1] x[n-1] + … */\n";
        if (e.fixed)
            c += "/* En Q" + std::to_string(e.tapShift) + " : la valeur h s'écrit h × 2^" + std::to_string(e.tapShift) +
                 " ; l'accumulateur, décalé de " + std::to_string(e.tapShift) + " à droite, donne la sortie. */\n";
        c += "static const " + coefficient + " " + n + "_h[" + std::to_string(stored) + "] = {\n" + joined(items, 6, "    ") + "\n};\n";
        if (!e.fixed && e.outputGain != 1.0)
            c += "static const " + sample + " " + n + "_gain = " + floatLiteral(e.outputGain, single) + ";\n";
        c += "\nvoid " + n + "_init(" + n + "_t *f)\n{\n    int i;\n    for (i = 0; i < 2 * " + N + "_N; ++i)\n        f->ligne[i] = " +
             zero + ";\n    f->position = 0;\n}\n\n";
        c += sample + " " + n + "_echantillon(" + n + "_t *f, " + sample + " x)\n{\n";
        c += "    const " + sample + " *w;\n    " + accumulator + " acc = " + zero + ";\n    int k;\n";
        c += "    f->position = f->position == 0 ? " + N + "_N - 1 : f->position - 1;\n";
        c += "    f->ligne[f->position] = x;\n    f->ligne[f->position + " + N + "_N] = x;\n";
        c += "    w = f->ligne + f->position; /* w[k] = x[n - k] */\n";
        const std::string cast = e.fixed ? "(" + accumulator + ")" : "";
        if (e.symmetry == 0)
        {
            c += "    for (k = 0; k < " + N + "_N; ++k)\n        acc += " + cast + n + "_h[k] * w[k];\n";
        }
        else
        {
            const std::string pair = e.symmetry > 0 ? " + " : " - ";
            c += "    for (k = 0; k < " + N + "_N / 2; ++k)\n        acc += " + cast + n + "_h[k] * (" + cast + "w[k]" + pair +
                 "w[" + N + "_N - 1 - k]);\n";
            if (taps % 2 == 1 && e.symmetry > 0)
                c += "    acc += " + cast + n + "_h[" + N + "_N / 2] * w[" + N + "_N / 2];\n";
        }
        if (e.fixed)
            c += "    return " + n + "_sature((acc + ((" + accumulator + ")1 << " + std::to_string(e.tapShift - 1) + ")) >> " +
                 std::to_string(e.tapShift) + ");\n}\n";
        else
            c += std::string(e.outputGain != 1.0 ? "    return acc * " + n + "_gain;\n}\n" : "    return acc;\n}\n");
    }
    else
    {
        std::vector<std::string> rows;
        for (std::size_t s = 0; s < sections; ++s)
        {
            std::vector<std::string> items;
            for (std::size_t i = 0; i < 5; ++i)
                items.push_back(e.fixed ? integerLiteral(e.sectionIntegers[s][i], e.bits) : floatLiteral(e.sections[s][i], single));
            std::string row = "    { " + joined(items, 5, "") + " }";
            if (e.fixed) row += s + 1 < sections ? ", /* Q" + std::to_string(e.sectionShifts[s]) + " */" : "  /* Q" + std::to_string(e.sectionShifts[s]) + " */";
            else if (s + 1 < sections) row += ",";
            rows.push_back(row);
        }
        c += "/* b0, b1, b2, a1, a2 de chaque section (a0 = 1) :\n";
        c += " * y[n] = b0 x[n] + b1 x[n-1] + b2 x[n-2] - a1 y[n-1] - a2 y[n-2]";
        c += e.fixed ? ", la sortie de l'une entrant dans la suivante.\n * Chaque section dans son format Qd : la valeur c s'écrit c × 2^d, "
                       "et l'accumulateur décalé de d à droite\n * donne sa sortie (les b mis à l'échelle). */\n"
                     : ", la sortie de l'une entrant dans la suivante. */\n";
        c += "static const " + coefficient + " " + n + "_coefficients[" + N + "_SECTIONS][5] = {\n";
        for (const std::string &row : rows) c += row + "\n";
        c += "};\n";
        if (e.fixed)
        {
            std::vector<std::string> shifts;
            for (int d : e.sectionShifts) shifts.push_back(std::to_string(d));
            c += "static const uint8_t " + n + "_decalage[" + N + "_SECTIONS] = { " + joined(shifts, 16, "") + " };\n";
            if (e.gainStage)
                c += "/* Le reste du gain en sortie, " + plot::formatCompact(e.outputGain, "", 6) + ", en Q" + std::to_string(e.gainShift) +
                     ". */\nstatic const " + coefficient + " " + n + "_gain = " + integerLiteral(e.gainInteger, e.bits) + ";\n";
        }
        else if (e.outputGain != 1.0)
        {
            c += "static const " + sample + " " + n + "_gain = " + floatLiteral(e.outputGain, single) + ";\n";
        }
        c += "\nvoid " + n + "_init(" + n + "_t *f)\n{\n    int k;\n    for (k = 0; k < " + N + "_SECTIONS; ++k)\n    {\n";
        if (e.fixed)
            c += "        f->s[k].x1 = 0;\n        f->s[k].x2 = 0;\n        f->s[k].y1 = 0;\n        f->s[k].y2 = 0;\n";
        else
            c += "        f->z[k][0] = " + zero + ";\n        f->z[k][1] = " + zero + ";\n";
        c += "    }\n}\n\n";
        c += sample + " " + n + "_echantillon(" + n + "_t *f, " + sample + " x)\n{\n    int k;\n";
        c += "    for (k = 0; k < " + N + "_SECTIONS; ++k)\n    {\n";
        c += "        const " + coefficient + " *c = " + n + "_coefficients[k];\n";
        if (e.fixed)
        {
            const std::string a = "(" + accumulator + ")";
            c += "        " + n + "_section_t *s = &f->s[k];\n";
            c += "        const int d = " + n + "_decalage[k];\n";
            c += "        const " + accumulator + " acc = " + a + "c[0] * x + " + a + "c[1] * s->x1 + " + a + "c[2] * s->x2\n";
            c += "                          - " + a + "c[3] * s->y1 - " + a + "c[4] * s->y2;\n";
            c += "        const " + sample + " y = " + n + "_sature((acc + (" + a + "1 << (d - 1))) >> d);\n";
            c += "        s->x2 = s->x1;\n        s->x1 = x;\n        s->y2 = s->y1;\n        s->y1 = y;\n        x = y;\n";
        }
        else
        {
            c += "        const " + sample + " y = c[0] * x + f->z[k][0];\n";
            c += "        f->z[k][0] = c[1] * x - c[3] * y + f->z[k][1];\n";
            c += "        f->z[k][1] = c[2] * x - c[4] * y;\n";
            c += "        x = y;\n";
        }
        c += "    }\n";
        if (e.fixed && e.gainStage)
            c += "    return " + n + "_sature(((" + accumulator + ")" + n + "_gain * x + ((" + accumulator + ")1 << " +
                 std::to_string(e.gainShift - 1) + ")) >> " + std::to_string(e.gainShift) + ");\n}\n";
        else
            c += !e.fixed && e.outputGain != 1.0 ? "    return x * " + n + "_gain;\n}\n" : "    return x;\n}\n";
    }
    c += "\nvoid " + n + "_bloc(" + n + "_t *f, const " + sample + " *x, " + sample + " *y, int n)\n{\n    int i;\n";
    c += "    for (i = 0; i < n; ++i)\n        y[i] = " + n + "_echantillon(f, x[i]);\n}\n\n";

    // The self-test.
    const std::string length = std::to_string(kCodeTestLength);
    c += "/* Autotest : ces entrées donnent, dans l'application, ces sorties" +
         std::string(e.fixed ? " (au bit près)." : " (à l'arrondi près).") + " */\n";
    c += "static const " + sample + " " + n + "_test_entree[" + length + "] = {\n" + joined(literals(testInput), 6, "    ") + "\n};\n";
    c += "static const " + sample + " " + n + "_test_sortie[" + length + "] = {\n" + joined(literals(testOutput), 6, "    ") + "\n};\n\n";
    c += "int " + n + "_autotest(void)\n{\n    " + n + "_t f;\n    int i, ecarts = 0;\n    " + n + "_init(&f);\n";
    c += "    for (i = 0; i < " + length + "; ++i)\n    {\n";
    if (e.fixed)
    {
        c += "        if (" + n + "_echantillon(&f, " + n + "_test_entree[i]) != " + n + "_test_sortie[i])\n            ++ecarts;\n";
    }
    else
    {
        // float: a fused multiply-add, the order of a compiler, a few ulp; far less than a wrong coefficient.
        const double tolerance = (single ? 1e-4 : 1e-10) * std::max(1.0, largestOutput);
        c += "        const " + sample + " y = " + n + "_echantillon(&f, " + n + "_test_entree[i]);\n";
        c += "        const " + sample + " d = y > " + n + "_test_sortie[i] ? y - " + n + "_test_sortie[i] : " + n +
             "_test_sortie[i] - y;\n";
        c += "        if (!(d <= " + floatLiteral(tolerance, single) + "))\n            ++ecarts;\n";
    }
    c += "    }\n";
    c += "    /* Une impulsion, puis des zéros, " + std::to_string(impulseLength) +
         " échantillons : la somme des (n + 1) y[n], tous les coefficients y comptent. */\n";
    c += "    " + n + "_init(&f);\n";
    if (e.fixed)
    {
        c += "    {\n        int64_t somme = 0;\n";
        c += "        for (i = 0; i < " + std::to_string(impulseLength) + "; ++i)\n";
        c += "            somme += (int64_t)(i + 1) * " + n + "_echantillon(&f, (" + sample + ")(i == 0 ? " +
             integerLiteral(toFixed(impulseLevel, e.bits), e.bits) + " : 0));\n";
        c += "        if (somme != " + std::to_string(weightedInteger) + (e.bits == 32 || std::abs(weightedInteger) > 2147483647 ? "LL" : "") +
             ")\n            ++ecarts;\n    }\n";
    }
    else
    {
        // A fused multiply-add changes the last bits, and a recursion carries them on: a high-pass of order 4 in
        // float moves by 5e-6 of the sum of |(n + 1) y[n]|; a FIR, by 1e-7.
        const double tolerance = (single ? (e.fir ? 1e-5 : 1e-4) : (e.fir ? 1e-12 : 1e-10)) * std::max(weightedSize, 1e-30);
        c += "    {\n        double somme = 0.0, d;\n";
        c += "        for (i = 0; i < " + std::to_string(impulseLength) + "; ++i)\n";
        c += "            somme += (double)(i + 1) * (double)" + n + "_echantillon(&f, i == 0 ? " +
             floatLiteral(impulseLevel, single) + " : " + zero + ");\n";
        c += "        d = somme > " + floatLiteral(weighted, false) + " ? somme - " + floatLiteral(weighted, false) + " : " +
             floatLiteral(weighted, false) + " - somme;\n";
        c += "        if (!(d <= " + floatLiteral(tolerance, false) + "))\n            ++ecarts;\n    }\n";
    }
    c += "    return ecarts;\n}\n";
    out.source = c;
    return out;
}
} // namespace qacq::analysis
