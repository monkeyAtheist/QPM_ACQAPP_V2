#pragma once

// Spectra of sampled records. Every function uses ALL the samples it is given: choosing a part of a
// record is the caller's decision, never a silent truncation (audit of 20/11/2026, defect 3).

#include "qacq/dsp/window.h"

#include <cstddef>
#include <span>
#include <vector>

namespace qacq::dsp
{
struct SpectrumOptions
{
    Window window = Window::Hann;
    double kaiserBeta = 8.6;
    std::size_t zeroPadFactor = 1; // FFT length = factor x record length, exactly (any length)
    // FFT length chosen by the caller instead (at least the record length, zeros after it): a power of two
    // keeps a long record fast (Bluestein's algorithm needs four times the memory). 0: zeroPadFactor.
    std::size_t fftSize = 0;
    bool removeMean = false;
    // With removeMean: subtract the mean weighted by the window, sum(w x) / sum(w), rather than the plain
    // mean. A record that does not hold a whole number of periods has a plain mean that is not its DC
    // part; removing it leaves a step the window turns into a false line at DC and in the first bins.
    // The weighted mean leaves nothing at DC (bin 0 exactly zero) and nothing beside it.
    bool windowedMean = false;
};

// One-sided spectrum of a real record.
//  amplitude: peak amplitude of a sinusoid centred on the bin (coherent gain compensated;
//             DC and Nyquist bins are not doubled). A tone between two bins reads low by the window's
//             scalloping loss (1.42 dB for Hann, 0.01 dB for FlatTop).
//  psd:       one-sided power spectral density in unit^2/Hz; sum(psd) * binWidth = mean square.
struct Spectrum
{
    std::vector<double> frequencyHz;
    std::vector<double> amplitude;
    std::vector<double> psd;
    std::vector<double> phaseRad;
    double sampleRateHz = 0.0;
    double binWidthHz = 0.0;
    double enbwHz = 0.0;
    double coherentGain = 0.0;
    std::size_t samplesUsed = 0;
    std::size_t fftSize = 0;

    bool empty() const noexcept { return amplitude.empty(); }
};

Spectrum spectrum(std::span<const double> x, double sampleRateHz, const SpectrumOptions &options = {});

// Two-sided spectrum of a complex baseband record (I/Q), frequency offsets from -fs/2 to +fs/2
// (centred, "fftshift"). amplitude = |X| / sum(w): a complex tone A exp(j 2 pi f t) reads A.
struct ComplexSpectrum
{
    std::vector<double> frequencyHz;
    std::vector<double> amplitude;
    std::vector<double> psd; // two-sided, unit^2/Hz
    std::vector<double> phaseRad;
    double sampleRateHz = 0.0;
    double binWidthHz = 0.0;
    double enbwHz = 0.0;
    std::size_t samplesUsed = 0;
    std::size_t fftSize = 0;

    bool empty() const noexcept { return amplitude.empty(); }
};

ComplexSpectrum complexSpectrum(std::span<const double> inPhase, std::span<const double> quadrature,
                                double sampleRateHz, const SpectrumOptions &options = {});

// Welch averaged periodogram, same conventions as scipy.signal.welch(..., scaling="density",
// detrend="constant" when removeSegmentMean, average="mean"). Segments that do not fit entirely are
// dropped, as SciPy does; a record shorter than one segment gives one segment of the record's length.
struct WelchOptions
{
    Window window = Window::Hann;
    double kaiserBeta = 8.6;
    std::size_t segmentLength = 4096;
    double overlap = 0.5; // fraction of a segment, [0, 0.95]
    bool removeSegmentMean = true;
    bool windowedMean = false; // as SpectrumOptions::windowedMean, segment by segment (not SciPy's)
};

struct WelchResult
{
    std::vector<double> frequencyHz;
    std::vector<double> psd;
    std::size_t segments = 0;
    std::size_t segmentLength = 0;
    std::size_t hop = 0;
    double enbwHz = 0.0;

    bool empty() const noexcept { return psd.empty(); }
};

WelchResult welch(std::span<const double> x, double sampleRateHz, const WelchOptions &options = {});

// Short-time spectrum (amplitude in dB, same scaling as spectrum()). When the record would give more
// than maximumFrames frames, the hop grows (the result says which hop was used).
struct SpectrogramOptions
{
    Window window = Window::Hann;
    double kaiserBeta = 8.6;
    std::size_t segmentLength = 1024;
    double overlap = 0.75;
    std::size_t maximumFrames = 2000;
    bool removeSegmentMean = false;
    bool windowedMean = false; // as SpectrumOptions::windowedMean, frame by frame
};

struct Spectrogram
{
    std::vector<double> timeSeconds; // frame centres
    std::vector<double> frequencyHz;
    std::vector<double> amplitudeDb; // frames x bins, row-major (one row per frame)
    std::size_t frames = 0;
    std::size_t bins = 0;
    std::size_t segmentLength = 0;
    std::size_t hop = 0;
    double frameDurationSeconds = 0.0;
    double frameStepSeconds = 0.0;

    bool empty() const noexcept { return amplitudeDb.empty(); }
    double at(std::size_t frame, std::size_t bin) const { return amplitudeDb[frame * bins + bin]; }
};

Spectrogram spectrogram(std::span<const double> x, double sampleRateHz, const SpectrogramOptions &options = {});
} // namespace qacq::dsp
