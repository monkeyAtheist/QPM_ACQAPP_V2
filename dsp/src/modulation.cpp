#include "qacq/dsp/modulation.h"

#include "qacq/dsp/constants.h"
#include "qacq/dsp/rotator.h"
#include "qacq/dsp/filtering.h"
#include "qacq/dsp/random.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <utility>

namespace qacq::dsp
{
std::string_view modulationName(Modulation modulation) noexcept
{
    switch (modulation)
    {
        case Modulation::Am: return "AM";
        case Modulation::Fm: return "FM";
        case Modulation::Pm: return "PM";
        case Modulation::Ask: return "ASK";
        case Modulation::Fsk: return "FSK";
        case Modulation::Bpsk: return "BPSK";
        case Modulation::Qpsk: return "QPSK";
        case Modulation::Psk8: return "8-PSK";
        case Modulation::Qam16: return "16-QAM";
        case Modulation::Qam64: return "64-QAM";
        case Modulation::Qam256: return "256-QAM";
    }
    return "?";
}

bool isAnalog(Modulation modulation) noexcept
{
    return modulation == Modulation::Am || modulation == Modulation::Fm || modulation == Modulation::Pm;
}

int bitsPerSymbol(Modulation modulation) noexcept
{
    switch (modulation)
    {
        case Modulation::Ask:
        case Modulation::Fsk:
        case Modulation::Bpsk: return 1;
        case Modulation::Qpsk: return 2;
        case Modulation::Psk8: return 3;
        case Modulation::Qam16: return 4;
        case Modulation::Qam64: return 6;
        case Modulation::Qam256: return 8;
        default: return 0;
    }
}

bool hasConstellation(Modulation modulation) noexcept
{
    return !isAnalog(modulation) && modulation != Modulation::Fsk;
}

double rotationalSymmetryDegrees(Modulation modulation) noexcept
{
    switch (modulation)
    {
        case Modulation::Bpsk: return 180.0;
        case Modulation::Psk8: return 45.0;
        case Modulation::Qpsk:
        case Modulation::Qam16:
        case Modulation::Qam64:
        case Modulation::Qam256: return 90.0;
        default: return 0.0;
    }
}

std::size_t grayCode(std::size_t value) noexcept
{
    return value ^ (value >> 1);
}

std::size_t grayDecode(std::size_t code) noexcept
{
    std::size_t value = code;
    for (std::size_t shift = code >> 1; shift != 0; shift >>= 1) value ^= shift;
    return value;
}

namespace
{
// Levels per axis of a square QAM (0 for the others).
std::size_t qamLevels(Modulation modulation) noexcept
{
    switch (modulation)
    {
        case Modulation::Qam16: return 4;
        case Modulation::Qam64: return 8;
        case Modulation::Qam256: return 16;
        default: return 0;
    }
}

// The level q (0 to L - 1) of a QAM axis, at the scale of the constellation: (2q - L + 1) / sqrt(2 (M - 1) / 3).
double qamLevel(std::size_t q, std::size_t levels) noexcept
{
    const double m = static_cast<double>(levels * levels);
    return (2.0 * static_cast<double>(q) - static_cast<double>(levels) + 1.0) / std::sqrt(2.0 * (m - 1.0) / 3.0);
}

// The level nearest to x on a QAM axis.
std::size_t qamSlice(double x, std::size_t levels) noexcept
{
    const double m = static_cast<double>(levels * levels);
    const double u = 0.5 * (x * std::sqrt(2.0 * (m - 1.0) / 3.0) + static_cast<double>(levels) - 1.0);
    if (!(u > 0.0)) return 0; // also NaN
    const double q = std::floor(u + 0.5);
    return q >= static_cast<double>(levels - 1) ? levels - 1 : static_cast<std::size_t>(q);
}
} // namespace

std::vector<std::complex<double>> constellation(Modulation modulation, double askLowLevel)
{
    constexpr double h = 0.70710678118654752440;
    switch (modulation)
    {
        case Modulation::Ask: return {{askLowLevel, 0.0}, {1.0, 0.0}};
        case Modulation::Bpsk: return {{1.0, 0.0}, {-1.0, 0.0}};
        // b0 on Q, b1 on I (the convention of the old application): 00 (+,+), 01 (-,+), 10 (+,-), 11 (-,-).
        case Modulation::Qpsk: return {{h, h}, {-h, h}, {h, -h}, {-h, -h}};
        case Modulation::Psk8:
        {
            std::vector<std::complex<double>> points(8);
            for (std::size_t p = 0; p < 8; ++p) points[grayCode(p)] = std::polar(1.0, kPi / 4.0 * static_cast<double>(p));
            return points;
        }
        case Modulation::Qam16:
        case Modulation::Qam64:
        case Modulation::Qam256:
        {
            const std::size_t levels = qamLevels(modulation);
            const int half = bitsPerSymbol(modulation) / 2;
            std::vector<std::complex<double>> points(levels * levels);
            for (std::size_t v = 0; v < points.size(); ++v)
            {
                const std::size_t i = grayDecode(v >> half), q = grayDecode(v & (levels - 1));
                points[v] = {qamLevel(i, levels), qamLevel(q, levels)};
            }
            return points;
        }
        default: return {};
    }
}

std::size_t decideSymbol(Modulation modulation, std::complex<double> point, double askLowLevel) noexcept
{
    switch (modulation)
    {
        case Modulation::Ask: return point.real() > 0.5 * (1.0 + askLowLevel) ? 1 : 0;
        case Modulation::Bpsk: return point.real() < 0.0 ? 1 : 0;
        case Modulation::Qpsk: return (point.imag() < 0.0 ? 2u : 0u) | (point.real() < 0.0 ? 1u : 0u);
        case Modulation::Psk8:
        {
            // The sector of its angle: p = round(angle / (pi / 4)) modulo 8.
            const double turns = std::atan2(point.imag(), point.real()) / (kPi / 4.0);
            const auto p = static_cast<long>(std::floor(turns + 0.5));
            return grayCode(static_cast<std::size_t>(((p % 8) + 8) % 8));
        }
        case Modulation::Qam16:
        case Modulation::Qam64:
        case Modulation::Qam256:
        {
            const std::size_t levels = qamLevels(modulation);
            const int half = bitsPerSymbol(modulation) / 2;
            return (grayCode(qamSlice(point.real(), levels)) << half) | grayCode(qamSlice(point.imag(), levels));
        }
        default: return 0;
    }
}

std::vector<std::uint8_t> bitsFromText(std::string_view text)
{
    std::vector<std::uint8_t> bits;
    for (char c : text)
        if (c == '0' || c == '1') bits.push_back(c == '1' ? 1 : 0);
    return bits;
}

std::vector<std::uint8_t> bitsFromBytesMsbFirst(std::span<const std::uint8_t> bytes)
{
    std::vector<std::uint8_t> bits;
    bits.reserve(bytes.size() * 8);
    for (std::uint8_t byte : bytes)
        for (int b = 7; b >= 0; --b) bits.push_back(static_cast<std::uint8_t>((byte >> b) & 1u));
    return bits;
}

std::vector<std::uint8_t> uart8n1(std::span<const std::uint8_t> bytes)
{
    std::vector<std::uint8_t> bits;
    bits.reserve(bytes.size() * 10);
    for (std::uint8_t byte : bytes)
    {
        bits.push_back(0); // start
        for (int b = 0; b < 8; ++b) bits.push_back(static_cast<std::uint8_t>((byte >> b) & 1u));
        bits.push_back(1); // stop
    }
    return bits;
}

std::vector<std::uint8_t> randomBits(std::size_t count, std::uint64_t seed)
{
    Random random(seed);
    std::vector<std::uint8_t> bits(count);
    for (auto &b : bits) b = static_cast<std::uint8_t>(random.next() >> 63);
    return bits;
}

namespace
{
// The root raised cosine t symbols from its peak, before it is scaled.
double rootRaisedValue(double beta, double t) noexcept
{
    if (std::abs(t) < 1e-12) return 1.0 - beta + 4.0 * beta / kPi;
    if (beta > 0.0 && std::abs(std::abs(t) - 1.0 / (4.0 * beta)) < 1e-9)
        return beta / std::sqrt(2.0) * ((1.0 + 2.0 / kPi) * std::sin(kPi / (4.0 * beta)) + (1.0 - 2.0 / kPi) * std::cos(kPi / (4.0 * beta)));
    return (std::sin(kPi * t * (1.0 - beta)) + 4.0 * beta * t * std::cos(kPi * t * (1.0 + beta))) /
           (kPi * t * (1.0 - (4.0 * beta * t) * (4.0 * beta * t)));
}

// A pulse h of sps samples per symbol through itself: the pair at the instants of the other symbols (k sps from
// the peak) against the peak, in power, dB.
double pairInterferenceDb(const std::vector<double> &h, std::size_t sps) noexcept
{
    double own = 0.0, others = 0.0;
    for (double v : h) own += v * v;
    for (std::size_t k = 1; sps > 0 && k * sps < h.size(); ++k)
    {
        double g = 0.0;
        for (std::size_t n = 0; n + k * sps < h.size(); ++n) g += h[n] * h[n + k * sps];
        others += 2.0 * g * g;
    }
    return own > 0.0 && others > 0.0 ? 10.0 * std::log10(others / (own * own)) : -std::numeric_limits<double>::infinity();
}
} // namespace

std::vector<double> rootRaisedCosine(double beta, std::size_t sps, std::size_t span)
{
    if (sps == 0) return {1.0};
    std::size_t length = std::max<std::size_t>(1, span) * sps;
    length += length % 2; // odd number of taps: the peak sits on a sample and the delay is an integer
    const std::size_t taps = length + 1;
    const double centre = 0.5 * static_cast<double>(length);
    std::vector<double> h(taps);
    for (std::size_t i = 0; i < taps; ++i)
        h[i] = rootRaisedValue(beta, (static_cast<double>(i) - centre) / static_cast<double>(sps)); // t in symbols
    double energy = 0.0;
    for (double v : h) energy += v * v;
    for (double &v : h) v /= std::sqrt(energy);
    return h;
}

double rootRaisedCosineInterferenceDb(double rolloff, std::size_t samplesPerSymbol, std::size_t spanSymbols)
{
    if (samplesPerSymbol == 0) return -std::numeric_limits<double>::infinity();
    return pairInterferenceDb(rootRaisedCosine(rolloff, samplesPerSymbol, spanSymbols), samplesPerSymbol);
}

std::size_t rootRaisedCosineSpan(double rolloff)
{
    const double beta = std::isfinite(rolloff) ? std::clamp(rolloff, 0.0, 1.0) : 0.35;
    // The spans already found (a link asks for its own several times, a curve of the bit error rate at each point).
    static std::mutex mutex;
    static std::vector<std::pair<double, std::size_t>> found;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        for (const auto &[b, span] : found)
            if (b == beta) return span;
    }
    // The pulse once to the longest span, one side of it (it is even); each shorter one is its middle.
    constexpr std::size_t sps = 16;
    const std::size_t reach = kMaximumPulseSpanSymbols / 2 * sps;
    std::vector<double> side(reach + 1);
    for (std::size_t i = 0; i <= reach; ++i) side[i] = rootRaisedValue(beta, static_cast<double>(i) / static_cast<double>(sps));
    std::size_t span = kMaximumPulseSpanSymbols;
    std::vector<double> h;
    for (std::size_t s = kMinimumPulseSpanSymbols; s < kMaximumPulseSpanSymbols; s += 2)
    {
        const std::size_t half = s / 2 * sps;
        h.assign(2 * half + 1, 0.0);
        for (std::size_t i = 0; i <= 2 * half; ++i) h[i] = side[i >= half ? i - half : half - i];
        if (pairInterferenceDb(h, sps) <= kPulseInterferenceTargetDb)
        {
            span = s;
            break;
        }
    }
    const std::lock_guard<std::mutex> lock(mutex);
    if (found.size() >= 64) found.erase(found.begin());
    found.emplace_back(beta, span);
    return span;
}

std::size_t pulseSpanSymbolsOf(std::size_t given, double rolloff)
{
    return given > 0 ? given : rootRaisedCosineSpan(rolloff);
}

namespace
{
double fraction(double x) noexcept
{
    return x - std::floor(x);
}
} // namespace

AnalogModulator::AnalogModulator(Modulation type, double sampleRateHz, double amplitude, double amIndex,
                                 double frequencyDeviationHz, double phaseDeviationRad) noexcept
    : m_type(type), m_sampleRate(sampleRateHz), m_amplitude(amplitude), m_amIndex(amIndex),
      m_deviation(frequencyDeviationHz), m_phaseDeviation(phaseDeviationRad)
{
}

std::complex<double> AnalogModulator::next(double m) noexcept
{
    // (a cos, a sin rather than std::polar: the same numbers, and defined for a negative amplitude.)
    switch (m_type)
    {
        case Modulation::Am: return {m_amplitude * (1.0 + m_amIndex * m), 0.0};
        case Modulation::Fm:
        {
            const std::complex<double> b(m_amplitude * std::cos(m_theta), m_amplitude * std::sin(m_theta));
            m_theta = std::remainder(m_theta + kTwoPi * m_deviation * m / m_sampleRate, kTwoPi);
            return b;
        }
        default:
        {
            const double phase = m_phaseDeviation * m;
            return {m_amplitude * std::cos(phase), m_amplitude * std::sin(phase)};
        }
    }
}

ModulatedSignal modulate(const ModulationSpec &spec)
{
    ModulatedSignal out;
    const double fs = spec.sampleRateHz;
    const std::size_t n = spec.sampleCount;
    const bool passband = spec.representation == Representation::RealPassband;
    if (!(std::isfinite(fs) && fs > 0.0))
    {
        out.error = "La fréquence d'échantillonnage doit être positive.";
        return out;
    }
    if (n == 0 || n > 100'000'000)
    {
        out.error = "Le nombre d'échantillons doit être compris entre 1 et 100 millions.";
        return out;
    }
    if (passband && !(spec.carrierFrequencyHz > 0.0 && spec.carrierFrequencyHz < 0.5 * fs))
    {
        out.error = "La porteuse doit être comprise entre 0 et fs/2.";
        return out;
    }

    std::vector<std::complex<double>> b(n);
    const double a = spec.carrierAmplitude;

    if (isAnalog(spec.type))
    {
        out.message.resize(n);
        if (spec.message.empty())
        {
            const double ratio = spec.messageFrequencyHz / fs;
            for (std::size_t i = 0; i < n; ++i) out.message[i] = std::sin(kTwoPi * fraction(ratio * static_cast<double>(i)));
        }
        else
        {
            for (std::size_t i = 0; i < n; ++i) out.message[i] = spec.message[i % spec.message.size()];
        }
        AnalogModulator modulator(spec.type, fs, a, spec.amIndex, spec.frequencyDeviationHz, spec.phaseDeviationRad);
        for (std::size_t i = 0; i < n; ++i) b[i] = modulator.next(out.message[i]);
        const double fm = spec.messageFrequencyHz;
        out.occupiedBandwidthHz = spec.type == Modulation::Am   ? 2.0 * fm
                                  : spec.type == Modulation::Fm ? 2.0 * (spec.frequencyDeviationHz + fm)
                                                                : 2.0 * (spec.phaseDeviationRad + 1.0) * fm;
        if (spec.type == Modulation::Am && spec.amIndex > 1.0)
            out.warnings.emplace_back("Indice de modulation supérieur à 1 : surmodulation, l'enveloppe ne suit plus le message.");
    }
    else
    {
        const double rs = spec.symbolRate;
        if (!(std::isfinite(rs) && rs > 0.0))
        {
            out.error = "Le débit de symboles doit être positif.";
            return out;
        }
        const double sps = fs / rs;
        if (sps < 1.0)
        {
            out.error = "Il faut au moins un échantillon par symbole (fs >= débit de symboles).";
            return out;
        }
        if (spec.bits.empty())
        {
            out.error = "Aucun bit à transmettre.";
            return out;
        }
        const bool rrc = spec.pulse == PulseShape::RootRaisedCosine && spec.type != Modulation::Fsk;
        const std::size_t spsInteger = static_cast<std::size_t>(std::llround(sps));
        if (rrc && std::abs(sps - static_cast<double>(spsInteger)) > 1e-9)
        {
            out.error = "Le cosinus surélevé demande un nombre entier d'échantillons par symbole.";
            return out;
        }
        if (rrc && !(spec.rolloff >= 0.0 && spec.rolloff <= 1.0))
        {
            out.error = "Le facteur de retombée doit être compris entre 0 et 1.";
            return out;
        }
        const int bps = bitsPerSymbol(spec.type);
        const auto symbolCount = static_cast<std::size_t>(std::ceil(static_cast<double>(n) * rs / fs - 1e-9));
        out.bits.resize(symbolCount * static_cast<std::size_t>(bps));
        for (std::size_t i = 0; i < out.bits.size(); ++i)
            out.bits[i] = spec.repeatBits ? spec.bits[i % spec.bits.size()] : (i < spec.bits.size() ? spec.bits[i] : 0);
        if (!spec.repeatBits && out.bits.size() > spec.bits.size())
            out.warnings.emplace_back("La charge utile est plus courte que l'enregistrement : complétée par des 0.");
        out.samplesPerSymbol = sps;
        out.bitRate = rs * bps;

        auto symbolIndex = [&](std::size_t i) {
            return std::min(symbolCount - 1, static_cast<std::size_t>(std::floor(static_cast<double>(i) * rs / fs)));
        };

        if (spec.type == Modulation::Fsk)
        {
            // The phase runs on from one sample to the next (continuous phase); its phasor from the angle every
            // Rotator::kBlock samples, and in between turned by the step of the bit.
            double theta = 0.0;
            const double step = kTwoPi * spec.fskDeviationHz / fs;
            const std::complex<double> up(std::cos(step), std::sin(step)), down = std::conj(up);
            std::complex<double> phasor;
            for (std::size_t i = 0; i < n; ++i)
            {
                const std::size_t k = symbolIndex(i);
                if (i % Rotator::kBlock == 0) phasor = std::polar(a, theta);
                b[i] = phasor;
                const bool one = out.bits[k] != 0;
                theta = std::remainder(theta + (one ? step : -step), kTwoPi);
                phasor = turned(phasor, one ? up : down);
            }
            for (std::size_t k = 0; k < symbolCount; ++k) out.symbols.emplace_back(out.bits[k] ? 1.0 : -1.0, 0.0);
            out.occupiedBandwidthHz = 2.0 * spec.fskDeviationHz + 2.0 * rs;
            const double spacing = 2.0 * spec.fskDeviationHz / rs; // tone spacing in units of Rs
            if (spacing < 0.99 || std::abs(spacing - std::round(spacing)) > 0.01)
                out.warnings.emplace_back("Tonalités FSK non orthogonales (leur écart 2 Δf n'est pas un multiple du débit) : "
                                          "le taux d'erreur dépasse la théorie, faite pour des tonalités orthogonales.");
        }
        else
        {
            // Each symbol: the point of its bits, read most significant first.
            const std::vector<std::complex<double>> points = constellation(spec.type, spec.askLowLevel);
            out.symbols.resize(symbolCount);
            for (std::size_t k = 0; k < symbolCount; ++k)
            {
                std::size_t value = 0;
                for (int bit = 0; bit < bps; ++bit)
                    value = (value << 1) | (out.bits[k * static_cast<std::size_t>(bps) + static_cast<std::size_t>(bit)] ? 1u : 0u);
                out.symbols[k] = points[value];
            }
            if (!rrc)
            {
                for (std::size_t i = 0; i < n; ++i) b[i] = a * out.symbols[symbolIndex(i)];
                out.occupiedBandwidthHz = 2.0 * rs;
            }
            else
            {
                // The pulses one after the other, each centred on its symbol (k sps): the filter of the impulses
                // at the symbol instants, as firFilterAligned would give it, but only where there are impulses
                // (one sample in sps: sps times less work).
                const std::vector<double> h =
                    rootRaisedCosine(spec.rolloff, spsInteger, pulseSpanSymbolsOf(spec.pulseSpanSymbols, spec.rolloff));
                const double gain = a * std::sqrt(static_cast<double>(spsInteger));
                const std::size_t delay = (h.size() - 1) / 2;
                for (std::size_t k = 0; k < symbolCount && k * spsInteger < n; ++k)
                {
                    const std::complex<double> v = gain * out.symbols[k];
                    const std::size_t centre = k * spsInteger;
                    const std::size_t first = centre >= delay ? 0 : delay - centre; // taps before sample 0
                    for (std::size_t t = first; t < h.size() && centre + t - delay < n; ++t) b[centre + t - delay] += v * h[t];
                }
                out.occupiedBandwidthHz = (1.0 + spec.rolloff) * rs;
            }
        }
    }

    const std::complex<double> rotation = std::polar(1.0, spec.carrierPhaseDegrees * kPi / 180.0);
    out.inPhase.resize(n);
    out.quadrature.resize(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        b[i] *= rotation;
        out.inPhase[i] = b[i].real();
        out.quadrature[i] = b[i].imag();
    }
    if (passband)
    {
        out.passband.resize(n);
        Rotator carrier(spec.carrierFrequencyHz / fs);
        for (std::size_t i = 0; i < n; ++i)
        {
            const std::complex<double> c = carrier.next();
            out.passband[i] = b[i].real() * c.real() - b[i].imag() * c.imag();
        }
        const double lower = spec.carrierFrequencyHz - 0.5 * out.occupiedBandwidthHz;
        const double upper = spec.carrierFrequencyHz + 0.5 * out.occupiedBandwidthHz;
        if (lower < 0.0 || upper > 0.5 * fs)
            out.warnings.emplace_back("La bande occupée sort de 0..fs/2 : le spectre est replié.");
    }
    return out;
}
} // namespace qacq::dsp
