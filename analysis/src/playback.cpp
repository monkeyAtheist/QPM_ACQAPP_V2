#include "qacq/analysis/playback.h"

#include "qacq/dsp/constants.h"
#include "qacq/plot/axis.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace qacq::analysis
{
namespace
{
double besselI0(double x) noexcept
{
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 200; ++k)
    {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < 1e-17 * sum) break;
    }
    return sum;
}

// sinc(s) times a Kaiser window, for s from 0 to kZeros, kSteps values between two zero crossings.
class SincTable
{
public:
    static constexpr int kZeros = 24;
    static constexpr int kSteps = 512;

    SincTable()
    {
        constexpr double beta = 8.0;
        const double norm = besselI0(beta);
        m_values.resize(static_cast<std::size_t>(kZeros * kSteps) + 2, 0.0);
        for (std::size_t i = 0; i < m_values.size(); ++i)
        {
            const double s = static_cast<double>(i) / kSteps;
            if (s >= kZeros) continue;
            const double sinc = s == 0.0 ? 1.0 : std::sin(dsp::kPi * s) / (dsp::kPi * s);
            const double r = s / kZeros;
            m_values[i] = sinc * besselI0(beta * std::sqrt(1.0 - r * r)) / norm;
        }
    }

    double at(double s) const noexcept
    {
        s = std::abs(s);
        if (s >= kZeros) return 0.0;
        const double p = s * kSteps;
        const auto i = static_cast<std::size_t>(p);
        const double f = p - static_cast<double>(i);
        return m_values[i] + f * (m_values[i + 1] - m_values[i]);
    }

private:
    std::vector<double> m_values;
};

const SincTable &sincTable()
{
    static const SincTable table;
    return table;
}

std::string decibels(double db)
{
    return plot::formatCompact(db, "dB", 3);
}
} // namespace

std::vector<double> resample(std::span<const double> x, double inputRateHz, double outputRateHz, bool periodic,
                             const model::Progress &progress)
{
    const std::size_t n = x.size();
    if (n == 0 || !(inputRateHz > 0.0) || !(outputRateHz > 0.0) || !std::isfinite(inputRateHz) || !std::isfinite(outputRateHz))
        return {};
    if (inputRateHz == outputRateHz) return {x.begin(), x.end()};
    const double ratio = outputRateHz / inputRateHz; // output samples per input sample
    const auto count = static_cast<std::size_t>(std::max(1.0, std::round(static_cast<double>(n) * ratio)));
    // The kernel in input samples: zero crossings every 1/scale, cut at 0.45 of the lower rate.
    const double scale = 0.9 * std::min(1.0, ratio);
    const double half = SincTable::kZeros / scale;
    const double step = 1.0 / ratio;
    const SincTable &table = sincTable();
    const auto length = static_cast<long long>(n);
    std::vector<double> y(count);
    for (std::size_t j = 0; j < count; ++j)
    {
        if ((j & 4095U) == 0U && progress.stopped()) return {};
        const double t = static_cast<double>(j) * step;
        const auto first = static_cast<long long>(std::ceil(t - half));
        const auto last = static_cast<long long>(std::floor(t + half));
        double sum = 0.0;
        for (long long k = first; k <= last; ++k)
        {
            double v = 0.0;
            if (k >= 0 && k < length)
                v = x[static_cast<std::size_t>(k)];
            else if (periodic)
                v = x[static_cast<std::size_t>(((k % length) + length) % length)];
            else
                continue;
            sum += v * table.at(scale * (t - static_cast<double>(k)));
        }
        y[j] = scale * sum;
    }
    return y;
}

PlaybackBuffer preparePlayback(const model::Signal &signal, const PlaybackRequest &request, const model::Progress &progress)
{
    PlaybackBuffer out;
    const std::size_t total = signal.frameCount();
    if (!signal.data || total == 0)
    {
        out.error = "Le signal est vide : rien à écouter.";
        return out;
    }
    if (request.channels.empty() || request.channels.size() > 2)
    {
        out.error = "Une voie (mono) ou deux (stéréo) s'écoutent.";
        return out;
    }
    for (std::size_t c : request.channels)
    {
        if (c >= signal.channelCount())
        {
            out.error = "Le signal n'a pas de voie " + std::to_string(c + 1) + ".";
            return out;
        }
    }
    if (!(request.outputRateHz > 0.0) || request.outputChannels == 0)
    {
        out.error = "La sortie audio n'a pas de format utilisable.";
        return out;
    }
    if (!(signal.sampleRateHz > 0.0) || !std::isfinite(signal.sampleRateHz))
    {
        out.error = "Le signal n'a pas de fréquence d'échantillonnage.";
        return out;
    }

    // The start only beyond the longest.
    std::size_t n = total;
    const double longest = std::floor(request.maximumSeconds * signal.sampleRateHz);
    if (static_cast<double>(n) > longest && longest >= 1.0)
    {
        n = static_cast<std::size_t>(longest);
        out.warnings.push_back("Seules les " + plot::formatCompact(request.maximumSeconds / 60.0, "", 3) +
                               " premières minutes s'écoutent.");
    }

    std::vector<std::vector<double>> heard;
    double before = 0.0;
    for (std::size_t c : request.channels)
    {
        if (progress.stopped())
        {
            out.cancelled = true;
            return out;
        }
        const std::span<const double> x = signal.channel(c).first(n);
        for (double v : x)
            if (std::isfinite(v)) before = std::max(before, std::abs(v));
        heard.push_back(resample(x, signal.sampleRateHz, request.outputRateHz, request.periodic, progress));
        if (progress.stopped())
        {
            out.cancelled = true;
            return out;
        }
    }
    double peak = 0.0;
    for (const std::vector<double> &h : heard)
        for (double v : h)
            if (std::isfinite(v)) peak = std::max(peak, std::abs(v));
    // What goes beyond what the card carries is cut: say so when it was most of the signal.
    if (request.outputRateHz < signal.sampleRateHz && before > 0.0 && peak < 0.1 * before)
        out.warnings.push_back("L'essentiel du signal est au-delà de " + plot::formatCompact(0.45 * request.outputRateHz, "Hz", 3) +
                               " : la carte ne le rend pas (on n'entend que ce qui est en dessous).");

    double gain = 1.0;
    if (request.normalized && peak > 0.0) gain = std::pow(10.0, -1.0 / 20.0) / peak;
    out.gainDb = 20.0 * std::log10(gain);
    if (!request.normalized && peak > 0.0 && peak < 0.01)
        out.warnings.push_back("Crête à " + decibels(20.0 * std::log10(peak)) + "FS tel quel : à peine audible (« Normalisé » la porte à −1 dBFS).");

    const std::size_t frames = heard.front().size();
    const std::size_t channels = request.outputChannels;
    out.frames = frames;
    out.seconds = static_cast<double>(frames) / request.outputRateHz;
    out.samples.resize(frames * channels);
    const bool stereo = heard.size() == 2;
    for (std::size_t f = 0; f < frames; ++f)
    {
        const double left = heard[0][f] * gain;
        const double right = stereo ? heard[1][f] * gain : left;
        for (std::size_t o = 0; o < channels; ++o)
        {
            double v = !stereo ? left : channels == 1 ? 0.5 * (left + right) : o == 0 ? left : o == 1 ? right : 0.0;
            if (!std::isfinite(v)) v = 0.0;
            if (v > 1.0 || v < -1.0)
            {
                v = v > 1.0 ? 1.0 : -1.0;
                ++out.clipped;
            }
            out.samples[f * channels + o] = static_cast<float>(v);
        }
    }
    if (out.clipped > 0)
        out.warnings.push_back(std::to_string(out.clipped) +
                               " échantillons au-delà de la pleine échelle (±1) sont écrêtés : « Normalisé » l'évite.");
    return out;
}

std::size_t bytesPerSample(SampleFormat format) noexcept
{
    switch (format)
    {
        case SampleFormat::UInt8: return 1;
        case SampleFormat::Int16: return 2;
        case SampleFormat::Int32:
        case SampleFormat::Float32: return 4;
    }
    return 4;
}

std::vector<std::uint8_t> encodeSamples(std::span<const float> samples, SampleFormat format)
{
    const std::size_t size = bytesPerSample(format);
    std::vector<std::uint8_t> out(samples.size() * size);
    std::uint8_t *p = out.data();
    for (float sample : samples)
    {
        const double v = std::isfinite(sample) ? std::clamp(static_cast<double>(sample), -1.0, 1.0) : 0.0;
        switch (format)
        {
            case SampleFormat::UInt8: *p = static_cast<std::uint8_t>(std::lround(v * 127.0) + 128); break;
            case SampleFormat::Int16:
            {
                const auto q = static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(v * 32767.0)));
                p[0] = static_cast<std::uint8_t>(q & 0xFF);
                p[1] = static_cast<std::uint8_t>(q >> 8);
                break;
            }
            case SampleFormat::Int32:
            {
                const auto q = static_cast<std::uint32_t>(static_cast<std::int32_t>(std::llround(v * 2147483647.0)));
                for (int b = 0; b < 4; ++b) p[b] = static_cast<std::uint8_t>((q >> (8 * b)) & 0xFF);
                break;
            }
            case SampleFormat::Float32:
            {
                const auto q = std::bit_cast<std::uint32_t>(static_cast<float>(v));
                for (int b = 0; b < 4; ++b) p[b] = static_cast<std::uint8_t>((q >> (8 * b)) & 0xFF);
                break;
            }
        }
        p += size;
    }
    return out;
}
} // namespace qacq::analysis
