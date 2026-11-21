#include "qacq_test.h"
#include "signals.h"

#include "qacq/analysis/analysis.h"
#include "qacq/analysis/measures.h"
#include "qacq/plot/axis.h"
#include "qacq/plot/trace.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <sstream>

using namespace qacq::analysis;
using qacq::model::ChannelInfo;
using qacq::model::Representation;
using qacq::model::SampleData;
using qacq::model::Signal;

namespace
{
Signal makeSignal(std::vector<std::vector<double>> channels, std::vector<ChannelInfo> info, double rate,
                  Representation representation = Representation::Real)
{
    auto data = std::make_shared<SampleData>();
    data->channels = std::move(channels);
    Signal s;
    s.id = 1;
    s.name = "essai";
    s.sampleRateHz = rate;
    s.representation = representation;
    s.channels = std::move(info);
    s.data = std::move(data);
    return s;
}

std::vector<double> sine(std::size_t n, double fs, double f, double amplitude, double offset = 0.0)
{
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i)
        x[i] = offset + amplitude * std::sin(2.0 * std::numbers::pi * f * static_cast<double>(i) / fs);
    return x;
}
} // namespace

TEST_CASE("amplitude spectrum of a zone: dBV at the tone, measurements of the channel", "[analysis]")
{
    // 0,5 V at 1 kHz on 0,2 V of DC, 4800 samples at 48 kHz: the tone sits on bin 100.
    const Signal s = makeSignal({sine(48000, 48000.0, 1000.0, 0.5, 0.2)}, {{"Entrée", "V"}}, 48000.0);
    const AnalysisResult r = analyse(s, 1000, 4800, AnalysisSettings{});
    REQUIRE(r.ok());
    CHECK(r.zoneFirstFrame == 1000);
    CHECK(r.zoneFrames == 4800);
    CHECK(r.fftSize == 4800);
    CHECK(!r.paddedToPowerOfTwo);
    CHECK(r.bins == 2401);
    CHECK_NEAR(r.binWidthHz, 10.0, 1e-12, 0.0);
    REQUIRE(r.curves.size() == 1);
    const SpectrumCurve &c = r.curves[0];
    CHECK(c.peakBin == 100);
    CHECK_NEAR(r.frequencyAt(static_cast<double>(c.peakBin)), 1000.0, 1e-12, 0.0);
    CHECK_NEAR(c.level[100], 20.0 * std::log10(0.5), 0.0, 1e-6);
    CHECK_NEAR(c.linear[100], 0.5, 1e-6, 0.0);
    CHECK(c.level[0] < -100.0); // mean removed
    CHECK(levelUnit(c.unit, r.kind, r.scale) == "dBV");
    REQUIRE(r.measures.size() == 1);
    const ChannelMeasures &m = r.measures[0];
    CHECK(m.samples == 4800);
    CHECK_NEAR(m.mean, 0.2, 0.0, 1e-9);
    CHECK_NEAR(m.acRms, 0.5 / std::sqrt(2.0), 1e-9, 0.0);
    CHECK_NEAR(m.rms, std::sqrt(0.04 + 0.125), 1e-9, 0.0);
    CHECK_NEAR(m.peakToPeak, 1.0, 1e-6, 0.0);
    CHECK_NEAR(m.crestFactor, std::sqrt(2.0), 1e-5, 0.0);
    CHECK(m.tone.detected);
    CHECK_NEAR(m.tone.fundamentalHz, 1000.0, 1e-7, 0.0);
    CHECK_NEAR(m.tone.fundamentalAmplitude, 0.5, 1e-6, 0.0);
    CHECK(r.notes.empty());

    // Linear scale: the amplitude itself, axis from 0.
    AnalysisSettings linear;
    linear.scale = LevelScale::Linear;
    const AnalysisResult l = analyse(s, 1000, 4800, linear);
    REQUIRE(l.ok());
    CHECK_NEAR(l.curves[0].level[100], 0.5, 1e-6, 0.0);
    CHECK(levelUnit("V", l.kind, l.scale) == "V");
    // The same result levelled again in dB.
    AnalysisResult again = l;
    setScale(again, LevelScale::Decibel);
    CHECK(again.scale == LevelScale::Decibel);
    CHECK_NEAR(again.curves[0].level[100], c.level[100], 0.0, 1e-9);
    CHECK(again.curves[0].peakBin == 100);
}

TEST_CASE("a long zone: FFT padded to the next power of two, every sample used", "[analysis]")
{
    // 100 000 samples, 12 kHz at 48 kHz: a bin centre of the 131 072-point grid.
    const Signal s = makeSignal({sine(100000, 48000.0, 12000.0, 1.0)}, {{"x", ""}}, 48000.0);
    const AnalysisResult r = analyse(s, 0, 100000, AnalysisSettings{});
    REQUIRE(r.ok());
    CHECK(r.fftSize == 131072);
    CHECK(r.paddedToPowerOfTwo);
    CHECK(r.bins == 65537);
    CHECK(r.curves[0].peakBin == 32768);
    CHECK_NEAR(r.curves[0].linear[32768], 1.0, 1e-6, 0.0);
    CHECK(r.curves[0].pyramid.levelCount() > 0);
    CHECK(levelUnit("", r.kind, r.scale) == "dB");
    REQUIRE(r.notes.size() == 1);
    CHECK(r.notes[0].find("131") != std::string::npos);

    // Past 2^24 points (zeros included): refused, with what to do.
    AnalysisSettings padded;
    padded.zeroPadFactor = 8;
    const Signal big = makeSignal({std::vector<double>((std::size_t{1} << 21) + 1, 0.0)}, {{"x", "V"}}, 1000.0);
    const AnalysisResult refused = analyse(big, 0, big.frameCount(), padded);
    CHECK(!refused.ok());
    CHECK(refused.error.find("Welch") != std::string::npos);
}

TEST_CASE("density (Welch): white noise reads its level per hertz", "[analysis]")
{
    // Uniform noise in [-0.5, 0.5): variance 1/12, one-sided density 2 sigma^2 / fs.
    const double fs = 1000.0;
    const Signal s = makeSignal({qacq::test::lcgNoise(65536, 7u)}, {{"Bruit", "V"}}, fs);
    AnalysisSettings settings;
    settings.kind = SpectrumKind::Density;
    settings.segmentLength = 1024;
    const AnalysisResult r = analyse(s, 0, 65536, settings);
    REQUIRE(r.ok());
    CHECK(r.fftSize == 1024);
    CHECK(r.segments == 127);
    CHECK(r.bins == 513);
    CHECK_NEAR(r.binWidthHz, fs / 1024.0, 1e-12, 0.0);
    double sum = 0.0;
    for (std::size_t k = 1; k + 1 < r.bins; ++k) sum += r.curves[0].linear[k];
    const double mean = sum / static_cast<double>(r.bins - 2);
    CHECK_NEAR(mean, 2.0 / 12.0 / fs, 0.05, 0.0);
    CHECK(levelUnit("V", r.kind, LevelScale::Decibel) == "dBV/\xE2\x88\x9AHz");
    CHECK(levelUnit("V", r.kind, LevelScale::Linear) == "V/\xE2\x88\x9AHz");
    CHECK(levelUnit("", r.kind, LevelScale::Decibel) == "dB/Hz");
    CHECK(levelUnit("A", r.kind, LevelScale::Decibel) == "dB re 1 A/\xE2\x88\x9AHz");
    CHECK(levelUnit("A", SpectrumKind::Amplitude, LevelScale::Decibel) == "dB re 1 A");
    // Linear density = amplitude spectral density, unit/√Hz.
    settings.scale = LevelScale::Linear;
    const AnalysisResult l = analyse(s, 0, 65536, settings);
    CHECK_NEAR(l.curves[0].level[100], std::sqrt(l.curves[0].linear[100]), 1e-12, 0.0);
}

TEST_CASE("an I/Q pair: two-sided spectrum, negative frequencies", "[analysis]")
{
    // exp(j 2 pi f t), f = -2 kHz, 1600 samples at 16 kHz.
    const std::size_t n = 1600;
    std::vector<double> i(n), q(n);
    for (std::size_t k = 0; k < n; ++k)
    {
        const double phase = 2.0 * std::numbers::pi * -2000.0 * static_cast<double>(k) / 16000.0;
        i[k] = std::cos(phase);
        q[k] = std::sin(phase);
    }
    const Signal s = makeSignal({i, q}, {{"I", ""}, {"Q", ""}}, 16000.0, Representation::IQ);
    const AnalysisResult r = analyse(s, 0, n, AnalysisSettings{});
    REQUIRE(r.ok());
    CHECK(r.twoSided);
    CHECK(r.bins == n);
    CHECK_NEAR(r.firstFrequencyHz, -8000.0, 1e-12, 0.0);
    REQUIRE(r.curves.size() == 1);
    CHECK(r.curves[0].name == "I + jQ");
    CHECK_NEAR(r.frequencyAt(static_cast<double>(r.curves[0].peakBin)), -2000.0, 1e-9, 0.0);
    CHECK_NEAR(r.curves[0].level[r.curves[0].peakBin], 0.0, 0.0, 1e-6);
    CHECK(r.measures.size() == 2);
    CHECK(!r.measures[0].tone.detected);
    CHECK(!r.notes.empty());

    AnalysisSettings density;
    density.kind = SpectrumKind::Density;
    density.segmentLength = 256;
    const AnalysisResult d = analyse(s, 0, n, density);
    REQUIRE(d.ok());
    CHECK(d.bins == 256);
    CHECK(d.segments == 11); // hop 128: (1600 - 256) / 128 + 1
    CHECK_NEAR(d.frequencyAt(static_cast<double>(d.curves[0].peakBin)), -2000.0, 1e-9, 0.0);
}

TEST_CASE("analysis: missing samples, short zones, cancellation", "[analysis]")
{
    std::vector<double> x = sine(2000, 1000.0, 50.0, 1.0);
    x[100] = std::numeric_limits<double>::quiet_NaN();
    x[101] = std::numeric_limits<double>::quiet_NaN();
    const Signal s = makeSignal({x}, {{"x", "V"}}, 1000.0);
    const AnalysisResult r = analyse(s, 0, 2000, AnalysisSettings{});
    REQUIRE(r.ok());
    for (double v : r.curves[0].level) CHECK(std::isfinite(v));
    CHECK(r.measures[0].samples == 1998);
    CHECK(!r.measures[0].tone.detected); // not on a record with holes
    REQUIRE(!r.notes.empty());
    CHECK(r.notes[0].find("manquants") != std::string::npos);

    CHECK(!analyse(s, 1995, 100, AnalysisSettings{}).ok()); // clipped to 5 samples
    CHECK(analyse(s, 1995, 100, AnalysisSettings{}).zoneFrames == 5);
    Signal unknown = s;
    unknown.sampleRateHz = 0.0;
    CHECK(!analyse(unknown, 0, 2000, AnalysisSettings{}).error.empty());

    const std::atomic<bool> stop{true};
    CHECK(analyse(s, 0, 2000, AnalysisSettings{}, &stop).error == "annulé");
}

TEST_CASE("analysis settings: JSON round trip, values out of range", "[analysis]")
{
    AnalysisSettings s;
    s.kind = SpectrumKind::Density;
    s.window = qacq::dsp::Window::FlatTop;
    s.kaiserBeta = 6.0;
    s.zeroPadFactor = 4;
    s.removeMean = false;
    s.segmentLength = 2048;
    s.overlap = 0.75;
    s.scale = LevelScale::Linear;
    s.logFrequency = true;
    s.harmonics = 7;
    s.spectrogramSegment = 4096;
    s.spectrogramOverlap = 0.5;
    const AnalysisSettings b = analysisSettingsFromJson(toJson(s));
    CHECK(b.kind == s.kind);
    CHECK(b.window == s.window);
    CHECK(b.kaiserBeta == 6.0);
    CHECK(b.zeroPadFactor == 4);
    CHECK(!b.removeMean);
    CHECK(b.segmentLength == 2048);
    CHECK(b.overlap == 0.75);
    CHECK(b.scale == LevelScale::Linear);
    CHECK(b.logFrequency);
    CHECK(b.harmonics == 7);
    CHECK(b.spectrogramSegment == 4096);
    CHECK(b.spectrogramOverlap == 0.5);

    qacq::model::Json bad = qacq::model::Json::object();
    bad["window"] = "triangle";
    bad["zeroPadFactor"] = 3;
    bad["overlap"] = 2.0;
    bad["segmentLength"] = 1;
    bad["spectrogramSegment"] = 1e9;
    const AnalysisSettings d = analysisSettingsFromJson(bad);
    CHECK(d.window == qacq::dsp::Window::Hann);
    CHECK(d.zeroPadFactor == 1);
    CHECK(d.overlap == 0.95);
    CHECK(d.segmentLength == 16);
    CHECK(d.spectrogramSegment == 65536);
    CHECK(d.spectrogramOverlap == 0.75);
    CHECK(analysisSettingsFromJson(qacq::model::Json()).kind == SpectrumKind::Amplitude);
}

TEST_CASE("analysis: level scale, export", "[analysis]")
{
    const qacq::plot::Range db = levelRange(-312.0, -6.02, LevelScale::Decibel);
    CHECK(db.high == 0.0);
    CHECK(db.low == -170.0);
    const qacq::plot::Range narrow = levelRange(-7.0, -6.0, LevelScale::Decibel);
    CHECK(narrow.high - narrow.low >= 20.0);
    const qacq::plot::Range lin = levelRange(0.001, 0.5, LevelScale::Linear);
    CHECK(lin.low == 0.0);
    CHECK(lin.high > 0.5);
    CHECK(levelOf(0.0, SpectrumKind::Amplitude, LevelScale::Decibel) == -400.0);

    const Signal s = makeSignal({sine(100, 100.0, 10.0, 1.0)}, {{"Tension", "V"}}, 100.0);
    const AnalysisResult r = analyse(s, 0, 100, AnalysisSettings{});
    std::ostringstream out;
    writeSpectrumCsv(out, r);
    const std::string text = out.str();
    CHECK(text.rfind("Fréquence (Hz);Tension (dBV);Tension (V)\n", 0) == 0);
    std::size_t lines = 0;
    for (char ch : text) lines += ch == '\n' ? 1 : 0;
    CHECK(lines == 52);
    const std::size_t line = text.find("\n10;");
    REQUIRE(line != std::string::npos);
    const std::string row = text.substr(line + 1, text.find('\n', line + 1) - line - 1);
    CHECK(row.substr(row.rfind(';') + 1).rfind("0,99999", 0) == 0 || row.substr(row.rfind(';') + 1).rfind("1", 0) == 0);
}

TEST_CASE("analysis: harmonics that stand out of the noise", "[analysis]")
{
    // 1 V at 1 kHz, its third harmonic at −40 dB, noise 1 mV rms (uniform), 48 000 samples at 48 kHz.
    const std::size_t n = 48000;
    std::vector<double> x = qacq::test::lcgNoise(n, 3u);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i) / 48000.0;
        x[i] = x[i] * 1e-3 * std::sqrt(12.0) + std::sin(2.0 * std::numbers::pi * 1000.0 * t) +
               0.01 * std::sin(2.0 * std::numbers::pi * 3000.0 * t);
    }
    const Signal s = makeSignal({x}, {{"x", "V"}}, 48000.0);
    const AnalysisResult r = analyse(s, 0, n, AnalysisSettings{});
    REQUIRE(r.ok());
    const ChannelMeasures &m = r.measures[0];
    REQUIRE(m.tone.detected);
    REQUIRE(m.harmonicStandsOut.size() == m.tone.harmonics.size());
    CHECK(m.harmonicStandsOut[0]);  // H1
    CHECK(!m.harmonicStandsOut[1]); // H2: noise only
    CHECK(m.harmonicStandsOut[2]);  // H3
    CHECK(!m.harmonicStandsOut[3]);
    // The noise of one bin of the spectrum (Hann): 2 sigma sqrt(1,5 / 48 000) = 11 µV; the median of the
    // bins (Rayleigh) is a little below it.
    CHECK_NEAR(m.noiseInBin, 2.0 * 1e-3 * std::sqrt(1.5 / 48000.0), 0.15, 0.0);
    CHECK_NEAR(m.spectrumFloor, m.noiseInBin, 0.35, 0.0);

    // Lines that are not harmonics count as noise in the SNR, not in the spectrum around the harmonics:
    // 0,12 A at 3 Hz plus a 1 Hz square wave of ±0,05 A (the "Courant" of the test session). Its odd
    // harmonics at 9, 15, 21, 27 Hz are harmonics 3, 5, 7, 9 of 3 Hz and stand out by 100 dB.
    std::vector<double> current(20000);
    for (std::size_t i = 0; i < current.size(); ++i)
    {
        const double t = static_cast<double>(i) / 2000.0;
        current[i] = 0.12 * std::sin(2.0 * std::numbers::pi * 3.0 * t - 0.6) + (std::fmod(t, 1.0) < 0.5 ? 0.05 : -0.05);
    }
    const Signal comb = makeSignal({current}, {{"Courant", "A"}}, 2000.0);
    const AnalysisResult cr = analyse(comb, 0, current.size(), AnalysisSettings{});
    REQUIRE(cr.measures[0].tone.detected);
    const std::vector<bool> &marks = cr.measures[0].harmonicStandsOut;
    REQUIRE(marks.size() >= 9);
    CHECK(!marks[1]); // 6 Hz: an even harmonic of the square wave, absent
    CHECK(marks[2]);  // 9 Hz
    CHECK(!marks[3]); // 12 Hz
    CHECK(marks[4]);  // 15 Hz
    CHECK(marks[6]);  // 21 Hz
    CHECK(marks[8]);  // 27 Hz: below the "noise" of the SNR, far above the spectrum around it

    // A pure sine (no noise, no harmonic): only the fundamental is marked.
    const Signal pure = makeSignal({sine(40, 100.0, 6.25, 1.0)}, {{"x", "V"}}, 100.0);
    const AnalysisResult p = analyse(pure, 0, 40, AnalysisSettings{});
    REQUIRE(p.measures[0].tone.detected);
    for (std::size_t k = 1; k < p.measures[0].harmonicStandsOut.size(); ++k) CHECK(!p.measures[0].harmonicStandsOut[k]);

    // Welch on a zone shorter than two segments: said so.
    AnalysisSettings density;
    density.kind = SpectrumKind::Density;
    const AnalysisResult d = analyse(pure, 0, 40, density);
    REQUIRE(d.ok());
    CHECK(d.segments == 1);
    CHECK(!d.notes.empty());
}

TEST_CASE("traces on a log axis: sparse at the bottom, strokes at the top, peaks kept", "[plot]")
{
    std::vector<double> values(1001, 0.0);
    values[900] = 5.0;
    const qacq::plot::ChannelView view{values, 0.0, {}, nullptr};
    // Bins 1 to 1000 over 100 pixels, logarithmic.
    std::vector<double> edges(101);
    for (std::size_t c = 0; c <= 100; ++c) edges[c] = std::pow(10.0, 3.0 * static_cast<double>(c) / 100.0);
    const qacq::plot::Trace t = qacq::plot::buildTraceEdges(view, edges);
    CHECK(!t.dense);
    CHECK(t.maximum == 5.0);
    REQUIRE(!t.points.empty());
    // Bin 2 sits at log10(2) / 3 of the width: x = 10,03 (linear inside its column).
    bool found = false;
    for (const auto &p : t.points)
    {
        if (std::abs(p.x - 100.0 * std::log10(2.0) / 3.0) < 0.01) found = true;
    }
    CHECK(found);
    // Bin 0 lies left of the view: drawn just outside so that the line enters.
    CHECK(t.points.front().x < 0.0);
    CHECK(qacq::plot::takesPrefix("V/\xE2\x88\x9AHz"));
    CHECK(qacq::plot::formatQuantity(2.5e-6, "V/\xE2\x88\x9AHz") == "2,500 \xC2\xB5V/\xE2\x88\x9AHz");

    // A spectrum keeps the peak of each column: its line does not fill down to the notches between them.
    std::vector<double> comb(10000, -300.0);
    for (std::size_t k = 0; k < comb.size(); k += 20) comb[k] = -20.0;
    const qacq::plot::ChannelView combView{comb, 0.0, {}, nullptr};
    const qacq::plot::Trace peaks = qacq::plot::buildTrace(combView, {0.0, 100.0, 100}, qacq::plot::Columns::Peaks);
    REQUIRE(peaks.dense);
    for (const auto &column : peaks.columns) CHECK(column.minimum == -20.0);
    CHECK(peaks.minimum == -20.0);
    const qacq::plot::Trace extremes = qacq::plot::buildTrace(combView, {0.0, 100.0, 100});
    CHECK(extremes.minimum == -300.0);
    const qacq::plot::Trace logPeaks = qacq::plot::buildTraceEdges(combView, edges, qacq::plot::Columns::Peaks);
    CHECK(logPeaks.maximum == -20.0);
    // Values beyond the prefixes: a power of ten.
    CHECK(qacq::plot::formatQuantity(-3.429e-17, "A") == "\xE2\x88\x92" "3,429\xC3\x97" "10\xE2\x81\xBB\xC2\xB9\xE2\x81\xB7 A");
    CHECK(qacq::plot::formatQuantity(2.09e-4, "\xC2\xB0" "C") == "0,0002090 \xC2\xB0" "C");
    CHECK(qacq::plot::formatQuantity(1e-6, "\xC2\xB0" "C").find("\xC3\x97" "10") != std::string::npos);
}

TEST_CASE("measures: statistics of a span, oscilloscope measures of a trapezoid wave", "[analysis]")
{
    const std::vector<double> few = {1.0, std::numeric_limits<double>::quiet_NaN(), 3.0, -1.0};
    const SpanStatistics s = spanStatistics(few);
    CHECK(s.samples == 3);
    CHECK_NEAR(s.mean, 1.0, 1e-12, 0.0);
    CHECK_NEAR(s.peakToPeak, 4.0, 1e-12, 0.0);
    CHECK_NEAR(s.rms, std::sqrt(11.0 / 3.0), 1e-12, 0.0);

    // 100 Hz, 0 to 5 V, duty 30 % (between the middles of the edges), rise 1 ms and fall 0,5 ms (ramps),
    // 100 kHz: rise 10-90 % = 0,8 ms, fall 0,4 ms.
    const double fs = 100000.0, period = 0.01, tr = 0.001, tf = 0.0005;
    std::vector<double> x(100000);
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        const double t = static_cast<double>(i) / fs;
        const double u = std::fmod(t + tr, period) - tr;
        const double up = (u + tr / 2.0) / tr, down = (0.3 * period + tf / 2.0 - u) / tf;
        x[i] = 5.0 * std::clamp(std::min(up, down), 0.0, 1.0);
    }
    const WaveMeasures w = waveMeasures(x, fs);
    REQUIRE(w.levels);
    CHECK_NEAR(w.top, 5.0, 0.0, 1e-9);
    CHECK_NEAR(w.base, 0.0, 0.0, 1e-9);
    REQUIRE(w.periodic);
    CHECK_NEAR(w.frequencyHz, 100.0, 1e-9, 0.0);
    REQUIRE(w.duty);
    CHECK_NEAR(w.dutyCyclePercent, 30.0, 0.0, 1e-6);
    REQUIRE(w.rise);
    CHECK_NEAR(w.riseSeconds, 0.8e-3, 1e-6, 0.0);
    REQUIRE(w.fall);
    CHECK_NEAR(w.fallSeconds, 0.4e-3, 1e-6, 0.0);
    CHECK_NEAR(w.overshootPercent, 0.0, 0.0, 1e-9);
    // 100 periods in 1 s; the record starts in the middle of the first rising edge (not counted).
    CHECK(w.risingEdges == 99);
    CHECK(w.fallingEdges == 100);

    // A sine (no flat part): the extremes as levels; rise 10-90 % = 2 asin(0,8) / (2 pi f).
    const std::vector<double> sineWave = sine(20000, 2000.0, 3.0, 1.0, 1.0);
    const WaveMeasures ws = waveMeasures(sineWave, 2000.0);
    REQUIRE(ws.periodic);
    CHECK_NEAR(ws.frequencyHz, 3.0, 1e-4, 0.0);
    CHECK_NEAR(ws.dutyCyclePercent, 50.0, 0.0, 0.1);
    CHECK_NEAR(ws.riseSeconds, 2.0 * std::asin(0.8) / (2.0 * std::numbers::pi * 3.0), 2e-3, 0.0);
    CHECK_NEAR(ws.top, 2.0, 0.0, 1e-6);

    // Flat, or too short: nothing claimed.
    CHECK(!waveMeasures(std::vector<double>(100, 1.0), 1000.0).levels);
    CHECK(!waveMeasures(sine(1000, 1000.0, 0.2, 1.0), 1000.0).periodic); // a fifth of a period
}

TEST_CASE("measures: peaks of a spectrum, power of a band", "[analysis]")
{
    std::vector<double> x(4800);
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        const double t = static_cast<double>(i) / 48000.0;
        x[i] = std::sin(2.0 * std::numbers::pi * 1000.0 * t) + 0.1 * std::sin(2.0 * std::numbers::pi * 3000.0 * t);
    }
    const Signal s = makeSignal({x}, {{"x", "V"}}, 48000.0);
    const AnalysisResult r = analyse(s, 0, x.size(), AnalysisSettings{});
    REQUIRE(r.curves[0].peaks.size() >= 2);
    CHECK_NEAR(r.curves[0].peaks[0].frequencyHz, 1000.0, 1e-6, 0.0);
    CHECK_NEAR(r.curves[0].peaks[0].level, 0.0, 0.0, 1e-6);
    CHECK_NEAR(r.curves[0].peaks[1].frequencyHz, 3000.0, 1e-6, 0.0);
    CHECK_NEAR(r.curves[0].peaks[1].level, -20.0, 0.0, 1e-6);
    // The tone of 1 V in its band: mean square 1/2; both tones: 0,505.
    CHECK_NEAR(bandMeanSquare(r, 0, 900.0, 1100.0), 0.5, 1e-3, 0.0);
    CHECK_NEAR(bandMeanSquare(r, 0, 0.0, 24000.0), 0.505, 1e-3, 0.0);
    // Peaks follow the scale of the levels.
    AnalysisResult linear = r;
    setScale(linear, LevelScale::Linear);
    CHECK_NEAR(linear.curves[0].peaks[1].level, 0.1, 1e-6, 0.0);

    // Density: white noise of variance 1/12 holds (band / Nyquist) of it.
    AnalysisSettings density;
    density.kind = SpectrumKind::Density;
    density.segmentLength = 1024;
    const Signal noise = makeSignal({qacq::test::lcgNoise(65536, 11u)}, {{"b", "V"}}, 1000.0);
    const AnalysisResult d = analyse(noise, 0, 65536, density);
    CHECK_NEAR(bandMeanSquare(d, 0, 100.0, 200.0), (1.0 / 12.0) * 0.2, 0.06, 0.0);
}
