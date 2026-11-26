#include "qacq_test.h"

#include "qacq/analysis/link.h"
#include "qacq/dsp/constants.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>

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
    return std::any_of(lines.begin(), lines.end(), [&](const std::string &l) { return contains(l, part); });
}

// The errors counted are binomial: 5 standard deviations, and a margin of the model.
void checkAgainstTheory(const LinkResult &r, const char *label, double margin = 0.05)
{
    const double expected = r.theoryBer * static_cast<double>(r.comparedBits);
    const double sigma = std::sqrt(expected * (1.0 - r.theoryBer));
    if (qacq::test::state().verbose)
        std::printf("      %-36s BER %.3e  theory %.3e  (%zu errors / %zu bits)\n", label, r.ber, r.theoryBer, r.bitErrors,
                    r.comparedBits);
    CHECK(r.comparedBits > 0);
    CHECK(std::abs(static_cast<double>(r.bitErrors) - expected) <= 5.0 * sigma + margin * expected);
}
} // namespace

TEST_CASE("link: every modulation and example, end to end", "[link]")
{
    // Without noise, every example is received without an error; with its noise, it says what it measured.
    for (const LinkExample &e : linkExamples())
    {
        LinkRecipe quiet = e.recipe;
        quiet.noise = false;
        const LinkResult clean = simulateLink(quiet);
        REQUIRE(clean.ok());
        CHECK(clean.bitErrors == 0);
        CHECK(clean.comparedBits > 1000);
        CHECK(std::isnan(clean.theoryBer));
        const LinkResult noisy = simulateLink(e.recipe);
        REQUIRE(noisy.ok());
        if (e.recipe.noise) CHECK(std::abs(noisy.measuredEbN0Db - e.recipe.ebN0Db) < 0.1);
        if (e.recipe.noise && e.recipe.fading == LinkFading::None)
        {
            // Without fading the errors are independent: Wilson's interval as it is.
            double lower = 0.0, upper = 0.0;
            binomialInterval(noisy.bitErrors, noisy.comparedBits, lower, upper);
            CHECK(noisy.berBatches == 0);
            CHECK(noisy.berLower == lower);
            CHECK(noisy.berUpper == upper);
        }
        const bool fsk = e.recipe.modulation == dsp::Modulation::Fsk;
        CHECK(noisy.constellation.size() == (fsk ? 2 : std::size_t{1} << dsp::bitsPerSymbol(e.recipe.modulation)));
        // The eye: the matched output at every sample, and where each symbol is decided (not for FSK).
        CHECK(noisy.reception.matchedI.empty() == fsk);
        // The examples: a channel for the synchronized receiver to find, or several paths.
        const bool echoes = linkHasPaths(e.recipe);
        const bool fades = e.recipe.fading != LinkFading::None;
        const bool equalized = e.recipe.equalizer != LinkEqualizer::None;
        const std::size_t edge = (e.recipe.pulse == dsp::PulseShape::RootRaisedCosine ? 6u : 0u);
        CHECK(linkHasChannel(e.recipe) == (e.recipe.receiver == LinkReceiver::Synchronized || echoes || fades));
        CHECK(noisy.equalization.paths == echoes);
        CHECK(noisy.equalization.used == equalized);
        CHECK(noisy.skipEndSymbols == edge + (equalized ? (e.recipe.equalizerTaps - 1) / 2 : 0));
        if (equalized) CHECK(noisy.equalization.pointsBefore.size() == noisy.reception.decisionPoints.size());
        if (e.recipe.receiver == LinkReceiver::Synchronized)
        {
            // The synchronized receiver works at its own rate (8 samples per symbol), decides a few symbols after
            // the end of the frame too, and says what it found: without noise, the channel of the recipe.
            CHECK(noisy.reception.decisionPoints.size() >= noisy.sentSymbols.size() - 1);
            CHECK(noisy.reception.decisionTimes.size() == noisy.reception.decisionPoints.size());
            CHECK(noisy.reception.matchedRateHz == 8.0 * e.recipe.symbolRate);
            CHECK(noisy.skipSymbols == e.recipe.preambleSymbols);
            CHECK(noisy.receivedPassband.size() > noisy.sentPassband.size());
            for (const LinkResult *r : {&clean, &noisy})
            {
                const LinkSync &y = r->sync;
                CHECK(y.used);
                CHECK(y.frameFound);
                CHECK(y.timingError.size() == r->sentSymbols.size());
                CHECK(y.phaseError.size() == y.timingError.size());
                CHECK(y.rawPoints.size() == r->reception.decisionPoints.size());
                CHECK(y.trueDelaySymbols == e.recipe.delaySymbols); // (the examples: the strongest path first)
                CHECK(y.trueFrequencyOffsetHz == e.recipe.frequencyOffsetHz);
                CHECK(y.trueClockOffsetPpm == e.recipe.clockOffsetPpm);
                CHECK(!y.timingLost);
                // (Through an echo the frame, the clock and the level are found a little off: what the equalizer takes up.
                // Fading: the level is that of the gain over the preamble; the line of sight turns at its Doppler shift.)
                CHECK(std::abs(y.delaySymbols - e.recipe.delaySymbols) < (echoes ? 0.15 : 0.05));
                CHECK(std::abs(y.frequencyOffsetHz - e.recipe.frequencyOffsetHz - y.lineOfSightDopplerHz) < 0.5);
                CHECK(std::abs(y.lineOfSightDopplerHz) <= (fades ? e.recipe.dopplerHz : 0.0));
                CHECK(std::abs(y.clockOffsetPpm - e.recipe.clockOffsetPpm) < (echoes ? 40.0 : 10.0));
                CHECK(std::abs(y.gain - 1.0) < (echoes || fades ? 0.4 : 0.15));
            }
            CHECK(clean.sync.frameQuality > (echoes ? 0.85 : 0.99));
            CHECK(clean.sync.phaseSlips == 0);
            CHECK(clean.sync.timingErrorRms < (echoes ? 0.1 : 0.03));
            // (Fading: the carrier loop follows the phase of the gain as it wanders, a few degrees behind.)
            CHECK(clean.sync.phaseErrorRms < (fades ? 10.0 : echoes ? 2.0 : 0.3));
            CHECK(std::abs(std::remainder(clean.sync.phaseDegrees - clean.sync.truePhaseDegrees, 360.0)) < (fades ? 5.0 : echoes ? 3.0 : 0.2));
            CHECK(std::abs(clean.sync.frequencyOffsetHz - e.recipe.frequencyOffsetHz - clean.sync.lineOfSightDopplerHz) <
                  (echoes || fades ? 0.1 : 0.01));
            CHECK(anyContains(linkSummary(noisy), "Récepteur synchronisé : préambule de 64 symboles connus"));
            CHECK(anyContains(linkSummary(noisy), "Après le préambule : erreur de rythme "));
            continue;
        }
        CHECK(!noisy.sync.used);
        CHECK(noisy.sync.timingError.empty());
        CHECK(noisy.reception.matchedRateHz == e.recipe.sampleRateHz);
        CHECK(noisy.reception.decisionPoints.size() == noisy.sentSymbols.size());
        if (!fsk)
        {
            REQUIRE(noisy.reception.decisionTimes.size() == noisy.reception.decisionPoints.size());
            // At its instant, the matched output (without noise) is the decision point: the same sum for the root raised
            // cosine; rectangular pulses, the mean over a symbol of the baseband freed of its image, whose transitions
            // the image filter rounds (the decision integrates the raw output): within 0,06.
            // (With an equalizer: the point before it; the eye is what goes into it. Fading: the receiver divides by
            // the modulus of the gain at the decision instant, the eye shows what the matched filter gives: the same
            // phase, the point the larger.)
            const double within = e.recipe.pulse == dsp::PulseShape::RootRaisedCosine ? 1e-12 : 0.06;
            const std::vector<std::complex<double>> &points = equalized ? clean.equalization.pointsBefore : clean.reception.decisionPoints;
            for (std::size_t k = 10; k + 10 < clean.reception.decisionTimes.size(); k += 97)
            {
                const auto at = static_cast<std::size_t>(clean.reception.decisionTimes[k]);
                const std::complex<double> matched(clean.reception.matchedI[at], clean.reception.matchedQ[at]);
                if (fades)
                {
                    CHECK(std::abs(std::arg(matched / points[k])) < 1e-9);
                    CHECK(std::abs(matched) < std::abs(points[k]) * 4.0);
                    continue;
                }
                CHECK(std::abs(matched.real() - points[k].real()) < within);
                CHECK(std::abs(matched.imag() - points[k].imag()) < within);
            }
        }
        CHECK(!linkSummary(noisy).empty());
    }
    // Each modulation, on a carrier and as I/Q, rectangular and root raised cosine.
    for (dsp::Modulation m : {dsp::Modulation::Ask, dsp::Modulation::Fsk, dsp::Modulation::Bpsk, dsp::Modulation::Qpsk,
                              dsp::Modulation::Psk8, dsp::Modulation::Qam16, dsp::Modulation::Qam64, dsp::Modulation::Qam256})
        for (dsp::Representation rep : {dsp::Representation::RealPassband, dsp::Representation::ComplexBaseband})
            for (dsp::PulseShape pulse : {dsp::PulseShape::Rectangular, dsp::PulseShape::RootRaisedCosine})
            {
                LinkRecipe r = defaultLinkRecipe();
                r.modulation = m;
                r.representation = rep;
                r.pulse = pulse;
                r.noise = false;
                r.symbols = 600;
                r.carrierPhaseDegrees = 30.0;
                const LinkResult res = simulateLink(r);
                REQUIRE(res.ok());
                CHECK(res.bitErrors == 0);
                if (res.bitErrors != 0)
                    std::printf("      %s: %zu errors\n", linkText(r).c_str(), res.bitErrors);
                if (dsp::hasConstellation(m)) CHECK(res.evmPercent < 1.0);
                CHECK(res.skipSymbols == (pulse == dsp::PulseShape::RootRaisedCosine && m != dsp::Modulation::Fsk ? 6u : 0u));
            }
}

TEST_CASE("link: the synchronized examples, what each one shows", "[link]")
{
    const auto named = [](const char *part) {
        for (const LinkExample &e : linkExamples())
            if (contains(e.name, part)) return e.recipe;
        return LinkRecipe{};
    };
    CHECK(linkExamples().size() == 21);
    // The examples before keep their place (recipes saved and tests point at them).
    CHECK(linkExamples()[6].recipe.modulation == dsp::Modulation::Ask);
    CHECK(linkExamples()[8].name == "QPSK sans bruit : un texte");
    for (std::size_t k = 0; k < 9; ++k) CHECK(linkExamples()[k].recipe.receiver == LinkReceiver::Ideal);
    for (std::size_t k = 9; k < 13; ++k) CHECK(linkExamples()[k].recipe.receiver == LinkReceiver::Synchronized);
    for (std::size_t k = 0; k < 13; ++k) CHECK(!linkHasPaths(linkExamples()[k].recipe));
    for (std::size_t k = 13; k < 17; ++k) CHECK(linkHasPaths(linkExamples()[k].recipe));
    for (std::size_t k = 0; k < 17; ++k) CHECK(linkExamples()[k].recipe.fading == LinkFading::None);
    // Then fading, and the decision feedback equalizer.
    CHECK(linkExamples()[17].recipe.fading == LinkFading::Rayleigh);
    CHECK(linkExamples()[18].recipe.fading == LinkFading::Rice);
    CHECK(linkExamples()[19].recipe.equalizer == LinkEqualizer::DecisionFeedback);
    CHECK(linkHasPaths(linkExamples()[19].recipe));
    CHECK(linkExamples()[20].recipe.receiver == LinkReceiver::Synchronized);
    CHECK(linkExamples()[20].recipe.fading == LinkFading::Rice);

    // QPSK through an offset carrier and clock: found, and as few errors as the theory says (none in 7 860 bits).
    LinkRecipe r = named("QPSK synchronisée");
    REQUIRE(r.receiver == LinkReceiver::Synchronized);
    CHECK(r.frequencyOffsetHz == 40.0);
    CHECK(r.clockOffsetPpm == 200.0);
    LinkResult res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(res.comparedBits == 7860); // 4 000 symbols less the preamble (64) and the edge (6), 2 bits each
    CHECK(res.ber < 1e-3);
    CHECK(res.sync.phaseSlips == 0);
    CHECK(res.sync.timingErrorRms < 0.02);
    CHECK(res.sync.phaseErrorRms < 2.0);
    CHECK(anyContains(linkSummary(res), "Canal : porteuse décalée de 40 Hz, tournée de 70°, retard de 5,3 symboles, horloge du récepteur +200 ppm."));
    CHECK(anyContains(linkSummary(res), "Trame trouvée à 5,30 symboles (vrai 5,3, corrélation 9"));
    CHECK(anyContains(linkSummary(res), "aucun glissement de phase"));
    CHECK(anyContains(linkSummary(res), "Le préambule (64 symboles) et les 6 derniers symboles ne comptent pas."));
    // Before synchronization, the points turn (40 Hz at 1 kBd: a turn every 25 symbols) and the eye closes as the
    // clock drifts: far from the points decided.
    {
        double before = 0.0, after = 0.0;
        std::size_t counted = 0;
        for (std::size_t k = res.skipSymbols; k + res.skipEndSymbols < res.sentSymbols.size(); ++k, ++counted)
        {
            const std::complex<double> sent = res.constellation[res.sentSymbols[k]];
            before += std::norm(res.sync.rawPoints[k] / res.sync.gain - sent);
            after += std::norm(res.reception.decisionPoints[k] - sent);
        }
        REQUIRE(counted == 3930);
        CHECK(std::sqrt(before / static_cast<double>(counted)) > 1.0); // of the order of the distance between two points
        CHECK(std::sqrt(after / static_cast<double>(counted)) < 0.3);
    }

    // 16-QAM arriving 37.4 symbols later, turned by -120 degrees.
    r = named("16-QAM synchronisée");
    REQUIRE(r.modulation == dsp::Modulation::Qam16);
    res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(res.ber < 1e-3);
    CHECK(std::abs(res.sync.delaySymbols - 37.4) < 0.03);
    CHECK(std::abs(std::remainder(res.sync.phaseDegrees + 120.0, 360.0)) < 1.0);
    CHECK(res.sync.phaseSlips == 0);
    CHECK(anyContains(linkSummary(res), "Canal : tournée de −120°, retard de 37,4 symboles."));

    // The carrier loop too wide for 0 dB: it slips by a quarter turn, and what follows is decided a quadrant away;
    // the same link with the narrow loop holds, at the theory.
    r = named("la boucle de porteuse glisse");
    REQUIRE(r.receiver == LinkReceiver::Synchronized);
    CHECK(r.phaseBandwidth == 0.01);
    CHECK(r.ebN0Db == 0.0);
    res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(res.sync.phaseSlips >= 1);
    CHECK(res.ber > 2.0 * res.theoryBer);
    CHECK(res.sync.phaseErrorRms > 20.0);
    CHECK(anyContains(linkSummary(res), "glissement"));
    CHECK(!anyContains(linkSummary(res), "aucun glissement de phase"));
    r.phaseBandwidth = 0.003;
    res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(res.sync.phaseSlips == 0);
    CHECK(res.sync.phaseErrorRms < 10.0);
    checkAgainstTheory(res, "QPSK 0 dB, synchronized, narrow loop", 0.15);

    // ASK: its carrier line gives the frequency; no symmetry to slip to; rectangular pulses, no edge at the end.
    r = named("ASK synchronisée");
    REQUIRE(r.modulation == dsp::Modulation::Ask);
    res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(res.ber < 1e-2);
    CHECK(res.skipEndSymbols == 0);
    CHECK(res.sync.phaseSlips == 0);
    CHECK(std::abs(res.sync.frequencyOffsetHz - 25.0) < 0.1);
    CHECK(anyContains(linkSummary(res), "Le préambule (64 symboles) ne compte pas."));
    CHECK(dsp::rotationalSymmetryDegrees(dsp::Modulation::Ask) == 0.0);
    CHECK(dsp::rotationalSymmetryDegrees(dsp::Modulation::Fsk) == 0.0);
    CHECK(dsp::rotationalSymmetryDegrees(dsp::Modulation::Bpsk) == 180.0);
    CHECK(dsp::rotationalSymmetryDegrees(dsp::Modulation::Qpsk) == 90.0);
    CHECK(dsp::rotationalSymmetryDegrees(dsp::Modulation::Psk8) == 45.0);
    CHECK(dsp::rotationalSymmetryDegrees(dsp::Modulation::Qam256) == 90.0);

    // A delay under two symbols is read in the singular.
    r = named("QPSK synchronisée");
    r.delaySymbols = 1.25;
    r.noise = false;
    res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(anyContains(linkSummary(res), "retard de 1,25 symbole, horloge"));
    CHECK(anyContains(linkSummary(res), "Trame trouvée à 1,2"));
    CHECK(anyContains(linkSummary(res), " symbole (vrai 1,25, corrélation 100 %)."));
}

TEST_CASE("link: several paths, as typed and as the decisions see them", "[link]")
{
    // Typed: delay in symbols, gain in dB, a phase after "@"; read back as written.
    std::string error = "x";
    std::vector<LinkPath> paths = parseLinkPaths("0:0; 1 : -6 dB ; 2,5 symb. : \xE2\x88\x92" "12 @ 90\xC2\xB0\n 3 : 1.5", &error);
    CHECK(error.empty());
    REQUIRE(paths.size() == 4);
    CHECK((paths[0] == LinkPath{0.0, 0.0, 0.0}));
    CHECK((paths[1] == LinkPath{1.0, -6.0, 0.0}));
    CHECK((paths[2] == LinkPath{2.5, -12.0, 90.0}));
    CHECK((paths[3] == LinkPath{3.0, 1.5, 0.0}));
    CHECK(linkPathsText(paths) == "0 : 0 ; 1 : \xE2\x88\x92" "6 ; 2,5 : \xE2\x88\x92" "12 @ 90 ; 3 : 1,5");
    CHECK((parseLinkPaths(linkPathsText(paths)) == paths));
    CHECK(parseLinkPaths("", &error).empty());
    CHECK(error.empty());
    CHECK(parseLinkPaths(" ; \n ;", &error).empty());
    CHECK(error.empty());
    CHECK(parseLinkPaths("0 : 0 ; 1", &error).empty());
    CHECK(contains(error, "Trajet 2"));
    CHECK(contains(error, "retard : gain"));
    CHECK(parseLinkPaths("a : 0", &error).empty());
    CHECK(contains(error, "retard illisible"));
    CHECK(parseLinkPaths("1 : b", &error).empty());
    CHECK(contains(error, "gain illisible"));
    CHECK(parseLinkPaths("1 : 0 @ c", &error).empty());
    CHECK(contains(error, "phase illisible"));
    CHECK(parseLinkPaths("-1 : 0", &error).empty());
    CHECK(contains(error, "retard de 0 à 32 symboles"));
    CHECK(parseLinkPaths("40 : 0", &error).empty());
    CHECK(parseLinkPaths("1 : 200", &error).empty());
    CHECK(contains(error, "gain de"));
    CHECK(parseLinkPaths("0:0;1:0;2:0;3:0;4:0;5:0;6:0;7:0;8:0", &error).empty());
    CHECK(contains(error, "Trop de trajets : 8 au plus"));
    CHECK(parseLinkPaths("0:0;1:0;2:0;3:0;4:0;5:0;6:0;7:0").size() == 8);

    // One path as sent is no channel; one late or turned, or several, are.
    LinkRecipe r = defaultLinkRecipe();
    CHECK(!linkHasPaths(r));
    CHECK(!linkHasChannel(r));
    for (const char *none : {"", "0 : 0", "0 : -6", " ; "})
    {
        r.paths = none;
        CHECK(!linkHasPaths(r));
        CHECK(!linkHasChannel(r));
        CHECK(linkText(r) == linkText(defaultLinkRecipe()));
    }
    for (const char *some : {"1 : 0", "0 : 0 @ 30", "0 : 0 ; 1 : -6"})
    {
        r.paths = some;
        CHECK(linkHasPaths(r));
        CHECK(linkHasChannel(r));
    }
    r.paths = "0 : 0 ; 1 : -6";
    CHECK(contains(linkText(r), ", 2 trajets"));
    CHECK(!contains(linkText(r), "égalisée"));
    r.equalizer = LinkEqualizer::Linear;
    CHECK(contains(linkText(r), ", 2 trajets, égalisée"));
    r.paths = "1,5 : 0";
    CHECK(contains(linkText(r), ", un trajet retardé"));
    // Paths that cannot be read, taps and steps out of bounds: said, nothing computed.
    r.paths = "0 : 0 ; nope";
    CHECK(!linkHasPaths(r));
    LinkResult res = simulateLink(r);
    CHECK(!res.ok());
    CHECK(contains(res.error, "Trajet 2"));
    r.paths = "0 : 0 ; 1 : -6";
    r.equalizerTaps = 10;
    CHECK(contains(simulateLink(r).error, "nombre impair de prises, de 3 à 129"));
    r.equalizerTaps = 131;
    CHECK(!simulateLink(r).ok());
    r.equalizerTaps = 11;
    r.equalizerStep = 0.0;
    CHECK(contains(simulateLink(r).error, "Pas de l'égaliseur"));
    r.equalizerStep = 0.05;
    // The recipe keeps them.
    r.paths = "0 : 0 ; 1,5 : -6 @ 45";
    r.equalizerTaps = 21;
    r.equalizerStep = 0.02;
    LinkRecipe back = linkRecipeFromJson(toJson(r));
    CHECK(back.paths == r.paths);
    CHECK(back.equalizer == LinkEqualizer::Linear);
    CHECK(back.equalizerTaps == 21);
    CHECK(back.equalizerStep == 0.02);
    CHECK(toJson(r)["equalizer"].toString() == "linear");
    CHECK(equalizerKey(LinkEqualizer::None) == "none");
    model::Json json = toJson(r);
    json["equalizerTaps"] = 20; // an even number typed: the next odd one
    json["equalizerStep"] = 7.0;
    json["equalizer"] = "none";
    back = linkRecipeFromJson(json);
    CHECK(back.equalizerTaps == 21);
    CHECK(back.equalizerStep == 1.0);
    CHECK(back.equalizer == LinkEqualizer::None);
    CHECK(linkRecipeFromJson(toJson(defaultLinkRecipe())).paths.empty());
    CHECK(linkRecipeFromJson(toJson(defaultLinkRecipe())).equalizer == LinkEqualizer::None);

    // An echo of half the amplitude one symbol later, the ideal receiver: each symbol gets half the one before.
    r = defaultLinkRecipe();
    r.paths = "0 : 0 ; 1 : -6";
    r.ebN0Db = 12.0;
    res = simulateLink(r);
    REQUIRE(res.ok());
    const LinkEqualization &q = res.equalization;
    CHECK(q.paths);
    CHECK(!q.used);
    CHECK(q.mainPath == 0);
    REQUIRE(q.typed.size() == 2);
    REQUIRE(q.amplitude.size() == 2);
    CHECK_REL(q.amplitude[0] * q.amplitude[0] + q.amplitude[1] * q.amplitude[1], 1.0, 1e-12); // their powers add up to 1
    CHECK_REL(q.amplitude[1] / q.amplitude[0], std::pow(10.0, -6.0 / 20.0), 1e-12);
    REQUIRE(!q.response.empty());
    const auto tap = [&](int index) { return q.response[static_cast<std::size_t>(index - q.responseFirst)]; };
    CHECK(q.responseFirst == -8);
    CHECK(q.response.size() == 18);
    CHECK(std::abs(tap(0) - 1.0) < 1e-3);
    CHECK(std::abs(tap(1) - std::pow(10.0, -6.0 / 20.0)) < 1e-3);
    for (int j : {-3, -2, -1, 2, 3, 4}) CHECK(std::abs(tap(j)) < 1e-3);
    // What a decision sees: the echo (a quarter of the power) and the noise, scaled as the strongest path is.
    const double noise = 1.0 / (2.0 * std::pow(10.0, 1.2)) / (q.amplitude[0] * q.amplitude[0]);
    CHECK(std::abs(q.sinrBeforeDb - 10.0 * std::log10(1.0 / (std::pow(10.0, -0.6) + noise))) < 0.05);
    CHECK(std::abs(res.merDb - q.sinrBeforeDb) < 0.3); // and it is what is measured
    CHECK(res.ber > 1e-3);                              // against 9e-9 in the theory
    CHECK(res.theoryBer < 1e-8);
    CHECK(res.skipSymbols == 6);
    CHECK(res.skipEndSymbols == 6);
    CHECK(std::isnan(q.sinrAfterDb));
    CHECK(std::isnan(q.predictedBer));
    CHECK(anyContains(linkSummary(res), "Canal : 2 trajets : 0 symb. à −0,97 dB, 1 symb. à −6,97 dB."));
    CHECK(anyContains(linkSummary(res), "Aux instants de décision, sans égaliseur"));
    CHECK(anyContains(linkSummary(res), "les autres trajets restent étalés sur les symboles voisins"));
    // The record received holds the last path too.
    CHECK(res.receivedPassband.size() > res.sentPassband.size() + 48);
    // The strongest path is the one decided on, wherever it comes: here the second, two symbols late.
    r.paths = "0 : -10 ; 2 : 0 @ 50";
    res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(res.equalization.mainPath == 1);
    CHECK(res.equalization.responseFirst == -10);
    {
        const LinkEqualization &late = res.equalization;
        const auto at = [&](int index) { return late.response[static_cast<std::size_t>(index - late.responseFirst)]; };
        CHECK(std::abs(at(0) - 1.0) < 1e-3);
        CHECK(std::abs(std::abs(at(-2)) - std::pow(10.0, -0.5)) < 1e-3);
        CHECK(std::abs(std::arg(at(-2)) + 50.0 * dsp::kPi / 180.0) < 1e-3); // turned back with the strongest
    }
    CHECK(res.ber < 0.01);
    // FSK has paths too, but no constellation to see them on, and no equalizer.
    r = defaultLinkRecipe();
    r.modulation = dsp::Modulation::Fsk;
    r.paths = "0 : 0 ; 1 : -6";
    r.equalizer = LinkEqualizer::Linear;
    res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(res.equalization.paths);
    CHECK(res.equalization.response.empty());
    CHECK(!res.equalization.used);
    CHECK(res.skipEndSymbols == 0);
    CHECK(res.warnings.empty());
    CHECK(anyContains(linkSummary(res), "pas d'égaliseur pour elle"));
    r.equalizer = LinkEqualizer::None;
    CHECK(!anyContains(linkSummary(simulateLink(r)), "pas d'égaliseur pour elle"));
    r.equalizer = LinkEqualizer::Linear;
    CHECK(!contains(linkText(r), "égalisée"));
}

TEST_CASE("link: the equalizer computed by the ideal receiver, as announced", "[link]")
{
    // The echo of the examples, repaired: the points back on the constellation, the error rate the theory gives at
    // the Eb/N0 that is left.
    LinkRecipe r = defaultLinkRecipe();
    r.paths = "0 : 0 ; 1 : -6";
    r.equalizer = LinkEqualizer::Linear;
    r.symbols = 20000;
    std::size_t errors = 0, bits = 0;
    double predicted = 0.0;
    for (std::uint64_t seed : {11u, 12u, 13u})
    {
        r.noiseSeed = seed;
        r.dataSeed = seed;
        const LinkResult res = simulateLink(r);
        REQUIRE(res.ok());
        const LinkEqualization &q = res.equalization;
        CHECK(q.used);
        CHECK(!q.adaptive);
        CHECK(q.taps.size() == 11);
        CHECK(q.tapsFirst == -5);
        CHECK(q.pointsBefore.size() == res.reception.decisionPoints.size());
        CHECK(res.skipEndSymbols == 11);
        CHECK(q.sinrAfterDb > q.sinrBeforeDb + 3.0);
        CHECK(std::abs(q.equivalentEbN0Db - (q.sinrAfterDb - 10.0 * std::log10(2.0))) < 1e-9);
        CHECK(std::abs(res.merDb - q.sinrAfterDb) < 0.3);
        // What the symbols see after it: one symbol, the others nearly gone.
        REQUIRE(!q.combined.empty());
        for (std::size_t m = 0; m < q.combined.size(); ++m)
        {
            const int index = q.combinedFirst + static_cast<int>(m);
            CHECK(std::abs(q.combined[m] - (index == 0 ? 1.0 : 0.0)) < (index == 0 ? 1e-9 : 0.12));
        }
        errors += res.bitErrors;
        bits += res.comparedBits;
        predicted = q.predictedBer;
    }
    const double expected = predicted * static_cast<double>(bits);
    if (qacq::test::state().verbose)
        std::printf("      echo at -6 dB, 11 taps: %zu errors over %zu bits, %.1f announced\n", errors, bits, expected);
    CHECK(expected > 300.0);
    CHECK(std::abs(static_cast<double>(errors) - expected) <= 5.0 * std::sqrt(expected) + 0.15 * expected);
    // Far below what the echo did alone.
    r.symbols = 4000;
    r.noiseSeed = 2;
    r.dataSeed = 1;
    LinkRecipe bare = r;
    bare.equalizer = LinkEqualizer::None;
    const LinkResult without = simulateLink(bare), with = simulateLink(r);
    REQUIRE(without.ok());
    REQUIRE(with.ok());
    CHECK(without.ber > 5.0 * with.ber);
    CHECK(anyContains(linkSummary(with), "Égaliseur linéaire calculé (11 prises, erreur quadratique minimale)"));
    CHECK(anyContains(linkSummary(with), "Soit un Eb/N0 équivalent de "));
    CHECK(anyContains(linkSummary(with), "Les 6 premiers symboles et les 11 derniers (l'égaliseur n'a pas leurs voisins suivants) ne comptent pas."));
    // Without noise it inverts the channel: nothing left but what its length cannot reach.
    r.noise = false;
    r.equalizerTaps = 31;
    LinkResult res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(res.bitErrors == 0);
    CHECK(res.merDb > 30.0);
    CHECK(res.equalization.sinrAfterDb > 60.0);
    CHECK(anyContains(linkSummary(res), "sans bruit : inversion"));
    // Bits typed repeat every 4 symbols: the response is measured on symbols of its own, the taps are the same.
    LinkRecipe typed = r;
    typed.data = LinkData::Bits;
    const LinkResult periodic = simulateLink(typed);
    REQUIRE(periodic.ok());
    CHECK(periodic.bitErrors == 0);
    REQUIRE(periodic.equalization.taps.size() == res.equalization.taps.size());
    for (std::size_t m = 0; m < res.equalization.taps.size(); ++m)
        CHECK(std::abs(periodic.equalization.taps[m] - res.equalization.taps[m]) < 1e-12);
    // No echo: the taps are a single 1, nothing changes but the symbols left out at the end.
    r = defaultLinkRecipe();
    const LinkResult plain = simulateLink(r);
    r.equalizer = LinkEqualizer::Linear;
    res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(res.equalization.used);
    CHECK(!res.equalization.paths);
    CHECK(res.skipEndSymbols == 11);
    for (std::size_t k = 100; k < 3000; k += 123) CHECK(std::abs(res.reception.decisionPoints[k] - plain.reception.decisionPoints[k]) < 1e-9);
    // Two paths nearly equal: the notch costs several dB, as announced; a curve of the error rate runs through it.
    r.paths = "0 : 0 ; 1 : -1";
    r.equalizerTaps = 31;
    r.ebN0Db = 12.0;
    res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(r.ebN0Db - res.equalization.equivalentEbN0Db > 4.0);
    CHECK(r.ebN0Db - res.equalization.equivalentEbN0Db < 8.0);
    BerSweep sweep;
    sweep.maxBits = 40000;
    sweep.targetErrors = 100;
    const BerPoint point = measureBer(r, 8.0, sweep, 5);
    CHECK(!point.cancelled);
    CHECK(point.bits >= 40000);
    LinkRecipe at8 = r;
    at8.ebN0Db = 8.0;
    const double announced = simulateLink(at8).equalization.predictedBer;
    if (qacq::test::state().verbose) std::printf("      nearly equal paths at 8 dB: measured %.4f, announced %.4f\n", point.ber, announced);
    CHECK(point.ber > 0.7 * announced);
    CHECK(point.ber < 1.4 * announced);
    CHECK(point.ber > 20.0 * point.theory); // far from the theory without paths
}

TEST_CASE("link: the equalizer learnt by the synchronized receiver", "[link][sync]")
{
    const auto named = [](const char *part) {
        for (const LinkExample &e : linkExamples())
            if (contains(e.name, part)) return e.recipe;
        return LinkRecipe{};
    };
    // 16-QAM through a carrier and a clock off and an echo at -8 dB: without equalizer its decisions are so often
    // wrong that the carrier loop slips; with it, taps learnt on the preamble then on its decisions, and the carrier
    // followed after them.
    LinkRecipe r = named("l'égaliseur apprend");
    REQUIRE(r.modulation == dsp::Modulation::Qam16);
    REQUIRE(r.receiver == LinkReceiver::Synchronized);
    REQUIRE(r.equalizer == LinkEqualizer::Linear);
    LinkResult res = simulateLink(r);
    REQUIRE(res.ok());
    const LinkEqualization &q = res.equalization;
    CHECK(q.used);
    CHECK(q.adaptive);
    CHECK(q.taps.size() == 11);
    CHECK(std::isnan(q.predictedBer));
    // The channel is seen from where its timing loop settled (on the strongest path, here: the echo is a whole
    // symbol later), and its taps with that channel leave a single symbol, to the scale of the receiver.
    CHECK(std::abs(q.instantSymbols) < 0.03);
    CHECK(std::abs(q.symbolGain - 1.0) < 0.02);
    REQUIRE(!q.response.empty());
    REQUIRE(!q.combined.empty());
    for (std::size_t m = 0; m < q.combined.size(); ++m)
    {
        const int index = q.combinedFirst + static_cast<int>(m);
        if (index == 0)
            CHECK(std::abs(std::abs(q.combined[m]) - 1.0) < 0.1);
        else
            CHECK(std::abs(q.combined[m]) < 0.05);
    }
    CHECK(std::abs(q.sinrBeforeDb - 7.9) < 0.3);
    CHECK(q.squaredError.size() == res.reception.decisionPoints.size());
    CHECK(q.pointsBefore.size() == res.reception.decisionPoints.size());
    CHECK(res.ber < 1e-3);
    CHECK(res.merDb > 17.0);
    CHECK(res.sync.phaseSlips == 0);
    CHECK(res.sync.phaseErrorRms < 3.0);
    CHECK(!res.sync.timingLost);
    CHECK(std::abs(res.sync.frequencyOffsetHz - 40.0) < 0.2);
    CHECK(std::abs(std::remainder(res.sync.phaseDegrees - res.sync.truePhaseDegrees, 360.0)) < 5.0);
    CHECK(res.skipSymbols == 64);
    CHECK(res.skipEndSymbols == 11);
    // Its error settles some 20 dB under a symbol, within 1 dB of what the taps computed from the channel leave; its
    // taps are those: 1, then the echo undone again and again (-0.4, 0.16, -0.06...), to the scale of the receiver.
    double settled = 0.0;
    for (std::size_t k = 3000; k < 3900; ++k) settled += q.squaredError[k];
    CHECK(10.0 * std::log10(settled / 900.0) < -17.0);
    LinkRecipe known = r;
    known.receiver = LinkReceiver::Ideal;
    const LinkResult byChannel = simulateLink(known);
    REQUIRE(byChannel.ok());
    CHECK(q.settledErrorDb < -(byChannel.equalization.sinrAfterDb - 1.0));
    CHECK(q.settledErrorDb > -(byChannel.equalization.sinrAfterDb + 1.0));
    REQUIRE(q.tapsFirst == -5);
    const std::complex<double> main = q.taps[5];
    CHECK(std::abs(std::abs(main) - 1.0) < 0.1);
    for (int m = 1; m <= 3; ++m) CHECK(std::abs(q.taps[static_cast<std::size_t>(5 + m)] / main - std::pow(-0.398, m)) < 0.04);
    for (int m = -5; m < 0; ++m) CHECK(std::abs(q.taps[static_cast<std::size_t>(5 + m)]) < 0.04);
    CHECK(anyContains(linkSummary(res), "Égaliseur linéaire appris (11 prises)"));
    CHECK(anyContains(linkSummary(res), "La boucle de porteuse travaille après lui"));
    CHECK(anyContains(linkSummary(res), "Le préambule (64 symboles) et les 11 derniers symboles (l'égaliseur n'a pas leurs voisins suivants) ne comptent pas."));
    LinkRecipe bare = r;
    bare.equalizer = LinkEqualizer::None;
    const LinkResult without = simulateLink(bare);
    REQUIRE(without.ok());
    CHECK(without.ber > 0.1);
    CHECK(without.sync.phaseSlips >= 1);
    // QPSK, three paths the second of which is turned half-way round: about what the ideal receiver gets from the
    // taps it computes.
    r = named("QPSK synchronisée");
    r.paths = "0 : 0 ; 1 : -3 @ 180 ; 2 : -9 @ 60";
    r.equalizer = LinkEqualizer::Linear;
    r.equalizerTaps = 21;
    res = simulateLink(r);
    REQUIRE(res.ok());
    LinkRecipe ideal = r;
    ideal.receiver = LinkReceiver::Ideal;
    const LinkResult computed = simulateLink(ideal);
    REQUIRE(computed.ok());
    if (qacq::test::state().verbose)
        std::printf("      three paths: learnt %.4f, computed %.4f (announced %.4f)\n", res.ber, computed.ber, computed.equalization.predictedBer);
    CHECK(res.ber < 4.0 * computed.equalization.predictedBer + 0.002);
    CHECK(res.sync.phaseSlips == 0);
    bare = r;
    bare.equalizer = LinkEqualizer::None;
    CHECK(simulateLink(bare).ber > 0.2);
    // An echo in phase one symbol later digs the spectrum at the edges of the band, where the timing loop reads the
    // clock: it loses its lock, and says so.
    r = named("QPSK synchronisée");
    r.paths = "0 : 0 ; 1 : -3";
    r.equalizer = LinkEqualizer::Linear;
    res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(res.sync.timingLost);
    CHECK(anyContains(res.warnings, "La boucle de rythme a décroché"));
    CHECK(res.ber > 0.2);
    // (The same echo turned half-way round digs the middle of the band instead: the timing holds.)
    r.paths = "0 : 0 ; 1 : -3 @ 180";
    res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(!res.sync.timingLost);
    CHECK(res.ber < 0.02);
    // The curve of the error rate runs the same chain.
    BerSweep sweep;
    sweep.maxBits = 20000;
    const BerPoint point = measureBer(named("l'égaliseur apprend"), 14.0, sweep, 3);
    CHECK(!point.cancelled);
    CHECK(point.bits >= 15000);
    CHECK(point.ber < 0.01);
}

TEST_CASE("link: paths a fraction of a symbol apart, and where the decisions are taken", "[link][sync]")
{
    // An echo half a symbol after the strongest path adds to the symbol itself at its decision instant: the ideal
    // receiver, which knows the channel, brings the symbol back to its size. What it measures is then what was
    // announced (before: a constellation 1.44 times too large, every dense one decided wrong).
    LinkRecipe r = defaultLinkRecipe();
    r.modulation = dsp::Modulation::Qam16;
    r.paths = "0 : 0 ; 0,5 : -3";
    r.noise = false;
    LinkResult res = simulateLink(r);
    REQUIRE(res.ok());
    {
        const LinkEqualization &q = res.equalization;
        CHECK(q.instantSymbols == 0.0);
        CHECK(q.symbolGain > 1.40);
        CHECK(q.symbolGain < 1.48);
        REQUIRE(!q.response.empty());
        const auto tap = [&](int index) { return q.response[static_cast<std::size_t>(index - q.responseFirst)]; };
        CHECK(std::abs(tap(0) - 1.0) < 1e-9); // the symbol itself: 1, as the receiver brings it
        CHECK(std::abs(tap(1).real() - 0.305) < 0.01);
        CHECK(std::abs(tap(-1).real() + 0.080) < 0.01);
        CHECK(std::abs(res.merDb - q.sinrBeforeDb) < 0.15);
        CHECK(std::abs(q.sinrBeforeDb - 9.69) < 0.05);
        CHECK(anyContains(linkSummary(res), "les trajets donnent au symbole lui-même 1,44 fois"));
        // The mean energy of its decision points: that of the constellation, and what the neighbours add.
        double energy = 0.0;
        std::size_t counted = 0;
        for (std::size_t k = 100; k + 100 < res.reception.decisionPoints.size(); ++k, ++counted) energy += std::norm(res.reception.decisionPoints[k]);
        CHECK_REL(energy / static_cast<double>(counted), 1.0 + std::pow(10.0, -q.sinrBeforeDb / 10.0), 0.03);
    }
    // With noise too; and the curve of the error rate runs the same receiver.
    r.noise = true;
    r.ebN0Db = 14.0;
    res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(std::abs(res.merDb - res.equalization.sinrBeforeDb) < 0.3);
    BerSweep sweep;
    sweep.maxBits = 30000;
    const BerPoint point = measureBer(r, 14.0, sweep, 4);
    CHECK(!point.cancelled);
    CHECK(point.ber > 0.5 * res.ber);
    CHECK(point.ber < 2.0 * res.ber);
    // Its equalizer starts from there: the taps computed leave a single symbol.
    r.noise = false;
    r.equalizer = LinkEqualizer::Linear;
    res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(res.bitErrors == 0);
    CHECK(res.merDb > 30.0);
    CHECK(res.equalization.pointsBefore.size() == res.reception.decisionPoints.size());
    // An echo a whole symbol later adds nothing to the symbol itself.
    r.equalizer = LinkEqualizer::None;
    r.paths = "0 : 0 ; 1 : -3";
    res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(std::abs(res.equalization.symbolGain - 1.0) < 1e-3);
    CHECK(!anyContains(linkSummary(res), "les trajets donnent au symbole lui-même"));

    // The synchronized receiver: its timing loop settles between the two paths, where the eye is most open, and the
    // channel is seen from there. It does better than the ideal receiver, which stays on the strongest path.
    r = defaultLinkRecipe();
    r.modulation = dsp::Modulation::Qam16;
    r.symbolRate = 500.0;
    r.rolloff = 1.0;
    r.paths = "0 : 0 ; 0,5 : -3";
    r.noise = false;
    r.receiver = LinkReceiver::Synchronized;
    res = simulateLink(r);
    REQUIRE(res.ok());
    {
        const LinkEqualization &q = res.equalization;
        CHECK(!res.sync.timingLost);
        CHECK(q.instantSymbols > 0.12);
        CHECK(q.instantSymbols < 0.27);
        CHECK(std::abs(q.instantSymbols - res.sync.timingErrorRms) < 0.01); // (a steady offset: its rms is its mean)
        CHECK(q.symbolGain > 1.35);
        CHECK(std::abs(res.merDb - q.sinrBeforeDb) < 0.6);
        CHECK(q.sinrBeforeDb > 17.0);
        CHECK(res.bitErrors == 0);
        CHECK(anyContains(linkSummary(res), "aux instants que sa boucle de rythme a trouvés, 0,"));
        CHECK(anyContains(linkSummary(res), "symbole après le trajet le plus fort"));
        LinkRecipe ideal = r;
        ideal.receiver = LinkReceiver::Ideal;
        const LinkResult onStrongest = simulateLink(ideal);
        REQUIRE(onStrongest.ok());
        CHECK(std::abs(onStrongest.equalization.sinrBeforeDb - 11.65) < 0.1);
        CHECK(std::abs(onStrongest.merDb - onStrongest.equalization.sinrBeforeDb) < 0.15);
        CHECK(q.sinrBeforeDb > onStrongest.equalization.sinrBeforeDb + 5.0);
    }
    // Its equalizer learns from those instants: with the channel seen there, a single symbol left.
    r.equalizer = LinkEqualizer::Linear;
    res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(res.bitErrors == 0);
    CHECK(res.merDb > 35.0);
    REQUIRE(!res.equalization.combined.empty());
    for (std::size_t m = 0; m < res.equalization.combined.size(); ++m)
    {
        const int index = res.equalization.combinedFirst + static_cast<int>(m);
        CHECK(std::abs(std::abs(res.equalization.combined[m]) - (index == 0 ? 1.0 : 0.0)) < (index == 0 ? 0.08 : 0.03));
    }
    // A timing loop that lost its lock has no instant of its own: the channel as the ideal receiver sees it.
    for (const LinkExample &e : linkExamples())
        if (contains(e.name, "QPSK synchronisée")) r = e.recipe;
    REQUIRE(r.receiver == LinkReceiver::Synchronized);
    r.paths = "0 : 0 ; 1 : -3";
    r.equalizer = LinkEqualizer::Linear;
    res = simulateLink(r);
    REQUIRE(res.ok());
    REQUIRE(res.sync.timingLost);
    CHECK(res.equalization.instantSymbols == 0.0);
    CHECK(!res.equalization.response.empty());
    CHECK(anyContains(linkSummary(res), "vu du trajet le plus fort, comme par le récepteur idéal : la boucle de rythme a décroché"));

    // Two paths that cancel at the decision instants: nothing of the symbol left to scale by; said, not divided by.
    r = defaultLinkRecipe();
    r.paths = "0 : 0 ; 0 : 0 @ 180";
    res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(anyContains(res.warnings, "Les trajets s'annulent presque aux instants de décision"));
    CHECK(res.equalization.symbolGain == 1.0);
    CHECK(std::isnan(res.equalization.sinrBeforeDb));
    CHECK(res.ber > 0.3);
}

TEST_CASE("link: a small roll-off, a longer pulse; too small, it says so", "[link]")
{
    // A small roll-off decays slowly: the pulse is cut longer (22 symbols at 0.1, 44 at 0.05), and the pair leaves
    // nothing to see between symbols; said in the summary.
    for (const double alpha : {0.05, 0.1})
    {
        LinkRecipe r = defaultLinkRecipe();
        r.modulation = dsp::Modulation::Qam64;
        r.rolloff = alpha;
        r.noise = false;
        const LinkResult res = simulateLink(r);
        REQUIRE(res.ok());
        CHECK(res.warnings.empty());
        CHECK(res.merDb > 44.0);
        CHECK(res.bitErrors == 0);
        const std::size_t span = dsp::rootRaisedCosineSpan(alpha);
        CHECK(span == (alpha == 0.1 ? 22u : 44u));
        CHECK(res.skipSymbols == span / 2);
        CHECK(res.skipEndSymbols == span / 2);
        CHECK(anyContains(linkSummary(res), "Impulsions coupées à " + std::to_string(span / 2) + " symboles de part et d'autre"));
        CHECK(anyContains(linkSummary(res), "Les " + std::to_string(span / 2) + " premiers et derniers symboles ne comptent pas"));
        if (qacq::test::state().verbose) std::printf("      roll-off %.2f: span %zu, MER %.2f dB\n", alpha, span, res.merDb);
    }
    // 256-QAM at 24 dB with a roll-off of 0.1: as the theory says (2.7e-7: nothing), where a pulse cut to 12 symbols
    // left 632 errors (an interference at -25 dB).
    {
        LinkRecipe r = defaultLinkRecipe();
        r.modulation = dsp::Modulation::Qam256;
        r.representation = dsp::Representation::ComplexBaseband;
        r.rolloff = 0.1;
        r.ebN0Db = 24.0;
        const LinkResult res = simulateLink(r);
        REQUIRE(res.ok());
        CHECK(res.bitErrors == 0);
        CHECK(std::abs(res.merDb - (24.0 + 10.0 * std::log10(8.0))) < 0.5);
    }
    // Under 0.02, even cut to 96 symbols it is no longer quite a Nyquist pulse: said, with what is measured without
    // noise.
    {
        LinkRecipe r = defaultLinkRecipe();
        r.modulation = dsp::Modulation::Qam64;
        r.rolloff = 0.01;
        r.noise = false;
        const LinkResult res = simulateLink(r);
        REQUIRE(res.ok());
        REQUIRE(res.warnings.size() == 1);
        CHECK(contains(res.warnings.front(), "n'est plus tout à fait une impulsion de Nyquist"));
        CHECK(contains(res.warnings.front(), "même coupée à 48 symboles de part et d'autre"));
        CHECK(!anyContains(linkSummary(res), "Impulsions coupées à"));
        // "… une interférence à −32,2 dB du symbole …": the MER measured.
        const std::string &text = res.warnings.front();
        const std::size_t at = text.find("interférence à \xE2\x88\x92");
        REQUIRE(at != std::string::npos);
        std::string number = text.substr(at + std::string("interférence à \xE2\x88\x92").size(), 4);
        for (char &ch : number)
            if (ch == ',') ch = '.';
        CHECK(std::abs(std::stod(number) - res.merDb) < 0.5);
        if (qacq::test::state().verbose) std::printf("      roll-off 0.01: announced -%s dB, measured %.2f dB\n", number.c_str(), res.merDb);
    }
    // From 0.2 on, nothing to say; the 12 symbols that do at 0.35 kept; nor for other pulses.
    for (const double alpha : {0.2, 0.25, 0.35, 0.5, 1.0})
    {
        LinkRecipe r = defaultLinkRecipe();
        r.rolloff = alpha;
        r.noise = false;
        const LinkResult res = simulateLink(r);
        REQUIRE(res.ok());
        CHECK(res.warnings.empty());
        CHECK(res.merDb > 44.0);
        CHECK(anyContains(linkSummary(res), "Impulsions coupées à") == (dsp::rootRaisedCosineSpan(alpha) > 12));
    }
    CHECK(dsp::rootRaisedCosineSpan(0.35) == 12);
    LinkRecipe r = defaultLinkRecipe();
    r.rolloff = 0.01;
    r.pulse = dsp::PulseShape::Rectangular;
    LinkResult res = simulateLink(r);
    CHECK(res.warnings.empty());
    CHECK(res.skipSymbols == 0);
    r.pulse = dsp::PulseShape::RootRaisedCosine;
    r.modulation = dsp::Modulation::Fsk;
    CHECK(simulateLink(r).warnings.empty());
    // The synchronized receiver: its preamble is longer than the edge here; where it is not, the first symbols are
    // said to be those of the edge too.
    r = defaultLinkRecipe();
    r.receiver = LinkReceiver::Synchronized;
    r.rolloff = 0.05; // an edge of 22 symbols
    r.preambleSymbols = 16;
    res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(res.skipSymbols == 22);
    CHECK(anyContains(linkSummary(res), "Les 22 premiers symboles (le préambule, 16, et ceux dont le bord de l'enregistrement coupe encore les impulsions)"));
}

TEST_CASE("link: the bit error rate measured matches the theory", "[link]")
{
    LinkRecipe r = defaultLinkRecipe(); // QPSK, root raised cosine, on a carrier
    r.symbols = 20000;
    r.ebN0Db = 5.0;
    checkAgainstTheory(simulateLink(r), "QPSK RRC on a carrier 5 dB");
    r.modulation = dsp::Modulation::Qam16;
    r.pulse = dsp::PulseShape::Rectangular;
    r.ebN0Db = 9.0;
    r.carrierHz = 7000.0; // 2 fc T = 14
    checkAgainstTheory(simulateLink(r), "16-QAM rectangular 9 dB");
    r.modulation = dsp::Modulation::Ask;
    r.askLowLevel = 0.3;
    r.ebN0Db = 11.0;
    const LinkResult ask = simulateLink(r);
    CHECK_NEAR(ask.theoryBer, dsp::gaussianQ(std::sqrt(std::pow(10.0, 1.1) * 0.49 / 1.09)), 1e-14, 0);
    checkAgainstTheory(ask, "ASK low level 0.3, 11 dB");
    r.modulation = dsp::Modulation::Fsk;
    r.fskDeviationHz = 500.0; // tones 1 kHz apart at 1 kBd: orthogonal
    r.ebN0Db = 9.0;
    const LinkResult fsk = simulateLink(r);
    CHECK(!anyContains(fsk.warnings, "non orthogonales"));
    checkAgainstTheory(fsk, "FSK orthogonal 9 dB");
    // The symbol error rate, and the EVM of noise alone: 1 / sqrt(Es/N0) for unit symbols (QPSK: Es = 2 Eb).
    r = defaultLinkRecipe();
    r.symbols = 20000;
    r.ebN0Db = 10.0;
    r.representation = dsp::Representation::ComplexBaseband;
    const LinkResult q = simulateLink(r);
    CHECK_NEAR(q.evmPercent, 100.0 / std::sqrt(2.0 * 10.0), 0.03, 0);
    CHECK_NEAR(q.merDb, 13.01, 0, 0.15);
}

TEST_CASE("link: a text sent and received, bits typed", "[link]")
{
    LinkRecipe r = defaultLinkRecipe();
    r.data = LinkData::Text;
    r.text = "Été à Noël, 3 € ! ";
    r.noise = false;
    LinkResult res = simulateLink(r);
    REQUIRE(res.ok());
    // From where the count starts: the text from its first letter (the edges carry its end, as if it went on).
    CHECK(res.sentText.rfind(r.text, 0) == 0);
    CHECK(res.receivedText == res.sentText);
    CHECK(res.textErrors == 0);
    r.uart = true;
    r.modulation = dsp::Modulation::Bpsk;
    res = simulateLink(r);
    CHECK(res.sentText.rfind(r.text + r.text, 0) == 0);
    CHECK(res.receivedText == res.sentText);
    // Much noise: letters wrong, shown « � » when they do not read.
    r.noise = true;
    r.ebN0Db = 0.0;
    res = simulateLink(r);
    CHECK(res.textErrors > 10);
    CHECK(contains(res.receivedText, "\xEF\xBF\xBD"));
    CHECK(res.receivedText.size() > 50);
    // Longer than the record: its start only, said.
    r.text = std::string(2000, 'x');
    r.symbols = 1000;
    CHECK(anyContains(simulateLink(r).warnings, "plus court que le texte"));
    r.text = std::string(5000, 'y');
    CHECK(anyContains(simulateLink(r).warnings, "Texte coupé"));
    // Bits typed, repeated.
    r = defaultLinkRecipe();
    r.data = LinkData::Bits;
    r.bits = "1 1 0";
    r.modulation = dsp::Modulation::Bpsk;
    r.pulse = dsp::PulseShape::Rectangular;
    r.noise = false;
    res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(res.sentBits[0] == 1);
    CHECK(res.sentBits[2] == 0);
    CHECK(res.sentBits[5] == 0);
    r.bits = "abc";
    CHECK(contains(simulateLink(r).error, "Aucun bit"));
}

TEST_CASE("link: what is refused, texts, keys, JSON, signals", "[link]")
{
    LinkRecipe r = defaultLinkRecipe();
    CHECK(linkText(r) == "QPSK 1 kBd, racine de cosinus surélevé α = 0,35, porteuse 6 kHz");
    CHECK(linkShortText(r) == "QPSK 1 kBd");
    r.symbolRate = 1100.0; // 43,6 samples per symbol
    CHECK(contains(simulateLink(r).error, "nombre entier d'échantillons par symbole"));
    r.pulse = dsp::PulseShape::Rectangular;
    CHECK(simulateLink(r).ok());
    r.carrierHz = 30000.0;
    CHECK(contains(simulateLink(r).error, "La porteuse doit être comprise entre 0 et fs/2"));
    r.carrierHz = 6000.0;
    r.symbolRate = 50000.0;
    CHECK(contains(simulateLink(r).error, "au moins un échantillon par symbole"));
    r = defaultLinkRecipe();
    r.data = LinkData::Text;
    r.text.clear();
    CHECK(contains(simulateLink(r).error, "Aucun texte"));
    r = defaultLinkRecipe();
    r.symbols = 100000; // 4,8 million samples: brought back
    const LinkResult big = simulateLink(r);
    REQUIRE(big.ok());
    CHECK(anyContains(big.warnings, "Enregistrement ramené à 21\u202F845 symboles"));
    CHECK(big.sentPassband.size() <= kMaximumLinkSamples);

    r = defaultLinkRecipe();
    r.modulation = dsp::Modulation::Fsk;
    r.fskDeviationHz = 300.0;
    CHECK(linkText(r) == "FSK 1 kBd, tonalités 5,7 kHz et 6,3 kHz");
    CHECK(anyContains(simulateLink(r).warnings, "non orthogonales"));
    r.representation = dsp::Representation::ComplexBaseband;
    CHECK(linkText(r) == "FSK 1 kBd, tonalités à ±300 Hz, en I/Q");
    r = defaultLinkRecipe();
    r.modulation = dsp::Modulation::Ask;
    r.pulse = dsp::PulseShape::Rectangular;
    r.symbolRate = 300.0;
    CHECK(linkText(r) == "ASK 300 Bd (tout ou rien), impulsions rectangulaires, porteuse 6 kHz");

    CHECK(rateText(0.0) == "0");
    CHECK(rateText(0.25) == "0,25");
    CHECK(rateText(0.0312) == "0,031");
    CHECK(rateText(1.234e-4) == "1,2×10⁻⁴");

    // JSON both ways, unknown values to the default, bounds.
    r = linkExamples()[6].recipe;
    CHECK(toJson(linkRecipeFromJson(toJson(r))) == toJson(r));
    model::Json wild = toJson(r);
    wild["modulation"] = "1024qam";
    wild["rolloff"] = 3.0;
    wild["ebN0"] = 500.0;
    wild["symbols"] = 2.0;
    const LinkRecipe tamed = linkRecipeFromJson(wild);
    CHECK(tamed.modulation == dsp::Modulation::Qpsk);
    CHECK(tamed.rolloff == 1.0);
    CHECK(tamed.ebN0Db == 60.0);
    CHECK(tamed.symbols == kMinimumLinkSymbols);
    for (dsp::Modulation m : {dsp::Modulation::Ask, dsp::Modulation::Fsk, dsp::Modulation::Bpsk, dsp::Modulation::Qpsk,
                              dsp::Modulation::Psk8, dsp::Modulation::Qam16, dsp::Modulation::Qam64, dsp::Modulation::Qam256})
    {
        model::Json j = toJson(defaultLinkRecipe());
        j["modulation"] = modulationKey(m);
        CHECK(linkRecipeFromJson(j).modulation == m);
    }

    // As signals of the session: one channel on a carrier, I and Q otherwise.
    r = defaultLinkRecipe();
    LinkResult res = simulateLink(r);
    model::Signal sent = linkSignal(res, false);
    CHECK(sent.name == "QPSK 1 kBd (émis)");
    CHECK(sent.channelCount() == 1);
    CHECK(sent.channels[0].unit == "V");
    CHECK(model::validateSignal(sent).empty());
    CHECK(sent.provenance["operation"].toString() == "modulation");
    const model::Signal received = linkSignal(res, true);
    CHECK(received.name == "QPSK 1 kBd (reçu, Eb/N0 = 7,00 dB)");
    CHECK(received.channel(0)[100] != sent.channel(0)[100]);
    r.representation = dsp::Representation::ComplexBaseband;
    res = simulateLink(r);
    sent = linkSignal(res, false);
    CHECK(sent.channelCount() == 2);
    CHECK(sent.representation == model::Representation::IQ);
    CHECK(model::validateSignal(sent).empty());
}

TEST_CASE("link: points of a curve of the bit error rate", "[link]")
{
    BerSweep sweep;
    std::string error;
    CHECK(berSweepValues(sweep, &error).size() == 13);
    sweep.stepDb = 0.0;
    CHECK(berSweepValues(sweep, &error).empty());
    CHECK(contains(error, "pas"));
    sweep.stepDb = 0.1;
    sweep.toDb = 30.0;
    CHECK(berSweepValues(sweep, &error).empty());
    CHECK(contains(error, "Trop de points"));
    sweep = BerSweep{};
    sweep.toDb = -1.0;
    CHECK(berSweepValues(sweep, &error).empty());

    double lower = 0.0, upper = 0.0;
    binomialInterval(0, 1000, lower, upper);
    CHECK(lower == 0.0);
    CHECK_NEAR(upper, 3.84 / 1003.84, 1e-3, 0); // z² / (n + z²)
    binomialInterval(50, 1000, lower, upper);
    CHECK(lower < 0.05);
    CHECK(upper > 0.05);
    CHECK_NEAR(upper - lower, 2.0 * 1.96 * std::sqrt(0.05 * 0.95 / 1000.0), 0.05, 0);

    // BPSK on rectangular pulses at 4 dB: as many bits as 400 errors take at the rate of the theory (32 000, in pieces
    // of 5 461), and the interval holds the theory; at 9 dB, all the bits allowed.
    LinkRecipe r = defaultLinkRecipe();
    r.modulation = dsp::Modulation::Bpsk;
    r.pulse = dsp::PulseShape::Rectangular;
    sweep.maxBits = 400000;
    sweep.targetErrors = 400;
    const BerPoint p = measureBer(r, 4.0, sweep, 13);
    CHECK(!p.cancelled);
    CHECK(p.bits == 6 * 5461);
    CHECK(std::abs(static_cast<double>(p.errors) - p.theory * static_cast<double>(p.bits)) <
          4.0 * std::sqrt(p.theory * static_cast<double>(p.bits)));
    CHECK(p.lower < p.ber);
    CHECK(p.upper > p.ber);
    // Over many points, no bias: the errors of 12 seeds (about 1 600) against the theory, within 3 standard
    // deviations (stopping at the 100th error would have counted some 130 too many).
    sweep.targetErrors = 100;
    double errors = 0.0, bits = 0.0;
    for (std::uint64_t seed = 100; seed < 112; ++seed)
    {
        const BerPoint q = measureBer(r, 4.0, sweep, seed);
        errors += static_cast<double>(q.errors);
        bits += static_cast<double>(q.bits);
    }
    CHECK(std::abs(errors - p.theory * bits) < 3.0 * std::sqrt(p.theory * bits));
    sweep.targetErrors = 400;
    sweep.maxBits = 30000;
    CHECK(measureBer(r, 9.0, sweep, 11).bits >= 30000);
    CHECK_NEAR(p.theory, dsp::theoreticalBer(dsp::Modulation::Bpsk, 4.0), 1e-15, 0);
    // The same seed, the same count; stopped at once when asked.
    CHECK(measureBer(r, 4.0, sweep, 13).errors == p.errors);
    std::atomic<bool> cancelled{true};
    const model::Progress stop{&cancelled, nullptr};
    CHECK(measureBer(r, 4.0, sweep, 11, stop).cancelled);
    // The data of the recipe do not matter: random bits.
    r.data = LinkData::Text;
    r.text.clear();
    CHECK(!measureBer(r, 4.0, sweep, 11).cancelled);
}

TEST_CASE("link: fading, its theory, and the decision feedback equalizer", "[link]")
{
    const std::vector<LinkExample> &examples = linkExamples();
    // QPSK through Rayleigh fading (fd = 20 Hz), the ideal receiver: the theory averaged over the gain, ten orders
    // above the one without fading, and the error rate as it says over many fades (a point of the curve: several
    // records, each its own fades).
    const LinkRecipe rayleigh = examples[17].recipe;
    LinkResult res = simulateLink(rayleigh);
    REQUIRE(res.ok());
    CHECK_NEAR(res.theoryBer, dsp::fadingBer(dsp::Modulation::Qpsk, 15.0), 1e-12, 0);
    CHECK_NEAR(res.theorySer, dsp::fadingSer(dsp::Modulation::Qpsk, 15.0), 1e-12, 0);
    CHECK_NEAR(res.awgnTheoryBer, dsp::theoreticalBer(dsp::Modulation::Qpsk, 15.0), 1e-12, 0);
    CHECK(res.theoryBer > 1e10 * res.awgnTheoryBer);
    CHECK(linkTheoryBer(rayleigh, 15.0) == res.theoryBer);
    CHECK(res.bitErrors > 20);
    // Its interval comes from blocks of five periods of fd (250 symbols): the errors come in bursts, which Wilson's
    // interval, counting each bit apart, does not see.
    CHECK(res.berBatches == 16);
    CHECK(res.berBatchSymbols == 250);
    double wilsonLower = 0.0, wilsonUpper = 0.0;
    binomialInterval(res.bitErrors, res.comparedBits, wilsonLower, wilsonUpper);
    CHECK(res.berLower < wilsonLower);
    CHECK(res.berUpper >= wilsonUpper);
    CHECK(res.berLower <= res.ber);
    std::vector<std::string> summary = linkSummary(res);
    CHECK(anyContains(summary, "d'après 16 lots de 250 symboles : les évanouissements groupent les erreurs"));
    CHECK(anyContains(summary, "théorie avec ces évanouissements"));
    CHECK(anyContains(summary, "évanouissements de Rayleigh, fd = 20 Hz (fd·T = 0,02"));
    CHECK(anyContains(summary, "80 périodes de fd dans l'enregistrement"));
    CHECK(anyContains(summary, "le filtre adapté moyenne les creux les plus brefs"));
    CHECK(anyContains(summary, "son gain à chaque instant compris"));
    CHECK(linkText(rayleigh) == "QPSK 1 kBd, racine de cosinus surélevé α = 0,35, porteuse 6 kHz, évanouissements de Rayleigh (fd = 20 Hz)");
    BerSweep sweep;
    sweep.maxBits = 60000;
    sweep.targetErrors = 1000;
    const BerPoint at10 = measureBer(rayleigh, 10.0, sweep, 7);
    CHECK_NEAR(at10.theory, dsp::fadingBer(dsp::Modulation::Qpsk, 10.0), 1e-12, 0);
    if (qacq::test::state().verbose)
        std::printf("      Rayleigh at 10 dB: measured %.4f (%.4f to %.4f, %zu blocks), theory %.4f\n", at10.ber, at10.lower, at10.upper, at10.batches,
                    at10.theory);
    CHECK(at10.ber > 0.8 * at10.theory);
    CHECK(at10.ber < 1.25 * at10.theory);
    CHECK(at10.batches > 100);
    CHECK(at10.lower <= at10.theory);
    CHECK(at10.theory <= at10.upper);
    // The same seed, the same fades; another, others.
    CHECK(simulateLink(rayleigh).reception.bits == res.reception.bits);
    LinkRecipe other = rayleigh;
    other.fadingSeed = 4;
    CHECK(simulateLink(other).reception.bits != res.reception.bits);

    // 16-QAM through Rice fading (K = 10 dB): its line of sight leaves fewer and shallower fades.
    const LinkRecipe rice = examples[18].recipe;
    const BerPoint riceAt10 = measureBer(rice, 10.0, sweep, 7);
    CHECK_NEAR(riceAt10.theory, dsp::fadingBer(dsp::Modulation::Qam16, 10.0, 0.0, 10.0), 1e-12, 0);
    if (qacq::test::state().verbose)
        std::printf("      Rice at 10 dB: measured %.5f (%.5f to %.5f, %zu blocks), theory %.5f\n", riceAt10.ber, riceAt10.lower, riceAt10.upper,
                    riceAt10.batches, riceAt10.theory);
    CHECK(riceAt10.ber > 0.75 * riceAt10.theory);
    CHECK(riceAt10.ber < 1.3 * riceAt10.theory);
    CHECK(riceAt10.batches > 20);
    CHECK(riceAt10.lower <= riceAt10.theory);
    CHECK(riceAt10.theory <= riceAt10.upper);
    CHECK(riceAt10.theory < 0.2 * dsp::fadingBer(dsp::Modulation::Qam16, 10.0));
    CHECK(linkText(rice) == "16-QAM 1 kBd, racine de cosinus surélevé α = 0,35, porteuse 6 kHz, évanouissements de Rice (K = 10,0 dB, fd = 10 Hz)");
    // fd = 0: each gain drawn once, then still.
    LinkRecipe frozen = rice;
    frozen.dopplerHz = 0.0;
    res = simulateLink(frozen);
    REQUIRE(res.ok());
    CHECK(anyContains(linkSummary(res), "figés (fd = 0 : un gain tiré par trajet)"));
    CHECK(res.berBatches == 0); // (one draw: one block, nothing to say more than Wilson)
    // Several paths that fade: no theory; the ideal receiver's equalizer is computed for the paths as they are on
    // average, and says so.
    LinkRecipe both = rayleigh;
    both.paths = "0 : 0 ; 1 : -6";
    both.equalizer = LinkEqualizer::Linear;
    res = simulateLink(both);
    REQUIRE(res.ok());
    CHECK(std::isnan(res.theoryBer));
    CHECK(std::isnan(linkTheoryBer(both, 15.0)));
    summary = linkSummary(res);
    CHECK(anyContains(summary, "la théorie ne dit rien de plusieurs trajets qui s'évanouissent"));
    CHECK(anyContains(summary, "calculé pour les trajets tels qu'ils sont en moyenne"));
    CHECK(anyContains(summary, "s'évanouissent à leur façon"));
    // A Doppler shift beyond a quarter of the symbol rate is refused.
    LinkRecipe fast = rayleigh;
    fast.dopplerHz = 251.0;
    CHECK(contains(simulateLink(fast).error, "un quart du débit symbole"));
    fast.dopplerHz = 250.0;
    CHECK(simulateLink(fast).ok());

    // The recipe keeps them; out of bounds, the nearest bound; saved before them: no fading, 4 feedback taps.
    LinkRecipe kept = rice;
    kept.dopplerHz = 7.5;
    kept.riceKDb = 3.0;
    kept.fadingSeed = 11;
    kept.paths = "0 : 0 ; 1 : -6";
    kept.equalizer = LinkEqualizer::DecisionFeedback;
    kept.feedbackTaps = 6;
    model::Json json = toJson(kept);
    CHECK(json["fading"].toString() == "rice");
    CHECK(json["equalizer"].toString() == "dfe");
    const LinkRecipe back = linkRecipeFromJson(json);
    CHECK(back.fading == LinkFading::Rice);
    CHECK(back.dopplerHz == 7.5);
    CHECK(back.riceKDb == 3.0);
    CHECK(back.fadingSeed == 11);
    CHECK(back.equalizer == LinkEqualizer::DecisionFeedback);
    CHECK(back.feedbackTaps == 6);
    CHECK((toJson(back) == json));
    json["riceK"] = 99.0;
    json["feedbackTaps"] = 0;
    json["fading"] = "nope";
    const LinkRecipe clamped = linkRecipeFromJson(json);
    CHECK(clamped.riceKDb == kMaximumRiceKDb);
    CHECK(clamped.feedbackTaps == 1);
    CHECK(clamped.fading == LinkFading::None);
    model::Json old = toJson(defaultLinkRecipe());
    old.erase("fading");
    old.erase("feedbackTaps");
    CHECK(linkRecipeFromJson(old).fading == LinkFading::None);
    CHECK(linkRecipeFromJson(old).feedbackTaps == 4);
    CHECK(fadingKey(LinkFading::Rayleigh) == "rayleigh");
    CHECK(equalizerKey(LinkEqualizer::DecisionFeedback) == "dfe");

    // Two paths nearly equal: the decision feedback equalizer computed by the ideal receiver, 4 dB better than the
    // linear one; what the symbols before leave, taken off up to its feedback taps.
    const LinkRecipe dfe = examples[19].recipe;
    res = simulateLink(dfe);
    REQUIRE(res.ok());
    const LinkEqualization &q = res.equalization;
    CHECK(q.used);
    CHECK(!q.adaptive);
    CHECK(q.taps.size() == 31);
    REQUIRE(q.feedback.size() == 4);
    for (int m = 1; m <= 4; ++m)
    {
        const int at = m - q.combinedFirst;
        REQUIRE(at >= 0);
        REQUIRE(static_cast<std::size_t>(at) < q.combined.size());
        CHECK(std::abs(q.combined[static_cast<std::size_t>(at)]) < 1e-12);
    }
    LinkRecipe linear = dfe;
    linear.equalizer = LinkEqualizer::Linear;
    const LinkResult filled = simulateLink(linear);
    REQUIRE(filled.ok());
    CHECK(filled.equalization.feedback.empty());
    CHECK(q.sinrAfterDb > filled.equalization.sinrAfterDb + 3.0);
    CHECK(res.bitErrors < filled.bitErrors);
    CHECK(res.skipEndSymbols == filled.skipEndSymbols);
    CHECK(contains(linkText(dfe), ", 2 trajets, égalisée (retour de décision)"));
    summary = linkSummary(res);
    CHECK(anyContains(summary, "Égaliseur à retour de décision calculé (31 prises avant, 4 en retour, erreur quadratique minimale si les décisions sont justes)"));
    CHECK(anyContains(summary, "les erreurs viennent en salves"));
    // Learnt by the synchronized receiver, on its preamble then on its decisions: far fewer errors than the linear one.
    LinkRecipe learnt = dfe;
    learnt.receiver = LinkReceiver::Synchronized;
    const LinkResult fed = simulateLink(learnt);
    learnt.equalizer = LinkEqualizer::Linear;
    const LinkResult learntLinear = simulateLink(learnt);
    REQUIRE(fed.ok());
    REQUIRE(learntLinear.ok());
    CHECK(fed.equalization.adaptive);
    CHECK(fed.equalization.feedback.size() == 4);
    if (qacq::test::state().verbose) std::printf("      learnt: decision feedback %.4f, linear %.4f\n", fed.ber, learntLinear.ber);
    CHECK(fed.ber < 0.3 * learntLinear.ber);
    CHECK(anyContains(linkSummary(fed), "Égaliseur à retour de décision appris (31 prises avant, 4 en retour)"));
    // Feedback taps out of bounds are refused.
    LinkRecipe wrong = dfe;
    wrong.feedbackTaps = 0;
    CHECK(contains(simulateLink(wrong).error, "prises de retour"));
    wrong.feedbackTaps = kMaximumLinkFeedbackTaps + 1;
    CHECK(!simulateLink(wrong).ok());

    // The synchronized receiver through Rice fading (fd = 1 Hz): its loops follow, no error; the truth of its phase
    // turns with the gain, and the line of sight's Doppler shift is said.
    const LinkRecipe followed = examples[20].recipe;
    res = simulateLink(followed);
    REQUIRE(res.ok());
    CHECK(res.sync.frameFound);
    CHECK(res.sync.phaseSlips == 0);
    CHECK(res.bitErrors == 0);
    summary = linkSummary(res);
    CHECK(anyContains(summary, "avec celle du gain du canal au premier symbole"));
    CHECK(anyContains(summary, "la visée directe y ajoute son Doppler") == (res.sync.lineOfSightDopplerHz != 0.0));
    // Faster (fd = 10 Hz), its two passes fit straight lines to a phase that wanders: now and then it ends its
    // preamble on another symmetry of the constellation, and says so.
    std::size_t warned = 0;
    for (std::uint64_t seed : {4u, 6u, 9u, 11u})
    {
        LinkRecipe faster = followed;
        faster.dopplerHz = 10.0;
        faster.fadingSeed = seed;
        const LinkResult r = simulateLink(faster);
        REQUIRE(r.ok());
        for (const std::string &w : r.warnings)
            if (contains(w, "sur une autre symétrie de la constellation")) ++warned;
    }
    CHECK(warned >= 2);
}
