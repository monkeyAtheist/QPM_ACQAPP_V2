#include "qacq/analysis/measures.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace qacq::analysis
{
namespace
{
// Time where the line from sample i to i + 1 crosses `level` (seconds), i + 1 inside the record.
double crossing(std::span<const double> x, std::size_t i, double level, double fs)
{
    const double a = x[i], b = x[i + 1];
    const double fraction = b != a ? std::clamp((level - a) / (b - a), 0.0, 1.0) : 0.5;
    return (static_cast<double>(i) + fraction) / fs;
}
} // namespace

SpanStatistics spanStatistics(std::span<const double> values)
{
    SpanStatistics s;
    double sum = 0.0, squares = 0.0;
    double mn = std::numeric_limits<double>::infinity(), mx = -mn;
    for (double v : values)
    {
        if (!std::isfinite(v)) continue;
        if (s.samples == 0) s.first = v;
        s.last = v;
        ++s.samples;
        sum += v;
        squares += v * v;
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    if (s.samples == 0) return s;
    const auto n = static_cast<double>(s.samples);
    s.mean = sum / n;
    s.rms = std::sqrt(squares / n);
    double ac = 0.0;
    for (double v : values)
    {
        if (std::isfinite(v)) ac += (v - s.mean) * (v - s.mean);
    }
    s.acRms = std::sqrt(ac / n);
    s.minimum = mn;
    s.maximum = mx;
    s.peakToPeak = mx - mn;
    return s;
}

namespace
{
struct EdgeScan
{
    std::vector<double> rises, falls; // middle crossings (s)
    double riseSum = 0.0, fallSum = 0.0;
    std::size_t riseCount = 0, fallCount = 0;
};

// Levels of the record: modes of the two halves of a histogram, if they stand out; otherwise the extremes.
// False when the record is flat.
bool findLevels(std::span<const double> x, const SpanStatistics &stats, WaveMeasures &w)
{
    const double mn = stats.minimum, mx = stats.maximum;
    if (!(mx - mn > 1e-12 * std::max({std::abs(mx), std::abs(mn), 1e-300}))) return false;
    constexpr std::size_t kClasses = 100;
    std::vector<std::size_t> histogram(kClasses, 0);
    const double width = (mx - mn) / static_cast<double>(kClasses);
    for (double v : x)
        ++histogram[std::min(kClasses - 1, static_cast<std::size_t>((v - mn) / width))];
    const auto lowMode = std::max_element(histogram.begin(), histogram.begin() + kClasses / 2);
    const auto highMode = std::max_element(histogram.begin() + kClasses / 2, histogram.end());
    const auto total = static_cast<double>(x.size());
    const double clear = 0.02 * total; // a level holds 2 % of the samples at least
    const bool clearBase = static_cast<double>(*lowMode) >= clear;
    const bool clearTop = static_cast<double>(*highMode) >= clear;
    w.base = clearBase ? mn + (static_cast<double>(lowMode - histogram.begin()) + 0.5) * width : mn;
    w.top = clearTop ? mn + (static_cast<double>(highMode - histogram.begin()) + 0.5) * width : mx;
    // The extreme classes hold the extremes themselves: no half class of error on a clean square wave.
    if (lowMode == histogram.begin()) w.base = mn;
    if (highMode == histogram.end() - 1) w.top = mx;
    w.flatBase = static_cast<double>(*lowMode) >= 0.08 * total;
    w.flatTop = static_cast<double>(*highMode) >= 0.08 * total;
    w.amplitude = w.top - w.base;
    return w.amplitude > 0.0;
}

// Edges: the middle crossed, with a hysteresis of 10 % of the amplitude; rise and fall times between
// lowFraction and 1 − lowFraction of the amplitude.
EdgeScan scanEdges(std::span<const double> x, double fs, const WaveMeasures &w, double lowFraction)
{
    EdgeScan e;
    const double middle = 0.5 * (w.top + w.base);
    const double high = middle + 0.1 * w.amplitude, low = middle - 0.1 * w.amplitude;
    const double levelLow = w.base + lowFraction * w.amplitude, levelHigh = w.top - lowFraction * w.amplitude;
    enum class State { Unknown, Low, High } state = State::Unknown;
    std::size_t lastBelow = 0, lastAbove = 0; // last samples at or under / at or over the middle
    std::size_t previousEdge = 0;
    const std::size_t n = x.size();
    for (std::size_t i = 0; i < n; ++i)
    {
        const double v = x[i];
        if (v <= middle) lastBelow = i;
        if (v >= middle) lastAbove = i;
        if (v > high && state != State::High)
        {
            if (state == State::Low && lastBelow + 1 < n)
            {
                e.rises.push_back(crossing(x, lastBelow, middle, fs));
                // Low threshold before the crossing (back to the previous edge), high one after it.
                std::size_t a = lastBelow;
                while (a > previousEdge && x[a] > levelLow) --a;
                std::size_t b = lastBelow + 1;
                while (b + 1 < n && x[b] < levelHigh && x[b] >= low) ++b;
                if (x[a] <= levelLow && x[b] >= levelHigh && b > 0 && a + 1 < n)
                {
                    e.riseSum += crossing(x, b - 1, levelHigh, fs) - crossing(x, a, levelLow, fs);
                    ++e.riseCount;
                }
                previousEdge = lastBelow;
            }
            state = State::High;
        }
        else if (v < low && state != State::Low)
        {
            if (state == State::High && lastAbove + 1 < n)
            {
                e.falls.push_back(crossing(x, lastAbove, middle, fs));
                std::size_t a = lastAbove;
                while (a > previousEdge && x[a] < levelHigh) --a;
                std::size_t b = lastAbove + 1;
                while (b + 1 < n && x[b] > levelLow && x[b] <= high) ++b;
                if (x[a] >= levelHigh && x[b] <= levelLow && b > 0 && a + 1 < n)
                {
                    e.fallSum += crossing(x, b - 1, levelLow, fs) - crossing(x, a, levelHigh, fs);
                    ++e.fallCount;
                }
                previousEdge = lastAbove;
            }
            state = State::Low;
        }
    }
    return e;
}

// Time after `edge` (s) from which x stays within `tolerance` of `level` up to the end of the record. A
// zone that ends on the way to the next edge (more than 10 % of the amplitude away, going away steadily)
// is measured up to where that departure starts.
Settling settlingAfter(std::span<const double> x, double fs, double edge, double level, double tolerance,
                       double amplitude)
{
    Settling s;
    std::size_t n = x.size();
    const auto from = static_cast<std::size_t>(std::clamp(std::ceil(edge * fs), 0.0, static_cast<double>(n)));
    if (from >= n) return s;
    if (std::abs(x[n - 1] - level) > 0.1 * amplitude)
    {
        while (n > from + 1 && std::abs(x[n - 1] - level) > tolerance &&
               std::abs(x[n - 1] - level) >= std::abs(x[n - 2] - level))
            --n;
    }
    std::size_t outside = n; // last sample outside the band
    for (std::size_t i = n; i-- > from;)
    {
        if (std::abs(x[i] - level) > tolerance)
        {
            outside = i;
            break;
        }
    }
    if (outside == n - 1) return s; // still outside at the end of the zone
    s.reached = true;
    if (outside == n)
    {
        s.seconds = std::max(0.0, static_cast<double>(from) / fs - edge);
        return s;
    }
    // Where the line from the last sample outside to the next one enters the band.
    const double a = x[outside], b = x[outside + 1];
    const double bound = a > level ? level + tolerance : level - tolerance;
    const double fraction = b != a ? std::clamp((bound - a) / (b - a), 0.0, 1.0) : 1.0;
    s.seconds = std::max(0.0, (static_cast<double>(outside) + fraction) / fs - edge);
    return s;
}
} // namespace

WaveMeasures waveMeasures(std::span<const double> x, double fs, double edgeLowFraction)
{
    WaveMeasures w;
    w.edgeLowFraction = std::isfinite(edgeLowFraction) ? std::clamp(edgeLowFraction, 0.01, 0.4) : 0.1;
    const SpanStatistics stats = spanStatistics(x);
    if (stats.samples < 4 || !(fs > 0.0) || stats.samples < x.size()) return w; // holes: no edge timing
    if (!findLevels(x, stats, w)) return w;
    w.levels = true;
    w.overshootPercent = 100.0 * (stats.maximum - w.top) / w.amplitude;
    w.undershootPercent = 100.0 * (w.base - stats.minimum) / w.amplitude;

    const EdgeScan e = scanEdges(x, fs, w, w.edgeLowFraction);
    const std::vector<double> &rises = e.rises;
    const std::vector<double> &falls = e.falls;
    w.risingEdges = rises.size();
    w.fallingEdges = falls.size();
    if (rises.size() >= 2)
    {
        w.periodic = true;
        w.periodSeconds = (rises.back() - rises.front()) / static_cast<double>(rises.size() - 1);
        w.frequencyHz = w.periodSeconds > 0.0 ? 1.0 / w.periodSeconds : 0.0;
        double dutySum = 0.0;
        std::size_t dutyCount = 0;
        std::size_t j = 0;
        for (std::size_t k = 0; k + 1 < rises.size(); ++k)
        {
            while (j < falls.size() && falls[j] <= rises[k]) ++j;
            if (j < falls.size() && falls[j] < rises[k + 1])
            {
                dutySum += (falls[j] - rises[k]) / (rises[k + 1] - rises[k]);
                ++dutyCount;
            }
        }
        if (dutyCount > 0)
        {
            w.duty = true;
            w.dutyCyclePercent = 100.0 * dutySum / static_cast<double>(dutyCount);
        }
        // Whole periods: the samples from the first rising edge up to (not including) the last one.
        const auto first = static_cast<std::size_t>(std::ceil(rises.front() * fs));
        const auto last = std::min(x.size(), static_cast<std::size_t>(std::ceil(rises.back() * fs)));
        if (last > first)
        {
            double sum = 0.0, squares = 0.0;
            for (std::size_t i = first; i < last; ++i)
            {
                sum += x[i];
                squares += x[i] * x[i];
            }
            const auto count = static_cast<double>(last - first);
            w.cycles = true;
            w.cycleCount = rises.size() - 1;
            w.cycleMean = sum / count;
            w.cycleRms = std::sqrt(squares / count);
        }
    }
    if (e.riseCount > 0)
    {
        w.rise = true;
        w.riseSeconds = e.riseSum / static_cast<double>(e.riseCount);
    }
    if (e.fallCount > 0)
    {
        w.fall = true;
        w.fallSeconds = e.fallSum / static_cast<double>(e.fallCount);
    }
    if (!rises.empty() || !falls.empty())
    {
        const double lastRise = rises.empty() ? -1.0 : rises.back();
        const double lastFall = falls.empty() ? -1.0 : falls.back();
        w.lastEdgeRising = lastRise > lastFall;
        w.settlingMeasured = w.lastEdgeRising ? w.flatTop : w.flatBase;
    }
    if (w.settlingMeasured)
    {
        const double edge = w.lastEdgeRising ? rises.back() : falls.back();
        const double level = w.lastEdgeRising ? w.top : w.base;
        w.settling1 = settlingAfter(x, fs, edge, level, 0.01 * w.amplitude, w.amplitude);
        w.settling01 = settlingAfter(x, fs, edge, level, 0.001 * w.amplitude, w.amplitude);
    }
    return w;
}

std::vector<double> risingCrossings(std::span<const double> x, double fs)
{
    const SpanStatistics stats = spanStatistics(x);
    if (stats.samples < 4 || !(fs > 0.0) || stats.samples < x.size()) return {};
    WaveMeasures w;
    if (!findLevels(x, stats, w)) return {};
    return scanEdges(x, fs, w, 0.1).rises;
}

std::vector<Peak> findPeaks(const AnalysisResult &result, std::size_t curve, std::size_t count,
                            const AnalysisSettings &settings)
{
    std::vector<Peak> peaks;
    if (curve >= result.curves.size() || count == 0 || result.zoneFrames == 0) return peaks;
    const SpectrumCurve &c = result.curves[curve];
    const std::vector<double> &a = c.linear;
    const std::size_t n = a.size();
    if (n < 3) return peaks;
    const double padding = static_cast<double>(result.fftSize) / static_cast<double>(result.zoneFrames);
    const auto lobe = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::ceil(mainLobeHalfWidth(settings) *
                                              (result.kind == SpectrumKind::Density ? 1.0 : padding))));
    // The DC lobe (bin of 0 Hz) is left out when the mean was removed: what is left there is the window.
    const double dcBin = result.binAt(0.0);
    struct Candidate
    {
        std::size_t bin;
        double value;
    };
    std::vector<Candidate> candidates;
    for (std::size_t k = 1; k + 1 < n; ++k)
    {
        const double v = a[k];
        if (!(v > 0.0) || !(v >= a[k - 1]) || !(v > a[k + 1])) continue;
        if (settings.removeMean && std::abs(static_cast<double>(k) - dcBin) <= static_cast<double>(lobe)) continue;
        const std::size_t from = k > lobe ? k - lobe : 0;
        const std::size_t to = std::min(n - 1, k + lobe);
        bool highest = true;
        for (std::size_t j = from; j <= to && highest; ++j) highest = !(a[j] > v);
        if (highest) candidates.push_back({k, v});
    }
    const std::size_t keep = std::min(count, candidates.size());
    std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(keep), candidates.end(),
                      [](const Candidate &x, const Candidate &y) { return x.value > y.value; });
    for (std::size_t i = 0; i < keep; ++i)
    {
        const std::size_t k = candidates[i].bin;
        Peak p;
        p.bin = k;
        p.linear = a[k];
        p.level = k < c.level.size() ? c.level[k] : 0.0;
        double offset = 0.0;
        const double a0 = a[k - 1], a1 = a[k], a2 = a[k + 1];
        if (a0 > 0.0 && a2 > 0.0)
        {
            const double l0 = std::log(a0), l1 = std::log(a1), l2 = std::log(a2);
            const double denominator = l0 - 2.0 * l1 + l2;
            if (denominator < 0.0) offset = std::clamp(0.5 * (l0 - l2) / denominator, -0.5, 0.5);
        }
        p.frequencyHz = result.frequencyAt(static_cast<double>(k) + offset);
        peaks.push_back(p);
    }
    return peaks;
}

double bandMeanSquare(const AnalysisResult &result, std::size_t curve, double f0, double f1)
{
    if (curve >= result.curves.size() || !(result.binWidthHz > 0.0)) return 0.0;
    if (f1 < f0) std::swap(f0, f1);
    const std::vector<double> &a = result.curves[curve].linear;
    const auto n = static_cast<double>(a.size());
    const double first = std::clamp(std::ceil(result.binAt(f0)), 0.0, n);
    const double last = std::clamp(std::floor(result.binAt(f1)), -1.0, n - 1.0);
    double sum = 0.0;
    for (double k = first; k <= last; k += 1.0)
    {
        const double v = a[static_cast<std::size_t>(k)];
        if (!std::isfinite(v)) continue;
        if (result.kind == SpectrumKind::Density) sum += v;
        else if (result.enbwHz > 0.0) sum += v * v / ((result.twoSided ? 1.0 : 2.0) * result.enbwHz);
    }
    return sum * result.binWidthHz;
}
HalfPowerBand halfPowerBand(const AnalysisResult &result, std::size_t curve)
{
    HalfPowerBand band;
    if (curve >= result.curves.size()) return band;
    const SpectrumCurve &c = result.curves[curve];
    const std::vector<double> &a = c.linear;
    const std::size_t n = a.size();
    const std::size_t peak = c.peakBin;
    if (n < 3 || peak >= n || !(a[peak] > 0.0) || !std::isfinite(a[peak])) return band;
    // Density: power; amplitude spectrum: amplitude.
    const double threshold = a[peak] * (result.kind == SpectrumKind::Density ? 0.5 : std::sqrt(0.5));
    // Frequency where the line from bin k to bin j (adjacent) crosses the threshold.
    const auto at = [&](std::size_t k, std::size_t j) {
        const double fraction = a[j] != a[k] ? std::clamp((threshold - a[k]) / (a[j] - a[k]), 0.0, 1.0) : 0.0;
        return result.frequencyAt(static_cast<double>(k) + fraction * (static_cast<double>(j) - static_cast<double>(k)));
    };
    std::size_t k = peak;
    while (k > 0 && a[k - 1] >= threshold) --k;
    if (k == 0)
    {
        band.lowOpen = true;
        band.lowHz = result.frequencyAt(0.0);
    }
    else band.lowHz = at(k, k - 1);
    k = peak;
    while (k + 1 < n && a[k + 1] >= threshold) ++k;
    if (k + 1 == n)
    {
        band.highOpen = true;
        band.highHz = result.frequencyAt(static_cast<double>(n - 1));
    }
    else band.highHz = at(k, k + 1);
    band.peakHz = result.frequencyAt(static_cast<double>(peak));
    band.found = !(band.lowOpen && band.highOpen);
    return band;
}
} // namespace qacq::analysis
