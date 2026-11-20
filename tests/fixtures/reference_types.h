#pragma once

// Record types of tests/fixtures/scipy_reference.inc.

#include <vector>

namespace qacq::test
{
struct IirReference
{
    const char *name;
    int family;   // 0 Butterworth, 1 Chebyshev I, 2 Chebyshev II, 3 Bessel (norm "mag")
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

struct WelchReference
{
    const char *name;
    int window;
    int segment;
    int overlap;
    std::vector<double> psd; // scipy.signal.welch(testSignal, 48000, ...)
};

struct BesselPoleReference
{
    int order;
    std::vector<double> real; // scipy.signal.besselap(order, norm="mag"), sorted by (imag, real)
    std::vector<double> imag;
};

#include "scipy_reference.inc"
} // namespace qacq::test
