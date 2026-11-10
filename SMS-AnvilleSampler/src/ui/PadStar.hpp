#pragma once

#include "MidichopperLayout.hpp"

#include <cmath>

namespace midichopper::ui {

/** Favorite star hit area: the top-right corner of a pad cell (base canvas units). */
[[nodiscard]] constexpr sms::ui::Rect padStarRect(const sms::ui::Rect& cell) noexcept
{
    return {cell.x + cell.width - 26.0f, cell.y + 4.0f, 22.0f, 22.0f};
}

/** Five-point star: filled gold when favorite, a faint outline otherwise. */
template <class Canvas>
void drawPadStar(Canvas& canvas, const sms::ui::Rect& cell, const bool favorite, const bool hot)
{
    const auto r = padStarRect(cell);
    const float cx = r.x + r.width * 0.5f;
    const float cy = r.y + r.height * 0.5f + 1.0f;
    constexpr float outer = 8.0f, inner = 3.6f, pi = 3.14159265f;
    canvas.beginPath();
    for (int i = 0; i < 10; ++i) {
        const float radius = (i % 2 == 0) ? outer : inner;
        const float angle = -pi * 0.5f + static_cast<float>(i) * pi / 5.0f;
        const float px = cx + radius * std::cos(angle), py = cy + radius * std::sin(angle);
        if (i == 0) canvas.moveTo(px, py); else canvas.lineTo(px, py);
    }
    canvas.closePath();
    if (favorite) {
        canvas.fillColor(255, 209, 51, 255);
        canvas.fill();
    } else if (hot) {
        canvas.strokeColor(255, 255, 255, 140);
        canvas.strokeWidth(1.2f);
        canvas.stroke();
    }
}

} // namespace midichopper::ui
