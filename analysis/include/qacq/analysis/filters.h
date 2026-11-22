#pragma once

// The module "Filtres": a filter as a recipe (an IIR of a family, a FIR by the window method or equiripple by
// Parks-McClellan, or coefficients b and a typed), its order and frequencies chosen or computed from a template
// (gabarit: the least order that holds it), designed by qacq::dsp (IIR always as second-order sections), what it does
// (gain, phase and group delay at any frequency, impulse and step responses, poles and zeros), and run
// over a signal: causally (as in real time), forward then backward (zero phase, |H|²), or a FIR with its
// delay (N − 1)/2 taken off.
// Pure C++20, no Qt.

#include "qacq/dsp/filter_design.h"
#include "qacq/dsp/filtering.h"
#include "qacq/dsp/window.h"
#include "qacq/model/json.h"
#include "qacq/model/progress.h"
#include "qacq/model/signal.h"

#include <array>
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
    Iir,  // poles and zeros of a family (Butterworth, Chebyshev I and II, Bessel, elliptic), as sections
    Fir,  // window method (scipy.signal.firwin) or equiripple (scipy.signal.remez)
    Typed // coefficients typed: H(z) = Σ b[k] z^−k / Σ a[k] z^−k, as scipy.signal.lfilter(b, a); a FIR if a = 1
};

// How a FIR is designed.
enum class FirMethod
{
    Window,    // the ideal response truncated by a window
    Equiripple // Parks-McClellan: the transitions given, the error spread evenly (weights from Rp and Rs)
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
    FirMethod firMethod = FirMethod::Window;
    dsp::Window window = dsp::Window::Hamming;
    double kaiserBeta = 8.6; // FIR with a Kaiser window
    // Equiripple: the width of each transition, centred on the edge (a low-pass at 1 kHz with 200 Hz: pass
    // band up to 900 Hz, stop band from 1,1 kHz).
    double transitionHz = 200.0;
    double sampleRateHz = 48000.0;
    // Follows the signal filtered (the module sets sampleRateHz from it); else the rate typed.
    bool rateOfSignal = true;
    double frequency1Hz = 1000.0; // cut-off, or lower edge of the band
    double frequency2Hz = 2000.0; // upper edge (band-pass, band-stop)
    double passbandRippleDb = 1.0;       // Chebyshev I, elliptic, equiripple
    double stopbandAttenuationDb = 40.0; // Chebyshev II (frequency1 is where its stop band starts), elliptic,
                                         // equiripple
    FilterApplication application = FilterApplication::Causal;
    // At zero phase, how the record is prolonged beyond its ends before the filter runs forward and backward:
    // predicted by default (the signal goes on as it went: no burst at the ends), or as SciPy does.
    dsp::EdgeExtension edges = dsp::EdgeExtension::Predicted;
    // At the output: the signal filtered times 10^(gain/20), once whatever the application (at zero phase too,
    // where the rest of the filter counts twice). As an active filter has one; −120 to +120 dB.
    double gainDb = 0.0;
    // Typed coefficients (kind Typed): b and a, lists of numbers or formulas ("1 ; −2 cos(w0) ; 1"), or one
    // formula of k (the index, from 0) with how many coefficients (a number, or a formula of the variables:
    // "N"). The formulas know fs, Te (= 1/fs), pi, e and the variables ("N = 8 ; w0 = 2 pi 50/fs").
    std::string numerator = "1";
    std::string denominator = "1";
    std::string numeratorCount;
    std::string denominatorCount;
    std::string variables;
    // The order (a FIR: the number of coefficients) chosen, or the least that holds a template (gabarit): the edges
    // of its bands as dsp::FilterTemplate has them (a low-pass passes up to passEdge1Hz and stops from
    // stopEdge1Hz; a band-pass stops up to stopEdge1Hz, passes from passEdge1Hz to passEdge2Hz, stops from
    // stopEdge2Hz…), at most passbandRippleDb of attenuation over the pass band(s) (peak to peak: an IIR between 0
    // and −Ap dB, a FIR around 0 dB) and at least stopbandAttenuationDb over the stop band(s). The order, the
    // frequencies (frequency1Hz, frequency2Hz), the taps, β of a Kaiser window and the transition of an equiripple
    // FIR are then computed: the recipe of the design has them. Not for coefficients typed.
    bool fromTemplate = false;
    double passEdge1Hz = 1000.0;
    double passEdge2Hz = 2000.0;
    double stopEdge1Hz = 2000.0;
    double stopEdge2Hz = 4000.0;
};

// What a template asks of each design: the least order of each family of IIR (beyond its maximum: the maximum + 1;
// 0: not to be held), and Kaiser's estimates of the taps of a FIR by the window of Kaiser and by equiripple.
struct TemplateOrders
{
    int butterworth = 0, chebyshev = 0, elliptic = 0, bessel = 0;
    std::size_t kaiserTaps = 0, equirippleTaps = 0;
};

inline constexpr std::size_t kMaximumFirTaps = 65535;
inline constexpr std::size_t kMaximumEquirippleTaps = dsp::kMaximumRemezTaps;
inline constexpr double kMaximumFilterGainDb = 120.0;
// Beyond this attenuation (dB, the smaller of the two errors as Kaiser's formula expects it), an equiripple
// design is computed on fewer taps, the others zeros: its best error would lie near or below the rounding of
// the computation, where the exchange no longer converges.
inline constexpr double kEquirippleReachDb = 170.0;
// The exchange is given more iterations than SciPy's 25: long filters need up to about 60.
inline constexpr int kEquirippleIterations = 100;
// Zeros of a FIR found (Aberth) up to this many taps; beyond, they are not shown.
inline constexpr std::size_t kMaximumFirRoots = 512;
// Longest impulse response shown (it is cut once it has died out).
inline constexpr std::size_t kMaximumResponseLength = 262144;
// Coefficients typed: a recursive filter has at most this many b and as many a (order 256); a FIR (a = 1) as
// many b as a FIR designed has taps.
inline constexpr std::size_t kMaximumTypedIirCoefficients = 257;

struct FilterDesign
{
    FilterRecipe recipe;
    dsp::SosCascade sections; // IIR
    std::vector<double> taps; // FIR
    std::vector<std::complex<double>> zeros;
    std::vector<std::complex<double>> poles; // a FIR: none (N − 1 at the origin, not listed)
    bool rootsKnown = false;                 // a long FIR: its zeros are not computed
    // Where the stop band begins, in Hz (an elliptic filter: one edge, two for a band); empty otherwise.
    std::vector<double> stopEdgesHz;
    // An equiripple FIR: what it reached over its bands (peak-to-peak ripple of the pass bands, least
    // attenuation of the stop bands, in dB), and the iterations of the exchange; Kaiser's estimate of the taps
    // the template needs; the taps computed (fewer than N when N would reach far beyond the precision of the
    // computation: the others, at both ends, are zeros); how the exchange stopped early (empty: it did not).
    double achievedRippleDb = 0.0;
    double achievedAttenuationDb = 0.0;
    int iterations = 0;
    std::size_t estimatedTaps = 0;
    std::size_t computedTaps = 0;
    std::string exchangeNote;
    std::string error;                       // in French, empty when designed
    std::vector<std::string> warnings;
    // The gain at the output, 10^(gainDb/20): not in the sections nor in the taps (they are those of the filter
    // as designed, 0 dB where it passes); in what follows, the responses and the filtering.
    double gain = 1.0;
    // Coefficients typed: as evaluated (before a[0] divides them), whether each list is one formula of k.
    std::vector<double> typedNumerator;
    std::vector<double> typedDenominator;
    bool numeratorOfK = false;
    bool denominatorOfK = false;
    // Designed, but not to be run over a signal (why, in French): coefficients typed that make it unstable, or
    // a pole on the unit circle at zero phase (it would never die out).
    std::string applicationError;
    // Designed from a template (recipe.fromTemplate; the recipe here has the order, the taps, the frequencies and β
    // computed): what it holds in dB (the pass bands peak to peak, the least attenuation of the stop bands) and
    // whether that is within the template; the order the template asks as a real number (an IIR Butterworth,
    // Chebyshev or elliptic: the order is it rounded up); what the other designs would need.
    double templateLossDb = 0.0;
    double templateAttenuationDb = 0.0;
    bool templateHeld = false;
    double exactOrder = 0.0;
    TemplateOrders templateOrders;

    bool ok() const noexcept { return error.empty() && (!sections.empty() || !taps.empty()); }
    // Taps rather than sections: a FIR designed, or coefficients typed with a = 1.
    bool fir() const noexcept { return !taps.empty() || (sections.empty() && recipe.kind == FilterKind::Fir); }
    // H at f (Hz), as designed, the gain at the output included.
    std::complex<double> response(double frequencyHz) const noexcept;
    // Of the sections or the taps alone, without the gain.
    std::complex<double> unitResponse(double frequencyHz) const noexcept;
    // As run over a record (the application), g the gain: zero phase g|H|², aligned g·H·e^{jω(N−1)/2}, causal g·H.
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

// The template of a recipe as dsp sees it: its edges, Ap = passbandRippleDb, As = stopbandAttenuationDb.
dsp::FilterTemplate filterTemplate(const FilterRecipe &recipe);
// Its edges in the order its response wants, within 0 to fs/2 (dsp::templateError says it otherwise).
bool templateEdgesValid(const FilterRecipe &recipe);
// Edges for its response from its frequencies (frequency1Hz, frequency2Hz: the cut-off, or the band): a low-pass
// passes up to the cut-off and stops from twice it; a high-pass stops up to half of it; a band-pass passes over the
// band and stops an octave beyond each edge; a band-stop stops over the middle half of the band, in logarithm.
void setTemplateFromFrequencies(FilterRecipe &recipe);
// Edges of a template the design holds, its Amax and Amin those of `recipe` (whose response is the design's): where
// its gain leaves the zone of the pass band and enters that of the stop band, rounded to 3 significant digits towards
// an easier template (the pass bands narrower, the stop bands too). False, the edges untouched, when its gain does
// not go there (a filter too gentle for Amin, a template that would not be in order).
bool templateFromDesign(const FilterDesign &design, FilterRecipe &recipe);
// Where the gain of the filter at 0 dB must stay, linear: over each pass band between passLow and passHigh (an IIR
// from 10^(−Ap/20) to 1; a FIR 1 ± δp, (1 + δp)/(1 − δp) = 10^(Ap/20)), over each stop band under stopHigh
// (10^(−As/20)). Bands in Hz, from 0 to fs/2.
struct TemplateZones
{
    std::vector<std::array<double, 2>> passBands;
    std::vector<std::array<double, 2>> stopBands;
    double passLow = 0.0, passHigh = 1.0, stopHigh = 0.0;
};
TemplateZones templateZones(const FilterRecipe &recipe);
// What a design holds of the template of its recipe, measured on its gain at 0 dB: over a grid (a FIR: one FFT
// of its taps; an IIR: 2 048 points a band) and the edges, the extremes refined. Held: within the zones to 1e-7.
struct TemplateMeasure
{
    double lossDb = 0.0;        // over the pass bands, peak to peak
    double attenuationDb = 0.0; // the least over the stop bands
    double excessDb = 0.0;      // how far beyond the zones it goes at worst, dB (negative: by how much it holds)
    bool held = false;
};
TemplateMeasure measureTemplate(const FilterDesign &design);

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

// What can be applied: a FIR only is aligned (coefficients typed: if a = 1, which the design tells).
bool applicationAllowed(FilterKind kind, FilterApplication application) noexcept;

// Coefficients typed. The items of a list: split at ";" and new lines, and at "," unless it is a decimal comma
// (between two digits, in a text without "."); plain numbers separated by spaces too ("1 -2 1", as MATLAB and
// NumPy print them); brackets around it, and NumPy's "array(…)", left out; empty items left out.
std::vector<std::string> splitCoefficients(std::string_view text);
struct TypedCoefficients
{
    std::vector<double> numerator;   // b, as evaluated
    std::vector<double> denominator; // a
    bool numeratorOfK = false;       // one formula of k, repeated as many times as said
    bool denominatorOfK = false;
    std::string error; // in French, empty when both read
};
// Evaluated with fs, Te (1/fs), k (the index) and the variables of the recipe.
TypedCoefficients evaluateTypedCoefficients(const FilterRecipe &recipe);

// "Passe-bas Butterworth d'ordre 4, 1 kHz" ; "Passe-bande RIF de 101 coefficients (Hamming), 1 kHz à 2 kHz" ;
// from a template, the order computed (the recipe of the design): "Passe-bas Butterworth d'ordre 7, gabarit 1 dB
// jusqu'à 1 kHz, 40 dB dès 2 kHz".
std::string filterText(const FilterRecipe &recipe);
// Short, for the name of a signal: "passe-bas 1 kHz", "coupe-bande 45 Hz à 55 Hz" (a template: the pass band
// edges, a band-stop's stop band).
std::string filterShortText(const FilterRecipe &recipe);
// The template in a sentence: "Gabarit : au plus 1 dB jusqu'à 1 kHz, au moins 40 dB dès 2 kHz."
std::string templateText(const FilterRecipe &recipe);
// A gain in dB, signed: "+6 dB", "−3 dB".
std::string filterGainText(double gainDb);
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

// Keys: "iir"/"fir"/"typed"; "butterworth", "chebyshev1", "chebyshev2", "bessel"; "lowpass", "highpass",
// "bandpass", "bandstop"; windows as the spectra ("hamming", "kaiser"…); "causal", "zerophase", "aligned".
std::string_view filterKindKey(FilterKind kind) noexcept;
std::string_view firMethodKey(FirMethod method) noexcept; // "window", "equiripple"
std::string_view filterFamilyKey(dsp::FilterFamily family) noexcept;
std::string_view filterResponseKey(dsp::FilterResponse response) noexcept;
std::string_view filterApplicationKey(FilterApplication application) noexcept;
std::string_view edgeExtensionKey(dsp::EdgeExtension extension) noexcept; // "predicted", "odd", "even", "constant", "none"
// What the extension does, in a sentence (the summary of a zero-phase filter).
std::string edgeExtensionText(dsp::EdgeExtension extension);
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
