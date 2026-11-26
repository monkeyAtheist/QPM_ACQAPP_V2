#pragma once

// The generator of the application: a signal of one or more channels, each the sum of components (the
// waveforms of qacq::dsp::generate: sine, square, triangle, sawtooth, pulses, constant, step, Dirac
// impulse, white noise, sweep; and Fourier series whose coefficients are formulas of n) and of an offset.
// How a signal is made (its recipe) is kept in the provenance of each signal made from it, so that it can
// be taken up again.
//
// Noise: component k of channel c draws from seed + 1000 c + k: the same recipe gives the same samples,
// and two noise components are independent.
//
// Fourier series: x(t) = a₀ + Σₙ aₙ cos(nω₀t) + bₙ sin(nω₀t), n = 1..N, the coefficients typed as formulas
// (qacq/analysis/expression.h) of n (also N or k), Nmax (= N), f0, w0 (ω0), T0, fs and the variables of the
// component ("E = 1 ; D = 0,25"). Or the complex coefficients Cₙ of a real signal (C₋ₙ = Cₙ*): C₀ = a₀,
// Cₙ = (aₙ − j bₙ)/2, typed as Re Cₙ and Im Cₙ, or |Cₙ| and arg Cₙ in degrees.
//
// Drawn motif: points (x across the period, y from −1 to 1) joined by steps, segments or a smooth curve,
// repeated at its frequency, or drawn once over the whole signal. Band-limited, it is its exact Fourier
// series below fs/2 (each piece is a polynomial).
//
// Recurrence: y[n] typed as a formula of the values before it (y[n−1]…, x[n], x[n−1]…), of n, t, fs, Te, N
// and variables, from initial values y[−1]…, driven by an input x[n] (none, Dirac, step, white noise, or the
// components above it in the channel, which y[n] then replaces: it filters them).
//
// Operation between channels: y typed as a formula of the other channels v1…v16 (each as made, offset
// included), of x (the components above it in its channel, which y then replaces), of n, t, fs, Te, N and of
// variables: "v1*v2", "(v1 + v2)/2", "sin(2*pi*50*t)^3". A channel is made after those it reads; a channel
// cannot read itself, even through others.
//
// Modulated carrier: A cos(2π fc t + φ) modulated in amplitude, frequency or phase by a message typed as an
// operation ("sin(2*pi*fm*t)", "v1", "x"), through qacq::dsp::AnalogModulator (the module Modulations uses
// the same); or in amplitude without its carrier (double sideband: A m cos(2π fc t + φ)), or keeping one
// sideband only (single sideband: A (m cos(θ) ∓ m̂ sin(θ)), m̂ the Hilbert transform of the message, taken by
// the DFT of the whole message).
//
// Recorded signal: a channel of a signal of the session (generateSignal is given them), brought to the rate
// of the recipe (analysis::resample) from an instant of the record, placed after a delay, once (0 after its
// end) or in a loop, times a gain or scaled to a peak. The component names its signal by its id in the
// session, and keeps its name, the name of the channel and its number of samples to find it again (a recipe
// taken up in another session must not read a stranger of the same id).
//
// Digital frame: data typed (bits, hexadecimal, text) as two levels, as they are or framed by a protocol (UART,
// I2C, SPI, USB: qacq/analysis/frames.h), at a bit rate, from an instant, once or again and again. A component is
// one line of the frame (the data, the clock, SDA, SCL…); the other lines are other components, in other
// channels, of the same group: they share everything but their line (spreadFrame, addFrameLines).
//
// A channel may be left out of the signal (GeneratorChannel::output false): made as the others, read by their
// operations, but not among the channels of the result (a carrier, a message, a step of a computation).

#include "qacq/analysis/frames.h"
#include "qacq/dsp/generator.h"
#include "qacq/model/json.h"
#include "qacq/model/progress.h"
#include "qacq/model/signal.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qacq::analysis
{
// The kinds of component: the waveforms of qacq::dsp::generate, then the Fourier series made here.
enum class WaveKind
{
    Sine,
    Square,
    Triangle,
    Sawtooth,
    Pulse,
    Dc,
    Step,
    Impulse,
    Noise,
    Sweep,
    FourierSeries,
    Drawn,
    Recurrence,
    Formula,    // operation between channels
    Modulation, // modulated carrier
    Recorded,   // a channel of a signal of the session
    Frame       // a line of a digital frame
};

// How a carrier is modulated by its message m: amplitude A (1 + index m), frequency fc + Δf m, phase φ + Δφ m;
// amplitude without the carrier (A m: double sideband), one sideband only (upper, lower).
enum class CarrierModulation
{
    Am,
    Fm,
    Pm,
    Dsb,
    Usb,
    Lsb
};

enum class SeriesForm
{
    SineCosine, // a₀, aₙ, bₙ
    Cartesian,  // C₀, Re Cₙ, Im Cₙ
    Polar       // C₀, |Cₙ|, arg Cₙ (degrees)
};

// How the points of a drawn motif are joined: each held until the next (steps), straight segments, or a
// smooth curve through them (monotone cubic, Fritsch–Carlson: never beyond its points, flat at a peak).
enum class MotifInterpolation
{
    Steps,
    Linear,
    Smooth
};

// A point of a drawn motif: x from 0 to 1 across the period (or the signal), y from −1 to 1 (times the
// amplitude).
struct MotifPoint
{
    double x = 0.0;
    double y = 0.0;
    friend bool operator==(const MotifPoint &, const MotifPoint &) = default;
};

inline constexpr std::size_t kMaximumMotifPoints = 1024;

// The input x[n] of a recurrence: nothing (0), a Dirac (1 at n = 0), a step (1 from n = 0), a Gaussian white
// noise (standard deviation 1), or the sum of the components above it in the channel (which y[n] replaces).
enum class RecurrenceInput
{
    None,
    Impulse,
    Step,
    Noise,
    Above
};

inline constexpr std::size_t kRecurrenceDepth = 16; // y[n−16] and x[n−16] at most

struct GeneratorComponent
{
    WaveKind waveform = WaveKind::Sine;
    bool enabled = true;
    double amplitude = 1.0;          // peak; constant: its value; noise: standard deviation (Gaussian) or peak (uniform)
    double frequencyHz = 1000.0;     // periodic waveforms; start of a sweep; fundamental of a series
    double phaseDegrees = 0.0;       // periodic waveforms and sweep
    double dutyCycle = 0.5;          // square, pulses
    double eventSeconds = 0.0;       // step, Dirac: from the start of the signal
    double endFrequencyHz = 10000.0; // end of a sweep
    dsp::ChirpLaw sweepLaw = dsp::ChirpLaw::Linear;
    dsp::NoiseDistribution noise = dsp::NoiseDistribution::Gaussian;
    // Square, triangle, sawtooth, pulses: sampled as they are (harmonics above fs/2 fold back), or their
    // Fourier series below fs/2 (no aliasing, Gibbs ripples), limited to `harmonics` terms (0: all).
    // A Fourier series: `harmonics` terms (0: all below fs/2); Fourier: those at or above fs/2 left out,
    // Sampled: they fold back.
    dsp::BandLimit bandLimit = dsp::BandLimit::Sampled;
    std::size_t harmonics = 0;
    // A Fourier series: the form of its coefficients, its variables, its three formulas (a₀, aₙ, bₙ; or
    // C₀ and the two parts of Cₙ).
    SeriesForm seriesForm = SeriesForm::SineCosine;
    std::string variables;
    std::string formula0 = "0";
    std::string formulaA = "0";
    std::string formulaB = "0";
    // A drawn motif: its points (normalizedMotif: in the order of x, two at the same x a jump), how they are
    // joined, and whether it is drawn once over the whole signal (frequency and phase unused) rather than
    // at each period. Periodic, `bandLimit` and `harmonics` as for a square.
    std::vector<MotifPoint> motif;
    MotifInterpolation interpolation = MotifInterpolation::Linear;
    bool motifOnce = false;
    // A recurrence: the formula of y[n] (also typed "y[n] = …"), with y[n-1]…y[n-16], x[n], x[n-1]…x[n-16], n,
    // t (n/fs), fs, Te (1/fs), N (samples) and its `variables`; its initial values ("y[-1] = 1 ; y[-2] = 0", the
    // others 0); its input.
    std::string recurrence = "0";
    std::string initialValues;
    RecurrenceInput recurrenceInput = RecurrenceInput::None;
    // An operation between channels: y (also typed "y = …"), a formula of v1…v16 (also V1…), x, n, t, fs, Te,
    // N and its `variables`.
    std::string expression = "x";
    // A modulated carrier: amplitude, frequencyHz and phaseDegrees are those of the carrier; its message m (a
    // formula as an operation, also typed "m = …") modulates it in amplitude (amIndex), frequency (deviationHz
    // per unit of m) or phase (phaseDeviationDegrees per unit of m).
    CarrierModulation modulation = CarrierModulation::Am;
    std::string message = "sin(2*pi*fm*t)";
    double amIndex = 0.5;
    double deviationHz = 1000.0;
    double phaseDeviationDegrees = 90.0;
    // A recorded signal: which one (setRecordedSource fills these five), from which instant of the record,
    // once or in a loop; `amplitude` is a gain (times), or the peak it is scaled to (sourceNormalize: the
    // peak of the part read); `eventSeconds` is where it starts in the signal made (a delay; rounded to a
    // sample).
    std::uint64_t sourceId = 0;      // of the signal, in its session
    std::string sourceName;          // of the signal
    std::size_t sourceFrames = 0;    // its samples per channel
    std::size_t sourceChannel = 0;
    std::string sourceChannelName;
    double sourceStartSeconds = 0.0;
    bool sourceLoop = false;
    bool sourceNormalize = false;
    // A digital frame: what it carries and how; which of its lines this component is (a key of frameLines; empty
    // or unknown: the first); `amplitude` is the level of a 1 and `frameLow` that of a 0, `frequencyHz` its bit
    // rate (bit/s), `eventSeconds` the instant it starts at; once, or again and again with `frameGapBits` bits of
    // rest between two frames. Components of the same group (frameGroup, not 0) are the lines of one frame.
    FrameSpec frame;
    std::string frameLine;
    double frameLow = 0.0;
    bool frameRepeat = false;
    double frameGapBits = 10.0;
    std::uint64_t frameGroup = 0;
};

struct GeneratorChannel
{
    std::string name = "Voie 1";
    std::string unit = "V";
    double offset = 0.0; // added to the sum
    bool output = true;  // among the channels of the signal made (false: made, read by the others, left out)
    std::vector<GeneratorComponent> components;
};

struct GeneratorRecipe
{
    std::string name = "Signal généré";
    double sampleRateHz = 48000.0;
    double durationSeconds = 1.0;
    std::uint64_t seed = 1;
    std::vector<GeneratorChannel> channels;
};

inline constexpr std::size_t kMaximumGeneratorSamples = 50'000'000; // every channel together (400 Mo)
inline constexpr std::size_t kMaximumGeneratorChannels = 16;
inline constexpr std::size_t kMaximumGeneratorComponents = 32; // per channel
// A single sideband: samples at most (the Hilbert transform of its message is taken over the whole signal).
inline constexpr std::size_t kMaximumSidebandSamples = std::size_t{1} << 20;
// Fourier series: harmonics × samples of a component at most (about a second of computation); beyond, the
// series is shortened and a warning says so.
inline constexpr double kGeneratorFourierBudget = 4e8;

// A blank page: one channel "Voie 1" (V) without any waveform (worth its offset, 0), at 48 kHz for 1 s. What the
// module opens on, and what "Par défaut" brings back.
GeneratorRecipe blankGeneratorRecipe();
// A new channel: named after its place (index 0: "Voie 1"), without any waveform.
GeneratorChannel blankGeneratorChannel(std::size_t index);
// A channel without waveform nor offset (it is 0); a recipe whose channels all are (nothing in it yet).
bool isBlankChannel(const GeneratorChannel &channel) noexcept;
bool isBlankRecipe(const GeneratorRecipe &recipe) noexcept;
// One channel "Voie 1" (V) holding a sine of 1 kHz and 1 V, at 48 kHz for 1 s (the example "Sinus 1 kHz").
GeneratorRecipe sineGeneratorRecipe();
// A channel named after its place (index 0: "Voie 1"), a sine suited to the rate.
GeneratorChannel sineGeneratorChannel(std::size_t index, double sampleRateHz = 48000.0);
// A new component of that waveform with usual values for a signal of that rate and duration: 1 kHz (at
// most fs/20), noise of 0.1, events at a tenth of the duration, a sweep of 20 Hz to 20 kHz (at most 0.45 fs);
// a Fourier series: the square wave ±E (E = 1), 15 harmonics, none at or above fs/2; a drawn motif: a
// trapezoid; a recurrence: the oscillator of that frequency.
GeneratorComponent defaultGeneratorComponent(WaveKind waveform, double sampleRateHz = 48000.0,
                                             double durationSeconds = 1.0);

// The component with another waveform: what both share is kept (the amplitude; frequency and phase from
// one periodic waveform to another; the duty cycle; the Fourier series; the instant of a step or a Dirac),
// the rest takes the usual values of defaultGeneratorComponent. A square, triangle, sawtooth or pulses
// made a Fourier series: its coefficients (seriesModel), of the same amplitude and duty cycle, all the
// harmonics below fs/2. Made a drawn motif: its points (the same samples; a sine: 16 points, smooth). A
// sine made a recurrence: the oscillator of its amplitude and frequency.
GeneratorComponent withWaveform(const GeneratorComponent &component, WaveKind waveform,
                                double sampleRateHz = 48000.0, double durationSeconds = 1.0);

// Coefficients of a Fourier series as formulas of a known waveform, for the variables E (amplitude) and D
// (duty cycle): "square" (±E, high for D of the period), "triangle" (±E), "sawtooth" (−E to +E), "pulse" (E for
// D of the period),
// "fullwave" (E |sin|, f0 twice that of the sine), "halfwave" (E sin, its positive half). The form becomes
// a₀/aₙ/bₙ; false for an unknown key.
bool applySeriesModel(GeneratorComponent &component, std::string_view key, double amplitude = 1.0,
                      double dutyCycle = 0.5);

// The same series written in another form: its formulas become those of the same coefficients
// (Re Cₙ = aₙ/2, Im Cₙ = −bₙ/2; |Cₙ| = √(aₙ² + bₙ²)/2, arg Cₙ = deg(atan2(−bₙ; aₙ))), kept short: the
// number in front scaled ("4*E/(pi*n)*odd(n)" → Im Cₙ = "-2*E/(pi*n)*odd(n)"), one term of one sign
// in polar form a constant phase (bₙ ≥ 0 → |Cₙ| = bₙ/2 at −90°, evaluated for n = 1..N at that rate), and
// back to the form before, the formulas as they were.
GeneratorComponent withSeriesForm(const GeneratorComponent &component, SeriesForm form, double sampleRateHz = 48000.0);

// The points as a drawn motif keeps them: finite, x within [0, 1] and y within [−1, 1], in the order of x
// (points of the same x keep their order: a jump), at most two at the same x (the first and the last), at
// most kMaximumMotifPoints (the first ones).
std::vector<MotifPoint> normalizedMotif(std::vector<MotifPoint> points);

// A motif (normalized points) as a function of u, the place in the period, or in the signal, from 0 to 1.
class MotifShape
{
public:
    MotifShape(std::span<const MotifPoint> points, MotifInterpolation interpolation, bool periodic);
    // Periodic: u modulo 1, the last point joined to the first one of the next period; once: before the
    // first point its value, after the last one its value. Right after a jump, the value after it. No point: 0.
    double operator()(double u) const noexcept;
    double mean() const noexcept; // over a period, or over [0, 1]
    // Periodic: x(u) = dc + Σ cosine_k cos(2πku) + sine_k sin(2πku), k = 1..count, exact: from the jumps of
    // the value and of its derivatives where the pieces meet.
    dsp::FourierSeries fourierSeries(std::size_t count) const;

private:
    struct Piece
    {
        double x0, x1;         // x1 > x0
        double c0, c1, c2, c3; // c0 + c1 t + c2 t² + c3 t³, t = u − x0
        double at(double t) const noexcept { return c0 + t * (c1 + t * (c2 + t * c3)); }
    };
    std::vector<Piece> m_pieces; // periodic: covering [x of the first point, + 1)
    bool m_periodic = true;
    double m_start = 0.0, m_end = 0.0;   // x of the first and of the last point
    double m_first = 0.0, m_last = 0.0; // y of the first and of the last point
};

// A stroke drawn across a motif with the mouse (points in the order drawn): where it passes, it replaces the
// motif (the last pass over an x wins), simplified to within `tolerance` (of the width, and of the height
// from −1 to 1; Ramer–Douglas–Peucker), 256 points at most. One point alone is added.
std::vector<MotifPoint> withStroke(std::span<const MotifPoint> motif, std::span<const MotifPoint> stroke,
                                   double tolerance = 0.002);

// "0 -1 ; 0,1 1 ; 0,5 1 ; 0,6 -1": x and y with 4 decimals at most, a comma, " ; " between the points.
std::string motifText(std::span<const MotifPoint> points);
// Back: points separated by ";" or a new line, x and y by spaces, 0,5 or 0.5; x within [0, 1], y within
// [−1, 1]. False with why (in French), the points unchanged.
bool parseMotif(std::string_view text, std::vector<MotifPoint> &points, std::string &error);

// Ready-made motifs: "ramp" (−1 to +1), "stairs" (4 steps), "trapezoid", "rc" (charge and discharge of a
// capacitor, τ = T/10), "heartbeat" (an electrocardiogram). False for an unknown key.
bool applyMotifModel(GeneratorComponent &component, std::string_view key);

// What is wrong in a recurrence (its formula, its variables, its initial values), in French, or empty:
// "y[n] : Nom inconnu « b » (caractère 3)", "Valeurs initiales : « z » n'est pas une valeur y[-1] à y[-16]".
std::string checkRecurrence(const GeneratorComponent &component, double sampleRateHz);
// Whether its formula reads its input (x[n], x[n-1]…): if not, the input chosen changes nothing.
bool recurrenceUsesInput(const GeneratorComponent &component, double sampleRateHz);
// Ready-made recurrences: "oscillator" (a sine of amplitude A, frequency f and phase: 2 cos(w) y[n-1] − y[n-2]),
// "lowpass" (first order, τ = 1 ms, of a step), "resonator" (its response to a Dirac), "average" (moving
// average of 4 samples of a white noise), "walk" (random walk), "logistic" (r = 3,9: chaos). False for an
// unknown key.
bool applyRecurrenceModel(GeneratorComponent &component, std::string_view key, double amplitude = 1.0,
                          double frequencyHz = 1000.0, double phaseDegrees = 0.0);

// What an operation between channels (or the message of a modulated carrier) reads, and what is wrong in it,
// in French: its formula ("y : Nom inconnu « w » (caractère 3)", "Message : …"), its variables, a channel
// that does not exist, its own channel, a loop through other channels ("Voies 2 → 3 → 2 : …").
struct OperationCheck
{
    std::string error;
    std::vector<std::size_t> channels; // read, from 0
    bool readsAbove = false;           // x: the components above it, which it then replaces
};
OperationCheck checkOperation(const GeneratorRecipe &recipe, std::size_t channel, std::size_t component);
// The formula of a new operation in that channel: the sum of the first two other channels ("v1 + v2"), the
// other one and the components above ("v1 + x"), or with one channel only the components above ("x").
std::string defaultOperation(std::size_t channel, std::size_t channelCount);
// Ready-made operations of a and b, the first two channels other than this one (with one other channel, b is
// x; with none, a is x and only the operations of one value are given): "sum", "difference", "product",
// "ratio", "mean", "magnitude" (√(a² + b²)), "abs", "square", "halfwave" (max(a; 0)). Ready-made messages
// of a modulated carrier: "sine" (sin(2*pi*fm*t)), "square" (sign(sin(2*pi*fm*t))), "above" (x), "v1"…
// (another channel).
struct OperationModel
{
    std::string key;     // "product"
    std::string name;    // "Produit"
    std::string formula; // "v1*v2"
};
std::vector<OperationModel> operationModels(std::size_t channel, std::size_t channelCount);
bool applyOperationModel(GeneratorComponent &component, std::string_view key, std::size_t channel, std::size_t channelCount);
std::vector<OperationModel> messageModels(std::size_t channel, std::size_t channelCount);
// The message of that model; "sine" and "square" give the variables fm (a tenth of the carrier) if they have
// no fm.
bool applyMessageModel(GeneratorComponent &component, std::string_view key, std::size_t channel, std::size_t channelCount);
// The channels whose operations or modulated carriers name that one (v2…), counted or not, from 0.
std::vector<std::size_t> channelReaders(const GeneratorRecipe &recipe, std::size_t channel);
// The recipe without that channel; the formulas that name channels after it name them by their new number
// (v3 becomes v2). False, the recipe unchanged, with why (in French): it is the last one, or others read it.
bool removeGeneratorChannel(GeneratorRecipe &recipe, std::size_t channel, std::string *error = nullptr);
std::string_view carrierModulationKey(CarrierModulation modulation) noexcept; // "am", "fm", "pm", "dsb", "usb", "lsb"

// The record a component (WaveKind::Recorded) reads among the signals of the session: the one of its id if it
// still has the name or the number of samples it had when chosen, else the one of that name; its channel where
// it was if it still has its name, else the one of that name, else where it was.
struct RecordedSource
{
    model::SignalPtr signal; // null: not found (error says what, in French)
    std::size_t channel = 0;
    std::string error;

    bool ok() const noexcept { return signal != nullptr; }
};
RecordedSource findRecordedSource(const GeneratorComponent &component, std::span<const model::SignalPtr> sources);
// The component reading that channel of that signal (its id, its names and its number of samples).
void setRecordedSource(GeneratorComponent &component, const model::Signal &signal, std::size_t channel);
// Whether the recipe reads recorded signals (components counted or not).
bool readsRecordedSignals(const GeneratorRecipe &recipe) noexcept;
// A channel of a signal of the session used in a recipe: "wave", a channel made of it; "am", "fm", "pm", "dsb",
// "usb" or "lsb", a channel where it is the message, scaled to a peak of 1, of a carrier so modulated (1 V, the
// carrier of defaultGeneratorComponent: 10 kHz, at most fs/4; AM: index 0.8, the peak of the message at 80 %), the
// message (the record) above the carrier, which reads it as x. A blank recipe becomes that channel alone, at the
// rate and for the duration of the record (and its name, if it had the default one); otherwise the channel takes
// the place of the first blank channel that no other reads, or comes after the others.
struct SignalUse
{
    std::size_t channel = 0; // where the channel made is
    bool alone = false;      // the recipe was blank: it is that channel alone
    bool timed = false;      // at the rate and for the duration of the record
    std::string text;        // what was done, in French, for the user
    std::string error;       // why nothing was done, in French
    bool ok() const noexcept { return error.empty(); }
};
SignalUse useSignalInRecipe(GeneratorRecipe &recipe, const model::Signal &signal, std::size_t sourceChannel, std::string_view use);
// A channel of the recipe itself as the message of a carrier so modulated ("am", "fm", "pm", "dsb", "usb", "lsb"; AM:
// index 0.8), in a channel of its own that reads it as it is (message "v<k>": not scaled to a peak of 1, as a record
// is), named after it ("A modulé", "Voie 1 modulée"), placed as useSignalInRecipe places one (the first blank channel
// that no other reads, or after the others); the rate and the duration stay. Refused, the recipe unchanged: no such
// channel, a blank one (nothing to modulate with), no room.
SignalUse modulateChannelInRecipe(GeneratorRecipe &recipe, std::size_t channel, std::string_view use);
// The message of a modulated carrier (channel, component) taken from a channel of a signal of the session: that
// record, scaled to a peak of 1, as the component just above the carrier (in place of a record already there),
// the carrier reading the components above it (x). False, the recipe unchanged, with why (in French): not a
// carrier, no such channel in the record, no room in the channel.
bool useSignalAsMessage(GeneratorRecipe &recipe, std::size_t channel, std::size_t component, const model::Signal &signal,
                        std::size_t sourceChannel, std::string *error = nullptr);
// The recipe at the rate and for the duration of the record a component reads (from its start chosen; in a
// loop, the whole record once). False, the recipe unchanged, if it reads none.
bool adoptRecordedTiming(GeneratorRecipe &recipe, std::size_t channel, std::size_t component,
                         std::span<const model::SignalPtr> sources);

// A digital frame as a component gives it: where it lies in time.
struct FrameTiming
{
    BuiltFrame frame;             // as built (its error: what is wrong in the data or the settings)
    std::size_t line = 0;         // of the component, among those of its protocol
    double bitSeconds = 0.0;      // 1 / bit rate
    double durationSeconds = 0.0; // of one frame
    double endSeconds = 0.0;      // start + duration
    double samplesPerBit = 0.0;
    double samplesPerTick = 0.0;  // under 1, levels are missed

    bool ok() const noexcept { return frame.ok(); }
};
FrameTiming frameTiming(const GeneratorComponent &component, double sampleRateHz);
// The lines of the frame a component belongs to, in the recipe: itself, and the components of its group.
struct FrameMember
{
    std::size_t channel = 0, component = 0;
    std::size_t line = 0; // among those of the protocol
};
std::vector<FrameMember> frameMembers(const GeneratorRecipe &recipe, std::size_t channel, std::size_t component);
// The settings of that component (its frame, bit rate, start, repetition, levels) given to the other lines of
// its group; `before`: the protocol it had (another one now: each line takes the part it played, the clock stays
// a clock). Nothing to do for a component alone.
void spreadFrame(GeneratorRecipe &recipe, std::size_t channel, std::size_t component, FrameProtocol before);
// The other lines of its protocol, each as a new channel named after it (the channel of the component too, if it
// still has the name it was made with); the component and the new ones form a group. The number of channels
// added; false with why (in French) when there is no room for them.
bool addFrameLines(GeneratorRecipe &recipe, std::size_t channel, std::size_t component, std::size_t *added = nullptr,
                   std::string *error = nullptr);
// The recipe framed on that frame: it starts after two bits of rest and the signal ends two bits after it, at a
// rate of at least ten samples a bit (five a tick), a whole number of them; with `realRate`, at the bit rate of
// its protocol first (USB). False, the recipe unchanged, if the frame cannot be built.
bool adoptFrameTiming(GeneratorRecipe &recipe, std::size_t channel, std::size_t component, bool realRate = false);

// The coefficients of a Fourier series, in the form aₙ / bₙ whatever the form typed, for n = 1..count.
struct SeriesCoefficients
{
    double dc = 0.0;            // a₀
    std::vector<double> a, b;   // aₙ and bₙ, index n − 1
    std::string error;          // "bₙ : Nom inconnu « D » … (caractère 3)", "aₙ n'est pas un nombre pour n = 1…"
    bool ok() const noexcept { return error.empty(); }
};
SeriesCoefficients seriesCoefficients(const GeneratorComponent &component, double sampleRateHz, std::size_t count);
// Its number of harmonics: `harmonics`, or with 0 all those below fs/2.
std::size_t seriesHarmonics(const GeneratorComponent &component, double sampleRateHz) noexcept;
// "a₀", "aₙ", "bₙ" (or "C₀", "Re Cₙ", "Im Cₙ"; "C₀", "|Cₙ|", "arg Cₙ") for formula 0, 1, 2.
std::string_view seriesTermName(SeriesForm form, int formula) noexcept;

// Samples per channel: the duration times the rate, rounded, at least 1 (saturates far beyond the maximum).
std::size_t generatorSampleCount(const GeneratorRecipe &recipe) noexcept;
// Empty when the recipe can be generated, else why (rate, duration, too many samples), in French.
std::string checkGeneratorRecipe(const GeneratorRecipe &recipe);

std::string_view waveformKey(WaveKind waveform) noexcept; // "sine", "square"... as the simulator; "fourier", "drawn",
                                                         // "recurrence", "formula", "modulation", "recorded", "frame"
bool waveformFromKey(std::string_view key, WaveKind &waveform) noexcept;
std::string_view generatorWaveformName(WaveKind waveform) noexcept; // "Sinus"... "Dirac", "Balayage"
// The waveform of qacq::dsp::generate behind a kind (false for a Fourier series).
bool dspWaveform(WaveKind waveform, dsp::Waveform &out) noexcept;

model::Json toJson(const GeneratorComponent &component);
model::Json toJson(const GeneratorChannel &channel);
model::Json toJson(const GeneratorRecipe &recipe);
// What is missing or wrong takes the default; numbers are brought within their bounds; a component of an
// unknown waveform is dropped, so are channels and components beyond the maxima. A recipe always has at
// least one channel.
GeneratorComponent generatorComponentFromJson(const model::Json &json);
GeneratorChannel generatorChannelFromJson(const model::Json &json, std::size_t index);
GeneratorRecipe generatorRecipeFromJson(const model::Json &json);

// "Sinus 1 kHz, 1 V crête", "Bruit gaussien, écart type 100 mV", "Balayage 20 Hz → 20 kHz (log.), 1 V crête",
// "Série de Fourier 1 kHz, 15 harmoniques : bₙ = 4*E/(pi*n)*odd(n) (E = 1)", "Motif dessiné 1 kHz, 4 points
// (segments), 1 V crête", "Opération entre voies : v1*v2", "Porteuse 10 kHz, 1 V crête, modulée en amplitude
// (m = 0,5) par sin(2*pi*fm*t) (fm = 1000)", "Signal enregistré « Voix » (voie Micro), crête ramenée à 1 V",
// "Trame UART 8N1 à 9,6 kbit/s (7 octets : 42 6F 6E 6A 6F 75 72), de 0 V à 3,3 V".
std::string componentText(const GeneratorComponent &component, const std::string &unit);

// The first values of a recurrence as generated (12 at most), to show them.
struct RecurrenceTrace
{
    std::size_t channel = 0, component = 0;
    std::vector<double> x, y;
};

struct GeneratorResult
{
    model::Signal signal; // id 0, origin Generated, provenance {"generator": recipe}, notes: what it holds
    std::vector<std::string> warnings; // "Voie 1, composante 2 (carré 30 kHz) : la fréquence dépasse fs/2…"
    std::string error;
    bool cancelled = false; // stopped through Progress::cancelled (error: "Génération annulée.")
    std::vector<RecurrenceTrace> traces; // of each recurrence generated

    bool ok() const noexcept { return error.empty(); }
};

// The signal of the recipe: every channel the offset plus its enabled components, made after the channels
// its operations read; the channels left out of the signal (output false) are made too, and not kept. The
// progress goes by components. `sources`: the signals its recorded components may read (those of the session).
GeneratorResult generateSignal(const GeneratorRecipe &recipe, const model::Progress &progress = {},
                               std::span<const model::SignalPtr> sources = {});

// Ready-made recipes for the menu "Exemples".
struct GeneratorExample
{
    std::string name; // "Sinus et bruit"
    std::string tip;  // what it shows
    GeneratorRecipe recipe;
};
const std::vector<GeneratorExample> &generatorExamples();
} // namespace qacq::analysis
