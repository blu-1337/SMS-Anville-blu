#pragma once

// DPF-free SPEC heatmap colouring: 8-bit levels (column-major, bin 0 = lowest
// frequency) to RGBA rows (top row = highest frequency). Skin-neutral: dark and
// transparent at silence, warm amber when loud, never opaque so the waveform
// and slice tints stay readable on top.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sms::ui {

inline void spectrogramPixel(const std::uint8_t level, std::uint8_t* const rgba) noexcept
{
    const float t = static_cast<float>(level) / 255.0F;
    rgba[0] = static_cast<std::uint8_t>(40.0F + 215.0F * t);
    rgba[1] = static_cast<std::uint8_t>(20.0F + 130.0F * t * t);
    rgba[2] = static_cast<std::uint8_t>(40.0F * (1.0F - t) + 10.0F);
    rgba[3] = static_cast<std::uint8_t>(150.0F * t * t);  // at most about 59% opaque
}

/** `rgba` must hold columns * bins * 4 bytes. */
inline void spectrogramPixels(const std::vector<std::uint8_t>& levels, const std::size_t columns,
                              const std::size_t bins, std::vector<std::uint8_t>& rgba) noexcept
{
    if (levels.size() < columns * bins || rgba.size() < columns * bins * 4U)
        return;
    for (std::size_t c = 0; c < columns; ++c)
        for (std::size_t b = 0; b < bins; ++b)
            spectrogramPixel(levels[c * bins + b], &rgba[((bins - 1U - b) * columns + c) * 4U]);
}

} // namespace sms::ui
