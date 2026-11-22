// FIR design by the Parks-McClellan algorithm, as the Fortran program of McClellan, Parks and Rabiner runs it
// ("A computer program for designing optimum FIR linear phase digital filters", IEEE Trans. Audio
// Electroacoustics AU-21, 1973), the program scipy.signal.remez runs after Egil Kvaleberg's C translation;
// symmetric filters only (type "bandpass"). The dense grid, the initial guess equally spaced along it; the
// exchange: the levelled error δ through the extremal frequencies, then each of them moved towards the
// largest error near it, a larger one looked for below the first and above the last, until none moves;
// then the cosine series by an inverse DFT of the response, and the taps.
//
// The search is local, as in the program: noise in the error far from the extremal frequencies does not stop
// it. Where δ stops growing (rounding), the program stops ("the impulse response may be correct"; SciPy
// raises). That happens to a long filter whose equally spaced initial guess levels δ at the rounding (1e-16)
// though its best error is far above it: then the exchange starts again from the extremal frequencies of a
// filter half as long, spread band by band over more points ("reference scaling", S.-I. Filip, ACM TOMS 43,
// 2016), which level a δ close to the best from the first iteration. Wherever SciPy converges, nothing
// changes. Still stopped (rounding, or the last iteration allowed): the taps are given all the same, and it
// is said (RemezDesign::stopped): measure the response.
//
// The arrays are numbered from 1 and the labels of the listing kept (L100 to L370), so that the exchange
// can be checked line by line against it. Checked against scipy.signal.remez (tests/test_filters.cpp).
#include "qacq/dsp/filter_design.h"

#include "qacq/dsp/constants.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace qacq::dsp
{
namespace
{
// An array numbered from 1, as in the listing (element 0 unused).
template <typename T> class One
{
public:
    explicit One(int last = 0) : m_values(static_cast<std::size_t>(std::max(0, last)) + 1, T{}) {}
    T &operator[](int i) { return m_values[static_cast<std::size_t>(i)]; }
    const T &operator[](int i) const { return m_values[static_cast<std::size_t>(i)]; }

private:
    std::vector<T> m_values;
};

struct Exchange
{
    int nfcns = 0; // cosine functions: r + 1 = nfcns + 1 extremal frequencies
    int ngrid = 0;
    One<double> grid, des, wt; // [1..ngrid]
    One<int> iext;             // [1..nfcns + 2]
    One<double> ad, x, y;      // [1..nfcns + 2]
    One<double> alpha;         // [1..nfcns + 2]: the cosine series of the result
    double dev = 0.0;
    int niter = 0;
};

// Function D of the program: the coefficients of the Lagrange interpolation in barycentric form; the
// products in strides of m, so that they stay within range.
double lagrange(int k, int n, int m, const One<double> &x)
{
    double product = 1.0;
    const double q = x[k];
    for (int l = 1; l <= m; ++l)
        for (int j = l; j <= n; j += m)
            if (j != k) product *= 2.0 * (q - x[j]);
    return 1.0 / product;
}

// Function GEE: the response at a frequency (fraction of fs), through the n extremal points.
double gee(const Exchange &e, double frequency, int n)
{
    double p = 0.0, d = 0.0;
    const double xf = std::cos(kTwoPi * frequency);
    for (int j = 1; j <= n; ++j)
    {
        const double c = e.ad[j] / (xf - e.x[j]);
        d += c;
        p += c * e.y[j];
    }
    return p / d;
}

enum class Ending
{
    Converged,  // no extremal frequency moved
    Iterations, // itrmax iterations
    Rounding    // δ did not grow
};

// Subroutine REMEZ, the exchange (labels 100 to 370). The search never evaluates the error at one of the
// extremal frequencies it started from (where the barycentric formula divides by zero): each one is looked
// for strictly between its neighbours.
Ending remez(Exchange &e, int itrmax)
{
    const int nfcns = e.nfcns, ngrid = e.ngrid;
    const int nz = nfcns + 1, nzz = nfcns + 2;
    One<int> &iext = e.iext;
    double devl = -1.0, comp = 0.0, ynz = 0.0, y1 = 0.0, err = 0.0;
    int j = 1, k = 1, l = 1, nu = 1, nut = 1, nut1 = 1, jchnge = 0, k1 = 1, knz = 1, klow = 0, kup = 1, luck = 0;
    const auto error = [&e, nz](int at) { return (gee(e, e.grid[at], nz) - e.des[at]) * e.wt[at]; };
    e.niter = 0;

L100:
    iext[nzz] = ngrid + 1;
    ++e.niter;
    if (e.niter > itrmax)
    {
        e.niter = itrmax;
        return Ending::Iterations;
    }
    for (j = 1; j <= nz; ++j) e.x[j] = std::cos(kTwoPi * e.grid[iext[j]]);
    {
        const int jet = (nfcns - 1) / 15 + 1;
        for (j = 1; j <= nz; ++j) e.ad[j] = lagrange(j, nz, jet, e.x);
    }
    {
        double dnum = 0.0, dden = 0.0;
        k = 1;
        for (j = 1; j <= nz; ++j)
        {
            l = iext[j];
            dnum += e.ad[j] * e.des[l];
            dden += k * e.ad[j] / e.wt[l];
            k = -k;
        }
        e.dev = dnum / dden;
    }
    nu = 1;
    if (e.dev > 0.0) nu = -1;
    e.dev = -nu * e.dev;
    k = nu;
    for (j = 1; j <= nz; ++j)
    {
        l = iext[j];
        e.y[j] = e.des[l] + k * e.dev / e.wt[l];
        k = -k;
    }
    if (!(e.dev > devl)) return Ending::Rounding;
    devl = e.dev;
    jchnge = 0;
    k1 = iext[1];
    knz = iext[nz];
    klow = 0;
    nut = -nu;
    j = 1;

    // The search for the extremal frequencies of the best approximation.
L200:
    if (j == nzz) ynz = comp;
    if (j >= nzz) goto L300;
    kup = iext[j + 1];
    l = iext[j] + 1;
    nut = -nut;
    if (j == 2) y1 = comp;
    comp = e.dev;
    if (l >= kup) goto L220;
    err = error(l);
    if (nut * err - comp <= 0.0) goto L220;
    comp = nut * err;
L210:
    if (++l >= kup) goto L215;
    err = error(l);
    if (nut * err - comp <= 0.0) goto L215;
    comp = nut * err;
    goto L210;
L215:
    iext[j++] = l - 1;
    klow = l - 1;
    ++jchnge;
    goto L200;
L220:
    --l;
L225:
    if (--l <= klow) goto L250;
    err = error(l);
    if (nut * err - comp > 0.0) goto L230;
    if (jchnge <= 0) goto L225;
    goto L260;
L230:
    comp = nut * err;
L235:
    if (--l <= klow) goto L240;
    err = error(l);
    if (nut * err - comp <= 0.0) goto L240;
    comp = nut * err;
    goto L235;
L240:
    klow = iext[j];
    iext[j] = l + 1;
    ++j;
    ++jchnge;
    goto L200;
L250:
    l = iext[j] + 1;
    if (jchnge > 0) goto L215;
L255:
    if (++l >= kup) goto L260;
    err = error(l);
    if (nut * err - comp <= 0.0) goto L255;
    comp = nut * err;
    goto L210;
L260:
    klow = iext[j++];
    goto L200;

    // A larger error below the first extremal frequency, or above the last.
L300:
    if (j > nzz) goto L320;
    if (k1 > iext[1]) k1 = iext[1];
    if (knz < iext[nz]) knz = iext[nz];
    nut1 = nut;
    nut = -nu;
    l = 0;
    kup = k1;
    comp = ynz * 1.00001;
    luck = 1;
L310:
    if (++l >= kup) goto L315;
    err = error(l);
    if (nut * err - comp <= 0.0) goto L310;
    comp = nut * err;
    j = nzz;
    goto L210;
L315:
    luck = 6;
    goto L325;
L320:
    if (luck > 9) goto L350;
    if (comp > y1) y1 = comp;
    k1 = iext[nzz];
L325:
    l = ngrid + 1;
    klow = knz;
    nut = -nut1;
    comp = y1 * 1.00001;
L330:
    if (--l <= klow) goto L340;
    err = error(l);
    if (nut * err - comp <= 0.0) goto L330;
    j = nzz;
    comp = nut * err;
    luck += 10;
    goto L235;
L340:
    if (luck == 6) goto L370;
    for (j = 1; j <= nfcns; ++j) iext[nzz - j] = iext[nz - j];
    iext[1] = k1;
    goto L100;
L350:
    {
        const int kn = iext[nzz];
        for (j = 1; j <= nfcns; ++j) iext[j] = iext[j + 1];
        iext[nz] = kn;
    }
    goto L100;
L370:
    if (jchnge > 0) goto L100;
    return Ending::Converged;
}

// Label 400 of REMEZ: the coefficients of the best approximation by the inverse DFT of its response at
// nfcns equally spaced points. When the grid does not span 0 to fs/2, the points are taken within it, on a
// variable mapped there, and the series brought back (aa, bb): the response outside the bands, where
// nothing holds it, is not sampled.
void coefficients(Exchange &e)
{
    const int nfcns = e.nfcns, ngrid = e.ngrid;
    const int nz = nfcns + 1, nzz = nfcns + 2, nm1 = nfcns - 1;
    const double fsh = 1.0e-6;
    One<double> a(nzz), p(nzz), q(nzz);
    e.x[nzz] = -2.0;
    const int cn = 2 * nfcns - 1;
    const double delf = 1.0 / cn;
    int l = 1;
    bool kkk = (e.grid[1] < 0.01 && e.grid[ngrid] > 0.49) || nfcns <= 3;
    double aa = 0.0, bb = 0.0;
    if (!kkk)
    {
        const double dtemp = std::cos(kTwoPi * e.grid[1]);
        const double dnum = std::cos(kTwoPi * e.grid[ngrid]);
        aa = 2.0 / (dtemp - dnum);
        bb = -(dtemp + dnum) / (dtemp - dnum);
    }
    for (int j = 1; j <= nfcns; ++j)
    {
        double ft = (j - 1) * delf;
        double xt = std::cos(kTwoPi * ft);
        if (!kkk)
        {
            xt = (xt - bb) / aa;
            ft = std::acos(xt) / kTwoPi;
        }
        for (;;)
        {
            const double xe = e.x[l];
            if (xt > xe)
            {
                if (xt - xe < fsh)
                    a[j] = e.y[l];
                else
                    a[j] = gee(e, ft, nz);
                break;
            }
            if (xe - xt < fsh)
            {
                a[j] = e.y[l];
                break;
            }
            ++l;
        }
        if (l > 1) --l;
    }
    const double dden = kTwoPi / cn;
    for (int j = 1; j <= nfcns; ++j)
    {
        double dtemp = 0.0;
        const double dnum = (j - 1) * dden;
        for (int k = 1; k <= nm1; ++k) dtemp += a[k + 1] * std::cos(dnum * k);
        e.alpha[j] = 2.0 * dtemp + a[1];
    }
    for (int j = 2; j <= nfcns; ++j) e.alpha[j] = 2.0 * e.alpha[j] / cn;
    e.alpha[1] = e.alpha[1] / cn;
    if (!kkk)
    {
        p[1] = 2.0 * e.alpha[nfcns] * bb + e.alpha[nm1];
        p[2] = 2.0 * aa * e.alpha[nfcns];
        q[1] = e.alpha[nfcns - 2] - e.alpha[nfcns];
        for (int j = 2; j <= nm1; ++j)
        {
            if (j >= nm1)
            {
                aa *= 0.5;
                bb *= 0.5;
            }
            p[j + 1] = 0.0;
            for (int k = 1; k <= j; ++k)
            {
                a[k] = p[k];
                p[k] = 2.0 * bb * a[k];
            }
            p[2] += a[1] * 2.0 * aa;
            for (int k = 1; k <= j - 1; ++k) p[k] += q[k] + aa * a[k + 1];
            for (int k = 3; k <= j + 1; ++k) p[k] += aa * a[k - 1];
            if (j != nm1)
            {
                for (int k = 1; k <= j; ++k) q[k] = -a[k];
                q[1] += e.alpha[nfcns - 1 - j];
            }
        }
        for (int j = 1; j <= nfcns; ++j) e.alpha[j] = p[j];
    }
    if (nfcns <= 3)
    {
        e.alpha[nfcns + 1] = 0.0;
        e.alpha[nfcns + 2] = 0.0;
    }
}
} // namespace

namespace
{
// What is asked, the edges as fractions of fs.
struct Problem
{
    std::vector<double> edges, desired, weights;
    int density = 16;
    int itrmax = 25;
};

struct Solution
{
    Exchange e;
    Ending ending = Ending::Converged;
    std::vector<std::pair<int, int>> bands; // grid indices of each band, first and last (first > last: none)
    bool usable() const noexcept { return ending != Ending::Rounding && std::isfinite(e.dev); }
};

// The dense grid of the main program: from each lower edge in steps of 0.5/(density·nfcns) while within the
// band, its last point moved onto the upper edge. An even number of taps has a zero at fs/2: a last point near
// it is dropped, and the problem made the same for a cosine series (D/cos πf, W·cos πf). Then the initial
// guess, equally spaced along the grid. False: too few grid points.
bool setUp(const Problem &problem, int nfilt, Solution &out)
{
    const bool nodd = nfilt % 2 == 1;
    Exchange &e = out.e;
    e.nfcns = nfilt / 2 + (nodd ? 1 : 0);
    const int nfcns = e.nfcns;
    const double delf = 0.5 / (problem.density * nfcns);
    const std::size_t bands = problem.desired.size();
    std::vector<double> grid, des, wt;
    grid.reserve(static_cast<std::size_t>(problem.density) * static_cast<std::size_t>(nfcns) + 2 * bands + 2);
    out.bands.clear();
    for (std::size_t band = 0; band < bands; ++band)
    {
        const double fup = problem.edges[2 * band + 1];
        double temp = problem.edges[2 * band];
        const int first = static_cast<int>(grid.size()) + 1;
        for (;;)
        {
            grid.push_back(temp);
            des.push_back(problem.desired[band]);
            wt.push_back(problem.weights[band]);
            temp += delf;
            if (temp > fup) break;
        }
        grid.back() = fup;
        out.bands.emplace_back(first, static_cast<int>(grid.size()));
    }
    if (!nodd && grid.back() > 0.5 - delf)
    {
        grid.pop_back();
        des.pop_back();
        wt.pop_back();
        out.bands.back().second -= 1;
    }
    e.ngrid = static_cast<int>(grid.size());
    const int ngrid = e.ngrid;
    if (ngrid < nfcns + 2) return false;
    e.grid = One<double>(ngrid);
    e.des = One<double>(ngrid);
    e.wt = One<double>(ngrid);
    for (int i = 1; i <= ngrid; ++i)
    {
        e.grid[i] = grid[static_cast<std::size_t>(i - 1)];
        e.des[i] = des[static_cast<std::size_t>(i - 1)];
        e.wt[i] = wt[static_cast<std::size_t>(i - 1)];
    }
    if (!nodd)
    {
        for (int i = 1; i <= ngrid; ++i)
        {
            const double change = std::cos(kPi * e.grid[i]);
            e.des[i] /= change;
            e.wt[i] *= change;
        }
    }
    e.iext = One<int>(nfcns + 2);
    e.ad = One<double>(nfcns + 2);
    e.x = One<double>(nfcns + 2);
    e.y = One<double>(nfcns + 2);
    e.alpha = One<double>(nfcns + 2);
    const double temp = static_cast<double>(ngrid - 1) / nfcns;
    for (int j = 1; j <= nfcns; ++j) e.iext[j] = static_cast<int>((j - 1) * temp + 1.0);
    e.iext[nfcns + 1] = ngrid;
    return true;
}

// The extremal frequencies of a shorter design spread over the nfcns + 1 points of this one: in each band as
// many more as it had, at the same places along its points (linear interpolation in their index), each on the
// nearest grid point, all distinct.
void scaleReference(const Solution &shorter, const Problem &problem, Solution &out)
{
    const Exchange &s = shorter.e;
    Exchange &e = out.e;
    const std::size_t bands = out.bands.size();
    const int total = e.nfcns + 1, before = s.nfcns + 1;
    // The shorter reference, band by band.
    std::vector<std::vector<double>> old(bands);
    for (int j = 1; j <= before; ++j)
    {
        const double f = s.grid[s.iext[j]];
        for (std::size_t b = 0; b < bands; ++b)
        {
            if (f >= problem.edges[2 * b] - 1e-12 && f <= problem.edges[2 * b + 1] + 1e-12)
            {
                old[b].push_back(f);
                break;
            }
        }
    }
    // How many in each band: in proportion, the remainders to the largest fractions, within what it holds.
    std::vector<int> count(bands, 0), room(bands, 0);
    std::vector<double> fraction(bands, 0.0);
    int given = 0;
    for (std::size_t b = 0; b < bands; ++b)
    {
        room[b] = std::max(0, out.bands[b].second - out.bands[b].first + 1);
        const double share = static_cast<double>(old[b].size()) * total / std::max(1, before);
        count[b] = std::min(room[b], static_cast<int>(share));
        fraction[b] = share - std::floor(share);
        given += count[b];
    }
    while (given < total)
    {
        std::size_t best = bands;
        for (std::size_t b = 0; b < bands; ++b)
            if (count[b] < room[b] && (best == bands || fraction[b] > fraction[best])) best = b;
        if (best == bands) break;
        ++count[best];
        fraction[best] -= 1.0;
        ++given;
    }
    int at = 1;
    for (std::size_t b = 0; b < bands; ++b)
    {
        const int n = count[b];
        if (n == 0) continue;
        const int first = out.bands[b].first, last = out.bands[b].second;
        const std::vector<double> &p = old[b];
        std::vector<int> index(static_cast<std::size_t>(n));
        for (int k = 0; k < n; ++k)
        {
            double f = 0.0;
            if (p.size() >= 2 && n >= 2)
            {
                const double t = static_cast<double>(k) * static_cast<double>(p.size() - 1) / (n - 1);
                const auto i = std::min(static_cast<std::size_t>(t), p.size() - 2);
                f = p[i] + (t - static_cast<double>(i)) * (p[i + 1] - p[i]);
            }
            else if (p.size() == 1 && n == 1)
            {
                f = p[0];
            }
            else
            {
                f = e.grid[first] + (e.grid[last] - e.grid[first]) * (n == 1 ? 0.5 : static_cast<double>(k) / (n - 1));
            }
            // The nearest grid point: the band's points are evenly spaced but for its last one.
            auto lo = first, hi = last;
            while (hi - lo > 1)
            {
                const int mid = (lo + hi) / 2;
                (e.grid[mid] <= f ? lo : hi) = mid;
            }
            index[static_cast<std::size_t>(k)] = std::abs(e.grid[hi] - f) < std::abs(f - e.grid[lo]) ? hi : lo;
        }
        for (int k = 0; k < n; ++k)
        {
            auto &v = index[static_cast<std::size_t>(k)];
            v = std::max(v, k == 0 ? first : index[static_cast<std::size_t>(k - 1)] + 1);
        }
        for (int k = n - 1; k >= 0; --k)
        {
            auto &v = index[static_cast<std::size_t>(k)];
            v = std::min(v, k == n - 1 ? last : index[static_cast<std::size_t>(k + 1)] - 1);
        }
        for (int k = 0; k < n; ++k) e.iext[at++] = index[static_cast<std::size_t>(k)];
    }
}

// The exchange for nfilt taps, from the equally spaced guess; stopped by rounding, again from the reference of
// a design half as long (itself found the same way).
bool solve(const Problem &problem, int nfilt, Solution &out, int depth = 0)
{
    if (!setUp(problem, nfilt, out)) return false;
    out.ending = remez(out.e, problem.itrmax);
    if (out.usable() || out.e.nfcns < 24 || depth >= 12) return true;
    int shorterLength = nfilt / 2;
    if (shorterLength % 2 != nfilt % 2) ++shorterLength;
    Solution shorter;
    if (!solve(problem, shorterLength, shorter, depth + 1) || !shorter.usable()) return true;
    Solution again;
    if (!setUp(problem, nfilt, again)) return true;
    scaleReference(shorter, problem, again);
    again.ending = remez(again.e, problem.itrmax);
    if (again.usable() || !std::isfinite(out.e.dev)) out = std::move(again);
    return true;
}
} // namespace

RemezDesign designRemez(const RemezSpec &spec)
{
    RemezDesign design;
    const double fs = spec.sampleRateHz;
    const std::size_t numtaps = spec.taps;
    const std::size_t bands = spec.bandsHz.size() / 2;
    if (!(std::isfinite(fs) && fs > 0.0))
    {
        design.error = "La fréquence d'échantillonnage doit être positive.";
        return design;
    }
    if (numtaps < 3 || numtaps > kMaximumRemezTaps)
    {
        design.error = "Parks-McClellan : de 3 à " + std::to_string(kMaximumRemezTaps) + " coefficients.";
        return design;
    }
    if (bands == 0 || spec.bandsHz.size() % 2 != 0 || spec.desired.size() != bands ||
        (!spec.weights.empty() && spec.weights.size() != bands))
    {
        design.error = "Parks-McClellan : une paire de bords, un gain voulu (et un poids) par bande.";
        return design;
    }
    std::vector<double> edges(spec.bandsHz.size());
    for (std::size_t i = 0; i < edges.size(); ++i)
    {
        edges[i] = spec.bandsHz[i] / fs;
        if (!(edges[i] >= 0.0 && edges[i] <= 0.5) || (i > 0 && !(edges[i] > edges[i - 1])))
        {
            design.error = "Parks-McClellan : les bords des bandes doivent croître de 0 à fs/2.";
            return design;
        }
    }
    std::vector<double> weights = spec.weights.empty() ? std::vector<double>(bands, 1.0) : spec.weights;
    for (std::size_t b = 0; b < bands; ++b)
    {
        if (!(weights[b] > 0.0) || !std::isfinite(weights[b]) || !std::isfinite(spec.desired[b]))
        {
            design.error = "Parks-McClellan : les poids doivent être positifs, les gains voulus finis.";
            return design;
        }
    }

    // Symmetric: an odd number of taps (type I) has nfcns = (N + 1)/2 cosines, an even one (type II) N/2.
    const int nfilt = static_cast<int>(numtaps);
    const bool nodd = nfilt % 2 == 1;
    Problem problem;
    problem.edges = edges;
    problem.desired = spec.desired;
    problem.weights = weights;
    problem.density = std::max(1, spec.gridDensity);
    problem.itrmax = std::max(1, spec.maximumIterations);
    Solution solution;
    if (!solve(problem, nfilt, solution))
    {
        design.error = "Parks-McClellan : des bandes trop étroites pour tant de coefficients.";
        return design;
    }
    Exchange &e = solution.e;
    const int nfcns = e.nfcns;
    const Ending ending = solution.ending;
    design.iterations = e.niter;
    design.deviation = e.dev;
    if (!std::isfinite(e.dev))
    {
        design.error = "Parks-McClellan : le calcul n'aboutit pas (élargissez les transitions ou prenez moins de "
                       "coefficients).";
        return design;
    }
    if (ending == Ending::Iterations)
    {
        design.stop = RemezStop::Iterations;
        design.stopped = "l'ondulation n'était pas encore égale après " + std::to_string(e.niter) + " itérations";
    }
    else if (ending == Ending::Rounding)
    {
        design.stop = RemezStop::Rounding;
        design.stopped = "arrêté à l'itération " + std::to_string(e.niter) +
                         " : l'erreur égalisée ne croît plus, la précision des calculs est atteinte";
    }
    coefficients(e);

    // The impulse response from the cosine series (the main program), symmetric.
    const int nm1 = nfcns - 1, nz = nfcns + 1;
    One<double> h(nfilt);
    if (nodd)
    {
        for (int j = 1; j <= nm1; ++j) h[j] = 0.5 * e.alpha[nz - j];
        h[nfcns] = e.alpha[1];
    }
    else
    {
        h[1] = 0.25 * e.alpha[nfcns];
        for (int j = 2; j <= nm1; ++j) h[j] = 0.25 * (e.alpha[nz - j] + e.alpha[nfcns + 2 - j]);
        h[nfcns] = 0.5 * e.alpha[1] + 0.25 * e.alpha[2];
    }
    design.taps.resize(numtaps);
    for (int j = 1; j <= nfcns; ++j)
    {
        design.taps[static_cast<std::size_t>(j - 1)] = h[j];
        design.taps[static_cast<std::size_t>(nfilt - j)] = h[j];
    }
    for (double v : design.taps)
    {
        if (!std::isfinite(v))
        {
            design.taps.clear();
            design.error = "Parks-McClellan : le calcul n'aboutit pas (élargissez les transitions ou prenez moins de "
                           "coefficients).";
            return design;
        }
    }
    return design;
}
} // namespace qacq::dsp
