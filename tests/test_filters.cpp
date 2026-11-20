#include "qacq_test.h"
#include "signals.h"
#include "fixtures/reference_types.h"

#include "qacq/dsp/constants.h"
#include "qacq/dsp/filter_design.h"
#include "qacq/dsp/filtering.h"

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace qacq::dsp;

namespace
{
FilterFamily familyFromIndex(int i)
{
    switch (i)
    {
        case 0: return FilterFamily::Butterworth;
        case 1: return FilterFamily::ChebyshevI;
        case 2: return FilterFamily::ChebyshevII;
        default: return FilterFamily::Bessel;
    }
}

FilterResponse responseFromIndex(int i)
{
    switch (i)
    {
        case 0: return FilterResponse::LowPass;
        case 1: return FilterResponse::HighPass;
        case 2: return FilterResponse::BandPass;
        default: return FilterResponse::BandStop;
    }
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

IirDesign designFromReference(const qacq::test::IirReference &ref)
{
    IirSpec spec;
    spec.family = familyFromIndex(ref.family);
    spec.response = responseFromIndex(ref.response);
    spec.order = ref.order;
    spec.sampleRateHz = ref.fs;
    spec.frequency1Hz = ref.f1;
    spec.frequency2Hz = ref.f2;
    spec.passbandRippleDb = ref.rippleDb;
    spec.stopbandAttenuationDb = ref.attenuationDb;
    return designIir(spec);
}

double relativeRmsError(const std::vector<double> &value, const std::vector<double> &reference, std::size_t stride)
{
    double error = 0.0, energy = 0.0;
    for (std::size_t i = 0; i < reference.size(); ++i)
    {
        const double d = value[i * stride] - reference[i];
        error += d * d;
        energy += reference[i] * reference[i];
    }
    return std::sqrt(error / std::max(energy, 1e-300));
}

double sectionPoleRadius(const Biquad &s)
{
    // Roots of z^2 + a1 z + a2.
    const Complex disc = std::sqrt(Complex(s.a[1] * s.a[1] - 4.0 * s.a[2], 0.0));
    const Complex r1 = 0.5 * (-s.a[1] + disc), r2 = 0.5 * (-s.a[1] - disc);
    return std::max(std::abs(r1), std::abs(r2));
}
} // namespace

TEST_CASE("IIR designs match SciPy: response, causal and zero-phase filtering", "[iir]")
{
    const auto x = qacq::test::testSignal(3000, 48000.0);
    for (const auto &ref : qacq::test::kIirReferences)
    {
        const IirDesign design = designFromReference(ref);
        REQUIRE(design.ok());
        double worstDb = 0.0;
        for (std::size_t k = 0; k < ref.frequencyHz.size(); ++k)
        {
            if (ref.magnitudeDb[k] < -120.0) continue; // deep stop band: only round-off left
            const double db = 20.0 * std::log10(std::abs(sosResponse(design.sections, ref.frequencyHz[k], ref.fs)));
            worstDb = std::max(worstDb, std::abs(db - ref.magnitudeDb[k]));
        }
        const double causal = relativeRmsError(sosFilter(design.sections, x), ref.sosfilt, 3);
        const double zeroPhase = relativeRmsError(sosFiltFilt(design.sections, x), ref.sosfiltfilt, 3);
        CHECK(worstDb < 1e-7);
        CHECK(causal < 1e-9);
        CHECK(zeroPhase < 1e-9);
        if (worstDb >= 1e-7 || causal >= 1e-9 || zeroPhase >= 1e-9 || qacq::test::state().verbose)
            std::printf("    %-22s |dH| %.2g dB, sosfilt %.2g, sosfiltfilt %.2g\n", ref.name, worstDb, causal, zeroPhase);
        for (const auto &s : design.sections) CHECK(sectionPoleRadius(s) < 1.0);
    }
}

TEST_CASE("Bessel prototype poles match scipy.signal.besselap(norm='mag')", "[iir]")
{
    for (const auto &ref : qacq::test::kBesselPoles)
    {
        IirSpec spec;
        spec.family = FilterFamily::Bessel;
        spec.order = ref.order;
        spec.sampleRateHz = 2.0 * kPi * 1e6; // nearly analog: the digital poles carry the analog ones
        spec.frequency1Hz = 1.0 / (2.0 * kPi); // cut-off 1 rad/s
        const IirDesign design = designIir(spec);
        REQUIRE(design.ok());
        // Map the digital poles back through the inverse bilinear transform.
        std::vector<Complex> analog;
        const double fs2 = 2.0 * spec.sampleRateHz;
        for (const auto &z : design.digital.poles) analog.push_back(fs2 * (z - 1.0) / (z + 1.0));
        std::sort(analog.begin(), analog.end(), [](Complex a, Complex b) {
            const double ia = std::round(a.imag() * 1e9), ib = std::round(b.imag() * 1e9);
            return ia != ib ? ia < ib : a.real() < b.real();
        });
        REQUIRE(analog.size() == ref.real.size());
        for (std::size_t i = 0; i < analog.size(); ++i)
        {
            CHECK_NEAR(analog[i].real(), ref.real[i], 0, 1e-8);
            CHECK_NEAR(analog[i].imag(), ref.imag[i], 0, 1e-8);
        }
    }
}

TEST_CASE("Every low-pass and band-pass the old interface allowed is now right (audit defect 2)", "[iir][audit]")
{
    const double fs = 48000.0;
    int checked = 0;
    for (FilterFamily family : {FilterFamily::Butterworth, FilterFamily::ChebyshevI})
        for (int order = 1; order <= 10; ++order)
            for (double fc : {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0})
            {
                IirSpec spec;
                spec.family = family;
                spec.order = order;
                spec.sampleRateHz = fs;
                spec.frequency1Hz = fc;
                const IirDesign d = designIir(spec);
                REQUIRE(d.ok());
                // Gain at the cut-off: -3.0103 dB (Butterworth) or -ripple (Chebyshev I), as SciPy defines it.
                const double expected = family == FilterFamily::Butterworth ? -10.0 * std::log10(2.0) : -1.0;
                const double db = 20.0 * std::log10(std::abs(sosResponse(d.sections, fc, fs)));
                CHECK_NEAR(db, expected, 0, 1e-7);
                for (const auto &s : d.sections) CHECK(sectionPoleRadius(s) < 1.0);
                // A unit step settles to the DC gain, with no blow-up.
                std::vector<double> step(static_cast<std::size_t>(fs), 1.0);
                const auto y = sosFilter(d.sections, step);
                double peak = 0.0;
                for (double v : y) peak = std::max(peak, std::abs(v));
                CHECK(std::isfinite(peak));
                CHECK(peak < 2.0);
                ++checked;
            }
    for (int order = 1; order <= 6; ++order)
        for (double f1 : {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0})
        {
            IirSpec spec;
            spec.response = FilterResponse::BandPass;
            spec.order = order;
            spec.sampleRateHz = fs;
            spec.frequency1Hz = f1;
            spec.frequency2Hz = 1.4 * f1;
            const IirDesign d = designIir(spec);
            REQUIRE(d.ok());
            const double centre = std::sqrt(f1 * 1.4 * f1);
            // Peak gain 1 at the (pre-warped) geometric centre, -3 dB at both edges.
            const double fc = fs / kPi * std::atan(std::sqrt(std::tan(kPi * f1 / fs) * std::tan(kPi * 1.4 * f1 / fs)));
            CHECK_NEAR(std::abs(sosResponse(d.sections, fc, fs)), 1.0, 0, 1e-9);
            CHECK_NEAR(20.0 * std::log10(std::abs(sosResponse(d.sections, f1, fs))), -10.0 * std::log10(2.0), 0, 1e-7);
            (void)centre;
            ++checked;
        }
    CHECK(checked == 160 + 36);
}

TEST_CASE("Orders up to 32 stay accurate in sections", "[iir]")
{
    IirSpec spec;
    spec.order = 32;
    spec.sampleRateHz = 48000.0;
    spec.frequency1Hz = 30.0;
    const IirDesign d = designIir(spec);
    REQUIRE(d.ok());
    CHECK(d.sections.size() == 16);
    CHECK_NEAR(20.0 * std::log10(std::abs(sosResponse(d.sections, 30.0, 48000.0))), -10.0 * std::log10(2.0), 0, 1e-6);
    CHECK_NEAR(std::abs(sosResponse(d.sections, 1.0, 48000.0)), 1.0, 0, 1e-9);
    IirSpec tooHigh = spec;
    tooHigh.order = 33;
    CHECK(!designIir(tooHigh).ok());
    IirSpec bessel = spec;
    bessel.family = FilterFamily::Bessel;
    bessel.order = 13;
    CHECK(!designIir(bessel).ok());
}

TEST_CASE("Block-by-block filtering equals filtering in one piece", "[iir]")
{
    IirSpec spec;
    spec.family = FilterFamily::ChebyshevII;
    spec.response = FilterResponse::BandStop;
    spec.order = 5;
    spec.sampleRateHz = 48000.0;
    spec.frequency1Hz = 900.0;
    spec.frequency2Hz = 1100.0;
    spec.stopbandAttenuationDb = 60.0;
    const IirDesign d = designIir(spec);
    REQUIRE(d.ok());
    const auto x = qacq::test::testSignal(3000, 48000.0);
    const auto whole = sosFilter(d.sections, x);
    SosState state;
    std::vector<double> pieces;
    for (std::size_t start = 0; start < x.size(); start += 377)
    {
        const auto block = std::span<const double>(x).subspan(start, std::min<std::size_t>(377, x.size() - start));
        const auto y = sosFilter(d.sections, block, &state);
        pieces.insert(pieces.end(), y.begin(), y.end());
    }
    REQUIRE(pieces.size() == whole.size());
    double worst = 0.0;
    for (std::size_t i = 0; i < whole.size(); ++i) worst = std::max(worst, std::abs(pieces[i] - whole[i]));
    CHECK(worst < 1e-15);
}

TEST_CASE("Group delay agrees with the derivative of the phase", "[iir]")
{
    IirSpec spec;
    spec.family = FilterFamily::Bessel;
    spec.order = 4;
    spec.sampleRateHz = 10000.0;
    spec.frequency1Hz = 1000.0;
    const IirDesign d = designIir(spec);
    REQUIRE(d.ok());
    for (double f : {50.0, 400.0, 900.0, 2000.0})
    {
        const double h = 1e-3;
        const double p1 = std::arg(sosResponse(d.sections, f - h, spec.sampleRateHz));
        const double p2 = std::arg(sosResponse(d.sections, f + h, spec.sampleRateHz));
        double dp = p2 - p1;
        if (dp > kPi) dp -= 2.0 * kPi;
        if (dp < -kPi) dp += 2.0 * kPi;
        const double numeric = -dp / (kTwoPi * 2.0 * h / spec.sampleRateHz);
        CHECK_REL(sosGroupDelay(d.sections, f, spec.sampleRateHz), numeric, 1e-6);
    }
}

TEST_CASE("Invalid IIR specifications give a message instead of a filter", "[iir]")
{
    IirSpec spec;
    spec.frequency1Hz = 30000.0; // above fs/2
    CHECK(!designIir(spec).ok());
    CHECK(!designIir(spec).error.empty());
    spec.frequency1Hz = 1000.0;
    spec.response = FilterResponse::BandPass;
    spec.frequency2Hz = 900.0;
    CHECK(!designIir(spec).ok());
    spec.response = FilterResponse::LowPass;
    spec.order = 0;
    CHECK(!designIir(spec).ok());
}

TEST_CASE("FIR coefficients match scipy.signal.firwin", "[fir]")
{
    for (const auto &ref : qacq::test::kFirReferences)
    {
        FirSpec spec;
        spec.response = responseFromIndex(ref.response);
        spec.taps = static_cast<std::size_t>(ref.taps);
        spec.sampleRateHz = ref.fs;
        spec.frequency1Hz = ref.f1;
        spec.frequency2Hz = ref.f2;
        spec.window = windowFromIndex(ref.window);
        spec.kaiserBeta = ref.beta;
        const FirDesign d = designFir(spec);
        REQUIRE(d.ok());
        REQUIRE(d.taps.size() == ref.coefficients.size());
        double worst = 0.0, largest = 0.0;
        for (std::size_t i = 0; i < d.taps.size(); ++i)
        {
            worst = std::max(worst, std::abs(d.taps[i] - ref.coefficients[i]));
            largest = std::max(largest, std::abs(ref.coefficients[i]));
        }
        CHECK(worst < 1e-14 * std::max(1.0, largest) * 10.0);
        if (worst >= 1e-13) std::printf("    %s: worst coefficient error %.3g\n", ref.name, worst);
    }
    FirSpec even;
    even.response = FilterResponse::HighPass;
    even.taps = 64;
    CHECK(!designFir(even).ok());
}

TEST_CASE("FIR filtering: direct and FFT paths agree with a plain convolution", "[fir]")
{
    const auto x = qacq::test::testSignal(5000, 48000.0);
    for (std::size_t taps : {17u, 301u})
    {
        const auto h = qacq::test::lcgNoise(taps, 5u);
        const auto y = firFilter(h, x);
        REQUIRE(y.size() == x.size());
        double worst = 0.0;
        for (std::size_t n = 0; n < x.size(); n += 7)
        {
            double expected = 0.0;
            for (std::size_t k = 0; k < taps && k <= n; ++k) expected += h[k] * x[n - k];
            worst = std::max(worst, std::abs(y[n] - expected));
        }
        CHECK(worst < 1e-12);
    }
}

TEST_CASE("Long records are filtered whatever their length (audit defect 4)", "[fir][audit]")
{
    FirSpec spec;
    spec.taps = 127;
    spec.sampleRateHz = 48000.0;
    spec.frequency1Hz = 1000.0;
    const FirDesign d = designFir(spec);
    REQUIRE(d.ok());
    // 60 s at 48 kHz: refused by the old Filter Designer (366 million operations).
    const std::size_t n = 48000 * 60;
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i)
        x[i] = std::sin(kTwoPi * 200.0 * static_cast<double>(i) / 48000.0) + std::sin(kTwoPi * 9000.0 * static_cast<double>(i) / 48000.0);
    const auto t0 = std::chrono::steady_clock::now();
    const auto y = firFilterAligned(d.taps, x);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(y.size() == n);
    // In the middle of the record the 200 Hz tone passes (gain of the filter at 200 Hz) and 9 kHz is gone.
    const double g200 = std::abs(transferResponse(d.taps, std::vector<double>{1.0}, 200.0, 48000.0));
    double worst = 0.0;
    for (std::size_t i = n / 2; i < n / 2 + 4800; ++i)
        worst = std::max(worst, std::abs(y[i] - g200 * std::sin(kTwoPi * 200.0 * static_cast<double>(i) / 48000.0)));
    CHECK(worst < 1e-3);
    if (qacq::test::state().verbose) std::printf("    60 s at 48 kHz, 127 taps: %.0f ms\n", ms);
}

TEST_CASE("Kaiser design rule matches scipy.signal.kaiserord", "[fir]")
{
    const auto a = estimateKaiser(65.0, 50.0, 1000.0); // width 0.1 of Nyquist
    CHECK(a.taps == 81);
    CHECK_REL(a.beta, 6.20426, 1e-12);
    const auto b = estimateKaiser(40.0, 25.0, 1000.0);
    CHECK(b.taps == 91);
    CHECK_REL(b.beta, 3.3953210522614574, 1e-12);
    const auto c = estimateKaiser(20.0, 100.0, 1000.0);
    CHECK(c.taps == 10);
    CHECK(c.beta == 0.0);
}

TEST_CASE("Transfer-function filtering for typed coefficients", "[iir]")
{
    // y[n] = x[n] + 0.5 y[n-1] : impulse response 0.5^n
    const std::vector<double> b{1.0}, a{1.0, -0.5};
    std::vector<double> impulse(10, 0.0);
    impulse[0] = 1.0;
    const auto y = lfilter(b, a, impulse);
    for (std::size_t n = 0; n < y.size(); ++n) CHECK_REL(y[n], std::pow(0.5, static_cast<double>(n)), 1e-15);
    CHECK_NEAR(transferGroupDelay(std::vector<double>{0.0, 0.0, 1.0}, std::vector<double>{1.0}, 100.0, 1000.0), 2.0, 0, 1e-12);
}
