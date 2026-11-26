#include "qacq/dsp/receiver.h"

#include "qacq/dsp/constants.h"
#include "qacq/dsp/rotator.h"
#include "qacq/dsp/filter_design.h"
#include "qacq/dsp/filtering.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace qacq::dsp
{
double gaussianQ(double x) noexcept
{
    return 0.5 * std::erfc(x / std::sqrt(2.0));
}

namespace
{
// exp(-c); through flat fading (fading: true), its mean over the power x = |g|^2 of the gain (mean 1), E[exp(-c x)]:
// the moment generating function of x, (1 + K) / (1 + K + c) exp(-K c / (1 + K + c)) (Rice; K = 0: Rayleigh).
double attenuation(double c, double riceK, bool fading) noexcept
{
    if (!fading) return std::exp(-c);
    return (1.0 + riceK) / (1.0 + riceK + c) * std::exp(-riceK * c / (1.0 + riceK + c));
}

// Probability that the angle of a point of energy es (over N0) with noise passes alpha (0 < alpha < pi) on one
// side, by Craig's form: (1 / 2 pi) integral from 0 to pi - alpha of exp(-es sin^2 alpha / sin^2 t) dt. The
// integrand rises from 0 to its peak at pi/2 (or at the end of the range): Gauss-Legendre of 16 points on 64
// pieces, exact to the last digits from 0 to 40 dB. Through flat fading the exponential becomes its mean over the
// gain (attenuation): the same integral, smoother.
// Gauss-Legendre of 16 points: its nodes (+-x) and weights on [-1, 1].
constexpr double kGaussX[8] = {0.0950125098376374, 0.2816035507792589, 0.4580167776572274, 0.6178762444026438,
                               0.7554044083550030, 0.8656312023878318, 0.9445750230732326, 0.9894009349916499};
constexpr double kGaussW[8] = {0.1894506104550685, 0.1826034150449236, 0.1691565193950025, 0.1495959888165767,
                               0.1246289712555339, 0.0951585116824928, 0.0622535239386479, 0.0271524594117541};

// The integral of f over [0, upper]: Gauss-Legendre of 16 points on `pieces` equal pieces. graded: the first one
// cut in halves towards 0, 24 times (through fading, Craig's integrands rise sharply near 0 when the argument is
// small against the line of sight: there, every halving its 16 points).
template <typename Integrand>
double craigIntegral(double upper, int pieces, bool graded, const Integrand &f) noexcept
{
    const auto panel = [&f](double mid, double half) {
        double part = 0.0;
        for (int k = 0; k < 8; ++k)
            for (double sign : {-1.0, 1.0}) part += kGaussW[k] * f(mid + sign * half * kGaussX[k]);
        return half * part;
    };
    const double width = upper / pieces;
    double sum = 0.0;
    for (int piece = graded ? 1 : 0; piece < pieces; ++piece) sum += panel((piece + 0.5) * width, 0.5 * width);
    if (graded)
    {
        double to = width;
        for (int halving = 0; halving < 24; ++halving, to *= 0.5) sum += panel(0.75 * to, 0.25 * to);
    }
    return sum;
}

double beyondAngle(double es, double alpha, double riceK = 0.0, bool fading = false) noexcept
{
    const double s2 = std::sin(alpha) * std::sin(alpha);
    const double sum = craigIntegral(kPi - alpha, 64, fading, [&](double t) {
        const double st = std::sin(t);
        return attenuation(es * s2 / (st * st), riceK, fading);
    });
    return sum / (2.0 * kPi);
}

// The mean over flat fading (power x of the gain, mean 1) of Q(a sqrt(x)) and of Q(a sqrt(x))^2: Craig's forms, (1 / pi)
// times the integral over 0 ... pi / 2 (or pi / 4 for the square) of exp(-a^2 / (2 sin^2 t)), the exponential
// averaged in closed form (attenuation).
double fadedQ(double a, double riceK, bool squared) noexcept
{
    if (!(a > 0.0)) return squared ? 0.25 : 0.5;
    const double c = 0.5 * a * a;
    return craigIntegral(squared ? 0.25 * kPi : 0.5 * kPi, 32, true, [&](double t) {
               const double st = std::sin(t);
               return attenuation(c / (st * st), riceK, true);
           }) /
           kPi;
}

// Of the levels of a Gray-coded L-PAM, at r = half the distance between levels over the noise deviation: the
// bit errors per symbol (averaged over the levels sent) and the probability that a level is wrong. q: the Gaussian
// tail, or its mean over a fading (each of its arguments, the odd multiples of r up to 2 L - 1, taken once).
template <typename Tail>
void pamErrors(std::size_t levels, double r, double &bitErrors, double &symbolError, const Tail &q) noexcept
{
    std::vector<double> tail(levels + 1); // tail[m] = q((2 m - 1) r)
    for (std::size_t m = 1; m <= levels; ++m) tail[m] = q((2.0 * static_cast<double>(m) - 1.0) * r);
    bitErrors = 0.0;
    for (std::size_t i = 0; i < levels; ++i)
        for (std::size_t j = 0; j < levels; ++j)
        {
            if (i == j) continue;
            const std::size_t d = i < j ? j - i : i - j;
            // Between its two edges, 2d - 1 and 2d + 1 half-distances away (the outer levels have one edge).
            const bool outer = j == 0 || j + 1 == levels;
            const double p = tail[d] - (outer ? 0.0 : tail[d + 1]);
            bitErrors += p * static_cast<double>(std::popcount(grayCode(i) ^ grayCode(j)));
        }
    bitErrors /= static_cast<double>(levels);
    symbolError = 2.0 * (1.0 - 1.0 / static_cast<double>(levels)) * tail[1];
}

// 8-PSK: the probability of each sector away from the one sent (by symmetry of the circle), from the
// half-planes beyond its edges (fading: their means over the gain).
void psk8Errors(double ebN0, double &ber, double &ser, double riceK = 0.0, bool fading = false) noexcept
{
    const double es = 3.0 * ebN0;
    double beyond[5]; // beyond (2j - 1) pi / 8, j = 1 to 4
    for (int j = 1; j <= 4; ++j) beyond[j] = beyondAngle(es, (2.0 * j - 1.0) * kPi / 8.0, riceK, fading);
    // Sector j (centred on j pi / 4), j = 1, 2, 3 on each side, and 4 opposite.
    double sector[5] = {0.0, beyond[1] - beyond[2], beyond[2] - beyond[3], beyond[3] - beyond[4], 2.0 * beyond[4]};
    ser = 2.0 * beyond[1];
    // The bits each offset changes, averaged over the eight points sent.
    ber = 0.0;
    for (int offset = 1; offset < 8; ++offset)
    {
        const int j = offset <= 4 ? offset : 8 - offset;
        double flips = 0.0;
        for (std::size_t p = 0; p < 8; ++p)
            flips += std::popcount(grayCode(p) ^ grayCode((p + static_cast<std::size_t>(offset)) % 8));
        ber += sector[j] * flips / 8.0;
    }
    ber /= 3.0;
}

void errorRates(Modulation modulation, double ebN0Db, double askLowLevel, double &ber, double &ser) noexcept
{
    const double g = std::pow(10.0, ebN0Db / 10.0);
    switch (modulation)
    {
        case Modulation::Bpsk:
            ber = ser = gaussianQ(std::sqrt(2.0 * g));
            return;
        case Modulation::Qpsk:
        {
            ber = gaussianQ(std::sqrt(2.0 * g));
            ser = ber * (2.0 - ber); // (1 - (1 - p)^2, without losing a small p)
            return;
        }
        case Modulation::Ask:
        {
            const double l = askLowLevel;
            ber = ser = gaussianQ(std::sqrt(g * (1.0 - l) * (1.0 - l) / (1.0 + l * l)));
            return;
        }
        case Modulation::Fsk:
            ber = ser = 0.5 * std::exp(-0.5 * g);
            return;
        case Modulation::Psk8:
            psk8Errors(g, ber, ser);
            return;
        case Modulation::Qam16:
        case Modulation::Qam64:
        case Modulation::Qam256:
        {
            const int k = bitsPerSymbol(modulation);
            const std::size_t levels = std::size_t{1} << (k / 2);
            const double m = static_cast<double>(levels * levels);
            const double r = std::sqrt(3.0 * k * g / (m - 1.0));
            double perAxisBits = 0.0, perAxisSymbol = 0.0;
            pamErrors(levels, r, perAxisBits, perAxisSymbol, [](double a) { return gaussianQ(a); });
            ber = perAxisBits / (0.5 * k); // each axis carries half the bits, and errs alike
            ser = perAxisSymbol * (2.0 - perAxisSymbol);
            return;
        }
        default:
            ber = ser = std::numeric_limits<double>::quiet_NaN();
            return;
    }
}
} // namespace

double theoreticalBer(Modulation modulation, double ebN0Db, double askLowLevel) noexcept
{
    double ber = 0.0, ser = 0.0;
    errorRates(modulation, ebN0Db, askLowLevel, ber, ser);
    return ber;
}

double theoreticalSer(Modulation modulation, double ebN0Db, double askLowLevel) noexcept
{
    double ber = 0.0, ser = 0.0;
    errorRates(modulation, ebN0Db, askLowLevel, ber, ser);
    return ser;
}

namespace
{
// I0(z) e^-z (the Rice density at any K without overflow).
double scaledBesselI0(double z) noexcept
{
    // Its series (all terms positive: nothing cancels) up to 50; beyond, the asymptotic one, its terms
    // ((2k - 1)!!)^2 / (k! 8^k z^k), to the eighth (the next under 2e-13 there).
    if (z < 50.0)
    {
        const double q = 0.25 * z * z;
        double term = 1.0, sum = 1.0;
        for (int k = 1; k < 400; ++k)
        {
            term *= q / (static_cast<double>(k) * static_cast<double>(k));
            sum += term;
            if (term < 1e-17 * sum) break;
        }
        return sum * std::exp(-z);
    }
    const double r = 1.0 / z;
    double term = 1.0, sum = 1.0;
    for (int k = 1; k <= 8; ++k)
    {
        const double odd = 2.0 * k - 1.0;
        term *= odd * odd * r / (8.0 * k);
        sum += term;
    }
    return sum / std::sqrt(kTwoPi * z);
}

// The mean over flat fading of the bit or symbol rate (bits: true) at Eb/N0 times x, x = |g|^2 of mean 1.
double overFading(Modulation modulation, double ebN0Db, double askLowLevel, double riceK, bool bits) noexcept
{
    if (!std::isfinite(ebN0Db) || !std::isfinite(riceK)) return std::numeric_limits<double>::quiet_NaN();
    const double k = std::max(0.0, riceK);
    if (modulation == Modulation::Psk8)
    {
        // Craig's integrals with the exponential averaged in closed form: one integral, not one per gain.
        double ber = 0.0, ser = 0.0;
        psk8Errors(std::pow(10.0, ebN0Db / 10.0), ber, ser, k, true);
        return std::clamp(bits ? ber : ser, 0.0, 1.0);
    }
    if (modulation == Modulation::Qam16 || modulation == Modulation::Qam64 || modulation == Modulation::Qam256)
    {
        // The same for each tail of the levels; a symbol wrong on either axis, 2 p - p^2 with p = c Q: its mean
        // 2 c E[Q] - c^2 E[Q^2].
        const int bps = bitsPerSymbol(modulation);
        const std::size_t levels = std::size_t{1} << (bps / 2);
        const double m = static_cast<double>(levels * levels);
        const double r = std::sqrt(3.0 * bps * std::pow(10.0, ebN0Db / 10.0) / (m - 1.0));
        if (bits)
        {
            double perAxisBits = 0.0, perAxisSymbol = 0.0;
            pamErrors(levels, r, perAxisBits, perAxisSymbol, [k](double a) { return fadedQ(a, k, false); });
            return std::clamp(perAxisBits / (0.5 * bps), 0.0, 1.0);
        }
        const double c = 2.0 * (1.0 - 1.0 / static_cast<double>(levels));
        return std::clamp(2.0 * c * fadedQ(r, k, false) - c * c * fadedQ(r, k, true), 0.0, 1.0);
    }
    const auto density = [k](double x) {
        if (k == 0.0) return std::exp(-x);
        const double a = std::sqrt((1.0 + k) * x) - std::sqrt(k);
        return (1.0 + k) * std::exp(-a * a) * scaledBesselI0(2.0 * std::sqrt(k * (1.0 + k) * x));
    };
    const auto rate = [&](double x) {
        double ber = 0.0, ser = 0.0;
        errorRates(modulation, ebN0Db + 10.0 * std::log10(x), askLowLevel, ber, ser);
        return bits ? ber : ser;
    };
    // Simpson over s = ln x, from 1e-12 (below: the rate at no signal, over that little probability) to where the
    // density is under e^-60. A strong line of sight gathers |g|^2 about 1, sqrt(2 / (1 + K)) wide: from where the
    // density is under e^-120 (nothing below counts, short of rates under 1e-50), the steps an eighth of that.
    constexpr double kSmallest = 1e-12;
    const double root = std::sqrt(k), high = std::sqrt(60.0), deep = std::sqrt(120.0);
    const double largest = (root + high) * (root + high) / (1.0 + k);
    const double lowest = root > deep ? std::max(kSmallest, (root - deep) * (root - deep) / (1.0 + k)) : kSmallest;
    const double s0 = std::log(lowest), s1 = std::log(largest);
    const double spread = std::min(1.0, std::sqrt(2.0 / (1.0 + k)));
    const int intervals = 2 * std::clamp(static_cast<int>(std::ceil(4.0 * (s1 - s0) / spread)), 384, 10000);
    const double h = (s1 - s0) / intervals;
    double sum = 0.0;
    for (int i = 0; i <= intervals; ++i)
    {
        const double x = std::exp(s0 + h * i);
        const double weight = i == 0 || i == intervals ? 1.0 : i % 2 ? 4.0 : 2.0;
        sum += weight * rate(x) * density(x) * x;
    }
    const double result = sum * h / 3.0 + (lowest <= kSmallest ? rate(kSmallest) * density(0.0) * kSmallest : 0.0);
    return std::isfinite(result) ? std::clamp(result, 0.0, 1.0) : result; // (NaN: an analog modulation)
}
} // namespace

double fadingBer(Modulation modulation, double ebN0Db, double askLowLevel, double riceK) noexcept
{
    return overFading(modulation, ebN0Db, askLowLevel, riceK, true);
}

double fadingSer(Modulation modulation, double ebN0Db, double askLowLevel, double riceK) noexcept
{
    return overFading(modulation, ebN0Db, askLowLevel, riceK, false);
}

BitErrors countBitErrors(std::span<const std::uint8_t> sent, std::span<const std::uint8_t> received, std::size_t skip)
{
    BitErrors result;
    const std::size_t count = std::min(sent.size(), received.size());
    for (std::size_t i = skip; i < count; ++i)
    {
        ++result.compared;
        if ((sent[i] != 0) != (received[i] != 0)) ++result.errors;
    }
    result.rate = result.compared ? static_cast<double>(result.errors) / static_cast<double>(result.compared) : 0.0;
    return result;
}

namespace
{
// Back to complex baseband, carrier phase removed: r = 2 x exp(-j (2 pi fc t + phi)) for a passband record
// (r = b + conj(b) exp(-j 2 (2 pi fc t + phi)): the wanted signal plus its image around -2 fc), or
// r = b exp(-j phi) for a complex record.
bool mixDown(const ReceivedInput &input, const ReceiverSpec &spec, std::vector<double> &ri, std::vector<double> &rq,
             std::string &error)
{
    const double fs = spec.sampleRateHz;
    const std::complex<double> derotate = std::polar(1.0, -spec.carrierPhaseDegrees * kPi / 180.0);
    if (spec.representation == Representation::ComplexBaseband)
    {
        const std::size_t n = std::min(input.inPhase.size(), input.quadrature.size());
        if (n == 0)
        {
            error = "Signal I/Q vide.";
            return false;
        }
        ri.resize(n);
        rq.resize(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            const std::complex<double> v = std::complex<double>(input.inPhase[i], input.quadrature[i]) * derotate;
            ri[i] = v.real();
            rq[i] = v.imag();
        }
        return true;
    }
    const std::size_t n = input.passband.size();
    if (n == 0)
    {
        error = "Signal vide.";
        return false;
    }
    if (!(spec.carrierFrequencyHz > 0.0 && spec.carrierFrequencyHz < 0.5 * fs))
    {
        error = "La porteuse doit être comprise entre 0 et fs/2.";
        return false;
    }
    ri.resize(n);
    rq.resize(n);
    Rotator carrier(spec.carrierFrequencyHz / fs, 0.0, 2.0);
    for (std::size_t i = 0; i < n; ++i)
    {
        const std::complex<double> lo = turned(std::conj(carrier.next()), derotate);
        ri[i] = input.passband[i] * lo.real();
        rq[i] = input.passband[i] * lo.imag();
    }
    return true;
}

// Removes the 2 fc image of a mixed-down passband record with a linear-phase Kaiser low-pass (60 dB):
// the signal occupies |f| <= bandEdge, the image |f - image| <= bandEdge with image = 2 fc folded.
void removeImage(const ReceiverSpec &spec, double bandEdgeHz, std::vector<double> &ri, std::vector<double> &rq,
                 std::vector<std::string> &warnings)
{
    if (spec.representation == Representation::ComplexBaseband) return;
    const double fs = spec.sampleRateHz;
    const double folded = std::fmod(2.0 * spec.carrierFrequencyHz, fs);
    const double image = std::min(folded, fs - folded);
    const double transition = image - 2.0 * bandEdgeHz;
    if (transition <= 0.02 * fs)
    {
        warnings.emplace_back("L'image à 2 fc recouvre la bande du signal : choisir une autre porteuse.");
        return;
    }
    const KaiserEstimate kaiser = estimateKaiser(60.0, transition, fs);
    FirSpec lowPass;
    lowPass.sampleRateHz = fs;
    lowPass.frequency1Hz = 0.5 * image;
    lowPass.taps = std::clamp<std::size_t>(kaiser.taps | 1u, 15, 4001);
    lowPass.window = Window::Kaiser;
    lowPass.kaiserBeta = kaiser.beta;
    const FirDesign design = designFir(lowPass);
    if (!design.ok()) return;
    ri = firFilterAligned(design.taps, ri);
    rq = firFilterAligned(design.taps, rq);
}

void smooth(std::vector<double> &x, double cutoffHz, double fs)
{
    if (!(cutoffHz > 0.0 && cutoffHz < 0.45 * fs) || x.size() < 16) return;
    const KaiserEstimate kaiser = estimateKaiser(60.0, 0.5 * cutoffHz, fs);
    FirSpec lowPass;
    lowPass.sampleRateHz = fs;
    lowPass.frequency1Hz = 1.25 * cutoffHz;
    lowPass.taps = std::clamp<std::size_t>(kaiser.taps | 1u, 15, 4001);
    lowPass.window = Window::Kaiser;
    lowPass.kaiserBeta = kaiser.beta;
    const FirDesign design = designFir(lowPass);
    if (design.ok()) x = firFilterAligned(design.taps, x);
}
} // namespace

DigitalReception receiveDigital(const ReceivedInput &input, const ReceiverSpec &spec)
{
    DigitalReception out;
    const double fs = spec.sampleRateHz, rs = spec.symbolRate;
    if (isAnalog(spec.type))
    {
        out.error = "Modulation analogique : utiliser receiveAnalog.";
        return out;
    }
    if (!(fs > 0.0 && rs > 0.0 && fs >= rs))
    {
        out.error = "Il faut fs > 0, un débit de symboles positif et au moins un échantillon par symbole.";
        return out;
    }
    const bool rrc = spec.pulse == PulseShape::RootRaisedCosine && spec.type != Modulation::Fsk;
    const double bandEdge = receiverBandEdgeHz(spec);
    out.matchedRateHz = fs;
    std::vector<double> ri, rq;
    if (!mixDown(input, spec, ri, rq, out.error)) return out;
    out.basebandI = ri;
    out.basebandQ = rq;
    removeImage(spec, bandEdge, out.basebandI, out.basebandQ, out.warnings);
    const std::size_t n = ri.size();
    const double a = spec.carrierAmplitude != 0.0 ? spec.carrierAmplitude : 1.0;
    const auto symbols = static_cast<std::size_t>(std::floor(static_cast<double>(n) * rs / fs + 1e-9));
    const double sps = fs / rs;
    out.samplesPerSymbol = sps;

    if (rrc)
    {
        const auto spsInteger = static_cast<std::size_t>(std::llround(sps));
        if (std::abs(sps - static_cast<double>(spsInteger)) > 1e-9)
        {
            out.error = "Le cosinus surélevé demande un nombre entier d'échantillons par symbole.";
            return out;
        }
        const std::vector<double> h = rootRaisedCosine(spec.rolloff, spsInteger, pulseSpanSymbolsOf(spec.pulseSpanSymbols, spec.rolloff));
        const double scale = 1.0 / (a * std::sqrt(static_cast<double>(spsInteger)));
        if (spec.matchedOutput)
        {
            out.matchedI = firFilterAligned(h, out.basebandI); // image filter, then matched filter
            out.matchedQ = firFilterAligned(h, out.basebandQ);
            for (double &v : out.matchedI) v *= scale;
            for (double &v : out.matchedQ) v *= scale;
        }
        // The filter output at the symbol instants: sum of h[t] x[i + delay - t] (as firFilterAligned gives it).
        const std::size_t delay = (h.size() - 1) / 2;
        for (std::size_t k = 0; k * spsInteger < n; ++k)
        {
            const std::size_t i = k * spsInteger;
            std::complex<double> y;
            if (spec.matchedOutput)
            {
                y = {out.matchedI[i], out.matchedQ[i]};
            }
            else
            {
                double yi = 0.0, yq = 0.0;
                const std::size_t first = i + delay >= n ? i + delay - (n - 1) : 0; // taps beyond the last sample
                for (std::size_t t = first; t < h.size() && t <= i + delay; ++t)
                {
                    yi += h[t] * out.basebandI[i + delay - t];
                    yq += h[t] * out.basebandQ[i + delay - t];
                }
                y = {yi * scale, yq * scale};
            }
            out.decisionPoints.push_back(y);
            out.decisionTimes.push_back(static_cast<double>(i));
        }
    }
    else
    {
        out.decisionPoints.assign(symbols, {0.0, 0.0});
        std::vector<std::complex<double>> mark(symbols), space(symbols);
        std::vector<std::size_t> counts(symbols, 0);
        Rotator deviation(spec.fskDeviationHz / fs);
        for (std::size_t i = 0; i < n; ++i)
        {
            const auto k = static_cast<std::size_t>(std::floor(static_cast<double>(i) * rs / fs));
            if (k >= symbols) break;
            // Correlation receiver on the raw mixer output: integrating over the symbol is the matched filter
            // of the passband pulse itself, image included.
            const std::complex<double> v(ri[i], rq[i]);
            ++counts[k];
            if (spec.type == Modulation::Fsk)
            {
                const std::complex<double> tone = std::conj(deviation.next());
                mark[k] += turned(v, tone);
                space[k] += turned(v, std::conj(tone));
            }
            else
            {
                out.decisionPoints[k] += v;
            }
        }
        for (std::size_t k = 0; k < symbols; ++k)
        {
            const double c = static_cast<double>(std::max<std::size_t>(1, counts[k]));
            if (spec.type == Modulation::Fsk)
                out.decisionPoints[k] = {std::abs(mark[k]) / (c * a), std::abs(space[k]) / (c * a)};
            else
                out.decisionPoints[k] /= c * a;
        }
        if (spec.type != Modulation::Fsk && spec.matchedOutput)
        {
            // For the eye: the mean over one symbol of the baseband (free of the image), whose value at the last
            // sample of a symbol is that symbol integrated.
            const auto window = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(sps)));
            out.matchedI.assign(n, 0.0);
            out.matchedQ.assign(n, 0.0);
            double sumI = 0.0, sumQ = 0.0;
            for (std::size_t i = 0; i < n; ++i)
            {
                sumI += out.basebandI[i];
                sumQ += out.basebandQ[i];
                if (i >= window)
                {
                    sumI -= out.basebandI[i - window];
                    sumQ -= out.basebandQ[i - window];
                }
                const double count = static_cast<double>(std::min(i + 1, window));
                out.matchedI[i] = sumI / (count * a);
                out.matchedQ[i] = sumQ / (count * a);
            }
            for (std::size_t k = 0; k < symbols; ++k)
                out.decisionTimes.push_back(std::ceil(static_cast<double>(k + 1) * sps - 1e-9) - 1.0);
        }
    }

    const int bps = std::max(1, bitsPerSymbol(spec.type));
    out.symbols.reserve(out.decisionPoints.size());
    out.bits.reserve(out.decisionPoints.size() * static_cast<std::size_t>(bps));
    for (const auto &p : out.decisionPoints)
    {
        const std::size_t symbol = spec.type == Modulation::Fsk ? (p.real() > p.imag() ? 1u : 0u)
                                                                 : decideSymbol(spec.type, p, spec.askLowLevel);
        out.symbols.push_back(symbol);
        for (int b = bps - 1; b >= 0; --b) out.bits.push_back(static_cast<std::uint8_t>((symbol >> b) & 1u));
    }
    return out;
}

double receiverBandEdgeHz(const ReceiverSpec &spec) noexcept
{
    const double rs = spec.symbolRate;
    if (spec.type == Modulation::Fsk) return std::abs(spec.fskDeviationHz) + rs;
    if (spec.pulse == PulseShape::RootRaisedCosine) return 0.5 * (1.0 + spec.rolloff) * rs;
    return rs;
}

Baseband toBaseband(const ReceivedInput &input, const ReceiverSpec &spec)
{
    Baseband out;
    if (!(spec.sampleRateHz > 0.0))
    {
        out.error = "La fréquence d'échantillonnage doit être positive.";
        return out;
    }
    if (!mixDown(input, spec, out.inPhase, out.quadrature, out.error)) return out;
    removeImage(spec, receiverBandEdgeHz(spec), out.inPhase, out.quadrature, out.warnings);
    return out;
}

AnalogReception receiveAnalog(const ReceivedInput &input, const ReceiverSpec &spec)
{
    AnalogReception out;
    const double fs = spec.sampleRateHz;
    if (!isAnalog(spec.type))
    {
        out.error = "Modulation numérique : utiliser receiveDigital.";
        return out;
    }
    if (!(fs > 0.0) || !(spec.messageBandwidthHz > 0.0))
    {
        out.error = "Il faut fs > 0 et une bande de message positive.";
        return out;
    }
    const double bandEdge = spec.type == Modulation::Fm ? spec.frequencyDeviationHz + spec.messageBandwidthHz
                            : spec.type == Modulation::Pm ? (spec.phaseDeviationRad + 1.0) * spec.messageBandwidthHz
                                                          : spec.messageBandwidthHz;
    std::vector<double> bi, bq;
    if (!mixDown(input, spec, bi, bq, out.error)) return out;
    removeImage(spec, bandEdge, bi, bq, out.warnings);
    if (spec.type == Modulation::Pm && spec.phaseDeviationRad > kPi)
        out.warnings.emplace_back("Excursion de phase supérieure à pi : la phase est déroulée à 2 pi près.");
    const std::size_t n = bi.size();
    const double a = spec.carrierAmplitude != 0.0 ? spec.carrierAmplitude : 1.0;
    out.message.resize(n);
    switch (spec.type)
    {
        case Modulation::Am:
            if (!(spec.amIndex > 0.0))
            {
                out.error = "L'indice de modulation AM doit être positif.";
                return out;
            }
            for (std::size_t i = 0; i < n; ++i) out.message[i] = (std::hypot(bi[i], bq[i]) / a - 1.0) / spec.amIndex;
            break;
        case Modulation::Fm:
            if (!(spec.frequencyDeviationHz > 0.0))
            {
                out.error = "L'excursion de fréquence doit être positive.";
                return out;
            }
            for (std::size_t i = 1; i < n; ++i)
            {
                const std::complex<double> now(bi[i], bq[i]), before(bi[i - 1], bq[i - 1]);
                // The sample at i - 1 -> i carries the frequency of sample i - 1 (the modulator advances after).
                out.message[i - 1] = std::arg(now * std::conj(before)) * fs / (kTwoPi * spec.frequencyDeviationHz);
            }
            if (n >= 2) out.message[n - 1] = out.message[n - 2];
            break;
        default:
        {
            if (!(spec.phaseDeviationRad > 0.0))
            {
                out.error = "L'excursion de phase doit être positive.";
                return out;
            }
            double previous = 0.0, correction = 0.0;
            for (std::size_t i = 0; i < n; ++i)
            {
                const double raw = std::atan2(bq[i], bi[i]);
                if (i > 0)
                {
                    const double delta = raw - previous;
                    if (delta > kPi) correction -= kTwoPi;
                    else if (delta < -kPi) correction += kTwoPi;
                }
                previous = raw;
                out.message[i] = (raw + correction) / spec.phaseDeviationRad;
            }
            break;
        }
    }
    smooth(out.message, spec.messageBandwidthHz, fs);
    return out;
}
} // namespace qacq::dsp
