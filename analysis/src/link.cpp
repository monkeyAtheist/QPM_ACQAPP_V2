#include "qacq/analysis/link.h"

#include "qacq/dsp/channel.h"
#include "qacq/dsp/constants.h"
#include "qacq/dsp/equalizer.h"
#include "qacq/dsp/interpolation.h"
#include "qacq/dsp/synchronization.h"
#include "qacq/plot/axis.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <utility>

namespace qacq::analysis
{
namespace
{
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::size_t kShownTextBytes = 2000;
constexpr std::uint64_t kPreambleSeed = 0x9E3779B97F4A7C15ull; // the preamble: the same bits in every frame
constexpr std::size_t kChannelTailSymbols = 8;                  // received after the end of the transmission

std::string quantity(double value, std::string_view unit, int digits = 4)
{
    return plot::formatCompact(value, unit, digits);
}

std::string grouped(std::size_t value)
{
    const std::string digits = std::to_string(value);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i)
    {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += "\xE2\x80\xAF"; // narrow no-break space
        out += digits[i];
    }
    return out;
}

// Bauds: "1 kBd", "300 Bd".
std::string bauds(double value)
{
    return plot::formatCompact(value, "Bd", 4);
}

std::string decibels(double value)
{
    return plot::formatFixed(value, std::abs(value) < 10.0 ? 2 : 1) + " dB";
}

double number(const model::Json &json, std::string_view key, double fallback, double low, double high)
{
    const model::Json &v = json[key];
    if (!v.isNumber()) return fallback;
    const double x = v.toNumber(fallback);
    if (!std::isfinite(x)) return fallback;
    return std::clamp(x, low, high);
}

std::uint64_t mix(std::uint64_t a, std::uint64_t b) noexcept
{
    std::uint64_t z = a + 0x9E3779B97F4A7C15ull * (b + 1);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// The bytes of the text sent (at most kMaximumLinkTextBytes).
std::vector<std::uint8_t> textBytes(const LinkRecipe &r)
{
    const std::size_t n = std::min(r.text.size(), kMaximumLinkTextBytes);
    return std::vector<std::uint8_t>(r.text.begin(), r.text.begin() + static_cast<std::ptrdiff_t>(n));
}

// The payload repeated over the record, its start at bit `start` (what comes before it: its end, as if the
// transmission had been going on).
std::vector<std::uint8_t> repeated(const std::vector<std::uint8_t> &payload, std::size_t count, std::size_t start)
{
    std::vector<std::uint8_t> bits(count);
    const std::size_t p = payload.size();
    for (std::size_t i = 0; i < count; ++i) bits[i] = payload[(i + p - start % p) % p];
    return bits;
}

// Bytes back from the bits (most significant first, or 8N1 frames), from bit `from`, at most `count` of them;
// a frame whose start or stop bit is wrong gives -1.
std::vector<int> bytesOf(const std::vector<std::uint8_t> &bits, std::size_t from, std::size_t to, bool uart, std::size_t count)
{
    std::vector<int> bytes;
    const std::size_t width = uart ? 10 : 8;
    for (std::size_t at = from; at + width <= to && bytes.size() < count; at += width)
    {
        int value = 0;
        if (uart)
        {
            for (std::size_t b = 0; b < 8; ++b) value |= (bits[at + 1 + b] ? 1 : 0) << b;
            if (bits[at] != 0 || bits[at + 9] != 1) value = -1;
        }
        else
        {
            for (std::size_t b = 0; b < 8; ++b) value = (value << 1) | (bits[at + b] ? 1 : 0);
        }
        bytes.push_back(value);
    }
    return bytes;
}

// Bytes as text that can be shown: UTF-8 as it is, a byte that does not read « � », controls « · » (but tabs and
// new lines).
std::string displayText(const std::vector<int> &bytes)
{
    static const std::string replacement = "\xEF\xBF\xBD";
    std::string out;
    std::size_t i = 0;
    while (i < bytes.size())
    {
        const int b = bytes[i];
        if (b < 0)
        {
            out += replacement;
            ++i;
            continue;
        }
        if (b < 0x80)
        {
            out += (b < 0x20 && b != '\n' && b != '\t') || b == 0x7F ? std::string("\xC2\xB7") : std::string(1, static_cast<char>(b));
            ++i;
            continue;
        }
        // A sequence of 2 to 4 bytes, each continuation 10xxxxxx.
        const std::size_t length = (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
        bool valid = length > 0 && i + length <= bytes.size();
        for (std::size_t k = 1; valid && k < length; ++k) valid = bytes[i + k] >= 0 && (bytes[i + k] & 0xC0) == 0x80;
        if (valid && length == 2) valid = b >= 0xC2; // no overlong form
        if (!valid)
        {
            out += replacement;
            ++i;
            continue;
        }
        for (std::size_t k = 0; k < length; ++k) out += static_cast<char>(bytes[i + k]);
        i += length;
    }
    return out;
}

dsp::ModulationSpec modulationSpec(const LinkRecipe &r, std::vector<std::uint8_t> bits, std::size_t samples)
{
    dsp::ModulationSpec spec;
    spec.type = r.modulation;
    spec.representation = r.representation;
    spec.sampleRateHz = r.sampleRateHz;
    spec.sampleCount = samples;
    spec.carrierFrequencyHz = r.carrierHz;
    spec.carrierAmplitude = r.amplitude;
    spec.carrierPhaseDegrees = r.carrierPhaseDegrees;
    spec.bits = std::move(bits);
    spec.repeatBits = true;
    spec.symbolRate = r.symbolRate;
    spec.askLowLevel = r.askLowLevel;
    spec.fskDeviationHz = r.fskDeviationHz;
    spec.pulse = r.pulse;
    spec.rolloff = r.rolloff;
    return spec;
}

dsp::ReceiverSpec receiverSpec(const LinkRecipe &r)
{
    dsp::ReceiverSpec rx;
    rx.type = r.modulation;
    rx.representation = r.representation;
    rx.sampleRateHz = r.sampleRateHz;
    rx.carrierFrequencyHz = r.carrierHz;
    rx.carrierAmplitude = r.amplitude;
    rx.carrierPhaseDegrees = r.carrierPhaseDegrees;
    rx.symbolRate = r.symbolRate;
    rx.askLowLevel = r.askLowLevel;
    rx.fskDeviationHz = r.fskDeviationHz;
    rx.pulse = r.pulse;
    rx.rolloff = r.rolloff;
    return rx;
}

bool rootRaised(const LinkRecipe &r)
{
    return r.pulse == dsp::PulseShape::RootRaisedCosine && r.modulation != dsp::Modulation::Fsk;
}

// The span the modulator and the receivers cut the root raised cosine to (symbols; 0 for other pulses): the shortest
// from 12 on that leaves less than -50 dB between symbols (dsp::rootRaisedCosineSpan), longer for a small roll-off,
// whose tails decay slowly: 12 at 0.35, 22 at 0.1, 44 at 0.05, 96 (the most) under 0.02.
std::size_t pulseSpan(const LinkRecipe &r)
{
    return rootRaised(r) ? dsp::rootRaisedCosineSpan(r.rolloff) : 0;
}

// What the pair of pulses so cut leaves of the other symbols at the decision instants, against the symbol itself (dB;
// -infinity for other pulses): under -45 dB from a roll-off of 0.02 on, -32 dB at 0.01 even cut to 96 symbols. Said
// when it shows (an EVM of 1 %).
constexpr double kPulseInterferenceWarningDb = -40.0;

double pulseInterferenceDb(const LinkRecipe &r)
{
    const double none = -std::numeric_limits<double>::infinity();
    if (!rootRaised(r)) return none;
    const double sps = r.sampleRateHz / r.symbolRate;
    if (!(sps >= 1.0) || std::abs(sps - std::round(sps)) > 1e-9 * sps) return none;
    return dsp::rootRaisedCosineInterferenceDb(r.rolloff, static_cast<std::size_t>(std::llround(sps)), pulseSpan(r));
}

// Symbols left out at each end: half the span of the root raised cosine, whose tails the ends of the record cut.
std::size_t edgeSymbols(const LinkRecipe &r)
{
    return pulseSpan(r) / 2;
}

bool synchronizedReceiver(const LinkRecipe &r)
{
    return r.receiver == LinkReceiver::Synchronized;
}

// The first symbol of data: after the preamble (receiver Synchronized).
std::size_t preambleLength(const LinkRecipe &r)
{
    return synchronizedReceiver(r) ? r.preambleSymbols : 0;
}

// An equalizer asked for, and a constellation for it to work on (not the FSK, decided by the energies of its tones).
bool equalizing(const LinkRecipe &r)
{
    return r.equalizer != LinkEqualizer::None && dsp::hasConstellation(r.modulation);
}

// Symbols left out at the end: those of the edge, and with an equalizer those whose later neighbours it lacks.
std::size_t endSymbols(const LinkRecipe &r)
{
    return edgeSymbols(r) + (equalizing(r) ? (r.equalizerTaps - 1) / 2 : 0);
}

// The paths of a recipe as the chain uses them: none when there is a single one, as sent.
struct PathSet
{
    std::vector<LinkPath> typed;
    std::vector<double> amplitude; // their powers add up to 1
    std::size_t main = 0;          // the strongest (the first of them)
    double first = 0.0, last = 0.0; // the earliest and the latest, in symbols
    // The ideal receiver through them: where it takes its decisions against the strongest path (symbols: 0, unless
    // it stands where another receiver's timing settled), and what the symbol itself is there against that path
    // alone (1 when the paths are whole symbols apart; a path a fraction of a symbol away adds to it). It knows the
    // channel: it scales and turns its decisions by it.
    double instant = 0.0;
    std::complex<double> reference{1.0, 0.0};

    bool several() const noexcept { return !typed.empty(); }
    double mainDelaySymbols() const noexcept { return several() ? typed[main].delaySymbols : 0.0; }
    double mainPhaseRadians() const noexcept { return several() ? typed[main].phaseDegrees * dsp::kPi / 180.0 : 0.0; }
    double mainAmplitude() const noexcept { return several() ? amplitude[main] : 1.0; }
};

PathSet pathSet(const LinkRecipe &r)
{
    PathSet set;
    std::vector<LinkPath> typed = parseLinkPaths(r.paths);
    if (typed.empty()) return set;
    if (typed.size() == 1 && typed.front().delaySymbols == 0.0 && typed.front().phaseDegrees == 0.0) return set; // as sent
    double power = 0.0;
    for (const LinkPath &p : typed) power += std::pow(10.0, p.gainDb / 10.0);
    if (!(power > 0.0) || !std::isfinite(power)) return set;
    set.first = set.last = typed.front().delaySymbols;
    for (std::size_t k = 0; k < typed.size(); ++k)
    {
        set.amplitude.push_back(std::sqrt(std::pow(10.0, typed[k].gainDb / 10.0) / power));
        if (set.amplitude[k] > set.amplitude[set.main]) set.main = k;
        set.first = std::min(set.first, typed[k].delaySymbols);
        set.last = std::max(set.last, typed[k].delaySymbols);
    }
    set.typed = std::move(typed);
    return set;
}

dsp::ChannelSpec channelSpec(const LinkRecipe &r)
{
    dsp::ChannelSpec ch;
    const double sps = r.sampleRateHz / r.symbolRate;
    ch.sampleRateHz = r.sampleRateHz;
    ch.frequencyOffsetHz = r.frequencyOffsetHz;
    ch.phaseOffsetDegrees = r.phaseOffsetDegrees;
    ch.delaySamples = r.delaySymbols * sps;
    ch.clockOffsetPpm = r.clockOffsetPpm;
    ch.extraSamples = static_cast<std::size_t>(std::ceil(static_cast<double>(kChannelTailSymbols) * sps));
    const PathSet set = pathSet(r);
    for (std::size_t k = 0; k < set.typed.size(); ++k)
        ch.paths.push_back({set.typed[k].delaySymbols * sps, set.amplitude[k], set.typed[k].phaseDegrees});
    if (r.fading != LinkFading::None)
    {
        ch.fading.kind = r.fading == LinkFading::Rice ? dsp::ChannelFading::Kind::Rice : dsp::ChannelFading::Kind::Rayleigh;
        ch.fading.dopplerHz = r.dopplerHz;
        ch.fading.riceK = std::pow(10.0, r.riceKDb / 10.0);
        ch.fading.seed = r.fadingSeed;
    }
    return ch;
}

// The preamble's bits (drawn once, the same in every frame) and its symbols as the receiver knows them.
std::vector<std::uint8_t> preambleBits(const LinkRecipe &r)
{
    return dsp::randomBits(r.preambleSymbols * static_cast<std::size_t>(std::max(1, dsp::bitsPerSymbol(r.modulation))), kPreambleSeed);
}

std::vector<std::complex<double>> preambleSymbols(const LinkRecipe &r)
{
    const std::vector<std::uint8_t> bits = preambleBits(r);
    const int bps = std::max(1, dsp::bitsPerSymbol(r.modulation));
    const std::vector<std::complex<double>> points = dsp::constellation(r.modulation, r.askLowLevel);
    std::vector<std::complex<double>> out;
    for (std::size_t k = 0; k < r.preambleSymbols; ++k)
    {
        std::size_t v = 0;
        for (int b = 0; b < bps; ++b) v = (v << 1) | (bits[k * static_cast<std::size_t>(bps) + static_cast<std::size_t>(b)] ? 1u : 0u);
        if (r.modulation == dsp::Modulation::Fsk)
            out.emplace_back(v ? 1.0 : -1.0, 0.0);
        else
            out.push_back(points[v]);
    }
    return out;
}

// The ideal receiver through a channel: it knows it, and undoes its delay, clock and carrier exactly. The baseband
// at the nominal carrier (the transmitter's phase taken off, as the coherent receiver does), each sample of the
// transmission read where the record received shows it and turned back by the channel's carrier; then the
// coherent receiver. Several paths: where the strongest shows it (`instant` symbols later), turned back and scaled
// so that each symbol stands on the constellation at its decision instant (by that path, and by what the others
// add to the symbol there: `reference`); what the others spread over the neighbouring symbols stays: the
// equalizer's work. Fading: the strongest path's phase undone at every sample (the noise stays as it is), its
// modulus at each decision instant, after the matched filter (as a coherent receiver that knows the gain at each
// symbol: the noise of a decision grows in the fades, not that of its neighbours); the other paths keep fading
// against it.
dsp::DigitalReception idealThroughChannel(const dsp::ReceivedInput &input, const dsp::ReceiverSpec &spec, const dsp::ChannelSpec &ch,
                                          std::size_t samples, const PathSet &paths)
{
    dsp::Baseband bb = dsp::toBaseband(input, spec);
    if (!bb.ok())
    {
        dsp::DigitalReception failed;
        failed.error = bb.error;
        return failed;
    }
    std::vector<double> zi(samples), zq(samples);
    const dsp::SincInterpolator &sinc = dsp::sincInterpolator();
    const double ratio = ch.frequencyOffsetHz / ch.sampleRateHz;
    const double phi = ch.phaseOffsetDegrees * dsp::kPi / 180.0 + paths.mainPhaseRadians();
    const double late = (paths.mainDelaySymbols() + paths.instant) * spec.sampleRateHz / spec.symbolRate;
    const std::complex<double> scale = (1.0 / paths.mainAmplitude()) / paths.reference;
    dsp::FadingGains fading(ch.fading, ch.sampleRateHz, std::max<std::size_t>(1, ch.paths.size()));
    for (std::size_t m = 0; m < samples; ++m)
    {
        const double n = dsp::receivedPosition(ch, static_cast<double>(m) + late);
        const double cycles = ratio * n;
        std::complex<double> v =
            sinc.at(bb.inPhase, bb.quadrature, n) * scale * std::polar(1.0, -(dsp::kTwoPi * (cycles - std::floor(cycles)) + phi));
        if (fading.active())
        {
            const std::complex<double> g = fading.at(paths.main, n);
            if (std::abs(g) > 0.0) v *= std::conj(g) / std::abs(g);
        }
        zi[m] = v.real();
        zq[m] = v.imag();
    }
    dsp::ReceiverSpec baseband = spec;
    baseband.representation = dsp::Representation::ComplexBaseband;
    baseband.carrierPhaseDegrees = 0.0;
    dsp::ReceivedInput undone;
    undone.inPhase = zi;
    undone.quadrature = zq;
    dsp::DigitalReception out = dsp::receiveDigital(undone, baseband);
    if (out.ok() && ch.fading.active() && dsp::hasConstellation(spec.type) &&
        out.decisionTimes.size() == out.decisionPoints.size())
    {
        // The modulus of the strongest path's gain at each decision instant, undone; decided again.
        dsp::FadingGains gains(ch.fading, ch.sampleRateHz, std::max<std::size_t>(1, ch.paths.size()));
        const int bps = std::max(1, dsp::bitsPerSymbol(spec.type));
        out.symbols.clear();
        out.bits.clear();
        for (std::size_t k = 0; k < out.decisionPoints.size(); ++k)
        {
            const double size = std::abs(gains.at(paths.main, dsp::receivedPosition(ch, out.decisionTimes[k] + late)));
            if (size > 1e-12) out.decisionPoints[k] /= size;
            const std::size_t symbol = dsp::decideSymbol(spec.type, out.decisionPoints[k], spec.askLowLevel);
            out.symbols.push_back(symbol);
            for (int b = bps - 1; b >= 0; --b) out.bits.push_back(static_cast<std::uint8_t>((symbol >> b) & 1u));
        }
    }
    for (std::string &w : bb.warnings) out.warnings.push_back(std::move(w));
    out.basebandI = std::move(bb.inPhase); // what was received, the channel in it
    out.basebandQ = std::move(bb.quadrature);
    return out;
}

// What is wrong with the recipe before anything is computed (empty if nothing).
std::string recipeError(const LinkRecipe &r)
{
    if (dsp::isAnalog(r.modulation)) return "Modulation analogique : elle est dans le Générateur (porteuse modulée AM, FM, PM).";
    if (!(r.sampleRateHz > 0.0)) return "La fréquence d'échantillonnage doit être positive.";
    if (!(r.symbolRate > 0.0)) return "Le débit de symboles doit être positif.";
    if (r.symbolRate > r.sampleRateHz)
        return "Il faut au moins un échantillon par symbole : un débit de " + bauds(r.symbolRate) + " demande fs ≥ " +
               quantity(r.symbolRate, "Hz") + ".";
    if (rootRaised(r))
    {
        const double sps = r.sampleRateHz / r.symbolRate;
        if (std::abs(sps - std::round(sps)) > 1e-9 * sps)
            return "Le cosinus surélevé demande un nombre entier d'échantillons par symbole : fs/Rs = " +
                   plot::formatCompact(sps, "", 6) + " (" + bauds(r.symbolRate) + " à " +
                   quantity(r.sampleRateHz, "Hz") + "). Prenez un débit qui divise fs, ou des impulsions rectangulaires.";
    }
    if (r.representation == dsp::Representation::RealPassband && !(r.carrierHz > 0.0 && r.carrierHz < 0.5 * r.sampleRateHz))
        return "La porteuse doit être comprise entre 0 et fs/2 (" + quantity(0.5 * r.sampleRateHz, "Hz") + ").";
    if (!(r.amplitude > 0.0)) return "L'amplitude de la porteuse doit être positive.";
    if (!(r.delaySymbols >= 0.0 && r.delaySymbols <= kMaximumDelaySymbols)) return "Retard : de 0 à 1 000 symboles.";
    if (!(std::abs(r.clockOffsetPpm) <= kMaximumClockPpm)) return "Écart d'horloge : de −10 000 à +10 000 ppm.";
    if (!(std::abs(r.frequencyOffsetHz) <= r.symbolRate))
        return "Décalage de fréquence : au plus le débit de symboles (" + quantity(r.symbolRate, "Hz") + ").";
    if (!std::isfinite(r.phaseOffsetDegrees)) return "La phase du canal doit être un nombre.";
    if (r.representation == dsp::Representation::RealPassband && linkHasChannel(r) &&
        !(r.carrierHz - std::abs(r.frequencyOffsetHz) > 0.0 && r.carrierHz + std::abs(r.frequencyOffsetHz) < 0.5 * r.sampleRateHz))
        return "La porteuse décalée doit rester entre 0 et fs/2.";
    if (synchronizedReceiver(r))
    {
        if (r.sampleRateHz < 2.0 * r.symbolRate) return "La synchronisation demande au moins deux échantillons par symbole.";
        if (r.preambleSymbols < kMinimumPreambleSymbols || r.preambleSymbols > kMaximumPreambleSymbols)
            return "Préambule : de 8 à 1 024 symboles.";
        if (r.preambleSymbols + 16 > std::max(kMinimumLinkSymbols, r.symbols))
            return "Le préambule doit laisser des symboles de données : au moins 16 de plus que lui.";
        if (!(r.timingBandwidth >= 0.0 && r.timingBandwidth <= 0.1 && r.phaseBandwidth >= 0.0 && r.phaseBandwidth <= 0.1))
            return "Bandes des boucles : de 0 à 0,1 (fois le débit de symboles).";
    }
    {
        std::string wrong;
        parseLinkPaths(r.paths, &wrong);
        if (!wrong.empty()) return wrong;
    }
    if (equalizing(r))
    {
        if (r.equalizerTaps < kMinimumEqualizerTaps || r.equalizerTaps > kMaximumEqualizerTaps || r.equalizerTaps % 2 == 0)
            return "Égaliseur : un nombre impair de prises, de " + std::to_string(kMinimumEqualizerTaps) + " à " +
                   std::to_string(kMaximumEqualizerTaps) + ".";
        if (!(r.equalizerStep > 0.0 && r.equalizerStep <= 1.0)) return "Pas de l'égaliseur : entre 0 (exclu) et 1.";
        if (r.equalizer == LinkEqualizer::DecisionFeedback && (r.feedbackTaps < 1 || r.feedbackTaps > kMaximumLinkFeedbackTaps))
            return "Égaliseur à retour de décision : de 1 à " + std::to_string(kMaximumLinkFeedbackTaps) + " prises de retour.";
        if (r.equalizerTaps + 16 > std::max(kMinimumLinkSymbols, r.symbols))
            return "L'égaliseur a plus de prises que l'enregistrement n'a de symboles à lui donner.";
    }
    if (r.fading != LinkFading::None && !(r.dopplerHz >= 0.0 && r.dopplerHz <= kMaximumDopplerPerSymbol * r.symbolRate))
        return "Le décalage Doppler fd va de 0 à un quart du débit symbole (" + quantity(kMaximumDopplerPerSymbol * r.symbolRate, "Hz") +
               ") : au-delà, le canal changerait d'un symbole à l'autre.";
    if (r.data == LinkData::Text && r.text.empty()) return "Aucun texte à envoyer.";
    if (r.data == LinkData::Bits && dsp::bitsFromText(r.bits).empty()) return "Aucun bit à envoyer : tapez des 0 et des 1.";
    return {};
}

// The equalizer of a chain, settled once for a recipe and a noise level.
struct EqualizerPlan
{
    bool active = false;
    bool adaptive = false;                        // learnt (synchronized receiver)
    dsp::SymbolTaps taps;                         // computed (ideal receiver)
    std::vector<std::complex<double>> feedback;   // computed, decision feedback
    dsp::AdaptiveEqualizerSettings settings;      // learnt
    std::vector<std::complex<double>> training;   // the preamble's symbols
};

struct Chain
{
    dsp::ModulatedSignal tx;
    std::vector<double> passband, i, q; // received
    dsp::AwgnResult noise;
    dsp::DigitalReception rx;
    dsp::SyncReception sync; // receiver Synchronized (its reception moved into rx)
    // With an equalizer: the decision points before it, and what it learnt.
    std::vector<std::complex<double>> pointsBefore;
    dsp::AdaptiveEqualization learnt;
    std::string error;
};

// The decision points of a reception through the equalizer; symbols and bits decided again.
void equalize(const LinkRecipe &r, const EqualizerPlan &plan, Chain &c)
{
    c.pointsBefore = c.rx.decisionPoints;
    if (plan.adaptive)
    {
        // Through several paths the receiver's own carrier loop leans on decisions too often wrong: the equalizer
        // starts from the points as the preamble alone straightened them, and brings its own loop after itself.
        dsp::AdaptiveEqualizerSettings settings = plan.settings;
        const bool own = !c.sync.preamblePoints.empty() && c.sync.preamblePoints.size() == c.pointsBefore.size();
        if (!own) settings.carrierBandwidth = 0.0;
        // It learns from the symbols of the frame whose neighbours are symbols too: what the receiver decides after
        // the end of the frame teaches nothing.
        const std::size_t frame = c.tx.bits.size() / static_cast<std::size_t>(std::max(1, dsp::bitsPerSymbol(r.modulation)));
        settings.adaptSymbols = frame > endSymbols(r) ? frame - endSymbols(r) : frame;
        // Nor from the first ones, whose pulses the start of the transmission cuts.
        settings.adaptFrom = edgeSymbols(r);
        c.learnt = own ? dsp::adaptEqualizer(c.sync.preamblePoints, plan.training, r.modulation, r.askLowLevel, settings, c.sync.preambleHalfPoints)
                       : dsp::adaptEqualizer(c.pointsBefore, plan.training, r.modulation, r.askLowLevel, settings);
        if (!c.learnt.ok())
        {
            c.error = c.learnt.error;
            return;
        }
        c.rx.decisionPoints = c.learnt.points;
    }
    else if (!plan.feedback.empty())
        c.rx.decisionPoints = dsp::applyDecisionFeedback(c.pointsBefore, plan.taps, plan.feedback, r.modulation, r.askLowLevel);
    else
        c.rx.decisionPoints = dsp::applyEqualizer(c.pointsBefore, plan.taps);
    const int bps = std::max(1, dsp::bitsPerSymbol(r.modulation));
    c.rx.symbols.clear();
    c.rx.bits.clear();
    for (const std::complex<double> &z : c.rx.decisionPoints)
    {
        const std::size_t symbol = dsp::decideSymbol(r.modulation, z, r.askLowLevel);
        c.rx.symbols.push_back(symbol);
        for (int b = bps - 1; b >= 0; --b) c.rx.bits.push_back(static_cast<std::uint8_t>((symbol >> b) & 1u));
    }
}

// Modulated, through the channel, noise added (when asked), received, equalized (when planned). `known`: the paths
// as the ideal receiver stands in them (none given: on the strongest, scaled by it alone).
Chain runChain(const LinkRecipe &r, std::vector<std::uint8_t> bits, std::size_t samples, bool noise, double ebN0Db,
               std::uint64_t noiseSeed, bool eye = true, const EqualizerPlan *plan = nullptr, const PathSet *known = nullptr)
{
    Chain c;
    c.tx = dsp::modulate(modulationSpec(r, std::move(bits), samples));
    if (!c.tx.ok()) return c;
    const bool passband = r.representation == dsp::Representation::RealPassband;
    // The power of the transmission: what Eb/N0 refers to, whatever silence the channel adds before and after.
    double power = 0.0;
    if (passband)
    {
        for (double v : c.tx.passband) power += v * v;
        power /= static_cast<double>(std::max<std::size_t>(1, c.tx.passband.size()));
    }
    else
    {
        for (std::size_t k = 0; k < c.tx.inPhase.size(); ++k) power += c.tx.inPhase[k] * c.tx.inPhase[k] + c.tx.quadrature[k] * c.tx.quadrature[k];
        power /= static_cast<double>(std::max<std::size_t>(1, c.tx.inPhase.size()));
    }
    const dsp::ChannelSpec ch = channelSpec(r);
    if (linkHasChannel(r))
    {
        dsp::ChannelOutput y = dsp::applyChannel(c.tx.inPhase, c.tx.quadrature, ch);
        if (!y.ok())
        {
            c.error = y.error;
            return c;
        }
        if (passband)
            c.passband = dsp::toPassband(y.inPhase, y.quadrature, r.sampleRateHz, r.carrierHz);
        c.i = std::move(y.inPhase);
        c.q = std::move(y.quadrature);
    }
    else
    {
        c.passband = c.tx.passband;
        c.i = c.tx.inPhase;
        c.q = c.tx.quadrature;
    }
    if (noise)
    {
        dsp::AwgnSpec awgn;
        awgn.level = dsp::AwgnSpec::Level::EbN0Db;
        awgn.valueDb = ebN0Db;
        awgn.bitRate = c.tx.bitRate;
        awgn.seed = noiseSeed;
        awgn.referencePower = power;
        if (passband)
            c.noise = dsp::addAwgn(c.passband, r.sampleRateHz, awgn);
        else
            c.noise = dsp::addAwgn(c.i, c.q, r.sampleRateHz, awgn);
    }
    dsp::ReceivedInput input;
    input.passband = c.passband;
    input.inPhase = c.i;
    input.quadrature = c.q;
    dsp::ReceiverSpec rx = receiverSpec(r);
    rx.matchedOutput = eye;
    if (synchronizedReceiver(r))
    {
        dsp::SyncSettings settings;
        settings.preamble = preambleSymbols(r);
        settings.timingBandwidth = r.timingBandwidth;
        settings.phaseBandwidth = r.phaseBandwidth;
        c.sync = dsp::receiveSynchronized(input, rx, settings);
        c.rx = std::move(c.sync.reception);
        if (!c.sync.ok() && c.rx.error.empty()) c.rx.error = c.sync.error;
    }
    else if (linkHasChannel(r))
    {
        c.rx = idealThroughChannel(input, rx, ch, c.tx.inPhase.size(), known ? *known : pathSet(r));
    }
    else
    {
        c.rx = dsp::receiveDigital(input, rx);
    }
    if (plan && plan->active && c.rx.ok() && c.error.empty()) equalize(r, *plan, c);
    return c;
}

std::vector<std::size_t> symbolIndices(const std::vector<std::uint8_t> &bits, int bps, std::size_t symbols);

// What the decisions of a receiver see through the paths, `instant` symbols after the strongest one (0: the ideal
// receiver; elsewhere: where the timing loop of another one settled). `response`: c[0] is the symbol itself,
// brought to 1 as the receiver's gain and phase do; `reference`: what it was against the strongest path alone.
// The chain run once without noise on symbols drawn for the purpose, and the taps that explain best what the
// ideal receiver standing there decided. No response when the modulation has no constellation, or when the chain
// fails; `vanishing`: the paths cancel at the decision instants (less than a thousandth of the symbol left: the
// response is given against the strongest path, as it is).
struct Probe
{
    dsp::SymbolTaps response;
    std::complex<double> reference{1.0, 0.0};
    bool vanishing = false;
};

Probe probeDecisions(const LinkRecipe &recipe, PathSet paths, double instant)
{
    Probe none;
    if (!dsp::hasConstellation(recipe.modulation) || !std::isfinite(instant)) return none;
    LinkRecipe r = recipe;
    r.receiver = LinkReceiver::Ideal;
    r.equalizer = LinkEqualizer::None;
    r.noise = false;
    r.fading = LinkFading::None; // the paths as they are on average
    paths.instant = instant;
    paths.reference = {1.0, 0.0};
    const double sps = r.sampleRateHz / r.symbolRate;
    const int bps = std::max(1, dsp::bitsPerSymbol(r.modulation));
    // Taps: 8 symbols of the pulses on each side of the paths (about the decision instant).
    const int away = static_cast<int>(std::ceil(std::abs(instant)));
    const int first = -static_cast<int>(std::ceil(paths.mainDelaySymbols() - paths.first)) - 8 - away;
    const int last = static_cast<int>(std::ceil(paths.last - paths.mainDelaySymbols())) + 8 + away;
    const std::size_t edge = edgeSymbols(r);
    const std::size_t room = static_cast<std::size_t>(static_cast<double>(kMaximumLinkSamples) / sps) / 2;
    const std::size_t symbols = std::min<std::size_t>(std::max<std::size_t>(1024, 12 * static_cast<std::size_t>(last - first + 1)), room);
    const auto samples = static_cast<std::size_t>(std::ceil(static_cast<double>(symbols) * sps - 1e-9));
    const Chain c = runChain(r, dsp::randomBits(symbols * static_cast<std::size_t>(bps), 0xC0FFEEull), samples, false, 0.0, 0, false, nullptr, &paths);
    if (!c.tx.ok() || !c.rx.ok() || !c.error.empty()) return none;
    const std::vector<std::complex<double>> points = dsp::constellation(r.modulation, r.askLowLevel);
    const std::vector<std::size_t> sent = symbolIndices(c.tx.bits, bps, c.tx.bits.size() / static_cast<std::size_t>(bps));
    std::vector<std::complex<double>> symbolsSent(sent.size());
    for (std::size_t k = 0; k < sent.size(); ++k) symbolsSent[k] = points[sent[k] % points.size()];
    const std::size_t from = edge + static_cast<std::size_t>(std::max(0, last));
    const std::size_t decided = std::min(c.rx.decisionPoints.size(), sent.size());
    const std::size_t margin = edge + static_cast<std::size_t>(std::max(0, -first));
    if (decided <= from + margin) return none;
    Probe probe;
    probe.response = dsp::fitSymbolResponse(c.rx.decisionPoints, symbolsSent, first, last, from, decided - margin);
    if (probe.response.values.empty()) return probe;
    const std::complex<double> own = probe.response.at(0);
    if (std::abs(own) > 1e-3)
    {
        probe.reference = own;
        for (std::complex<double> &tap : probe.response.values) tap /= own;
    }
    else
        probe.vanishing = true;
    return probe;
}

// The noise at the decision points of the ideal receiver (E|n|^2, unit scale), for a constellation of that mean
// energy: Es/N0 = bits per symbol times Eb/N0, the noise scaled as the symbol is (by the strongest path, and by
// what the others add to it at the decision instants).
double decisionNoise(const LinkRecipe &r, bool noise, double ebN0Db, double symbolEnergy, const PathSet &paths)
{
    if (!noise) return 0.0;
    const double esN0 = static_cast<double>(std::max(1, dsp::bitsPerSymbol(r.modulation))) * std::pow(10.0, ebN0Db / 10.0);
    return symbolEnergy / esN0 / (paths.mainAmplitude() * paths.mainAmplitude() * std::norm(paths.reference));
}

// The equalizer of the recipe at a noise level: computed from the response (ideal receiver) or to be learnt
// (synchronized receiver). `design`: what was computed, for the ideal receiver.
EqualizerPlan equalizerPlan(const LinkRecipe &r, bool noise, double ebN0Db, const PathSet &paths, const dsp::SymbolTaps &response,
                            dsp::LinearEqualizer *design, std::string *error)
{
    EqualizerPlan plan;
    if (!equalizing(r)) return plan;
    const int half = static_cast<int>((r.equalizerTaps - 1) / 2);
    if (synchronizedReceiver(r))
    {
        plan.active = plan.adaptive = true;
        plan.settings.half = half;
        plan.settings.step = r.equalizerStep;
        plan.settings.feedback = r.equalizer == LinkEqualizer::DecisionFeedback ? static_cast<int>(r.feedbackTaps) : 0;
        plan.settings.carrierBandwidth = r.phaseBandwidth;
        plan.training = preambleSymbols(r);
        return plan;
    }
    // Without echo the response is a single 1, whatever was measured: the taps computed leave the points alone.
    dsp::SymbolTaps known = response;
    if (known.values.empty()) known = {{std::complex<double>{1.0, 0.0}}, 0};
    const dsp::SymbolMoments moments = dsp::symbolMoments(dsp::constellation(r.modulation, r.askLowLevel));
    const double noiseVariance = decisionNoise(r, noise, ebN0Db, moments.energy, paths);
    dsp::LinearEqualizer e = r.equalizer == LinkEqualizer::DecisionFeedback
                                 ? dsp::designDecisionFeedbackEqualizer(known, half, static_cast<int>(r.feedbackTaps), noiseVariance, moments)
                                 : dsp::designLinearEqualizer(known, half, noiseVariance, moments);
    if (!e.ok())
    {
        if (error) *error = e.error;
        return plan;
    }
    plan.active = true;
    plan.taps = e.taps;
    plan.feedback = e.feedback;
    if (design) *design = std::move(e);
    return plan;
}

// Symbols and bits counted between the edges (skip at the start, skipEnd at the end of the transmission).
void count(const LinkRecipe &r, const std::vector<std::size_t> &sent, const std::vector<std::size_t> &decided,
           std::size_t skip, std::size_t skipEnd, std::size_t &symbols, std::size_t &symbolErrors, std::size_t &bits,
           std::size_t &bitErrors)
{
    const std::size_t n = std::min(sent.size() > skipEnd ? sent.size() - skipEnd : 0, decided.size());
    const int bps = std::max(1, dsp::bitsPerSymbol(r.modulation));
    symbols = symbolErrors = bits = bitErrors = 0;
    for (std::size_t k = skip; k < n; ++k)
    {
        ++symbols;
        bits += static_cast<std::size_t>(bps);
        const std::size_t diff = sent[k] ^ decided[k];
        if (diff != 0)
        {
            ++symbolErrors;
            bitErrors += static_cast<std::size_t>(std::popcount(diff));
        }
    }
}

// The 97.5 % quantile of Student's t with `dof` degrees of freedom (a table up to 30, then 1.96 + 2.4 / dof: within
// 0.004 of it).
double studentT975(std::size_t dof) noexcept
{
    static constexpr double table[31] = {0.0,   12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
                                         2.201, 2.179,  2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086, 2.080,
                                         2.074, 2.069,  2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042};
    if (dof == 0) return std::numeric_limits<double>::infinity();
    if (dof <= 30) return table[dof];
    return 1.959963984540054 + 2.4 / static_cast<double>(dof);
}

// Bit errors gathered by blocks, for an interval that a fading channel does not make too narrow: a fade takes
// several symbols at once, so that its errors come in bursts, which the binomial interval (each bit apart) does
// not see. Blocks of five periods of fd (the gain forgets itself within less than half a period: J0 is 0 at
// 0.38 / fd), at least 64 symbols; a whole record when fd = 0 (the gains drawn once). Their spread gives the
// interval (batch means, Student's t). Nothing without fading.
class ErrorBatches
{
public:
    explicit ErrorBatches(const LinkRecipe &r)
    {
        if (r.fading == LinkFading::None) return;
        m_length = r.dopplerHz > 0.0 ? std::max<std::size_t>(64, static_cast<std::size_t>(std::ceil(5.0 * r.symbolRate / r.dopplerHz)))
                                     : std::numeric_limits<std::size_t>::max();
        m_bps = std::max(1, dsp::bitsPerSymbol(r.modulation));
    }
    // The symbols of a record, counted as count() counts them. A short remainder joins the block before it.
    void add(const std::vector<std::size_t> &sent, const std::vector<std::size_t> &decided, std::size_t skip, std::size_t skipEnd)
    {
        if (m_length == 0) return;
        const std::size_t n = std::min(sent.size() > skipEnd ? sent.size() - skipEnd : 0, decided.size());
        const std::size_t first = m_errors.size();
        double errors = 0.0, bits = 0.0;
        std::size_t inBlock = 0;
        for (std::size_t k = skip; k < n; ++k)
        {
            errors += static_cast<double>(std::popcount(sent[k] ^ decided[k]));
            bits += static_cast<double>(m_bps);
            if (++inBlock < m_length) continue;
            m_errors.push_back(errors);
            m_bits.push_back(bits);
            errors = bits = 0.0;
            inBlock = 0;
        }
        if (inBlock == 0) return;
        if (2 * inBlock < m_length && m_errors.size() > first)
        {
            m_errors.back() += errors;
            m_bits.back() += bits;
            return;
        }
        m_errors.push_back(errors);
        m_bits.push_back(bits);
    }
    std::size_t blocks() const noexcept { return m_errors.size() >= 2 ? m_errors.size() : 0; }
    std::size_t blockSymbols() const noexcept { return m_length == std::numeric_limits<std::size_t>::max() ? 0 : m_length; }
    // [lower, upper] widened to the interval the blocks give (at least two of them).
    void widen(double &lower, double &upper) const
    {
        const std::size_t m = m_errors.size();
        if (m < 2) return;
        double errors = 0.0, bits = 0.0;
        for (std::size_t b = 0; b < m; ++b)
        {
            errors += m_errors[b];
            bits += m_bits[b];
        }
        if (!(bits > 0.0)) return;
        const double p = errors / bits;
        double spread = 0.0;
        for (std::size_t b = 0; b < m; ++b) spread += (m_errors[b] - p * m_bits[b]) * (m_errors[b] - p * m_bits[b]);
        const double blocks = static_cast<double>(m);
        const double se = std::sqrt(spread / (blocks - 1.0) / blocks) / (bits / blocks);
        const double half = studentT975(m - 1) * se;
        lower = std::min(lower, std::max(0.0, p - half));
        upper = std::max(upper, std::min(1.0, p + half));
    }

private:
    std::size_t m_length = 0; // symbols a block (0: no fading)
    int m_bps = 1;
    std::vector<double> m_errors, m_bits;
};

// The index of each symbol sent, from its bits (most significant first).
std::vector<std::size_t> symbolIndices(const std::vector<std::uint8_t> &bits, int bps, std::size_t symbols)
{
    std::vector<std::size_t> out(symbols, 0);
    for (std::size_t k = 0; k < symbols; ++k)
    {
        std::size_t v = 0;
        for (int b = 0; b < bps; ++b)
        {
            const std::size_t at = k * static_cast<std::size_t>(bps) + static_cast<std::size_t>(b);
            v = (v << 1) | (at < bits.size() && bits[at] ? 1u : 0u);
        }
        out[k] = v;
    }
    return out;
}
} // namespace

LinkResult simulateLink(const LinkRecipe &recipe, const model::Progress &progress)
{
    LinkResult out;
    out.recipe = recipe;
    const LinkRecipe &r = recipe;
    out.error = recipeError(r);
    if (!out.error.empty()) return out;
    const double sps = r.sampleRateHz / r.symbolRate;
    const int bps = dsp::bitsPerSymbol(r.modulation);
    std::size_t symbols = std::max(kMinimumLinkSymbols, r.symbols);
    // The record received holds the delay (of the last path), the clock's stretch and a few symbols after the end too.
    const bool channel = linkHasChannel(r);
    PathSet paths = pathSet(r);
    const double stretch = channel ? 1.0 + 1e-6 * std::abs(r.clockOffsetPpm) : 1.0;
    const double extra = channel ? r.delaySymbols + paths.last + static_cast<double>(kChannelTailSymbols) + 1.0 : 0.0;
    if ((static_cast<double>(symbols) + extra) * sps * stretch > static_cast<double>(kMaximumLinkSamples))
    {
        const double fit = static_cast<double>(kMaximumLinkSamples) / (sps * stretch) - extra;
        symbols = std::max<std::size_t>(kMinimumLinkSymbols, static_cast<std::size_t>(std::max(0.0, fit)));
        out.warnings.push_back("Enregistrement ramené à " + grouped(symbols) + " symboles (" + grouped(kMaximumLinkSamples) +
                               " échantillons au plus).");
    }
    const auto samples = static_cast<std::size_t>(std::ceil(static_cast<double>(symbols) * sps - 1e-9));
    const std::size_t preamble = preambleLength(r);
    const std::size_t skip = std::max(std::min(edgeSymbols(r), symbols / 4), preamble);
    const std::size_t edge = std::min(endSymbols(r), symbols / 4); // at the end
    out.skipSymbols = skip;
    out.skipEndSymbols = edge;
    if (r.modulation == dsp::Modulation::Fsk && std::abs(r.frequencyOffsetHz) > 0.05 * r.symbolRate)
        out.warnings.push_back("Le récepteur FSK (non cohérent) ne corrige pas le décalage de fréquence : ses tonalités, "
                               "reçues à côté de ses filtres, perdent de l'énergie.");
    else if (synchronizedReceiver(r) && std::abs(r.frequencyOffsetHz) > 0.45 * r.symbolRate)
        out.warnings.push_back("Décalage de fréquence proche du demi-débit ou au-delà : la synchronisation ne le trouve "
                               "qu'entre −Rs/2 et +Rs/2.");

    // The bits: the preamble (receiver Synchronized), then data drawn, or the payload repeated with its start where
    // the count starts.
    const std::size_t bitCount = symbols * static_cast<std::size_t>(bps);
    const std::size_t preambleBitCount = preamble * static_cast<std::size_t>(bps);
    const std::size_t dataBitCount = bitCount - std::min(bitCount, preambleBitCount);
    std::vector<std::uint8_t> bits = preamble > 0 ? preambleBits(r) : std::vector<std::uint8_t>{};
    std::vector<std::uint8_t> payload;
    if (r.data == LinkData::Random)
    {
        const std::vector<std::uint8_t> data = dsp::randomBits(dataBitCount, r.dataSeed);
        bits.insert(bits.end(), data.begin(), data.end());
    }
    else
    {
        if (r.data == LinkData::Text)
        {
            const std::vector<std::uint8_t> bytes = textBytes(r);
            if (r.text.size() > bytes.size())
                out.warnings.push_back("Texte coupé à ses " + grouped(kMaximumLinkTextBytes) + " premiers octets.");
            payload = r.uart ? dsp::uart8n1(bytes) : dsp::bitsFromBytesMsbFirst(bytes);
        }
        else
        {
            payload = dsp::bitsFromText(r.bits);
        }
        const std::vector<std::uint8_t> data = repeated(payload, dataBitCount, (skip - preamble) * static_cast<std::size_t>(bps));
        bits.insert(bits.end(), data.begin(), data.end());
        if (r.data == LinkData::Text && payload.size() + (skip + edge) * static_cast<std::size_t>(bps) > bitCount)
            out.warnings.push_back("L'enregistrement est plus court que le texte : seul son début est envoyé (plus de symboles, "
                                   "ou un débit plus grand, pour l'envoyer en entier).");
    }
    if (progress.stopped()) return out;

    // Several paths: what they do at the decision instants (the chain once without noise), what a decision sees
    // without equalizer; then the equalizer, computed from that response or to be learnt.
    LinkEqualization &q = out.equalization;
    q.sinrBeforeDb = q.sinrAfterDb = q.equivalentEbN0Db = q.predictedBer = q.settledErrorDb = kNaN;
    dsp::SymbolTaps response;
    const bool constellated = dsp::hasConstellation(r.modulation);
    const dsp::SymbolMoments moments = constellated ? dsp::symbolMoments(dsp::constellation(r.modulation, r.askLowLevel)) : dsp::SymbolMoments{};
    // What the decisions see `instant` symbols after the strongest path, written into the result: the response
    // there, what the symbol itself is worth there, and its ratio to the other symbols and the noise.
    const auto look = [&](double instant) {
        Probe probe = probeDecisions(r, paths, instant);
        PathSet there = paths;
        there.reference = probe.reference;
        q.response = probe.response.values;
        q.responseFirst = probe.response.first;
        q.instantSymbols = instant;
        q.symbolGain = std::abs(probe.reference);
        q.sinrBeforeDb = kNaN;
        if (!probe.response.values.empty())
        {
            const dsp::LinearEqualizer bare = dsp::withoutEqualizer(probe.response, decisionNoise(r, r.noise, r.ebN0Db, moments.energy, there), moments);
            if (bare.ok()) q.sinrBeforeDb = 10.0 * std::log10(bare.sinr);
        }
        return probe;
    };
    if (paths.several())
    {
        q.paths = true;
        q.typed = paths.typed;
        q.amplitude = paths.amplitude;
        q.mainPath = paths.main;
        // (The synchronized receiver: seen from where its timing loop settles, once it has run.)
        if (!synchronizedReceiver(r))
        {
            const Probe probe = look(0.0);
            paths.reference = probe.reference;
            response = probe.response;
            if (response.values.empty() && constellated)
                out.warnings.push_back("La réponse du canal aux instants de décision n'a pas pu être mesurée.");
            if (probe.vanishing)
                out.warnings.push_back("Les trajets s'annulent presque aux instants de décision : il n'y reste pas un millième du symbole "
                                       "lui-même, le récepteur n'a plus rien à redresser.");
        }
    }
    if (progress.stopped()) return out;
    dsp::LinearEqualizer design;
    std::string planError;
    const EqualizerPlan plan = equalizerPlan(r, r.noise, r.ebN0Db, paths, response, &design, &planError);
    if (!planError.empty())
    {
        out.error = planError;
        return out;
    }

    Chain c = runChain(r, bits, samples, r.noise, r.ebN0Db, r.noiseSeed, true, &plan, &paths);
    if (!c.tx.ok())
    {
        out.error = c.tx.error;
        return out;
    }
    if (!c.error.empty())
    {
        out.error = c.error;
        return out;
    }
    if (!c.rx.ok())
    {
        out.error = c.rx.error;
        return out;
    }
    if (r.noise && !c.noise.ok())
    {
        out.error = c.noise.error;
        return out;
    }
    for (const std::string &w : c.tx.warnings) out.warnings.push_back(w);
    for (const std::string &w : c.rx.warnings) out.warnings.push_back(w);
    if (rootRaised(r) && sps < 2.0)
        out.warnings.push_back("Moins de deux échantillons par symbole : le cosinus surélevé est replié (fs < (1 + α) Rs).");
    // A very small roll-off decays so slowly that even cut to the longest span, the pair of pulses is no longer quite
    // a Nyquist pulse.
    if (const double left = pulseInterferenceDb(r); left > kPulseInterferenceWarningDb)
        out.warnings.push_back("Avec α = " + plot::formatCompact(r.rolloff, "", 3) + ", la racine de cosinus surélevé décroît si lentement que, "
                               "même coupée à " + std::to_string(pulseSpan(r) / 2) + " symboles de part et d'autre, elle n'est plus tout à fait "
                               "une impulsion de Nyquist : à elle seule, sans bruit ni trajets, elle laisse entre symboles une interférence à " +
                               decibels(left) + " du symbole (EVM " + plot::formatFixed(100.0 * std::pow(10.0, left / 20.0), 1) +
                               " %), qui se voit sur les constellations denses. À partir de α = 0,02, il n'en reste presque rien.");

    out.samplesPerSymbol = sps;
    out.bitRate = c.tx.bitRate;
    out.occupiedBandwidthHz = c.tx.occupiedBandwidthHz;
    out.sentBits = c.tx.bits;
    out.sentSymbols = symbolIndices(c.tx.bits, std::max(1, bps), c.tx.bits.size() / static_cast<std::size_t>(std::max(1, bps)));
    if (r.modulation == dsp::Modulation::Fsk)
        out.constellation = {{0.0, 1.0}, {1.0, 0.0}}; // the plane of the two energies: (mark, space)
    else
        out.constellation = dsp::constellation(r.modulation, r.askLowLevel);
    out.sentPassband = std::move(c.tx.passband);
    out.sentI = std::move(c.tx.inPhase);
    out.sentQ = std::move(c.tx.quadrature);
    if (r.representation == dsp::Representation::RealPassband)
    {
        out.receivedPassband = std::move(c.passband);
        // The envelope received: brought down (2 x e^-j(wt + phi)) and freed of its image by the receiver.
        out.receivedI = c.rx.basebandI;
        out.receivedQ = c.rx.basebandQ;
    }
    else
    {
        out.receivedI = std::move(c.i);
        out.receivedQ = std::move(c.q);
    }
    out.reception = std::move(c.rx);
    if (plan.active)
    {
        q.used = true;
        q.adaptive = plan.adaptive;
        q.pointsBefore = std::move(c.pointsBefore);
        if (plan.adaptive)
        {
            q.halfSymbolEarly = c.learnt.halfSymbolEarly;
            q.taps = c.learnt.taps.values;
            q.tapsFirst = c.learnt.taps.first;
            q.feedback = c.learnt.feedback;
            q.squaredError = std::move(c.learnt.squaredError);
            for (double &e : q.squaredError) e /= moments.energy > 0.0 ? moments.energy : 1.0;
            // What it settled to: over the last quarter of the symbols that count.
            double settled = 0.0;
            const std::size_t frame = out.sentSymbols.size();
            const std::size_t n = std::min(q.squaredError.size(), frame > edge ? frame - edge : frame), from = n - n / 4;
            for (std::size_t k = from; k < n; ++k) settled += q.squaredError[k];
            if (n > from && settled > 0.0) q.settledErrorDb = 10.0 * std::log10(settled / static_cast<double>(n - from));
        }
        else
        {
            q.taps = design.taps.values;
            q.tapsFirst = design.taps.first;
            q.feedback = design.feedback;
            q.combined = design.combined.values;
            q.combinedFirst = design.combined.first;
            // (Decision feedback: what is left once it took off q[1 ...].)
            for (std::size_t j = 1; j <= q.feedback.size(); ++j)
                if (const auto at = static_cast<std::ptrdiff_t>(j) - q.combinedFirst; at >= 0 && at < static_cast<std::ptrdiff_t>(q.combined.size()))
                    q.combined[static_cast<std::size_t>(at)] -= q.feedback[j - 1];
            q.sinrAfterDb = 10.0 * std::log10(design.sinr);
            // Per bit, and through the theory: the other symbols left counted as noise.
            q.equivalentEbN0Db = q.sinrAfterDb - 10.0 * std::log10(static_cast<double>(std::max(1, bps)));
            q.predictedBer = std::isfinite(q.equivalentEbN0Db) ? dsp::theoreticalBer(r.modulation, q.equivalentEbN0Db, r.askLowLevel) : 0.0;
        }
    }

    count(r, out.sentSymbols, out.reception.symbols, skip, edge, out.comparedSymbols, out.symbolErrors, out.comparedBits, out.bitErrors);
    out.ber = out.comparedBits > 0 ? static_cast<double>(out.bitErrors) / static_cast<double>(out.comparedBits) : 0.0;
    binomialInterval(out.bitErrors, out.comparedBits, out.berLower, out.berUpper);
    if (r.noise)
    {
        ErrorBatches batches(r);
        batches.add(out.sentSymbols, out.reception.symbols, skip, edge);
        batches.widen(out.berLower, out.berUpper);
        out.berBatches = batches.blocks();
        out.berBatchSymbols = out.berBatches > 0 ? batches.blockSymbols() : 0;
    }
    out.ser = out.comparedSymbols > 0 ? static_cast<double>(out.symbolErrors) / static_cast<double>(out.comparedSymbols) : 0.0;
    out.theoryBer = r.noise ? linkTheoryBer(r, r.ebN0Db) : kNaN;
    out.theorySer = r.noise ? linkTheorySer(r, r.ebN0Db) : kNaN;
    out.awgnTheoryBer = r.noise ? dsp::theoreticalBer(r.modulation, r.ebN0Db, r.askLowLevel) : kNaN;
    out.measuredEbN0Db = r.noise ? c.noise.measuredEbN0Db : kNaN;

    // EVM: the decision points against the points sent, over the symbols counted.
    out.evmPercent = out.merDb = kNaN;
    if (dsp::hasConstellation(r.modulation))
    {
        double error = 0.0, reference = 0.0;
        const std::size_t n =
            std::min(out.sentSymbols.size() > edge ? out.sentSymbols.size() - edge : 0, out.reception.decisionPoints.size());
        for (std::size_t k = skip; k < n; ++k)
        {
            const std::complex<double> s = out.constellation[out.sentSymbols[k]];
            error += std::norm(out.reception.decisionPoints[k] - s);
            reference += std::norm(s);
        }
        if (reference > 0.0)
        {
            out.evmPercent = 100.0 * std::sqrt(error / reference);
            out.merDb = error > 0.0 ? 10.0 * std::log10(reference / error) : std::numeric_limits<double>::infinity();
        }
    }

    // A text: as sent and as received, from where the count starts.
    if (r.data == LinkData::Text)
    {
        // The bits counted: those of the symbols between the edges.
        const std::size_t decided = std::min(out.sentSymbols.size() > edge ? out.sentSymbols.size() - edge : 0, out.reception.symbols.size());
        const std::size_t from = skip * static_cast<std::size_t>(bps);
        const std::size_t to = decided > skip ? decided * static_cast<std::size_t>(bps) : from;
        const std::vector<int> sent = bytesOf(out.sentBits, from, to, r.uart, kShownTextBytes);
        const std::vector<int> got = bytesOf(out.reception.bits, from, to, r.uart, kShownTextBytes);
        out.sentText = displayText(sent);
        out.receivedText = displayText(got);
        for (std::size_t k = 0; k < std::min(sent.size(), got.size()); ++k) out.textErrors += sent[k] != got[k] ? 1 : 0;
    }

    // The synchronized receiver: what it found, against the truth.
    if (synchronizedReceiver(r))
    {
        LinkSync &y = out.sync;
        const dsp::SyncReception &found = c.sync;
        const dsp::ReceiverSpec spec = receiverSpec(r);
        const dsp::ChannelSpec ch = channelSpec(r);
        const double lag = dsp::synchronizedLag(spec);
        const double transmitterPhase = r.carrierPhaseDegrees * dsp::kPi / 180.0;
        // (Several paths: the truth is the strongest one.)
        const double channelPhase = r.phaseOffsetDegrees * dsp::kPi / 180.0 + paths.mainPhaseRadians();
        const double late = paths.mainDelaySymbols() * sps;
        const auto wrapDegrees = [](double radians) { return std::remainder(radians, dsp::kTwoPi) * 180.0 / dsp::kPi; };
        y.used = true;
        y.frameFound = found.frameFound;
        y.frameQuality = found.frameQuality;
        y.gain = found.gain;
        y.trueDelaySymbols = r.delaySymbols + paths.mainDelaySymbols();
        y.delaySymbols = ((found.frameStart - lag) / (1.0 + 1e-6 * found.clockOffsetPpm) - dsp::synchronizedTarget(spec, 0.0)) / sps;
        y.trueFrequencyOffsetHz = r.frequencyOffsetHz;
        y.frequencyOffsetHz = found.frequencyOffsetHz;
        y.trueClockOffsetPpm = r.clockOffsetPpm;
        y.clockOffsetPpm = found.clockOffsetPpm;
        const bool fsk = r.modulation == dsp::Modulation::Fsk;
        // Fading: the phase of the strongest path's gain at the first symbol is part of the truth; Rice: its line of
        // sight turns at its own Doppler shift, as a carrier off by it would.
        double fadedStart = 0.0;
        if (ch.fading.active())
        {
            dsp::FadingGains start(ch.fading, ch.sampleRateHz, std::max<std::size_t>(1, ch.paths.size()));
            if (paths.main == 0) y.lineOfSightDopplerHz = start.lineOfSightDopplerHz();
            fadedStart = std::arg(start.at(paths.main, dsp::receivedPosition(ch, dsp::synchronizedTarget(spec, 0.0) + late)));
        }
        y.truePhaseDegrees = fsk ? 0.0 : wrapDegrees(transmitterPhase + channelPhase + fadedStart);
        y.phaseDegrees = fsk ? 0.0 : wrapDegrees(found.phaseRadians);
        y.rawPoints = found.rawPoints;
        const std::size_t n = std::min(found.instants.size(), out.sentSymbols.size());
        // With an equalizer the carrier is followed after it (see below): the frequency and the phase found are the
        // preamble's, plus what that loop added (the rate its phase kept after the preamble; its phase over it).
        if (!fsk && plan.adaptive && !c.learnt.phases.empty() && c.learnt.phases.size() == found.preamblePhases.size() &&
            found.instants.size() == found.preamblePhases.size() && found.instants.size() > preamble + 16)
        {
            const std::size_t m = found.instants.size();
            const double span = (found.instants.back() - found.instants.front()) / r.sampleRateHz;
            const double preambleHz = span > 0.0 ? (found.preamblePhases.back() - found.preamblePhases.front()) / (dsp::kTwoPi * span) : 0.0;
            double sk = 0.0, sp = 0.0;
            for (std::size_t k = preamble; k < m; ++k)
            {
                sk += static_cast<double>(k);
                sp += c.learnt.phases[k];
            }
            const double count = static_cast<double>(m - preamble), mk = sk / count, mp = sp / count;
            double num = 0.0, den = 0.0;
            for (std::size_t k = preamble; k < m; ++k)
            {
                num += (static_cast<double>(k) - mk) * (c.learnt.phases[k] - mp);
                den += (static_cast<double>(k) - mk) * (static_cast<double>(k) - mk);
            }
            const double rate = den > 0.0 ? num / den : 0.0; // radians per symbol
            y.frequencyOffsetHz = preambleHz + rate * r.symbolRate / dsp::kTwoPi;
            double over = 0.0;
            for (std::size_t k = 0; k < preamble; ++k) over += c.learnt.phases[k];
            const double start = found.preamblePhases.front() - dsp::kTwoPi * preambleHz * found.instants.front() / r.sampleRateHz;
            y.phaseDegrees = wrapDegrees(start + (preamble > 0 ? over / static_cast<double>(preamble) : 0.0));
        }
        // With an equalizer the carrier is followed after it: what the preamble found, then that loop.
        const bool after = plan.adaptive && !c.learnt.phases.empty() && c.learnt.phases.size() == found.phases.size() &&
                           found.preamblePhases.size() == found.phases.size();
        y.timingError.resize(n);
        if (!fsk) y.phaseError.resize(n);
        double timing2 = 0.0, phase2 = 0.0, timingSum = 0.0;
        std::size_t counted = 0;
        // Fading: the truth turns with the strongest path's gain too.
        dsp::FadingGains fadingGains(ch.fading, ch.sampleRateHz, std::max<std::size_t>(1, ch.paths.size()));
        for (std::size_t k = 0; k < n; ++k)
        {
            const double truth = dsp::receivedPosition(ch, dsp::synchronizedTarget(spec, static_cast<double>(k)) + late) + lag;
            y.timingError[k] = (found.instants[k] - truth) / sps;
            if (!fsk)
            {
                const double cycles = r.frequencyOffsetHz * truth / r.sampleRateHz;
                const double carrier = after ? found.preamblePhases[k] + c.learnt.phases[k] : found.phases[k];
                const double faded = fadingGains.active() ? std::arg(fadingGains.at(paths.main, truth - lag)) : 0.0;
                y.phaseError[k] = wrapDegrees(carrier - dsp::kTwoPi * (cycles - std::floor(cycles)) - transmitterPhase - channelPhase - faded);
            }
            if (k >= skip && k + edge < out.sentSymbols.size())
            {
                timing2 += y.timingError[k] * y.timingError[k];
                timingSum += y.timingError[k];
                if (!fsk) phase2 += y.phaseError[k] * y.phaseError[k];
                ++counted;
            }
        }
        y.timingErrorRms = counted > 0 ? std::sqrt(timing2 / static_cast<double>(counted)) : kNaN;
        y.phaseErrorRms = counted > 0 && !fsk ? std::sqrt(phase2 / static_cast<double>(counted)) : kNaN;
        // The timing lost: the last instants (their mean over 32 symbols) more than half a symbol away.
        if (n >= 64)
        {
            double tail = 0.0;
            for (std::size_t k = n - 32; k < n; ++k) tail += y.timingError[k];
            // (Two paths of like strength: it settles between them, up to half a symbol from the stronger.)
            y.timingLost = std::abs(tail / 32.0) > (paths.several() ? 0.75 : 0.5);
        }
        if (y.timingLost)
            out.warnings.push_back(paths.several()
                                       ? "La boucle de rythme a décroché : ces trajets creusent le spectre vers ±Rs/2, là où le détecteur de "
                                         "Gardner lit le rythme (un écho en phase un symbole plus tard, par exemple). Le récepteur idéal "
                                         "montre ce que l'égaliseur ferait d'un rythme connu."
                                       : "La boucle de rythme a décroché : ses instants ont fini à plus d'un demi-symbole des vrais.");
        // Several paths: what the decisions of this receiver see, from where its timing loop settled (the mean of
        // its instants against the strongest path; lost, from that path itself, as the ideal receiver). With an
        // equalizer that took its points half a symbol earlier: the channel from there.
        if (paths.several() && constellated && counted > 0)
        {
            const double settled = y.timingLost ? 0.0 : timingSum / static_cast<double>(counted);
            Probe probe = look(settled);
            if (plan.adaptive && c.learnt.halfSymbolEarly)
            {
                const double before = q.sinrBeforeDb;
                probe = look(settled - 0.5);
                q.sinrBeforeDb = before;
            }
            if (probe.response.values.empty())
                out.warnings.push_back("La réponse du canal aux instants de décision n'a pas pu être mesurée.");
            // What the symbols see after the taps learnt (to the scale and the phase of the receiver).
            if (plan.adaptive && !probe.response.values.empty() && !q.taps.empty())
            {
                const dsp::SymbolTaps both = dsp::convolveTaps(dsp::SymbolTaps{q.taps, q.tapsFirst}, probe.response);
                q.combined = both.values;
                q.combinedFirst = both.first;
                for (std::size_t j = 1; j <= q.feedback.size(); ++j)
                    if (const auto at = static_cast<std::ptrdiff_t>(j) - q.combinedFirst; at >= 0 && at < static_cast<std::ptrdiff_t>(q.combined.size()))
                        q.combined[static_cast<std::size_t>(at)] -= q.feedback[j - 1];
            }
        }
        // Slips: the phase error, averaged over 32 symbols, moving to another symmetry of the constellation.
        const double symmetry = dsp::rotationalSymmetryDegrees(r.modulation);
        if (symmetry > 0.0 && !y.phaseError.empty())
        {
            double unwrapped = y.phaseError.front(), sum = 0.0;
            long previous = 0;
            bool first = true;
            std::size_t inBlock = 0;
            for (std::size_t k = 0; k < y.phaseError.size(); ++k)
            {
                if (k > 0) unwrapped += std::remainder(y.phaseError[k] - y.phaseError[k - 1], 360.0);
                sum += unwrapped;
                if (++inBlock < 32) continue;
                const long turn = std::lround(sum / static_cast<double>(inBlock) / symmetry);
                if (!first && turn != previous) y.phaseSlips += static_cast<std::size_t>(std::labs(turn - previous));
                previous = turn;
                first = false;
                sum = 0.0;
                inBlock = 0;
            }
            // Fading: locked on another symmetry from the end of the preamble on.
            const std::size_t end = std::min(preamble, y.phaseError.size());
            if (r.fading != LinkFading::None && end >= 16)
            {
                std::complex<double> mean{};
                for (std::size_t k = end - 16; k < end; ++k) mean += std::polar(1.0, y.phaseError[k] * dsp::kPi / 180.0);
                const double off = std::arg(mean) * 180.0 / dsp::kPi;
                if (std::abs(off) > 0.5 * symmetry)
                    out.warnings.push_back("Le récepteur synchronisé s'est calé, dès la fin du préambule, " + plot::formatCompact(off, "", 3) +
                                           "° à côté de la porteuse : sur une autre symétrie de la constellation (ses points tournés de " +
                                           plot::formatCompact(symmetry * std::round(off / symmetry), "", 3) +
                                           "°). Ses deux passes supposent une porteuse qui tourne régulièrement (un décalage de fréquence), ce "
                                           "qu'un canal qui s'évanouit ne fait pas : son préambule y voit une phase qui erre.");
            }
        }
    }
    return out;
}

void binomialInterval(std::size_t errors, std::size_t trials, double &lower, double &upper)
{
    if (trials == 0)
    {
        lower = 0.0;
        upper = 1.0;
        return;
    }
    const double z = 1.959963984540054;
    const double n = static_cast<double>(trials);
    const double p = static_cast<double>(errors) / n;
    const double z2 = z * z;
    const double centre = (p + z2 / (2.0 * n)) / (1.0 + z2 / n);
    const double half = z * std::sqrt(p * (1.0 - p) / n + z2 / (4.0 * n * n)) / (1.0 + z2 / n);
    lower = errors == 0 ? 0.0 : std::max(0.0, centre - half);
    upper = std::min(1.0, centre + half);
}

std::vector<double> berSweepValues(const BerSweep &s, std::string *error)
{
    const auto fail = [error](const char *text) {
        if (error) *error = text;
        return std::vector<double>{};
    };
    if (!std::isfinite(s.fromDb) || !std::isfinite(s.toDb) || !std::isfinite(s.stepDb)) return fail("Eb/N0 : des nombres, en dB.");
    if (!(s.stepDb > 0.0)) return fail("Le pas doit être positif.");
    if (s.toDb < s.fromDb) return fail("La courbe va d'un Eb/N0 au plus grand : « à » doit dépasser « de ».");
    if (s.fromDb < -20.0 || s.toDb > 60.0) return fail("Eb/N0 de −20 à 60 dB.");
    const auto count = static_cast<std::size_t>(std::floor((s.toDb - s.fromDb) / s.stepDb + 1e-9)) + 1;
    if (count > kMaximumBerPoints) return fail("Trop de points (121 au plus) : un pas plus grand.");
    if (s.maxBits < 1000 || s.maxBits > kMaximumBerBits) return fail("Bits par point : de 1 000 à 100 millions.");
    if (error) error->clear();
    std::vector<double> values;
    for (std::size_t k = 0; k < count; ++k) values.push_back(s.fromDb + static_cast<double>(k) * s.stepDb);
    return values;
}

BerPoint measureBer(const LinkRecipe &recipe, double ebN0Db, const BerSweep &sweep, std::uint64_t seed,
                    const model::Progress &progress)
{
    BerPoint point;
    point.ebN0Db = ebN0Db;
    point.theory = linkTheoryBer(recipe, ebN0Db);
    LinkRecipe r = recipe;
    r.data = LinkData::Random;
    if (!recipeError(r).empty())
    {
        point.cancelled = true;
        return point;
    }
    const double sps = r.sampleRateHz / r.symbolRate;
    const int bps = dsp::bitsPerSymbol(r.modulation);
    // Pieces of a quarter of the largest record, 512 to 65 536 symbols (frames, each with its preamble when the
    // receiver is synchronized).
    const auto chunk = std::clamp<std::size_t>(static_cast<std::size_t>(static_cast<double>(kMaximumLinkSamples / 4) / sps), 512, 65536);
    const auto samples = static_cast<std::size_t>(std::ceil(static_cast<double>(chunk) * sps - 1e-9));
    const std::size_t preamble = std::min(preambleLength(r), chunk / 2);
    const std::size_t skip = std::max(edgeSymbols(r), preamble);
    const std::size_t edge = endSymbols(r); // at the end
    const std::vector<std::uint8_t> known = preamble > 0 ? preambleBits(r) : std::vector<std::uint8_t>{};
    // The ideal receiver through several paths: what the symbol is at its decision instants (it scales by it), and
    // its equalizer at this Eb/N0, computed from the response there.
    PathSet paths = pathSet(r);
    dsp::SymbolTaps response;
    if (paths.several() && !synchronizedReceiver(r))
    {
        Probe probe = probeDecisions(r, paths, 0.0);
        paths.reference = probe.reference;
        response = std::move(probe.response);
    }
    std::string planError;
    const EqualizerPlan plan = equalizerPlan(r, true, ebN0Db, paths, response, nullptr, &planError);
    if (!planError.empty() || chunk <= skip + edge)
    {
        point.cancelled = true;
        return point;
    }
    // How many bits, decided before counting: enough for about targetErrors at the rate of the theory, within
    // maxBits. Stopping at the targetErrors-th error would bias the rate upwards (stopped early on a run of
    // errors, later on a quiet stretch): about +8 % at 400 errors in pieces of 5 000 bits.
    const double perPiece = static_cast<double>((chunk - skip - edge) * static_cast<std::size_t>(bps));
    // (Several fading paths: the theory does not say; that without fading tells too few bits.)
    const double expected = std::isfinite(point.theory) ? point.theory : r.fading != LinkFading::None ? 0.0 : point.theory;
    double wanted = expected > 0.0 && std::isfinite(expected) ? static_cast<double>(sweep.targetErrors) / expected
                                                              : static_cast<double>(sweep.maxBits);
    // Fading: enough fades, whatever the errors (they come in bursts, a few per fade).
    if (r.fading != LinkFading::None)
        wanted = std::max(wanted, r.dopplerHz > 0.0 ? kFadingPeriodsPerPoint * r.symbolRate / r.dopplerHz * static_cast<double>(bps)
                                                    : 50.0 * perPiece);
    wanted = std::clamp(wanted, std::min(perPiece, static_cast<double>(sweep.maxBits)), static_cast<double>(sweep.maxBits));
    ErrorBatches batches(r);
    for (std::uint64_t piece = 0; static_cast<double>(point.bits) < wanted; ++piece)
    {
        if (progress.stopped())
        {
            point.cancelled = true;
            break;
        }
        std::vector<std::uint8_t> bits = known;
        const std::vector<std::uint8_t> data = dsp::randomBits((chunk - preamble) * static_cast<std::size_t>(bps), mix(seed, 3 * piece));
        bits.insert(bits.end(), data.begin(), data.end());
        // Each piece its own fading (the pieces add up to more periods of fd than one record).
        LinkRecipe faded = r;
        faded.fadingSeed = mix(seed, 3 * piece + 2);
        const Chain c = runChain(faded, bits, samples, true, ebN0Db, mix(seed, 3 * piece + 1), false, &plan, &paths);
        if (!c.tx.ok() || !c.rx.ok() || !c.error.empty())
        {
            point.cancelled = true;
            break;
        }
        const std::vector<std::size_t> sent = symbolIndices(c.tx.bits, bps, c.tx.bits.size() / static_cast<std::size_t>(bps));
        std::size_t symbols = 0, symbolErrors = 0, counted = 0, errors = 0;
        count(r, sent, c.rx.symbols, skip, edge, symbols, symbolErrors, counted, errors);
        batches.add(sent, c.rx.symbols, skip, edge);
        point.bits += counted;
        point.errors += errors;
        progress.report(static_cast<std::uint64_t>(std::min(1.0, static_cast<double>(point.bits) / wanted) * 1000.0), 1000);
    }
    point.ber = point.bits > 0 ? static_cast<double>(point.errors) / static_cast<double>(point.bits) : 0.0;
    binomialInterval(point.errors, point.bits, point.lower, point.upper);
    batches.widen(point.lower, point.upper);
    point.batches = batches.blocks();
    return point;
}

std::string rateText(double rate)
{
    if (!std::isfinite(rate)) return "—";
    if (rate == 0.0) return "0";
    if (rate >= 0.01) return plot::formatFixed(rate, rate >= 0.1 ? 2 : 3);
    return plot::formatQuantity(rate, "", 2);
}

std::string_view modulationKey(dsp::Modulation m) noexcept
{
    switch (m)
    {
        case dsp::Modulation::Ask: return "ask";
        case dsp::Modulation::Fsk: return "fsk";
        case dsp::Modulation::Bpsk: return "bpsk";
        case dsp::Modulation::Qpsk: return "qpsk";
        case dsp::Modulation::Psk8: return "8psk";
        case dsp::Modulation::Qam16: return "16qam";
        case dsp::Modulation::Qam64: return "64qam";
        case dsp::Modulation::Qam256: return "256qam";
        case dsp::Modulation::Am: return "am";
        case dsp::Modulation::Fm: return "fm";
        case dsp::Modulation::Pm: return "pm";
    }
    return "qpsk";
}

std::string_view pulseKey(dsp::PulseShape p) noexcept
{
    return p == dsp::PulseShape::RootRaisedCosine ? "rrc" : "rect";
}

std::string_view linkDataKey(LinkData d) noexcept
{
    switch (d)
    {
        case LinkData::Random: return "random";
        case LinkData::Text: return "text";
        case LinkData::Bits: return "bits";
    }
    return "random";
}

std::string_view representationKey(dsp::Representation r) noexcept
{
    return r == dsp::Representation::ComplexBaseband ? "iq" : "passband";
}

std::string_view receiverKey(LinkReceiver receiver) noexcept
{
    return receiver == LinkReceiver::Synchronized ? "sync" : "ideal";
}

std::string_view equalizerKey(LinkEqualizer equalizer) noexcept
{
    switch (equalizer)
    {
        case LinkEqualizer::None: return "none";
        case LinkEqualizer::Linear: return "linear";
        case LinkEqualizer::DecisionFeedback: return "dfe";
    }
    return "none";
}

std::size_t minimumLinkSymbols(const LinkRecipe &r) noexcept
{
    std::size_t n = kMinimumLinkSymbols;
    if (synchronizedReceiver(r)) n = std::max(n, r.preambleSymbols + 16);
    if (equalizing(r)) n = std::max(n, r.equalizerTaps + 16);
    return n;
}

bool linkHasPaths(const LinkRecipe &r) noexcept
{
    try
    {
        return pathSet(r).several();
    }
    catch (...)
    {
        return false;
    }
}

bool linkHasChannel(const LinkRecipe &r) noexcept
{
    return r.frequencyOffsetHz != 0.0 || r.phaseOffsetDegrees != 0.0 || r.delaySymbols != 0.0 || r.clockOffsetPpm != 0.0 ||
           r.fading != LinkFading::None || linkHasPaths(r);
}

std::string_view fadingKey(LinkFading fading) noexcept
{
    switch (fading)
    {
        case LinkFading::None: return "none";
        case LinkFading::Rayleigh: return "rayleigh";
        case LinkFading::Rice: return "rice";
    }
    return "none";
}

double linkTheoryBer(const LinkRecipe &r, double ebN0Db) noexcept
{
    if (r.fading == LinkFading::None) return dsp::theoreticalBer(r.modulation, ebN0Db, r.askLowLevel);
    if (linkHasPaths(r)) return kNaN;
    return dsp::fadingBer(r.modulation, ebN0Db, r.askLowLevel, r.fading == LinkFading::Rice ? std::pow(10.0, r.riceKDb / 10.0) : 0.0);
}

double linkTheorySer(const LinkRecipe &r, double ebN0Db) noexcept
{
    if (r.fading == LinkFading::None) return dsp::theoreticalSer(r.modulation, ebN0Db, r.askLowLevel);
    if (linkHasPaths(r)) return kNaN;
    return dsp::fadingSer(r.modulation, ebN0Db, r.askLowLevel, r.fading == LinkFading::Rice ? std::pow(10.0, r.riceKDb / 10.0) : 0.0);
}

std::vector<LinkPath> parseLinkPaths(std::string_view text, std::string *error)
{
    std::vector<LinkPath> paths;
    const auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return std::vector<LinkPath>{};
    };
    const auto trimmed = [](std::string_view t) {
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t' || t.front() == '\r')) t.remove_prefix(1);
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r')) t.remove_suffix(1);
        return t;
    };
    std::size_t at = 0;
    while (at <= text.size())
    {
        std::size_t end = text.find_first_of(";\n", at);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view item = trimmed(text.substr(at, end - at));
        at = end + 1;
        if (item.empty()) continue;
        const std::string where = "Trajet " + std::to_string(paths.size() + 1) + " (« " + std::string(item) + " ») : ";
        const std::size_t colon = item.find(':');
        if (colon == std::string_view::npos)
            return fail(where + "attendu « retard : gain », par exemple « 1 : −6 » (un symbole plus tard, 6 dB plus faible).");
        const std::string_view delayText = trimmed(item.substr(0, colon));
        std::string_view gainText = trimmed(item.substr(colon + 1));
        std::string_view phaseText;
        if (const std::size_t turn = gainText.find('@'); turn != std::string_view::npos)
        {
            phaseText = trimmed(gainText.substr(turn + 1));
            gainText = trimmed(gainText.substr(0, turn));
        }
        LinkPath path;
        const plot::Parsed delay = plot::parseQuantity(delayText, "symb.");
        if (!delay.ok) return fail(where + "retard illisible (en symboles : « 0 », « 1,5 »).");
        const plot::Parsed gain = plot::parseQuantity(gainText, "dB");
        if (!gain.ok) return fail(where + "gain illisible (en dB : « 0 », « −6 »).");
        path.delaySymbols = delay.value;
        path.gainDb = gain.value;
        if (!phaseText.empty())
        {
            const plot::Parsed phase = plot::parseQuantity(phaseText, "°");
            if (!phase.ok) return fail(where + "phase illisible (en degrés, après « @ » : « @ 90 »).");
            path.phaseDegrees = phase.value;
        }
        if (!(path.delaySymbols >= 0.0 && path.delaySymbols <= kMaximumPathDelaySymbols))
            return fail(where + "retard de 0 à " + plot::formatCompact(kMaximumPathDelaySymbols, "", 3) + " symboles.");
        if (!(path.gainDb >= -100.0 && path.gainDb <= 100.0)) return fail(where + "gain de −100 à +100 dB.");
        if (!(std::abs(path.phaseDegrees) <= 36000.0)) return fail(where + "phase trop grande.");
        if (paths.size() >= kMaximumLinkPaths) return fail("Trop de trajets : " + std::to_string(kMaximumLinkPaths) + " au plus.");
        paths.push_back(path);
    }
    if (error) error->clear();
    return paths;
}

std::string linkPathsText(const std::vector<LinkPath> &paths)
{
    std::string text;
    for (const LinkPath &p : paths)
    {
        if (!text.empty()) text += " ; ";
        text += plot::formatCompact(p.delaySymbols, "", 6) + " : " + plot::formatCompact(p.gainDb, "", 6);
        if (p.phaseDegrees != 0.0) text += " @ " + plot::formatCompact(p.phaseDegrees, "", 6);
    }
    return text;
}

std::string linkText(const LinkRecipe &r)
{
    std::string text = std::string(dsp::modulationName(r.modulation)) + " " + bauds(r.symbolRate);
    if (r.modulation == dsp::Modulation::Ask)
        text += r.askLowLevel == 0.0 ? " (tout ou rien)" : " (niveau bas " + plot::formatCompact(r.askLowLevel, "", 3) + ")";
    if (r.modulation == dsp::Modulation::Fsk)
    {
        if (r.representation == dsp::Representation::RealPassband)
            text += ", tonalités " + quantity(r.carrierHz - r.fskDeviationHz, "Hz") + " et " + quantity(r.carrierHz + r.fskDeviationHz, "Hz");
        else
            text += ", tonalités à ±" + quantity(r.fskDeviationHz, "Hz");
    }
    else if (rootRaised(r))
    {
        text += ", racine de cosinus surélevé α = " + plot::formatCompact(r.rolloff, "", 3);
    }
    else
    {
        text += ", impulsions rectangulaires";
    }
    if (r.representation == dsp::Representation::RealPassband)
    {
        if (r.modulation != dsp::Modulation::Fsk) text += ", porteuse " + quantity(r.carrierHz, "Hz");
    }
    else
    {
        text += ", en I/Q";
    }
    if (const PathSet paths = pathSet(r); paths.several())
        text += paths.typed.size() > 1 ? ", " + std::to_string(paths.typed.size()) + " trajets" : std::string(", un trajet retardé");
    if (r.fading == LinkFading::Rayleigh) text += ", évanouissements de Rayleigh (fd = " + quantity(r.dopplerHz, "Hz") + ")";
    if (r.fading == LinkFading::Rice)
        text += ", évanouissements de Rice (K = " + decibels(r.riceKDb) + ", fd = " + quantity(r.dopplerHz, "Hz") + ")";
    if (synchronizedReceiver(r)) text += ", synchronisée";
    if (equalizing(r)) text += r.equalizer == LinkEqualizer::DecisionFeedback ? ", égalisée (retour de décision)" : ", égalisée";
    return text;
}

std::string linkShortText(const LinkRecipe &r)
{
    return std::string(dsp::modulationName(r.modulation)) + " " + bauds(r.symbolRate);
}

std::vector<std::string> linkSummary(const LinkResult &res)
{
    std::vector<std::string> lines;
    if (!res.ok()) return lines;
    const LinkRecipe &r = res.recipe;
    const int bps = dsp::bitsPerSymbol(r.modulation);
    lines.push_back("Débit binaire " + quantity(res.bitRate, "bit/s") + " (" + std::to_string(bps) +
                    (bps > 1 ? " bits par symbole" : " bit par symbole") + " à " + bauds(r.symbolRate) +
                    ") ; bande occupée " + quantity(res.occupiedBandwidthHz, "Hz") + ".");
    const std::size_t symbols = res.sentSymbols.size();
    lines.push_back(grouped(symbols) + " symboles (" + quantity(static_cast<double>(symbols) / r.symbolRate, "s") + "), " +
                    plot::formatCompact(res.samplesPerSymbol, "", 4) + " échantillons par symbole.");
    // The pulse longer than the 12 symbols that do at 0.35 (a small roll-off; when long enough, else a warning).
    if (const std::size_t span = pulseSpan(r); span > dsp::kMinimumPulseSpanSymbols)
        if (const double left = pulseInterferenceDb(r); left <= kPulseInterferenceWarningDb)
            lines.push_back("Impulsions coupées à " + std::to_string(span / 2) + " symboles de part et d'autre : la plus courte longueur où, à α = " +
                            plot::formatCompact(r.rolloff, "", 3) + ", la paire émetteur et filtre adapté ne laisse entre symboles que " +
                            decibels(left) + " (6 suffisent à α = 0,35" +
                            (r.rolloff < 0.35 ? " ; plus α est petit, plus l'impulsion décroît lentement)." : ")."));
    if (r.noise)
    {
        lines.push_back("Bruit blanc : Eb/N0 demandé " + decibels(r.ebN0Db) + ", mesuré sur le bruit tiré " +
                        decibels(res.measuredEbN0Db) + ".");
        const double lower = res.berLower, upper = res.berUpper;
        std::string ber = "TEB mesuré " + rateText(res.ber) + " (" + grouped(res.bitErrors) +
                          (res.bitErrors > 1 ? " erreurs sur " : " erreur sur ") + grouped(res.comparedBits) + " bits";
        ber += res.bitErrors == 0 ? ", moins de " + rateText(upper) + " à 95 %" : ", entre " + rateText(lower) + " et " + rateText(upper) + " à 95 %";
        // (Fading: the interval of its blocks, the errors coming in bursts.)
        if (res.berBatches > 0)
            ber += ", d'après " + std::to_string(res.berBatches) + " lots" +
                   (res.berBatchSymbols > 0 ? " de " + grouped(res.berBatchSymbols) + " symboles" : std::string(" (un par tirage)")) +
                   " : les évanouissements groupent les erreurs";
        ber += ")";
        if (r.fading == LinkFading::None)
            ber += " ; théorie " + rateText(res.theoryBer) + ".";
        else if (std::isfinite(res.theoryBer))
            ber += " ; théorie avec ces évanouissements " + rateText(res.theoryBer) + " (sans : " + rateText(res.awgnTheoryBer) + ").";
        else
            ber += " ; la théorie ne dit rien de plusieurs trajets qui s'évanouissent (sans évanouissements : " + rateText(res.awgnTheoryBer) + ").";
        lines.push_back(ber);
        if (std::isfinite(res.theorySer))
            lines.push_back("TES (symboles) mesuré " + rateText(res.ser) + " ; théorie " + rateText(res.theorySer) + ".");
        else
            lines.push_back("TES (symboles) mesuré " + rateText(res.ser) + ".");
        const double periods = r.dopplerHz * static_cast<double>(res.sentSymbols.size()) / r.symbolRate;
        if (r.fading != LinkFading::None && periods < 20.0)
            lines.push_back("Peu de périodes de fd dans l'enregistrement : le TEB mesuré dépend des creux qui y tombent (graine des "
                            "évanouissements) ; la courbe TEB fait la moyenne sur bien plus de symboles.");
        if (r.fading != LinkFading::None && r.dopplerHz >= kNoticedDopplerPerSymbol * r.symbolRate)
            lines.push_back("Le gain change pendant la durée d'une impulsion (fd·T = " + plot::formatCompact(r.dopplerHz / r.symbolRate, "", 3) +
                            ") : le filtre adapté moyenne les creux les plus brefs, et le TEB mesuré passe sous la théorie, qui suppose le "
                            "gain constant sur chaque symbole (d'autant plus à fort Eb/N0, où ces creux font les erreurs).");
    }
    else
    {
        lines.push_back("Sans bruit : " + grouped(res.bitErrors) + (res.bitErrors > 1 ? " erreurs" : " erreur") + " sur " +
                        grouped(res.comparedBits) + " bits.");
    }
    if (std::isfinite(res.evmPercent))
        lines.push_back("EVM " + plot::formatFixed(res.evmPercent, res.evmPercent < 10.0 ? 2 : 1) + " % (MER " +
                        (std::isfinite(res.merDb) ? decibels(res.merDb) : std::string("infini")) +
                        ") : l'écart des points reçus aux points émis.");
    if (linkHasChannel(r))
    {
        std::vector<std::string> parts;
        if (r.frequencyOffsetHz != 0.0) parts.push_back("porteuse décalée de " + plot::formatCompact(r.frequencyOffsetHz, "Hz", 4));
        if (r.phaseOffsetDegrees != 0.0) parts.push_back("tournée de " + plot::formatCompact(r.phaseOffsetDegrees, "", 4) + "°");
        if (r.delaySymbols != 0.0)
            parts.push_back("retard de " + plot::formatCompact(r.delaySymbols, "", 5) + (r.delaySymbols < 2.0 ? " symbole" : " symboles"));
        if (r.clockOffsetPpm != 0.0)
            parts.push_back("horloge du récepteur " + std::string(r.clockOffsetPpm > 0.0 ? "+" : "") + plot::formatCompact(r.clockOffsetPpm, "", 4) + " ppm");
        const LinkEqualization &q = res.equalization;
        if (q.paths)
        {
            // Each path: when, how strong once their powers add up to that of the transmission, turned by how much.
            std::string list;
            for (std::size_t k = 0; k < q.typed.size(); ++k)
            {
                const LinkPath &path = q.typed[k];
                list += std::string(k > 0 ? ", " : "") + plot::formatCompact(path.delaySymbols, "", 4) + " symb. à " +
                        decibels(20.0 * std::log10(std::max(1e-12, q.amplitude[k])));
                if (path.phaseDegrees != 0.0) list += " (" + plot::formatCompact(path.phaseDegrees, "", 4) + "°)";
            }
            parts.push_back(std::string(q.typed.size() > 1 ? std::to_string(q.typed.size()) + " trajets" : "un trajet") + " : " + list);
        }
        if (r.fading != LinkFading::None)
        {
            // How fast, against the symbols; how many periods of fd the record holds (few: the fades drawn decide).
            const double perSymbol = r.dopplerHz / r.symbolRate;
            const double periods = r.dopplerHz * static_cast<double>(res.sentSymbols.size()) / r.symbolRate;
            std::string fading = r.fading == LinkFading::Rice ? "évanouissements de Rice (K = " + decibels(r.riceKDb) + ")"
                                                                : std::string("évanouissements de Rayleigh");
            fading += r.dopplerHz > 0.0 ? ", fd = " + quantity(r.dopplerHz, "Hz") + " (fd·T = " + plot::formatCompact(perSymbol, "", 3) +
                                              " : le gain change en " + plot::formatCompact(std::max(1.0, 1.0 / perSymbol), "", 2) +
                                              " symboles environ, " + plot::formatCompact(periods, "", 3) + " périodes de fd dans l'enregistrement)"
                                        : ", figés (fd = 0 : un gain tiré par trajet)";
            parts.push_back(fading);
        }
        std::string line = "Canal : ";
        for (std::size_t k = 0; k < parts.size(); ++k) line += (k > 0 ? ", " : "") + parts[k];
        lines.push_back(line + ".");
        if (q.paths && std::isfinite(q.sinrBeforeDb))
        {
            // From where: the strongest path (ideal receiver), or where the timing loop settled.
            std::string where = " (le récepteur idéal, calé sur le trajet le plus fort)";
            if (synchronizedReceiver(r) && res.sync.timingLost)
                where = " (vu du trajet le plus fort, comme par le récepteur idéal : la boucle de rythme a décroché)";
            else if (synchronizedReceiver(r))
            {
                const double decided = q.instantSymbols + (q.halfSymbolEarly ? 0.5 : 0.0);
                where = " (aux instants que sa boucle de rythme a trouvés";
                if (std::abs(decided) >= kShownDecisionOffsetSymbols)
                    where += ", " + plot::formatFixed(std::abs(decided), 2) + " symbole " + (decided > 0.0 ? "après" : "avant") + " le trajet le plus fort";
                where += ")";
            }
            lines.push_back("Aux instants de décision, sans égaliseur : chaque symbole contre les autres symboles et le bruit (SINR), " +
                            decibels(q.sinrBeforeDb) + where + ".");
            if (std::abs(q.symbolGain - 1.0) >= 0.01)
                lines.push_back("À ces instants, les trajets donnent au symbole lui-même " + plot::formatFixed(q.symbolGain, 2) +
                                " fois ce qu'en donne le plus fort seul (un trajet à une fraction de symbole de lui s'y ajoute ou s'en "
                                "retranche) : le récepteur le ramène à sa taille.");
        }
    }
    if (!synchronizedReceiver(r))
    {
        if (res.equalization.paths)
        {
            lines.push_back(r.fading != LinkFading::None
                                ? "Récepteur idéal : il connaît le canal ; il défait le retard, l'horloge, la porteuse et, à chaque instant, le "
                                  "gain du trajet le plus fort ; les autres trajets restent étalés sur les symboles voisins, et s'évanouissent à "
                                  "leur façon."
                                : "Récepteur idéal : il connaît le canal ; il défait le retard, l'horloge et la porteuse du trajet le plus "
                                  "fort, les autres trajets restent étalés sur les symboles voisins.");
            if (r.fading != LinkFading::None && res.equalization.used)
                lines.push_back("Son égaliseur est calculé pour les trajets tels qu'ils sont en moyenne : les évanouissements changent leurs "
                                "gains, il n'en sait rien (un égaliseur qui apprend, celui du récepteur synchronisé, les suit).");
        }
        else if (linkHasChannel(r))
            lines.push_back(r.fading != LinkFading::None
                                ? "Récepteur idéal : il connaît le canal, son gain à chaque instant compris, et le défait exactement : les "
                                  "erreurs tombent dans les creux, où le bruit l'emporte."
                                : "Récepteur idéal : il connaît le canal et le défait exactement (le TEB ne dépend que du bruit).");
    }
    else
    {
        const LinkSync &y = res.sync;
        lines.push_back("Récepteur synchronisé : préambule de " + std::to_string(r.preambleSymbols) + " symboles connus, boucles de " +
                        "bande " + plot::formatCompact(r.timingBandwidth, "", 3) + " (rythme) et " + plot::formatCompact(r.phaseBandwidth, "", 3) +
                        " (porteuse), fois le débit.");
        lines.push_back(std::string(y.frameFound ? "Trame trouvée" : "Trame mal trouvée") + " à " + plot::formatFixed(y.delaySymbols, 2) +
                        (std::abs(y.delaySymbols) < 2.0 ? " symbole" : " symboles") + " (vrai " + plot::formatCompact(y.trueDelaySymbols, "", 5) +
                        ", corrélation " +
                        std::to_string(static_cast<int>(std::lround(100.0 * y.frameQuality))) + " %).");
        if (r.modulation != dsp::Modulation::Fsk)
        {
            // (Rice: the line of sight's Doppler shift adds to the offset; fading: the gain's phase at the first symbol to the phase.)
            const std::string doppler = y.lineOfSightDopplerHz != 0.0
                                            ? " ; la visée directe y ajoute son Doppler, " + plot::formatFixed(y.lineOfSightDopplerHz, 2) + " Hz"
                                            : std::string();
            lines.push_back("Décalage de fréquence trouvé " + plot::formatFixed(y.frequencyOffsetHz, 2) + " Hz (vrai " +
                            plot::formatCompact(y.trueFrequencyOffsetHz, "", 4) + " Hz" + doppler + "), phase " +
                            plot::formatFixed(y.phaseDegrees, 1) + "° (vraie " + plot::formatFixed(y.truePhaseDegrees, 1) + "°" +
                            (r.fading != LinkFading::None ? ", avec celle du gain du canal au premier symbole" : "") + ").");
        }
        lines.push_back("Horloge trouvée " + std::string(y.clockOffsetPpm >= 0.0 ? "+" : "") + plot::formatFixed(y.clockOffsetPpm, 1) +
                        " ppm (vraie " + std::string(y.trueClockOffsetPpm >= 0.0 ? "+" : "") + plot::formatCompact(y.trueClockOffsetPpm, "", 4) + ").");
        std::string errors = "Après le préambule : erreur de rythme " + plot::formatFixed(y.timingErrorRms, 3) + " symbole efficace";
        if (r.modulation != dsp::Modulation::Fsk)
            errors += ", erreur de phase " + plot::formatFixed(y.phaseErrorRms, 2) + "° efficace, " +
                      (y.phaseSlips == 0 ? std::string("aucun glissement de phase")
                                         : std::to_string(y.phaseSlips) + (y.phaseSlips > 1 ? " glissements de phase" : " glissement de phase"));
        lines.push_back(errors + ".");
    }
    if (const LinkEqualization &q = res.equalization; q.used)
    {
        const bool dfe = !q.feedback.empty();
        const std::string taps = dfe ? std::to_string(q.taps.size()) + " prises avant, " + std::to_string(q.feedback.size()) + " en retour"
                                     : std::to_string(q.taps.size()) + " prises";
        const std::string kind = dfe ? "Égaliseur à retour de décision" : "Égaliseur linéaire";
        if (dfe)
            lines.push_back("Ses prises de retour retirent ce que les symboles déjà décidés laissent sur le suivant (les prises avant n'ont plus "
                            "à le combattre, et laissent passer moins de bruit) ; une décision fausse renvoie un symbole faux, qui peut "
                            "rendre faux les suivants : les erreurs viennent en salves.");
        if (q.adaptive)
        {
            lines.push_back(kind + " appris (" + taps + ") : les prises qui expliquent le mieux le préambule, puis déplacées "
                            "symbole après symbole d'après ses propres décisions (LMS normalisé de pas " + plot::formatCompact(r.equalizerStep, "", 3) +
                            ")" + (std::isfinite(q.settledErrorDb) ? " ; son erreur finit à " + decibels(q.settledErrorDb) + " de l'énergie d'un symbole." : std::string(".")));
            if (r.phaseBandwidth > 0.0)
                lines.push_back("La boucle de porteuse travaille après lui : ses décisions y sont meilleures.");
            if (q.halfSymbolEarly)
                lines.push_back("Il prend ses points un demi-symbole avant les instants de la boucle de rythme : il y retrouve mieux le préambule.");
        }
        else
        {
            lines.push_back(kind + " calculé (" + taps + ", erreur quadratique minimale" + (dfe ? " si les décisions sont justes" : "") +
                            (r.noise ? "" : " ; sans bruit : inversion") +
                            ") : après lui chaque symbole contre le reste (SINR), " + (std::isfinite(q.sinrAfterDb) ? decibels(q.sinrAfterDb) : std::string("sans limite")) +
                            (std::isfinite(q.sinrBeforeDb) ? " (" + decibels(q.sinrBeforeDb) + " avant)" : std::string()) + ".");
            if (r.noise && std::isfinite(q.equivalentEbN0Db))
                lines.push_back("Soit un Eb/N0 équivalent de " + decibels(q.equivalentEbN0Db) + " (" + decibels(r.ebN0Db - q.equivalentEbN0Db) +
                                " perdus sur le canal), où la théorie donne un TEB de " + rateText(q.predictedBer) +
                                " (les symboles voisins qui restent comptés comme du bruit).");
        }
    }
    const std::string lastOnes = res.equalization.used ? " (l'égaliseur n'a pas leurs voisins suivants)" : "";
    if (res.skipSymbols > r.preambleSymbols && synchronizedReceiver(r))
        lines.push_back("Les " + std::to_string(res.skipSymbols) + " premiers symboles (le préambule, " + std::to_string(r.preambleSymbols) +
                        ", et ceux dont le bord de l'enregistrement coupe encore les impulsions)" +
                        (res.skipEndSymbols > 0 ? " et les " + std::to_string(res.skipEndSymbols) + " derniers" + lastOnes : std::string()) +
                        " ne comptent pas.");
    else if (res.skipSymbols > 0 && synchronizedReceiver(r))
        lines.push_back("Le préambule (" + std::to_string(r.preambleSymbols) + " symboles)" +
                        (res.skipEndSymbols > 0 ? " et les " + std::to_string(res.skipEndSymbols) + " derniers symboles" + lastOnes + " ne comptent pas."
                                                : std::string(" ne compte pas.")));
    else if (res.skipSymbols > 0 && res.skipSymbols == res.skipEndSymbols)
        lines.push_back("Les " + std::to_string(res.skipSymbols) + " premiers et derniers symboles ne comptent pas : la racine de "
                        "cosinus surélevé y est coupée par les bords de l'enregistrement.");
    else if (res.skipSymbols > 0 || res.skipEndSymbols > 0)
        lines.push_back("Les " + std::to_string(res.skipSymbols) + " premiers symboles et les " + std::to_string(res.skipEndSymbols) +
                        " derniers" + lastOnes + " ne comptent pas.");
    if (r.modulation == dsp::Modulation::Fsk)
        lines.push_back(std::string("FSK non cohérente : chaque symbole décidé par l'énergie de ses deux tonalités (sans la phase") +
                        (r.equalizer != LinkEqualizer::None ? " ; pas d'égaliseur pour elle : aucune constellation à redresser)." : ")."));
    return lines;
}

model::Signal linkSignal(const LinkResult &res, bool received)
{
    model::Signal s;
    const LinkRecipe &r = res.recipe;
    s.name = linkShortText(r) + (received ? (r.noise ? " (reçu, Eb/N0 = " + decibels(r.ebN0Db) + ")" : std::string(" (reçu, sans bruit)"))
                                          : std::string(" (émis)"));
    s.origin = model::Origin::Generated;
    s.sampleRateHz = r.sampleRateHz;
    auto data = std::make_shared<model::SampleData>();
    if (r.representation == dsp::Representation::RealPassband)
    {
        data->channels.push_back(received ? res.receivedPassband : res.sentPassband);
        s.channels = {{received ? "Reçu" : "Émis", "V"}};
    }
    else
    {
        data->channels.push_back(received ? res.receivedI : res.sentI);
        data->channels.push_back(received ? res.receivedQ : res.sentQ);
        s.channels = model::defaultChannels(2, "V", model::Representation::IQ);
        s.representation = model::Representation::IQ;
    }
    s.data = std::move(data);
    s.notes = linkText(r) + (received ? ", reçu" : ", émis") + ".";
    for (const std::string &line : linkSummary(res)) s.notes += "\n" + line;
    model::Json provenance = model::Json::object();
    provenance["operation"] = "modulation";
    provenance["parameters"] = toJson(r);
    provenance["received"] = received;
    s.provenance = provenance;
    return s;
}

model::Json toJson(const LinkRecipe &r)
{
    model::Json j = model::Json::object();
    j["modulation"] = modulationKey(r.modulation);
    j["representation"] = representationKey(r.representation);
    j["sampleRate"] = r.sampleRateHz;
    j["symbolRate"] = r.symbolRate;
    j["carrier"] = r.carrierHz;
    j["amplitude"] = r.amplitude;
    j["phase"] = r.carrierPhaseDegrees;
    j["pulse"] = pulseKey(r.pulse);
    j["rolloff"] = r.rolloff;
    j["askLow"] = r.askLowLevel;
    j["fskDeviation"] = r.fskDeviationHz;
    j["data"] = linkDataKey(r.data);
    j["dataSeed"] = static_cast<unsigned long long>(r.dataSeed);
    j["text"] = r.text;
    j["uart"] = r.uart;
    j["bits"] = r.bits;
    j["symbols"] = static_cast<unsigned long long>(r.symbols);
    j["noise"] = r.noise;
    j["ebN0"] = r.ebN0Db;
    j["noiseSeed"] = static_cast<unsigned long long>(r.noiseSeed);
    j["frequencyOffset"] = r.frequencyOffsetHz;
    j["phaseOffset"] = r.phaseOffsetDegrees;
    j["delay"] = r.delaySymbols;
    j["clockPpm"] = r.clockOffsetPpm;
    j["receiver"] = receiverKey(r.receiver);
    j["preamble"] = static_cast<unsigned long long>(r.preambleSymbols);
    j["timingBandwidth"] = r.timingBandwidth;
    j["phaseBandwidth"] = r.phaseBandwidth;
    j["paths"] = r.paths;
    j["fading"] = fadingKey(r.fading);
    j["doppler"] = r.dopplerHz;
    j["riceK"] = r.riceKDb;
    j["fadingSeed"] = static_cast<unsigned long long>(r.fadingSeed);
    j["equalizer"] = equalizerKey(r.equalizer);
    j["equalizerTaps"] = static_cast<unsigned long long>(r.equalizerTaps);
    j["feedbackTaps"] = static_cast<unsigned long long>(r.feedbackTaps);
    j["equalizerStep"] = r.equalizerStep;
    return j;
}

LinkRecipe linkRecipeFromJson(const model::Json &j)
{
    LinkRecipe r;
    if (!j.isObject()) return r;
    const std::string modulation = j["modulation"].toString();
    for (dsp::Modulation m : {dsp::Modulation::Ask, dsp::Modulation::Fsk, dsp::Modulation::Bpsk, dsp::Modulation::Qpsk,
                              dsp::Modulation::Psk8, dsp::Modulation::Qam16, dsp::Modulation::Qam64, dsp::Modulation::Qam256})
        if (modulation == modulationKey(m)) r.modulation = m;
    r.representation = j["representation"].toString() == "iq" ? dsp::Representation::ComplexBaseband : dsp::Representation::RealPassband;
    r.sampleRateHz = number(j, "sampleRate", r.sampleRateHz, 1.0, 1e9);
    r.symbolRate = number(j, "symbolRate", r.symbolRate, 1e-3, 1e9);
    r.carrierHz = number(j, "carrier", r.carrierHz, 0.0, 1e9);
    r.amplitude = number(j, "amplitude", r.amplitude, 1e-9, 1e6);
    r.carrierPhaseDegrees = number(j, "phase", r.carrierPhaseDegrees, -360.0, 360.0);
    if (j["pulse"].isString()) r.pulse = j["pulse"].toString() == "rect" ? dsp::PulseShape::Rectangular : dsp::PulseShape::RootRaisedCosine;
    r.rolloff = number(j, "rolloff", r.rolloff, 0.0, 1.0);
    r.askLowLevel = number(j, "askLow", r.askLowLevel, 0.0, 0.95);
    r.fskDeviationHz = number(j, "fskDeviation", r.fskDeviationHz, 1e-3, 1e9);
    const std::string data = j["data"].toString();
    r.data = data == "text" ? LinkData::Text : data == "bits" ? LinkData::Bits : LinkData::Random;
    r.dataSeed = static_cast<std::uint64_t>(number(j, "dataSeed", static_cast<double>(r.dataSeed), 0.0, 4294967295.0));
    if (j["text"].isString()) r.text = j["text"].toString();
    if (j["uart"].isBool()) r.uart = j["uart"].toBool(false);
    if (j["bits"].isString()) r.bits = j["bits"].toString();
    r.symbols = static_cast<std::size_t>(number(j, "symbols", static_cast<double>(r.symbols), static_cast<double>(kMinimumLinkSymbols),
                                                static_cast<double>(kMaximumLinkSamples)));
    if (j["noise"].isBool()) r.noise = j["noise"].toBool(true);
    r.ebN0Db = number(j, "ebN0", r.ebN0Db, -20.0, 60.0);
    r.noiseSeed = static_cast<std::uint64_t>(number(j, "noiseSeed", static_cast<double>(r.noiseSeed), 0.0, 4294967295.0));
    r.frequencyOffsetHz = number(j, "frequencyOffset", r.frequencyOffsetHz, -1e6, 1e6);
    r.phaseOffsetDegrees = number(j, "phaseOffset", r.phaseOffsetDegrees, -360.0, 360.0);
    r.delaySymbols = number(j, "delay", r.delaySymbols, 0.0, kMaximumDelaySymbols);
    r.clockOffsetPpm = number(j, "clockPpm", r.clockOffsetPpm, -kMaximumClockPpm, kMaximumClockPpm);
    if (j["receiver"].isString()) r.receiver = j["receiver"].toString() == "sync" ? LinkReceiver::Synchronized : LinkReceiver::Ideal;
    r.preambleSymbols = static_cast<std::size_t>(number(j, "preamble", static_cast<double>(r.preambleSymbols),
                                                        static_cast<double>(kMinimumPreambleSymbols), static_cast<double>(kMaximumPreambleSymbols)));
    r.timingBandwidth = number(j, "timingBandwidth", r.timingBandwidth, 0.0, 0.1);
    r.phaseBandwidth = number(j, "phaseBandwidth", r.phaseBandwidth, 0.0, 0.1);
    if (j["paths"].isString()) r.paths = j["paths"].toString();
    const std::string fading = j["fading"].toString();
    r.fading = fading == "rayleigh" ? LinkFading::Rayleigh : fading == "rice" ? LinkFading::Rice : LinkFading::None;
    r.dopplerHz = number(j, "doppler", r.dopplerHz, 0.0, 1e6);
    r.riceKDb = number(j, "riceK", r.riceKDb, kMinimumRiceKDb, kMaximumRiceKDb);
    r.fadingSeed = static_cast<std::uint64_t>(number(j, "fadingSeed", static_cast<double>(r.fadingSeed), 0.0, 4294967295.0));
    if (j["equalizer"].isString())
    {
        const std::string equalizer = j["equalizer"].toString();
        r.equalizer = equalizer == "linear" ? LinkEqualizer::Linear : equalizer == "dfe" ? LinkEqualizer::DecisionFeedback : LinkEqualizer::None;
    }
    r.feedbackTaps = static_cast<std::size_t>(number(j, "feedbackTaps", static_cast<double>(r.feedbackTaps), 1.0,
                                                     static_cast<double>(kMaximumLinkFeedbackTaps)));
    // (An odd number of taps: an even one typed is the next odd one.)
    const auto taps = static_cast<std::size_t>(number(j, "equalizerTaps", static_cast<double>(r.equalizerTaps),
                                                      static_cast<double>(kMinimumEqualizerTaps), static_cast<double>(kMaximumEqualizerTaps)));
    r.equalizerTaps = std::min(kMaximumEqualizerTaps, taps | 1u);
    r.equalizerStep = number(j, "equalizerStep", r.equalizerStep, 1e-4, 1.0);
    return r;
}

LinkRecipe defaultLinkRecipe()
{
    LinkRecipe r;
    r.symbols = 4000;
    r.ebN0Db = 7.0;
    return r;
}

const std::vector<LinkExample> &linkExamples()
{
    static const std::vector<LinkExample> examples = [] {
        std::vector<LinkExample> list;
        const auto add = [&list](std::string name, std::string tip, const std::function<void(LinkRecipe &)> &set) {
            LinkRecipe r = defaultLinkRecipe();
            set(r);
            list.push_back({std::move(name), std::move(tip), r});
        };
        add("BPSK 1 kBd à 6 kHz, impulsions rectangulaires",
            "Deux phases, un bit par symbole : le plus robuste (TEB = Q(√(2 Eb/N0))) ; un lobe principal de 2 kHz",
            [](LinkRecipe &r) {
                r.modulation = dsp::Modulation::Bpsk;
                r.pulse = dsp::PulseShape::Rectangular;
                r.ebN0Db = 6.0;
            });
        add("QPSK en racine de cosinus surélevé (α = 0,35)",
            "Deux bits par symbole pour le même TEB que la BPSK ; la bande tenue à (1 + α) Rs, sans interférence entre symboles",
            [](LinkRecipe &) {});
        add("8-PSK à Eb/N0 = 12 dB", "Trois bits par symbole sur un cercle : les points se rapprochent, il faut plus d'Eb/N0",
            [](LinkRecipe &r) {
                r.modulation = dsp::Modulation::Psk8;
                r.ebN0Db = 12.0;
            });
        add("16-QAM à Eb/N0 = 12 dB", "Quatre bits par symbole : deux niveaux de Gray sur I, deux sur Q",
            [](LinkRecipe &r) {
                r.modulation = dsp::Modulation::Qam16;
                r.ebN0Db = 12.0;
            });
        add("64-QAM en I/Q à Eb/N0 = 18 dB", "Six bits par symbole, l'enveloppe complexe seule (sans porteuse)",
            [](LinkRecipe &r) {
                r.modulation = dsp::Modulation::Qam64;
                r.representation = dsp::Representation::ComplexBaseband;
                r.ebN0Db = 18.0;
            });
        add("256-QAM en I/Q à Eb/N0 = 24 dB", "Huit bits par symbole : chaque décibel compte",
            [](LinkRecipe &r) {
                r.modulation = dsp::Modulation::Qam256;
                r.representation = dsp::Representation::ComplexBaseband;
                r.ebN0Db = 24.0;
            });
        add("ASK tout ou rien : un texte en trame série 8N1",
            "La porteuse allumée pour un 1, éteinte pour un 0 ; le texte comme le ferait une liaison série (bit de start, 8 bits, stop)",
            [](LinkRecipe &r) {
                r.modulation = dsp::Modulation::Ask;
                r.pulse = dsp::PulseShape::Rectangular;
                r.symbolRate = 600.0;
                r.data = LinkData::Text;
                r.uart = true;
                r.symbols = 2400;
                r.ebN0Db = 12.0;
            });
        add("FSK sur les tonalités du Bell 202 (1 200 Bd, 1 200 et 2 200 Hz)",
            "Deux tonalités 1 000 Hz l'une de l'autre à 1 200 Bd : pas orthogonales, plus d'erreurs que la théorie (le prix "
            "d'un modem simple). Ici le 1 est la tonalité haute ; le Bell 202 met le sien, la marque, à 1 200 Hz",
            [](LinkRecipe &r) {
                r.modulation = dsp::Modulation::Fsk;
                r.pulse = dsp::PulseShape::Rectangular;
                r.symbolRate = 1200.0;
                r.carrierHz = 1700.0;
                r.fskDeviationHz = 500.0;
                r.data = LinkData::Text;
                r.uart = true;
                r.symbols = 4800;
                r.ebN0Db = 12.0;
            });
        add("QPSK sans bruit : un texte",
            "Le texte reçu tel qu'il est parti : ajoutez du bruit (Eb/N0) pour voir les premières lettres changer",
            [](LinkRecipe &r) {
                r.data = LinkData::Text;
                r.noise = false;
            });
        // A real channel, and a receiver that has to find it (appended: the examples before keep their place).
        add("QPSK synchronisée : porteuse à +40 Hz, horloge à +200 ppm",
            "Le récepteur ne connaît ni l'instant d'arrivée, ni l'écart de la porteuse, ni celui de son horloge : un "
            "préambule de 64 symboles connus lui donne la trame, la fréquence et la phase, puis deux boucles suivent le "
            "rythme et la porteuse. Leurs erreurs, symbole par symbole, sont tracées à côté de l'œil",
            [](LinkRecipe &r) {
                r.receiver = LinkReceiver::Synchronized;
                r.frequencyOffsetHz = 40.0;
                r.phaseOffsetDegrees = 70.0;
                r.delaySymbols = 5.3;
                r.clockOffsetPpm = 200.0;
                r.ebN0Db = 10.0;
            });
        add("16-QAM synchronisée, arrivée 37,4 symboles plus tard",
            "La trame arrive n'importe quand, la porteuse tournée de −120° : le préambule dit où elle commence, au "
            "centième de symbole près, et de combien la tourner pour remettre la constellation droite",
            [](LinkRecipe &r) {
                r.modulation = dsp::Modulation::Qam16;
                r.receiver = LinkReceiver::Synchronized;
                r.phaseOffsetDegrees = -120.0;
                r.delaySymbols = 37.4;
                r.ebN0Db = 14.0;
            });
        add("QPSK à 0 dB : la boucle de porteuse glisse",
            "Trop de bruit pour une boucle de porteuse de bande 0,01 : vers le milieu de la trame elle saute d'un quart de "
            "tour, et tous les symboles qui suivent sont décidés un quadrant plus loin (un bit sur deux faux, bien "
            "au-dessus de la théorie). Une boucle plus étroite (0,003, dans le panneau Liaison) tient",
            [](LinkRecipe &r) {
                r.receiver = LinkReceiver::Synchronized;
                r.frequencyOffsetHz = 15.0;
                r.delaySymbols = 2.5;
                r.clockOffsetPpm = -100.0;
                r.phaseBandwidth = 0.01;
                r.symbols = 8000;
                r.ebN0Db = 0.0;
                r.noiseSeed = 11;
            });
        add("ASK synchronisée : sa porteuse donne la fréquence",
            "Une ASK tout ou rien garde la moitié de sa puissance dans sa porteuse : le récepteur y lit d'abord l'écart de "
            "fréquence (une raie), puis cherche le préambule ; son seuil suit le niveau reçu",
            [](LinkRecipe &r) {
                r.modulation = dsp::Modulation::Ask;
                r.pulse = dsp::PulseShape::Rectangular;
                r.receiver = LinkReceiver::Synchronized;
                r.frequencyOffsetHz = 25.0;
                r.delaySymbols = 3.7;
                r.clockOffsetPpm = 150.0;
                r.ebN0Db = 12.0;
            });
        // Several paths, and the equalizer (appended too).
        add("QPSK, un écho : chaque point devient quatre",
            "Un second trajet 6 dB plus faible arrive un symbole plus tard : chaque symbole reçoit la moitié du précédent. "
            "Les quatre points de la constellation deviennent seize, l'œil se ferme à moitié, et à 12 dB le TEB passe de "
            "9×10⁻⁹ (la théorie) à quelques millièmes. La vue Canal montre ce que les décisions voient",
            [](LinkRecipe &r) {
                r.paths = "0 : 0 ; 1 : -6";
                r.ebN0Db = 12.0;
            });
        add("Le même écho, égalisé",
            "L'égaliseur linéaire (11 prises, calculées par le récepteur idéal qui connaît le canal) refait d'un écho un seul "
            "symbole : la constellation revient, au prix d'un peu de bruit en plus. Le TEB prévu est celui de la théorie à "
            "l'Eb/N0 qui reste après lui",
            [](LinkRecipe &r) {
                r.paths = "0 : 0 ; 1 : -6";
                r.ebN0Db = 12.0;
                r.equalizer = LinkEqualizer::Linear;
            });
        add("Deux trajets presque égaux : ce que coûte un creux",
            "Deux trajets à 1 dB l'un de l'autre creusent le spectre presque jusqu'à zéro vers ±Rs/2. L'égaliseur linéaire "
            "(31 prises) doit y remonter le signal, et le bruit avec : plusieurs dB d'Eb/N0 perdus, que la vue Canal "
            "montre (sa réponse grimpe là où celle du canal plonge)",
            [](LinkRecipe &r) {
                r.paths = "0 : 0 ; 1 : -1";
                r.ebN0Db = 12.0;
                r.equalizer = LinkEqualizer::Linear;
                r.equalizerTaps = 31;
            });
        add("16-QAM synchronisée, un écho : l'égaliseur apprend",
            "Le récepteur ne connaît rien du canal (porteuse à +40 Hz, horloge à +200 ppm, un écho à −8 dB). Sans égaliseur, "
            "ses décisions sont si souvent fausses que sa boucle de porteuse glisse. Avec lui : les prises apprises sur le "
            "préambule, puis suivies sur ses décisions, la boucle de porteuse placée après. Changez Égaliseur en Aucun "
            "pour voir la différence",
            [](LinkRecipe &r) {
                r.modulation = dsp::Modulation::Qam16;
                r.receiver = LinkReceiver::Synchronized;
                r.frequencyOffsetHz = 40.0;
                r.phaseOffsetDegrees = 70.0;
                r.delaySymbols = 5.3;
                r.clockOffsetPpm = 200.0;
                r.paths = "0 : 0 ; 1 : -8";
                r.ebN0Db = 16.0;
                r.equalizer = LinkEqualizer::Linear;
            });
        // Fading, and the decision feedback equalizer (appended too).
        add("QPSK, évanouissements de Rayleigh (fd = 20 Hz)",
            "Le canal s'évanouit : de temps à autre le signal plonge de 20 dB ou plus (l'enveloppe reçue le montre), et c'est "
            "là que tombent les erreurs. À 15 dB, le TEB passe de 10⁻¹⁵ sans évanouissement à près de 1 % : la théorie le dit "
            "(environ 1 / (4 Eb/N0)), et la courbe TEB le montre : 10 dB de plus ne le divisent que par 10. Le récepteur idéal "
            "connaît le gain du canal à chaque symbole",
            [](LinkRecipe &r) {
                r.fading = LinkFading::Rayleigh;
                r.dopplerHz = 20.0;
                r.ebN0Db = 15.0;
            });
        add("16-QAM, évanouissements de Rice (K = 10 dB)",
            "Une visée directe dix fois plus forte que les ondes dispersées autour d'elle : les creux sont rares et peu "
            "profonds. Le TEB s'approche de celui sans évanouissement quand K grandit (essayez 0 dB : Rayleigh)",
            [](LinkRecipe &r) {
                r.modulation = dsp::Modulation::Qam16;
                r.fading = LinkFading::Rice;
                r.riceKDb = 10.0;
                r.dopplerHz = 10.0;
                r.ebN0Db = 15.0;
            });
        add("Deux trajets presque égaux, égaliseur à retour de décision",
            "Le même creux qu'avec l'égaliseur linéaire, mais ses prises de retour retirent l'écho d'après les symboles déjà "
            "décidés : les prises avant n'ont plus à remonter le creux et laissent passer bien moins de bruit (13 dB après "
            "lui au lieu de 9). Une décision fausse renvoie un symbole faux : les erreurs viennent en salves",
            [](LinkRecipe &r) {
                r.paths = "0 : 0 ; 1 : -1";
                r.ebN0Db = 12.0;
                r.equalizer = LinkEqualizer::DecisionFeedback;
                r.equalizerTaps = 31;
                r.feedbackTaps = 4;
            });
        add("QPSK synchronisée, Rice K = 10 dB, fd = 1 Hz : les boucles suivent",
            "Le récepteur ne connaît pas le canal : sa boucle de porteuse suit la visée directe (son décalage Doppler fait un "
            "écart de fréquence) et l'errance de phase des ondes dispersées. Montez fd : il finit par se caler sur une autre "
            "symétrie de la constellation, ses deux passes supposant une porteuse qui tourne régulièrement",
            [](LinkRecipe &r) {
                r.receiver = LinkReceiver::Synchronized;
                r.fading = LinkFading::Rice;
                r.riceKDb = 10.0;
                r.dopplerHz = 1.0;
                r.ebN0Db = 15.0;
            });
        return list;
    }();
    return examples;
}
} // namespace qacq::analysis
