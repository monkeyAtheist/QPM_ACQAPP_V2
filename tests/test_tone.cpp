#include "qacq_test.h"
#include "signals.h"

#include "qacq/dsp/constants.h"
#include "qacq/dsp/tone.h"

#include <chrono>
#include <cmath>
#include <cstdint>

using namespace qacq::dsp;

namespace
{
const double kAuditThd = std::sqrt(0.1 * 0.1 + 0.05 * 0.05); // 11.1803 %
} // namespace

TEST_CASE("THD of the audit signal on 1 s (the old application already read it right)", "[tone]")
{
    const double fs = 48000.0, f0 = 1001.37;
    const auto x = qacq::test::harmonicSignal(48000, fs, f0, {1.0, 0.0, 0.1, 0.0, 0.05});
    const auto r = analyzeTone(x, fs);
    REQUIRE(r.detected);
    CHECK_REL(r.fundamentalHz, f0, 1e-11);
    CHECK_REL(r.fundamentalAmplitude, 1.0, 1e-10);
    CHECK_REL(r.thdRatio, kAuditThd, 1e-9);
    REQUIRE(r.harmonics.size() == 10);
    CHECK_REL(r.harmonics[2].amplitude, 0.1, 1e-9);
    CHECK(r.harmonics[1].amplitude < 1e-10);
    // sin(x + 0.3 k) = cos(x + 0.3 k - pi/2)
    CHECK_NEAR(r.fundamentalPhaseRad, 0.3 - kPi / 2.0, 0, 1e-9);
}

TEST_CASE("THD stays right on long records (audit defect 1: 1.1 % read instead of 11.2 %)", "[tone][audit]")
{
    const double fs = 48000.0, f0 = 1001.37;
    for (std::size_t n : {200000u, 480000u, 1000000u}) // 4.2 s, 10 s, 20.8 s
    {
        const auto x = qacq::test::harmonicSignal(n, fs, f0, {1.0, 0.0, 0.1, 0.0, 0.05});
        const auto t0 = std::chrono::steady_clock::now();
        const auto r = analyzeTone(x, fs);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        REQUIRE(r.detected);
        CHECK_REL(r.fundamentalHz, f0, 1e-11);
        CHECK_REL(r.fundamentalAmplitude, 1.0, 1e-9);
        CHECK_REL(r.thdPercent, 100.0 * kAuditThd, 1e-8);
        CHECK(r.samplesUsed == n);
        if (qacq::test::state().verbose) std::printf("    %.0f samples: THD %.6f %% in %.0f ms\n", static_cast<double>(n), r.thdPercent, ms);
    }
}

TEST_CASE("Short record of ten periods, strong harmonics", "[tone]")
{
    const double fs = 48000.0, f0 = 1001.37;
    const auto n = static_cast<std::size_t>(std::lround(10.0 * fs / f0)); // 479 samples
    const auto x = qacq::test::harmonicSignal(n, fs, f0, {1.0, 0.2, 0.1, 0.0, 0.05});
    const auto r = analyzeTone(x, fs);
    REQUIRE(r.detected);
    CHECK_REL(r.fundamentalHz, f0, 1e-9);
    CHECK_REL(r.thdRatio, std::sqrt(0.04 + 0.01 + 0.0025), 1e-7);
}

TEST_CASE("SNR of a tone in white noise", "[tone]")
{
    const double fs = 10000.0;
    const std::size_t n = 100000;
    const auto noise = qacq::test::lcgNoise(n, 2024u); // variance 1/12
    std::vector<double> x(n);
    const double sigma = 0.01;
    for (std::size_t i = 0; i < n; ++i)
        x[i] = std::sin(kTwoPi * 123.4 * static_cast<double>(i) / fs) + noise[i] * sigma * std::sqrt(12.0);
    const auto r = analyzeTone(x, fs);
    REQUIRE(r.detected);
    const double expected = 10.0 * std::log10(0.5 / (sigma * sigma));
    CHECK_NEAR(r.snrDb, expected, 0, 0.1);
    CHECK_NEAR(r.sinadDb, expected, 0, 0.1);
    CHECK_REL(r.fundamentalHz, 123.4, 1e-6);
}

TEST_CASE("SINAD and ENOB of a 12-bit quantised sine", "[tone]")
{
    const double fs = 1.0e6;
    const std::size_t n = 65536;
    const double f = 10007.3;
    const double step = 2.0 / 4096.0; // 12 bits over [-1, 1)
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i)
        x[i] = step * std::round(0.999 * std::sin(kTwoPi * f * static_cast<double>(i) / fs) / step);
    const auto r = analyzeTone(x, fs);
    REQUIRE(r.detected);
    CHECK_NEAR(r.sinadDb, 6.02 * 12.0 + 1.76, 0, 0.5);
    CHECK_NEAR(r.enob, 12.0, 0, 0.1);
}

TEST_CASE("SFDR sees a non-harmonic spur", "[tone]")
{
    const double fs = 48000.0;
    const std::size_t n = 48000;
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i) / fs;
        x[i] = std::sin(kTwoPi * 1000.0 * t) + 1e-3 * std::sin(kTwoPi * 7777.7 * t) + 1e-4 * std::sin(kTwoPi * 2000.0 * t);
    }
    const auto r = analyzeTone(x, fs);
    REQUIRE(r.detected);
    CHECK_NEAR(r.sfdrDb, 60.0, 0, 1.0);
    CHECK_NEAR(r.thdDb, -80.0, 0, 0.01);
}

TEST_CASE("Noise alone is not reported as a tone", "[tone]")
{
    const auto x = qacq::test::lcgNoise(20000, 77u);
    const auto r = analyzeTone(x, 1000.0);
    CHECK(!r.detected);
    CHECK_REL(r.acRms, std::sqrt(1.0 / 12.0), 0.02);
}

TEST_CASE("Search band restricts the fundamental", "[tone]")
{
    const double fs = 48000.0;
    const std::size_t n = 24000;
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i) / fs;
        x[i] = std::sin(kTwoPi * 50.0 * t) + 0.2 * std::sin(kTwoPi * 3300.0 * t);
    }
    ToneOptions options;
    options.minimumFrequencyHz = 1000.0;
    options.harmonicCount = 3;
    const auto r = analyzeTone(x, fs, options);
    REQUIRE(r.detected);
    // The 50 Hz tone is outside the harmonic model: its leakage into the fit is a few 1e-8.
    CHECK_REL(r.fundamentalHz, 3300.0, 1e-6);
    CHECK_REL(r.fundamentalAmplitude, 0.2, 1e-6);
}

TEST_CASE("Harmonic fit is exact on a non-integer number of periods", "[tone]")
{
    const double fs = 1000.0, f = 13.37;
    const std::size_t n = 333;
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i) / fs;
        x[i] = 0.25 + 1.5 * std::cos(kTwoPi * f * t + 0.7) + 0.3 * std::cos(kTwoPi * 2.0 * f * t - 1.1);
    }
    HarmonicFit fit;
    REQUIRE(fitHarmonics(x, fs, f, 4, fit));
    CHECK_REL(fit.dc, 0.25, 1e-10);
    CHECK_REL(std::abs(fit.coefficients[0]), 1.5, 1e-11);
    CHECK_NEAR(std::arg(fit.coefficients[0]), 0.7, 0, 1e-11);
    CHECK_REL(std::abs(fit.coefficients[1]), 0.3, 1e-10);
    CHECK_NEAR(std::arg(fit.coefficients[1]), -1.1, 0, 1e-10);
    CHECK(std::abs(fit.coefficients[2]) < 1e-11);
    CHECK(fit.residualMeanSquare < 1e-20);
}

TEST_CASE("A few periods only: the frequency of least residual (40 samples, 2.5 periods)", "[tone]")
{
    // The "Quelques points" signal of the test session: before, 6.2545 Hz and a THD of 0.07 % for a pure sine.
    std::vector<double> x(40);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = std::sin(kTwoPi * static_cast<double>(i) / 16.0);
    const ToneAnalysis r = analyzeTone(x, 100.0);
    REQUIRE(r.detected);
    CHECK_NEAR(r.fundamentalHz, 6.25, 1e-8, 0.0);
    CHECK_NEAR(r.fundamentalAmplitude, 1.0, 1e-7, 0.0);
    CHECK(r.thdDb < -140.0);
    CHECK(r.snrDb > 140.0);

    // With noise, still the right frequency to well within a bin (2.5 Hz).
    std::vector<double> noisy(64);
    const std::vector<double> noise = qacq::test::lcgNoise(noisy.size(), 99u);
    for (std::size_t i = 0; i < noisy.size(); ++i)
        noisy[i] = 0.8 * std::cos(kTwoPi * 7.3 * static_cast<double>(i) / 100.0 + 0.4) + 0.01 * noise[i];
    const ToneAnalysis n = analyzeTone(noisy, 100.0);
    REQUIRE(n.detected);
    CHECK_NEAR(n.fundamentalHz, 7.3, 2e-3, 0.0);
    CHECK_NEAR(n.fundamentalAmplitude, 0.8, 5e-3, 0.0);
}

TEST_CASE("A long record with a slow wave: found on the whole record (0.1 Hz over ten million samples)", "[tone][audit]")
{
    // The 4th test file: 208 s at 48 kHz. Its first 2^20 samples (21.8 s) hold 2.2 periods, inside their DC
    // lobe: before, 0.1305 Hz was read on the slope of the peak, amplitude 0.0133 instead of 0.3, SNR -27 dB.
    const double fs = 48000.0;
    const std::size_t n = 10000000;
    std::vector<double> x(n);
    std::uint32_t state = 5u;
    for (std::size_t i = 0; i < n; ++i)
    {
        state = 1664525u * state + 1013904223u;
        const double uniform = static_cast<double>(state) / 4294967296.0 - 0.5;
        x[i] = 0.3 * std::sin(kTwoPi * 0.1 * static_cast<double>(i) / fs) + 0.05 * std::sqrt(12.0) * uniform;
    }
    const ToneAnalysis r = analyzeTone(x, fs);
    REQUIRE(r.detected);
    CHECK_NEAR(r.fundamentalHz, 0.1, 1e-4, 0.0);
    CHECK_NEAR(r.fundamentalAmplitude, 0.3, 1e-3, 0.0);
    // Signal 0.045, noise 0.0025: 12.55 dB.
    CHECK_NEAR(r.snrDb, 12.55, 0.0, 0.1);
}
