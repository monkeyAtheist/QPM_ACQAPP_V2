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
        const bool fsk = e.recipe.modulation == dsp::Modulation::Fsk;
        CHECK(noisy.constellation.size() == (fsk ? 2 : std::size_t{1} << dsp::bitsPerSymbol(e.recipe.modulation)));
        CHECK(noisy.reception.decisionPoints.size() == noisy.sentSymbols.size());
        // The eye: the matched output at every sample, and where each symbol is decided (not for FSK).
        CHECK(noisy.reception.matchedI.empty() == fsk);
        if (!fsk)
        {
            REQUIRE(noisy.reception.decisionTimes.size() == noisy.reception.decisionPoints.size());
            // At its instant, the matched output (without noise) is the decision point: the same sum for the root raised
            // cosine; rectangular pulses, the mean over a symbol of the baseband freed of its image, whose transitions
            // the image filter rounds (the decision integrates the raw output): within 0,06.
            const double within = e.recipe.pulse == dsp::PulseShape::RootRaisedCosine ? 1e-12 : 0.06;
            for (std::size_t k = 10; k + 10 < clean.reception.decisionTimes.size(); k += 97)
            {
                const auto at = static_cast<std::size_t>(clean.reception.decisionTimes[k]);
                CHECK(std::abs(clean.reception.matchedI[at] - clean.reception.decisionPoints[k].real()) < within);
                CHECK(std::abs(clean.reception.matchedQ[at] - clean.reception.decisionPoints[k].imag()) < within);
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
