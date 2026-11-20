#pragma once

// Tone analysis: fundamental, harmonics, THD, SINAD, SNR, SFDR, ENOB.
//
// Correction of the audit's defect 1 (20/11/2026): the old code estimated f0 on the FFT of the last
// 65 536 samples and then projected the amplitudes over the whole record, so on 10 s at 48 kHz the
// THD read 1.1 % instead of 11.2 %. Here f0 and every amplitude come from the SAME samples:
//   1. coarse f0: peak of a Hann spectrum (on at most 2^20 samples), Gaussian interpolation;
//   2. refinement: least-squares fit of the fundamental and its harmonics on the two halves of a
//      growing part of the record; the phase drift between the halves gives the frequency error;
//   3. amplitudes: one joint least-squares fit of DC + all harmonics (cos and sin) on the whole record.
// The fit is exact for any record length (no leakage between harmonics), and costs O(N K).

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace qacq::dsp
{
struct ToneOptions
{
    std::size_t harmonicCount = 10;   // fundamental included; harmonics above Nyquist are left out
    double minimumFrequencyHz = 0.0;  // search band for the fundamental (0: just above the DC lobe)
    double maximumFrequencyHz = 0.0;  // 0: Nyquist
    double minimumProminenceDb = 15.0; // peak over the median of the band, below which no tone is reported
};

struct Harmonic
{
    int order = 0;
    double frequencyHz = 0.0;
    double amplitude = 0.0; // peak amplitude
    double phaseRad = 0.0;  // x(t) = A cos(2 pi f t + phase), t = 0 at the first sample
    double relativeDb = 0.0; // relative to the fundamental
};

struct ToneAnalysis
{
    bool detected = false;
    double dc = 0.0;
    double rms = 0.0;
    double acRms = 0.0;
    double prominenceDb = 0.0;
    double fundamentalHz = 0.0;
    double fundamentalAmplitude = 0.0;
    double fundamentalPhaseRad = 0.0;
    std::vector<Harmonic> harmonics; // order 1 first
    double thdRatio = 0.0;           // sqrt(sum_{k>=2} A_k^2) / A_1
    double thdPercent = 0.0;
    double thdDb = 0.0;
    double sinadDb = 0.0;            // fundamental over everything else (harmonics + noise)
    double snrDb = 0.0;              // fundamental over noise (harmonics excluded)
    double sfdrDb = 0.0;             // fundamental over the largest other spectral component
    double enob = 0.0;               // (SINAD - 1.76) / 6.02
    std::size_t samplesUsed = 0;
};

ToneAnalysis analyzeTone(std::span<const double> x, double sampleRateHz, const ToneOptions &options = {});

// Least-squares fit of x[n] ~ dc + sum_k Re(C_k exp(j 2 pi k f n / fs)) on all of x, k = 1..harmonics
// (harmonics at or above Nyquist are skipped and get C_k = 0). Exposed for tests and for the
// "measured Fourier coefficients" view. Returns false when the system is singular.
struct HarmonicFit
{
    double dc = 0.0;
    std::vector<std::complex<double>> coefficients; // C_1 ... C_K: amplitude |C_k|, phase arg(C_k)
    double residualMeanSquare = 0.0;                // mean of (x - model)^2
};
bool fitHarmonics(std::span<const double> x, double sampleRateHz, double frequencyHz, std::size_t harmonics,
                  HarmonicFit &fit, std::size_t firstSampleIndex = 0);
} // namespace qacq::dsp
