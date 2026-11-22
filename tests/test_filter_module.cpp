// The module "Filtres": a filter as a recipe, designed, what it does, run over a signal.
#include "qacq_test.h"

#include "qacq/analysis/filters.h"
#include "qacq/dsp/filter_design.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <string>

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
                              dsp::FilterFamily::ChebyshevII, dsp::FilterFamily::Bessel})
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
