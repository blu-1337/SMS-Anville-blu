#pragma once

#include "Configuration.hpp"
#include "../plugin/AutoChopProtocol.hpp"
#include "../plugin/MainSampleProtocol.hpp"
#include "UI/Geometry.hpp"
#include "WaveformViewport.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace midichopper::ui {

/** Effects of the Main Sample view, supplied by SamplerUiSession. */
class MainSampleHost {
public:
    virtual ~MainSampleHost() = default;
    virtual void sendMainSampleState(const char* key, const std::string& value) = 0;
    virtual void repaintMainSample() = 0;
};

/** A W reply kept with its source coordinates (1-4 tiles of 128 bins). */
struct MainSampleWaveformView {
    bool ready = false;
    plugin::MainSampleWaveform waveform{};
};

/**
 * Mirrors the plug-in's Main Sample session for the view (Docs/AI/MAIN-SAMPLE-UI.md).
 * DPF-free: it sends MS1/MP1 values through the host and accepts the replies.
 * Marker drags stay local until endDrag(), which sends exactly one S.
 */
class MainSampleController {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr std::uint32_t kDetailTiles = 4U;

    explicit MainSampleController(MainSampleHost& host) noexcept : fHost(host) {}

    // View lifecycle. open() and the first idle() after construction send Q.
    void open();
    void close();
    [[nodiscard]] bool isOpen() const noexcept { return fOpen; }

    // Replies and host notifications. Each returns true when the view changed.
    bool acceptStatus(std::string_view encoded, Time now = Clock::now());
    bool acceptWaveform(std::string_view encoded);
    bool acceptPreviewPosition(float value) noexcept;
    void padsChanged(Time now) noexcept;
    void idle(Time now);
    void missedReplies();

    // Requests.
    void query();
    void load(const std::string& path);
    void clear();
    bool assignSelection(std::uint32_t pad);
    /** First visible (local) pad, in pad-number order, with no sample ('0' or '.'); -1 if none. */
    [[nodiscard]] static int firstFreePad(std::string_view padState, int visiblePads) noexcept;
    bool confirmAssign(bool confirm);
    /**
     * Arrange: asks the plug-in to renumber the linked pads in slice time order.
     * False (and nothing sent) when there is nothing to move. The moves are kept
     * until takeArrangedMoves() so the session can carry pad colours along.
     */
    bool arrange();
    /**
     * Zone colour of slice number `rank` (0-based, time order) as a 1-based palette index:
     * an interleaved walk of the palette so neighbours contrast. `press` rotates the walk
     * (deterministic: the same press always gives the same colours).
     */
    [[nodiscard]] static int spreadColor(std::size_t rank, std::size_t paletteSize,
                                         std::size_t press) noexcept;
    /**
     * AUTO COLOR: advances the press counter (a new spread each time) and returns the colour
     * of every linked pad as (global pad, 1-based palette index). Unlinked zones follow the
     * same spread. Local only; the session stores the pad colours.
     */
    [[nodiscard]] std::vector<std::pair<std::uint32_t, int>> autoColor(std::size_t paletteSize);
    /** CLEAR COLORS: zones become neutral until the next AUTO COLOR; the session resets the pads. */
    void clearColors() noexcept { fColorPress = 0U; fNeutralZones = true; }
    [[nodiscard]] std::size_t colorPress() const noexcept { return fColorPress; }
    [[nodiscard]] bool neutralZones() const noexcept { return fNeutralZones; }
    /** Moves of the arrange the plug-in confirmed; empty until its reply. Consumed once. */
    [[nodiscard]] std::vector<plugin::ArrangeMove> takeArrangedMoves();
    bool deleteSlice(std::uint16_t sliceId);
    bool preview(std::uint32_t start, std::uint32_t end, bool loop);
    void stopPreview();
    bool previewFrom(std::uint32_t frame);

    // Auto chop (AC1): panel settings, request and reply. Pad and slice effects
    // arrive through the usual main_sample_status.
    void cycleAutoChopMode() noexcept;
    void adjustAutoChopCount(int delta) noexcept;
    void adjustAutoChopFirstPad(int delta) noexcept;
    /** Click on the count field; the second click within 400 ms starts typing the count. */
    void clickCountField(Time now) noexcept;
    [[nodiscard]] bool countEditing() const noexcept { return fCountEditing; }
    [[nodiscard]] const std::string& countText() const noexcept { return fCountText; }
    /** Digits only, at most two; false for anything else. */
    bool countCharacter(std::uint32_t codepoint) noexcept;
    void countBackspace() noexcept;
    /** Enter: applies the typed number clamped to 1..max (empty text keeps the old count). */
    void commitCount() noexcept;
    void cancelCount() noexcept;
    bool autoChop(bool confirm = false);
    bool acceptAutoChopStatus(std::string_view encoded);
    [[nodiscard]] plugin::AutoChopMode autoChopMode() const noexcept { return fAutoMode; }
    [[nodiscard]] std::uint32_t autoChopCount() const noexcept { return fAutoCount; }
    [[nodiscard]] std::uint32_t autoChopFirstPad() const noexcept { return fAutoFirstPad; }
    [[nodiscard]] bool autoChopConfirmPending() const noexcept { return fAutoConfirm; }
    [[nodiscard]] const std::string& autoChopMessage() const noexcept { return fAutoMessage; }
    [[nodiscard]] const char* autoChopModeName() const noexcept;

    // Transport (Play/Stop, Loop) and zoom helpers for the header buttons.
    /** Plays the selection, else the selected slice, else the whole sample; stops when playing. */
    bool togglePlay();
    [[nodiscard]] bool playing() const noexcept { return fPreviewPosition > 0.0f; }
    void toggleLoop();
    [[nodiscard]] bool looping() const noexcept { return fLoop; }
    /** Index into displaySlices() of the slice holding the frame, or -1. */
    [[nodiscard]] int sliceIndexAt(std::uint32_t frame) const noexcept;
    /** Selects the 1-based slice number (keys 1-9); false when there is none. */
    bool selectSliceNumber(std::size_t number) noexcept;
    bool zoomFit(Time now) noexcept;
    bool zoomToRange(std::uint32_t start, std::uint32_t end, Time now) noexcept;
    // Scrolls a zoomed view (keeping its width) so [start, end] is visible; no-op if unzoomed.
    bool revealRange(std::uint32_t start, std::uint32_t end, Time now) noexcept;

    // Selection (source frames) on empty waveform.
    void beginSelection(std::uint32_t frame) noexcept;
    void extendSelection(std::uint32_t frame) noexcept;
    void clearSelection() noexcept { fSelectionStart = fSelectionEnd = 0U; }
    [[nodiscard]] bool hasSelection() const noexcept { return fSelectionEnd > fSelectionStart; }
    [[nodiscard]] std::uint32_t selectionStart() const noexcept { return fSelectionStart; }
    [[nodiscard]] std::uint32_t selectionEnd() const noexcept { return fSelectionEnd; }

    // Slice chosen by the last marker press (0 = none); Delete removes it.
    void selectSlice(std::uint16_t sliceId) noexcept
    {
        fSelectedSlice = sliceId;
        if (sliceId != 0U)
            clearSelection();
    }
    [[nodiscard]] std::uint16_t selectedSlice() const noexcept { return fSelectedSlice; }

    // Marker drags: boundary of one slice; a shared boundary moves both slices.
    /** solo: edit only this slice, never the neighbour sharing the boundary (a slice is selected). */
    bool beginMarkerDrag(std::uint16_t sliceId, bool endEdge, bool solo = false) noexcept;
    void dragTo(std::uint32_t frame) noexcept;
    /**
     * Right-drag editing: picks the slice edge (or selection edge) nearest to the frame within
     * the tolerance and starts dragging it with dragTo()/endDrag(). False when none is near.
     */
    bool beginEdgeDrag(std::uint32_t frame, std::uint32_t toleranceFrames) noexcept;
    bool endDrag();
    void cancelDrag() noexcept;
    [[nodiscard]] bool dragging() const noexcept { return fDrag.has_value() || fSelectionEdge != 0; }

    // Viewport over the source (wheel zoom, Shift+wheel or overview pan).
    bool zoom(float delta, float anchorX, sms::ui::Rect bounds, Time now) noexcept;
    bool pan(float delta, Time now) noexcept;
    bool setScrollPosition(float position, Time now) noexcept;

    // View accessors. displaySlices() includes the local drag.
    [[nodiscard]] const plugin::MainSampleStatus& status() const noexcept { return fStatus; }
    [[nodiscard]] const std::vector<plugin::MainSampleSlice>& displaySlices() const noexcept
    { return fDisplaySlices; }
    [[nodiscard]] bool hasSource() const noexcept
    { return fStatus.sourceId != 0U && fStatus.frames != 0U; }
    [[nodiscard]] const sms::ui::waveform::Viewport& viewport() const noexcept
    { return fViewport; }
    [[nodiscard]] const MainSampleWaveformView& overview() const noexcept { return fOverview; }
    [[nodiscard]] const MainSampleWaveformView& detail() const noexcept { return fDetail; }
    /** 0 = idle, else 1 + frame / source frames (kParameterMainSamplePreviewPosition). */
    [[nodiscard]] float previewPosition() const noexcept { return fPreviewPosition; }
    [[nodiscard]] bool confirmPending() const noexcept
    { return fConfirm.has_value() || fAutoConfirm; }
    [[nodiscard]] int sliceForPad(std::uint32_t pad) const noexcept;
    [[nodiscard]] std::uint64_t lastSentSequence() const noexcept { return fSequence; }

private:
    struct Drag {
        std::uint16_t sliceId = 0U;
        bool endEdge = false;
        bool solo = false;
        std::uint32_t original = 0U;
        std::uint32_t frame = 0U;
    };
    struct PendingAssign {
        std::uint32_t pad = 0U;
        std::uint32_t start = 0U;
        std::uint32_t end = 0U;
    };

    void send(plugin::MainSampleRequest request);
    void sendWaveform(std::uint32_t start, std::uint32_t end, std::uint64_t& sequence,
                      Time& deadline, Time now);
    void sourceChanged();
    void rebuildDisplaySlices();
    bool scheduleDetail(bool changed, Time now) noexcept;
    [[nodiscard]] std::uint32_t clampFrame(std::uint32_t frame) const noexcept;

    MainSampleHost& fHost;
    plugin::MainSampleStatus fStatus{};
    std::vector<plugin::MainSampleSlice> fDisplaySlices;
    std::uint64_t fSequence = 0U;          // last MS1 sequence sent
    std::uint64_t fAcceptedSequence = 0U;  // newest status accepted
    bool fOpen = false;
    bool fStarted = false;
    bool fLoop = false;
    std::uint32_t fPlayStart = 0U;
    std::uint32_t fPlayEnd = 0U;
    float fPreviewPosition = 0.0f;
    std::uint32_t fSelectionStart = 0U;
    std::uint32_t fSelectionEnd = 0U;
    std::uint16_t fSelectedSlice = 0U;
    std::size_t fColorPress = 0U;
    bool fCountEditing = false;
    std::string fCountText;
    Time fCountClick{};
    bool fNeutralZones = false;
    std::vector<plugin::ArrangeMove> fArrangeMoves;   // sent, awaiting the reply
    std::uint64_t fArrangeSequence = 0U;
    std::vector<plugin::ArrangeMove> fArrangeConfirmed;
    std::optional<Drag> fDrag;
    int fSelectionEdge = 0; // 1 = start, 2 = end of the selection while right-dragging
    std::optional<PendingAssign> fConfirm;
    std::optional<PendingAssign> fLastAssign;

    sms::ui::waveform::Viewport fViewport{};
    MainSampleWaveformView fOverview{};
    MainSampleWaveformView fDetail{};
    std::uint64_t fOverviewSequence = 0U;  // 0 = not requested for this source
    std::uint64_t fDetailSequence = 0U;
    bool fDetailDirty = false;
    Time fDetailDeadline{};
    Time fOverviewDeadline{};
    plugin::AutoChopMode fAutoMode = plugin::AutoChopMode::transient;
    std::uint32_t fAutoCount = 16U;
    std::uint32_t fAutoFirstPad = 0U;
    std::uint64_t fAutoSequence = 0U;
    bool fAutoConfirm = false;
    std::string fAutoMessage;
    bool fQueryDue = false;
    Time fQueryDeadline{};
};

} // namespace midichopper::ui
