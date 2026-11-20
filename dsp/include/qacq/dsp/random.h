#pragma once

// Reproducible random numbers: the same seed gives the same noise on every compiler and system
// (std::normal_distribution does not: libstdc++ and MSVC differ). xoshiro256** seeded by splitmix64,
// normal values by the Box-Muller transform.

#include <cmath>
#include <cstdint>

#include "qacq/dsp/constants.h"

namespace qacq::dsp
{
class Random
{
public:
    explicit Random(std::uint64_t seed = 1) noexcept
    {
        std::uint64_t s = seed;
        for (auto &word : m_state) word = splitmix(s);
    }

    std::uint64_t next() noexcept
    {
        const std::uint64_t result = rotl(m_state[1] * 5, 7) * 9;
        const std::uint64_t t = m_state[1] << 17;
        m_state[2] ^= m_state[0];
        m_state[3] ^= m_state[1];
        m_state[1] ^= m_state[2];
        m_state[0] ^= m_state[3];
        m_state[2] ^= t;
        m_state[3] = rotl(m_state[3], 45);
        return result;
    }

    // Uniform in [0, 1), 53 random bits.
    double uniform() noexcept { return static_cast<double>(next() >> 11) * 0x1.0p-53; }

    // Standard normal (mean 0, variance 1).
    double normal() noexcept
    {
        if (m_hasSpare)
        {
            m_hasSpare = false;
            return m_spare;
        }
        double u1 = uniform();
        while (u1 <= 0.0) u1 = uniform();
        const double u2 = uniform();
        const double radius = std::sqrt(-2.0 * std::log(u1));
        m_spare = radius * std::sin(kTwoPi * u2);
        m_hasSpare = true;
        return radius * std::cos(kTwoPi * u2);
    }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }
    static std::uint64_t splitmix(std::uint64_t &x) noexcept
    {
        std::uint64_t z = (x += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    std::uint64_t m_state[4]{};
    double m_spare = 0.0;
    bool m_hasSpare = false;
};
} // namespace qacq::dsp
