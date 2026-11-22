#include "qacq_test.h"
#include "signals.h"
#include "fixtures/reference_types.h"

#include "qacq/dsp/constants.h"
#include "qacq/dsp/filter_design.h"
#include "qacq/dsp/filtering.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

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
        case 3: return FilterFamily::Bessel;
        default: return FilterFamily::Elliptic;
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

TEST_CASE("Elliptic filters: equiripple in both bands, the stop band where it is said", "[iir]")
{
    // The special functions, against values of scipy.special.
    const JacobiValues j = jacobiElliptic(0.7, 0.3);
    CHECK_NEAR(j.sn, 0.6323047763108646, 1e-14, 0.0);
    CHECK_NEAR(j.cn, 0.7747197363269298, 1e-14, 0.0);
    CHECK_NEAR(j.dn, 0.9381136396814304, 1e-14, 0.0);
    CHECK_NEAR(ellipticK(0.3), 1.713889448178791, 1e-14, 0.0);
    CHECK_NEAR(ellipticKComplement(1e-10), 12.8992198263876, 1e-13, 0.0);
    CHECK_NEAR(ellipticStopbandEdge(4, 1.0, 40.0), 1.5154840743221603, 1e-12, 0.0);

    for (const int order : {2, 3, 4, 5, 7, 9})
    {
        IirSpec spec;
        spec.family = FilterFamily::Elliptic;
        spec.order = order;
        spec.sampleRateHz = 48000.0;
        spec.frequency1Hz = 2000.0;
        spec.passbandRippleDb = 0.5;
        spec.stopbandAttenuationDb = 60.0;
        const IirDesign d = designIir(spec);
        REQUIRE(d.ok());
        // Pass band: between 0 and −0.5 dB, both reached (equiripple).
        double high = -1e9, low = 1e9;
        for (int i = 0; i <= 4000; ++i)
        {
            const double db = 20.0 * std::log10(std::abs(sosResponse(d.sections, 2000.0 * i / 4000.0, 48000.0)));
            high = std::max(high, db);
            low = std::min(low, db);
        }
        CHECK_NEAR(high, 0.0, 0.0, 1e-6);
        CHECK_NEAR(low, -0.5, 0.0, 1e-3);
        // Stop band: from the edge (the analog one, warped back), never above −60 dB, and −60 dB reached.
        const double edge = 48000.0 / kPi * std::atan(ellipticStopbandEdge(order, 0.5, 60.0) * std::tan(kPi * 2000.0 / 48000.0));
        double worst = -1e9;
        for (int i = 0; i <= 20000; ++i)
        {
            const double f = edge + (24000.0 - edge) * i / 20000.0;
            worst = std::max(worst, 20.0 * std::log10(std::abs(sosResponse(d.sections, f, 48000.0)) + 1e-300));
        }
        CHECK(worst < -60.0 + 1e-6);
        CHECK(worst > -60.0 - 0.05);
        // Zeros on the unit circle.
        for (const auto &z : d.digital.zeros) CHECK_NEAR(std::abs(z), 1.0, 0.0, 1e-9);
    }
    IirSpec bad;
    bad.family = FilterFamily::Elliptic;
    bad.passbandRippleDb = 3.0;
    bad.stopbandAttenuationDb = 2.0;
    CHECK(!designIir(bad).ok());
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

TEST_CASE("Parks-McClellan designs match scipy.signal.remez", "[fir]")
{
    for (const auto &ref : qacq::test::kRemezReferences)
    {
        RemezSpec spec;
        spec.taps = static_cast<std::size_t>(ref.taps);
        spec.sampleRateHz = ref.fs;
        spec.bandsHz = ref.bands;
        spec.desired = ref.desired;
        spec.weights = ref.weights;
        const RemezDesign d = designRemez(spec);
        if (!d.ok()) std::printf("    %s: %s\n", ref.name, d.error.c_str());
        REQUIRE(d.ok());
        REQUIRE(d.taps.size() == ref.coefficients.size());
        double largest = 0.0, gap = 0.0;
        for (std::size_t k = 0; k < d.taps.size(); ++k)
        {
            largest = std::max(largest, std::abs(ref.coefficients[k]));
            gap = std::max(gap, std::abs(d.taps[k] - ref.coefficients[k]));
        }
        // Over the bands, the same response; and the weighted error, the largest over the bands.
        double bandGap = 0.0, weighted = 0.0;
        const std::vector<double> one{1.0};
        for (std::size_t b = 0; b + 1 < ref.bands.size(); b += 2)
        {
            for (int i = 0; i <= 2000; ++i)
            {
                const double f = ref.bands[b] + (ref.bands[b + 1] - ref.bands[b]) * i / 2000.0;
                const double h = std::abs(transferResponse(d.taps, one, f, ref.fs));
                bandGap = std::max(bandGap, std::abs(h - std::abs(transferResponse(ref.coefficients, one, f, ref.fs))));
                weighted = std::max(weighted, ref.weights[b / 2] * std::abs(h - ref.desired[b / 2]));
            }
        }
        // A filter far better than asked (about 160 dB) is where double precision runs out: another libm's
        // cosines, a unit in the last place apart, end the exchange elsewhere (here, the same taps as SciPy to
        // 1e-17); what counts is that it does as well, its weighted error below 1e-6.
        const bool overdesigned = std::string(ref.name).find("overdesigned") != std::string::npos;
        if (overdesigned)
        {
            CHECK(weighted < 1e-6);
        }
        else
        {
            CHECK(bandGap < 1e-7);
            CHECK(gap < 1e-9 * largest);
            CHECK(d.stop == RemezStop::None);
        }
        if ((!overdesigned && (gap >= 1e-9 * largest || bandGap >= 1e-7)) || weighted >= 1e-6 * (overdesigned ? 1.0 : 1e12) ||
            qacq::test::state().verbose)
            std::printf("    %-20s %zu taps, %d iterations, taps %.2g, bands %.2g, weighted error %.2g (δ × %.3f)\n",
                        ref.name, d.taps.size(), d.iterations, gap / largest, bandGap, weighted, weighted / d.deviation);
        // Linear phase.
        for (std::size_t k = 0; k < d.taps.size() / 2; ++k) CHECK(d.taps[k] == d.taps[d.taps.size() - 1 - k]);
    }
    // What cannot be designed is said.
    RemezSpec bad;
    bad.taps = 31;
    bad.sampleRateHz = 1000.0;
    bad.bandsHz = {0.0, 200.0, 150.0, 500.0}; // overlapping
    bad.desired = {1.0, 0.0};
    CHECK(!designRemez(bad).ok());
    bad.bandsHz = {0.0, 100.0, 150.0, 600.0}; // beyond fs/2
    CHECK(!designRemez(bad).ok());
    bad.bandsHz = {0.0, 100.0, 150.0, 500.0};
    bad.desired = {1.0};
    CHECK(!designRemez(bad).ok());
    bad.desired = {1.0, 0.0};
    bad.taps = 2;
    CHECK(!designRemez(bad).ok());
}

TEST_CASE("Parks-McClellan: long filters SciPy cannot design, and where precision runs out", "[fir]")
{
    // 525 taps, about 110 dB: the equally spaced initial guess levels δ at 8e-17, the rounding; the exchange
    // (and scipy.signal.remez: "Failure to converge at iteration 2") stops there. From the reference of a
    // design half as long it converges, equiripple.
    RemezSpec spec;
    spec.taps = 525;
    spec.sampleRateHz = 1.0;
    spec.bandsHz = {0.0, 0.09508425777502369, 0.10893255573391461, 0.5};
    spec.desired = {0.0, 1.0};
    spec.weights = {3.761190803041479, 1.0};
    const RemezDesign d = designRemez(spec);
    REQUIRE(d.ok());
    CHECK(d.stop == RemezStop::None);
    CHECK(d.stopped.empty());
    CHECK(d.deviation > 1e-7);
    double weighted = 0.0;
    const std::vector<double> one{1.0};
    for (std::size_t b = 0; b < 2; ++b)
    {
        for (int i = 0; i <= 20000; ++i)
        {
            const double f = spec.bandsHz[2 * b] + (spec.bandsHz[2 * b + 1] - spec.bandsHz[2 * b]) * i / 20000.0;
            weighted = std::max(weighted, spec.weights[b] * std::abs(std::abs(transferResponse(d.taps, one, f, 1.0)) -
                                                                    spec.desired[b]));
        }
    }
    // Equiripple: δ on the grid, a little more between its points (7 %, near the edge of the narrow last lobe;
    // 0,4 % with twice the density).
    CHECK(weighted < 1.1 * d.deviation);
    CHECK(weighted > 0.99 * d.deviation);
    if (qacq::test::state().verbose)
        std::printf("    525 taps: %d iterations, δ %.3g, weighted error %.3g\n", d.iterations, d.deviation, weighted);

    // 1001 taps for a transition of a tenth of fs: its best error would be far below the rounding. It stops,
    // says so, and gives the taps all the same.
    RemezSpec far = spec;
    far.taps = 1001;
    far.bandsHz = {0.0, 0.15, 0.25, 0.5};
    far.desired = {1.0, 0.0};
    far.weights = {1.0, 1.0};
    const RemezDesign e = designRemez(far);
    CHECK(e.stop == RemezStop::Rounding);
    CHECK(!e.stopped.empty());

    // Too few iterations allowed (the low-pass of 51 taps needs 7): stopped, said, the taps given.
    RemezSpec hurried;
    hurried.taps = 51;
    hurried.sampleRateHz = 10000.0;
    hurried.bandsHz = {0.0, 1000.0, 1500.0, 5000.0};
    hurried.desired = {1.0, 0.0};
    hurried.maximumIterations = 3;
    const RemezDesign h = designRemez(hurried);
    CHECK(h.ok());
    CHECK(h.stop == RemezStop::Iterations);
    CHECK(h.iterations == 3);
    CHECK(!h.stopped.empty());
}

TEST_CASE("Zero-phase filtering with each of SciPy's edge extensions", "[iir]")
{
    for (const auto &ref : qacq::test::kFiltFiltReferences)
    {
        const std::string padtype = ref.padtype;
        const EdgeExtension extension = padtype == "odd" ? EdgeExtension::Odd
                                        : padtype == "even" ? EdgeExtension::Even
                                        : padtype == "constant" ? EdgeExtension::Constant
                                                                : EdgeExtension::None;
        std::vector<double> y;
        if (!ref.sos.empty())
        {
            SosCascade sections;
            for (std::size_t i = 0; i + 5 < ref.sos.size(); i += 6)
                sections.push_back(Biquad{{ref.sos[i], ref.sos[i + 1], ref.sos[i + 2]}, {1.0, ref.sos[i + 4], ref.sos[i + 5]}});
            y = sosFiltFilt(sections, ref.x, -1, extension);
        }
        else
        {
            y = firFiltFilt(ref.taps, ref.x, -1, extension);
        }
        REQUIRE(y.size() == ref.y.size());
        double largest = 0.0, gap = 0.0;
        for (std::size_t i = 0; i < y.size(); ++i)
        {
            largest = std::max(largest, std::abs(ref.y[i]));
            gap = std::max(gap, std::abs(y[i] - ref.y[i]));
        }
        CHECK(gap < 1e-11 * largest);
        if (gap >= 1e-11 * largest || qacq::test::state().verbose)
            std::printf("    %-14s %-9s gap %.2g\n", ref.name, ref.padtype, gap / largest);
    }
}

TEST_CASE("Zero-phase filtering with a predicted extension: no burst at the ends", "[iir]")
{
    // What an endless record would give, from a longer one filtered with its ends far away.
    const double fs = 48000.0;
    const auto run = [fs](auto signalAt, const SosCascade &sections, std::vector<double> &shortOut, std::vector<double> &truth,
                          EdgeExtension extension) {
        const std::size_t n = 960, before = 2400;
        std::vector<double> lengthy(n + 2 * before), x(n);
        for (std::size_t i = 0; i < lengthy.size(); ++i)
            lengthy[i] = signalAt((static_cast<double>(i) - static_cast<double>(before)) / fs);
        for (std::size_t i = 0; i < n; ++i) x[i] = lengthy[before + i];
        const std::vector<double> whole = sosFiltFilt(sections, lengthy);
        truth.assign(whole.begin() + static_cast<std::ptrdiff_t>(before), whole.begin() + static_cast<std::ptrdiff_t>(before + n));
        shortOut = sosFiltFilt(sections, x, -1, extension);
    };
    const auto edgeError = [](const std::vector<double> &y, const std::vector<double> &truth) {
        double error = 0.0, largest = 0.0;
        for (std::size_t i = 0; i < y.size(); ++i)
        {
            largest = std::max(largest, std::abs(truth[i]));
            if (i < 200 || i + 200 >= y.size()) error = std::max(error, std::abs(y[i] - truth[i]));
        }
        return error / largest;
    };
    IirSpec band;
    band.order = 10;
    band.response = FilterResponse::BandPass;
    band.sampleRateHz = fs;
    band.frequency1Hz = 10500.0;
    band.frequency2Hz = 15000.0;
    const SosCascade bp = designIir(band).sections;
    IirSpec low;
    low.sampleRateHz = fs;
    low.frequency1Hz = 1000.0;
    const SosCascade lp = designIir(low).sections;
    // An AM that starts on a crest of its carrier (as the generator's): the odd extension of SciPy makes a burst
    // five times the signal filtered; the predicted one, nothing to see.
    const auto am = [](double t) { return (1.0 + 0.5 * std::sin(kTwoPi * 500.0 * t)) * std::cos(kTwoPi * 10000.0 * t); };
    const auto ramp = [](double t) { return 250.0 * t + std::sin(kTwoPi * 200.0 * t); };
    const auto square = [](double t) { return std::sin(kTwoPi * 1000.0 * t + 0.3) >= 0.0 ? 1.0 : -1.0; };
    std::vector<double> y, truth;
    run(am, bp, y, truth, EdgeExtension::Predicted);
    const double predictedAm = edgeError(y, truth);
    run(am, bp, y, truth, EdgeExtension::Odd);
    const double oddAm = edgeError(y, truth);
    CHECK(predictedAm < 1e-3);
    CHECK(oddAm > 2.0);
    run(ramp, lp, y, truth, EdgeExtension::Predicted);
    const double predictedRamp = edgeError(y, truth);
    CHECK(predictedRamp < 1e-3);
    run(square, lp, y, truth, EdgeExtension::Predicted);
    const double predictedSquare = edgeError(y, truth);
    CHECK(predictedSquare < 0.02);
    if (qacq::test::state().verbose)
        std::printf("    ends, relative error: AM predicted %.2g (odd %.2g), ramp %.2g, square %.2g\n", predictedAm, oddAm,
                    predictedRamp, predictedSquare);
    // At rest near the ends: at rest beyond them, whatever the middle holds.
    std::vector<double> impulse(501, 0.0);
    impulse[250] = 1.0;
    const std::vector<double> extended = extendEdges(impulse, 300, EdgeExtension::Predicted);
    REQUIRE(extended.size() == 1101);
    for (std::size_t i = 0; i < 300; ++i)
    {
        CHECK(extended[i] == 0.0);
        CHECK(extended[extended.size() - 1 - i] == 0.0);
    }
    // A FIR as well.
    const FirDesign fir = designFir([] {
        FirSpec s;
        s.taps = 63;
        s.sampleRateHz = 48000.0;
        s.response = FilterResponse::LowPass;
        s.frequency1Hz = 12000.0;
        return s;
    }());
    std::vector<double> x(960);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = am(static_cast<double>(i) / fs);
    const std::vector<double> z = firFiltFilt(fir.taps, x, -1, EdgeExtension::Predicted);
    REQUIRE(z.size() == x.size());
    // Below 12 kHz everything passes: what comes out is what went in, ends included.
    double gap = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) gap = std::max(gap, std::abs(z[i] - x[i]));
    CHECK(gap < 0.01);
}

TEST_CASE("Linear prediction: a sum of sinusoids goes on, nothing grows, what fades out ends at zero", "[iir]")
{
    std::vector<double> h(512);
    const auto tones = [](double n) {
        return std::sin(kTwoPi * 0.01 * n) + 0.5 * std::cos(kTwoPi * 0.037 * n + 0.3) + 0.25 * std::sin(kTwoPi * 0.11 * n);
    };
    for (std::size_t i = 0; i < h.size(); ++i) h[i] = tones(static_cast<double>(i));
    const std::vector<double> p = predictLinear(h, 4000);
    REQUIRE(p.size() == 4000);
    double gap = 0.0;
    for (std::size_t i = 0; i < p.size(); ++i) gap = std::max(gap, std::abs(p[i] - tones(static_cast<double>(h.size() + i))));
    CHECK(gap < 1e-6);
    // A constant goes on; zeros stay zeros; a single sample is repeated; nothing asked, nothing given.
    const std::vector<double> three(100, 3.0);
    for (double v : predictLinear(three, 50)) CHECK(std::abs(v - 3.0) < 1e-12);
    for (double v : predictLinear(std::vector<double>(64, 0.0), 50)) CHECK(v == 0.0);
    for (double v : predictLinear(std::vector<double>{2.5}, 5)) CHECK(v == 2.5);
    CHECK(predictLinear(three, 0).empty());
    CHECK(predictLinear(std::vector<double>{}, 3) == std::vector<double>(3, 0.0));
    // Noise: what is predicted stays within what was seen.
    std::vector<double> noise(2048);
    std::uint32_t state = 12345;
    double swing = 0.0;
    for (double &v : noise)
    {
        state = 1664525u * state + 1013904223u;
        v = static_cast<double>(state) / 4294967296.0 - 0.5;
        swing = std::max(swing, std::abs(v));
    }
    for (double v : predictLinear(noise, 20000))
    {
        CHECK(std::isfinite(v));
        CHECK(std::abs(v) <= 4.0 * swing);
    }
    // A damped oscillation dies out, then is exactly zero: no denormal (a hundred times slower to compute) over
    // the 300 000 samples a very low cut-off can ask for.
    std::vector<double> damped(1024);
    for (std::size_t i = 0; i < damped.size(); ++i)
        damped[i] = std::pow(0.995, static_cast<double>(i)) * std::sin(kTwoPi * 0.02 * static_cast<double>(i));
    const std::vector<double> dying = predictLinear(damped, 300000);
    REQUIRE(dying.size() == 300000);
    CHECK(std::abs(dying[0] - std::pow(0.995, 1024.0) * std::sin(kTwoPi * 0.02 * 1024.0)) < 1e-9);
    CHECK(dying.back() == 0.0);
    std::size_t denormals = 0;
    for (double v : dying) denormals += v != 0.0 && std::abs(v) < 1e-300 ? 1 : 0;
    CHECK(denormals == 0);
}

TEST_CASE("How long a cascade rings, from its poles", "[iir]")
{
    IirSpec band;
    band.order = 10;
    band.response = FilterResponse::BandPass;
    band.sampleRateHz = 48000.0;
    band.frequency1Hz = 10500.0;
    band.frequency2Hz = 15000.0;
    const SosCascade bp = designIir(band).sections;
    const std::size_t ring = sosRingLength(bp);
    // Its impulse response, simulated, falls to 1e-6 of its peak after 334 samples.
    CHECK(ring > 250);
    CHECK(ring < 450);
    // No poles (a FIR in sections): a few samples; a pole on or outside the circle: the maximum.
    const SosCascade fir{Biquad{{1.0, 2.0, 1.0}, {1.0, 0.0, 0.0}}};
    CHECK(sosRingLength(fir) == 3);
    const SosCascade unstable{Biquad{{1.0, 0.0, 0.0}, {1.0, -2.1, 1.1}}}; // poles at 1 and 1,1
    CHECK(sosRingLength(unstable, 1e-6, 5000) == 5000);
}

namespace
{
FilterTemplate templateFromReference(const qacq::test::OrderReference &ref)
{
    FilterTemplate t;
    t.response = responseFromIndex(ref.response);
    t.sampleRateHz = ref.fs;
    t.passHz = {ref.pass[0], ref.pass.size() > 1 ? ref.pass[1] : 0.0};
    t.stopHz = {ref.stop[0], ref.stop.size() > 1 ? ref.stop[1] : 0.0};
    t.passbandLossDb = ref.passbandLossDb;
    t.stopbandAttenuationDb = ref.stopbandAttenuationDb;
    return t;
}

// Whether sections hold a template, on a dense grid of each band (in logarithm over more than a decade), to 1e-6:
// over the pass bands between −Ap dB and 0 dB, over the stop bands under −As dB.
bool holdsTemplate(const SosCascade &sections, const FilterTemplate &t)
{
    const double fs = t.sampleRateHz, n = 0.5 * fs;
    const auto &p = t.passHz;
    const auto &s = t.stopHz;
    std::vector<std::array<double, 2>> pass, stop;
    switch (t.response)
    {
        case FilterResponse::LowPass: pass = {{0.0, p[0]}}; stop = {{s[0], n}}; break;
        case FilterResponse::HighPass: stop = {{0.0, s[0]}}; pass = {{p[0], n}}; break;
        case FilterResponse::BandPass: stop = {{0.0, s[0]}, {s[1], n}}; pass = {{p[0], p[1]}}; break;
        case FilterResponse::BandStop: pass = {{0.0, p[0]}, {p[1], n}}; stop = {{s[0], s[1]}}; break;
    }
    const auto each = [&](const std::array<double, 2> &band, auto &&f) {
        const bool logarithmic = band[0] > 0.0 && band[1] > 20.0 * band[0];
        for (int i = 0; i <= 20000; ++i)
        {
            const double x = i / 20000.0;
            f(std::abs(sosResponse(sections, logarithmic ? band[0] * std::pow(band[1] / band[0], x) : band[0] + (band[1] - band[0]) * x, fs)));
        }
    };
    const double passLow = std::pow(10.0, -t.passbandLossDb / 20.0), stopHigh = std::pow(10.0, -t.stopbandAttenuationDb / 20.0);
    bool ok = true;
    for (const auto &b : pass) each(b, [&](double m) { ok = ok && m >= passLow * (1.0 - 1e-6) && m <= 1.0 + 1e-6; });
    for (const auto &b : stop) each(b, [&](double m) { ok = ok && m <= stopHigh * (1.0 + 1e-6); });
    return ok;
}
} // namespace

TEST_CASE("The least order for a template matches buttord, cheb1ord, cheb2ord and ellipord", "[iir]")
{
    for (const auto &ref : qacq::test::kOrderReferences)
    {
        const FilterTemplate t = templateFromReference(ref);
        const FilterFamily family = familyFromIndex(ref.family);
        const IirOrder o = minimumIirOrder(family, t);
        REQUIRE(o.ok());
        CHECK(o.order == ref.order);
        CHECK(o.exactOrder <= o.order);
        CHECK(o.exactOrder > o.order - 1);
        // The frequencies as SciPy gives them; a band-stop's edges moved by a bounded minimisation (to its tolerance,
        // 1e-5 of tan(π f/fs): another libm may stop it elsewhere within that).
        const double tolerance = t.response == FilterResponse::BandStop ? 1e-4 : 1e-10;
        const double e1 = std::abs(o.frequency1Hz - ref.wn[0]) / ref.wn[0];
        const double e2 = ref.wn.size() > 1 ? std::abs(o.frequency2Hz - ref.wn[1]) / ref.wn[1] : 0.0;
        CHECK(e1 < tolerance);
        CHECK(e2 < tolerance);
        // Designed with them, the template is held.
        IirSpec spec;
        spec.family = family;
        spec.response = t.response;
        spec.order = o.order;
        spec.sampleRateHz = t.sampleRateHz;
        spec.frequency1Hz = o.frequency1Hz;
        spec.frequency2Hz = o.frequency2Hz;
        spec.passbandRippleDb = t.passbandLossDb;
        spec.stopbandAttenuationDb = t.stopbandAttenuationDb;
        const IirDesign d = designIir(spec);
        REQUIRE(d.ok());
        const bool held = holdsTemplate(d.sections, t);
        CHECK(held);
        if (!held || e1 >= tolerance || e2 >= tolerance || o.order != ref.order || qacq::test::state().verbose)
            std::printf("    %-18s order %d (SciPy %d, %.4f), frequencies off by %.2g and %.2g\n", ref.name, o.order, ref.order,
                        o.exactOrder, e1, e2);
    }

    // Bessel, which has no formula: the least order whose magnitude falls from −Ap to −As within the ratio of the
    // edges (prewarped), whatever its cut-off.
    FilterTemplate t;
    t.response = FilterResponse::LowPass;
    t.sampleRateHz = 48000.0;
    t.passHz = {1000.0, 0.0};
    t.stopHz = {6000.0, 0.0};
    t.passbandLossDb = 1.0;
    t.stopbandAttenuationDb = 40.0;
    const IirOrder bessel = minimumIirOrder(FilterFamily::Bessel, t);
    REQUIRE(bessel.ok());
    CHECK(bessel.order == 6);
    CHECK(bessel.exactOrder == 0.0);
    const auto ratio = [&t](int order) {
        IirSpec spec;
        spec.family = FilterFamily::Bessel;
        spec.order = order;
        spec.sampleRateHz = t.sampleRateHz;
        spec.frequency1Hz = 1000.0;
        const IirDesign d = designIir(spec);
        const auto at = [&](double db) {
            double low = 0.0, high = 0.5 * t.sampleRateHz;
            for (int i = 0; i < 200; ++i)
            {
                const double middle = 0.5 * (low + high);
                (20.0 * std::log10(std::abs(sosResponse(d.sections, middle, t.sampleRateHz))) > -db ? low : high) = middle;
            }
            return std::tan(kPi * 0.5 * (low + high) / t.sampleRateHz);
        };
        return at(t.stopbandAttenuationDb) / at(t.passbandLossDb);
    };
    const double nat = std::tan(kPi * 6000.0 / 48000.0) / std::tan(kPi * 1000.0 / 48000.0);
    CHECK(ratio(bessel.order) <= nat * (1.0 + 1e-9));
    CHECK(ratio(bessel.order - 1) > nat);
    {
        IirSpec spec;
        spec.family = FilterFamily::Bessel;
        spec.order = bessel.order;
        spec.sampleRateHz = t.sampleRateHz;
        spec.frequency1Hz = bessel.frequency1Hz;
        const IirDesign d = designIir(spec);
        REQUIRE(d.ok());
        CHECK(holdsTemplate(d.sections, t));
        // −Ap exactly at the pass band edge.
        CHECK_NEAR(20.0 * std::log10(std::abs(sosResponse(d.sections, 1000.0, 48000.0))), -1.0, 0.0, 1e-9);
    }
    // Beyond its order 12: said by the order, no frequency.
    FilterTemplate steep = t;
    steep.stopHz = {1200.0, 0.0};
    const IirOrder beyond = minimumIirOrder(FilterFamily::Bessel, steep);
    CHECK(beyond.order == kMaximumBesselOrder + 1);
    CHECK(beyond.frequency1Hz == 0.0);

    // What makes a template impossible.
    FilterTemplate bad = t;
    bad.stopHz = {800.0, 0.0}; // a low-pass stopping before it stops passing
    CHECK(!minimumIirOrder(FilterFamily::Butterworth, bad).ok());
    CHECK(templateError(bad).find("passe-bas") != std::string::npos);
    bad = t;
    bad.stopbandAttenuationDb = 0.5; // As under Ap
    CHECK(templateError(bad).find("Amin") != std::string::npos);
    bad = t;
    bad.response = FilterResponse::BandStop; // its edges in the order of a low-pass
    CHECK(!templateError(bad).empty());
    bad = t;
    bad.stopHz = {30000.0, 0.0}; // beyond fs/2
    CHECK(!templateError(bad).empty());
    CHECK(templateError(t).empty());
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
