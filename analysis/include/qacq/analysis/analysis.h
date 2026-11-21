#pragma once
// The "Analyse" module without Qt: what is measured on a part of a signal, and its spectrum as the
// plot shows it.
//
//   analyse()          one call for a zone of a signal: the spectrum of every channel (or I/Q pair),
//                      in the level scale asked, and the measurements of every channel
//   levelUnit()        how the level axis reads: "dBV", "dB re 1 A", "V/√Hz"...
//   levelRange()       automatic scale of a spectrum (decibels: the top 160 dB at most)
//   writeSpectrumCsv() the spectrum as a table (French: ";" and decimal comma)
//
// Every sample of the zone is used. A single FFT longer than 65 536 points gets zeros up to the next
// power of two (all samples kept, frequencies a little closer together), which keeps ten million
// samples within a few seconds and a few hundred megabytes; the result says so.
#include "qacq/dsp/tone.h"
#include "qacq/dsp/window.h"
#include "qacq/model/json.h"
#include "qacq/model/signal.h"
#include "qacq/plot/trace.h"

#include <atomic>
#include <cstddef>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace qacq::analysis
{
enum class SpectrumKind
{
    Amplitude, // one FFT of the whole zone: peak amplitude of each sinusoid, tone measurements
    Density    // Welch: average of overlapping segments, noise density (unit/√Hz)
};

enum class LevelScale
{
    Decibel, // 20 log10 of the amplitude (density: of unit/√Hz)
    Linear
};

std::string_view spectrumKindKey(SpectrumKind kind) noexcept; // "amplitude", "density" (files)
std::string_view windowKey(dsp::Window window) noexcept;      // "hann"...
bool windowFromKey(std::string_view key, dsp::Window &window) noexcept;

struct AnalysisSettings
{
    SpectrumKind kind = SpectrumKind::Amplitude;
    dsp::Window window = dsp::Window::Hann;
    double kaiserBeta = 8.6;
    std::size_t zeroPadFactor = 1; // amplitude spectrum: 1, 2, 4 or 8 times the zone
    bool removeMean = true;        // the DC part out of the spectrum (it is still measured)
    std::size_t segmentLength = 4096; // Welch
    double overlap = 0.5;             // Welch, fraction of a segment
    LevelScale scale = LevelScale::Decibel;
    bool logFrequency = false;
    std::size_t harmonics = 10; // fundamental included
    double edgeLow = 0.1;       // rise and fall times from 10 % to 90 % (0.2: 20 % to 80 %)
};

model::Json toJson(const AnalysisSettings &settings);
// Missing or wrong members keep their default value (settings saved by another version).
AnalysisSettings analysisSettingsFromJson(const model::Json &json);

// Largest zone for one FFT (amplitude spectrum): 2^24 points after the zeros.
inline constexpr std::size_t kMaximumFftSize = std::size_t{1} << 24;
// From this zone length on, the FFT length is the next power of two.
inline constexpr std::size_t kExactFftUpTo = std::size_t{1} << 16;

struct SpanStatistics
{
    std::size_t samples = 0; // finite samples
    double minimum = 0.0;
    double maximum = 0.0;
    double mean = 0.0;
    double rms = 0.0;
    double acRms = 0.0;
    double peakToPeak = 0.0;
    double first = 0.0; // first and last finite samples
    double last = 0.0;
};
// Times in seconds from the first sample. Levels: "top" and "base" are the most frequent values of the
// upper and lower halves (histogram of 100 classes), as an oscilloscope finds the flat parts of a square
// wave; without a clear one (sine, triangle) the extremes. Edges cross the middle with a hysteresis of
// 10 % of the amplitude; each measure says whether it could be made.
struct Settling
{
    bool reached = false; // inside the band before the end of the zone, and staying there
    double seconds = 0.0; // from the middle crossing of the edge
};
struct WaveMeasures
{
    bool levels = false; // the signal is not flat
    // The level is a flat part (8 % of the samples in its class of the histogram at least: a square wave,
    // a step; a sine puts 6 % in its extreme classes). Settling is measured towards a flat level only.
    bool flatTop = false, flatBase = false;
    double top = 0.0, base = 0.0, amplitude = 0.0;
    double edgeLowFraction = 0.1; // rise and fall measured from this fraction of the amplitude to 1 − it
    std::size_t risingEdges = 0, fallingEdges = 0;
    bool periodic = false; // at least two rising edges
    double periodSeconds = 0.0, frequencyHz = 0.0;
    bool duty = false;
    double dutyCyclePercent = 0.0;
    bool rise = false, fall = false;
    double riseSeconds = 0.0, fallSeconds = 0.0; // 10 % to 90 % of top - base, mean over the edges
    double overshootPercent = 0.0;  // (max - top) / amplitude
    double undershootPercent = 0.0; // (base - min) / amplitude
    // Mean and rms over the whole periods only (first to last rising edge): no bias from a part of a
    // period at either end of the zone.
    bool cycles = false;
    std::size_t cycleCount = 0;
    double cycleMean = 0.0, cycleRms = 0.0;
    // After the last edge of the zone (towards a flat level only), to ±1 % and ±0.1 % of the amplitude
    // around the level it goes to.
    bool settlingMeasured = false;
    bool lastEdgeRising = false;
    Settling settling1, settling01;
};
struct Peak
{
    double frequencyHz = 0.0; // interpolated between the bins (Gaussian on the amplitude)
    double level = 0.0;       // as drawn (dB or linear) at the bin
    double linear = 0.0;      // amplitude or density at the bin
    std::size_t bin = 0;
};
struct ChannelMeasures
{
    std::string name;
    std::string unit;
    std::size_t samples = 0; // finite samples
    double mean = 0.0;
    double rms = 0.0;
    double acRms = 0.0; // around the mean
    double minimum = 0.0;
    double maximum = 0.0;
    double peakToPeak = 0.0;
    double first = 0.0;       // first and last finite samples of the zone
    double last = 0.0;
    double integral = 0.0;    // area under the zone, unit·s (trapezoids, holes left out)
    double crestFactor = 0.0; // max |x - mean| / acRms
    dsp::ToneAnalysis tone;   // real channels only; tone.detected false otherwise
    // Floor of the amplitude spectrum (median of its bins above the DC lobe; amplitude spectrum only).
    double spectrumFloor = 0.0;
    // Level of white noise in one bin of the amplitude spectrum that would give the SNR of the tone:
    // 2 sigma sqrt(ENBW / fs). Only a fallback: lines that are not harmonics count as noise in the SNR.
    double noiseInBin = 0.0;
    // One per tone.harmonics (the fundamental always): the harmonic stands out of the spectrum around it.
    // Amplitude spectrum: measured amplitude above three times the median of the bins on either side of
    // it (outside its lobe), about 10 dB; otherwise three times noiseInBin. Never below −150 dBc.
    std::vector<bool> harmonicStandsOut;
    // Oscilloscope measures of the zone (qacq/analysis/measures.h), filled by analyse().
    WaveMeasures wave; // oscilloscope measures of the zone (real channels)
};

// One curve of the spectrum: a real channel, or an I/Q pair (two-sided).
// Where a curve stays within 3 dB of its highest bin, around it: half power for a density, 1/√2 of the
// amplitude for an amplitude spectrum. A side that never falls by 3 dB (down to DC or up to the end) is
// open: lowpass or highpass shape. On a pure line it is the width of the window's main lobe.
struct HalfPowerBand
{
    bool found = false;
    bool lowOpen = false, highOpen = false;
    double lowHz = 0.0, highHz = 0.0, peakHz = 0.0;
    double centreHz() const noexcept { return 0.5 * (lowHz + highHz); }
    double widthHz() const noexcept { return highHz - lowHz; }
};

struct SpectrumCurve
{
    std::string name;
    std::string unit;              // of the channel ("V")
    std::vector<double> level;     // as drawn: dB or linear (amplitude, or density in unit/√Hz)
    std::vector<double> linear;    // amplitude (unit) or density (unit²/Hz), for the readout
    plot::MinMaxPyramid pyramid;   // of `level`, built when the curve is long
    std::vector<int> channels;     // indices of the channels of the signal it comes from
    std::size_t peakBin = 0;       // largest level
    std::vector<Peak> peaks;       // the highest peaks, highest first (findPeaks, at most 10)
    HalfPowerBand band;            // around peakBin (halfPowerBand)
};

struct AnalysisResult
{
    std::string error; // in French: why there is no spectrum (zone too short, too long...)
    SpectrumKind kind = SpectrumKind::Amplitude;
    LevelScale scale = LevelScale::Decibel;
    bool twoSided = false;      // I/Q: frequencies from −fs/2 to +fs/2
    double sampleRateHz = 0.0;
    double firstFrequencyHz = 0.0; // of bin 0
    double binWidthHz = 0.0;
    std::size_t bins = 0;
    double enbwHz = 0.0;        // noise bandwidth of a bin
    std::size_t fftSize = 0;    // amplitude: FFT length; density: segment length
    std::size_t segments = 0;   // density
    std::size_t zoneFirstFrame = 0;
    std::size_t zoneFrames = 0;
    bool paddedToPowerOfTwo = false;
    std::vector<SpectrumCurve> curves;
    std::vector<ChannelMeasures> measures;
    std::vector<std::string> notes; // remarks in French (an I/Q pair without tone measurement...)

    bool ok() const noexcept { return error.empty() && !curves.empty(); }
    double frequencyAt(double bin) const noexcept { return firstFrequencyHz + bin * binWidthHz; }
    double binAt(double frequencyHz) const noexcept
    {
        return binWidthHz > 0.0 ? (frequencyHz - firstFrequencyHz) / binWidthHz : 0.0;
    }
};

// The zone [firstFrame, firstFrame + frameCount) of `signal` (clipped to it). `cancelled`, when given,
// is read between the channels: a result no one waits for any more stops early (error "annulé").
// `spectrumChannels`, when given, lists the channels whose spectrum is computed (an I/Q pair when one of
// its channels is listed); empty: no spectrum, the measurements only (result.ok() is then false). Every
// channel is measured either way.
AnalysisResult analyse(const model::Signal &signal, std::size_t firstFrame, std::size_t frameCount,
                       const AnalysisSettings &settings, const std::atomic<bool> *cancelled = nullptr,
                       const std::vector<std::size_t> *spectrumChannels = nullptr);

// Half-width of the main lobe of the window of the settings, in bins of the record.
double mainLobeHalfWidth(const AnalysisSettings &settings) noexcept;

// The same result in another level scale (the curves are levelled again; nothing is computed anew).
void setScale(AnalysisResult &result, LevelScale scale);

// Level of a linear value as drawn (dB: floor at −400 dB instead of −∞).
double levelOf(double linear, SpectrumKind kind, LevelScale scale) noexcept;
// Unit of the level axis for a channel unit: amplitude dB "dBV" (V), "dB" (no unit), "dB re 1 A";
// density dB "dBV/√Hz", "dB/Hz", "dB re 1 A/√Hz"; linear "V", "V/√Hz", "" and "1/√Hz".
std::string levelUnit(std::string_view unit, SpectrumKind kind, LevelScale scale);
// Automatic scale for levels between minimum and maximum: decibels round to 10 dB and show at most the
// top 160 dB; linear starts at 0.
plot::Range levelRange(double minimum, double maximum, LevelScale scale);

// "Fréquence (Hz);Voie 1 (dBV);Voie 1 (V);..." then one line per bin, decimal comma.
void writeSpectrumCsv(std::ostream &out, const AnalysisResult &result);
} // namespace qacq::analysis
