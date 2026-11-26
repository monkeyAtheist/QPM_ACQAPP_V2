#pragma once

// Receivers for the modulations of modulation.h, and bit error rates.
//
// Digital, coherent (carrier frequency, phase and symbol timing known, as in the old laboratory):
//   1. back to complex baseband: r = 2 x exp(-j (2 pi fc t + phi)) then a linear-phase low-pass filter
//      that removes the 2 fc image (passband), or r = b exp(-j phi) (complex baseband);
//   2. matched filter: mean over each symbol (rectangular pulses) or root-raised-cosine filter sampled at
//      the symbol centres;
//   3. decisions: the nearest point of the constellation (sign for BPSK and QPSK, threshold halfway between
//      the two levels for ASK, the sector of the angle for 8-PSK, each axis on its own for QAM); FSK is
//      detected without phase reference, by comparing the energies at fc + deltaF and fc - deltaF.
//   Rectangular pulses and FSK are integrated on the raw mixer output (correlation receiver, matched to
//   the passband pulse itself). When 2 fc / Rs is not an integer the image term makes the energy of each
//   symbol depend on the carrier phase at its start (about +-Rs / (2 pi fc)): slightly more errors than
//   the theory, a difference that fades as fc / Rs grows.
// Analog: AM by envelope, FM by phase-difference discriminator, PM by unwrapped phase.

#include "qacq/dsp/modulation.h"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace qacq::dsp
{
struct ReceiverSpec
{
    Modulation type = Modulation::Bpsk;
    Representation representation = Representation::RealPassband;
    double sampleRateHz = 48000.0;
    double carrierFrequencyHz = 12000.0;
    double carrierAmplitude = 1.0; // scale of the decision points
    double carrierPhaseDegrees = 0.0;
    // Digital.
    double symbolRate = 2000.0;
    double askLowLevel = 0.0;
    double fskDeviationHz = 1000.0;
    PulseShape pulse = PulseShape::Rectangular;
    double rolloff = 0.35;
    std::size_t pulseSpanSymbols = 0; // symbols the root raised cosine is cut to; 0: rootRaisedCosineSpan(rolloff)
    // Analog.
    double messageBandwidthHz = 3000.0; // low-pass cut-off of the recovered message
    double amIndex = 0.8;
    double frequencyDeviationHz = 5000.0;
    double phaseDeviationRad = 1.0;
    // The output of the matched filter at every sample (the eye diagram); false: only at the decision instants
    // (a curve of the bit error rate: the root raised cosine sps times faster).
    bool matchedOutput = true;
};

// The received signal: passband (RealPassband) or inPhase + quadrature (ComplexBaseband).
struct ReceivedInput
{
    std::span<const double> passband;
    std::span<const double> inPhase;
    std::span<const double> quadrature;
};

struct DigitalReception
{
    std::vector<std::complex<double>> decisionPoints; // matched-filter outputs, unit scale (constellation)
    std::vector<std::size_t> symbols;                  // decided (constellations; FSK: the bit)
    std::vector<std::uint8_t> bits;
    std::vector<double> basebandI, basebandQ;         // after the image filter (display)
    // The output of the matched filter, unit scale, sample after sample (the eye diagram): the root-raised-
    // cosine filter, or for rectangular pulses the mean over one symbol of the baseband; each decision point is
    // its value at decisionTimes[k] (in samples: k fs/Rs for the root raised cosine, the last sample of symbol
    // k for rectangular pulses). Empty for FSK.
    std::vector<double> matchedI, matchedQ;
    std::vector<double> decisionTimes;
    double samplesPerSymbol = 0.0;
    // The rate of matchedI, matchedQ and decisionTimes (the record's, or the synchronized receiver's own).
    double matchedRateHz = 0.0;
    std::vector<std::string> warnings;
    std::string error;

    bool ok() const noexcept { return error.empty(); }
};

DigitalReception receiveDigital(const ReceivedInput &input, const ReceiverSpec &spec);

// The complex baseband of a record received, as receiveDigital starts from it: a passband record mixed down by
// the carrier the receiver knows, 2 x exp(-j (2 pi fc t + phi)), and freed of its 2 fc image by a linear-phase
// low-pass that keeps the band of the modulation (receiverBandEdgeHz); a complex record turned by -phi.
struct Baseband
{
    std::vector<double> inPhase, quadrature;
    std::vector<std::string> warnings;
    std::string error;

    bool ok() const noexcept { return error.empty(); }
};

Baseband toBaseband(const ReceivedInput &input, const ReceiverSpec &spec);
// The band a digital modulation occupies about its carrier: FSK deltaF + Rs, root raised cosine (1 + rolloff) Rs / 2,
// rectangular pulses Rs (their main lobe).
double receiverBandEdgeHz(const ReceiverSpec &spec) noexcept;

struct AnalogReception
{
    std::vector<double> message; // normalised like the transmitted message
    std::vector<std::string> warnings;
    std::string error;

    bool ok() const noexcept { return error.empty(); }
};

AnalogReception receiveAnalog(const ReceivedInput &input, const ReceiverSpec &spec);

struct BitErrors
{
    std::size_t compared = 0;
    std::size_t errors = 0;
    double rate = 0.0;
};

// Compares received bits with the transmitted ones, skipping the first skip bits (filter start-up).
BitErrors countBitErrors(std::span<const std::uint8_t> sent, std::span<const std::uint8_t> received,
                         std::size_t skip = 0);

// Theoretical bit error rate in AWGN for the receivers above, exact (Gray mapping):
// BPSK, QPSK: Q(sqrt(2 Eb/N0)); ASK with the half-way threshold: Q(sqrt(Eb/N0 (1 - l)^2 / (1 + l^2))), l the
// low level (on-off: Q(sqrt(Eb/N0))); FSK non-coherent with orthogonal tones: exp(-Eb/N0 / 2) / 2;
// 8-PSK: the probability of each sector (Craig's integral of the half-planes), each weighted by the bits it
// changes; square QAM: each axis an L-level PAM, every wrong level weighted by its bits. NaN for analog
// modulations.
double theoreticalBer(Modulation modulation, double ebN0Db, double askLowLevel = 0.0) noexcept;
// The same for the symbols (a symbol wrong: one bit or more).
double theoreticalSer(Modulation modulation, double ebN0Db, double askLowLevel = 0.0) noexcept;
// The same through flat fading (a single path whose gain g, of mean power 1, the receiver knows at each symbol):
// the rates at Eb/N0 times |g|^2, averaged over its distribution. riceK (linear) 0: Rayleigh (|g|^2 exponential:
// BPSK gives (1 - sqrt(Γ / (1 + Γ))) / 2, about 1 / (4 Γ) at a high mean Eb/N0 Γ: the deep fades, not the noise,
// make the errors); larger: Rice (a line of sight K times the scattered power: noncentral chi-square), towards the
// rates without fading as K grows. Integrated over the logarithm of |g|^2 (the deep fades resolved at any Eb/N0).
double fadingBer(Modulation modulation, double ebN0Db, double askLowLevel = 0.0, double riceK = 0.0) noexcept;
double fadingSer(Modulation modulation, double ebN0Db, double askLowLevel = 0.0, double riceK = 0.0) noexcept;

// Gaussian tail Q(x) = 0.5 erfc(x / sqrt(2)).
double gaussianQ(double x) noexcept;
} // namespace qacq::dsp
