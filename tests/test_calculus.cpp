#include "qacq_test.h"

#include "qacq/analysis/analysis.h"
#include "qacq/analysis/calculus.h"
#include "qacq/analysis/measures.h"
#include "qacq/dsp/spectrum.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

using namespace qacq::analysis;
using qacq::model::ChannelInfo;
using qacq::model::Representation;
using qacq::model::SampleData;
using qacq::model::Signal;

namespace
{
constexpr double kPi = std::numbers::pi;

Signal makeSignal(std::vector<std::vector<double>> channels, std::vector<ChannelInfo> info, double rate)
{
    auto data = std::make_shared<SampleData>();
    data->channels = std::move(channels);
    Signal s;
    s.id = 7;
    s.name = "essai";
    s.sampleRateHz = rate;
    s.channels = std::move(info);
    s.data = std::move(data);
    return s;
}

std::vector<double> wave(std::size_t n, double fs, double f, double amplitude, double offset = 0.0, bool cosine = false)
{
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double phase = 2.0 * kPi * f * static_cast<double>(i) / fs;
        x[i] = offset + amplitude * (cosine ? std::cos(phase) : std::sin(phase));
    }
    return x;
}
} // namespace

TEST_CASE("spectrum: the mean weighted by the window leaves nothing at DC and beside it", "[analysis]")
{
    // 440 Hz on 0,1 V of DC, 4655 samples at 44,1 kHz: 46,4 periods, not a whole number. The plain mean
    // is not the DC part: removing it leaves a step that the window spreads over the first bins.
    const std::vector<double> x = wave(4655, 44100.0, 440.0, 0.4, 0.1);
    qacq::dsp::SpectrumOptions options;
    options.removeMean = true;
    const qacq::dsp::Spectrum plain = qacq::dsp::spectrum(x, 44100.0, options);
    options.windowedMean = true;
    const qacq::dsp::Spectrum weighted = qacq::dsp::spectrum(x, 44100.0, options);
    const double peak = *std::max_element(weighted.amplitude.begin(), weighted.amplitude.end());
    CHECK_NEAR(peak, 0.4, 0.0, 0.06); // 0,4 V less the scalloping loss (0,44 bin off)
    // The plain mean leaves a false line at −42 dB on bins 0 and 1; the weighted one nothing at DC and,
    // beside it, only the window's own leakage of the tone (about −100 dB here, as on bins 2, 3...).
    CHECK(plain.amplitude[1] > 3e-3 * peak);
    CHECK(weighted.amplitude[0] < 1e-12);
    CHECK(weighted.amplitude[1] < 3e-5 * peak);
    CHECK(weighted.amplitude[1] < 3.0 * weighted.amplitude[3]);

    // The analysis module removes it this way, segment by segment in density too.
    const Signal s = makeSignal({x}, {{"Voie 1", "V"}}, 44100.0);
    AnalysisSettings settings;
    const AnalysisResult r = analyse(s, 0, x.size(), settings);
    REQUIRE(r.ok());
    CHECK(r.curves[0].linear[1] < 3e-5 * peak);
    CHECK_NEAR(r.measures[0].mean, 0.1, 0.0, 0.01); // still measured
    settings.kind = SpectrumKind::Density;
    settings.segmentLength = 1000;
    const AnalysisResult d = analyse(s, 0, x.size(), settings);
    REQUIRE(d.ok());
    const double top = *std::max_element(d.curves[0].linear.begin(), d.curves[0].linear.end());
    CHECK(d.curves[0].linear[1] < 1e-9 * top);
}

TEST_CASE("calculus: integral, running integral, derivative", "[analysis]")
{
    // 2 V for 1 s: 2 V·s.
    CHECK_NEAR(integral(std::vector<double>(1001, 2.0), 1000.0), 2.0, 1e-12, 0.0);
    CHECK_NEAR(integral(std::vector<double>{1.0}, 1000.0), 0.0, 0.0, 0.0);
    // Holes are left out.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK_NEAR(integral(std::vector<double>{1.0, 1.0, nan, 1.0, 1.0}, 1.0), 2.0, 1e-12, 0.0);

    // ∫ cos(2π f t) dt = sin(2π f t) / (2π f); d sin / dt = 2π f cos.
    const double fs = 10000.0, f = 50.0;
    const std::vector<double> c = wave(10000, fs, f, 1.0, 0.0, true);
    const std::vector<double> s = cumulativeIntegral(c, fs);
    REQUIRE(s.size() == c.size());
    CHECK(s[0] == 0.0);
    double worst = 0.0;
    for (std::size_t i = 0; i < s.size(); ++i)
        worst = std::max(worst, std::abs(s[i] - std::sin(2.0 * kPi * f * static_cast<double>(i) / fs) / (2.0 * kPi * f)));
    CHECK(worst < 2e-4 / (2.0 * kPi * f)); // trapezoids: (ω h)² / 12 of the amplitude
    const std::vector<double> sn = wave(10000, fs, f, 1.0);
    const std::vector<double> dc = derivative(sn, fs);
    worst = 0.0;
    for (std::size_t i = 1; i + 1 < dc.size(); ++i)
        worst = std::max(worst, std::abs(dc[i] - 2.0 * kPi * f * std::cos(2.0 * kPi * f * static_cast<double>(i) / fs)));
    CHECK(worst < 2e-3 * 2.0 * kPi * f);

    // An offset makes the integral drift; with the mean removed it stays a sine.
    const std::vector<double> offset = wave(10000, fs, f, 1.0, 0.5, true);
    CHECK_NEAR(cumulativeIntegral(offset, fs).back(), 0.5 * 0.9999, 0.0, 1e-3);
    const std::vector<double> centred = cumulativeIntegral(offset, fs, true);
    const auto [low, high] = std::minmax_element(centred.begin(), centred.end());
    CHECK_NEAR(*high, 1.0 / (2.0 * kPi * f), 0.0, 1e-5);
    CHECK_NEAR(*low, -1.0 / (2.0 * kPi * f), 0.0, 1e-5);
}

TEST_CASE("calculus: one value per period, on a sweep", "[analysis]")
{
    // A sweep from 20 Hz to 60 Hz in 2 s at 20 kHz: phase 2π (f0 t + k t² / 2).
    const double fs = 20000.0, f0 = 20.0, k = 20.0;
    std::vector<double> x(40000);
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        const double t = static_cast<double>(i) / fs;
        x[i] = 0.3 + std::sin(2.0 * kPi * (f0 * t + 0.5 * k * t * t));
    }
    const std::vector<double> freq = perCycle(x, fs, CycleQuantity::Frequency);
    REQUIRE(freq.size() == x.size());
    CHECK(std::isnan(freq[0])); // before the first rising edge (t = 0 is one, not counted)
    CHECK(std::isnan(freq.back()));
    std::size_t checked = 0;
    for (std::size_t i = 1000; i < 38000; i += 997)
    {
        if (!std::isfinite(freq[i])) continue;
        const double t = static_cast<double>(i) / fs;
        CHECK_NEAR(freq[i], f0 + k * t, 0.0, 0.6); // within the period the value stands for
        ++checked;
    }
    CHECK(checked > 30);
    const std::vector<double> mean = perCycle(x, fs, CycleQuantity::Mean);
    const std::vector<double> rms = perCycle(x, fs, CycleQuantity::Rms);
    CHECK_NEAR(mean[20000], 0.3, 0.0, 2e-3);
    CHECK_NEAR(rms[20000], std::sqrt(0.09 + 0.5), 0.0, 2e-3);
    // Holes, or no period: nothing.
    std::vector<double> holed = x;
    holed[100] = std::numeric_limits<double>::quiet_NaN();
    const std::vector<double> none = perCycle(holed, fs, CycleQuantity::Mean);
    CHECK(std::none_of(none.begin(), none.end(), [](double v) { return std::isfinite(v); }));
}

TEST_CASE("calculus: units and names of the computed curves", "[analysis]")
{
    CHECK(operationUnit(Operation::Integral, "V") == "V\xC2\xB7s");
    CHECK(operationUnit(Operation::Integral, "m/s") == "m");
    CHECK(operationUnit(Operation::Integral, "") == "s");
    CHECK(operationUnit(Operation::Derivative, "V") == "V/s");
    CHECK(operationUnit(Operation::Derivative, "m/s") == "m/s\xC2\xB2");
    CHECK(operationUnit(Operation::Derivative, "V\xC2\xB7s") == "V");
    CHECK(operationUnit(Operation::Derivative, "") == "1/s");
    CHECK(operationUnit(Operation::CycleRms, "A") == "A");
    CHECK(operationUnit(Operation::CycleFrequency, "A") == "Hz");
    CHECK(operationChannelName(Operation::Integral, "Voie 1") == "\xE2\x88\xAB Voie 1");
    Operation o = Operation::Integral;
    CHECK(operationFromKey("cycleFrequency", o));
    CHECK(o == Operation::CycleFrequency);
    CHECK(!operationFromKey("fft", o));
}

TEST_CASE("calculus: a derived signal for the session", "[analysis]")
{
    const Signal s = makeSignal({std::vector<double>(1000, 2.0), wave(1000, 1000.0, 10.0, 1.0)},
                                {{"Courant", "A"}, {"Tension", "V"}}, 1000.0);
    DeriveOptions options;
    options.firstFrame = 100;
    options.frameCount = 500;
    Derived d = derive(s, options);
    REQUIRE(d.ok());
    const Signal &r = d.signal;
    CHECK(r.origin == qacq::model::Origin::Derived);
    CHECK(r.name == "Intégrale de « essai »");
    REQUIRE(r.channelCount() == 4); // the sources first
    CHECK(r.channels[0].name == "Courant");
    CHECK(r.channels[2].name == "\xE2\x88\xAB Courant");
    CHECK(r.channels[2].unit == "A\xC2\xB7s");
    CHECK(r.frameCount() == 500);
    CHECK_NEAR(r.startSeconds, 0.1, 1e-12, 0.0);
    CHECK_NEAR(r.channel(2).back(), 2.0 * 0.499, 1e-12, 0.0);
    CHECK(r.provenance["operation"].toString() == "integral");
    CHECK(r.provenance["sourceId"].toNumber() == 7.0);
    CHECK(qacq::model::validateSignal(r).empty());

    // One channel only, without the source.
    options.channels = {1};
    options.keepSource = false;
    options.operation = Operation::Derivative;
    d = derive(s, options);
    REQUIRE(d.ok());
    REQUIRE(d.signal.channelCount() == 1);
    CHECK(d.signal.channels[0].unit == "V/s");

    // Per period: the constant channel has none (said in the notes), the sine one has.
    options.channels = {};
    options.firstFrame = 0;
    options.frameCount = 1000;
    options.operation = Operation::CycleFrequency;
    d = derive(s, options);
    REQUIRE(d.ok());
    CHECK(d.signal.notes.find("Courant") != std::string::npos);
    CHECK_NEAR(d.signal.channel(1)[500], 10.0, 1e-9, 0.0);
    // Nothing usable, too short, I/Q: an error.
    options.channels = {0};
    CHECK(!derive(s, options).ok());
    options.frameCount = 1;
    options.channels = {};
    CHECK(!derive(s, options).ok());
    Signal iq = s;
    iq.representation = Representation::IQ;
    options.frameCount = 1000;
    CHECK(!derive(iq, options).ok());
    options.channels = {5};
    CHECK(!derive(s, options).ok());
}

TEST_CASE("measures: whole periods, 20-80 % edges, settling, first and last values, area", "[analysis]")
{
    // 1 V on 1 V of offset for 3,3 periods: the plain mean is off, the mean over whole periods is not.
    const std::vector<double> x = wave(33000, 10000.0, 1.0, 1.0, 1.0);
    const WaveMeasures w = waveMeasures(x, 10000.0);
    REQUIRE(w.cycles);
    CHECK(w.cycleCount == 2); // rising edges at 1, 2 and 3 s (the one at 0 s is the start)
    CHECK_NEAR(w.cycleMean, 1.0, 0.0, 1e-3);
    CHECK_NEAR(w.cycleRms, std::sqrt(1.5), 0.0, 1e-3);
    CHECK(!w.flatTop); // a sine: no flat parts, no settling
    CHECK(!w.flatBase);
    CHECK(!w.settlingMeasured);

    // Trapezoid of the oscilloscope test: 20 → 80 % of a 1 ms rise is 0,6 ms, of a 0,5 ms fall 0,3 ms.
    const double fs = 100000.0, period = 0.01, tr = 0.001, tf = 0.0005;
    std::vector<double> t(100000);
    for (std::size_t i = 0; i < t.size(); ++i)
    {
        const double time = static_cast<double>(i) / fs;
        const double u = std::fmod(time + tr, period) - tr;
        const double up = (u + tr / 2.0) / tr, down = (0.3 * period + tf / 2.0 - u) / tf;
        t[i] = 5.0 * std::clamp(std::min(up, down), 0.0, 1.0);
    }
    const WaveMeasures w20 = waveMeasures(t, fs, 0.2);
    CHECK_NEAR(w20.edgeLowFraction, 0.2, 0.0, 0.0);
    CHECK_NEAR(w20.riseSeconds, 0.6e-3, 1e-6, 0.0);
    CHECK_NEAR(w20.fallSeconds, 0.3e-3, 1e-6, 0.0);
    REQUIRE(w20.flatTop);
    REQUIRE(w20.flatBase);
    REQUIRE(w20.settlingMeasured);
    CHECK(!w20.lastEdgeRising); // the record ends after a fall, part of the way into the next rise
    REQUIRE(w20.settling1.reached);
    // From the middle of a linear fall of 0,5 ms to 1 % of the base: 0,245 ms.
    CHECK_NEAR(w20.settling1.seconds, 0.245e-3, 0.0, 2e-6);

    // A first-order step (τ = 1 ms) at 10 ms: from the middle (τ ln 2) to 1 % (τ ln 100) and 0,1 %.
    const double tau = 1e-3, t0 = 0.01;
    std::vector<double> step(5000);
    for (std::size_t i = 0; i < step.size(); ++i)
    {
        const double time = static_cast<double>(i) / fs;
        step[i] = time < t0 ? 0.0 : 1.0 - std::exp(-(time - t0) / tau);
    }
    const WaveMeasures ws = waveMeasures(step, fs);
    REQUIRE(ws.flatTop);
    REQUIRE(ws.settlingMeasured);
    CHECK(ws.lastEdgeRising);
    REQUIRE(ws.settling1.reached);
    CHECK_NEAR(ws.settling1.seconds, tau * (std::log(100.0) - std::log(2.0)), 0.0, 3e-6);
    REQUIRE(ws.settling01.reached);
    CHECK_NEAR(ws.settling01.seconds, tau * (std::log(1000.0) - std::log(2.0)), 0.0, 3e-6);
    // A ripple of 0,5 % that never dies out: within 1 % at once, never within 0,1 %.
    std::vector<double> ripple(5000);
    for (std::size_t i = 0; i < ripple.size(); ++i)
    {
        const double time = static_cast<double>(i) / fs;
        ripple[i] = time < t0 ? 0.0 : 1.0 + 0.005 * std::sin(2.0 * kPi * 3000.0 * (time - t0));
    }
    const WaveMeasures wr = waveMeasures(ripple, fs);
    REQUIRE(wr.settlingMeasured);
    REQUIRE(wr.settling1.reached);
    CHECK(wr.settling1.seconds < 1e-5);
    CHECK(!wr.settling01.reached);

    // First and last values, area: in the measurements of a zone.
    const Signal s = makeSignal({wave(4800, 48000.0, 1000.0, 1.0, 0.5)}, {{"x", "V"}}, 48000.0);
    const AnalysisResult r = analyse(s, 0, 4800, AnalysisSettings{});
    REQUIRE(r.measures.size() == 1);
    CHECK_NEAR(r.measures[0].first, 0.5, 1e-12, 0.0);
    CHECK_NEAR(r.measures[0].last, s.channel(0).back(), 1e-12, 0.0);
    const double area = 0.5 * 4799.0 / 48000.0 + (1.0 - std::cos(2.0 * kPi * 1000.0 * 4799.0 / 48000.0)) / (2.0 * kPi * 1000.0);
    CHECK_NEAR(r.measures[0].integral, area, 0.0, 1e-8);
    const SpanStatistics st = spanStatistics(std::vector<double>{std::numeric_limits<double>::quiet_NaN(), 3.0, 4.0});
    CHECK(st.first == 3.0);
    CHECK(st.last == 4.0);
}

TEST_CASE("measures: band within 3 dB of the highest bin", "[analysis]")
{
    // A tone on a bin, Hann: the main lobe is 0,5 at ±1 bin, so the band reaches 0,586 bin each side.
    const Signal s = makeSignal({wave(4800, 48000.0, 1000.0, 1.0)}, {{"x", "V"}}, 48000.0);
    const AnalysisResult r = analyse(s, 0, 4800, AnalysisSettings{});
    const HalfPowerBand &b = r.curves[0].band;
    REQUIRE(b.found);
    CHECK(!b.lowOpen);
    CHECK(!b.highOpen);
    CHECK_NEAR(b.peakHz, 1000.0, 1e-12, 0.0);
    CHECK_NEAR(b.centreHz(), 1000.0, 1e-9, 0.0);
    CHECK_NEAR(b.widthHz(), 2.0 * 10.0 * (1.0 - std::sqrt(0.5)) / 0.5, 1e-9, 0.0);

    // Highest at DC (mean kept): a lowpass shape, open below.
    AnalysisSettings keep;
    keep.removeMean = false;
    const Signal dc = makeSignal({wave(4800, 48000.0, 1000.0, 0.1, 1.0)}, {{"x", "V"}}, 48000.0);
    const AnalysisResult l = analyse(dc, 0, 4800, keep);
    const HalfPowerBand &lb = l.curves[0].band;
    CHECK(lb.found);
    CHECK(lb.lowOpen);
    CHECK(lb.highHz > 0.0);
    CHECK(lb.highHz < 20.0); // the DC line of Hann spreads over bin 1 (10 Hz)
}
