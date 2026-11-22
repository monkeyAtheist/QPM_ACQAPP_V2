#pragma once

// Applying filters to records.
//
// One engine for every module (the old application had three FIR paths and an arbitrary refusal above
// 80 million operations): IIR filters run as second-order sections, FIR filters switch from direct
// convolution to FFT overlap-add as soon as that is faster, with no size limit.

#include "qacq/dsp/filter_design.h"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace qacq::dsp
{
// State of a cascade (two values per section, transposed direct form II).
using SosState = std::vector<std::array<double, 2>>;

// Causal filtering. When state is given it is used as the initial state and updated, so a stream can
// be filtered block by block with the same result as in one piece.
std::vector<double> sosFilter(const SosCascade &sections, std::span<const double> x, SosState *state = nullptr);

// Steady-state initial conditions for a unit step (scipy.signal.sosfilt_zi): multiply by x[0] to start
// a record without a transient.
SosState sosStepState(const SosCascade &sections);

// How a record is prolonged beyond its ends before zero-phase filtering (forward, then backward), so that the
// filter starts and ends as if the signal went on. The record's ends start a transient otherwise: a filter of
// high order or narrow band rings there for as long as its impulse response lasts.
enum class EdgeExtension
{
    // Linear prediction from the samples near each end (predictLinear, order 96, on 4 096 samples at most):
    // the signal goes on as it went, a sum of sinusoids exactly, a record at rest at rest; prolonged for as long
    // as the filter rings (its slowest pole down to 1e-6), more than the record if need be.
    Predicted,
    Odd,      // 2·x[0] − x[k], SciPy's default: keeps a trend; a signal that starts on a crest jumps there
    Even,     // x[k], the mirror: keeps a crest; a signal that starts with a slope turns back there
    Constant, // x[0] repeated
    None      // no extension: the filter starts in the steady state of x[0] (SciPy's padtype=None)
};

// Zero-phase forward-backward filtering, as scipy.signal.sosfiltfilt (Odd, Even, Constant, None: the same
// results, padtype "odd", "even", "constant", None). padLength = -1 picks the default: SciPy's (3 × (2 ×
// sections + 1 − trivial terms)), reduced when the record is too short; with Predicted, at least as long as
// the filter rings (sosRingLength), up to 2^20 samples.
std::vector<double> sosFiltFilt(const SosCascade &sections, std::span<const double> x, long padLength = -1,
                                EdgeExtension extension = EdgeExtension::Odd);

// The same for a FIR (scipy.signal.filtfilt(taps, 1), padlen 3·N; Predicted: N at least).
std::vector<double> firFiltFilt(std::span<const double> taps, std::span<const double> x, long padLength = -1,
                                EdgeExtension extension = EdgeExtension::Odd);

// How many samples sosFiltFilt and firFiltFilt prolong a record of n samples by at each end, by default
// (padLength = -1): SciPy's for Odd, Even and Constant (3 × the length of the filter, less than the record),
// none for None; for Predicted, as long as the filter rings (to 1e-6; a FIR: its length).
std::size_t sosFiltFiltPad(const SosCascade &sections, std::size_t n, EdgeExtension extension);
std::size_t firFiltFiltPad(std::size_t taps, std::size_t n, EdgeExtension extension);

// The record prolonged by `pad` samples at each end (None: as it is).
std::vector<double> extendEdges(std::span<const double> x, std::size_t pad, EdgeExtension extension);

// The `count` samples that would follow `history` (oldest first), by linear prediction of order `order`
// (at most a third of the history): least squares, exact for a sum of sinusoids, over the forward and backward
// predictions for a steady history (the modified covariance method), over the forward ones for a history that
// dies out or grows (the covariance method: a ring that fades goes on fading, down to exactly zero); should
// that model grow (a root outside the unit circle, or beyond four times the history's swing), the other one,
// then Burg's, stable by construction (then damped); at worst the last sample repeated. A record at rest near
// its end goes on at rest.
std::vector<double> predictLinear(std::span<const double> history, std::size_t count, int order = 96);

// How many samples the impulse response of the cascade takes to fall to `tolerance`, from its slowest pole
// (r^n = tolerance); `maximum` for a pole on or outside the unit circle.
std::size_t sosRingLength(const SosCascade &sections, double tolerance = 1e-6, std::size_t maximum = 1u << 20);

// Transfer-function form, for coefficients typed by the user (scipy.signal.lfilter). Prefer sections
// for designed filters: this form loses accuracy for high orders and low cut-off frequencies.
std::vector<double> lfilter(std::span<const double> b, std::span<const double> a, std::span<const double> x);

// Full linear convolution (length a + b - 1); direct or FFT overlap-add, whichever is cheaper.
std::vector<double> convolve(std::span<const double> a, std::span<const double> b);

// Causal FIR filtering, output as long as x (y[n] = sum_k h[k] x[n - k], x = 0 before the record).
std::vector<double> firFilter(std::span<const double> taps, std::span<const double> x);

// FIR filtering with the group delay of a symmetric filter removed: output as long as x and aligned
// with it (an offline operation; the samples beyond the record are taken as 0).
std::vector<double> firFilterAligned(std::span<const double> taps, std::span<const double> x);
} // namespace qacq::dsp
