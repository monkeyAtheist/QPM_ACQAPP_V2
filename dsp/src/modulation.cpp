#include "qacq/dsp/modulation.h"

#include "qacq/dsp/constants.h"
#include "qacq/dsp/filtering.h"
#include "qacq/dsp/random.h"

#include <algorithm>
#include <cmath>

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

std::vector<double> rootRaisedCosine(double beta, std::size_t sps, std::size_t span)
{
    if (sps == 0) return {1.0};
    std::size_t length = std::max<std::size_t>(1, span) * sps;
    length += length % 2; // odd number of taps: the peak sits on a sample and the delay is an integer
    const std::size_t taps = length + 1;
    const double centre = 0.5 * static_cast<double>(length);
    std::vector<double> h(taps);
    for (std::size_t i = 0; i < taps; ++i)
    {
        const double t = (static_cast<double>(i) - centre) / static_cast<double>(sps); // in symbols
        double v;
        if (std::abs(t) < 1e-12)
            v = 1.0 - beta + 4.0 * beta / kPi;
        else if (beta > 0.0 && std::abs(std::abs(t) - 1.0 / (4.0 * beta)) < 1e-9)
            v = beta / std::sqrt(2.0) *
                ((1.0 + 2.0 / kPi) * std::sin(kPi / (4.0 * beta)) + (1.0 - 2.0 / kPi) * std::cos(kPi / (4.0 * beta)));
        else
            v = (std::sin(kPi * t * (1.0 - beta)) + 4.0 * beta * t * std::cos(kPi * t * (1.0 + beta))) /
                (kPi * t * (1.0 - (4.0 * beta * t) * (4.0 * beta * t)));
        h[i] = v;
    }
    double energy = 0.0;
    for (double v : h) energy += v * v;
    for (double &v : h) v /= std::sqrt(energy);
    return h;
}

namespace
{
constexpr double kInvSqrt2 = 0.70710678118654752440;

std::complex<double> symbolOf(Modulation type, const std::uint8_t *bits, double askLow) noexcept
{
    switch (type)
    {
        case Modulation::Bpsk: return {bits[0] ? -1.0 : 1.0, 0.0};
        case Modulation::Qpsk: return {bits[1] ? -kInvSqrt2 : kInvSqrt2, bits[0] ? -kInvSqrt2 : kInvSqrt2};
        case Modulation::Ask: return {bits[0] ? 1.0 : askLow, 0.0};
        default: return {0.0, 0.0};
    }
}

double fraction(double x) noexcept
{
    return x - std::floor(x);
}
} // namespace

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
        double theta = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            const double m = out.message[i];
            switch (spec.type)
            {
                case Modulation::Am: b[i] = {a * (1.0 + spec.amIndex * m), 0.0}; break;
                case Modulation::Fm:
                    b[i] = std::polar(a, theta);
                    theta = std::remainder(theta + kTwoPi * spec.frequencyDeviationHz * m / fs, kTwoPi);
                    break;
                default: b[i] = std::polar(a, spec.phaseDeviationRad * m); break;
            }
        }
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
            double theta = 0.0;
            for (std::size_t i = 0; i < n; ++i)
            {
                const std::size_t k = symbolIndex(i);
                b[i] = std::polar(a, theta);
                const double f = out.bits[k] ? spec.fskDeviationHz : -spec.fskDeviationHz;
                theta = std::remainder(theta + kTwoPi * f / fs, kTwoPi);
            }
            for (std::size_t k = 0; k < symbolCount; ++k) out.symbols.emplace_back(out.bits[k] ? 1.0 : -1.0, 0.0);
            out.occupiedBandwidthHz = 2.0 * spec.fskDeviationHz + 2.0 * rs;
            const double spacing = 2.0 * spec.fskDeviationHz / rs; // tone spacing in units of Rs
            if (spacing < 0.99 || std::abs(spacing - std::round(spacing)) > 0.01)
                out.warnings.emplace_back("Tonalités FSK non orthogonales (écart 2 delta f non multiple du débit) : "
                                          "le taux d'erreur dépasse la théorie.");
        }
        else
        {
            out.symbols.resize(symbolCount);
            for (std::size_t k = 0; k < symbolCount; ++k)
                out.symbols[k] = symbolOf(spec.type, &out.bits[k * static_cast<std::size_t>(bps)], spec.askLowLevel);
            if (!rrc)
            {
                for (std::size_t i = 0; i < n; ++i) b[i] = a * out.symbols[symbolIndex(i)];
                out.occupiedBandwidthHz = 2.0 * rs;
            }
            else
            {
                const std::vector<double> h = rootRaisedCosine(spec.rolloff, spsInteger, spec.pulseSpanSymbols);
                std::vector<double> ui(n, 0.0), uq(n, 0.0);
                const double gain = a * std::sqrt(static_cast<double>(spsInteger));
                for (std::size_t k = 0; k < symbolCount && k * spsInteger < n; ++k)
                {
                    ui[k * spsInteger] = gain * out.symbols[k].real();
                    uq[k * spsInteger] = gain * out.symbols[k].imag();
                }
                const auto si = firFilterAligned(h, ui);
                const auto sq = firFilterAligned(h, uq);
                for (std::size_t i = 0; i < n; ++i) b[i] = {si[i], sq[i]};
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
        const double ratio = spec.carrierFrequencyHz / fs;
        for (std::size_t i = 0; i < n; ++i)
            out.passband[i] = (b[i] * std::polar(1.0, kTwoPi * fraction(ratio * static_cast<double>(i)))).real();
        const double lower = spec.carrierFrequencyHz - 0.5 * out.occupiedBandwidthHz;
        const double upper = spec.carrierFrequencyHz + 0.5 * out.occupiedBandwidthHz;
        if (lower < 0.0 || upper > 0.5 * fs)
            out.warnings.emplace_back("La bande occupée sort de 0..fs/2 : le spectre est replié.");
    }
    return out;
}
} // namespace qacq::dsp
