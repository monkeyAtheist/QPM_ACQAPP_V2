#include "qacq/acquisition/rate_estimator.h"
#include "qacq/acquisition/trigger.h"

#include <algorithm>
#include <cmath>

namespace qacq::acquisition
{
// ------------------------------------------------------------------------------------------- rate
void RateEstimator::add(double timeSeconds, std::uint64_t totalFrames)
{
    if (!std::isfinite(timeSeconds)) return;
    if (!m_points.empty() && timeSeconds <= m_points.back().t)
    {
        // Same instant (several reads in one event): keep the latest count.
        if (timeSeconds == m_points.back().t) m_points.back().n = static_cast<double>(totalFrames);
        return;
    }
    m_points.push_back({timeSeconds, static_cast<double>(totalFrames)});
    while (m_points.size() > 2 && m_points.front().t < timeSeconds - m_window) m_points.pop_front();
    while (m_points.size() > 20000) m_points.pop_front();
}

double RateEstimator::spanSeconds() const
{
    return m_points.size() < 2 ? 0.0 : m_points.back().t - m_points.front().t;
}

bool RateEstimator::fit(double &slope, double &slopeError) const
{
    const std::size_t n = m_points.size();
    if (n < 3) return false;
    double tm = 0.0, nm = 0.0;
    for (const Point &p : m_points)
    {
        tm += p.t;
        nm += p.n;
    }
    tm /= static_cast<double>(n);
    nm /= static_cast<double>(n);
    double stt = 0.0, stn = 0.0;
    for (const Point &p : m_points)
    {
        stt += (p.t - tm) * (p.t - tm);
        stn += (p.t - tm) * (p.n - nm);
    }
    if (!(stt > 0.0)) return false;
    slope = stn / stt;
    double residual = 0.0;
    for (const Point &p : m_points)
    {
        const double r = p.n - nm - slope * (p.t - tm);
        residual += r * r;
    }
    slopeError = std::sqrt(residual / static_cast<double>(n - 2) / stt);
    return true;
}

double RateEstimator::rateHz(double minimumSpan) const
{
    double slope = 0.0, error = 0.0;
    if (spanSeconds() < minimumSpan || !fit(slope, error) || !(slope > 0.0)) return 0.0;
    return slope;
}

double RateEstimator::relativeUncertainty() const
{
    double slope = 0.0, error = 0.0;
    if (!fit(slope, error) || !(slope > 0.0)) return 0.0;
    return error / slope;
}

// ------------------------------------------------------------------------------------------- trigger
void Trigger::configure(const TriggerSettings &settings)
{
    m_settings = settings;
    m_settings.length = std::max<std::size_t>(2, m_settings.length);
    m_settings.preTrigger = std::clamp(m_settings.preTrigger, 0.0, 1.0);
    m_settings.hysteresis = std::abs(m_settings.hysteresis);
    m_pending.clear();
    m_armed = true;
}

void Trigger::arm()
{
    m_armed = true;
    m_pending.clear();
    if (m_started) m_lastCaptureEnd = std::max(m_lastCaptureEnd, m_nextAllowed);
}

void Trigger::reset()
{
    m_haveLast = false;
    m_belowArmed = m_aboveArmed = false;
    m_started = false;
    m_pending.clear();
    m_nextAllowed = 0;
}

std::size_t Trigger::preFrames() const noexcept
{
    const auto pre = static_cast<std::size_t>(std::llround(m_settings.preTrigger * static_cast<double>(m_settings.length)));
    return std::min(pre, m_settings.length - 1);
}

std::vector<TriggerCapture> Trigger::process(std::span<const double> samples, std::uint64_t firstFrame)
{
    std::vector<TriggerCapture> done;
    if (samples.empty()) return done;
    if (!m_started)
    {
        m_started = true;
        m_streamStart = firstFrame;
        m_lastCaptureEnd = firstFrame;
        m_nextAllowed = firstFrame;
    }
    const TriggerSettings &s = m_settings;
    const std::size_t pre = preFrames();
    const std::size_t timeout = s.autoTimeout ? s.autoTimeout : s.length;
    const bool wantRising = s.edge != TriggerEdge::Falling;
    const bool wantFalling = s.edge != TriggerEdge::Rising;

    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const std::uint64_t frame = firstFrame + i;
        const double x = samples[i];
        if (std::isnan(x))
        {
            m_haveLast = false;
            m_belowArmed = m_aboveArmed = false;
            continue;
        }
        bool fired = false, rising = true;
        if (m_haveLast)
        {
            if (wantRising && m_belowArmed && x >= s.level)
            {
                fired = true;
                rising = true;
            }
            else if (wantFalling && m_aboveArmed && x <= s.level)
            {
                fired = true;
                rising = false;
            }
        }
        if (x < s.level - s.hysteresis || (s.hysteresis == 0.0 && x < s.level)) m_belowArmed = true;
        if (x > s.level + s.hysteresis || (s.hysteresis == 0.0 && x > s.level)) m_aboveArmed = true;
        if (fired)
        {
            if (rising) m_belowArmed = false;
            else m_aboveArmed = false;
        }

        const bool historyOk = frame >= m_streamStart + pre;
        if (fired && m_armed && m_pending.empty() && frame >= m_nextAllowed && historyOk)
        {
            TriggerCapture c;
            c.triggerFrame = frame;
            c.firstFrame = frame - pre;
            c.length = s.length;
            c.rising = rising;
            const double d = x - m_last;
            c.triggerFraction = d != 0.0 ? std::clamp((s.level - m_last) / d, 0.0, 1.0) : 1.0;
            m_pending.push_back(c);
            m_nextAllowed = frame + (s.length - pre) + s.holdoff;
            if (s.mode == TriggerMode::Single) m_armed = false;
        }
        else if (s.mode == TriggerMode::Auto && m_armed && m_pending.empty() && frame + 1 >= m_lastCaptureEnd + timeout &&
                 frame + 1 >= m_streamStart + s.length)
        {
            TriggerCapture c;
            c.firstFrame = frame + 1 - s.length;
            c.length = s.length;
            c.triggerFrame = c.firstFrame + pre;
            c.forced = true;
            done.push_back(c);
            m_lastCaptureEnd = frame + 1;
            m_nextAllowed = frame + 1;
        }
        // A pending capture is complete once its last frame arrived.
        if (!m_pending.empty() && frame + 1 >= m_pending.front().firstFrame + m_pending.front().length)
        {
            done.push_back(m_pending.front());
            m_lastCaptureEnd = m_pending.front().firstFrame + m_pending.front().length;
            m_pending.erase(m_pending.begin());
        }
        m_last = x;
        m_haveLast = true;
    }
    return done;
}
} // namespace qacq::acquisition
