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

// Zero-phase forward-backward filtering, same as scipy.signal.sosfiltfilt with odd extension.
// padLength = -1 picks SciPy's default (3 * (2 * sections + 1 - trivial terms)); it is reduced when the
// record is too short.
std::vector<double> sosFiltFilt(const SosCascade &sections, std::span<const double> x, long padLength = -1);

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
