#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <unordered_set>
#include <utility>

namespace midichopper::plugin {

/**
 * Bounded undo/redo stacks of whole-state snapshots (control thread only; no
 * audio-thread use). A snapshot type provides
 *   void addUniqueBytes(std::unordered_set<const void*>& seen, std::size_t& total) const
 * which adds the bytes of every shared block it holds that is not yet in `seen`,
 * so audio shared between neighbouring steps is counted once.
 * push() starts a new branch: redo is dropped. The oldest step is evicted when
 * the step count or the byte budget is exceeded (the newest step is always kept).
 */
template <class Snapshot>
class UndoHistory {
public:
    UndoHistory(const std::size_t maxSteps, const std::size_t maxBytes) noexcept
        : maxSteps_(maxSteps == 0U ? 1U : maxSteps), maxBytes_(maxBytes) {}

    /** Record the state as it was before a user edit. */
    void push(Snapshot before)
    {
        redo_.clear();
        undo_.push_back(std::move(before));
        trim();
    }

    /** `current` is kept for redo. Empty when there is nothing to undo. */
    [[nodiscard]] std::optional<Snapshot> undo(Snapshot current)
    {
        if (undo_.empty())
            return std::nullopt;
        Snapshot target = std::move(undo_.back());
        undo_.pop_back();
        redo_.push_back(std::move(current));
        trim();
        return target;
    }

    [[nodiscard]] std::optional<Snapshot> redo(Snapshot current)
    {
        if (redo_.empty())
            return std::nullopt;
        Snapshot target = std::move(redo_.back());
        redo_.pop_back();
        undo_.push_back(std::move(current));
        trim();
        return target;
    }

    /** Re-adds a target that could not be applied, so a failed undo/redo loses nothing. */
    void putBackUndo(Snapshot target, const bool wasUndo)
    {
        if (wasUndo) {
            if (!redo_.empty()) redo_.pop_back();
            undo_.push_back(std::move(target));
        } else {
            if (!undo_.empty()) undo_.pop_back();
            redo_.push_back(std::move(target));
        }
    }

    void clear() noexcept { undo_.clear(); redo_.clear(); }
    [[nodiscard]] std::size_t undoDepth() const noexcept { return undo_.size(); }
    [[nodiscard]] std::size_t redoDepth() const noexcept { return redo_.size(); }
    [[nodiscard]] bool canUndo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !redo_.empty(); }

    [[nodiscard]] std::size_t totalBytes() const
    {
        std::unordered_set<const void*> seen;
        std::size_t total = 0U;
        for (const auto& snapshot : undo_) snapshot.addUniqueBytes(seen, total);
        for (const auto& snapshot : redo_) snapshot.addUniqueBytes(seen, total);
        return total;
    }

private:
    void trim()
    {
        while (undo_.size() + redo_.size() > maxSteps_)
            dropOldest();
        while (undo_.size() + redo_.size() > 1U && totalBytes() > maxBytes_)
            dropOldest();
    }

    void dropOldest()
    {
        if (!undo_.empty()) undo_.pop_front();
        else if (!redo_.empty()) redo_.pop_front();
    }

    std::size_t maxSteps_;
    std::size_t maxBytes_;
    std::deque<Snapshot> undo_;
    std::deque<Snapshot> redo_;
};

} // namespace midichopper::plugin
