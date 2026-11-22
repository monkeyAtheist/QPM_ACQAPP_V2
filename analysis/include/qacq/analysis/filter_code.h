#pragma once

// The module "Filtres", code for a microcontroller: the filter designed as a board runs it, causally, sample
// after sample, in floating point (float or double) or in fixed point (Q15: 16-bit samples and coefficients,
// 32-bit accumulator; Q31: 32-bit samples and coefficients, 64-bit accumulator); what rounding does to it
// (response, poles, computing noise, overflow); and the C that does it, a header and a source in C99 that
// any compiler for microcontrollers takes (declarations at the top of blocks, no 16-bit int overflow), with a
// self-test whose expected outputs come from the same arithmetic simulated here: bit for bit in fixed point.
//
// IIR (sections): floating point in transposed direct form II, fixed point in direct form I (one accumulator
// per section, its output rounded and saturated: what CMSIS-DSP does), each section's coefficients in their
// own Q format, chosen so that the accumulator cannot overflow; the sections scaled so that what passes from
// one to the next fits (a sine at full scale, or anything). FIR: a doubled delay line (each sample written
// twice, the last N always contiguous), symmetric taps folded (half the multiplications).
// Pure C++20, no Qt.

#include "qacq/analysis/filters.h"

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qacq::analysis
{
enum class CodeFormat
{
    Float32, // float: a microcontroller with a floating-point unit (Cortex-M4F, M7, ESP32)
    Float64, // double
    Q15,     // int16_t samples and coefficients, int32_t accumulator (Cortex-M0/M3, dsPIC, AVR, MSP430)
    Q31      // int32_t samples and coefficients, int64_t accumulator
};

// Fixed point: how the sections are scaled so that the signal between them fits in [−1, 1).
enum class CodeScaling
{
    Sum, // the sum of |h| of each impulse response from the input to a section's output at most 0,99 (L1):
         // nothing can overflow inside the filter, whatever the input; the default (an overflow in a recursive
         // section, even saturated, can keep it oscillating wrongly as long as a loud input lasts)
    Peak // the gain from the input to each section's output at most 0,99 at every frequency (L∞, Jackson): a
         // steady sine at full scale never overflows, a few dB less noise, but a transient can
};

struct CodeOptions
{
    CodeFormat format = CodeFormat::Float32;
    CodeScaling scaling = CodeScaling::Sum;
    std::string name = "filtre"; // prefix of every C name: a letter, then letters, digits or _ (31 at most)
};

// Longest FIR made into code (twice as many samples in memory).
inline constexpr std::size_t kMaximumCodeTaps = 4096;
// Samples of the self-test written in the code.
inline constexpr std::size_t kCodeTestLength = 32;

// The filter as the board runs it.
struct EmbeddedFilter
{
    CodeOptions options;
    std::string text;      // filterText of the design
    double sampleRateHz = 48000.0;
    bool fir = false;
    int bits = 32;         // fixed point: 16 or 32 (Q15, Q31); floating point: 32 or 64
    bool fixed = false;
    // Fixed point: the width of the accumulator. Q15: 32 bits when no section can overflow it with its
    // coefficients in their finest format, 64 otherwise (as CMSIS-DSP does); Q31: 64 bits, the coefficients a bit
    // coarser if need be.
    int accumulatorBits = 64;
    // IIR: b0, b1, b2, a1, a2 of each section (a0 = 1), the values the numbers stored stand for (scaled in
    // fixed point: the scale factor of each section is on its b); fixed point: the integers stored, and the
    // right shift of each section's accumulator (its coefficients are in Q(shift)).
    std::vector<std::array<double, 5>> sections;
    std::vector<std::array<std::int64_t, 5>> sectionIntegers;
    std::vector<int> sectionShifts;
    std::vector<double> sectionScales;
    // FIR: the taps as stored (the gain at the output in them, in fixed point), their integers and shift;
    // symmetry: 1 even (h[k] = h[N−1−k]), −1 odd, 0 none.
    std::vector<double> taps;
    std::vector<std::int64_t> tapIntegers;
    int tapShift = 0;
    int symmetry = 0;
    // The gain at the output, multiplied at the end (1: none). Fixed point: a FIR has it in its taps; an IIR in
    // the b of its last section as far as its output stays under 0,99 of full scale where it passes most, the
    // rest in this stage, an integer in Q(gainShift) (gainStage), so that the poles of the last section keep
    // the precision of the others.
    double outputGain = 1.0;
    bool gainStage = false;
    std::int64_t gainInteger = 0;
    int gainShift = 0;

    // What rounding does. The largest |Hq − H| over the frequencies, relative to the largest |H| (dB); the
    // largest |20 log10 |Hq/H|| where |H| is within 6 dB of its largest (dB); the largest |H| and |Hq| where |H|
    // is 60 dB or more below its largest (relative, linear; 0 if nowhere); the pole of the stored coefficients
    // nearest the unit circle; the computing noise on white noise, signal to noise ratio against double
    // precision (dB); samples saturated by a sine at full scale at the frequency of the largest gain (each
    // section's output counted); the largest |g H| over the frequencies (fixed point: above 1, an input at full
    // scale saturates the output there), and where.
    double deviationDb = -400.0;
    double passbandDeviationDb = 0.0;
    double stopbandDesigned = 0.0;
    double stopbandEmbedded = 0.0;
    double largestPoleRadius = 0.0;
    double snrDb = 400.0;
    std::size_t saturations = 0;
    double peakGain = 1.0;
    double peakFrequencyHz = 0.0;

    std::vector<std::string> notes;    // in French: the format, the scaling, what rounding does
    std::vector<std::string> warnings; // in French: too little precision, saturation...
    std::string error;                 // in French: nothing to make code from, and why

    bool ok() const noexcept { return error.empty() && (!sections.empty() || !taps.empty()); }
};

// The filter of the design made ready for a board: rounded, scaled, analysed. The application of the recipe
// (zero phase, aligned) does not carry over: a board filters causally.
EmbeddedFilter embedFilter(const FilterDesign &design, const CodeOptions &options);

// Run as the board runs it, from rest. x: samples in [−1, 1) for fixed point (rounded to the integers, saturated);
// the result likewise divided back. saturated: incremented for each sample clipped (at any section's output).
std::vector<double> runEmbedded(const EmbeddedFilter &filter, std::span<const double> x, std::size_t *saturated = nullptr);

// H of the coefficients as stored, the gain at the output included.
std::complex<double> embeddedResponse(const EmbeddedFilter &filter, double frequencyHz) noexcept;

struct GeneratedCode
{
    std::string headerName; // "filtre.h"
    std::string header;
    std::string sourceName; // "filtre.c"
    std::string source;
    std::string error; // the filter was not ok
};
// The C of the filter; design: what it was designed as (said at the top of the header).
GeneratedCode generateCode(const EmbeddedFilter &filter, const FilterDesign &design);

// A name the code can take: a letter or _, then letters, digits or _, 31 at most, not a word of C.
bool validCodeName(std::string_view name) noexcept;
// Keys: "float", "double", "q15", "q31"; "peak", "sum".
std::string_view codeFormatKey(CodeFormat format) noexcept;
std::string_view codeScalingKey(CodeScaling scaling) noexcept;
bool codeFormatFromKey(std::string_view key, CodeFormat &format) noexcept;
bool codeScalingFromKey(std::string_view key, CodeScaling &scaling) noexcept;
// "Flottant simple précision (float)", "Virgule fixe Q15 (int16_t)"...
std::string codeFormatText(CodeFormat format);
} // namespace qacq::analysis
