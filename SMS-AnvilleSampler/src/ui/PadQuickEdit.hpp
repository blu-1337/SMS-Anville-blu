#pragma once

#include "DSP/SampleMixerSettings.hpp"
#include "../plugin/PadQuickEditCodec.hpp"

#include <chrono>
#include <cstdio>
#include "UI/Geometry.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace midichopper::ui {

/** Drag on a Main-view pad: vertical sets volume, horizontal sets pan (one axis per drag). */
enum class PadQuickAxis { none, volume, pan, swap };

inline constexpr float kPadQuickDragThreshold = 7.0f; // logical px before a press becomes a drag
inline constexpr float kPadQuickPanDeadZone = 0.05f;  // snaps to centre near the middle

/** A press in the lower third of a pad starts a pad swap; the upper two-thirds keep volume/pan. */
[[nodiscard]] inline bool padSwapZone(const sms::ui::Rect& cell, const float y) noexcept
{
    return cell.height > 0.0f && y >= cell.y + cell.height * (2.0f / 3.0f) && y <= cell.y + cell.height;
}

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

/** Overlay text while dragging: volume -> "VOL 50%" + "-6.0 dB" (or "MUTE"); pan -> "50% L", "C", "30% R". */
inline void padQuickValueText(const PadQuickAxis axis, const float volume, const float pan,
                              char* const line1, const std::size_t size1, char* const line2,
                              const std::size_t size2) noexcept
{
    if (size1) line1[0] = '\0';
    if (size2) line2[0] = '\0';
    if (axis == PadQuickAxis::volume) {
        const float v = std::clamp(volume, 0.0f, 1.0f);
        std::snprintf(line1, size1, "VOL %d%%", static_cast<int>(std::lround(v * 100.0f)));
        if (v <= 0.001f) std::snprintf(line2, size2, "MUTE");
        else std::snprintf(line2, size2, "%.1f dB", static_cast<double>(padQuickGainDecibels(v)));
    } else if (axis == PadQuickAxis::pan) {
        const int percent = static_cast<int>(std::lround(std::fabs(std::clamp(pan, -1.0f, 1.0f)) * 100.0f));
        if (percent == 0) std::snprintf(line1, size1, "C");
        else std::snprintf(line1, size1, "%d%% %c", percent, pan < 0.0f ? 'L' : 'R');
    }
}

class PadQuickEdit {
public:
    void begin(const int pad, const sms::ui::Rect& cell, const sms::ui::Point start,
               const bool swapAllowed = false) noexcept
    {
        *this = PadQuickEdit{};
        fSwapZone = swapAllowed && padSwapZone(cell, start.y);
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
    [[nodiscard]] bool swapping() const noexcept { return fPad >= 0 && fAxis == PadQuickAxis::swap; }
    [[nodiscard]] sms::ui::Point point() const noexcept { return fPoint; }

    /** Returns true while the gesture owns the pointer (the axis is locked on the first move past the threshold). */
    using Clock = std::chrono::steady_clock;
    static constexpr auto kSendInterval = std::chrono::milliseconds(60);
    /** Phase to send now: 'B' on the first send, 'U' when 60 ms passed and the value moved, else 0. */
    [[nodiscard]] char due(const Clock::time_point now) noexcept
    {
        if (!dragging() || swapping()) return 0;
        const float value = fAxis == PadQuickAxis::volume ? fVolume : fPan;
        if (!fBegun) {
            fBegun = true;
            fLastSend = now;
            fSent = value;
            return 'B';
        }
        if (now - fLastSend < kSendInterval || std::fabs(value - fSent) < 0.002f) return 0;
        fLastSend = now;
        fSent = value;
        return 'U';
    }
    /** Phase for the release: 'E' after a begin, 'S' for a drag that never sent, else 0. */
    [[nodiscard]] char finish() const noexcept
    {
        return !dragging() || swapping() ? char{0} : (fBegun ? 'E' : 'S');
    }
    bool update(const sms::ui::Point point) noexcept
    {
        if (fPad < 0) return false;
        fPoint = point;
        if (fAxis == PadQuickAxis::none) {
            const float dx = point.x - fStart.x;
            const float dy = point.y - fStart.y;
            if (std::max(std::fabs(dx), std::fabs(dy)) < kPadQuickDragThreshold) return false;
            fAxis = fSwapZone ? PadQuickAxis::swap
                : std::fabs(dy) >= std::fabs(dx) ? PadQuickAxis::volume : PadQuickAxis::pan;
        }
        if (fAxis == PadQuickAxis::swap) return true;
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
    bool fBegun = false;
    bool fSwapZone = false;
    sms::ui::Point fPoint{};
    Clock::time_point fLastSend{};
    float fSent = 0.0f;
};

[[nodiscard]] inline std::string encodePadQuickEdit(const PadQuickAxis axis, const float volume,
                                                    const float pan, const char phase = 'S')
{
    plugin::PadQuickEditDelta edit;
    edit.hasGain = axis == PadQuickAxis::volume;
    edit.hasPan = axis == PadQuickAxis::pan;
    edit.gainDecibels = padQuickGainDecibels(volume);
    edit.pan = pan;
    edit.phase = phase;
    return plugin::encodePadQuickEdit(edit);
}

} // namespace midichopper::ui
