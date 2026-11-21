#include "qacq/plot/trace.h"

#include <algorithm>
#include <cmath>

namespace qacq::plot
{
namespace
{
constexpr double kInfinity = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

void scanRaw(std::span<const double> values, std::size_t first, std::size_t last, double &minimum, double &maximum)
{
    for (std::size_t i = first; i < last; ++i)
    {
        const double v = values[i];
        if (!std::isfinite(v)) continue;
        minimum = std::min(minimum, v);
        maximum = std::max(maximum, v);
    }
}

bool isBreak(std::span<const std::size_t> breaks, std::size_t index)
{
    return std::binary_search(breaks.begin(), breaks.end(), index);
}

void include(Trace &trace, double minimum, double maximum)
{
    if (!(trace.minimum <= trace.maximum))
    {
        trace.minimum = minimum;
        trace.maximum = maximum;
        return;
    }
    trace.minimum = std::min(trace.minimum, minimum);
    trace.maximum = std::max(trace.maximum, maximum);
}

std::size_t clampIndex(double value, std::size_t count)
{
    if (!(value > 0.0)) return 0;
    if (value >= static_cast<double>(count)) return count;
    return static_cast<std::size_t>(value);
}

// A value of the column that is a number (the first one from `from`, scanning a few samples).
double firstNumber(std::span<const double> values, std::size_t from, std::size_t to, double fallback)
{
    for (std::size_t i = from, n = 0; i < to && n < 8; ++i, ++n)
    {
        if (std::isfinite(values[i])) return values[i];
    }
    return fallback;
}

double lastNumber(std::span<const double> values, std::size_t from, std::size_t to, double fallback)
{
    for (std::size_t i = to, n = 0; i > from && n < 8; --i, ++n)
    {
        if (std::isfinite(values[i - 1])) return values[i - 1];
    }
    return fallback;
}

float pixel(double value)
{
    return static_cast<float>(std::clamp(value, -1.0e5, 1.0e5));
}
} // namespace

void MinMaxPyramid::clear()
{
    m_levels.clear();
    m_count = 0;
}

void MinMaxPyramid::build(std::span<const double> values)
{
    clear();
    m_count = values.size();
    if (m_count < kFirstBlock) return;
    Level first;
    first.block = kFirstBlock;
    const std::size_t blocks = (m_count + kFirstBlock - 1) / kFirstBlock;
    first.minimum.resize(blocks);
    first.maximum.resize(blocks);
    for (std::size_t b = 0; b < blocks; ++b)
    {
        double mn = kInfinity, mx = -kInfinity;
        scanRaw(values, b * kFirstBlock, std::min(m_count, (b + 1) * kFirstBlock), mn, mx);
        first.minimum[b] = mn;
        first.maximum[b] = mx;
    }
    m_levels.push_back(std::move(first));
    while (m_levels.back().minimum.size() > 4)
    {
        const Level &below = m_levels.back();
        Level next;
        next.block = below.block * 4;
        const std::size_t count = (below.minimum.size() + 3) / 4;
        next.minimum.resize(count);
        next.maximum.resize(count);
        for (std::size_t b = 0; b < count; ++b)
        {
            double mn = kInfinity, mx = -kInfinity;
            for (std::size_t k = b * 4; k < std::min(below.minimum.size(), b * 4 + 4); ++k)
            {
                mn = std::min(mn, below.minimum[k]);
                mx = std::max(mx, below.maximum[k]);
            }
            next.minimum[b] = mn;
            next.maximum[b] = mx;
        }
        m_levels.push_back(std::move(next));
    }
}

void MinMaxPyramid::range(std::span<const double> values, std::size_t level, std::size_t first, std::size_t last,
                          double &minimum, double &maximum) const
{
    if (first >= last) return;
    const Level &l = m_levels[level];
    const std::size_t a = (first + l.block - 1) / l.block * l.block;
    const std::size_t z = last / l.block * l.block;
    if (a >= z)
    {
        if (level == 0) scanRaw(values, first, last, minimum, maximum);
        else range(values, level - 1, first, last, minimum, maximum);
        return;
    }
    if (level == 0)
    {
        scanRaw(values, first, a, minimum, maximum);
        scanRaw(values, z, last, minimum, maximum);
    }
    else
    {
        range(values, level - 1, first, a, minimum, maximum);
        range(values, level - 1, z, last, minimum, maximum);
    }
    for (std::size_t b = a / l.block; b < z / l.block; ++b)
    {
        minimum = std::min(minimum, l.minimum[b]);
        maximum = std::max(maximum, l.maximum[b]);
    }
}

bool MinMaxPyramid::extremes(std::span<const double> values, std::size_t first, std::size_t last, double &minimum,
                             double &maximum) const
{
    last = std::min(last, values.size());
    double mn = kInfinity, mx = -kInfinity;
    if (first < last)
    {
        if (m_levels.empty() || m_count != values.size() || last - first < 2 * kFirstBlock)
        {
            scanRaw(values, first, last, mn, mx);
        }
        else
        {
            std::size_t level = 0;
            while (level + 1 < m_levels.size() && m_levels[level + 1].block * 2 <= last - first) ++level;
            range(values, level, first, last, mn, mx);
        }
    }
    if (!(mn <= mx)) return false;
    minimum = mn;
    maximum = mx;
    return true;
}

Trace buildTrace(const ChannelView &channel, const XWindow &window)
{
    Trace trace;
    const std::span<const double> values = channel.values;
    const std::size_t n = values.size();
    const double fpp = window.framesPerPixel;
    if (window.width <= 0 || n == 0 || !(fpp > 0.0) || !std::isfinite(window.firstFrame)) return trace;
    const double offset = window.firstFrame - channel.firstFrame; // index at the left edge
    const auto width = static_cast<std::size_t>(window.width);

    if (fpp > 2.0)
    {
        trace.dense = true;
        trace.columns.resize(width);
        bool previous = false;
        for (std::size_t c = 0; c < width; ++c)
        {
            TraceColumn &column = trace.columns[c];
            const std::size_t a = clampIndex(std::ceil(offset + static_cast<double>(c) * fpp), n);
            const std::size_t b = clampIndex(std::ceil(offset + static_cast<double>(c + 1) * fpp), n);
            double mn = kInfinity, mx = -kInfinity;
            bool found = false;
            if (a < b)
            {
                if (channel.pyramid) found = channel.pyramid->extremes(values, a, b, mn, mx);
                else
                {
                    scanRaw(values, a, b, mn, mx);
                    found = mn <= mx;
                }
            }
            if (!found)
            {
                previous = false;
                continue;
            }
            column.minimum = mn;
            column.maximum = mx;
            column.first = firstNumber(values, a, b, mn);
            column.last = lastNumber(values, a, b, mx);
            column.valid = true;
            column.joined = previous && !isBreak(channel.breaks, a) && std::isfinite(values[a]) && a > 0 &&
                            std::isfinite(values[a - 1]);
            previous = true;
            include(trace, mn, mx);
        }
        return trace;
    }

    const double left = std::floor(offset) - 1.0;
    const double right = std::ceil(offset + static_cast<double>(width) * fpp) + 2.0;
    const std::size_t i0 = clampIndex(left, n);
    const std::size_t i1 = clampIndex(right, n);
    bool previous = false;
    for (std::size_t i = i0; i < i1; ++i)
    {
        const double v = values[i];
        if (!std::isfinite(v))
        {
            previous = false;
            continue;
        }
        const double x = (static_cast<double>(i) - offset) / fpp;
        trace.points.push_back({x, v, previous && !isBreak(channel.breaks, i)});
        previous = true;
        if (x >= -0.5 && x <= static_cast<double>(width) + 0.5) include(trace, v, v);
    }
    return trace;
}

double YMap::toPixel(double value) const noexcept
{
    if (logarithmic)
    {
        if (!(value > 0.0) || !(low > 0.0) || !(high > low)) return kNaN;
        const double l0 = std::log10(low), l1 = std::log10(high);
        return top + (l1 - std::log10(value)) / (l1 - l0) * height;
    }
    if (!(high > low)) return top + 0.5 * height;
    return top + (high - value) / (high - low) * height;
}

double YMap::toValue(double pixel) const noexcept
{
    if (!(height > 0.0)) return kNaN;
    const double f = (pixel - top) / height;
    if (logarithmic)
    {
        if (!(low > 0.0) || !(high > low)) return kNaN;
        const double l0 = std::log10(low), l1 = std::log10(high);
        return std::pow(10.0, l1 - f * (l1 - l0));
    }
    return high - f * (high - low);
}

std::vector<std::vector<PixelPoint>> traceLines(const Trace &trace, const YMap &y)
{
    std::vector<std::vector<PixelPoint>> lines;
    std::vector<PixelPoint> current;
    const auto flush = [&] {
        if (!current.empty()) lines.push_back(std::move(current));
        current.clear();
    };
    const auto add = [&](float x, double value) {
        const double py = y.toPixel(value);
        if (!std::isfinite(py))
        {
            flush();
            return;
        }
        const PixelPoint p{x, pixel(py)};
        if (!current.empty() && current.back().x == p.x && current.back().y == p.y) return;
        current.push_back(p);
    };
    if (trace.dense)
    {
        current.reserve(trace.columns.size() * 4);
        for (std::size_t c = 0; c < trace.columns.size(); ++c)
        {
            const TraceColumn &column = trace.columns[c];
            if (!column.valid)
            {
                flush();
                continue;
            }
            if (!column.joined) flush();
            const float x = static_cast<float>(c) + 0.5f;
            add(x, column.first);
            add(x, column.minimum);
            add(x, column.maximum);
            add(x, column.last);
        }
    }
    else
    {
        current.reserve(trace.points.size());
        for (const TracePoint &p : trace.points)
        {
            if (!p.joined) flush();
            add(pixel(p.x), p.value);
        }
    }
    flush();
    return lines;
}

bool Range::valid() const noexcept
{
    return std::isfinite(low) && std::isfinite(high) && high > low;
}

Range paddedRange(double minimum, double maximum, double margin)
{
    if (!std::isfinite(minimum) || !std::isfinite(maximum)) return {-1.0, 1.0};
    if (maximum < minimum) std::swap(minimum, maximum);
    const double span = maximum - minimum;
    const double centre = 0.5 * (minimum + maximum);
    if (!(span > std::abs(centre) * 1e-12))
    {
        const double half = centre != 0.0 ? std::abs(centre) * 0.1 : 1.0;
        return {centre - half, centre + half};
    }
    return {minimum - margin * span, maximum + margin * span};
}

Range steadyRange(const Range &current, double minimum, double maximum, double shrinkBelow)
{
    if (!std::isfinite(minimum) || !std::isfinite(maximum)) return current.valid() ? current : Range{-1.0, 1.0};
    const Range padded = paddedRange(minimum, maximum);
    if (!current.valid()) return padded;
    if (minimum < current.low || maximum > current.high) return padded;
    if (std::abs(maximum - minimum) < shrinkBelow * current.span()) return padded;
    return current;
}

Range zoomRange(const Range &range, double factor, double anchor)
{
    if (!(factor > 0.0) || !std::isfinite(factor)) return range;
    return {anchor + (range.low - anchor) * factor, anchor + (range.high - anchor) * factor};
}

Range clampRange(const Range &range, const Range &limits, double minimumSpan)
{
    double span = std::max(range.span(), minimumSpan);
    double low = range.low;
    if (span != range.span()) low = 0.5 * (range.low + range.high) - 0.5 * span;
    if (limits.valid())
    {
        if (span > limits.span())
        {
            span = limits.span();
            low = limits.low;
        }
        low = std::clamp(low, limits.low, limits.high - span);
    }
    return {low, low + span};
}

std::ptrdiff_t nearestSample(double frame, std::size_t count) noexcept
{
    if (count == 0) return -1;
    if (!(frame > 0.0)) return 0;
    const double last = static_cast<double>(count - 1);
    if (frame >= last) return static_cast<std::ptrdiff_t>(count - 1);
    return static_cast<std::ptrdiff_t>(std::llround(frame));
}
} // namespace qacq::plot
