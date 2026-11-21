#include "qacq/dsp/spectrum.h"

#include "qacq/dsp/fft.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace qacq::dsp
{
namespace
{
bool validRate(double fs) noexcept
{
    return std::isfinite(fs) && fs > 0.0;
}

// True for the bins that stand for both a positive and a negative frequency in a one-sided spectrum.
bool doubledBin(std::size_t k, std::size_t nfft) noexcept
{
    return k > 0 && !(nfft % 2 == 0 && k == nfft / 2);
}

double mean(std::span<const double> x) noexcept
{
    double sum = 0.0;
    for (double v : x) sum += v;
    return x.empty() ? 0.0 : sum / static_cast<double>(x.size());
}

// Mean of x weighted by the window (plain mean when `weighted` is false or the window sums to zero).
double offsetOf(std::span<const double> x, std::span<const double> w, bool weighted) noexcept
{
    if (!weighted || w.size() < x.size()) return mean(x);
    double sum = 0.0, weights = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        sum += w[i] * x[i];
        weights += w[i];
    }
    return weights != 0.0 ? sum / weights : mean(x);
}

std::size_t overlapSamples(double overlap, std::size_t segment) noexcept
{
    const double clamped = std::clamp(overlap, 0.0, 0.95);
    const auto samples = static_cast<std::size_t>(std::floor(clamped * static_cast<double>(segment) + 1e-9));
    return std::min(samples, segment > 0 ? segment - 1 : 0);
}

// Windowed one-sided periodogram of one segment, accumulated into psd (unit^2/Hz).
void accumulatePsd(std::span<const double> segment, std::span<const double> window, bool removeMean,
                   bool windowedMean, const FftPlan &plan, double fs, double windowSquares,
                   std::vector<Complex> &work, std::vector<double> &psd)
{
    const double offset = removeMean ? offsetOf(segment, window, windowedMean) : 0.0;
    const std::size_t nfft = plan.size();
    std::fill(work.begin(), work.end(), Complex(0.0, 0.0));
    for (std::size_t i = 0; i < segment.size(); ++i) work[i] = Complex((segment[i] - offset) * window[i], 0.0);
    plan.forward(work);
    const double scale = 1.0 / (fs * windowSquares);
    for (std::size_t k = 0; k < psd.size(); ++k)
        psd[k] += std::norm(work[k]) * scale * (doubledBin(k, nfft) ? 2.0 : 1.0);
}
std::size_t fftLength(std::size_t n, const SpectrumOptions &options) noexcept
{
    return options.fftSize >= n ? options.fftSize : n * std::max<std::size_t>(1, options.zeroPadFactor);
}

} // namespace

Spectrum spectrum(std::span<const double> x, double fs, const SpectrumOptions &options)
{
    Spectrum result;
    if (x.size() < 2 || !validRate(fs)) return result;

    const std::size_t n = x.size();
    const std::size_t nfft = fftLength(n, options);
    const std::vector<double> w = makeWindow(options.window, n, WindowSymmetry::Periodic, options.kaiserBeta);
    double sum = 0.0, squares = 0.0;
    for (double v : w)
    {
        sum += v;
        squares += v * v;
    }
    const double offset = options.removeMean ? offsetOf(x, w, options.windowedMean) : 0.0;

    std::vector<Complex> data(nfft, Complex(0.0, 0.0));
    for (std::size_t i = 0; i < n; ++i) data[i] = Complex((x[i] - offset) * w[i], 0.0);
    FftPlan(nfft).forward(data);

    const std::size_t bins = nfft / 2 + 1;
    result.frequencyHz.resize(bins);
    result.amplitude.resize(bins);
    result.psd.resize(bins);
    result.phaseRad.resize(bins);
    for (std::size_t k = 0; k < bins; ++k)
    {
        const double factor = doubledBin(k, nfft) ? 2.0 : 1.0;
        result.frequencyHz[k] = static_cast<double>(k) * fs / static_cast<double>(nfft);
        result.amplitude[k] = factor * std::abs(data[k]) / sum;
        result.psd[k] = factor * std::norm(data[k]) / (fs * squares);
        result.phaseRad[k] = std::arg(data[k]);
    }
    result.sampleRateHz = fs;
    result.binWidthHz = fs / static_cast<double>(nfft);
    result.coherentGain = sum / static_cast<double>(n);
    result.enbwHz = fs * squares / (sum * sum);
    result.samplesUsed = n;
    result.fftSize = nfft;
    return result;
}

ComplexSpectrum complexSpectrum(std::span<const double> inPhase, std::span<const double> quadrature, double fs,
                                const SpectrumOptions &options)
{
    ComplexSpectrum result;
    const std::size_t n = std::min(inPhase.size(), quadrature.size());
    if (n < 2 || !validRate(fs)) return result;

    const std::size_t nfft = fftLength(n, options);
    const std::vector<double> w = makeWindow(options.window, n, WindowSymmetry::Periodic, options.kaiserBeta);
    double sum = 0.0, squares = 0.0;
    for (double v : w)
    {
        sum += v;
        squares += v * v;
    }
    double meanI = 0.0, meanQ = 0.0;
    if (options.removeMean)
    {
        meanI = offsetOf(inPhase.first(n), w, options.windowedMean);
        meanQ = offsetOf(quadrature.first(n), w, options.windowedMean);
    }
    std::vector<Complex> data(nfft, Complex(0.0, 0.0));
    for (std::size_t i = 0; i < n; ++i) data[i] = Complex(inPhase[i] - meanI, quadrature[i] - meanQ) * w[i];
    FftPlan(nfft).forward(data);

    result.frequencyHz.resize(nfft);
    result.amplitude.resize(nfft);
    result.psd.resize(nfft);
    result.phaseRad.resize(nfft);
    // Output index j holds frequency (j - floor(nfft/2)) * fs / nfft: the same layout as numpy.fft.fftshift.
    const std::size_t half = nfft / 2;
    for (std::size_t j = 0; j < nfft; ++j)
    {
        const std::size_t k = (j + nfft - half) % nfft;
        const double bin = static_cast<double>(j) - static_cast<double>(half);
        result.frequencyHz[j] = bin * fs / static_cast<double>(nfft);
        result.amplitude[j] = std::abs(data[k]) / sum;
        result.psd[j] = std::norm(data[k]) / (fs * squares);
        result.phaseRad[j] = std::arg(data[k]);
    }
    result.sampleRateHz = fs;
    result.binWidthHz = fs / static_cast<double>(nfft);
    result.enbwHz = fs * squares / (sum * sum);
    result.samplesUsed = n;
    result.fftSize = nfft;
    return result;
}

WelchResult welch(std::span<const double> x, double fs, const WelchOptions &options)
{
    WelchResult result;
    if (x.size() < 2 || !validRate(fs)) return result;

    const std::size_t segment = std::clamp<std::size_t>(options.segmentLength, 2, x.size());
    const std::size_t hop = segment - overlapSamples(options.overlap, segment);
    const std::vector<double> w = makeWindow(options.window, segment, WindowSymmetry::Periodic, options.kaiserBeta);
    double sum = 0.0, squares = 0.0;
    for (double v : w)
    {
        sum += v;
        squares += v * v;
    }

    const FftPlan plan(segment);
    std::vector<Complex> work(segment);
    std::vector<double> psd(segment / 2 + 1, 0.0);
    std::size_t count = 0;
    for (std::size_t start = 0; start + segment <= x.size(); start += hop)
    {
        accumulatePsd(x.subspan(start, segment), w, options.removeSegmentMean, options.windowedMean, plan, fs, squares,
                      work, psd);
        ++count;
    }
    for (double &v : psd) v /= static_cast<double>(count);

    result.frequencyHz.resize(psd.size());
    for (std::size_t k = 0; k < psd.size(); ++k)
        result.frequencyHz[k] = static_cast<double>(k) * fs / static_cast<double>(segment);
    result.psd = std::move(psd);
    result.segments = count;
    result.segmentLength = segment;
    result.hop = hop;
    result.enbwHz = fs * squares / (sum * sum);
    return result;
}

Spectrogram spectrogram(std::span<const double> x, double fs, const SpectrogramOptions &options, const Progress &progress)
{
    Spectrogram result;
    if (x.size() < 2 || !validRate(fs)) return result;

    const std::size_t segment = std::clamp<std::size_t>(options.segmentLength, 2, x.size());
    std::size_t hop = segment - overlapSamples(options.overlap, segment);
    const std::size_t maximum = std::max<std::size_t>(1, options.maximumFrames);
    // The segments every `hop` samples, and one more that ends with the record when they stop short of it
    // (otherwise up to hop - 1 samples at the end would be in none).
    const std::size_t regular = 1 + (x.size() - segment) / hop;
    const bool spread = !options.averageSegments && regular > maximum;
    std::size_t perFrame = 1; // segments averaged into one frame
    if (spread)
    {
        // Spread the frames over the whole record rather than stopping early.
        hop = std::max(hop, (x.size() - segment) / std::max<std::size_t>(1, maximum - 1));
        while (1 + (x.size() - segment) / hop > maximum) ++hop;
    }
    const std::size_t stepped = 1 + (x.size() - segment) / hop;
    const bool endSegment = !spread && (stepped - 1) * hop + segment < x.size();
    const std::size_t segments = stepped + (endSegment ? 1 : 0);
    if (options.averageSegments && segments > maximum) perFrame = (segments + maximum - 1) / maximum;
    const auto startOf = [&](std::size_t g) { return g < stepped ? g * hop : x.size() - segment; };
    const std::size_t frames = (segments + perFrame - 1) / perFrame;
    const std::vector<double> w = makeWindow(options.window, segment, WindowSymmetry::Periodic, options.kaiserBeta);
    double sum = 0.0;
    for (double v : w) sum += v;

    const FftPlan plan(segment);
    const std::size_t bins = segment / 2 + 1;
    std::vector<Complex> work(segment);
    std::vector<double> power(bins);
    result.frequencyHz.resize(bins);
    for (std::size_t k = 0; k < bins; ++k)
        result.frequencyHz[k] = static_cast<double>(k) * fs / static_cast<double>(segment);
    result.amplitudeDb.reserve(frames * bins);
    result.timeSeconds.reserve(frames);
    std::size_t done = 0;
    for (std::size_t f = 0; f < frames; ++f)
    {
        const std::size_t first = f * perFrame;
        const std::size_t last = std::min(segments, first + perFrame); // past the last segment of the frame
        std::fill(power.begin(), power.end(), 0.0);
        for (std::size_t g = first; g < last; ++g)
        {
            const auto part = x.subspan(startOf(g), segment);
            const double offset = options.removeSegmentMean ? offsetOf(part, w, options.windowedMean) : 0.0;
            for (std::size_t i = 0; i < segment; ++i) work[i] = Complex((part[i] - offset) * w[i], 0.0);
            plan.forward(work);
            for (std::size_t k = 0; k < bins; ++k)
            {
                const double amplitude = (doubledBin(k, segment) ? 2.0 : 1.0) * std::abs(work[k]) / sum;
                power[k] += amplitude * amplitude;
            }
            if (++done % 64 == 0)
            {
                if (progress.cancelled && progress.cancelled->load(std::memory_order_relaxed)) return {};
                if (progress.permille)
                    progress.permille->store(static_cast<int>(done * 1000 / segments), std::memory_order_relaxed);
            }
        }
        // The amplitude of the mean power: a steady sinusoid reads its own amplitude.
        const double count = static_cast<double>(last - first);
        for (std::size_t k = 0; k < bins; ++k)
            result.amplitudeDb.push_back(10.0 * std::log10(std::max(power[k] / count, 1e-300)));
        const double from = static_cast<double>(startOf(first));
        const double to = static_cast<double>(startOf(last - 1) + segment);
        result.timeSeconds.push_back(0.5 * (from + to) / fs);
        ++result.frames;
    }
    if (progress.permille) progress.permille->store(1000, std::memory_order_relaxed);
    result.bins = bins;
    result.segmentLength = segment;
    result.hop = hop;
    result.segmentsPerFrame = perFrame;
    result.frameDurationSeconds = static_cast<double>((perFrame - 1) * hop + segment) / fs;
    result.frameStepSeconds = static_cast<double>(perFrame * hop) / fs;
    return result;
}
} // namespace qacq::dsp
