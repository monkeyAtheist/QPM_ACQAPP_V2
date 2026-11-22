#include "qacq_test.h"

#include "qacq/dsp/channel.h"
#include "qacq/dsp/constants.h"
#include "qacq/dsp/filtering.h"
#include "qacq/dsp/modulation.h"
#include "qacq/dsp/receiver.h"
#include "qacq/dsp/spectrum.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace qacq::dsp;

namespace
{
ReceiverSpec receiverFor(const ModulationSpec &tx)
{
    ReceiverSpec rx;
    rx.type = tx.type;
    rx.representation = tx.representation;
    rx.sampleRateHz = tx.sampleRateHz;
    rx.carrierFrequencyHz = tx.carrierFrequencyHz;
    rx.carrierAmplitude = tx.carrierAmplitude;
    rx.carrierPhaseDegrees = tx.carrierPhaseDegrees;
    rx.symbolRate = tx.symbolRate;
    rx.askLowLevel = tx.askLowLevel;
    rx.fskDeviationHz = tx.fskDeviationHz;
    rx.pulse = tx.pulse;
    rx.rolloff = tx.rolloff;
    rx.pulseSpanSymbols = tx.pulseSpanSymbols;
    rx.amIndex = tx.amIndex;
    rx.frequencyDeviationHz = tx.frequencyDeviationHz;
    rx.phaseDeviationRad = tx.phaseDeviationRad;
    return rx;
}

struct Link
{
    ModulatedSignal tx;
    DigitalReception rx;
    BitErrors errors;
    AwgnResult noise;
};

// Modulates, adds noise at Eb/N0 (no noise if ebN0Db is NaN), receives and counts the bit errors.
Link runLink(const ModulationSpec &spec, double ebN0Db, std::size_t skipBits = 0)
{
    Link link;
    link.tx = modulate(spec);
    if (!link.tx.ok()) return link;
    std::vector<double> passband = link.tx.passband, i = link.tx.inPhase, q = link.tx.quadrature;
    AwgnSpec awgn;
    awgn.valueDb = ebN0Db;
    awgn.bitRate = link.tx.bitRate;
    awgn.seed = 2024;
    if (!std::isnan(ebN0Db))
    {
        link.noise = spec.representation == Representation::RealPassband ? addAwgn(passband, spec.sampleRateHz, awgn)
                                                                         : addAwgn(i, q, spec.sampleRateHz, awgn);
    }
    ReceivedInput input;
    input.passband = passband;
    input.inPhase = i;
    input.quadrature = q;
    link.rx = receiveDigital(input, receiverFor(spec));
    link.errors = countBitErrors(link.tx.bits, link.rx.bits, skipBits);
    return link;
}

// The error count is binomial: accept 5 standard deviations plus a modelling margin.
void checkBer(const Link &link, double theory, const char *label, double margin = 0.05)
{
    const double expected = theory * static_cast<double>(link.errors.compared);
    const double sigma = std::sqrt(expected * (1.0 - theory));
    const double measured = static_cast<double>(link.errors.errors);
    std::printf("      %-34s BER %.3e  theory %.3e  (%.0f errors / %.0f bits)\n", label, link.errors.rate, theory,
                measured, static_cast<double>(link.errors.compared));
    CHECK(link.errors.compared > 0);
    CHECK(std::abs(measured - expected) <= 5.0 * sigma + margin * expected);
}

double rmsError(const std::vector<double> &a, const std::vector<double> &b, std::size_t skip)
{
    double s = 0.0;
    std::size_t count = 0;
    for (std::size_t i = skip; i + skip < std::min(a.size(), b.size()); ++i)
    {
        s += (a[i] - b[i]) * (a[i] - b[i]);
        ++count;
    }
    return std::sqrt(s / static_cast<double>(std::max<std::size_t>(1, count)));
}
} // namespace

TEST_CASE("Payloads: text bits, bytes MSB first, UART 8N1 framing", "[modulation]")
{
    CHECK((bitsFromText("10 1x0") == std::vector<std::uint8_t>{1, 0, 1, 0}));
    const std::uint8_t letter[] = {0x41}; // 'A' = 0100 0001
    CHECK((bitsFromBytesMsbFirst(letter) == std::vector<std::uint8_t>{0, 1, 0, 0, 0, 0, 0, 1}));
    // Start 0, data LSB first 1000 0010, stop 1.
    CHECK((uart8n1(letter) == std::vector<std::uint8_t>{0, 1, 0, 0, 0, 0, 0, 1, 0, 1}));
    const auto bits = randomBits(100000, 9);
    const auto again = randomBits(100000, 9);
    CHECK(bits == again);
    const double ones = static_cast<double>(std::count(bits.begin(), bits.end(), 1));
    CHECK_NEAR(ones / 100000.0, 0.5, 0, 0.008);
}

TEST_CASE("Constellations: BPSK and Gray-coded QPSK of the old application", "[modulation]")
{
    ModulationSpec spec;
    spec.type = Modulation::Qpsk;
    spec.representation = Representation::ComplexBaseband;
    spec.sampleRateHz = 8000.0;
    spec.symbolRate = 1000.0;
    spec.sampleCount = 32;
    spec.bits = {0, 0, 0, 1, 1, 1, 1, 0};
    spec.repeatBits = false;
    const auto q = modulate(spec);
    REQUIRE(q.ok());
    REQUIRE(q.symbols.size() == 4);
    const double h = std::sqrt(0.5);
    CHECK_NEAR(q.symbols[0].real(), h, 0, 1e-15);
    CHECK_NEAR(q.symbols[0].imag(), h, 0, 1e-15);
    CHECK_NEAR(q.symbols[1].real(), -h, 0, 1e-15); // 01 -> (-, +)
    CHECK_NEAR(q.symbols[1].imag(), h, 0, 1e-15);
    CHECK_NEAR(q.symbols[2].real(), -h, 0, 1e-15); // 11 -> (-, -)
    CHECK_NEAR(q.symbols[2].imag(), -h, 0, 1e-15);
    CHECK_NEAR(q.symbols[3].real(), h, 0, 1e-15); // 10 -> (+, -)
    CHECK_NEAR(q.symbols[3].imag(), -h, 0, 1e-15);
    CHECK_NEAR(q.inPhase[9], -h, 0, 1e-15);
    CHECK(q.bitRate == 2000.0);

    spec.type = Modulation::Bpsk;
    spec.bits = {0, 1};
    const auto b = modulate(spec);
    CHECK(b.symbols[0].real() == 1.0);
    CHECK(b.symbols[1].real() == -1.0);
    CHECK(b.warnings.size() == 1); // payload shorter than the record, padded with 0
}

TEST_CASE("Constellations: 8-PSK and square QAM, Gray-coded, unit energy, decided by the nearest point", "[modulation]")
{
    for (Modulation type : {Modulation::Bpsk, Modulation::Qpsk, Modulation::Psk8, Modulation::Qam16, Modulation::Qam64,
                            Modulation::Qam256})
    {
        const auto points = constellation(type);
        const std::size_t m = std::size_t{1} << bitsPerSymbol(type);
        REQUIRE(points.size() == m);
        double energy = 0.0, nearest = 1e9;
        for (const auto &p : points) energy += std::norm(p);
        CHECK_NEAR(energy / static_cast<double>(m), 1.0, 0, 1e-12);
        for (std::size_t a = 0; a < m; ++a)
            for (std::size_t b = a + 1; b < m; ++b) nearest = std::min(nearest, std::abs(points[a] - points[b]));
        // Gray: two points at the least distance differ by one bit; every point decided as itself, and still when
        // moved by 0.49 of that distance in any direction.
        std::size_t pairs = 0;
        for (std::size_t a = 0; a < m; ++a)
        {
            CHECK(decideSymbol(type, points[a]) == a);
            for (int turn = 0; turn < 16; ++turn)
                CHECK(decideSymbol(type, points[a] + std::polar(0.49 * nearest, kTwoPi * turn / 16.0)) == a);
            for (std::size_t b = a + 1; b < m; ++b)
            {
                if (std::abs(points[a] - points[b]) > nearest * (1.0 + 1e-9)) continue;
                ++pairs;
                CHECK(std::popcount(a ^ b) == 1);
            }
        }
        CHECK(pairs >= m - 1);
    }
    // 8-PSK: 000 at 0°, 001 at 45°, 011 at 90°… 100 at 315°; 16-QAM: the first two bits on I (00 at -3, 01 at -1,
    // 11 at +1, 10 at +3), the last two on Q.
    const auto psk = constellation(Modulation::Psk8);
    CHECK_NEAR(std::arg(psk[0b011]), kPi / 2.0, 0, 1e-15);
    CHECK_NEAR(std::arg(psk[0b100]), -kPi / 4.0, 0, 1e-15);
    const auto qam = constellation(Modulation::Qam16);
    const double unit = 1.0 / std::sqrt(10.0);
    CHECK_NEAR(qam[0b0000].real(), -3.0 * unit, 0, 1e-15);
    CHECK_NEAR(qam[0b0000].imag(), -3.0 * unit, 0, 1e-15);
    CHECK_NEAR(qam[0b1101].real(), unit, 0, 1e-15);
    CHECK_NEAR(qam[0b1101].imag(), -unit, 0, 1e-15);
    CHECK_NEAR(qam[0b1001].real(), 3.0 * unit, 0, 1e-15);
    CHECK(decideSymbol(Modulation::Qam16, {10.0, -10.0}) == 0b1000); // far out: the corner
    CHECK(decideSymbol(Modulation::Ask, {0.61, 0.0}, 0.2) == 1);
    CHECK(decideSymbol(Modulation::Ask, {0.59, 0.0}, 0.2) == 0);
    CHECK(constellation(Modulation::Fsk).empty());
    for (std::size_t v = 0; v < 300; ++v) CHECK(grayDecode(grayCode(v)) == v);
}

TEST_CASE("Error-free links: every modulation, passband and baseband, odd ratios", "[modulation]")
{
    for (Representation representation : {Representation::RealPassband, Representation::ComplexBaseband})
    {
        for (Modulation type : {Modulation::Ask, Modulation::Fsk, Modulation::Bpsk, Modulation::Qpsk, Modulation::Psk8,
                                Modulation::Qam16, Modulation::Qam64, Modulation::Qam256})
        {
            ModulationSpec spec;
            spec.type = type;
            spec.representation = representation;
            spec.sampleRateHz = 48000.0;
            spec.symbolRate = 1300.0; // 36.92 samples per symbol
            spec.fskDeviationHz = 650.0;
            spec.carrierFrequencyHz = 11111.0;
            spec.carrierPhaseDegrees = 37.0;
            spec.carrierAmplitude = 0.4;
            spec.askLowLevel = 0.2;
            spec.sampleCount = 48000;
            spec.bits = randomBits(5000, 3);
            const Link link = runLink(spec, std::nan(""));
            REQUIRE(link.tx.ok());
            REQUIRE(link.rx.ok());
            CHECK(link.errors.compared >= 1300);
            CHECK(link.errors.errors == 0);
            if (link.errors.errors != 0)
                std::printf("      %s %s: %.0f errors\n", modulationName(type).data(),
                            representation == Representation::RealPassband ? "passband" : "baseband",
                            static_cast<double>(link.errors.errors));
        }
    }
}

TEST_CASE("AWGN: requested Eb/N0 and SNR are the measured ones", "[modulation]")
{
    std::vector<double> x(1'000'000);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = std::sin(0.01 * static_cast<double>(i)); // power 1/2
    auto noisy = x;
    AwgnSpec spec;
    spec.level = AwgnSpec::Level::SnrDb;
    spec.valueDb = 10.0;
    AwgnResult r = addAwgn(noisy, 1000.0, spec);
    REQUIRE(r.ok());
    CHECK_NEAR(r.signalPower, 0.5, 1e-3, 0);
    CHECK_NEAR(r.noiseVariance, r.signalPower / 10.0, 1e-12, 0);
    CHECK_NEAR(r.measuredSnrDb, 10.0, 0, 0.02);

    noisy = x;
    spec.level = AwgnSpec::Level::EbN0Db;
    spec.valueDb = 7.0;
    spec.bitRate = 100.0;
    r = addAwgn(noisy, 1000.0, spec);
    // Eb = 0.5 / 100, N0 = Eb / 10^0.7, variance N0 fs / 2.
    CHECK_NEAR(r.noiseVariance, 0.5 * (r.signalPower / 100.0 / std::pow(10.0, 0.7)) * 1000.0, 1e-12, 0);
    CHECK_NEAR(r.measuredEbN0Db, 7.0, 0, 0.02);

    std::vector<double> i(500000, 1.0), q(500000, 0.0);
    r = addAwgn(i, q, 1000.0, spec);
    CHECK_NEAR(r.noiseVariance, 0.5 * (0.01 / std::pow(10.0, 0.7)) * 1000.0, 1e-12, 0);
    CHECK_NEAR(r.measuredEbN0Db, 7.0, 0, 0.02);

    std::vector<double> zero(100, 0.0);
    CHECK(!addAwgn(zero, 1000.0, spec).ok());
    spec.bitRate = 0.0;
    CHECK(!addAwgn(noisy, 1000.0, spec).ok());
}

TEST_CASE("Bit error rates match the theory (BPSK, QPSK, ASK, FSK)", "[ber]")
{
    ModulationSpec spec;
    spec.sampleRateHz = 16000.0;
    spec.symbolRate = 1000.0;
    spec.carrierFrequencyHz = 3500.0; // 2 fc T = 7: the image term is orthogonal to every symbol
    spec.carrierPhaseDegrees = 20.0;
    spec.bits = randomBits(200000, 11);
    spec.repeatBits = false;

    struct Case
    {
        Modulation type;
        Representation representation;
        double ebN0Db;
        const char *label;
    };
    const Case cases[] = {
        {Modulation::Bpsk, Representation::RealPassband, 6.0, "BPSK passband 6 dB"},
        {Modulation::Bpsk, Representation::ComplexBaseband, 4.0, "BPSK baseband 4 dB"},
        {Modulation::Qpsk, Representation::RealPassband, 6.0, "QPSK passband 6 dB"},
        {Modulation::Qpsk, Representation::ComplexBaseband, 6.0, "QPSK baseband 6 dB"},
        {Modulation::Ask, Representation::RealPassband, 9.0, "ASK on-off passband 9 dB"},
        {Modulation::Fsk, Representation::RealPassband, 10.0, "FSK non-coherent passband 10 dB"},
        {Modulation::Fsk, Representation::ComplexBaseband, 8.0, "FSK non-coherent baseband 8 dB"},
    };
    for (const Case &c : cases)
    {
        spec.type = c.type;
        spec.representation = c.representation;
        spec.fskDeviationHz = 500.0; // tones 1 / T apart: orthogonal
        const std::size_t symbols = 200000 / static_cast<std::size_t>(bitsPerSymbol(c.type));
        spec.sampleCount = symbols * 16;
        const Link link = runLink(spec, c.ebN0Db);
        REQUIRE(link.rx.ok());
        CHECK_NEAR(link.noise.measuredEbN0Db, c.ebN0Db, 0, 0.05);
        checkBer(link, theoreticalBer(c.type, c.ebN0Db), c.label);
    }

    // 2 fc T = 7.4: the energy of each symbol depends on the carrier phase at its start (+-6 % here),
    // a small excess of errors over the theory that fades as fc / Rs grows (receiver.h).
    spec.carrierFrequencyHz = 3700.0;
    spec.type = Modulation::Bpsk;
    spec.representation = Representation::RealPassband;
    spec.sampleCount = 200000 * 16;
    const Link link = runLink(spec, 6.0);
    checkBer(link, theoreticalBer(Modulation::Bpsk, 6.0), "BPSK passband 6 dB, 2 fc T = 7.4", 0.15);
    CHECK(link.errors.rate >= 0.95 * theoreticalBer(Modulation::Bpsk, 6.0));
}

TEST_CASE("Root-raised-cosine pulses: unit energy, no intersymbol interference, BER", "[ber]")
{
    const std::size_t sps = 8;
    const auto h = rootRaisedCosine(0.35, sps, 12);
    REQUIRE(h.size() == 97);
    double energy = 0.0;
    for (double v : h) energy += v * v;
    CHECK_NEAR(energy, 1.0, 0, 1e-12);
    CHECK_NEAR(h.front(), h.back(), 0, 1e-15);
    // Transmit and receive filters together: a raised cosine, zero at the other symbol instants.
    const auto rc = convolve(h, h);
    const std::size_t centre = (rc.size() - 1) / 2;
    CHECK_NEAR(rc[centre], 1.0, 0, 1e-12);
    double isi = 0.0, isiPower = 0.0;
    for (std::size_t k = 1; k * sps <= centre; ++k)
    {
        isi = std::max(isi, std::abs(rc[centre + k * sps]));
        isiPower += 2.0 * rc[centre + k * sps] * rc[centre + k * sps];
    }
    CHECK(isi < 2e-3);           // span 12 (default); span 8 would leave 1 %
    CHECK(isiPower < 1e-5);      // -50 dB
    CHECK(rootRaisedCosine(0.5, 3, 3).size() % 2 == 1); // odd length even when span * sps is odd

    ModulationSpec spec;
    spec.type = Modulation::Bpsk;
    spec.pulse = PulseShape::RootRaisedCosine;
    spec.rolloff = 0.35;
    spec.sampleRateHz = 8000.0;
    spec.symbolRate = 1000.0;
    spec.carrierFrequencyHz = 2100.0;
    spec.bits = randomBits(100000, 5);
    spec.repeatBits = false;
    spec.sampleCount = 100000 * sps;
    for (Representation representation : {Representation::ComplexBaseband, Representation::RealPassband})
    {
        spec.representation = representation;
        const Link clean = runLink(spec, std::nan(""));
        REQUIRE(clean.rx.ok());
        CHECK(clean.errors.errors == 0);
        CHECK_NEAR(clean.tx.occupiedBandwidthHz, 1350.0, 1e-12, 0);
        const Link noisy = runLink(spec, 5.0, 12);
        checkBer(noisy, theoreticalBer(Modulation::Bpsk, 5.0),
                 representation == Representation::RealPassband ? "BPSK RRC passband 5 dB" : "BPSK RRC baseband 5 dB");
    }
    spec.symbolRate = 1100.0;
    CHECK(!modulate(spec).ok()); // 7.27 samples per symbol: refused for RRC
}

TEST_CASE("8-PSK and QAM: bit error rates match the exact theory", "[ber]")
{
    struct Case
    {
        Modulation type;
        Representation representation;
        PulseShape pulse;
        double ebN0Db;
        const char *label;
    };
    const Case cases[] = {
        {Modulation::Psk8, Representation::RealPassband, PulseShape::Rectangular, 10.0, "8-PSK passband 10 dB"},
        {Modulation::Psk8, Representation::ComplexBaseband, PulseShape::RootRaisedCosine, 8.0, "8-PSK RRC baseband 8 dB"},
        {Modulation::Qam16, Representation::RealPassband, PulseShape::RootRaisedCosine, 10.0, "16-QAM RRC passband 10 dB"},
        {Modulation::Qam16, Representation::ComplexBaseband, PulseShape::Rectangular, 8.0, "16-QAM baseband 8 dB"},
        {Modulation::Qam64, Representation::RealPassband, PulseShape::Rectangular, 14.0, "64-QAM passband 14 dB"},
        {Modulation::Qam64, Representation::ComplexBaseband, PulseShape::RootRaisedCosine, 12.0, "64-QAM RRC baseband 12 dB"},
        {Modulation::Qam256, Representation::ComplexBaseband, PulseShape::RootRaisedCosine, 18.0, "256-QAM RRC baseband 18 dB"},
    };
    for (const Case &c : cases)
    {
        ModulationSpec spec;
        spec.type = c.type;
        spec.representation = c.representation;
        spec.pulse = c.pulse;
        spec.rolloff = 0.35;
        spec.sampleRateHz = 16000.0;
        spec.symbolRate = 1000.0;
        spec.carrierFrequencyHz = 3500.0; // 2 fc T = 7
        spec.carrierPhaseDegrees = 20.0;
        const std::size_t symbols = 60000;
        spec.bits = randomBits(symbols * static_cast<std::size_t>(bitsPerSymbol(c.type)), 17);
        spec.repeatBits = false;
        spec.sampleCount = symbols * 16;
        const std::size_t skip = c.pulse == PulseShape::RootRaisedCosine ? 12 * static_cast<std::size_t>(bitsPerSymbol(c.type)) : 0;
        const Link link = runLink(spec, c.ebN0Db, skip);
        REQUIRE(link.rx.ok());
        CHECK_NEAR(link.noise.measuredEbN0Db, c.ebN0Db, 0, 0.05);
        checkBer(link, theoreticalBer(c.type, c.ebN0Db), c.label);
    }
}

TEST_CASE("RRC spectrum stays inside (1 + rolloff) Rs", "[modulation]")
{
    ModulationSpec spec;
    spec.type = Modulation::Qpsk;
    spec.representation = Representation::ComplexBaseband;
    spec.pulse = PulseShape::RootRaisedCosine;
    spec.rolloff = 0.25;
    spec.sampleRateHz = 16000.0;
    spec.symbolRate = 2000.0;
    spec.bits = randomBits(40000, 8);
    spec.sampleCount = 160000;
    const auto tx = modulate(spec);
    REQUIRE(tx.ok());
    // Power outside |f| <= 1.05 (1 + rolloff) Rs / 2, from the complex spectrum.
    const ComplexSpectrum s = complexSpectrum(tx.inPhase, tx.quadrature, spec.sampleRateHz, {});
    double inside = 0.0, outside = 0.0;
    for (std::size_t k = 0; k < s.frequencyHz.size(); ++k)
    {
        const double p = s.psd[k];
        (std::abs(s.frequencyHz[k]) <= 1.05 * 1250.0 ? inside : outside) += p;
    }
    CHECK(outside / (inside + outside) < 1e-3);
}

TEST_CASE("Analog modulations: AM, FM and PM recover the message", "[modulation]")
{
    for (Representation representation : {Representation::RealPassband, Representation::ComplexBaseband})
    {
        for (Modulation type : {Modulation::Am, Modulation::Fm, Modulation::Pm})
        {
            ModulationSpec spec;
            spec.type = type;
            spec.representation = representation;
            spec.sampleRateHz = 48000.0;
            spec.sampleCount = 24000;
            spec.carrierFrequencyHz = 12000.0;
            spec.carrierPhaseDegrees = 45.0;
            spec.messageFrequencyHz = 700.0;
            spec.amIndex = 0.8;
            spec.frequencyDeviationHz = 5000.0;
            spec.phaseDeviationRad = 2.0;
            const auto tx = modulate(spec);
            REQUIRE(tx.ok());
            CHECK(tx.warnings.empty());
            ReceivedInput input{tx.passband, tx.inPhase, tx.quadrature};
            ReceiverSpec rx = receiverFor(spec);
            rx.messageBandwidthHz = 3000.0;
            const auto out = receiveAnalog(input, rx);
            REQUIRE(out.ok());
            CHECK(out.warnings.empty());
            const double error = rmsError(out.message, tx.message, 400);
            if (error >= 2e-3)
                std::printf("      %s %s: rms error %.2e\n", modulationName(type).data(),
                            representation == Representation::RealPassband ? "passband" : "baseband", error);
            CHECK(error < 2e-3);
        }
    }
}

TEST_CASE("Analog modulations: envelope, deviation and warnings", "[modulation]")
{
    ModulationSpec spec;
    spec.type = Modulation::Am;
    spec.representation = Representation::ComplexBaseband;
    spec.sampleRateHz = 10000.0;
    spec.sampleCount = 10000;
    spec.messageFrequencyHz = 100.0;
    spec.carrierAmplitude = 2.0;
    spec.amIndex = 0.5;
    auto tx = modulate(spec);
    REQUIRE(tx.ok());
    const auto [lo, hi] = std::minmax_element(tx.inPhase.begin(), tx.inPhase.end());
    CHECK_NEAR(*hi, 3.0, 0, 1e-12);
    CHECK_NEAR(*lo, 1.0, 0, 1e-12);
    spec.amIndex = 1.2;
    CHECK(modulate(spec).warnings.size() == 1); // overmodulation

    spec.type = Modulation::Fm;
    spec.frequencyDeviationHz = 300.0;
    tx = modulate(spec);
    // Constant envelope, Carson bandwidth 2 (deviation + fm).
    for (std::size_t i = 0; i < tx.inPhase.size(); i += 97)
        CHECK_NEAR(std::hypot(tx.inPhase[i], tx.quadrature[i]), 2.0, 0, 1e-12);
    CHECK_NEAR(tx.occupiedBandwidthHz, 800.0, 1e-12, 0);

    spec.representation = Representation::RealPassband;
    spec.carrierFrequencyHz = 6000.0;
    CHECK(!modulate(spec).ok()); // carrier above fs / 2
    spec.carrierFrequencyHz = 4800.0;
    CHECK(modulate(spec).warnings.size() == 1); // band beyond fs / 2

    spec.type = Modulation::Bpsk;
    spec.bits.clear();
    CHECK(!modulate(spec).ok());
}

TEST_CASE("Theoretical BER of 8-PSK and QAM, exact (against SciPy)", "[modulation]")
{
    // tools: an independent computation in Python (SciPy quad over the angle of the received point for 8-PSK;
    // the closed form of Cho and Yoon, 2002, for square QAM).
    struct Reference
    {
        Modulation type;
        double ebN0Db;
        double ber;
    };
    const Reference references[] = {
        {Modulation::Psk8, 0.0, 0.12269276107850498},     {Modulation::Psk8, 4.0, 0.04589491846553633},
        {Modulation::Psk8, 8.0, 0.0061810560837676325},   {Modulation::Psk8, 12.0, 6.337878823300316e-05},
        {Modulation::Psk8, 16.0, 1.1098699782314876e-09}, {Modulation::Qam16, 4.0, 0.058623737283404406},
        {Modulation::Qam16, 10.0, 0.0017541506178927301}, {Modulation::Qam16, 14.0, 2.763208001687779e-06},
        {Modulation::Qam16, 18.0, 4.5223090048184457e-13}, {Modulation::Qam16, 22.0, 7.738404045698097e-30},
        {Modulation::Qam64, 8.0, 0.05233386284889217},     {Modulation::Qam64, 14.0, 0.0021540037571801227},
        {Modulation::Qam64, 22.0, 4.9744412172460104e-12}, {Modulation::Qam256, 10.0, 0.07859627551807417},
        {Modulation::Qam256, 18.0, 0.0034720959081667047}, {Modulation::Qam256, 22.0, 2.6336099443566805e-05},
    };
    for (const Reference &r : references) CHECK_NEAR(theoreticalBer(r.type, r.ebN0Db), r.ber, 1e-12, 0);
    // Symbols: 8-PSK at 0 dB (SciPy: 1 - P(right sector)); QPSK 1 - (1 - p)^2; 16-QAM per axis 2 (1 - 1/4) Q(r).
    CHECK_NEAR(theoreticalSer(Modulation::Psk8, 0.0), 0.3478008711998929, 1e-12, 0);
    const double p = theoreticalBer(Modulation::Qpsk, 6.0);
    CHECK_NEAR(theoreticalSer(Modulation::Qpsk, 6.0), 1.0 - (1.0 - p) * (1.0 - p), 1e-14, 0);
    const double axis = 1.5 * gaussianQ(std::sqrt(3.0 * 4.0 * 10.0 / 15.0));
    CHECK_NEAR(theoreticalSer(Modulation::Qam16, 10.0), 1.0 - (1.0 - axis) * (1.0 - axis), 1e-14, 0);
    // Far out on the tail, still a number (8-PSK at 30 dB: 1.4e-193), then 0.
    CHECK(theoreticalBer(Modulation::Psk8, 30.0) > 1e-200);
    CHECK(theoreticalBer(Modulation::Psk8, 30.0) < 1e-190);
    // ASK: the low level costs (1 - l)^2 / (1 + l^2) of Eb/N0.
    CHECK_NEAR(theoreticalBer(Modulation::Ask, 10.0, 0.2), gaussianQ(std::sqrt(10.0 * 0.64 / 1.04)), 1e-14, 0);
    CHECK(theoreticalBer(Modulation::Ask, 10.0) == gaussianQ(std::sqrt(10.0)));
}

TEST_CASE("Theoretical BER and Q function", "[modulation]")
{
    CHECK_NEAR(gaussianQ(0.0), 0.5, 0, 1e-15);
    CHECK_NEAR(gaussianQ(3.0), 1.3498980316301e-3, 1e-12, 0);
    CHECK_NEAR(theoreticalBer(Modulation::Bpsk, 9.6), 1.0e-5, 0.03, 0); // the textbook 9.6 dB for 1e-5
    CHECK(theoreticalBer(Modulation::Qpsk, 6.0) == theoreticalBer(Modulation::Bpsk, 6.0));
    CHECK_NEAR(theoreticalBer(Modulation::Fsk, 10.0), 0.5 * std::exp(-5.0), 1e-14, 0);
    CHECK(std::isnan(theoreticalBer(Modulation::Am, 10.0)));
    const std::uint8_t a[] = {0, 1, 1, 0, 1}, b[] = {1, 1, 0, 0, 1, 1};
    const BitErrors e = countBitErrors(a, b, 1);
    CHECK(e.compared == 4);
    CHECK(e.errors == 1);
}
