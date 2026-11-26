#include "qacq/dsp/equalizer.h"

#include "qacq/dsp/constants.h"
#include "qacq/dsp/synchronization.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace qacq::dsp
{
namespace
{
using Complex = std::complex<double>;

// a x = b for a square complex system, by elimination with partial pivoting; false when a is singular (a pivot
// under `tolerance` times its largest term).
bool solve(std::vector<Complex> &a, std::vector<Complex> &b, std::size_t n, double tolerance = 1e-14)
{
    double scale = 0.0;
    for (const Complex &v : a) scale = std::max(scale, std::abs(v));
    if (!(scale > 0.0)) return false;
    for (std::size_t col = 0; col < n; ++col)
    {
        std::size_t pivot = col;
        for (std::size_t row = col + 1; row < n; ++row)
            if (std::abs(a[row * n + col]) > std::abs(a[pivot * n + col])) pivot = row;
        if (!(std::abs(a[pivot * n + col]) > tolerance * scale)) return false;
        if (pivot != col)
        {
            for (std::size_t k = 0; k < n; ++k) std::swap(a[col * n + k], a[pivot * n + k]);
            std::swap(b[col], b[pivot]);
        }
        const Complex inverse = 1.0 / a[col * n + col];
        for (std::size_t row = col + 1; row < n; ++row)
        {
            const Complex factor = a[row * n + col] * inverse;
            if (factor == Complex{}) continue;
            for (std::size_t k = col; k < n; ++k) a[row * n + k] -= factor * a[col * n + k];
            b[row] -= factor * b[col];
        }
    }
    for (std::size_t col = n; col-- > 0;)
    {
        Complex v = b[col];
        for (std::size_t k = col + 1; k < n; ++k) v -= a[col * n + k] * b[k];
        b[col] = v / a[col * n + col];
    }
    return true;
}

// What is left to a decision after taps w on a response c (q = w * c scaled to q[0] = 1, w scaled with it); with
// `feedback` taps on the decisions, q[1 ... feedback] taken off by them (b[j] = q[j]).
void account(LinearEqualizer &e, const SymbolTaps &response, double noiseVariance, const SymbolMoments &moments, int feedback = 0)
{
    e.combined = convolveTaps(e.taps, response);
    const Complex centre = e.combined.at(0);
    if (!(std::abs(centre) > 1e-12))
    {
        e.error = "La réponse du canal est nulle à l'instant de décision : rien à égaliser.";
        return;
    }
    for (Complex &v : e.taps.values) v /= centre;
    for (Complex &v : e.combined.values) v /= centre;
    e.feedback.clear();
    for (int m = 1; m <= feedback; ++m) e.feedback.push_back(e.combined.at(m));
    const double variance = std::max(0.0, moments.energy - std::norm(moments.mean));
    double others = 0.0, through = 0.0;
    Complex sum{};
    for (int m = e.combined.first; m <= e.combined.last(); ++m)
    {
        if (m == 0 || (m >= 1 && m <= feedback)) continue;
        // Real symbols decided on the real axis: only what the others leave there disturbs, and it all does.
        const Complex q = moments.realAxis ? Complex{e.combined.at(m).real(), 0.0} : e.combined.at(m);
        others += std::norm(q);
        sum += q;
    }
    for (const Complex &v : e.taps.values) through += std::norm(v);
    const double energy = moments.energy > 0.0 ? moments.energy : 1.0;
    e.interference = (moments.realAxis ? 2.0 : 1.0) * (variance * others + std::norm(moments.mean) * std::norm(sum)) / energy;
    e.noise = std::max(0.0, noiseVariance) * through / energy;
    const double left = e.interference + e.noise;
    e.sinr = left > 0.0 ? 1.0 / left : std::numeric_limits<double>::infinity();
}
} // namespace

SymbolMoments symbolMoments(std::span<const std::complex<double>> constellation) noexcept
{
    SymbolMoments m;
    if (constellation.empty()) return m;
    double energy = 0.0;
    Complex mean{};
    for (const Complex &p : constellation)
    {
        energy += std::norm(p);
        mean += p;
    }
    m.energy = energy / static_cast<double>(constellation.size());
    m.mean = mean / static_cast<double>(constellation.size());
    m.realAxis = true;
    for (const Complex &p : constellation)
        if (std::abs(p.imag()) > 1e-12 * std::sqrt(std::max(m.energy, 1e-300))) m.realAxis = false;
    return m;
}

SymbolTaps fitSymbolResponse(std::span<const std::complex<double>> points, std::span<const std::complex<double>> symbols,
                             int first, int last, std::size_t from, std::size_t to)
{
    SymbolTaps out;
    if (last < first) return out;
    const auto count = static_cast<std::size_t>(last - first + 1);
    to = std::min(to, points.size());
    if (to <= from || to - from < 2 * count) return out;
    const auto symbol = [&](std::size_t k, int j) -> Complex {
        const auto at = static_cast<std::ptrdiff_t>(k) - j;
        return at < 0 || at >= static_cast<std::ptrdiff_t>(symbols.size()) ? Complex{} : symbols[static_cast<std::size_t>(at)];
    };
    // Normal equations: G c = r, G[a][b] = sum_k conj(s[k - ja]) s[k - jb], r[a] = sum_k conj(s[k - ja]) x[k].
    std::vector<Complex> g(count * count), r(count);
    std::vector<Complex> row(count);
    for (std::size_t k = from; k < to; ++k)
    {
        for (std::size_t a = 0; a < count; ++a) row[a] = symbol(k, first + static_cast<int>(a));
        for (std::size_t a = 0; a < count; ++a)
        {
            const Complex ca = std::conj(row[a]);
            if (ca == Complex{}) continue;
            r[a] += ca * points[k];
            for (std::size_t b = 0; b < count; ++b) g[a * count + b] += ca * row[b];
        }
    }
    // Symbols that repeat (a short pattern) leave the equations without a single answer: none given.
    if (!solve(g, r, count, 1e-9)) return out;
    for (const Complex &v : r)
        if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) return out;
    out.first = first;
    out.values = std::move(r);
    return out;
}

SymbolTaps convolveTaps(const SymbolTaps &a, const SymbolTaps &b)
{
    SymbolTaps out;
    if (a.values.empty() || b.values.empty()) return out;
    out.first = a.first + b.first;
    out.values.assign(a.values.size() + b.values.size() - 1, Complex{});
    for (std::size_t i = 0; i < a.values.size(); ++i)
        for (std::size_t j = 0; j < b.values.size(); ++j) out.values[i + j] += a.values[i] * b.values[j];
    return out;
}

std::complex<double> tapsResponse(const SymbolTaps &taps, double cyclesPerSymbol) noexcept
{
    Complex sum{};
    for (std::size_t m = 0; m < taps.values.size(); ++m)
    {
        const double cycles = cyclesPerSymbol * static_cast<double>(taps.first + static_cast<int>(m));
        sum += taps.values[m] * std::polar(1.0, -kTwoPi * (cycles - std::floor(cycles)));
    }
    return sum;
}

LinearEqualizer withoutEqualizer(const SymbolTaps &response, double noiseVariance, const SymbolMoments &moments)
{
    LinearEqualizer e;
    if (response.values.empty())
    {
        e.error = "Réponse du canal inconnue.";
        return e;
    }
    e.taps.first = 0;
    e.taps.values = {Complex{1.0, 0.0}};
    account(e, response, noiseVariance, moments);
    return e;
}

LinearEqualizer designLinearEqualizer(const SymbolTaps &response, int half, double noiseVariance, const SymbolMoments &moments)
{
    LinearEqualizer e;
    if (response.values.empty())
    {
        e.error = "Réponse du canal inconnue.";
        return e;
    }
    if (half < 0 || half > kMaximumEqualizerHalf)
    {
        e.error = "Un égaliseur a de 1 à " + std::to_string(2 * kMaximumEqualizerHalf + 1) + " prises.";
        return e;
    }
    const auto n = static_cast<std::size_t>(2 * half + 1);
    const double variance = std::max(0.0, moments.energy - std::norm(moments.mean));
    const double meanPower = std::norm(moments.mean);
    // rho[d] = sum_j c[j + d] conj(c[j]); the sum of the response for the mean of the symbols.
    const auto rho = [&](int d) {
        Complex sum{};
        for (int j = response.first; j <= response.last(); ++j) sum += response.at(j + d) * std::conj(response.at(j));
        return sum;
    };
    Complex total{};
    for (const Complex &v : response.values) total += v;
    std::vector<Complex> lag(2 * n - 1);
    for (std::size_t d = 0; d < lag.size(); ++d) lag[d] = rho(static_cast<int>(d) - static_cast<int>(n) + 1);
    // Without noise the system may have no single answer (a null of the response, more taps than it takes): a
    // trace of noise picks the smallest taps.
    const double floor = 1e-10 * std::max(1e-300, variance * std::abs(lag[n - 1]) + meanPower * std::norm(total));
    const double sigma = std::max(std::max(0.0, noiseVariance), floor);
    std::vector<Complex> r(n * n), p(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        for (std::size_t l = 0; l < n; ++l)
            r[i * n + l] = variance * lag[i + n - 1 - l] + meanPower * std::norm(total) + (i == l ? sigma : 0.0);
        const int index = static_cast<int>(i) - half;
        p[i] = variance * std::conj(response.at(-index)) + meanPower * std::conj(total);
    }
    if (!solve(r, p, n))
    {
        e.error = "Égaliseur impossible à calculer pour cette réponse.";
        return e;
    }
    e.taps.first = -half;
    e.taps.values = std::move(p);
    account(e, response, noiseVariance, moments);
    return e;
}

LinearEqualizer designDecisionFeedbackEqualizer(const SymbolTaps &response, int half, int feedback, double noiseVariance,
                                                const SymbolMoments &moments)
{
    LinearEqualizer e;
    if (response.values.empty())
    {
        e.error = "Réponse du canal inconnue.";
        return e;
    }
    if (half < 0 || half > kMaximumEqualizerHalf)
    {
        e.error = "Un égaliseur a de 1 à " + std::to_string(2 * kMaximumEqualizerHalf + 1) + " prises.";
        return e;
    }
    if (feedback < 1 || feedback > kMaximumFeedbackTaps)
    {
        e.error = "Un égaliseur à retour de décision a de 1 à " + std::to_string(kMaximumFeedbackTaps) + " prises de retour.";
        return e;
    }
    const auto n = static_cast<std::size_t>(2 * half + 1);
    const double variance = std::max(0.0, moments.energy - std::norm(moments.mean));
    const double meanPower = std::norm(moments.mean);
    Complex total{};
    for (const Complex &v : response.values) total += v;
    // For taps i and l (from -half): what the symbols outside the feedback make of them, sum over m not in
    // 1..feedback of c[m - i] conj(c[m - l]); and the sum of c[m - i] over those m (the mean of the symbols).
    const auto kept = [&](int i, int l) {
        Complex sum{};
        for (int j = response.first; j <= response.last(); ++j)
        {
            const int m = j + i; // c[m - i] = c[j]
            if (m >= 1 && m <= feedback) continue;
            sum += response.at(j) * std::conj(response.at(m - l));
        }
        return sum;
    };
    std::vector<Complex> outside(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        Complex sum = total;
        for (int m = 1; m <= feedback; ++m) sum -= response.at(m - (static_cast<int>(i) - half));
        outside[i] = sum;
    }
    double scale = 0.0;
    for (const Complex &v : response.values) scale += std::norm(v);
    const double floor = 1e-10 * std::max(1e-300, variance * scale + meanPower * std::norm(total));
    const double sigma = std::max(std::max(0.0, noiseVariance), floor);
    // The equation of tap l: sum_i a[l][i] w[i] = p[l].
    std::vector<Complex> a(n * n), p(n);
    for (std::size_t l = 0; l < n; ++l)
    {
        const int tl = static_cast<int>(l) - half;
        for (std::size_t i = 0; i < n; ++i)
        {
            const int ti = static_cast<int>(i) - half;
            a[l * n + i] = variance * kept(ti, tl) + meanPower * outside[i] * std::conj(outside[l]) + (i == l ? sigma : 0.0);
        }
        p[l] = variance * std::conj(response.at(-tl)) + meanPower * std::conj(outside[l]);
    }
    if (!solve(a, p, n))
    {
        e.error = "Égaliseur impossible à calculer pour cette réponse.";
        return e;
    }
    e.taps.first = -half;
    e.taps.values = std::move(p);
    account(e, response, noiseVariance, moments, feedback);
    return e;
}

std::vector<std::complex<double>> applyDecisionFeedback(std::span<const std::complex<double>> points, const SymbolTaps &taps,
                                                        std::span<const std::complex<double>> feedback, Modulation modulation,
                                                        double askLowLevel)
{
    const std::vector<Complex> ideal = constellation(modulation, askLowLevel);
    std::vector<Complex> out(points.size()), decided(points.size());
    const auto n = static_cast<std::ptrdiff_t>(points.size());
    for (std::ptrdiff_t k = 0; k < n; ++k)
    {
        Complex z{};
        for (std::size_t m = 0; m < taps.values.size(); ++m)
        {
            const std::ptrdiff_t at = k - (taps.first + static_cast<std::ptrdiff_t>(m));
            if (at >= 0 && at < n) z += taps.values[m] * points[static_cast<std::size_t>(at)];
        }
        for (std::size_t j = 1; j <= feedback.size(); ++j)
        {
            const std::ptrdiff_t at = k - static_cast<std::ptrdiff_t>(j);
            if (at >= 0) z -= feedback[j - 1] * decided[static_cast<std::size_t>(at)];
        }
        const auto index = static_cast<std::size_t>(k);
        out[index] = z;
        decided[index] = ideal.empty() ? z : ideal[decideSymbol(modulation, z, askLowLevel)];
    }
    return out;
}

std::vector<std::complex<double>> applyEqualizer(std::span<const std::complex<double>> points, const SymbolTaps &taps)
{
    std::vector<Complex> out(points.size());
    const auto n = static_cast<std::ptrdiff_t>(points.size());
    for (std::ptrdiff_t k = 0; k < n; ++k)
    {
        Complex z{};
        for (std::size_t m = 0; m < taps.values.size(); ++m)
        {
            const std::ptrdiff_t at = k - (taps.first + static_cast<std::ptrdiff_t>(m));
            if (at >= 0 && at < n) z += taps.values[m] * points[static_cast<std::size_t>(at)];
        }
        out[static_cast<std::size_t>(k)] = z;
    }
    return out;
}

AdaptiveEqualization adaptEqualizer(std::span<const std::complex<double>> given, std::span<const std::complex<double>> training,
                                    Modulation modulation, double askLowLevel, const AdaptiveEqualizerSettings &settings,
                                    std::span<const std::complex<double>> earlier)
{
    AdaptiveEqualization out;
    if (!hasConstellation(modulation))
    {
        out.error = "Pas d'égaliseur pour cette modulation : elle n'a pas de constellation.";
        return out;
    }
    if (settings.half < 0 || settings.half > kMaximumEqualizerHalf)
    {
        out.error = "Un égaliseur a de 1 à " + std::to_string(2 * kMaximumEqualizerHalf + 1) + " prises.";
        return out;
    }
    if (!(settings.step > 0.0 && settings.step <= 1.0))
    {
        out.error = "Le pas de l'égaliseur adaptatif est compris entre 0 (exclu) et 1.";
        return out;
    }
    if (settings.feedback < 0 || settings.feedback > kMaximumFeedbackTaps)
    {
        out.error = "Un égaliseur à retour de décision a de 1 à " + std::to_string(kMaximumFeedbackTaps) + " prises de retour.";
        return out;
    }
    const std::vector<Complex> ideal = constellation(modulation, askLowLevel);
    const int half = settings.half;
    const auto taps = static_cast<std::size_t>(2 * half + 1);
    // Decision feedback: its taps after the forward ones, in the same unknowns (dims of them).
    const auto back = static_cast<std::size_t>(settings.feedback);
    const std::size_t dims = taps + back;
    std::span<const Complex> points = given;
    const auto n = static_cast<std::ptrdiff_t>(points.size());
    const std::size_t known = std::min(training.size(), points.size());
    // The first point whose pulses are whole, within the first quarter of the preamble (enough of it left to fit the
    // taps on); and the first point learnt from: that one when the preamble shows little noise (the cut points then
    // stand out), else the first (the noise hides them, and every known symbol counts).
    const std::size_t whole = std::min(settings.adaptFrom, known / 4);
    std::size_t from = 0;
    std::vector<Complex> w(taps), window(taps), b(back);
    const auto fill = [&](std::ptrdiff_t k) {
        double power = 0.0;
        for (std::size_t m = 0; m < taps; ++m)
        {
            const std::ptrdiff_t at = k - (static_cast<std::ptrdiff_t>(m) - half);
            window[m] = at >= 0 && at < n ? points[static_cast<std::size_t>(at)] : Complex{};
            power += std::norm(window[m]);
        }
        return power;
    };
    const bool carrier = settings.carrierBandwidth > 0.0;
    // Over the preamble from `from`: the taps that leave the least sum of |d[k] e^(j nu k) - sum_m w[m] x[k - i_m]|^2
    // (a little of their own size added to it, `share` of the energy they see: with few known symbols for many
    // taps, the smallest taps that do); what they leave of it over the whole points, and how much their output
    // still turns from the first half of the preamble to the second.
    // The regressors of point k: the window, then (decision feedback) minus the known symbols before it, turned as the
    // symbol wanted is.
    std::vector<Complex> regressor(dims);
    const auto regress = [&](std::size_t k, Complex turnedBy) {
        const double power = fill(static_cast<std::ptrdiff_t>(k));
        for (std::size_t i = 0; i < taps; ++i) regressor[i] = window[i];
        for (std::size_t j = 1; j <= back; ++j) regressor[taps + j - 1] = k >= j ? -training[k - j] * turnedBy : Complex{};
        return power;
    };
    const auto fit = [&](double nu, double share, std::vector<Complex> &found, double &turning) {
        std::vector<Complex> a(dims * dims), right(dims);
        double total = 0.0;
        for (std::size_t k = from; k < known; ++k)
        {
            const Complex turn = std::polar(1.0, nu * static_cast<double>(k));
            total += regress(k, turn);
            const Complex wanted = training[k] * turn;
            for (std::size_t i = 0; i < dims; ++i)
            {
                const Complex ci = std::conj(regressor[i]);
                if (ci == Complex{}) continue;
                right[i] += ci * wanted;
                for (std::size_t l = 0; l < dims; ++l) a[i * dims + l] += ci * regressor[l];
            }
        }
        const double ridge = share * total / static_cast<double>(taps);
        for (std::size_t i = 0; i < dims; ++i) a[i * dims + i] += ridge;
        if (!(total > 0.0) || !solve(a, right, dims)) return std::numeric_limits<double>::infinity();
        double left = 0.0;
        Complex early{}, late{};
        const std::size_t middle = from + (known - from) / 2;
        for (std::size_t k = from; k < known; ++k)
        {
            const Complex turn = std::polar(1.0, nu * static_cast<double>(k));
            regress(k, turn);
            Complex y{};
            for (std::size_t m = 0; m < dims; ++m) y += right[m] * regressor[m];
            const Complex wanted = training[k] * turn;
            if (k >= whole) left += std::norm(wanted - y);
            (k < middle ? early : late) += y * std::conj(wanted);
        }
        turning = std::abs(early) > 0.0 && std::abs(late) > 0.0 ? std::arg(late * std::conj(early)) / (0.5 * static_cast<double>(known - from)) : 0.0;
        found = std::move(right);
        return left;
    };
    // With a carrier loop to follow: the rate the points still turn at over the preamble (a carrier found through
    // several paths is a little off), found with the taps in a few rounds. The ridge: 2 % of the energy, as much as
    // a noise at -17 dB would add. When the preamble shows less noise than that (what the taps leave of it on the
    // whole points, the part their fit took off given back), they are fitted again on the whole points alone, the
    // ridge that noise: 2 % would hold them 2 % short, an error at -34 dB that a 256-QAM at 33 dB pays for over
    // hundreds of symbols, as for the first point, half its size. `start`: the first point they learnt from.
    const double energy = [&] {
        double e = 0.0;
        for (std::size_t k = whole; k < known; ++k) e += std::norm(training[k]);
        return e;
    }();
    const auto learn = [&](std::vector<Complex> &found, double &nu, std::size_t &start) {
        const auto rounds = [&](double share) {
            nu = 0.0;
            double left = std::numeric_limits<double>::infinity();
            for (int round = 0; round < (carrier && known >= 16 ? 4 : 1); ++round)
            {
                double turning = 0.0;
                left = fit(nu, share, found, turning);
                if (!std::isfinite(left)) break;
                if (round < 3) nu += turning;
            }
            return left;
        };
        constexpr double kRidge = 0.02, kLeastRidge = 1e-4;
        from = start = 0;
        const double left = rounds(kRidge);
        const double count = static_cast<double>(known - whole);
        if (!std::isfinite(left) || !(energy > 0.0) || count <= static_cast<double>(2 * taps)) return left;
        const double noise = left / energy / (1.0 - static_cast<double>(taps) / count);
        if (!(noise < kRidge)) return left;
        from = start = whole;
        return rounds(std::max(noise, kLeastRidge));
    };
    bool trained = false;
    double rate = 0.0;
    std::size_t start = 0; // the first point the taps learn from
    // (Forward taps then feedback ones, as the fit found them.)
    const auto keep = [&](const std::vector<Complex> &found) {
        w.assign(found.begin(), found.begin() + static_cast<std::ptrdiff_t>(taps));
        b.assign(found.begin() + static_cast<std::ptrdiff_t>(taps), found.end());
    };
    if (known >= whole + 2)
    {
        std::vector<Complex> onTime, before;
        double nuOnTime = 0.0, nuBefore = 0.0;
        std::size_t startOnTime = 0, startBefore = 0;
        const double left = learn(onTime, nuOnTime, startOnTime);
        trained = std::isfinite(left);
        if (trained)
        {
            keep(onTime);
            rate = nuOnTime;
            start = startOnTime;
        }
        // The same half a symbol earlier: kept when it explains the preamble clearly better.
        if (earlier.size() == given.size())
        {
            points = earlier;
            const double other = learn(before, nuBefore, startBefore);
            if (std::isfinite(other) && (!trained || other < 0.7 * left))
            {
                keep(before);
                rate = nuBefore;
                start = startBefore;
                trained = true;
                out.halfSymbolEarly = true;
            }
            else
                points = given;
        }
    }
    if (!trained)
    {
        w.assign(taps, Complex{});
        w[static_cast<std::size_t>(half)] = 1.0;
        b.assign(back, Complex{});
        rate = 0.0;
    }
    // The tap kept real while the carrier loop runs: the strongest.
    std::size_t reference = static_cast<std::size_t>(half);
    for (std::size_t m = 0; m < taps; ++m)
        if (std::abs(w[m]) > std::abs(w[reference])) reference = m;
    out.points.resize(points.size());
    out.squaredError.resize(points.size());
    double proportional = 0.0, integral = 0.0, phase = 0.0;
    if (!carrier) rate = 0.0;
    if (carrier)
    {
        loopGains(std::min(settings.carrierBandwidth, 0.2), proportional, integral);
        out.phases.resize(points.size());
    }
    const double meanEnergy = std::max(1e-300, symbolMoments(ideal).energy);
    const std::ptrdiff_t learning = settings.adaptSymbols > 0 ? static_cast<std::ptrdiff_t>(std::min(settings.adaptSymbols, points.size())) : n;
    // The symbols decided so far (the preamble's known ones over it), for the feedback.
    std::vector<Complex> decided(back > 0 ? points.size() : 0);
    // Symbol after symbol: the output, then the taps (and the loop) moved towards what it should have been.
    for (std::ptrdiff_t k = 0; k < n; ++k)
    {
        double power = fill(k) + 1e-9 * static_cast<double>(dims);
        Complex y{};
        for (std::size_t m = 0; m < taps; ++m) y += w[m] * window[m];
        const Complex turn = carrier ? std::polar(1.0, phase) : Complex{1.0, 0.0};
        Complex z = y * std::conj(turn);
        const auto index = static_cast<std::size_t>(k);
        for (std::size_t j = 1; j <= back && j <= index; ++j)
        {
            z -= b[j - 1] * decided[index - j];
            power += std::norm(decided[index - j]);
        }
        const Complex target = index < known ? training[index] : ideal[decideSymbol(modulation, z, askLowLevel)];
        if (back > 0) decided[index] = target;
        const Complex error = target - z;
        out.points[index] = z;
        out.squaredError[index] = std::norm(error);
        if (carrier) out.phases[index] = phase;
        if (k >= learning || index < start)
        {
            // Nothing to learn from (yet, or any more): the loop turns on at its rate, the taps stay.
            phase += rate;
            continue;
        }
        // (The taps see the error as it is before the loop turns their output; the feedback works after it.)
        const Complex gain = settings.step * error * turn / power;
        for (std::size_t m = 0; m < taps; ++m) w[m] += gain * std::conj(window[m]);
        for (std::size_t j = 1; j <= back && j <= index; ++j) b[j - 1] -= settings.step * error * std::conj(decided[index - j]) / power;
        if (carrier)
        {
            const double e = (z * std::conj(target)).imag() / meanEnergy;
            phase += proportional * e + rate;
            rate += integral * e;
            // What the taps turned by goes to the loop: the same output, the carrier in one place.
            const double turned = std::arg(w[reference]);
            if (turned != 0.0 && std::abs(w[reference]) > 0.0)
            {
                const Complex undo = std::polar(1.0, -turned);
                for (std::size_t m = 0; m < taps; ++m) w[m] *= undo;
                phase -= turned;
            }
        }
    }
    out.taps.first = -half;
    out.taps.values = std::move(w);
    out.feedback = std::move(b);
    return out;
}
} // namespace qacq::dsp
