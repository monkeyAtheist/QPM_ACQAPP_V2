#include "qacq/dsp/tone.h"

#include "qacq/dsp/constants.h"
#include "qacq/dsp/fft.h"
#include "qacq/dsp/window.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace qacq::dsp
{
namespace
{
constexpr std::size_t kCoarseLength = std::size_t{1} << 20;
// Records up to this length get the residual search of step 2b (each trial is one fit: O(N K)).
constexpr std::size_t kResidualSearchUpTo = std::size_t{1} << 18;
constexpr std::size_t kResync = 256; // the phasor recurrence is re-anchored every kResync samples

// exp(j 2 pi ratio n) for a large integer n, with the phase reduced exactly enough (ratio = f / fs).
Complex phasorAt(double ratio, std::size_t n) noexcept
{
    const double cycles = ratio * static_cast<double>(n);
    const double fraction = cycles - std::floor(cycles);
    return std::polar(1.0, kTwoPi * fraction);
}

// sum_{n = first}^{first + count - 1} exp(j phi n), in closed form (Dirichlet kernel).
Complex phasorSum(double phi, std::size_t first, std::size_t count) noexcept
{
    const double halfStep = std::sin(0.5 * phi);
    const double centre = static_cast<double>(first) + 0.5 * (static_cast<double>(count) - 1.0);
    if (std::abs(halfStep) < 1e-14) return Complex(static_cast<double>(count), 0.0) * std::polar(1.0, phi * centre);
    const double ratio = std::sin(0.5 * phi * static_cast<double>(count)) / halfStep;
    return std::polar(ratio, phi * centre);
}

// Dense solve with partial pivoting; a is row-major p x p, b has p entries (overwritten by x).
bool solveDense(std::vector<double> &a, std::vector<double> &b, std::size_t p)
{
    for (std::size_t col = 0; col < p; ++col)
    {
        std::size_t pivot = col;
        for (std::size_t r = col + 1; r < p; ++r)
            if (std::abs(a[r * p + col]) > std::abs(a[pivot * p + col])) pivot = r;
        if (!(std::abs(a[pivot * p + col]) > 1e-300)) return false;
        if (pivot != col)
        {
            for (std::size_t c = 0; c < p; ++c) std::swap(a[col * p + c], a[pivot * p + c]);
            std::swap(b[col], b[pivot]);
        }
        for (std::size_t r = col + 1; r < p; ++r)
        {
            const double factor = a[r * p + col] / a[col * p + col];
            if (factor == 0.0) continue;
            for (std::size_t c = col; c < p; ++c) a[r * p + c] -= factor * a[col * p + c];
            b[r] -= factor * b[col];
        }
    }
    for (std::size_t i = p; i-- > 0;)
    {
        double s = b[i];
        for (std::size_t c = i + 1; c < p; ++c) s -= a[i * p + c] * b[c];
        b[i] = s / a[i * p + i];
    }
    return true;
}

std::size_t usableHarmonics(double f, double fs, std::size_t requested) noexcept
{
    std::size_t k = 0;
    while (k < requested && static_cast<double>(k + 1) * f / fs < 0.5 - 1e-9) ++k;
    return k;
}

// Model dc + sum_k Re(C_k exp(j k theta n)) evaluated on count samples starting at absolute index first.
std::vector<double> evaluateModel(const HarmonicFit &fit, double ratio, std::size_t first, std::size_t count)
{
    std::vector<double> y(count, fit.dc);
    const Complex step = std::polar(1.0, kTwoPi * ratio);
    Complex z;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (i % kResync == 0)
            z = phasorAt(ratio, first + i);
        else
            z *= step;
        Complex zk = z;
        double v = fit.dc;
        for (const Complex &c : fit.coefficients)
        {
            v += (c * zk).real();
            zk *= z;
        }
        y[i] = v;
    }
    return y;
}
struct Coarse
{
    bool found = false;
    double frequencyHz = 0.0;
    double amplitude = 0.0; // of a sinusoid at the peak (Hann coherent gain compensated)
    double prominenceDb = 0.0;
};

// Peak of a Hann spectrum `data` (already transformed, nfft points, of m windowed samples at `rate`):
// between minimumHz and maximumHz, above the DC lobe, Gaussian interpolation.
Coarse peakOf(const std::vector<Complex> &data, std::size_t nfft, std::size_t m, double windowSum, double rate,
              double minimumHz, double maximumHz, std::size_t *lowestBin = nullptr)
{
    Coarse c;
    const double binWidth = rate / static_cast<double>(nfft);
    const std::size_t guard =
        static_cast<std::size_t>(std::ceil(2.0 * static_cast<double>(nfft) / static_cast<double>(m))) + 1;
    const std::size_t kMin =
        std::max(guard, static_cast<std::size_t>(std::ceil(std::max(0.0, minimumHz) / binWidth)));
    if (lowestBin) *lowestBin = kMin;
    const std::size_t kMax = std::min(nfft / 2 - 1, static_cast<std::size_t>(std::floor(maximumHz / binWidth)));
    if (kMin + 2 > kMax) return c;
    std::vector<double> magnitude(kMax + 2);
    for (std::size_t k = 0; k < magnitude.size(); ++k) magnitude[k] = std::abs(data[k]);
    std::size_t peak = kMin;
    for (std::size_t k = kMin; k <= kMax; ++k)
        if (magnitude[k] > magnitude[peak]) peak = k;
    std::vector<double> band(magnitude.begin() + static_cast<std::ptrdiff_t>(kMin),
                             magnitude.begin() + static_cast<std::ptrdiff_t>(kMax) + 1);
    std::nth_element(band.begin(), band.begin() + static_cast<std::ptrdiff_t>(band.size() / 2), band.end());
    const double median = band[band.size() / 2];
    c.prominenceDb = median > 0.0 ? 20.0 * std::log10(magnitude[peak] / median) : 300.0;
    double f = static_cast<double>(peak) * binWidth;
    const double a0 = magnitude[peak - 1], a1 = magnitude[peak], a2 = magnitude[peak + 1];
    if (a0 > 0.0 && a1 > 0.0 && a2 > 0.0)
    {
        const double l0 = std::log(a0), l1 = std::log(a1), l2 = std::log(a2);
        const double denominator = l0 - 2.0 * l1 + l2;
        if (denominator < 0.0) f += std::clamp(0.5 * (l0 - l2) / denominator, -0.5, 0.5) * binWidth;
    }
    c.found = true;
    c.frequencyHz = f;
    c.amplitude = windowSum > 0.0 ? 2.0 * magnitude[peak] / windowSum : 0.0;
    return c;
}

// Peak of the WHOLE record averaged by blocks of `block` samples (a low-pass and a decimation in one): a
// slow wave of a few periods over a long record, that the first 2^20 samples cannot tell from DC (ten
// million samples at 48 kHz carrying 0,1 Hz: 2,2 periods in the first 2^20 samples, inside their DC lobe).
Coarse coarseOfWholeRecord(std::span<const double> x, double dc, double fs, std::size_t block, double minimumHz,
                           double maximumHz)
{
    const std::size_t m = x.size() / block;
    if (m < 16) return {};
    const std::vector<double> w = makeWindow(Window::Hann, m);
    const std::size_t nfft = m < kCoarseLength / 2 ? 2 * nextPowerOfTwo(m) : nextPowerOfTwo(m);
    std::vector<Complex> data(nfft, Complex(0.0, 0.0));
    double windowSum = 0.0;
    for (std::size_t i = 0; i < m; ++i)
    {
        double sum = 0.0;
        for (std::size_t j = i * block; j < (i + 1) * block; ++j) sum += x[j];
        data[i] = Complex((sum / static_cast<double>(block) - dc) * w[i], 0.0);
        windowSum += w[i];
    }
    FftPlan(nfft).forward(data);
    const double rate = fs / static_cast<double>(block);
    // Below 0,4 of the reduced rate: block averaging attenuates and folds what lies above.
    return peakOf(data, nfft, m, windowSum, rate, minimumHz, std::min(maximumHz, 0.4 * rate));
}

} // namespace

bool fitHarmonics(std::span<const double> x, double fs, double f, std::size_t harmonics, HarmonicFit &fit,
                  std::size_t firstSampleIndex)
{
    fit = HarmonicFit{};
    if (x.empty() || !(fs > 0.0) || !(f > 0.0) || !std::isfinite(f)) return false;
    const std::size_t k = usableHarmonics(f, fs, harmonics);
    const std::size_t p = 1 + 2 * k;
    const std::size_t n = x.size();
    if (n < p) return false;
    const double ratio = f / fs;
    const double theta = kTwoPi * ratio;

    // Right-hand side: sum y, sum y cos(k theta n), sum y sin(k theta n), with a re-anchored phasor.
    std::vector<Complex> accumulated(k, Complex(0.0, 0.0));
    double sumY = 0.0;
    double sumSquares = 0.0;
    const Complex step = std::polar(1.0, theta);
    Complex z;
    for (std::size_t i = 0; i < n; ++i)
    {
        if (i % kResync == 0)
            z = phasorAt(ratio, firstSampleIndex + i);
        else
            z *= step;
        const double y = x[i];
        sumY += y;
        sumSquares += y * y;
        Complex zk = z;
        for (std::size_t h = 0; h < k; ++h)
        {
            accumulated[h] += y * zk;
            zk *= z;
        }
    }
    // accumulated = sum y exp(+j k theta n): real part = sum y cos, imaginary part = sum y sin.
    std::vector<double> rhs(p);
    rhs[0] = sumY;
    for (std::size_t h = 0; h < k; ++h)
    {
        rhs[1 + 2 * h] = accumulated[h].real();
        rhs[2 + 2 * h] = accumulated[h].imag();
    }

    // Gram matrix in closed form.
    std::vector<double> gram(p * p, 0.0);
    if (gram.empty()) return false;
    double *const g = gram.data();
    auto at = [g, p](std::size_t r, std::size_t c) -> double & { return g[r * p + c]; };
    auto sumAt = [&](long long m) { return phasorSum(static_cast<double>(m) * theta, firstSampleIndex, n); };
    at(0, 0) = static_cast<double>(n);
    for (std::size_t a = 1; a <= k; ++a)
    {
        const Complex s = sumAt(static_cast<long long>(a));
        at(0, 2 * a - 1) = at(2 * a - 1, 0) = s.real();
        at(0, 2 * a) = at(2 * a, 0) = s.imag();
        for (std::size_t b = a; b <= k; ++b)
        {
            const Complex difference = sumAt(static_cast<long long>(b) - static_cast<long long>(a));
            const Complex total = sumAt(static_cast<long long>(a + b));
            const double cc = 0.5 * (difference.real() + total.real());
            const double ss = 0.5 * (difference.real() - total.real());
            // cos(a) sin(b) = (sin((a+b)) - sin((a-b))) / 2 ; sin((a-b)) = -sin((b-a))
            const double cs = 0.5 * (total.imag() + difference.imag());
            const double sc = 0.5 * (total.imag() - difference.imag());
            at(2 * a - 1, 2 * b - 1) = at(2 * b - 1, 2 * a - 1) = cc;
            at(2 * a, 2 * b) = at(2 * b, 2 * a) = ss;
            at(2 * a - 1, 2 * b) = at(2 * b, 2 * a - 1) = cs; // sum cos(a) sin(b)
            at(2 * a, 2 * b - 1) = at(2 * b - 1, 2 * a) = sc; // sum sin(a) cos(b)
        }
    }

    std::vector<double> solution = rhs;
    if (!solveDense(gram, solution, p)) return false;

    fit.dc = solution[0];
    fit.coefficients.assign(harmonics, Complex(0.0, 0.0));
    double explained = solution[0] * rhs[0];
    for (std::size_t h = 0; h < k; ++h)
    {
        const double a = solution[1 + 2 * h];
        const double b = solution[2 + 2 * h];
        fit.coefficients[h] = Complex(a, -b); // a cos + b sin = Re((a - j b) exp(j theta n))
        explained += a * rhs[1 + 2 * h] + b * rhs[2 + 2 * h];
    }
    fit.residualMeanSquare = std::max(0.0, (sumSquares - explained) / static_cast<double>(n));
    return true;
}

ToneAnalysis analyzeTone(std::span<const double> x, double fs, const ToneOptions &options)
{
    ToneAnalysis result;
    const std::size_t n = x.size();
    if (n < 8 || !(fs > 0.0) || !std::isfinite(fs)) return result;
    result.samplesUsed = n;

    double sum = 0.0, squares = 0.0;
    for (double v : x)
    {
        sum += v;
        squares += v * v;
    }
    result.dc = sum / static_cast<double>(n);
    result.rms = std::sqrt(squares / static_cast<double>(n));
    double ac = 0.0;
    for (double v : x) ac += (v - result.dc) * (v - result.dc);
    result.acRms = std::sqrt(ac / static_cast<double>(n));
    if (!(result.acRms > 1e-12 * std::max(result.rms, 1e-300))) return result;

    // 1. Coarse estimate on the first 2^20 samples at most.
    const std::size_t coarseLength = std::min(n, kCoarseLength);
    const std::size_t nfft = coarseLength < kCoarseLength / 2 ? 2 * nextPowerOfTwo(coarseLength)
                                                                : nextPowerOfTwo(coarseLength);
    const std::vector<double> w = makeWindow(Window::Hann, coarseLength);
    std::vector<Complex> data(nfft, Complex(0.0, 0.0));
    for (std::size_t i = 0; i < coarseLength; ++i) data[i] = Complex((x[i] - result.dc) * w[i], 0.0);
    FftPlan(nfft).forward(data);
    const double binWidth = fs / static_cast<double>(nfft);
    const std::size_t guard =
        static_cast<std::size_t>(std::ceil(2.0 * static_cast<double>(nfft) / static_cast<double>(coarseLength))) + 1;
    const double upper = options.maximumFrequencyHz > 0.0 ? std::min(options.maximumFrequencyHz, 0.5 * fs) : 0.5 * fs;
    double windowSum = 0.0;
    for (double v : w) windowSum += v;
    std::size_t kMin = guard;
    const Coarse head = peakOf(data, nfft, coarseLength, windowSum, fs, options.minimumFrequencyHz, upper, &kMin);
    Coarse chosen = head;
    // 1b. A long record: a stronger tone below what its first 2^20 samples resolve is looked for on the whole
    // record (block averages). It wins only below their lowest frequency and when it is the larger.
    if (n > kCoarseLength)
    {
        const double lowest = static_cast<double>(kMin) * binWidth;
        const std::size_t block = (n + kCoarseLength - 1) / kCoarseLength;
        const Coarse whole = coarseOfWholeRecord(x, result.dc, fs, block, options.minimumFrequencyHz, upper);
        if (whole.found && whole.frequencyHz < lowest && (!head.found || whole.amplitude > head.amplitude))
            chosen = whole;
    }
    if (!chosen.found) return result;
    result.prominenceDb = chosen.prominenceDb;
    if (result.prominenceDb < options.minimumProminenceDb) return result;
    double f = chosen.frequencyHz;

    // 2. Refinement from the phase drift between the two halves of a growing part of the record.
    const std::size_t refineHarmonics = std::min<std::size_t>(std::max<std::size_t>(1, options.harmonicCount), 10);
    bool driftOnWhole = false; // the drift was measured on the whole record at least once
    for (std::size_t length = coarseLength;; length = std::min(n, 4 * length))
    {
        const std::size_t half = length / 2;
        for (int iteration = 0; iteration < 4; ++iteration)
        {
            if (static_cast<double>(half) * f / fs < 2.0) break; // fewer than 2 periods per half
            if (length == n) driftOnWhole = true;
            HarmonicFit first, second;
            if (!fitHarmonics(x.first(half), fs, f, refineHarmonics, first, 0)) break;
            if (!fitHarmonics(x.subspan(half, length - half), fs, f, refineHarmonics, second, half)) break;
            const Complex c1 = first.coefficients.front(), c2 = second.coefficients.front();
            if (std::abs(c1) == 0.0 || std::abs(c2) == 0.0) break;
            const double centreDistance = 0.5 * static_cast<double>(length) ; // (half + (length-half)) / 2
            const double delta = std::arg(c2 * std::conj(c1)) * fs / (kTwoPi * centreDistance);
            f += delta;
            if (std::abs(delta) < 1e-13 * f) break;
        }
        if (length == n) break;
    }

    // 2b. A short record (fewer than four periods: no drift to measure): the frequency that leaves the
    // smallest residual to the fit of the fundamental and its harmonics, searched within one bin of the record
    // (golden section). A pure sine of 40 samples, 2,5 periods, then reads its frequency to 1e-9.
    if (n <= kResidualSearchUpTo && !driftOnWhole)
    {
        const auto residual = [&](double frequency) {
            HarmonicFit trial;
            if (!(frequency > 0.0) || !fitHarmonics(x, fs, frequency, refineHarmonics, trial, 0))
                return std::numeric_limits<double>::infinity();
            return trial.residualMeanSquare;
        };
        const double bin = fs / static_cast<double>(n);
        double a = std::max(f - bin, 0.5 * f), b = std::min(f + bin, 0.5 * fs);
        const double g = 0.5 * (std::sqrt(5.0) - 1.0);
        double c = b - g * (b - a), d = a + g * (b - a);
        double fc = residual(c), fd = residual(d);
        for (int iteration = 0; iteration < 80 && b - a > 1e-15 * f; ++iteration)
        {
            if (fc < fd)
            {
                b = d;
                d = c;
                fd = fc;
                c = b - g * (b - a);
                fc = residual(c);
            }
            else
            {
                a = c;
                c = d;
                fc = fd;
                d = a + g * (b - a);
                fd = residual(d);
            }
        }
        const double best = 0.5 * (a + b);
        if (residual(best) <= residual(f)) f = best;
    }

    // 3. Joint least-squares fit on the whole record.
    const std::size_t harmonics = std::max<std::size_t>(1, options.harmonicCount);
    HarmonicFit fit;
    if (!fitHarmonics(x, fs, f, harmonics, fit, 0) || std::abs(fit.coefficients.front()) == 0.0) return result;

    result.detected = true;
    result.fundamentalHz = f;
    result.fundamentalAmplitude = std::abs(fit.coefficients.front());
    result.fundamentalPhaseRad = std::arg(fit.coefficients.front());
    const std::size_t usable = usableHarmonics(f, fs, harmonics);
    double harmonicPower = 0.0;
    double largestHarmonic = 0.0;
    for (std::size_t h = 0; h < usable; ++h)
    {
        Harmonic entry;
        entry.order = static_cast<int>(h + 1);
        entry.frequencyHz = static_cast<double>(h + 1) * f;
        entry.amplitude = std::abs(fit.coefficients[h]);
        entry.phaseRad = std::arg(fit.coefficients[h]);
        entry.relativeDb = 20.0 * std::log10(std::max(entry.amplitude, 1e-300) / result.fundamentalAmplitude);
        result.harmonics.push_back(entry);
        if (h > 0)
        {
            harmonicPower += 0.5 * entry.amplitude * entry.amplitude;
            largestHarmonic = std::max(largestHarmonic, entry.amplitude);
        }
    }

    const double fundamentalPower = 0.5 * result.fundamentalAmplitude * result.fundamentalAmplitude;
    const double noisePower = std::max(fit.residualMeanSquare, 1e-30 * fundamentalPower);
    result.thdRatio = std::sqrt(harmonicPower / fundamentalPower);
    result.thdPercent = 100.0 * result.thdRatio;
    result.thdDb = 20.0 * std::log10(std::max(result.thdRatio, 1e-15));
    result.snrDb = 10.0 * std::log10(fundamentalPower / noisePower);
    result.sinadDb = 10.0 * std::log10(fundamentalPower / (harmonicPower + noisePower));
    result.enob = (result.sinadDb - 1.76) / 6.02;

    // SFDR: largest of the fitted harmonics and of the residual's spectral peaks (Blackman-Harris,
    // scalloping <= 0.83 dB) on the coarse part of the record.
    const std::vector<double> model = evaluateModel(fit, f / fs, 0, coarseLength);
    const std::vector<double> bh = makeWindow(Window::BlackmanHarris, coarseLength);
    double bhSum = 0.0;
    for (double v : bh) bhSum += v;
    std::fill(data.begin(), data.end(), Complex(0.0, 0.0));
    for (std::size_t i = 0; i < coarseLength; ++i) data[i] = Complex((x[i] - model[i]) * bh[i], 0.0);
    FftPlan(nfft).forward(data);
    double largestSpur = largestHarmonic;
    const std::size_t residualGuard = 2 * guard; // Blackman-Harris main lobe is twice as wide as Hann's
    for (std::size_t k = residualGuard; k + 1 < nfft / 2; ++k)
        largestSpur = std::max(largestSpur, 2.0 * std::abs(data[k]) / bhSum);
    result.sfdrDb = largestSpur > 0.0 ? 20.0 * std::log10(result.fundamentalAmplitude / largestSpur) : 300.0;
    return result;
}
} // namespace qacq::dsp
