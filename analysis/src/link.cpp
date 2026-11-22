#include "qacq/analysis/link.h"

#include "qacq/dsp/channel.h"
#include "qacq/dsp/constants.h"
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

// Symbols left out at each end: half the span of the root raised cosine (12 symbols), whose tails the ends cut.
std::size_t edgeSymbols(const LinkRecipe &r)
{
    return rootRaised(r) ? 6 : 0;
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
    if (r.data == LinkData::Text && r.text.empty()) return "Aucun texte à envoyer.";
    if (r.data == LinkData::Bits && dsp::bitsFromText(r.bits).empty()) return "Aucun bit à envoyer : tapez des 0 et des 1.";
    return {};
}

struct Chain
{
    dsp::ModulatedSignal tx;
    std::vector<double> passband, i, q; // received
    dsp::AwgnResult noise;
    dsp::DigitalReception rx;
};

// Modulated, noise added (when asked), received.
Chain runChain(const LinkRecipe &r, std::vector<std::uint8_t> bits, std::size_t samples, bool noise, double ebN0Db,
               std::uint64_t noiseSeed, bool eye = true)
{
    Chain c;
    c.tx = dsp::modulate(modulationSpec(r, std::move(bits), samples));
    if (!c.tx.ok()) return c;
    c.passband = c.tx.passband;
    c.i = c.tx.inPhase;
    c.q = c.tx.quadrature;
    if (noise)
    {
        dsp::AwgnSpec awgn;
        awgn.level = dsp::AwgnSpec::Level::EbN0Db;
        awgn.valueDb = ebN0Db;
        awgn.bitRate = c.tx.bitRate;
        awgn.seed = noiseSeed;
        if (r.representation == dsp::Representation::RealPassband)
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
    c.rx = dsp::receiveDigital(input, rx);
    return c;
}

// Symbols and bits counted between the edges.
void count(const LinkRecipe &r, const std::vector<std::size_t> &sent, const std::vector<std::size_t> &decided,
           std::size_t skip, std::size_t &symbols, std::size_t &symbolErrors, std::size_t &bits, std::size_t &bitErrors)
{
    const std::size_t n = std::min(sent.size(), decided.size());
    const int bps = std::max(1, dsp::bitsPerSymbol(r.modulation));
    symbols = symbolErrors = bits = bitErrors = 0;
    for (std::size_t k = skip; k + skip < n; ++k)
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
    if (static_cast<double>(symbols) * sps > static_cast<double>(kMaximumLinkSamples))
    {
        symbols = std::max<std::size_t>(kMinimumLinkSymbols, static_cast<std::size_t>(static_cast<double>(kMaximumLinkSamples) / sps));
        out.warnings.push_back("Enregistrement ramené à " + grouped(symbols) + " symboles (" + grouped(kMaximumLinkSamples) +
                               " échantillons au plus).");
    }
    const auto samples = static_cast<std::size_t>(std::ceil(static_cast<double>(symbols) * sps - 1e-9));
    const std::size_t skip = std::min(edgeSymbols(r), symbols / 4);
    out.skipSymbols = skip;

    // The bits: drawn, or the payload repeated with its start where the count starts.
    const std::size_t bitCount = symbols * static_cast<std::size_t>(bps);
    std::vector<std::uint8_t> bits;
    std::vector<std::uint8_t> payload;
    if (r.data == LinkData::Random)
    {
        bits = dsp::randomBits(bitCount, r.dataSeed);
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
        bits = repeated(payload, bitCount, skip * static_cast<std::size_t>(bps));
        if (r.data == LinkData::Text && payload.size() + 2 * skip * static_cast<std::size_t>(bps) > bitCount)
            out.warnings.push_back("L'enregistrement est plus court que le texte : seul son début est envoyé (plus de symboles, "
                                   "ou un débit plus grand, pour l'envoyer en entier).");
    }
    if (progress.stopped()) return out;

    Chain c = runChain(r, bits, samples, r.noise, r.ebN0Db, r.noiseSeed);
    if (!c.tx.ok())
    {
        out.error = c.tx.error;
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

    count(r, out.sentSymbols, out.reception.symbols, skip, out.comparedSymbols, out.symbolErrors, out.comparedBits, out.bitErrors);
    out.ber = out.comparedBits > 0 ? static_cast<double>(out.bitErrors) / static_cast<double>(out.comparedBits) : 0.0;
    out.ser = out.comparedSymbols > 0 ? static_cast<double>(out.symbolErrors) / static_cast<double>(out.comparedSymbols) : 0.0;
    out.theoryBer = r.noise ? dsp::theoreticalBer(r.modulation, r.ebN0Db, r.askLowLevel) : kNaN;
    out.theorySer = r.noise ? dsp::theoreticalSer(r.modulation, r.ebN0Db, r.askLowLevel) : kNaN;
    out.measuredEbN0Db = r.noise ? c.noise.measuredEbN0Db : kNaN;

    // EVM: the decision points against the points sent, over the symbols counted.
    out.evmPercent = out.merDb = kNaN;
    if (dsp::hasConstellation(r.modulation))
    {
        double error = 0.0, reference = 0.0;
        const std::size_t n = std::min(out.sentSymbols.size(), out.reception.decisionPoints.size());
        for (std::size_t k = skip; k + skip < n; ++k)
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
        const std::size_t decided = std::min(out.sentSymbols.size(), out.reception.symbols.size());
        const std::size_t from = skip * static_cast<std::size_t>(bps);
        const std::size_t to = decided > 2 * skip ? (decided - skip) * static_cast<std::size_t>(bps) : from;
        const std::vector<int> sent = bytesOf(out.sentBits, from, to, r.uart, kShownTextBytes);
        const std::vector<int> got = bytesOf(out.reception.bits, from, to, r.uart, kShownTextBytes);
        out.sentText = displayText(sent);
        out.receivedText = displayText(got);
        for (std::size_t k = 0; k < std::min(sent.size(), got.size()); ++k) out.textErrors += sent[k] != got[k] ? 1 : 0;
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
    point.theory = dsp::theoreticalBer(recipe.modulation, ebN0Db, recipe.askLowLevel);
    LinkRecipe r = recipe;
    r.data = LinkData::Random;
    if (!recipeError(r).empty())
    {
        point.cancelled = true;
        return point;
    }
    const double sps = r.sampleRateHz / r.symbolRate;
    const int bps = dsp::bitsPerSymbol(r.modulation);
    // Pieces of a quarter of the largest record, 512 to 65 536 symbols.
    const auto chunk = std::clamp<std::size_t>(static_cast<std::size_t>(static_cast<double>(kMaximumLinkSamples / 4) / sps), 512, 65536);
    const auto samples = static_cast<std::size_t>(std::ceil(static_cast<double>(chunk) * sps - 1e-9));
    const std::size_t skip = edgeSymbols(r);
    // How many bits, decided before counting: enough for about targetErrors at the rate of the theory, within
    // maxBits. Stopping at the targetErrors-th error would bias the rate upwards (stopped early on a run of
    // errors, later on a quiet stretch): about +8 % at 400 errors in pieces of 5 000 bits.
    const double perPiece = static_cast<double>(chunk * static_cast<std::size_t>(bps));
    const double wanted = std::clamp(point.theory > 0.0 && std::isfinite(point.theory)
                                         ? static_cast<double>(sweep.targetErrors) / point.theory
                                         : static_cast<double>(sweep.maxBits),
                                     std::min(perPiece, static_cast<double>(sweep.maxBits)), static_cast<double>(sweep.maxBits));
    for (std::uint64_t piece = 0; static_cast<double>(point.bits) < wanted; ++piece)
    {
        if (progress.stopped())
        {
            point.cancelled = true;
            break;
        }
        std::vector<std::uint8_t> bits = dsp::randomBits(chunk * static_cast<std::size_t>(bps), mix(seed, 2 * piece));
        const Chain c = runChain(r, bits, samples, true, ebN0Db, mix(seed, 2 * piece + 1), false);
        if (!c.tx.ok() || !c.rx.ok())
        {
            point.cancelled = true;
            break;
        }
        const std::vector<std::size_t> sent = symbolIndices(c.tx.bits, bps, c.tx.bits.size() / static_cast<std::size_t>(bps));
        std::size_t symbols = 0, symbolErrors = 0, counted = 0, errors = 0;
        count(r, sent, c.rx.symbols, skip, symbols, symbolErrors, counted, errors);
        point.bits += counted;
        point.errors += errors;
        progress.report(static_cast<std::uint64_t>(std::min(1.0, static_cast<double>(point.bits) / wanted) * 1000.0), 1000);
    }
    point.ber = point.bits > 0 ? static_cast<double>(point.errors) / static_cast<double>(point.bits) : 0.0;
    binomialInterval(point.errors, point.bits, point.lower, point.upper);
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
    if (r.noise)
    {
        lines.push_back("Bruit blanc : Eb/N0 demandé " + decibels(r.ebN0Db) + ", mesuré sur le bruit tiré " +
                        decibels(res.measuredEbN0Db) + ".");
        double lower = 0.0, upper = 0.0;
        binomialInterval(res.bitErrors, res.comparedBits, lower, upper);
        std::string ber = "TEB mesuré " + rateText(res.ber) + " (" + grouped(res.bitErrors) +
                          (res.bitErrors > 1 ? " erreurs sur " : " erreur sur ") + grouped(res.comparedBits) + " bits";
        ber += res.bitErrors == 0 ? ", moins de " + rateText(upper) + " à 95 %)" : ", entre " + rateText(lower) + " et " + rateText(upper) + " à 95 %)";
        ber += " ; théorie " + rateText(res.theoryBer) + ".";
        lines.push_back(ber);
        lines.push_back("TES (symboles) mesuré " + rateText(res.ser) + " ; théorie " + rateText(res.theorySer) + ".");
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
    if (res.skipSymbols > 0)
        lines.push_back("Les " + std::to_string(res.skipSymbols) + " premiers et derniers symboles ne comptent pas : la racine de "
                        "cosinus surélevé y est coupée par les bords de l'enregistrement.");
    if (r.modulation == dsp::Modulation::Fsk)
        lines.push_back("FSK non cohérente : chaque symbole décidé par l'énergie de ses deux tonalités (sans la phase).");
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
        add("FSK comme un modem Bell 202 (1 200 Bd, 1 200 et 2 200 Hz)",
            "Deux tonalités 1 000 Hz l'une de l'autre à 1 200 Bd : pas orthogonales, plus d'erreurs que la théorie (le prix d'un modem simple)",
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
        return list;
    }();
    return examples;
}
} // namespace qacq::analysis
