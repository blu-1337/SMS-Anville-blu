#pragma once

#include "PadQuickEdit.hpp"
#include "ViewRenderers.hpp"
#include "DPF/AnalogMaterials.hpp"
#include "DPF/Controls.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace midichopper::ui::rendering {

// Drag feedback on a pad: volume lowers the solid fill from the top down; pan paints a red
// gradient on the chosen side (stronger red to the left, darker red to the right).
inline void drawQuickEdit(const ViewRenderContext& ctx, const sms::ui::Rect& cell)
{
    using DGL_NAMESPACE::Color;
    auto& canvas = ctx.canvas;
    const sms::ui::dpf::ScopedCanvasState state(canvas);
    if (ctx.state.quickEditAxis == 1) {
        const float cut = cell.height * (1.0f - std::clamp(ctx.state.quickEditVolume, 0.0f, 1.0f));
        if (cut > 0.5f) {
            canvas.scissor(cell.x, cell.y, cell.width, cut);
            canvas.beginPath();
            canvas.roundedRect(cell.x, cell.y, cell.width, cell.height, 9.0f);
            canvas.fillColor(Color(0.05f, 0.05f, 0.07f, 0.82f));
            canvas.fill();
        }
    } else if (ctx.state.quickEditAxis == 2) {
        const float pan = std::clamp(ctx.state.quickEditPan, -1.0f, 1.0f);
        if (std::fabs(pan) > 0.001f) {
            const bool left = pan < 0.0f;
            const float strength = std::fabs(pan);
            const Color edge = left ? Color(1.0f, 0.18f, 0.18f, 0.35f + 0.55f * strength)
                                    : Color(0.45f, 0.02f, 0.04f, 0.35f + 0.55f * strength);
            const Color none = left ? Color(1.0f, 0.18f, 0.18f, 0.0f) : Color(0.45f, 0.02f, 0.04f, 0.0f);
            const float reach = cell.width * (0.35f + 0.65f * strength);
            const float x0 = left ? cell.x : cell.x + cell.width;
            const float x1 = left ? cell.x + reach : cell.x + cell.width - reach;
            canvas.beginPath();
            canvas.roundedRect(cell.x, cell.y, cell.width, cell.height, 9.0f);
            canvas.fillPaint(canvas.linearGradient(x0, cell.y, x1, cell.y, edge, none));
            canvas.fill();
        }
    }
    char line1[24];
    char line2[24];
    midichopper::ui::padQuickValueText(static_cast<midichopper::ui::PadQuickAxis>(ctx.state.quickEditAxis),
                               ctx.state.quickEditVolume, ctx.state.quickEditPan, line1, sizeof(line1),
                               line2, sizeof(line2));
    canvas.resetScissor();
    canvas.fontFace(NANOVG_DEJAVU_SANS_TTF);
    canvas.fontSize(15.0f);
    canvas.textAlign(DGL_NAMESPACE::NanoVG::ALIGN_CENTER | DGL_NAMESPACE::NanoVG::ALIGN_MIDDLE);
    canvas.fillColor(Color(1.0f, 1.0f, 1.0f, 0.95f));
    canvas.text(cell.x + cell.width * 0.5f, cell.y + cell.height * (line2[0] ? 0.28f : 0.32f), line1, nullptr);
    if (line2[0]) canvas.text(cell.x + cell.width * 0.5f, cell.y + cell.height * 0.42f, line2, nullptr);
}

} // namespace midichopper::ui::rendering
