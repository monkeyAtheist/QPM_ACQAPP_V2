#pragma once

// The one FFT of the project (the old application had four).
//
// Any length: radix-2 for powers of two, Bluestein's chirp-z algorithm otherwise, so a record is never
// truncated or padded behind the caller's back. Convention:
//   forward  X[k] = sum_n x[n] exp(-j 2 pi k n / N)
//   inverse  x[n] = (1/N) sum_k X[k] exp(+j 2 pi k n / N)
// Twiddle factors are computed directly (no running products), so the round-off stays at a few ulp
// times log2(N) even for N = 2^24.

#include <complex>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace qacq::dsp
{
using Complex = std::complex<double>;

bool isPowerOfTwo(std::size_t n) noexcept;
// Smallest power of two >= n (1 for n = 0).
std::size_t nextPowerOfTwo(std::size_t n) noexcept;

// A prepared transform of one length. Reuse it when many records of the same length are transformed
// (Welch, spectrogram, overlap-add convolution): the twiddle tables are built once.
// A plan is immutable after construction and can be shared between threads.
class FftPlan
{
public:
    explicit FftPlan(std::size_t n);
    ~FftPlan();
    FftPlan(const FftPlan &) = delete;
    FftPlan &operator=(const FftPlan &) = delete;
    FftPlan(FftPlan &&) noexcept;
    FftPlan &operator=(FftPlan &&) noexcept;

    std::size_t size() const noexcept { return m_n; }

    // In place; data.size() must equal size().
    void forward(std::span<Complex> data) const;
    void inverse(std::span<Complex> data) const;

private:
    struct Radix2;
    struct Bluestein;
    std::size_t m_n = 0;
    std::unique_ptr<Radix2> m_radix2;       // used when n is a power of two
    std::unique_ptr<Bluestein> m_bluestein; // used otherwise
};

// One-shot helpers (they build a plan each time).
std::vector<Complex> fft(std::span<const Complex> x);
std::vector<Complex> ifft(std::span<const Complex> x);

// DFT of a real record, zero-padded to n points (n = 0 means x.size(); n must be >= x.size()).
// Returns the n/2 + 1 bins of the non-negative frequencies.
std::vector<Complex> rfft(std::span<const double> x, std::size_t n = 0);
} // namespace qacq::dsp
