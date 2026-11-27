#include "VintagePanelController.hpp"

namespace midichopper::ui {

void VintagePanelController::commit(const int target, const sms::dsp::VintageSettings& next)
{
    if (next != fSession.vintageSettings(target))
        fSession.commitVintage(target, next);
}

void VintagePanelController::press(const sms::ui::InteractiveTarget clicked,
    const sms::ui::Point position, const bool left, const bool middle,
    const KnobAdjustment adjustment, const Clock::time_point now)
{
    const int popup = fSession.vintagePopup();
    const auto milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    const bool knob = isTarget(clicked, InteractiveType::vintageKnob) ||
                      isTarget(clicked, InteractiveType::vintageValueLabel);
    // Outside, the close button and a right-click dismiss without click-through.
    if (!clicked.valid() || isTarget(clicked, InteractiveType::vintageClose) || (!left && !middle)) {
        fSession.closeVintage();
        return;
    }
    if (!fSession.vintageReady(popup))
        return;
    const auto current = fSession.vintageSettings(popup);
    if (knob && (middle || fDoubleClick.press(clicked, position, milliseconds))) {
        fDragKnob = -1;
        commit(popup, vintageWithDefault(current, clicked.index));
        return;
    }
    if (!left)
        return;
    if (knob) {
        fDragKnob = clicked.index;
        fDragPopup = popup;
        fDragStartY = position.y;
        fDragStart = fDragValue = current;
        fDragAdjustment = adjustment;
    } else if (isTarget(clicked, InteractiveType::vintageModel)) {
        commit(popup, vintageWithModel(current, clicked.index));
    } else if (isTarget(clicked, InteractiveType::vintageVoicing)) {
        auto next = current;
        next.voicing = static_cast<float>(clicked.index);
        commit(popup, next);
    }
}

void VintagePanelController::motion(const sms::ui::Point position)
{
    if (fDragKnob < 0 || fSession.vintagePopup() != fDragPopup)
        return;
    const float travel = fDragAdjustment == KnobAdjustment::fine ? 0.0012f : 0.006f;
    fDragValue = vintageWithKnob(fDragStart, fDragKnob,
        vintageKnobFraction(fDragStart, fDragKnob) + (fDragStartY - position.y) * travel);
    fSession.previewVintage(fDragPopup, fDragValue);
}

bool VintagePanelController::release()
{
    if (fDragKnob < 0)
        return false;
    fDragKnob = -1;
    if (fDragValue != fDragStart)
        fSession.commitVintage(fDragPopup, fDragValue);
    return true;
}

void VintagePanelController::wheel(const sms::ui::InteractiveTarget hovered, const float delta,
                                   const KnobAdjustment adjustment)
{
    const int popup = fSession.vintagePopup();
    if (delta == 0.0f || !std::isfinite(delta) || !fSession.vintageReady(popup) ||
        !(isTarget(hovered, InteractiveType::vintageKnob) ||
          isTarget(hovered, InteractiveType::vintageValueLabel)))
        return;
    const auto current = fSession.vintageSettings(popup);
    const auto next = vintageStepped(current, hovered.index, delta,
                                     adjustment == KnobAdjustment::fine);
    if (next != current)
        fSession.adjustVintage(popup, next);
}

} // namespace midichopper::ui
