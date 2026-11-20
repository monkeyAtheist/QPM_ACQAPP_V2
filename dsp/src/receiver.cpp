#include "qacq/dsp/receiver.h"

#include "qacq/dsp/constants.h"
#include "qacq/dsp/filter_design.h"
#include "qacq/dsp/filtering.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace qacq::dsp
{
double gaussianQ(double x) noexcept
{
    return 0.5 * std::erfc(x / std::sqrt(2.0));
}

double theoreticalBer(Modulation modulation, double ebN0Db) noexcept
{
    const double g = std::pow(10.0, ebN0Db / 10.0);
    switch (modulation)
    {
        case Modulation::Bpsk:
        case Modulation::Qpsk: return gaussianQ(std::sqrt(2.0 * g));
        case Modulation::Ask: return gaussianQ(std::sqrt(g));
        case Modulation::Fsk: return 0.5 * std::exp(-0.5 * g);
        default: return std::numeric_limits<double>::quiet_NaN();
    }
}

BitErrors countBitErrors(std::span<const std::uint8_t> sent, std::span<const std::uint8_t> received, std::size_t skip)
{
    BitErrors result;
    const std::size_t count = std::min(sent.size(), received.size());
    for (std::size_t i = skip; i < count; ++i)
    {
        ++result.compared;
        if ((sent[i] != 0) != (received[i] != 0)) ++result.errors;
    }
    result.rate = result.compared ? static_cast<double>(result.errors) / static_cast<double>(result.compared) : 0.0;
    return result;
}

namespace
{
double fraction(double x) noexcept
{
    return x - std::floor(x);
}

// Back to complex baseband, carrier phase removed: r = 2 x exp(-j (2 pi fc t + phi)) for a passband record
// (r = b + conj(b) exp(-j 2 (2 pi fc t + phi)): the wanted signal plus its image around -2 fc), or
// r = b exp(-j phi) for a complex record.
bool mixDown(const ReceivedInput &input, const ReceiverSpec &spec, std::vector<double> &ri, std::vector<double> &rq,
             std::string &error)
{
    const double fs = spec.sampleRateHz;
    const std::complex<double> derotate = std::polar(1.0, -spec.carrierPhaseDegrees * kPi / 180.0);
    if (spec.representation == Representation::ComplexBaseband)
    {
        const std::size_t n = std::min(input.inPhase.size(), input.quadrature.size());
        if (n == 0)
        {
            error = "Signal I/Q vide.";
            return false;
        }
        ri.resize(n);
        rq.resize(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            const std::complex<double> v = std::complex<double>(input.inPhase[i], input.quadrature[i]) * derotate;
            ri[i] = v.real();
            rq[i] = v.imag();
        }
        return true;
    }
    const std::size_t n = input.passband.size();
    if (n == 0)
    {
        error = "Signal vide.";
        return false;
    }
    if (!(spec.carrierFrequencyHz > 0.0 && spec.carrierFrequencyHz < 0.5 * fs))
    {
        error = "La porteuse doit être comprise entre 0 et fs/2.";
        return false;
    }
    ri.resize(n);
    rq.resize(n);
    const double ratio = spec.carrierFrequencyHz / fs;
    for (std::size_t i = 0; i < n; ++i)
    {
        const std::complex<double> lo = std::polar(2.0, -kTwoPi * fraction(ratio * static_cast<double>(i))) * derotate;
        const std::complex<double> v = input.passband[i] * lo;
        ri[i] = v.real();
        rq[i] = v.imag();
    }
    return true;
}

// Removes the 2 fc image of a mixed-down passband record with a linear-phase Kaiser low-pass (60 dB):
// the signal occupies |f| <= bandEdge, the image |f - image| <= bandEdge with image = 2 fc folded.
void removeImage(const ReceiverSpec &spec, double bandEdgeHz, std::vector<double> &ri, std::vector<double> &rq,
                 std::vector<std::string> &warnings)
{
    if (spec.representation == Representation::ComplexBaseband) return;
    const double fs = spec.sampleRateHz;
    const double folded = std::fmod(2.0 * spec.carrierFrequencyHz, fs);
    const double image = std::min(folded, fs - folded);
    const double transition = image - 2.0 * bandEdgeHz;
    if (transition <= 0.02 * fs)
    {
        warnings.emplace_back("L'image à 2 fc recouvre la bande du signal : choisir une autre porteuse.");
        return;
    }
    const KaiserEstimate kaiser = estimateKaiser(60.0, transition, fs);
    FirSpec lowPass;
    lowPass.sampleRateHz = fs;
    lowPass.frequency1Hz = 0.5 * image;
    lowPass.taps = std::clamp<std::size_t>(kaiser.taps | 1u, 15, 4001);
    lowPass.window = Window::Kaiser;
    lowPass.kaiserBeta = kaiser.beta;
    const FirDesign design = designFir(lowPass);
    if (!design.ok()) return;
    ri = firFilterAligned(design.taps, ri);
    rq = firFilterAligned(design.taps, rq);
}

void smooth(std::vector<double> &x, double cutoffHz, double fs)
{
    if (!(cutoffHz > 0.0 && cutoffHz < 0.45 * fs) || x.size() < 16) return;
    const KaiserEstimate kaiser = estimateKaiser(60.0, 0.5 * cutoffHz, fs);
    FirSpec lowPass;
    lowPass.sampleRateHz = fs;
    lowPass.frequency1Hz = 1.25 * cutoffHz;
    lowPass.taps = std::clamp<std::size_t>(kaiser.taps | 1u, 15, 4001);
    lowPass.window = Window::Kaiser;
    lowPass.kaiserBeta = kaiser.beta;
    const FirDesign design = designFir(lowPass);
    if (design.ok()) x = firFilterAligned(design.taps, x);
}
} // namespace

DigitalReception receiveDigital(const ReceivedInput &input, const ReceiverSpec &spec)
{
    DigitalReception out;
    const double fs = spec.sampleRateHz, rs = spec.symbolRate;
    if (isAnalog(spec.type))
    {
        out.error = "Modulation analogique : utiliser receiveAnalog.";
        return out;
    }
    if (!(fs > 0.0 && rs > 0.0 && fs >= rs))
    {
        out.error = "Il faut fs > 0, un débit de symboles positif et au moins un échantillon par symbole.";
        return out;
    }
    const bool rrc = spec.pulse == PulseShape::RootRaisedCosine && spec.type != Modulation::Fsk;
    const double bandEdge = spec.type == Modulation::Fsk ? spec.fskDeviationHz + rs
                            : rrc                       ? 0.5 * (1.0 + spec.rolloff) * rs
                                                        : rs;
    std::vector<double> ri, rq;
    if (!mixDown(input, spec, ri, rq, out.error)) return out;
    out.basebandI = ri;
    out.basebandQ = rq;
    removeImage(spec, bandEdge, out.basebandI, out.basebandQ, out.warnings);
    const std::size_t n = ri.size();
    const double a = spec.carrierAmplitude != 0.0 ? spec.carrierAmplitude : 1.0;
    const auto symbols = static_cast<std::size_t>(std::floor(static_cast<double>(n) * rs / fs + 1e-9));

    if (rrc)
    {
        const double sps = fs / rs;
        const auto spsInteger = static_cast<std::size_t>(std::llround(sps));
        if (std::abs(sps - static_cast<double>(spsInteger)) > 1e-9)
        {
            out.error = "Le cosinus surélevé demande un nombre entier d'échantillons par symbole.";
            return out;
        }
        const std::vector<double> h = rootRaisedCosine(spec.rolloff, spsInteger, spec.pulseSpanSymbols);
        const auto yi = firFilterAligned(h, out.basebandI); // image filter, then matched filter
        const auto yq = firFilterAligned(h, out.basebandQ);
        const double scale = 1.0 / (a * std::sqrt(static_cast<double>(spsInteger)));
        for (std::size_t k = 0; k * spsInteger < n; ++k)
            out.decisionPoints.emplace_back(yi[k * spsInteger] * scale, yq[k * spsInteger] * scale);
    }
    else
    {
        out.decisionPoints.assign(symbols, {0.0, 0.0});
        std::vector<std::complex<double>> mark(symbols), space(symbols);
        std::vector<std::size_t> counts(symbols, 0);
        const double ratio = spec.fskDeviationHz / fs;
        for (std::size_t i = 0; i < n; ++i)
        {
            const auto k = static_cast<std::size_t>(std::floor(static_cast<double>(i) * rs / fs));
            if (k >= symbols) break;
            // Correlation receiver on the raw mixer output: integrating over the symbol is the matched filter
            // of the passband pulse itself, image included.
            const std::complex<double> v(ri[i], rq[i]);
            ++counts[k];
            if (spec.type == Modulation::Fsk)
            {
                const std::complex<double> tone = std::polar(1.0, -kTwoPi * fraction(ratio * static_cast<double>(i)));
                mark[k] += v * tone;
                space[k] += v * std::conj(tone);
            }
            else
            {
                out.decisionPoints[k] += v;
            }
        }
        for (std::size_t k = 0; k < symbols; ++k)
        {
            const double c = static_cast<double>(std::max<std::size_t>(1, counts[k]));
            if (spec.type == Modulation::Fsk)
                out.decisionPoints[k] = {std::abs(mark[k]) / (c * a), std::abs(space[k]) / (c * a)};
            else
                out.decisionPoints[k] /= c * a;
        }
    }

    for (const auto &p : out.decisionPoints)
    {
        switch (spec.type)
        {
            case Modulation::Bpsk: out.bits.push_back(p.real() < 0.0 ? 1 : 0); break;
            case Modulation::Qpsk:
                out.bits.push_back(p.imag() < 0.0 ? 1 : 0);
                out.bits.push_back(p.real() < 0.0 ? 1 : 0);
                break;
            case Modulation::Ask: out.bits.push_back(p.real() > 0.5 * (1.0 + spec.askLowLevel) ? 1 : 0); break;
            case Modulation::Fsk: out.bits.push_back(p.real() > p.imag() ? 1 : 0); break;
            default: break;
        }
    }
    return out;
}

AnalogReception receiveAnalog(const ReceivedInput &input, const ReceiverSpec &spec)
{
    AnalogReception out;
    const double fs = spec.sampleRateHz;
    if (!isAnalog(spec.type))
    {
        out.error = "Modulation numérique : utiliser receiveDigital.";
        return out;
    }
    if (!(fs > 0.0) || !(spec.messageBandwidthHz > 0.0))
    {
        out.error = "Il faut fs > 0 et une bande de message positive.";
        return out;
    }
    const double bandEdge = spec.type == Modulation::Fm ? spec.frequencyDeviationHz + spec.messageBandwidthHz
                            : spec.type == Modulation::Pm ? (spec.phaseDeviationRad + 1.0) * spec.messageBandwidthHz
                                                          : spec.messageBandwidthHz;
    std::vector<double> bi, bq;
    if (!mixDown(input, spec, bi, bq, out.error)) return out;
    removeImage(spec, bandEdge, bi, bq, out.warnings);
    if (spec.type == Modulation::Pm && spec.phaseDeviationRad > kPi)
        out.warnings.emplace_back("Excursion de phase supérieure à pi : la phase est déroulée à 2 pi près.");
    const std::size_t n = bi.size();
    const double a = spec.carrierAmplitude != 0.0 ? spec.carrierAmplitude : 1.0;
    out.message.resize(n);
    switch (spec.type)
    {
        case Modulation::Am:
            if (!(spec.amIndex > 0.0))
            {
                out.error = "L'indice de modulation AM doit être positif.";
                return out;
            }
            for (std::size_t i = 0; i < n; ++i) out.message[i] = (std::hypot(bi[i], bq[i]) / a - 1.0) / spec.amIndex;
            break;
        case Modulation::Fm:
            if (!(spec.frequencyDeviationHz > 0.0))
            {
                out.error = "L'excursion de fréquence doit être positive.";
                return out;
            }
            for (std::size_t i = 1; i < n; ++i)
            {
                const std::complex<double> now(bi[i], bq[i]), before(bi[i - 1], bq[i - 1]);
                // The sample at i - 1 -> i carries the frequency of sample i - 1 (the modulator advances after).
                out.message[i - 1] = std::arg(now * std::conj(before)) * fs / (kTwoPi * spec.frequencyDeviationHz);
            }
            if (n >= 2) out.message[n - 1] = out.message[n - 2];
            break;
        default:
        {
            if (!(spec.phaseDeviationRad > 0.0))
            {
                out.error = "L'excursion de phase doit être positive.";
                return out;
            }
            double previous = 0.0, correction = 0.0;
            for (std::size_t i = 0; i < n; ++i)
            {
                const double raw = std::atan2(bq[i], bi[i]);
                if (i > 0)
                {
                    const double delta = raw - previous;
                    if (delta > kPi) correction -= kTwoPi;
                    else if (delta < -kPi) correction += kTwoPi;
                }
                previous = raw;
                out.message[i] = (raw + correction) / spec.phaseDeviationRad;
            }
            break;
        }
    }
    smooth(out.message, spec.messageBandwidthHz, fs);
    return out;
}
} // namespace qacq::dsp
