#pragma once

// Record types of tests/fixtures/scipy_reference.inc.

#include <vector>

namespace qacq::test
{
struct IirReference
{
    const char *name;
    int family;   // 0 Butterworth, 1 Chebyshev I, 2 Chebyshev II, 3 Bessel (norm "mag"), 4 elliptic
    int response; // 0 low-pass, 1 high-pass, 2 band-pass, 3 band-stop
    int order;
    double fs, f1, f2, rippleDb, attenuationDb;
    std::vector<double> frequencyHz;
    std::vector<double> magnitudeDb; // scipy.signal.sosfreqz
    std::vector<double> sosfilt;     // scipy.signal.sosfilt(testSignal)[::3]
    std::vector<double> sosfiltfilt; // scipy.signal.sosfiltfilt(testSignal)[::3]
};

struct FirReference
{
    const char *name;
    int response;
    int taps;
    double fs, f1, f2;
    int window; // 0 boxcar, 1 hann, 2 hamming, 3 blackman, 4 blackmanharris, 5 flattop, 6 kaiser
    double beta;
    std::vector<double> coefficients; // scipy.signal.firwin(..., scale=True)
};

struct RemezReference
{
    const char *name;
    int taps;
    double fs;
    std::vector<double> bands;
    std::vector<double> desired;
    std::vector<double> weights;
    std::vector<double> coefficients; // scipy.signal.remez(taps, bands, desired, weight=weights, fs=fs)
};

// The least order for a template (gabarit).
struct OrderReference
{
    const char *name;
    int family;   // as IirReference (no Bessel: SciPy has no besselord)
    int response;
    double fs;
    std::vector<double> pass; // the pass band edges (two for a band)
    std::vector<double> stop; // the stop band edges
    double passbandLossDb, stopbandAttenuationDb;
    int order;              // scipy.signal.buttord, cheb1ord, cheb2ord, ellipord(..., fs=fs)
    std::vector<double> wn; // the frequencies they give (Hz)
};

struct WelchReference
{
    const char *name;
    int window;
    int segment;
    int overlap;
    std::vector<double> psd; // scipy.signal.welch(testSignal, 48000, ...)
};

// Zero-phase filtering with each of SciPy's edge extensions (padtype "odd", "even", "constant", None).
struct FiltFiltReference
{
    const char *name;
    const char *padtype;           // "odd", "even", "constant", "none"
    std::vector<double> sos;       // 6 per section (b0 b1 b2 a0 a1 a2); empty for a FIR
    std::vector<double> taps;      // a FIR (filtfilt(taps, 1)); empty for sections
    std::vector<double> x;
    std::vector<double> y;         // scipy.signal.sosfiltfilt(sos, x, padtype=...) or filtfilt(taps, 1, x, ...)
};

struct BesselPoleReference
{
    int order;
    std::vector<double> real; // scipy.signal.besselap(order, norm="mag"), sorted by (imag, real)
    std::vector<double> imag;
};

#include "scipy_reference.inc"
} // namespace qacq::test
