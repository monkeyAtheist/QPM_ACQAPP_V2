#pragma once

// Analog and digital modulations (AM, FM, PM, ASK, FSK, BPSK, QPSK), as a real passband signal or as a
// complex baseband (I/Q) signal.
//
// Conventions (kept from the old application):
//   passband(t) = Re{ b(t) exp(j 2 pi fc t) },  b = I + jQ is the complex baseband signal, which already
//   contains the carrier phase: b = envelope * exp(j phi). In ComplexBaseband mode fc is not synthesised.
//   BPSK: bit 0 -> +1, bit 1 -> -1. QPSK (Gray): bits (b0 b1) 00 -> (+,+), 01 -> (-,+), 11 -> (-,-),
//   10 -> (+,-), each component 1/sqrt(2). ASK: bit 1 -> A, bit 0 -> askLowLevel * A. FSK: continuous
//   phase, f = fc + deltaF (bit 1) or fc - deltaF (bit 0).
// Symbol k covers the samples n with floor(n * Rs / fs) = k (rectangular pulses; any fs / Rs ratio).
// Root-raised-cosine pulses need an integer number of samples per symbol.

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qacq::dsp
{
enum class Modulation
{
    Am,
    Fm,
    Pm,
    Ask,
    Fsk,
    Bpsk,
    Qpsk
};

enum class Representation
{
    RealPassband,
    ComplexBaseband
};

enum class PulseShape
{
    Rectangular,
    RootRaisedCosine
};

std::string_view modulationName(Modulation modulation) noexcept;
bool isAnalog(Modulation modulation) noexcept;
int bitsPerSymbol(Modulation modulation) noexcept; // 0 for analog modulations

// ------------------------------------------------------------------ payloads (one bit per byte, 0 or 1)
std::vector<std::uint8_t> bitsFromText(std::string_view text); // keeps the '0' and '1' characters
std::vector<std::uint8_t> bitsFromBytesMsbFirst(std::span<const std::uint8_t> bytes);
// Serial line framing 8N1: start bit 0, 8 data bits LSB first, stop bit 1 (what a UART sends).
std::vector<std::uint8_t> uart8n1(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> randomBits(std::size_t count, std::uint64_t seed);

// Root-raised-cosine impulse response, span symbols long (span * sps + 1 taps, one more if that is even),
// sum of squares = 1.
std::vector<double> rootRaisedCosine(double rolloff, std::size_t samplesPerSymbol, std::size_t spanSymbols);

struct ModulationSpec
{
    Modulation type = Modulation::Bpsk;
    Representation representation = Representation::RealPassband;
    double sampleRateHz = 48000.0;
    std::size_t sampleCount = 48000;
    double carrierFrequencyHz = 12000.0;
    double carrierAmplitude = 1.0;
    double carrierPhaseDegrees = 0.0;

    // Analog: message m(t) normalised to |m| <= 1, at sampleRateHz, repeated if shorter than the record.
    // Empty: a sine at messageFrequencyHz.
    std::vector<double> message;
    double messageFrequencyHz = 1000.0;
    double amIndex = 0.8;              // envelope A (1 + index m)
    double frequencyDeviationHz = 5000.0;
    double phaseDeviationRad = 1.0;

    // Digital.
    std::vector<std::uint8_t> bits; // repeated when repeatBits, else padded with 0
    bool repeatBits = true;
    double symbolRate = 2000.0;
    double askLowLevel = 0.0;
    double fskDeviationHz = 1000.0;
    PulseShape pulse = PulseShape::Rectangular;
    double rolloff = 0.35;
    std::size_t pulseSpanSymbols = 12; // ISI of the truncated pair below -50 dB
};

struct ModulatedSignal
{
    std::vector<double> passband;            // RealPassband only
    std::vector<double> inPhase, quadrature; // complex baseband b(t) (both representations)
    std::vector<double> message;             // analog: the message actually used
    std::vector<std::uint8_t> bits;          // digital: the bits actually sent, in order
    std::vector<std::complex<double>> symbols; // digital: transmitted constellation points (unit scale)
    double samplesPerSymbol = 0.0;
    double bitRate = 0.0;
    double occupiedBandwidthHz = 0.0; // estimate: Carson for FM, (1 + rolloff) Rs for RRC, 2 Rs main lobe...
    std::vector<std::string> warnings;
    std::string error;

    bool ok() const noexcept { return error.empty() && !inPhase.empty(); }
};

ModulatedSignal modulate(const ModulationSpec &spec);

// The complex envelope b of an analog modulation (AM, FM, PM), sample after sample, of a message m (|m| <= 1
// is the usual scale): AM amplitude (1 + amIndex m); FM amplitude exp(j theta), theta advanced by
// 2 pi deltaF m / fs after each sample (from 0); PM amplitude exp(j phaseDeviation m). The passband is
// Re{b exp(j (2 pi fc t + phi))}. modulate() goes through it, so does the generator of the application (a
// modulated carrier): one place for these formulas.
class AnalogModulator
{
public:
    AnalogModulator(Modulation type, double sampleRateHz, double amplitude, double amIndex, double frequencyDeviationHz,
                    double phaseDeviationRad) noexcept;
    std::complex<double> next(double message) noexcept;

private:
    Modulation m_type;
    double m_sampleRate, m_amplitude, m_amIndex, m_deviation, m_phaseDeviation;
    double m_theta = 0.0;
};
} // namespace qacq::dsp
