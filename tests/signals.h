#pragma once

// Deterministic test signals, reproduced bit for bit by tools/reference/make_scipy_reference.py.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

namespace qacq::test
{
// Uniform values in [-0.5, 0.5) from the classic 32-bit LCG (Numerical Recipes constants).
inline std::vector<double> lcgNoise(std::size_t n, std::uint32_t seed)
{
    std::vector<double> out(n);
    std::uint32_t state = seed;
    for (std::size_t i = 0; i < n; ++i)
    {
        state = 1664525u * state + 1013904223u;
        out[i] = static_cast<double>(state) / 4294967296.0 - 0.5;
    }
    return out;
}

inline std::vector<double> testSignal(std::size_t n, double fs)
{
    std::vector<double> x = lcgNoise(n, 12345u);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i) / fs;
        x[i] += 0.7 * std::sin(2.0 * std::numbers::pi * 1000.0 * t) + 0.3 * std::sin(2.0 * std::numbers::pi * 37.0 * t + 0.4);
    }
    return x;
}

// sum_k a_k sin(2 pi k f0 t + 0.3 k), the harmonic test signal of the audit.
inline std::vector<double> harmonicSignal(std::size_t n, double fs, double f0, const std::vector<double> &amplitudes)
{
    std::vector<double> x(n, 0.0);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i) / fs;
        for (std::size_t k = 0; k < amplitudes.size(); ++k)
        {
            const double order = static_cast<double>(k + 1);
            x[i] += amplitudes[k] * std::sin(2.0 * std::numbers::pi * order * f0 * t + 0.3 * order);
        }
    }
    return x;
}
} // namespace qacq::test
