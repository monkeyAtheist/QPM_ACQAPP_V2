#pragma once
// Curves computed from the channels of a signal, without Qt (the "Courbe calculée" of the interface).
//
//   integral()            area under a part of a channel (trapezoids), unit·s
//   cumulativeIntegral()  the running integral from the first sample (0 there)
//   derivative()          the slope, unit/s (centred differences, one-sided at both ends)
//   perCycle()            mean, rms or frequency of each whole period (rising edge to rising edge),
//                         held over that period; NaN before the first and after the last whole period
//   derive()              a new signal made of these curves, ready for the session
//
// Missing samples (NaN) are left out of an integral (it stays level across them) and give NaN in a
// derivative; a channel with holes has no per-period curve.
#include "qacq/model/signal.h"

#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qacq::analysis
{
double integral(std::span<const double> x, double sampleRateHz) noexcept;

// removeMean: the mean of the part is taken off first, so that a periodic signal with an offset does not
// drift away (the integral of a sine stays a sine).
std::vector<double> cumulativeIntegral(std::span<const double> x, double sampleRateHz, bool removeMean = false);

std::vector<double> derivative(std::span<const double> x, double sampleRateHz);

enum class CycleQuantity
{
    Mean,
    Rms,
    Frequency
};
std::vector<double> perCycle(std::span<const double> x, double sampleRateHz, CycleQuantity quantity);

enum class Operation
{
    Integral,
    Derivative,
    CycleMean,
    CycleRms,
    CycleFrequency
};
std::string_view operationKey(Operation operation) noexcept; // "integral"... (files, interface)
bool operationFromKey(std::string_view key, Operation &operation) noexcept;
// Unit of the result for a channel unit: "V" gives "V·s", "V/s", "V", "V", "Hz"; "m/s" integrates to "m"
// and derives to "m/s²"; no unit gives "s", "1/s", "", "", "Hz".
std::string operationUnit(Operation operation, std::string_view unit);
// "∫ Voie 1", "d(Voie 1)/dt", "Moyenne par période de Voie 1"...
std::string operationChannelName(Operation operation, std::string_view channel);
// "Intégrale", "Dérivée", "Moyenne par période"... (start of the signal name).
std::string operationTitle(Operation operation);

struct DeriveOptions
{
    Operation operation = Operation::Integral;
    std::size_t firstFrame = 0;                                     // part of the signal used
    std::size_t frameCount = std::numeric_limits<std::size_t>::max(); // clipped to the signal
    std::vector<std::size_t> channels; // channels to compute; empty: all of them
    bool keepSource = true;            // the source channels first, then the computed ones
    bool removeMean = false;           // integral only
};

struct Derived
{
    model::Signal signal; // Origin::Derived, id 0, name "Intégrale de « … »" (the caller makes it unique)
    std::string error;    // in French; signal empty then
    bool ok() const noexcept { return error.empty(); }
};

Derived derive(const model::Signal &source, const DeriveOptions &options);
} // namespace qacq::analysis
