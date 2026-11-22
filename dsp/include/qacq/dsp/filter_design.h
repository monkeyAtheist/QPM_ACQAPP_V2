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
    Bessel,  // normalised to -3 dB at the cut-off (scipy norm="mag")
    Elliptic // Cauer: ripple in both bands (passband ripple at the cut-off, as scipy.signal.ellip)
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
    double passbandRippleDb = 1.0;  // Chebyshev I, elliptic
    double stopbandAttenuationDb = 40.0; // Chebyshev II: frequency1 is where the stop band starts (gain -Rs), as in SciPy;
                                         // elliptic: the least attenuation of the stop band
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

// A template (gabarit): the edges of the pass and stop bands, at most Ap of attenuation over the pass band(s),
// at least As over the stop band(s). Edges in Hz, [0] the lower and [1] the upper (bands): a low-pass passes up
// to passHz[0] and stops from stopHz[0]; a high-pass stops up to stopHz[0] and passes from passHz[0]; a
// band-pass stops up to stopHz[0], passes from passHz[0] to passHz[1], stops from stopHz[1]; a band-stop passes
// up to passHz[0], stops from stopHz[0] to stopHz[1], passes from passHz[1].
struct FilterTemplate
{
    FilterResponse response = FilterResponse::LowPass;
    double sampleRateHz = 48000.0;
    std::array<double, 2> passHz{1000.0, 2000.0};
    std::array<double, 2> stopHz{2000.0, 4000.0};
    double passbandLossDb = 1.0;         // Ap > 0
    double stopbandAttenuationDb = 40.0; // As > Ap
};

// In French, what makes the template impossible (edges out of order or beyond fs/2, As not above Ap); empty
// when it is consistent.
std::string templateError(const FilterTemplate &t);

struct IirOrder
{
    int order = 0;             // the least that holds the template (it may be beyond what designIir designs)
    double exactOrder = 0.0;   // what the template asks, as a real number, rounded up to `order` (Bessel: 0)
    double frequency1Hz = 0.0; // the frequencies of IirSpec that design it with exactly −Ap at the pass band
    double frequency2Hz = 0.0; // edges (frequency2: a band)
    std::string error;         // in French

    bool ok() const noexcept { return error.empty() && order > 0; }
};

// The least order of a family that holds a template, as scipy.signal.buttord, cheb1ord, cheb2ord and ellipord
// compute it (Bessel, which has no formula: its magnitude solved, up to kMaximumBesselOrder; beyond, order is
// kMaximumBesselOrder + 1 and no frequency is given), and the frequencies to design it with: −Ap exactly at
// the pass band edges, the excess in the stop band. A band-stop's pass band edges are first moved towards its
// stop band where that lowers the order, as SciPy and MATLAB do (SciPy's bounded minimisation, step for step);
// where SciPy's search, which stops within 1e-5 of tan(π f/fs), would cost an order (low frequencies, the best
// being the edge itself), an edge is left where it was: one order fewer than SciPy, the template held all the same.
IirOrder minimumIirOrder(FilterFamily family, const FilterTemplate &t);

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

// FIR filters by the Parks-McClellan algorithm (Remez exchange), as scipy.signal.remez with type
// "bandpass": symmetric taps (linear phase), the weighted error equiripple, as small as it can be over the
// bands given. Bands: edge pairs in Hz, ascending, within [0, fs/2]; one desired gain and one weight per
// band (a larger weight, a smaller error there). Between the bands, nothing is asked: the transitions.
struct RemezSpec
{
    std::size_t taps = 101;
    double sampleRateHz = 48000.0;
    std::vector<double> bandsHz;  // [low0, high0, low1, high1, ...]
    std::vector<double> desired;  // per band
    std::vector<double> weights;  // per band (empty: all 1)
    int gridDensity = 16;         // grid points per extremum, as SciPy
    int maximumIterations = 25;   // as SciPy
};

// How the exchange ended: every extremal frequency in place; after maximumIterations; where the levelled
// error stopped growing (rounding: a filter whose best error is near or below the precision of the
// computation). Stopped early, the taps are given all the same: they may do, measure the response.
enum class RemezStop
{
    None,
    Iterations,
    Rounding
};

struct RemezDesign
{
    std::vector<double> taps;
    std::string error; // in French: nothing designed
    int iterations = 0;
    double deviation = 0.0; // the weighted error the exchange levelled (δ)
    RemezStop stop = RemezStop::None;
    std::string stopped; // in French, how it stopped early; empty when it went to the end

    bool ok() const noexcept { return error.empty() && !taps.empty(); }
};

inline constexpr std::size_t kMaximumRemezTaps = 4095;

RemezDesign designRemez(const RemezSpec &spec);

// Kaiser window design rule (Kaiser 1974): beta and number of taps for a stop-band attenuation (dB)
// and a transition width (Hz).
struct KaiserEstimate
{
    double beta = 0.0;
    std::size_t taps = 0;
};
KaiserEstimate estimateKaiser(double attenuationDb, double transitionWidthHz, double sampleRateHz) noexcept;

// Roots of a real polynomial given in ascending powers (its last coefficient not 0), by the Aberth-Ehrlich
// iteration: O(degree²) per step; the zeros of a FIR of a few hundred taps in a few milliseconds.
std::vector<Complex> polynomialRoots(std::span<const double> ascending);

// Elliptic functions, for the elliptic (Cauer) filters (parameter m = k², 0 <= m < 1).
// K(m), complete integral of the first kind, by the arithmetic-geometric mean; K(1 - p) from p, accurate
// when p is tiny (a selective filter).
double ellipticK(double m) noexcept;
double ellipticKComplement(double p) noexcept;
struct JacobiValues
{
    double sn = 0.0, cn = 1.0, dn = 1.0;
};
// sn, cn, dn of u (the algorithm of Cephes ellpj, as scipy.special.ellipj).
JacobiValues jacobiElliptic(double u, double m) noexcept;

// The analog elliptic low-pass of order n (passband edge 1 rad/s, ripple Rp, stopband at least Rs down),
// as scipy.signal.ellipap; and the edge of its stop band (where the attenuation first reaches Rs), in
// rad/s.
Zpk ellipticPrototype(int n, double rippleDb, double attenuationDb);
double ellipticStopbandEdge(int n, double rippleDb, double attenuationDb);
} // namespace qacq::dsp
