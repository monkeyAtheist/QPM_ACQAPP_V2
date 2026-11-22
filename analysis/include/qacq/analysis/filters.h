#pragma once

// The module "Filtres": a filter as a recipe (an IIR of a family, or a FIR by the window method), designed
// by qacq::dsp (IIR always as second-order sections), what it does (gain, phase and group delay at any
// frequency, impulse and step responses, poles and zeros), and run over a signal: causally (as in real
// time), forward then backward (zero phase, |H|²), or a FIR with its delay (N − 1)/2 taken off.
// Pure C++20, no Qt.

#include "qacq/dsp/filter_design.h"
#include "qacq/dsp/window.h"
#include "qacq/model/json.h"
#include "qacq/model/progress.h"
#include "qacq/model/signal.h"

#include <complex>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qacq::analysis
{
enum class FilterKind
{
    Iir, // poles and zeros of a family (Butterworth, Chebyshev I and II, Bessel), as sections
    Fir  // window method (scipy.signal.firwin)
};

// How a filter goes over a record.
enum class FilterApplication
{
    Causal,    // as in real time: its gain, its phase, its delay
    ZeroPhase, // forward then backward (scipy filtfilt): |H|², no phase, no delay
    Aligned    // a FIR whose delay (N − 1)/2 is taken off: |H|, linear phase gone (a FIR only)
};

struct FilterRecipe
{
    FilterKind kind = FilterKind::Iir;
    dsp::FilterFamily family = dsp::FilterFamily::Butterworth;
    dsp::FilterResponse response = dsp::FilterResponse::LowPass;
    int order = 4;           // IIR: of the prototype (a band filter has twice as many poles)
    std::size_t taps = 101;  // FIR
    dsp::Window window = dsp::Window::Hamming;
    double kaiserBeta = 8.6; // FIR with a Kaiser window
    double sampleRateHz = 48000.0;
    // Follows the signal filtered (the module sets sampleRateHz from it); else the rate typed.
    bool rateOfSignal = true;
    double frequency1Hz = 1000.0; // cut-off, or lower edge of the band
    double frequency2Hz = 2000.0; // upper edge (band-pass, band-stop)
    double passbandRippleDb = 1.0;       // Chebyshev I
    double stopbandAttenuationDb = 40.0; // Chebyshev II: frequency1 is where the stop band starts
    FilterApplication application = FilterApplication::Causal;
};

inline constexpr std::size_t kMaximumFirTaps = 65535;
// Zeros of a FIR found (Aberth) up to this many taps; beyond, they are not shown.
inline constexpr std::size_t kMaximumFirRoots = 512;
// Longest impulse response shown (it is cut once it has died out).
inline constexpr std::size_t kMaximumResponseLength = 262144;

struct FilterDesign
{
    FilterRecipe recipe;
    dsp::SosCascade sections; // IIR
    std::vector<double> taps; // FIR
    std::vector<std::complex<double>> zeros;
    std::vector<std::complex<double>> poles; // a FIR: none (N − 1 at the origin, not listed)
    bool rootsKnown = false;                 // a long FIR: its zeros are not computed
    std::string error;                       // in French, empty when designed
    std::vector<std::string> warnings;

    bool ok() const noexcept { return error.empty() && (!sections.empty() || !taps.empty()); }
    bool fir() const noexcept { return recipe.kind == FilterKind::Fir; }
    // H at f (Hz), as designed.
    std::complex<double> response(double frequencyHz) const noexcept;
    // As run over a record (the application): zero phase |H|², aligned H·e^{jω(N−1)/2}, causal H.
    std::complex<double> applied(double frequencyHz) const noexcept;
    // Group delay in samples, as designed.
    double groupDelay(double frequencyHz) const noexcept;
    // As applied: 0 at zero phase, without the delay removed by an aligned FIR.
    double appliedGroupDelay(double frequencyHz) const noexcept;
    // The phase of H at f in radians, unwrapped: continuous along the frequencies, but for a jump of π where
    // a zero lies on the unit circle (or where a linear-phase FIR changes sign). Computed from the roots of
    // each section (of the FIR), or as −ω(N−1)/2 for a symmetric FIR, so that it reads the same at any zoom.
    // phaseUnwrapped() false (a FIR neither symmetric nor antisymmetric whose zeros are not known): only
    // arg H(f), within ±π.
    double phase(double frequencyHz) const noexcept;
    bool phaseUnwrapped() const noexcept;
    // As applied: 0 at zero phase, without the linear part removed by an aligned FIR.
    double appliedPhase(double frequencyHz) const noexcept;
    // The delay an aligned FIR removes, in samples ((N − 1)/2); 0 otherwise.
    double alignment() const noexcept;
    // The pole nearest the unit circle (0 without poles): the closer to 1, the longer it rings.
    double largestPoleRadius() const noexcept;
};

FilterDesign designFilter(const FilterRecipe &recipe);

// h[n] for n = 0..; an IIR is cut once it has died out (below 1e-9 of its peak for a while), at most
// kMaximumResponseLength samples (`settled` false then); a FIR is its taps.
// As applied (at zero phase, aligned): the response of the filter run over an impulse as over a record,
// from n = appliedStart (negative: it starts before the impulse); empty when applied causal.
struct ImpulseResponse
{
    std::vector<double> impulse;
    std::vector<double> step; // running sum of the impulse response
    bool settled = true;
    std::vector<double> appliedImpulse;
    std::vector<double> appliedStep;
    long appliedStart = 0;
};
ImpulseResponse impulseResponse(const FilterDesign &design);

// What can be applied: a FIR only is aligned.
bool applicationAllowed(FilterKind kind, FilterApplication application) noexcept;

// "Passe-bas Butterworth d'ordre 4, 1 kHz" ; "Passe-bande FIR de 101 coefficients (Hamming), 1 kHz à 2 kHz".
std::string filterText(const FilterRecipe &recipe);
// Short, for the name of a signal: "passe-bas 1 kHz", "coupe-bande 45 Hz à 55 Hz".
std::string filterShortText(const FilterRecipe &recipe);
// "Causal (comme en temps réel)", "Aller-retour à phase nulle (gain au carré)", "Retard du FIR retiré".
std::string applicationText(FilterApplication application);
// Lines on the design: sections or taps, stability, delay, what the application does.
std::vector<std::string> designSummary(const FilterDesign &design);

struct FilteredSignal
{
    model::Signal signal;
    std::vector<std::string> warnings;
    std::string error;
    bool cancelled = false;
};
// The signal filtered, every channel (or those listed) on its own, at the rate of the design (refused
// if the signal has another one); named "<source> · passe-bas 1 kHz", provenance {"operation": "filter",
// "source", "sourceId", "parameters": the recipe}. The progress goes by channel. firstFrames: 0 for all
// of them, else only the first ones (a preview).
FilteredSignal applyFilter(const FilterDesign &design, const model::Signal &source,
                           const std::vector<std::size_t> &channels = {}, std::size_t firstFrames = 0,
                           const model::Progress &progress = {});
// One channel, as applyFilter runs it.
std::vector<double> runFilter(const FilterDesign &design, std::span<const double> x);

// Keys: "iir"/"fir"; "butterworth", "chebyshev1", "chebyshev2", "bessel"; "lowpass", "highpass",
// "bandpass", "bandstop"; windows as the spectra ("hamming", "kaiser"…); "causal", "zerophase", "aligned".
std::string_view filterKindKey(FilterKind kind) noexcept;
std::string_view filterFamilyKey(dsp::FilterFamily family) noexcept;
std::string_view filterResponseKey(dsp::FilterResponse response) noexcept;
std::string_view filterApplicationKey(FilterApplication application) noexcept;
model::Json toJson(const FilterRecipe &recipe);
// Values out of bounds are brought back within them; unknown keys keep the default.
FilterRecipe filterRecipeFromJson(const model::Json &json);

// A usual first filter at that rate: Butterworth low-pass of order 4 at 1 kHz (at most fs/10).
FilterRecipe defaultFilterRecipe(double sampleRateHz = 48000.0);

struct FilterExample
{
    std::string name;
    std::string tip;
    FilterRecipe recipe; // its rate: that of the signal filtered (rateOfSignal)
};
const std::vector<FilterExample> &filterExamples();
} // namespace qacq::analysis
