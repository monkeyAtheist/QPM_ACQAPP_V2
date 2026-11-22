#include "qacq/analysis/analysis.h"
#include "qacq/analysis/calculus.h"
#include "qacq/analysis/measures.h"

#include "qacq/dsp/fft.h"
#include "qacq/dsp/spectrum.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <ostream>
#include <utility>

namespace qacq::analysis
{
namespace
{
constexpr std::string_view kRootHertz = "\xE2\x88\x9AHz"; // √Hz

// The name of the spectrum of a pair I, Q: "I + jQ"; a pair named alike, "I émis" and "Q émis": "I + jQ émis"
// (shorter than "I émis + jQ émis"); otherwise both names.
std::string complexCurveName(const std::string &i, const std::string &q)
{
    if (!i.empty() && !q.empty() && i.front() == 'I' && q.front() == 'Q' && i.compare(1, std::string::npos, q, 1) == 0)
        return "I + jQ" + i.substr(1);
    return i + " + j" + q;
}
constexpr std::size_t kPyramidFrom = 32768;
constexpr std::size_t kMinimumZone = 8;

constexpr std::array<std::pair<dsp::Window, std::string_view>, 7> kWindows = {{
    {dsp::Window::Rectangular, "rectangular"},
    {dsp::Window::Hann, "hann"},
    {dsp::Window::Hamming, "hamming"},
    {dsp::Window::Blackman, "blackman"},
    {dsp::Window::BlackmanHarris, "blackmanHarris"},
    {dsp::Window::FlatTop, "flatTop"},
    {dsp::Window::Kaiser, "kaiser"},
}};

// "10 000 000" with narrow no-break spaces, as the interface writes counts.
std::string grouped(std::size_t value)
{
    std::string digits = std::to_string(value);
    std::string out;
    const std::size_t n = digits.size();
    for (std::size_t i = 0; i < n; ++i)
    {
        if (i > 0 && (n - i) % 3 == 0) out += "\xE2\x80\xAF";
        out += digits[i];
    }
    return out;
}

std::string number(double value, int digits)
{
    if (!std::isfinite(value)) return {};
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.*g", digits, value);
    std::string text = buffer;
    std::replace(text.begin(), text.end(), '.', ',');
    return text;
}

bool cancelledNow(const std::atomic<bool> *cancelled) noexcept
{
    return cancelled && cancelled->load(std::memory_order_relaxed);
}

// The zone of one channel, missing samples (NaN) replaced by the mean of the others.
std::vector<double> zoneOf(std::span<const double> values, std::size_t &missing)
{
    std::vector<double> x(values.begin(), values.end());
    double sum = 0.0;
    std::size_t finite = 0;
    for (double v : x)
    {
        if (std::isfinite(v))
        {
            sum += v;
            ++finite;
        }
    }
    missing = x.size() - finite;
    if (missing > 0)
    {
        const double mean = finite > 0 ? sum / static_cast<double>(finite) : 0.0;
        for (double &v : x)
        {
            if (!std::isfinite(v)) v = mean;
        }
    }
    return x;
}

ChannelMeasures measure(std::span<const double> x, const model::ChannelInfo &info, bool tone,
                        const AnalysisSettings &settings, double sampleRateHz)
{
    ChannelMeasures m;
    m.name = info.name;
    m.unit = info.unit;
    double sum = 0.0, squares = 0.0;
    double mn = std::numeric_limits<double>::infinity(), mx = -mn;
    for (double v : x)
    {
        if (!std::isfinite(v)) continue;
        if (m.samples == 0) m.first = v;
        m.last = v;
        ++m.samples;
        sum += v;
        squares += v * v;
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    if (m.samples == 0) return m;
    const double count = static_cast<double>(m.samples);
    m.mean = sum / count;
    m.rms = std::sqrt(squares / count);
    double ac = 0.0;
    for (double v : x)
    {
        if (std::isfinite(v)) ac += (v - m.mean) * (v - m.mean);
    }
    m.acRms = std::sqrt(ac / count);
    m.minimum = mn;
    m.maximum = mx;
    m.peakToPeak = mx - mn;
    m.crestFactor = m.acRms > 0.0 ? std::max(mx - m.mean, m.mean - mn) / m.acRms : 0.0;
    m.integral = integral(x, sampleRateHz);
    if (tone && m.samples == x.size() && x.size() >= 16)
    {
        dsp::ToneOptions options;
        options.harmonicCount = settings.harmonics;
        m.tone = dsp::analyzeTone(x, sampleRateHz, options);
        if (m.tone.detected)
        {
            // ENBW in bins hardly depends on the length: taken on a short window.
            const double enbwBins = dsp::equivalentNoiseBandwidthBins(
                dsp::makeWindow(settings.window, 1024, dsp::WindowSymmetry::Periodic, settings.kaiserBeta));
            const double sigma = m.tone.fundamentalAmplitude / std::sqrt(2.0) * std::pow(10.0, -m.tone.snrDb / 20.0);
            m.noiseInBin = 2.0 * sigma * std::sqrt(enbwBins / static_cast<double>(x.size()));
            for (const dsp::Harmonic &h : m.tone.harmonics)
                m.harmonicStandsOut.push_back(h.order == 1 ||
                                              (h.relativeDb > -150.0 && h.amplitude > 3.0 * m.noiseInBin));
        }
    }
    return m;
}

void finishCurve(SpectrumCurve &curve, const AnalysisResult &result)
{
    curve.level.resize(curve.linear.size());
    std::size_t peak = 0;
    double best = -std::numeric_limits<double>::infinity();
    for (std::size_t k = 0; k < curve.linear.size(); ++k)
    {
        curve.level[k] = levelOf(curve.linear[k], result.kind, result.scale);
        if (curve.level[k] > best)
        {
            best = curve.level[k];
            peak = k;
        }
    }
    curve.peakBin = peak;
    if (curve.level.size() >= kPyramidFrom) curve.pyramid.build(curve.level);
}

// Welch on an I/Q pair: the average of the two-sided densities of overlapping segments.
dsp::ComplexSpectrum complexWelch(std::span<const double> i, std::span<const double> q, double fs,
                                  const AnalysisSettings &settings, std::size_t &segments)
{
    const std::size_t n = std::min(i.size(), q.size());
    const std::size_t length = std::clamp<std::size_t>(settings.segmentLength, 2, n);
    const auto overlapped = static_cast<std::size_t>(std::llround(std::clamp(settings.overlap, 0.0, 0.95) *
                                                                  static_cast<double>(length)));
    const std::size_t hop = std::max<std::size_t>(1, length - std::min(overlapped, length - 1));
    dsp::SpectrumOptions options;
    options.window = settings.window;
    options.kaiserBeta = settings.kaiserBeta;
    options.removeMean = settings.removeMean;
    options.windowedMean = true;
    dsp::ComplexSpectrum total;
    segments = 0;
    for (std::size_t start = 0; start + length <= n; start += hop)
    {
        dsp::ComplexSpectrum one = dsp::complexSpectrum(i.subspan(start, length), q.subspan(start, length), fs, options);
        if (segments == 0) total = std::move(one);
        else
        {
            for (std::size_t k = 0; k < total.psd.size(); ++k) total.psd[k] += one.psd[k];
        }
        ++segments;
    }
    if (segments > 1)
    {
        for (double &v : total.psd) v /= static_cast<double>(segments);
    }
    return total;
}
// (lobeHalfWidth: mainLobeHalfWidth below.)
double lobeHalfWidth(const AnalysisSettings &settings) noexcept
{
    switch (settings.window)
    {
        case dsp::Window::Rectangular: return 1.0;
        case dsp::Window::Hann:
        case dsp::Window::Hamming: return 2.0;
        case dsp::Window::Blackman: return 3.0;
        case dsp::Window::BlackmanHarris: return 4.0;
        case dsp::Window::FlatTop: return 5.0;
        case dsp::Window::Kaiser: return std::sqrt(1.0 + (settings.kaiserBeta / 3.141592653589793) *
                                                             (settings.kaiserBeta / 3.141592653589793));
    }
    return 2.0;
}

double medianOf(std::vector<double> values)
{
    if (values.empty()) return 0.0;
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

// Which harmonics stand out of the amplitude spectrum around them: the bins on either side of each one,
// outside its main lobe, give the local floor (median: other lines nearby hardly move it).
void markHarmonics(ChannelMeasures &m, const SpectrumCurve &curve, const AnalysisResult &result,
                   const AnalysisSettings &settings)
{
    if (!m.tone.detected || curve.linear.empty() || result.zoneFrames == 0) return;
    const double padding = static_cast<double>(result.fftSize) / static_cast<double>(result.zoneFrames);
    const auto lobe = static_cast<std::ptrdiff_t>(std::ceil((lobeHalfWidth(settings) + 1.0) * padding));
    const std::ptrdiff_t side = std::max<std::ptrdiff_t>(16, 8 * lobe);
    const auto bins = static_cast<std::ptrdiff_t>(curve.linear.size());
    std::vector<double> all;
    all.reserve(curve.linear.size());
    for (std::ptrdiff_t k = lobe; k < bins; ++k) all.push_back(curve.linear[static_cast<std::size_t>(k)]);
    m.spectrumFloor = medianOf(std::move(all));
    m.harmonicStandsOut.assign(m.tone.harmonics.size(), false);
    for (std::size_t j = 0; j < m.tone.harmonics.size(); ++j)
    {
        const dsp::Harmonic &h = m.tone.harmonics[j];
        if (h.order == 1)
        {
            m.harmonicStandsOut[j] = true;
            continue;
        }
        const auto centre = static_cast<std::ptrdiff_t>(std::llround(result.binAt(h.frequencyHz)));
        std::vector<double> around;
        for (std::ptrdiff_t k = centre - lobe - side; k <= centre + lobe + side; ++k)
        {
            if (std::abs(k - centre) <= lobe || k < lobe || k >= bins) continue;
            around.push_back(curve.linear[static_cast<std::size_t>(k)]);
        }
        const double floor = around.size() >= 8 ? medianOf(std::move(around)) : m.spectrumFloor;
        m.harmonicStandsOut[j] = h.relativeDb > -150.0 && h.amplitude > 3.0 * floor;
    }
}
} // namespace

double mainLobeHalfWidth(const AnalysisSettings &settings) noexcept
{
    return lobeHalfWidth(settings);
}

std::string_view spectrumKindKey(SpectrumKind kind) noexcept
{
    return kind == SpectrumKind::Density ? "density" : "amplitude";
}

std::string_view windowKey(dsp::Window window) noexcept
{
    for (const auto &[w, key] : kWindows)
    {
        if (w == window) return key;
    }
    return "hann";
}

bool windowFromKey(std::string_view key, dsp::Window &window) noexcept
{
    for (const auto &[w, k] : kWindows)
    {
        if (k == key)
        {
            window = w;
            return true;
        }
    }
    return false;
}

model::Json toJson(const AnalysisSettings &s)
{
    model::Json j = model::Json::object();
    j["kind"] = spectrumKindKey(s.kind);
    j["window"] = windowKey(s.window);
    j["kaiserBeta"] = s.kaiserBeta;
    j["zeroPadFactor"] = static_cast<unsigned long long>(s.zeroPadFactor);
    j["removeMean"] = s.removeMean;
    j["segmentLength"] = static_cast<unsigned long long>(s.segmentLength);
    j["overlap"] = s.overlap;
    j["scale"] = s.scale == LevelScale::Linear ? "linear" : "db";
    j["logFrequency"] = s.logFrequency;
    j["harmonics"] = static_cast<unsigned long long>(s.harmonics);
    j["edgeLow"] = s.edgeLow;
    j["spectrogramSegment"] = static_cast<unsigned long long>(s.spectrogramSegment);
    j["spectrogramOverlap"] = s.spectrogramOverlap;
    return j;
}

AnalysisSettings analysisSettingsFromJson(const model::Json &j)
{
    AnalysisSettings s;
    if (!j.isObject()) return s;
    s.kind = j["kind"].toString() == "density" ? SpectrumKind::Density : SpectrumKind::Amplitude;
    dsp::Window w = s.window;
    if (windowFromKey(j["window"].toString(), w)) s.window = w;
    s.kaiserBeta = std::clamp(j["kaiserBeta"].toNumber(s.kaiserBeta), 0.0, 50.0);
    const double pad = j["zeroPadFactor"].toNumber(1.0);
    s.zeroPadFactor = (pad == 2.0 || pad == 4.0 || pad == 8.0) ? static_cast<std::size_t>(pad) : 1;
    s.removeMean = j["removeMean"].toBool(s.removeMean);
    const double segment = j["segmentLength"].toNumber(static_cast<double>(s.segmentLength));
    if (std::isfinite(segment)) s.segmentLength = static_cast<std::size_t>(std::clamp(segment, 16.0, 1048576.0));
    const double overlap = j["overlap"].toNumber(s.overlap);
    if (std::isfinite(overlap)) s.overlap = std::clamp(overlap, 0.0, 0.95);
    s.scale = j["scale"].toString() == "linear" ? LevelScale::Linear : LevelScale::Decibel;
    s.logFrequency = j["logFrequency"].toBool(s.logFrequency);
    const double harmonics = j["harmonics"].toNumber(static_cast<double>(s.harmonics));
    if (std::isfinite(harmonics)) s.harmonics = static_cast<std::size_t>(std::clamp(harmonics, 2.0, 50.0));
    s.edgeLow = j["edgeLow"].toNumber(s.edgeLow) == 0.2 ? 0.2 : 0.1;
    const double stft = j["spectrogramSegment"].toNumber(static_cast<double>(s.spectrogramSegment));
    if (std::isfinite(stft)) s.spectrogramSegment = static_cast<std::size_t>(std::clamp(stft, 16.0, 65536.0));
    const double stftOverlap = j["spectrogramOverlap"].toNumber(s.spectrogramOverlap);
    if (std::isfinite(stftOverlap)) s.spectrogramOverlap = std::clamp(stftOverlap, 0.0, 0.9);
    return s;
}

double levelOf(double linear, SpectrumKind kind, LevelScale scale) noexcept
{
    if (!std::isfinite(linear)) return std::numeric_limits<double>::quiet_NaN();
    if (kind == SpectrumKind::Amplitude)
        return scale == LevelScale::Decibel ? 20.0 * std::log10(std::max(std::abs(linear), 1e-20)) : linear;
    // Density in unit²/Hz.
    return scale == LevelScale::Decibel ? 10.0 * std::log10(std::max(linear, 1e-40)) : std::sqrt(std::max(linear, 0.0));
}

std::string levelUnit(std::string_view unit, SpectrumKind kind, LevelScale scale)
{
    const bool density = kind == SpectrumKind::Density;
    if (scale == LevelScale::Linear)
    {
        if (!density) return std::string(unit);
        return (unit.empty() ? std::string("1") : std::string(unit)) + "/" + std::string(kRootHertz);
    }
    if (unit.empty()) return density ? "dB/Hz" : "dB";
    if (unit == "V" || unit == "W") return "dB" + std::string(unit) + (density ? "/" + std::string(kRootHertz) : "");
    return "dB re 1 " + std::string(unit) + (density ? "/" + std::string(kRootHertz) : "");
}

plot::Range levelRange(double minimum, double maximum, LevelScale scale)
{
    if (!(minimum <= maximum) || !std::isfinite(minimum) || !std::isfinite(maximum)) return {0.0, 1.0};
    if (scale == LevelScale::Decibel)
    {
        const double high = std::ceil((maximum + 3.0) / 10.0) * 10.0;
        double low = std::floor(std::max(minimum, maximum - 160.0) / 10.0) * 10.0;
        if (high - low < 20.0) low = high - 20.0;
        return {low, high};
    }
    const double low = std::min(0.0, minimum);
    double high = maximum + 0.1 * (maximum - low); // room for the marks of the harmonics
    if (!(high > low)) high = low + 1.0;
    return {low, high};
}

AnalysisResult analyse(const model::Signal &signal, std::size_t firstFrame, std::size_t frameCount,
                       const AnalysisSettings &settings, const std::atomic<bool> *cancelled,
                       const std::vector<std::size_t> *spectrumChannels)
{
    AnalysisResult result;
    result.kind = settings.kind;
    result.scale = settings.scale;
    const std::size_t frames = signal.frameCount();
    const double fs = signal.sampleRateHz;
    if (frames == 0 || signal.channelCount() == 0)
    {
        result.error = "Ce signal n'a aucun échantillon.";
        return result;
    }
    if (!(fs > 0.0) || !std::isfinite(fs))
    {
        result.error = "La fréquence d'échantillonnage de ce signal est inconnue : indiquez-la dans ses propriétés.";
        return result;
    }
    firstFrame = std::min(firstFrame, frames);
    frameCount = std::min(frameCount, frames - firstFrame);
    result.zoneFirstFrame = firstFrame;
    result.zoneFrames = frameCount;
    result.sampleRateHz = fs;
    if (frameCount < kMinimumZone)
    {
        result.error = "Zone trop courte : il faut au moins " + std::to_string(kMinimumZone) +
                       " échantillons (élargissez la vue du signal).";
        return result;
    }

    const bool iq = signal.representation == model::Representation::IQ && signal.channelCount() >= 2;
    result.twoSided = iq;
    const auto zone = [&](std::size_t c) { return signal.channel(c).subspan(firstFrame, frameCount); };
    // The curves asked for: every channel (or I/Q pair), or those of the list (a pair when one of its two
    // channels is in it).
    const std::size_t curveCount = iq ? signal.channelCount() / 2 : signal.channelCount();
    const auto asked = [&](std::size_t k) {
        if (!spectrumChannels) return true;
        const auto listed = [&](std::size_t c) {
            return std::find(spectrumChannels->begin(), spectrumChannels->end(), c) != spectrumChannels->end();
        };
        return iq ? listed(2 * k) || listed(2 * k + 1) : listed(k);
    };
    bool anyCurve = false;
    for (std::size_t k = 0; k < curveCount; ++k) anyCurve = anyCurve || asked(k);

    // FFT length of the amplitude spectrum.
    std::size_t fftSize = 0;
    if (settings.kind == SpectrumKind::Amplitude && anyCurve)
    {
        const std::size_t factor = std::max<std::size_t>(1, settings.zeroPadFactor);
        const std::size_t wanted = frameCount * factor;
        fftSize = frameCount > kExactFftUpTo ? dsp::nextPowerOfTwo(wanted) : wanted;
        if (fftSize > kMaximumFftSize)
        {
            result.error = "Zone de " + grouped(frameCount) + " échantillons : un spectre d'une seule FFT est limité à " +
                           grouped(kMaximumFftSize) + " points" + (factor > 1 ? " (zéros ajoutés compris)" : "") +
                           ". Zoomez sur une partie du signal, ou passez en densité spectrale (Welch), "
                           "qui parcourt tout le signal par segments.";
            return result;
        }
        result.paddedToPowerOfTwo = fftSize != wanted;
    }

    dsp::SpectrumOptions spectrumOptions;
    spectrumOptions.window = settings.window;
    spectrumOptions.kaiserBeta = settings.kaiserBeta;
    spectrumOptions.removeMean = settings.removeMean;
    spectrumOptions.windowedMean = true;
    spectrumOptions.fftSize = fftSize;

    std::size_t missingTotal = 0;
    for (std::size_t k = 0; k < curveCount; ++k)
    {
        if (!asked(k)) continue;
        if (cancelledNow(cancelled))
        {
            result.error = "annulé";
            return result;
        }
        SpectrumCurve curve;
        if (iq)
        {
            const std::size_t ci = 2 * k, cq = 2 * k + 1;
            std::size_t missingI = 0, missingQ = 0;
            const std::vector<double> i = zoneOf(zone(ci), missingI);
            const std::vector<double> q = zoneOf(zone(cq), missingQ);
            missingTotal += missingI + missingQ;
            curve.name = complexCurveName(signal.channels[ci].name, signal.channels[cq].name);
            curve.unit = signal.channels[ci].unit;
            curve.channels = {static_cast<int>(ci), static_cast<int>(cq)};
            dsp::ComplexSpectrum s;
            if (settings.kind == SpectrumKind::Amplitude)
            {
                s = dsp::complexSpectrum(i, q, fs, spectrumOptions);
                curve.linear = std::move(s.amplitude);
                result.fftSize = s.fftSize;
            }
            else
            {
                s = complexWelch(i, q, fs, settings, result.segments);
                curve.linear = std::move(s.psd);
                result.fftSize = s.fftSize;
            }
            result.firstFrequencyHz = s.frequencyHz.empty() ? 0.0 : s.frequencyHz.front();
            result.binWidthHz = s.binWidthHz;
            result.enbwHz = s.enbwHz;
        }
        else
        {
            std::size_t missing = 0;
            const std::vector<double> x = zoneOf(zone(k), missing);
            missingTotal += missing;
            curve.name = signal.channels[k].name;
            curve.unit = signal.channels[k].unit;
            curve.channels = {static_cast<int>(k)};
            result.firstFrequencyHz = 0.0;
            if (settings.kind == SpectrumKind::Amplitude)
            {
                dsp::Spectrum s = dsp::spectrum(x, fs, spectrumOptions);
                curve.linear = std::move(s.amplitude);
                result.binWidthHz = s.binWidthHz;
                result.enbwHz = s.enbwHz;
                result.fftSize = s.fftSize;
            }
            else
            {
                dsp::WelchOptions options;
                options.window = settings.window;
                options.kaiserBeta = settings.kaiserBeta;
                options.segmentLength = std::min(settings.segmentLength, frameCount);
                options.overlap = settings.overlap;
                options.removeSegmentMean = settings.removeMean;
                options.windowedMean = true;
                dsp::WelchResult w = dsp::welch(x, fs, options);
                curve.linear = std::move(w.psd);
                result.segments = w.segments;
                result.fftSize = w.segmentLength;
                result.binWidthHz = w.segmentLength > 0 ? fs / static_cast<double>(w.segmentLength) : 0.0;
                result.enbwHz = w.enbwHz;
            }
        }
        result.bins = curve.linear.size();
        finishCurve(curve, result);
        result.curves.push_back(std::move(curve));
        result.curves.back().peaks = findPeaks(result, result.curves.size() - 1, 10, settings);
        result.curves.back().band = halfPowerBand(result, result.curves.size() - 1);
    }

    for (std::size_t c = 0; c < signal.channelCount(); ++c)
    {
        if (cancelledNow(cancelled))
        {
            result.error = "annulé";
            return result;
        }
        result.measures.push_back(measure(zone(c), signal.channels[c], !iq, settings, fs));
        if (!iq) result.measures.back().wave = waveMeasures(zone(c), fs, settings.edgeLow);
        if (!iq && settings.kind == SpectrumKind::Amplitude)
        {
            for (const SpectrumCurve &curve : result.curves)
            {
                if (!curve.channels.empty() && curve.channels.front() == static_cast<int>(c))
                    markHarmonics(result.measures.back(), curve, result, settings);
            }
        }
    }

    if (missingTotal > 0)
        result.notes.push_back(grouped(missingTotal) +
                               " échantillons manquants remplacés par la moyenne de leur voie pour le spectre.");
    if (result.paddedToPowerOfTwo)
        result.notes.push_back("FFT de " + grouped(result.fftSize) + " points : zéros ajoutés après les " +
                               grouped(frameCount) + " échantillons (tous utilisés).");
    if (settings.kind == SpectrumKind::Density && result.segments == 1)
        result.notes.push_back("Un seul segment tient dans la zone : pas de moyenne (choisissez des segments "
                               "plus courts ou un recouvrement plus grand).");
    if (iq)
        result.notes.push_back("Paire I/Q : spectre de −fs/2 à +fs/2, pas de mesure de tonalité.");
    return result;
}

void setScale(AnalysisResult &result, LevelScale scale)
{
    if (result.scale == scale) return;
    result.scale = scale;
    for (SpectrumCurve &curve : result.curves)
    {
        curve.pyramid.clear();
        finishCurve(curve, result);
        for (Peak &peak : curve.peaks) peak.level = levelOf(peak.linear, result.kind, scale);
    }
}

void writeSpectrumCsv(std::ostream &out, const AnalysisResult &result)
{
    out << "Fréquence (Hz)";
    for (const SpectrumCurve &c : result.curves)
    {
        out << ';' << c.name << " (" << levelUnit(c.unit, result.kind, LevelScale::Decibel) << ')';
        out << ';' << c.name << " (" << levelUnit(c.unit, result.kind, LevelScale::Linear) << ')';
    }
    out << '\n';
    for (std::size_t k = 0; k < result.bins; ++k)
    {
        out << number(result.frequencyAt(static_cast<double>(k)), 12);
        for (const SpectrumCurve &c : result.curves)
        {
            const double v = k < c.linear.size() ? c.linear[k] : std::numeric_limits<double>::quiet_NaN();
            out << ';' << number(levelOf(v, result.kind, LevelScale::Decibel), 8);
            out << ';' << number(levelOf(v, result.kind, LevelScale::Linear), 8);
        }
        out << '\n';
    }
}
} // namespace qacq::analysis
