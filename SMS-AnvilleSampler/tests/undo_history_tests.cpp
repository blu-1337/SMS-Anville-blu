#include "UndoHistory.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

namespace {
using midichopper::plugin::UndoHistory;

void check(const bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct Snap {
    int value = 0;
    std::shared_ptr<std::vector<float>> block;
    void addUniqueBytes(std::unordered_set<const void*>& seen, std::size_t& total) const
    {
        if (block && seen.insert(block.get()).second) total += block->size() * sizeof(float);
    }
};

void pushUndoRedo()
{
    UndoHistory<Snap> history(50U, 1U << 30U);
    check(!history.canUndo() && !history.canRedo(), "starts empty");
    history.push({1, {}});
    history.push({2, {}});
    check(history.undoDepth() == 2U, "two steps");
    auto target = history.undo({3, {}});
    check(target && target->value == 2 && history.redoDepth() == 1U, "undo returns newest step");
    target = history.undo({2, {}});
    check(target && target->value == 1 && history.undoDepth() == 0U && history.redoDepth() == 2U,
          "second undo");
    check(!history.undo({1, {}}), "nothing left to undo");
    target = history.redo({1, {}});
    check(target && target->value == 2, "redo returns the undone state");
    target = history.redo({2, {}});
    check(target && target->value == 3 && !history.canRedo(), "redo back to the newest state");
    check(!history.redo({3, {}}), "nothing left to redo");
}

void branchTruncates()
{
    UndoHistory<Snap> history(50U, 1U << 30U);
    history.push({1, {}});
    history.push({2, {}});
    static_cast<void>(history.undo({3, {}}));
    check(history.canRedo(), "redo available");
    history.push({9, {}});
    check(!history.canRedo() && history.undoDepth() == 2U, "a new edit drops redo");
}

void stepCap()
{
    UndoHistory<Snap> history(3U, 1U << 30U);
    for (int i = 1; i <= 5; ++i) history.push({i, {}});
    check(history.undoDepth() == 3U, "step cap");
    auto target = history.undo({6, {}});
    check(target && target->value == 5, "newest kept");
    static_cast<void>(history.undo({5, {}}));
    target = history.undo({4, {}});
    check(target && target->value == 3 && !history.canUndo(), "oldest steps were evicted");
}

void byteCap()
{
    UndoHistory<Snap> history(50U, 5000U * sizeof(float));
    auto shared = std::make_shared<std::vector<float>>(4000U);
    history.push({1, shared});
    history.push({2, shared});
    check(history.undoDepth() == 2U && history.totalBytes() == 4000U * sizeof(float),
          "shared blocks are counted once");
    history.push({3, std::make_shared<std::vector<float>>(4000U)});
    check(history.undoDepth() == 1U, "oldest evicted when the byte budget is exceeded");
    history.push({4, std::make_shared<std::vector<float>>(9000U)});
    check(history.undoDepth() == 1U, "the newest step is always kept");
}

void failedApplyLosesNothing()
{
    UndoHistory<Snap> history(50U, 1U << 30U);
    history.push({1, {}});
    auto target = history.undo({2, {}});
    history.putBackUndo(std::move(*target), true);
    check(history.undoDepth() == 1U && history.redoDepth() == 0U, "undo put back");
    target = history.undo({2, {}});
    target = history.redo({1, {}});
    history.putBackUndo(std::move(*target), false);
    check(history.redoDepth() == 1U && history.undoDepth() == 0U, "redo put back");
}
} // namespace

int main()
{
    pushUndoRedo();
    branchTruncates();
    stepCap();
    byteCap();
    failedApplyLosesNothing();
    std::cout << "Undo history tests passed\n";
}
