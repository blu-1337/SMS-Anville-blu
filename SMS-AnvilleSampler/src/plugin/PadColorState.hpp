#pragma once

#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace midichopper::plugin {

// pad_color_N v2: bit 8 (256) is the FAVORITE flag, the low byte is the colour index.
// Version-1 values (plain 0..palette) have the bit clear, so old projects load as
// "not favorite"; an old plugin reads a favorite value as uncoloured.
inline constexpr int kPadFavoriteBit = 256;
inline constexpr const char* kFavoriteRefusal = "Pad is a favorite \xE2\x80\x94 unfavorite to change";

[[nodiscard]] inline int decodePadColorValue(const std::string_view value) noexcept
{
    int raw = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), raw);
    if (error != std::errc{} || end != value.data() + value.size() ||
        raw < 0 || raw > (kPadFavoriteBit | 255))
        return 0;
    return raw;
}

// Zero is the uncolored pad. Out-of-palette and malformed saved values are
// displayed as uncolored by the UI, including when a theme has fewer colors.
inline int decodePadColorIndex(const std::string_view value,
                               const int paletteSize) noexcept
{
    const int index = decodePadColorValue(value) & 255;
    return index > paletteSize ? 0 : index;
}

[[nodiscard]] inline bool decodePadFavorite(const std::string_view value) noexcept
{
    return (decodePadColorValue(value) & kPadFavoriteBit) != 0;
}

// Plugin-side bitmask of favorite pads (kPadCount <= 64).
template <class Array>
[[nodiscard]] inline std::uint64_t favoriteMaskOf(const Array& colors) noexcept
{
    std::uint64_t mask = 0U;
    for (std::size_t pad = 0; pad < colors.size() && pad < 64U; ++pad)
        if ((colors[pad].load(std::memory_order_relaxed) & kPadFavoriteBit) != 0)
            mask |= std::uint64_t{1} << pad;
    return mask;
}

} // namespace midichopper::plugin
