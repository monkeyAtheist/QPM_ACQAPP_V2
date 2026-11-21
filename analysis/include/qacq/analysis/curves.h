#pragma once
// The curves added under the signal in the module "Signaux", without Qt: what each one is (a kind and a
// channel), its name and unit, how it is computed, how the list is kept in the session file.
//
// A curve is a view of the signal chosen, computed again whenever that signal changes; it becomes a
// signal of the session only when asked (signalFromCurves). Spectra go to the graph of the frequencies
// (computed by analyse()); spectrograms to a graph of their own, on the time axis of the signal; every
// other kind goes to the graph of time, under the signal, on the same time axis: the integral of a channel
// lines up with the channel.
#include "qacq/analysis/calculus.h"
#include "qacq/dsp/spectrum.h"
#include "qacq/model/json.h"
#include "qacq/model/signal.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qacq::analysis
{
enum class CurveKind
{
    Spectrum,      // amplitude spectrum or density (the setting of the graph of the frequencies)
    Source,        // the channel itself, to lay a computed curve over it
    Integral,
    Derivative,
    CycleMean,
    CycleRms,
    CycleFrequency,
    Spectrogram    // the frequencies as time goes (its own graph, on the time axis of the signal)
};

std::string_view curveKindKey(CurveKind kind) noexcept; // "spectrum", "source", "integral"... (files, QML)
bool curveKindFromKey(std::string_view key, CurveKind &kind) noexcept;
bool isTimeCurve(CurveKind kind) noexcept; // the graph of time: every kind but Spectrum and Spectrogram
std::string_view curveGraph(CurveKind kind) noexcept; // "time", "frequency" or "spectrogram"
bool hasPart(CurveKind kind) noexcept; // computed over a part of the signal (all or between A and B)
std::string curveKindName(CurveKind kind);  // "Spectre", "Voie d'origine", "Intégrale"...

struct CurveSpec
{
    std::uint64_t id = 0; // given by the list, unique in it
    CurveKind kind = CurveKind::Spectrum;
    std::size_t channel = 0; // of the signal chosen
    bool visible = true;
    bool removeMean = false;   // integral: the mean of the part taken off first
    std::size_t firstFrame = 0; // time curves and spectrograms: the part computed (frames of the signal)
    std::size_t frameCount = std::numeric_limits<std::size_t>::max(); // max: up to the end

    bool samePlace(const CurveSpec &other) const noexcept; // same kind, channel, part and options
};

// [{"id", "kind", "channel", "visible", "removeMean", "firstFrame", "frameCount"}...]; a part up to the end
// is written without "frameCount".
model::Json toJson(const std::vector<CurveSpec> &curves);
// Unknown kinds and malformed entries are left out; ids kept (duplicates renumbered).
std::vector<CurveSpec> curveSpecsFromJson(const model::Json &json);

// Name and unit of a curve on a channel of a signal ("∫ Tension", "V·s"); empty name: no such channel.
std::string curveName(const CurveSpec &curve, const model::Signal &signal);
std::string curveUnit(const CurveSpec &curve, const model::Signal &signal);
// Where a time curve lies in the frames of the signal: {first, count}, clipped to it.
std::pair<std::size_t, std::size_t> curvePart(const CurveSpec &curve, const model::Signal &signal);

// The samples of a computed time curve over its part (empty for Spectrum and Source, or a channel that
// does not exist). A per-period curve of a channel with holes, or with fewer than two periods: all NaN.
std::vector<double> computeTimeCurve(const model::Signal &signal, const CurveSpec &curve);

// How spectrograms are computed (the window and the segments come from the settings of the analysis).
struct SpectrogramSettings
{
    dsp::Window window = dsp::Window::Hann;
    double kaiserBeta = 8.6;
    std::size_t segmentLength = 1024; // longer: finer in frequency, coarser in time
    double overlap = 0.75;
    std::size_t maximumFrames = 3000; // columns: enough for a plot across a wide screen
    bool removeMean = true;           // segment by segment, weighted by the window: nothing false at 0 Hz
    // Levels kept per spectrogram (frames x bins, 8 bytes each): very long segments get fewer columns
    // (16 384 samples: 732 columns, 48 MB) rather than hundreds of megabytes per channel.
    std::size_t maximumLevels = 6'000'000;
};

// The spectrogram of a curve of kind Spectrogram over its part, times from the start of the part. Every
// sample counts (a long record has frames that average several segments); missing samples count as
// zeros. Empty for another kind, a channel that does not exist, or when stopped early.
dsp::Spectrogram computeSpectrogram(const model::Signal &signal, const CurveSpec &curve, const SpectrogramSettings &settings,
                                    const dsp::Progress &progress = {});

// One spectrogram of its graph, as the plot draws it.
struct SpectrogramCurve
{
    std::uint64_t id = 0;
    std::string name;        // the channel's
    std::string unit;        // the channel's ("V"); the levels are in dB of it
    std::size_t channel = 0;
    bool visible = true;
    double startSeconds = 0.0; // time of the first sample of its part (its times are from there)
    std::shared_ptr<const dsp::Spectrogram> result; // null until computed
};

struct SpectrogramSet
{
    model::SignalPtr signal;              // the version they come from
    std::vector<SpectrogramCurve> curves; // in the order of the list, hidden ones too
};

// One curve of the graph of time: its samples (computed, or the channel itself for Source), placed in the
// frames of the signal.
struct TimeCurve
{
    std::uint64_t id = 0;
    std::string name;
    std::string unit;
    std::size_t channel = 0;
    bool visible = true;
    std::size_t firstFrame = 0;                          // frame of the signal of values[0]
    std::shared_ptr<const std::vector<double>> computed; // null for Source
    std::span<const double> values;                      // into `computed`, or into the signal's data
};

struct TimeCurveSet
{
    model::SignalPtr signal;       // the version the curves come from (keeps its data alive)
    std::vector<TimeCurve> curves; // in the order of the list, hidden ones too (stable colours)
};

// The visible curves as a signal of their own, for the session: one channel per curve over the union of
// their parts (NaN where a curve has no value), the rate and time of the source. Error (French) when
// there is nothing to keep.
Derived signalFromCurves(const TimeCurveSet &set);
} // namespace qacq::analysis
