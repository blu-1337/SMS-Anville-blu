#include "UndoController.hpp"

#include "PadColorState.hpp"
#include "PadFingerprint.hpp"
#include "StateCodec.hpp"
#include "StateKeys.hpp"

#include <algorithm>
#include <iterator>

namespace midichopper::plugin {

void UndoSnapshot::addUniqueBytes(std::unordered_set<const void*>& seen, std::size_t& total) const
{
    for (const auto& pad : pads)
        if (pad.audio && seen.insert(pad.audio.get()).second)
            total += pad.audio->stereo.size() * sizeof(float);
}

UndoSnapshot UndoController::capture()
{
    UndoSnapshot snapshot;
    std::array<std::uint64_t, kPadCount> provenance{};
    std::array<std::uint64_t, kPadCount> generations{};
    sampler_.padProvenance(provenance, generations);
    for (std::uint32_t pad = 0; pad < kPadCount; ++pad) {
        auto& slot = snapshot.pads[pad];
        slot.provenance = provenance[pad];
        sampler_.snapshotPadSettings(pad, slot.playback, slot.mixer);
        snapshot.colors[pad] = colors_[pad].load(std::memory_order_relaxed);
        if (!sampler_.padMetadata(pad).occupied)
            continue;
        auto& cached = cache_[pad];
        if (cached.generation == generations[pad] && generations[pad] != 0U) {
            if (auto audio = cached.audio.lock()) {
                slot.audio = std::move(audio);
                slot.fingerprint = cached.fingerprint;
                continue;
            }
        }
        PadData data;
        if (!sampler_.exportPublishedPad(pad, data) || data.frames == 0U)
            continue;
        slot.fingerprint = fingerprint(data);
        const auto key = slot.fingerprint ^ (static_cast<std::uint64_t>(data.frames) << 40U);
        auto& shared = dedupe_[key];
        slot.audio = shared.lock();
        if (!slot.audio || slot.audio->frames != data.frames || slot.audio->stereo != data.stereo) {
            slot.audio = std::make_shared<const PadData>(std::move(data));
            shared = slot.audio;
        }
        cached = {generations[pad], slot.audio, slot.fingerprint};
    }
    snapshot.session = main_.session();
    if (dedupe_.size() > 4U * kPadCount) {
        for (auto it = dedupe_.begin(); it != dedupe_.end();)
            it = it->second.expired() ? dedupe_.erase(it) : std::next(it);
    }
    return snapshot;
}

bool UndoController::sameState(const UndoSnapshot& a, const UndoSnapshot& b)
{
    if (a.colors != b.colors || encodeMainSampleSession(a.session) != encodeMainSampleSession(b.session))
        return false;
    for (std::uint32_t pad = 0; pad < kPadCount; ++pad) {
        const auto& x = a.pads[pad];
        const auto& y = b.pads[pad];
        if (static_cast<bool>(x.audio) != static_cast<bool>(y.audio) || x.fingerprint != y.fingerprint ||
            x.provenance != y.provenance ||
            (x.audio && y.audio && x.audio->frames != y.audio->frames) ||
            encodePlaybackSettings(x.playback) != encodePlaybackSettings(y.playback) ||
            encodeMixerSettings(x.mixer) != encodeMixerSettings(y.mixer))
            return false;
    }
    return true;
}

void UndoController::step(const bool undo)
{
    const std::lock_guard lock(mutex_);
    auto current = capture();
    auto target = undo ? history_.undo(current) : history_.redo(current);
    if (!target) {
        publishLocked();
        return;
    }
    MainSampleWorkflows::HistoryChange change;
    change.session = &target->session;
    for (std::uint32_t pad = 0; pad < kPadCount; ++pad) {
        const auto& t = target->pads[pad];
        const auto& c = current.pads[pad];
        const bool audioDiffers = static_cast<bool>(t.audio) != static_cast<bool>(c.audio) ||
            t.fingerprint != c.fingerprint ||
            (t.audio && c.audio && t.audio->frames != c.audio->frames);
        const bool settingsDiffer =
            encodePlaybackSettings(t.playback) != encodePlaybackSettings(c.playback) ||
            encodeMixerSettings(t.mixer) != encodeMixerSettings(c.mixer);
        if (audioDiffers) {
            PadImport import;
            import.pad = pad;
            import.audio = t.audio.get();
            import.playback = t.playback;
            import.mixer = t.mixer;
            import.provenance = t.provenance;
            change.imports.push_back(import);
        } else {
            if (settingsDiffer)
                change.settings.push_back({pad, t.playback, t.mixer});
            if (t.provenance != c.provenance)
                change.links.push_back({pad, t.provenance, c.fingerprint});
        }
        if (audioDiffers || settingsDiffer || t.provenance != c.provenance)
            change.affected |= (1ULL << pad);
    }
    if (!main_.applyHistory(change)) {
        history_.putBackUndo(std::move(*target), undo);
        publishLocked();
        return;
    }
    for (std::uint32_t pad = 0; pad < kPadCount; ++pad)
        colors_[pad].store(target->colors[pad], std::memory_order_relaxed);
    sampler_.setFavoriteMask(favoriteMaskOf(colors_));
    lastKind_ = Kind::action;
    publishLocked();
}

void UndoController::hostRestored(const Clock::time_point now)
{
    const std::lock_guard lock(mutex_);
    restoreStamp_ = now;
    history_.clear();
    cache_ = {};
    dedupe_.clear();
    publishLocked();
}

void UndoController::clear()
{
    const std::lock_guard lock(mutex_);
    history_.clear();
    publishLocked();
}

std::size_t UndoController::undoDepth() const
{
    const std::lock_guard lock(mutex_);
    return history_.undoDepth();
}

std::size_t UndoController::redoDepth() const
{
    const std::lock_guard lock(mutex_);
    return history_.redoDepth();
}

void UndoController::publishLocked()
{
    std::string text = "UD1;" + std::to_string(++sequence_) + ';' +
        std::to_string(history_.undoDepth()) + ';' + std::to_string(history_.redoDepth()) + ';';
    for (std::uint32_t pad = 0; pad < kPadCount; ++pad)
    {
        const int value = colors_[pad].load(std::memory_order_relaxed);
        const int index = std::clamp(value & 255, 0, 9);
        text.push_back(static_cast<char>(((value & kPadFavoriteBit) != 0 ? 'a' : '0') + index));
    }
    observer_.publishUiState(kUndoStatusKey, text.c_str());
}

} // namespace midichopper::plugin
