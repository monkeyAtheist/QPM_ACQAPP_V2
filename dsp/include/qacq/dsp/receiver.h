#pragma once

// Receivers for the modulations of modulation.h, and bit error rates.
//
// Digital, coherent (carrier frequency, phase and symbol timing known, as in the old laboratory):
//   1. back to complex baseband: r = 2 x exp(-j (2 pi fc t + phi)) then a linear-phase low-pass filter
//      that removes the 2 fc image (passband), or r = b exp(-j phi) (complex baseband);
//   2. matched filter: mean over each symbol (rectangular pulses) or root-raised-cosine filter sampled at
//      the symbol centres;
//   3. decisions: sign (BPSK, QPSK), threshold halfway between the two levels (ASK); FSK is detected
//      without phase reference, by comparing the energies at fc + deltaF and fc - deltaF.
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
    std::size_t pulseSpanSymbols = 12; // ISI of the truncated pair below -50 dB
    // Analog.
    double messageBandwidthHz = 3000.0; // low-pass cut-off of the recovered message
    double amIndex = 0.8;
    double frequencyDeviationHz = 5000.0;
    double phaseDeviationRad = 1.0;
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
    std::vector<std::uint8_t> bits;
    std::vector<double> basebandI, basebandQ;         // after the image filter (eye diagram, display)
    std::vector<std::string> warnings;
    std::string error;

    bool ok() const noexcept { return error.empty(); }
};

DigitalReception receiveDigital(const ReceivedInput &input, const ReceiverSpec &spec);

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

// Theoretical bit error rate in AWGN for the receivers above:
// BPSK, QPSK: Q(sqrt(2 Eb/N0)); ASK on-off with the half-way threshold: Q(sqrt(Eb/N0));
// FSK non-coherent with orthogonal tones: exp(-Eb/N0 / 2) / 2. NaN for analog modulations.
double theoreticalBer(Modulation modulation, double ebN0Db) noexcept;

// Gaussian tail Q(x) = 0.5 erfc(x / sqrt(2)).
double gaussianQ(double x) noexcept;
} // namespace qacq::dsp
