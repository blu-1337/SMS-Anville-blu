#pragma once

#include "UI/Geometry.hpp"
#include "ContextMenu.hpp"

#include <array>

namespace midichopper::ui::layout {

inline constexpr unsigned int canvasWidth = 1344U;
inline constexpr unsigned int canvasHeight = 756U;
inline constexpr unsigned int minimumWidth = 960U;
inline constexpr unsigned int minimumHeight = 540U;
inline constexpr float contentOffsetX = 40.0f;
inline constexpr sms::ui::Rect contentBounds{
    0.0f, 0.0f, static_cast<float>(canvasWidth) - contentOffsetX * 2.0f,
    static_cast<float>(canvasHeight)};

inline constexpr sms::ui::Rect inputMeter{6.0f, 140.0f, 44.0f, 476.0f};
inline constexpr sms::ui::Rect outputMeter{1294.0f, 140.0f, 44.0f, 476.0f};

/** Level controls use content coordinates for the shared input resolver. */
[[nodiscard]] constexpr sms::ui::Rect levelFader(const int index) noexcept
{
    const auto meter = index == 0 ? inputMeter : outputMeter;
    return {meter.x - contentOffsetX, meter.y, meter.width, meter.height};
}

[[nodiscard]] constexpr sms::ui::Rect levelValueLabel(const int index) noexcept
{
    const auto meter = levelFader(index);
    return {meter.x - 2.0f, meter.y + meter.height + 24.0f, meter.width + 4.0f, 18.0f};
}

[[nodiscard]] constexpr sms::ui::Rect levelFaderTrack(const sms::ui::Rect bounds) noexcept
{
    return {bounds.x + bounds.width * 0.5f - 3.0f, bounds.y + 17.0f,
            6.0f, bounds.height - 34.0f};
}

inline constexpr sms::ui::Rect mainPanel{24.0f, 96.0f, 934.0f, 596.0f};
inline constexpr sms::ui::Rect sidePanel{974.0f, 96.0f, 266.0f, 596.0f};
inline constexpr sms::ui::Rect menuButton{1208.0f, 25.0f, 32.0f, 32.0f};
// Undo / redo of pad and slice edits: a single pair at the right end of the Main Sample header
// button row (same y and height as PLAY / LOOP / - / + / FIT / SNAP), right-aligned to the
// waveform window's right edge (x 936).
inline constexpr sms::ui::Rect historyUndo{800.0f, 114.0f, 64.0f, 30.0f};
inline constexpr sms::ui::Rect historyRedo{872.0f, 114.0f, 64.0f, 30.0f};
enum class MenuSubmenu { none, padLayout, bankMode, skin };

inline constexpr sms::ui::Rect menuPanel{1048.0f, 68.0f, 192.0f, 298.0f};
inline constexpr sms::ui::Rect layoutMenuButton{1060.0f, 80.0f, 168.0f, 28.0f};
inline constexpr sms::ui::Rect bankModeMenuButton{1060.0f, 116.0f, 168.0f, 28.0f};
inline constexpr sms::ui::Rect skinMenuButton{1060.0f, 152.0f, 168.0f, 28.0f};

[[nodiscard]] constexpr sms::ui::Rect resetMenuButton(const bool hasSkins) noexcept
{
    return {1060.0f, hasSkins ? 188.0f : 152.0f, 168.0f, 28.0f};
}

[[nodiscard]] constexpr sms::ui::Rect mainSampleMenuButton(const bool hasSkins) noexcept
{
    return {1060.0f, hasSkins ? 224.0f : 188.0f, 168.0f, 28.0f};
}

[[nodiscard]] constexpr sms::ui::Rect menuPanelBounds(const bool hasSkins) noexcept
{
    auto bounds = menuPanel;
    if (!hasSkins) bounds.height -= 36.0f;
    return bounds;
}

[[nodiscard]] inline sms::ui::ContextMenuGeometry layoutMenuGeometry() noexcept
{
    return {{menuPanel.x - 240.0f + 2.0f, layoutMenuButton.y - 5.0f},
            3, contentBounds, 240.0f};
}

[[nodiscard]] inline sms::ui::ContextMenuGeometry bankModeMenuGeometry() noexcept
{
    return {{menuPanel.x - 240.0f + 2.0f, bankModeMenuButton.y - 5.0f},
            2, contentBounds, 240.0f};
}

// Pin Default at row zero and the download link below the scrolling choices.
inline constexpr int skinMenuMaximumChoices = 18;

[[nodiscard]] constexpr int skinMenuVisibleCount(const int total) noexcept
{
    return std::clamp(total, 0, skinMenuMaximumChoices);
}

[[nodiscard]] constexpr int skinMenuMaxOffset(const int total) noexcept
{
    return std::max(total - skinMenuVisibleCount(total), 0);
}

[[nodiscard]] constexpr int skinMenuEntry(const int row, const int offset) noexcept
{
    return row <= 0 ? row : row + offset;
}

[[nodiscard]] inline sms::ui::ContextMenuGeometry skinMenuGeometry(const int total) noexcept
{
    const int choices = skinMenuVisibleCount(total);
    std::array<sms::ui::ContextMenuItemKind, skinMenuMaximumChoices + 2> kinds{};
    kinds[static_cast<std::size_t>(choices)] = sms::ui::ContextMenuItemKind::separator;
    return {{menuPanel.x - 240.0f + 2.0f, skinMenuButton.y - 5.0f},
            std::span<const sms::ui::ContextMenuItemKind>{kinds.data(),
                static_cast<std::size_t>(choices + 2)}, contentBounds, 240.0f};
}

inline constexpr sms::ui::Rect footer{24.0f, 706.0f, 1216.0f, 32.0f};
inline constexpr sms::ui::Rect mainPadBounds{46.0f, 196.0f, 890.0f, 470.0f};
inline constexpr sms::ui::Rect overviewWaveform{46.0f, 176.0f, 890.0f, 10.0f};
inline constexpr sms::ui::Rect editorWaveform{74.0f, 158.0f, 862.0f, 208.0f};
inline constexpr sms::ui::Rect editorZoom{46.0f, 158.0f, 16.0f, 208.0f};
inline constexpr sms::ui::Rect editorScroll{74.0f, 378.0f, 862.0f, 12.0f};
inline constexpr sms::ui::Rect envelopeGraph{46.0f, 448.0f, 370.0f, 104.0f};
inline constexpr sms::ui::Rect editorPadBounds{994.0f, 204.0f, 222.0f, 406.0f};
inline constexpr sms::ui::Rect editorPlayStop{994.0f, 628.0f, 106.0f, 34.0f};
inline constexpr sms::ui::Rect playOnSelect{1106.0f, 628.0f, 110.0f, 34.0f};
inline constexpr sms::ui::Rect closeEditor{994.0f, 112.0f, 222.0f, 32.0f};
inline constexpr sms::ui::Rect openEditor{994.0f, 112.0f, 106.0f, 32.0f};
inline constexpr sms::ui::Rect openMainSample{1110.0f, 112.0f, 106.0f, 32.0f};
inline constexpr sms::ui::Rect chopWaveform{74.0f, 170.0f, 862.0f, 228.0f};
inline constexpr sms::ui::Rect chopZoom{46.0f, 170.0f, 16.0f, 228.0f};
inline constexpr sms::ui::Rect chopScroll{74.0f, 412.0f, 862.0f, 12.0f};
inline constexpr sms::ui::Rect chopAction{994.0f, 480.0f, 222.0f, 38.0f};
inline constexpr sms::ui::Rect chopPrevious{994.0f, 542.0f, 64.0f, 52.0f};
inline constexpr sms::ui::Rect chopExit{1074.0f, 534.0f, 64.0f, 64.0f};
inline constexpr sms::ui::Rect chopNext{1152.0f, 542.0f, 64.0f, 52.0f};

[[nodiscard]] constexpr bool chopExitContains(const sms::ui::Point point) noexcept
{
    const float centerX = chopExit.x + chopExit.width * 0.5f;
    const float centerY = chopExit.y + chopExit.height * 0.5f;
    const float dx = point.x - centerX;
    const float dy = point.y - centerY;
    const float radius = chopExit.width * 0.5f;
    return dx * dx + dy * dy <= radius * radius;
}

[[nodiscard]] constexpr float chopArrowTailX(const sms::ui::Rect bounds,
                                             const bool pointsRight) noexcept
{
    return bounds.x + bounds.width * 0.5f + (pointsRight ? -9.0f : 9.0f);
}

[[nodiscard]] constexpr float chopArrowTipX(const sms::ui::Rect bounds,
                                            const bool pointsRight) noexcept
{
    return bounds.x + bounds.width * 0.5f + (pointsRight ? 3.0f : -3.0f);
}
inline constexpr sms::ui::Rect modeToggle{994.0f, 154.0f, 222.0f, 42.0f};
inline constexpr sms::ui::Rect sequentialMode{994.0f, 220.0f, 106.0f, 38.0f};
inline constexpr sms::ui::Rect fixedMode{1108.0f, 220.0f, 108.0f, 38.0f};
inline constexpr sms::ui::Rect fixedLength{994.0f, 298.0f, 222.0f, 36.0f};
inline constexpr sms::ui::Rect oneShotMode{994.0f, 220.0f, 106.0f, 38.0f};
inline constexpr sms::ui::Rect holdMode{1108.0f, 220.0f, 108.0f, 38.0f};
inline constexpr sms::ui::Rect voiceLimit{994.0f, 298.0f, 222.0f, 28.0f};
inline constexpr sms::ui::Rect mainPlayStop{1049.0f, 342.0f, 112.0f, 32.0f};

[[nodiscard]] constexpr sms::ui::Rect preRoll(const bool fixedCapture) noexcept
{
    return {994.0f, fixedCapture ? 352.0f : 298.0f, 222.0f, 28.0f};
}

inline constexpr sms::ui::Rect monitor{994.0f, 651.0f, 222.0f, 34.0f};
inline constexpr float globalMixerLabelY = 386.0f;
inline constexpr sms::ui::Rect padMixerSeparator{354.0f, 586.0f, 1.0f, 77.0f};
inline constexpr sms::ui::Rect padGlobalMixerSeparator{854.0f, 586.0f, 1.0f, 77.0f};
inline constexpr sms::ui::Rect padGlobalMixer{879.0f, 604.0f, 42.0f, 34.0f};
// CHOKE and OUT chips on the Sample Editor heading line, well left of CLOSE (x 994): 74 units clear, 16 between the chips.
inline constexpr sms::ui::Rect padOutput{800.0f, 112.0f, 120.0f, 28.0f};
// HOLD-mode loop toggle, directly under the GLOBAL MIXER button (same x and width).
inline constexpr sms::ui::Rect padLoop{879.0f, 646.0f, 42.0f, 34.0f};
inline constexpr sms::ui::Rect padChoke{664.0f, 112.0f, 120.0f, 28.0f};
// Drop-down lists under the CHOKE (kind 1: Off, 1-8) and OUT (kind 2: Main, 1-16) chips.
inline constexpr float dropdownCellWidth = 56.0f;
inline constexpr float dropdownCellHeight = 24.0f;
inline constexpr float dropdownInset = 4.0f;
inline constexpr int dropdownColumns = 3;
[[nodiscard]] constexpr int dropdownCount(const int kind) noexcept { return kind == 1 ? 9 : 17; }
[[nodiscard]] constexpr sms::ui::Rect dropdownBox(const int kind) noexcept
{
    const sms::ui::Rect anchor = kind == 1 ? padChoke : padOutput;
    const int rows = (dropdownCount(kind) + dropdownColumns - 1) / dropdownColumns;
    return {anchor.x, anchor.y + anchor.height + 2.0f,
            dropdownColumns * dropdownCellWidth + 2.0f * dropdownInset,
            static_cast<float>(rows) * dropdownCellHeight + 2.0f * dropdownInset};
}
[[nodiscard]] constexpr sms::ui::Rect dropdownItem(const int kind, const int index) noexcept
{
    const auto box = dropdownBox(kind);
    return {box.x + dropdownInset + static_cast<float>(index % dropdownColumns) * dropdownCellWidth,
            box.y + dropdownInset + static_cast<float>(index / dropdownColumns) * dropdownCellHeight,
            dropdownCellWidth, dropdownCellHeight};
}
inline constexpr sms::ui::Rect padDirection{64.0f, 604.0f, 42.0f, 34.0f};
inline constexpr sms::ui::Rect globalDirection{1007.0f, 428.0f, 42.0f, 34.0f};
inline constexpr sms::ui::Rect finalizeAction{994.0f, 610.0f, 70.0f, 34.0f};
inline constexpr sms::ui::Rect undoAction{1070.0f, 610.0f, 70.0f, 34.0f};
inline constexpr sms::ui::Rect clearAction{1146.0f, 610.0f, 70.0f, 34.0f};

[[nodiscard]] constexpr sms::ui::Rect menuLinkOption(
    const int index, const bool hasSkins = true) noexcept
{
    return {1060.0f, (hasSkins ? 292.0f : 256.0f) +
            static_cast<float>(index) * 34.0f, 168.0f, 27.0f};
}

[[nodiscard]] constexpr sms::ui::Rect mainBank(const int index) noexcept
{
    return {80.0f + static_cast<float>(index) * 56.0f, 140.0f, 50.0f, 28.0f};
}

// Shown only while MIDI is set to "selected bank": notes past that bank's pads play nothing.
// Clicking it switches to All Banks (one continuous note range across banks A-D).
inline constexpr sms::ui::Rect midiBankHint{310.0f, 140.0f, 560.0f, 28.0f};

[[nodiscard]] constexpr sms::ui::Rect editorBank(const int index) noexcept
{
    return {994.0f + static_cast<float>(index) * 57.0f, 166.0f, 51.0f, 26.0f};
}

[[nodiscard]] constexpr sms::ui::Rect editorSlider(const int index) noexcept
{
    return {440.0f, 438.0f + static_cast<float>(index) * 29.0f, 472.0f, 24.0f};
}

[[nodiscard]] constexpr sms::ui::Rect mixerKnob(const int index) noexcept
{
    switch (index) {
    case 0: return {145.0f, 590.0f, 80.0f, 72.0f}; // Gain
    case 1: return {255.0f, 590.0f, 80.0f, 72.0f}; // Pan
    case 6: return {380.0f, 590.0f, 80.0f, 72.0f}; // Dirty
    case 2: return {500.0f, 590.0f, 80.0f, 72.0f}; // Tune
    case 3: return {630.0f, 590.0f, 80.0f, 72.0f}; // Lowpass
    case 4: return {760.0f, 590.0f, 80.0f, 72.0f}; // Highpass
    case 5: return {636.0f, 663.0f, 198.0f, 28.0f}; // Slope hit area
    default: return {};
    }
}

[[nodiscard]] constexpr sms::ui::Rect mixerValueLabel(const int index) noexcept
{
    const auto knob = mixerKnob(index);
    return {knob.x, knob.y + 52.0f, knob.width, 18.0f};
}

[[nodiscard]] constexpr sms::ui::Rect globalMixerKnob(const int index) noexcept
{
    switch (index) {
    case 1: return {1068.0f, 408.0f, 68.0f, 72.0f}; // Pan
    case 6: return {1142.0f, 408.0f, 68.0f, 72.0f}; // Dirty
    case 2: return {994.0f, 494.0f, 68.0f, 72.0f}; // Tune
    case 3: return {1068.0f, 494.0f, 68.0f, 72.0f}; // Lowpass
    case 4: return {1142.0f, 494.0f, 68.0f, 72.0f}; // Highpass
    case 5: return {1068.0f, 570.0f, 142.0f, 28.0f}; // Slope hit area
    default: return {};
    }
}

[[nodiscard]] constexpr sms::ui::Rect globalMixerValueLabel(const int index) noexcept
{
    const auto knob = globalMixerKnob(index);
    return {knob.x, knob.y + 52.0f, knob.width, 18.0f};
}

[[nodiscard]] constexpr sms::ui::Rect slopeTrack(const sms::ui::Rect bounds) noexcept
{
    return {bounds.x + 10.0f, bounds.y + 14.0f, bounds.width - 20.0f, 6.0f};
}

[[nodiscard]] constexpr sms::ui::Rect chopPadButton(const int index) noexcept
{
    return {46.0f + static_cast<float>(index) * 300.0f, 450.0f, 290.0f, 82.0f};
}

// Main Sample view (Docs/AI/MAIN-SAMPLE-UI.md). The side panel keeps y 154-380
// free for the auto-chop panel.
inline constexpr sms::ui::Rect mainSampleWaveform{46.0f, 158.0f, 890.0f, 208.0f};
// Slice-number badges at the top of each region (half the former 18x16 tab).
inline constexpr float sliceBadgeWidth = 10.0f;
inline constexpr float sliceBadgeWideWidth = 14.0f; // two-digit numbers
inline constexpr float sliceBadgeHeight = 9.0f;
inline constexpr sms::ui::Rect mainSampleOverview{46.0f, 374.0f, 890.0f, 20.0f};
inline constexpr sms::ui::Rect mainSamplePads{46.0f, 402.0f, 890.0f, 264.0f};
// A-D bank row at the foot of the side panel, below the slice tools (they end at y 626).
[[nodiscard]] constexpr sms::ui::Rect mainSampleBank(const int index) noexcept
{
    return {994.0f + static_cast<float>(index) * 57.0f, 642.0f, 51.0f, 28.0f};
}
inline constexpr sms::ui::Rect mainSampleClose{994.0f, 112.0f, 222.0f, 32.0f};
inline constexpr sms::ui::Rect mainSampleInfo{994.0f, 392.0f, 222.0f, 60.0f};
inline constexpr sms::ui::Rect mainSampleLoad{994.0f, 462.0f, 104.0f, 32.0f};
inline constexpr sms::ui::Rect mainSampleClear{1112.0f, 462.0f, 104.0f, 32.0f};
inline constexpr sms::ui::Rect mainSampleStatus{994.0f, 506.0f, 222.0f, 40.0f};
// Slice tools below the status text: 0 ARRANGE (full width), 1 AUTO COLOR, 2 CLEAR COLORS.
// Assigning is done by clicking a pad, so there is no ASSIGN button.
inline constexpr int kMainSampleActionCount = 3;
[[nodiscard]] constexpr sms::ui::Rect mainSampleAction(const int index) noexcept
{
    return index == 0 ? sms::ui::Rect{994.0f, 554.0f, 222.0f, 32.0f}
                      : sms::ui::Rect{994.0f + static_cast<float>(index - 1) * 114.0f, 594.0f, 108.0f, 32.0f};
}
inline constexpr sms::ui::Rect mainSampleModal{300.0f, 270.0f, 380.0f, 150.0f};
inline constexpr sms::ui::Rect mainSampleConfirm{330.0f, 366.0f, 150.0f, 34.0f};
inline constexpr sms::ui::Rect mainSampleCancel{500.0f, 366.0f, 150.0f, 34.0f};
// Transport and zoom buttons in the header row: play/stop, loop, zoom out, zoom in, fit.
// Index 5 is the SNAP on/off toggle for edge and selection drags.
inline constexpr std::array<sms::ui::Rect, 6> mainSampleTransport{{
    {190.0f, 114.0f, 84.0f, 30.0f}, {282.0f, 114.0f, 64.0f, 30.0f}, {366.0f, 114.0f, 36.0f, 30.0f},
    {408.0f, 114.0f, 36.0f, 30.0f}, {450.0f, 114.0f, 52.0f, 30.0f}, {510.0f, 114.0f, 56.0f, 30.0f}}};
// Auto chop panel in the free column right of the waveform.
inline constexpr sms::ui::Rect autoChopMode{994.0f, 166.0f, 222.0f, 28.0f};
inline constexpr sms::ui::Rect autoChopCountMinus{994.0f, 202.0f, 44.0f, 28.0f};
// Double-click the count between - and + to type it.
inline constexpr sms::ui::Rect autoChopCountField{1042.0f, 202.0f, 126.0f, 28.0f};
inline constexpr sms::ui::Rect autoChopCountPlus{1172.0f, 202.0f, 44.0f, 28.0f};
inline constexpr sms::ui::Rect autoChopPadMinus{994.0f, 238.0f, 44.0f, 28.0f};
inline constexpr sms::ui::Rect autoChopPadPlus{1172.0f, 238.0f, 44.0f, 28.0f};
inline constexpr sms::ui::Rect autoChopApply{994.0f, 274.0f, 222.0f, 32.0f};
inline constexpr sms::ui::Rect autoChopMessage{994.0f, 312.0f, 222.0f, 48.0f};
inline constexpr std::array<sms::ui::Rect, 8> mainSampleRects{
    mainSampleWaveform, mainSampleOverview, mainSamplePads, mainSampleClose,
    mainSampleInfo, mainSampleLoad, mainSampleClear, mainSampleStatus};

} // namespace midichopper::ui::layout
