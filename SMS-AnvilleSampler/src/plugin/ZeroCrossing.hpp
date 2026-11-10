#pragma once

#include <cmath>
#include <cstddef>
#include <span>

namespace midichopper::plugin {

/**
 * Nearest zero crossing of a mono signal to `centre`, searched in
 * [centre - radius, centre + radius] (clamped to the span). A crossing sits
 * between samples i-1 and i when their signs differ or sample i is zero; the
 * returned index is whichever of the two is closer to zero, so an edge placed
 * there starts (or ends) next to the sign change. Returns `centre` unchanged
 * when the window holds none. The result never leaves the window.
 */
[[nodiscard]] inline std::size_t nearestZeroCrossing(const std::span<const float> mono,
                                                     const std::size_t centre,
                                                     const std::size_t radius) noexcept
{
    if (mono.empty() || centre >= mono.size())
        return centre;
    const std::size_t low = centre > radius ? centre - radius : 0U;
    const std::size_t high = centre + radius < mono.size() ? centre + radius : mono.size() - 1U;
    bool found = false;
    std::size_t best = centre;
    std::size_t bestDistance = 0U;
    for (std::size_t i = low; i <= high; ++i) {
        const float b = mono[i];
        const float a = i > 0U ? mono[i - 1U] : b;
        const bool cross = b == 0.0f || (a < 0.0f && b > 0.0f) || (a > 0.0f && b < 0.0f);
        if (!cross)
            continue;
        std::size_t pick = i;
        if (i > low && std::fabs(a) < std::fabs(b))
            pick = i - 1U;
        const std::size_t distance = pick > centre ? pick - centre : centre - pick;
        if (!found || distance < bestDistance) {
            found = true;
            best = pick;
            bestDistance = distance;
        }
    }
    return best;
}

} // namespace midichopper::plugin
