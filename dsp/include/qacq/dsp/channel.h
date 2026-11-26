#pragma once

// What lies between a transmitter and its receiver: several paths (echoes), each fading or not, the carrier off in
// frequency and phase, a delay, a clock that runs at another rate (applyChannel); then additive white Gaussian
// noise, set by a signal-to-noise ratio or by Eb/N0 (addAwgn).
//
// The signal power P is measured on the record (or given: the power of the transmission itself, when the
// record also holds silence). With Eb/N0: Eb = P / Rb and N0 = Eb / (Eb/N0); a real
// sampled record gets noise of variance N0 * fs / 2 (two-sided density N0/2 over -fs/2..fs/2), a complex
// record N0 * fs / 2 on each of I and Q (N0 * fs in total). With an SNR the noise power is P / SNR over
// the whole sampled band (0..fs/2 for a real record, -fs/2..fs/2 for a complex one).

#include "qacq/dsp/rotator.h"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace qacq::dsp
{
struct AwgnSpec
{
    enum class Level
    {
        SnrDb,
        EbN0Db
    };
    Level level = Level::EbN0Db;
    double valueDb = 10.0;
    double bitRate = 0.0; // needed for Eb/N0
    std::uint64_t seed = 1;
    // The power of the signal while it is sent; 0: the mean square of the whole record.
    double referencePower = 0.0;
};

struct AwgnResult
{
    double signalPower = 0.0;    // mean square of the record (I^2 + Q^2 for a complex record)
    double noiseVariance = 0.0;  // per real sample, or per component for a complex record
    double n0 = 0.0;             // one-sided noise density, unit^2/Hz
    double measuredSnrDb = 0.0;  // from the noise actually drawn
    double measuredEbN0Db = 0.0; // idem, when a bit rate is known
    std::string error;

    bool ok() const noexcept { return error.empty(); }
};

// ------------------------------------------------------------------ channel
// On the complex envelope b of a transmission (modulate(): inPhase, quadrature), the record received:
//   y[n] = sum_k g_k b(p(n) - tau_k) exp(j (2 pi df n / fs + phi)),   p(n) = n / (1 + clock) - delay
// b counted in its samples and rebuilt between them by the sinc interpolator (0 outside its record): the
// transmission arrives delaySamples late; the receiver's clock runs (1 + clock) times as fast as the
// transmitter's (clock = clockOffsetPpm 1e-6: each symbol lasts 1 + clock times as many of its samples); its
// carrier is off by frequencyOffsetHz (oscillators, Doppler) and phaseOffsetDegrees. Several paths: each a copy
// tau_k samples (of the transmission) after the first arrival, of complex gain g_k (gain, phaseDegrees); none
// listed: one path of gain 1. Fading: each g_k times its fading gain at n (FadingGains). The record received holds
// all the transmission (its last path too), then extraSamples more.
struct ChannelPath
{
    double delaySamples = 0.0;
    double gain = 1.0;
    double phaseDegrees = 0.0;
};

inline constexpr std::size_t kMaximumChannelPaths = 16;

// Fading: the gain of each path changes during the transmission, as when many waves arrive from every direction
// at a receiver that moves (or among things that move), each shifted in frequency by up to ±fd, the maximum
// Doppler shift (speed over wavelength): Clarke's model. Rayleigh: no line of sight; the gain is a complex
// Gaussian (its modulus Rayleigh distributed: deep fades now and then), whose spectrum is that of the Doppler
// shifts (autocorrelation J0(2 pi fd tau): it changes over about 1 / fd). Rice: a line of sight on the first path,
// riceK times as strong as the waves scattered around it (K = 0: Rayleigh; large: hardly any fading); the other
// paths Rayleigh. Each path fades on its own; the mean power of each gain is 1 (the powers of the paths stay on
// average what they are given). fd = 0: each gain drawn once, then still. Made as sums of kFadingSinusoids
// sinusoids per path (Doppler shifts fd cos(alpha), alpha evenly spread round the circle from a random start,
// random phases: the seed decides), computed every few samples (64 a period of fd at least) and interpolated
// linearly between.
struct ChannelFading
{
    enum class Kind
    {
        None,
        Rayleigh,
        Rice
    };
    Kind kind = Kind::None;
    double dopplerHz = 0.0;
    double riceK = 0.0; // linear (10^(K_dB / 10))
    std::uint64_t seed = 1;

    bool active() const noexcept { return kind != Kind::None; }
};
inline constexpr std::size_t kFadingSinusoids = 32;

// The gains of the paths through time, as applyChannel multiplies the paths by them (and an ideal receiver, which
// knows the channel, divides by them): at received samples taken in increasing order (each instance only moves
// forward; going back starts it again from the beginning).
class FadingGains
{
public:
    FadingGains(const ChannelFading &fading, double sampleRateHz, std::size_t paths);
    // The gain of a path at a received sample (fractional); 1 without fading.
    std::complex<double> at(std::size_t path, double sample);
    bool active() const noexcept { return m_active; }
    std::size_t paths() const noexcept { return m_paths; }
    // Rice: the Doppler shift of the line of sight (fd cos of its angle; the first path turns at that rate on
    // average, as a carrier off by it would); 0 otherwise.
    double lineOfSightDopplerHz() const noexcept { return m_lineOfSightHz; }

private:
    struct Sinusoid
    {
        double ratio = 0.0; // cycles per step
        double phase = 0.0;
    };
    void restart();
    void advance();                              // the values one step on
    std::vector<std::complex<double>> nextValues(); // the values at the step the oscillators are at, then one step on

    bool m_active = false;
    std::size_t m_paths = 0;
    double m_step = 1.0;                  // samples between two values
    double m_scattered = 1.0, m_los = 0.0; // amplitudes on the first path (Rice)
    double m_lineOfSightHz = 0.0;
    std::vector<Sinusoid> m_sinusoids;    // kFadingSinusoids per path
    Sinusoid m_lineOfSight;
    std::vector<Rotator> m_oscillators;   // one per sinusoid, then the line of sight
    std::size_t m_index = 0;              // the values at m_index and m_index + 1 are known
    std::vector<std::complex<double>> m_now, m_next;
};

struct ChannelSpec
{
    double sampleRateHz = 48000.0;
    double frequencyOffsetHz = 0.0;
    double phaseOffsetDegrees = 0.0;
    double delaySamples = 0.0;
    double clockOffsetPpm = 0.0;
    std::size_t extraSamples = 0;
    std::vector<ChannelPath> paths;
    ChannelFading fading;

    // One path, as sent (none listed, or a single one neither late, nor scaled, nor turned).
    bool singlePath() const noexcept
    {
        return paths.empty() ||
               (paths.size() == 1 && paths.front().delaySamples == 0.0 && paths.front().gain == 1.0 && paths.front().phaseDegrees == 0.0);
    }
    bool identity() const noexcept
    {
        return frequencyOffsetHz == 0.0 && phaseOffsetDegrees == 0.0 && delaySamples == 0.0 && clockOffsetPpm == 0.0 &&
               extraSamples == 0 && singlePath() && !fading.active();
    }
};

struct ChannelOutput
{
    std::vector<double> inPhase, quadrature;
    std::string error;

    bool ok() const noexcept { return error.empty(); }
};

ChannelOutput applyChannel(std::span<const double> inPhase, std::span<const double> quadrature, const ChannelSpec &spec);
// The position in the transmission (its samples) that sample n of the record received shows, and back.
double transmittedPosition(const ChannelSpec &spec, double receivedSample) noexcept;
double receivedPosition(const ChannelSpec &spec, double transmittedSample) noexcept;
// The real passband of a complex envelope: Re{b exp(j 2 pi fc n / fs)} (as modulate() makes it).
std::vector<double> toPassband(std::span<const double> inPhase, std::span<const double> quadrature, double sampleRateHz,
                               double carrierHz);

// ------------------------------------------------------------------ noise
AwgnResult addAwgn(std::vector<double> &signal, double sampleRateHz, const AwgnSpec &spec);
AwgnResult addAwgn(std::vector<double> &inPhase, std::vector<double> &quadrature, double sampleRateHz,
                   const AwgnSpec &spec);
} // namespace qacq::dsp
