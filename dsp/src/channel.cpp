#include "qacq/dsp/channel.h"

#include "qacq/dsp/constants.h"
#include "qacq/dsp/rotator.h"
#include "qacq/dsp/interpolation.h"
#include "qacq/dsp/random.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace qacq::dsp
{
namespace
{
bool prepare(const AwgnSpec &spec, double fs, double power, bool complexRecord, AwgnResult &result)
{
    result.signalPower = power;
    if (!(std::isfinite(fs) && fs > 0.0))
    {
        result.error = "La fréquence d'échantillonnage doit être positive.";
        return false;
    }
    if (!std::isfinite(spec.valueDb))
    {
        result.error = "Le rapport signal sur bruit doit être fini.";
        return false;
    }
    if (!(power > 0.0))
    {
        result.error = "Le signal est nul : impossible de régler le bruit par rapport à lui.";
        return false;
    }
    const double ratio = std::pow(10.0, spec.valueDb / 10.0);
    if (spec.level == AwgnSpec::Level::EbN0Db)
    {
        if (!(spec.bitRate > 0.0))
        {
            result.error = "Eb/N0 demande un débit binaire positif.";
            return false;
        }
        result.n0 = power / spec.bitRate / ratio;
        result.noiseVariance = 0.5 * result.n0 * fs; // per real sample, or per component
    }
    else
    {
        const double noisePower = power / ratio;
        result.noiseVariance = complexRecord ? 0.5 * noisePower : noisePower;
        result.n0 = complexRecord ? noisePower / fs : 2.0 * noisePower / fs;
    }
    return true;
}

void measured(AwgnResult &result, double drawnPower)
{
    if (drawnPower > 0.0) result.measuredSnrDb = 10.0 * std::log10(result.signalPower / drawnPower);
}
} // namespace

FadingGains::FadingGains(const ChannelFading &fading, double sampleRateHz, std::size_t paths) : m_paths(paths)
{
    m_active = fading.active() && paths > 0 && std::isfinite(sampleRateHz) && sampleRateHz > 0.0 && std::isfinite(fading.dopplerHz) &&
               fading.dopplerHz >= 0.0;
    if (!m_active) return;
    const double fd = fading.dopplerHz;
    // 64 values a period of fd at least (linear interpolation: within 0.12 % of the gain); fd = 0: still.
    m_step = fd > 0.0 ? std::max(1.0, sampleRateHz / (64.0 * fd)) : 1e12;
    const double perStep = fd * m_step / sampleRateHz; // the largest shift, in cycles per step
    Random random(fading.seed ^ 0xFAD1C6A1EULL);
    const auto n = kFadingSinusoids;
    m_sinusoids.resize(paths * n);
    for (std::size_t p = 0; p < paths; ++p)
    {
        const double start = random.uniform();
        for (std::size_t m = 0; m < n; ++m)
        {
            const double alpha = kTwoPi * (static_cast<double>(m) + start) / static_cast<double>(n);
            m_sinusoids[p * n + m] = {perStep * std::cos(alpha), kTwoPi * random.uniform()};
        }
    }
    if (fading.kind == ChannelFading::Kind::Rice && std::isfinite(fading.riceK) && fading.riceK > 0.0)
    {
        m_scattered = std::sqrt(1.0 / (1.0 + fading.riceK));
        m_los = std::sqrt(fading.riceK / (1.0 + fading.riceK));
        const double cosine = std::cos(kTwoPi * random.uniform());
        m_lineOfSight = {perStep * cosine, kTwoPi * random.uniform()};
        m_lineOfSightHz = fd * cosine;
    }
    restart();
}

std::vector<std::complex<double>> FadingGains::nextValues()
{
    std::vector<std::complex<double>> out(m_paths);
    const auto n = kFadingSinusoids;
    const double scale = 1.0 / std::sqrt(static_cast<double>(n));
    for (std::size_t p = 0; p < m_paths; ++p)
    {
        std::complex<double> sum{};
        for (std::size_t m = 0; m < n; ++m) sum += m_oscillators[p * n + m].next();
        out[p] = scale * sum;
    }
    if (m_los > 0.0) out[0] = m_scattered * out[0] + m_los * m_oscillators.back().next();
    return out;
}

void FadingGains::restart()
{
    // (Each oscillator computes its value from its angle every Rotator::kBlock steps: nothing builds up.)
    m_oscillators.clear();
    for (const Sinusoid &s : m_sinusoids) m_oscillators.emplace_back(s.ratio, s.phase);
    m_oscillators.emplace_back(m_lineOfSight.ratio, m_lineOfSight.phase);
    m_index = 0;
    m_now = nextValues();
    m_next = nextValues();
}

void FadingGains::advance()
{
    ++m_index;
    m_now = std::move(m_next);
    m_next = nextValues();
}

std::complex<double> FadingGains::at(std::size_t path, double sample)
{
    if (!m_active || path >= m_paths) return {1.0, 0.0};
    const double position = std::max(0.0, sample) / m_step;
    if (position < static_cast<double>(m_index)) restart();
    while (position >= static_cast<double>(m_index + 1)) advance();
    const double t = position - static_cast<double>(m_index);
    return m_now[path] + t * (m_next[path] - m_now[path]);
}

double transmittedPosition(const ChannelSpec &spec, double receivedSample) noexcept
{
    return receivedSample / (1.0 + 1e-6 * spec.clockOffsetPpm) - spec.delaySamples;
}

double receivedPosition(const ChannelSpec &spec, double transmittedSample) noexcept
{
    return (transmittedSample + spec.delaySamples) * (1.0 + 1e-6 * spec.clockOffsetPpm);
}

ChannelOutput applyChannel(std::span<const double> inPhase, std::span<const double> quadrature, const ChannelSpec &spec)
{
    ChannelOutput out;
    const std::size_t n = std::min(inPhase.size(), quadrature.size());
    const double fs = spec.sampleRateHz;
    const double clock = 1e-6 * spec.clockOffsetPpm;
    if (!(std::isfinite(fs) && fs > 0.0))
    {
        out.error = "La fréquence d'échantillonnage doit être positive.";
        return out;
    }
    if (!std::isfinite(spec.frequencyOffsetHz) || !std::isfinite(spec.phaseOffsetDegrees) || !std::isfinite(spec.delaySamples) ||
        !std::isfinite(clock))
    {
        out.error = "Le canal demande des nombres finis.";
        return out;
    }
    if (spec.delaySamples < 0.0)
    {
        out.error = "Le retard ne peut pas être négatif.";
        return out;
    }
    if (!(std::abs(clock) < 0.1))
    {
        out.error = "L'écart d'horloge doit rester sous 100 000 ppm.";
        return out;
    }
    if (n == 0)
    {
        out.error = "Signal vide.";
        return out;
    }
    if (spec.paths.size() > kMaximumChannelPaths)
    {
        out.error = "Trop de trajets (" + std::to_string(kMaximumChannelPaths) + " au plus).";
        return out;
    }
    const bool fading = spec.fading.active();
    if (fading && (!std::isfinite(spec.fading.dopplerHz) || spec.fading.dopplerHz < 0.0 || spec.fading.dopplerHz > 0.5 * fs ||
                   !std::isfinite(spec.fading.riceK) || spec.fading.riceK < 0.0))
    {
        out.error = "L'évanouissement demande un décalage Doppler entre 0 et fs/2 et un facteur K positif ou nul.";
        return out;
    }
    // The paths: where each one reads the transmission, its complex gain, and which fading gain it takes.
    struct Path
    {
        double delay;
        std::complex<double> gain;
        std::size_t index;
    };
    std::vector<Path> paths;
    double longest = 0.0;
    for (std::size_t k = 0; k < spec.paths.size(); ++k)
    {
        const ChannelPath &p = spec.paths[k];
        if (!std::isfinite(p.delaySamples) || !std::isfinite(p.gain) || !std::isfinite(p.phaseDegrees) || p.delaySamples < 0.0)
        {
            out.error = "Chaque trajet demande un retard positif ou nul, un gain et une phase finis.";
            return out;
        }
        if (p.gain == 0.0) continue;
        paths.push_back({p.delaySamples, std::polar(p.gain, p.phaseDegrees * kPi / 180.0), k});
        longest = std::max(longest, p.delaySamples);
    }
    // Fading without paths listed: the one path fades.
    if (fading && spec.paths.empty()) paths.push_back({0.0, {1.0, 0.0}, 0});
    const bool single = spec.singlePath() && !fading;
    FadingGains gains(spec.fading, fs, std::max<std::size_t>(1, spec.paths.size()));
    // Every sample of the transmission (of its last path), then extraSamples.
    const double last = receivedPosition(spec, static_cast<double>(n - 1) + longest);
    const auto count = static_cast<std::size_t>(std::ceil(last)) + 1 + spec.extraSamples;
    out.inPhase.resize(count);
    out.quadrature.resize(count);
    const SincInterpolator &sinc = sincInterpolator();
    const bool moved = spec.delaySamples != 0.0 || clock != 0.0;
    const double ratio = spec.frequencyOffsetHz / fs;
    const double phi = spec.phaseOffsetDegrees * kPi / 180.0;
    const bool offCarrier = ratio != 0.0 || phi != 0.0;
    Rotator carrier(ratio, phi);
    for (std::size_t i = 0; i < count; ++i)
    {
        std::complex<double> v;
        if (!single)
        {
            const double at = transmittedPosition(spec, static_cast<double>(i));
            for (const Path &p : paths)
            {
                const double position = at - p.delay;
                const std::complex<double> g = fading ? p.gain * gains.at(p.index, static_cast<double>(i)) : p.gain;
                // (A path that falls on a sample of an unmoved transmission: that sample itself.)
                if (!moved && position == std::floor(position))
                {
                    if (position >= 0.0 && position < static_cast<double>(n))
                    {
                        const auto k = static_cast<std::size_t>(position);
                        v += g * std::complex<double>(inPhase[k], quadrature[k]);
                    }
                }
                else
                    v += g * sinc.at(inPhase.first(n), quadrature.first(n), position);
            }
        }
        else if (moved)
            v = sinc.at(inPhase.first(n), quadrature.first(n), transmittedPosition(spec, static_cast<double>(i)));
        else if (i < n)
            v = {inPhase[i], quadrature[i]};
        if (offCarrier) v = turned(v, carrier.next());
        out.inPhase[i] = v.real();
        out.quadrature[i] = v.imag();
    }
    return out;
}

std::vector<double> toPassband(std::span<const double> inPhase, std::span<const double> quadrature, double fs, double carrierHz)
{
    const std::size_t n = std::min(inPhase.size(), quadrature.size());
    std::vector<double> out(n);
    Rotator carrier(carrierHz / fs);
    for (std::size_t i = 0; i < n; ++i)
    {
        const std::complex<double> c = carrier.next();
        out[i] = inPhase[i] * c.real() - quadrature[i] * c.imag();
    }
    return out;
}

AwgnResult addAwgn(std::vector<double> &signal, double fs, const AwgnSpec &spec)
{
    AwgnResult result;
    double power = 0.0;
    for (double v : signal) power += v * v;
    power /= static_cast<double>(std::max<std::size_t>(1, signal.size()));
    if (spec.referencePower > 0.0) power = spec.referencePower;
    if (!prepare(spec, fs, power, false, result)) return result;
    Random random(spec.seed);
    const double sigma = std::sqrt(result.noiseVariance);
    double drawn = 0.0;
    for (double &v : signal)
    {
        const double w = sigma * random.normal();
        v += w;
        drawn += w * w;
    }
    drawn /= static_cast<double>(signal.size());
    measured(result, drawn);
    if (spec.bitRate > 0.0)
        result.measuredEbN0Db = 10.0 * std::log10((power / spec.bitRate) / (2.0 * drawn / fs));
    return result;
}

AwgnResult addAwgn(std::vector<double> &inPhase, std::vector<double> &quadrature, double fs, const AwgnSpec &spec)
{
    AwgnResult result;
    const std::size_t n = std::min(inPhase.size(), quadrature.size());
    double power = 0.0;
    for (std::size_t i = 0; i < n; ++i) power += inPhase[i] * inPhase[i] + quadrature[i] * quadrature[i];
    power /= static_cast<double>(std::max<std::size_t>(1, n));
    if (spec.referencePower > 0.0) power = spec.referencePower;
    if (!prepare(spec, fs, power, true, result)) return result;
    Random random(spec.seed);
    const double sigma = std::sqrt(result.noiseVariance);
    double drawn = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const double wi = sigma * random.normal();
        const double wq = sigma * random.normal();
        inPhase[i] += wi;
        quadrature[i] += wq;
        drawn += wi * wi + wq * wq;
    }
    drawn /= static_cast<double>(n);
    measured(result, drawn);
    if (spec.bitRate > 0.0) result.measuredEbN0Db = 10.0 * std::log10((power / spec.bitRate) / (drawn / fs));
    return result;
}
} // namespace qacq::dsp
