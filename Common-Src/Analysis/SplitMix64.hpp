// Seeded SplitMix64 generator. Deterministic across platforms so analysis and
// slice-selection tests can compare exact output. Not for cryptography.
#pragma once

#include <cstdint>

namespace sms::analysis {

class SplitMix64 {
public:
    constexpr explicit SplitMix64(const std::uint64_t seed = 0) noexcept : state_(seed) {}

    [[nodiscard]] constexpr std::uint64_t next() noexcept
    {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

    // Uniform in [0, 1) with 53 bits of precision.
    [[nodiscard]] constexpr double nextUnit() noexcept
    {
        return static_cast<double>(next() >> 11U) * (1.0 / 9007199254740992.0);
    }

    // Uniform in [0, bound); returns 0 when bound is 0. The modulo bias is
    // negligible for the small bounds used by slice selection.
    [[nodiscard]] constexpr std::uint64_t nextBelow(const std::uint64_t bound) noexcept
    {
        return bound == 0 ? 0 : next() % bound;
    }

private:
    std::uint64_t state_;
};

} // namespace sms::analysis
