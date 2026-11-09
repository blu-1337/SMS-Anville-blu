#include "MainSampleController.hpp"

#include "../plugin/StateKeys.hpp"

#include <algorithm>

namespace midichopper::ui {

namespace {
using plugin::MainSampleRequest;
using plugin::MainSampleSlice;
using plugin::MainSampleState;
using plugin::MainSampleVerb;

constexpr auto kRetry = std::chrono::milliseconds(250);
constexpr auto kDebounce = std::chrono::milliseconds(50);
} // namespace

void MainSampleController::open()
{
    fOpen = true;
    query();
    fHost.repaintMainSample();
}

void MainSampleController::close()
{
    if (!fOpen)
        return;
    cancelDrag();
    stopPreview();
    fOpen = false;
    fHost.repaintMainSample();
}

void MainSampleController::send(MainSampleRequest request)
{
    request.sequence = ++fSequence;
    fHost.sendMainSampleState(plugin::kMainSampleRequestKey,
                              plugin::encodeMainSampleRequest(request));
}

void MainSampleController::query()
{
    fQueryDue = false;
    send({});
}

void MainSampleController::load(const std::string& path)
{
    if (path.empty())
        return;
    MainSampleRequest request;
    request.verb = MainSampleVerb::load;
    request.path = path;
    send(std::move(request));
}

void MainSampleController::clear()
{
    MainSampleRequest request;
    request.verb = MainSampleVerb::clear;
    send(std::move(request));
}

bool MainSampleController::acceptStatus(const std::string_view encoded, const Time now)
{
    plugin::MainSampleStatus decoded;
    if (!plugin::decodeMainSampleStatus(encoded, decoded))
        return false;
    // Sequence 0 is a restore broadcast: links tagged by later pad keys need a Q.
    if (decoded.sequence == 0U) {
        fQueryDue = true;
        fQueryDeadline = now;
    } else if (decoded.sequence < fAcceptedSequence) {
        return false;
    } else {
        fAcceptedSequence = decoded.sequence;
    }

    const auto state = decoded.state;
    if (state == MainSampleState::loading || state == MainSampleState::error ||
        state == MainSampleState::confirm) {
        // These carry a message, not the session; keep the mirrored source.
        fStatus.sequence = decoded.sequence;
        fStatus.state = state;
        fStatus.text = std::move(decoded.text);
        fStatus.affectedMask = decoded.affectedMask;
        if (state == MainSampleState::confirm && fLastAssign &&
            decoded.affectedMask == (std::uint64_t{1} << fLastAssign->pad))
            fConfirm = fLastAssign;
        if (state == MainSampleState::error) {
            cancelDrag();
            fQueryDue = true; // An optimistic edit may have been refused.
            fQueryDeadline = now;
        }
    } else {
        const bool changed = decoded.sourceId != fStatus.sourceId ||
                             decoded.frames != fStatus.frames;
        fStatus = std::move(decoded);
        fConfirm.reset();
        if (changed)
            sourceChanged();
        else if (fDrag && std::none_of(fStatus.slices.begin(), fStatus.slices.end(),
                     [&](const MainSampleSlice& slice) { return slice.id == fDrag->sliceId; }))
            cancelDrag();
    }
    rebuildDisplaySlices();
    fHost.repaintMainSample();
    return true;
}

void MainSampleController::sourceChanged()
{
    fDrag.reset();
    fConfirm.reset();
    fLastAssign.reset();
    clearSelection();
    fViewport.reset(fStatus.frames);
    fOverview = {};
    fDetail = {};
    fOverviewSequence = 0U;
    fDetailSequence = 0U;
    fDetailDirty = false;
}

bool MainSampleController::acceptWaveform(const std::string_view encoded)
{
    plugin::MainSampleWaveform reply;
    if (!plugin::decodeMainSampleWaveform(encoded, reply) || !hasSource())
        return false;
    if (reply.sequence == fOverviewSequence && reply.start == 0U &&
        reply.end == fStatus.frames) {
        fOverview = {true, reply};
    } else if (reply.sequence == fDetailSequence && fViewport.zoomed() &&
               reply.start == fViewport.start && reply.end == fViewport.end) {
        fDetail = {true, reply};
        fDetailDirty = false;
    } else {
        return false;
    }
    fHost.repaintMainSample();
    return true;
}

bool MainSampleController::acceptPreviewPosition(const float value) noexcept
{
    if (value == fPreviewPosition)
        return false;
    fPreviewPosition = value;
    if (fOpen)
        fHost.repaintMainSample();
    return true;
}

void MainSampleController::padsChanged(const Time now) noexcept
{
    // A pad edit the view did not request may unlink or (on restore) link pads.
    if (!hasSource() || fQueryDue)
        return;
    fQueryDue = true;
    fQueryDeadline = now + kRetry;
}

void MainSampleController::sendWaveform(const std::uint32_t start, const std::uint32_t end,
    std::uint64_t& sequence, Time& deadline, const Time now)
{
    MainSampleRequest request;
    request.verb = MainSampleVerb::waveform;
    request.start = start;
    request.end = end;
    request.tiles = kDetailTiles;
    send(request);
    sequence = fSequence;
    deadline = now + kRetry;
}

void MainSampleController::idle(const Time now)
{
    if (!fStarted || (fQueryDue && now >= fQueryDeadline)) {
        fStarted = true;
        query();
    }
    if (!fOpen || !hasSource())
        return;
    if (!fOverview.ready && (fOverviewSequence == 0U || now >= fOverviewDeadline))
        sendWaveform(0U, fStatus.frames, fOverviewSequence, fOverviewDeadline, now);
    if (fDetailDirty && fViewport.zoomed() && now >= fDetailDeadline)
        sendWaveform(static_cast<std::uint32_t>(fViewport.start),
                     static_cast<std::uint32_t>(fViewport.end), fDetailSequence,
                     fDetailDeadline, now);
}

void MainSampleController::missedReplies()
{
    if (!fOverview.ready)
        fOverviewSequence = 0U;
    fDetailDirty = fViewport.zoomed() && !fDetail.ready;
    query();
}

bool MainSampleController::assignSelection(const std::uint32_t pad)
{
    if (!hasSource() || !hasSelection() || pad >= kPadCount)
        return false;
    MainSampleRequest request;
    request.verb = MainSampleVerb::assign;
    request.pad = pad;
    request.start = fSelectionStart;
    request.end = fSelectionEnd;
    fLastAssign = PendingAssign{pad, fSelectionStart, fSelectionEnd};
    fConfirm.reset();
    send(request);
    return true;
}

bool MainSampleController::confirmAssign(const bool confirm)
{
    if (!fConfirm)
        return false;
    const auto pending = *fConfirm;
    fConfirm.reset();
    if (confirm) {
        MainSampleRequest request;
        request.verb = MainSampleVerb::assign;
        request.pad = pending.pad;
        request.start = pending.start;
        request.end = pending.end;
        request.confirm = true;
        send(request);
    }
    fHost.repaintMainSample();
    return true;
}

bool MainSampleController::deleteSlice(const std::uint16_t sliceId)
{
    if (sliceId == 0U || std::none_of(fStatus.slices.begin(), fStatus.slices.end(),
            [&](const MainSampleSlice& slice) { return slice.id == sliceId; }))
        return false;
    MainSampleRequest request;
    request.verb = MainSampleVerb::remove;
    request.sliceId = sliceId;
    send(request);
    return true;
}

bool MainSampleController::preview(const std::uint32_t start, const std::uint32_t end,
                                   const bool loop)
{
    if (!hasSource() || start >= end || end > fStatus.frames)
        return false;
    fHost.sendMainSampleState(plugin::kMainPreviewRequestKey,
        plugin::encodeSourcePreviewRequest({true, loop, fStatus.sourceId, start, end}));
    return true;
}

void MainSampleController::stopPreview()
{
    fHost.sendMainSampleState(plugin::kMainPreviewRequestKey,
                              plugin::encodeSourcePreviewRequest({}));
}

bool MainSampleController::previewFrom(const std::uint32_t frame)
{
    std::uint32_t end = fStatus.frames;
    for (const auto& slice : fStatus.slices)
        if (slice.start <= frame && frame < slice.end)
            end = std::min(end, slice.end);
    return preview(frame, end, false);
}

std::uint32_t MainSampleController::clampFrame(const std::uint32_t frame) const noexcept
{
    return std::min(frame, fStatus.frames);
}

void MainSampleController::beginSelection(const std::uint32_t frame) noexcept
{
    fSelectionStart = fSelectionEnd = clampFrame(frame);
    fDrag.reset();
}

void MainSampleController::extendSelection(const std::uint32_t frame) noexcept
{
    const auto clamped = clampFrame(frame);
    if (clamped < fSelectionStart)
        fSelectionStart = clamped;
    else
        fSelectionEnd = clamped;
}

bool MainSampleController::beginMarkerDrag(const std::uint16_t sliceId, const bool endEdge) noexcept
{
    const auto slice = std::find_if(fStatus.slices.begin(), fStatus.slices.end(),
        [&](const MainSampleSlice& item) { return item.id == sliceId; });
    if (slice == fStatus.slices.end())
        return false;
    const auto edge = endEdge ? slice->end : slice->start;
    fDrag = Drag{sliceId, endEdge, edge, edge};
    return true;
}

void MainSampleController::dragTo(const std::uint32_t frame) noexcept
{
    if (!fDrag)
        return;
    // Each slice touching the boundary keeps at least one frame.
    std::uint32_t low = 0U;
    std::uint32_t high = fStatus.frames;
    for (const auto& slice : fStatus.slices) {
        if (slice.end == fDrag->original && slice.start < fDrag->original &&
            (slice.id == fDrag->sliceId || !fDrag->endEdge))
            low = std::max(low, slice.start + 1U);
        if (slice.start == fDrag->original && slice.end > fDrag->original &&
            (slice.id == fDrag->sliceId || fDrag->endEdge))
            high = std::min(high, slice.end - 1U);
    }
    fDrag->frame = std::clamp(frame, low, std::max(low, high));
    rebuildDisplaySlices();
    fHost.repaintMainSample();
}

bool MainSampleController::endDrag()
{
    if (!fDrag)
        return false;
    const auto drag = *fDrag;
    fDrag.reset();
    if (drag.frame == drag.original) {
        rebuildDisplaySlices();
        return false;
    }
    MainSampleRequest request;
    request.verb = MainSampleVerb::edit;
    // The dragged slice first, then at most one neighbour sharing the boundary.
    for (const bool own : {true, false}) {
        for (auto& slice : fStatus.slices) {
            if ((slice.id == drag.sliceId) != own)
                continue;
            const bool movesEnd = slice.end == drag.original &&
                                  (own ? drag.endEdge : !drag.endEdge);
            const bool movesStart = slice.start == drag.original &&
                                    (own ? !drag.endEdge : drag.endEdge);
            if (!movesEnd && !movesStart)
                continue;
            (movesEnd ? slice.end : slice.start) = drag.frame; // Optimistic until the reply.
            request.edits[request.editCount++] = slice;
            break;
        }
    }
    rebuildDisplaySlices();
    if (request.editCount == 0U)
        return false;
    send(request);
    return true;
}

void MainSampleController::cancelDrag() noexcept
{
    if (!fDrag)
        return;
    fDrag.reset();
    rebuildDisplaySlices();
}

void MainSampleController::rebuildDisplaySlices()
{
    fDisplaySlices = fStatus.slices;
    if (!fDrag)
        return;
    for (const bool own : {true, false}) {
        for (auto& slice : fDisplaySlices) {
            if ((slice.id == fDrag->sliceId) != own)
                continue;
            if (slice.end == fDrag->original && (own ? fDrag->endEdge : !fDrag->endEdge)) {
                slice.end = fDrag->frame;
                break;
            }
            if (slice.start == fDrag->original && (own ? !fDrag->endEdge : fDrag->endEdge)) {
                slice.start = fDrag->frame;
                break;
            }
        }
    }
}

bool MainSampleController::scheduleDetail(const bool changed, const Time now) noexcept
{
    if (changed) {
        fDetail.ready = false;
        fDetailDirty = fViewport.zoomed();
        fDetailDeadline = now + kDebounce;
        fHost.repaintMainSample();
    }
    return changed;
}

bool MainSampleController::zoom(const float delta, const float anchorX,
                                const sms::ui::Rect bounds, const Time now) noexcept
{
    return scheduleDetail(fViewport.zoom(delta, anchorX, bounds), now);
}

bool MainSampleController::pan(const float delta, const Time now) noexcept
{
    return scheduleDetail(fViewport.pan(delta), now);
}

bool MainSampleController::setScrollPosition(const float position, const Time now) noexcept
{
    return scheduleDetail(fViewport.setScrollPosition(position), now);
}

int MainSampleController::sliceForPad(const std::uint32_t pad) const noexcept
{
    for (const auto& link : fStatus.links) {
        if (link.pad != pad)
            continue;
        for (std::size_t index = 0; index < fStatus.slices.size(); ++index)
            if (fStatus.slices[index].id == link.sliceId)
                return static_cast<int>(index);
    }
    return -1;
}

} // namespace midichopper::ui
