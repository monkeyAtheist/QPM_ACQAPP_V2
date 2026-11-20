#include "qacq_test.h"
#include "signals.h"

#include "qacq/dsp/constants.h"
#include "qacq/dsp/fft.h"

#include <algorithm>
#include <cmath>

using namespace qacq::dsp;

namespace
{
std::vector<Complex> naiveDft(const std::vector<Complex> &x)
{
    const std::size_t n = x.size();
    std::vector<Complex> out(n);
    for (std::size_t k = 0; k < n; ++k)
    {
        Complex sum(0.0, 0.0);
        for (std::size_t i = 0; i < n; ++i)
        {
            const std::size_t r = (k * i) % n; // exact angle
            sum += x[i] * std::polar(1.0, -kTwoPi * static_cast<double>(r) / static_cast<double>(n));
        }
        out[k] = sum;
    }
    return out;
}

std::vector<Complex> testComplex(std::size_t n)
{
    const auto re = qacq::test::lcgNoise(n, 7u);
    const auto im = qacq::test::lcgNoise(n, 99u);
    std::vector<Complex> x(n);
    for (std::size_t i = 0; i < n; ++i) x[i] = Complex(re[i], im[i]);
    return x;
}

double maxError(const std::vector<Complex> &a, const std::vector<Complex> &b)
{
    double e = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) e = std::max(e, std::abs(a[i] - b[i]));
    return e;
}

double norm2(const std::vector<Complex> &a)
{
    double s = 0.0;
    for (const auto &v : a) s += std::norm(v);
    return std::sqrt(s);
}
} // namespace

TEST_CASE("FFT matches a direct DFT for powers of two and other lengths", "[fft]")
{
    for (std::size_t n : {1u, 2u, 3u, 5u, 7u, 8u, 16u, 100u, 127u, 997u, 1000u, 1024u})
    {
        const auto x = testComplex(n);
        const auto reference = naiveDft(x);
        const auto fast = fft(x);
        CHECK(maxError(fast, reference) <= 1e-12 * std::max(1.0, norm2(reference)));
    }
}

TEST_CASE("Inverse FFT restores the record (radix 2 and Bluestein)", "[fft]")
{
    for (std::size_t n : {4096u, 4097u, 48000u, 177147u, 262144u})
    {
        const auto x = testComplex(n);
        const auto back = ifft(fft(x));
        CHECK(maxError(back, x) < 1e-13);
    }
}

TEST_CASE("Parseval and real-input FFT", "[fft]")
{
    const auto x = qacq::test::testSignal(3001, 48000.0);
    std::vector<Complex> cx(x.begin(), x.end());
    const auto full = fft(cx);
    double timeEnergy = 0.0, freqEnergy = 0.0;
    for (double v : x) timeEnergy += v * v;
    for (const auto &v : full) freqEnergy += std::norm(v);
    CHECK_REL(freqEnergy / static_cast<double>(x.size()), timeEnergy, 1e-12);

    const auto half = rfft(x);
    REQUIRE(half.size() == x.size() / 2 + 1);
    double e = 0.0;
    for (std::size_t k = 0; k < half.size(); ++k) e = std::max(e, std::abs(half[k] - full[k]));
    CHECK(e < 1e-11);

    const auto padded = rfft(x, 8192);
    CHECK(padded.size() == 4097);
    CHECK_NEAR(padded[0].real(), full[0].real(), 1e-12, 1e-12);
}

TEST_CASE("Large FFT stays accurate (no running twiddle products)", "[fft]")
{
    const std::size_t n = std::size_t{1} << 20;
    const std::size_t bin = 12345;
    std::vector<Complex> x(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const std::size_t r = (bin * i) % n;
        x[i] = Complex(std::cos(kTwoPi * static_cast<double>(r) / static_cast<double>(n)), 0.0);
    }
    const auto X = fft(x);
    CHECK_REL(std::abs(X[bin]), 0.5 * static_cast<double>(n), 1e-12);
    double leakage = 0.0;
    for (std::size_t k = 0; k < n; ++k)
        if (k != bin && k != n - bin) leakage = std::max(leakage, std::abs(X[k]));
    CHECK(leakage < 1e-8); // relative to the peak (5e5): below -290 dB
}

TEST_CASE("Power-of-two helpers", "[fft]")
{
    CHECK(isPowerOfTwo(1));
    CHECK(isPowerOfTwo(1024));
    CHECK(!isPowerOfTwo(0));
    CHECK(!isPowerOfTwo(1000));
    CHECK(nextPowerOfTwo(0) == 1);
    CHECK(nextPowerOfTwo(1000) == 1024);
    CHECK(nextPowerOfTwo(1024) == 1024);
}
