// What lies between a transmitter and a receiver that does not know it (a delay, a clock at another rate, a
// carrier off in frequency and phase), and the receiver that finds it: interpolation, channel, synchronization,
// and the link that uses them.
#include "qacq_test.h"

#include "qacq/analysis/link.h"
#include "qacq/dsp/channel.h"
#include "qacq/dsp/constants.h"
#include "qacq/dsp/interpolation.h"
#include "qacq/dsp/synchronization.h"

#include <algorithm>
#include <cmath>
#include <complex>
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
    return std::any_of(lines.begin(), lines.end(), [&](const std::string &l) { return contains(l, part); });
}

// A carrier 40 Hz off and turned by 30 degrees, a frame 12.37 symbols late, a clock 200 ppm fast.
LinkRecipe throughChannel(LinkRecipe r)
{
    r.frequencyOffsetHz = 40.0;
    r.phaseOffsetDegrees = 30.0;
    r.delaySymbols = 12.37;
    r.clockOffsetPpm = 200.0;
    return r;
}

// The errors counted are binomial: 5 standard deviations, and a margin of the model.
void checkAgainstTheory(const LinkResult &r, const char *label, double margin)
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

TEST_CASE("sync: a band-limited signal read between its samples", "[sync]")
{
    const dsp::SincInterpolator &sinc = dsp::sincInterpolator();
    CHECK(sinc.halfWidth() == 8);
    // Two tones below 0.3 fs: what the samples say between them, to 1e-4 of the amplitude.
    const auto tone = [](double n) {
        return std::sin(dsp::kTwoPi * 0.1 * n) + 0.5 * std::cos(dsp::kTwoPi * 0.23 * n + 1.0);
    };
    std::vector<double> x(400), q(400);
    std::vector<std::complex<double>> z(400);
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        x[i] = tone(static_cast<double>(i));
        q[i] = tone(static_cast<double>(i) + 3.3);
        z[i] = {x[i], q[i]};
    }
    double gap = 0.0, gapComplex = 0.0, gapParts = 0.0;
    for (double p = 50.0; p < 350.0; p += 0.37)
    {
        gap = std::max(gap, std::abs(sinc.at(x, p) - tone(p)));
        const std::complex<double> both = sinc.at(x, q, p);
        gapParts = std::max(gapParts, std::abs(both - std::complex<double>(sinc.at(x, p), sinc.at(q, p))));
        gapComplex = std::max(gapComplex, std::abs(sinc.at(z, p) - both));
    }
    CHECK(gap < 1e-4);
    CHECK(gapParts < 1e-13);
    CHECK(gapComplex < 1e-13);
    // On a sample: the sample; a constant stays one; outside the record, nothing.
    for (const std::size_t i : {std::size_t{20}, std::size_t{133}, std::size_t{379}}) CHECK(std::abs(sinc.at(x, static_cast<double>(i)) - x[i]) < 1e-12);
    const std::vector<double> ones(64, 1.0);
    for (const double p : {20.0, 20.25, 31.5, 40.999}) CHECK(std::abs(sinc.at(ones, p) - 1.0) < 1e-12);
    CHECK(sinc.at(x, -20.0) == 0.0);
    CHECK(sinc.at(x, 1000.0) == 0.0);
    CHECK(sinc.at(std::vector<double>(), 2.0) == 0.0);
}

TEST_CASE("sync: the channel delays, stretches and turns the envelope, and says where each sample comes from", "[sync]")
{
    std::vector<double> i(600), q(600);
    for (std::size_t n = 0; n < i.size(); ++n)
    {
        i[n] = std::sin(dsp::kTwoPi * 0.05 * static_cast<double>(n));
        q[n] = 0.5 * std::cos(dsp::kTwoPi * 0.11 * static_cast<double>(n));
    }
    // Nothing between them: the transmission, then the samples asked for after it.
    dsp::ChannelSpec spec;
    CHECK(spec.identity());
    spec.extraSamples = 7;
    CHECK(!spec.identity());
    dsp::ChannelOutput y = dsp::applyChannel(i, q, spec);
    REQUIRE(y.ok());
    REQUIRE(y.inPhase.size() == 607);
    CHECK(std::equal(i.begin(), i.end(), y.inPhase.begin()));
    CHECK(std::equal(q.begin(), q.end(), y.quadrature.begin()));
    CHECK(y.inPhase[606] == 0.0);
    // Five samples late.
    spec = {};
    spec.delaySamples = 5.0;
    y = dsp::applyChannel(i, q, spec);
    REQUIRE(y.ok());
    REQUIRE(y.inPhase.size() == 605);
    double gap = 0.0;
    for (std::size_t n = 0; n < 600; ++n) gap = std::max(gap, std::abs(y.inPhase[n + 5] - i[n]) + std::abs(y.quadrature[n + 5] - q[n]));
    CHECK(gap < 1e-12);
    CHECK(std::abs(y.inPhase[2]) < 1e-12);
    CHECK(dsp::transmittedPosition(spec, 105.0) == 100.0);
    CHECK(dsp::receivedPosition(spec, 100.0) == 105.0);
    // Half a sample late: the envelope between its samples (away from the ends of the record).
    spec.delaySamples = 0.5;
    y = dsp::applyChannel(i, q, spec);
    REQUIRE(y.ok());
    gap = 0.0;
    for (std::size_t n = 20; n < 580; ++n)
        gap = std::max(gap, std::abs(y.inPhase[n] - std::sin(dsp::kTwoPi * 0.05 * (static_cast<double>(n) - 0.5))));
    CHECK(gap < 1e-4);
    // The carrier 100 Hz off and turned by 30 degrees: each sample turned by 2 pi df n / fs + phi.
    spec = {};
    spec.sampleRateHz = 8000.0;
    spec.frequencyOffsetHz = 100.0;
    spec.phaseOffsetDegrees = 30.0;
    y = dsp::applyChannel(i, q, spec);
    REQUIRE(y.ok());
    REQUIRE(y.inPhase.size() == 600);
    gap = 0.0;
    for (std::size_t n = 0; n < 600; ++n)
    {
        const std::complex<double> turned =
            std::complex<double>(i[n], q[n]) * std::polar(1.0, dsp::kTwoPi * 100.0 * static_cast<double>(n) / 8000.0 + dsp::kPi / 6.0);
        gap = std::max(gap, std::abs(std::complex<double>(y.inPhase[n], y.quadrature[n]) - turned));
    }
    CHECK(gap < 1e-12);
    // A clock 1 000 ppm fast: the record is that much longer, sample n shows the transmission at n / 1.001.
    spec = {};
    spec.clockOffsetPpm = 1000.0;
    y = dsp::applyChannel(i, q, spec);
    REQUIRE(y.ok());
    CHECK(y.inPhase.size() == 601); // ceil(599 * 1.001) + 1
    CHECK_NEAR(dsp::transmittedPosition(spec, 500.5), 500.0, 1e-12, 0.0);
    CHECK_NEAR(dsp::receivedPosition(spec, 500.0), 500.5, 1e-12, 0.0);
    gap = 0.0;
    for (std::size_t n = 20; n < 580; ++n)
        gap = std::max(gap, std::abs(y.inPhase[n] - std::sin(dsp::kTwoPi * 0.05 * static_cast<double>(n) / 1.001)));
    CHECK(gap < 1e-4);
    spec.delaySamples = 3.25;
    for (const double p : {0.0, 17.5, 599.0}) CHECK_NEAR(dsp::transmittedPosition(spec, dsp::receivedPosition(spec, p)), p, 1e-12, 1e-12);
    // What is refused.
    spec = {};
    spec.delaySamples = -1.0;
    CHECK(!dsp::applyChannel(i, q, spec).ok());
    spec = {};
    spec.clockOffsetPpm = 200000.0;
    CHECK(!dsp::applyChannel(i, q, spec).ok());
    spec = {};
    spec.frequencyOffsetHz = std::nan("");
    CHECK(!dsp::applyChannel(i, q, spec).ok());
}

TEST_CASE("sync: the receiver finds the frame, the carrier and the clock, and decides as the coherent one", "[sync]")
{
    // Without noise: no error after the preamble, and what it found is what the channel did.
    LinkRecipe r = throughChannel(defaultLinkRecipe());
    r.receiver = LinkReceiver::Synchronized;
    r.symbols = 6000;
    r.noise = false;
    LinkResult res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(res.bitErrors == 0);
    CHECK(res.skipSymbols == 64); // the preamble
    CHECK(res.skipEndSymbols == 6);
    CHECK(res.comparedBits == (6000 - 64 - 6) * 2);
    CHECK(res.sync.used);
    CHECK(res.sync.frameFound);
    CHECK(res.sync.frameQuality > 0.95);
    CHECK(std::abs(res.sync.delaySymbols - 12.37) < 0.03);
    CHECK(res.sync.trueDelaySymbols == 12.37);
    CHECK(std::abs(res.sync.frequencyOffsetHz - 40.0) < 0.05);
    CHECK(std::abs(res.sync.phaseDegrees - 30.0) < 2.0);
    CHECK(std::abs(res.sync.truePhaseDegrees - 30.0) < 1e-9);
    CHECK(std::abs(res.sync.clockOffsetPpm - 200.0) < 2.0);
    CHECK(std::abs(res.sync.gain - 1.0) < 0.02);
    CHECK(res.sync.timingErrorRms < 0.01);
    CHECK(res.sync.phaseErrorRms < 0.5);
    CHECK(res.sync.phaseSlips == 0);
    // An error for each symbol sent; the receiver goes on deciding to the end of the record (the delay, a few
    // symbols after the transmission).
    CHECK(res.sync.timingError.size() == res.sentSymbols.size());
    CHECK(res.sync.phaseError.size() == res.sync.timingError.size());
    CHECK(res.reception.symbols.size() >= res.sentSymbols.size());
    CHECK(res.sync.rawPoints.size() == res.reception.decisionPoints.size());
    CHECK(res.evmPercent < 2.0);
    CHECK(res.reception.matchedRateHz == 8000.0); // its own rate: 8 samples per symbol
    // The record received: the delay, the clock's stretch and the samples after the end.
    CHECK(res.receivedPassband.size() > res.sentPassband.size() + 12 * 48);
    // Before synchronization the points turn (40 Hz off: a turn every 25 symbols) and slide; after it they sit on
    // the constellation.
    double before = 0.0, after = 0.0;
    for (std::size_t k = 200; k < 400; ++k)
    {
        const std::complex<double> wanted = res.constellation[res.sentSymbols[k]];
        before += std::norm(res.sync.rawPoints[k] - wanted);
        after += std::norm(res.reception.decisionPoints[k] - wanted);
    }
    CHECK(before > 100.0 * after);
    CHECK(anyContains(linkSummary(res), "Canal : porteuse décalée de 40 Hz, tournée de 30°, retard de 12,37 symboles, horloge du récepteur +200 ppm."));
    CHECK(anyContains(linkSummary(res), "Récepteur synchronisé : préambule de 64 symboles connus"));
    CHECK(anyContains(linkSummary(res), "aucun glissement de phase"));
    CHECK(anyContains(linkSummary(res), "Le préambule (64 symboles) et les 6 derniers symboles ne comptent pas."));

    // In noise: the bit error rate of the theory, a little more (what the loops' jitter costs).
    r.noise = true;
    r.ebN0Db = 7.0;
    r.symbols = 20000;
    res = simulateLink(r);
    REQUIRE(res.ok());
    CHECK(res.sync.frameFound);
    CHECK(res.sync.phaseSlips == 0);
    CHECK(std::abs(res.sync.frequencyOffsetHz - 40.0) < 0.1);
    CHECK(std::abs(res.sync.clockOffsetPpm - 200.0) < 3.0);
    checkAgainstTheory(res, "QPSK synchronised, 7 dB", 0.15);

    // The other modulations, each through the same channel (FSK, not coherent: its carrier 10 Hz off only).
    struct Case
    {
        dsp::Modulation modulation;
        dsp::Representation representation;
        dsp::PulseShape pulse;
        const char *label;
    };
    const Case cases[] = {
        {dsp::Modulation::Bpsk, dsp::Representation::RealPassband, dsp::PulseShape::Rectangular, "BPSK rectangular"},
        {dsp::Modulation::Psk8, dsp::Representation::RealPassband, dsp::PulseShape::RootRaisedCosine, "8-PSK"},
        {dsp::Modulation::Qam16, dsp::Representation::ComplexBaseband, dsp::PulseShape::RootRaisedCosine, "16-QAM I/Q"},
        {dsp::Modulation::Qam64, dsp::Representation::RealPassband, dsp::PulseShape::RootRaisedCosine, "64-QAM"},
        {dsp::Modulation::Ask, dsp::Representation::RealPassband, dsp::PulseShape::Rectangular, "ASK"},
        {dsp::Modulation::Fsk, dsp::Representation::RealPassband, dsp::PulseShape::Rectangular, "FSK"},
    };
    for (const Case &c : cases)
    {
        LinkRecipe x = throughChannel(defaultLinkRecipe());
        x.receiver = LinkReceiver::Synchronized;
        x.modulation = c.modulation;
        x.representation = c.representation;
        x.pulse = c.pulse;
        x.symbols = 3000;
        x.noise = false;
        if (c.modulation == dsp::Modulation::Fsk) x.frequencyOffsetHz = 10.0;
        const LinkResult one = simulateLink(x);
        REQUIRE(one.ok());
        if (qacq::test::state().verbose)
            std::printf("      %-18s %zu errors / %zu bits, delay %.3f, %.2f Hz, %.1f ppm\n", c.label, one.bitErrors, one.comparedBits,
                        one.sync.delaySymbols, one.sync.frequencyOffsetHz, one.sync.clockOffsetPpm);
        CHECK(one.sync.frameFound);
        CHECK(one.bitErrors == 0);
        CHECK(one.comparedBits > 2800);
        CHECK(std::abs(one.sync.delaySymbols - 12.37) < 0.1);
        CHECK(std::abs(one.sync.clockOffsetPpm - 200.0) < 10.0);
        if (c.modulation != dsp::Modulation::Fsk) CHECK(std::abs(one.sync.frequencyOffsetHz - 40.0) < 0.2);
    }

    // A point of a curve: frames with their preamble, the errors counted after it.
    LinkRecipe curve = throughChannel(defaultLinkRecipe());
    curve.receiver = LinkReceiver::Synchronized;
    BerSweep sweep;
    sweep.maxBits = 60000;
    const BerPoint point = measureBer(curve, 6.0, sweep, 5);
    CHECK(point.bits > 50000);
    CHECK(point.ber > 0.6 * point.theory);
    CHECK(point.ber < 1.6 * point.theory);
}

TEST_CASE("sync: a channel the ideal receiver undoes; the recipe, its texts, what it refuses", "[sync]")
{
    // The ideal receiver knows the channel: no error without noise, the theory with it, on a carrier and in I/Q.
    for (const dsp::Representation representation : {dsp::Representation::RealPassband, dsp::Representation::ComplexBaseband})
    {
        LinkRecipe r = throughChannel(defaultLinkRecipe());
        r.representation = representation;
        r.symbols = 3000;
        r.noise = false;
        CHECK(linkHasChannel(r));
        LinkResult res = simulateLink(r);
        REQUIRE(res.ok());
        CHECK(res.bitErrors == 0);
        CHECK(!res.sync.used);
        CHECK(res.skipSymbols == 6);
        CHECK(res.evmPercent < 1.0);
        CHECK(res.receivedI.size() > res.sentI.size());
        CHECK(anyContains(linkSummary(res), "Récepteur idéal : il connaît le canal et le défait exactement"));
        r.noise = true;
        r.ebN0Db = 6.0;
        r.symbols = 20000;
        res = simulateLink(r);
        REQUIRE(res.ok());
        checkAgainstTheory(res, "QPSK through a channel, ideal, 6 dB", 0.08);
    }
    CHECK(!linkHasChannel(defaultLinkRecipe()));

    // The recipe as JSON and back; its keys; its text.
    LinkRecipe r = throughChannel(defaultLinkRecipe());
    r.receiver = LinkReceiver::Synchronized;
    r.preambleSymbols = 128;
    r.timingBandwidth = 0.002;
    r.phaseBandwidth = 0.005;
    const model::Json j = toJson(r);
    CHECK(j["frequencyOffset"].toNumber() == 40.0);
    CHECK(j["phaseOffset"].toNumber() == 30.0);
    CHECK(j["delay"].toNumber() == 12.37);
    CHECK(j["clockPpm"].toNumber() == 200.0);
    CHECK(j["receiver"].toString() == "sync");
    CHECK(j["preamble"].toNumber() == 128.0);
    CHECK((toJson(linkRecipeFromJson(j)) == j));
    CHECK(receiverKey(LinkReceiver::Ideal) == "ideal");
    CHECK(receiverKey(LinkReceiver::Synchronized) == "sync");
    CHECK(contains(linkText(r), ", synchronisée"));
    CHECK(!contains(linkText(defaultLinkRecipe()), "synchronisée"));
    // A recipe of before: no channel, the ideal receiver.
    model::Json old = toJson(defaultLinkRecipe());
    for (const char *key : {"frequencyOffset", "phaseOffset", "delay", "clockPpm", "receiver", "preamble", "timingBandwidth", "phaseBandwidth"})
        old.erase(key);
    const LinkRecipe read = linkRecipeFromJson(old);
    CHECK(!linkHasChannel(read));
    CHECK(read.receiver == LinkReceiver::Ideal);
    CHECK(read.preambleSymbols == 64);

    // What is refused, before anything is computed.
    const auto refused = [](LinkRecipe x) { return simulateLink(x).error; };
    LinkRecipe bad = defaultLinkRecipe();
    bad.delaySymbols = -1.0;
    CHECK(contains(refused(bad), "Retard : de 0 à 1 000 symboles."));
    bad = defaultLinkRecipe();
    bad.clockOffsetPpm = 20000.0;
    CHECK(contains(refused(bad), "Écart d'horloge"));
    bad = defaultLinkRecipe();
    bad.frequencyOffsetHz = 1500.0; // beyond the symbol rate
    CHECK(contains(refused(bad), "Décalage de fréquence : au plus le débit de symboles"));
    bad = defaultLinkRecipe();
    bad.receiver = LinkReceiver::Synchronized;
    bad.preambleSymbols = 4;
    CHECK(contains(refused(bad), "Préambule : de 8 à 1 024 symboles."));
    bad.preambleSymbols = 64;
    bad.symbols = 70;
    CHECK(contains(refused(bad), "Le préambule doit laisser des symboles de données"));
    bad = defaultLinkRecipe();
    bad.receiver = LinkReceiver::Synchronized;
    bad.sampleRateHz = 1500.0;
    bad.carrierHz = 300.0;
    bad.representation = dsp::Representation::ComplexBaseband;
    bad.pulse = dsp::PulseShape::Rectangular;
    CHECK(contains(refused(bad), "La synchronisation demande au moins deux échantillons par symbole."));
    // What is only said: a frequency offset the synchronization cannot tell from its opposite.
    LinkRecipe far = defaultLinkRecipe();
    far.receiver = LinkReceiver::Synchronized;
    far.frequencyOffsetHz = 480.0;
    far.symbols = 600;
    const LinkResult said = simulateLink(far);
    CHECK(anyContains(said.warnings, "la synchronisation ne le trouve qu'entre −Rs/2 et +Rs/2"));
}
