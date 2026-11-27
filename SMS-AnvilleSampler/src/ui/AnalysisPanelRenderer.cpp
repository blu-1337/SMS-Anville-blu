#include "ViewRenderers.hpp"

#include "DPF/AnalogMaterials.hpp"
#include "DPF/Controls.hpp"

namespace midichopper::ui::rendering {
namespace {
namespace uiLayout = midichopper::ui::layout;

void readout(const ViewRenderContext& ctx, const sms::ui::Rect bounds, const char* label,
             const int field)
{
    const auto& colors = sms::ui::dpf::theme();
    const float x = bounds.x + bounds.width * 0.5f;
    ctx.canvas.textAlign(DGL_NAMESPACE::NanoVG::ALIGN_CENTER | DGL_NAMESPACE::NanoVG::ALIGN_MIDDLE);
    ctx.canvas.fontSize(9.0f);
    ctx.canvas.fillColor(colors.contentSecondary);
    text(ctx, x, bounds.y + 9.0f, label, nullptr);
    const auto value = analysisReadout(ctx.state.analysis, field);
    ctx.canvas.fontSize(12.0f);
    ctx.canvas.fillColor(colors.contentPrimary);
    ctx.canvas.text(x, bounds.y + 22.0f, value.c_str(), nullptr);
}
} // namespace

void drawAnalysisPanel(const ViewRenderContext& ctx)
{
    const auto& colors = sms::ui::dpf::theme();
    const auto& view = ctx.state.analysis;
    sms::ui::dpf::drawInsetSurface(ctx.canvas, uiLayout::analysisPanel, 6.0f);
    sms::ui::dpf::drawSegment(ctx.canvas, uiLayout::analysisToggle, view.on ? "ON" : "OFF", view.on,
                              colors.activityPlayback, hovered(ctx, InteractiveType::analysisToggle));
    ctx.canvas.fontFace(NANOVG_DEJAVU_SANS_TTF);
    readout(ctx, uiLayout::analysisNote, "NOTE", 0);
    readout(ctx, uiLayout::analysisChord, "CHORD", 1);
    readout(ctx, uiLayout::analysisKey, "KEY", 2);

    const float confidence = analysisConfidence(view);
    const auto dot = uiLayout::analysisConfidence;
    ctx.canvas.beginPath();
    ctx.canvas.circle(dot.x + dot.width * 0.5f, dot.y + dot.height * 0.5f, 5.0f);
    ctx.canvas.fillColor(confidence >= 0.75f ? colors.meterGreen
                         : confidence >= 0.40f ? colors.meterYellow
                                               : colors.contentSecondary.withAlpha(colors.disabledAlpha));
    ctx.canvas.fill();

    // TUNE arrives in a later version: drawn disabled, with no hit target.
    sms::ui::dpf::drawSegment(ctx.canvas, uiLayout::tuneButton, "TUNE", false, colors.controlAccent,
                              false, false);
    const auto caret = uiLayout::tuneDropdown;
    sms::ui::dpf::drawSegment(ctx.canvas, caret, "", false, colors.controlAccent, false, false);
    const float x = caret.x + caret.width * 0.5f;
    const float y = caret.y + caret.height * 0.5f;
    ctx.canvas.beginPath();
    ctx.canvas.moveTo(x - 5.0f, y - 2.0f);
    ctx.canvas.lineTo(x + 5.0f, y - 2.0f);
    ctx.canvas.lineTo(x, y + 4.0f);
    ctx.canvas.closePath();
    ctx.canvas.fillColor(colors.contentSecondary.withAlpha(colors.disabledAlpha));
    ctx.canvas.fill();
}

} // namespace midichopper::ui::rendering
