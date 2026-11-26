#include "qacq/dsp/synchronization.h"

#include "qacq/dsp/constants.h"
#include "qacq/dsp/fft.h"
#include "qacq/dsp/filter_design.h"
#include "qacq/dsp/filtering.h"
#include "qacq/dsp/interpolation.h"
#include "qacq/dsp/modulation.h"
#include "qacq/dsp/rotator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace qacq::dsp
{
namespace
{
using Complex = std::complex<double>;

constexpr std::size_t kMinimumPreamble = 8;
constexpr double kQualityWarning = 0.3;
constexpr double kAcquisitionTimingBandwidth = 0.01; // the first pass's loops (B_L T)
constexpr double kAcquisitionPhaseBandwidth = 0.01;

bool rootRaisedPulse(const ReceiverSpec &spec) noexcept
{
    return spec.type != Modulation::Fsk && spec.pulse == PulseShape::RootRaisedCosine;
}

double wrap(double radians) noexcept
{
    return std::remainder(radians, kTwoPi);
}

// The matched filter at ns samples per symbol, unit scale (a symbol sent at the carrier amplitude comes out as its
// point): the root raised cosine, the mean over one symbol (window ending at the sample), or for FSK the energies
// of its two tones over one symbol and their difference (in y, real).
struct Filtered
{
    std::vector<Complex> y;
    std::vector<double> mark, space; // FSK: at fc + deltaF (a 1), at fc - deltaF (a 0)
};

Filtered matchedFilter(const std::vector<Complex> &x, const ReceiverSpec &spec, int ns, double amplitude)
{
    Filtered f;
    const std::size_t n = x.size();
    const double a = amplitude != 0.0 ? amplitude : 1.0;
    const auto window = static_cast<std::size_t>(ns);
    f.y.assign(n, {});
    if (spec.type == Modulation::Fsk)
    {
        f.mark.assign(n, 0.0);
        f.space.assign(n, 0.0);
        const double ratio = spec.fskDeviationHz / (static_cast<double>(ns) * spec.symbolRate);
        std::vector<Complex> up(n), down(n);
        Rotator deviation(ratio);
        for (std::size_t i = 0; i < n; ++i)
        {
            const Complex tone = std::conj(deviation.next());
            up[i] = turned(x[i], tone);
            down[i] = turned(x[i], std::conj(tone));
        }
        Complex su, sd;
        const double scale = 1.0 / (static_cast<double>(ns) * a);
        for (std::size_t i = 0; i < n; ++i)
        {
            su += up[i];
            sd += down[i];
            if (i >= window)
            {
                su -= up[i - window];
                sd -= down[i - window];
            }
            f.mark[i] = std::abs(su) * scale;
            f.space[i] = std::abs(sd) * scale;
            f.y[i] = {f.mark[i] - f.space[i], 0.0};
        }
        return f;
    }
    std::vector<double> re(n), im(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        re[i] = x[i].real();
        im[i] = x[i].imag();
    }
    if (rootRaisedPulse(spec))
    {
        const std::vector<double> h = rootRaisedCosine(spec.rolloff, window, pulseSpanSymbolsOf(spec.pulseSpanSymbols, spec.rolloff));
        const std::vector<double> yi = firFilterAligned(h, re), yq = firFilterAligned(h, im);
        const double scale = 1.0 / (a * std::sqrt(static_cast<double>(ns)));
        for (std::size_t i = 0; i < n; ++i) f.y[i] = {yi[i] * scale, yq[i] * scale};
        return f;
    }
    double si = 0.0, sq = 0.0;
    const double scale = 1.0 / (static_cast<double>(ns) * a);
    for (std::size_t i = 0; i < n; ++i)
    {
        si += re[i];
        sq += im[i];
        if (i >= window)
        {
            si -= re[i - window];
            sq -= im[i - window];
        }
        f.y[i] = {si * scale, sq * scale};
    }
    return f;
}

// The frequency of a tone sampled once per symbol (v_k = y_k conj(a_k): the preamble's symbols taken off), in
// cycles per symbol within +-1/2: the peak of its DFT zero-padded 64 times, refined by a parabola through the
// three bins about it (the maximum-likelihood estimate, near the Cramér-Rao bound once the peak stands out).
double toneFrequency(const std::vector<Complex> &v)
{
    const std::size_t size = nextPowerOfTwo(std::max<std::size_t>(64 * v.size(), 1024));
    std::vector<Complex> data(size);
    std::copy(v.begin(), v.end(), data.begin());
    FftPlan(size).forward(data);
    std::size_t best = 0;
    for (std::size_t k = 1; k < size; ++k)
        if (std::norm(data[k]) > std::norm(data[best])) best = k;
    const double l = std::abs(data[(best + size - 1) % size]), c = std::abs(data[best]), r = std::abs(data[(best + 1) % size]);
    const double curve = l - 2.0 * c + r;
    const double shift = curve < 0.0 ? std::clamp(0.5 * (l - r) / curve, -0.5, 0.5) : 0.0;
    double f = (static_cast<double>(best) + shift) / static_cast<double>(size);
    if (f >= 0.5) f -= 1.0;
    return f;
}

// The slope of values[from..to) against their index (least squares).
double slopeOf(const std::vector<double> &values, std::size_t from, std::size_t to) noexcept
{
    if (to <= from + 1) return 0.0;
    const auto m = static_cast<double>(to - from);
    double sk = 0.0, sv = 0.0;
    for (std::size_t k = from; k < to; ++k)
    {
        sk += static_cast<double>(k);
        sv += values[k];
    }
    const double mk = sk / m, mv = sv / m;
    double num = 0.0, den = 0.0;
    for (std::size_t k = from; k < to; ++k)
    {
        const double dk = static_cast<double>(k) - mk;
        num += dk * (values[k] - mv);
        den += dk * dk;
    }
    return den > 0.0 ? num / den : 0.0;
}

// Where the filter peaks for symbol k, in its samples (ns per symbol), the frame from sample 0.
double apexAt(const ReceiverSpec &spec, int ns, double k) noexcept
{
    const double n = static_cast<double>(ns);
    return rootRaisedPulse(spec) ? k * n : k * n + n - 1.0;
}

// Gardner's detector at t: Re{conj(y(t - T/2)) (y(t - T) - y(t))}.
double gardner(const SincInterpolator &sinc, const std::vector<Complex> &y, double t, int ns)
{
    const double n = static_cast<double>(ns);
    const Complex late = sinc.at(y, t), early = sinc.at(y, t - n), middle = sinc.at(y, t - 0.5 * n);
    return (std::conj(middle) * (early - late)).real();
}

// The slope of Gardner's detector for this pulse and constellation, per sample of lateness, at unit gain: measured
// without noise on 400 random symbols, half a sample either side of the peaks.
double gardnerSlope(const ReceiverSpec &spec, int ns)
{
    ModulationSpec m;
    m.type = spec.type;
    m.representation = Representation::ComplexBaseband;
    m.symbolRate = spec.symbolRate;
    m.sampleRateHz = static_cast<double>(ns) * spec.symbolRate;
    m.carrierAmplitude = 1.0;
    m.askLowLevel = spec.askLowLevel;
    m.fskDeviationHz = spec.fskDeviationHz;
    m.pulse = spec.pulse;
    m.rolloff = spec.rolloff;
    m.pulseSpanSymbols = spec.pulseSpanSymbols;
    constexpr std::size_t symbols = 400;
    m.bits = randomBits(symbols * static_cast<std::size_t>(std::max(1, bitsPerSymbol(spec.type))), 0x5107E);
    m.repeatBits = false;
    m.sampleCount = symbols * static_cast<std::size_t>(ns);
    const ModulatedSignal tx = modulate(m);
    if (!tx.ok()) return 0.0;
    std::vector<Complex> x(tx.inPhase.size());
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = {tx.inPhase[i], tx.quadrature[i]};
    const Filtered f = matchedFilter(x, spec, ns, 1.0);
    const SincInterpolator &sinc = sincInterpolator();
    double later = 0.0, earlier = 0.0;
    for (std::size_t k = 20; k + 20 < symbols; ++k)
    {
        const double apex = apexAt(spec, ns, static_cast<double>(k));
        later += gardner(sinc, f.y, apex + 0.5, ns);
        earlier += gardner(sinc, f.y, apex - 0.5, ns);
    }
    return (later - earlier) / static_cast<double>(symbols - 40);
}
} // namespace

int synchronizedSamplesPerSymbol(const ReceiverSpec &spec) noexcept
{
    int ns = 8;
    if (spec.type == Modulation::Fsk)
        while (std::abs(spec.fskDeviationHz) + 1.5 * spec.symbolRate > 0.3 * ns * spec.symbolRate && ns < 64) ns *= 2;
    return ns;
}

double synchronizedTarget(const ReceiverSpec &spec, double symbol) noexcept
{
    const double sps = spec.sampleRateHz / spec.symbolRate;
    return rootRaisedPulse(spec) ? symbol * sps : symbol * sps + 0.5 * sps - 0.5;
}

double synchronizedLag(const ReceiverSpec &spec) noexcept
{
    if (rootRaisedPulse(spec)) return 0.0;
    const double ns = static_cast<double>(synchronizedSamplesPerSymbol(spec));
    return 0.5 * (ns - 1.0) * spec.sampleRateHz / (ns * spec.symbolRate);
}

void loopGains(double bandwidth, double &proportional, double &integral) noexcept
{
    const double zeta = 1.0 / std::sqrt(2.0);
    const double theta = bandwidth / (zeta + 0.25 / zeta);
    const double d = 1.0 + 2.0 * zeta * theta + theta * theta;
    proportional = 4.0 * zeta * theta / d;
    integral = 4.0 * theta * theta / d;
}

SyncReception receiveSynchronized(const ReceivedInput &input, const ReceiverSpec &given, const SyncSettings &settings)
{
    SyncReception out;
    DigitalReception &rx = out.reception;
    ReceiverSpec spec = given;
    spec.carrierPhaseDegrees = 0.0; // the receiver finds it
    const double fs = spec.sampleRateHz, rs = spec.symbolRate;
    const bool fsk = spec.type == Modulation::Fsk;
    if (isAnalog(spec.type))
    {
        out.error = rx.error = "Modulation analogique : pas de synchronisation de symboles.";
        return out;
    }
    if (!(fs > 0.0 && rs > 0.0 && fs >= 2.0 * rs))
    {
        out.error = rx.error = "La synchronisation demande au moins deux échantillons par symbole.";
        return out;
    }
    const std::vector<Complex> &preamble = settings.preamble;
    const std::size_t length = preamble.size();
    if (length < kMinimumPreamble)
    {
        out.error = rx.error = "Le préambule doit compter au moins 8 symboles.";
        return out;
    }
    double preambleEnergy = 0.0;
    for (const Complex &a : preamble) preambleEnergy += std::norm(a);
    if (!(preambleEnergy > 0.0))
    {
        out.error = rx.error = "Préambule sans énergie.";
        return out;
    }

    // 1. Baseband at the nominal carrier, resampled to ns samples per symbol.
    Baseband bb = toBaseband(input, spec);
    if (!bb.ok())
    {
        out.error = rx.error = bb.error;
        return out;
    }
    rx.warnings = bb.warnings;
    const int ns = synchronizedSamplesPerSymbol(spec);
    const double n = static_cast<double>(ns);
    const double fsr = n * rs;
    const double ratio = fs / fsr; // samples of the record per sample of the receiver
    if (ratio > 1.0 + 1e-12)
    {
        // Low-pass before going down: what lies beyond 0.45 fsr would fold.
        const double transition = 0.2 * fsr;
        const KaiserEstimate kaiser = estimateKaiser(60.0, transition, fs);
        FirSpec lowPass;
        lowPass.sampleRateHz = fs;
        lowPass.frequency1Hz = 0.35 * fsr;
        lowPass.taps = std::clamp<std::size_t>(kaiser.taps | 1u, 15, 8001);
        lowPass.window = Window::Kaiser;
        lowPass.kaiserBeta = kaiser.beta;
        const FirDesign design = designFir(lowPass);
        if (design.ok())
        {
            bb.inPhase = firFilterAligned(design.taps, bb.inPhase);
            bb.quadrature = firFilterAligned(design.taps, bb.quadrature);
        }
    }
    const SincInterpolator &sinc = sincInterpolator();
    const std::size_t inputCount = std::min(bb.inPhase.size(), bb.quadrature.size());
    const auto count = static_cast<std::size_t>(std::floor(static_cast<double>(inputCount - 1) / ratio)) + 1;
    std::vector<Complex> x(count);
    for (std::size_t j = 0; j < count; ++j) x[j] = sinc.at(bb.inPhase, bb.quadrature, static_cast<double>(j) * ratio);
    const double a = spec.carrierAmplitude != 0.0 ? spec.carrierAmplitude : 1.0;
    const Filtered first = matchedFilter(x, spec, ns, a);

    // 2. The frame.
    const std::size_t span = (length - 1) * static_cast<std::size_t>(ns);
    if (count <= span + 2)
    {
        out.error = rx.error = "L'enregistrement est plus court que le préambule.";
        return out;
    }
    const auto turnedBack = [&](double hz) {
        std::vector<Complex> back(count);
        Rotator offCarrier(hz / fsr);
        for (std::size_t j = 0; j < count; ++j) back[j] = turned(x[j], std::conj(offCarrier.next()));
        return matchedFilter(back, spec, ns, a);
    };
    // A constellation off centre (ASK) keeps half its power in the carrier: products of filter outputs have the
    // same sign wherever they are taken, and its centred levels are weak. Its carrier line gives the frequency
    // offset first (the peak of the spectrum of the filter output within +-Rs/2), then the preamble is searched
    // coherently, on the signal less its mean (over 32 symbols about each sample) and the preamble less its own.
    Complex preambleMean;
    for (const Complex &symbol : preamble) preambleMean += symbol;
    preambleMean /= static_cast<double>(length);
    bool offCentreSymbols = false; // the constellation's own mean is not 0 (ASK), not that of the preamble's few symbols
    if (!fsk)
    {
        Complex mean;
        const std::vector<Complex> all = constellation(spec.type, spec.askLowLevel);
        for (const Complex &p : all) mean += p;
        offCentreSymbols = !all.empty() && std::abs(mean) > 1e-9 * static_cast<double>(all.size());
    }
    std::vector<Complex> centredPreamble = preamble;
    if (offCentreSymbols)
        for (Complex &symbol : centredPreamble) symbol -= preambleMean;
    const auto centred = [&](const std::vector<Complex> &y) {
        if (!offCentreSymbols) return y;
        const std::size_t half = 16 * static_cast<std::size_t>(ns);
        std::vector<Complex> sum(y.size() + 1);
        for (std::size_t i = 0; i < y.size(); ++i) sum[i + 1] = sum[i] + y[i];
        std::vector<Complex> c(y.size());
        for (std::size_t i = 0; i < y.size(); ++i)
        {
            const std::size_t from = i > half ? i - half : 0, to = std::min(y.size(), i + half + 1);
            c[i] = y[i] - (sum[to] - sum[from]) / static_cast<double>(to - from);
        }
        return c;
    };
    const std::size_t last = count - 1 - span;
    std::vector<double> metric(last + 1, 0.0);
    std::vector<double> bounds(last + 1, 0.0);
    double offset = 0.0;
    std::vector<Complex> searched;
    std::vector<Complex> weights(length - 1);
    for (std::size_t k = 0; k + 1 < length; ++k) weights[k] = std::conj(preamble[k + 1]) * preamble[k];
    if (offCentreSymbols)
    {
        const std::size_t size = nextPowerOfTwo(count);
        std::vector<Complex> spectrum(size);
        std::copy(first.y.begin(), first.y.end(), spectrum.begin());
        FftPlan(size).forward(spectrum);
        const auto reach = static_cast<std::size_t>(0.5 * rs / fsr * static_cast<double>(size));
        std::size_t peak = 0;
        for (std::size_t k = 0; k < size; ++k)
        {
            if (k > reach && k < size - reach) continue;
            if (std::norm(spectrum[k]) > std::norm(spectrum[peak])) peak = k;
        }
        const double l = std::abs(spectrum[(peak + size - 1) % size]), c = std::abs(spectrum[peak]), r = std::abs(spectrum[(peak + 1) % size]);
        const double curve = l - 2.0 * c + r;
        double f = (static_cast<double>(peak) + (curve < 0.0 ? std::clamp(0.5 * (l - r) / curve, -0.5, 0.5) : 0.0)) / static_cast<double>(size);
        if (f >= 0.5) f -= 1.0;
        offset = f * fsr;
        searched = centred(turnedBack(offset).y);
        for (std::size_t s = 0; s <= last; ++s)
        {
            Complex sum;
            double bound = 0.0;
            for (std::size_t k = 0; k < length; ++k)
            {
                const Complex v = searched[s + k * static_cast<std::size_t>(ns)];
                sum += v * std::conj(centredPreamble[k]);
                bound += std::abs(v) * std::abs(centredPreamble[k]);
            }
            metric[s] = std::abs(sum);
            bounds[s] = bound;
        }
    }
    else
    {
        // The products of filter outputs one symbol apart against those of the preamble's symbols: whatever the
        // frequency offset (it turns them all alike), within +-Rs/2.
        searched = first.y;
        std::vector<Complex> products(count - static_cast<std::size_t>(ns));
        for (std::size_t s = 0; s < products.size(); ++s) products[s] = searched[s + static_cast<std::size_t>(ns)] * std::conj(searched[s]);
        for (std::size_t s = 0; s <= last; ++s)
        {
            Complex d;
            double bound = 0.0;
            for (std::size_t k = 0; k + 1 < length; ++k)
            {
                const Complex p = products[s + k * static_cast<std::size_t>(ns)];
                d += p * weights[k];
                bound += std::abs(p) * std::abs(weights[k]);
            }
            metric[s] = std::abs(d);
            bounds[s] = bound;
        }
    }
    std::size_t best = 0;
    for (std::size_t s = 1; s <= last; ++s)
        if (metric[s] > metric[best]) best = s;
    double start = static_cast<double>(best);
    if (best > 0 && best < last)
    {
        const double l = metric[best - 1], c = metric[best], r = metric[best + 1];
        const double curve = l - 2.0 * c + r;
        if (curve < 0.0) start += std::clamp(0.5 * (l - r) / curve, -0.5, 0.5);
    }
    out.frameQuality = bounds[best] > 0.0 ? metric[best] / bounds[best] : 0.0;
    out.frameFound = out.frameQuality >= kQualityWarning;
    if (!out.frameFound)
        out.warnings.push_back("Préambule mal trouvé (corrélation " + std::to_string(static_cast<int>(std::lround(100.0 * out.frameQuality))) +
                               " %) : trop de bruit, ou un décalage de fréquence trop grand.");

    // The frequency offset (not FSK: non-coherent, it needs none). Coarse: how much the preamble turns from one
    // symbol to the next (within +-Rs/2, but noisy: a few hertz at 1 kBd), or the carrier line of an ASK; then
    // fine, the baseband turned back by it and filtered again: the preamble's symbols taken off leave a tone,
    // whose frequency the DFT finds.
    Filtered filtered;
    if (!fsk)
    {
        if (!offCentreSymbols)
        {
            Complex d;
            for (std::size_t k = 0; k + 1 < length; ++k)
                d += sinc.at(searched, start + static_cast<double>(k + 1) * n) * std::conj(sinc.at(searched, start + static_cast<double>(k) * n)) *
                     weights[k];
            offset = std::arg(d) * rs / kTwoPi;
        }
        const Filtered coarse = turnedBack(offset);
        std::vector<Complex> tone(length);
        for (std::size_t k = 0; k < length; ++k) tone[k] = sinc.at(coarse.y, start + static_cast<double>(k) * n) * std::conj(preamble[k]);
        offset += toneFrequency(tone) * rs;
        filtered = turnedBack(offset);
        // The start again, finer: where the preamble's correlation itself peaks (sharper than that of the products,
        // and unbiased whatever the levels of the constellation), a parabola through the best sample.
        const std::vector<Complex> filteredCentred = centred(filtered.y);
        const auto correlation = [&](double at) {
            Complex sum;
            for (std::size_t k = 0; k < length; ++k)
                sum += sinc.at(filteredCentred, at + static_cast<double>(k) * n) * std::conj(centredPreamble[k]);
            return std::abs(sum);
        };
        const double from = std::max(0.0, std::floor(start) - 0.5 * n), to = std::min(static_cast<double>(last), std::ceil(start) + 0.5 * n);
        double bestAt = std::round(start), bestValue = correlation(bestAt);
        for (double at = from; at <= to; at += 1.0)
        {
            const double v = correlation(at);
            if (v > bestValue)
            {
                bestValue = v;
                bestAt = at;
            }
        }
        const double l = correlation(bestAt - 1.0), r = correlation(bestAt + 1.0);
        const double curve = l - 2.0 * bestValue + r;
        start = bestAt + (curve < 0.0 ? std::clamp(0.5 * (l - r) / curve, -0.5, 0.5) : 0.0);
    }
    else
    {
        filtered = first;
    }

    // 3. Phase and gain from the preamble.
    Complex c;
    for (std::size_t k = 0; k < length; ++k) c += sinc.at(filtered.y, start + static_cast<double>(k) * n) * std::conj(preamble[k]);
    c /= preambleEnergy;
    const double gain = std::abs(c);
    if (!(gain > 1e-12))
    {
        out.error = rx.error = "Préambule introuvable : rien de reçu.";
        return out;
    }
    out.gain = gain;
    out.phaseRadians = fsk ? 0.0 : std::arg(c);
    out.frameStart = start * ratio;

    // 4 and 5. Timing and carrier loops, symbol after symbol, in two passes. The first, with wide loops, learns
    // over the whole frame how the instants and the carrier phase run (a straight line each, by least squares:
    // the clock's drift, what the preamble left of the frequency offset); the second, with the bandwidths asked
    // for, starts on those lines and follows what departs from them, with little jitter.
    const double slope = gardnerSlope(spec, ns);
    const bool timing = std::abs(slope) > 1e-9 && settings.timingBandwidth > 0.0;
    if (!(std::abs(slope) > 1e-9)) out.warnings.push_back("Le détecteur de rythme ne voit rien sur ce signal : instants tenus fixes.");
    const std::vector<Complex> points = fsk ? std::vector<Complex>{} : constellation(spec.type, spec.askLowLevel);
    const double limit = static_cast<double>(count) - static_cast<double>(sinc.halfWidth()) - 1.0;
    Complex centre; // the mean of the symbols (ASK: half-way between its levels): Gardner's detector sees the signal without it
    for (const Complex &p : points) centre += p;
    if (!points.empty()) centre /= static_cast<double>(points.size());
    const bool offCentre = std::abs(centre) > 1e-9;
    double meanEnergy = 0.0; // of the constellation: the phase detector Im(z conj(d)) over it has a unit slope on average
    for (const Complex &p : points) meanEnergy += std::norm(p);
    meanEnergy = points.empty() ? 1.0 : std::max(1e-9, meanEnergy / static_cast<double>(points.size()));

    const double preamblePhase = out.phaseRadians; // what the preamble alone says of the carrier
    struct Pass
    {
        std::vector<double> instants, thetas;
        std::vector<Complex> points;
        double nu = 0.0;
    };
    const auto run = [&](double timingBandwidth, double phaseBandwidth, double t, double drift, double theta, double nu, bool keep) {
        Pass pass;
        double tp = 0.0, ti = 0.0, pp = 0.0, pi = 0.0;
        if (timing) loopGains(timingBandwidth, tp, ti);
        if (!fsk && phaseBandwidth > 0.0) loopGains(phaseBandwidth, pp, pi);
        double level = 1.0;
        Complex previous, mean = centre * std::polar(1.0, theta);
        for (std::size_t k = 0; t < limit; ++k)
        {
            const Complex value = sinc.at(filtered.y, t) / gain;
            double error = 0.0;
            if (k > 0 && tp > 0.0)
            {
                const Complex middle = sinc.at(filtered.y, t - 0.5 * n) / gain - mean;
                error = std::clamp((std::conj(middle) * (previous - value)).real() / slope, -0.5 * n, 0.5 * n);
                drift += ti * error;
            }
            pass.instants.push_back(t);
            if (fsk)
            {
                pass.points.emplace_back(sinc.at(filtered.mark, t), sinc.at(filtered.space, t));
                pass.thetas.push_back(0.0);
            }
            else
            {
                const Complex z = value * std::polar(1.0, -theta) / level;
                pass.points.push_back(z);
                pass.thetas.push_back(theta);
                if (keep)
                {
                    out.preamblePoints.push_back(value * std::polar(1.0, -preamblePhase));
                    out.preambleHalfPoints.push_back(sinc.at(filtered.y, t - 0.5 * n) / gain * std::polar(1.0, -preamblePhase));
                }
                const Complex wanted = k < length ? preamble[k] : points[decideSymbol(spec.type, z, spec.askLowLevel)];
                const double energy = std::norm(wanted);
                const double e = (z * std::conj(wanted)).imag() / meanEnergy;
                theta += pp * e + nu;
                nu += pi * e;
                // The level, after the preamble: what the decisions say of the amplitude, over some 256 symbols.
                if (k >= length && energy > 1e-18) level *= 1.0 + ((z * std::conj(wanted)).real() / energy - 1.0) / 256.0;
            }
            if (keep) out.rawPoints.push_back(sinc.at(first.y, start + static_cast<double>(k) * n));
            previous = value;
            if (offCentre) mean += (value - mean) / 32.0;
            t += n - std::clamp(tp * error + drift, -0.25 * n, 0.25 * n);
        }
        pass.nu = nu;
        return pass;
    };
    const double timingBandwidth = std::max(0.0, settings.timingBandwidth);
    const double phaseBandwidth = std::max(0.0, settings.phaseBandwidth);
    double t0 = start, drift0 = 0.0, theta0 = out.phaseRadians, nu0 = 0.0;
    {
        const Pass wide = run(std::max(timingBandwidth, kAcquisitionTimingBandwidth), std::max(phaseBandwidth, kAcquisitionPhaseBandwidth), start, 0.0,
                              out.phaseRadians, 0.0, false);
        const std::size_t m = wide.instants.size();
        if (m > length + 64)
        {
            // From the end of the preamble on (the wide loops have settled by then).
            const std::size_t from = std::min(m - 32, length + 64);
            const auto line = [&](const std::vector<double> &v, double &intercept, double &rate) {
                rate = slopeOf(v, from, m);
                double mv = 0.0, mk = 0.0;
                for (std::size_t k = from; k < m; ++k)
                {
                    mv += v[k];
                    mk += static_cast<double>(k);
                }
                mv /= static_cast<double>(m - from);
                mk /= static_cast<double>(m - from);
                intercept = mv - rate * mk;
            };
            double period = n;
            if (timing) line(wide.instants, t0, period);
            drift0 = n - period;
            if (!fsk && phaseBandwidth > 0.0) line(wide.thetas, theta0, nu0);
        }
    }
    const Pass pass = run(timingBandwidth, phaseBandwidth, t0, drift0, theta0, nu0, true);
    rx.decisionPoints = pass.points;
    rx.decisionTimes = pass.instants;
    std::vector<double> thetas = pass.thetas;
    for (std::size_t k = 0; k < pass.instants.size(); ++k)
    {
        out.instants.push_back(pass.instants[k] * ratio);
        out.phases.push_back(fsk ? 0.0 : kTwoPi * offset * pass.instants[k] / fsr + pass.thetas[k]);
        if (!fsk) out.preamblePhases.push_back(kTwoPi * offset * pass.instants[k] / fsr + preamblePhase);
    }
    const double nu = pass.nu;
    const std::size_t symbols = rx.decisionPoints.size();
    if (symbols == 0)
    {
        out.error = rx.error = "Aucun symbole après le préambule.";
        return out;
    }
    out.frequencyOffsetHz = offset + nu * rs / kTwoPi;
    // Where symbol 0 is decided, and the carrier phase at the start of the record: the phases the loop took off over
    // the preamble, less the frequency found times their instants (a mean on the circle).
    out.frameStart = out.instants.front();
    if (!fsk)
    {
        Complex at0;
        for (std::size_t k = 0; k < std::min(length, out.phases.size()); ++k)
            at0 += std::polar(1.0, out.phases[k] - kTwoPi * out.frequencyOffsetHz * out.instants[k] / fs);
        if (std::abs(at0) > 0.0) out.phaseRadians = std::arg(at0);
    }
    // The clock: the period the instants keep after the preamble (least squares: the loop's jitter averages out).
    if (symbols > length + 16)
    {
        double sk = 0.0, st = 0.0;
        const auto m = static_cast<double>(symbols - length);
        for (std::size_t k = length; k < symbols; ++k)
        {
            sk += static_cast<double>(k);
            st += rx.decisionTimes[k];
        }
        const double mk = sk / m, mt = st / m;
        double num = 0.0, den = 0.0;
        for (std::size_t k = length; k < symbols; ++k)
        {
            const double dk = static_cast<double>(k) - mk;
            num += dk * (rx.decisionTimes[k] - mt);
            den += dk * dk;
        }
        if (den > 0.0) out.clockOffsetPpm = 1e6 * (num / den / n - 1.0);
    }

    // Decisions.
    const int bps = std::max(1, bitsPerSymbol(spec.type));
    rx.symbols.reserve(symbols);
    rx.bits.reserve(symbols * static_cast<std::size_t>(bps));
    for (const Complex &p : rx.decisionPoints)
    {
        const std::size_t symbol = fsk ? (p.real() > p.imag() ? 1u : 0u) : decideSymbol(spec.type, p, spec.askLowLevel);
        rx.symbols.push_back(symbol);
        for (int b = bps - 1; b >= 0; --b) rx.bits.push_back(static_cast<std::uint8_t>((symbol >> b) & 1u));
    }
    rx.samplesPerSymbol = n;
    rx.matchedRateHz = fsr;
    rx.basebandI = std::move(bb.inPhase);
    rx.basebandQ = std::move(bb.quadrature);
    // The eye: the filter output turned back by the carrier found (its phase between two symbols in a straight
    // line), at unit scale.
    if (spec.matchedOutput && !fsk)
    {
        rx.matchedI.assign(count, 0.0);
        rx.matchedQ.assign(count, 0.0);
        std::size_t k = 0;
        for (std::size_t j = 0; j < count; ++j)
        {
            const double at = static_cast<double>(j);
            while (k + 1 < symbols && rx.decisionTimes[k + 1] <= at) ++k;
            double phase = thetas[k];
            if (k + 1 < symbols && at > rx.decisionTimes[k])
            {
                const double u = (at - rx.decisionTimes[k]) / (rx.decisionTimes[k + 1] - rx.decisionTimes[k]);
                phase += u * wrap(thetas[k + 1] - thetas[k]);
            }
            const Complex v = filtered.y[j] / gain * std::polar(1.0, -phase);
            rx.matchedI[j] = v.real();
            rx.matchedQ[j] = v.imag();
        }
    }
    for (const std::string &w : out.warnings) rx.warnings.push_back(w);
    if (!fsk && spec.pulse == PulseShape::RootRaisedCosine && spec.rolloff < 0.2)
        rx.warnings.push_back("Facteur de retombée sous 0,2 : le détecteur de Gardner voit peu le rythme (il vit de l'excès de bande).");
    return out;
}
} // namespace qacq::dsp
