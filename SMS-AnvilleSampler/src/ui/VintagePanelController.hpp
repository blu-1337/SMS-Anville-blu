#pragma once

// Input for the vintage FX popup. Pad clicks and knob gestures write one VF1 value (one
// undo step); the wheel writes a coalescing mixer edit; global edits persist directly.

#include "MidichopperInteraction.hpp"
#include "SamplerUiSession.hpp"

#include <chrono>

namespace midichopper::ui {

class VintagePanelController {
public:
    using Clock = std::chrono::steady_clock;
    explicit VintagePanelController(SamplerUiSession& session) noexcept : fSession(session) {}

    /** A press while the popup is open; the popup is modal, so it is always consumed. */
    void press(sms::ui::InteractiveTarget clicked, sms::ui::Point position, bool left,
               bool middle, KnobAdjustment adjustment, Clock::time_point now = Clock::now());
    void motion(sms::ui::Point position);
    /** Ends a knob drag, also after the popup closed under it. False without a drag. */
    bool release();
    void wheel(sms::ui::InteractiveTarget hovered, float delta, KnobAdjustment adjustment);
    [[nodiscard]] bool dragging() const noexcept { return fDragKnob >= 0; }

private:
    void commit(int target, const sms::dsp::VintageSettings& next);

    SamplerUiSession& fSession;
    DoubleClickTracker fDoubleClick;
    int fDragKnob = -1;
    int fDragPopup = -1;
    float fDragStartY = 0.0f;
    sms::dsp::VintageSettings fDragStart{};
    sms::dsp::VintageSettings fDragValue{};
    KnobAdjustment fDragAdjustment = KnobAdjustment::normal;
};

} // namespace midichopper::ui
