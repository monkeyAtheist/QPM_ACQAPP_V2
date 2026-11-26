// The module "Filtres": a filter as a recipe, designed, what it does, run over a signal.
#include "qacq_test.h"

#include "qacq/analysis/filters.h"
#include "qacq/dsp/filter_design.h"
#include "qacq/dsp/filtering.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <span>
#include <string>
#include <utility>

using namespace qacq;
using namespace qacq::analysis;

namespace
{
constexpr double kPi = std::numbers::pi;

bool contains(const std::string &text, const std::string &part)
{
    return text.find(part) != std::string::npos;
}

double db(std::complex<double> h)
{
    return 20.0 * std::log10(std::abs(h));
}

model::Signal tones(double fs, std::size_t n, std::vector<std::pair<double, double>> parts, std::size_t channels = 1)
{
    model::Signal s;
    s.id = 7;
    s.name = "Essai";
    s.sampleRateHz = fs;
    s.channels = model::defaultChannels(channels, "V");
    auto data = std::make_shared<model::SampleData>();
    for (std::size_t c = 0; c < channels; ++c)
    {
        std::vector<double> x(n, 0.0);
        for (std::size_t i = 0; i < n; ++i)
            for (const auto &[f, a] : parts) x[i] += a * std::sin(2.0 * kPi * f * static_cast<double>(i) / fs);
        data->channels.push_back(std::move(x));
    }
    s.data = data;
    return s;
}

// Amplitude and phase (radians, of a sine) of the component at f over [from, to).
std::complex<double> component(const std::vector<double> &x, double fs, double f, std::size_t from, std::size_t to)
{
    std::complex<double> sum(0.0, 0.0);
    for (std::size_t i = from; i < to; ++i)
        sum += x[i] * std::polar(1.0, -2.0 * kPi * f * static_cast<double>(i) / fs);
    return 2.0 * sum / static_cast<double>(to - from) * std::complex<double>(0.0, 1.0); // sin → 1
}
} // namespace

TEST_CASE("filters: the blank page, no filter: the output is the input", "[analysis]")
{
    // The rest of the recipe is the usual first filter, which a kind chosen starts from.
    const FilterRecipe blank = blankFilterRecipe(20000.0);
    CHECK(blank.kind == FilterKind::None);
    CHECK(blank.sampleRateHz == 20000.0);
    CHECK(blank.frequency1Hz == defaultFilterRecipe(20000.0).frequency1Hz);
    FilterRecipe chosen = blank;
    chosen.kind = FilterKind::Iir;
    CHECK(toJson(chosen) == toJson(defaultFilterRecipe(20000.0)));
    CHECK(!designedKind(FilterKind::None));
    CHECK(!designedKind(FilterKind::Typed));
    CHECK(designedKind(FilterKind::Iir));
    CHECK(designedKind(FilterKind::Fir));

    // Designed: one coefficient, 1; flat, no delay; said so.
    FilterRecipe r = blank;
    r.fromTemplate = true;                         // means nothing without a filter
    r.application = FilterApplication::ZeroPhase; // nor does this
    const FilterDesign d = designFilter(r);
    REQUIRE(d.ok());
    CHECK(d.fir());
    REQUIRE(d.taps.size() == 1);
    CHECK(d.taps[0] == 1.0);
    CHECK(d.rootsKnown);
    CHECK(d.zeros.empty());
    CHECK(!d.recipe.fromTemplate);
    CHECK(d.recipe.application == FilterApplication::Causal);
    CHECK(std::abs(d.unitResponse(1234.0) - std::complex<double>(1.0, 0.0)) < 1e-15);
    CHECK(filterText(d.recipe) == "Aucun filtre");
    CHECK(filterShortText(d.recipe) == "sans filtre");
    const std::vector<std::string> lines = designSummary(d);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "Aucun filtre : la sortie est l'entrée (H(z) = 1), sans retard ni déphasage.");
    CHECK(contains(lines[1], "Le premier choisi part d'un passe-bas à 1 kHz."));
    FilterRecipe louder = blank;
    louder.gainDb = 6.0;
    CHECK(filterText(louder) == "Aucun filtre, gain +6 dB");
    CHECK(contains(designSummary(designFilter(louder))[0], "multipliée par le gain en sortie (+6 dB)."));
    CHECK(!applicationAllowed(FilterKind::None, FilterApplication::ZeroPhase));
    CHECK(applicationAllowed(FilterKind::None, FilterApplication::Causal));

    // Its key, and back.
    CHECK(filterKindKey(FilterKind::None) == "none");
    CHECK(toJson(blank)["kind"].toString() == "none");
    CHECK(filterRecipeFromJson(toJson(blank)).kind == FilterKind::None);

    // Run over a signal: the same samples.
    const model::Signal s = tones(20000.0, 2000, {{50.0, 1.0}, {3000.0, 0.5}});
    const FilteredSignal out = applyFilter(d, s);
    REQUIRE(out.error.empty());
    CHECK(out.signal.channel(0)[0] == s.channel(0)[0]);
    CHECK(std::equal(s.channel(0).begin(), s.channel(0).end(), out.signal.channel(0).begin()));
    // No template from it.
    FilterRecipe t = defaultFilterRecipe(20000.0);
    CHECK(!templateFromDesign(d, t));
}

TEST_CASE("filters: an IIR designed as sections, its gain, phase, delay, poles and zeros", "[analysis]")
{
    FilterRecipe r = defaultFilterRecipe(48000.0);
    CHECK(r.kind == FilterKind::Iir);
    CHECK(r.frequency1Hz == 1000.0);
    FilterDesign d = designFilter(r);
    REQUIRE(d.ok());
    CHECK(d.sections.size() == 2);
    CHECK(d.poles.size() == 4);
    CHECK(d.zeros.size() == 4);
    CHECK(d.rootsKnown);
    CHECK(d.largestPoleRadius() < 1.0);
    CHECK_NEAR(std::abs(d.response(0.0)), 1.0, 0.0, 1e-12);
    CHECK_NEAR(db(d.response(1000.0)), -3.0103, 0.0, 1e-3);
    CHECK(db(d.response(4000.0)) < -45.0); // two octaves, 24 dB each
    CHECK(d.warnings.empty());
    // Applied forward and backward: the gain squared, no phase.
    r.application = FilterApplication::ZeroPhase;
    d = designFilter(r);
    CHECK_NEAR(db(d.applied(1000.0)), -6.0206, 0.0, 1e-3);
    CHECK(d.applied(1000.0).imag() == 0.0);
    CHECK(d.groupDelay(100.0) > 0.0); // as designed
    // A FIR only is aligned.
    r.application = FilterApplication::Aligned;
    CHECK(designFilter(r).recipe.application == FilterApplication::Causal);
    CHECK(!applicationAllowed(FilterKind::Iir, FilterApplication::Aligned));
    CHECK(applicationAllowed(FilterKind::Fir, FilterApplication::Aligned));

    // What cannot be designed.
    r.frequency1Hz = 30000.0;
    d = designFilter(r);
    CHECK(!d.ok());
    CHECK(contains(d.error, "entre 0 et fs/2"));
    r = defaultFilterRecipe(48000.0);
    r.response = dsp::FilterResponse::BandPass;
    r.frequency1Hz = 2000.0;
    r.frequency2Hz = 1000.0;
    CHECK(contains(designFilter(r).error, "0 < f1 < f2 < fs/2"));

    // A pole very near the circle is said.
    r = defaultFilterRecipe(48000.0);
    r.response = dsp::FilterResponse::HighPass;
    r.order = 2;
    r.frequency1Hz = 0.01;
    d = designFilter(r);
    REQUIRE(d.ok());
    REQUIRE(d.warnings.size() == 1);
    CHECK(contains(d.warnings[0], "du cercle unité"));

    // The summary.
    const std::vector<std::string> lines = designSummary(designFilter(defaultFilterRecipe(48000.0)));
    REQUIRE(lines.size() == 3);
    CHECK(contains(lines[0], "2 sections du second ordre (4 pôles), stable"));
    CHECK(contains(lines[1], "Temps de propagation de groupe à 0 Hz"));
    CHECK(contains(lines[2], "Appliqué causalement"));
}

TEST_CASE("filters: a FIR by the window method, its linear phase and its zeros", "[analysis]")
{
    FilterRecipe r = defaultFilterRecipe(48000.0);
    r.kind = FilterKind::Fir;
    r.taps = 101;
    r.frequency1Hz = 2000.0;
    FilterDesign d = designFilter(r);
    REQUIRE(d.ok());
    CHECK(d.fir());
    CHECK(d.taps.size() == 101);
    CHECK_NEAR(std::abs(d.response(0.0)), 1.0, 0.0, 1e-9);
    CHECK_NEAR(db(d.response(2000.0)), -6.02, 0.0, 0.1); // the window method: half the gain at the cut-off
    CHECK_NEAR(d.groupDelay(500.0), 50.0, 0.0, 1e-6);
    CHECK_NEAR(d.groupDelay(1500.0), 50.0, 0.0, 1e-6);
    // Its 100 zeros, each with its reciprocal (linear phase): H(1/z) = z^(N-1) H(z).
    REQUIRE(d.rootsKnown);
    REQUIRE(d.zeros.size() == 100);
    for (const std::complex<double> &z : d.zeros)
    {
        if (std::abs(std::abs(z) - 1.0) < 1e-3) continue; // on the circle: its own
        const std::complex<double> mirror = 1.0 / std::conj(z);
        const double nearest = std::abs(*std::min_element(d.zeros.begin(), d.zeros.end(), [&](auto a, auto b) {
                                            return std::abs(a - mirror) < std::abs(b - mirror);
                                        }) - mirror);
        CHECK(nearest < 1e-5 * std::max(1.0, std::abs(mirror)));
    }
    // Aligned: no phase left (0 or 180°).
    r.application = FilterApplication::Aligned;
    d = designFilter(r);
    for (double f : {300.0, 1200.0, 5000.0})
        CHECK(std::abs(d.applied(f).imag()) < 1e-9 * std::max(1e-12, std::abs(d.applied(f))) + 1e-12);
    // A high-pass needs an odd number of taps; too many taps; no zeros beyond kMaximumFirRoots.
    r.response = dsp::FilterResponse::HighPass;
    r.taps = 100;
    CHECK(contains(designFilter(r).error, "nombre impair"));
    r.taps = kMaximumFirTaps + 2;
    CHECK(contains(designFilter(r).error, "coefficients n'est pas conçu ici"));
    r.taps = kMaximumFirRoots + 1;
    d = designFilter(r);
    REQUIRE(d.ok());
    CHECK(!d.rootsKnown);
    CHECK(d.zeros.empty());
    // A window with zeros at its ends: as many zeros at the origin.
    r = defaultFilterRecipe(48000.0);
    r.kind = FilterKind::Fir;
    r.window = dsp::Window::Hann;
    r.taps = 21;
    d = designFilter(r);
    REQUIRE(d.ok());
    REQUIRE(d.taps.front() == 0.0);
    CHECK(d.zeros.size() == 19); // the last tap is 0 too: one zero at the origin, and the first one a delay
    CHECK(std::count(d.zeros.begin(), d.zeros.end(), std::complex<double>(0.0, 0.0)) == 1);

    const std::vector<std::string> lines = designSummary(d);
    REQUIRE(!lines.empty());
    CHECK(contains(lines[0], "21 coefficients, phase linéaire : retard de 10 échantillons"));
}

TEST_CASE("filters: impulse and step responses", "[analysis]")
{
    const FilterDesign iir = designFilter(defaultFilterRecipe(48000.0));
    const ImpulseResponse r = impulseResponse(iir);
    CHECK(r.settled);
    CHECK(r.impulse.size() > 100);
    CHECK(r.impulse.size() < 4096);
    CHECK_NEAR(r.step.back(), 1.0, 0.0, 1e-4); // the gain at DC
    // The same samples as the filter run over an impulse.
    std::vector<double> impulse(r.impulse.size(), 0.0);
    impulse[0] = 1.0;
    const std::vector<double> y = runFilter(iir, impulse);
    double gap = 0.0;
    for (std::size_t i = 0; i < y.size(); ++i) gap = std::max(gap, std::abs(y[i] - r.impulse[i]));
    CHECK(gap < 1e-15);

    FilterRecipe f = defaultFilterRecipe(48000.0);
    f.kind = FilterKind::Fir;
    f.taps = 31;
    const FilterDesign fir = designFilter(f);
    const ImpulseResponse h = impulseResponse(fir);
    REQUIRE(h.impulse.size() == 35);
    CHECK(std::equal(fir.taps.begin(), fir.taps.end(), h.impulse.begin()));
    CHECK(h.impulse.back() == 0.0);
    CHECK_NEAR(h.step.back(), 1.0, 0.0, 1e-12);
    CHECK(impulseResponse(designFilter([] {
              FilterRecipe bad;
              bad.frequency1Hz = -1.0;
              return bad;
          }())).impulse.empty());
    CHECK(r.appliedImpulse.empty()); // causal: as designed

    // At zero phase: symmetric about n = 0, the autocorrelation of h; its sum, the gain at DC squared.
    FilterRecipe z = defaultFilterRecipe(48000.0);
    z.application = FilterApplication::ZeroPhase;
    const ImpulseResponse zr = impulseResponse(designFilter(z));
    REQUIRE(!zr.appliedImpulse.empty());
    CHECK(zr.appliedStart < 0);
    const long centre = -zr.appliedStart;
    REQUIRE(centre < static_cast<long>(zr.appliedImpulse.size()));
    double energy = 0.0;
    for (double v : zr.impulse) energy += v * v;
    CHECK_NEAR(zr.appliedImpulse[static_cast<std::size_t>(centre)], energy, 1e-9, 0.0);
    double asymmetry = 0.0;
    for (long k = 1; k < 200; ++k)
        asymmetry = std::max(asymmetry, std::abs(zr.appliedImpulse[static_cast<std::size_t>(centre + k)] -
                                                 zr.appliedImpulse[static_cast<std::size_t>(centre - k)]));
    CHECK(asymmetry < 1e-12);
    CHECK_NEAR(zr.appliedStep.back(), 1.0, 0.0, 1e-4);

    // A FIR aligned: its taps, centred on n = 0, with a few zeros either side.
    f.application = FilterApplication::Aligned;
    const ImpulseResponse a = impulseResponse(designFilter(f));
    REQUIRE(!a.appliedImpulse.empty());
    const long margin = -15 - a.appliedStart;
    CHECK(margin >= 4);
    REQUIRE(a.appliedImpulse.size() >= static_cast<std::size_t>(31 + 2 * margin));
    double tapGap = 0.0;
    for (std::size_t k = 0; k < 31; ++k)
        tapGap = std::max(tapGap, std::abs(a.appliedImpulse[k + static_cast<std::size_t>(margin)] - fir.taps[k]));
    CHECK(tapGap < 1e-15);
    CHECK(a.appliedImpulse.front() == 0.0);
}

TEST_CASE("filters: the phase unwrapped, and as applied", "[analysis]")
{
    const auto wrapped = [](double x) { return std::remainder(x, 2.0 * kPi); };
    // An IIR: the same as arg H (to 2π), continuous, and its slope is the group delay.
    for (const auto family : {dsp::FilterFamily::Butterworth, dsp::FilterFamily::ChebyshevI,
                              dsp::FilterFamily::ChebyshevII, dsp::FilterFamily::Bessel, dsp::FilterFamily::Elliptic})
    {
        for (const auto response : {dsp::FilterResponse::LowPass, dsp::FilterResponse::HighPass,
                                    dsp::FilterResponse::BandPass, dsp::FilterResponse::BandStop})
        {
            FilterRecipe r = defaultFilterRecipe(48000.0);
            r.family = family;
            r.response = response;
            r.order = 5;
            const FilterDesign d = designFilter(r);
            REQUIRE(d.ok());
            CHECK(d.phaseUnwrapped());
            double worstArg = 0.0, worstSlope = 0.0, worstStep = 0.0;
            double previous = d.phase(10.0), before = 10.0;
            // Whether a zero on the circle lies between two frequencies.
            const auto zeroBetween = [&d](double f1, double f2) {
                return std::any_of(d.zeros.begin(), d.zeros.end(), [&](const std::complex<double> &z) {
                    const double fz = std::arg(z) * 48000.0 / (2.0 * kPi);
                    return std::abs(std::abs(z) - 1.0) < 1e-6 && fz >= f1 && fz <= f2;
                });
            };
            for (int i = 1; i <= 2000; ++i)
            {
                const double f = 10.0 + 23990.0 * i / 2000.0;
                const std::complex<double> h = d.response(f);
                const double p = d.phase(f);
                if (std::abs(h) > 1e-6) worstArg = std::max(worstArg, std::abs(wrapped(p - std::arg(h))));
                // But across a zero on the circle, no jump between neighbours 12 Hz apart.
                if (!zeroBetween(before, f)) worstStep = std::max(worstStep, std::abs(p - previous));
                previous = p;
                before = f;
                if (std::abs(h) > 1e-2)
                {
                    // τ = −dφ/dω, ω = 2πf/fs.
                    const double df = 0.01;
                    const double slope = -(d.phase(f + df) - d.phase(f - df)) / (2.0 * kPi * 2.0 * df / 48000.0);
                    const double tau = d.groupDelay(f);
                    worstSlope = std::max(worstSlope, std::abs(slope - tau) / std::max(1.0, std::abs(tau)));
                }
            }
            CHECK(worstArg < 1e-9);
            CHECK(worstStep < 1.0);
            CHECK(worstSlope < 1e-4);
        }
    }

    // A zero on the circle (Chebyshev II): a jump of π across it.
    FilterRecipe c2 = defaultFilterRecipe(48000.0);
    c2.family = dsp::FilterFamily::ChebyshevII;
    const FilterDesign cheby = designFilter(c2);
    double notch = 0.0;
    for (const auto &z : cheby.zeros)
        if (z.imag() > 1e-6 && std::abs(std::abs(z) - 1.0) < 1e-9) notch = std::arg(z) * 48000.0 / (2.0 * kPi);
    REQUIRE(notch > 1000.0);
    CHECK_NEAR(cheby.phase(notch + 1e-3) - cheby.phase(notch - 1e-3), kPi, 0.0, 1e-3);

    // A FIR of linear phase: −ω(N−1)/2 in the passband; aligned, 0 there; its delay, none once aligned.
    FilterRecipe f = defaultFilterRecipe(48000.0);
    f.kind = FilterKind::Fir;
    f.taps = 101;
    f.application = FilterApplication::Causal;
    const FilterDesign fir = designFilter(f);
    CHECK(fir.phaseUnwrapped());
    CHECK_NEAR(fir.phase(500.0), -2.0 * kPi * 500.0 / 48000.0 * 50.0, 0.0, 1e-12);
    CHECK_NEAR(fir.groupDelay(500.0), 50.0, 0.0, 1e-9);
    CHECK(fir.alignment() == 0.0);
    f.application = FilterApplication::Aligned;
    const FilterDesign aligned = designFilter(f);
    CHECK(aligned.alignment() == 50.0);
    CHECK_NEAR(aligned.appliedPhase(500.0), 0.0, 0.0, 1e-12);
    CHECK_NEAR(aligned.appliedGroupDelay(500.0), 0.0, 0.0, 1e-9);
    double armWorst = 0.0;
    for (int i = 1; i < 500; ++i)
    {
        const double fr = 24000.0 * i / 500.0;
        const std::complex<double> h = aligned.response(fr);
        if (std::abs(h) > 1e-5) armWorst = std::max(armWorst, std::abs(wrapped(aligned.phase(fr) - std::arg(h))));
    }
    CHECK(armWorst < 1e-9);
    // Many taps (no zeros computed): still unwrapped, by its symmetry.
    f.taps = 1001;
    const FilterDesign longFir = designFilter(f);
    CHECK(!longFir.rootsKnown);
    CHECK(longFir.phaseUnwrapped());
    CHECK_NEAR(longFir.phase(100.0), -2.0 * kPi * 100.0 / 48000.0 * 500.0, 0.0, 1e-10);
    // Neither symmetric nor with its zeros: only arg H.
    FilterDesign odd = longFir;
    odd.taps[0] += 0.01;
    CHECK(!odd.phaseUnwrapped());
    CHECK_NEAR(odd.phase(100.0), std::arg(odd.response(100.0)), 0.0, 1e-15);
    // ... but with its zeros known (few taps), from them: the same as arg H.
    FilterDesign skew = fir;
    skew.taps = {0.0, 0.5, 1.0, -0.25, 0.125};
    skew.zeros = dsp::polynomialRoots(std::vector<double>{0.125, -0.25, 1.0, 0.5});
    skew.rootsKnown = true;
    CHECK(skew.phaseUnwrapped());
    double skewWorst = 0.0;
    for (int i = 0; i <= 100; ++i)
    {
        const double fr = 24000.0 * i / 100.0;
        skewWorst = std::max(skewWorst, std::abs(wrapped(skew.phase(fr) - std::arg(skew.response(fr)))));
    }
    CHECK(skewWorst < 1e-12);

    // At zero phase: nothing, and no delay.
    FilterRecipe z = defaultFilterRecipe(48000.0);
    z.application = FilterApplication::ZeroPhase;
    const FilterDesign zero = designFilter(z);
    CHECK(zero.appliedPhase(1234.0) == 0.0);
    CHECK(zero.appliedGroupDelay(1234.0) == 0.0);
    CHECK(zero.groupDelay(1234.0) > 0.0);
}

TEST_CASE("filters: run over a signal, causal, forward and backward, aligned", "[analysis]")
{
    const double fs = 48000.0;
    const model::Signal source = tones(fs, 48000, {{100.0, 1.0}, {5000.0, 1.0}}, 2);
    FilterRecipe r = defaultFilterRecipe(fs);
    FilterDesign d = designFilter(r);
    FilteredSignal out = applyFilter(d, source);
    REQUIRE(out.error.empty());
    const model::Signal &s = out.signal;
    REQUIRE(s.channelCount() == 2);
    REQUIRE(s.frameCount() == 48000);
    CHECK(s.name == "Essai · passe-bas 1 kHz");
    CHECK(s.origin == model::Origin::Derived);
    CHECK(s.sampleRateHz == fs);
    CHECK(s.channels[1].unit == "V");
    CHECK(contains(s.notes, "Passe-bas Butterworth d'ordre 4, 1 kHz, appliqué à « Essai » : causal (comme en temps réel)."));
    CHECK(s.provenance["operation"].toString() == "filter");
    CHECK(s.provenance["sourceId"].toNumber() == 7.0);
    CHECK(s.provenance["parameters"]["family"].toString() == "butterworth");
    // The 5 kHz is gone (−55 dB), the 100 Hz passes with the phase of H.
    const std::vector<double> y(s.channel(0).begin(), s.channel(0).end());
    const std::complex<double> low = component(y, fs, 100.0, 4800, 48000);
    const std::complex<double> high = component(y, fs, 5000.0, 4800, 48000);
    CHECK_NEAR(std::abs(low), std::abs(d.response(100.0)), 0.0, 1e-3);
    CHECK_NEAR(std::arg(low), std::arg(d.response(100.0)), 0.0, 1e-3);
    CHECK(std::abs(high) < 2.0 * std::abs(d.response(5000.0)) + 1e-6);

    // Forward and backward: no phase, the gain squared.
    r.application = FilterApplication::ZeroPhase;
    d = designFilter(r);
    out = applyFilter(d, source, {1});
    REQUIRE(out.error.empty());
    REQUIRE(out.signal.channelCount() == 1);
    const std::vector<double> z(out.signal.channel(0).begin(), out.signal.channel(0).end());
    const std::complex<double> still = component(z, fs, 100.0, 4800, 43200);
    CHECK_NEAR(std::abs(still), std::norm(d.response(100.0)), 0.0, 1e-3);
    CHECK(std::abs(std::arg(still)) < 1e-3);

    // A FIR: aligned (no delay), forward and backward (no phase either), causal (its delay).
    FilterRecipe f = defaultFilterRecipe(fs);
    f.kind = FilterKind::Fir;
    f.taps = 201;
    for (FilterApplication a : {FilterApplication::Aligned, FilterApplication::ZeroPhase, FilterApplication::Causal})
    {
        f.application = a;
        const FilterDesign fir = designFilter(f);
        const FilteredSignal filtered = applyFilter(fir, source, {0});
        REQUIRE(filtered.error.empty());
        const std::vector<double> w(filtered.signal.channel(0).begin(), filtered.signal.channel(0).end());
        const std::complex<double> c = component(w, fs, 100.0, 4800, 43200);
        const std::complex<double> expected = fir.applied(100.0);
        CHECK_NEAR(std::abs(c), std::abs(expected), 0.0, 1e-3);
        CHECK_NEAR(std::arg(c), std::arg(expected), 0.0, 1e-3);
    }

    // The first frames only (a preview); another rate; a channel that does not exist; stopped.
    out = applyFilter(designFilter(defaultFilterRecipe(fs)), source, {}, 1000);
    REQUIRE(out.error.empty());
    CHECK(out.signal.frameCount() == 1000);
    CHECK(contains(out.signal.notes, "Les 1\xE2\x80\xAF" "000 premiers échantillons seulement."));
    const FilterDesign other = designFilter(defaultFilterRecipe(44100.0));
    out = applyFilter(other, source);
    CHECK(contains(out.error, "Le filtre est conçu pour fs = 44,1 kHz et « Essai » est échantillonné à 48 kHz"));
    out = applyFilter(designFilter(defaultFilterRecipe(fs)), source, {2});
    CHECK(out.error == "Voie 3 inexistante dans « Essai ».");
    const std::atomic<bool> stop{true};
    out = applyFilter(designFilter(defaultFilterRecipe(fs)), source, {}, 0, model::Progress{&stop, nullptr});
    CHECK(out.cancelled);
    model::Signal holes = source;
    auto data = std::make_shared<model::SampleData>(*source.data);
    data->discontinuities = {100};
    holes.data = data;
    out = applyFilter(designFilter(defaultFilterRecipe(fs)), holes);
    REQUIRE(out.warnings.size() == 1);
    CHECK(contains(out.warnings[0], "a des trous"));
    CHECK((out.signal.data->discontinuities == std::vector<std::size_t>{100}));
}

TEST_CASE("filters: zero phase, the edges prolonged as chosen", "[analysis]")
{
    // An AM that starts on a crest of its carrier, through a band-pass around it.
    const double fs = 48000.0;
    model::Signal am = tones(fs, 4800, {});
    auto data = std::make_shared<model::SampleData>(*am.data);
    for (std::size_t i = 0; i < data->channels[0].size(); ++i)
    {
        const double t = static_cast<double>(i) / fs;
        data->channels[0][i] = (1.0 + 0.5 * std::sin(2.0 * kPi * 500.0 * t)) * std::cos(2.0 * kPi * 10000.0 * t);
    }
    am.data = data;
    FilterRecipe r = defaultFilterRecipe(fs);
    r.response = dsp::FilterResponse::BandPass;
    r.order = 6;
    r.frequency1Hz = 9000.0;
    r.frequency2Hz = 11000.0;
    r.application = FilterApplication::ZeroPhase;
    CHECK(r.edges == dsp::EdgeExtension::Predicted); // the default
    // The largest output in the middle and within 480 samples of an end.
    const auto peaks = [&](dsp::EdgeExtension e) {
        r.edges = e;
        const FilteredSignal out = applyFilter(designFilter(r), am);
        double middle = 0.0, edge = 0.0;
        const std::span<const double> y = out.signal.channel(0);
        for (std::size_t i = 0; i < y.size(); ++i)
            (i < 480 || i + 480 >= y.size() ? edge : middle) = std::max(i < 480 || i + 480 >= y.size() ? edge : middle,
                                                                       std::abs(y[i]));
        return std::pair{middle, edge};
    };
    const auto [middle, predicted] = peaks(dsp::EdgeExtension::Predicted);
    const auto [same, odd] = peaks(dsp::EdgeExtension::Odd);
    CHECK_NEAR(middle, 1.5, 0.02, 0.0);
    CHECK(predicted < 1.01 * middle);
    CHECK(odd > 1.08 * middle); // the burst of SciPy's odd extension
    if (qacq::test::state().verbose)
        std::printf("    AM through a band-pass, largest output: %.4g in the middle, %.4g at the ends predicted, %.4g odd\n",
                    middle, predicted, odd);

    // Each extension: what the summary says, as dsp runs it, through JSON.
    for (dsp::EdgeExtension e : {dsp::EdgeExtension::Predicted, dsp::EdgeExtension::Odd, dsp::EdgeExtension::Even,
                                 dsp::EdgeExtension::Constant, dsp::EdgeExtension::None})
    {
        r.edges = e;
        const FilterDesign d = designFilter(r);
        const std::vector<std::string> summary = designSummary(d);
        CHECK(std::find(summary.begin(), summary.end(), edgeExtensionText(e)) != summary.end());
        const std::span<const double> x = am.channel(0);
        const std::vector<double> y = runFilter(d, x);
        const std::vector<double> z = dsp::sosFiltFilt(d.sections, x, -1, e);
        REQUIRE(y.size() == z.size());
        double gap = 0.0;
        for (std::size_t i = 0; i < y.size(); ++i) gap = std::max(gap, std::abs(y[i] - z[i]));
        CHECK(gap == 0.0);
        CHECK(filterRecipeFromJson(toJson(r)).edges == e);
    }
    CHECK(edgeExtensionKey(dsp::EdgeExtension::Predicted) == "predicted");
    CHECK(edgeExtensionKey(dsp::EdgeExtension::None) == "none");
    model::Json wild = toJson(r);
    wild["edges"] = "reflect";
    CHECK(filterRecipeFromJson(wild).edges == dsp::EdgeExtension::Predicted); // unknown: the default
    CHECK(contains(edgeExtensionText(dsp::EdgeExtension::Odd), "comme filtfilt de SciPy"));
    // Not said when the filter runs as in real time.
    r.application = FilterApplication::Causal;
    for (const std::string &line : designSummary(designFilter(r))) CHECK(!contains(line, "Bords"));

    // A filter that rings over a large part of the record: said, with how far.
    FilterRecipe drift = defaultFilterRecipe(1000.0);
    drift.response = dsp::FilterResponse::HighPass;
    drift.order = 2;
    drift.frequency1Hz = 0.5;
    drift.application = FilterApplication::ZeroPhase;
    const FilterDesign slow = designFilter(drift);
    // It rings for 2 074 samples (its slowest pole, to 1 %): more than a record of 2 s, all of which then depends
    // on the extension; half of a record of 3 s; 21 % of one of 20 s.
    CHECK(dsp::sosRingLength(slow.sections, 1e-2, std::size_t{1} << 30) == 2074);
    FilteredSignal short2s = applyFilter(slow, tones(1000.0, 2000, {{5.0, 1.0}}));
    REQUIRE(short2s.warnings.size() == 1);
    CHECK(contains(short2s.warnings[0], "Ce filtre met environ 2,1 s à se taire"));
    CHECK(contains(short2s.warnings[0], ", plus que les 2 s du signal : tout le résultat dépend du prolongement"));
    CHECK(!contains(short2s.warnings[0], "Les bords")); // predicted, for as long as it rings
    FilteredSignal short3s = applyFilter(slow, tones(1000.0, 3000, {{5.0, 1.0}}));
    REQUIRE(short3s.warnings.size() == 1);
    CHECK(contains(short3s.warnings[0], "à moins de cette durée de chaque bord, soit tout le signal, le résultat dépend"));
    FilteredSignal long60s = applyFilter(slow, tones(1000.0, 60000, {{5.0, 1.0}}));
    CHECK(long60s.warnings.empty());
    FilteredSignal longer = applyFilter(slow, tones(1000.0, 20000, {{5.0, 1.0}}));
    REQUIRE(longer.warnings.size() == 1);
    CHECK(contains(longer.warnings[0], "soit 21 % du signal"));
    drift.application = FilterApplication::Causal;
    CHECK(applyFilter(designFilter(drift), tones(1000.0, 2000, {{5.0, 1.0}})).warnings.empty());

    // A band of 40 Hz at 10,5 kHz (order 10) over 20 ms of an AM at 10 kHz, 500 Hz, 50 %: it rings for 0,23 s,
    // twelve times the record. Predicted, the record goes on as the three sinusoids it is, for as long as the
    // filter rings: the upper side band alone comes out, 0,25 V, everywhere. Prolonged as SciPy does, by 63
    // samples, or not at all, the filter hardly starts: 0,2 µV at most (scipy.signal.sosfiltfilt gives the same).
    FilterRecipe narrow = defaultFilterRecipe(fs);
    narrow.response = dsp::FilterResponse::BandPass;
    narrow.order = 10;
    narrow.frequency1Hz = 10480.0;
    narrow.frequency2Hz = 10520.0;
    narrow.application = FilterApplication::ZeroPhase;
    model::Signal am20 = tones(fs, 960, {});
    auto data20 = std::make_shared<model::SampleData>(*am20.data);
    for (std::size_t i = 0; i < data20->channels[0].size(); ++i)
    {
        const double t = static_cast<double>(i) / fs;
        data20->channels[0][i] = (1.0 + 0.5 * std::cos(2.0 * kPi * 500.0 * t)) * std::cos(2.0 * kPi * 10000.0 * t);
    }
    am20.data = data20;
    const FilterDesign sideBand = designFilter(narrow);
    REQUIRE(sideBand.ok());
    CHECK(dsp::sosRingLength(sideBand.sections, 1e-2, std::size_t{1} << 30) == 11251);
    CHECK(dsp::sosFiltFiltPad(sideBand.sections, 960, dsp::EdgeExtension::Odd) == 63);
    CHECK(dsp::sosFiltFiltPad(sideBand.sections, 960, dsp::EdgeExtension::Predicted) == 33752);
    const FilteredSignal kept = applyFilter(sideBand, am20);
    REQUIRE(kept.warnings.size() == 1);
    CHECK(contains(kept.warnings[0], "Ce filtre met environ 234 ms à se taire (sa réponse impulsionnelle retombe à 1 %), "
                                     "plus que les 20 ms du signal : tout le résultat dépend du prolongement des bords"));
    CHECK(!contains(kept.warnings[0], "Les bords"));
    const std::span<const double> upper = kept.signal.channel(0);
    double low = 1.0, high = 0.0;
    for (std::size_t i = 0; i + 92 <= upper.size(); i += 92) // a period of 10,5 kHz: 32/7 samples, 7 periods: 32
    {
        double peak = 0.0;
        for (std::size_t k = i; k < i + 92; ++k) peak = std::max(peak, std::abs(upper[k]));
        low = std::min(low, peak);
        high = std::max(high, peak);
    }
    CHECK_NEAR(low, 0.25, 0.0, 0.002);
    CHECK_NEAR(high, 0.25, 0.0, 0.002);
    for (dsp::EdgeExtension e : {dsp::EdgeExtension::Odd, dsp::EdgeExtension::Even, dsp::EdgeExtension::Constant,
                                 dsp::EdgeExtension::None})
    {
        narrow.edges = e;
        const FilteredSignal started = applyFilter(designFilter(narrow), am20);
        double largest = 0.0;
        for (double v : started.signal.channel(0)) largest = std::max(largest, std::abs(v));
        CHECK(largest < 3e-7);
        REQUIRE(started.warnings.size() == 1);
        CHECK(contains(started.warnings[0], e == dsp::EdgeExtension::None
                                                ? " Les bords ne sont pas prolongés : la mise en route du filtre s'y voit ; "
                                                  "« Prédits » les prolonge aussi longtemps qu'il sonne."
                                                : " Les bords ne sont prolongés que de 63 échantillons (comme SciPy) : la mise "
                                                  "en route du filtre s'y voit ; « Prédits » les prolonge aussi longtemps qu'il sonne."));
        if (qacq::test::state().verbose)
            std::printf("    40 Hz at 10,5 kHz over 20 ms, %s: %.3g V at most\n", std::string(edgeExtensionKey(e)).c_str(), largest);
    }
}

namespace
{
// b and a of a cascade, multiplied out (as the coefficients a user would copy from SciPy's sos2tf).
std::pair<std::vector<double>, std::vector<double>> multiplied(const dsp::SosCascade &sections)
{
    std::vector<double> b{1.0}, a{1.0};
    const auto times = [](const std::vector<double> &p, const std::array<double, 3> &q) {
        std::vector<double> r(p.size() + 2, 0.0);
        for (std::size_t i = 0; i < p.size(); ++i)
            for (std::size_t j = 0; j < 3; ++j) r[i + j] += p[i] * q[j];
        return r;
    };
    for (const dsp::Biquad &s : sections)
    {
        b = times(b, s.b);
        a = times(a, s.a);
    }
    return {b, a};
}

std::string listed(const std::vector<double> &v)
{
    std::string text;
    char buffer[40];
    for (double x : v)
    {
        std::snprintf(buffer, sizeof buffer, "%.17g", x);
        text += (text.empty() ? "" : " ; ") + std::string(buffer);
    }
    return text;
}

FilterRecipe typedRecipe(std::string b, std::string a, std::string variables = {}, double fs = 48000.0)
{
    FilterRecipe r = defaultFilterRecipe(fs);
    r.kind = FilterKind::Typed;
    r.numerator = std::move(b);
    r.denominator = std::move(a);
    r.variables = std::move(variables);
    return r;
}
} // namespace

TEST_CASE("filters: coefficients typed, lists and formulas of k", "[analysis]")
{
    // How a list is read: French, SciPy, MATLAB, NumPy.
    using List = std::vector<std::string>;
    CHECK((splitCoefficients("1 ; -2 ; 1") == List{"1", "-2", "1"}));
    CHECK((splitCoefficients("[1, -2, 1]") == List{"1", "-2", "1"}));
    CHECK((splitCoefficients("[1 -2 1]") == List{"1", "-2", "1"}));
    CHECK((splitCoefficients("array([ 0.5,  0.25])") == List{"0.5", "0.25"}));
    CHECK((splitCoefficients("array([1., 2.], dtype=float32)") == List{"1.", "2."}));
    CHECK((splitCoefficients("[0.5,0.25]") == List{"0.5", "0.25"}));
    CHECK((splitCoefficients("0,5 ; 0,25") == List{"0,5", "0,25"}));
    CHECK((splitCoefficients("1,5") == List{"1,5"}));
    CHECK((splitCoefficients("1,2,3") == List{"1", "2", "3"}));
    CHECK((splitCoefficients("1,5 2,5") == List{"1,5", "2,5"}));
    CHECK((splitCoefficients("min(1, 2) ; 3") == List{"min(1, 2)", "3"}));
    CHECK((splitCoefficients("1\n2\n\n3 ;") == List{"1", "2", "3"}));
    CHECK((splitCoefficients("2 pi") == List{"2 pi"}));
    CHECK((splitCoefficients("1 - 2") == List{"1 - 2"}));
    CHECK((splitCoefficients("\xE2\x88\x92" "1 2") == List{"\xE2\x88\x92" "1", "2"}));
    CHECK(splitCoefficients("  ").empty());

    // Evaluated with fs, Te, k and the variables; one formula as many times as said.
    FilterRecipe r = typedRecipe("1/N", "1", "N = 4");
    r.numeratorCount = "N";
    TypedCoefficients t = evaluateTypedCoefficients(r);
    REQUIRE(t.error.empty());
    CHECK((t.numerator == std::vector<double>(4, 0.25)));
    CHECK(t.numeratorOfK);
    CHECK(!t.denominatorOfK);
    r.numerator = "k^2";
    r.numeratorCount = "3";
    CHECK((evaluateTypedCoefficients(r).numerator == std::vector<double>{0.0, 1.0, 4.0}));
    r.numerator = "fs ; -1/Te ; k";
    CHECK((evaluateTypedCoefficients(r).numerator == std::vector<double>{48000.0, -48000.0, 2.0}));
    // What does not read: said, and where.
    const auto error = [](FilterRecipe x) { return evaluateTypedCoefficients(x).error; };
    r.numerator = "k";
    r.numeratorCount = "";
    CHECK(error(r) == "b[k] dépend de k : combien de coefficients ? (« Nombre de b »)");
    r.numeratorCount = "2,5";
    CHECK(error(r) == "Nombre de b : 2,5 n'est pas un nombre entier de 1 à 65\xE2\x80\xAF" "535.");
    r.numeratorCount = "0";
    CHECK(contains(error(r), "Nombre de b : 0 n'est pas un nombre entier"));
    r.numerator = "1 ; x";
    r.numeratorCount = "";
    CHECK(contains(error(r), "b[1] : Nom inconnu « x »"));
    r.numerator = "1/k";
    r.numeratorCount = "2";
    CHECK(error(r) == "b[0] n'est pas un nombre fini (une division par zéro ?).");
    r.numerator = "";
    CHECK(error(r) == "b : aucun coefficient.");
    r.numerator = "1";
    r.denominator = " ";
    CHECK(error(r) == "a : aucun coefficient (« 1 » pour un RIF).");
    r.denominator = "1";
    r.variables = "k = 2";
    CHECK(contains(error(r), "« k » est déjà"));
    r.variables = "fs = 2";
    CHECK(contains(error(r), "« fs » est déjà"));

    // A moving average: a FIR, its zeros at fs/N, 2 fs/N…
    r = typedRecipe("1/N", "1", "N = 8");
    r.numeratorCount = "N";
    FilterDesign d = designFilter(r);
    REQUIRE(d.ok());
    CHECK(d.fir());
    CHECK((d.taps == std::vector<double>(8, 0.125)));
    CHECK(d.zeros.size() == 7);
    CHECK_NEAR(std::abs(d.response(0.0)), 1.0, 1e-15, 0.0);
    CHECK(std::abs(d.response(6000.0)) < 1e-15);
    CHECK(filterText(r) == "Coefficients tapés, RIF de 8 coefficients");
    CHECK(filterShortText(r) == "coefficients tapés");
    r.application = FilterApplication::Aligned; // a FIR: aligned is allowed
    CHECK(designFilter(r).recipe.application == FilterApplication::Aligned);

    // Exponential smoothing: y[n] = α x[n] + (1 − α) y[n − 1], as lfilter does it.
    r = typedRecipe("alpha", "1 ; alpha - 1", "alpha = 0,1");
    d = designFilter(r);
    REQUIRE(d.ok());
    CHECK(!d.fir());
    CHECK(d.poles.size() == 1);
    CHECK_NEAR(d.poles[0].real(), 0.9, 1e-15, 0.0);
    CHECK(filterText(r) == "Coefficients tapés, RII d'ordre 1 (1 b, 2 a)");
    const model::Signal noise = tones(48000.0, 4800, {{440.0, 1.0}, {9000.0, 0.5}});
    const std::span<const double> x = noise.channel(0);
    const std::vector<double> smoothed = runFilter(d, x);
    const std::vector<double> reference = dsp::lfilter(std::vector<double>{0.1}, std::vector<double>{1.0, -0.9}, x);
    double gap = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) gap = std::max(gap, std::abs(smoothed[i] - reference[i]));
    CHECK(gap < 1e-14);
    r.application = FilterApplication::Aligned; // an IIR: not aligned
    CHECK(designFilter(r).recipe.application == FilterApplication::Causal);

    // Divided by a[0]; the b at 0 in front, a delay; trailing zeros left out.
    d = designFilter(typedRecipe("2 ; 2 ; 0", "2 ; -1 ; 0"));
    REQUIRE(d.ok());
    CHECK(contains(designSummary(d)[0], "Coefficients divisés par a[0] = 2"));
    CHECK_NEAR(std::abs(d.response(0.0)), 4.0, 1e-14, 0.0); // (1 + 1)/(1 − 0,5)
    d = designFilter(typedRecipe("0 ; 0 ; 0 ; 1", "1 ; -0,5"));
    REQUIRE(d.ok());
    const ImpulseResponse delayed = impulseResponse(d);
    REQUIRE(delayed.impulse.size() > 8);
    for (std::size_t i = 0; i < 8; ++i)
        CHECK_NEAR(delayed.impulse[i], i < 3 ? 0.0 : std::pow(0.5, static_cast<double>(i - 3)), 0.0, 1e-14);

    // A Butterworth of order 6 typed from its b and a (as copied from SciPy): the same filter.
    FilterRecipe designed = defaultFilterRecipe(48000.0);
    designed.order = 6;
    designed.frequency1Hz = 4000.0;
    const FilterDesign reference6 = designFilter(designed);
    const auto [b6, a6] = multiplied(reference6.sections);
    d = designFilter(typedRecipe(listed(b6), listed(a6)));
    REQUIRE(d.ok());
    CHECK(d.warnings.empty());
    CHECK(d.poles.size() == 6);
    CHECK(contains(filterText(d.recipe), "RII d'ordre 6 (7 b, 7 a)"));
    double worst = 0.0;
    for (double f = 0.0; f <= 24000.0; f += 250.0)
        worst = std::max(worst, std::abs(d.response(f) - reference6.response(f)));
    CHECK(worst < 1e-9);
    const std::vector<double> y6 = runFilter(d, x);
    const std::vector<double> z6 = dsp::lfilter(b6, a6, x);
    gap = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) gap = std::max(gap, std::abs(y6[i] - z6[i]));
    CHECK(gap < 1e-9);
    if (qacq::test::state().verbose)
        std::printf("    Butterworth 6 typed: response off by %.2g, filtered off by %.2g from lfilter\n", worst, gap);

    // Unstable: designed (its responses shown) but not applied; its impulse response only while it grows.
    r = typedRecipe("1", "1 ; -1,05");
    d = designFilter(r);
    REQUIRE(d.ok());
    REQUIRE(d.warnings.size() == 1);
    CHECK(contains(d.warnings[0], "Instable : un pôle à |z| = 1,05000"));
    CHECK(contains(d.applicationError, "instable"));
    CHECK(contains(applyFilter(d, noise).error, "Il n'est pas appliqué"));
    const ImpulseResponse growing = impulseResponse(d);
    CHECK(!growing.settled);
    CHECK(growing.impulse.size() == 284); // ln(1e6)/ln(1,05)
    CHECK(growing.appliedImpulse.empty());
    // On the unit circle (an integrator by trapezoids): causal yes, zero phase no.
    r = typedRecipe("Te/2 ; Te/2", "1 ; -1", "", 1000.0);
    d = designFilter(r);
    REQUIRE(d.ok());
    CHECK(d.applicationError.empty());
    CHECK(contains(d.warnings[0], "sur le cercle unité"));
    const model::Signal ones = tones(1000.0, 1000, {});
    auto unit = std::make_shared<model::SampleData>(*ones.data);
    std::fill(unit->channels[0].begin(), unit->channels[0].end(), 1.0);
    model::Signal constant = ones;
    constant.data = unit;
    const FilteredSignal integrated = applyFilter(d, constant);
    REQUIRE(integrated.error.empty());
    CHECK_NEAR(integrated.signal.channel(0)[999], 0.9995, 1e-12, 0.0); // ∫ 1 dt over 999,5 periods
    r.application = FilterApplication::ZeroPhase;
    CHECK(contains(designFilter(r).applicationError, "appliquez ce filtre en causal"));

    // What cannot be designed.
    CHECK(designFilter(typedRecipe("1", "0 ; 1")).error ==
          "a[0] = 0 : le premier coefficient du dénominateur, celui de y[n], ne peut pas être nul.");
    CHECK(designFilter(typedRecipe("0 ; 0", "1")).error == "Tous les b sont nuls : ce filtre ne laisse rien passer.");
    r = typedRecipe("1", "k + 1", "");
    r.denominatorCount = "300";
    CHECK(contains(designFilter(r).error, "au plus 257 b et 257 a"));

    // JSON: back as it was; numbers and lists of numbers read as well.
    r = typedRecipe("1/N", "1 ; -0,5", "N = 8");
    r.numeratorCount = "N";
    CHECK(toJson(filterRecipeFromJson(toJson(r))) == toJson(r));
    CHECK(filterRecipeFromJson(toJson(r)).kind == FilterKind::Typed);
    model::Json j = toJson(r);
    j["b"] = model::Json::array();
    j["b"].push_back(1.0);
    j["b"].push_back(-2.0);
    j["b"].push_back(0.1);
    j["a"] = 1.0;
    const FilterRecipe fromNumbers = filterRecipeFromJson(j);
    CHECK(fromNumbers.numerator == "1 ; -2 ; 0.1");
    CHECK(fromNumbers.denominator == "1");
}

TEST_CASE("filters: elliptic, where its stop band starts", "[analysis]")
{
    FilterRecipe r = defaultFilterRecipe(48000.0);
    r.family = dsp::FilterFamily::Elliptic;
    r.order = 5;
    r.passbandRippleDb = 0.5;
    r.stopbandAttenuationDb = 60.0;
    const FilterDesign d = designFilter(r);
    REQUIRE(d.ok());
    CHECK(filterText(r) == "Passe-bas elliptique d'ordre 5 (0,5 dB, 60 dB), 1 kHz");
    REQUIRE(d.stopEdgesHz.size() == 1);
    const double edge = d.stopEdgesHz[0];
    CHECK(edge > 1700.0);
    CHECK(edge < 1800.0);
    // There, −60 dB; beyond, never more.
    CHECK_NEAR(db(d.response(edge)), -60.0, 0.0, 1e-6);
    double worst = -1e9;
    for (int i = 0; i <= 5000; ++i) worst = std::max(worst, db(d.response(edge + (24000.0 - edge) * i / 5000.0)));
    CHECK(worst < -60.0 + 1e-6);
    bool said = false;
    for (const std::string &line : designSummary(d)) said = said || contains(line, "Atténuation d'au moins 60 dB au-delà de 1,77");
    CHECK(said);

    // Every response: the edges, and −Rs there.
    for (const auto response : {dsp::FilterResponse::HighPass, dsp::FilterResponse::BandPass, dsp::FilterResponse::BandStop})
    {
        FilterRecipe x = r;
        x.response = response;
        x.order = 4;
        const FilterDesign e = designFilter(x);
        REQUIRE(e.ok());
        const std::size_t expected = response == dsp::FilterResponse::HighPass ? 1 : 2;
        REQUIRE(e.stopEdgesHz.size() == expected);
        for (double f : e.stopEdgesHz) CHECK_NEAR(db(e.response(f)), -60.0, 0.0, 1e-5);
        if (response == dsp::FilterResponse::BandPass)
        {
            CHECK(e.stopEdgesHz[0] < x.frequency1Hz);
            CHECK(e.stopEdgesHz[1] > x.frequency2Hz);
        }
        if (response == dsp::FilterResponse::BandStop)
        {
            CHECK(e.stopEdgesHz[0] > x.frequency1Hz);
            CHECK(e.stopEdgesHz[1] < x.frequency2Hz);
        }
    }
    CHECK(toJson(filterRecipeFromJson(toJson(r))) == toJson(r));
    CHECK(filterRecipeFromJson(toJson(r)).family == dsp::FilterFamily::Elliptic);
    // A ripple above the attenuation: said.
    r.passbandRippleDb = 70.0;
    CHECK(!designFilter(r).ok());
}

TEST_CASE("filters: an equiripple FIR (Parks-McClellan), what it reached", "[analysis]")
{
    FilterRecipe r = defaultFilterRecipe(48000.0);
    r.kind = FilterKind::Fir;
    r.firMethod = FirMethod::Equiripple;
    r.taps = 101;
    r.frequency1Hz = 2000.0;
    r.transitionHz = 1400.0;
    r.passbandRippleDb = 0.1;
    r.stopbandAttenuationDb = 60.0;
    const FilterDesign d = designFilter(r);
    REQUIRE(d.ok());
    CHECK(filterText(r) == "Passe-bas RIF équiondulation de 101 coefficients (transition de 1,4 kHz), 2 kHz");
    CHECK(d.taps.size() == 101);
    for (std::size_t k = 0; k < 50; ++k) CHECK(d.taps[k] == d.taps[100 - k]);
    CHECK(d.iterations > 1);
    // Better than asked, and the same as measured here.
    CHECK(d.achievedRippleDb < 0.1);
    CHECK(d.achievedAttenuationDb > 60.0);
    CHECK(d.warnings.empty());
    double stop = 0.0, high = 0.0, low = 1e9;
    for (int i = 0; i <= 20000; ++i)
    {
        stop = std::max(stop, std::abs(d.response(2700.0 + 21300.0 * i / 20000.0)));
        const double h = std::abs(d.response(1300.0 * i / 20000.0));
        high = std::max(high, h);
        low = std::min(low, h);
    }
    CHECK_NEAR(-20.0 * std::log10(stop), d.achievedAttenuationDb, 0.0, 0.01);
    CHECK_NEAR(20.0 * std::log10(high / low), d.achievedRippleDb, 0.0, 1e-3);
    bool said = false;
    for (const std::string &line : designSummary(d)) said = said || contains(line, "Équiondulation (Parks-McClellan");
    CHECK(said);
    CHECK(toJson(filterRecipeFromJson(toJson(r))) == toJson(r));
    CHECK(filterRecipeFromJson(toJson(r)).firMethod == FirMethod::Equiripple);
    CHECK(filterRecipeFromJson(toJson(r)).transitionHz == 1400.0);

    // Every response, odd taps.
    for (const auto response : {dsp::FilterResponse::HighPass, dsp::FilterResponse::BandPass, dsp::FilterResponse::BandStop})
    {
        FilterRecipe x = r;
        x.response = response;
        x.frequency1Hz = 4000.0;
        x.frequency2Hz = 9000.0;
        const FilterDesign e = designFilter(x);
        REQUIRE(e.ok());
        CHECK(e.achievedAttenuationDb > 40.0);
    }
    // Kaiser's estimate of what the template needs.
    CHECK(d.estimatedTaps > 80);
    CHECK(d.estimatedTaps < 110);
    CHECK(d.computedTaps == 101);
    CHECK(d.exchangeNote.empty());
    // Too few taps for the template: said, how far, and how many it would need.
    FilterRecipe tight = r;
    tight.transitionHz = 200.0;
    const FilterDesign t = designFilter(tight);
    REQUIRE(t.ok());
    REQUIRE(t.warnings.size() == 1);
    CHECK(contains(t.warnings[0], "Le gabarit n'est pas tenu avec 101 coefficients"));
    CHECK(contains(t.warnings[0], "il en faudrait environ " + std::to_string(t.estimatedTaps)));
    CHECK(t.estimatedTaps > 600);
    // Long enough for it: 1 001 taps, converged, the template held.
    FilterRecipe longer = tight;
    longer.taps = 1001;
    const FilterDesign l = designFilter(longer);
    REQUIRE(l.ok());
    CHECK(l.warnings.empty());
    CHECK(l.exchangeNote.empty());
    CHECK(l.achievedAttenuationDb > 60.0);
    // 1 001 taps for a transition of 1,4 kHz would reach far beyond the precision of the computation (its best
    // error near 1e-30): computed on the taps of the middle, the others zeros; still linear phase, (N − 1)/2.
    FilterRecipe far = r;
    far.taps = 1001;
    const FilterDesign f = designFilter(far);
    REQUIRE(f.ok());
    CHECK(f.warnings.empty());
    REQUIRE(f.taps.size() == 1001);
    CHECK(f.computedTaps < 1001);
    CHECK(f.computedTaps % 2 == 1);
    CHECK(f.taps.front() == 0.0);
    CHECK(f.taps.back() == 0.0);
    CHECK(f.taps[500] != 0.0);
    for (std::size_t k = 0; k < 500; ++k) CHECK(f.taps[k] == f.taps[1000 - k]);
    CHECK(f.achievedAttenuationDb > 150.0);
    CHECK(f.achievedRippleDb < 1e-3);
    bool middle = false;
    for (const std::string &line : designSummary(f)) middle = middle || contains(line, "coefficients du milieu");
    CHECK(middle);
    // What cannot be designed.
    FilterRecipe wide = r;
    wide.transitionHz = 4100.0; // 2 kHz − 2,05 kHz < 0
    CHECK(contains(designFilter(wide).error, "les bandes se chevauchent"));
    FilterRecipe even = r;
    even.response = dsp::FilterResponse::HighPass;
    even.taps = 100;
    CHECK(contains(designFilter(even).error, "impair"));
    FilterRecipe many = r;
    many.taps = 5001;
    CHECK(contains(designFilter(many).error, "4\xE2\x80\xAF" "095 coefficients au plus"));
}

TEST_CASE("filters: a gain at the output, once whatever the application", "[analysis]")
{
    FilterRecipe r = defaultFilterRecipe(48000.0);
    r.gainDb = 20.0 * std::log10(2.0); // ×2
    FilterDesign d = designFilter(r);
    REQUIRE(d.ok());
    CHECK_NEAR(d.gain, 2.0, 1e-12, 0.0);
    // The sections are those of the filter at 0 dB; the responses carry the gain.
    CHECK_NEAR(std::abs(d.unitResponse(0.0)), 1.0, 1e-9, 0.0);
    CHECK_NEAR(std::abs(d.response(0.0)), 2.0, 1e-9, 0.0);
    CHECK_NEAR(std::abs(d.applied(1000.0)), 2.0 * std::sqrt(0.5), 1e-6, 0.0);
    // At zero phase the filter counts twice, the gain once: 2 × |H|², 1 at the cut-off (not 4 × 0,5).
    r.application = FilterApplication::ZeroPhase;
    d = designFilter(r);
    CHECK_NEAR(d.applied(0.0).real(), 2.0, 1e-9, 0.0);
    CHECK_NEAR(d.applied(1000.0).real(), 1.0, 1e-6, 0.0);
    const std::vector<double> ones(4000, 1.0);
    const std::vector<double> y = runFilter(d, ones);
    CHECK_NEAR(y[2000], 2.0, 1e-9, 0.0);
    r.application = FilterApplication::Causal;
    d = designFilter(r);
    CHECK_NEAR(runFilter(d, ones).back(), 2.0, 1e-9, 0.0);
    // The impulse and step responses too: the step settles at 2.
    const ImpulseResponse ir = impulseResponse(d);
    CHECK_NEAR(ir.step.back(), 2.0, 1e-6, 0.0);
    // A FIR, aligned.
    FilterRecipe f = r;
    f.kind = FilterKind::Fir;
    f.application = FilterApplication::Aligned;
    f.gainDb = -20.0 * std::log10(2.0); // ×0,5
    const FilterDesign g = designFilter(f);
    REQUIRE(g.ok());
    double sum = 0.0;
    for (double t : g.taps) sum += t;
    CHECK_NEAR(sum, 1.0, 1e-9, 0.0);
    CHECK_NEAR(runFilter(g, ones)[2000], 0.5, 1e-9, 0.0);
    CHECK_NEAR(impulseResponse(g).step[g.taps.size() + 2], 0.5, 1e-9, 0.0);

    // Said.
    CHECK(filterText(r) == "Passe-bas Butterworth d'ordre 4, 1 kHz, gain +6,021 dB");
    CHECK(filterShortText(r) == "passe-bas 1 kHz, +6,021 dB");
    CHECK(contains(filterText(f), ", gain \xE2\x88\x92" "6,021 dB"));
    bool said = false;
    for (const std::string &line : designSummary(d)) said = said || contains(line, "Gain en sortie de +6,021 dB (×2)");
    CHECK(said);
    // Kept, within bounds.
    CHECK(filterRecipeFromJson(toJson(r)).gainDb == r.gainDb);
    model::Json wild = toJson(r);
    wild["gain"] = 500.0;
    CHECK(filterRecipeFromJson(wild).gainDb == kMaximumFilterGainDb);
    CHECK(filterRecipeFromJson(model::Json::object()).gainDb == 0.0);
}

namespace
{
// What a design holds of its template, on a grid of 5 001 points a band (in logarithm over more than a decade), to
// 1e-6: independent of measureTemplate.
bool holdsZones(const FilterDesign &d)
{
    const TemplateZones z = templateZones(d.recipe);
    bool ok = true;
    const auto each = [&d](const std::array<double, 2> &band, auto &&f) {
        const bool logarithmic = band[0] > 0.0 && band[1] > 20.0 * band[0];
        for (int i = 0; i <= 5000; ++i)
        {
            const double x = i / 5000.0;
            f(std::abs(d.unitResponse(logarithmic ? band[0] * std::pow(band[1] / band[0], x) : band[0] + (band[1] - band[0]) * x)));
        }
    };
    for (const auto &b : z.passBands)
        each(b, [&](double m) { ok = ok && m >= z.passLow * (1.0 - 1e-6) && m <= z.passHigh * (1.0 + 1e-6); });
    for (const auto &b : z.stopBands) each(b, [&](double m) { ok = ok && m <= z.stopHigh * (1.0 + 1e-6); });
    return ok;
}

// A template at 48 kHz for each response: 0,5 dB at most over the pass band, 50 dB at least over the stop band,
// transitions of 400 Hz or 600 Hz.
FilterRecipe templateOf(dsp::FilterResponse response)
{
    FilterRecipe r;
    r.fromTemplate = true;
    r.sampleRateHz = 48000.0;
    r.response = response;
    r.passbandRippleDb = 0.5;
    r.stopbandAttenuationDb = 50.0;
    switch (response)
    {
        case dsp::FilterResponse::LowPass:
            r.passEdge1Hz = 1000.0;
            r.stopEdge1Hz = 1600.0;
            break;
        case dsp::FilterResponse::HighPass:
            r.stopEdge1Hz = 1000.0;
            r.passEdge1Hz = 1600.0;
            break;
        case dsp::FilterResponse::BandPass:
            r.stopEdge1Hz = 600.0;
            r.passEdge1Hz = 1000.0;
            r.passEdge2Hz = 3000.0;
            r.stopEdge2Hz = 3400.0;
            break;
        case dsp::FilterResponse::BandStop:
            r.passEdge1Hz = 1000.0;
            r.stopEdge1Hz = 1400.0;
            r.stopEdge2Hz = 2600.0;
            r.passEdge2Hz = 3000.0;
            break;
    }
    return r;
}

constexpr std::array kResponses{dsp::FilterResponse::LowPass, dsp::FilterResponse::HighPass, dsp::FilterResponse::BandPass,
                                dsp::FilterResponse::BandStop};
} // namespace

TEST_CASE("filters: from a template, the least IIR order that holds it", "[analysis]")
{
    for (const dsp::FilterResponse response : kResponses)
    {
        for (const dsp::FilterFamily family : {dsp::FilterFamily::Butterworth, dsp::FilterFamily::ChebyshevI,
                                               dsp::FilterFamily::ChebyshevII, dsp::FilterFamily::Elliptic, dsp::FilterFamily::Bessel})
        {
            FilterRecipe r = templateOf(response);
            r.family = family;
            const dsp::IirOrder o = dsp::minimumIirOrder(family, filterTemplate(r));
            REQUIRE(o.ok());
            const FilterDesign d = designFilter(r);
            const int maximum = family == dsp::FilterFamily::Bessel ? dsp::kMaximumBesselOrder : dsp::kMaximumIirOrder;
            if (o.order > maximum)
            {
                CHECK(!d.ok());
                CHECK(contains(d.error, "Ce gabarit demande un"));
                CHECK(contains(d.error, "elliptique d'ordre"));
                continue;
            }
            REQUIRE(d.ok());
            CHECK(d.recipe.order == o.order);
            CHECK(d.recipe.frequency1Hz == o.frequency1Hz);
            CHECK(d.templateHeld);
            CHECK(holdsZones(d));
            CHECK(d.templateLossDb <= 0.5 + 1e-6);
            CHECK(d.templateAttenuationDb >= 50.0 - 1e-6);
            CHECK(d.exactOrder == o.exactOrder);
            // What the other designs would need, and the text.
            CHECK(d.templateOrders.elliptic == dsp::minimumIirOrder(dsp::FilterFamily::Elliptic, filterTemplate(r)).order);
            CHECK(d.templateOrders.butterworth >= d.templateOrders.chebyshev);
            CHECK(d.templateOrders.chebyshev >= d.templateOrders.elliptic);
            CHECK(d.templateOrders.equirippleTaps > 0);
            CHECK(contains(filterText(d.recipe), "d'ordre " + std::to_string(o.order) + ", gabarit 0,5 dB"));
            const std::vector<std::string> summary = designSummary(d);
            REQUIRE(summary.size() > 4);
            CHECK(summary[0] == templateText(d.recipe));
            CHECK(contains(summary[1], "le plus petit qui le tient"));
            CHECK(contains(summary[2], "Tenu : "));
            CHECK(contains(summary[3], "Pour ce gabarit : en RII, Butterworth d'ordre"));
            if (qacq::test::state().verbose)
                std::printf("    %s %s : ordre %d, %s\n", std::string(filterResponseKey(response)).c_str(),
                            std::string(filterFamilyKey(family)).c_str(), o.order, summary[2].c_str());
        }
    }
    // An order beyond what is designed: what does hold it is said.
    FilterRecipe steep = templateOf(dsp::FilterResponse::LowPass);
    steep.stopEdge1Hz = 1050.0;
    const FilterDesign beyond = designFilter(steep);
    CHECK(!beyond.ok());
    CHECK(contains(beyond.error, "Butterworth d'ordre"));
    CHECK(contains(beyond.error, "un elliptique d'ordre"));
    // A template that cannot be: edges out of order, Amin under Amax.
    FilterRecipe wrong = templateOf(dsp::FilterResponse::LowPass);
    wrong.stopEdge1Hz = 900.0;
    CHECK(contains(designFilter(wrong).error, "Gabarit d'un passe-bas"));
    CHECK(!templateEdgesValid(wrong));
    wrong = templateOf(dsp::FilterResponse::BandPass);
    wrong.stopbandAttenuationDb = 0.25;
    CHECK(contains(designFilter(wrong).error, "Amin"));
    CHECK(templateEdgesValid(wrong));
}

TEST_CASE("filters: from a template, the least FIR that holds it (windows, equiripple)", "[analysis]")
{
    for (const dsp::FilterResponse response : kResponses)
    {
        const bool odd = response == dsp::FilterResponse::HighPass || response == dsp::FilterResponse::BandStop;
        // The same design with fewer taps (without its zeros: long to find).
        const auto shorter = [](const FilterDesign &d, std::size_t taps) {
            FilterDesign e;
            e.recipe = d.recipe;
            e.recipe.taps = taps;
            if (d.recipe.firMethod == FirMethod::Window)
            {
                dsp::FirSpec spec;
                spec.response = d.recipe.response;
                spec.taps = taps;
                spec.sampleRateHz = d.recipe.sampleRateHz;
                spec.frequency1Hz = d.recipe.frequency1Hz;
                spec.frequency2Hz = d.recipe.frequency2Hz;
                spec.window = d.recipe.window;
                spec.kaiserBeta = d.recipe.kaiserBeta;
                e.taps = dsp::designFir(spec).taps;
                return e;
            }
            // Equiripple over the bands of the template, the stop bands weighted by δp/δs.
            const TemplateZones z = templateZones(d.recipe);
            const double deltaPass = 0.5 * (z.passHigh - z.passLow);
            std::vector<std::array<double, 3>> bands; // low, high, pass
            for (const auto &b : z.passBands) bands.push_back({b[0], b[1], 1.0});
            for (const auto &b : z.stopBands) bands.push_back({b[0], b[1], 0.0});
            std::sort(bands.begin(), bands.end());
            dsp::RemezSpec spec;
            spec.taps = taps;
            spec.sampleRateHz = d.recipe.sampleRateHz;
            spec.maximumIterations = kEquirippleIterations;
            for (const auto &b : bands)
            {
                spec.bandsHz.push_back(b[0]);
                spec.bandsHz.push_back(b[1]);
                spec.desired.push_back(b[2]);
                spec.weights.push_back(b[2] > 0.5 ? 1.0 : deltaPass / z.stopHigh);
            }
            e.taps = dsp::designRemez(spec).taps;
            return e;
        };
        // One tap fewer (an even number: not for a high-pass or a band-stop) and two do not hold it.
        const auto fewerFail = [odd, &shorter](const FilterDesign &d) {
            bool fail = true;
            for (const std::size_t fewer : {std::size_t{1}, std::size_t{2}})
            {
                if (odd && fewer == 1) continue;
                const FilterDesign e = shorter(d, d.recipe.taps - fewer);
                fail = fail && !e.taps.empty() && !measureTemplate(e).held;
            }
            return fail;
        };
        for (const dsp::Window window : {dsp::Window::Hamming, dsp::Window::Kaiser})
        {
            FilterRecipe r = templateOf(response);
            r.kind = FilterKind::Fir;
            r.window = window;
            const FilterDesign d = designFilter(r);
            REQUIRE(d.ok());
            CHECK(d.templateHeld);
            CHECK(holdsZones(d));
            CHECK(!odd || d.recipe.taps % 2 == 1);
            CHECK(fewerFail(d));
            // The cut-offs in the middle of the transitions; Kaiser's β from the deviation asked.
            const double middle = 0.5 * (std::min(r.passEdge1Hz, r.stopEdge1Hz) + std::max(r.passEdge1Hz, r.stopEdge1Hz));
            CHECK_NEAR(d.recipe.frequency1Hz, middle, 0.0, 1e-9);
            if (window == dsp::Window::Kaiser)
                CHECK_REL(d.recipe.kaiserBeta, dsp::estimateKaiser(50.0, 400.0, 48000.0).beta, 1e-12);
            if (qacq::test::state().verbose)
                std::printf("    %s %s : %zu coefficients\n", std::string(filterResponseKey(response)).c_str(),
                            std::string(dsp::windowName(window)).c_str(), d.recipe.taps);
        }
        FilterRecipe r = templateOf(response);
        r.kind = FilterKind::Fir;
        r.firMethod = FirMethod::Equiripple;
        const FilterDesign d = designFilter(r);
        REQUIRE(d.ok());
        CHECK(d.templateHeld);
        CHECK(holdsZones(d));
        CHECK(fewerFail(d));
        CHECK(d.estimatedTaps > 0);
        CHECK(d.iterations > 0);
        CHECK(d.recipe.transitionHz == (response == dsp::FilterResponse::LowPass || response == dsp::FilterResponse::HighPass ? 600.0 : 400.0));
        CHECK(contains(designSummary(d)[1], "en équiondulation"));
        // Designed again (the taps remembered): the same.
        if (response == dsp::FilterResponse::LowPass) CHECK(designFilter(r).taps == d.taps);
        if (qacq::test::state().verbose)
            std::printf("    %s equiripple : %zu coefficients (Kaiser : %zu)\n", std::string(filterResponseKey(response)).c_str(),
                        d.recipe.taps, d.estimatedTaps);
    }
    // What a window cannot reach, what is too long.
    FilterRecipe r = templateOf(dsp::FilterResponse::LowPass);
    r.kind = FilterKind::Fir;
    r.window = dsp::Window::Hamming;
    r.stopbandAttenuationDb = 80.0;
    CHECK(contains(designFilter(r).error, "Avec la fenêtre de Hamming"));
    r.window = dsp::Window::Rectangular;
    CHECK(contains(designFilter(r).error, "la fenêtre rectangulaire"));
    r.firMethod = FirMethod::Equiripple;
    r.stopEdge1Hz = 1001.0;
    CHECK(contains(designFilter(r).error, "coefficients en équiondulation, plus que 4"));
    r.firMethod = FirMethod::Window;
    r.window = dsp::Window::Kaiser;
    r.stopEdge1Hz = 1000.1;
    CHECK(contains(designFilter(r).error, "avec la fenêtre de Kaiser, plus que 65"));
}

TEST_CASE("filters: a template, its edges, texts and JSON", "[analysis]")
{
    // Edges from the frequencies of a filter, for each response; in order.
    for (const dsp::FilterResponse response : kResponses)
    {
        FilterRecipe r = defaultFilterRecipe(48000.0);
        r.response = response;
        r.frequency1Hz = 1000.0;
        r.frequency2Hz = 2000.0;
        r.passEdge1Hz = r.passEdge2Hz = r.stopEdge1Hz = r.stopEdge2Hz = 0.0;
        CHECK(!templateEdgesValid(r));
        setTemplateFromFrequencies(r);
        CHECK(templateEdgesValid(r));
        CHECK(r.passEdge1Hz == 1000.0);
    }
    FilterRecipe low = defaultFilterRecipe(48000.0);
    low.frequency1Hz = 20000.0; // twice would be beyond fs/2: half the way to it
    setTemplateFromFrequencies(low);
    CHECK(low.stopEdge1Hz == 22000.0);
    FilterRecipe notch = defaultFilterRecipe(48000.0);
    notch.response = dsp::FilterResponse::BandStop;
    notch.frequency1Hz = 45.0;
    notch.frequency2Hz = 55.0;
    setTemplateFromFrequencies(notch);
    CHECK_NEAR(notch.stopEdge1Hz * notch.stopEdge2Hz, 45.0 * 55.0, 1e-12, 0.0); // geometric about the middle
    CHECK(notch.stopEdge1Hz > 45.0);
    CHECK(notch.stopEdge2Hz < 55.0);

    // A template read on the gain of a filter: it holds it, and its least order is at most the filter's.
    for (const dsp::FilterResponse response : kResponses)
    {
        for (const dsp::FilterFamily family : {dsp::FilterFamily::Butterworth, dsp::FilterFamily::ChebyshevI,
                                               dsp::FilterFamily::ChebyshevII, dsp::FilterFamily::Elliptic, dsp::FilterFamily::Bessel})
        {
            FilterRecipe m = defaultFilterRecipe(48000.0);
            m.response = response;
            m.family = family;
            const FilterDesign d = designFilter(m);
            REQUIRE(d.ok());
            FilterRecipe t = m;
            t.fromTemplate = true;
            const bool read = templateFromDesign(d, t);
            CHECK(read);
            if (!read) continue;
            CHECK(templateEdgesValid(t));
            FilterDesign probe;
            probe.recipe = t;
            probe.sections = d.sections;
            CHECK(measureTemplate(probe).held);
            const FilterDesign e = designFilter(t);
            REQUIRE(e.ok());
            CHECK(e.recipe.order <= m.order);
            if (qacq::test::state().verbose)
                std::printf("    %s %s : %s -> ordre %d\n", std::string(filterResponseKey(response)).c_str(),
                            std::string(filterFamilyKey(family)).c_str(), templateText(t).c_str(), e.recipe.order);
        }
    }
    {
        // A FIR by a window: its transitions made symmetric about its cut-off, the same filter again.
        FilterRecipe m = defaultFilterRecipe(48000.0);
        m.kind = FilterKind::Fir;
        const FilterDesign d = designFilter(m);
        FilterRecipe t = m;
        t.fromTemplate = true;
        REQUIRE(templateFromDesign(d, t));
        const FilterDesign e = designFilter(t);
        REQUIRE(e.ok());
        CHECK(e.recipe.taps <= m.taps);
        CHECK_NEAR(e.recipe.frequency1Hz, 1000.0, 0.01, 0.0);
        // A 50 Hz notch of order 2, 20 dB: still order 2 (its edges left where SciPy's search would cost an order).
        FilterRecipe mains = defaultFilterRecipe(48000.0);
        mains.response = dsp::FilterResponse::BandStop;
        mains.order = 2;
        mains.frequency1Hz = 45.0;
        mains.frequency2Hz = 55.0;
        mains.stopbandAttenuationDb = 20.0;
        FilterRecipe n = mains;
        n.fromTemplate = true;
        REQUIRE(templateFromDesign(designFilter(mains), n));
        CHECK(designFilter(n).recipe.order == 2);
        // Too gentle for Amin (a Chebyshev II of 40 dB, 60 dB asked): no template, the edges untouched.
        FilterRecipe gentle = defaultFilterRecipe(48000.0);
        gentle.family = dsp::FilterFamily::ChebyshevII;
        FilterRecipe g = gentle;
        g.fromTemplate = true;
        g.stopbandAttenuationDb = 60.0;
        CHECK(!templateFromDesign(designFilter(gentle), g));
        CHECK(g.stopEdge1Hz == gentle.stopEdge1Hz);
    }

    // The zones: an IIR from −Ap dB to 0 dB, a FIR 1 ± δp.
    FilterRecipe r = templateOf(dsp::FilterResponse::BandStop);
    TemplateZones z = templateZones(r);
    REQUIRE(z.passBands.size() == 2);
    REQUIRE(z.stopBands.size() == 1);
    CHECK(z.passBands[1][1] == 24000.0);
    CHECK_REL(z.passLow, std::pow(10.0, -0.5 / 20.0), 1e-15);
    CHECK(z.passHigh == 1.0);
    CHECK_REL(z.stopHigh, std::pow(10.0, -50.0 / 20.0), 1e-15);
    r.kind = FilterKind::Fir;
    z = templateZones(r);
    CHECK_REL(z.passHigh / z.passLow, std::pow(10.0, 0.5 / 20.0), 1e-14);
    CHECK_NEAR(z.passHigh + z.passLow, 2.0, 0.0, 1e-15);

    // Texts.
    CHECK(templateText(templateOf(dsp::FilterResponse::LowPass)) ==
          "Gabarit : au plus 0,5 dB d'atténuation jusqu'à 1 kHz, au moins 50 dB d'atténuation dès 1,6 kHz.");
    CHECK(templateText(r) == "Gabarit : au plus 0,5 dB d'ondulation (crête à crête) jusqu'à 1 kHz et dès 3 kHz, au moins "
                             "50 dB d'atténuation de 1,4 kHz à 2,6 kHz.");
    CHECK(filterShortText(r) == "coupe-bande 1,4 kHz à 2,6 kHz");
    CHECK(filterShortText(templateOf(dsp::FilterResponse::BandPass)) == "passe-bande 1 kHz à 3 kHz");
    FilterRecipe high = templateOf(dsp::FilterResponse::HighPass);
    high.family = dsp::FilterFamily::Elliptic;
    const FilterDesign d = designFilter(high);
    REQUIRE(d.ok());
    CHECK(filterText(d.recipe) == "Passe-haut elliptique d'ordre " + std::to_string(d.recipe.order) +
                                      ", gabarit 0,5 dB dès 1,6 kHz, 50 dB jusqu'à 1 kHz");

    // JSON: the template kept, bounded.
    const model::Json j = toJson(r);
    CHECK(j["fromTemplate"].toBool(false));
    CHECK(j["pass1"].toNumber(0.0) == 1000.0);
    CHECK(j["stop2"].toNumber(0.0) == 2600.0);
    CHECK(toJson(filterRecipeFromJson(j)) == j);
    model::Json wild = j;
    wild["stop1"] = -5.0;
    wild["fromTemplate"] = "oui";
    const FilterRecipe bounded = filterRecipeFromJson(wild);
    CHECK(bounded.stopEdge1Hz == 0.0);
    CHECK(!bounded.fromTemplate);
    // Coefficients typed: no template.
    FilterRecipe typed = r;
    typed.kind = FilterKind::Typed;
    typed.numerator = "0,5 ; 0,5";
    typed.denominator = "1";
    const FilterDesign t = designFilter(typed);
    REQUIRE(t.ok());
    CHECK(!contains(filterText(t.recipe), "gabarit"));
    CHECK(!t.templateHeld);
}

TEST_CASE("filters: texts, JSON, examples", "[analysis]")
{
    FilterRecipe r = defaultFilterRecipe(48000.0);
    CHECK(filterText(r) == "Passe-bas Butterworth d'ordre 4, 1 kHz");
    r.family = dsp::FilterFamily::ChebyshevI;
    r.response = dsp::FilterResponse::BandPass;
    r.frequency1Hz = 300.0;
    r.frequency2Hz = 3400.0;
    CHECK(filterText(r) == "Passe-bande Chebyshev I d'ordre 4 (1 dB), 300 Hz à 3,4 kHz");
    CHECK(filterShortText(r) == "passe-bande 300 Hz à 3,4 kHz");
    r.kind = FilterKind::Fir;
    r.window = dsp::Window::Kaiser;
    r.taps = 1001;
    CHECK(filterText(r) == "Passe-bande RIF de 1\xE2\x80\xAF" "001 coefficients (Kaiser, β = 8,6), 300 Hz à 3,4 kHz");
    CHECK(applicationText(FilterApplication::ZeroPhase) == "Aller-retour à phase nulle (gain au carré)");

    r.application = FilterApplication::Aligned;
    r.rateOfSignal = false;
    const FilterRecipe back = filterRecipeFromJson(toJson(r));
    CHECK(toJson(back) == toJson(r));
    CHECK(back.kind == FilterKind::Fir);
    CHECK(back.window == dsp::Window::Kaiser);
    CHECK(!back.rateOfSignal);
    CHECK(back.application == FilterApplication::Aligned);
    model::Json wild = toJson(r);
    wild["kind"] = "iir";
    wild["order"] = 500;
    wild["taps"] = 1;
    wild["family"] = "elliptique";
    wild["ripple"] = -3.0;
    const FilterRecipe bounded = filterRecipeFromJson(wild);
    CHECK(bounded.order == dsp::kMaximumIirOrder);
    CHECK(bounded.taps == 3);
    CHECK(bounded.family == dsp::FilterFamily::Butterworth); // unknown: the default
    CHECK(bounded.passbandRippleDb == 1e-3);
    CHECK(bounded.application == FilterApplication::Causal); // an IIR is not aligned
    CHECK(filterRecipeFromJson(model::Json()).order == 4);
    CHECK(defaultFilterRecipe(1000.0).frequency1Hz == 100.0);
    CHECK(defaultFilterRecipe(0.0).sampleRateHz == 48000.0);

    const auto &examples = filterExamples();
    CHECK(examples.size() >= 6);
    for (const FilterExample &e : examples)
    {
        CHECK(!e.name.empty() && !e.tip.empty());
        FilterRecipe x = e.recipe;
        x.sampleRateHz = 48000.0;
        const FilterDesign d = designFilter(x);
        CHECK(d.ok());
        CHECK(d.warnings.empty());
        CHECK(toJson(filterRecipeFromJson(toJson(x))) == toJson(x));
    }
}

TEST_CASE("filters: roots of a polynomial", "[analysis]")
{
    // (z − 0,5)(z + 2)(z² + 1) = z⁴ + 1,5 z³ + 0 z² + 1,5 z − 1 … in ascending powers.
    const std::vector<double> ascending = {-1.0, 1.5, 0.0, 1.5, 1.0};
    std::vector<std::complex<double>> roots = qacq::dsp::polynomialRoots(ascending);
    REQUIRE(roots.size() == 4);
    for (std::complex<double> expected : {std::complex<double>(0.5, 0.0), std::complex<double>(-2.0, 0.0),
                                          std::complex<double>(0.0, 1.0), std::complex<double>(0.0, -1.0)})
    {
        const auto nearest = std::min_element(roots.begin(), roots.end(), [&](auto a, auto b) {
            return std::abs(a - expected) < std::abs(b - expected);
        });
        CHECK(std::abs(*nearest - expected) < 1e-12);
    }
    CHECK(qacq::dsp::polynomialRoots(std::vector<double>{3.0}).empty());
}
