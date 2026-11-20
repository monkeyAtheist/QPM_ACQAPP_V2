#pragma once

// Analysis and FIR-design windows.
//
// Spectral analysis uses PERIODIC windows (DFT-even, the length-(N+1) symmetric window without its last
// point): their coherent gain and ENBW are exact on the DFT grid. FIR design uses SYMMETRIC windows,
// as scipy.signal.firwin does.

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace qacq::dsp
{
enum class Window
{
    Rectangular,
    Hann,
    Hamming,
    Blackman,
    BlackmanHarris, // 4-term, -92 dB side lobes
    FlatTop,        // amplitude-accurate to 0.01 dB (SciPy coefficients)
    Kaiser          // shape set by beta
};

enum class WindowSymmetry
{
    Periodic,
    Symmetric
};

std::string_view windowName(Window window) noexcept;

std::vector<double> makeWindow(Window window, std::size_t n, WindowSymmetry symmetry = WindowSymmetry::Periodic,
                               double kaiserBeta = 8.6);

// Sum of the coefficients divided by N (amplitude of a bin-centred tone after windowing).
double coherentGain(std::span<const double> window) noexcept;
// Equivalent noise bandwidth in bins: N * sum(w^2) / sum(w)^2 (1.5 for Hann).
double equivalentNoiseBandwidthBins(std::span<const double> window) noexcept;

// Modified Bessel function of the first kind, order 0 (power series, relative error < 1e-15
// for the Kaiser range |x| <= 50).
double besselI0(double x) noexcept;
} // namespace qacq::dsp
