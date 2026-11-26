#include "qacq_test.h"

#include "qacq/dsp/channel.h"
#include "qacq/dsp/constants.h"
#include "qacq/dsp/equalizer.h"
#include "qacq/dsp/random.h"
#include "qacq/dsp/receiver.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

using namespace qacq;
using namespace qacq::dsp;

namespace
{
using Complex = std::complex<double>;

bool contains(const std::string &text, const std::string &part)
{
    return text.find(part) != std::string::npos;
}

// Symbols drawn from a constellation, and what a receiver holds of them through a response and a noise.
struct Received
{
    std::vector<Complex> symbols, points;
};

Received through(Modulation modulation, double low, const SymbolTaps &response, double noiseVariance, std::size_t count, std::uint64_t seed)
{
    const std::vector<Complex> ideal = constellation(modulation, low);
    Random random(seed);
    Received r;
    r.symbols.resize(count);
    r.points.resize(count);
    for (Complex &s : r.symbols) s = ideal[static_cast<std::size_t>(random.uniform() * static_cast<double>(ideal.size())) % ideal.size()];
    const double sigma = std::sqrt(0.5 * noiseVariance);
    for (std::size_t k = 0; k < count; ++k)
    {
        Complex v{};
        for (int j = response.first; j <= response.last(); ++j)
        {
            const auto at = static_cast<std::ptrdiff_t>(k) - j;
            if (at >= 0 && at < static_cast<std::ptrdiff_t>(count)) v += response.at(j) * r.symbols[static_cast<std::size_t>(at)];
        }
        r.points[k] = v + sigma * Complex(random.normal(), random.normal());
    }
    return r;
}

// Mean square of points - symbols over the middle of the record, against the energy of a symbol.
double meanSquare(const std::vector<Complex> &points, const std::vector<Complex> &symbols, double energy, std::size_t from = 100)
{
    double sum = 0.0;
    std::size_t counted = 0;
    for (std::size_t k = from; k + 100 < symbols.size(); ++k, ++counted) sum += std::norm(points[k] - symbols[k]);
    return sum / static_cast<double>(counted) / energy;
}

double symbolErrorRate(Modulation modulation, double low, const std::vector<Complex> &points, const std::vector<Complex> &symbols, std::size_t from = 100)
{
    std::size_t wrong = 0, counted = 0;
    for (std::size_t k = from; k + 100 < symbols.size(); ++k, ++counted)
        wrong += decideSymbol(modulation, points[k], low) != decideSymbol(modulation, symbols[k], low) ? 1 : 0;
    return static_cast<double>(wrong) / static_cast<double>(counted);
}
} // namespace

TEST_CASE("equalizer: moments of a constellation, taps and their response", "[equalizer]")
{
    const SymbolMoments qpsk = symbolMoments(constellation(Modulation::Qpsk));
    CHECK(std::abs(qpsk.energy - 1.0) < 1e-12);
    CHECK(std::abs(qpsk.mean) < 1e-12);
    CHECK(!qpsk.realAxis);
    const SymbolMoments onOff = symbolMoments(constellation(Modulation::Ask, 0.0));
    CHECK(std::abs(onOff.energy - 0.5) < 1e-12);
    CHECK(std::abs(onOff.mean - Complex(0.5, 0.0)) < 1e-12);
    CHECK(onOff.realAxis);
    CHECK(symbolMoments(constellation(Modulation::Bpsk)).realAxis);
    CHECK(!symbolMoments(constellation(Modulation::Qam16)).realAxis);

    const SymbolTaps a{{1.0, 0.5}, 0}, b{{1.0, -0.5}, -1};
    const SymbolTaps c = convolveTaps(a, b);
    CHECK(c.first == -1);
    REQUIRE(c.values.size() == 3);
    CHECK(std::abs(c.at(-1) - 1.0) < 1e-15);
    CHECK(std::abs(c.at(0)) < 1e-15);
    CHECK(std::abs(c.at(1) + 0.25) < 1e-15);
    CHECK(c.last() == 1);
    CHECK(c.at(5) == Complex{});
    CHECK(c.at(-2) == Complex{});
    CHECK(convolveTaps(a, SymbolTaps{}).values.empty());
    // A response: 1 + 0.5 z^-1 at 0 (1.5), at half the symbol rate (0.5), at a quarter (1 - 0.5 j).
    CHECK(std::abs(tapsResponse(a, 0.0) - 1.5) < 1e-12);
    CHECK(std::abs(tapsResponse(a, 0.5) - 0.5) < 1e-12);
    CHECK(std::abs(tapsResponse(a, -0.5) - 0.5) < 1e-12);
    CHECK(std::abs(tapsResponse(a, 0.25) - Complex(1.0, -0.5)) < 1e-12);
    CHECK(std::abs(tapsResponse(SymbolTaps{{1.0}, 3}, 0.1) - std::polar(1.0, -kTwoPi * 0.3)) < 1e-12);
    // Taps applied: z[k] = sum_i w[i] x[k - i], nothing outside the record.
    const std::vector<Complex> x{1.0, 2.0, 3.0, 4.0};
    const std::vector<Complex> later = applyEqualizer(x, SymbolTaps{{1.0}, 1});
    CHECK((later == std::vector<Complex>{0.0, 1.0, 2.0, 3.0}));
    const std::vector<Complex> sooner = applyEqualizer(x, SymbolTaps{{Complex(0.0, 1.0)}, -1});
    CHECK(std::abs(sooner[0] - Complex(0.0, 2.0)) < 1e-15);
    CHECK(sooner[3] == Complex{});
    const std::vector<Complex> both = applyEqualizer(x, SymbolTaps{{1.0, 1.0, 1.0}, -1});
    CHECK((both == std::vector<Complex>{3.0, 6.0, 9.0, 7.0}));
}

TEST_CASE("equalizer: the response of a chain found by least squares", "[equalizer]")
{
    // Before and after the symbol itself, complex: found exactly without noise, closely with it.
    const SymbolTaps response{{Complex(0.2, 0.1), 1.0, Complex(-0.3, 0.3), 0.1}, -1};
    const Received clean = through(Modulation::Qpsk, 0.0, response, 0.0, 2000, 3);
    const SymbolTaps exact = fitSymbolResponse(clean.points, clean.symbols, -4, 6, 16, 1984);
    REQUIRE(exact.values.size() == 11);
    CHECK(exact.first == -4);
    for (int j = -4; j <= 6; ++j) CHECK(std::abs(exact.at(j) - response.at(j)) < 1e-12);
    const Received noisy = through(Modulation::Qam16, 0.0, response, 0.05, 4000, 4);
    const SymbolTaps close = fitSymbolResponse(noisy.points, noisy.symbols, -4, 6, 16, 3984);
    REQUIRE(close.values.size() == 11);
    for (int j = -4; j <= 6; ++j) CHECK(std::abs(close.at(j) - response.at(j)) < 0.02);
    // An on-off ASK (its symbols do not average to 0) all the same.
    const Received ask = through(Modulation::Ask, 0.0, SymbolTaps{{1.0, 0.4}, 0}, 0.0, 1000, 5);
    const SymbolTaps askFit = fitSymbolResponse(ask.points, ask.symbols, -2, 3, 8, 992);
    REQUIRE(askFit.values.size() == 6);
    CHECK(std::abs(askFit.at(0) - 1.0) < 1e-10);
    CHECK(std::abs(askFit.at(1) - 0.4) < 1e-10);
    CHECK(std::abs(askFit.at(-1)) < 1e-10);
    // Symbols that repeat every 4 cannot tell 11 taps apart; too few equations; a range upside down.
    Received periodic = clean;
    for (std::size_t k = 0; k < periodic.symbols.size(); ++k) periodic.symbols[k] = clean.symbols[k % 4];
    CHECK(fitSymbolResponse(periodic.points, periodic.symbols, -4, 6, 16, 1984).values.empty());
    CHECK(fitSymbolResponse(clean.points, clean.symbols, -4, 6, 16, 30).values.empty());
    CHECK(fitSymbolResponse(clean.points, clean.symbols, 3, 2, 16, 1984).values.empty());
}

TEST_CASE("equalizer: the Wiener taps, and what they leave as measured", "[equalizer]")
{
    struct Case
    {
        const char *name;
        Modulation modulation;
        double low;
        SymbolTaps response;
        double noise;
        int half;
    };
    const std::vector<Case> cases{
        {"QPSK, an echo at -6 dB", Modulation::Qpsk, 0.0, {{1.0, 0.5}, 0}, 0.1, 5},
        {"QPSK, before and after", Modulation::Qpsk, 0.0, {{Complex(0.2, 0.1), 1.0, Complex(-0.3, 0.3), 0.1}, -1}, 0.05, 7},
        {"16-QAM, an echo", Modulation::Qam16, 0.0, {{1.0, Complex(0.3, -0.2)}, 0}, 0.01, 7},
        {"8-PSK, two echoes", Modulation::Psk8, 0.0, {{1.0, Complex(0.0, 0.3), -0.2}, 0}, 0.02, 7},
        {"QPSK, two paths nearly equal", Modulation::Qpsk, 0.0, {{1.0, 0.9}, 0}, 0.05, 15},
    };
    for (const Case &c : cases)
    {
        const SymbolMoments moments = symbolMoments(constellation(c.modulation, c.low));
        const Received r = through(c.modulation, c.low, c.response, c.noise, 20000, 7);
        const LinearEqualizer bare = withoutEqualizer(c.response, c.noise, moments);
        const LinearEqualizer e = designLinearEqualizer(c.response, c.half, c.noise, moments);
        REQUIRE(bare.ok());
        REQUIRE(e.ok());
        CHECK(e.taps.first == -c.half);
        CHECK(e.taps.values.size() == static_cast<std::size_t>(2 * c.half + 1));
        CHECK(std::abs(e.combined.at(0) - 1.0) < 1e-12); // no bias: the points on the constellation
        CHECK(std::abs(bare.combined.at(0) - 1.0) < 1e-12);
        // What is left is what was announced (the mean square of the error against the energy of a symbol).
        const double before = meanSquare(applyEqualizer(r.points, bare.taps), r.symbols, moments.energy);
        const double after = meanSquare(applyEqualizer(r.points, e.taps), r.symbols, moments.energy);
        if (qacq::test::state().verbose)
            std::printf("      %-30s without %.4f (announced %.4f), with %.4f (announced %.4f: others %.4f, noise %.4f)\n", c.name, before,
                        1.0 / bare.sinr, after, 1.0 / e.sinr, e.interference, e.noise);
        CHECK_REL(before, 1.0 / bare.sinr, 0.05);
        CHECK_REL(after, 1.0 / e.sinr, 0.05);
        CHECK(std::abs(e.interference + e.noise - 1.0 / e.sinr) < 1e-12);
        CHECK(e.sinr > 1.5 * bare.sinr);
        CHECK(e.interference < 0.2 * bare.interference); // the other symbols nearly gone, for a noise that weighs about as much
    }
    // Without noise the response is inverted, as far as the taps reach: (1 + 0.5 z^-1)^-1 = sum (-0.5)^m z^-m.
    const SymbolMoments qpsk = symbolMoments(constellation(Modulation::Qpsk));
    const LinearEqualizer zero = designLinearEqualizer(SymbolTaps{{1.0, 0.5}, 0}, 15, 0.0, qpsk);
    REQUIRE(zero.ok());
    for (int m = 0; m <= 8; ++m) CHECK(std::abs(zero.taps.at(m) - std::pow(-0.5, m)) < 1e-3);
    for (int m = -15; m < 0; ++m) CHECK(std::abs(zero.taps.at(m)) < 1e-3);
    CHECK(zero.sinr > 1e6);
    CHECK(zero.noise == 0.0);
    for (int m = zero.combined.first; m <= zero.combined.last(); ++m)
        if (m != 0) CHECK(std::abs(zero.combined.at(m)) < 1e-3);
    // Nothing to repair: the taps are a single 1.
    const LinearEqualizer nothing = designLinearEqualizer(SymbolTaps{{1.0}, 0}, 3, 0.1, qpsk);
    REQUIRE(nothing.ok());
    CHECK(std::abs(nothing.taps.at(0) - 1.0) < 1e-12);
    for (int m : {-3, -2, -1, 1, 2, 3}) CHECK(std::abs(nothing.taps.at(m)) < 1e-12);
    CHECK_REL(nothing.sinr, 10.0, 1e-9);
    // Without equalizer the others weigh as their taps do: 0.25 for an echo of half the amplitude.
    const LinearEqualizer bare = withoutEqualizer(SymbolTaps{{2.0, 1.0}, 0}, 0.4, qpsk);
    CHECK_REL(bare.interference, 0.25, 1e-12);
    CHECK_REL(bare.noise, 0.1, 1e-12); // scaled as the symbol is: 0.4 / 2^2
    CHECK_REL(bare.sinr, 1.0 / 0.35, 1e-12);
    // What cannot be done is said.
    CHECK(!designLinearEqualizer(SymbolTaps{}, 5, 0.1, qpsk).ok());
    CHECK(!designLinearEqualizer(SymbolTaps{{1.0, 0.5}, 0}, -1, 0.1, qpsk).ok());
    CHECK(!designLinearEqualizer(SymbolTaps{{1.0, 0.5}, 0}, kMaximumEqualizerHalf + 1, 0.1, qpsk).ok());
    CHECK(!withoutEqualizer(SymbolTaps{}, 0.1, qpsk).ok());
    CHECK(!withoutEqualizer(SymbolTaps{{0.0, 1.0}, 0}, 0.1, qpsk).ok()); // nothing of the symbol at its own instant
}

TEST_CASE("equalizer: a constellation on the real axis is disturbed on that axis alone", "[equalizer]")
{
    // BPSK through an echo in phase, then through the same echo a quarter turn away: the second leaves its mark
    // across the decision, and counts for less. The error rate measured is that of the theory at the ratio announced
    // (BPSK: Q(sqrt(2 sinr))).
    const SymbolMoments moments = symbolMoments(constellation(Modulation::Bpsk));
    REQUIRE(moments.realAxis);
    const double noise = 0.2;
    double announced[2] = {0.0, 0.0};
    int at = 0;
    for (const Complex echo : {Complex(0.5, 0.0), Complex(0.0, 0.5)})
    {
        const SymbolTaps response{{1.0, echo}, 0};
        const LinearEqualizer e = designLinearEqualizer(response, 5, noise, moments);
        REQUIRE(e.ok());
        const Received r = through(Modulation::Bpsk, 0.0, response, noise, 60000, 11);
        const double measured = symbolErrorRate(Modulation::Bpsk, 0.0, applyEqualizer(r.points, e.taps), r.symbols);
        const double expected = gaussianQ(std::sqrt(2.0 * e.sinr));
        if (qacq::test::state().verbose)
            std::printf("      echo (%.1f, %.1f): ratio %.2f dB, errors %.5f, theory there %.5f\n", echo.real(), echo.imag(),
                        10.0 * std::log10(e.sinr), measured, expected);
        CHECK(measured > 0.7 * expected);
        CHECK(measured < 1.4 * expected);
        announced[at++] = e.sinr;
    }
    CHECK(announced[1] > 1.1 * announced[0]);
    // Without equalizer: an echo across the axis disturbs nothing but through the noise.
    const LinearEqualizer across = withoutEqualizer(SymbolTaps{{1.0, Complex(0.0, 0.5)}, 0}, noise, moments);
    CHECK(across.interference == 0.0);
    const LinearEqualizer along = withoutEqualizer(SymbolTaps{{1.0, 0.5}, 0}, noise, moments);
    CHECK_REL(along.interference, 0.5, 1e-12); // twice 0.25: all of it on the axis decided
}

TEST_CASE("equalizer: learnt on a preamble, then on its own decisions", "[equalizer]")
{
    const SymbolTaps response{{Complex(0.2, 0.1), 1.0, Complex(-0.3, 0.3), 0.1}, -1};
    const double noise = 0.05;
    const SymbolMoments moments = symbolMoments(constellation(Modulation::Qpsk));
    const Received r = through(Modulation::Qpsk, 0.0, response, noise, 12000, 21);
    const std::vector<Complex> preamble(r.symbols.begin(), r.symbols.begin() + 64);
    AdaptiveEqualizerSettings settings;
    settings.half = 7;
    settings.step = 0.05;
    const AdaptiveEqualization learnt = adaptEqualizer(r.points, preamble, Modulation::Qpsk, 0.0, settings);
    REQUIRE(learnt.ok());
    REQUIRE(learnt.points.size() == r.points.size());
    CHECK(learnt.squaredError.size() == r.points.size());
    CHECK(learnt.phases.empty());
    CHECK(!learnt.halfSymbolEarly);
    CHECK(learnt.taps.first == -7);
    CHECK(learnt.taps.values.size() == 15);
    // As good as the taps computed from the response, to 1 dB; far better than nothing.
    const LinearEqualizer wiener = designLinearEqualizer(response, 7, noise, moments);
    const double after = meanSquare(learnt.points, r.symbols, moments.energy, 2000);
    const double without = meanSquare(r.points, r.symbols, moments.energy, 2000);
    if (qacq::test::state().verbose) std::printf("      learnt %.4f, Wiener %.4f, without %.4f\n", after, 1.0 / wiener.sinr, without);
    CHECK(after < 1.26 / wiener.sinr);
    CHECK(after < 0.3 * without);
    CHECK(symbolErrorRate(Modulation::Qpsk, 0.0, learnt.points, r.symbols, 2000) < 0.001);
    // Its error at the end is the mean square measured.
    double settled = 0.0;
    for (std::size_t k = 9000; k < 11000; ++k) settled += learnt.squaredError[k];
    CHECK_REL(settled / 2000.0 / moments.energy, after, 0.2);
    // Taught by its first 6 000 points only (what comes after the end of a frame teaches nothing): the same up to
    // there, then the taps as they were, applied to the rest.
    settings.adaptSymbols = 6000;
    const AdaptiveEqualization part = adaptEqualizer(r.points, preamble, Modulation::Qpsk, 0.0, settings);
    settings.adaptSymbols = 0;
    REQUIRE(part.ok());
    REQUIRE(part.points.size() == r.points.size());
    bool same = true;
    for (std::size_t k = 0; k < 6000; ++k) same = same && part.points[k] == learnt.points[k];
    CHECK(same);
    const std::vector<Complex> frozen = applyEqualizer(r.points, part.taps);
    double apart = 0.0;
    for (std::size_t k = 6000; k < r.points.size(); ++k) apart = std::max(apart, std::abs(part.points[k] - frozen[k]));
    CHECK(apart < 1e-12);
    CHECK(part.taps.values != learnt.taps.values);
    CHECK(meanSquare(part.points, r.symbols, moments.energy, 7000) < 1.3 / wiener.sinr);
    // Without a preamble it starts from a single tap and learns from its decisions alone, when they are mostly right.
    const Received mild = through(Modulation::Qpsk, 0.0, SymbolTaps{{1.0, 0.3}, 0}, 0.02, 8000, 22);
    const AdaptiveEqualization blind = adaptEqualizer(mild.points, {}, Modulation::Qpsk, 0.0, settings);
    REQUIRE(blind.ok());
    CHECK(meanSquare(blind.points, mild.symbols, 1.0, 3000) < 0.5 * meanSquare(mild.points, mild.symbols, 1.0, 3000));

    // Points that still turn (a carrier a little off: 0.23 degree per symbol, 2 760 degrees over the record): lost
    // without a carrier loop after the taps, followed with one; its phase runs as the points turn.
    const double turn = 0.004;
    std::vector<Complex> turning(r.points.size());
    for (std::size_t k = 0; k < turning.size(); ++k) turning[k] = r.points[k] * std::polar(1.0, turn * static_cast<double>(k));
    const AdaptiveEqualization lost = adaptEqualizer(turning, preamble, Modulation::Qpsk, 0.0, settings);
    REQUIRE(lost.ok());
    CHECK(symbolErrorRate(Modulation::Qpsk, 0.0, lost.points, r.symbols, 2000) > 0.2);
    settings.carrierBandwidth = 0.003;
    const AdaptiveEqualization followed = adaptEqualizer(turning, preamble, Modulation::Qpsk, 0.0, settings);
    REQUIRE(followed.ok());
    REQUIRE(followed.phases.size() == turning.size());
    CHECK(symbolErrorRate(Modulation::Qpsk, 0.0, followed.points, r.symbols, 2000) < 0.002);
    CHECK(meanSquare(followed.points, r.symbols, moments.energy, 2000) < 1.4 / wiener.sinr);
    CHECK_REL((followed.phases[11000] - followed.phases[1000]) / 10000.0, turn, 0.01);
    // (The taps do not turn with the points: their strongest one stays real.)
    std::size_t strongest = 0;
    for (std::size_t m = 0; m < followed.taps.values.size(); ++m)
        if (std::abs(followed.taps.values[m]) > std::abs(followed.taps.values[strongest])) strongest = m;
    CHECK(std::abs(std::arg(followed.taps.values[strongest])) < 1e-9);

    // Points given twice, on the instants and half a symbol before them: the ones that explain the preamble.
    Random random(5);
    std::vector<Complex> useless(r.points.size());
    for (Complex &v : useless) v = Complex(random.normal(), random.normal());
    settings.carrierBandwidth = 0.0;
    const AdaptiveEqualization early = adaptEqualizer(useless, preamble, Modulation::Qpsk, 0.0, settings, r.points);
    REQUIRE(early.ok());
    CHECK(early.halfSymbolEarly);
    CHECK(symbolErrorRate(Modulation::Qpsk, 0.0, early.points, r.symbols, 2000) < 0.001);
    const AdaptiveEqualization onTime = adaptEqualizer(r.points, preamble, Modulation::Qpsk, 0.0, settings, useless);
    REQUIRE(onTime.ok());
    CHECK(!onTime.halfSymbolEarly);
    CHECK(symbolErrorRate(Modulation::Qpsk, 0.0, onTime.points, r.symbols, 2000) < 0.001);

    // What it cannot do.
    CHECK(!adaptEqualizer(r.points, preamble, Modulation::Fsk, 0.0, settings).ok());
    settings.step = 0.0;
    CHECK(!adaptEqualizer(r.points, preamble, Modulation::Qpsk, 0.0, settings).ok());
    settings.step = 0.05;
    settings.half = kMaximumEqualizerHalf + 1;
    CHECK(!adaptEqualizer(r.points, preamble, Modulation::Qpsk, 0.0, settings).ok());
}

// The mean square of points - symbols over [from, to), against the energy of a symbol.
double meanSquareOver(const std::vector<Complex> &points, const std::vector<Complex> &symbols, double energy, std::size_t from, std::size_t to)
{
    double sum = 0.0;
    for (std::size_t k = from; k < to; ++k) sum += std::norm(points[k] - symbols[k]);
    return sum / static_cast<double>(to - from) / energy;
}

TEST_CASE("equalizer: a quiet preamble, its first points cut short", "[equalizer]")
{
    // A 256-QAM with little noise (-37 dB) and no echo, the first points of its frame cut short as the start of a
    // transmission cuts their pulses (the first half its size): the taps learn from the whole points alone, with a
    // ridge as small as the noise the preamble shows, and start where they should; taught by every point with the
    // ridge of a noisy preamble (2 %), they start 2 % short and bent by the first point, and take hundreds of
    // symbols to get there.
    const double noise = 2e-4;
    const SymbolMoments moments = symbolMoments(constellation(Modulation::Qam256));
    Received r = through(Modulation::Qam256, 0.0, SymbolTaps{{1.0}, 0}, noise, 4000, 31);
    r.points[0] *= 0.5;
    r.points[1] *= 0.9;
    r.points[2] *= 0.98;
    const std::vector<Complex> preamble(r.symbols.begin(), r.symbols.begin() + 64);
    AdaptiveEqualizerSettings settings;
    settings.half = 5;
    settings.adaptFrom = 6;
    const AdaptiveEqualization whole = adaptEqualizer(r.points, preamble, Modulation::Qam256, 0.0, settings);
    settings.adaptFrom = 0;
    const AdaptiveEqualization every = adaptEqualizer(r.points, preamble, Modulation::Qam256, 0.0, settings);
    REQUIRE(whole.ok());
    REQUIRE(every.ok());
    const double started = meanSquareOver(whole.points, r.symbols, moments.energy, 64, 192);
    const double bent = meanSquareOver(every.points, r.symbols, moments.energy, 64, 192);
    if (qacq::test::state().verbose)
        for (const std::size_t to : {128, 192, 320, 640, 1280})
            std::printf("      after the preamble, to %zu: whole points %.1f dB, every point %.1f dB, noise %.1f dB\n", to,
                        10.0 * std::log10(meanSquareOver(whole.points, r.symbols, moments.energy, 64, to)),
                        10.0 * std::log10(meanSquareOver(every.points, r.symbols, moments.energy, 64, to)), 10.0 * std::log10(noise / moments.energy));
    CHECK(started < 1.4 * noise / moments.energy);
    CHECK(bent > 1.8 * started);
    CHECK(symbolErrorRate(Modulation::Qam256, 0.0, whole.points, r.symbols, 64) == 0.0);
    // (The points before are equalized all the same; the error on the first one is what was cut off.)
    CHECK(whole.points.size() == r.points.size());
    CHECK(std::abs(whole.points[0] - 0.5 * r.symbols[0]) < 0.05);

    // A noisy preamble (QPSK at -13 dB) hides the points cut short: every known symbol counts, the same taps whether
    // the first ones are said to be cut or not.
    const Received loud = through(Modulation::Qpsk, 0.0, SymbolTaps{{1.0, 0.3}, 0}, 0.05, 3000, 32);
    const std::vector<Complex> known(loud.symbols.begin(), loud.symbols.begin() + 64);
    settings.adaptFrom = 6;
    const AdaptiveEqualization a = adaptEqualizer(loud.points, known, Modulation::Qpsk, 0.0, settings);
    settings.adaptFrom = 0;
    const AdaptiveEqualization b = adaptEqualizer(loud.points, known, Modulation::Qpsk, 0.0, settings);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK(a.points == b.points);
    CHECK(a.taps.values == b.taps.values);
}

TEST_CASE("equalizer: a channel with several paths", "[equalizer][sync]")
{
    // A band-limited envelope (a few tones under 0.2 fs).
    const std::size_t n = 4000;
    std::vector<double> i(n), q(n);
    for (std::size_t k = 0; k < n; ++k)
    {
        const double t = static_cast<double>(k);
        i[k] = std::cos(kTwoPi * 0.013 * t) + 0.5 * std::sin(kTwoPi * 0.071 * t + 0.3);
        q[k] = 0.7 * std::sin(kTwoPi * 0.029 * t) - 0.4 * std::cos(kTwoPi * 0.153 * t);
    }
    ChannelSpec single;
    CHECK(single.singlePath());
    CHECK(single.identity());
    single.paths = {{0.0, 1.0, 0.0}};
    CHECK(single.singlePath());
    CHECK(single.identity());
    ChannelSpec echo;
    echo.paths = {{0.0, 0.8, 0.0}, {48.0, 0.5, 90.0}, {70.5, 0.3, -30.0}};
    CHECK(!echo.singlePath());
    CHECK(!echo.identity());
    const ChannelOutput y = applyChannel(i, q, echo);
    REQUIRE(y.ok());
    // As long as the transmission and its last path.
    CHECK(y.inPhase.size() == n + 71);
    // The sum of the paths taken one by one (each a delayed copy, scaled and turned).
    std::vector<Complex> sum(y.inPhase.size());
    for (const ChannelPath &p : echo.paths)
    {
        ChannelSpec one;
        one.delaySamples = p.delaySamples;
        const ChannelOutput copy = applyChannel(i, q, one);
        REQUIRE(copy.ok());
        const Complex gain = std::polar(p.gain, p.phaseDegrees * kPi / 180.0);
        for (std::size_t k = 0; k < std::min(sum.size(), copy.inPhase.size()); ++k) sum[k] += gain * Complex(copy.inPhase[k], copy.quadrature[k]);
    }
    double worst = 0.0;
    for (std::size_t k = 0; k < sum.size(); ++k) worst = std::max(worst, std::abs(Complex(y.inPhase[k], y.quadrature[k]) - sum[k]));
    CHECK(worst < 1e-12);
    // A path a whole number of samples late: those samples themselves.
    for (std::size_t k : {100u, 1000u, 3000u})
    {
        ChannelSpec whole;
        whole.paths = {{48.0, 0.5, 90.0}};
        const ChannelOutput late = applyChannel(i, q, whole);
        REQUIRE(late.ok());
        CHECK(std::abs(Complex(late.inPhase[k + 48], late.quadrature[k + 48]) - Complex(0.0, 0.5) * Complex(i[k], q[k])) < 1e-15);
        CHECK(late.inPhase[10] == 0.0);
    }
    // With the rest of the channel: delay, clock and carrier apply to every path alike.
    ChannelSpec moved = echo;
    moved.delaySamples = 12.25;
    moved.clockOffsetPpm = 300.0;
    moved.frequencyOffsetHz = 25.0;
    moved.phaseOffsetDegrees = 40.0;
    const ChannelOutput through = applyChannel(i, q, moved);
    REQUIRE(through.ok());
    ChannelSpec rest = moved;
    rest.paths.clear();
    std::vector<double> yi(y.inPhase), yq(y.quadrature);
    const ChannelOutput twice = applyChannel(yi, yq, rest); // the paths, then the rest: the same thing
    REQUIRE(twice.ok());
    worst = 0.0;
    for (std::size_t k = 200; k + 200 < std::min(through.inPhase.size(), twice.inPhase.size()); ++k)
        worst = std::max(worst, std::abs(Complex(through.inPhase[k], through.quadrature[k]) - Complex(twice.inPhase[k], twice.quadrature[k])));
    CHECK(worst < 2e-4); // (interpolated twice instead of once)
    // What is refused.
    ChannelSpec wrong;
    wrong.paths = {{-1.0, 1.0, 0.0}};
    CHECK(!applyChannel(i, q, wrong).ok());
    wrong.paths.assign(kMaximumChannelPaths + 1, ChannelPath{});
    CHECK(!applyChannel(i, q, wrong).ok());
}

TEST_CASE("equalizer: fading gains, as Clarke's model draws them", "[equalizer]")
{
    // J0, the autocorrelation of Clarke's model: (1 / pi) times the integral of cos(x sin t) over 0 ... pi (the
    // midpoint rule is exact to rounding for this periodic integrand).
    const auto besselJ0 = [](double x) {
        double sum = 0.0;
        for (int k = 0; k < 64; ++k) sum += std::cos(x * std::sin(kPi * (k + 0.5) / 64.0));
        return sum / 64.0;
    };
    const double fs = 1000.0, fd = 10.0;
    const std::size_t n = 200000; // 2 000 periods of fd
    for (ChannelFading::Kind kind : {ChannelFading::Kind::Rayleigh, ChannelFading::Kind::Rice})
    {
        const bool rice = kind == ChannelFading::Kind::Rice;
        ChannelFading f;
        f.kind = kind;
        f.dopplerHz = fd;
        f.riceK = 10.0;
        f.seed = 4;
        CHECK(f.active());
        FadingGains gains(f, fs, 2);
        CHECK(gains.active());
        CHECK(gains.paths() == 2);
        // Rice: the line of sight turns at fd times the cosine of its angle; Rayleigh has none.
        CHECK(std::abs(gains.lineOfSightDopplerHz()) <= fd);
        if (!rice) CHECK(gains.lineOfSightDopplerHz() == 0.0);
        std::vector<Complex> first(n), second(n);
        for (std::size_t k = 0; k < n; ++k)
        {
            first[k] = gains.at(0, static_cast<double>(k));
            second[k] = gains.at(1, static_cast<double>(k));
        }
        // Each path of mean power 1. Rayleigh: |g|^2 exponential, under -10 dB 1 - exp(-0.1) = 9.5 % of the time; the
        // first path of Rice (K = 10, its line of sight) hardly ever.
        for (const std::vector<Complex> *g : {&first, &second})
        {
            double power = 0.0;
            std::size_t below = 0;
            for (const Complex &v : *g)
            {
                power += std::norm(v);
                below += std::norm(v) < 0.1 ? 1 : 0;
            }
            const double fraction = static_cast<double>(below) / static_cast<double>(n);
            if (qacq::test::state().verbose)
                std::printf("      %s, path %d: power %.4f, under -10 dB %.4f\n", rice ? "Rice" : "Rayleigh", g == &first ? 1 : 2,
                            power / static_cast<double>(n), fraction);
            CHECK(std::abs(power / static_cast<double>(n) - 1.0) < 0.02);
            if (rice && g == &first)
                CHECK(fraction < 0.003);
            else
                CHECK(std::abs(fraction - (1.0 - std::exp(-0.1))) < 0.015);
        }
        // The scattered waves change as J0(2 pi fd tau) says: half as alike after 0.24 / fd, no longer at 0.38 / fd.
        const std::vector<Complex> &scattered = rice ? second : first;
        const auto correlation = [&](std::size_t lag) {
            Complex sum{};
            for (std::size_t k = 0; k + lag < n; ++k) sum += scattered[k + lag] * std::conj(scattered[k]);
            return sum / static_cast<double>(n - lag);
        };
        const double zero = correlation(0).real();
        for (double x : {1.0, 2.404825557695773, 4.0})
        {
            const auto lag = static_cast<std::size_t>(std::lround(x / (kTwoPi * fd) * fs));
            const double expected = besselJ0(kTwoPi * fd * static_cast<double>(lag) / fs);
            CHECK(std::abs(correlation(lag).real() / zero - expected) < 0.01);
        }
        // Drawn again with the same seed: the same gains; going back starts again from the beginning.
        FadingGains again(f, fs, 2);
        CHECK(again.at(1, 777.25) == gains.at(1, 777.25));
        CHECK(again.at(0, 150000.0) == first[150000]);
        CHECK(again.at(0, 10.0) == first[10]);
        // Another seed: other gains.
        f.seed = 5;
        FadingGains other(f, fs, 2);
        CHECK(std::abs(other.at(0, 1000.0) - first[1000]) > 1e-6);
    }
    // fd = 0: each gain drawn once, then still.
    ChannelFading still;
    still.kind = ChannelFading::Kind::Rayleigh;
    still.dopplerHz = 0.0;
    FadingGains fixed(still, fs, 1);
    const Complex g0 = fixed.at(0, 0.0);
    CHECK(std::abs(g0) > 0.0);
    CHECK(std::abs(fixed.at(0, 1e7) - g0) < 1e-12);
    // No fading: 1, whatever is asked.
    FadingGains none(ChannelFading{}, fs, 2);
    CHECK(!none.active());
    CHECK(none.at(1, 123.0) == Complex(1.0, 0.0));
    CHECK(none.at(5, 0.0) == Complex(1.0, 0.0));
}

TEST_CASE("equalizer: a channel that fades, each path with its own gain", "[equalizer]")
{
    const std::size_t n = 6000;
    std::vector<double> i(n), q(n);
    for (std::size_t k = 0; k < n; ++k)
    {
        const double t = static_cast<double>(k);
        i[k] = std::cos(kTwoPi * 0.013 * t) + 0.5 * std::sin(kTwoPi * 0.071 * t + 0.3);
        q[k] = 0.7 * std::sin(kTwoPi * 0.029 * t) - 0.4 * std::cos(kTwoPi * 0.153 * t);
    }
    ChannelSpec spec;
    spec.sampleRateHz = 8000.0;
    spec.fading.kind = ChannelFading::Kind::Rayleigh;
    spec.fading.dopplerHz = 5.0;
    spec.fading.seed = 9;
    CHECK(spec.singlePath());
    CHECK(!spec.identity());
    // A single path: the transmission times its gain, sample by sample.
    const ChannelOutput y = applyChannel(i, q, spec);
    REQUIRE(y.ok());
    REQUIRE(y.inPhase.size() == n);
    FadingGains gains(spec.fading, spec.sampleRateHz, 1);
    double worst = 0.0;
    for (std::size_t k = 0; k < n; ++k)
        worst = std::max(worst, std::abs(Complex(y.inPhase[k], y.quadrature[k]) - gains.at(0, static_cast<double>(k)) * Complex(i[k], q[k])));
    CHECK(worst < 1e-12);
    // Several paths: each its own gain (the first path the first one), on top of its own amplitude and phase.
    spec.paths = {{0.0, 0.8, 0.0}, {24.0, 0.5, 90.0}};
    const ChannelOutput two = applyChannel(i, q, spec);
    REQUIRE(two.ok());
    REQUIRE(two.inPhase.size() == n + 24);
    FadingGains both(spec.fading, spec.sampleRateHz, 2);
    worst = 0.0;
    for (std::size_t k = 0; k < two.inPhase.size(); ++k)
    {
        Complex expected{};
        if (k < n) expected += 0.8 * both.at(0, static_cast<double>(k)) * Complex(i[k], q[k]);
        if (k >= 24) expected += Complex(0.0, 0.5) * both.at(1, static_cast<double>(k)) * Complex(i[k - 24], q[k - 24]);
        worst = std::max(worst, std::abs(Complex(two.inPhase[k], two.quadrature[k]) - expected));
    }
    CHECK(worst < 1e-12);
    // Rice with a strong line of sight: the first path barely fades, the second (Rayleigh) does.
    spec.fading.kind = ChannelFading::Kind::Rice;
    spec.fading.riceK = 1e4;
    FadingGains strong(spec.fading, spec.sampleRateHz, 2);
    double low = 10.0, lowSecond = 10.0;
    for (std::size_t k = 0; k < 200000; k += 7)
    {
        low = std::min(low, std::abs(strong.at(0, static_cast<double>(k))));
        lowSecond = std::min(lowSecond, std::abs(strong.at(1, static_cast<double>(k))));
    }
    CHECK(low > 0.95);
    CHECK(lowSecond < 0.3);
    // What is refused: a Doppler shift beyond fs / 2, a factor K below 0, a number that is none.
    for (double doppler : {-1.0, 4001.0, std::nan("")})
    {
        ChannelSpec wrong = spec;
        wrong.fading.dopplerHz = doppler;
        CHECK(contains(applyChannel(i, q, wrong).error, "Doppler"));
    }
    ChannelSpec negative = spec;
    negative.fading.riceK = -1.0;
    CHECK(!applyChannel(i, q, negative).ok());
}

TEST_CASE("equalizer: decision feedback, computed and learnt", "[equalizer]")
{
    const SymbolMoments qpsk = symbolMoments(constellation(Modulation::Qpsk));
    // An echo after the symbol and hardly any noise: the forward taps leave it alone (a single 1), the first
    // feedback tap takes it off.
    const LinearEqualizer clean = designDecisionFeedbackEqualizer(SymbolTaps{{1.0, Complex(0.5, 0.2)}, 0}, 5, 4, 1e-9, qpsk);
    REQUIRE(clean.ok());
    REQUIRE(clean.feedback.size() == 4);
    for (int m = -5; m <= 5; ++m) CHECK(std::abs(clean.taps.at(m) - (m == 0 ? 1.0 : 0.0)) < 1e-6);
    CHECK(std::abs(clean.feedback[0] - Complex(0.5, 0.2)) < 1e-6);
    for (std::size_t j = 1; j < 4; ++j) CHECK(std::abs(clean.feedback[j]) < 1e-6);
    CHECK(clean.sinr > 1e6);
    CHECK(std::abs(clean.combined.at(0) - 1.0) < 1e-12);
    CHECK(std::abs(clean.combined.at(1) - clean.feedback[0]) < 1e-12); // what the feedback takes off

    // Two paths nearly equal: a deep notch in the band. The linear equalizer fills it and lets the noise through; the
    // feedback takes the echo off instead: 4 dB better, as measured on symbols drawn through it.
    const SymbolTaps notch{{1.0, 0.89}, 0};
    const double noise = 0.05;
    const LinearEqualizer linear = designLinearEqualizer(notch, 15, noise, qpsk);
    const LinearEqualizer dfe = designDecisionFeedbackEqualizer(notch, 15, 4, noise, qpsk);
    REQUIRE(linear.ok());
    REQUIRE(dfe.ok());
    CHECK(linear.feedback.empty());
    CHECK(dfe.feedback.size() == 4);
    CHECK(dfe.sinr > 2.3 * linear.sinr);
    CHECK(std::abs(dfe.combined.at(0) - 1.0) < 1e-12);
    for (std::size_t j = 0; j < 4; ++j) CHECK(std::abs(dfe.feedback[j] - dfe.combined.at(static_cast<int>(j) + 1)) < 1e-12);
    CHECK(std::abs(dfe.interference + dfe.noise - 1.0 / dfe.sinr) < 1e-12);
    const Received r = through(Modulation::Qpsk, 0.0, notch, noise, 20000, 31);
    const std::vector<Complex> fed = applyDecisionFeedback(r.points, dfe.taps, dfe.feedback, Modulation::Qpsk, 0.0);
    const std::vector<Complex> filled = applyEqualizer(r.points, linear.taps);
    REQUIRE(fed.size() == r.points.size());
    if (qacq::test::state().verbose)
        std::printf("      notch: linear %.4f (announced %.4f), decision feedback %.4f (announced %.4f)\n",
                    meanSquare(filled, r.symbols, 1.0), 1.0 / linear.sinr, meanSquare(fed, r.symbols, 1.0), 1.0 / dfe.sinr);
    CHECK_REL(meanSquare(fed, r.symbols, 1.0), 1.0 / dfe.sinr, 0.05);
    CHECK_REL(meanSquare(filled, r.symbols, 1.0), 1.0 / linear.sinr, 0.05);
    CHECK(symbolErrorRate(Modulation::Qpsk, 0.0, fed, r.symbols) < 0.0005);
    CHECK(symbolErrorRate(Modulation::Qpsk, 0.0, filled, r.symbols) > 0.001);
    // Without feedback taps: the linear equalizer (the forward taps alone).
    const std::vector<Complex> alone = applyDecisionFeedback(r.points, linear.taps, {}, Modulation::Qpsk, 0.0);
    double apart = 0.0;
    for (std::size_t k = 0; k < alone.size(); ++k) apart = std::max(apart, std::abs(alone[k] - filled[k]));
    CHECK(apart < 1e-12);

    // Learnt over a preamble then on its own decisions: near what is computed from the channel, far better than
    // the linear equalizer learnt the same way.
    const std::vector<Complex> preamble(r.symbols.begin(), r.symbols.begin() + 64);
    AdaptiveEqualizerSettings settings;
    settings.half = 15;
    settings.step = 0.02;
    settings.feedback = 4;
    const AdaptiveEqualization learnt = adaptEqualizer(r.points, preamble, Modulation::Qpsk, 0.0, settings);
    REQUIRE(learnt.ok());
    REQUIRE(learnt.points.size() == r.points.size());
    CHECK(learnt.feedback.size() == 4);
    settings.feedback = 0;
    const AdaptiveEqualization learntLinear = adaptEqualizer(r.points, preamble, Modulation::Qpsk, 0.0, settings);
    REQUIRE(learntLinear.ok());
    CHECK(learntLinear.feedback.empty());
    const double after = meanSquare(learnt.points, r.symbols, 1.0, 2000);
    if (qacq::test::state().verbose)
        std::printf("      learnt: decision feedback %.4f (first feedback tap %.3f), linear %.4f\n", after, std::abs(learnt.feedback[0]),
                    meanSquare(learntLinear.points, r.symbols, 1.0, 2000));
    CHECK(after < 1.3 / dfe.sinr);
    CHECK(after < 0.6 * meanSquare(learntLinear.points, r.symbols, 1.0, 2000));
    CHECK(std::abs(learnt.feedback[0]) > 0.3); // it found the echo
    CHECK(symbolErrorRate(Modulation::Qpsk, 0.0, learnt.points, r.symbols, 2000) < 0.0005);
    // What cannot be done is said.
    CHECK(!designDecisionFeedbackEqualizer(notch, 15, 0, noise, qpsk).ok());
    CHECK(contains(designDecisionFeedbackEqualizer(notch, 15, kMaximumFeedbackTaps + 1, noise, qpsk).error, "prises de retour"));
    CHECK(!designDecisionFeedbackEqualizer(SymbolTaps{}, 15, 4, noise, qpsk).ok());
}
