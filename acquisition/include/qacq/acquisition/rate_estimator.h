#pragma once

// Sample rate measured from the arrival of the frames. A serial port or a socket delivers frames in
// bursts (driver buffers, USB polling every millisecond, Nagle...): the rate is the least-squares slope
// of the frame count against the arrival time over a sliding window, which averages the bursts out.

#include <cstddef>
#include <cstdint>
#include <deque>

namespace qacq::acquisition
{
class RateEstimator
{
public:
    explicit RateEstimator(double windowSeconds = 5.0) : m_window(windowSeconds) {}

    void reset() { m_points.clear(); }
    // timeSeconds: monotonic clock of the arrival; totalFrames: frames received since the start.
    void add(double timeSeconds, std::uint64_t totalFrames);

    // 0 until the window spans at least minimumSpan seconds with three points or more.
    double rateHz(double minimumSpan = 0.5) const;
    // Relative standard error of the slope (0.001 = 0.1 %), 0 when unknown.
    double relativeUncertainty() const;
    double spanSeconds() const;

private:
    struct Point
    {
        double t;
        double n;
    };
    bool fit(double &slope, double &slopeError) const;

    double m_window;
    std::deque<Point> m_points;
};
} // namespace qacq::acquisition
