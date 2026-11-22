#pragma once

// The module "Modulations": a digital link simulated end to end, as a recipe. Bits (drawn at random, a text, or bits
// typed) → modulator (ASK, FSK, BPSK, QPSK, 8-PSK, 16-, 64- or 256-QAM; rectangular or root-raised-cosine
// pulses; on a carrier, or as I/Q) → channel (white Gaussian noise at Eb/N0) → coherent receiver (matched filter,
// the nearest point of the constellation; FSK by the energies of its tones) → the bits again, counted against
// those sent and against the theory (exact, Gray mapping). Then the bit error rate along Eb/N0, measured point by
// point on the same chain, with its 95 % interval.
// Pure C++20, no Qt.

#include "qacq/dsp/modulation.h"
#include "qacq/dsp/receiver.h"
#include "qacq/model/json.h"
#include "qacq/model/progress.h"
#include "qacq/model/signal.h"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace qacq::analysis
{
// What is sent.
enum class LinkData
{
    Random, // bits drawn at random (dataSeed)
    Text,   // the bytes of a text (UTF-8), most significant bit first, or framed as on a serial line (8N1)
    Bits    // bits typed ("1011 0010"), repeated
};

struct LinkRecipe
{
    dsp::Modulation modulation = dsp::Modulation::Qpsk;
    // On a carrier (a real signal, what a sound card plays), or as I/Q (the complex envelope).
    dsp::Representation representation = dsp::Representation::RealPassband;
    double sampleRateHz = 48000.0;
    double symbolRate = 1000.0;  // bauds
    double carrierHz = 6000.0;
    double amplitude = 1.0;      // of the carrier (V)
    double carrierPhaseDegrees = 0.0;
    dsp::PulseShape pulse = dsp::PulseShape::RootRaisedCosine; // FSK: always rectangular (continuous phase)
    double rolloff = 0.35;
    double askLowLevel = 0.0;    // ASK: the level of a 0, times that of a 1 (0: on-off)
    double fskDeviationHz = 500.0; // FSK: the tones at fc ± Δf (orthogonal when 2 Δf is a multiple of Rs)
    LinkData data = LinkData::Random;
    std::uint64_t dataSeed = 1;
    std::string text = "Bonjour ! ";
    bool uart = false;           // the text framed as a serial line sends it: start 0, 8 bits LSB first, stop 1
    std::string bits = "1011 0010";
    std::size_t symbols = 2000;  // length of the record
    bool noise = true;
    double ebN0Db = 10.0;
    std::uint64_t noiseSeed = 2;
};

inline constexpr std::size_t kMinimumLinkSymbols = 16;
// At most this many samples in a record (the preview, and each piece of a curve).
inline constexpr std::size_t kMaximumLinkSamples = std::size_t{1} << 20;
inline constexpr std::size_t kMaximumLinkTextBytes = 4096;

struct LinkResult
{
    LinkRecipe recipe;
    std::string error;
    std::vector<std::string> warnings;
    double samplesPerSymbol = 0.0;
    double bitRate = 0.0;
    double occupiedBandwidthHz = 0.0;
    // Sent and received (noise added): on a carrier, the real signal; always the complex envelope (I, Q).
    std::vector<double> sentPassband, sentI, sentQ;
    std::vector<double> receivedPassband, receivedI, receivedQ;
    std::vector<std::uint8_t> sentBits;
    std::vector<std::size_t> sentSymbols; // index in the constellation (FSK: the bit)
    dsp::DigitalReception reception;      // decision points, symbols and bits decided, matched-filter output
    std::vector<std::complex<double>> constellation; // its points at unit amplitude (empty for FSK)
    // Symbols left out of the count at each end (the root raised cosine cut by the ends of the record).
    std::size_t skipSymbols = 0;
    std::size_t comparedBits = 0, bitErrors = 0, comparedSymbols = 0, symbolErrors = 0;
    double ber = 0.0, ser = 0.0;
    double theoryBer = 0.0, theorySer = 0.0; // at the Eb/N0 asked (NaN without noise)
    double measuredEbN0Db = 0.0;             // of the noise drawn (NaN without noise)
    double evmPercent = 0.0, merDb = 0.0;    // decision points against the points sent (NaN for FSK)
    // A text sent: as sent, and as received (a wrong byte shown as « � »); the first characters.
    std::string sentText, receivedText;
    std::size_t textErrors = 0;              // characters received wrong among those shown

    bool ok() const noexcept { return error.empty(); }
};

// The link once: modulated, noise added, received, counted.
LinkResult simulateLink(const LinkRecipe &recipe, const model::Progress &progress = {});

// The bit error rate along Eb/N0: at each point, pieces of the chain (random bits, the recipe's modulation, pulses,
// carrier and rate) over as many bits as it takes to count about targetErrors at the rate of the theory, at most
// maxBits (decided before counting: stopping at the targetErrors-th error would bias the rate upwards).
struct BerSweep
{
    double fromDb = 0.0;
    double toDb = 12.0;
    double stepDb = 1.0;
    std::size_t maxBits = 200000;
    std::size_t targetErrors = 200;
};
inline constexpr std::size_t kMaximumBerPoints = 121;
inline constexpr std::size_t kMaximumBerBits = 100'000'000;

struct BerPoint
{
    double ebN0Db = 0.0;
    std::size_t bits = 0, errors = 0;
    double ber = 0.0;
    double lower = 0.0, upper = 0.0; // 95 % (Wilson); no error: [0, about 3.8 / bits]
    double theory = 0.0;
    bool cancelled = false;
};
// The Eb/N0 of the curve (from, from + step… up to to, at most kMaximumBerPoints); empty and `error` if wrong.
std::vector<double> berSweepValues(const BerSweep &sweep, std::string *error = nullptr);
BerPoint measureBer(const LinkRecipe &recipe, double ebN0Db, const BerSweep &sweep, std::uint64_t seed,
                    const model::Progress &progress = {});
// The 95 % interval of a rate measured as `errors` out of `trials` (Wilson).
void binomialInterval(std::size_t errors, std::size_t trials, double &lower, double &upper);

// "QPSK 1 kBd, racine de cosinus surélevé α = 0,35, porteuse 6 kHz" (I/Q: "en I/Q").
std::string linkText(const LinkRecipe &recipe);
// Short, for the name of a signal: "QPSK 1 kBd".
std::string linkShortText(const LinkRecipe &recipe);
// Lines on the result: rates, bandwidth, what is measured against the theory.
std::vector<std::string> linkSummary(const LinkResult &result);
// A rate as read: "1,2 × 10⁻⁴", "0,032", "0".
std::string rateText(double rate);

// The record sent or received as a signal of the session: on a carrier one channel (V), as I/Q two ("I", "Q");
// named "QPSK 1 kBd (émis)" or "(reçu, Eb/N0 = 10 dB)", the recipe in its notes and provenance
// {"operation": "modulation", "parameters": the recipe, "received"}.
model::Signal linkSignal(const LinkResult &result, bool received);

// Keys: "ask", "fsk", "bpsk", "qpsk", "8psk", "16qam", "64qam", "256qam"; "rect", "rrc"; "random", "text",
// "bits"; "passband", "iq".
std::string_view modulationKey(dsp::Modulation modulation) noexcept;
std::string_view pulseKey(dsp::PulseShape pulse) noexcept;
std::string_view linkDataKey(LinkData data) noexcept;
std::string_view representationKey(dsp::Representation representation) noexcept;
model::Json toJson(const LinkRecipe &recipe);
// Values out of bounds are brought back within them; unknown keys keep the default.
LinkRecipe linkRecipeFromJson(const model::Json &json);
LinkRecipe defaultLinkRecipe();

struct LinkExample
{
    std::string name;
    std::string tip;
    LinkRecipe recipe;
};
const std::vector<LinkExample> &linkExamples();
} // namespace qacq::analysis
