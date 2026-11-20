#include "qacq/dsp/fft.h"

#include "qacq/dsp/constants.h"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace qacq::dsp
{
bool isPowerOfTwo(std::size_t n) noexcept
{
    return n != 0 && (n & (n - 1)) == 0;
}

std::size_t nextPowerOfTwo(std::size_t n) noexcept
{
    std::size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

// --------------------------------------------------------------------------- radix 2
struct FftPlan::Radix2
{
    std::size_t n = 0;
    std::vector<Complex> twiddle;      // exp(-j 2 pi k / n), k < n / 2
    std::vector<std::size_t> reversed; // bit-reversal permutation

    explicit Radix2(std::size_t size) : n(size)
    {
        twiddle.resize(n / 2);
        for (std::size_t k = 0; k < n / 2; ++k)
        {
            const double angle = -kTwoPi * static_cast<double>(k) / static_cast<double>(n);
            twiddle[k] = Complex(std::cos(angle), std::sin(angle));
        }
        reversed.resize(n);
        std::size_t bits = 0;
        while ((std::size_t{1} << bits) < n) ++bits;
        for (std::size_t i = 0; i < n; ++i)
        {
            std::size_t r = 0;
            for (std::size_t b = 0; b < bits; ++b)
                if (i & (std::size_t{1} << b)) r |= std::size_t{1} << (bits - 1 - b);
            reversed[i] = r;
        }
    }

    void run(std::span<Complex> data, bool inverse) const
    {
        for (std::size_t i = 0; i < n; ++i)
            if (i < reversed[i]) std::swap(data[i], data[reversed[i]]);
        for (std::size_t length = 2; length <= n; length <<= 1)
        {
            const std::size_t half = length / 2;
            const std::size_t stride = n / length;
            for (std::size_t start = 0; start < n; start += length)
            {
                for (std::size_t j = 0; j < half; ++j)
                {
                    const Complex w = inverse ? std::conj(twiddle[j * stride]) : twiddle[j * stride];
                    const Complex u = data[start + j];
                    const Complex v = data[start + j + half] * w;
                    data[start + j] = u + v;
                    data[start + j + half] = u - v;
                }
            }
        }
    }
};

// --------------------------------------------------------------------------- Bluestein
struct FftPlan::Bluestein
{
    std::size_t n = 0;
    std::size_t m = 0;
    std::vector<Complex> chirp;       // exp(-j pi k^2 / n), k < n
    std::vector<Complex> kernelFft;   // FFT_m of the conjugate chirp, wrapped
    std::unique_ptr<Radix2> inner;

    explicit Bluestein(std::size_t size) : n(size), m(nextPowerOfTwo(2 * size - 1))
    {
        chirp.resize(n);
        const std::uint64_t period = 2 * static_cast<std::uint64_t>(n);
        for (std::size_t k = 0; k < n; ++k)
        {
            // k^2 mod 2n keeps the angle small, hence exact to the last bits even for large k.
            const std::uint64_t kk = static_cast<std::uint64_t>(k);
            const std::uint64_t r = (kk * kk) % period;
            const double angle = -kPi * static_cast<double>(r) / static_cast<double>(n);
            chirp[k] = Complex(std::cos(angle), std::sin(angle));
        }
        inner = std::make_unique<Radix2>(m);
        kernelFft.assign(m, Complex(0.0, 0.0));
        kernelFft[0] = std::conj(chirp[0]);
        for (std::size_t k = 1; k < n; ++k)
        {
            kernelFft[k] = std::conj(chirp[k]);
            kernelFft[m - k] = std::conj(chirp[k]);
        }
        inner->run(kernelFft, false);
    }

    void forward(std::span<Complex> data) const
    {
        std::vector<Complex> work(m, Complex(0.0, 0.0));
        for (std::size_t k = 0; k < n; ++k) work[k] = data[k] * chirp[k];
        inner->run(work, false);
        for (std::size_t k = 0; k < m; ++k) work[k] *= kernelFft[k];
        inner->run(work, true);
        const double scale = 1.0 / static_cast<double>(m);
        for (std::size_t k = 0; k < n; ++k) data[k] = work[k] * scale * chirp[k];
    }
};

// --------------------------------------------------------------------------- plan
FftPlan::FftPlan(std::size_t n) : m_n(n)
{
    if (n == 0) return;
    if (isPowerOfTwo(n))
        m_radix2 = std::make_unique<Radix2>(n);
    else
        m_bluestein = std::make_unique<Bluestein>(n);
}

FftPlan::~FftPlan() = default;
FftPlan::FftPlan(FftPlan &&) noexcept = default;
FftPlan &FftPlan::operator=(FftPlan &&) noexcept = default;

void FftPlan::forward(std::span<Complex> data) const
{
    if (data.size() != m_n) throw std::invalid_argument("FftPlan::forward: wrong data size");
    if (m_radix2)
        m_radix2->run(data, false);
    else if (m_bluestein)
        m_bluestein->forward(data);
}

void FftPlan::inverse(std::span<Complex> data) const
{
    if (data.size() != m_n) throw std::invalid_argument("FftPlan::inverse: wrong data size");
    if (m_n == 0) return;
    if (m_radix2)
    {
        m_radix2->run(data, true);
    }
    else
    {
        for (auto &value : data) value = std::conj(value);
        m_bluestein->forward(data);
        for (auto &value : data) value = std::conj(value);
    }
    const double scale = 1.0 / static_cast<double>(m_n);
    for (auto &value : data) value *= scale;
}

std::vector<Complex> fft(std::span<const Complex> x)
{
    std::vector<Complex> data(x.begin(), x.end());
    FftPlan(data.size()).forward(data);
    return data;
}

std::vector<Complex> ifft(std::span<const Complex> x)
{
    std::vector<Complex> data(x.begin(), x.end());
    FftPlan(data.size()).inverse(data);
    return data;
}

std::vector<Complex> rfft(std::span<const double> x, std::size_t n)
{
    if (n == 0) n = x.size();
    if (n < x.size()) throw std::invalid_argument("rfft: n must not be shorter than the record");
    if (n == 0) return {};
    std::vector<Complex> data(n, Complex(0.0, 0.0));
    for (std::size_t i = 0; i < x.size(); ++i) data[i] = Complex(x[i], 0.0);
    FftPlan(n).forward(data);
    data.resize(n / 2 + 1);
    return data;
}
} // namespace qacq::dsp
