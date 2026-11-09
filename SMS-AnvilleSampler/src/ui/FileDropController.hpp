#pragma once

#include "PadActionController.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace midichopper::ui {

/** What a drop point is over. Main Sample targets are rejected until that view exists. */
struct DropTarget {
    enum class Kind : std::uint8_t { none, pad, mainSample };
    Kind kind = Kind::none;
    std::uint32_t globalPad = kPadCount;
};

/** Adapter projection read for each drag event; the controller keeps no copy. */
struct DropContext {
    PadActionContext pads;
    bool captureActive = false;
    bool chopEditorActive = false;
    bool padActionsBusy = false;
};

/** Adapter effects. releaseDroppedFile receives every path once it is no longer needed. */
class FileDropHost {
public:
    virtual ~FileDropHost() = default;
    virtual void setFileDropStatus(const char* message) = 0;
    virtual void repaintFileDrop() = 0;
    virtual void releaseDroppedFile(const std::string& utf8Path) = 0;
};

/** Turns dropped WAV paths into one-at-a-time pad imports through
 * PadActionController::importFile. Files fill consecutive visible pads from the
 * drop pad across banks. Event precedence stays with the input controller.
 */
class FileDropController {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t kMaximumFiles = kPadCount;
    static constexpr auto kConfirmationTimeout = std::chrono::seconds(5);

    explicit FileDropController(FileDropHost& host) noexcept : fHost(host) {}

    /** A visible pad of the current bank, or none outside the grid. */
    [[nodiscard]] static DropTarget padTarget(const PadActionContext&, int localPad) noexcept;

    /** Updates the highlight and returns whether a drop here would be accepted. */
    bool hover(DropTarget, std::span<const std::string> utf8Paths, const DropContext&);
    void leave();
    /** Queues the supported paths; every path is released through the host eventually. */
    void enqueue(DropTarget, std::vector<std::string> utf8Paths, const DropContext&,
                 Clock::time_point now = Clock::now());
    void idle(PadActionController&, const PadActionContext&, Clock::time_point now = Clock::now());
    /** Releases queued and pending paths. The in-flight file is released by idle or the destructor. */
    void cancel();

    [[nodiscard]] std::optional<std::uint32_t> highlightedPad() const noexcept { return fHighlight; }
    [[nodiscard]] bool active() const noexcept { return fInFlight || !fQueue.empty(); }
    [[nodiscard]] bool confirmationPending() const noexcept { return !fPending.empty(); }

    ~FileDropController();
    FileDropController(const FileDropController&) = delete;
    FileDropController& operator=(const FileDropController&) = delete;

private:
    struct Item {
        std::uint32_t pad = kPadCount;
        std::string path;
    };

    /** nullptr when a drop is accepted, else the status explaining why not. */
    [[nodiscard]] const char* rejection(DropTarget, std::size_t supportedCount, const DropContext&) const noexcept;
    [[nodiscard]] static bool mayReplaceOtherPads(const PadActionContext&, std::span<const Item>) noexcept;
    void setHighlight(std::optional<std::uint32_t>);
    void release(std::vector<std::string>& paths);
    void finishBatch();

    FileDropHost& fHost;
    std::optional<std::uint32_t> fHighlight;
    std::deque<Item> fQueue;
    bool fInFlight = false;
    std::string fInFlightPath;
    std::size_t fBatchTotal = 0;
    std::size_t fBatchLoaded = 0;
    std::vector<std::string> fPending;
    std::uint32_t fPendingPad = kPadCount;
    std::size_t fPendingCount = 0;
    Clock::time_point fPendingDeadline{};
};

} // namespace midichopper::ui
