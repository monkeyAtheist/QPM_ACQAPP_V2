#pragma once

// Signal generator.
//
// Periodic waveforms are computed from an exact phase u = frac(f n / fs + phase / 360) (no running
// accumulator, so no drift over long records). Two ways to sample them:
//  - BandLimit::Sampled: the ideal waveform is sampled as is. Its harmonics above fs/2 fold back into the
//    band: this is the aliasing an ADC would show (a 1.3 kHz square at 48 kHz has its folded power only
//    16.4 dB under the harmonics, the strongest folded line at -25.6 dB; audit of 20/11/2026).
//  - BandLimit::Fourier: the Fourier series truncated below fs/2 (or to maximumHarmonics): no aliasing,
//    Gibbs oscillations visible at the edges. This is what an ideal anti-aliasing filter would leave.
// Phase convention: every periodic waveform starts like a sine (value 0 rising at u = 0), except the
// square and the pulse, which are high on [0, duty).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qacq::dsp
{
enum class Waveform
{
    Sine,
    Square,   // +A on [0, duty), -A after
    Triangle,
    Sawtooth, // rising from -A to +A, jump at u = 1/2 (starts at 0 like a sine)
    Pulse,    // A on [0, duty), 0 after
    Dc,       // constant A
    Step,     // 0 before eventTime, A after
    Impulse,  // A at the sample nearest eventTime, 0 elsewhere
    WhiteNoise,
    Chirp     // sine sweeping from frequencyHz to chirpEndFrequencyHz over the record
};

enum class BandLimit
{
    Sampled,
    Fourier
};

enum class NoiseDistribution
{
    Gaussian, // amplitude = standard deviation
    Uniform   // amplitude = peak value (uniform in [-A, A))
};

enum class ChirpLaw
{
    Linear,
    Logarithmic
};

std::string_view waveformName(Waveform waveform) noexcept;
bool isPeriodic(Waveform waveform) noexcept;

struct WaveformSpec
{
    Waveform waveform = Waveform::Sine;
    double amplitude = 1.0;
    double frequencyHz = 100.0;
    double phaseDegrees = 0.0;
    double offset = 0.0; // added to every waveform
    double dutyCycle = 0.5;
    double sampleRateHz = 10000.0;
    std::size_t sampleCount = 1000;
    BandLimit bandLimit = BandLimit::Sampled;
    std::size_t maximumHarmonics = 0; // Fourier mode: 0 = every harmonic below fs/2
    double eventTimeSeconds = 0.0;    // step and impulse
    double chirpEndFrequencyHz = 1000.0;
    ChirpLaw chirpLaw = ChirpLaw::Linear;
    NoiseDistribution noise = NoiseDistribution::Gaussian;
    std::uint64_t seed = 1;
};

inline constexpr std::size_t kMaximumGeneratedSamples = 100'000'000;

struct GeneratedSignal
{
    std::vector<double> samples;
    std::size_t harmonicsUsed = 0;     // Fourier mode
    std::vector<std::string> warnings; // in French, for the interface
    std::string error;

    bool ok() const noexcept { return error.empty() && !samples.empty(); }
};

GeneratedSignal generate(const WaveformSpec &spec);

// Fourier series of a periodic waveform of unit amplitude, in the phase u of the generator:
//   x(u) = dc + sum_k cosine_k cos(2 pi k u) + sine_k sin(2 pi k u),  k = 1..harmonics.
struct FourierSeries
{
    double dc = 0.0;
    std::vector<double> cosine; // index k - 1
    std::vector<double> sine;
};
FourierSeries fourierSeries(Waveform waveform, double dutyCycle, std::size_t harmonics);

// Sum of sinusoids (Fourier composition): offset + sum A_i sin(2 pi f_i t + phase_i).
struct SineComponent
{
    double amplitude = 1.0;
    double frequencyHz = 100.0;
    double phaseDegrees = 0.0;
    bool enabled = true;
};
std::vector<double> synthesize(std::span<const SineComponent> components, double offset, double sampleRateHz,
                               std::size_t sampleCount);
} // namespace qacq::dsp
