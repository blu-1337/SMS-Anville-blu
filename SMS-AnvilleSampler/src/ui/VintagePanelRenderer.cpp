#include "ProductControlsRenderer.hpp"
#include "DPF/Controls.hpp"
#include "ViewRenderContext.hpp"
#include "ViewRenderers.hpp"
#include "VintagePanel.hpp"

namespace midichopper::ui::rendering {

namespace uiLayout = layout;

namespace {

void segment(const ViewRenderContext& ctx, const sms::ui::Rect bounds, const char* const label,
             const bool on, const InteractiveType type, const int index)
{
    sms::ui::dpf::drawSegment(ctx.canvas, bounds, label, on,
                              sms::ui::dpf::theme().controlAccent, hovered(ctx, type, index));
}

} // namespace

void drawVintageButton(const ViewRenderContext& ctx, const bool pad)
{
    const auto& settings = pad ? ctx.state.mixerSettings.vintage : ctx.state.globalVintage;
    char label[48];
    vintageButtonText(label, sizeof(label), settings, pad);
    segment(ctx, pad ? uiLayout::padVintage : uiLayout::globalVintage, label,
            settings.model >= 0.5f,
            pad ? InteractiveType::padVintage : InteractiveType::globalVintage, 0);
}

void drawVintagePanel(const ViewRenderContext& ctx)
{
    const int popup = ctx.state.vintagePopup;
    if (popup < 0)
        return;
    const bool global = popup == kVintageGlobalTarget;
    const auto& settings = global ? ctx.state.globalVintage : ctx.state.mixerSettings.vintage;
    const auto& colors = sms::ui::dpf::theme();
    const auto box = uiLayout::vintagePanel;
    ctx.canvas.beginPath();
    ctx.canvas.roundedRect(box.x, box.y, box.width, box.height, 7.0f);
    ctx.canvas.fillColor(colors.recess);
    ctx.canvas.fill();
    ctx.canvas.strokeColor(colors.controlAccent);
    ctx.canvas.strokeWidth(1.5f);
    ctx.canvas.stroke();

    char title[32];
    if (global)
        std::snprintf(title, sizeof(title), "VINTAGE FX: GLOBAL");
    else
        std::snprintf(title, sizeof(title), "VINTAGE FX: PAD %d", popup % 16 + 1);
    const auto caption = [&](const float x, const float y, const char* const label) {
        ctx.canvas.fontFace(NANOVG_DEJAVU_SANS_TTF);
        ctx.canvas.fontSize(12.0f);
        ctx.canvas.textAlign(DGL_NAMESPACE::NanoVG::ALIGN_LEFT |
                             DGL_NAMESPACE::NanoVG::ALIGN_MIDDLE);
        ctx.canvas.fillColor(colors.contentSecondary);
        text(ctx, x, y, label, nullptr);
    };
    caption(box.x + 16.0f, uiLayout::vintageClose.y + 12.0f, title);
    segment(ctx, uiLayout::vintageClose, "X", false, InteractiveType::vintageClose, 0);

    const int model = static_cast<int>(settings.model);
    for (int i = 0; i < 3; ++i)
        segment(ctx, uiLayout::vintageModel(i), vintageModelName(i), model == i,
                InteractiveType::vintageModel, i);
    if (model == 0) {
        caption(box.x + 16.0f, uiLayout::vintageKnob(0).y + 24.0f,
                "Choose a model to colour the sound.");
        return;
    }
    if (model == 1) {
        constexpr const char* voicings[]{"SWEEP", "DARK", "MID", "OPEN", "BRIGHT", "RAW"};
        for (int i = 0; i < sms::dsp::kVintageVoicingCount; ++i)
            segment(ctx, uiLayout::vintageVoicing(i), voicings[i],
                    static_cast<int>(settings.voicing) == i, InteractiveType::vintageVoicing, i);
    }
    constexpr const char* names[]{"BITS", "RATE", "DRIVE", "TONE", "MIX", "RESONANCE"};
    for (int knob = 0; knob < kVintageKnobCount; ++knob) {
        if (!vintageKnobShown(settings, knob))
            continue;
        char value[24];
        vintageKnobText(value, sizeof(value), settings, knob);
        drawMixerKnob(ctx, uiLayout::vintageKnob(knob), InteractiveType::vintageKnob, knob,
                      names[knob], vintageKnobFraction(settings, knob), 0.0f, 1.0f, value);
    }
}

} // namespace midichopper::ui::rendering
