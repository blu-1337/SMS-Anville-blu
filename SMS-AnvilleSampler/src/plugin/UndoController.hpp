#pragma once

#include "MainSampleWorkflows.hpp"
#include "UndoHistory.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace midichopper::plugin {

/** One pad inside an undo snapshot; audio is shared and immutable. */
struct UndoPad {
    std::shared_ptr<const PadData> audio; // null = empty pad
    std::uint64_t fingerprint = 0U;
    std::uint64_t provenance = 0U;
    sms::dsp::SamplePlaybackSettings playback{};
    sms::dsp::SampleMixerSettings mixer{};
};

struct UndoSnapshot {
    std::array<UndoPad, kPadCount> pads{};
    MainSampleSession session;
    std::array<int, kPadCount> colors{};
    void addUniqueBytes(std::unordered_set<const void*>& seen, std::size_t& total) const;
};

/**
 * Control-thread undo/redo of pad audio, pad settings, colours and Main Sample
 * slices (design: handoff/PROGRESS.md). Never touched by the audio thread.
 * Restores go through MainSampleWorkflows::applyHistory (one atomic importPads).
 */
class UndoController {
public:
    static constexpr std::size_t kMaxSteps = 50U;
    static constexpr std::size_t kMaxBytes = 256U * 1024U * 1024U;
    static constexpr auto kCoalesce = std::chrono::milliseconds(500);
    static constexpr auto kRestoreQuiet = std::chrono::milliseconds(2000);

    enum class Kind { settings, colors, action };
    using Clock = std::chrono::steady_clock;

    UndoController(SamplerEngine& sampler, MainSampleWorkflows& main, PadWorkflowObserver& observer,
                   std::array<std::atomic<int>, kPadCount>& colors,
                   std::size_t maxSteps = kMaxSteps, std::size_t maxBytes = kMaxBytes)
        : sampler_(sampler), main_(main), observer_(observer), colors_(colors),
          history_(maxSteps, maxBytes) {}

    /** Capture, run `apply`, and keep the step (actions: only if the state changed). */
    template <class F>
    void record(const Kind kind, const Clock::time_point now, F&& apply)
    {
        const std::lock_guard lock(mutex_);
        if (kind == Kind::action) {
            auto before = capture();
            apply();
            if (!sameState(before, capture())) {
                history_.push(std::move(before));
                lastKind_ = Kind::action;
                publishLocked();
            }
            return;
        }
        if (restoreStamp_ != Clock::time_point{} && now - restoreStamp_ < kRestoreQuiet) {
            apply();
            return;
        }
        const bool coalesce = lastKind_ == kind && now - lastEdit_ < kCoalesce;
        lastEdit_ = now;
        lastKind_ = kind;
        const bool pushStep = !coalesce;
        if (pushStep)
            history_.push(capture());
        apply();
        // Publish AFTER the edit: the UI replaces its pad colours from this status, so a
        // pre-edit publish wiped the colours it had just set locally (first AUTO COLOR).
        if (pushStep)
            publishLocked();
    }
    /**
     * Live drag edits: 'B' captures the before-state, 'U' only applies, 'E' applies and
     * pushes ONE step if the state differs from the 'B' capture, 'S' is a plain action.
     * A stray 'U'/'E' without 'B' degrades to a plain action.
     */
    template <class F>
    void recordGesture(const char phase, const Clock::time_point now, F&& apply)
    {
        if (phase == 'S' || ((phase == 'U' || phase == 'E') && !gestureActive_)) {
            record(Kind::action, now, std::forward<F>(apply));
            return;
        }
        const std::lock_guard lock(mutex_);
        if (phase == 'B') {
            gestureBefore_ = capture();
            gestureActive_ = true;
        }
        apply();
        if (phase == 'E') {
            gestureActive_ = false;
            if (!sameState(gestureBefore_, capture())) {
                history_.push(std::move(gestureBefore_));
                lastKind_ = Kind::action;
                publishLocked();
            }
            gestureBefore_ = UndoSnapshot{};
        }
    }
    void undo() { step(true); }
    void redo() { step(false); }
    /** Host restored durable state: history no longer describes it. */
    void hostRestored(Clock::time_point now);
    void clear();
    [[nodiscard]] std::size_t undoDepth() const;
    [[nodiscard]] std::size_t redoDepth() const;

private:
    [[nodiscard]] UndoSnapshot capture();
    [[nodiscard]] static bool sameState(const UndoSnapshot& a, const UndoSnapshot& b);
    void step(bool undo);
    void publishLocked();

    struct CacheEntry {
        std::uint64_t generation = 0U;
        std::weak_ptr<const PadData> audio;
        std::uint64_t fingerprint = 0U;
    };
    SamplerEngine& sampler_;
    MainSampleWorkflows& main_;
    PadWorkflowObserver& observer_;
    std::array<std::atomic<int>, kPadCount>& colors_;
    mutable std::mutex mutex_;
    UndoHistory<UndoSnapshot> history_;
    std::array<CacheEntry, kPadCount> cache_{};
    std::unordered_map<std::uint64_t, std::weak_ptr<const PadData>> dedupe_;
    UndoSnapshot gestureBefore_;
    bool gestureActive_ = false;
    Kind lastKind_ = Kind::action;
    Clock::time_point lastEdit_{};
    Clock::time_point restoreStamp_{};
    std::uint64_t sequence_ = 0U;
};

} // namespace midichopper::plugin
