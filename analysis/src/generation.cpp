#include "qacq/analysis/generation.h"

#include "qacq/analysis/expression.h"
#include "qacq/analysis/playback.h"
#include "qacq/dsp/constants.h"
#include "qacq/dsp/fft.h"
#include "qacq/dsp/modulation.h"
#include "qacq/plot/axis.h"

#include <algorithm>
#include <charconv>
#include <array>
#include <cmath>
#include <complex>
#include <iterator>
#include <limits>
#include <memory>
#include <utility>

namespace qacq::analysis
{
namespace
{
using Kind = WaveKind;

constexpr std::pair<Kind, std::string_view> kKeys[] = {
    {Kind::Sine, "sine"},         {Kind::Square, "square"}, {Kind::Triangle, "triangle"},
    {Kind::Sawtooth, "sawtooth"}, {Kind::Pulse, "pulse"},   {Kind::Dc, "dc"},
    {Kind::Step, "step"},         {Kind::Impulse, "impulse"}, {Kind::Noise, "noise"},
    {Kind::Sweep, "sweep"},       {Kind::FourierSeries, "fourier"}, {Kind::Drawn, "drawn"},
    {Kind::Recurrence, "recurrence"}, {Kind::Formula, "formula"}, {Kind::Modulation, "modulation"},
    {Kind::Recorded, "recorded"}, {Kind::Frame, "frame"}};

constexpr double kLargest = 1e15;              // amplitudes and offsets
constexpr double kHighestFrequency = 1e12;     // Hz
constexpr double kLargestHarmonics = 1'000'000; // terms of a Fourier series

std::string grouped(std::size_t value)
{
    std::string digits = std::to_string(value);
    for (std::size_t i = digits.size(); i > 3; i -= 3) digits.insert(i - 3, " ");
    return digits;
}

std::string quantity(double value, std::string_view unit)
{
    return plot::formatCompact(value, unit, 4);
}

// The number under `key` if it is a finite number, within [low, high]; otherwise the fallback.
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
    double decade = std::pow(10.0, std::floor(std::log10(value)));
    if (decade * 10.0 <= value) decade *= 10.0; // log10 a hair under an integer
    if (decade > value) decade /= 10.0;
    const double m = value / decade;
    const double step = m >= 5.0 ? 5.0 : m >= 2.0 ? 2.0 : 1.0;
    return step * decade;
}

// 1, 2 or 5 times a power of ten, at least `value` (value > 0): the one after niceFloor (twice a 1 or a 5, two
// and a half times a 2), so that it is as exact as niceFloor is.
double niceCeil(double value)
{
    const double below = niceFloor(value);
    if (below >= value) return below;
    const double m = below / std::pow(10.0, std::floor(std::log10(below))); // 1, 2 or 5 (10: log10 a hair under an integer)
    return below * (m > 1.5 && m < 3.5 ? 2.5 : 2.0);
}

bool hasSeries(Kind w) noexcept
{
    return w == Kind::Square || w == Kind::Triangle || w == Kind::Sawtooth || w == Kind::Pulse;
}

// The periodic waveforms of qacq::dsp::generate (a Fourier series has a period too, but no phase).
bool periodic(Kind w) noexcept
{
    return w == Kind::Sine || hasSeries(w);
}

bool takesPhase(Kind w) noexcept
{
    return periodic(w) || w == Kind::Sweep;
}

std::string channelName(const GeneratorChannel &channel, std::size_t index)
{
    return channel.name.empty() ? "Voie " + std::to_string(index + 1) : channel.name;
}

dsp::WaveformSpec specOf(const GeneratorComponent &c, double fs, std::size_t n, std::uint64_t seed)
{
    dsp::WaveformSpec spec;
    dspWaveform(c.waveform, spec.waveform);
    spec.amplitude = c.amplitude;
    spec.frequencyHz = c.frequencyHz;
    spec.phaseDegrees = c.phaseDegrees;
    spec.offset = 0.0;
    spec.dutyCycle = c.dutyCycle;
    spec.sampleRateHz = fs;
    spec.sampleCount = n;
    spec.bandLimit = hasSeries(c.waveform) ? c.bandLimit : dsp::BandLimit::Sampled;
    spec.maximumHarmonics = c.harmonics;
    spec.eventTimeSeconds = c.eventSeconds;
    spec.chirpEndFrequencyHz = c.endFrequencyHz;
    spec.chirpLaw = c.sweepLaw;
    spec.noise = c.noise;
    spec.seed = seed;
    return spec;
}

std::string_view trim(std::string_view s) noexcept
{
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}

// The formula of a term as typed, without what may come before it: "=" (as in a spreadsheet) or the name
// of the term ("bn = 4/(pi*n)", "Re(Cn) = …"). Empty: 0.
std::string formulaBody(std::string_view text, int which)
{
    std::string_view t = trim(text);
    std::size_t equal = std::string_view::npos;
    for (std::size_t i = 0; i < t.size(); ++i)
    {
        if (t[i] != '=') continue;
        const char before = i > 0 ? t[i - 1] : ' ';
        const char after = i + 1 < t.size() ? t[i + 1] : ' ';
        if (before == '<' || before == '>' || before == '!' || before == '=' || after == '=') continue;
        equal = i;
        break;
    }
    if (equal != std::string_view::npos)
    {
        std::string left;
        for (char c : t.substr(0, equal))
            if (c != ' ' && c != '_' && c != '{' && c != '}' && c != '(' && c != ')' && c != '[' && c != ']') left += c;
        static const std::vector<std::string> names[3] = {
            {"", "a0", "A0", "C0", "c0", "a\xE2\x82\x80", "A\xE2\x82\x80", "C\xE2\x82\x80", "c\xE2\x82\x80"},
            {"", "an", "An", "ReCn", "Recn", "|Cn|", "|cn|", "Cn", "modCn", "a\xE2\x82\x99", "A\xE2\x82\x99",
             "ReC\xE2\x82\x99", "|C\xE2\x82\x99|"},
            {"", "bn", "Bn", "ImCn", "Imcn", "argCn", "phin", "phasen", "b\xE2\x82\x99", "B\xE2\x82\x99",
             "ImC\xE2\x82\x99", "argC\xE2\x82\x99", "\xCF\x86n", "\xCF\x86\xE2\x82\x99"}};
        const std::vector<std::string> &accepted = names[std::clamp(which, 0, 2)];
        if (std::find(accepted.begin(), accepted.end(), left) != accepted.end()) t = trim(t.substr(equal + 1));
    }
    return t.empty() ? std::string("0") : std::string(t);
}

struct ComponentSamples
{
    std::vector<double> samples;
    std::vector<std::string> warnings;
    std::string error;
    bool cancelled = false;  // stopped half way (a long recurrence)
    bool readsAbove = false; // an operation or a modulated carrier of x: it replaces the components above
};

// A waveform of qacq::dsp::generate; for its Fourier series, the budget of computation.
ComponentSamples waveformSamples(const GeneratorComponent &component, double fs, std::size_t n, std::uint64_t seed)
{
    ComponentSamples out;
    dsp::WaveformSpec spec = specOf(component, fs, n, seed);
    std::string shortened;
    if (spec.bandLimit == dsp::BandLimit::Fourier && std::isfinite(component.frequencyHz) && component.frequencyHz > 0.0)
    {
        // As many terms as generate() would take, then the budget of computation.
        const double below = std::floor((0.5 - 1e-12) * fs / component.frequencyHz);
        const double wanted = component.harmonics > 0 ? std::min(below, static_cast<double>(component.harmonics)) : below;
        const double budget = std::max(1.0, std::floor(kGeneratorFourierBudget / static_cast<double>(n)));
        if (wanted > budget)
        {
            spec.maximumHarmonics = static_cast<std::size_t>(budget);
            shortened = "Série de Fourier limitée à " + grouped(spec.maximumHarmonics) + " harmoniques sur " +
                        grouped(static_cast<std::size_t>(wanted)) +
                        " (temps de calcul) : les plus hautes manquent ; baissez la fréquence d'échantillonnage ou la durée.";
        }
    }
    dsp::GeneratedSignal g = dsp::generate(spec);
    out.error = std::move(g.error);
    out.warnings = std::move(g.warnings);
    if (!shortened.empty()) out.warnings.push_back(std::move(shortened));
    out.samples = std::move(g.samples);
    return out;
}

// Adds Σ aₙ cos(2πnu) + bₙ sin(2πnu), u = ratio i + phase (in turns), to sample i: Re Σ cₙ zⁿ with
// cₙ = aₙ − j bₙ and z = exp(j2πu), by Horner's scheme (|z| = 1: no growth), z recomputed exactly every 256
// samples.
void addSeries(std::vector<double> &samples, std::span<const double> a, std::span<const double> b, double ratio,
               double phase)
{
    const std::size_t count = std::min(a.size(), b.size());
    if (count == 0) return;
    std::vector<double> cr(count), ci(count);
    for (std::size_t k = 0; k < count; ++k)
    {
        cr[k] = a[k];
        ci[k] = -b[k];
    }
    const double stepR = std::cos(dsp::kTwoPi * ratio), stepI = std::sin(dsp::kTwoPi * ratio);
    double zr = 1.0, zi = 0.0;
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        if (i % 256 == 0)
        {
            const double turns = ratio * static_cast<double>(i) + phase;
            const double angle = dsp::kTwoPi * (turns - std::floor(turns));
            zr = std::cos(angle);
            zi = std::sin(angle);
        }
        else
        {
            const double r = zr * stepR - zi * stepI;
            zi = zr * stepI + zi * stepR;
            zr = r;
        }
        double sr = 0.0, si = 0.0;
        for (std::size_t k = count; k-- > 0;)
        {
            const double ar = sr + cr[k], ai = si + ci[k];
            sr = ar * zr - ai * zi;
            si = ar * zi + ai * zr;
        }
        samples[i] += sr;
    }
}

// A Fourier series given by its coefficients (addSeries).
ComponentSamples seriesSamples(const GeneratorComponent &c, double fs, std::size_t n)
{
    ComponentSamples out;
    const double f0 = c.frequencyHz;
    if (!(std::isfinite(f0) && f0 > 0.0))
    {
        out.error = "La fondamentale f0 doit être positive.";
        return out;
    }
    std::size_t count = seriesHarmonics(c, fs);
    const auto below = static_cast<std::size_t>(std::max(0.0, std::floor((0.5 - 1e-12) * fs / f0)));
    if (count > below)
    {
        if (c.bandLimit == dsp::BandLimit::Fourier)
        {
            out.warnings.push_back(below == 0 ? std::string("f0 atteint fs/2 : aucun harmonique en dessous, seul le terme constant reste.")
                                              : "Harmoniques " + grouped(below + 1) + " à " + grouped(count) +
                                                    " au-delà de fs/2 laissés de côté (décochez « Sans repliement » pour les replier).");
            count = below;
        }
        else
        {
            out.warnings.push_back("Les harmoniques de rang " + grouped(below + 1) + " et plus dépassent fs/2 : ils se replient.");
        }
    }
    const auto budget = static_cast<std::size_t>(std::max(1.0, std::floor(kGeneratorFourierBudget / static_cast<double>(n))));
    if (count > budget)
    {
        out.warnings.push_back("Série limitée à " + grouped(budget) + " harmoniques sur " + grouped(count) +
                               " (temps de calcul) : baissez la fréquence d'échantillonnage ou la durée.");
        count = budget;
    }
    const SeriesCoefficients s = seriesCoefficients(c, fs, count);
    if (!s.ok())
    {
        out.error = s.error;
        out.warnings.clear();
        return out;
    }
    out.samples.assign(n, s.dc);
    addSeries(out.samples, s.a, s.b, f0 / fs, 0.0);
    return out;
}

std::string notesOf(const GeneratorRecipe &recipe)
{
    std::string notes = "Généré à " + quantity(recipe.sampleRateHz, "Hz") + " pendant " +
                        quantity(recipe.durationSeconds, "s") + ".";
    for (std::size_t c = 0; c < recipe.channels.size(); ++c)
    {
        const GeneratorChannel &channel = recipe.channels[c];
        std::string line;
        for (const GeneratorComponent &k : channel.components)
        {
            if (!k.enabled) continue;
            if (!line.empty()) line += " + ";
            line += componentText(k, channel.unit);
        }
        if (channel.offset != 0.0)
            line += (line.empty() ? "Continu " : " ; décalage ") + quantity(channel.offset, channel.unit);
        if (line.empty()) line = "nulle";
        notes += "\n" + channelName(channel, c) + (channel.output ? "" : " (voie de calcul, hors du signal)") + " : " + line + ".";
    }
    return notes;
}

GeneratorComponent component(Kind w, double amplitude, double frequency)
{
    GeneratorComponent c = defaultGeneratorComponent(w);
    c.amplitude = amplitude;
    c.frequencyHz = frequency;
    return c;
}

GeneratorRecipe recipeOf(std::string name, double rate, double duration, std::vector<GeneratorChannel> channels)
{
    GeneratorRecipe r;
    r.name = std::move(name);
    r.sampleRateHz = rate;
    r.durationSeconds = duration;
    r.channels = std::move(channels);
    return r;
}

GeneratorChannel channelOf(std::string name, std::string unit, std::vector<GeneratorComponent> components)
{
    GeneratorChannel c;
    c.name = std::move(name);
    c.unit = std::move(unit);
    c.components = std::move(components);
    return c;
}
} // namespace

GeneratorRecipe blankGeneratorRecipe()
{
    GeneratorRecipe r;
    r.channels.push_back(blankGeneratorChannel(0));
    return r;
}

GeneratorChannel blankGeneratorChannel(std::size_t index)
{
    GeneratorChannel c;
    c.name = "Voie " + std::to_string(index + 1);
    return c;
}

bool isBlankChannel(const GeneratorChannel &channel) noexcept
{
    return channel.components.empty() && channel.offset == 0.0;
}

bool isBlankRecipe(const GeneratorRecipe &recipe) noexcept
{
    return std::all_of(recipe.channels.begin(), recipe.channels.end(), [](const GeneratorChannel &c) { return isBlankChannel(c); });
}

GeneratorRecipe sineGeneratorRecipe()
{
    GeneratorRecipe r;
    r.channels.push_back(sineGeneratorChannel(0, r.sampleRateHz));
    return r;
}

GeneratorChannel sineGeneratorChannel(std::size_t index, double sampleRateHz)
{
    GeneratorChannel c;
    c.name = "Voie " + std::to_string(index + 1);
    c.components.push_back(defaultGeneratorComponent(Kind::Sine, sampleRateHz));
    return c;
}

GeneratorComponent defaultGeneratorComponent(Kind waveform, double sampleRateHz, double durationSeconds)
{
    const double fs = std::isfinite(sampleRateHz) && sampleRateHz > 0.0 ? sampleRateHz : 48000.0;
    const double duration = std::isfinite(durationSeconds) && durationSeconds > 0.0 ? durationSeconds : 1.0;
    GeneratorComponent c;
    c.waveform = waveform;
    c.frequencyHz = std::min(1000.0, niceFloor(fs / 20.0));
    c.dutyCycle = waveform == Kind::Pulse ? 0.1 : 0.5;
    c.amplitude = waveform == Kind::Noise ? 0.1 : 1.0;
    if (waveform == Kind::Step || waveform == Kind::Impulse) c.eventSeconds = duration / 10.0;
    if (waveform == Kind::Sweep)
    {
        c.endFrequencyHz = niceFloor(std::min(20000.0, 0.45 * fs));
        c.frequencyHz = std::min(20.0, niceFloor(c.endFrequencyHz / 100.0));
        c.sweepLaw = dsp::ChirpLaw::Logarithmic;
    }
    if (waveform == Kind::FourierSeries)
    {
        c.bandLimit = dsp::BandLimit::Fourier;
        c.harmonics = 15;
        applySeriesModel(c, "square");
    }
    if (waveform == Kind::Drawn) applyMotifModel(c, "trapezoid");
    if (waveform == Kind::Recurrence) applyRecurrenceModel(c, "oscillator", 1.0, c.frequencyHz);
    if (waveform == Kind::Modulation)
    {
        // A carrier of 10 kHz (at most fs/4), a message of a tenth of it, 50 % of AM, Δf of a fifth of it.
        c.frequencyHz = std::min(10000.0, niceFloor(fs / 4.0));
        c.variables = "fm = " + plot::formatCompact(c.frequencyHz / 10.0, "", 6);
        c.message = "sin(2*pi*fm*t)";
        c.deviationHz = c.frequencyHz / 5.0;
    }
    if (waveform == Kind::Frame)
    {
        // "Bonjour" as bits, at a rate a frame of eighty bits fits the signal at (eight samples a bit at least),
        // after a tenth of the duration.
        c.frequencyHz = std::min(niceCeil(80.0 / duration), niceFloor(fs / 8.0));
        c.eventSeconds = duration / 10.0;
    }
    return c;
}

bool applySeriesModel(GeneratorComponent &c, std::string_view key, double amplitude, double dutyCycle)
{
    const double e = std::isfinite(amplitude) ? amplitude : 1.0;
    const double d = std::isfinite(dutyCycle) ? std::clamp(dutyCycle, 0.001, 0.999) : 0.5;
    std::string variables = "E = " + plot::formatCompact(e, "", 6);
    std::string f0 = "0", fa = "0", fb = "0";
    if (key == "square" && std::abs(d - 0.5) < 1e-12)
        fb = "4*E/(pi*n)*odd(n)";
    else if (key == "square")
    {
        // ±E, high for D of the period: 2 × pulses − E.
        variables += " ; D = " + plot::formatCompact(d, "", 6);
        f0 = "E*(2*D - 1)";
        fa = "2*E/(pi*n)*sin(2*pi*n*D)";
        fb = "2*E/(pi*n)*(1 - cos(2*pi*n*D))";
    }
    else if (key == "triangle")
        fb = "8*E/(pi^2*n^2)*sin(n*pi/2)";
    else if (key == "sawtooth")
        fb = "2*E/(pi*n)*(-1)^(n+1)";
    else if (key == "pulse")
    {
        variables += " ; D = " + plot::formatCompact(d, "", 6);
        f0 = "E*D";
        fa = "E/(pi*n)*sin(2*pi*n*D)";
        fb = "E/(pi*n)*(1 - cos(2*pi*n*D))";
    }
    else if (key == "fullwave")
    {
        f0 = "2*E/pi";
        fa = "-4*E/(pi*(4*n^2 - 1))";
    }
    else if (key == "halfwave")
    {
        f0 = "E/pi";
        fa = "if(even(n); -2*E/(pi*(n^2 - 1)); 0)";
        fb = "if(n == 1; E/2; 0)";
    }
    else
        return false;
    c.seriesForm = SeriesForm::SineCosine;
    c.variables = std::move(variables);
    c.formula0 = std::move(f0);
    c.formulaA = std::move(fa);
    c.formulaB = std::move(fb);
    return true;
}

namespace
{
constexpr std::string_view kMinusSign = "\xE2\x88\x92"; // −
constexpr std::string_view kTimesSign = "\xC3\x97";     // ×
constexpr std::string_view kDotSign = "\xC2\xB7";       // ·
constexpr std::string_view kDivideSign = "\xC3\xB7";    // ÷

// A formula that needs no parentheses around it: a number or a name.
bool simpleFormula(std::string_view f) noexcept
{
    return !f.empty() && std::all_of(f.begin(), f.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.' ||
               c == ',' || u >= 0x80;
    });
}

std::string wrapped(const std::string &f)
{
    return simpleFormula(f) ? f : "(" + f + ")";
}

bool digitAt(std::string_view f, std::size_t i) noexcept
{
    return i < f.size() && f[i] >= '0' && f[i] <= '9';
}

// The length of the space at f[i] (an ordinary space or one of the Unicode ones the formulas allow), or 0.
std::size_t spaceAt(std::string_view f, std::size_t i) noexcept
{
    if (i >= f.size()) return 0;
    if (f[i] == ' ' || f[i] == '\t') return 1;
    for (std::string_view s : {std::string_view("\xC2\xA0"), std::string_view("\xE2\x80\xAF"), std::string_view("\xE2\x80\x89")})
        if (f.substr(i).starts_with(s)) return s.size();
    return 0;
}

// What a formula is made of at its top level (out of any parentheses).
struct TopLevel
{
    bool oneTerm = true;     // factors joined by * and / (or an implicit product), a leading sign allowed
    bool comparison = false; // a <, >, =, ≤, ≥, ≠ or !=
};

TopLevel topLevel(std::string_view f) noexcept
{
    TopLevel out;
    int depth = 0;
    bool operand = false; // what came last at the top level ends an operand
    for (std::size_t i = 0; i < f.size();)
    {
        const char c = f[i];
        if (c == '(')
        {
            ++depth;
            ++i;
            continue;
        }
        if (c == ')')
        {
            if (--depth < 0) out.oneTerm = false;
            operand = true;
            ++i;
            continue;
        }
        if (depth > 0)
        {
            ++i;
            continue;
        }
        if (const std::size_t space = spaceAt(f, i))
        {
            i += space;
            continue;
        }
        const std::string_view rest = f.substr(i);
        const bool minus = c == '-' || rest.starts_with(kMinusSign);
        if (c == '+' || minus)
        {
            const bool exponent = i >= 2 && (f[i - 1] == 'e' || f[i - 1] == 'E') && digitAt(f, i - 2);
            if (operand && !exponent) out.oneTerm = false;
            i += c == '+' || c == '-' ? 1 : kMinusSign.size();
            operand = false;
            continue;
        }
        if (c == '<' || c == '>' || c == '=' || c == '!' || rest.starts_with("\xE2\x89\xA4") || rest.starts_with("\xE2\x89\xA5") ||
            rest.starts_with("\xE2\x89\xA0"))
        {
            out.comparison = true;
            out.oneTerm = false;
            i += static_cast<unsigned char>(c) >= 0x80 ? 3 : 1;
            operand = false;
            continue;
        }
        if (c == ';') out.oneTerm = false;
        if (c == '*' || c == '/' || c == '^' || rest.starts_with(kTimesSign) || rest.starts_with(kDotSign) ||
            rest.starts_with(kDivideSign))
        {
            operand = false;
            i += static_cast<unsigned char>(c) >= 0x80 ? 2 : 1;
            continue;
        }
        operand = true;
        ++i;
    }
    if (depth != 0) out.oneTerm = false;
    return out;
}

bool oneTerm(std::string_view f) noexcept
{
    return topLevel(f).oneTerm;
}

// The number a one-term formula starts with: "4*E/(pi*n)" → 4 and "*E/(pi*n)"; "-0,5" → −0.5 and "".
// False without one, or when the number is raised to a power ("2^n": its sign is not the term's).
bool leadingNumber(std::string_view f, double &value, std::string_view &rest, bool &comma)
{
    std::size_t i = 0;
    bool negative = false;
    if (f.starts_with('-'))
    {
        negative = true;
        i = 1;
    }
    else if (f.starts_with(kMinusSign))
    {
        negative = true;
        i = kMinusSign.size();
    }
    std::string digits;
    while (digitAt(f, i)) digits += f[i++];
    if (i + 1 < f.size() && (f[i] == '.' || f[i] == ',') && digitAt(f, i + 1))
    {
        comma = f[i] == ',';
        digits += '.';
        ++i;
        while (digitAt(f, i)) digits += f[i++];
    }
    if (digits.empty()) return false;
    if (i < f.size() && (f[i] == 'e' || f[i] == 'E'))
    {
        std::size_t j = i + 1;
        if (j < f.size() && (f[j] == '+' || f[j] == '-')) ++j;
        if (digitAt(f, j))
        {
            digits += 'e';
            digits.append(f.substr(i + 1, j - i - 1));
            i = j;
            while (digitAt(f, i)) digits += f[i++];
        }
    }
    std::size_t k = i;
    while (const std::size_t space = spaceAt(f, k)) k += space;
    if (k < f.size() && (f[k] == '^' || f[k] == '.' || f[k] == ',' || digitAt(f, k))) return false;
    double v = 0.0;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), v);
    if (ec != std::errc() || end != digits.data() + digits.size() || !std::isfinite(v)) return false;
    value = negative ? -v : v;
    rest = f.substr(i);
    return true;
}

// "X/d" (or X÷d), d a number ending a one-term formula: X and d. "E/(pi*n)/2" → "E/(pi*n)" and 2.
bool trailingDivisor(std::string_view f, std::string &x, double &d, bool &comma)
{
    std::size_t i = f.size();
    while (i > 0 && (digitAt(f, i - 1) || f[i - 1] == '.' || f[i - 1] == ',')) --i;
    if (i == f.size() || !digitAt(f, i)) return false;
    std::string number(f.substr(i));
    const bool withComma = number.find(',') != std::string::npos;
    std::replace(number.begin(), number.end(), ',', '.');
    double v = 0.0;
    const auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), v);
    if (ec != std::errc() || end != number.data() + number.size() || !std::isfinite(v) || v == 0.0) return false;
    std::size_t j = i;
    while (j > 0 && f[j - 1] == ' ') --j;
    if (j >= 1 && f[j - 1] == '/')
        j -= 1;
    else if (j >= kDivideSign.size() && f.substr(j - kDivideSign.size(), kDivideSign.size()) == kDivideSign)
        j -= kDivideSign.size();
    else
        return false;
    const std::string_view before = trim(f.substr(0, j));
    if (before.empty()) return false;
    const char last = before.back();
    if (last == '*' || last == '/' || last == '^' || last == '(' || last == '-' || last == '+' || last == ';') return false;
    x = std::string(before);
    d = v;
    comma = withComma;
    return true;
}

std::string numberText(double v, bool comma)
{
    if (v == 0.0) return "0";
    std::array<char, 40> buffer{};
    const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), v);
    std::string text = ec == std::errc() ? std::string(buffer.data(), end) : std::to_string(v);
    if (comma) std::replace(text.begin(), text.end(), '.', ',');
    return text;
}

bool wholeNumber(double v) noexcept
{
    return std::isfinite(v) && std::abs(v) < 1e15 && v == std::round(v);
}

// "(X)" → X when the parentheses enclose all of it.
std::string unwrapped(std::string_view f)
{
    if (f.size() < 2 || f.front() != '(' || f.back() != ')') return std::string(f);
    int depth = 0;
    for (std::size_t i = 0; i < f.size(); ++i)
    {
        if (f[i] == '(') ++depth;
        else if (f[i] == ')' && --depth == 0 && i + 1 < f.size()) return std::string(f);
    }
    return std::string(f.substr(1, f.size() - 2));
}

// The formula times k, written short and as one term when it was one: the number in front changed
// ("4*E/(pi*n)" × 0.5 → "2*E/(pi*n)"), without one the divisor at the end changed or gone ("X/2" × 2 →
// X, "E/2" × −0.5 → "-E/4"), otherwise k in front or "/2" after. Scaled back by 1/k, it is the formula
// it was.
std::string scaled(std::string_view formula, double k)
{
    const std::string f(trim(formula));
    if (f.empty() || f == "0" || k == 0.0) return "0";
    if (k == 1.0) return f;
    if (!oneTerm(f))
    {
        if (k == -1.0) return "-(" + f + ")";
        if (k == 0.5) return "(" + f + ")/2";
        if (k == -0.5) return "-(" + f + ")/2";
        return numberText(k, false) + "*(" + f + ")";
    }
    double value = 0.0;
    std::string_view rest;
    bool numberComma = false;
    if (leadingNumber(f, value, rest, numberComma) && std::isfinite(value * k))
    {
        const double v = value * k;
        if (v == 0.0) return "0";
        const std::string_view tail = trim(rest);
        if (tail.empty()) return numberText(v, numberComma);
        const bool times = tail.front() == '*' || tail.starts_with(kTimesSign) || tail.starts_with(kDotSign);
        const bool explicitOperator = times || tail.front() == '/' || tail.starts_with(kDivideSign);
        if (v == 1.0 || v == -1.0)
        {
            const std::string sign = v < 0.0 ? "-" : "";
            if (times) return sign + std::string(trim(tail.substr(tail.front() == '*' ? 1 : 2)));
            if (!explicitOperator) return sign + std::string(tail);
        }
        return numberText(v, numberComma) + (explicitOperator ? "" : "*") + std::string(tail);
    }
    // Without a number in front, the divisor at the end changes (or goes).
    std::string x;
    double d = 0.0;
    bool comma = false;
    if (trailingDivisor(f, x, d, comma))
    {
        const double q = d / k;
        if (q == 1.0) return x;
        if (q == -1.0) return scaled(x, -1.0);
        if (std::abs(q) >= 2.0 && wholeNumber(q)) return (q < 0.0 ? scaled(x, -1.0) : x) + "/" + numberText(std::abs(q), comma);
    }
    if (f.front() == '-') return scaled(std::string_view(f).substr(1), -k);
    if (f.starts_with(kMinusSign)) return scaled(std::string_view(f).substr(kMinusSign.size()), -k);
    if (k == -1.0) return "-" + f;
    if (k == 0.5) return f + "/2";
    if (k == -0.5) return "-" + f + "/2";
    return numberText(k, false) + "*" + f;
}

// A coefficient formula times k, standing alone (without parentheses around all of it).
std::string scaledFormula(std::string_view formula, double k)
{
    return unwrapped(scaled(formula, k));
}

// The names a formula of a series knows, with their values (the rank n, N, k left to fill), or an error.
struct SeriesContext
{
    std::vector<std::string> names;
    std::vector<double> values;
    std::string error;
};

SeriesContext seriesContext(const GeneratorComponent &c, double fs)
{
    SeriesContext out;
    const double f0 = c.frequencyHz;
    const double w0 = dsp::kTwoPi * f0;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // The rank first (three spellings), then the constants of the series, then the variables.
    out.names = {"n", "N", "k", "Nmax", "f0", "w0", "\xCF\x89" "0", "T0", "fs"};
    out.values = {nan, nan, nan, static_cast<double>(seriesHarmonics(c, fs)), f0, w0, w0, f0 > 0.0 ? 1.0 / f0 : nan, fs};
    const Variables variables = parseVariables(c.variables, out.names, out.values);
    if (!variables.error.empty())
    {
        out.error = variables.error;
        return out;
    }
    out.names.insert(out.names.end(), variables.names.begin(), variables.names.end());
    out.values.insert(out.values.end(), variables.values.begin(), variables.values.end());
    return out;
}

// The value of a formula that is the same for every harmonic (without n, N, k nor Nmax), if it reads and
// is finite.
bool constantValue(const GeneratorComponent &c, double fs, const std::string &formula, double &value)
{
    const SeriesContext context = seriesContext(c, fs);
    if (!context.error.empty()) return false;
    const Expression e = Expression::parse(formula, context.names);
    if (!e.valid() || e.uses(0) || e.uses(1) || e.uses(2) || e.uses(3)) return false;
    value = e.evaluate(context.values);
    return std::isfinite(value);
}

bool zeroFormula(const GeneratorComponent &c, double fs, const std::string &formula)
{
    double value = 1.0;
    return formula == "0" || (constantValue(c, fs, formula, value) && value == 0.0);
}

// +1 when a formula is ≥ 0 for each harmonic of the series (n = 1..N, 4 096 at most) and not always 0,
// −1 when it is ≤ 0, 0 otherwise (or when it does not read, or is not finite).
int signOf(const GeneratorComponent &c, double fs, const std::string &formula)
{
    SeriesContext context = seriesContext(c, fs);
    if (!context.error.empty()) return 0;
    const Expression e = Expression::parse(formula, context.names);
    if (!e.valid()) return 0;
    const std::size_t count = std::min<std::size_t>(4096, seriesHarmonics(c, fs));
    bool positive = false, negative = false;
    for (std::size_t k = 0; k < count; ++k)
    {
        context.values[0] = context.values[1] = context.values[2] = static_cast<double>(k + 1);
        const double v = e.evaluate(context.values);
        if (!std::isfinite(v)) return 0;
        positive = positive || v > 0.0;
        negative = negative || v < 0.0;
    }
    return positive && !negative ? 1 : negative && !positive ? -1 : 0;
}

// "X" as the first argument of a comparison: in parentheses if it is one itself.
std::string compared(const std::string &x)
{
    return topLevel(x).comparison ? "(" + x + ")" : x;
}

// aₙ and bₙ of a series as formulas, whatever its form (what withSeriesForm writes reads back as it was).
void sineCosineFormulas(const GeneratorComponent &c, double fs, std::string &a, std::string &b)
{
    const std::string p = formulaBody(c.formulaA, 1), q = formulaBody(c.formulaB, 2);
    switch (c.seriesForm)
    {
        case SeriesForm::SineCosine:
            a = p;
            b = q;
            return;
        case SeriesForm::Cartesian: // aₙ = 2 Re Cₙ, bₙ = −2 Im Cₙ
            a = scaledFormula(p, 2.0);
            b = scaledFormula(q, -2.0);
            return;
        case SeriesForm::Polar: // aₙ = 2|Cₙ| cos φ, bₙ = −2|Cₙ| sin φ
        {
            a = b = "0";
            if (zeroFormula(c, fs, p)) return;
            const auto inside = [](std::string_view text, std::string_view open, std::string_view close, std::string &x) {
                if (text.size() < open.size() + close.size() || !text.starts_with(open) || !text.ends_with(close)) return false;
                x = std::string(text.substr(open.size(), text.size() - open.size() - close.size()));
                return true;
            };
            std::string x, y;
            if (inside(p, "abs(", ")/2", x))
            {
                if (inside(q, "-90*sign(", ")", y) && y == x)
                {
                    b = x;
                    return;
                }
                if (inside(q, "if(", " < 0; 180; 0)", y) && y == compared(x))
                {
                    a = x;
                    return;
                }
            }
            if (inside(q, "deg(atan2(-", "))", y))
            {
                // y = wrapped(B) + "; " + A, as written for sqrt(A^2 + B^2)/2.
                int depth = 0;
                for (std::size_t i = 0; i + 1 < y.size(); ++i)
                {
                    if (y[i] == '(') ++depth;
                    else if (y[i] == ')') --depth;
                    else if (depth == 0 && y[i] == ';' && y[i + 1] == ' ')
                    {
                        const std::string A = y.substr(i + 2), B = unwrapped(std::string_view(y).substr(0, i));
                        if (p == "sqrt(" + wrapped(A) + "^2 + " + wrapped(B) + "^2)/2")
                        {
                            a = A;
                            b = B;
                            return;
                        }
                        break;
                    }
                }
            }
            // The same phase for every harmonic, a multiple of 90°: the cosine and sine are 0 or ±1.
            double phase = 0.0;
            if (constantValue(c, fs, q, phase) && std::abs(std::remainder(phase, 90.0)) < 1e-9)
            {
                const double angle = phase * dsp::kPi / 180.0;
                const double cosine = std::round(std::cos(angle)), sine = std::round(std::sin(angle));
                a = scaledFormula(p, 2.0 * cosine);
                b = scaledFormula(p, -2.0 * sine);
                return;
            }
            a = scaled(p, 2.0) + "*cosd(" + q + ")";
            b = scaled(p, -2.0) + "*sind(" + q + ")";
            return;
        }
    }
}
} // namespace

GeneratorComponent withSeriesForm(const GeneratorComponent &component, SeriesForm form, double sampleRateHz)
{
    GeneratorComponent c = component;
    if (c.waveform != Kind::FourierSeries || form == c.seriesForm) return c;
    std::string a, b;
    sineCosineFormulas(component, sampleRateHz, a, b);
    c.seriesForm = form;
    c.formula0 = formulaBody(component.formula0, 0);
    switch (form)
    {
        case SeriesForm::SineCosine:
            c.formulaA = a;
            c.formulaB = b;
            break;
        case SeriesForm::Cartesian: // Cₙ = (aₙ − j bₙ)/2
            c.formulaA = scaledFormula(a, 0.5);
            c.formulaB = scaledFormula(b, -0.5);
            break;
        case SeriesForm::Polar:
        {
            // One of aₙ, bₙ zero and the other of one sign for the harmonics there are: |Cₙ| is half of it (or
            // minus half), the phase a constant.
            const bool zeroA = zeroFormula(component, sampleRateHz, a), zeroB = zeroFormula(component, sampleRateHz, b);
            const int signA = zeroA ? 0 : signOf(component, sampleRateHz, a);
            const int signB = zeroB ? 0 : signOf(component, sampleRateHz, b);
            if (zeroA && zeroB)
            {
                c.formulaA = c.formulaB = "0";
            }
            else if (zeroA && signB != 0) // Cₙ = −j bₙ/2
            {
                c.formulaA = scaledFormula(b, 0.5 * signB);
                c.formulaB = signB > 0 ? "-90" : "90";
            }
            else if (zeroB && signA != 0) // Cₙ = aₙ/2
            {
                c.formulaA = scaledFormula(a, 0.5 * signA);
                c.formulaB = signA > 0 ? "0" : "180";
            }
            else if (zeroA)
            {
                c.formulaA = "abs(" + b + ")/2";
                c.formulaB = "-90*sign(" + b + ")";
            }
            else if (zeroB)
            {
                c.formulaA = "abs(" + a + ")/2";
                c.formulaB = "if(" + compared(a) + " < 0; 180; 0)";
            }
            else
            {
                c.formulaA = "sqrt(" + wrapped(a) + "^2 + " + wrapped(b) + "^2)/2";
                c.formulaB = "deg(atan2(-" + wrapped(b) + "; " + a + "))";
            }
            break;
        }
    }
    return c;
}

std::size_t seriesHarmonics(const GeneratorComponent &c, double fs) noexcept
{
    if (c.harmonics > 0) return c.harmonics;
    if (!(std::isfinite(fs) && fs > 0.0 && std::isfinite(c.frequencyHz) && c.frequencyHz > 0.0)) return 0;
    const double below = std::floor((0.5 - 1e-12) * fs / c.frequencyHz);
    return below > 0.0 ? static_cast<std::size_t>(std::min(below, kLargestHarmonics)) : 0;
}

std::string_view seriesTermName(SeriesForm form, int formula) noexcept
{
    static constexpr std::string_view kNames[3][3] = {
        {"a\xE2\x82\x80", "a\xE2\x82\x99", "b\xE2\x82\x99"},
        {"C\xE2\x82\x80", "Re C\xE2\x82\x99", "Im C\xE2\x82\x99"},
        {"C\xE2\x82\x80", "|C\xE2\x82\x99|", "arg C\xE2\x82\x99"}};
    const int f = std::clamp(formula, 0, 2);
    switch (form)
    {
        case SeriesForm::SineCosine: return kNames[0][f];
        case SeriesForm::Cartesian: return kNames[1][f];
        case SeriesForm::Polar: return kNames[2][f];
    }
    return kNames[0][f];
}

SeriesCoefficients seriesCoefficients(const GeneratorComponent &c, double fs, std::size_t count)
{
    SeriesCoefficients out;
    SeriesContext context = seriesContext(c, fs);
    if (!context.error.empty())
    {
        out.error = context.error;
        return out;
    }
    std::vector<std::string> &names = context.names;
    std::vector<double> &values = context.values;
    Expression formulas[3];
    const std::string *texts[3] = {&c.formula0, &c.formulaA, &c.formulaB};
    for (int f = 0; f < 3; ++f)
    {
        std::string why;
        formulas[f] = Expression::parse(formulaBody(*texts[f], f), names, &why);
        if (!formulas[f].valid())
        {
            out.error = std::string(seriesTermName(c.seriesForm, f)) + " : " + why;
            return out;
        }
    }
    const std::string name0(seriesTermName(c.seriesForm, 0));
    if (formulas[0].uses(0) || formulas[0].uses(1) || formulas[0].uses(2))
    {
        out.error = name0 + " ne dépend pas de n : c'est la valeur moyenne du signal.";
        return out;
    }
    out.dc = formulas[0].evaluate(values);
    if (!std::isfinite(out.dc))
    {
        out.error = name0 + " n'est pas un nombre fini.";
        return out;
    }
    out.a.resize(count);
    out.b.resize(count);
    for (std::size_t k = 0; k < count; ++k)
    {
        const auto rank = static_cast<double>(k + 1);
        values[0] = values[1] = values[2] = rank;
        double p = formulas[1].evaluate(values), q = formulas[2].evaluate(values);
        for (int f = 1; f < 3; ++f)
        {
            if (std::isfinite(f == 1 ? p : q)) continue;
            out.a.clear();
            out.b.clear();
            out.error = std::string(seriesTermName(c.seriesForm, f)) + " n'est pas un nombre fini pour n = " +
                        std::to_string(k + 1) + " (une division par zéro ? if(n == " + std::to_string(k + 1) +
                        "; …; …) l'évite).";
            return out;
        }
        switch (c.seriesForm)
        {
            case SeriesForm::SineCosine: break;
            case SeriesForm::Cartesian:
                p = 2.0 * p;
                q = -2.0 * q;
                break;
            case SeriesForm::Polar:
            {
                const double magnitude = p, angle = q * dsp::kPi / 180.0;
                p = 2.0 * magnitude * std::cos(angle);
                q = -2.0 * magnitude * std::sin(angle);
                break;
            }
        }
        out.a[k] = p;
        out.b[k] = q;
    }
    return out;
}

// ---- Drawn motif ----

std::vector<MotifPoint> normalizedMotif(std::vector<MotifPoint> points)
{
    std::erase_if(points, [](const MotifPoint &p) { return !std::isfinite(p.x) || !std::isfinite(p.y); });
    if (points.size() > kMaximumMotifPoints) points.resize(kMaximumMotifPoints);
    for (MotifPoint &p : points)
    {
        p.x = std::clamp(p.x, 0.0, 1.0);
        p.y = std::clamp(p.y, -1.0, 1.0);
    }
    std::stable_sort(points.begin(), points.end(), [](const MotifPoint &a, const MotifPoint &b) { return a.x < b.x; });
    std::vector<MotifPoint> out;
    out.reserve(points.size());
    for (std::size_t i = 0; i < points.size();)
    {
        std::size_t j = i;
        while (j + 1 < points.size() && points[j + 1].x == points[i].x) ++j;
        out.push_back(points[i]);
        if (j > i) out.push_back(points[j]);
        i = j + 1;
    }
    return out;
}

namespace
{
// Slope at the end of a run of the monotone cubic (Moler's pchip): from the segment there (h0, d0) and the
// next one inward (h1, d1); kept of the sign of d0 and at most 3 d0 where the data turn.
double endSlope(double h0, double d0, double h1, double d1) noexcept
{
    const double s = ((2.0 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
    const auto sign = [](double v) { return (v > 0.0) - (v < 0.0); };
    if (sign(s) != sign(d0)) return 0.0;
    if (sign(d0) != sign(d1) && std::abs(s) > std::abs(3.0 * d0)) return 3.0 * d0;
    return s;
}
} // namespace

MotifShape::MotifShape(std::span<const MotifPoint> points, MotifInterpolation interpolation, bool periodic)
    : m_periodic(periodic)
{
    const std::size_t m = points.size();
    if (m == 0) return;
    m_start = points.front().x;
    m_end = points.back().x;
    m_first = points.front().y;
    m_last = points.back().y;
    // Segment i goes from node i to node i + 1; periodic, node m is the first point one period later.
    const std::size_t segments = periodic ? m : m - 1;
    const auto nodeX = [&](std::size_t i) { return i < m ? points[i].x : points[i - m].x + 1.0; };
    const auto nodeY = [&](std::size_t i) { return points[i % m].y; };
    std::vector<double> h(segments), d(segments, 0.0);
    for (std::size_t i = 0; i < segments; ++i)
    {
        h[i] = std::max(0.0, nodeX(i + 1) - nodeX(i));
        if (h[i] > 0.0) d[i] = (nodeY(i + 1) - nodeY(i)) / h[i];
    }
    std::vector<double> slope(m, 0.0);
    if (interpolation == MotifInterpolation::Smooth)
    {
        // Runs of segments of some width, between the jumps; periodic, the runs go round.
        const auto segment = [&](std::size_t node, bool left, std::size_t &out) {
            if (left)
            {
                if (node == 0 && !periodic) return false;
                out = node == 0 ? segments - 1 : node - 1;
            }
            else
            {
                if (node >= segments) return false;
                out = node;
            }
            return h[out] > 0.0;
        };
        for (std::size_t i = 0; i < m; ++i)
        {
            std::size_t left = 0, right = 0;
            const bool hasLeft = segment(i, true, left), hasRight = segment(i, false, right);
            if (hasLeft && hasRight)
            {
                if (d[left] * d[right] > 0.0)
                {
                    const double w1 = 2.0 * h[right] + h[left], w2 = h[right] + 2.0 * h[left];
                    slope[i] = (w1 + w2) / (w1 / d[left] + w2 / d[right]);
                }
            }
            else if (hasRight)
            {
                // The start of a run: with the segment after, if it belongs to the run.
                const std::size_t next = right + 1 < segments ? right + 1 : (periodic ? 0 : segments);
                slope[i] = next < segments && next != right && h[next] > 0.0 ? endSlope(h[right], d[right], h[next], d[next])
                                                                               : d[right];
            }
            else if (hasLeft)
            {
                const std::size_t before = left > 0 ? left - 1 : (periodic ? segments - 1 : segments);
                slope[i] = before < segments && before != left && h[before] > 0.0
                               ? endSlope(h[left], d[left], h[before], d[before])
                               : d[left];
            }
        }
    }
    for (std::size_t i = 0; i < segments; ++i)
    {
        if (!(h[i] > 0.0)) continue;
        Piece p{nodeX(i), nodeX(i + 1), nodeY(i), 0.0, 0.0, 0.0};
        if (interpolation == MotifInterpolation::Linear)
        {
            p.c1 = d[i];
        }
        else if (interpolation == MotifInterpolation::Smooth)
        {
            const double s0 = slope[i], s1 = slope[(i + 1) % m];
            p.c1 = s0;
            p.c2 = (3.0 * d[i] - 2.0 * s0 - s1) / h[i];
            p.c3 = (s0 + s1 - 2.0 * d[i]) / (h[i] * h[i]);
        }
        m_pieces.push_back(p);
    }
}

double MotifShape::operator()(double u) const noexcept
{
    if (m_pieces.empty()) return u < m_start ? m_first : m_last;
    double w = u;
    if (m_periodic)
    {
        const double turns = u - m_start;
        w = m_start + (turns - std::floor(turns));
    }
    else
    {
        if (u < m_start) return m_first;
        if (u >= m_end) return m_last;
    }
    // The last piece starting at or before w.
    auto it = std::upper_bound(m_pieces.begin(), m_pieces.end(), w, [](double v, const Piece &p) { return v < p.x0; });
    const Piece &p = it == m_pieces.begin() ? m_pieces.front() : *std::prev(it);
    return p.at(std::clamp(w - p.x0, 0.0, p.x1 - p.x0));
}

double MotifShape::mean() const noexcept
{
    double sum = 0.0;
    for (const Piece &p : m_pieces)
    {
        const double h = p.x1 - p.x0;
        sum += h * (p.c0 + h * (p.c1 / 2.0 + h * (p.c2 / 3.0 + h * p.c3 / 4.0)));
    }
    if (m_pieces.empty()) return m_periodic ? m_last : (m_start * m_first + (1.0 - m_start) * m_last);
    if (!m_periodic) sum += m_start * m_first + (1.0 - m_end) * m_last;
    return sum;
}

dsp::FourierSeries MotifShape::fourierSeries(std::size_t count) const
{
    dsp::FourierSeries out;
    out.dc = mean();
    out.cosine.assign(count, 0.0);
    out.sine.assign(count, 0.0);
    if (m_pieces.empty() || count == 0) return out;
    // cₖ = ∫ x(u) e^(−j2πku) du = Σ over the meeting points xᵢ of e^(−jωxᵢ) (−J₁/ω² + J₃/ω⁴ + j(−J₀/ω + J₂/ω³)),
    // Jᵣ the jump of the r-th derivative there (the piece starting at xᵢ minus the one ending there).
    struct Joint
    {
        double x;
        double j0, j1, j2, j3;
    };
    std::vector<Joint> joints;
    joints.reserve(m_pieces.size() + 1);
    const auto add = [&joints](double x, double v0, double v1, double v2, double v3) {
        if (v0 != 0.0 || v1 != 0.0 || v2 != 0.0 || v3 != 0.0) joints.push_back({x, v0, v1, v2, v3});
    };
    for (std::size_t i = 0; i < m_pieces.size(); ++i)
    {
        const Piece &p = m_pieces[i];
        const bool first = i == 0;
        // The piece ending at x0: the previous one, or (periodic) the last one; once, nothing before.
        double e0 = 0.0, e1 = 0.0, e2 = 0.0, e3 = 0.0;
        if (!first || m_periodic)
        {
            const Piece &q = first ? m_pieces.back() : m_pieces[i - 1];
            const double h = q.x1 - q.x0;
            e0 = q.at(h);
            e1 = q.c1 + h * (2.0 * q.c2 + 3.0 * q.c3 * h);
            e2 = 2.0 * q.c2 + 6.0 * q.c3 * h;
            e3 = 6.0 * q.c3;
        }
        add(p.x0, p.c0 - e0, p.c1 - e1, 2.0 * p.c2 - e2, 6.0 * p.c3 - e3);
    }
    // e^(−jωx) for k = 1, 2...: each joint turns by e^(−j2πx) at each k, recomputed exactly every 64.
    std::vector<double> stepR(joints.size()), stepI(joints.size()), zr(joints.size()), zi(joints.size());
    for (std::size_t j = 0; j < joints.size(); ++j)
    {
        stepR[j] = std::cos(dsp::kTwoPi * joints[j].x);
        stepI[j] = -std::sin(dsp::kTwoPi * joints[j].x);
        zr[j] = 1.0;
        zi[j] = 0.0;
    }
    for (std::size_t k = 1; k <= count; ++k)
    {
        const double omega = dsp::kTwoPi * static_cast<double>(k);
        const double w1 = 1.0 / omega, w2 = w1 * w1, w3 = w2 * w1, w4 = w2 * w2;
        double sr = 0.0, si = 0.0;
        for (std::size_t j = 0; j < joints.size(); ++j)
        {
            if (k % 64 == 0)
            {
                const double turns = static_cast<double>(k) * joints[j].x;
                const double angle = dsp::kTwoPi * (turns - std::floor(turns));
                zr[j] = std::cos(angle);
                zi[j] = -std::sin(angle);
            }
            else
            {
                const double r = zr[j] * stepR[j] - zi[j] * stepI[j];
                zi[j] = zr[j] * stepI[j] + zi[j] * stepR[j];
                zr[j] = r;
            }
            const Joint &jt = joints[j];
            const double dr = -jt.j1 * w2 + jt.j3 * w4, di = -jt.j0 * w1 + jt.j2 * w3;
            sr += zr[j] * dr - zi[j] * di;
            si += zr[j] * di + zi[j] * dr;
        }
        out.cosine[k - 1] = 2.0 * sr;
        out.sine[k - 1] = -2.0 * si;
    }
    return out;
}

namespace
{
// Ramer–Douglas–Peucker on (x, y/2): the points that keep the line within `tolerance`.
std::vector<MotifPoint> simplified(const std::vector<MotifPoint> &line, double tolerance)
{
    if (line.size() <= 2) return line;
    std::vector<char> keep(line.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<std::size_t, std::size_t>> stack{{0, line.size() - 1}};
    const double limit = tolerance * tolerance;
    while (!stack.empty())
    {
        const auto [a, b] = stack.back();
        stack.pop_back();
        const double ax = line[a].x, ay = 0.5 * line[a].y;
        const double dx = line[b].x - ax, dy = 0.5 * line[b].y - ay, length = dx * dx + dy * dy;
        double farthest = -1.0;
        std::size_t at = a;
        for (std::size_t i = a + 1; i < b; ++i)
        {
            const double px = line[i].x - ax, py = 0.5 * line[i].y - ay;
            const double t = length > 0.0 ? std::clamp((px * dx + py * dy) / length, 0.0, 1.0) : 0.0;
            const double ex = px - t * dx, ey = py - t * dy;
            const double distance = ex * ex + ey * ey;
            if (distance > farthest)
            {
                farthest = distance;
                at = i;
            }
        }
        if (farthest > limit)
        {
            keep[at] = 1;
            stack.push_back({a, at});
            stack.push_back({at, b});
        }
    }
    std::vector<MotifPoint> out;
    for (std::size_t i = 0; i < line.size(); ++i)
        if (keep[i]) out.push_back(line[i]);
    return out;
}

// A number of a motif as typed: "0,25", "-1", "−0.5", "1e-3".
bool readNumber(std::string_view token, double &value)
{
    std::string t;
    for (std::size_t i = 0; i < token.size(); ++i)
    {
        if (token.substr(i).starts_with("\xE2\x88\x92"))
        {
            t += '-';
            i += 2;
        }
        else
        {
            t += token[i] == ',' ? '.' : token[i];
        }
    }
    if (!t.empty() && t.front() == '+') t.erase(t.begin());
    if (t.empty()) return false;
    double v = 0.0;
    const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
    if (ec != std::errc() || end != t.data() + t.size() || !std::isfinite(v)) return false;
    value = v;
    return true;
}

// 4 decimals at most, a comma: "0,25", "-1", "0,3333".
std::string decimalText(double v)
{
    double r = std::round(v * 1e4) / 1e4;
    if (r == 0.0) r = 0.0; // no "-0"
    std::array<char, 48> buffer{};
    const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), r, std::chars_format::fixed, 4);
    std::string text = ec == std::errc() ? std::string(buffer.data(), end) : std::string("0");
    if (text.find('.') != std::string::npos)
    {
        while (text.back() == '0') text.pop_back();
        if (text.back() == '.') text.pop_back();
    }
    std::replace(text.begin(), text.end(), '.', ',');
    return text;
}

// The motif of a waveform of qacq::dsp::generate, the same samples (a sine: 16 points, smooth).
bool waveformMotif(Kind w, double duty, std::vector<MotifPoint> &points, MotifInterpolation &interpolation)
{
    switch (w)
    {
        case Kind::Square:
            points = {{0.0, 1.0}, {duty, -1.0}};
            interpolation = MotifInterpolation::Steps;
            return true;
        case Kind::Pulse:
            points = {{0.0, 1.0}, {duty, 0.0}};
            interpolation = MotifInterpolation::Steps;
            return true;
        case Kind::Triangle:
            points = {{0.0, 0.0}, {0.25, 1.0}, {0.75, -1.0}};
            interpolation = MotifInterpolation::Linear;
            return true;
        case Kind::Sawtooth:
            points = {{0.0, 0.0}, {0.5, 1.0}, {0.5, -1.0}};
            interpolation = MotifInterpolation::Linear;
            return true;
        case Kind::Sine:
            points.clear();
            for (int i = 0; i < 16; ++i)
            {
                const double x = i / 16.0;
                const double y = std::sin(dsp::kTwoPi * x);
                points.push_back({x, std::abs(y) < 1e-12 ? 0.0 : y});
            }
            interpolation = MotifInterpolation::Smooth;
            return true;
        default: return false;
    }
}
} // namespace

std::vector<MotifPoint> withStroke(std::span<const MotifPoint> motif, std::span<const MotifPoint> stroke, double tolerance)
{
    std::vector<MotifPoint> line;
    for (const MotifPoint &p : stroke)
        if (std::isfinite(p.x) && std::isfinite(p.y)) line.push_back({std::clamp(p.x, 0.0, 1.0), std::clamp(p.y, -1.0, 1.0)});
    std::vector<MotifPoint> out(motif.begin(), motif.end());
    if (line.size() <= 1)
    {
        out.insert(out.end(), line.begin(), line.end());
        return normalizedMotif(std::move(out));
    }
    // Painted on columns across the width: each step of the stroke sets the columns it crosses. A step
    // up or down within a column is a jump there: the value it came with is kept beside the last one.
    constexpr std::size_t kColumns = 1024;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> column(kColumns + 1, nan), entry(kColumns + 1, nan);
    const auto scale = static_cast<double>(kColumns);
    for (std::size_t i = 1; i < line.size(); ++i)
    {
        const double ca = line[i - 1].x * scale, cb = line[i].x * scale;
        const double ya = line[i - 1].y, yb = line[i].y;
        const auto lo = static_cast<std::size_t>(std::ceil(std::min(ca, cb)));
        const auto hi = static_cast<std::size_t>(std::floor(std::max(ca, cb)));
        if (lo > hi)
        {
            const auto c = static_cast<std::size_t>(std::lround(cb));
            if (std::isnan(entry[c])) entry[c] = std::isnan(column[c]) ? ya : column[c];
            column[c] = yb;
            continue;
        }
        for (std::size_t c = lo; c <= hi; ++c)
        {
            const double t = cb == ca ? 1.0 : (static_cast<double>(c) - ca) / (cb - ca);
            column[c] = ya + t * (yb - ya);
            entry[c] = nan;
        }
    }
    const bool leftward = line.back().x < line.front().x;
    std::size_t first = kColumns + 1, last = 0;
    for (std::size_t c = 0; c <= kColumns; ++c)
    {
        if (std::isnan(column[c])) continue;
        first = std::min(first, c);
        last = c;
    }
    if (first > last) return normalizedMotif(std::move(out));
    std::vector<MotifPoint> painted;
    for (std::size_t c = first; c <= last; ++c)
    {
        if (std::isnan(column[c])) continue;
        const double x = static_cast<double>(c) / scale;
        if (!std::isnan(entry[c]) && std::abs(entry[c] - column[c]) > 2.0 * tolerance)
        {
            // Drawn rightward the jump goes from the value it came with to the last one; leftward, back.
            painted.push_back({x, leftward ? column[c] : entry[c]});
            painted.push_back({x, leftward ? entry[c] : column[c]});
            continue;
        }
        painted.push_back({x, column[c]});
    }
    double t = std::max(tolerance, 1e-6);
    std::vector<MotifPoint> kept = simplified(painted, t);
    while (kept.size() > 256)
    {
        t *= 2.0;
        kept = simplified(painted, t);
    }
    const double x0 = static_cast<double>(first) / scale, x1 = static_cast<double>(last) / scale;
    std::erase_if(out, [&](const MotifPoint &p) { return p.x >= x0 && p.x <= x1; });
    out.insert(out.end(), kept.begin(), kept.end());
    return normalizedMotif(std::move(out));
}

std::string motifText(std::span<const MotifPoint> points)
{
    std::string text;
    for (const MotifPoint &p : points)
    {
        if (!text.empty()) text += " ; ";
        text += decimalText(p.x) + " " + decimalText(p.y);
    }
    return text;
}

bool parseMotif(std::string_view text, std::vector<MotifPoint> &points, std::string &error)
{
    std::vector<MotifPoint> out;
    std::size_t index = 0;
    std::size_t start = 0;
    while (start <= text.size())
    {
        std::size_t end = text.find_first_of(";\n", start);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view pair = trim(text.substr(start, end - start));
        start = end + 1;
        if (pair.empty()) continue;
        ++index;
        const std::string where = "Point " + std::to_string(index);
        // x and y, separated by spaces (also the spaces a French keyboard may put).
        std::vector<std::string> tokens;
        std::string token;
        for (std::size_t i = 0; i < pair.size();)
        {
            std::size_t space = 0;
            if (pair[i] == ' ' || pair[i] == '\t' || pair[i] == '\r') space = 1;
            for (std::string_view s : {std::string_view("\xC2\xA0"), std::string_view("\xE2\x80\xAF"), std::string_view("\xE2\x80\x89")})
                if (pair.substr(i).starts_with(s)) space = s.size();
            if (space > 0)
            {
                if (!token.empty()) tokens.push_back(std::move(token));
                token.clear();
                i += space;
            }
            else
            {
                token += pair[i++];
            }
        }
        if (!token.empty()) tokens.push_back(std::move(token));
        if (tokens.size() != 2)
        {
            error = where + " (« " + std::string(pair) + " ») : deux nombres attendus, x puis y, séparés par une espace.";
            return false;
        }
        MotifPoint p;
        if (!readNumber(tokens[0], p.x) || !readNumber(tokens[1], p.y))
        {
            error = where + " : « " + (readNumber(tokens[0], p.x) ? tokens[1] : tokens[0]) + " » n'est pas un nombre.";
            return false;
        }
        if (p.x < 0.0 || p.x > 1.0)
        {
            error = where + " : x = " + decimalText(p.x) + " doit être entre 0 et 1.";
            return false;
        }
        if (p.y < -1.0 || p.y > 1.0)
        {
            error = where + " : y = " + decimalText(p.y) + " doit être entre -1 et 1.";
            return false;
        }
        if (out.size() >= kMaximumMotifPoints)
        {
            error = "Au plus " + grouped(kMaximumMotifPoints) + " points.";
            return false;
        }
        out.push_back(p);
    }
    points = normalizedMotif(std::move(out));
    return true;
}

bool applyMotifModel(GeneratorComponent &component, std::string_view key)
{
    std::vector<MotifPoint> p;
    MotifInterpolation interpolation = MotifInterpolation::Linear;
    if (key == "ramp")
    {
        p = {{0.0, -1.0}, {1.0, 1.0}};
    }
    else if (key == "stairs")
    {
        p = {{0.0, -1.0}, {0.25, -1.0 / 3.0}, {0.5, 1.0 / 3.0}, {0.75, 1.0}};
        interpolation = MotifInterpolation::Steps;
    }
    else if (key == "trapezoid")
    {
        p = {{0.0, -1.0}, {0.1, 1.0}, {0.5, 1.0}, {0.6, -1.0}};
    }
    else if (key == "rc")
    {
        // A capacitor charged then discharged through a resistor by a square wave, τ = T/10, in its steady
        // state between −v and +v; the two corners kept sharp (two points at the same x).
        const double tau = 0.1, v = std::tanh(0.25 / tau);
        for (double x : {0.0, 0.02, 0.05, 0.1, 0.15, 0.2, 0.3, 0.4, 0.5})
            p.push_back({x, 1.0 - (1.0 + v) * std::exp(-x / tau)});
        p.back().y = v;
        p.push_back({0.5, v});
        for (double x : {0.52, 0.55, 0.6, 0.65, 0.7, 0.8, 0.9, 1.0})
            p.push_back({x, -1.0 + (1.0 + v) * std::exp(-(x - 0.5) / tau)});
        p.back().y = -v;
        interpolation = MotifInterpolation::Smooth;
    }
    else if (key == "heartbeat")
    {
        // P wave, QRS complex, T wave of an electrocardiogram (R = 1).
        p = {{0.0, 0.0},    {0.1, 0.0},   {0.13, 0.07}, {0.16, 0.1}, {0.19, 0.07}, {0.22, 0.0},  {0.28, 0.0},
             {0.3, -0.1},   {0.33, 1.0},  {0.36, -0.25}, {0.39, 0.0}, {0.48, 0.02}, {0.54, 0.12}, {0.6, 0.25},
             {0.66, 0.12},  {0.72, 0.02}, {0.78, 0.0},  {1.0, 0.0}};
        interpolation = MotifInterpolation::Smooth;
    }
    else
    {
        return false;
    }
    component.motif = normalizedMotif(std::move(p));
    component.interpolation = interpolation;
    return true;
}


namespace
{
// A drawn motif: sampled as it is, or by its Fourier series below fs/2 within the budget of computation.
ComponentSamples motifSamples(const GeneratorComponent &c, double fs, std::size_t n)
{
    ComponentSamples out;
    const MotifShape shape(c.motif, c.interpolation, !c.motifOnce);
    const double a = c.amplitude;
    out.samples.resize(n);
    if (c.motifOnce)
    {
        const auto total = static_cast<double>(n);
        for (std::size_t i = 0; i < n; ++i) out.samples[i] = a * shape(static_cast<double>(i) / total);
        return out;
    }
    const double f0 = c.frequencyHz;
    if (!(std::isfinite(f0) && f0 > 0.0))
    {
        out.samples.clear();
        out.error = "La fréquence doit être positive.";
        return out;
    }
    const double ratio = f0 / fs, phase = c.phaseDegrees / 360.0;
    if (c.bandLimit != dsp::BandLimit::Fourier)
    {
        if (f0 >= 0.5 * fs) out.warnings.emplace_back("La fréquence dépasse fs/2 : le signal échantillonné est replié.");
        for (std::size_t i = 0; i < n; ++i) out.samples[i] = a * shape(ratio * static_cast<double>(i) + phase);
        return out;
    }
    const double below = std::min(std::floor((0.5 - 1e-12) * fs / f0), kLargestHarmonics);
    std::size_t count = below > 0.0 ? static_cast<std::size_t>(below) : 0;
    if (c.harmonics > 0) count = std::min(count, c.harmonics);
    if (count == 0)
        out.warnings.emplace_back("f0 atteint fs/2 : aucun harmonique en dessous, seul le terme constant reste.");
    const auto budget = static_cast<std::size_t>(std::max(1.0, std::floor(kGeneratorFourierBudget / static_cast<double>(n))));
    if (count > budget)
    {
        out.warnings.push_back("Série de Fourier limitée à " + grouped(budget) + " harmoniques sur " + grouped(count) +
                               " (temps de calcul) : les plus hautes manquent ; baissez la fréquence d'échantillonnage ou la durée.");
        count = budget;
    }
    dsp::FourierSeries series = shape.fourierSeries(count);
    for (double &v : series.cosine) v *= a;
    for (double &v : series.sine) v *= a;
    std::fill(out.samples.begin(), out.samples.end(), a * series.dc);
    addSeries(out.samples, series.cosine, series.sine, ratio, phase);
    return out;
}
} // namespace

// ---- Recurrence ----

namespace
{
bool identifierChar(char c) noexcept
{
    const auto u = static_cast<unsigned char>(c);
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || u >= 0x80;
}

// After the x or y at text[i]: an index "[n-2]", "(n−2)", "_{n-2}", "[n]" (or, for initial values, "[-2]").
// The delay (2 for n−2, 0 for n, −1 for n+1) and where it ends.
bool indexAt(std::string_view text, std::size_t i, bool initial, int &delay, std::size_t &end)
{
    std::size_t j = i + 1;
    const auto skip = [&] {
        while (j < text.size() && text[j] == ' ') ++j;
    };
    char close = 0;
    if (j < text.size() && (text[j] == '[' || text[j] == '('))
    {
        close = text[j] == '[' ? ']' : ')';
        ++j;
    }
    else if (j + 1 < text.size() && text[j] == '_' && (text[j + 1] == '{' || text[j + 1] == '('))
    {
        close = text[j + 1] == '{' ? '}' : ')';
        j += 2;
    }
    else
    {
        return false;
    }
    skip();
    if (!initial)
    {
        if (j >= text.size() || text[j] != 'n') return false;
        ++j;
        skip();
    }
    int sign = 0;
    if (j < text.size() && (text[j] == '-' || text[j] == '+'))
    {
        sign = text[j] == '-' ? -1 : 1;
        ++j;
    }
    else if (text.substr(j).starts_with("\xE2\x88\x92"))
    {
        sign = -1;
        j += 3;
    }
    skip();
    int value = 0;
    bool digits = false;
    while (j < text.size() && text[j] >= '0' && text[j] <= '9')
    {
        if (value < 100000) value = 10 * value + (text[j] - '0');
        digits = true;
        ++j;
    }
    skip();
    if (j >= text.size() || text[j] != close) return false;
    if (digits != (sign != 0) || (initial && !digits)) return false;
    delay = -sign * value;
    end = j + 1;
    return true;
}

// The recurrence as names of the formulas: y[n-2] → y2, x[n] → x, x[n-3] → x3; initial values: y[-1] → y1.
bool recurrenceNames(std::string_view text, bool initial, std::string &out, std::string &error)
{
    out.clear();
    for (std::size_t i = 0; i < text.size();)
    {
        const char c = text[i];
        int delay = 0;
        std::size_t end = 0;
        if ((c == 'x' || c == 'y') && (i == 0 || !identifierChar(text[i - 1])) && indexAt(text, i, initial, delay, end))
        {
            const std::string written(text.substr(i, end - i));
            if (initial && c == 'x')
            {
                error = "« " + written + " » : seules les valeurs de y d'avant n = 0 se donnent (y[-1] à y[-" +
                        std::to_string(kRecurrenceDepth) + "]).";
                return false;
            }
            if (delay < 0 || (c == 'y' && delay == 0))
            {
                error = "« " + written + " » : y[n] ne dépend que des valeurs d'avant (y[n-1], y[n-2]…, x[n], x[n-1]…).";
                return false;
            }
            if (static_cast<std::size_t>(delay) > kRecurrenceDepth)
            {
                error = "« " + written + " » : au plus " + std::to_string(kRecurrenceDepth) + " valeurs d'avant (y[n-" +
                        std::to_string(kRecurrenceDepth) + "]).";
                return false;
            }
            out += c;
            if (delay > 0) out += std::to_string(delay);
            i = end;
            continue;
        }
        out += c;
        ++i;
    }
    return true;
}

// The formula of y[n] as typed, without "y[n] =" (or "y =", "=") before it. Empty: 0.
std::string recurrenceBody(std::string_view text)
{
    std::string_view t = trim(text);
    for (std::size_t i = 0; i < t.size(); ++i)
    {
        if (t[i] != '=') continue;
        const char before = i > 0 ? t[i - 1] : ' ';
        const char after = i + 1 < t.size() ? t[i + 1] : ' ';
        if (before == '<' || before == '>' || before == '!' || before == '=' || after == '=') continue;
        std::string left;
        for (char c : t.substr(0, i))
            if (c != ' ' && c != '_' && c != '{' && c != '}') left += c;
        if (left.empty() || left == "y" || left == "y[n]" || left == "y(n)" || left == "yn") t = trim(t.substr(i + 1));
        break;
    }
    return t.empty() ? std::string("0") : std::string(t);
}

// A recurrence ready to run: its names (n, t, fs, Te, N, the variables, then x, x1…x16, y1…y16), their
// values, its formula, how far back it looks, its initial values; or what is wrong.
struct RecurrenceProgram
{
    std::vector<std::string> names;
    std::vector<double> values;
    Expression formula;
    std::size_t inputs = 0; // index of x; x[n−k] at inputs + k, y[n−k] at inputs + kRecurrenceDepth + k
    std::size_t depthX = 0, depthY = 0;
    std::array<double, kRecurrenceDepth> initial{}; // y[−1]…y[−16]
    std::string error;
};

RecurrenceProgram compileRecurrence(const GeneratorComponent &c, double fs, std::size_t total)
{
    RecurrenceProgram p;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    p.names = {"n", "t", "fs", "Te", "N"};
    p.values = {nan, nan, fs, 1.0 / fs, static_cast<double>(total)};
    std::vector<std::string> recurrence = {"x"};
    for (std::size_t k = 1; k <= kRecurrenceDepth; ++k) recurrence.push_back("x" + std::to_string(k));
    for (std::size_t k = 1; k <= kRecurrenceDepth; ++k) recurrence.push_back("y" + std::to_string(k));

    // Variables: constants (of fs, Te, N and those before them); n, t, x and y are taken.
    std::vector<std::string> names = p.names;
    names.insert(names.end(), recurrence.begin(), recurrence.end());
    std::vector<double> values = p.values;
    values.resize(names.size(), nan);
    const Variables variables = parseVariables(c.variables, names, values);
    if (!variables.error.empty())
    {
        p.error = "Variables : " + variables.error;
        return p;
    }
    for (std::size_t i = 0; i < variables.names.size(); ++i)
    {
        if (std::isfinite(variables.values[i])) continue;
        p.error = "Variables : « " + variables.names[i] + " » n'est pas un nombre fini (une variable est une constante, sans n, t, x ni y).";
        return p;
    }
    p.names.insert(p.names.end(), variables.names.begin(), variables.names.end());
    p.values.insert(p.values.end(), variables.values.begin(), variables.values.end());

    // Initial values, of the constants and the variables.
    std::string typed, why;
    if (!recurrenceNames(c.initialValues, true, typed, why))
    {
        p.error = "Valeurs initiales : " + why;
        return p;
    }
    const Variables initial = parseVariables(typed, p.names, p.values);
    if (!initial.error.empty())
    {
        p.error = "Valeurs initiales : " + initial.error;
        return p;
    }
    for (std::size_t i = 0; i < initial.names.size(); ++i)
    {
        const std::string &name = initial.names[i];
        std::size_t k = 0;
        if (name.size() >= 2 && name.size() <= 3 && name.front() == 'y' &&
            std::all_of(name.begin() + 1, name.end(), [](char d) { return d >= '0' && d <= '9'; }))
            k = static_cast<std::size_t>(std::stoul(name.substr(1)));
        if (k < 1 || k > kRecurrenceDepth)
        {
            p.error = "Valeurs initiales : « " + name + " » n'est pas une valeur y[-1] à y[-" + std::to_string(kRecurrenceDepth) +
                      "] (« y[-1] = 1 ; y[-2] = 0 »).";
            return p;
        }
        if (!std::isfinite(initial.values[i]))
        {
            p.error = "Valeurs initiales : y[-" + std::to_string(k) + "] n'est pas un nombre fini.";
            return p;
        }
        p.initial[k - 1] = initial.values[i];
    }

    // The formula.
    if (!recurrenceNames(recurrenceBody(c.recurrence), false, typed, why))
    {
        p.error = "y[n] : " + why;
        return p;
    }
    p.inputs = p.names.size();
    p.names.insert(p.names.end(), recurrence.begin(), recurrence.end());
    p.values.resize(p.names.size(), 0.0);
    p.formula = Expression::parse(typed, p.names, &why);
    if (!p.formula.valid())
    {
        p.error = "y[n] : " + why;
        return p;
    }
    for (std::size_t k = 1; k <= kRecurrenceDepth; ++k)
    {
        if (p.formula.uses(p.inputs + k)) p.depthX = k;
        if (p.formula.uses(p.inputs + kRecurrenceDepth + k)) p.depthY = k;
    }
    return p;
}

std::string_view inputName(RecurrenceInput input) noexcept
{
    switch (input)
    {
        case RecurrenceInput::None: return "aucune";
        case RecurrenceInput::Impulse: return "Dirac";
        case RecurrenceInput::Step: return "échelon";
        case RecurrenceInput::Noise: return "bruit blanc";
        case RecurrenceInput::Above: return "les formes d'onde au-dessus";
    }
    return "?";
}

std::string_view inputKey(RecurrenceInput input) noexcept
{
    switch (input)
    {
        case RecurrenceInput::None: return "none";
        case RecurrenceInput::Impulse: return "impulse";
        case RecurrenceInput::Step: return "step";
        case RecurrenceInput::Noise: return "noise";
        case RecurrenceInput::Above: return "above";
    }
    return "none";
}

RecurrenceInput inputFromKey(std::string_view key) noexcept
{
    for (RecurrenceInput i : {RecurrenceInput::Impulse, RecurrenceInput::Step, RecurrenceInput::Noise, RecurrenceInput::Above})
        if (inputKey(i) == key) return i;
    return RecurrenceInput::None;
}

// y[n] for n = 0..count−1 from its input (`above`: the sum of the components above, empty if none). Its first
// values into `trace`.
ComponentSamples recurrenceSamples(const GeneratorComponent &c, double fs, std::size_t n, std::span<const double> above,
                                   std::uint64_t seed, const model::Progress &progress, RecurrenceTrace *trace)
{
    ComponentSamples out;
    RecurrenceProgram p = compileRecurrence(c, fs, n);
    if (!p.error.empty())
    {
        out.error = p.error;
        return out;
    }
    std::vector<double> input;
    switch (c.recurrenceInput)
    {
        case RecurrenceInput::None: input.assign(n, 0.0); break;
        case RecurrenceInput::Impulse:
            input.assign(n, 0.0);
            if (n > 0) input[0] = 1.0;
            break;
        case RecurrenceInput::Step: input.assign(n, 1.0); break;
        case RecurrenceInput::Noise:
        {
            dsp::WaveformSpec spec;
            spec.waveform = dsp::Waveform::WhiteNoise;
            spec.amplitude = 1.0;
            spec.sampleRateHz = fs;
            spec.sampleCount = n;
            spec.noise = dsp::NoiseDistribution::Gaussian;
            spec.seed = seed;
            dsp::GeneratedSignal g = dsp::generate(spec);
            input = std::move(g.samples);
            input.resize(n, 0.0);
            break;
        }
        case RecurrenceInput::Above:
            if (above.size() == n) input.assign(above.begin(), above.end());
            else input.assign(n, 0.0);
            break;
    }
    out.samples.resize(n);
    std::vector<double> &values = p.values;
    const std::size_t x = p.inputs, y = p.inputs + kRecurrenceDepth;
    for (std::size_t i = 0; i < n; ++i)
    {
        if ((i & 0xFFFF) == 0xFFFF && progress.stopped())
        {
            out.cancelled = true;
            out.samples.clear();
            return out;
        }
        values[0] = static_cast<double>(i);
        values[1] = static_cast<double>(i) / fs;
        values[x] = input[i];
        for (std::size_t k = 1; k <= p.depthX; ++k) values[x + k] = i >= k ? input[i - k] : 0.0;
        for (std::size_t k = 1; k <= p.depthY; ++k) values[y + k] = i >= k ? out.samples[i - k] : p.initial[k - i - 1];
        const double v = p.formula.evaluate(values);
        if (!std::isfinite(v))
        {
            out.samples.clear();
            out.error = "y[n] n'est pas un nombre fini pour n = " + grouped(i) +
                        (i > 0 ? " : la récurrence diverge (ou une division par zéro)." : " (une division par zéro ?).");
            return out;
        }
        out.samples[i] = v;
    }
    if (trace)
    {
        const std::size_t shown = std::min<std::size_t>(12, n);
        trace->x.assign(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(shown));
        trace->y.assign(out.samples.begin(), out.samples.begin() + static_cast<std::ptrdiff_t>(shown));
    }
    return out;
}
} // namespace

std::string checkRecurrence(const GeneratorComponent &component, double sampleRateHz)
{
    return compileRecurrence(component, sampleRateHz, 0).error;
}

bool recurrenceUsesInput(const GeneratorComponent &component, double sampleRateHz)
{
    const RecurrenceProgram p = compileRecurrence(component, sampleRateHz, 0);
    if (!p.error.empty()) return false;
    for (std::size_t k = 0; k <= kRecurrenceDepth; ++k)
        if (p.formula.uses(p.inputs + k)) return true;
    return false;
}

bool applyRecurrenceModel(GeneratorComponent &c, std::string_view key, double amplitude, double frequencyHz,
                          double phaseDegrees)
{
    const std::string a = plot::formatCompact(std::isfinite(amplitude) ? amplitude : 1.0, "", 6);
    const std::string f = plot::formatCompact(std::isfinite(frequencyHz) && frequencyHz > 0.0 ? frequencyHz : 1000.0, "", 6);
    if (key == "oscillator")
    {
        // y[n] = A sin(w n + φ): y[n] = 2 cos(w) y[n-1] − y[n-2], from y[-1] = A sin(φ − w), y[-2] = A sin(φ − 2w).
        c.variables = "A = " + a + " ; f = " + f + " ; w = 2*pi*f/fs";
        c.recurrence = "2*cos(w)*y[n-1] - y[n-2]";
        c.initialValues = "y[-1] = -A*sin(w) ; y[-2] = -A*sin(2*w)";
        if (std::isfinite(phaseDegrees) && phaseDegrees != 0.0)
        {
            c.variables += " ; phi = rad(" + plot::formatCompact(phaseDegrees, "", 6) + ")";
            c.initialValues = "y[-1] = A*sin(phi - w) ; y[-2] = A*sin(phi - 2*w)";
        }
        c.recurrenceInput = RecurrenceInput::None;
    }
    else if (key == "lowpass")
    {
        c.variables = "tau = 0,001 ; a = exp(-Te/tau)";
        c.recurrence = "a*y[n-1] + (1 - a)*x[n]";
        c.initialValues.clear();
        c.recurrenceInput = RecurrenceInput::Step;
    }
    else if (key == "resonator")
    {
        c.variables = "f = " + f + " ; r = 0,999 ; w = 2*pi*f/fs";
        c.recurrence = "2*r*cos(w)*y[n-1] - r^2*y[n-2] + x[n]";
        c.initialValues.clear();
        c.recurrenceInput = RecurrenceInput::Impulse;
    }
    else if (key == "average")
    {
        c.variables.clear();
        c.recurrence = "(x[n] + x[n-1] + x[n-2] + x[n-3])/4";
        c.initialValues.clear();
        c.recurrenceInput = RecurrenceInput::Noise;
    }
    else if (key == "walk")
    {
        c.variables = "s = 0,01";
        c.recurrence = "y[n-1] + s*x[n]";
        c.initialValues.clear();
        c.recurrenceInput = RecurrenceInput::Noise;
    }
    else if (key == "logistic")
    {
        c.variables = "r = 3,9";
        c.recurrence = "r*y[n-1]*(1 - y[n-1])";
        c.initialValues = "y[-1] = 0,2";
        c.recurrenceInput = RecurrenceInput::None;
    }
    else
    {
        return false;
    }
    return true;
}

// ---- Operations between channels, modulated carriers ----

namespace
{
constexpr std::size_t kChannelNames = kMaximumGeneratorChannels; // v1…v16

bool operationKind(Kind w) noexcept
{
    return w == Kind::Formula || w == Kind::Modulation;
}

// The formula of an operation as typed, without "y =" (or "y(t) =", "=") before it; a message without "m ="
// ("m(t) =", "message ="). Empty: 0.
std::string operationBody(std::string_view text, bool message)
{
    std::string_view t = trim(text);
    for (std::size_t i = 0; i < t.size(); ++i)
    {
        if (t[i] != '=') continue;
        const char before = i > 0 ? t[i - 1] : ' ';
        const char after = i + 1 < t.size() ? t[i + 1] : ' ';
        if (before == '<' || before == '>' || before == '!' || before == '=' || after == '=') continue;
        std::string left;
        for (char c : t.substr(0, i))
            if (c != ' ') left += c;
        const bool named = message ? left.empty() || left == "m" || left == "m(t)" || left == "message" || left == "Message"
                                   : left.empty() || left == "y" || left == "y(t)" || left == "y[n]";
        if (named) t = trim(t.substr(i + 1));
        break;
    }
    return t.empty() ? std::string("0") : std::string(t);
}

// An operation ready to run: its names (n, t, fs, Te, N, its variables, then x, v1…v16, V1…V16), their
// values, what it reads; or what is wrong ("y : …", "Message : …", "Variables : …").
struct OperationProgram
{
    std::vector<std::string> names;
    std::vector<double> values;
    Expression formula;
    std::size_t above = 0;          // index of x
    std::size_t channels = 0;       // index of v1 (V1 at channels + kChannelNames)
    std::vector<std::size_t> reads; // channels read, from 0
    bool readsAbove = false;
    std::string error;
};

OperationProgram compileOperation(std::string_view text, std::string_view variablesText, bool message, double fs,
                                  std::size_t total)
{
    OperationProgram p;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    p.names = {"n", "t", "fs", "Te", "N"};
    p.values = {nan, nan, fs, 1.0 / fs, static_cast<double>(total)};
    std::vector<std::string> reserved = {"x"};
    for (std::size_t k = 1; k <= kChannelNames; ++k) reserved.push_back("v" + std::to_string(k));
    for (std::size_t k = 1; k <= kChannelNames; ++k) reserved.push_back("V" + std::to_string(k));

    // Variables: constants (of fs, Te, N and those before them); n, t, x and the channels are taken.
    std::vector<std::string> names = p.names;
    names.insert(names.end(), reserved.begin(), reserved.end());
    std::vector<double> values = p.values;
    values.resize(names.size(), nan);
    const Variables variables = parseVariables(variablesText, names, values);
    if (!variables.error.empty())
    {
        p.error = "Variables : " + variables.error;
        return p;
    }
    for (std::size_t i = 0; i < variables.names.size(); ++i)
    {
        if (std::isfinite(variables.values[i])) continue;
        p.error = "Variables : « " + variables.names[i] +
                  " » n'est pas un nombre fini (une variable est une constante, sans n, t, x ni voie).";
        return p;
    }
    p.names.insert(p.names.end(), variables.names.begin(), variables.names.end());
    p.values.insert(p.values.end(), variables.values.begin(), variables.values.end());

    p.above = p.names.size();
    p.channels = p.above + 1;
    p.names.insert(p.names.end(), reserved.begin(), reserved.end());
    p.values.resize(p.names.size(), 0.0);
    std::string why;
    p.formula = Expression::parse(operationBody(text, message), p.names, &why);
    if (!p.formula.valid())
    {
        p.error = std::string(message ? "Message" : "y") + " : " + why;
        return p;
    }
    p.readsAbove = p.formula.uses(p.above);
    for (std::size_t k = 0; k < kChannelNames; ++k)
        if (p.formula.uses(p.channels + k) || p.formula.uses(p.channels + kChannelNames + k)) p.reads.push_back(k);
    return p;
}

OperationProgram compileOperation(const GeneratorComponent &c, double fs, std::size_t total)
{
    const bool message = c.waveform == Kind::Modulation;
    return compileOperation(message ? c.message : c.expression, c.variables, message, fs, total);
}

// The channels each channel reads through its counted operations and modulated carriers (whose formula is
// right, and only the channels that exist).
std::vector<std::vector<std::size_t>> channelGraph(const GeneratorRecipe &recipe)
{
    std::vector<std::vector<std::size_t>> reads(recipe.channels.size());
    for (std::size_t c = 0; c < recipe.channels.size(); ++c)
    {
        for (const GeneratorComponent &k : recipe.channels[c].components)
        {
            if (!k.enabled || !operationKind(k.waveform)) continue;
            for (std::size_t j : compileOperation(k, recipe.sampleRateHz, 0).reads)
                if (j < recipe.channels.size() && std::find(reads[c].begin(), reads[c].end(), j) == reads[c].end())
                    reads[c].push_back(j);
        }
    }
    return reads;
}

// A path of reads from `from` back to it ("Voies 2 → 3 → 2"), or empty: the shortest one.
std::string loopThrough(const std::vector<std::vector<std::size_t>> &reads, std::size_t from)
{
    std::vector<std::size_t> parent(reads.size(), reads.size());
    std::vector<std::size_t> queue;
    const auto reach = [&](std::size_t j, std::size_t by) {
        if (parent[j] != reads.size()) return;
        parent[j] = by;
        queue.push_back(j);
    };
    for (std::size_t j : reads[from]) reach(j, from);
    for (std::size_t q = 0; q < queue.size(); ++q)
    {
        const std::size_t c = queue[q];
        if (c == from)
        {
            std::vector<std::size_t> path = {from};
            for (std::size_t node = parent[from]; node != from; node = parent[node]) path.push_back(node);
            path.push_back(from);
            std::string text = "Voies ";
            for (std::size_t i = path.size(); i-- > 0;)
                text += std::to_string(path[i] + 1) + (i > 0 ? " → " : "");
            return text + " : une voie ne peut pas dépendre d'elle-même (une opération y lit une voie qui la lit).";
        }
        for (std::size_t j : reads[c]) reach(j, c);
    }
    return {};
}

// The order to make the channels in: each after those it reads (the order of the recipe otherwise); false
// with why on a loop.
bool channelOrder(const GeneratorRecipe &recipe, std::vector<std::size_t> &order, std::string &error)
{
    const std::vector<std::vector<std::size_t>> reads = channelGraph(recipe);
    const std::size_t count = recipe.channels.size();
    std::vector<bool> made(count, false);
    order.clear();
    while (order.size() < count)
    {
        bool picked = false;
        for (std::size_t c = 0; c < count && !picked; ++c)
        {
            if (made[c]) continue;
            if (!std::all_of(reads[c].begin(), reads[c].end(), [&](std::size_t j) { return j != c && made[j]; })) continue;
            made[c] = true;
            order.push_back(c);
            picked = true;
        }
        if (picked) continue;
        for (std::size_t c = 0; c < count; ++c)
        {
            if (made[c]) continue;
            error = loopThrough(reads, c);
            if (!error.empty()) return false;
        }
        error = "Les voies se lisent l'une l'autre : une voie ne peut pas dépendre d'elle-même.";
        return false;
    }
    return true;
}

// The channels a formula names (v2, V3…), from 0, as written: wherever it is right or not.
std::vector<std::size_t> namedChannels(std::string_view text)
{
    std::vector<std::size_t> named;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if ((c != 'v' && c != 'V') || (i > 0 && identifierChar(text[i - 1]))) continue;
        std::size_t j = i + 1;
        while (j < text.size() && text[j] >= '0' && text[j] <= '9') ++j;
        if (j == i + 1 || j - i - 1 > 3 || (j < text.size() && identifierChar(text[j]))) continue;
        const std::size_t k = static_cast<std::size_t>(std::stoul(std::string(text.substr(i + 1, j - i - 1))));
        if (k >= 1 && std::find(named.begin(), named.end(), k - 1) == named.end()) named.push_back(k - 1);
    }
    return named;
}

// The formula with the channels after `removed` named one number lower (v3 → v2 when the channel 2 goes).
std::string renumbered(std::string_view text, std::size_t removed)
{
    std::string out;
    for (std::size_t i = 0; i < text.size();)
    {
        const char c = text[i];
        if ((c == 'v' || c == 'V') && (i == 0 || !identifierChar(text[i - 1])))
        {
            std::size_t j = i + 1;
            while (j < text.size() && text[j] >= '0' && text[j] <= '9') ++j;
            if (j > i + 1 && j - i - 1 <= 3 && (j == text.size() || !identifierChar(text[j])))
            {
                const auto k = static_cast<std::size_t>(std::stoul(std::string(text.substr(i + 1, j - i - 1))));
                if (k > removed + 1)
                {
                    out += c;
                    out += std::to_string(k - 1);
                    i = j;
                    continue;
                }
            }
        }
        out += c;
        ++i;
    }
    return out;
}

// The values of an operation for every sample: from the channels already made and the components above it
// (`above`, empty if none). A value that is not a finite number becomes 0, and a warning says so.
std::vector<double> operationValues(OperationProgram &p, double fs, std::size_t n, std::span<const double> above,
                                    std::span<const std::vector<double>> channels, const model::Progress &progress,
                                    std::string_view what, ComponentSamples &out)
{
    std::vector<double> result(n);
    std::vector<double> &values = p.values;
    std::vector<std::span<const double>> read;
    for (std::size_t k : p.reads)
        read.push_back(k < channels.size() && channels[k].size() == n ? std::span<const double>(channels[k])
                                                                       : std::span<const double>());
    std::size_t undefined = 0, first = 0;
    for (std::size_t i = 0; i < n; ++i)
    {
        if ((i & 0xFFFF) == 0xFFFF && progress.stopped())
        {
            out.cancelled = true;
            return {};
        }
        values[0] = static_cast<double>(i);
        values[1] = static_cast<double>(i) / fs;
        values[p.above] = above.size() == n ? above[i] : 0.0;
        for (std::size_t r = 0; r < p.reads.size(); ++r)
        {
            const double v = read[r].empty() ? 0.0 : read[r][i];
            values[p.channels + p.reads[r]] = v;
            values[p.channels + kChannelNames + p.reads[r]] = v;
        }
        const double v = p.formula.evaluate(values);
        if (std::isfinite(v))
        {
            result[i] = v;
            continue;
        }
        if (undefined++ == 0) first = i;
        result[i] = 0.0;
    }
    if (undefined > 0)
        out.warnings.push_back(std::string(what) + " n'est pas un nombre fini en " + grouped(undefined) +
                               (undefined > 1 ? " échantillons" : " échantillon") + " (le premier à t = " +
                               quantity(static_cast<double>(first) / fs, "s") +
                               " : une division par zéro, la racine d'un nombre négatif ?) : " +
                               (undefined > 1 ? "ils valent 0." : "il vaut 0."));
    return result;
}

ComponentSamples operationSamples(const GeneratorComponent &c, double fs, std::size_t n, std::span<const double> above,
                                  std::span<const std::vector<double>> channels, const model::Progress &progress)
{
    ComponentSamples out;
    OperationProgram p = compileOperation(c, fs, n);
    if (!p.error.empty())
    {
        out.error = p.error;
        return out;
    }
    out.readsAbove = p.readsAbove;
    out.samples = operationValues(p, fs, n, above, channels, progress, "y", out);
    return out;
}

// The Hilbert transform of a record (the imaginary part of its analytic signal), by the DFT of the whole record:
// exact for what is periodic over it; a record that does not end as it starts is taken as repeated, and its
// transform rings near both ends.
std::vector<double> hilbertTransform(std::span<const double> x)
{
    const std::size_t n = x.size();
    std::vector<double> out(n, 0.0);
    if (n < 2) return out;
    std::vector<dsp::Complex> spectrum(n);
    for (std::size_t i = 0; i < n; ++i) spectrum[i] = dsp::Complex(x[i], 0.0);
    const dsp::FftPlan plan(n);
    plan.forward(spectrum);
    // The analytic signal: the positive frequencies twice, the negative ones gone (0 and fs/2 once).
    const std::size_t half = n / 2;
    for (std::size_t k = 1; k < n; ++k)
    {
        if (k < half || (k == half && n % 2 != 0))
            spectrum[k] *= 2.0;
        else if (!(k == half && n % 2 == 0))
            spectrum[k] = dsp::Complex(0.0, 0.0);
    }
    plan.inverse(spectrum);
    for (std::size_t i = 0; i < n; ++i) out[i] = spectrum[i].imag();
    return out;
}

ComponentSamples modulationSamples(const GeneratorComponent &c, double fs, std::size_t n, std::span<const double> above,
                                   std::span<const std::vector<double>> channels, const model::Progress &progress)
{
    ComponentSamples out;
    OperationProgram p = compileOperation(c, fs, n);
    if (!p.error.empty())
    {
        out.error = p.error;
        return out;
    }
    out.readsAbove = p.readsAbove;
    const std::vector<double> message = operationValues(p, fs, n, above, channels, progress, "Le message", out);
    if (out.cancelled) return out;

    const dsp::Modulation type = c.modulation == CarrierModulation::Fm   ? dsp::Modulation::Fm
                                 : c.modulation == CarrierModulation::Pm ? dsp::Modulation::Pm
                                                                         : dsp::Modulation::Am;
    dsp::AnalogModulator modulator(type, fs, c.amplitude, c.amIndex, c.deviationHz, c.phaseDeviationDegrees * dsp::kPi / 180.0);
    // Without its carrier: the envelope is the message itself (double sideband), or the message and its Hilbert
    // transform in quadrature, A (m ± j m̂): the spectrum of the message on one side of the carrier only (upper: +).
    const bool sideband = c.modulation == CarrierModulation::Usb || c.modulation == CarrierModulation::Lsb;
    const bool suppressed = sideband || c.modulation == CarrierModulation::Dsb;
    std::vector<double> quadrature;
    if (sideband)
    {
        if (n > kMaximumSidebandSamples)
        {
            out.error = "une bande latérale unique se calcule sur " + grouped(kMaximumSidebandSamples) +
                        " échantillons au plus (la transformée de Hilbert du message porte sur tout le signal) : " +
                        "raccourcissez la durée ou baissez la fréquence d'échantillonnage.";
            return out;
        }
        quadrature = hilbertTransform(message);
        if (c.modulation == CarrierModulation::Lsb)
            for (double &v : quadrature) v = -v;
    }
    // As qacq::dsp::modulate: Re{b exp(jφ) exp(j 2π fc t)}.
    const std::complex<double> rotation(std::cos(c.phaseDegrees * dsp::kPi / 180.0), std::sin(c.phaseDegrees * dsp::kPi / 180.0));
    const double ratio = c.frequencyHz / fs;
    out.samples.resize(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const std::complex<double> envelope =
            suppressed ? c.amplitude * std::complex<double>(message[i], sideband ? quadrature[i] : 0.0) : modulator.next(message[i]);
        const std::complex<double> b = envelope * rotation;
        const double turn = ratio * static_cast<double>(i);
        const double angle = dsp::kTwoPi * (turn - std::floor(turn));
        out.samples[i] = (b * std::complex<double>(std::cos(angle), std::sin(angle))).real();
    }

    if (c.frequencyHz >= 0.5 * fs)
        out.warnings.push_back("la porteuse (" + quantity(c.frequencyHz, "Hz") + ") dépasse fs/2 (" + quantity(0.5 * fs, "Hz") +
                               ") : elle se replie.");
    double low = 0.0, high = 0.0;
    if (!message.empty())
    {
        const auto [lowest, highest] = std::minmax_element(message.begin(), message.end());
        low = *lowest;
        high = *highest;
    }
    if (c.modulation == CarrierModulation::Am)
    {
        const double depth = std::abs(c.amIndex) * std::max(std::abs(low), std::abs(high));
        if (depth > 1.0 + 1e-12)
            out.warnings.push_back("surmodulation : m × crête du message = " + quantity(depth, "") +
                                   " > 1 ; l'enveloppe ne suit plus le message (une détection d'enveloppe le déforme).");
    }
    else if (c.modulation == CarrierModulation::Fm)
    {
        const double a = c.frequencyHz + c.deviationHz * low, b = c.frequencyHz + c.deviationHz * high;
        const double fmin = std::min(a, b), fmax = std::max(a, b);
        if (fmin < 0.0 || fmax > 0.5 * fs)
            out.warnings.push_back("la fréquence instantanée va de " + quantity(fmin, "Hz") + " à " + quantity(fmax, "Hz") +
                                   " : hors de 0…fs/2 (" + quantity(0.5 * fs, "Hz") + "), le spectre se replie.");
    }
    return out;
}

// A recorded signal: the part of the record read, at the rate of the recipe, placed in the signal.
ComponentSamples recordedSamples(const GeneratorComponent &c, double fs, std::size_t n, std::span<const model::SignalPtr> sources,
                                 const model::Progress &progress)
{
    ComponentSamples out;
    const RecordedSource source = findRecordedSource(c, sources);
    if (!source.ok())
    {
        out.error = source.error;
        return out;
    }
    const model::Signal &signal = *source.signal;
    const std::span<const double> record = signal.channel(source.channel);
    const double rate = signal.sampleRateHz;
    if (record.empty() || !(std::isfinite(rate) && rate > 0.0))
    {
        out.error = "le signal « " + signal.name + " » est vide.";
        return out;
    }
    // From where in the record.
    const double skipped = std::isfinite(c.sourceStartSeconds) ? std::max(0.0, c.sourceStartSeconds) * rate : 0.0;
    if (skipped >= static_cast<double>(record.size()))
    {
        out.error = "le début choisi (" + quantity(c.sourceStartSeconds, "s") + ") est après la fin de l'enregistrement (" +
                    quantity(static_cast<double>(record.size()) / rate, "s") + ").";
        return out;
    }
    std::span<const double> part = record.subspan(static_cast<std::size_t>(std::min(skipped + 0.5, static_cast<double>(record.size() - 1))));
    // Where it starts in the signal made (a whole number of samples).
    const double delaySamples = std::isfinite(c.eventSeconds) ? std::clamp(std::round(c.eventSeconds * fs), -1e15, 1e15) : 0.0;
    const auto delay = static_cast<long long>(delaySamples);
    out.samples.assign(n, 0.0);
    if (!c.sourceLoop)
    {
        // No more of the record than the signal lasts (and what the interpolation reads beyond).
        const double needed = (static_cast<double>(n) - delaySamples) / fs * rate + 64.0;
        if (needed <= 64.0)
        {
            out.warnings.push_back("retardé de " + quantity(c.eventSeconds, "s") + ", il commence après la fin du signal : rien n'en est lu.");
            return out;
        }
        if (needed < static_cast<double>(part.size())) part = part.first(static_cast<std::size_t>(std::ceil(needed)));
    }
    std::vector<double> converted;
    std::span<const double> read = part;
    const bool sameRate = std::abs(rate - fs) <= 1e-9 * fs;
    if (!sameRate)
    {
        converted = resample(part, rate, fs, c.sourceLoop, progress);
        if (progress.stopped())
        {
            out.cancelled = true;
            return out;
        }
        read = converted;
        std::string text = "rééchantillonné de " + quantity(rate, "Hz") + " à " + quantity(fs, "Hz");
        if (fs < rate) text += " : ce qu'il contient au-delà de " + quantity(0.45 * fs, "Hz") + " est retiré";
        out.warnings.push_back(text + ".");
    }
    if (read.empty())
    {
        out.error = "il ne reste aucun échantillon de l'enregistrement à cette fréquence d'échantillonnage.";
        return out;
    }
    const auto length = static_cast<long long>(read.size());
    double factor = c.amplitude;
    if (c.sourceNormalize)
    {
        // The peak of what is placed in the signal (in a loop: of the whole part read).
        const long long first = c.sourceLoop ? 0 : std::max(0LL, -delay);
        const long long last = c.sourceLoop ? length : std::min(length, static_cast<long long>(n) - delay);
        double peak = 0.0;
        for (long long j = first; j < last; ++j)
        {
            const double v = read[static_cast<std::size_t>(j)];
            if (std::isfinite(v)) peak = std::max(peak, std::abs(v));
        }
        if (peak > 0.0)
        {
            factor = c.amplitude / peak;
        }
        else
        {
            factor = 0.0;
            out.warnings.push_back("la partie lue de l'enregistrement est nulle : rien à mettre à l'échelle.");
        }
    }
    for (std::size_t i = 0; i < n; ++i)
    {
        long long j = static_cast<long long>(i) - delay;
        if (c.sourceLoop)
            j = ((j % length) + length) % length;
        else if (j < 0 || j >= length)
            continue;
        out.samples[i] = factor * read[static_cast<std::size_t>(j)];
    }
    if (!c.sourceLoop && static_cast<double>(delay) + static_cast<double>(length) < static_cast<double>(n))
        out.warnings.push_back("l'enregistrement s'arrête à " + quantity((static_cast<double>(delay) + static_cast<double>(length)) / fs, "s") +
                               " : la suite vaut 0 (« En boucle » le répète).");
    return out;
}

// A line of a digital frame: the level of the tick each sample falls in (a millionth of a tick early, so that a
// sample right on an edge is after it whatever the rounding).
ComponentSamples frameSamples(const GeneratorComponent &c, double fs, std::size_t n)
{
    ComponentSamples out;
    const FrameTiming timing = frameTiming(c, fs);
    if (!timing.ok())
    {
        out.error = timing.frame.error;
        return out;
    }
    const BuiltFrame &frame = timing.frame;
    const std::vector<std::uint8_t> &levels = frame.levels[timing.line];
    const double high = c.amplitude, low = c.frameLow;
    const double rest = frame.idle[timing.line] ? high : low;
    const double ticks = static_cast<double>(frame.tickCount);
    const double perSample = 1.0 / timing.samplesPerTick; // ticks a sample
    const double startSample = c.eventSeconds * fs;
    const double period = ticks + std::max(0.0, c.frameGapBits) * static_cast<double>(frame.ticksPerBit);
    out.samples.resize(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        double at = (static_cast<double>(i) - startSample) * perSample + 1e-6;
        double value = rest;
        if (at >= 0.0)
        {
            if (c.frameRepeat) at = std::max(0.0, at - period * std::floor(at / period));
            if (at < ticks) value = levels[static_cast<std::size_t>(at)] ? high : low;
        }
        out.samples[i] = value;
    }

    const double duration = static_cast<double>(n) / fs;
    if (timing.samplesPerTick < 1.0 - 1e-9)
        out.warnings.push_back(quantity(timing.samplesPerBit, "") + (timing.samplesPerBit >= 2.0 ? " échantillons" : " échantillon") +
                               " par bit à " + quantity(c.frequencyHz, "bit/s") + " : il en faut au moins " +
                               std::to_string(frame.ticksPerBit) +
                               " pour que tous ses niveaux y soient ; montez la fréquence d'échantillonnage ou baissez le débit "
                               "(« Cadrer la trame » le fait).");
    if (!c.frameRepeat)
    {
        if (c.eventSeconds >= duration)
            out.warnings.push_back("elle commence à " + quantity(c.eventSeconds, "s") + ", après la fin du signal (" +
                                   quantity(duration, "s") + ") : rien n'en est généré.");
        else if (timing.endSeconds > duration * (1.0 + 1e-9) + 0.5 / fs)
            out.warnings.push_back("elle dure " + quantity(timing.durationSeconds, "s") + " et finit à " + quantity(timing.endSeconds, "s") +
                                   ", après la fin du signal (" + quantity(duration, "s") + ") : sa fin manque (« Cadrer la trame » ajuste la durée).");
    }
    for (const std::string &w : frame.warnings) out.warnings.push_back(w);
    return out;
}

std::string countText(std::size_t channels)
{
    return channels == 1 ? std::string("une voie") : std::to_string(channels) + " voies";
}

// "la voie 3", "les voies 3 et 4", "les voies 2, 3 et 5" (numbers from 1).
std::string channelList(const std::vector<std::size_t> &channels)
{
    if (channels.size() == 1) return "la voie " + std::to_string(channels[0] + 1);
    std::string text = "les voies ";
    for (std::size_t i = 0; i < channels.size(); ++i)
        text += (i == 0 ? "" : i + 1 == channels.size() ? " et " : ", ") + std::to_string(channels[i] + 1);
    return text;
}
} // namespace

OperationCheck checkOperation(const GeneratorRecipe &recipe, std::size_t channel, std::size_t component)
{
    OperationCheck out;
    if (channel >= recipe.channels.size() || component >= recipe.channels[channel].components.size()) return out;
    const GeneratorComponent &c = recipe.channels[channel].components[component];
    if (!operationKind(c.waveform)) return out;
    const OperationProgram p = compileOperation(c, recipe.sampleRateHz, generatorSampleCount(recipe));
    out.channels = p.reads;
    out.readsAbove = p.readsAbove;
    if (!p.error.empty())
    {
        out.error = p.error;
        return out;
    }
    const std::string what = c.waveform == Kind::Modulation ? "Message" : "y";
    for (std::size_t k : p.reads)
    {
        if (k == channel)
        {
            out.error = what + " : « v" + std::to_string(k + 1) + " » est la voie elle-même ; x donne les formes d'onde au-dessus de celle-ci.";
            return out;
        }
        if (k >= recipe.channels.size())
        {
            out.error = what + " : la voie " + std::to_string(k + 1) + " n'existe pas (le signal a " + countText(recipe.channels.size()) + ").";
            return out;
        }
    }
    // Through other channels back to this one.
    if (!p.reads.empty()) out.error = loopThrough(channelGraph(recipe), channel);
    return out;
}

std::string defaultOperation(std::size_t channel, std::size_t channelCount)
{
    std::vector<std::size_t> others;
    for (std::size_t k = 0; k < channelCount && others.size() < 2; ++k)
        if (k != channel) others.push_back(k);
    if (others.empty()) return "x";
    if (others.size() == 1) return "v" + std::to_string(others[0] + 1) + " + x";
    return "v" + std::to_string(others[0] + 1) + " + v" + std::to_string(others[1] + 1);
}

std::vector<OperationModel> operationModels(std::size_t channel, std::size_t channelCount)
{
    std::vector<std::string> others;
    for (std::size_t k = 0; k < channelCount && others.size() < 2 && k < kChannelNames; ++k)
        if (k != channel) others.push_back("v" + std::to_string(k + 1));
    const std::string a = others.empty() ? std::string("x") : others[0];
    std::vector<OperationModel> list;
    if (!others.empty())
    {
        const std::string b = others.size() > 1 ? others[1] : std::string("x");
        list.push_back({"sum", "Somme", a + " + " + b});
        list.push_back({"difference", "Différence", a + " - " + b});
        list.push_back({"product", "Produit", a + "*" + b});
        list.push_back({"ratio", "Quotient", a + "/" + b});
        list.push_back({"mean", "Moyenne", "(" + a + " + " + b + ")/2"});
        list.push_back({"magnitude", "Module", "sqrt(" + a + "^2 + " + b + "^2)"});
    }
    list.push_back({"abs", "Valeur absolue", "abs(" + a + ")"});
    list.push_back({"square", "Carré", a + "^2"});
    list.push_back({"halfwave", "Redressement simple alternance", "max(" + a + "; 0)"});
    return list;
}

bool applyOperationModel(GeneratorComponent &component, std::string_view key, std::size_t channel, std::size_t channelCount)
{
    for (const OperationModel &m : operationModels(channel, channelCount))
    {
        if (m.key != key) continue;
        component.expression = m.formula;
        return true;
    }
    return false;
}

std::vector<OperationModel> messageModels(std::size_t channel, std::size_t channelCount)
{
    std::vector<OperationModel> list = {{"sine", "Un sinus de fréquence fm", "sin(2*pi*fm*t)"},
                                        {"square", "Un carré de fréquence fm", "sign(sin(2*pi*fm*t))"},
                                        {"above", "Les formes d'onde au-dessus", "x"}};
    for (std::size_t k = 0; k < channelCount && k < kChannelNames; ++k)
        if (k != channel) list.push_back({"v" + std::to_string(k + 1), "La voie " + std::to_string(k + 1), "v" + std::to_string(k + 1)});
    return list;
}

bool applyMessageModel(GeneratorComponent &component, std::string_view key, std::size_t channel, std::size_t channelCount)
{
    for (const OperationModel &m : messageModels(channel, channelCount))
    {
        if (m.key != key) continue;
        component.message = m.formula;
        if (key == "sine" || key == "square")
        {
            // fm: a tenth of the carrier, unless the variables have one.
            const Variables v = parseVariables(component.variables, {}, {});
            if (std::find(v.names.begin(), v.names.end(), "fm") == v.names.end())
            {
                const std::string fm = "fm = " + plot::formatCompact(component.frequencyHz / 10.0, "", 6);
                const std::string_view before = trim(component.variables);
                component.variables = before.empty() ? fm : std::string(before) + " ; " + fm;
            }
        }
        return true;
    }
    return false;
}

std::vector<std::size_t> channelReaders(const GeneratorRecipe &recipe, std::size_t channel)
{
    std::vector<std::size_t> readers;
    for (std::size_t c = 0; c < recipe.channels.size(); ++c)
    {
        if (c == channel) continue;
        for (const GeneratorComponent &k : recipe.channels[c].components)
        {
            if (!operationKind(k.waveform)) continue;
            const std::vector<std::size_t> named = namedChannels(k.waveform == Kind::Modulation ? k.message : k.expression);
            if (std::find(named.begin(), named.end(), channel) == named.end()) continue;
            readers.push_back(c);
            break;
        }
    }
    return readers;
}

bool removeGeneratorChannel(GeneratorRecipe &recipe, std::size_t channel, std::string *error)
{
    const auto fail = [&](std::string why) {
        if (error) *error = std::move(why);
        return false;
    };
    if (channel >= recipe.channels.size()) return fail("Cette voie n'existe pas.");
    if (recipe.channels.size() <= 1) return fail("Le signal garde au moins une voie.");
    const std::vector<std::size_t> readers = channelReaders(recipe, channel);
    if (!readers.empty())
    {
        const bool one = readers.size() == 1;
        return fail("La voie " + std::to_string(channel + 1) + " est lue par " + channelList(readers) + " (v" +
                    std::to_string(channel + 1) + (one ? " dans son opération ou son message" : " dans leurs opérations ou messages") +
                    ") : changez d'abord " + (one ? "sa formule." : "leurs formules."));
    }
    recipe.channels.erase(recipe.channels.begin() + static_cast<std::ptrdiff_t>(channel));
    for (GeneratorChannel &c : recipe.channels)
    {
        for (GeneratorComponent &k : c.components)
        {
            if (!operationKind(k.waveform)) continue;
            k.expression = renumbered(k.expression, channel);
            k.message = renumbered(k.message, channel);
        }
    }
    return true;
}

std::string_view carrierModulationKey(CarrierModulation modulation) noexcept
{
    switch (modulation)
    {
        case CarrierModulation::Am: return "am";
        case CarrierModulation::Fm: return "fm";
        case CarrierModulation::Pm: return "pm";
        case CarrierModulation::Dsb: return "dsb";
        case CarrierModulation::Usb: return "usb";
        case CarrierModulation::Lsb: return "lsb";
    }
    return "am";
}

bool dspWaveform(WaveKind waveform, dsp::Waveform &out) noexcept
{
    switch (waveform)
    {
        case Kind::Sine: out = dsp::Waveform::Sine; return true;
        case Kind::Square: out = dsp::Waveform::Square; return true;
        case Kind::Triangle: out = dsp::Waveform::Triangle; return true;
        case Kind::Sawtooth: out = dsp::Waveform::Sawtooth; return true;
        case Kind::Pulse: out = dsp::Waveform::Pulse; return true;
        case Kind::Dc: out = dsp::Waveform::Dc; return true;
        case Kind::Step: out = dsp::Waveform::Step; return true;
        case Kind::Impulse: out = dsp::Waveform::Impulse; return true;
        case Kind::Noise: out = dsp::Waveform::WhiteNoise; return true;
        case Kind::Sweep: out = dsp::Waveform::Chirp; return true;
        case Kind::FourierSeries:
        case Kind::Drawn:
        case Kind::Recurrence:
        case Kind::Formula:
        case Kind::Modulation:
        case Kind::Recorded:
        case Kind::Frame: return false;
    }
    return false;
}

GeneratorComponent withWaveform(const GeneratorComponent &component, Kind waveform, double sampleRateHz,
                                double durationSeconds)
{
    if (waveform == component.waveform) return component;
    GeneratorComponent c = defaultGeneratorComponent(waveform, sampleRateHz, durationSeconds);
    c.enabled = component.enabled;
    c.amplitude = component.amplitude;
    // A drawn motif drawn once over the signal has no frequency nor phase to give.
    const bool repeated = component.waveform != Kind::Drawn || !component.motifOnce;
    const auto tonal = [](Kind w) {
        return periodic(w) || w == Kind::FourierSeries || w == Kind::Drawn || w == Kind::Modulation;
    };
    if (tonal(component.waveform) && tonal(waveform) && repeated) c.frequencyHz = component.frequencyHz;
    const auto phased = [](Kind w) { return periodic(w) || w == Kind::Drawn || w == Kind::Modulation; };
    if (phased(component.waveform) && phased(waveform) && repeated) c.phaseDegrees = component.phaseDegrees;
    // A carrier of the frequency kept: its message of a tenth of it, its excursion of a fifth.
    if (waveform == Kind::Modulation && component.waveform != Kind::Modulation)
    {
        c.variables = "fm = " + plot::formatCompact(c.frequencyHz / 10.0, "", 6);
        c.deviationHz = c.frequencyHz / 5.0;
    }
    const auto duty = [](Kind w) { return w == Kind::Square || w == Kind::Pulse; };
    if (duty(component.waveform) && duty(waveform)) c.dutyCycle = component.dutyCycle;
    const auto limited = [](Kind w) { return hasSeries(w) || w == Kind::Drawn; };
    if (limited(component.waveform) && limited(waveform))
    {
        c.bandLimit = component.bandLimit;
        c.harmonics = component.harmonics;
    }
    if (waveform == Kind::Drawn)
    {
        // The same samples as the waveform (a sine: close to it).
        std::vector<MotifPoint> points;
        MotifInterpolation interpolation = MotifInterpolation::Linear;
        if (waveformMotif(component.waveform, component.dutyCycle, points, interpolation))
        {
            c.motif = normalizedMotif(std::move(points));
            c.interpolation = interpolation;
        }
    }
    if (waveform == Kind::FourierSeries && hasSeries(component.waveform))
    {
        // The same waveform written as its series: every harmonic below fs/2 (or as many as it had).
        applySeriesModel(c, waveformKey(component.waveform), component.amplitude, component.dutyCycle);
        c.harmonics = component.bandLimit == dsp::BandLimit::Fourier ? component.harmonics : 0;
    }
    const auto event = [](Kind w) { return w == Kind::Step || w == Kind::Impulse; };
    if (event(component.waveform) && event(waveform)) c.eventSeconds = component.eventSeconds;
    // A sine made a recurrence: the oscillator of the same sine.
    if (waveform == Kind::Recurrence && component.waveform == Kind::Sine)
        applyRecurrenceModel(c, "oscillator", component.amplitude, component.frequencyHz, component.phaseDegrees);
    return c;
}

std::size_t generatorSampleCount(const GeneratorRecipe &recipe) noexcept
{
    const double n = std::round(recipe.durationSeconds * recipe.sampleRateHz);
    if (!std::isfinite(n) || n < 1.0) return 1;
    if (n > 1e15) return static_cast<std::size_t>(1e15);
    return static_cast<std::size_t>(n);
}

std::string checkGeneratorRecipe(const GeneratorRecipe &recipe)
{
    if (!(std::isfinite(recipe.sampleRateHz) && recipe.sampleRateHz > 0.0))
        return "La fréquence d'échantillonnage doit être positive.";
    if (!(std::isfinite(recipe.durationSeconds) && recipe.durationSeconds > 0.0)) return "La durée doit être positive.";
    if (recipe.channels.empty()) return "Le signal n'a aucune voie.";
    if (recipe.channels.size() > kMaximumGeneratorChannels)
        return "Au plus " + std::to_string(kMaximumGeneratorChannels) + " voies.";
    if (std::none_of(recipe.channels.begin(), recipe.channels.end(), [](const GeneratorChannel &c) { return c.output; }))
        return "Aucune voie n'est gardée dans le signal : cochez « Dans le signal » pour l'une d'elles au moins.";
    for (const GeneratorChannel &c : recipe.channels)
    {
        if (c.components.size() > kMaximumGeneratorComponents)
            return "Au plus " + std::to_string(kMaximumGeneratorComponents) + " composantes par voie.";
    }
    const std::size_t n = generatorSampleCount(recipe);
    const double total = static_cast<double>(n) * static_cast<double>(recipe.channels.size());
    if (total > static_cast<double>(kMaximumGeneratorSamples))
    {
        std::string text = "Le signal compterait " + grouped(static_cast<std::size_t>(total)) + " échantillons";
        if (recipe.channels.size() > 1) text += " (" + grouped(n) + " par voie)";
        return text + " : au plus " + grouped(kMaximumGeneratorSamples) +
               ", toutes voies comprises. Raccourcissez la durée ou baissez la fréquence d'échantillonnage.";
    }
    return {};
}

std::string_view waveformKey(Kind waveform) noexcept
{
    for (const auto &[w, key] : kKeys)
        if (w == waveform) return key;
    return "sine";
}

bool waveformFromKey(std::string_view key, Kind &waveform) noexcept
{
    for (const auto &[w, k] : kKeys)
    {
        if (k == key)
        {
            waveform = w;
            return true;
        }
    }
    return false;
}

std::string_view generatorWaveformName(Kind waveform) noexcept
{
    switch (waveform)
    {
        case Kind::Sine: return "Sinus";
        case Kind::Square: return "Carré";
        case Kind::Triangle: return "Triangle";
        case Kind::Sawtooth: return "Dent de scie";
        case Kind::Pulse: return "Impulsions";
        case Kind::Dc: return "Continu";
        case Kind::Step: return "Échelon";
        case Kind::Impulse: return "Dirac";
        case Kind::Noise: return "Bruit";
        case Kind::Sweep: return "Balayage";
        case Kind::FourierSeries: return "Série de Fourier";
        case Kind::Drawn: return "Motif dessiné";
        case Kind::Recurrence: return "Récurrence";
        case Kind::Formula: return "Opération entre voies";
        case Kind::Modulation: return "Porteuse modulée";
        case Kind::Recorded: return "Signal enregistré";
        case Kind::Frame: return "Trame numérique";
    }
    return "?";
}

model::Json toJson(const GeneratorComponent &c)
{
    model::Json j = model::Json::object();
    j["waveform"] = waveformKey(c.waveform);
    j["enabled"] = c.enabled;
    j["amplitude"] = c.amplitude;
    j["frequency"] = c.frequencyHz;
    j["phase"] = c.phaseDegrees;
    j["duty"] = c.dutyCycle;
    j["time"] = c.eventSeconds;
    j["endFrequency"] = c.endFrequencyHz;
    j["law"] = c.sweepLaw == dsp::ChirpLaw::Logarithmic ? "log" : "linear";
    j["noise"] = c.noise == dsp::NoiseDistribution::Uniform ? "uniform" : "gaussian";
    j["bandLimit"] = c.bandLimit == dsp::BandLimit::Fourier ? "fourier" : "sampled";
    j["harmonics"] = static_cast<unsigned long long>(c.harmonics);
    if (c.waveform == Kind::FourierSeries)
    {
        j["form"] = c.seriesForm == SeriesForm::Cartesian ? "cartesian" : c.seriesForm == SeriesForm::Polar ? "polar" : "ab";
        j["variables"] = c.variables;
        j["a0"] = c.formula0;
        j["an"] = c.formulaA;
        j["bn"] = c.formulaB;
    }
    if (c.waveform == Kind::Drawn)
    {
        model::Json points = model::Json::array();
        for (const MotifPoint &p : c.motif)
        {
            model::Json pair = model::Json::array();
            pair.push_back(p.x);
            pair.push_back(p.y);
            points.push_back(std::move(pair));
        }
        j["motif"] = std::move(points);
        j["interpolation"] = c.interpolation == MotifInterpolation::Steps    ? "steps"
                             : c.interpolation == MotifInterpolation::Smooth ? "smooth"
                                                                             : "linear";
        j["once"] = c.motifOnce;
    }
    if (c.waveform == Kind::Recurrence)
    {
        j["recurrence"] = c.recurrence;
        j["initial"] = c.initialValues;
        j["input"] = inputKey(c.recurrenceInput);
        j["variables"] = c.variables;
    }
    if (c.waveform == Kind::Formula)
    {
        j["expression"] = c.expression;
        j["variables"] = c.variables;
    }
    if (c.waveform == Kind::Modulation)
    {
        j["modulation"] = carrierModulationKey(c.modulation);
        j["message"] = c.message;
        j["variables"] = c.variables;
        j["index"] = c.amIndex;
        j["deviation"] = c.deviationHz;
        j["phaseDeviation"] = c.phaseDeviationDegrees;
    }
    if (c.waveform == Kind::Recorded)
    {
        j["source"] = static_cast<unsigned long long>(c.sourceId);
        j["sourceName"] = c.sourceName;
        j["sourceFrames"] = static_cast<unsigned long long>(c.sourceFrames);
        j["sourceChannel"] = static_cast<unsigned long long>(c.sourceChannel);
        j["sourceChannelName"] = c.sourceChannelName;
        j["sourceStart"] = c.sourceStartSeconds;
        j["loop"] = c.sourceLoop;
        j["normalize"] = c.sourceNormalize;
    }
    if (c.waveform == Kind::Frame)
    {
        frameToJson(c.frame, j);
        j["line"] = frameLines(c.frame.protocol)[frameLineIndex(c.frame.protocol, c.frameLine)].key;
        j["low"] = c.frameLow;
        j["repeat"] = c.frameRepeat;
        j["gap"] = c.frameGapBits;
        j["group"] = static_cast<unsigned long long>(c.frameGroup);
    }
    return j;
}

model::Json toJson(const GeneratorChannel &channel)
{
    model::Json j = model::Json::object();
    j["name"] = channel.name;
    j["unit"] = channel.unit;
    j["offset"] = channel.offset;
    j["output"] = channel.output;
    model::Json list = model::Json::array();
    for (const GeneratorComponent &c : channel.components) list.push_back(toJson(c));
    j["components"] = std::move(list);
    return j;
}

model::Json toJson(const GeneratorRecipe &recipe)
{
    model::Json j = model::Json::object();
    j["name"] = recipe.name;
    j["sampleRate"] = recipe.sampleRateHz;
    j["duration"] = recipe.durationSeconds;
    j["seed"] = static_cast<unsigned long long>(recipe.seed);
    model::Json list = model::Json::array();
    for (const GeneratorChannel &c : recipe.channels) list.push_back(toJson(c));
    j["channels"] = std::move(list);
    return j;
}

GeneratorComponent generatorComponentFromJson(const model::Json &j)
{
    GeneratorComponent c;
    if (!j.isObject()) return c;
    waveformFromKey(j["waveform"].toString(), c.waveform);
    c.enabled = j["enabled"].toBool(true);
    c.amplitude = number(j, "amplitude", c.amplitude, -kLargest, kLargest);
    c.frequencyHz = number(j, "frequency", c.frequencyHz, 0.0, kHighestFrequency);
    c.phaseDegrees = number(j, "phase", 0.0, -1e6, 1e6);
    c.dutyCycle = number(j, "duty", c.dutyCycle, 0.001, 0.999);
    c.eventSeconds = number(j, "time", 0.0, -1e9, 1e9);
    c.endFrequencyHz = number(j, "endFrequency", c.endFrequencyHz, 0.0, kHighestFrequency);
    c.sweepLaw = j["law"].toString() == "log" ? dsp::ChirpLaw::Logarithmic : dsp::ChirpLaw::Linear;
    c.noise = j["noise"].toString() == "uniform" ? dsp::NoiseDistribution::Uniform : dsp::NoiseDistribution::Gaussian;
    c.bandLimit = j["bandLimit"].toString() == "fourier" ? dsp::BandLimit::Fourier : dsp::BandLimit::Sampled;
    c.harmonics = static_cast<std::size_t>(std::floor(number(j, "harmonics", 0.0, 0.0, kLargestHarmonics)));
    if (c.waveform == Kind::FourierSeries)
    {
        const std::string form = j["form"].toString();
        c.seriesForm = form == "cartesian" ? SeriesForm::Cartesian : form == "polar" ? SeriesForm::Polar : SeriesForm::SineCosine;
        c.variables = j["variables"].toString();
        c.formula0 = j["a0"].toString("0");
        c.formulaA = j["an"].toString("0");
        c.formulaB = j["bn"].toString("0");
    }
    if (c.waveform == Kind::Drawn)
    {
        // [[x, y], ...] (or [{"x": .., "y": ..}, ...]).
        std::vector<MotifPoint> points;
        for (const model::Json &item : j["motif"].items())
        {
            if (points.size() >= kMaximumMotifPoints) break;
            MotifPoint p;
            if (item.isArray() && item.size() >= 2 && item.items()[0].isNumber() && item.items()[1].isNumber())
                p = {item.items()[0].toNumber(), item.items()[1].toNumber()};
            else if (item.isObject() && item["x"].isNumber() && item["y"].isNumber())
                p = {item["x"].toNumber(), item["y"].toNumber()};
            else
                continue;
            points.push_back(p);
        }
        c.motif = normalizedMotif(std::move(points));
        const std::string interpolation = j["interpolation"].toString();
        c.interpolation = interpolation == "steps"    ? MotifInterpolation::Steps
                          : interpolation == "smooth" ? MotifInterpolation::Smooth
                                                      : MotifInterpolation::Linear;
        c.motifOnce = j["once"].toBool(false);
    }
    if (c.waveform == Kind::Recurrence)
    {
        c.recurrence = j["recurrence"].toString("0");
        c.initialValues = j["initial"].toString();
        c.recurrenceInput = inputFromKey(j["input"].toString());
        c.variables = j["variables"].toString();
    }
    if (c.waveform == Kind::Formula)
    {
        c.expression = j["expression"].toString("x");
        c.variables = j["variables"].toString();
    }
    if (c.waveform == Kind::Modulation)
    {
        const std::string modulation = j["modulation"].toString();
        c.modulation = CarrierModulation::Am;
        for (const CarrierModulation m : {CarrierModulation::Fm, CarrierModulation::Pm, CarrierModulation::Dsb, CarrierModulation::Usb,
                                          CarrierModulation::Lsb})
            if (modulation == carrierModulationKey(m)) c.modulation = m;
        c.message = j["message"].toString("sin(2*pi*fm*t)");
        c.variables = j["variables"].toString();
        c.amIndex = number(j, "index", c.amIndex, -1e6, 1e6);
        c.deviationHz = number(j, "deviation", c.deviationHz, -kHighestFrequency, kHighestFrequency);
        c.phaseDeviationDegrees = number(j, "phaseDeviation", c.phaseDeviationDegrees, -1e9, 1e9);
    }
    if (c.waveform == Kind::Recorded)
    {
        c.sourceId = static_cast<std::uint64_t>(std::floor(number(j, "source", 0.0, 0.0, 9e15)));
        c.sourceName = j["sourceName"].toString();
        c.sourceFrames = static_cast<std::size_t>(std::floor(number(j, "sourceFrames", 0.0, 0.0, 9e15)));
        c.sourceChannel = static_cast<std::size_t>(std::floor(number(j, "sourceChannel", 0.0, 0.0, 1e6)));
        c.sourceChannelName = j["sourceChannelName"].toString();
        c.sourceStartSeconds = number(j, "sourceStart", 0.0, 0.0, 1e9);
        c.sourceLoop = j["loop"].toBool(false);
        c.sourceNormalize = j["normalize"].toBool(false);
    }
    if (c.waveform == Kind::Frame)
    {
        c.frame = frameFromJson(j);
        c.frameLine = std::string(frameLines(c.frame.protocol)[frameLineIndex(c.frame.protocol, j["line"].toString())].key);
        c.frameLow = number(j, "low", 0.0, -kLargest, kLargest);
        c.frameRepeat = j["repeat"].toBool(false);
        c.frameGapBits = number(j, "gap", 10.0, 0.0, 1e9);
        c.frameGroup = static_cast<std::uint64_t>(std::floor(number(j, "group", 0.0, 0.0, 9e15)));
    }
    return c;
}

GeneratorChannel generatorChannelFromJson(const model::Json &j, std::size_t index)
{
    GeneratorChannel c;
    c.name = j["name"].toString();
    if (c.name.empty()) c.name = "Voie " + std::to_string(index + 1);
    c.unit = j.contains("unit") ? j["unit"].toString() : std::string("V");
    c.offset = number(j, "offset", 0.0, -kLargest, kLargest);
    c.output = j["output"].toBool(true);
    for (const model::Json &item : j["components"].items())
    {
        if (c.components.size() >= kMaximumGeneratorComponents) break;
        Kind w;
        if (!item.isObject() || !waveformFromKey(item["waveform"].toString(), w)) continue;
        c.components.push_back(generatorComponentFromJson(item));
    }
    return c;
}

GeneratorRecipe generatorRecipeFromJson(const model::Json &j)
{
    GeneratorRecipe r;
    r.name = j["name"].toString();
    if (r.name.empty()) r.name = "Signal généré";
    r.sampleRateHz = j["sampleRate"].toNumber(0.0) > 0.0 ? number(j, "sampleRate", 48000.0, 1e-3, 1e11) : 48000.0;
    r.durationSeconds = j["duration"].toNumber(0.0) > 0.0 ? number(j, "duration", 1.0, 1e-9, 1e9) : 1.0;
    r.seed = static_cast<std::uint64_t>(std::floor(number(j, "seed", 1.0, 0.0, 9e15)));
    for (const model::Json &item : j["channels"].items())
    {
        if (r.channels.size() >= kMaximumGeneratorChannels) break;
        if (!item.isObject()) continue;
        r.channels.push_back(generatorChannelFromJson(item, r.channels.size()));
    }
    if (r.channels.empty()) r.channels.push_back(blankGeneratorChannel(0));
    return r;
}

std::string componentText(const GeneratorComponent &c, const std::string &unit)
{
    const std::string amplitude = quantity(c.amplitude, unit);
    const std::string frequency = quantity(c.frequencyHz, "Hz");
    const std::string duty = quantity(100.0 * c.dutyCycle, "%");
    std::string text;
    switch (c.waveform)
    {
        case Kind::Sine:
        case Kind::Triangle:
        case Kind::Sawtooth:
            text = std::string(generatorWaveformName(c.waveform)) + " " + frequency + ", " + amplitude + " crête";
            break;
        case Kind::Square:
            text = "Carré " + frequency + ", " + amplitude + " crête";
            if (std::abs(c.dutyCycle - 0.5) > 1e-9) text += ", rapport cyclique " + duty;
            break;
        case Kind::Pulse: text = "Impulsions " + frequency + ", " + amplitude + ", rapport cyclique " + duty; break;
        case Kind::Dc: text = "Continu " + amplitude; break;
        case Kind::Step: text = "Échelon de " + amplitude + " à " + quantity(c.eventSeconds, "s"); break;
        case Kind::Impulse: text = "Dirac de " + amplitude + " à " + quantity(c.eventSeconds, "s"); break;
        case Kind::Noise:
            text = c.noise == dsp::NoiseDistribution::Uniform ? "Bruit uniforme, ± " + quantity(std::abs(c.amplitude), unit)
                                                              : "Bruit gaussien, écart type " + amplitude;
            break;
        case Kind::Sweep:
            text = "Balayage " + frequency + " → " + quantity(c.endFrequencyHz, "Hz") +
                   (c.sweepLaw == dsp::ChirpLaw::Logarithmic ? " (log.)" : "") + ", " + amplitude + " crête";
            break;
        case Kind::FourierSeries:
        {
            text = "Série de Fourier " + frequency + ", " +
                   (c.harmonics == 0 ? std::string("harmoniques jusqu'à fs/2")
                                     : grouped(c.harmonics) + (c.harmonics > 1 ? " harmoniques" : " harmonique"));
            const std::string *texts[3] = {&c.formula0, &c.formulaA, &c.formulaB};
            std::string terms;
            for (int f = 0; f < 3; ++f)
            {
                const std::string body = formulaBody(*texts[f], f);
                if (body == "0") continue;
                terms += (terms.empty() ? " : " : ", ") + std::string(seriesTermName(c.seriesForm, f)) + " = " + body;
            }
            text += terms.empty() ? std::string(" : nulle") : terms;
            const std::string_view variables = trim(c.variables);
            if (!variables.empty()) text += " (" + std::string(variables) + ")";
            break;
        }
        case Kind::Formula:
        {
            text = "Opération entre voies : " + operationBody(c.expression, false);
            const std::string_view variables = trim(c.variables);
            if (!variables.empty()) text += " (" + std::string(variables) + ")";
            break;
        }
        case Kind::Modulation:
        {
            text = "Porteuse " + frequency + ", " + amplitude + " crête";
            if (c.phaseDegrees != 0.0) text += ", phase " + quantity(c.phaseDegrees, "") + "°";
            switch (c.modulation)
            {
                case CarrierModulation::Am: text += ", modulée en amplitude (m = " + quantity(c.amIndex, "") + ")"; break;
                case CarrierModulation::Fm: text += ", modulée en fréquence (Δf = " + quantity(c.deviationHz, "Hz") + ")"; break;
                case CarrierModulation::Pm: text += ", modulée en phase (Δφ = " + quantity(c.phaseDeviationDegrees, "") + "°)"; break;
                case CarrierModulation::Dsb: text += ", modulée en amplitude sans porteuse (double bande latérale)"; break;
                case CarrierModulation::Usb: text += ", modulée en bande latérale unique (supérieure)"; break;
                case CarrierModulation::Lsb: text += ", modulée en bande latérale unique (inférieure)"; break;
            }
            text += " par " + operationBody(c.message, true);
            const std::string_view variables = trim(c.variables);
            if (!variables.empty()) text += " (" + std::string(variables) + ")";
            break;
        }
        case Kind::Recorded:
        {
            text = "Signal enregistré " + (c.sourceName.empty() ? std::string("(à choisir)") : "« " + c.sourceName + " »");
            if (!c.sourceChannelName.empty()) text += " (voie " + c.sourceChannelName + ")";
            text += c.sourceNormalize ? ", crête ramenée à " + amplitude : c.amplitude != 1.0 ? ", × " + quantity(c.amplitude, "") : std::string();
            if (c.sourceStartSeconds > 0.0) text += ", à partir de " + quantity(c.sourceStartSeconds, "s");
            if (c.eventSeconds != 0.0) text += ", retardé de " + quantity(c.eventSeconds, "s");
            if (c.sourceLoop) text += ", en boucle";
            break;
        }
        case Kind::Frame:
        {
            const BuiltFrame frame = buildFrame(c.frame);
            const std::span<const FrameLineInfo> lines = frameLines(c.frame.protocol);
            const std::string title = frameTitle(c.frame);
            text = (c.frame.protocol == FrameProtocol::Bits ? "" : "Trame ") + title +
                   (title.find(',') == std::string::npos ? " à " : ", à ") + quantity(c.frequencyHz, "bit/s");
            // (The data line of bits sent as they are goes without saying.)
            const std::size_t line = frameLineIndex(c.frame.protocol, c.frameLine);
            if (lines.size() > 1 && (c.frame.protocol != FrameProtocol::Bits || line != 0)) text += ", ligne " + std::string(lines[line].name);
            text += frame.ok() ? " (" + frame.summary + ")" : std::string(" (à corriger)");
            text += ", de " + quantity(c.frameLow, unit) + " à " + amplitude;
            if (c.eventSeconds != 0.0) text += ", à partir de " + quantity(c.eventSeconds, "s");
            if (c.frameRepeat) text += ", répétée";
            break;
        }
        case Kind::Recurrence:
        {
            text = "Récurrence y[n] = " + recurrenceBody(c.recurrence);
            if (c.recurrenceInput != RecurrenceInput::None) text += ", entrée : " + std::string(inputName(c.recurrenceInput));
            const std::string_view variables = trim(c.variables), initial = trim(c.initialValues);
            if (!variables.empty()) text += " (" + std::string(variables) + ")";
            if (!initial.empty()) text += ", " + std::string(initial);
            break;
        }
        case Kind::Drawn:
        {
            const std::size_t points = c.motif.size();
            text = std::string("Motif dessiné ") + (c.motifOnce ? std::string("sur toute la durée") : frequency) + ", " +
                   grouped(points) + (points > 1 ? " points (" : " point (") +
                   (c.interpolation == MotifInterpolation::Steps    ? "paliers"
                    : c.interpolation == MotifInterpolation::Smooth ? "courbe lisse"
                                                                    : "segments") +
                   "), " + amplitude + " crête";
            break;
        }
    }
    const bool drawnRepeated = c.waveform == Kind::Drawn && !c.motifOnce;
    if ((takesPhase(c.waveform) || drawnRepeated) && c.phaseDegrees != 0.0)
        text += ", phase " + quantity(c.phaseDegrees, "") + "°";
    if ((hasSeries(c.waveform) || drawnRepeated) && c.bandLimit == dsp::BandLimit::Fourier)
        text += c.harmonics > 0 ? ", série de Fourier (" + grouped(c.harmonics) + " harmoniques)" : ", série de Fourier";
    if (!c.enabled) text += " (désactivée)";
    return text;
}

RecordedSource findRecordedSource(const GeneratorComponent &c, std::span<const model::SignalPtr> sources)
{
    RecordedSource out;
    if (c.sourceId == 0 && c.sourceName.empty())
    {
        out.error = "Aucun signal enregistré choisi.";
        return out;
    }
    // The one of its id, if it is still the one chosen (ids are those of a session); else the one of its name.
    for (const model::SignalPtr &s : sources)
    {
        if (!s || s->id != c.sourceId || c.sourceId == 0) continue;
        if (c.sourceName.empty() || s->name == c.sourceName || s->frameCount() == c.sourceFrames) out.signal = s;
        break;
    }
    if (!out.signal && !c.sourceName.empty())
    {
        for (const model::SignalPtr &s : sources)
        {
            if (!s || s->name != c.sourceName) continue;
            if (!out.signal || s->frameCount() == c.sourceFrames) out.signal = s;
            if (s->frameCount() == c.sourceFrames) break;
        }
    }
    if (!out.signal)
    {
        out.error = "Le signal enregistré " + (c.sourceName.empty() ? std::string("choisi") : "« " + c.sourceName + " »") +
                    " n'est pas dans la session : choisissez-en un autre.";
        return out;
    }
    const std::vector<model::ChannelInfo> &channels = out.signal->channels;
    std::size_t channel = c.sourceChannel;
    if (!(channel < channels.size() && (c.sourceChannelName.empty() || channels[channel].name == c.sourceChannelName)))
    {
        std::size_t named = channels.size();
        for (std::size_t k = 0; k < channels.size() && !c.sourceChannelName.empty(); ++k)
        {
            if (channels[k].name != c.sourceChannelName) continue;
            named = k;
            break;
        }
        if (named < channels.size())
        {
            channel = named;
        }
        else if (channel >= channels.size())
        {
            out.error = "La voie " + (c.sourceChannelName.empty() ? std::to_string(c.sourceChannel + 1) : "« " + c.sourceChannelName + " »") +
                        " du signal « " + out.signal->name + " » n'existe plus : choisissez-en une autre.";
            out.signal.reset();
            return out;
        }
    }
    out.channel = channel;
    return out;
}

void setRecordedSource(GeneratorComponent &component, const model::Signal &signal, std::size_t channel)
{
    component.sourceId = signal.id;
    component.sourceName = signal.name;
    component.sourceFrames = signal.frameCount();
    component.sourceChannel = channel < signal.channels.size() ? channel : 0;
    component.sourceChannelName = component.sourceChannel < signal.channels.size() ? signal.channels[component.sourceChannel].name : std::string();
}

bool readsRecordedSignals(const GeneratorRecipe &recipe) noexcept
{
    for (const GeneratorChannel &channel : recipe.channels)
        for (const GeneratorComponent &c : channel.components)
            if (c.waveform == Kind::Recorded) return true;
    return false;
}

namespace
{
// How a carrier is modulated, said after "modulée" ("en amplitude (indice 0,8…)").
std::string modulationPhrase(const GeneratorComponent &carrier)
{
    switch (carrier.modulation)
    {
        case CarrierModulation::Am:
            return "en amplitude (indice " + quantity(carrier.amIndex, "") + " : aux crêtes du message, l'enveloppe va de " +
                   quantity(carrier.amplitude * std::abs(1.0 - carrier.amIndex), "V") + " à " +
                   quantity(carrier.amplitude * (1.0 + carrier.amIndex), "V") + ")";
        case CarrierModulation::Fm: return "en fréquence (Δf = " + quantity(carrier.deviationHz, "Hz") + ")";
        case CarrierModulation::Pm: return "en phase (Δφ = " + quantity(carrier.phaseDeviationDegrees, "") + "°)";
        case CarrierModulation::Dsb: return "en amplitude, porteuse supprimée (double bande latérale)";
        case CarrierModulation::Usb: return "en bande latérale unique supérieure";
        case CarrierModulation::Lsb: return "en bande latérale unique inférieure";
    }
    return {};
}
} // namespace

SignalUse useSignalInRecipe(GeneratorRecipe &recipe, const model::Signal &signal, std::size_t sourceChannel, std::string_view use)
{
    SignalUse out;
    if (signal.channels.empty() || sourceChannel >= signal.channels.size())
    {
        out.error = "Ce signal n'a pas cette voie.";
        return out;
    }
    const bool wave = use == "wave";
    CarrierModulation modulation = CarrierModulation::Am;
    bool known = wave || use == "am";
    for (const CarrierModulation m : {CarrierModulation::Fm, CarrierModulation::Pm, CarrierModulation::Dsb, CarrierModulation::Usb, CarrierModulation::Lsb})
    {
        if (use != carrierModulationKey(m)) continue;
        modulation = m;
        known = true;
    }
    if (!known)
    {
        out.error = "Usage inconnu d'un signal : « " + std::string(use) + " ».";
        return out;
    }
    GeneratorRecipe next = recipe;
    out.alone = isBlankRecipe(recipe);
    // Where: the whole page when it is blank; else the first blank channel nothing reads, or after the others.
    std::size_t at = next.channels.size();
    if (!out.alone)
    {
        for (std::size_t k = 0; k < next.channels.size(); ++k)
        {
            if (!isBlankChannel(next.channels[k]) || !channelReaders(next, k).empty()) continue;
            at = k;
            break;
        }
        if (at == next.channels.size() && next.channels.size() >= kMaximumGeneratorChannels)
        {
            out.error = "Au plus " + std::to_string(kMaximumGeneratorChannels) + " voies : retirez-en une du Générateur.";
            return out;
        }
    }
    std::string untimed; // why a blank page could not take the timing of the record
    if (out.alone && std::isfinite(signal.sampleRateHz) && signal.sampleRateHz > 0.0 && signal.frameCount() > 0)
    {
        // A blank page takes the timing of the record (unless it is too large for the generator).
        GeneratorRecipe timed = next;
        timed.sampleRateHz = signal.sampleRateHz;
        timed.durationSeconds = signal.durationSeconds();
        timed.channels = {GeneratorChannel{}};
        untimed = checkGeneratorRecipe(timed);
        if (untimed.empty())
        {
            next.sampleRateHz = timed.sampleRateHz;
            next.durationSeconds = timed.durationSeconds;
            out.timed = true;
        }
    }
    const std::string &channelName = signal.channels[sourceChannel].name;
    const bool several = signal.channels.size() > 1;
    GeneratorChannel channel;
    channel.unit = signal.channels[sourceChannel].unit;
    GeneratorComponent record = defaultGeneratorComponent(Kind::Recorded, next.sampleRateHz, next.durationSeconds);
    setRecordedSource(record, signal, sourceChannel);
    GeneratorComponent carrier;
    if (wave)
    {
        channel.name = several ? channelName : signal.name;
        channel.components.push_back(record);
    }
    else
    {
        // The message of a carrier: scaled to a peak of 1, so that the index or the excursion are what they say.
        record.sourceNormalize = true;
        record.amplitude = 1.0;
        carrier = defaultGeneratorComponent(Kind::Modulation, next.sampleRateHz, next.durationSeconds);
        carrier.message = "x";
        carrier.variables.clear();
        carrier.modulation = modulation;
        carrier.amIndex = 0.8;
        channel.name = (several ? channelName : signal.name) + " modulé";
        channel.unit = "V";
        channel.components = {record, carrier};
    }
    if (out.alone)
    {
        next.channels = {channel};
        at = 0;
        if (next.name == GeneratorRecipe{}.name) next.name = wave ? signal.name + " (générateur)" : signal.name + " modulé";
    }
    else if (at < next.channels.size())
        next.channels[at] = channel;
    else
        next.channels.push_back(channel);
    out.channel = at;

    // What was done.
    const std::string where = "Voie " + std::to_string(at + 1) + " « " + channel.name + " »";
    const std::string source = "« " + signal.name + " »" + (several ? " (voie " + channelName + ")" : std::string());
    const std::string timing = out.timed ? " (" + quantity(next.sampleRateHz, "Hz") + ", " + quantity(next.durationSeconds, "s") + ")" : std::string();
    if (wave)
        out.text = where + " : le signal " + source + (out.timed ? ", à sa fréquence d'échantillonnage et pour sa durée" + timing : std::string()) +
                   ". Ajoutez-lui d'autres formes d'onde (un bruit…), ou lisez-la dans une opération (v" + std::to_string(at + 1) + ").";
    else
        out.text = where + " : " + source + ", ramené à 1 V crête, est le message d'une porteuse de " + quantity(carrier.frequencyHz, "Hz") +
                   " modulée " + modulationPhrase(carrier) +
                   (out.timed ? ", à la fréquence d'échantillonnage et pour la durée de l'enregistrement" + timing : std::string()) +
                   ". Sa carte « Porteuse modulée » règle la modulation (AM, FM, PM, DBL, BLU), la porteuse et l'indice ; « Ajouter » met le "
                   "signal dans la session.";
    if (!wave && modulation == CarrierModulation::Am)
    {
        // A voice, music: its peaks far above its level, the envelope moves little but at them. Said, with how to see it.
        const std::span<const double> samples = signal.channel(sourceChannel);
        double peak = 0.0, energy = 0.0;
        for (const double v : samples)
        {
            if (!std::isfinite(v)) continue;
            peak = std::max(peak, std::abs(v));
            energy += v * v;
        }
        const double rms = samples.empty() ? 0.0 : std::sqrt(energy / static_cast<double>(samples.size()));
        const double crest = rms > 0.0 ? peak / rms : 0.0;
        if (crest > 3.0)
            out.text += " Son facteur de crête (crête ÷ valeur efficace) est de " + plot::formatCompact(crest, "", 2) + " : l'enveloppe ne va de " +
                        quantity(carrier.amplitude * std::abs(1.0 - carrier.amIndex), "V") + " à " +
                        quantity(carrier.amplitude * (1.0 + carrier.amIndex), "V") + " qu'à ses crêtes, et ne s'écarte de " +
                        quantity(carrier.amplitude, "V") + " que de " + std::to_string(std::lround(100.0 * carrier.amIndex / crest)) +
                        " % en valeur efficace. Pour mieux voir la modulation : un passage fort (zoom), les bandes latérales autour de " +
                        quantity(carrier.frequencyHz, "Hz") + " dans le spectre, ou un indice plus grand (jusqu'à 1).";
    }
    // The page kept its timing, not that of the record: said, with what sets it (or why it cannot).
    const double recordDuration = signal.durationSeconds();
    const bool otherDuration = std::isfinite(recordDuration) && recordDuration > 0.0 &&
                               std::abs(recordDuration - next.durationSeconds) > 1e-9 * std::max(recordDuration, next.durationSeconds);
    const bool otherRate = std::isfinite(signal.sampleRateHz) && signal.sampleRateHz > 0.0 && signal.sampleRateHz != next.sampleRateHz;
    if (!out.timed && (otherDuration || otherRate))
    {
        out.text += " Le signal " + (otherDuration ? "dure " + quantity(recordDuration, "s") + ", la page " + quantity(next.durationSeconds, "s")
                                                   : "est à " + quantity(signal.sampleRateHz, "Hz") + ", la page à " + quantity(next.sampleRateHz, "Hz"));
        if (untimed.empty())
            out.text += " : « Sa fréquence et sa durée », dans la carte du signal enregistré, règle la page sur lui.";
        else
        {
            if (untimed.front() >= 'A' && untimed.front() <= 'Z') untimed.front() = static_cast<char>(untimed.front() - 'A' + 'a');
            out.text += " ; la page ne peut pas prendre sa fréquence et sa durée : " + untimed;
        }
    }
    recipe = std::move(next);
    return out;
}

namespace
{
// How a carrier is modulated by a channel read as it is (not scaled to a peak of 1), said after "modulée".
std::string channelModulationPhrase(const GeneratorComponent &carrier, const std::string &variable)
{
    switch (carrier.modulation)
    {
        case CarrierModulation::Am:
            return "en amplitude (indice " + quantity(carrier.amIndex, "") + " : l'enveloppe vaut " + quantity(carrier.amplitude, "V") +
                   " × (1 + " + quantity(carrier.amIndex, "") + " × " + variable + "), sans surmodulation tant que la crête de " + variable +
                   " reste sous " + quantity(1.0 / std::abs(carrier.amIndex), "") + ")";
        case CarrierModulation::Fm: return "en fréquence (Δf = " + quantity(carrier.deviationHz, "Hz") + " pour " + variable + " = 1)";
        case CarrierModulation::Pm: return "en phase (Δφ = " + quantity(carrier.phaseDeviationDegrees, "") + "° pour " + variable + " = 1)";
        default: return modulationPhrase(carrier);
    }
}
} // namespace

SignalUse modulateChannelInRecipe(GeneratorRecipe &recipe, std::size_t channel, std::string_view use)
{
    SignalUse out;
    if (channel >= recipe.channels.size())
    {
        out.error = "Cette voie n'existe plus.";
        return out;
    }
    const GeneratorChannel &source = recipe.channels[channel];
    if (source.components.empty())
    {
        out.error = "La voie " + std::to_string(channel + 1) + " « " + source.name + " » est vide : rien pour moduler une porteuse.";
        return out;
    }
    CarrierModulation modulation = CarrierModulation::Am;
    bool known = use == "am";
    for (const CarrierModulation m : {CarrierModulation::Fm, CarrierModulation::Pm, CarrierModulation::Dsb, CarrierModulation::Usb, CarrierModulation::Lsb})
    {
        if (use != carrierModulationKey(m)) continue;
        modulation = m;
        known = true;
    }
    if (!known)
    {
        out.error = "Modulation inconnue : « " + std::string(use) + " ».";
        return out;
    }
    // Where: the first blank channel nothing reads (not the message itself), or after the others.
    std::size_t at = recipe.channels.size();
    for (std::size_t k = 0; k < recipe.channels.size(); ++k)
    {
        if (k == channel || !isBlankChannel(recipe.channels[k]) || !channelReaders(recipe, k).empty()) continue;
        at = k;
        break;
    }
    if (at == recipe.channels.size() && recipe.channels.size() >= kMaximumGeneratorChannels)
    {
        out.error = "Au plus " + std::to_string(kMaximumGeneratorChannels) + " voies : retirez-en une du Générateur.";
        return out;
    }
    const std::string variable = "v" + std::to_string(channel + 1);
    GeneratorComponent carrier = defaultGeneratorComponent(Kind::Modulation, recipe.sampleRateHz, recipe.durationSeconds);
    carrier.message = variable;
    carrier.variables.clear();
    carrier.modulation = modulation;
    carrier.amIndex = 0.8;
    GeneratorChannel made;
    made.name = source.name + (source.name.rfind("Voie", 0) == 0 ? " modulée" : " modulé");
    made.unit = "V";
    made.components = {carrier};
    GeneratorRecipe next = recipe;
    if (at < next.channels.size())
        next.channels[at] = made;
    else
        next.channels.push_back(made);
    out.channel = at;
    out.text = "Voie " + std::to_string(at + 1) + " « " + made.name + " » : la voie " + std::to_string(channel + 1) + " « " + source.name +
               " », lue telle quelle (" + variable + "), est le message d'une porteuse de " + quantity(carrier.frequencyHz, "Hz") + " modulée " +
               channelModulationPhrase(carrier, variable) + ". Sa carte « Porteuse modulée » règle la modulation (AM, FM, PM, DBL, BLU), la "
               "porteuse et l'indice ; décochez « Dans le signal » de la voie " + std::to_string(channel + 1) +
               " pour ne garder que la porteuse modulée.";
    recipe = std::move(next);
    return out;
}

bool useSignalAsMessage(GeneratorRecipe &recipe, std::size_t channel, std::size_t component, const model::Signal &signal,
                        std::size_t sourceChannel, std::string *error)
{
    const auto fail = [error](std::string text) {
        if (error) *error = std::move(text);
        return false;
    };
    if (channel >= recipe.channels.size() || component >= recipe.channels[channel].components.size())
        return fail("Cette forme d'onde n'existe plus.");
    if (recipe.channels[channel].components[component].waveform != Kind::Modulation) return fail("Seule une porteuse modulée a un message.");
    if (signal.channels.empty() || sourceChannel >= signal.channels.size()) return fail("Ce signal n'a pas cette voie.");
    GeneratorChannel next = recipe.channels[channel];
    std::size_t carrier = component;
    if (component > 0 && next.components[component - 1].waveform == Kind::Recorded)
    {
        // A record already its message: the new one in its place (its start, delay and reading kept).
        GeneratorComponent &above = next.components[component - 1];
        setRecordedSource(above, signal, sourceChannel);
        above.sourceNormalize = true;
        above.amplitude = 1.0;
        above.enabled = true;
    }
    else
    {
        if (next.components.size() >= kMaximumGeneratorComponents)
            return fail("Au plus " + std::to_string(kMaximumGeneratorComponents) + " formes d'onde par voie : retirez-en une pour placer le message.");
        GeneratorComponent record = defaultGeneratorComponent(Kind::Recorded, recipe.sampleRateHz, recipe.durationSeconds);
        setRecordedSource(record, signal, sourceChannel);
        record.sourceNormalize = true;
        record.amplitude = 1.0;
        next.components.insert(next.components.begin() + static_cast<std::ptrdiff_t>(component), record);
        carrier = component + 1;
    }
    GeneratorComponent &modulated = next.components[carrier];
    modulated.message = "x";
    // The variables of the message it had, when they were only its fm: x needs none.
    const Variables v = parseVariables(modulated.variables, {}, {});
    if (v.error.empty() && v.names.size() == 1 && v.names.front() == "fm") modulated.variables.clear();
    recipe.channels[channel] = std::move(next);
    if (error) error->clear();
    return true;
}

bool adoptRecordedTiming(GeneratorRecipe &recipe, std::size_t channel, std::size_t component, std::span<const model::SignalPtr> sources)
{
    if (channel >= recipe.channels.size() || component >= recipe.channels[channel].components.size()) return false;
    const GeneratorComponent &c = recipe.channels[channel].components[component];
    if (c.waveform != Kind::Recorded) return false;
    const RecordedSource source = findRecordedSource(c, sources);
    if (!source.ok()) return false;
    const double rate = source.signal->sampleRateHz;
    const double whole = source.signal->durationSeconds();
    const double left = c.sourceLoop ? whole : whole - std::max(0.0, c.sourceStartSeconds) + std::max(0.0, c.eventSeconds);
    if (!(std::isfinite(rate) && rate > 0.0) || !(left > 0.0)) return false;
    recipe.sampleRateHz = rate;
    recipe.durationSeconds = left;
    return true;
}

FrameTiming frameTiming(const GeneratorComponent &c, double sampleRateHz)
{
    FrameTiming t;
    t.frame = buildFrame(c.frame);
    if (t.frame.ok() && !(std::isfinite(c.frequencyHz) && c.frequencyHz > 0.0)) t.frame.error = "Le débit doit être positif.";
    if (!t.frame.ok()) return t;
    t.line = frameLineIndex(c.frame.protocol, c.frameLine);
    t.bitSeconds = 1.0 / c.frequencyHz;
    t.durationSeconds = t.frame.bitCount() * t.bitSeconds;
    t.endSeconds = c.eventSeconds + t.durationSeconds;
    t.samplesPerBit = sampleRateHz / c.frequencyHz;
    t.samplesPerTick = t.samplesPerBit / static_cast<double>(t.frame.ticksPerBit);
    return t;
}

std::vector<FrameMember> frameMembers(const GeneratorRecipe &recipe, std::size_t channel, std::size_t component)
{
    std::vector<FrameMember> out;
    if (channel >= recipe.channels.size() || component >= recipe.channels[channel].components.size()) return out;
    const GeneratorComponent &own = recipe.channels[channel].components[component];
    if (own.waveform != Kind::Frame) return out;
    for (std::size_t c = 0; c < recipe.channels.size(); ++c)
    {
        for (std::size_t k = 0; k < recipe.channels[c].components.size(); ++k)
        {
            const GeneratorComponent &other = recipe.channels[c].components[k];
            const bool self = c == channel && k == component;
            if (!self && (own.frameGroup == 0 || other.waveform != Kind::Frame || other.frameGroup != own.frameGroup)) continue;
            out.push_back({c, k, frameLineIndex(other.frame.protocol, other.frameLine)});
        }
    }
    return out;
}

void spreadFrame(GeneratorRecipe &recipe, std::size_t channel, std::size_t component, FrameProtocol before)
{
    const std::vector<FrameMember> members = frameMembers(recipe, channel, component);
    if (members.size() < 2) return;
    const GeneratorComponent own = recipe.channels[channel].components[component];
    for (const FrameMember &m : members)
    {
        if (m.channel == channel && m.component == component) continue;
        GeneratorComponent &other = recipe.channels[m.channel].components[m.component];
        if (own.frame.protocol != before) other.frameLine = std::string(frameLineFor(before, other.frameLine, own.frame.protocol));
        other.frame = own.frame;
        other.frequencyHz = own.frequencyHz;
        other.eventSeconds = own.eventSeconds;
        other.frameRepeat = own.frameRepeat;
        other.frameGapBits = own.frameGapBits;
        other.amplitude = own.amplitude;
        other.frameLow = own.frameLow;
    }
}

bool addFrameLines(GeneratorRecipe &recipe, std::size_t channel, std::size_t component, std::size_t *added, std::string *error)
{
    if (added) *added = 0;
    const std::vector<FrameMember> members = frameMembers(recipe, channel, component);
    if (members.empty()) return false;
    const FrameProtocol protocol = recipe.channels[channel].components[component].frame.protocol;
    const std::span<const FrameLineInfo> lines = frameLines(protocol);
    std::vector<bool> present(lines.size(), false);
    for (const FrameMember &m : members) present[m.line] = true;
    const auto missing = static_cast<std::size_t>(std::count(present.begin(), present.end(), false));
    if (missing == 0) return true;
    if (recipe.channels.size() + missing > kMaximumGeneratorChannels)
    {
        if (error)
            *error = "Il faudrait " + countText(missing) + " de plus pour les autres lignes : au plus " +
                     std::to_string(kMaximumGeneratorChannels) + " voies.";
        return false;
    }
    // A group of its own, if it has none yet.
    if (recipe.channels[channel].components[component].frameGroup == 0)
    {
        std::uint64_t last = 0;
        for (const GeneratorChannel &c : recipe.channels)
            for (const GeneratorComponent &k : c.components) last = std::max(last, k.frameGroup);
        recipe.channels[channel].components[component].frameGroup = last + 1;
    }
    GeneratorComponent &kept = recipe.channels[channel].components[component];
    kept.frameLine = std::string(lines[frameLineIndex(protocol, kept.frameLine)].key);
    const GeneratorComponent own = kept;
    const std::string unit = recipe.channels[channel].unit;
    // Its channel takes the name of its line, if it still has the name it was made with.
    if (recipe.channels[channel].name == "Voie " + std::to_string(channel + 1))
        recipe.channels[channel].name = std::string(lines[frameLineIndex(protocol, own.frameLine)].name);
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        if (present[i]) continue;
        GeneratorComponent line = own;
        line.enabled = true;
        line.frameLine = std::string(lines[i].key);
        recipe.channels.push_back(channelOf(std::string(lines[i].name), unit, {std::move(line)}));
    }
    if (added) *added = missing;
    return true;
}

bool adoptFrameTiming(GeneratorRecipe &recipe, std::size_t channel, std::size_t component, bool realRate)
{
    if (channel >= recipe.channels.size() || component >= recipe.channels[channel].components.size()) return false;
    GeneratorComponent &c = recipe.channels[channel].components[component];
    if (c.waveform != Kind::Frame) return false;
    GeneratorComponent framed = c;
    if (realRate && fixedFrameRate(framed.frame) > 0.0) framed.frequencyHz = fixedFrameRate(framed.frame);
    const FrameTiming t = frameTiming(framed, recipe.sampleRateHz);
    if (!t.ok()) return false;
    // A whole number of samples a tick: five at least, ten a bit at least. The rate stays if it gives that.
    const auto perBit = static_cast<double>(t.frame.ticksPerBit);
    const double least = std::max(5.0, std::ceil(10.0 / perBit));
    const double whole = std::round(t.samplesPerTick);
    const bool suits = whole >= least && std::abs(t.samplesPerTick - whole) <= 1e-9 * whole;
    const double rate = suits ? recipe.sampleRateHz : framed.frequencyHz * perBit * least;
    if (!(std::isfinite(rate) && rate >= 1e-3 && rate <= 1e11)) return false;
    // Two bits of rest, the frame (three of them when it repeats), two bits of rest.
    const double rest = 2.0 * t.bitSeconds;
    const double gap = std::max(0.0, framed.frameGapBits) * t.bitSeconds;
    const double shown = framed.frameRepeat ? 3.0 * t.durationSeconds + 2.0 * gap : t.durationSeconds;
    c.frequencyHz = framed.frequencyHz;
    c.eventSeconds = rest;
    recipe.sampleRateHz = rate;
    recipe.durationSeconds = rest + shown + rest;
    spreadFrame(recipe, channel, component, c.frame.protocol);
    return true;
}

GeneratorResult generateSignal(const GeneratorRecipe &recipe, const model::Progress &progress, std::span<const model::SignalPtr> sources)
{
    GeneratorResult out;
    out.error = checkGeneratorRecipe(recipe);
    if (!out.error.empty()) return out;

    const double fs = recipe.sampleRateHz;
    const std::size_t n = generatorSampleCount(recipe);

    // One step per enabled component, one for a channel without any.
    std::uint64_t total = 0, done = 0;
    for (const GeneratorChannel &channel : recipe.channels)
    {
        const auto enabled = std::count_if(channel.components.begin(), channel.components.end(),
                                           [](const GeneratorComponent &k) { return k.enabled; });
        total += static_cast<std::uint64_t>(std::max<std::ptrdiff_t>(1, enabled));
    }
    const auto stop = [&out] {
        out.cancelled = true;
        out.error = "Génération annulée.";
    };
    const auto labelOf = [&recipe](std::size_t c, std::size_t k) {
        return channelName(recipe.channels[c], c) + ", composante " + std::to_string(k + 1) + " (" +
               std::string(generatorWaveformName(recipe.channels[c].components[k].waveform)) + ")";
    };

    // Operations and modulated carriers: what they read exists and is not their channel; each channel is
    // made after those it reads.
    for (std::size_t c = 0; c < recipe.channels.size(); ++c)
    {
        for (std::size_t k = 0; k < recipe.channels[c].components.size(); ++k)
        {
            if (!recipe.channels[c].components[k].enabled) continue;
            const OperationCheck check = checkOperation(recipe, c, k);
            if (check.error.empty()) continue;
            out.error = labelOf(c, k) + " — " + check.error;
            return out;
        }
    }
    std::vector<std::size_t> order;
    if (!channelOrder(recipe, order, out.error)) return out;

    std::vector<std::vector<double>> made(recipe.channels.size());
    for (const std::size_t c : order)
    {
        const GeneratorChannel &channel = recipe.channels[c];
        std::vector<double> x;
        bool empty = true;
        for (std::size_t k = 0; k < channel.components.size(); ++k)
        {
            const GeneratorComponent &component = channel.components[k];
            if (!component.enabled) continue;
            if (progress.stopped())
            {
                stop();
                return out;
            }
            const std::string label = labelOf(c, k);
            const std::uint64_t seed = recipe.seed + 1000 * c + k;
            const std::span<const double> above = empty ? std::span<const double>() : std::span<const double>(x);
            RecurrenceTrace trace;
            ComponentSamples g;
            if (component.waveform == Kind::Recurrence)
                g = recurrenceSamples(component, fs, n, above, seed, progress, &trace);
            else if (component.waveform == Kind::Formula)
                g = operationSamples(component, fs, n, above, made, progress);
            else if (component.waveform == Kind::Modulation)
                g = modulationSamples(component, fs, n, above, made, progress);
            else if (component.waveform == Kind::Recorded)
                g = recordedSamples(component, fs, n, sources, progress);
            else if (component.waveform == Kind::Frame)
                g = frameSamples(component, fs, n);
            else if (component.waveform == Kind::FourierSeries)
                g = seriesSamples(component, fs, n);
            else if (component.waveform == Kind::Drawn)
                g = motifSamples(component, fs, n);
            else
                g = waveformSamples(component, fs, n, seed);
            if (g.cancelled)
            {
                stop();
                return out;
            }
            if (!g.error.empty())
            {
                out.error = label + " — " + g.error;
                return out;
            }
            for (const std::string &w : g.warnings) out.warnings.push_back(label + " — " + w);
            if (component.waveform == Kind::Recurrence)
            {
                trace.channel = c;
                trace.component = k;
                out.traces.push_back(std::move(trace));
            }
            // A recurrence driven by the components above replaces them (it filters them); so does an operation
            // or a modulated carrier that reads them (x).
            const bool replaces =
                (component.waveform == Kind::Recurrence && component.recurrenceInput == RecurrenceInput::Above) || g.readsAbove;
            if (empty || replaces)
            {
                x = std::move(g.samples);
                empty = false;
            }
            else
            {
                for (std::size_t i = 0; i < n; ++i) x[i] += g.samples[i];
            }
            progress.report(++done, total);
        }
        if (empty)
        {
            x.assign(n, channel.offset);
            progress.report(++done, total);
        }
        else if (channel.offset != 0.0)
        {
            for (double &v : x) v += channel.offset;
        }
        made[c] = std::move(x);
    }
    // The channels of the signal: those not left out.
    auto data = std::make_shared<model::SampleData>();
    for (std::size_t c = 0; c < recipe.channels.size(); ++c)
        if (recipe.channels[c].output) data->channels.push_back(std::move(made[c]));
    if (progress.stopped())
    {
        stop();
        return out;
    }

    model::Signal &s = out.signal;
    s.name = recipe.name.empty() ? std::string("Signal généré") : recipe.name;
    s.origin = model::Origin::Generated;
    s.representation = model::Representation::Real;
    s.sampleRateHz = fs;
    s.startSeconds = 0.0;
    for (std::size_t c = 0; c < recipe.channels.size(); ++c)
        if (recipe.channels[c].output) s.channels.push_back({channelName(recipe.channels[c], c), recipe.channels[c].unit});
    s.data = std::move(data);
    s.provenance = model::Json::object();
    s.provenance["generator"] = toJson(recipe);
    s.notes = notesOf(recipe);
    return out;
}

const std::vector<GeneratorExample> &generatorExamples()
{
    static const std::vector<GeneratorExample> examples = [] {
        std::vector<GeneratorExample> list;
        list.push_back({"Sinus 1 kHz", "Une voie : 1 V crête à 1 kHz, échantillonnée à 48 kHz pendant 1 s.",
                        recipeOf("Sinus 1 kHz", 48000.0, 1.0, {channelOf("Voie 1", "V", {component(Kind::Sine, 1.0, 1000.0)})})});

        GeneratorComponent noise = defaultGeneratorComponent(Kind::Noise);
        noise.amplitude = 0.05;
        list.push_back({"Sinus et bruit",
                        "Un sinus de 1 kHz dans un bruit gaussien de 50 mV : le plancher du spectre, le SNR, le SINAD et l'ENOB des mesures.",
                        recipeOf("Sinus et bruit", 48000.0, 1.0,
                                 {channelOf("Voie 1", "V", {component(Kind::Sine, 1.0, 1000.0), noise})})});

        list.push_back({"Multiton", "Trois sinus de 1 kHz, 2,5 kHz et 7 kHz à 1 V, 0,5 V et 0,25 V : trois raies.",
                        recipeOf("Multiton", 48000.0, 1.0,
                                 {channelOf("Voie 1", "V",
                                            {component(Kind::Sine, 1.0, 1000.0), component(Kind::Sine, 0.5, 2500.0),
                                             component(Kind::Sine, 0.25, 7000.0)})})});

        GeneratorComponent fourier = component(Kind::Square, 1.0, 1000.0);
        fourier.bandLimit = dsp::BandLimit::Fourier;
        list.push_back({"Carré sans repliement",
                        "Un carré de 1 kHz par sa série de Fourier jusqu'à fs/2 : harmoniques impairs en 1/n, ondulations de Gibbs sur les fronts.",
                        recipeOf("Carré 1 kHz (série de Fourier)", 48000.0, 0.1, {channelOf("Voie 1", "V", {fourier})})});

        list.push_back({"Carré échantillonné",
                        "Un carré de 1,3 kHz échantillonné tel quel : ses harmoniques au-delà de fs/2 se replient entre les raies, comme sur un CAN sans filtre.",
                        recipeOf("Carré 1,3 kHz échantillonné", 48000.0, 0.1,
                                 {channelOf("Voie 1", "V", {component(Kind::Square, 1.0, 1300.0)})})});

        GeneratorComponent sweep = defaultGeneratorComponent(Kind::Sweep, 48000.0, 2.0);
        sweep.amplitude = 0.5;
        list.push_back({"Balayage 20 Hz → 20 kHz",
                        "Un balayage logarithmique de 2 s : à regarder en spectrogramme dans le module Signaux.",
                        recipeOf("Balayage 20 Hz – 20 kHz", 48000.0, 2.0, {channelOf("Voie 1", "V", {sweep})})});

        GeneratorComponent step = defaultGeneratorComponent(Kind::Step, 10000.0, 1.0);
        step.eventSeconds = 0.2;
        GeneratorComponent small = defaultGeneratorComponent(Kind::Noise);
        small.amplitude = 0.02;
        list.push_back({"Échelon et bruit",
                        "Un échelon de 1 V à 0,2 s dans un léger bruit : réponse indicielle, intégrale et dérivée dans le module Signaux.",
                        recipeOf("Échelon", 10000.0, 1.0, {channelOf("Voie 1", "V", {step, small})})});

        GeneratorComponent dirac = defaultGeneratorComponent(Kind::Impulse, 48000.0, 0.1);
        list.push_back({"Dirac", "Une seule valeur non nulle : un spectre plat, toutes les fréquences au même niveau.",
                        recipeOf("Dirac", 48000.0, 0.1, {channelOf("Voie 1", "V", {dirac})})});

        GeneratorComponent current = component(Kind::Sine, 14.14, 50.0);
        current.phaseDegrees = -30.0;
        list.push_back({"Réseau 50 Hz : tension et courant",
                        "Deux voies : 230 V efficaces, et un courant de 10 A efficaces en retard de 30° avec un harmonique 3 (THD).",
                        recipeOf("Réseau 50 Hz", 10000.0, 0.2,
                                 {channelOf("Tension", "V", {component(Kind::Sine, 325.27, 50.0)}),
                                  channelOf("Courant", "A", {current, component(Kind::Sine, 2.0, 150.0)})})});

        GeneratorComponent series = defaultGeneratorComponent(Kind::FourierSeries);
        series.frequencyHz = 100.0;
        list.push_back({"Série de Fourier d'un carré",
                        "Les 15 premiers harmoniques d'un carré, bₙ = 4E/(πn) pour n impair : les ondulations de Gibbs près des fronts. "
                        "Ses formules sont dans le panneau Composition.",
                        recipeOf("Série de Fourier d'un carré", 48000.0, 0.05, {channelOf("Voie 1", "V", {series})})});

        GeneratorComponent rectified = defaultGeneratorComponent(Kind::FourierSeries);
        applySeriesModel(rectified, "fullwave", 325.27);
        rectified.frequencyHz = 100.0;
        rectified.harmonics = 40;
        list.push_back({"Redressement double alternance",
                        "Le secteur (230 V efficaces) redressé par un pont de diodes, par sa série de Fourier : valeur moyenne 2E/π, "
                        "harmoniques de 100 Hz en 1/(4n² − 1).",
                        recipeOf("Redressement double alternance", 48000.0, 0.1, {channelOf("Tension", "V", {rectified})})});

        GeneratorComponent heart = defaultGeneratorComponent(Kind::Drawn, 1000.0, 5.0);
        applyMotifModel(heart, "heartbeat");
        heart.frequencyHz = 1.2;
        GeneratorComponent mains = component(Kind::Sine, 0.1, 50.0);
        GeneratorComponent noisy = defaultGeneratorComponent(Kind::Noise);
        noisy.amplitude = 0.01;
        list.push_back({"Cardiogramme et secteur",
                        "Un battement cardiaque dessiné (72 par minute, R = 1 mV), le 50 Hz du secteur et un léger bruit : "
                        "son motif se retouche à la souris dans le panneau Composition.",
                        recipeOf("Cardiogramme", 1000.0, 5.0, {channelOf("ECG", "mV", {heart, mains, noisy})})});

        GeneratorComponent filtered = defaultGeneratorComponent(Kind::Recurrence);
        applyRecurrenceModel(filtered, "lowpass");
        filtered.variables = "tau = 0,0005 ; a = exp(-Te/tau)";
        filtered.recurrenceInput = RecurrenceInput::Above;
        list.push_back({"Carré filtré par une récurrence",
                        "Un carré de 200 Hz passé dans un passe-bas du premier ordre, y[n] = a·y[n−1] + (1 − a)·x[n] "
                        "(τ = 0,5 ms) : chaque front devient la charge ou la décharge d'un condensateur.",
                        recipeOf("Carré filtré (RC)", 48000.0, 0.02,
                                 {channelOf("Voie 1", "V", {component(Kind::Square, 1.0, 200.0), filtered})})});

        // Operations between channels, modulated carriers.
        GeneratorComponent lagging = component(Kind::Sine, 14.142, 50.0);
        lagging.phaseDegrees = -30.0;
        GeneratorComponent power = defaultGeneratorComponent(Kind::Formula);
        power.expression = "v1*v2";
        list.push_back({"Puissance instantanée : v1 × v2",
                        "La tension (230 V efficaces) fois le courant (10 A efficaces, en retard de 30°) : une puissance qui "
                        "oscille à 100 Hz autour de sa moyenne, P = U·I·cos φ ≈ 1 992 W (Mesures ▸ Moyenne de la voie 3).",
                        recipeOf("Puissance instantanée", 10000.0, 0.1,
                                 {channelOf("Tension", "V", {component(Kind::Sine, 325.27, 50.0)}),
                                  channelOf("Courant", "A", {lagging}), channelOf("Puissance", "W", {power})})});

        GeneratorComponent am = defaultGeneratorComponent(Kind::Modulation, 48000.0, 0.02);
        am.frequencyHz = 10000.0;
        am.amIndex = 0.5;
        am.message = "v1";
        am.variables.clear();
        list.push_back({"Modulation d'amplitude (AM)",
                        "Un message de 500 Hz (voie 1) module en amplitude une porteuse de 10 kHz (m = 0,5) : l'enveloppe "
                        "suit le message, le spectre montre la porteuse et deux raies latérales à ±500 Hz, 12 dB plus bas.",
                        recipeOf("Modulation d'amplitude", 48000.0, 0.02,
                                 {channelOf("Message", "V", {component(Kind::Sine, 1.0, 500.0)}),
                                  channelOf("AM", "V", {am})})});

        GeneratorComponent fm = am;
        fm.modulation = CarrierModulation::Fm;
        fm.deviationHz = 2000.0;
        list.push_back({"Modulation de fréquence (FM)",
                        "Le même message module en fréquence la porteuse de 10 kHz (Δf = 2 kHz, indice β = 4) : des raies "
                        "tous les 500 Hz dont les niveaux suivent les fonctions de Bessel, sur la bande de Carson 2(Δf + fm) = 5 kHz.",
                        recipeOf("Modulation de fréquence", 48000.0, 0.1,
                                 {channelOf("Message", "V", {component(Kind::Sine, 1.0, 500.0)}),
                                  channelOf("FM", "V", {fm})})});

        // Digital frames: each line a channel, framed as « Cadrer la trame » does.
        const auto frameExample = [&list](std::string name, std::string tip, const FrameSpec &spec, double rate, double high) {
            GeneratorComponent line = defaultGeneratorComponent(Kind::Frame);
            line.frame = spec;
            line.frameLine = std::string(frameLines(spec.protocol).front().key);
            line.frequencyHz = rate;
            line.amplitude = high;
            GeneratorRecipe recipe = recipeOf(name, 48000.0, 1.0, {channelOf("Voie 1", "V", {line})});
            addFrameLines(recipe, 0, 0);
            if (recipe.channels.size() == 1) recipe.channels[0].name = std::string(frameLines(spec.protocol).front().name);
            adoptFrameTiming(recipe, 0, 0);
            list.push_back({std::move(name), std::move(tip), std::move(recipe)});
        };
        FrameSpec uart = withFrameProtocol(FrameSpec{}, FrameProtocol::Uart);
        frameExample("Trame UART « Bonjour »",
                     "Le texte « Bonjour » en UART 8N1 à 9 600 bit/s, entre 0 et 3,3 V : chaque octet entre un bit de départ (0) et un "
                     "bit d'arrêt (1), poids faible d'abord. La trame construite se lit dans le panneau Composition.",
                     uart, 9600.0, 3.3);
        FrameSpec i2c = withFrameProtocol(FrameSpec{}, FrameProtocol::I2c);
        i2c.data = "0x01 0xA5";
        frameExample("Bus I2C : écriture de deux octets",
                     "SDA et SCL à 100 kbit/s : départ, adresse 0x48 et bit d'écriture, deux octets (0x01 0xA5) acquittés chacun, arrêt. "
                     "SDA ne change que pendant que SCL est bas, sauf au départ et à l'arrêt.",
                     i2c, 100000.0, 3.3);
        FrameSpec spi = withFrameProtocol(FrameSpec{}, FrameProtocol::Spi);
        spi.data = "0x9F 0x00 0x00 0x00";
        spi.reply = "0x00 0xEF 0x40 0x18";
        frameExample("Bus SPI : lecture d'un identifiant",
                     "MOSI, MISO, SCLK et CS en mode 0 à 1 Mbit/s : la commande 0x9F, puis les trois octets de la réponse (EF 40 18, "
                     "l'identifiant JEDEC d'une mémoire flash), lus sur les fronts montants de l'horloge.",
                     spi, 1e6, 3.3);
        FrameSpec usb = withFrameProtocol(FrameSpec{}, FrameProtocol::Usb);
        usb.usbPacket = UsbPacket::SetupTransaction;
        usb.data = "0x80 0x06 0x00 0x01 0x00 0x00 0x12 0x00";
        frameExample("USB : transaction SETUP",
                     "D+ et D− en bas débit (1,5 Mbit/s) : le jeton SETUP, le paquet DATA0 de la requête GET_DESCRIPTOR (8 octets et "
                     "leur CRC16) et l'acquittement ACK ; codage NRZI, fin de paquet par les deux lignes à 0.",
                     usb, 1.5e6, 3.3);
        FrameSpec manchester;
        manchester.data = "1011 0010";
        manchester.code = LineCode::Manchester;
        frameExample("Code Manchester et son horloge",
                     "L'octet 1011 0010 en code Manchester à 1 kbit/s, avec son horloge : un front au milieu de chaque bit, montant "
                     "pour un 1, descendant pour un 0 ; jamais plus d'un bit sans transition.",
                     manchester, 1000.0, 1.0);

        GeneratorComponent white = defaultGeneratorComponent(Kind::Noise);
        white.amplitude = 1.0;
        list.push_back({"Bruit blanc", "Un bruit gaussien d'écart type 1 V : une densité spectrale plate (spectre en densité).",
                        recipeOf("Bruit blanc", 48000.0, 1.0, {channelOf("Voie 1", "V", {white})})});
        return list;
    }();
    return examples;
}
} // namespace qacq::analysis
