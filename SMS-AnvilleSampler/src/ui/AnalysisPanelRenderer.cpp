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
    ctx.canvas.globalAlpha(ctx.state.topGspEnabled ? 1.0f : colors.disabledAlpha);
    sms::ui::dpf::drawSegment(ctx.canvas, uiLayout::topGsp, "GSP", ctx.state.topGspOn,
                              colors.controlAccent,
                              ctx.state.topGspEnabled && hovered(ctx, InteractiveType::topGsp));
    ctx.canvas.globalAlpha(1.0f);
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

    sms::ui::dpf::drawSegment(ctx.canvas, uiLayout::tuneButton, "TUNE", false, colors.controlAccent,
                              hovered(ctx, InteractiveType::tuneButton), view.tunable);
    const auto caret = uiLayout::tuneDropdown;
    sms::ui::dpf::drawSegment(ctx.canvas, caret, "", view.tuneMenu, colors.controlAccent,
                              hovered(ctx, InteractiveType::tuneDropdown), view.tunable);
    const float x = caret.x + caret.width * 0.5f;
    const float y = caret.y + caret.height * 0.5f;
    ctx.canvas.beginPath();
    ctx.canvas.moveTo(x - 5.0f, y - 2.0f);
    ctx.canvas.lineTo(x + 5.0f, y - 2.0f);
    ctx.canvas.lineTo(x, y + 4.0f);
    ctx.canvas.closePath();
    ctx.canvas.fillColor(view.tunable
        ? colors.contentPrimary : colors.contentSecondary.withAlpha(colors.disabledAlpha));
    ctx.canvas.fill();
}

// Drawn after the view so it lies over it; lit cell = the sounding note's pitch class.
void drawTuneMenu(const ViewRenderContext& ctx)
{
    const auto& view = ctx.state.analysis;
    if (!view.tuneMenu)
        return;
    const auto& colors = sms::ui::dpf::theme();
    const auto box = uiLayout::tuneDropdownBox;
    ctx.canvas.beginPath();
    ctx.canvas.roundedRect(box.x, box.y, box.width, box.height, 5.0f);
    ctx.canvas.fillColor(colors.recess);
    ctx.canvas.fill();
    ctx.canvas.strokeColor(colors.controlAccent);
    ctx.canvas.strokeWidth(1.5f);
    ctx.canvas.stroke();
    ctx.canvas.fontFace(NANOVG_DEJAVU_SANS_TTF);
    ctx.canvas.fontSize(11.0f);
    ctx.canvas.textAlign(DGL_NAMESPACE::NanoVG::ALIGN_CENTER | DGL_NAMESPACE::NanoVG::ALIGN_MIDDLE);
    const long nearest = std::lround(view.result.note.midi + view.tune);
    const int current = static_cast<int>(((nearest % 12) + 12) % 12);
    for (int item = 0; item < 12; ++item) {
        const auto cell = uiLayout::tuneNoteItem(item);
        const bool selected = item == current;
        const bool hot = ctx.state.hoveredTarget.is(static_cast<int>(InteractiveType::tuneNoteItem)) &&
                         ctx.state.hoveredTarget.index == item;
        if (selected || hot) {
            ctx.canvas.beginPath();
            ctx.canvas.roundedRect(cell.x + 1.0f, cell.y + 1.0f, cell.width - 2.0f, cell.height - 2.0f, 4.0f);
            ctx.canvas.fillColor(colors.controlAccent.withAlpha(selected ? 0.35f : 0.18f));
            ctx.canvas.fill();
        }
        ctx.canvas.fillColor(selected ? colors.controlAccent : colors.contentPrimary);
        text(ctx, cell.x + cell.width * 0.5f, cell.y + cell.height * 0.5f,
             sms::analysis::kPitchClassNames[static_cast<std::size_t>(item)], nullptr);
    }
}

} // namespace midichopper::ui::rendering
