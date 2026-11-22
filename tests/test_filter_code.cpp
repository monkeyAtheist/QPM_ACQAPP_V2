// The module "Filtres", code for a microcontroller: the filter as a board runs it, what rounding does, the C.
#include "qacq_test.h"

#include "qacq/analysis/filter_code.h"
#include "qacq/analysis/filters.h"
#include "qacq/dsp/constants.h"
#include "qacq/dsp/filtering.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace qacq;
using namespace qacq::analysis;

namespace
{
bool contains(const std::string &text, const std::string &part)
{
    return text.find(part) != std::string::npos;
}

bool anyContains(const std::vector<std::string> &lines, const std::string &part)
{
    return std::any_of(lines.begin(), lines.end(), [&part](const std::string &l) { return contains(l, part); });
}

std::vector<double> noise(std::size_t n, double amplitude, std::uint32_t seed = 7)
{
    std::vector<double> x(n);
    for (double &v : x)
    {
        seed = 1664525u * seed + 1013904223u;
        v = amplitude * (static_cast<double>(seed) / 2147483648.0 - 1.0);
    }
    return x;
}

FilterRecipe lowPass(int order, double f, double fs = 48000.0)
{
    FilterRecipe r = defaultFilterRecipe(fs);
    r.order = order;
    r.frequency1Hz = f;
    return r;
}

CodeOptions options(CodeFormat format, CodeScaling scaling = CodeScaling::Sum, std::string name = "filtre")
{
    CodeOptions o;
    o.format = format;
    o.scaling = scaling;
    o.name = std::move(name);
    return o;
}

// Direct form I in integers, written again here: what the C does, from the integers the code stores.
std::vector<std::int64_t> referenceFixed(const EmbeddedFilter &e, const std::vector<std::int64_t> &x)
{
    const std::int64_t top = (std::int64_t{1} << (e.bits - 1)) - 1, bottom = -(std::int64_t{1} << (e.bits - 1));
    const auto clip = [top, bottom](std::int64_t v) { return std::clamp(v, bottom, top); };
    std::vector<std::array<std::int64_t, 4>> state(e.sectionIntegers.size(), std::array<std::int64_t, 4>{0, 0, 0, 0});
    std::vector<std::int64_t> y;
    for (std::int64_t v : x)
    {
        for (std::size_t k = 0; k < e.sectionIntegers.size(); ++k)
        {
            const auto &c = e.sectionIntegers[k];
            auto &s = state[k];
            const std::int64_t acc = c[0] * v + c[1] * s[0] + c[2] * s[1] - c[3] * s[2] - c[4] * s[3];
            const int d = e.sectionShifts[k];
            const std::int64_t out = clip((acc + (std::int64_t{1} << (d - 1))) >> d);
            s = {v, s[0], out, s[2]};
            v = out;
        }
        if (e.gainStage) v = clip((e.gainInteger * v + (std::int64_t{1} << (e.gainShift - 1))) >> e.gainShift);
        y.push_back(v);
    }
    return y;
}
} // namespace

TEST_CASE("code: names and keys", "[code]")
{
    CHECK(validCodeName("filtre"));
    CHECK(validCodeName("_passe_bas_2"));
    CHECK(validCodeName("PB1kHz"));
    CHECK(!validCodeName(""));
    CHECK(!validCodeName("2filtre"));
    CHECK(!validCodeName("passe-bas"));
    CHECK(!validCodeName("filtré"));
    CHECK(!validCodeName("int"));
    CHECK(!validCodeName("class"));
    CHECK(!validCodeName("__x"));
    CHECK(!validCodeName("_X"));
    CHECK(!validCodeName(std::string(32, 'a')));
    CHECK(validCodeName(std::string(31, 'a')));
    for (CodeFormat f : {CodeFormat::Float32, CodeFormat::Float64, CodeFormat::Q15, CodeFormat::Q31})
    {
        CodeFormat back = CodeFormat::Float32;
        CHECK(codeFormatFromKey(codeFormatKey(f), back));
        CHECK(back == f);
        CHECK(!codeFormatText(f).empty());
    }
    CodeScaling scaling = CodeScaling::Peak;
    CHECK(codeScalingFromKey("sum", scaling));
    CHECK(scaling == CodeScaling::Sum);
    CHECK(!codeScalingFromKey("rms", scaling));
    CHECK(CodeOptions{}.scaling == CodeScaling::Sum); // the safe one by default
    const EmbeddedFilter bad = embedFilter(designFilter(lowPass(4, 1000.0)), options(CodeFormat::Float32, CodeScaling::Sum, "1x"));
    CHECK(!bad.ok());
    CHECK(contains(bad.error, "ne peut pas nommer le code"));
}

TEST_CASE("code: floating point, the design to the rounding of float", "[code]")
{
    const FilterDesign lp = designFilter(lowPass(6, 2000.0));
    const std::vector<double> x = noise(4096, 0.5);
    const std::vector<double> reference = runFilter(lp, x); // causal, as designed
    for (CodeFormat f : {CodeFormat::Float32, CodeFormat::Float64})
    {
        const EmbeddedFilter e = embedFilter(lp, options(f));
        REQUIRE(e.ok());
        CHECK(!e.fixed);
        CHECK(e.sections.size() == 3);
        const double tolerance = f == CodeFormat::Float32 ? 1e-5 : 1e-13;
        double response = 0.0;
        for (double freq = 0.0; freq <= 24000.0; freq += 100.0)
            response = std::max(response, std::abs(embeddedResponse(e, freq) - lp.response(freq)));
        CHECK(response < tolerance);
        const std::vector<double> y = runEmbedded(e, x);
        double gap = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i) gap = std::max(gap, std::abs(y[i] - reference[i]));
        CHECK(gap < 10.0 * tolerance);
        CHECK(anyContains(e.notes, "forme directe II transposée"));
        CHECK(e.warnings.empty());
        CHECK(e.snrDb > (f == CodeFormat::Float32 ? 100.0 : 280.0));
    }
    // A FIR, symmetric: folded; the gain at the output multiplied at the end.
    FilterRecipe fir = lowPass(4, 2000.0);
    fir.kind = FilterKind::Fir;
    fir.taps = 101;
    fir.gainDb = 6.0;
    const FilterDesign d = designFilter(fir);
    const EmbeddedFilter e = embedFilter(d, options(CodeFormat::Float64));
    REQUIRE(e.ok());
    CHECK(e.fir);
    CHECK(e.symmetry == 1);
    CHECK(e.outputGain == d.gain);
    const std::vector<double> y = runEmbedded(e, x);
    const std::vector<double> z = runFilter(d, x);
    double gap = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) gap = std::max(gap, std::abs(y[i] - z[i]));
    CHECK(gap < 1e-12);
    CHECK(anyContains(e.notes, "repliés"));
    // On the unit circle (an integrator): in floating point only.
    FilterRecipe integrator = defaultFilterRecipe(1000.0);
    integrator.kind = FilterKind::Typed;
    integrator.numerator = "Te";
    integrator.denominator = "1 ; -1";
    const FilterDesign sum = designFilter(integrator);
    CHECK(embedFilter(sum, options(CodeFormat::Float32)).ok());
    CHECK(contains(embedFilter(sum, options(CodeFormat::Q15)).error, "cercle unité"));
}

TEST_CASE("code: fixed point, bit for bit, and nothing overflows inside", "[code]")
{
    // A low-pass, a high-pass with gain, a narrow band-stop, an elliptic: Q15 and Q31, both scalings.
    std::vector<FilterRecipe> recipes{lowPass(6, 2000.0)};
    FilterRecipe hp = lowPass(4, 500.0);
    hp.response = dsp::FilterResponse::HighPass;
    hp.gainDb = 6.0;
    recipes.push_back(hp);
    FilterRecipe bs = lowPass(2, 45.0, 1000.0);
    bs.response = dsp::FilterResponse::BandStop;
    bs.frequency2Hz = 55.0;
    recipes.push_back(bs);
    FilterRecipe el = lowPass(5, 1000.0);
    el.family = dsp::FilterFamily::Elliptic;
    el.passbandRippleDb = 0.5;
    el.stopbandAttenuationDb = 60.0;
    recipes.push_back(el);
    for (const FilterRecipe &r : recipes)
    {
        const FilterDesign d = designFilter(r);
        for (CodeFormat f : {CodeFormat::Q15, CodeFormat::Q31})
            for (CodeScaling s : {CodeScaling::Sum, CodeScaling::Peak})
            {
                const EmbeddedFilter e = embedFilter(d, options(f, s));
                REQUIRE(e.ok());
                CHECK(e.fixed);
                const int bits = f == CodeFormat::Q15 ? 16 : 32;
                const double unit = std::ldexp(1.0, bits - 1);
                // The accumulator cannot overflow: Σ|c|·2^(bits−1) + the rounding, within its width.
                for (std::size_t k = 0; k < e.sectionIntegers.size(); ++k)
                {
                    long double sum = 0.0L;
                    for (std::int64_t c : e.sectionIntegers[k])
                    {
                        CHECK(std::abs(static_cast<double>(c)) < unit);
                        sum += std::abs(static_cast<long double>(c));
                    }
                    const long double worst = sum * static_cast<long double>(unit) + std::ldexp(1.0L, e.sectionShifts[k] - 1);
                    CHECK(worst <= std::ldexp(1.0L, e.accumulatorBits - 1) - 1.0L);
                    CHECK(e.sectionShifts[k] >= 1);
                }
                // Bit for bit against the reference, on noise as loud as the filter takes (and louder).
                for (const double level : {0.5 / std::max(1.0, e.peakGain), 0.999})
                {
                    const std::vector<double> x = noise(8192, level, 11);
                    std::vector<std::int64_t> xi;
                    for (double v : x) xi.push_back(std::llround(v * unit));
                    std::vector<double> xq;
                    for (std::int64_t v : xi) xq.push_back(static_cast<double>(v) / unit);
                    const std::vector<double> y = runEmbedded(e, xq);
                    const std::vector<std::int64_t> z = referenceFixed(e, xi);
                    std::size_t differ = 0;
                    for (std::size_t i = 0; i < y.size(); ++i) differ += std::llround(y[i] * unit) != z[i] ? 1 : 0;
                    CHECK(differ == 0);
                }
                // Scaled by the sum: the worst input there is, full scale with the signs of the impulse response
                // read backwards, saturates nowhere in the sections (only the gain stage at the output may clip
                // what passes above full scale; left out here).
                if (s == CodeScaling::Sum)
                {
                    EmbeddedFilter inside = e;
                    inside.gainStage = false;
                    constexpr std::size_t kLength = 8192;
                    std::vector<double> h(kLength, 0.0);
                    {
                        std::vector<std::array<double, 4>> st(inside.sections.size(), std::array<double, 4>{0, 0, 0, 0});
                        for (std::size_t i = 0; i < kLength; ++i)
                        {
                            double v = i == 0 ? 1.0 : 0.0;
                            for (std::size_t k = 0; k < inside.sections.size(); ++k)
                            {
                                const auto &c = inside.sections[k];
                                auto &q = st[k];
                                const double out = c[0] * v + c[1] * q[0] + c[2] * q[1] - c[3] * q[2] - c[4] * q[3];
                                q = {v, q[0], out, q[2]};
                                v = out;
                            }
                            h[i] = v;
                        }
                    }
                    std::vector<double> worst(kLength);
                    for (std::size_t i = 0; i < kLength; ++i) worst[i] = h[kLength - 1 - i] < 0.0 ? -1.0 : (unit - 1.0) / unit;
                    std::size_t saturated = 0;
                    const std::vector<double> y = runEmbedded(inside, worst, &saturated);
                    CHECK(saturated == 0);
                    CHECK(std::abs(y.back()) > 0.9); // the bound is reached (0,99 of full scale)
                }
            }
    }
    // Q15: a 32-bit accumulator when it costs no precision, 64 bits otherwise.
    CHECK(embedFilter(designFilter(lowPass(6, 2000.0)), options(CodeFormat::Q15)).accumulatorBits == 32);
    CHECK(embedFilter(designFilter(hp), options(CodeFormat::Q15)).accumulatorBits == 64);
    CHECK(embedFilter(designFilter(hp), options(CodeFormat::Q31)).accumulatorBits == 64);
    // The gain the last section cannot take without overflowing goes to a stage of its own.
    const EmbeddedFilter withGain = embedFilter(designFilter(hp), options(CodeFormat::Q15));
    CHECK(withGain.gainStage);
    CHECK(withGain.peakGain > 1.9);
    CHECK(anyContains(withGain.notes, "une entrée au-delà de 0,501 de la pleine échelle y sature la sortie"));
    CHECK(!embedFilter(designFilter(lowPass(6, 2000.0)), options(CodeFormat::Q15, CodeScaling::Peak)).gainStage);
}

TEST_CASE("code: what rounding does, said", "[code]")
{
    // An elliptic of order 5 at 1 kHz over 48 kHz: its poles near the unit circle want more than Q15.
    FilterRecipe el = lowPass(5, 1000.0);
    el.family = dsp::FilterFamily::Elliptic;
    el.passbandRippleDb = 0.5;
    el.stopbandAttenuationDb = 60.0;
    const FilterDesign d = designFilter(el);
    const EmbeddedFilter q15 = embedFilter(d, options(CodeFormat::Q15));
    REQUIRE(q15.ok());
    CHECK(q15.snrDb < 40.0);
    CHECK(anyContains(q15.warnings, "Prenez Q31 ou du flottant"));
    const EmbeddedFilter q31 = embedFilter(d, options(CodeFormat::Q31));
    REQUIRE(q31.ok());
    CHECK(q31.snrDb > 120.0);
    CHECK(q31.warnings.empty());
    CHECK(q31.passbandDeviationDb < 1e-5);
    CHECK(std::abs(q31.largestPoleRadius - d.largestPoleRadius()) < 1e-7);
    // A notch keeps its zeros on the unit circle once rounded (b2 = b0): it still stops its frequency.
    FilterRecipe notch = lowPass(2, 45.0, 1000.0);
    notch.response = dsp::FilterResponse::BandStop;
    notch.frequency2Hz = 55.0;
    const FilterDesign n = designFilter(notch);
    const EmbeddedFilter nq = embedFilter(n, options(CodeFormat::Q15));
    REQUIRE(nq.ok());
    for (const auto &c : nq.sectionIntegers) CHECK(c[2] == c[0]);
    double deepest = 1.0;
    for (double f = 45.0; f <= 55.0; f += 0.01) deepest = std::min(deepest, std::abs(embeddedResponse(nq, f)));
    CHECK(deepest < 1e-3);
    // A long FIR in Q15: its stop band floor rises, said.
    FilterRecipe fir = lowPass(4, 2000.0);
    fir.kind = FilterKind::Fir;
    fir.taps = 255;
    fir.window = dsp::Window::Kaiser;
    fir.kaiserBeta = 12.0;
    const EmbeddedFilter fq = embedFilter(designFilter(fir), options(CodeFormat::Q15));
    REQUIRE(fq.ok());
    CHECK(fq.accumulatorBits == 32);
    CHECK(anyContains(fq.notes, "Dans la bande coupée, l'arrondi remonte le gain"));
    if (qacq::test::state().verbose)
        std::printf("    elliptic Q15 %.1f dB, Q31 %.1f dB; notch Q15 depth %.2g; FIR Q15 floor %.1f dB\n", q15.snrDb, q31.snrDb,
                    deepest, 20.0 * std::log10(fq.stopbandEmbedded));
    // What cannot be made into code.
    FilterRecipe unstable = defaultFilterRecipe(48000.0);
    unstable.kind = FilterKind::Typed;
    unstable.numerator = "1";
    unstable.denominator = "1 ; -1,05";
    CHECK(contains(embedFilter(designFilter(unstable), options(CodeFormat::Float32)).error, "instable"));
    FilterRecipe derivative = defaultFilterRecipe(48000.0);
    derivative.kind = FilterKind::Typed;
    derivative.numerator = "fs ; -fs";
    CHECK(contains(embedFilter(designFilter(derivative), options(CodeFormat::Q15)).error, "trop grands pour Q15"));
    CHECK(embedFilter(designFilter(derivative), options(CodeFormat::Q31)).ok());
    FilterRecipe huge = lowPass(4, 2000.0);
    huge.kind = FilterKind::Fir;
    huge.taps = 5001;
    CHECK(contains(embedFilter(designFilter(huge), options(CodeFormat::Float32)).error, "ne tient pas"));
    FilterRecipe none = lowPass(4, 30000.0);
    CHECK(!embedFilter(designFilter(none), options(CodeFormat::Float32)).ok());
}

TEST_CASE("code: the C, its names and its self-test", "[code]")
{
    FilterRecipe hp = lowPass(4, 500.0);
    hp.response = dsp::FilterResponse::HighPass;
    hp.gainDb = 6.0;
    hp.application = FilterApplication::ZeroPhase;
    const FilterDesign d = designFilter(hp);
    const EmbeddedFilter e = embedFilter(d, options(CodeFormat::Q15, CodeScaling::Sum, "passe_haut"));
    REQUIRE(e.ok());
    const GeneratedCode code = generateCode(e, d);
    REQUIRE(code.error.empty());
    CHECK(code.headerName == "passe_haut.h");
    CHECK(code.sourceName == "passe_haut.c");
    CHECK(contains(code.header, "#ifndef PASSE_HAUT_H"));
    CHECK(contains(code.header, "#define PASSE_HAUT_SECTIONS 2"));
    CHECK(contains(code.header, "int16_t passe_haut_echantillon(passe_haut_t *f, int16_t x);"));
    CHECK(contains(code.header, "int passe_haut_autotest(void);"));
    CHECK(contains(code.header, "extern \"C\""));
    CHECK(contains(code.header, "à phase nulle"));             // what the application did, and the board does not
    CHECK(contains(code.header, "accumulateur int64_t"));
    CHECK(contains(code.source, "#include \"passe_haut.h\""));
    CHECK(contains(code.source, "static const int16_t passe_haut_coefficients[PASSE_HAUT_SECTIONS][5] = {"));
    CHECK(contains(code.source, "static const int16_t passe_haut_gain = "));
    CHECK(contains(code.source, "static const int16_t passe_haut_test_entree[32] = {"));
    // The integers of the first section, as stored.
    const auto &c = e.sectionIntegers[0];
    CHECK(contains(code.source, "{ " + std::to_string(c[0]) + ", " + std::to_string(c[1]) + ", " + std::to_string(c[2]) + ", " +
                                    std::to_string(c[3]) + ", " + std::to_string(c[4]) + " }"));
    // No line of the comment over 100 columns (counted in characters), none starting with a lone ":".
    std::size_t start = 0;
    while (start < code.header.size())
    {
        const std::size_t end = code.header.find('\n', start);
        const std::string line = code.header.substr(start, end - start);
        std::size_t characters = 0;
        for (unsigned char ch : line) characters += (ch & 0xC0) != 0x80 ? 1 : 0;
        CHECK(characters <= 100);
        CHECK(line.rfind(" *   :", 0) != 0);
        start = end == std::string::npos ? code.header.size() : end + 1;
    }
    // Floating point: float literals, a tolerance; a FIR folded.
    FilterRecipe fir = lowPass(4, 2000.0);
    fir.kind = FilterKind::Fir;
    fir.taps = 31;
    const FilterDesign fd = designFilter(fir);
    const GeneratedCode fc = generateCode(embedFilter(fd, options(CodeFormat::Float32, CodeScaling::Sum, "rif")), fd);
    CHECK(contains(fc.header, "#define RIF_N 31"));
    CHECK(contains(fc.source, "static const float rif_h[16] = {"));
    CHECK(contains(fc.source, "acc += rif_h[k] * (w[k] + w[RIF_N - 1 - k]);"));
    CHECK(contains(fc.source, "acc += rif_h[RIF_N / 2] * w[RIF_N / 2];"));
    CHECK(contains(fc.source, "f;"));
    CHECK(contains(fc.source, "if (!(d <= "));
    // Nothing to write: said.
    CHECK(!generateCode(EmbeddedFilter{}, fd).error.empty());
}
