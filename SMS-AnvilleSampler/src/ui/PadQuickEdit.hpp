#pragma once

#include "DSP/SampleMixerSettings.hpp"
#include "../plugin/PadQuickEditCodec.hpp"
#include "UI/Geometry.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace midichopper::ui {

/** Drag on a Main-view pad: vertical sets volume, horizontal sets pan (one axis per drag). */
enum class PadQuickAxis { none, volume, pan };

inline constexpr float kPadQuickDragThreshold = 7.0f; // logical px before a press becomes a drag
inline constexpr float kPadQuickPanDeadZone = 0.05f;  // snaps to centre near the middle

/** Top of the pad = full level, bottom = muted. Linear amplitude, so the middle is about -6 dB. */
[[nodiscard]] inline float padQuickVolumeFromY(const float y, const sms::ui::Rect& cell) noexcept
{
    if (!(cell.height > 0.0f)) return 1.0f;
    return std::clamp((cell.y + cell.height - y) / cell.height, 0.0f, 1.0f);
}

[[nodiscard]] inline float padQuickGainDecibels(const float volume) noexcept
{
    if (!(volume > 0.001f)) return sms::dsp::kMinimumSampleGainDecibels;
    return std::clamp(20.0f * std::log10(std::min(volume, 1.0f)),
                      sms::dsp::kMinimumSampleGainDecibels, 0.0f);
}

[[nodiscard]] inline float padQuickPanFromX(const float x, const sms::ui::Rect& cell) noexcept
{
    if (!(cell.width > 0.0f)) return 0.0f;
    const float pan = std::clamp((x - (cell.x + cell.width * 0.5f)) / (cell.width * 0.5f), -1.0f, 1.0f);
    return std::fabs(pan) < kPadQuickPanDeadZone ? 0.0f : pan;
}

class PadQuickEdit {
public:
    void begin(const int pad, const sms::ui::Rect& cell, const sms::ui::Point start) noexcept
    {
        *this = PadQuickEdit{};
        fPad = pad;
        fCell = cell;
        fStart = start;
    }
    void cancel() noexcept { *this = PadQuickEdit{}; }
    [[nodiscard]] bool pending() const noexcept { return fPad >= 0; }
    [[nodiscard]] bool dragging() const noexcept { return fPad >= 0 && fAxis != PadQuickAxis::none; }
    [[nodiscard]] int pad() const noexcept { return fPad; }
    [[nodiscard]] PadQuickAxis axis() const noexcept { return fAxis; }
    [[nodiscard]] float volume() const noexcept { return fVolume; }
    [[nodiscard]] float pan() const noexcept { return fPan; }

    /** Returns true while the gesture owns the pointer (the axis is locked on the first move past the threshold). */
    bool update(const sms::ui::Point point) noexcept
    {
        if (fPad < 0) return false;
        if (fAxis == PadQuickAxis::none) {
            const float dx = point.x - fStart.x;
            const float dy = point.y - fStart.y;
            if (std::max(std::fabs(dx), std::fabs(dy)) < kPadQuickDragThreshold) return false;
            fAxis = std::fabs(dy) >= std::fabs(dx) ? PadQuickAxis::volume : PadQuickAxis::pan;
        }
        if (fAxis == PadQuickAxis::volume) fVolume = padQuickVolumeFromY(point.y, fCell);
        else fPan = padQuickPanFromX(point.x, fCell);
        return true;
    }

private:
    int fPad = -1;
    sms::ui::Rect fCell{};
    sms::ui::Point fStart{};
    PadQuickAxis fAxis = PadQuickAxis::none;
    float fVolume = 1.0f;
    float fPan = 0.0f;
};

[[nodiscard]] inline std::string encodePadQuickEdit(const PadQuickAxis axis, const float volume,
                                                    const float pan)
{
    plugin::PadQuickEditDelta edit;
    edit.hasGain = axis == PadQuickAxis::volume;
    edit.hasPan = axis == PadQuickAxis::pan;
    edit.gainDecibels = padQuickGainDecibels(volume);
    edit.pan = pan;
    return plugin::encodePadQuickEdit(edit);
}

} // namespace midichopper::ui
