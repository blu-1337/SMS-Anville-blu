#pragma once

#include "UI/Geometry.hpp"

#include <algorithm>

namespace sms::ui {

/** Fits a fixed logical canvas into a window, centered, preserving aspect ratio. */
struct ViewportTransform {
    float scale = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
};

[[nodiscard]] inline ViewportTransform fitViewport(const float width, const float height,
                                                   const float logicalWidth,
                                                   const float logicalHeight) noexcept
{
    const float scale = std::max(0.1f, std::min(width / logicalWidth, height / logicalHeight));
    return {scale, (width - logicalWidth * scale) * 0.5f, (height - logicalHeight * scale) * 0.5f};
}

/** Window pixels (mouse events, native file drops) to logical canvas units. */
[[nodiscard]] inline Point toLogical(const ViewportTransform& transform, const double x,
                                     const double y) noexcept
{
    return {(static_cast<float>(x) - transform.offsetX) / transform.scale,
            (static_cast<float>(y) - transform.offsetY) / transform.scale};
}

} // namespace sms::ui
