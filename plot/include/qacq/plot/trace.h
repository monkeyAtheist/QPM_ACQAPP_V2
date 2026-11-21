#pragma once

// Drawing a channel quickly at any zoom, the same way in every view of the application.
//
// The visible time span is cut in pixel columns. With more than two samples per column, each column
// keeps its extremes and its first and last samples (a vertical stroke joined to its neighbours: no
// peak is ever lost, whatever the zoom); otherwise the samples themselves are joined. Discontinuities
// of the record and missing samples (NaN) cut the line. A pyramid of block extremes answers the
// extremes of any range in O(log n), so ten million samples draw as fast as a thousand.
// Pure C++20, no Qt: the plot item only turns the lines into QPainter strokes.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace qacq::plot
{
class MinMaxPyramid
{
public:
    static constexpr std::size_t kFirstBlock = 16; // samples per block of the first level, then x4

    void build(std::span<const double> values);
    void clear();
    std::size_t sampleCount() const noexcept { return m_count; }
    std::size_t levelCount() const noexcept { return m_levels.size(); }
    // Extremes of values[first, last), NaN ignored; false when there is none. `values` must be the span
    // the pyramid was built from.
    bool extremes(std::span<const double> values, std::size_t first, std::size_t last, double &minimum,
                  double &maximum) const;

private:
    struct Level
    {
        std::size_t block = 0;
        std::vector<double> minimum, maximum;
    };
    void range(std::span<const double> values, std::size_t level, std::size_t first, std::size_t last,
               double &minimum, double &maximum) const;

    std::vector<Level> m_levels;
    std::size_t m_count = 0;
};

// One channel as a plot reads it.
struct ChannelView
{
    std::span<const double> values;
    double firstFrame = 0.0; // frame number of values[0] in the time base of the plot
    // Indices in `values` before which the line is cut (sorted): discontinuities of the record.
    std::span<const std::size_t> breaks;
    const MinMaxPyramid *pyramid = nullptr; // optional, built from `values`
};

// The visible part of the time base: frame `firstFrame` at the left edge, `framesPerPixel` per pixel.
struct XWindow
{
    double firstFrame = 0.0;
    double framesPerPixel = 1.0;
    int width = 0;
};

struct TraceColumn
{
    double minimum = 0.0, maximum = 0.0, first = 0.0, last = 0.0;
    bool valid = false;  // at least one sample
    bool joined = false; // joined to the column before
};

struct TracePoint
{
    double x = 0.0; // in pixels from the left edge (may lie just outside: the line enters the view)
    double value = 0.0;
    bool joined = false; // joined to the point before
};

struct Trace
{
    bool dense = false;               // columns (more than two samples per pixel) rather than points
    std::vector<TraceColumn> columns; // dense: one per pixel column, drawn at x = index + 0.5
    std::vector<TracePoint> points;   // otherwise
    // Extremes of the samples in view (NaN when there is none), for the automatic scale.
    double minimum = std::numeric_limits<double>::quiet_NaN();
    double maximum = std::numeric_limits<double>::quiet_NaN();
    bool empty() const noexcept { return !(minimum <= maximum); }
};

// What a column holding many samples shows: its extremes (a time signal: no peak lost, no dip either), or
// its highest value only (a spectrum, as an analyser's peak detector: the lines between the peaks of a
// spectrum are not filled down to its deepest notch).
enum class Columns
{
    Extremes,
    Peaks
};
Trace buildTrace(const ChannelView &channel, const XWindow &window, Columns columns = Columns::Extremes);
// The same on an axis that is not linear in frames (frequencies on a log scale): `edges` holds the frame
// position of each pixel boundary, increasing, width + 1 values (column c spans edges[c] to edges[c + 1]).
// A column holding more than two samples becomes a vertical stroke at its centre (first, extremes, last);
// otherwise its samples sit at their own place. The result is always points (Trace::dense false).
Trace buildTraceEdges(const ChannelView &channel, std::span<const double> edges, Columns columns = Columns::Extremes);

// Value axis of a lane: `high` at pixel `top`, `low` at `top + height`; log10 scale if asked (values
// <= 0 are then not drawn).
struct YMap
{
    double low = 0.0, high = 1.0;
    double top = 0.0, height = 1.0;
    bool logarithmic = false;

    double toPixel(double value) const noexcept;
    double toValue(double pixel) const noexcept;
};

struct PixelPoint
{
    float x = 0.0f, y = 0.0f;
};

// The broken lines to draw (pixel coordinates, x from the left edge of the window).
std::vector<std::vector<PixelPoint>> traceLines(const Trace &trace, const YMap &y);

struct Range
{
    double low = 0.0, high = 1.0;
    double span() const noexcept { return high - low; }
    bool valid() const noexcept;
};

// Range shown for data in [minimum, maximum]: `margin` of the span each side; a flat signal gets ±10 %
// of its value (±1 at zero).
Range paddedRange(double minimum, double maximum, double margin = 0.05);
// Automatic scale that does not shake: it follows data going out of it at once, and shrinks only when
// the data use less than `shrinkBelow` of it.
Range steadyRange(const Range &current, double minimum, double maximum, double shrinkBelow = 0.4);
// Zoom about `anchor` (factor < 1 zooms in).
Range zoomRange(const Range &range, double factor, double anchor);
// Kept inside `limits` (moved, then cut) and at least `minimumSpan` wide.
Range clampRange(const Range &range, const Range &limits, double minimumSpan);
// The range moved by a fraction of its span (positive: towards higher values), same width.
Range shiftRange(const Range &range, double fraction) noexcept;

// What a drag of a plot moves, decided once the pointer has gone `threshold` pixels from where it was
// pressed: mostly sideways, the horizontal axis only; mostly up or down, the vertical one only; in between
// (from about 27° to 63° from the horizontal), both. A scroll in time that wanders a little up or down
// leaves the scale of the values alone.
enum class DragAxes
{
    Undecided,
    Horizontal,
    Vertical,
    Both
};
DragAxes dragAxes(double dx, double dy, double threshold = 6.0) noexcept;
// The nearest sample to a frame position, clamped to [0, count); -1 when count is 0.
std::ptrdiff_t nearestSample(double frame, std::size_t count) noexcept;
} // namespace qacq::plot
