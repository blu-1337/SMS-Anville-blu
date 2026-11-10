#include "MainSampleController.hpp"

#include "../plugin/StateKeys.hpp"

#include <algorithm>
#include <utility>

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
    if (fArrangeSequence != 0U && decoded.sequence == fArrangeSequence) {
        if (state == MainSampleState::ready)
            fArrangeConfirmed = std::move(fArrangeMoves);
        fArrangeMoves.clear();
        fArrangeSequence = 0U;
    }
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
    fSelectedSlice = 0U;
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

int MainSampleController::firstFreePad(const std::string_view padState, const int visiblePads) noexcept
{
    const int count = std::min(visiblePads, static_cast<int>(padState.size()));
    for (int pad = 0; pad < count; ++pad)
        if (padState[static_cast<std::size_t>(pad)] == '0' || padState[static_cast<std::size_t>(pad)] == '.')
            return pad;
    return -1;
}

bool MainSampleController::confirmAssign(const bool confirm)
{
    if (fAutoConfirm) {
        fAutoConfirm = false;
        if (confirm)
            autoChop(true);
        fHost.repaintMainSample();
        return true;
    }
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

bool MainSampleController::arrange()
{
    if (!hasSource())
        return false;
    auto moves = plugin::arrangeMoves(fStatus.slices, fStatus.links);
    if (moves.empty())
        return false;
    MainSampleRequest request;
    request.verb = MainSampleVerb::arrange;
    send(request);
    fArrangeMoves = std::move(moves);
    fArrangeSequence = fSequence;
    return true;
}

std::vector<plugin::ArrangeMove> MainSampleController::takeArrangedMoves()
{
    return std::exchange(fArrangeConfirmed, {});
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
    fPlayStart = start;
    fPlayEnd = end;
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

bool MainSampleController::togglePlay()
{
    if (playing()) {
        stopPreview();
        return true;
    }
    if (!hasSource())
        return false;
    std::uint32_t start = 0U;
    std::uint32_t end = fStatus.frames;
    if (hasSelection()) {
        start = fSelectionStart;
        end = fSelectionEnd;
    } else {
        for (const auto& slice : fDisplaySlices)
            if (slice.id == fSelectedSlice && fSelectedSlice != 0U) {
                start = slice.start;
                end = slice.end;
            }
    }
    return preview(start, end, fLoop);
}

void MainSampleController::toggleLoop()
{
    fLoop = !fLoop;
    // A running preview restarts so the new mode applies at once.
    if (playing() && fPlayEnd > fPlayStart)
        preview(fPlayStart, fPlayEnd, fLoop);
    fHost.repaintMainSample();
}

int MainSampleController::sliceIndexAt(const std::uint32_t frame) const noexcept
{
    for (std::size_t index = 0; index < fDisplaySlices.size(); ++index)
        if (fDisplaySlices[index].start <= frame && frame < fDisplaySlices[index].end)
            return static_cast<int>(index);
    return -1;
}

bool MainSampleController::selectSliceNumber(const std::size_t number) noexcept
{
    if (number == 0U || number > fDisplaySlices.size())
        return false;
    selectSlice(fDisplaySlices[number - 1U].id);
    fHost.repaintMainSample();
    return true;
}

bool MainSampleController::zoomFit(const Time now) noexcept
{
    const auto before = fViewport;
    fViewport.reset(fStatus.frames);
    return scheduleDetail(before.start != fViewport.start || before.end != fViewport.end, now);
}

bool MainSampleController::zoomToRange(const std::uint32_t start, const std::uint32_t end,
                                       const Time now) noexcept
{
    if (!hasSource() || end <= start)
        return false;
    const std::uint64_t margin = (end - start) / 20U;
    const std::uint64_t first = start > margin ? start - margin : 0U;
    const std::uint64_t last = std::min<std::uint64_t>(fStatus.frames, std::uint64_t{end} + margin);
    if (last - first < 16U)
        return false;
    const auto before = fViewport;
    fViewport.start = first;
    fViewport.end = last;
    return scheduleDetail(before.start != first || before.end != last, now);
}

bool MainSampleController::revealRange(const std::uint32_t start, const std::uint32_t end,
                                       const Time now) noexcept
{
    if (!hasSource() || end <= start || !fViewport.zoomed())
        return false;
    const std::uint64_t width = fViewport.end - fViewport.start;
    if (start >= fViewport.start && end <= fViewport.end)
        return false;
    std::uint64_t first = start;
    if (end - start < width)
        first = start > (width - (end - start)) / 2U ? start - (width - (end - start)) / 2U : 0U;
    first = std::min<std::uint64_t>(first, fStatus.frames > width ? fStatus.frames - width : 0U);
    const auto before = fViewport;
    fViewport.start = first;
    fViewport.end = first + width;
    return scheduleDetail(before.start != fViewport.start, now);
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

bool MainSampleController::beginMarkerDrag(const std::uint16_t sliceId, const bool endEdge,
                                           const bool solo) noexcept
{
    const auto slice = std::find_if(fStatus.slices.begin(), fStatus.slices.end(),
        [&](const MainSampleSlice& item) { return item.id == sliceId; });
    if (slice == fStatus.slices.end())
        return false;
    const auto edge = endEdge ? slice->end : slice->start;
    fDrag = Drag{sliceId, endEdge, solo, edge, edge};
    return true;
}

bool MainSampleController::beginEdgeDrag(const std::uint32_t frame,
                                         const std::uint32_t toleranceFrames) noexcept
{
    if (!hasSource())
        return false;
    const auto distance = [&](const std::uint32_t edge) {
        return edge > frame ? edge - frame : frame - edge;
    };
    std::uint32_t best = toleranceFrames + 1U;
    std::uint16_t sliceId = 0U;
    bool endEdge = false;
    int selection = 0;
    if (hasSelection()) {
        if (distance(fSelectionStart) < best) { best = distance(fSelectionStart); selection = 1; }
        if (distance(fSelectionEnd) < best) { best = distance(fSelectionEnd); selection = 2; }
    }
    const bool prioritized = fSelectedSlice != 0U && !hasSelection();
    for (const auto& slice : fStatus.slices) {
        if (prioritized && slice.id != fSelectedSlice)
            continue; // The active slice wins over any closer or overlapping marker.
        if (distance(slice.start) < best) {
            best = distance(slice.start); sliceId = slice.id; endEdge = false; selection = 0;
        }
        if (distance(slice.end) < best) {
            best = distance(slice.end); sliceId = slice.id; endEdge = true; selection = 0;
        }
    }
    if (best > toleranceFrames)
        return false;
    if (selection != 0) {
        fDrag.reset();
        fSelectionEdge = selection;
        return true;
    }
    const bool solo = fSelectedSlice != 0U;
    selectSlice(sliceId);
    return beginMarkerDrag(sliceId, endEdge, solo);
}

void MainSampleController::dragTo(const std::uint32_t frame) noexcept
{
    if (fSelectionEdge != 0) {
        const auto clamped = clampFrame(frame);
        if (fSelectionEdge == 1)
            fSelectionStart = std::min(clamped, fSelectionEnd - 1U);
        else
            fSelectionEnd = std::max(clamped, fSelectionStart + 1U);
        fHost.repaintMainSample();
        return;
    }
    if (!fDrag)
        return;
    // Each slice touching the boundary keeps at least one frame.
    std::uint32_t low = 0U;
    std::uint32_t high = fStatus.frames;
    for (const auto& slice : fStatus.slices) {
        if (fDrag->solo && slice.id != fDrag->sliceId)
            continue;
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
    if (fSelectionEdge != 0) {
        fSelectionEdge = 0;
        return false;
    }
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
            if ((slice.id == drag.sliceId) != own || (drag.solo && !own))
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
    fSelectionEdge = 0;
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
            if ((slice.id == fDrag->sliceId) != own || (fDrag->solo && !own))
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

namespace midichopper::ui {

const char* MainSampleController::autoChopModeName() const noexcept
{
    switch (fAutoMode) {
    case plugin::AutoChopMode::transient: return "TRANSIENT";
    case plugin::AutoChopMode::findSamples: return "FIND SAMPLES";
    case plugin::AutoChopMode::equal: return "EQUAL";
    case plugin::AutoChopMode::beat: return "BEAT";
    case plugin::AutoChopMode::random: return "RANDOM";
    }
    return "";
}

void MainSampleController::cycleAutoChopMode() noexcept
{
    constexpr plugin::AutoChopMode order[] = {
        plugin::AutoChopMode::transient, plugin::AutoChopMode::findSamples,
        plugin::AutoChopMode::equal, plugin::AutoChopMode::beat, plugin::AutoChopMode::random};
    std::size_t index = 0;
    while (index < 5U && order[index] != fAutoMode)
        ++index;
    fAutoMode = order[(index + 1U) % 5U];
    fHost.repaintMainSample();
}

void MainSampleController::adjustAutoChopCount(const int delta) noexcept
{
    const int value = static_cast<int>(fAutoCount) + delta;
    fAutoCount = static_cast<std::uint32_t>(
        std::clamp(value, 1, static_cast<int>(plugin::kAutoChopMaxCount)));
    fHost.repaintMainSample();
}

void MainSampleController::adjustAutoChopFirstPad(const int delta) noexcept
{
    const int value = static_cast<int>(fAutoFirstPad) + delta;
    fAutoFirstPad = static_cast<std::uint32_t>(
        std::clamp(value, 0, static_cast<int>(kPadCount) - 1));
    fHost.repaintMainSample();
}

bool MainSampleController::autoChop(const bool confirm)
{
    if (!hasSource())
        return false;
    plugin::AutoChopRequest request;
    request.sequence = ++fSequence;
    request.mode = fAutoMode;
    request.firstPad = fAutoFirstPad;
    request.count = fAutoCount;
    request.confirm = confirm;
    request.sensitivityPermille = 500U;
    request.minGapMs = 50U;
    request.bpmCenti = 0U; // host tempo
    request.beatsMilli = 1000U;
    request.snap = true;
    fAutoSequence = request.sequence;
    fAutoConfirm = false;
    fAutoMessage = "Chopping...";
    fHost.sendMainSampleState(plugin::kAutoChopRequestKey, plugin::encodeAutoChopRequest(request));
    fHost.repaintMainSample();
    return true;
}

bool MainSampleController::acceptAutoChopStatus(const std::string_view encoded)
{
    plugin::AutoChopStatus status;
    if (!plugin::decodeAutoChopStatus(encoded, status) || status.sequence != fAutoSequence)
        return false;
    fAutoConfirm = false;
    switch (status.state) {
    case plugin::AutoChopState::ok:
        fAutoMessage = status.found < status.requested
            ? "Found " + std::to_string(status.found) + " of " + std::to_string(status.requested)
            : "Chopped " + std::to_string(status.found) + " slices";
        break;
    case plugin::AutoChopState::confirm:
        fAutoConfirm = true;
        fAutoMessage = "Replace " + std::to_string(status.found) + " pads with other audio?";
        break;
    case plugin::AutoChopState::busy: fAutoMessage = "Busy, try again"; break;
    case plugin::AutoChopState::error: fAutoMessage = status.text; break;
    }
    fHost.repaintMainSample();
    return true;
}

int MainSampleController::spreadColor(const std::size_t rank, const std::size_t paletteSize,
                                      const std::size_t press) noexcept
{
    if (paletteSize == 0U)
        return 0;
    const std::size_t slot = (rank + press) % paletteSize;
    const std::size_t half = (paletteSize + 1U) / 2U;
    // 0, half, 1, half+1, ...: halves of the colour wheel alternate.
    const std::size_t index = slot % 2U == 0U ? slot / 2U : half + slot / 2U;
    return static_cast<int>(index) + 1;
}

std::vector<std::pair<std::uint32_t, int>> MainSampleController::autoColor(const std::size_t paletteSize)
{
    ++fColorPress;
    fNeutralZones = false;
    std::vector<std::pair<std::uint32_t, int>> result;
    for (const auto& link : fStatus.links)
        for (std::size_t index = 0U; index < fDisplaySlices.size(); ++index)
            if (fDisplaySlices[index].id == link.sliceId)
                result.emplace_back(link.pad, spreadColor(index, paletteSize, fColorPress));
    return result;
}

} // namespace midichopper::ui
