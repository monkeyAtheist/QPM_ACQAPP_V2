#pragma once

// The module "Modulations": a digital link simulated end to end, as a recipe. Bits (drawn at random, a text, or bits
// typed) → modulator (ASK, FSK, BPSK, QPSK, 8-PSK, 16-, 64- or 256-QAM; rectangular or root-raised-cosine
// pulses; on a carrier, or as I/Q) → channel (several paths, the carrier off in frequency and phase, a delay, the
// receiver's clock at another rate: dsp::applyChannel; then white Gaussian noise at Eb/N0) → receiver → the bits
// again, counted against those sent and against the theory (exact, Gray mapping). The receiver is ideal (it knows
// the channel: its delay, clock and carrier undone exactly, on the strongest path, then the coherent receiver:
// matched filter, the nearest point; FSK by the energies of its tones) or synchronized (dsp::receiveSynchronized:
// a preamble of known symbols at the start of the frame, then its timing and carrier loops). After either, an
// equalizer (dsp/equalizer.h) can repair what several paths did to the symbols: computed from the channel for the
// ideal receiver (which knows it), learnt on the preamble then on its own decisions for the synchronized one.
// Then the bit error rate along Eb/N0, measured point by point on the same chain, with its 95 % interval.
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
// The receiver.
enum class LinkReceiver
{
    Ideal,       // it knows the channel (delay, clock, carrier) and undoes it exactly
    Synchronized // it finds them: a preamble, then its loops
};

// What repairs, after the receiver, the symbols that several paths spread over their neighbours.
enum class LinkEqualizer
{
    None,
    Linear,          // taps once per symbol: computed (ideal receiver) or learnt (synchronized receiver)
    DecisionFeedback // the same, then taps on the symbols already decided (they take off what those leave)
};
inline constexpr std::size_t kMaximumLinkFeedbackTaps = 32;

// The paths fading during the transmission (dsp::ChannelFading): Rayleigh (no line of sight), Rice (a line of sight
// on the first path).
enum class LinkFading
{
    None,
    Rayleigh,
    Rice
};
inline constexpr double kMaximumDopplerPerSymbol = 0.25; // fd T at most (the channel changing within a few symbols)
// From this fd T on, the gain changes within a pulse: the matched filter averages the shortest fades, and the
// error rate measured falls below the theory (the gain constant over each symbol); said in the summary.
inline constexpr double kNoticedDopplerPerSymbol = 0.02;
inline constexpr double kMinimumRiceKDb = -20.0, kMaximumRiceKDb = 40.0;

// One path of the channel: when it arrives after the first one (symbols), its gain against the others (dB), its
// phase (degrees).
struct LinkPath
{
    double delaySymbols = 0.0;
    double gainDb = 0.0;
    double phaseDegrees = 0.0;

    bool operator==(const LinkPath &) const = default;
};
inline constexpr std::size_t kMaximumLinkPaths = 8;
inline constexpr double kMaximumPathDelaySymbols = 32.0;
inline constexpr std::size_t kMinimumEqualizerTaps = 3;
inline constexpr std::size_t kMaximumEqualizerTaps = 129;

// Paths as typed: "0 : 0 ; 1 : -6 ; 2,5 : -12 @ 90" (delay in symbols : gain in dB @ phase in degrees; ";" or a new
// line between two; decimal comma or point; the units may be written). Nothing typed: no path listed. A path
// that cannot be read: none returned and `error` says which.
std::vector<LinkPath> parseLinkPaths(std::string_view text, std::string *error = nullptr);
// "0 : 0 ; 1 : −6 ; 2,5 : −12 @ 90" (as parseLinkPaths reads it back).
std::string linkPathsText(const std::vector<LinkPath> &paths);

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
    // The channel, what the receiver does not know (dsp::ChannelSpec).
    double frequencyOffsetHz = 0.0;  // the carrier received this far off (oscillators, Doppler)
    double phaseOffsetDegrees = 0.0; // and turned by this much
    double delaySymbols = 0.0;       // the frame arrives this late (its fraction of a symbol: the timing to find)
    double clockOffsetPpm = 0.0;     // the receiver's clock this much fast: its symbols drift
    // Several paths, as typed (parseLinkPaths); empty, or a single path neither late nor turned: one path, as
    // sent. The gains count against each other: the powers of the paths add up to that of the transmission (Eb/N0
    // stays that of what is received, on average when they fade).
    std::string paths;
    // Fading: each path (or the one) changes during the transmission at the pace of the Doppler shift fd (over about
    // 1 / fd), Rice with a line of sight on the first path K times the power scattered around it. The ideal receiver
    // knows the strongest path at every instant (its gain and phase undone, as a coherent receiver that follows it
    // perfectly); the synchronized one follows with its loops (and its equalizer, when it learns).
    LinkFading fading = LinkFading::None;
    double dopplerHz = 10.0;
    double riceKDb = 6.0;
    std::uint64_t fadingSeed = 3;
    LinkReceiver receiver = LinkReceiver::Ideal;
    std::size_t preambleSymbols = 64; // known symbols at the start of the frame (receiver Synchronized)
    double timingBandwidth = 0.001;   // B_L T of its loops (after the wide first pass)
    double phaseBandwidth = 0.003;
    LinkEqualizer equalizer = LinkEqualizer::None;
    std::size_t equalizerTaps = 11;   // odd, kMinimumEqualizerTaps to kMaximumEqualizerTaps
    std::size_t feedbackTaps = 4;     // decision feedback: 1 to kMaximumLinkFeedbackTaps
    double equalizerStep = 0.05;      // of the normalized LMS (synchronized receiver), 0 to 1
};

inline constexpr std::size_t kMinimumLinkSymbols = 16;
// At most this many samples in a record (the preview, and each piece of a curve).
inline constexpr std::size_t kMaximumLinkSamples = std::size_t{1} << 20;
inline constexpr std::size_t kMaximumLinkTextBytes = 4096;
inline constexpr std::size_t kMinimumPreambleSymbols = 8;
inline constexpr std::size_t kMaximumPreambleSymbols = 1024;
inline constexpr double kMaximumDelaySymbols = 1000.0;
inline constexpr double kMaximumClockPpm = 10000.0;

// What the synchronized receiver found, against the truth (the simulation knows it).
struct LinkSync
{
    bool used = false;
    bool frameFound = false;
    double frameQuality = 0.0;        // correlation of the preamble at its peak, 0 to 1
    double delaySymbols = 0.0, trueDelaySymbols = 0.0; // where the frame starts
    double frequencyOffsetHz = 0.0, trueFrequencyOffsetHz = 0.0;
    double lineOfSightDopplerHz = 0.0; // Rice: the line of sight's Doppler shift, on top of the true offset (it turns as one)
    // The carrier phase at the start (transmitter's and channel's; fading: with the strongest path's gain at the
    // first symbol).
    double phaseDegrees = 0.0, truePhaseDegrees = 0.0;
    double clockOffsetPpm = 0.0, trueClockOffsetPpm = 0.0;
    double gain = 0.0;
    // Each symbol decided: its instant less the true one (symbols), its carrier phase less the true one (degrees,
    // within +-180).
    std::vector<double> timingError, phaseError;
    double timingErrorRms = 0.0, phaseErrorRms = 0.0; // over the symbols counted
    // The timing loop lost its lock: its instants ended more than half a symbol from the true ones.
    bool timingLost = false;
    std::size_t phaseSlips = 0; // the carrier loop jumping to another symmetry of the constellation
    // Before synchronization: the filter output at the frame start + k T, neither turned back nor following the
    // clock (unit scale).
    std::vector<std::complex<double>> rawPoints;
};

// Several paths as the decisions see them, and what the equalizer makes of them.
struct LinkEqualization
{
    bool paths = false;           // the channel has several paths (or one late or turned)
    std::vector<LinkPath> typed;  // as typed
    std::vector<double> amplitude; // of each, once their powers add up to 1
    std::size_t mainPath = 0;     // the strongest: the ideal receiver decides on it
    // The response at the decision instants of the receiver, measured on the chain without noise: response[m]
    // is c[responseFirst + m], c[0] the symbol itself, brought to 1 as the receiver's gain and phase bring it;
    // empty when it has none (FSK). The ideal receiver decides on the strongest path; the synchronized one where
    // its timing loop settled (between two paths a fraction of a symbol apart), `instantSymbols` after that path;
    // with an equalizer that took its points half a symbol earlier, from there. `symbolGain`: what the symbol
    // itself is at those instants against the strongest path alone (1 when the paths are whole symbols apart: a
    // path a fraction of a symbol away adds to it or takes from it).
    std::vector<std::complex<double>> response;
    int responseFirst = 0;
    double instantSymbols = 0.0; // said when at least kShownDecisionOffsetSymbols
    double symbolGain = 1.0;
    // What a decision sees without equalizer: the symbol against the other symbols and the noise (dB; NaN when
    // unknown).
    double sinrBeforeDb = 0.0;
    bool used = false;     // an equalizer ran
    bool adaptive = false; // learnt (synchronized receiver) rather than computed
    // Learnt: it found its symbols better half a symbol before the instants of the timing loop (which settles
    // between two paths of like strength), and took its points there.
    bool halfSymbolEarly = false;
    std::vector<std::complex<double>> taps; // taps[m] is w[tapsFirst + m] (learnt: as they are at the end)
    int tapsFirst = 0;
    std::vector<std::complex<double>> feedback; // decision feedback: b[1 ...] (computed: q[1 ...]; learnt: at the end)
    // taps * response: what the symbols see after it, about a single 1 (learnt: to the scale and the phase of the
    // receiver; empty when the response is unknown); decision feedback: less what it takes off (q[1 ...] - b). Computed: the ratio a decision then sees, per bit (an Eb/N0),
    // and the bit error rate of the theory there, the other symbols left counted as noise.
    std::vector<std::complex<double>> combined;
    int combinedFirst = 0;
    double sinrAfterDb = 0.0, equivalentEbN0Db = 0.0, predictedBer = 0.0; // NaN when not computed
    std::vector<double> squaredError;               // learnt: |error|^2 against the energy of a symbol, symbol by symbol
    double settledErrorDb = 0.0;                    // learnt: its mean over the last quarter of the frame (dB); NaN otherwise
    std::vector<std::complex<double>> pointsBefore; // the decision points before the equalizer
};

// Decisions closer than this to the strongest path (symbols) are taken on it: what is left is the jitter of the timing
// loop (the mean of its error over the frame), not said.
inline constexpr double kShownDecisionOffsetSymbols = 0.02;

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
    // Symbols left out of the count at the start (the root raised cosine cut by the record; the preamble) and at
    // the end.
    std::size_t skipSymbols = 0;
    std::size_t skipEndSymbols = 0;
    LinkSync sync;
    LinkEqualization equalization;
    std::size_t comparedBits = 0, bitErrors = 0, comparedSymbols = 0, symbolErrors = 0;
    double ber = 0.0, ser = 0.0;
    // The 95 % interval of the bit error rate: Wilson's; through fading, widened to what the errors of blocks of
    // berBatchSymbols symbols (five periods of fd) say, when there are at least two (berBatches: their number; 0
    // otherwise): a fade takes several symbols at once, so that the errors come in bursts, which the binomial
    // interval, counting each bit apart, does not see.
    double berLower = 0.0, berUpper = 1.0;
    std::size_t berBatches = 0, berBatchSymbols = 0;
    double theoryBer = 0.0, theorySer = 0.0; // at the Eb/N0 asked (NaN without noise); fading: over its distribution
                                             // (a single path; NaN with several)
    double awgnTheoryBer = 0.0;              // the same without fading (NaN without noise)
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
// maxBits (decided before counting: stopping at the targetErrors-th error would bias the rate upwards). Through
// fading, at least kFadingPeriodsPerPoint periods of fd (50 pieces, each its own still gains, when fd = 0): its
// errors come in bursts, a few per fade, and it is the number of fades that makes the rate.
inline constexpr double kFadingPeriodsPerPoint = 1000.0;
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
    double lower = 0.0, upper = 0.0; // 95 % (Wilson); no error: [0, about 3.8 / bits]; fading: widened by blocks
    std::size_t batches = 0;         // those blocks (0: Wilson's alone; see LinkResult::berLower)
    double theory = 0.0;
    bool cancelled = false;
};
// The Eb/N0 of the curve (from, from + step… up to to, at most kMaximumBerPoints); empty and `error` if wrong.
std::vector<double> berSweepValues(const BerSweep &sweep, std::string *error = nullptr);
BerPoint measureBer(const LinkRecipe &recipe, double ebN0Db, const BerSweep &sweep, std::uint64_t seed,
                    const model::Progress &progress = {});
// The 95 % interval of a rate measured as `errors` out of `trials` (Wilson).
void binomialInterval(std::size_t errors, std::size_t trials, double &lower, double &upper);

// "QPSK 1 kBd, racine de cosinus surélevé α = 0,35, porteuse 6 kHz" (I/Q: "en I/Q"; ", 2 trajets"; ", synchronisée";
// ", égalisée").
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
// "bits"; "passband", "iq"; "ideal", "sync".
std::string_view modulationKey(dsp::Modulation modulation) noexcept;
std::string_view pulseKey(dsp::PulseShape pulse) noexcept;
std::string_view linkDataKey(LinkData data) noexcept;
std::string_view representationKey(dsp::Representation representation) noexcept;
std::string_view receiverKey(LinkReceiver receiver) noexcept; // "ideal", "sync"
std::string_view equalizerKey(LinkEqualizer equalizer) noexcept; // "none", "linear", "dfe"
std::string_view fadingKey(LinkFading fading) noexcept;          // "none", "rayleigh", "rice"
// The rates of the theory for the recipe at an Eb/N0: without fading, or averaged over a flat fading (one path);
// NaN when the theory does not say (several fading paths, an analog modulation).
double linkTheoryBer(const LinkRecipe &recipe, double ebN0Db) noexcept;
double linkTheorySer(const LinkRecipe &recipe, double ebN0Db) noexcept;
// Whether the recipe puts anything between transmitter and receiver but noise.
bool linkHasChannel(const LinkRecipe &recipe) noexcept;
// Whether its channel has several paths (or a single one late or turned).
bool linkHasPaths(const LinkRecipe &recipe) noexcept;
// The fewest symbols the recipe allows: kMinimumLinkSymbols, its preamble and 16 data symbols when the receiver
// is synchronized, its equalizer's taps and 16 more when it equalizes (what a short check of the recipe needs).
std::size_t minimumLinkSymbols(const LinkRecipe &recipe) noexcept;
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
