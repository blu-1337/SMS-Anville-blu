#include "MainSampleController.hpp"
#include "MidichopperLayout.hpp"
#include "SamplerUiSession.hpp"
#include "StateKeys.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace midichopper;
using namespace midichopper::plugin;
using namespace midichopper::ui;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

void check(const bool condition, const char* const message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct Host final : MainSampleHost {
    std::vector<std::pair<std::string, std::string>> sent;
    int repaints = 0;
    void sendMainSampleState(const char* key, const std::string& value) override
    { sent.emplace_back(key, value); }
    void repaintMainSample() override { ++repaints; }

    [[nodiscard]] std::vector<MainSampleRequest> requests() const
    {
        std::vector<MainSampleRequest> result;
        for (const auto& [key, value] : sent) {
            MainSampleRequest request;
            if (key == kMainSampleRequestKey && decodeMainSampleRequest(value, request))
                result.push_back(request);
        }
        return result;
    }
};

std::string status(const std::uint64_t sequence, const MainSampleState state = MainSampleState::ready,
                   std::vector<MainSampleSlice> slices = {{1U, 0U, 400U}, {2U, 400U, 1000U}},
                   std::vector<MainSampleLink> links = {{3U, 2U}}, const std::uint64_t mask = 0U)
{
    MainSampleStatus value;
    value.sequence = sequence;
    value.state = state;
    value.sourceId = 0xABCU;
    value.frames = 1000U;
    value.sampleRate = 48000U;
    value.slices = std::move(slices);
    value.links = std::move(links);
    value.affectedMask = mask;
    value.text = "loop.wav";
    return encodeMainSampleStatus(value);
}

void queriesAndStaleReplies()
{
    Host host;
    MainSampleController controller(host);
    const auto now = Clock::now();
    controller.idle(now);
    check(host.requests().size() == 1U && host.requests()[0].verb == MainSampleVerb::query,
          "the first idle after construction sends Q");
    controller.idle(now);
    check(host.requests().size() == 1U, "Q is sent once");

    controller.open();
    check(controller.isOpen() && host.requests().back().verb == MainSampleVerb::query,
          "opening the view sends Q");
    check(controller.acceptStatus(status(2U), now) && controller.hasSource() &&
          controller.displaySlices().size() == 2U && controller.sliceForPad(3U) == 1,
          "a READY status mirrors slices and links");
    check(!controller.acceptStatus(status(1U, MainSampleState::ready, {}), now) &&
          controller.displaySlices().size() == 2U, "an older reply is ignored");

    const auto before = host.requests().size();
    check(controller.acceptStatus(status(0U), now), "a restore broadcast is accepted");
    controller.idle(now);
    check(host.requests().size() >= before + 1U &&
          host.requests()[before].verb == MainSampleVerb::query,
          "a sequence-0 status makes the view send Q");

    const auto error = status(controller.lastSentSequence() + 1U, MainSampleState::error, {}, {});
    check(controller.acceptStatus(error, now) && controller.status().state == MainSampleState::error &&
          controller.displaySlices().size() == 2U, "an ERROR keeps the mirrored session");
}

void waveformRequests()
{
    Host host;
    MainSampleController controller(host);
    const auto now = Clock::now();
    controller.idle(now);
    controller.open();
    check(controller.acceptStatus(status(controller.lastSentSequence()), now), "source arrives");
    controller.idle(now);
    const auto overview = host.requests().back();
    check(overview.verb == MainSampleVerb::waveform && overview.start == 0U &&
          overview.end == 1000U && overview.tiles == 4U, "the overview is requested once");
    controller.idle(now + 10ms);
    check(host.requests().back().sequence == overview.sequence, "no repeat before the retry");
    controller.idle(now + 300ms);
    const auto retry = host.requests().back();
    check(retry.verb == MainSampleVerb::waveform && retry.sequence > overview.sequence,
          "an unanswered overview is retried");

    MainSampleWaveform reply;
    reply.sequence = overview.sequence;
    reply.start = 0U;
    reply.end = 1000U;
    reply.count = 1U;
    reply.tiles[0].frames = 1000U;
    check(!controller.acceptWaveform(encodeMainSampleWaveform(reply)),
          "a superseded overview reply is ignored");
    reply.sequence = retry.sequence;
    check(controller.acceptWaveform(encodeMainSampleWaveform(reply)) &&
          controller.overview().ready, "the current overview reply is kept");
    const auto count = host.requests().size();
    controller.idle(now + 1s);
    check(host.requests().size() == count, "a ready overview is not requested again");

    const sms::ui::Rect bounds = layout::mainSampleWaveform;
    check(controller.zoom(4.0f, bounds.x + bounds.width * 0.5f, bounds, now + 1s) &&
          controller.viewport().zoomed(), "zoom narrows the viewport");
    controller.idle(now + 1s + 20ms);
    check(host.requests().size() == count, "detail requests are debounced");
    controller.idle(now + 1s + 60ms);
    const auto detail = host.requests().back();
    check(detail.verb == MainSampleVerb::waveform &&
          detail.start == controller.viewport().start && detail.end == controller.viewport().end,
          "detail covers the visible range");
}

void markerDragSendsOneEdit()
{
    Host host;
    MainSampleController controller(host);
    const auto now = Clock::now();
    controller.idle(now);
    check(controller.acceptStatus(status(1U), now), "source arrives");
    const auto count = host.requests().size();

    check(controller.beginMarkerDrag(1U, true), "grab the shared boundary");
    controller.dragTo(300U);
    controller.dragTo(450U);
    check(host.requests().size() == count && controller.displaySlices()[0].end == 450U &&
          controller.displaySlices()[1].start == 450U, "a drag is local and moves both slices");
    check(controller.endDrag(), "release commits");
    const auto edit = host.requests().back();
    check(host.requests().size() == count + 1U && edit.verb == MainSampleVerb::edit &&
          edit.editCount == 2U && edit.edits[0] == MainSampleSlice{1U, 0U, 450U} &&
          edit.edits[1] == MainSampleSlice{2U, 450U, 1000U},
          "exactly one S carries both edits of a shared boundary");

    check(controller.beginMarkerDrag(2U, true), "grab the last end");
    controller.dragTo(5000U);
    check(controller.displaySlices()[1].end == 1000U, "a drag stays inside the source");
    controller.dragTo(0U);
    check(controller.displaySlices()[1].end == 451U, "a slice keeps at least one frame");
    check(controller.endDrag() && host.requests().back().editCount == 1U,
          "an unshared boundary sends one edit");

    const auto after = host.requests().size();
    check(controller.beginMarkerDrag(1U, false), "grab the first start");
    check(!controller.endDrag() && host.requests().size() == after, "an unmoved marker sends nothing");
}

void assignConfirmDeleteAndPreview()
{
    Host host;
    MainSampleController controller(host);
    const auto now = Clock::now();
    controller.idle(now);
    check(controller.acceptStatus(status(1U), now), "source arrives");
    check(!controller.assignSelection(0U), "assign needs a selection");
    controller.beginSelection(700U);
    controller.extendSelection(100U);
    check(controller.selectionStart() == 100U && controller.selectionEnd() == 700U,
          "dragging left of the anchor selects back to the pointer");
    controller.extendSelection(900U);
    check(controller.selectionStart() == 700U && controller.selectionEnd() == 900U,
          "dragging back across the anchor flips direction and never re-anchors");
    controller.extendSelection(400U);
    controller.extendSelection(400U);
    check(controller.selectionStart() == 400U && controller.selectionEnd() == 700U,
          "the anchor is fixed for the whole drag");
    check(controller.assignSelection(5U), "assign the selection");
    const auto assign = host.requests().back();
    check(assign.verb == MainSampleVerb::assign && assign.pad == 5U && assign.start == 400U &&
          assign.end == 700U && !assign.confirm, "A carries the pad and range");
    check(controller.acceptStatus(status(assign.sequence, MainSampleState::confirm, {}, {},
                                         std::uint64_t{1} << 5U), now) &&
          controller.confirmPending(), "CONFIRM for the pad waits for the user");
    check(controller.confirmAssign(true) && host.requests().back().confirm &&
          host.requests().back().pad == 5U && !controller.confirmPending(),
          "confirming resends A with confirm");

    check(!controller.deleteSlice(9U) && controller.deleteSlice(2U) &&
          host.requests().back().verb == MainSampleVerb::remove &&
          host.requests().back().sliceId == 2U, "D only names a known slice");

    check(controller.previewFrom(500U), "preview from a click");
    SourcePreviewRequest preview;
    check(host.sent.back().first == kMainPreviewRequestKey &&
          decodeSourcePreviewRequest(host.sent.back().second, preview) && preview.play &&
          preview.sourceId == 0xABCU && preview.start == 500U && preview.end == 1000U,
          "a click previews to the end of its slice");
    check(!controller.preview(10U, 2000U, false), "a range past the source is refused");
    controller.stopPreview();
    check(decodeSourcePreviewRequest(host.sent.back().second, preview) && !preview.play,
          "stop sends MP1 stop");
    check(controller.acceptPreviewPosition(1.5f) && controller.previewPosition() == 1.5f &&
          !controller.acceptPreviewPosition(1.5f), "the playhead follows the parameter");
}

void transportPlaysSelectedSlice()
{
    Host host;
    MainSampleController controller(host);
    const auto now = Clock::now();
    controller.acceptStatus(status(1U), now);
    SourcePreviewRequest preview;
    auto last = [&] { return decodeSourcePreviewRequest(host.sent.back().second, preview); };

    check(controller.togglePlay() && last() && preview.play && preview.start == 0U &&
          preview.end == 1000U && !preview.loop, "Play with nothing selected plays the whole sample");
    controller.acceptPreviewPosition(1.1f);
    check(controller.playing() && controller.togglePlay() && last() && !preview.play,
          "Play while playing stops");
    controller.acceptPreviewPosition(0.0f);

    check(controller.selectSliceNumber(2U) && !controller.selectSliceNumber(3U) &&
          controller.selectedSlice() == 2U, "number keys pick slices that exist");
    controller.togglePlay();
    check(last() && preview.start == 400U && preview.end == 1000U, "Play plays exactly the selected slice");

    controller.acceptPreviewPosition(0.0f);
    controller.toggleLoop();
    controller.selectSliceNumber(1U);
    controller.togglePlay();
    check(last() && preview.loop && preview.start == 0U && preview.end == 400U, "Loop applies to the slice");
    controller.acceptPreviewPosition(1.1f);
    controller.toggleLoop();
    check(last() && preview.play && !preview.loop && preview.end == 400U,
          "turning Loop off restarts the running slice without looping");

    controller.acceptPreviewPosition(0.0f);
    controller.beginSelection(100U);
    controller.extendSelection(300U);
    controller.togglePlay();
    check(last() && preview.start == 100U && preview.end == 300U, "a drag selection wins over the slice");
    check(controller.sliceIndexAt(399U) == 0 && controller.sliceIndexAt(400U) == 1, "slice lookup by frame");
}

void zoomHelpers()
{
    Host host;
    MainSampleController controller(host);
    const auto now = Clock::now();
    controller.acceptStatus(status(1U), now);
    check(!controller.viewport().zoomed(), "a new source starts fitted");
    check(controller.zoomToRange(400U, 800U, now) && controller.viewport().zoomed() &&
          controller.viewport().start <= 400U && controller.viewport().end >= 800U &&
          controller.viewport().end - controller.viewport().start < 1000U,
          "zoom to a slice shows it with a small margin");
    check(controller.zoomFit(now) && !controller.viewport().zoomed() && !controller.zoomFit(now),
          "Fit restores the whole sample");
    const sms::ui::Rect bounds{0.0f, 0.0f, 100.0f, 10.0f};
    check(controller.zoom(1.0f, 50.0f, bounds, now) && controller.viewport().end -
          controller.viewport().start == 500U, "zoom in halves the window around the pointer");
}

void padChangesRequery()
{
    Host host;
    MainSampleController controller(host);
    const auto now = Clock::now();
    controller.idle(now);
    controller.padsChanged(now);
    controller.idle(now + 1s);
    check(host.requests().size() == 1U, "no source: pad changes need no Q");
    check(controller.acceptStatus(status(1U), now), "source arrives");
    controller.padsChanged(now);
    controller.padsChanged(now + 100ms);
    controller.idle(now + 100ms);
    check(host.requests().size() == 1U, "pad changes are debounced");
    controller.idle(now + 300ms);
    check(host.requests().size() == 2U && host.requests().back().verb == MainSampleVerb::query,
          "pad changes the view did not request send one Q");
}

struct SessionHost final : SamplerUiHost {
    std::vector<std::pair<std::string, std::string>> sent;
    void sendUiParameter(std::uint32_t, float) override {}
    void sendUiState(const char* key, const std::string& value) override
    { sent.emplace_back(key, value); }
    void sendUiNote(std::uint8_t, std::uint8_t) override {}
    void repaintUi() override {}
    bool showUiPadFileBrowser(bool, const char*) override { return false; }
    void invalidateUiInteraction(UiInteractionInvalidation) override {}
};

void sessionRouting()
{
    SessionHost host;
    SamplerUiSession session(host);
    session.openMainSample();
    check(session.state().mainSampleMode && session.mainSample().isOpen() &&
          host.sent.back().first == kMainSampleRequestKey, "the session opens the view with Q");
    check(session.acceptState(kMainSampleStatusKey, status(1U).c_str()) &&
          session.mainSample().hasSource(), "main_sample_status reaches the controller");
    session.acceptParameter(kParameterMainSamplePreviewPosition, 1.25f);
    check(session.mainSample().previewPosition() == 1.25f, "the preview position is routed");
    session.acceptParameter(kParameterMainSamplePreviewPosition, 0.0f);
    check(session.mainSample().playheadFrame() < 0.0, "no playhead while idle");
    session.acceptParameter(kParameterPlaybackPosition, 4.5f); // pad 3 (slice 2: 400..1000) half way
    check(std::abs(session.mainSample().playheadFrame() - 700.0) < 0.5,
          "a MIDI-played linked pad moves the playhead through its slice");
    session.acceptParameter(kParameterPlaybackPosition, 1.5f); // pad 0 is not linked
    check(session.mainSample().playheadFrame() < 0.0, "an unlinked pad shows no playhead");
    session.acceptParameter(kParameterPlaybackPosition, 4.25f);
    session.acceptParameter(kParameterMainSamplePreviewPosition, 1.1f);
    check(std::abs(session.mainSample().playheadFrame() - 100.0) < 0.5, "the preview wins over a pad voice");
    session.acceptParameter(kParameterMainSamplePreviewPosition, 0.0f);
    session.acceptParameter(kParameterPlaybackPosition, 0.0f);
    check(session.mainSample().playheadFrame() < 0.0, "the playhead clears when the voice ends");
    session.closeMainSample();
    check(!session.state().mainSampleMode && !session.mainSample().isOpen(),
          "closing leaves the view");

    SessionHost silentHost;
    SamplerUiCapabilities capabilities;
    capabilities.stateTransport = false;
    SamplerUiSession silent(silentHost, capabilities);
    silent.openMainSample();
    silent.idle(Clock::now(), 0.0f);
    check(!silent.state().mainSampleMode && silentHost.sent.empty(),
          "without state transport the view stays closed and silent");
}

void editorHeaderChipsFit()
{
    using namespace layout;
    // Heading text starts at x 46 and, at 13 pt in the widest skin font, stays under x 440.
    const std::array<sms::ui::Rect, 2> chips{padChoke, padOutput};
    for (const auto& chip : chips) {
        check(chip.x >= 440.0f && chip.x + chip.width <= closeEditor.x - 8.0f,
              "chips sit right of the heading and left of CLOSE");
        check(chip.y >= 108.0f && chip.y + chip.height <= 152.0f, "chips stay on the heading line");
        check(chip.width >= 110.0f && chip.height >= 26.0f, "chips stay legible");
    }
    check(padChoke.x + padChoke.width < padOutput.x, "CHOKE and OUT chips do not overlap");
    check(padLoop.y > padChoke.y + padChoke.height, "LOOP toggle is clear of the chips");
    check(closeEditor.x - (padOutput.x + padOutput.width) >= 64.0f,
          "the chips keep clear spacing from CLOSE and the menu");
    check(padOutput.x - (padChoke.x + padChoke.width) >= 12.0f, "the chips have a clean gap between them");
    check(padLoop.x == padGlobalMixer.x && padLoop.width == padGlobalMixer.width &&
          padLoop.y >= padGlobalMixer.y + padGlobalMixer.height + 4.0f &&
          padLoop.y <= padGlobalMixer.y + padGlobalMixer.height + 16.0f,
          "LOOP sits directly under the GLOBAL MIXER button, aligned");
    check(padLoop.y + padLoop.height <= 700.0f, "LOOP stays inside the editor panel");
    const sms::ui::Rect loopBox = padLoop;
    for (const auto& other : {padOutput, padChoke, padDirection, padGlobalMixer, mixerKnob(4)})
        check(loopBox.x >= other.x + other.width || other.x >= loopBox.x + loopBox.width ||
              loopBox.y >= other.y + other.height || other.y >= loopBox.y + loopBox.height,
              "LOOP overlaps no neighbouring control");
}

void countFieldEntry()
{
    Host host;
    MainSampleController controller(host);
    const auto t0 = MainSampleController::Clock::now();
    controller.clickCountField(t0);
    check(!controller.countEditing(), "a single click does not start typing");
    controller.clickCountField(t0 + std::chrono::milliseconds(900));
    check(!controller.countEditing(), "slow clicks are not a double-click");
    controller.clickCountField(t0 + std::chrono::milliseconds(1100));
    check(controller.countEditing(), "double-click starts typing");
    check(!controller.countCharacter('x') && controller.countCharacter('1') && controller.countCharacter('6'),
          "only digits are accepted");
    controller.countCharacter('9');
    controller.commitCount();
    check(!controller.countEditing() && controller.autoChopCount() == 16U, "Enter applies the count (two digits max)");
    controller.clickCountField(t0 + std::chrono::milliseconds(2000));
    controller.clickCountField(t0 + std::chrono::milliseconds(2100));
    controller.countCharacter('9'); controller.countCharacter('9');
    controller.commitCount();
    check(controller.autoChopCount() == 64U, "large counts clamp to the maximum");
    controller.clickCountField(t0 + std::chrono::milliseconds(3000));
    controller.clickCountField(t0 + std::chrono::milliseconds(3100));
    controller.countCharacter('0');
    controller.commitCount();
    check(controller.autoChopCount() == 1U, "zero clamps to 1");
    controller.clickCountField(t0 + std::chrono::milliseconds(4000));
    controller.clickCountField(t0 + std::chrono::milliseconds(4100));
    controller.countCharacter('7'); controller.countBackspace();
    controller.commitCount();
    check(controller.autoChopCount() == 1U, "empty entry keeps the count");
    controller.clickCountField(t0 + std::chrono::milliseconds(5000));
    controller.clickCountField(t0 + std::chrono::milliseconds(5100));
    controller.countCharacter('5'); controller.cancelCount();
    check(!controller.countEditing() && controller.autoChopCount() == 1U, "Escape cancels");
    controller.adjustAutoChopCount(1);
    check(controller.autoChopCount() == 2U, "plus still works");
    const auto field = layout::autoChopCountField;
    check(field.x >= layout::autoChopCountMinus.x + layout::autoChopCountMinus.width &&
          field.x + field.width <= layout::autoChopCountPlus.x, "count field sits between - and +");
}

void snapToSliceBoundaries()
{
    Host host;
    MainSampleController controller(host);
    const auto now = Clock::now();
    controller.idle(now);
    check(controller.acceptStatus(status(1U), now), "source arrives");
    check(controller.snapEnabled(), "snap defaults to on");
    const auto edge = controller.status().slices.front().end; // shared boundary
    check(controller.snapFrame(edge + 3U, 1.0) == edge && controller.snapFrame(edge - 7U, 1.0) == edge,
          "within 8 px the frame snaps to the boundary");
    check(controller.snapFrame(edge + 9U, 1.0) == edge + 9U, "beyond 8 px nothing snaps");
    check(controller.snapFrame(edge + 30U, 4.0) == edge, "the threshold scales with the zoom (pixels, not frames)");
    controller.toggleSnap();
    check(!controller.snapEnabled() && controller.snapFrame(edge + 3U, 1.0) == edge + 3U, "snap off passes frames through");
    controller.toggleSnap();
    check(controller.beginEdgeDrag(edge - 2U, 10U), "grab the shared boundary");
    check(controller.snapFrame(edge + 2U, 1.0) == edge + 2U, "an edge never snaps back onto its own start");
    controller.endDrag();
    using namespace layout;
    check(mainSampleTransport[5].x >= mainSampleTransport[4].x + mainSampleTransport[4].width &&
          mainSampleTransport[5].x + mainSampleTransport[5].width < 640.0f, "SNAP sits after FIT, clear of the hint");
}

void layoutRects()
{
    using namespace layout;
    for (std::size_t first = 0; first < mainSampleRects.size(); ++first) {
        const auto& rect = mainSampleRects[first];
        const auto& panel = first < 3U ? mainPanel : sidePanel;
        check(rect.x >= panel.x && rect.y >= panel.y && rect.x + rect.width <= panel.x + panel.width &&
              rect.y + rect.height <= panel.y + panel.height, "Main Sample rects stay in their panel");
        for (std::size_t second = first + 1U; second < mainSampleRects.size(); ++second) {
            const auto& other = mainSampleRects[second];
            check(rect.x + rect.width <= other.x || other.x + other.width <= rect.x ||
                  rect.y + rect.height <= other.y || other.y + other.height <= rect.y,
                  "Main Sample rects do not overlap");
        }
        if (first >= 3U && first != 3U)
            check(rect.y > 380.0f, "the side panel leaves y 154-380 for auto-chop");
    }
    const auto disjoint = [](const sms::ui::Rect& r, const sms::ui::Rect& o) {
        return r.x + r.width <= o.x || o.x + o.width <= r.x ||
               r.y + r.height <= o.y || o.y + o.height <= r.y;
    };
    for (int bank = 0; bank < 4; ++bank) {
        const auto rect = mainSampleBank(bank);
        check(rect.x >= sidePanel.x && rect.y >= sidePanel.y &&
              rect.x + rect.width <= sidePanel.x + sidePanel.width &&
              rect.y + rect.height <= sidePanel.y + sidePanel.height, "bank buttons stay in the side panel");
        for (int action = 0; action < kMainSampleActionCount; ++action)
            check(disjoint(rect, mainSampleAction(action)), "bank buttons clear the slice tools");
        for (const auto& other : {autoChopMode, autoChopCountMinus, autoChopCountPlus, autoChopCountField,
                                  autoChopPadMinus, autoChopPadPlus, autoChopApply, autoChopMessage})
            check(disjoint(rect, other), "bank buttons clear the auto-chop panel");
        for (const auto& other : mainSampleRects)
            check(disjoint(rect, other), "bank buttons overlap no Main Sample rect");
        for (int second = bank + 1; second < 4; ++second)
            check(disjoint(rect, mainSampleBank(second)), "bank buttons do not overlap each other");
    }
    for (int index = 0; index < kMainSampleActionCount; ++index) {
        const auto rect = mainSampleAction(index);
        check(rect.x >= sidePanel.x && rect.y >= sidePanel.y &&
              rect.x + rect.width <= sidePanel.x + sidePanel.width &&
              rect.y + rect.height <= sidePanel.y + sidePanel.height, "action buttons stay in the side panel");
        for (const auto& other : mainSampleRects)
            check(disjoint(rect, other), "action buttons overlap no other Main Sample rect");
        for (int second = index + 1; second < kMainSampleActionCount; ++second)
            check(disjoint(rect, mainSampleAction(second)), "action buttons do not overlap each other");
    }
}

void assignPicksFirstFreePad()
{
    check(MainSampleController::firstFreePad("11.0", 4) == 2, "'.' counts as free, in pad order");
    check(MainSampleController::firstFreePad("1111", 4) == -1, "no free pad reports -1");
    check(MainSampleController::firstFreePad("1110", 3) == -1, "pads beyond the visible count are ignored");
    check(MainSampleController::firstFreePad("0111", 4) == 0, "the lowest free pad wins");
}
} // namespace

void autoChopPanel()
{
    Host host;
    MainSampleController controller(host);
    check(!controller.autoChop(), "auto chop needs a source");
    controller.acceptStatus(status(1U));
    host.sent.clear();
    controller.cycleAutoChopMode();
    controller.adjustAutoChopCount(-3);
    controller.adjustAutoChopFirstPad(4);
    check(controller.autoChop(), "auto chop sends");
    AutoChopRequest request;
    check(host.sent.size() == 1U && host.sent[0].first == kAutoChopRequestKey &&
          decodeAutoChopRequest(host.sent[0].second, request) &&
          request.mode == AutoChopMode::findSamples && request.count == 13U &&
          request.firstPad == 4U && !request.confirm, "request carries the panel settings");
    AutoChopStatus reply;
    reply.sequence = request.sequence + 5U;
    reply.state = AutoChopState::ok;
    check(!controller.acceptAutoChopStatus(encodeAutoChopStatus(reply)), "stale status is ignored");
    reply.sequence = request.sequence;
    reply.state = AutoChopState::confirm;
    reply.mask = 0x10U;
    reply.found = 1U;
    check(controller.acceptAutoChopStatus(encodeAutoChopStatus(reply)) && controller.confirmPending() &&
          controller.autoChopConfirmPending(), "CONFIRM opens the modal");
    host.sent.clear();
    check(controller.confirmAssign(true) && host.sent.size() == 1U &&
          decodeAutoChopRequest(host.sent[0].second, request) && request.confirm &&
          !controller.confirmPending(), "confirming resends with confirm=1");
    reply.sequence = request.sequence;
    reply.state = AutoChopState::ok;
    reply.found = 3U;
    reply.requested = 5U;
    check(controller.acceptAutoChopStatus(encodeAutoChopStatus(reply)) &&
          controller.autoChopMessage() == "Found 3 of 5", "OK reports shortfalls");
    controller.autoChop();
    host.sent.clear();
    reply.sequence = controller.lastSentSequence();
    reply.state = AutoChopState::confirm;
    controller.acceptAutoChopStatus(encodeAutoChopStatus(reply));
    check(controller.confirmAssign(false) && host.sent.empty(), "cancel sends nothing");
}

void rightDragEdges()
{
    Host host;
    MainSampleController controller(host);
    const auto now = Clock::now();
    controller.idle(now);
    check(controller.acceptStatus(status(1U), now), "source arrives");
    const auto count = host.requests().size();

    check(!controller.beginEdgeDrag(200U, 10U), "no edge near the middle of a slice");
    check(controller.beginEdgeDrag(395U, 10U), "grab the shared boundary near its end side");
    controller.dragTo(450U);
    check(controller.endDrag() && host.requests().size() == count + 1U &&
          host.requests().back().editCount == 2U && host.requests().back().edits[0].end == 450U,
          "edge drag sends one re-cut request");

    check(controller.beginEdgeDrag(2U, 10U), "grab the first start");
    controller.dragTo(0U);
    check(!controller.endDrag(), "unmoved edge sends nothing");

    controller.beginSelection(100U);
    controller.extendSelection(300U);
    const auto before = host.requests().size();
    check(controller.beginEdgeDrag(105U, 10U), "selection start edge is grabbed");
    controller.dragTo(150U);
    check(!controller.endDrag() && controller.selectionStart() == 150U &&
          controller.selectionEnd() == 300U && host.requests().size() == before,
          "selection edge moves locally without a request");
    check(controller.beginEdgeDrag(298U, 10U), "selection end edge is grabbed");
    controller.dragTo(100U);
    controller.endDrag();
    check(controller.selectionEnd() == 151U, "selection keeps one frame");

    controller.beginSelection(100U);
    controller.extendSelection(900U);
    check(!controller.beginSelectionHalfDrag(50U) && !controller.beginSelectionHalfDrag(950U),
          "a press outside the selection does not grab a selection edge");
    check(controller.beginSelectionHalfDrag(200U), "a press in the left half grabs the start");
    controller.dragTo(300U);
    controller.endDrag();
    check(controller.selectionStart() == 300U && controller.selectionEnd() == 900U,
          "left-half drag moves the start and follows the pointer");
    check(controller.beginSelectionHalfDrag(800U), "a press in the right half grabs the end");
    controller.dragTo(950U);
    controller.endDrag();
    check(controller.selectionStart() == 300U && controller.selectionEnd() == 950U,
          "right-half drag moves the end and follows the pointer");
}

void arrangeSendsOneRequestAndKeepsMoves()
{
    Host host;
    MainSampleController controller(host);
    const auto now = Clock::now();
    controller.idle(now);
    check(controller.acceptStatus(status(1U, MainSampleState::ready, {{1U, 0U, 400U}, {2U, 400U, 1000U}},
          {{3U, 2U}, {5U, 1U}}), now), "source arrives");
    const auto count = host.requests().size();
    check(controller.arrange() && host.requests().size() == count + 1U &&
          host.requests().back().verb == MainSampleVerb::arrange, "arrange sends one request");
    check(controller.takeArrangedMoves().empty(), "moves are not confirmed before the reply");
    const auto sequence = controller.lastSentSequence();
    check(controller.acceptStatus(status(sequence, MainSampleState::ready, {{1U, 0U, 400U}, {2U, 400U, 1000U}},
          {{3U, 1U}, {5U, 2U}}), now), "reply arrives");
    const auto moves = controller.takeArrangedMoves();
    check(moves.size() == 2U && moves[0] == ArrangeMove{5U, 3U} && moves[1] == ArrangeMove{3U, 5U},
          "the confirmed moves carry pad colours");
    check(controller.takeArrangedMoves().empty(), "moves are consumed once");
    check(!controller.arrange() && host.requests().size() == count + 1U, "ordered pads send nothing");
}

void spreadColorsContrast()
{
    for (std::size_t press = 0U; press < 4U; ++press) {
        for (std::size_t rank = 0U; rank < 12U; ++rank) {
            const int a = MainSampleController::spreadColor(rank, 6U, press);
            const int b = MainSampleController::spreadColor(rank + 1U, 6U, press);
            check(a >= 1 && a <= 6 && a != b, "adjacent zones differ");
            check(a == MainSampleController::spreadColor(rank, 6U, press), "deterministic");
        }
    }
    check(MainSampleController::spreadColor(0U, 6U, 0U) != MainSampleController::spreadColor(0U, 6U, 1U),
          "a new press changes the colours");
}

void autoColorPressesAndClear()
{
    Host host;
    MainSampleController controller(host);
    check(controller.colorPress() == 0U && !controller.neutralZones(), "default colours");
    (void)controller.autoColor(6U);
    check(controller.colorPress() == 1U, "press counted");
    (void)controller.autoColor(6U);
    check(controller.colorPress() == 2U, "a new press, a new spread");
    controller.clearColors();
    check(controller.neutralZones() && controller.colorPress() == 0U, "clear neutralises zones");
    (void)controller.autoColor(6U);
    check(!controller.neutralZones(), "auto colour brings the zones back");
}

void selectedSliceKeepsEditPriority()
{
    Host host;
    MainSampleController controller(host);
    const auto now = Clock::now();
    controller.idle(now);
    // Overlapping slices: 1 = [0,500), 2 = [400,1000).
    check(controller.acceptStatus(status(1U, MainSampleState::ready, {{1U, 0U, 500U}, {2U, 400U, 1000U}},
          {{0U, 1U}, {1U, 2U}}), now), "source arrives");
    controller.selectSlice(2U);
    const auto count = host.requests().size();
    check(!controller.beginEdgeDrag(500U, 10U), "a first-slice marker is ignored while the second slice is selected");
    check(controller.beginEdgeDrag(405U, 10U), "the selected slice's own edge is grabbed");
    controller.dragTo(450U);
    check(controller.endDrag() && host.requests().size() == count + 1U, "one request");
    const auto edit = host.requests().back();
    check(edit.editCount == 1U && edit.edits[0] == MainSampleSlice{2U, 450U, 1000U},
          "only the selected slice changes");
    check(controller.displaySlices()[0] == MainSampleSlice{1U, 0U, 500U}, "the other slice is untouched");

    // A shared boundary is not moved with the selected slice.
    controller.acceptStatus(status(2U, MainSampleState::ready, {{1U, 0U, 400U}, {2U, 400U, 1000U}},
          {{0U, 1U}, {1U, 2U}}), now);
    controller.selectSlice(2U);
    check(controller.beginMarkerDrag(2U, false, true), "grab the shared boundary solo");
    controller.dragTo(450U);
    check(controller.endDrag() && host.requests().back().editCount == 1U &&
          host.requests().back().edits[0] == MainSampleSlice{2U, 450U, 1000U}, "no move-both while selected");
    controller.selectSlice(0U);
    check(controller.beginEdgeDrag(402U, 10U), "without selection the nearest marker works");
}

int main()
{
    queriesAndStaleReplies();
    waveformRequests();
    markerDragSendsOneEdit();
    rightDragEdges();
    selectedSliceKeepsEditPriority();
    spreadColorsContrast();
    autoColorPressesAndClear();
    arrangeSendsOneRequestAndKeepsMoves();
    assignConfirmDeleteAndPreview();
    transportPlaysSelectedSlice();
    zoomHelpers();
    padChangesRequery();
    sessionRouting();
    editorHeaderChipsFit();
    countFieldEntry();
    snapToSliceBoundaries();
    layoutRects();
    assignPicksFirstFreePad();
    autoChopPanel();
    std::cout << "Main Sample UI tests passed\n";
}
