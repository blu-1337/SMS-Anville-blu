#pragma once

#include "SampleData.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace midichopper {

/** Stereo frames sampled by a fingerprint; longer audio is sampled evenly. */
inline constexpr std::uint32_t kFingerprintFrames = 4096U;

namespace detail {
inline constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

constexpr void fnvMix(std::uint64_t& hash, const std::uint64_t value) noexcept
{
    for (unsigned byte = 0U; byte < 8U; ++byte) {
        hash ^= (value >> (byte * 8U)) & 0xffU;
        hash *= kFnvPrime;
    }
}
} // namespace detail

/**
 * FNV-1a over the frame count, the rate in whole hertz, and the bit patterns
 * of up to kFingerprintFrames evenly spaced stereo frames. It identifies the
 * pad audio a main-sample link was saved with, so a restored or edited pad
 * whose PCM differs drops its link. read(frame) returns {left, right}.
 */
template<class Reader>
[[nodiscard]] std::uint64_t fingerprintAudio(const std::uint32_t frames,
    const double sampleRate, Reader&& read)
{
    std::uint64_t hash = detail::kFnvOffset;
    detail::fnvMix(hash, frames);
    const bool validRate = std::isfinite(sampleRate) && sampleRate > 0.0 && sampleRate < 1.0e9;
    detail::fnvMix(hash, validRate ? static_cast<std::uint64_t>(std::llround(sampleRate)) : 0U);
    const auto count = std::min(frames, kFingerprintFrames);
    for (std::uint32_t index = 0U; index < count; ++index) {
        const auto frame = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(index) * frames / count);
        const std::array<float, 2> sample = read(frame);
        detail::fnvMix(hash, std::bit_cast<std::uint32_t>(sample[0]));
        detail::fnvMix(hash, std::bit_cast<std::uint32_t>(sample[1]));
    }
    return hash;
}

[[nodiscard]] inline std::uint64_t fingerprint(const PadData& data)
{
    const auto frames = static_cast<std::uint32_t>(
        std::min<std::size_t>(data.frames, data.stereo.size() / 2U));
    return fingerprintAudio(frames, data.sampleRate, [&data](const std::uint32_t frame) {
        const auto offset = static_cast<std::size_t>(frame) * 2U;
        return std::array<float, 2>{data.stereo[offset], data.stereo[offset + 1U]};
    });
}

} // namespace midichopper
