#pragma once

// Symbol-spaced linear equalizers: what repairs, after the matched filter, the symbols that the paths of a
// channel spread over their neighbours.
//
// At its decision instants a receiver holds x[k] = sum_j c[j] s[k - j] + n[k]: the symbols s through the response
// c of the whole chain sampled once per symbol (pulse, paths, matched filter; without echo c = delta), and a
// noise n, white at that rate, of variance noiseVariance (E|n|^2). A linear equalizer of 2 half + 1 taps:
//   z[k] = sum_i w[i] x[k - i],   i = -half ... half
// (the record is there: taps on both sides, no delay to count), so that the symbols see q = w * c instead of c.
//
//   designLinearEqualizer  the taps that leave the least mean square error E|z[k] - s[k]|^2 (Wiener: the response
//                          and the noise known), then scaled so that q[0] = 1 (no bias: the points on the
//                          constellation). Without noise it inverts the response as far as its length allows
//                          (zero forcing). What is left: the other symbols (sum over m != 0 of |q[m]|^2) and the
//                          noise the taps let through (noiseVariance sum |w|^2), both against the energy of a
//                          symbol; one over their sum is the ratio a decision sees (sinr). Counted as noise, the
//                          symbols left give an equivalent Eb/N0 and, through the theory, a bit error rate: close
//                          when many small terms are left, pessimistic or optimistic by a little otherwise.
//                          A constellation on the real axis (BPSK, ASK) is decided on that axis alone: half the
//                          noise counts, and of the other symbols what falls on it (the real parts of q); the
//                          ratio is then given as that of a noise spread over both axes that would disturb the
//                          decision as much (twice what lies on the real axis), so that the theory applies as is.
//   designDecisionFeedbackEqualizer
//                          the same forward taps, then feedback taps b[1 ... feedback] on the symbols already
//                          decided: z[k] = sum_i w[i] x[k - i] - sum_j b[j] d[k - j]. With right decisions, they take
//                          off what the symbols before leave after the forward taps (b[j] = q[j]): those taps only
//                          fight the symbols after (and before, beyond the feedback) and the noise, and let much less
//                          noise through than a linear equalizer where the response has a deep notch. Designed for
//                          the least mean square error with right decisions (MMSE-DFE); a wrong decision feeds a
//                          wrong symbol back, which may make the next ones wrong (errors in bursts).
//   withoutEqualizer       the same account for the response as it is (one tap, 1 / c[0]).
//   adaptEqualizer         the same taps learnt without knowing the response: over the known symbols of a
//                          preamble first (the taps that explain them best, by least squares), then moved symbol
//                          after symbol by the normalized LMS, led by its own decisions.
//   fitSymbolResponse      the response c itself, by least squares, from the points a receiver holds and the
//                          symbols that were sent.

#include "qacq/dsp/modulation.h"

#include <complex>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace qacq::dsp
{
// Taps about index 0: values[m] is the tap of index first + m.
struct SymbolTaps
{
    std::vector<std::complex<double>> values;
    int first = 0;

    int last() const noexcept { return first + static_cast<int>(values.size()) - 1; }
    std::complex<double> at(int index) const noexcept
    {
        return index < first || index > last() ? std::complex<double>{} : values[static_cast<std::size_t>(index - first)];
    }
};

// Symbols drawn evenly from a constellation: E|s|^2 and E[s] (0 for PSK and QAM, not for an on-off ASK), and
// whether all its points lie on the real axis.
struct SymbolMoments
{
    double energy = 1.0;
    std::complex<double> mean{};
    bool realAxis = false;
};
SymbolMoments symbolMoments(std::span<const std::complex<double>> constellation) noexcept;

// The response c[first ... last] for which points[k] is closest to sum_j c[j] symbols[k - j], over k = from ...
// to - 1 (symbols outside their record count as 0). Empty when the symbols cannot tell (too few, or periodic).
SymbolTaps fitSymbolResponse(std::span<const std::complex<double>> points, std::span<const std::complex<double>> symbols,
                             int first, int last, std::size_t from, std::size_t to);

struct LinearEqualizer
{
    SymbolTaps taps;           // w
    std::vector<std::complex<double>> feedback; // b[1 ...] (decision feedback; empty for a linear equalizer)
    SymbolTaps combined;       // q = w * c, q[0] = 1 (decision feedback: q[1 ...] taken off by b)
    double interference = 0.0; // what the other symbols leave, over the energy of a symbol (real axis: twice what lies on it)
    double noise = 0.0;        // what the noise leaves, idem
    double sinr = 0.0;         // 1 / (interference + noise); infinite when nothing is left
    std::string error;

    bool ok() const noexcept { return error.empty(); }
};
inline constexpr int kMaximumEqualizerHalf = 64;
inline constexpr int kMaximumFeedbackTaps = 32;

LinearEqualizer designLinearEqualizer(const SymbolTaps &response, int half, double noiseVariance, const SymbolMoments &moments);
LinearEqualizer designDecisionFeedbackEqualizer(const SymbolTaps &response, int half, int feedback, double noiseVariance,
                                                const SymbolMoments &moments);
LinearEqualizer withoutEqualizer(const SymbolTaps &response, double noiseVariance, const SymbolMoments &moments);

// z[k] = sum_i taps[i] points[k - i] (points outside the record count as 0).
std::vector<std::complex<double>> applyEqualizer(std::span<const std::complex<double>> points, const SymbolTaps &taps);
// The same, less sum_j feedback[j - 1] d[k - j], d the point of the constellation nearest each output (decided one
// after the other; none before the first).
std::vector<std::complex<double>> applyDecisionFeedback(std::span<const std::complex<double>> points, const SymbolTaps &taps,
                                                        std::span<const std::complex<double>> feedback, Modulation modulation,
                                                        double askLowLevel);
// (a * b)[m] = sum_i a[i] b[m - i].
SymbolTaps convolveTaps(const SymbolTaps &a, const SymbolTaps &b);
// The response of taps at a frequency in cycles per symbol (-0.5 to 0.5): sum_m t[m] exp(-j 2 pi f m).
std::complex<double> tapsResponse(const SymbolTaps &taps, double cyclesPerSymbol) noexcept;

struct AdaptiveEqualizerSettings
{
    int half = 5;       // 2 half + 1 taps
    double step = 0.05; // of the normalized LMS, 0 to 1
    // A carrier loop after the equalizer (second order, noise bandwidth B_L T, damping 0.707; detector
    // Im(z conj(d)) over the mean energy of the constellation), led like the taps by the preamble then by the
    // decisions: z[k] = (sum_i w[i] x[k - i]) exp(-j phi[k]). 0: none. It follows what the points still turn by
    // (a carrier found on the preamble alone is a little off in frequency), where the decisions are good: after
    // the equalizer. The taps are kept from turning with it: their strongest one stays real (what it would turn
    // by goes to the loop).
    double carrierBandwidth = 0.0;
    // The taps and the loop learn from this many points (the symbols of the frame whose neighbours are symbols
    // too); past them they stay as they are, the loop turning on at the rate it found (0: from all the points).
    std::size_t adaptSymbols = 0;
    // Decision feedback: this many taps on the symbols already decided (the preamble's known ones over it), learnt
    // with the others (0: a linear equalizer). They work after the carrier loop, where the decisions are.
    int feedback = 0;
    // Nor do they learn from the points before this one, though they equalize them: the first symbols of a frame
    // whose pulses the start of the transmission cuts (half the span of a root raised cosine: the first point half
    // its size) would lead them astray, the fit over the preamble as the steps after it.
    std::size_t adaptFrom = 0;
};

struct AdaptiveEqualization
{
    std::vector<std::complex<double>> points; // z[k]
    SymbolTaps taps;                          // as they are at the end (after the last point they learnt from)
    std::vector<std::complex<double>> feedback; // b[1 ...], idem (decision feedback)
    // |d - z|^2, symbol by symbol: against the known symbol over the preamble, against the decision afterwards.
    std::vector<double> squaredError;
    std::vector<double> phases; // what the carrier loop took off, symbol by symbol (radians; empty without loop)
    bool halfSymbolEarly = false; // it took the points given half a symbol earlier (they explained the preamble better)
    std::string error;

    bool ok() const noexcept { return error.empty(); }
};

// points: what the receiver holds at its decision instants, from the first symbol of the preamble; training: the
// preamble's symbols (unit scale). earlier (optional, as long as points): the same half a symbol before each
// instant; the equalizer works on whichever of the two explains the preamble better (a symbol-spaced equalizer
// depends on where in the symbol its points are taken).
AdaptiveEqualization adaptEqualizer(std::span<const std::complex<double>> points, std::span<const std::complex<double>> training,
                                    Modulation modulation, double askLowLevel, const AdaptiveEqualizerSettings &settings,
                                    std::span<const std::complex<double>> earlier = {});
} // namespace qacq::dsp
