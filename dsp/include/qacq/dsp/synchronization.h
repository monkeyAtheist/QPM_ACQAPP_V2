#pragma once

// The receiver of a real link: it knows the modulation, the symbol rate, the pulse, the nominal carrier and a
// preamble (the first symbols of every frame); not when the frame arrives, nor the offset and phase of the
// carrier, nor the error of its own clock. From the complex baseband (toBaseband, at the nominal carrier):
//   1. resampled to 8 samples per symbol (more for an FSK whose tones lie far out), then the matched filter: the
//      root raised cosine, or the mean over a symbol; FSK the difference of the energies of its two tones over a
//      symbol;
//   2. the frame: for a constellation centred on 0 (PSK, QAM) and FSK, the products of consecutive filter
//      outputs against those of the preamble's symbols, at every instant (insensitive to a frequency offset,
//      within +-Rs/2); for an ASK (half its power in its carrier) the carrier line gives the frequency offset
//      first, then the preamble is correlated coherently (both less their mean). The peak tells where the frame
//      starts (a parabola through it for the fraction of a sample) and a coarse frequency offset;
//   3. the frequency offset refined: the baseband turned back by it and filtered again, the preamble's symbols
//      taken off leave a tone whose frequency the DFT finds (within +-Rs/2, near the Cramér-Rao bound); turned
//      back again, the start found again where the preamble's correlation peaks; phase and gain of the carrier
//      from that correlation;
//   4. symbol timing: Gardner's detector (the filter output half-way between two symbols, an ASK less its mean),
//      a second-order loop of noise bandwidth timingBandwidth (times T), values between samples by the sinc
//      interpolator;
//   5. carrier: a second-order phase-locked loop of bandwidth phaseBandwidth (detector Im(z conj(d)) over the
//      mean energy of the constellation), led by the preamble's symbols, then by the decisions; the level of the
//      symbols followed by the decisions over some 256 symbols.
// The loops run twice: first wide (timing 0.01, carrier 0.01, or the bandwidths asked for if wider), over the
// whole frame, to learn how the instants and the carrier phase run (a straight line each, by least squares
// from the end of the preamble on: the clock's drift, what the preamble left of the frequency offset); then with
// the bandwidths asked for, from those lines: they follow what departs from them, with little jitter.
// FSK, non-coherent: steps 1, 2 (the frame alone) and 4.
// The loop gains: Gardner's slope measured once on the pulse and the constellation without noise; damping 0.707.
// Measured (QPSK, 16- and 64-QAM, 8-PSK, BPSK, ASK, FSK at 1 kBd and 48 kHz on a 6 kHz carrier, 30 Hz off, a
// clock 200 ppm fast, a delay of a few symbols): 0.05 to 0.35 dB more Eb/N0 than the theory needs for the same
// bit error rate (256-QAM the most); below about 1 dB (QPSK), 3 dB (BPSK) the carrier loop slips now and then.

#include "qacq/dsp/receiver.h"

#include <complex>
#include <cstddef>
#include <string>
#include <vector>

namespace qacq::dsp
{
struct SyncSettings
{
    // The symbols the receiver knows, at the start of the frame (unit scale: points of the constellation; FSK:
    // +1 for a 1, -1 for a 0). At least 8.
    std::vector<std::complex<double>> preamble;
    double timingBandwidth = 0.001; // B_L T of the timing loop
    double phaseBandwidth = 0.003;  // B_L T of the carrier loop
};

struct SyncReception
{
    // Decision points (unit scale), symbols and bits, from the start of the frame (symbol 0: the first of the
    // preamble); the eye from the filter output at the receiver's own rate (matchedRateHz), turned back by the
    // carrier found.
    DigitalReception reception;
    bool frameFound = false;
    double frameStart = 0.0;   // where symbol 0 is decided, in samples of the record received
    double frameQuality = 0.0; // the correlation of the preamble at its peak, 0 to 1 (1: without noise)
    double frequencyOffsetHz = 0.0; // the preamble's estimate, then what the carrier loop added
    double phaseRadians = 0.0;      // carrier phase at the record's first sample: the loop's over the preamble, less 2 pi df t
    double gain = 0.0;              // amplitude of the symbols over that of the carrier announced
    double clockOffsetPpm = 0.0;    // from the period the timing loop settled to
    std::vector<double> instants;   // each symbol: where it was decided, in samples of the record received
    std::vector<double> phases;     // each symbol: the carrier phase taken off (radians: 2 pi df t + the loop's)
    // Before synchronization: the filter output at the frame start + k T, neither turned back nor following the
    // clock (what a receiver that only found the frame would decide).
    std::vector<std::complex<double>> rawPoints;
    // For an equalizer followed by its own carrier loop (several paths: the decisions the loop above leans on are
    // too often wrong before the equalizer): the filter output at the instants decided (the timing followed),
    // turned back by what the preamble alone gave (its frequency and its phase, preamblePhases), neither the
    // carrier loop nor the level followed afterwards; and the same half a symbol earlier (between two symbols:
    // where the timing loop looks for its zero), for an equalizer that finds its symbols better there (two paths
    // of the same strength: the timing settles between them). Empty for FSK.
    std::vector<std::complex<double>> preamblePoints, preambleHalfPoints;
    std::vector<double> preamblePhases; // each symbol: 2 pi df t + phase, as the preamble alone found them (radians)
    std::vector<std::string> warnings;
    std::string error;

    bool ok() const noexcept { return error.empty(); }
};

SyncReception receiveSynchronized(const ReceivedInput &input, const ReceiverSpec &spec, const SyncSettings &settings);

// The samples per symbol the synchronized receiver works at (8, or more for an FSK whose tones lie beyond 2.4 Rs).
int synchronizedSamplesPerSymbol(const ReceiverSpec &spec) noexcept;
// Where the synchronized receiver decides symbol k: the instant of the transmission it aims at, in samples of the
// transmission (k sps for the root raised cosine; the middle of the symbol for rectangular pulses and FSK), and
// how far after it its filter peaks, in samples of the record received (0; for a window of one symbol, half of
// it less half a sample of its own). A channel moves the first: receivedPosition(target) + lag.
double synchronizedTarget(const ReceiverSpec &spec, double symbol) noexcept;
double synchronizedLag(const ReceiverSpec &spec) noexcept;
// The loop gains of a second-order loop of noise bandwidth bandwidth (times the update period), damping 0.707,
// for a detector of unit slope: proportional, integral.
void loopGains(double bandwidth, double &proportional, double &integral) noexcept;
} // namespace qacq::dsp
