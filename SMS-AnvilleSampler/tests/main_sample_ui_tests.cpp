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
    controller.extendSelection(900U);
    check(controller.selectionStart() == 100U && controller.selectionEnd() == 900U,
          "the selection grows in both directions");
    check(controller.assignSelection(5U), "assign the selection");
    const auto assign = host.requests().back();
    check(assign.verb == MainSampleVerb::assign && assign.pad == 5U && assign.start == 100U &&
          assign.end == 900U && !assign.confirm, "A carries the pad and range");
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
          request.mode == AutoChopMode::findSamples && request.count == 5U &&
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

int main()
{
    queriesAndStaleReplies();
    waveformRequests();
    markerDragSendsOneEdit();
    assignConfirmDeleteAndPreview();
    transportPlaysSelectedSlice();
    zoomHelpers();
    padChangesRequery();
    sessionRouting();
    layoutRects();
    autoChopPanel();
    std::cout << "Main Sample UI tests passed\n";
}
