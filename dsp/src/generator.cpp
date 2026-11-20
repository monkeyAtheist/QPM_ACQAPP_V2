#include "qacq/dsp/generator.h"

#include "qacq/dsp/constants.h"
#include "qacq/dsp/random.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace qacq::dsp
{
std::string_view waveformName(Waveform waveform) noexcept
{
    switch (waveform)
    {
        case Waveform::Sine: return "Sinus";
        case Waveform::Square: return "Carré";
        case Waveform::Triangle: return "Triangle";
        case Waveform::Sawtooth: return "Dent de scie";
        case Waveform::Pulse: return "Impulsions";
        case Waveform::Dc: return "Continu";
        case Waveform::Step: return "Échelon";
        case Waveform::Impulse: return "Impulsion unité";
        case Waveform::WhiteNoise: return "Bruit blanc";
        case Waveform::Chirp: return "Vobulation";
    }
    return "?";
}

bool isPeriodic(Waveform waveform) noexcept
{
    switch (waveform)
    {
        case Waveform::Sine:
        case Waveform::Square:
        case Waveform::Triangle:
        case Waveform::Sawtooth:
        case Waveform::Pulse: return true;
        default: return false;
    }
}

FourierSeries fourierSeries(Waveform waveform, double duty, std::size_t harmonics)
{
    FourierSeries series;
    series.cosine.assign(harmonics, 0.0);
    series.sine.assign(harmonics, 0.0);
    for (std::size_t i = 0; i < harmonics; ++i)
    {
        const double k = static_cast<double>(i + 1);
        const bool odd = (i % 2) == 0;
        switch (waveform)
        {
            case Waveform::Sine:
                if (i == 0) series.sine[0] = 1.0;
                break;
            case Waveform::Square:
                series.cosine[i] = 2.0 * std::sin(kTwoPi * k * duty) / (kPi * k);
                series.sine[i] = 2.0 * (1.0 - std::cos(kTwoPi * k * duty)) / (kPi * k);
                break;
            case Waveform::Pulse:
                series.cosine[i] = std::sin(kTwoPi * k * duty) / (kPi * k);
                series.sine[i] = (1.0 - std::cos(kTwoPi * k * duty)) / (kPi * k);
                break;
            case Waveform::Triangle:
                if (odd) series.sine[i] = 8.0 / (kPi * kPi * k * k) * ((i / 2) % 2 == 0 ? 1.0 : -1.0);
                break;
            case Waveform::Sawtooth: series.sine[i] = 2.0 / (kPi * k) * (odd ? 1.0 : -1.0); break;
            default: break;
        }
    }
    if (waveform == Waveform::Square) series.dc = 2.0 * duty - 1.0;
    if (waveform == Waveform::Pulse) series.dc = duty;
    return series;
}

namespace
{
double fraction(double x) noexcept
{
    return x - std::floor(x);
}

double idealValue(Waveform waveform, double u, double duty) noexcept
{
    switch (waveform)
    {
        case Waveform::Sine: return std::sin(kTwoPi * u);
        case Waveform::Square: return u < duty ? 1.0 : -1.0;
        case Waveform::Pulse: return u < duty ? 1.0 : 0.0;
        case Waveform::Triangle: return 1.0 - 4.0 * std::abs(fraction(u + 0.25) - 0.5);
        case Waveform::Sawtooth: return 2.0 * fraction(u + 0.5) - 1.0;
        default: return 0.0;
    }
}
} // namespace

GeneratedSignal generate(const WaveformSpec &spec)
{
    GeneratedSignal out;
    const double fs = spec.sampleRateHz;
    const std::size_t n = spec.sampleCount;
    if (!(std::isfinite(fs) && fs > 0.0))
    {
        out.error = "La fréquence d'échantillonnage doit être positive.";
        return out;
    }
    if (n == 0 || n > kMaximumGeneratedSamples)
    {
        out.error = "Le nombre d'échantillons doit être compris entre 1 et 100 millions.";
        return out;
    }
    const bool periodic = isPeriodic(spec.waveform);
    if (periodic && !(std::isfinite(spec.frequencyHz) && spec.frequencyHz > 0.0))
    {
        out.error = "La fréquence doit être positive.";
        return out;
    }
    if ((spec.waveform == Waveform::Square || spec.waveform == Waveform::Pulse) &&
        !(spec.dutyCycle > 0.0 && spec.dutyCycle < 1.0))
    {
        out.error = "Le rapport cyclique doit être strictement compris entre 0 et 1.";
        return out;
    }
    if (spec.waveform == Waveform::Chirp && spec.chirpLaw == ChirpLaw::Logarithmic &&
        !(spec.frequencyHz > 0.0 && spec.chirpEndFrequencyHz > 0.0))
    {
        out.error = "Une vobulation logarithmique demande des fréquences positives.";
        return out;
    }
    if (periodic && spec.frequencyHz >= 0.5 * fs)
        out.warnings.emplace_back("La fréquence dépasse fs/2 : le signal échantillonné est replié.");
    if (spec.waveform == Waveform::Chirp && std::max(spec.frequencyHz, spec.chirpEndFrequencyHz) >= 0.5 * fs)
        out.warnings.emplace_back("La vobulation dépasse fs/2 : la fin du balayage est repliée.");

    out.samples.assign(n, spec.offset);
    auto &x = out.samples;
    const double a = spec.amplitude;
    const double ratio = spec.frequencyHz / fs;
    const double phase0 = spec.phaseDegrees / 360.0;
    auto phaseAt = [&](std::size_t i) { return fraction(ratio * static_cast<double>(i) + phase0); };

    if (periodic && spec.bandLimit == BandLimit::Fourier && spec.waveform != Waveform::Sine)
    {
        std::size_t k = static_cast<std::size_t>(std::floor((0.5 - 1e-12) / ratio));
        if (spec.maximumHarmonics > 0) k = std::min(k, spec.maximumHarmonics);
        out.harmonicsUsed = k;
        const FourierSeries series = fourierSeries(spec.waveform, spec.dutyCycle, k);
        std::vector<std::complex<double>> coefficient(k);
        for (std::size_t h = 0; h < k; ++h) coefficient[h] = {series.cosine[h], -series.sine[h]};
        const std::complex<double> step = std::polar(1.0, kTwoPi * ratio);
        std::complex<double> z;
        for (std::size_t i = 0; i < n; ++i)
        {
            if (i % 256 == 0)
                z = std::polar(1.0, kTwoPi * phaseAt(i));
            else
                z *= step;
            std::complex<double> zk = z;
            double v = series.dc;
            for (std::size_t h = 0; h < k; ++h)
            {
                v += (coefficient[h] * zk).real();
                zk *= z;
            }
            x[i] += a * v;
        }
        return out;
    }

    switch (spec.waveform)
    {
        case Waveform::Sine:
        case Waveform::Square:
        case Waveform::Triangle:
        case Waveform::Sawtooth:
        case Waveform::Pulse:
            for (std::size_t i = 0; i < n; ++i) x[i] += a * idealValue(spec.waveform, phaseAt(i), spec.dutyCycle);
            if (spec.waveform == Waveform::Sine) out.harmonicsUsed = 1;
            break;
        case Waveform::Dc:
            for (double &v : x) v += a;
            break;
        case Waveform::Step:
            for (std::size_t i = 0; i < n; ++i)
                if (static_cast<double>(i) / fs >= spec.eventTimeSeconds) x[i] += a;
            break;
        case Waveform::Impulse:
        {
            const double index = std::round(spec.eventTimeSeconds * fs);
            if (index >= 0.0 && index < static_cast<double>(n)) x[static_cast<std::size_t>(index)] += a;
            else out.warnings.emplace_back("L'instant de l'impulsion est hors de l'enregistrement.");
            break;
        }
        case Waveform::WhiteNoise:
        {
            Random random(spec.seed);
            for (double &v : x)
                v += spec.noise == NoiseDistribution::Gaussian ? a * random.normal() : a * (2.0 * random.uniform() - 1.0);
            break;
        }
        case Waveform::Chirp:
        {
            const double duration = static_cast<double>(n) / fs;
            const double f0 = spec.frequencyHz, f1 = spec.chirpEndFrequencyHz;
            const double start = kTwoPi * phase0;
            for (std::size_t i = 0; i < n; ++i)
            {
                const double t = static_cast<double>(i) / fs;
                double cycles = 0.0;
                if (spec.chirpLaw == ChirpLaw::Linear)
                {
                    cycles = f0 * t + 0.5 * (f1 - f0) * t * t / duration;
                }
                else
                {
                    const double rate = std::log(f1 / f0) / duration;
                    cycles = std::abs(rate) < 1e-15 ? f0 * t : f0 * std::expm1(rate * t) / rate;
                }
                x[i] += a * std::sin(kTwoPi * fraction(cycles) + start);
            }
            break;
        }
    }
    return out;
}

std::vector<double> synthesize(std::span<const SineComponent> components, double offset, double fs, std::size_t n)
{
    std::vector<double> x(n, offset);
    if (!(fs > 0.0)) return x;
    for (const auto &c : components)
    {
        if (!c.enabled) continue;
        const double ratio = c.frequencyHz / fs;
        const double phase0 = c.phaseDegrees / 360.0;
        for (std::size_t i = 0; i < n; ++i)
            x[i] += c.amplitude * std::sin(kTwoPi * fraction(ratio * static_cast<double>(i) + phase0));
    }
    return x;
}
} // namespace qacq::dsp
