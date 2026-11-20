#include "qacq_test.h"
#include "signals.h"
#include "fixtures/reference_types.h"

#include "qacq/dsp/constants.h"
#include "qacq/dsp/spectrum.h"
#include "qacq/dsp/window.h"

#include <algorithm>
#include <cmath>

using namespace qacq::dsp;

namespace
{
std::vector<double> tone(std::size_t n, double fs, double f, double amplitude, double phase = 0.3)
{
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i) x[i] = amplitude * std::cos(kTwoPi * f * static_cast<double>(i) / fs + phase);
    return x;
}

Window windowFromIndex(int index)
{
    switch (index)
    {
        case 0: return Window::Rectangular;
        case 1: return Window::Hann;
        case 2: return Window::Hamming;
        case 3: return Window::Blackman;
        case 4: return Window::BlackmanHarris;
        case 5: return Window::FlatTop;
        default: return Window::Kaiser;
    }
}

std::size_t argmax(const std::vector<double> &v)
{
    return static_cast<std::size_t>(std::max_element(v.begin(), v.end()) - v.begin());
}
} // namespace

TEST_CASE("Periodic windows: coefficients, coherent gain and ENBW", "[window]")
{
    const auto hann = makeWindow(Window::Hann, 8);
    for (std::size_t i = 0; i < 8; ++i) CHECK_NEAR(hann[i], 0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) / 8.0), 0, 1e-15);

    struct Expected { Window window; double gain; double enbw; };
    const Expected expected[] = {
        {Window::Rectangular, 1.0, 1.0},
        {Window::Hann, 0.5, 1.5},
        {Window::Hamming, 0.54, (0.54 * 0.54 + 0.5 * 0.46 * 0.46) / (0.54 * 0.54)},
        {Window::Blackman, 0.42, 1.726757369614512},
        {Window::BlackmanHarris, 0.35875, (0.35875 * 0.35875 + 0.5 * (0.48829 * 0.48829 + 0.14128 * 0.14128 + 0.01168 * 0.01168)) / (0.35875 * 0.35875)},
        {Window::FlatTop, 0.21557895, 3.770246447443429},
    };
    for (const auto &e : expected)
    {
        const auto w = makeWindow(e.window, 1024);
        CHECK_REL(coherentGain(w), e.gain, 1e-12);
        CHECK_REL(equivalentNoiseBandwidthBins(w), e.enbw, 1e-9);
    }
    const auto symmetric = makeWindow(Window::Hann, 9, WindowSymmetry::Symmetric);
    CHECK_NEAR(symmetric.front(), 0.0, 0, 1e-15);
    CHECK_NEAR(symmetric.back(), 0.0, 0, 1e-15);
    CHECK_NEAR(symmetric[4], 1.0, 0, 1e-15);
}

TEST_CASE("Kaiser window and Bessel I0 match SciPy", "[window]")
{
    CHECK_REL(besselI0(0.0), 1.0, 1e-15);
    CHECK_REL(besselI0(1.0), 1.2660658777520082, 1e-14);
    CHECK_REL(besselI0(8.6), 750.4611595631659, 1e-14);
    CHECK_REL(besselI0(30.0), 781672297823.9775, 1e-13);
    const auto w = makeWindow(Window::Kaiser, 16, WindowSymmetry::Periodic, 8.6);
    CHECK_REL(w[0], 0.0013325139979024198, 1e-12);
    CHECK_REL(w[3], 0.1723884340205331, 1e-12);
}

TEST_CASE("Amplitude spectrum reads a bin-centred tone exactly, any record length", "[spectrum]")
{
    const double fs = 48000.0;
    for (std::size_t n : {4800u, 4801u, 999u})
    {
        const double binWidth = fs / static_cast<double>(n);
        const double f = 100.0 * binWidth;
        for (std::size_t pad : {1u, 4u})
        {
            SpectrumOptions options;
            options.zeroPadFactor = pad;
            const auto s = spectrum(tone(n, fs, f, 0.7), fs, options);
            REQUIRE(s.samplesUsed == n);
            REQUIRE(s.fftSize == n * pad);
            CHECK(argmax(s.amplitude) == 100 * pad);
            CHECK_REL(s.amplitude[100 * pad], 0.7, 1e-11);
            CHECK_REL(s.frequencyHz[100 * pad], f, 1e-12);
        }
    }
    // DC is not doubled.
    const auto dc = spectrum(std::vector<double>(1000, 2.5), 1000.0);
    CHECK_REL(dc.amplitude[0], 2.5, 1e-12);
}

TEST_CASE("Flat-top window reads an off-bin tone within 0.01 dB", "[spectrum]")
{
    const double fs = 48000.0;
    const std::size_t n = 4096;
    SpectrumOptions options;
    options.window = Window::FlatTop;
    for (double offset : {0.0, 0.25, 0.5})
    {
        const double f = (300.0 + offset) * fs / static_cast<double>(n);
        const auto s = spectrum(tone(n, fs, f, 1.0), fs, options);
        const double peak = s.amplitude[argmax(s.amplitude)];
        CHECK(std::abs(20.0 * std::log10(peak)) < 0.01);
    }
}

TEST_CASE("PSD integrates to the mean square (rectangular window)", "[spectrum]")
{
    const auto x = qacq::test::testSignal(3001, 48000.0);
    SpectrumOptions options;
    options.window = Window::Rectangular;
    const auto s = spectrum(x, 48000.0, options);
    double meanSquare = 0.0;
    for (double v : x) meanSquare += v * v;
    meanSquare /= static_cast<double>(x.size());
    double integral = 0.0;
    for (double p : s.psd) integral += p * s.binWidthHz;
    CHECK_REL(integral, meanSquare, 1e-12);
}

TEST_CASE("Long records are analysed whole (audit defect 3)", "[spectrum]")
{
    const std::size_t n = 240000; // 5 s at 48 kHz: the old application used only the last 65 536 samples
    const auto s = spectrum(tone(n, 48000.0, 1000.0, 1.0), 48000.0);
    CHECK(s.samplesUsed == n);
    CHECK(s.fftSize == n);
    CHECK(argmax(s.amplitude) == 5000); // 1000 Hz on the 0.2 Hz grid
    CHECK_REL(s.amplitude[5000], 1.0, 1e-9);
}

TEST_CASE("Complex spectrum: centred axis, every I/Q sample used", "[spectrum]")
{
    const double fs = 48000.0;
    const std::size_t n = 3000; // the old application kept only 2048 of these
    const double f = -16.0 * fs / static_cast<double>(n);
    std::vector<double> i(n), q(n);
    for (std::size_t k = 0; k < n; ++k)
    {
        const double phase = kTwoPi * f * static_cast<double>(k) / fs + 0.2;
        i[k] = 0.4 * std::cos(phase);
        q[k] = 0.4 * std::sin(phase);
    }
    const auto s = complexSpectrum(i, q, fs);
    REQUIRE(s.samplesUsed == n);
    REQUIRE(s.frequencyHz.size() == n);
    CHECK_NEAR(s.frequencyHz[n / 2], 0.0, 0, 1e-12);
    CHECK_REL(s.frequencyHz.front(), -fs / 2.0, 1e-12);
    const std::size_t peak = argmax(s.amplitude);
    CHECK_REL(s.frequencyHz[peak], f, 1e-12);
    CHECK_REL(s.amplitude[peak], 0.4, 1e-11);
}

TEST_CASE("Welch PSD matches scipy.signal.welch", "[spectrum]")
{
    const auto x = qacq::test::testSignal(3000, 48000.0);
    for (const auto &ref : qacq::test::kWelchReferences)
    {
        WelchOptions options;
        options.window = windowFromIndex(ref.window);
        options.segmentLength = static_cast<std::size_t>(ref.segment);
        options.overlap = static_cast<double>(ref.overlap) / static_cast<double>(ref.segment);
        const auto w = welch(x, 48000.0, options);
        REQUIRE(w.psd.size() == ref.psd.size());
        double worst = 0.0;
        for (std::size_t k = 0; k < w.psd.size(); ++k)
            worst = std::max(worst, std::abs(w.psd[k] - ref.psd[k]) / std::max(ref.psd[k], 1e-30));
        CHECK(worst < 1e-9);
        if (worst >= 1e-9) std::printf("    %s: worst relative error %.3g\n", ref.name, worst);
    }
}

TEST_CASE("Welch PSD of white noise", "[spectrum]")
{
    // Uniform noise in [-0.5, 0.5): variance 1/12, one-sided density 2/(12 fs).
    const auto x = qacq::test::lcgNoise(400000, 4242u);
    const auto w = welch(x, 1000.0);
    double mean = 0.0;
    for (std::size_t k = 1; k + 1 < w.psd.size(); ++k) mean += w.psd[k];
    mean /= static_cast<double>(w.psd.size() - 2);
    CHECK_REL(mean, 2.0 / 12.0 / 1000.0, 0.01);
    CHECK(w.segments == (400000 - 2048) / 2048);
}

TEST_CASE("Spectrogram follows a frequency step and spreads its frames over the record", "[spectrum]")
{
    const double fs = 8000.0;
    const std::size_t n = 80000;
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i)
        x[i] = std::sin(kTwoPi * (i < n / 2 ? 500.0 : 1500.0) * static_cast<double>(i) / fs);
    SpectrogramOptions options;
    options.segmentLength = 256;
    options.maximumFrames = 50;
    const auto s = spectrogram(x, fs, options);
    REQUIRE(s.frames >= 40);
    REQUIRE(s.frames <= 50);
    CHECK(s.timeSeconds.back() > 0.9 * static_cast<double>(n) / fs);
    auto peakFrequency = [&](std::size_t frame) {
        std::size_t best = 0;
        for (std::size_t k = 1; k < s.bins; ++k)
            if (s.at(frame, k) > s.at(frame, best)) best = k;
        return s.frequencyHz[best];
    };
    CHECK_NEAR(peakFrequency(1), 500.0, 0, 32.0);
    CHECK_NEAR(peakFrequency(s.frames - 2), 1500.0, 0, 32.0);
}
