#pragma once

// Axes of the plots: where the graduations go and how they read.
//   linearTicks, logTicks   graduations 1-2-5 at least `spacing` pixels apart, minor ones between
//   axisLabels              one engineering prefix for the whole axis ("Temps (ms)" : 0 ; 0,5 ; 1...)
//   formatQuantity          a value and its unit as the readouts show it ("1,234 ms", "−3,300 V")
//   parseQuantity           what the user types ("2 kHz", "1,5 ms", "115 200", "3,3e-3 V", "10k")
// Pure C++20, no Qt: the plot items of the interface and the fields of the panels share it.

#include <string>
#include <string_view>
#include <vector>

namespace qacq::plot
{
struct Ticks
{
    std::vector<double> major;
    std::vector<double> minor;
    double step = 0.0; // between major ticks on a linear axis; 0 on a log axis
};

// A step of 1, 2 or 5 times a power of ten, at least `minimum`.
double niceStep(double minimum);

// Graduations of [low, high] drawn over `pixels`, major ones at least `spacing` pixels apart.
Ticks linearTicks(double low, double high, double pixels, double spacing);
// Log axis (0 < low < high): the decades, 2 and 5 between them when there is room, one decade out of
// two (three...) when there is not; minor ticks 2 to 9 when the decades are wide enough.
Ticks logTicks(double low, double high, double pixels, double spacing);

struct Prefix
{
    double factor = 1.0;
    std::string_view symbol; // "p", "n", "µ", "m", "", "k", "M", "G", "T"
};
// The engineering prefix of a magnitude (factor 1e-12 to 1e12). `largest`: highest power of a thousand
// allowed (0 for seconds: "1 000 s", never "1 ks").
Prefix engineeringPrefix(double magnitude, int largest = 4);
// Units written with a prefix: SI ones ("s", "Hz", "V", "A", "Pa", "m", "Ω"...), and compound units whose
// first symbol is one ("V·s", "V/s", "m/s²"). The others keep their unit as written: none, already
// prefixed ("hPa", "kHz", "mA"), decibels, "%", "°C", "rpm", "ppm", "1/s"...
bool takesPrefix(std::string_view unit);
// Significant digits that show `value` down to `resolution` (a fraction of the range on screen), within
// [minimum, maximum]: 2,4 GHz read to the hertz needs ten.
int significantFor(double value, double resolution, int minimum = 4, int maximum = 12);
// Significant digits for values that spread over `spread` around `magnitude` (statistics of a part, the
// values under two cursors): about twenty steps of the last digit across the spread, so that they can be
// told apart. 440 Hz ± 4 mHz → 7 ("440,0021 Hz"), 5 V ± 1 mV → 5, a signal using its whole scale (or no
// spread) → `minimum`.
int significantForSpread(double magnitude, double spread, int minimum = 4, int maximum = 9);

struct AxisLabels
{
    std::string unit;    // to show in the title: "ms", "kHz", "V", "×10⁶" or empty
    double factor = 1.0; // a tick reads value / factor
    int decimals = 0;
    std::vector<std::string> text; // one per major tick
};
AxisLabels axisLabels(const Ticks &ticks, std::string_view unit, char decimal = ',');
// A log axis: each tick with its own prefix and the unit ("100 Hz", "1 kHz", "10 kHz").
std::vector<std::string> logAxisLabels(const Ticks &ticks, std::string_view unit, char decimal = ',');

// Fixed decimals, minus sign U+2212, never "−0": formatFixed(-1.5, 2) == "−1,50". NaN and infinities: "—".
std::string formatFixed(double value, int decimals, char decimal = ',');
// `significant` digits with the prefix of the value: "1,234 ms", "48,00 kHz", "−3,300 V", "0 V".
// A bare number far from 1 gets a power of ten: "1,234×10⁶". NaN: "—".
std::string formatQuantity(double value, std::string_view unit, int significant = 4, char decimal = ',');
// Shortest form, at most `significant` digits, no trailing zero: "2 kHz", "1,5 ms", "500 Hz".
std::string formatCompact(double value, std::string_view unit, int significant = 3, char decimal = ',');

struct Parsed
{
    bool ok = false;
    double value = 0.0; // in the unit given to parseQuantity (no prefix)
    std::string error;  // in French, when !ok
};
// Reads a number with an optional unit: the unit written must be `unit`, with or without a prefix
// ("kHz", "mV", "µs"; "u" stands for "µ"); a lone prefix also counts ("10k"). A bare number is read in
// `bareFactor` times the unit (the prefix the field shows). Decimal point or comma, spaces between
// thousands, "−" or "-".
Parsed parseQuantity(std::string_view text, std::string_view unit, double bareFactor = 1.0);
} // namespace qacq::plot
