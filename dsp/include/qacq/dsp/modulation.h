#pragma once

// Analog and digital modulations (AM, FM, PM, ASK, FSK, BPSK, QPSK, 8-PSK, 16-, 64- and 256-QAM), as a real
// passband signal or as a complex baseband (I/Q) signal.
//
// Conventions (kept from the old application):
//   passband(t) = Re{ b(t) exp(j 2 pi fc t) },  b = I + jQ is the complex baseband signal, which already
//   contains the carrier phase: b = envelope * exp(j phi). In ComplexBaseband mode fc is not synthesised.
//   BPSK: bit 0 -> +1, bit 1 -> -1. QPSK (Gray): bits (b0 b1) 00 -> (+,+), 01 -> (-,+), 11 -> (-,-),
//   10 -> (+,-), each component 1/sqrt(2). ASK: bit 1 -> A, bit 0 -> askLowLevel * A. FSK: continuous
//   phase, f = fc + deltaF (bit 1) or fc - deltaF (bit 0).
//   8-PSK (Gray): the point at angle p pi/4 carries the bits p xor (p >> 1) (000, 001, 011, 010, 110, 111,
//   101, 100 around the circle: neighbours differ by one bit). Square QAM (Gray): the first half of the bits
//   of a symbol sets I, the second half Q, each a Gray-coded level among -(L-1) ... -1, +1 ... L-1 (L = sqrt M),
//   scaled to a mean energy of 1. The bits of a symbol are read most significant first.
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
    Qpsk,
    Psk8,
    Qam16,
    Qam64,
    Qam256
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

// The digital modulations whose symbols are points of a constellation, decided by the nearest one (ASK, BPSK,
// QPSK, 8-PSK, QAM; not FSK, detected by the energies of its two tones).
bool hasConstellation(Modulation modulation) noexcept;
// The angle after which its constellation falls back on itself, in degrees: 180 (BPSK), 90 (QPSK, QAM), 45
// (8-PSK); 0 for the others (ASK, FSK, analog). What a carrier recovered from the data alone cannot tell apart:
// a carrier loop that loses its lock comes back on one of these symmetries (a slip).
double rotationalSymmetryDegrees(Modulation modulation) noexcept;
// Its points, at a unit amplitude: index = the value of the bits of the symbol read most significant first
// (ASK: {askLowLevel, 1}; the PSK and QAM at a mean energy of 1). Empty for the others.
std::vector<std::complex<double>> constellation(Modulation modulation, double askLowLevel = 0.0);
// The symbol decided for a point received at that scale: the nearest point of the constellation (QAM: each
// axis on its own; PSK: the sector of its angle; ASK: the threshold half-way).
std::size_t decideSymbol(Modulation modulation, std::complex<double> point, double askLowLevel = 0.0) noexcept;
// Gray code of a number, and back (gray(p) = p xor (p >> 1)).
std::size_t grayCode(std::size_t value) noexcept;
std::size_t grayDecode(std::size_t code) noexcept;

// ------------------------------------------------------------------ payloads (one bit per byte, 0 or 1)
std::vector<std::uint8_t> bitsFromText(std::string_view text); // keeps the '0' and '1' characters
std::vector<std::uint8_t> bitsFromBytesMsbFirst(std::span<const std::uint8_t> bytes);
// Serial line framing 8N1: start bit 0, 8 data bits LSB first, stop bit 1 (what a UART sends).
std::vector<std::uint8_t> uart8n1(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> randomBits(std::size_t count, std::uint64_t seed);

// Root-raised-cosine impulse response, span symbols long (span * sps + 1 taps, one more if that is even),
// sum of squares = 1.
std::vector<double> rootRaisedCosine(double rolloff, std::size_t samplesPerSymbol, std::size_t spanSymbols);
// What the pair of them (the transmitter's pulse and the matched filter, both cut to spanSymbols) leaves of the
// other symbols at the decision instants, against the symbol itself, in dB (-infinity when nothing). Cut short, a
// slowly decaying pulse (a small roll-off) no longer makes quite a Nyquist pair: -17.6 dB at 0.01 and -25 dB at
// 0.1 when cut to 12 symbols, against -56 dB at 0.35.
double rootRaisedCosineInterferenceDb(double rolloff, std::size_t samplesPerSymbol, std::size_t spanSymbols);
// The span (symbols) the root raised cosine of a roll-off is cut to when none is given: the shortest even one from
// kMinimumPulseSpanSymbols on whose pair leaves at most kPulseInterferenceTargetDb (reckoned at 16 samples per
// symbol), within kMaximumPulseSpanSymbols, where the smallest roll-offs (under about 0.02) stop short of it: 12 at
// 0.35, 22 at 0.1, 44 at 0.05. The same whatever the samples per symbol: the transmitter and the receivers (8 samples
// per symbol for the synchronized one) cut their pulses alike. Not monotonic: 26 at 0.15, but 12 at 0.18 (a span
// that ends near a zero of the tails leaves less).
std::size_t rootRaisedCosineSpan(double rolloff);
// The span of a pulse: the one given, or rootRaisedCosineSpan(rolloff) for 0.
std::size_t pulseSpanSymbolsOf(std::size_t given, double rolloff);
inline constexpr std::size_t kMinimumPulseSpanSymbols = 12;
inline constexpr std::size_t kMaximumPulseSpanSymbols = 96;
inline constexpr double kPulseInterferenceTargetDb = -50.0;

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
    std::size_t pulseSpanSymbols = 0; // symbols the root raised cosine is cut to; 0: rootRaisedCosineSpan(rolloff)
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
