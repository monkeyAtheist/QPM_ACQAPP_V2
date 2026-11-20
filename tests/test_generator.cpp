#include "qacq_test.h"

#include "qacq/dsp/constants.h"
#include "qacq/dsp/fft.h"
#include "qacq/dsp/generator.h"
#include "qacq/dsp/random.h"
#include "qacq/dsp/spectrum.h"
#include "qacq/dsp/tone.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>
#include <vector>

using namespace qacq::dsp;

namespace
{
double mean(const std::vector<double> &x)
{
    double s = 0.0;
    for (double v : x) s += v;
    return s / static_cast<double>(x.size());
}

double variance(const std::vector<double> &x)
{
    const double m = mean(x);
    double s = 0.0;
    for (double v : x) s += (v - m) * (v - m);
    return s / static_cast<double>(x.size());
}

// Amplitudes of the bins of a coherent record (rectangular window): 2 |X_k| / n.
std::vector<double> binAmplitudes(const std::vector<double> &x)
{
    const auto spectrum = rfft(x, x.size());
    std::vector<double> a(spectrum.size());
    for (std::size_t k = 0; k < spectrum.size(); ++k)
        a[k] = (k == 0 ? 1.0 : 2.0) * std::abs(spectrum[k]) / static_cast<double>(x.size());
    return a;
}

std::size_t signChanges(const std::vector<double> &x)
{
    std::size_t count = 0;
    for (std::size_t i = 1; i < x.size(); ++i)
        if ((x[i - 1] < 0.0) != (x[i] < 0.0)) ++count;
    return count;
}
} // namespace

TEST_CASE("Random: published xoshiro256** and splitmix64 sequences", "[generator]")
{
    // splitmix64(1234567) -> 6457827717110365317, 3203168211198807973 (reference implementation),
    // seeding the four words of xoshiro256**; values below computed by the reference algorithms.
    Random random(12345);
    CHECK(random.next() == 0xbe6a36374160d49bull);
    CHECK(random.next() == 0x214aaa0637a688c6ull);
    CHECK(random.next() == 0xf69d16de9954d388ull);
    Random one(1);
    CHECK(one.next() == 0xb3f2af6d0fc710c5ull);
}

TEST_CASE("Random: uniform and normal statistics", "[generator]")
{
    Random random(7);
    const std::size_t n = 400000;
    double su = 0.0, su2 = 0.0, sn = 0.0, sn2 = 0.0, sn4 = 0.0;
    double lo = 1.0, hi = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const double u = random.uniform();
        lo = std::min(lo, u);
        hi = std::max(hi, u);
        su += u;
        su2 += u * u;
        const double g = random.normal();
        sn += g;
        sn2 += g * g;
        sn4 += g * g * g * g;
    }
    const double dn = static_cast<double>(n);
    CHECK(lo >= 0.0);
    CHECK(hi < 1.0);
    CHECK_NEAR(su / dn, 0.5, 0, 0.003);
    CHECK_NEAR(su2 / dn - (su / dn) * (su / dn), 1.0 / 12.0, 0, 0.001);
    CHECK_NEAR(sn / dn, 0.0, 0, 0.008);
    CHECK_NEAR(sn2 / dn, 1.0, 0, 0.012);
    CHECK_NEAR(sn4 / dn, 3.0, 0, 0.08); // kurtosis of a normal law
}

TEST_CASE("Sine: amplitude, frequency, phase and offset", "[generator]")
{
    WaveformSpec spec;
    spec.amplitude = 2.5;
    spec.frequencyHz = 1234.5;
    spec.phaseDegrees = 30.0;
    spec.offset = 0.75;
    spec.sampleRateHz = 48000.0;
    spec.sampleCount = 96000; // 2469 whole periods: the record mean is the offset
    const auto g = generate(spec);
    REQUIRE(g.ok());
    CHECK(g.warnings.empty());
    CHECK_NEAR(g.samples[0], 0.75 + 2.5 * 0.5, 0, 1e-12);
    const ToneAnalysis r = analyzeTone(g.samples, spec.sampleRateHz);
    REQUIRE(r.detected);
    CHECK_REL(r.fundamentalHz, 1234.5, 1e-10);
    CHECK_REL(r.fundamentalAmplitude, 2.5, 1e-9);
    CHECK_NEAR(r.dc, 0.75, 0, 1e-9);
    // sin(x + 30 deg) = cos(x - 60 deg)
    CHECK_NEAR(r.fundamentalPhaseRad, -kPi / 3.0, 0, 1e-8);
}

TEST_CASE("Sine: exact phase, no drift after 10^7 samples", "[generator]")
{
    WaveformSpec spec;
    spec.frequencyHz = 1000.1;
    spec.sampleRateHz = 48000.0;
    spec.sampleCount = 10'000'000;
    const auto g = generate(spec);
    REQUIRE(g.ok());
    const std::size_t last = spec.sampleCount - 1;
    const double exact = std::sin(kTwoPi * std::fmod(1000.1 * static_cast<double>(last), 48000.0) / 48000.0);
    CHECK_NEAR(g.samples[last], exact, 0, 1e-9);
}

TEST_CASE("Square 1.3 kHz at 48 kHz: aliasing when sampled, none from the Fourier series", "[generator]")
{
    // 1 s record: 1300 whole periods, so every component falls exactly on a bin. Harmonic k of the
    // ideal square has amplitude 4 / (pi k) (odd k); k >= 19 is above fs/2 and folds between harmonics.
    WaveformSpec spec;
    spec.waveform = Waveform::Square;
    spec.frequencyHz = 1300.0;
    spec.sampleRateHz = 48000.0;
    spec.sampleCount = 48000;

    spec.bandLimit = BandLimit::Fourier;
    const auto fourier = generate(spec);
    REQUIRE(fourier.ok());
    CHECK(fourier.harmonicsUsed == 18);
    const auto a = binAmplitudes(fourier.samples);
    double strayFourier = 0.0;
    for (std::size_t k = 0; k < a.size(); ++k)
    {
        if (k % 1300 == 0 && k > 0)
        {
            const std::size_t h = k / 1300;
            CHECK_NEAR(a[k], h % 2 ? 4.0 / (kPi * static_cast<double>(h)) : 0.0, 0, 1e-10);
        }
        else
        {
            strayFourier = std::max(strayFourier, a[k]);
        }
    }
    CHECK(strayFourier < 1e-10);

    spec.bandLimit = BandLimit::Sampled;
    const auto sampled = generate(spec);
    const auto s = binAmplitudes(sampled.samples);
    double straySampled = 0.0, foldedPower = 0.0, harmonicPower = 0.0;
    for (std::size_t k = 1; k < s.size(); ++k)
    {
        if (k % 1300 == 0) harmonicPower += 0.5 * s[k] * s[k];
        else
        {
            straySampled = std::max(straySampled, s[k]);
            foldedPower += 0.5 * s[k] * s[k];
        }
    }
    // Largest folded line: the 19th harmonic (24.7 kHz -> 23.3 kHz), 4 / (19 pi), i.e. -25.6 dB.
    CHECK_NEAR(20.0 * std::log10(straySampled / s[1300]), -25.58, 0, 0.3);
    // Folded power relative to the in-band harmonics: the audit figure (about -16 dB).
    const double ratioDb = 10.0 * std::log10(foldedPower / harmonicPower);
    CHECK(ratioDb > -17.5);
    CHECK(ratioDb < -14.5);
}

TEST_CASE("Fourier series: Gibbs overshoot of the square wave (8.95 % of the jump)", "[generator]")
{
    WaveformSpec spec;
    spec.waveform = Waveform::Square;
    spec.frequencyHz = 10.0;
    spec.sampleRateHz = 200000.0;
    spec.sampleCount = 20000; // one period, 9999 harmonics
    spec.bandLimit = BandLimit::Fourier;
    const auto g = generate(spec);
    REQUIRE(g.ok());
    const double peak = *std::max_element(g.samples.begin(), g.samples.end());
    CHECK_NEAR(peak, 1.178979744, 0, 2e-3);
    // Mean value 0 and value 0 at the edges (mid-point of the jump).
    CHECK_NEAR(mean(g.samples), 0.0, 0, 1e-9);
    CHECK_NEAR(g.samples[0], 0.0, 0, 1e-9);
    CHECK_NEAR(g.samples[10000], 0.0, 0, 1e-9);
}

TEST_CASE("Fourier series coefficients match the ideal waveforms (least squares)", "[generator]")
{
    // The truncated series of a waveform must be the best approximation: projecting the ideal
    // waveform, sampled very finely, on cos and sin gives back the coefficients.
    const std::size_t m = 1 << 16;
    for (Waveform w : {Waveform::Square, Waveform::Pulse, Waveform::Triangle, Waveform::Sawtooth})
    {
        WaveformSpec spec;
        spec.waveform = w;
        spec.dutyCycle = 0.3;
        spec.frequencyHz = 1.0;
        spec.sampleRateHz = static_cast<double>(m);
        spec.sampleCount = m;
        spec.phaseDegrees = 0.5 * 360.0 / static_cast<double>(m); // mid-sample: no sample on a jump
        const auto ideal = generate(spec);
        REQUIRE(ideal.ok());
        const FourierSeries series = fourierSeries(w, 0.3, 6);
        CHECK_NEAR(mean(ideal.samples), series.dc, 0, 2e-5);
        for (std::size_t k = 1; k <= 6; ++k)
        {
            double c = 0.0, s = 0.0;
            for (std::size_t i = 0; i < m; ++i)
            {
                const double u = (static_cast<double>(i) + 0.5) / static_cast<double>(m);
                c += ideal.samples[i] * std::cos(kTwoPi * static_cast<double>(k) * u);
                s += ideal.samples[i] * std::sin(kTwoPi * static_cast<double>(k) * u);
            }
            CHECK_NEAR(2.0 * c / static_cast<double>(m), series.cosine[k - 1], 0, 1e-4);
            CHECK_NEAR(2.0 * s / static_cast<double>(m), series.sine[k - 1], 0, 1e-4);
        }
    }
}

TEST_CASE("Pulse train: mean value equals the duty cycle", "[generator]")
{
    WaveformSpec spec;
    spec.waveform = Waveform::Pulse;
    spec.dutyCycle = 0.25;
    spec.amplitude = 2.0;
    spec.frequencyHz = 100.0;
    spec.sampleRateHz = 10000.0;
    spec.sampleCount = 10000; // 100 whole periods of 100 samples
    const auto sampled = generate(spec);
    CHECK_NEAR(mean(sampled.samples), 0.5, 0, 1e-12);
    spec.bandLimit = BandLimit::Fourier;
    const auto fourier = generate(spec);
    CHECK(fourier.harmonicsUsed == 49);
    CHECK_NEAR(mean(fourier.samples), 0.5, 0, 1e-10);
}

TEST_CASE("Triangle and sawtooth: start like a sine, right extremes", "[generator]")
{
    WaveformSpec spec;
    spec.frequencyHz = 100.0;
    spec.sampleRateHz = 10000.0;
    spec.sampleCount = 100;
    spec.waveform = Waveform::Triangle;
    auto g = generate(spec);
    CHECK_NEAR(g.samples[0], 0.0, 0, 1e-12);
    CHECK_NEAR(g.samples[25], 1.0, 0, 1e-12);
    CHECK_NEAR(g.samples[75], -1.0, 0, 1e-12);
    CHECK(g.samples[1] > 0.0);
    spec.waveform = Waveform::Sawtooth;
    g = generate(spec);
    CHECK_NEAR(g.samples[0], 0.0, 0, 1e-12);
    CHECK_NEAR(g.samples[25], 0.5, 0, 1e-12);
    CHECK_NEAR(g.samples[50], -1.0, 0, 1e-12);
    CHECK(g.samples[1] > 0.0);
}

TEST_CASE("Linear and logarithmic chirps: number of cycles", "[generator]")
{
    WaveformSpec spec;
    spec.waveform = Waveform::Chirp;
    spec.frequencyHz = 1000.0;
    spec.chirpEndFrequencyHz = 5000.0;
    spec.sampleRateHz = 48000.0;
    spec.sampleCount = 48000;
    spec.phaseDegrees = 90.0; // cosine start: no zero at the first sample
    auto g = generate(spec);
    REQUIRE(g.ok());
    // Linear: f0 T + (f1 - f0) T / 2 = 3000 cycles -> 6000 sign changes.
    CHECK_NEAR(static_cast<double>(signChanges(g.samples)), 6000.0, 0, 1.0);
    spec.chirpLaw = ChirpLaw::Logarithmic;
    g = generate(spec);
    REQUIRE(g.ok());
    // Logarithmic: (f1 - f0) T / ln(f1 / f0) = 2485.3 cycles.
    CHECK_NEAR(static_cast<double>(signChanges(g.samples)), 2.0 * 4000.0 / std::log(5.0), 0, 1.5);
    // Local frequency at mid-record: f0 sqrt(f1 / f0) = 2236 Hz.
    std::vector<double> middle(g.samples.begin() + 23000, g.samples.begin() + 25000);
    const ToneAnalysis r = analyzeTone(middle, 48000.0);
    REQUIRE(r.detected);
    CHECK_NEAR(r.fundamentalHz, 1000.0 * std::sqrt(5.0), 0, 5.0);
}

TEST_CASE("White noise: level, flat spectrum, reproducible", "[generator]")
{
    WaveformSpec spec;
    spec.waveform = Waveform::WhiteNoise;
    spec.amplitude = 2.0;
    spec.sampleRateHz = 10000.0;
    spec.sampleCount = 400000;
    spec.seed = 42;
    const auto a = generate(spec);
    const auto b = generate(spec);
    REQUIRE(a.ok());
    CHECK(a.samples == b.samples);
    CHECK_NEAR(variance(a.samples), 4.0, 0.01, 0);
    CHECK_NEAR(mean(a.samples), 0.0, 0, 0.02);
    // One-sided density of a variance s^2 noise: 2 s^2 / fs.
    WelchOptions options;
    options.segmentLength = 1024;
    const WelchResult w = welch(a.samples, spec.sampleRateHz, options);
    double inner = 0.0;
    std::size_t count = 0;
    for (std::size_t k = 1; k + 1 < w.psd.size(); ++k)
    {
        inner += w.psd[k];
        ++count;
    }
    CHECK_NEAR(inner / static_cast<double>(count), 2.0 * 4.0 / 10000.0, 0.01, 0);
    const double lowBand = std::accumulate(w.psd.begin() + 1, w.psd.begin() + 101, 0.0) / 100.0;
    const double highBand = std::accumulate(w.psd.end() - 101, w.psd.end() - 1, 0.0) / 100.0;
    CHECK_NEAR(lowBand / highBand, 1.0, 0.05, 0);

    spec.seed = 43;
    const auto c = generate(spec);
    CHECK(c.samples != a.samples);

    spec.noise = NoiseDistribution::Uniform;
    spec.amplitude = 1.0;
    const auto u = generate(spec);
    CHECK(*std::max_element(u.samples.begin(), u.samples.end()) < 1.0);
    CHECK(*std::min_element(u.samples.begin(), u.samples.end()) >= -1.0);
    CHECK_NEAR(variance(u.samples), 1.0 / 3.0, 0.01, 0);
}

TEST_CASE("Step, impulse and constant", "[generator]")
{
    WaveformSpec spec;
    spec.sampleRateHz = 1000.0;
    spec.sampleCount = 50;
    spec.amplitude = 3.0;
    spec.eventTimeSeconds = 0.0102;
    spec.waveform = Waveform::Step;
    auto g = generate(spec);
    CHECK(g.samples[10] == 0.0);
    CHECK(g.samples[11] == 3.0);
    CHECK(g.samples[49] == 3.0);
    spec.waveform = Waveform::Impulse;
    g = generate(spec);
    CHECK(g.samples[10] == 3.0);
    CHECK(std::accumulate(g.samples.begin(), g.samples.end(), 0.0) == 3.0);
    spec.eventTimeSeconds = 1.0;
    g = generate(spec);
    CHECK(g.ok());
    CHECK(g.warnings.size() == 1);
    spec.waveform = Waveform::Dc;
    spec.offset = 0.5;
    g = generate(spec);
    CHECK(g.samples[7] == 3.5);
}

TEST_CASE("Generator: invalid settings are refused, folding is reported", "[generator]")
{
    WaveformSpec spec;
    spec.sampleRateHz = 0.0;
    CHECK(!generate(spec).ok());
    spec.sampleRateHz = 1000.0;
    spec.sampleCount = 0;
    CHECK(!generate(spec).ok());
    spec.sampleCount = 100;
    spec.frequencyHz = -5.0;
    CHECK(!generate(spec).ok());
    spec.frequencyHz = 10.0;
    spec.waveform = Waveform::Square;
    spec.dutyCycle = 1.0;
    CHECK(!generate(spec).ok());
    spec.dutyCycle = 0.5;
    spec.frequencyHz = 600.0;
    const auto folded = generate(spec);
    CHECK(folded.ok());
    CHECK(folded.warnings.size() == 1);
}

TEST_CASE("Fourier composition of sines", "[generator]")
{
    const std::vector<SineComponent> parts{{1.0, 50.0, 0.0, true}, {0.5, 150.0, 90.0, true}, {9.0, 70.0, 0.0, false}};
    const auto x = synthesize(parts, 0.25, 1000.0, 1000);
    REQUIRE(x.size() == 1000);
    CHECK_NEAR(x[0], 0.25 + 0.5, 0, 1e-12);
    const auto a = binAmplitudes(x);
    CHECK_NEAR(a[0], 0.25, 0, 1e-12);
    CHECK_NEAR(a[50], 1.0, 0, 1e-12);
    CHECK_NEAR(a[150], 0.5, 0, 1e-12);
    CHECK_NEAR(a[70], 0.0, 0, 1e-12);
}
