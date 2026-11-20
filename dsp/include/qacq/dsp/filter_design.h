#pragma once

// Digital filter design.
//
// IIR filters are designed as poles, zeros and gain (analog prototype, frequency transformation,
// bilinear transform with pre-warping, exactly as scipy.signal does) and are ALWAYS executed as a cascade
// of second-order sections. Correction of the audit's defect 2 (20/11/2026): the old code expanded the
// poles into a single polynomial a(z); for a Butterworth of order 8 at 100 Hz (fs = 48 kHz) the roots of
// that polynomial reach 1.006 although the true poles are inside 0.998, and the filter diverged.
// With sections there is no order limit tied to numerical robustness (up to order 32 here, Bessel 12).

#include "qacq/dsp/window.h"

#include <array>
#include <complex>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qacq::dsp
{
using Complex = std::complex<double>;

enum class FilterFamily
{
    Butterworth,
    ChebyshevI,
    ChebyshevII,
    Bessel // normalised to -3 dB at the cut-off (scipy norm="mag")
};

enum class FilterResponse
{
    LowPass,
    HighPass,
    BandPass,
    BandStop
};

std::string_view familyName(FilterFamily family) noexcept;
std::string_view responseName(FilterResponse response) noexcept;

struct Zpk
{
    std::vector<Complex> zeros;
    std::vector<Complex> poles;
    double gain = 1.0;
};

// One second-order section: H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2).
struct Biquad
{
    std::array<double, 3> b{1.0, 0.0, 0.0};
    std::array<double, 3> a{1.0, 0.0, 0.0}; // a[0] is always 1
};
using SosCascade = std::vector<Biquad>;

struct IirSpec
{
    FilterFamily family = FilterFamily::Butterworth;
    FilterResponse response = FilterResponse::LowPass;
    int order = 4;                  // prototype order (band filters have 2 x order poles)
    double sampleRateHz = 48000.0;
    double frequency1Hz = 1000.0;   // cut-off, or lower band edge
    double frequency2Hz = 2000.0;   // upper band edge (band-pass, band-stop)
    double passbandRippleDb = 1.0;  // Chebyshev I
    double stopbandAttenuationDb = 40.0; // Chebyshev II: frequency1 is where the stop band starts (gain -Rs), as in SciPy
};

struct IirDesign
{
    Zpk digital;
    SosCascade sections;
    std::string error; // empty when the design succeeded (message in French, for the interface)

    bool ok() const noexcept { return error.empty() && !sections.empty(); }
};

inline constexpr int kMaximumIirOrder = 32;
inline constexpr int kMaximumBesselOrder = 12; // beyond, the roots of the Bessel polynomial lose accuracy in double

IirDesign designIir(const IirSpec &spec);

// Groups conjugate poles and zeros into sections. The section with the poles nearest the unit circle
// comes last and each pole pair takes its nearest zeros (SciPy's "nearest" strategy), which keeps the
// intermediate signals small. The overall gain is put in the first section.
SosCascade zpkToSos(const Zpk &zpk);

// Frequency response at f (Hz) for a sampling rate fs.
Complex sosResponse(const SosCascade &sections, double frequencyHz, double sampleRateHz) noexcept;
Complex transferResponse(std::span<const double> b, std::span<const double> a, double frequencyHz,
                         double sampleRateHz) noexcept;
// Group delay in samples (exact derivative of the phase, no unwrapping).
double sosGroupDelay(const SosCascade &sections, double frequencyHz, double sampleRateHz) noexcept;
double transferGroupDelay(std::span<const double> b, std::span<const double> a, double frequencyHz,
                          double sampleRateHz) noexcept;

// FIR filters by the window method, same coefficients as scipy.signal.firwin(..., scale=True):
// symmetric window, gain 1 at DC (low-pass, band-stop), at Nyquist (high-pass) or at the band centre
// (band-pass). High-pass and band-stop need an odd number of taps.
struct FirSpec
{
    FilterResponse response = FilterResponse::LowPass;
    std::size_t taps = 127;
    double sampleRateHz = 48000.0;
    double frequency1Hz = 1000.0;
    double frequency2Hz = 2000.0;
    Window window = Window::Hamming;
    double kaiserBeta = 8.6;
};

struct FirDesign
{
    std::vector<double> taps;
    std::string error;

    bool ok() const noexcept { return error.empty() && !taps.empty(); }
    // A symmetric FIR of N taps delays every frequency by (N - 1) / 2 samples.
    double delaySamples() const noexcept { return taps.empty() ? 0.0 : 0.5 * static_cast<double>(taps.size() - 1); }
};

FirDesign designFir(const FirSpec &spec);

// Kaiser window design rule (Kaiser 1974): beta and number of taps for a stop-band attenuation (dB)
// and a transition width (Hz).
struct KaiserEstimate
{
    double beta = 0.0;
    std::size_t taps = 0;
};
KaiserEstimate estimateKaiser(double attenuationDb, double transitionWidthHz, double sampleRateHz) noexcept;
} // namespace qacq::dsp
