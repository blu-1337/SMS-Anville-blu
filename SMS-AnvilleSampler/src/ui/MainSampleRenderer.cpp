#include "ViewRenderers.hpp"
#include "PadViewMapping.hpp"
#include "DPF/AnalogMaterials.hpp"
#include "DPF/Controls.hpp"
#include "DPF/WaveformRenderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace midichopper::ui::rendering {

namespace uiLayout = layout;

namespace {

constexpr int kMaxColumns = 512;
constexpr std::size_t kMaxMarkers = 64U;
constexpr std::uint64_t kBinsPerTile = sms::audio::kWaveformBins;

/** Extrema of source frames [first, last) in one reply; false when it does not cover them. */
bool extrema(const MainSampleWaveformView& view, const std::uint64_t first,
             const std::uint64_t last, float& low, float& high) noexcept
{
    const auto& waveform = view.waveform;
    if (!view.ready || waveform.count == 0U || waveform.end <= waveform.start ||
        first < waveform.start || last > waveform.end || last <= first)
        return false;
    const std::uint64_t length = waveform.end - waveform.start;
    const std::uint64_t bins = static_cast<std::uint64_t>(waveform.count) * kBinsPerTile;
    const std::uint64_t firstBin = (first - waveform.start) * bins / length;
    const std::uint64_t lastBin = std::max(firstBin,
        std::min(bins - 1U, ((last - waveform.start) * bins - 1U) / length));
    low = 1.0f;
    high = -1.0f;
    for (std::uint64_t bin = firstBin; bin <= lastBin; ++bin) {
        const auto& tile = waveform.tiles[static_cast<std::size_t>(bin / kBinsPerTile)];
        const auto index = static_cast<std::size_t>(bin % kBinsPerTile);
        low = std::min(low, tile.minimum[index]);
        high = std::max(high, tile.maximum[index]);
    }
    return low <= high;
}

float overviewPeak(const MainSampleWaveformView& view) noexcept
{
    float peak = 0.0f;
    for (std::uint32_t tile = 0; view.ready && tile < view.waveform.count &&
         tile < plugin::kMainSampleMaxTiles; ++tile)
        for (std::size_t bin = 0; bin < sms::audio::kWaveformBins; ++bin)
            peak = std::max({peak, std::abs(view.waveform.tiles[tile].minimum[bin]),
                             std::abs(view.waveform.tiles[tile].maximum[bin])});
    return peak;
}

/** One stroked path of vertical column lines over frames [start, end). */
void drawColumns(const ViewRenderContext& ctx, const MainSampleController& main,
                 const sms::ui::Rect bounds, const std::uint64_t start, const std::uint64_t end,
                 const bool useDetail, const float scale, const float amplitude,
                 const DGL_NAMESPACE::Color& color, const float lineWidth)
{
    if (end <= start || bounds.width < 1.0f)
        return;
    const int columns = std::clamp(static_cast<int>(bounds.width), 1, kMaxColumns);
    const std::uint64_t length = end - start;
    const float middle = bounds.y + bounds.height * 0.5f;
    ctx.canvas.beginPath();
    for (int column = 0; column < columns; ++column) {
        const auto first = start + length * static_cast<std::uint64_t>(column) /
                                       static_cast<std::uint64_t>(columns);
        const auto last = std::max(first + 1U, start + length * static_cast<std::uint64_t>(column + 1) /
                                                          static_cast<std::uint64_t>(columns));
        float low = 0.0f;
        float high = 0.0f;
        const bool found = (useDetail && extrema(main.detail(), first, last, low, high)) ||
                           extrema(main.overview(), first, last, low, high);
        if (!found)
            continue;
        const float x = bounds.x + (static_cast<float>(column) + 0.5f) * bounds.width /
                                       static_cast<float>(columns);
        ctx.canvas.moveTo(x, middle - high * scale * amplitude);
        ctx.canvas.lineTo(x, middle - low * scale * amplitude);
    }
    ctx.canvas.strokeColor(color);
    ctx.canvas.strokeWidth(lineWidth);
    ctx.canvas.stroke();
}

const DGL_NAMESPACE::Color* padTint(const ViewRenderContext& ctx, const int pad) noexcept
{
    const auto& colors = sms::ui::dpf::theme();
    if (pad < 0 || pad >= static_cast<int>(ctx.state.padColors.size()))
        return nullptr;
    const int color = ctx.state.padColors[static_cast<std::size_t>(pad)];
    return color > 0 && color <= static_cast<int>(colors.padColors.size())
        ? &colors.padColors[static_cast<std::size_t>(color - 1)].tint : nullptr;
}

/** Link of a slice to the lowest pad that plays it, or -1. */
int padForSlice(const MainSampleController& main, const std::uint16_t sliceId) noexcept
{
    int best = -1;
    for (const auto& link : main.status().links)
        if (link.sliceId == sliceId && (best < 0 || static_cast<int>(link.pad) < best))
            best = static_cast<int>(link.pad);
    return best;
}

/** Zone colour of a slice: its pad's colour, else a spread palette colour by time rank. */
const DGL_NAMESPACE::Color* zoneTint(const ViewRenderContext& ctx, const MainSampleController& main,
                                     const std::size_t index) noexcept
{
    const auto& slices = main.displaySlices();
    if (const auto* tint = padTint(ctx, padForSlice(main, slices[index].id)); tint != nullptr)
        return tint;
    if (main.neutralZones())
        return nullptr;
    const auto& palette = sms::ui::dpf::theme().padColors;
    const int color = MainSampleController::spreadColor(index, palette.size(), main.colorPress());
    return color > 0 ? &palette[static_cast<std::size_t>(color - 1)].tint : nullptr;
}

DGL_NAMESPACE::Color darker(const DGL_NAMESPACE::Color& color) noexcept
{
    return DGL_NAMESPACE::Color(color.red * 0.55f, color.green * 0.55f, color.blue * 0.55f, 1.0f);
}

/** Slice-number tab next to a marker line; start tabs sit right of it, end tabs left, darker. */
void drawMarkerTab(const ViewRenderContext& ctx, const float x, const sms::ui::Rect& bounds,
                   const unsigned number, const bool endEdge, const bool isHovered,
                   const DGL_NAMESPACE::Color& color)
{
    const auto& colors = sms::ui::dpf::theme();
    const float width = isHovered ? 22.0f : 18.0f;
    const float left = endEdge ? x - width : x;
    ctx.canvas.beginPath();
    ctx.canvas.roundedRect(left, bounds.y + 6.0f, width, isHovered ? 20.0f : 16.0f, 3.0f);
    ctx.canvas.fillColor(endEdge ? darker(color) : color);
    ctx.canvas.fill();
    char label[8];
    std::snprintf(label, sizeof(label), "%u", number);
    ctx.canvas.fontFace(NANOVG_DEJAVU_SANS_TTF);
    ctx.canvas.fontSize(9.0f);
    ctx.canvas.textAlign(DGL_NAMESPACE::NanoVG::ALIGN_CENTER | DGL_NAMESPACE::NanoVG::ALIGN_MIDDLE);
    ctx.canvas.fillColor(endEdge ? colors.contentPrimary : colors.canvas);
    text(ctx, left + width * 0.5f, bounds.y + 14.0f + (isHovered ? 2.0f : 0.0f), label, nullptr);
}

void drawWaveformArea(const ViewRenderContext& ctx, const MainSampleController& main)
{
    const auto& colors = sms::ui::dpf::theme();
    const auto bounds = uiLayout::mainSampleWaveform;
    sms::ui::dpf::drawInsetSurface(ctx.canvas, bounds, 8.0f);
    ctx.canvas.fontFace(NANOVG_DEJAVU_SANS_TTF);
    if (!main.hasSource()) {
        ctx.canvas.fontSize(14.0f);
        ctx.canvas.textAlign(DGL_NAMESPACE::NanoVG::ALIGN_CENTER |
                             DGL_NAMESPACE::NanoVG::ALIGN_MIDDLE);
        ctx.canvas.fillColor(colors.contentSecondary);
        text(ctx, bounds.x + bounds.width * 0.5f, bounds.y + bounds.height * 0.5f,
             main.status().state == plugin::MainSampleState::loading ? "LOADING..."
                                                                      : "LOAD A SAMPLE", nullptr);
        return;
    }
    const auto viewport = main.viewport();
    ctx.canvas.save();
    ctx.canvas.scissor(bounds.x, bounds.y, bounds.width, bounds.height);

    const float peak = overviewPeak(main.overview());
    const float scale = peak > 0.001f ? std::min(1.0f / peak, 8.0f) : 1.0f;

    // Slice backgrounds in the colour of the linked pad.
    const auto& slices = main.displaySlices();
    const std::size_t markerCount = std::min(slices.size(), kMaxMarkers);
    for (std::size_t index = 0; index < markerCount; ++index) {
        const auto& slice = slices[index];
        const float x0 = std::max(bounds.x, viewport.xForFrame(slice.start, bounds));
        const float x1 = std::min(bounds.x + bounds.width, viewport.xForFrame(slice.end, bounds));
        if (x1 <= x0)
            continue;
        const auto* tint = zoneTint(ctx, main, index);
        ctx.canvas.beginPath();
        ctx.canvas.rect(x0, bounds.y, x1 - x0, bounds.height);
        ctx.canvas.fillColor((tint != nullptr ? *tint : colors.selection)
            .withAlpha(slice.id == main.selectedSlice() ? 0.42f : 0.20f));
        ctx.canvas.fill();
        if (slice.id == main.selectedSlice()) {
            ctx.canvas.beginPath();
            ctx.canvas.rect(x0, bounds.y + 1.0f, x1 - x0, bounds.height - 2.0f);
            ctx.canvas.strokeColor((tint != nullptr ? *tint : colors.selection).withAlpha(0.9f));
            ctx.canvas.strokeWidth(2.0f);
            ctx.canvas.stroke();
        }
    }

    ctx.canvas.beginPath();
    ctx.canvas.moveTo(bounds.x, bounds.y + bounds.height * 0.5f);
    ctx.canvas.lineTo(bounds.x + bounds.width, bounds.y + bounds.height * 0.5f);
    ctx.canvas.strokeColor(colors.outline.withAlpha(0.7f));
    ctx.canvas.strokeWidth(colors.outlineWidth);
    ctx.canvas.stroke();
    drawColumns(ctx, main, bounds, viewport.start, viewport.end, viewport.zoomed(), scale,
                bounds.height * 0.44f, colors.activityPlayback, 1.6f);

    if (main.hasSelection()) {
        const float x0 = std::max(bounds.x, viewport.xForFrame(main.selectionStart(), bounds));
        const float x1 = std::min(bounds.x + bounds.width,
                                  viewport.xForFrame(main.selectionEnd(), bounds));
        if (x1 > x0) {
            ctx.canvas.beginPath();
            ctx.canvas.rect(x0, bounds.y, x1 - x0, bounds.height);
            ctx.canvas.fillColor(colors.selection.withAlpha(0.26f));
            ctx.canvas.fill();
        }
    }
    ctx.canvas.resetScissor();

    // Markers: one per distinct boundary. Wide spacing gets the labelled handle; crowded
    // spacing shrinks to a thin line, with the slice number only where it fits.
    struct Marker {
        float x;
        int slice;
        int edge;
        int endSlice; // slice whose end coincides with a start marker, or -1
    };
    std::array<Marker, kMaxMarkers * 2U> markers{};
    std::size_t markerTotal = 0U;
    for (std::size_t index = 0; index < markerCount; ++index) {
        const auto& slice = slices[index];
        for (int edge = 0; edge < 2; ++edge) {
            const auto frame = edge == 0 ? slice.start : slice.end;
            bool shared = false;
            for (std::size_t other = 0; edge == 1 && other < markerCount; ++other)
                shared = shared || slices[other].start == frame;
            if (shared || frame < viewport.start || frame > viewport.end)
                continue;
            int endSlice = -1;
            for (std::size_t other = 0; edge == 0 && other < markerCount; ++other)
                if (slices[other].end == frame && endSlice < 0)
                    endSlice = static_cast<int>(other);
            markers[markerTotal++] = {viewport.xForFrame(static_cast<double>(frame), bounds),
                                      static_cast<int>(index), edge, endSlice};
        }
    }
    std::sort(markers.begin(), markers.begin() + static_cast<std::ptrdiff_t>(markerTotal),
              [](const Marker& left, const Marker& right) { return left.x < right.x; });
    for (std::size_t order = 0; order < markerTotal; ++order) {
        const auto& marker = markers[order];
        float room = bounds.width;
        if (order > 0U)
            room = std::min(room, marker.x - markers[order - 1U].x);
        if (order + 1U < markerTotal)
            room = std::min(room, markers[order + 1U].x - marker.x);
        const bool isHovered =
            hovered(ctx, InteractiveType::mainSampleMarker, marker.slice * 2 + marker.edge);
        const auto sliceColor = [&](const int slice) {
            const auto* tint = zoneTint(ctx, main, static_cast<std::size_t>(slice));
            return tint != nullptr ? *tint : colors.selection;
        };
        if (room >= 38.0f) {
            const auto lineColor = sliceColor(marker.slice);
            ctx.canvas.beginPath();
            ctx.canvas.moveTo(marker.x, bounds.y);
            ctx.canvas.lineTo(marker.x, bounds.y + bounds.height);
            ctx.canvas.strokeColor(marker.edge == 1 ? darker(lineColor) : lineColor);
            ctx.canvas.strokeWidth(isHovered ? 4.0f : 2.0f);
            ctx.canvas.stroke();
            if (marker.endSlice >= 0)
                drawMarkerTab(ctx, marker.x, bounds, static_cast<unsigned>(marker.endSlice + 1), true,
                              false, sliceColor(marker.endSlice));
            drawMarkerTab(ctx, marker.x, bounds, static_cast<unsigned>(marker.slice + 1), marker.edge == 1,
                          isHovered, lineColor);
            continue;
        }
        ctx.canvas.beginPath();
        ctx.canvas.moveTo(marker.x, bounds.y);
        ctx.canvas.lineTo(marker.x, bounds.y + bounds.height);
        ctx.canvas.strokeColor(marker.edge == 1 ? darker(sliceColor(marker.slice)) : sliceColor(marker.slice));
        ctx.canvas.strokeWidth(isHovered ? 3.0f : 1.0f);
        ctx.canvas.stroke();
        if (marker.edge == 0 && (room >= 16.0f || isHovered)) {
            char label[8];
            std::snprintf(label, sizeof(label), "%u", static_cast<unsigned>(marker.slice + 1));
            ctx.canvas.fontFace(NANOVG_DEJAVU_SANS_TTF);
            ctx.canvas.fontSize(8.0f);
            ctx.canvas.textAlign(DGL_NAMESPACE::NanoVG::ALIGN_CENTER | DGL_NAMESPACE::NanoVG::ALIGN_TOP);
            ctx.canvas.fillColor(sliceColor(marker.slice));
            text(ctx, marker.x, bounds.y + 3.0f, label, nullptr);
        }
    }

    if (main.previewPosition() > 0.0f && main.status().frames != 0U) {
        const double frame = std::clamp(static_cast<double>(main.previewPosition() - 1.0f), 0.0, 1.0) *
                             main.status().frames;
        if (frame >= viewport.start && frame <= viewport.end) {
            const float x = viewport.xForFrame(frame, bounds);
            ctx.canvas.beginPath();
            ctx.canvas.moveTo(x, bounds.y);
            ctx.canvas.lineTo(x, bounds.y + bounds.height);
            ctx.canvas.strokeColor(colors.contentPrimary);
            ctx.canvas.strokeWidth(1.5f);
            ctx.canvas.stroke();
        }
    }
    sms::ui::dpf::drawWaveformViewportLabel(ctx.canvas, bounds, viewport,
                                            static_cast<double>(main.status().sampleRate));
}

void drawOverview(const ViewRenderContext& ctx, const MainSampleController& main)
{
    const auto& colors = sms::ui::dpf::theme();
    const auto bounds = uiLayout::mainSampleOverview;
    sms::ui::dpf::drawInsetSurface(ctx.canvas, bounds, 4.0f);
    if (!main.hasSource())
        return;
    ctx.canvas.save();
    ctx.canvas.scissor(bounds.x, bounds.y, bounds.width, bounds.height);
    const float peak = overviewPeak(main.overview());
    drawColumns(ctx, main, bounds, 0U, main.status().frames, false,
                peak > 0.001f ? std::min(1.0f / peak, 8.0f) : 1.0f, bounds.height * 0.45f,
                colors.contentSecondary.withAlpha(0.7f), 1.2f);
    ctx.canvas.resetScissor();
    const auto& viewport = main.viewport();
    const double frames = static_cast<double>(main.status().frames);
    if (viewport.zoomed() && frames > 0.0) {
        const float x0 = bounds.x + bounds.width * static_cast<float>(viewport.start / frames);
        const float x1 = bounds.x + bounds.width * static_cast<float>(viewport.end / frames);
        ctx.canvas.beginPath();
        ctx.canvas.rect(x0, bounds.y + 1.0f, std::max(2.0f, x1 - x0), bounds.height - 2.0f);
        ctx.canvas.fillColor(colors.selection.withAlpha(0.16f));
        ctx.canvas.fill();
        ctx.canvas.strokeColor(colors.selection);
        ctx.canvas.strokeWidth(hovered(ctx, InteractiveType::mainSampleOverview) ? 2.0f : 1.2f);
        ctx.canvas.stroke();
    }
    if (main.previewPosition() > 0.0f) {
        const float x = bounds.x + bounds.width *
            std::clamp(main.previewPosition() - 1.0f, 0.0f, 1.0f);
        ctx.canvas.beginPath();
        ctx.canvas.moveTo(x, bounds.y);
        ctx.canvas.lineTo(x, bounds.y + bounds.height);
        ctx.canvas.strokeColor(colors.contentPrimary);
        ctx.canvas.strokeWidth(1.2f);
        ctx.canvas.stroke();
    }
}

void drawPads(const ViewRenderContext& ctx, const MainSampleController& main)
{
    const sms::ui::BankedPadLayout pads(ctx.state.layout);
    const auto& colors = sms::ui::dpf::theme();
    const auto grid = pads.grid(uiLayout::mainSamplePads, 8.0f);
    for (int localPad = 0; localPad < pads.visiblePadCount(); ++localPad) {
        const int pad = globalPad(ctx, localPad);
        const auto cell = grid.cell(pads.visualIndex(localPad));
        const bool occupied = ctx.state.padState[static_cast<std::size_t>(localPad)] != '0' &&
                              ctx.state.padState[static_cast<std::size_t>(localPad)] != '.';
        const bool playing = ctx.state.padActivity[static_cast<std::size_t>(localPad)] != '0' ||
                             ctx.state.pressedPad == localPad;
        const bool padHovered = hovered(ctx, InteractiveType::mainSamplePad, localPad);
        sms::ui::dpf::drawRubberPad(
            ctx.canvas, cell,
            {occupied, playing, false, ctx.state.selectedPad == pad, padHovered,
             ctx.state.pressedPad == localPad, true, padTint(ctx, pad)});
        ctx.canvas.fontFace(NANOVG_DEJAVU_SANS_TTF);
        ctx.canvas.fontSize(11.0f);
        ctx.canvas.textAlign(DGL_NAMESPACE::NanoVG::ALIGN_LEFT | DGL_NAMESPACE::NanoVG::ALIGN_TOP);
        ctx.canvas.fillColor(padHovered ? colors.selection : colors.contentSecondary);
        char label[12];
        std::snprintf(label, sizeof(label), "%02d", localPad + 1);
        text(ctx, cell.x + 10.0f, cell.y + 8.0f, label, nullptr);
        const int slice = main.sliceForPad(static_cast<std::uint32_t>(pad));
        if (slice >= 0) {
            char number[16];
            std::snprintf(number, sizeof(number), "S%d", slice + 1);
            ctx.canvas.fontSize(20.0f);
            ctx.canvas.textAlign(DGL_NAMESPACE::NanoVG::ALIGN_CENTER |
                                 DGL_NAMESPACE::NanoVG::ALIGN_MIDDLE);
            ctx.canvas.fillColor(colors.contentPrimary);
            text(ctx, cell.x + cell.width * 0.5f, cell.y + cell.height * 0.54f, number, nullptr);
        }
    }
}

void drawSidePanel(const ViewRenderContext& ctx, const MainSampleController& main)
{
    const auto& colors = sms::ui::dpf::theme();
    sms::ui::dpf::drawSegment(ctx.canvas, uiLayout::mainSampleClose, "CLOSE", false,
                              colors.controlAccent,
                              hovered(ctx, InteractiveType::mainSampleClose));
    ctx.canvas.fontFace(NANOVG_DEJAVU_SANS_TTF);
    ctx.canvas.fontSize(10.0f);
    ctx.canvas.textAlign(DGL_NAMESPACE::NanoVG::ALIGN_LEFT | DGL_NAMESPACE::NanoVG::ALIGN_TOP);
    ctx.canvas.fillColor(colors.contentSecondary);
    for (int bank = 0; bank < static_cast<int>(kBankCount); ++bank) {
        char label[4];
        std::snprintf(label, sizeof(label), "%c", 'A' + bank);
        sms::ui::dpf::drawSegment(ctx.canvas, uiLayout::mainSampleBank(bank), label,
                                  bank == ctx.state.bank, colors.activityPlayback,
                                  hovered(ctx, InteractiveType::bank, bank));
    }

    const auto info = uiLayout::mainSampleInfo;
    ctx.canvas.save();
    ctx.canvas.scissor(info.x, info.y, info.width, info.height);
    ctx.canvas.fontSize(11.0f);
    ctx.canvas.fillColor(colors.contentPrimary);
    text(ctx, info.x, info.y, main.hasSource() && !main.status().text.empty()
         ? main.status().text.c_str() : "NO MAIN SAMPLE", nullptr);
    if (main.hasSource()) {
        char line[96];
        const double seconds = main.status().sampleRate != 0U
            ? static_cast<double>(main.status().frames) / main.status().sampleRate : 0.0;
        std::snprintf(line, sizeof(line), "%.2f s  /  %u Hz  /  %u slices", seconds,
                      static_cast<unsigned>(main.status().sampleRate),
                      static_cast<unsigned>(main.status().slices.size()));
        ctx.canvas.fillColor(colors.contentSecondary);
        text(ctx, info.x, info.y + 18.0f, line, nullptr);
    }
    ctx.canvas.restore();

    sms::ui::dpf::drawSegment(ctx.canvas, uiLayout::mainSampleLoad, "LOAD", false,
                              colors.activityPlayback,
                              hovered(ctx, InteractiveType::mainSampleLoad));
    sms::ui::dpf::drawSegment(ctx.canvas, uiLayout::mainSampleClear, "CLEAR", false,
                              colors.intentDanger, hovered(ctx, InteractiveType::mainSampleClear),
                              main.hasSource());

    if (main.hasSource()) {
        char line[48];
        const auto drawButton = [&](const sms::ui::Rect& rect, const char* label, const int index) {
            sms::ui::dpf::drawSegment(ctx.canvas, rect, label, false, colors.activityPlayback,
                                      hovered(ctx, InteractiveType::mainSampleAutoChop, index));
        };
        static constexpr const char* kActionLabels[] = {"ARRANGE", "AUTO COLOR", "CLEAR COLORS"};
        for (int index = 0; index < uiLayout::kMainSampleActionCount; ++index)
            sms::ui::dpf::drawSegment(ctx.canvas, uiLayout::mainSampleAction(index), kActionLabels[index],
                                      false, colors.activityPlayback,
                                      hovered(ctx, InteractiveType::mainSampleAction, index));
        drawButton(uiLayout::autoChopMode, main.autoChopModeName(), 0);
        drawButton(uiLayout::autoChopCountMinus, "-", 1);
        drawButton(uiLayout::autoChopCountPlus, "+", 2);
        drawButton(uiLayout::autoChopPadMinus, "-", 3);
        drawButton(uiLayout::autoChopPadPlus, "+", 4);
        drawButton(uiLayout::autoChopApply, "AUTO CHOP", 5);
        ctx.canvas.fontSize(10.0f);
        ctx.canvas.fillColor(colors.contentSecondary);
        if (main.countEditing())
            std::snprintf(line, sizeof(line), "COUNT %s_", main.countText().c_str());
        else
            std::snprintf(line, sizeof(line), "COUNT %u", static_cast<unsigned>(main.autoChopCount()));
        text(ctx, uiLayout::autoChopCountMinus.x + 54.0f, uiLayout::autoChopCountMinus.y + 8.0f, line, nullptr);
        std::snprintf(line, sizeof(line), "FIRST PAD %u", static_cast<unsigned>(main.autoChopFirstPad() + 1U));
        text(ctx, uiLayout::autoChopPadMinus.x + 54.0f, uiLayout::autoChopPadMinus.y + 8.0f, line, nullptr);
        ctx.canvas.save();
        const auto msg = uiLayout::autoChopMessage;
        ctx.canvas.scissor(msg.x, msg.y, msg.width, msg.height);
        text(ctx, msg.x, msg.y, main.autoChopMessage().c_str(), nullptr);
        ctx.canvas.restore();
    }

    const auto status = uiLayout::mainSampleStatus;
    const auto state = main.status().state;
    const char* heading = state == plugin::MainSampleState::loading ? "LOADING"
        : state == plugin::MainSampleState::error ? "ERROR"
        : state == plugin::MainSampleState::confirm ? "CONFIRM" : "";
    ctx.canvas.save();
    ctx.canvas.scissor(status.x, status.y, status.width, status.height);
    ctx.canvas.fontSize(10.0f);
    ctx.canvas.fillColor(state == plugin::MainSampleState::error ? colors.intentDanger
                                                                 : colors.contentSecondary);
    if (heading[0] != '\0') {
        text(ctx, status.x, status.y, heading, nullptr);
        text(ctx, status.x, status.y + 14.0f, main.status().text.c_str(), nullptr);
    } else if (ctx.state.status != nullptr && ctx.state.status[0] != '\0') {
        text(ctx, status.x, status.y, ctx.state.status, nullptr);
    }
    ctx.canvas.restore();
}

void drawConfirmModal(const ViewRenderContext& ctx, const MainSampleController& main)
{
    const auto& colors = sms::ui::dpf::theme();
    ctx.canvas.beginPath();
    ctx.canvas.rect(uiLayout::mainPanel.x, uiLayout::mainPanel.y, uiLayout::mainPanel.width,
                    uiLayout::mainPanel.height);
    ctx.canvas.fillColor(colors.canvas.withAlpha(0.6f));
    ctx.canvas.fill();
    sms::ui::dpf::drawPanel(ctx.canvas, uiLayout::mainSampleModal);
    ctx.canvas.fontFace(NANOVG_DEJAVU_SANS_TTF);
    ctx.canvas.fontSize(12.0f);
    ctx.canvas.textAlign(DGL_NAMESPACE::NanoVG::ALIGN_CENTER | DGL_NAMESPACE::NanoVG::ALIGN_MIDDLE);
    ctx.canvas.fillColor(colors.contentPrimary);
    text(ctx, uiLayout::mainSampleModal.x + uiLayout::mainSampleModal.width * 0.5f,
         uiLayout::mainSampleModal.y + 50.0f,
         main.autoChopConfirmPending() ? main.autoChopMessage().c_str()
             : main.status().text.empty() ? "Replace this pad?" : main.status().text.c_str(), nullptr);
    sms::ui::dpf::drawSegment(ctx.canvas, uiLayout::mainSampleConfirm, "CONFIRM", false,
                              colors.intentDanger,
                              hovered(ctx, InteractiveType::mainSampleConfirm, 0));
    sms::ui::dpf::drawSegment(ctx.canvas, uiLayout::mainSampleCancel, "CANCEL", false,
                              colors.activityPlayback,
                              hovered(ctx, InteractiveType::mainSampleConfirm, 1));
}

} // namespace

void drawMainSampleView(const ViewRenderContext& ctx)
{
    const auto& main = *ctx.state.mainSample;
    const auto& colors = sms::ui::dpf::theme();
    {
        const sms::ui::dpf::ScopedCanvasState panelState(ctx.canvas);
        ctx.canvas.globalAlpha(colors.padPanelAlpha);
        sms::ui::dpf::drawPanel(ctx.canvas, uiLayout::mainPanel);
    }
    ctx.canvas.fontFace(NANOVG_DEJAVU_SANS_TTF);
    ctx.canvas.fontSize(13.0f);
    ctx.canvas.textAlign(DGL_NAMESPACE::NanoVG::ALIGN_LEFT | DGL_NAMESPACE::NanoVG::ALIGN_TOP);
    ctx.canvas.fillColor(colors.contentSecondary);
    text(ctx, 46.0f, 120.0f, "MAIN SAMPLE", nullptr);
    ctx.canvas.fontSize(10.0f);
    ctx.canvas.textAlign(DGL_NAMESPACE::NanoVG::ALIGN_RIGHT | DGL_NAMESPACE::NanoVG::ALIGN_TOP);
    text(ctx, 936.0f, 122.0f, "SPACE PLAY/STOP   1-9 PICK SLICE   DRAG SELECT, THEN CLICK A PAD", nullptr);
    if (main.hasSource()) {
        const auto drawTransport = [&](const int index, const char* label, const bool active) {
            sms::ui::dpf::drawSegment(ctx.canvas, uiLayout::mainSampleTransport[static_cast<std::size_t>(index)],
                                      label, active, colors.activityPlayback,
                                      hovered(ctx, InteractiveType::mainSampleTransport, index));
        };
        drawTransport(0, main.playing() ? "STOP" : "PLAY", main.playing());
        drawTransport(1, "LOOP", main.looping());
        drawTransport(2, "-", false);
        drawTransport(3, "+", false);
        drawTransport(4, "FIT", main.viewport().zoomed());
    }

    drawWaveformArea(ctx, main);
    drawOverview(ctx, main);
    drawPads(ctx, main);
    drawSidePanel(ctx, main);
    if (main.confirmPending())
        drawConfirmModal(ctx, main);
}

} // namespace midichopper::ui::rendering
