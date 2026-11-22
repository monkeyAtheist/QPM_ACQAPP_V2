// The plot engine: graduations, labels, quantities, traces at any zoom, automatic scales.
#include "qacq_test.h"

#include "qacq/dsp/random.h"
#include "qacq/plot/axis.h"
#include "qacq/plot/trace.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace qacq::plot;

namespace
{
const std::string kMinus = "\xE2\x88\x92";
const std::string kMicro = "\xC2\xB5";
const double kNaN = std::numeric_limits<double>::quiet_NaN();

bool near(double a, double b, double tolerance = 1e-12)
{
    return std::abs(a - b) <= tolerance * std::max(1.0, std::abs(b));
}

bool same(const std::vector<double> &a, const std::vector<double> &b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (!near(a[i], b[i])) return false;
    }
    return true;
}
} // namespace

TEST_CASE("Ticks: steps of 1, 2 and 5, at least the spacing asked", "[plot]")
{
    CHECK(niceStep(1.4) == 2.0);
    CHECK(niceStep(0.1) == 0.1);
    CHECK(niceStep(0.11) == 0.2);
    CHECK(niceStep(3.0) == 5.0);
    CHECK(niceStep(6.0) == 10.0);
    CHECK(near(niceStep(2.3e-7), 5e-7));
    const Ticks t = linearTicks(0.0, 10.0, 500.0, 70.0);
    CHECK(t.step == 2.0);
    CHECK(same(t.major, {0, 2, 4, 6, 8, 10}));
    CHECK(t.minor.size() == 15); // quarters of 2 between the major ticks
    const Ticks n = linearTicks(-0.37, 0.52, 300.0, 40.0);
    CHECK(near(n.step, 0.2));
    CHECK(same(n.major, {-0.2, 0.0, 0.2, 0.4}));
    CHECK(n.major[1] == 0.0); // exactly zero, not 1e-17
    // Far from zero, zoomed in: still a sensible step.
    const Ticks far = linearTicks(1000.0001, 1000.0009, 400.0, 80.0);
    CHECK(near(far.step, 2e-4));
    CHECK(far.major.size() == 4);
    CHECK(linearTicks(1.0, 1.0, 100.0, 10.0).major.empty());
    CHECK(linearTicks(0.0, 1.0, 0.0, 10.0).major.empty());
}

TEST_CASE("Log ticks: decades, 2 and 5 when there is room, every other decade when there is not", "[plot]")
{
    const Ticks wide = logTicks(10.0, 100000.0, 800.0, 50.0); // 200 px per decade
    CHECK(same(wide.major, {10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000}));
    CHECK(wide.minor.size() == 4 * 6); // 3, 4, 6, 7, 8, 9 of four decades
    const Ticks narrow = logTicks(10.0, 100000.0, 160.0, 50.0); // 40 px per decade
    CHECK(same(narrow.major, {100, 10000}));
    CHECK(narrow.minor.empty());
    const Ticks plain = logTicks(10.0, 100000.0, 300.0, 50.0); // 75 px per decade
    CHECK(same(plain.major, {10, 100, 1000, 10000, 100000}));
    CHECK(!plain.minor.empty());
    CHECK(logTicks(0.0, 10.0, 100.0, 10.0).major.empty());
}

TEST_CASE("Axis labels: one prefix for the whole axis, decimals from the step", "[plot]")
{
    const AxisLabels ms = axisLabels(linearTicks(0.0, 0.01, 500.0, 70.0), "s");
    CHECK(ms.unit == "ms");
    CHECK(ms.factor == 1e-3);
    CHECK(ms.decimals == 0);
    CHECK(ms.text.front() == "0");
    CHECK(ms.text.back() == "10");
    const AxisLabels s = axisLabels(linearTicks(0.0, 1.5, 300.0, 50.0), "s");
    CHECK(s.unit == "s");
    CHECK(s.decimals == 1);
    CHECK(s.text[1] == "0,5");
    // Time never goes above the second.
    CHECK(axisLabels(linearTicks(0.0, 5000.0, 300.0, 60.0), "s").unit == "s");
    const AxisLabels khz = axisLabels(linearTicks(0.0, 24000.0, 600.0, 60.0), "Hz");
    CHECK(khz.unit == "kHz");
    CHECK(khz.text[1] == "5");
    CHECK(axisLabels(linearTicks(0.0, 24000.0, 1200.0, 60.0), "Hz").text[1] == "2");
    const AxisLabels volts = axisLabels(linearTicks(-1.5, 1.5, 400.0, 40.0), "V");
    CHECK(volts.unit == "V");
    CHECK(volts.decimals == 1);
    CHECK(volts.text.front() == kMinus + "1,5");
    const AxisLabels db = axisLabels(linearTicks(-120.0, 0.0, 300.0, 40.0), "dBFS");
    CHECK(db.unit == "dBFS");
    CHECK(db.factor == 1.0);
    const AxisLabels big = axisLabels(linearTicks(0.0, 4.0e6, 400.0, 60.0), "");
    CHECK(big.unit == "\xC3\x97" "10\xE2\x81\xB6");
    CHECK(big.text.back() == "4");
    const std::vector<std::string> log = logAxisLabels(logTicks(10.0, 100000.0, 160.0, 50.0), "Hz");
    REQUIRE(log.size() == 2);
    CHECK(log[0] == "100 Hz");
    CHECK(log[1] == "10 kHz");
}

TEST_CASE("Quantities as shown: significant digits, prefixes, minus sign, comma", "[plot]")
{
    CHECK(formatFixed(-1.5, 2) == kMinus + "1,50");
    CHECK(formatFixed(-0.0001, 2) == "0,00");
    CHECK((formatFixed(2.5, 0) == "2" || formatFixed(2.5, 0) == "3")); // never "2,5"
    CHECK(formatFixed(kNaN, 2) == "\xE2\x80\x94");
    CHECK(formatQuantity(0.001234, "s") == "1,234 ms");
    CHECK(formatQuantity(48000.0, "Hz") == "48,00 kHz");
    CHECK(formatQuantity(-3.3, "V") == kMinus + "3,300 V");
    CHECK(formatQuantity(0.0, "V") == "0 V");
    CHECK(formatQuantity(2.2e-6, "s", 3) == "2,20 " + kMicro + "s");
    CHECK(formatQuantity(999.96, "Hz") == "1,000 kHz");
    CHECK(formatQuantity(3600.0, "s") == "3600 s");
    CHECK(formatQuantity(-6.02, "dB", 3) == kMinus + "6,02 dB");
    CHECK(formatQuantity(1234567.0, "") == "1,235\xC3\x97" "10\xE2\x81\xB6");
    CHECK(formatQuantity(12.5, "") == "12,50");
    CHECK(formatQuantity(9.9996, "V") == "10,00 V");
    CHECK(formatCompact(2000.0, "Hz") == "2 kHz");
    CHECK(formatCompact(0.0015, "s") == "1,5 ms");
    CHECK(formatCompact(500.0, "Hz") == "500 Hz");
    CHECK(formatCompact(1e6, "Hz") == "1 MHz");
    CHECK(formatCompact(0.25, "") == "0,25");
    // Units already prefixed, or not SI, stay as written.
    CHECK(takesPrefix("A"));
    CHECK(takesPrefix("Bd")); // "1 kBd" of a symbol rate
    CHECK(takesPrefix("Pa"));
    CHECK(takesPrefix("m"));
    CHECK(takesPrefix("\xCE\xA9"));
    CHECK(!takesPrefix("hPa"));
    CHECK(!takesPrefix("kHz"));
    CHECK(!takesPrefix("mA"));
    CHECK(!takesPrefix("\xC2\xB0" "C"));
    CHECK(!takesPrefix("rpm"));
    CHECK(!takesPrefix("ppm"));
    CHECK(takesPrefix("V\xC2\xB7s"));
    CHECK(takesPrefix("V/s"));
    CHECK(takesPrefix("m/s\xC2\xB2"));
    CHECK(!takesPrefix("1/s"));
    CHECK(!takesPrefix("dB/Hz"));
    CHECK(!takesPrefix("hPa/s"));
    CHECK(formatQuantity(1013.25, "hPa") == "1013 hPa");
    CHECK(formatQuantity(0.12, "A") == "120,0 mA");
    CHECK(axisLabels(linearTicks(1009.0, 1014.0, 300.0, 30.0), "hPa").unit == "hPa");
    // Digits for a readout: enough to tell values a resolution apart.
    CHECK(significantFor(2.4e9, 6.0) == 10);
    CHECK(significantFor(0.5, 0.0022) == 4);
    CHECK(significantFor(67.6, 0.03) == 4);
    CHECK(significantFor(6000.0, 0.001) == 7);
    CHECK(significantFor(0.0, 1.0) == 4);
    CHECK(formatQuantity(2.400000301e9, "Hz", significantFor(2.400000301e9, 6.0)) == "2,400000301 GHz");
    // Digits for statistics: enough to see the spread of the values.
    CHECK(significantForSpread(440.0, 8.45e-3) == 7);
    CHECK(formatQuantity(440.0021, "Hz", significantForSpread(440.0, 8.45e-3)) == "440,0021 Hz");
    CHECK(significantForSpread(5.0, 2e-3) == 5);
    CHECK(significantForSpread(0.6, 1.2) == 4);
    CHECK(significantForSpread(440.0, 0.0) == 4);
    CHECK(significantForSpread(1e6, 1e-6) == 9);
}

TEST_CASE("Quantities typed: units, prefixes, comma or point, spaces between thousands", "[plot]")
{
    const auto value = [](std::string_view text, std::string_view unit, double bare = 1.0) {
        const Parsed p = parseQuantity(text, unit, bare);
        return p.ok ? p.value : kNaN;
    };
    CHECK(value("2 kHz", "Hz") == 2000.0);
    CHECK(value("2kHz", "Hz") == 2000.0);
    CHECK(value("1,5 ms", "s") == 0.0015);
    CHECK(value("1.5 ms", "s") == 0.0015);
    CHECK(value("115200", "") == 115200.0);
    CHECK(value("115 200", "") == 115200.0);
    CHECK(value("1\xC2\xA0" "200,5", "") == 1200.5);
    CHECK(value(kMinus + "3,3 V", "V") == -3.3);
    CHECK(value("-3.3", "V") == -3.3);
    CHECK(near(value("3,3e-3 V", "V"), 3.3e-3));
    CHECK(near(value("3,3E-3", "V"), 3.3e-3));
    CHECK(near(value("10 " + kMicro + "s", "s"), 1e-5));
    CHECK(near(value("10 us", "s"), 1e-5));
    CHECK(value("10k", "") == 10000.0);
    CHECK(value("10 k", "Hz") == 10000.0);
    CHECK(value("5 MHz", "Hz") == 5e6);
    CHECK(near(value("5 mHz", "Hz"), 5e-3));
    CHECK(value("5 hz", "Hz") == 5.0);
    CHECK(value("2", "Hz", 1000.0) == 2000.0); // a bare number in the prefix the field shows
    CHECK(value("3 m", "m") == 3.0);           // metres, not milli
    CHECK(value("  7  ", "V") == 7.0);
    CHECK(value("-6 dB", "dB") == -6.0);
    const Parsed wrong = parseQuantity("2 kV", "Hz");
    CHECK(!wrong.ok);
    CHECK(wrong.error.find("kV") != std::string::npos);
    CHECK(!parseQuantity("", "V").ok);
    CHECK(!parseQuantity("abc", "V").ok);
    CHECK(!parseQuantity("1e999", "V").ok);
    CHECK(!parseQuantity("10 kdB", "dB").ok);
}

TEST_CASE("Min/max pyramid: extremes of any range, NaN ignored, same as a plain scan", "[plot]")
{
    qacq::dsp::Random random(17);
    std::vector<double> values(100003);
    for (double &v : values) v = random.normal();
    for (std::size_t i = 5000; i < 5100; ++i) values[i] = kNaN;
    values[77777] = 9.0;
    values[123] = -8.0;
    MinMaxPyramid pyramid;
    pyramid.build(values);
    CHECK(pyramid.sampleCount() == values.size());
    CHECK(pyramid.levelCount() >= 6);
    bool all = true;
    for (int k = 0; k < 2000; ++k)
    {
        std::size_t a = static_cast<std::size_t>(random.uniform() * static_cast<double>(values.size()));
        std::size_t b = static_cast<std::size_t>(random.uniform() * static_cast<double>(values.size()));
        if (k % 4 == 0) b = a + static_cast<std::size_t>(random.uniform() * 70.0);
        if (a > b) std::swap(a, b);
        b = std::min(b, values.size());
        double mn = 0, mx = 0, rmn = std::numeric_limits<double>::infinity(), rmx = -rmn;
        const bool found = pyramid.extremes(values, a, b, mn, mx);
        for (std::size_t i = a; i < b; ++i)
        {
            if (std::isfinite(values[i]))
            {
                rmn = std::min(rmn, values[i]);
                rmx = std::max(rmx, values[i]);
            }
        }
        const bool expected = rmn <= rmx;
        all = all && found == expected && (!found || (mn == rmn && mx == rmx));
    }
    CHECK(all);
    double mn = 0, mx = 0;
    CHECK(pyramid.extremes(values, 0, values.size(), mn, mx));
    CHECK(mn == -8.0);
    CHECK(mx == 9.0);
    CHECK(!pyramid.extremes(values, 5010, 5090, mn, mx)); // only NaN
    CHECK(!pyramid.extremes(values, 10, 10, mn, mx));
}

TEST_CASE("Dense trace: every column keeps its extremes, joined in order", "[plot]")
{
    std::vector<double> values(10000);
    for (std::size_t i = 0; i < values.size(); ++i) values[i] = std::sin(static_cast<double>(i) * 0.01);
    values[4321] = 5.0; // a single-sample spike must show at any zoom
    MinMaxPyramid pyramid;
    pyramid.build(values);
    const MinMaxPyramid *withPyramid = &pyramid;
    for (const MinMaxPyramid *p : {static_cast<const MinMaxPyramid *>(nullptr), withPyramid})
    {
        ChannelView channel{values, 0.0, {}, p};
        const Trace trace = buildTrace(channel, {0.0, 10000.0 / 400.0, 400});
        REQUIRE(trace.dense);
        REQUIRE(trace.columns.size() == 400);
        CHECK(trace.maximum == 5.0);
        CHECK(trace.columns[4321 * 400 / 10000].maximum == 5.0);
        bool exact = true, joined = true;
        for (std::size_t c = 0; c < 400; ++c)
        {
            const std::size_t a = c * 25, b = a + 25;
            const double mn = *std::min_element(values.begin() + static_cast<std::ptrdiff_t>(a), values.begin() + static_cast<std::ptrdiff_t>(b));
            const double mx = *std::max_element(values.begin() + static_cast<std::ptrdiff_t>(a), values.begin() + static_cast<std::ptrdiff_t>(b));
            const TraceColumn &col = trace.columns[c];
            exact = exact && col.valid && col.minimum == mn && col.maximum == mx && col.first == values[a] &&
                    col.last == values[b - 1];
            joined = joined && col.joined == (c > 0);
        }
        CHECK(exact);
        CHECK(joined);
    }
    // One line, four points at most per column, inside the lane.
    ChannelView channel{values, 0.0, {}, &pyramid};
    const Trace trace = buildTrace(channel, {0.0, 25.0, 400});
    const auto lines = traceLines(trace, {-1.0, 5.0, 10.0, 300.0, false});
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].size() <= 1600);
    bool inside = true;
    for (const PixelPoint &p : lines[0]) inside = inside && p.y >= 10.0f - 1e-3f && p.y <= 310.0f + 1e-3f && p.x >= 0.0f && p.x <= 400.0f;
    CHECK(inside);
}

TEST_CASE("Sparse trace: the samples themselves, a line cut at breaks and missing samples", "[plot]")
{
    std::vector<double> values = {0, 1, 2, 3, 4, kNaN, 6, 7, 8, 9};
    const std::vector<std::size_t> breaks = {3};
    // Frames 100..109; the window shows frames 102 to 108 on 60 px (0.1 frame per pixel).
    ChannelView channel{values, 100.0, breaks, nullptr};
    const Trace trace = buildTrace(channel, {102.0, 0.1, 60});
    CHECK(!trace.dense);
    REQUIRE(!trace.points.empty());
    // One sample before the window and the ones after it are kept, so the line enters and leaves the view.
    CHECK(trace.points.front().x < 0.0);
    CHECK(trace.points.back().x > 60.0);
    CHECK(trace.minimum == 2.0);
    CHECK(trace.maximum == 8.0);
    const auto lines = traceLines(trace, {0.0, 10.0, 0.0, 100.0, false});
    // [1, 2] | [3, 4] (break before 3) | [6, 7, 8, 9] (missing 5)
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].size() == 2);
    CHECK(lines[1].size() == 2);
    CHECK(lines[2].size() == 4);
    CHECK(std::abs(lines[1][0].x - 10.0f) < 1e-4f); // frame 103 at (103 - 102) / 0.1
    CHECK(std::abs(lines[1][0].y - 70.0f) < 1e-4f);  // value 3 on 0..10 over 100 px
    // Dense columns cut at a break too.
    std::vector<double> ramp(1000, 1.0);
    const std::vector<std::size_t> cut = {500};
    const Trace dense = buildTrace({ramp, 0.0, cut, nullptr}, {0.0, 10.0, 100});
    CHECK(dense.dense);
    CHECK(!dense.columns[50].joined);
    CHECK(dense.columns[51].joined);
    CHECK(traceLines(dense, {0.0, 2.0, 0.0, 10.0, false}).size() == 2);
    // Outside the data: nothing.
    CHECK(buildTrace(channel, {5000.0, 1.0, 50}).empty());
    CHECK(buildTrace(channel, {0.0, 1.0, 0}).empty());
}

TEST_CASE("Value axes: linear and log mapping, padded and steady automatic ranges, zoom", "[plot]")
{
    const YMap linear{-1.0, 1.0, 10.0, 200.0, false};
    CHECK(linear.toPixel(1.0) == 10.0);
    CHECK(linear.toPixel(-1.0) == 210.0);
    CHECK(linear.toPixel(0.0) == 110.0);
    CHECK(near(linear.toValue(60.0), 0.5));
    const YMap log{1.0, 1000.0, 0.0, 300.0, true};
    CHECK(near(log.toPixel(10.0), 200.0));
    CHECK(near(log.toValue(100.0), 100.0));
    CHECK(std::isnan(log.toPixel(0.0)));

    const Range padded = paddedRange(-1.0, 3.0);
    CHECK(near(padded.low, -1.2));
    CHECK(near(padded.high, 3.2));
    const Range flat = paddedRange(5.0, 5.0);
    CHECK(near(flat.low, 4.5));
    CHECK(near(flat.high, 5.5));
    CHECK(paddedRange(0.0, 0.0).low == -1.0);
    CHECK(paddedRange(kNaN, 1.0).low == -1.0); // falls back to ±1

    Range r = steadyRange({0.0, 0.0}, -1.0, 1.0); // no scale yet: the padded one
    CHECK(near(r.low, -1.1));
    CHECK(near(r.high, 1.1));
    // Slightly smaller data: the scale stays.
    Range same = steadyRange(r, -0.8, 0.9);
    CHECK(same.low == r.low);
    CHECK(same.high == r.high);
    // Data going out: at once.
    Range out = steadyRange(r, -1.0, 2.0);
    CHECK(out.high > 2.0);
    // Much smaller data: it shrinks.
    Range small = steadyRange(r, -0.1, 0.1);
    CHECK(small.span() < 0.3);

    const Range z = zoomRange({0.0, 10.0}, 0.5, 2.0);
    CHECK(near(z.low, 1.0));
    CHECK(near(z.high, 6.0));
    const Range clamped = clampRange({-3.0, 2.0}, {0.0, 10.0}, 0.01);
    CHECK(clamped.low == 0.0);
    CHECK(clamped.high == 5.0);
    const Range wide = clampRange({-30.0, 30.0}, {0.0, 10.0}, 0.01);
    CHECK(wide.low == 0.0);
    CHECK(wide.high == 10.0);
    const Range narrow = clampRange({5.0, 5.000001}, {0.0, 10.0}, 0.01);
    CHECK(near(narrow.span(), 0.01));
    CHECK(near(0.5 * (narrow.low + narrow.high), 5.0000005));
    const Range up = shiftRange({-1.0, 3.0}, 0.25);
    CHECK(near(up.low, 0.0));
    CHECK(near(up.high, 4.0));
    const Range down = shiftRange({-1.0, 3.0}, -0.5);
    CHECK(near(down.low, -3.0));
    CHECK(near(down.high, 1.0));
    CHECK(shiftRange({2.0, 5.0}, kNaN).low == 2.0);
    CHECK(nearestSample(2.4, 10) == 2);
    CHECK(nearestSample(2.6, 10) == 3);
    CHECK(nearestSample(-5.0, 10) == 0);
    CHECK(nearestSample(50.0, 10) == 9);
    CHECK(nearestSample(1.0, 0) == -1);
}

TEST_CASE("Drags: the axis they move is chosen once the pointer has gone far enough", "[plot]")
{
    CHECK(dragAxes(3.0, 2.0) == DragAxes::Undecided); // 3.6 px: not yet
    CHECK(dragAxes(0.0, 0.0) == DragAxes::Undecided);
    CHECK(dragAxes(kNaN, 50.0) == DragAxes::Undecided);
    // Mostly sideways (a scroll in time that wanders a little): the time only.
    CHECK(dragAxes(10.0, 2.0) == DragAxes::Horizontal);
    CHECK(dragAxes(-10.0, 5.0) == DragAxes::Horizontal);
    CHECK(dragAxes(40.0, -19.0) == DragAxes::Horizontal);
    // Mostly up or down: the values only.
    CHECK(dragAxes(2.0, 10.0) == DragAxes::Vertical);
    CHECK(dragAxes(-4.0, -10.0) == DragAxes::Vertical);
    // In between: both.
    CHECK(dragAxes(8.0, 8.0) == DragAxes::Both);
    CHECK(dragAxes(10.0, -6.0) == DragAxes::Both);
    CHECK(dragAxes(-6.0, 10.0) == DragAxes::Both);
    // Another threshold.
    CHECK(dragAxes(3.0, 0.0, 2.0) == DragAxes::Horizontal);
    CHECK(dragAxes(3.0, 0.0, 4.0) == DragAxes::Undecided);
}
