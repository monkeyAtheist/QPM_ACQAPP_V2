#pragma once
// Measuring tools of the plots and of the analysis (module "Signaux"), without Qt.
//
//   spanStatistics()  min, max, mean, rms... of a part of a channel (between cursors A and B)
//   waveMeasures()    what an oscilloscope measures on its own: top and base levels, period, frequency,
//                     duty cycle, rise and fall times (10 % to 90 % or 20 % to 80 %), overshoot and
//                     undershoot, mean and rms over whole periods, settling after the last edge
//   risingCrossings() times of the rising edges (middle crossings), as waveMeasures finds them
//   findPeaks()       the highest peaks of a spectrum curve, one per lobe, frequency interpolated
//   bandMeanSquare()  power of a band of a spectrum (between cursors A and B)
//   halfPowerBand()   the band within 3 dB of the highest bin of a curve
#include "qacq/analysis/analysis.h"

#include <cstddef>
#include <span>
#include <vector>

namespace qacq::analysis
{
SpanStatistics spanStatistics(std::span<const double> values);

// edgeLowFraction: 0.1 for 10 % → 90 %, 0.2 for 20 % → 80 % (clamped to [0.01, 0.4]).
WaveMeasures waveMeasures(std::span<const double> x, double sampleRateHz, double edgeLowFraction = 0.1);

// Seconds from the first sample; empty when the record is flat, too short or has holes.
std::vector<double> risingCrossings(std::span<const double> x, double sampleRateHz);

// The `count` highest local maxima of a curve, at least one main lobe apart, the DC lobe left out when the
// mean is removed. Highest first.
std::vector<Peak> findPeaks(const AnalysisResult &result, std::size_t curve, std::size_t count,
                            const AnalysisSettings &settings);

// Mean square (unit²) of the frequencies [f0, f1] of a curve: the sum of the density times the bin width
// (amplitude spectrum: A² / (2 ENBW) per bin, two-sided: A² / ENBW). Its square root is the rms value of
// what lies in the band.
double bandMeanSquare(const AnalysisResult &result, std::size_t curve, double f0, double f1);

HalfPowerBand halfPowerBand(const AnalysisResult &result, std::size_t curve);
} // namespace qacq::analysis
