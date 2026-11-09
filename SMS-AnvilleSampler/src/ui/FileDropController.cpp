#include "FileDropController.hpp"

#include "FileDropEffect.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace midichopper::ui {

DropTarget FileDropController::padTarget(const PadActionContext& context, const int localPad) noexcept
{
    if (localPad < 0 || localPad >= context.visibleCount || context.firstPad < 0)
        return {};
    const auto pad = static_cast<std::uint32_t>(context.firstPad + localPad);
    return pad < kPadCount ? DropTarget{DropTarget::Kind::pad, pad} : DropTarget{};
}

const char* FileDropController::rejection(const DropTarget target, const std::size_t supportedCount,
                                          const DropContext& context) const noexcept
{
    if (supportedCount == 0U)
        return "Only WAV files can be dropped";
    if (target.kind != DropTarget::Kind::pad || target.globalPad >= kPadCount)
        return "Drop WAV files onto a pad";
    if (context.pads.armed || context.captureActive)
        return "Disarm recording to drop files";
    if (context.chopEditorActive)
        return "Close the cut editor to drop files";
    if (context.padActionsBusy || active())
        return "Wait for the current pad action";
    return nullptr;
}

bool FileDropController::hover(const DropTarget target, const std::span<const std::string> utf8Paths,
                               const DropContext& context)
{
    const auto supported = static_cast<std::size_t>(std::count_if(
        utf8Paths.begin(), utf8Paths.end(),
        [](const std::string& path) { return sms::ui::isSupportedAudioPath(path); }));
    const bool accepted = rejection(target, supported, context) == nullptr;
    setHighlight(accepted ? std::optional<std::uint32_t>(target.globalPad) : std::nullopt);
    return accepted;
}

void FileDropController::leave()
{
    setHighlight(std::nullopt);
}

void FileDropController::enqueue(const DropTarget target, std::vector<std::string> utf8Paths,
                                 const DropContext& context, const Clock::time_point now)
{
    setHighlight(std::nullopt);
    std::vector<std::string> supported;
    std::vector<std::string> unused;
    for (auto& path : utf8Paths)
        (sms::ui::isSupportedAudioPath(path) ? supported : unused).push_back(std::move(path));
    release(unused);

    if (const char* reason = rejection(target, supported.size(), context)) {
        release(supported);
        fHost.setFileDropStatus(reason);
        return;
    }

    const auto total = supported.size();
    const auto padsPerBank = static_cast<std::uint8_t>(context.pads.visibleCount);
    std::vector<Item> items;
    items.reserve(std::min(total, kMaximumFiles));
    for (std::size_t index = 0; index < total && index < kMaximumFiles; ++index) {
        const auto pad = advanceVisiblePad(target.globalPad, static_cast<std::uint32_t>(index),
                                           padsPerBank, context.pads.bankMode);
        if (pad >= kPadCount)
            break;
        items.push_back({pad, std::move(supported[index])});
    }
    // Files past the last visible pad (or the 64-file cap) are never imported.
    supported.erase(supported.begin(), supported.begin() + static_cast<std::ptrdiff_t>(items.size()));
    release(supported);

    const bool confirmed = !fPending.empty() && fPendingPad == target.globalPad &&
                           fPendingCount == total && now < fPendingDeadline;
    release(fPending);
    if (items.size() > 1U && !confirmed && mayReplaceOtherPads(context.pads, items)) {
        for (auto& item : items)
            fPending.push_back(std::move(item.path));
        fPendingPad = target.globalPad;
        fPendingCount = total;
        fPendingDeadline = now + kConfirmationTimeout;
        fHost.setFileDropStatus("Pads in use: drop the same files again within 5 s to replace");
        return;
    }

    for (auto& item : items)
        fQueue.push_back(std::move(item));
    fBatchTotal = total;
    fBatchLoaded = 0;
    if (total > 1U) {
        char message[64];
        std::snprintf(message, sizeof(message), "Loading %zu files...", total);
        fHost.setFileDropStatus(message);
    }
}

void FileDropController::idle(PadActionController& actions, const PadActionContext& context,
                              const Clock::time_point now)
{
    if (!fPending.empty() && now >= fPendingDeadline) {
        release(fPending);
        fHost.setFileDropStatus("File drop cancelled");
    }
    if (fInFlight) {
        if (actions.importInFlight())
            return;
        if (actions.lastImportSucceeded())
            ++fBatchLoaded;
        fHost.releaseDroppedFile(fInFlightPath);
        fInFlightPath.clear();
        fInFlight = false;
    }
    if (!fQueue.empty()) {
        if (context.armed) {
            // Recording takes over the pads; stop rather than race the capture target.
            std::vector<std::string> remaining;
            for (auto& item : fQueue)
                remaining.push_back(std::move(item.path));
            fQueue.clear();
            release(remaining);
            finishBatch();
            return;
        }
        if (actions.busy() || actions.importInFlight() || actions.dialogPending())
            return;
        auto item = std::move(fQueue.front());
        fQueue.pop_front();
        if (actions.importFile(item.pad, item.path)) {
            fInFlight = true;
            fInFlightPath = std::move(item.path);
        } else {
            fHost.releaseDroppedFile(item.path);
        }
        return;
    }
    if (fBatchTotal > 0U)
        finishBatch();
}

void FileDropController::cancel()
{
    std::vector<std::string> remaining;
    for (auto& item : fQueue)
        remaining.push_back(std::move(item.path));
    fQueue.clear();
    release(remaining);
    release(fPending);
}

FileDropController::~FileDropController()
{
    // The in-flight file may still be read by the plugin's worker; a private
    // copy left behind is removed by the next editor's stale-copy sweep.
    cancel();
}

bool FileDropController::mayReplaceOtherPads(const PadActionContext& context,
                                             const std::span<const Item> items) noexcept
{
    for (std::size_t index = 1; index < items.size(); ++index) {
        const auto local = static_cast<long long>(items[index].pad) - context.firstPad;
        // Occupancy is known only for the current bank's visible pads.
        if (local < 0 || local >= context.visibleCount ||
            static_cast<std::size_t>(local) >= context.padState.size())
            return true;
        const char state = context.padState[static_cast<std::size_t>(local)];
        if (state != '0' && state != '.')
            return true;
    }
    return false;
}

void FileDropController::setHighlight(const std::optional<std::uint32_t> highlight)
{
    if (fHighlight == highlight)
        return;
    fHighlight = highlight;
    fHost.repaintFileDrop();
}

void FileDropController::release(std::vector<std::string>& paths)
{
    for (const auto& path : paths)
        fHost.releaseDroppedFile(path);
    paths.clear();
}

void FileDropController::finishBatch()
{
    // A single file keeps PadActionController's own success or failure status.
    if (fBatchTotal > 1U) {
        char message[64];
        if (fBatchLoaded == fBatchTotal)
            std::snprintf(message, sizeof(message), "Loaded %zu files", fBatchTotal);
        else
            std::snprintf(message, sizeof(message), "Loaded %zu of %zu files", fBatchLoaded, fBatchTotal);
        fHost.setFileDropStatus(message);
    }
    fBatchTotal = 0;
    fBatchLoaded = 0;
}

} // namespace midichopper::ui
