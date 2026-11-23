#include "PadColorState.hpp"
#include "MainSampleWorkflows.hpp"
#include "ZeroCrossing.hpp"

#include "Analysis/SliceStrategies.hpp"
#include "Analysis/TempoEstimate.hpp"
#include "PadFileActions.hpp"
#include "PadFingerprint.hpp"
#include "StateCodec.hpp"
#include "StateKeys.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <bit>
#include <filesystem>
#include <utility>

namespace midichopper::plugin {
namespace {

constexpr std::uint64_t kSliceMask = 0xffffU;

[[nodiscard]] std::filesystem::path pathFromUtf8(const std::string_view path)
{
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(path.data()), path.size()));
}

[[nodiscard]] std::string fileName(const std::string_view path)
{
    const auto name = pathFromUtf8(path).filename().u8string();
    return {reinterpret_cast<const char*>(name.data()), name.size()};
}

[[nodiscard]] PadData padData(sms::audio::WavAudio&& audio)
{
    PadData data;
    data.sampleRate = audio.sampleRate;
    data.frames = audio.frames;
    data.stereo = std::move(audio.stereo);
    double sumSquares = 0.0;
    for (const float sample : data.stereo) {
        data.peak = std::max(data.peak, std::abs(sample));
        sumSquares += static_cast<double>(sample) * sample;
    }
    if (!data.stereo.empty())
        data.rms = static_cast<float>(
            std::sqrt(sumSquares / static_cast<double>(data.stereo.size())));
    return data;
}

[[nodiscard]] constexpr std::uint64_t link(const std::uint64_t sourceId,
                                           const std::uint16_t sliceId) noexcept
{
    return (sourceId << 16U) | sliceId;
}

} // namespace

MainSampleSession MainSampleWorkflows::session() const
{
    const std::lock_guard lock(mutationMutex_);
    return session_;
}

std::string MainSampleWorkflows::saveAudio() const
{
    const std::lock_guard lock(mutationMutex_);
    if (session_.empty()) {
        audioCache_ = {};
        audioCacheId_ = 0U;
        return {};
    }
    if (audioCacheId_ != session_.sourceId) {
        // Free the stale copy first: a long source is large in Base64 too.
        audioCache_ = {};
        audioCacheId_ = 0U;
        PadData source;
        std::uint64_t id = 0U;
        if (!sampler_.exportSource(source, &id) || id != session_.sourceId || source.frames == 0U)
            return {};
        const auto rate = static_cast<std::uint32_t>(std::clamp(source.sampleRate, 1.0, 384000.0));
        audioCache_ = encodePadState(source, rate);
        audioCacheId_ = id;
    }
    return audioCache_;
}

std::string MainSampleWorkflows::saveSession() const
{
    const std::lock_guard lock(mutationMutex_);
    return encodeMainSampleSession(session_);
}

std::string MainSampleWorkflows::saveLinks() const
{
    const std::lock_guard lock(mutationMutex_);
    if (session_.empty())
        return {};
    const auto pads = snapshotPads();
    std::vector<MainSampleLinkRecord> links;
    for (std::uint32_t pad = 0; pad < kPadCount; ++pad) {
        const auto provenance = pads.provenance[pad];
        if (provenance == 0U || (provenance >> 16U) != session_.sourceId ||
            session_.find(static_cast<std::uint16_t>(provenance & kSliceMask)) == nullptr)
            continue;
        MainSampleLinkRecord record{pad, provenance, 0U, 0U};
        record.fingerprint = sampler_.padAudioFingerprint(pad, &record.frames);
        if (record.frames != 0U)
            links.push_back(record);
    }
    return links.empty() ? std::string() : encodeMainSampleLinks(links);
}

void MainSampleWorkflows::restoreAudio(const char* const value)
{
    if (value == nullptr || value[0] == '\0') {
        const std::lock_guard lock(mutationMutex_);
        clearRestoredSourceLocked();
        return;
    }
    // Decode outside the mutex, like pad restores.
    DecodedPadState decoded;
    if (!decodePadState(value, decoded) || decoded.pad.frames == 0U)
        return;
    StagedAudio staged{std::move(decoded.pad), decoded.sourceSampleRate, value};
    const std::lock_guard lock(mutationMutex_);
    stagedAudio_ = std::move(staged);
    applyStagedSourceLocked();
}

void MainSampleWorkflows::restoreSession(const char* const value)
{
    const std::lock_guard lock(mutationMutex_);
    if (value == nullptr || value[0] == '\0') {
        clearRestoredSourceLocked();
        return;
    }
    MainSampleSession session;
    if (!decodeMainSampleSession(value, session))
        return;
    stagedSession_ = std::move(session);
    applyStagedSourceLocked();
}

void MainSampleWorkflows::restoreLinks(const char* const value)
{
    const std::lock_guard lock(mutationMutex_);
    std::vector<MainSampleLinkRecord> links;
    if (value != nullptr && value[0] != '\0' && !decodeMainSampleLinks(value, links))
        return;
    stagedLinks_ = std::move(links);
    // Index order (LV2) restores the pads first; tag those whose audio matches.
    // Pads restored later are tagged by restorePad from the staged entries.
    bool tagged = false;
    for (const auto& link : stagedLinks_)
        tagged = sampler_.setPadProvenance(link.pad, link.provenance, link.fingerprint) || tagged;
    if (tagged && !session_.empty())
        publishStatusLocked(0U, MainSampleState::ready);
}

bool MainSampleWorkflows::applyHistory(const HistoryChange& change)
{
    const std::lock_guard lock(mutationMutex_);
    // Undo/redo of a destructive cut reinstalls the other source first; a refused pad
    // transaction puts the current one back, so a failed step changes nothing.
    const bool swapSource = change.source != nullptr && change.session != nullptr &&
                            change.session->sourceId != session_.sourceId;
    PadData previousSource;
    std::uint64_t previousId = 0U;
    if (swapSource) {
        if (!sampler_.exportSource(previousSource, &previousId))
            previousId = 0U;
        if (!sampler_.setSource(change.source.get(), change.session->sourceId))
            return false;
    }
    if (!change.imports.empty() && !sampler_.importPads(change.imports, nullptr)) {
        if (swapSource && previousId != 0U)
            static_cast<void>(sampler_.setSource(&previousSource, previousId));
        return false;
    }
    for (const auto& item : change.settings)
        sampler_.restorePadEditorSettings(item.pad, item.playback, item.mixer);
    for (const auto& item : change.links)
        static_cast<void>(sampler_.setPadProvenance(item.pad, item.provenance, item.fingerprint));
    if (change.session != nullptr && (swapSource || (!session_.empty() && change.session->sourceId == session_.sourceId)))
        session_ = *change.session;
    if (swapSource) {
        audioCache_ = {};
        audioCacheId_ = 0U;
    }
    publishStatusLocked(0U, MainSampleState::ready, {}, change.affected);
    return true;
}

void MainSampleWorkflows::restorePad(const std::uint32_t pad, const PadData* const audio)
{
    const std::lock_guard lock(mutationMutex_);
    std::uint64_t provenance = 0U;
    if (audio != nullptr) {
        const auto link = std::find_if(stagedLinks_.begin(), stagedLinks_.end(),
            [pad](const MainSampleLinkRecord& record) { return record.pad == pad; });
        // An edited or different pad drops its link quietly.
        if (link != stagedLinks_.end() && link->frames == audio->frames &&
            fingerprint(*audio) == link->fingerprint)
            provenance = link->provenance;
    }
    static_cast<void>(sampler_.restorePad(pad, audio, provenance));
}

void MainSampleWorkflows::applyStagedSourceLocked()
{
    if (!stagedAudio_ || !stagedSession_)
        return;
    auto staged = std::move(*stagedAudio_);
    auto session = std::move(*stagedSession_);
    stagedAudio_.reset();
    stagedSession_.reset();
    if (staged.audio.frames != session.frames || staged.sampleRate != session.sampleRate)
        return; // Not a pair; keep the current source.
    if (!sampler_.restoreSource(&staged.audio, session.sourceId))
        return;
    // Slices index stored frames; a source stored at another rate cannot use them.
    const auto info = sampler_.sourceInfo();
    if (info.frames != session.frames ||
        static_cast<std::uint32_t>(std::lround(info.sampleRate)) != session.sampleRate) {
        clearRestoredSourceLocked();
        return;
    }
    session_ = std::move(session);
    audioCache_ = std::move(staged.encoded);
    audioCacheId_ = session_.sourceId;
    publishStatusLocked(0U, MainSampleState::ready);
}

void MainSampleWorkflows::clearRestoredSourceLocked()
{
    stagedAudio_.reset();
    stagedSession_.reset();
    audioCache_ = {};
    audioCacheId_ = 0U;
    if (session_.empty() && sampler_.sourceInfo().id == 0U)
        return;
    static_cast<void>(sampler_.restoreSource(nullptr, 0U));
    session_.reset(0U, 0U, 0U, {});
    publishStatusLocked(0U, MainSampleState::ready);
}

void MainSampleWorkflows::handleRequest(const std::string_view encoded)
{
    MainSampleRequest request;
    if (!decodeMainSampleRequest(encoded, request)) {
        publishStatus(request.sequence, MainSampleState::error, "Invalid main sample request");
        return;
    }
    try {
        handleRequestImpl(request);
    } catch (...) {
        publishStatus(request.sequence, MainSampleState::error, "Main sample action failed");
    }
}

void MainSampleWorkflows::handleRequestImpl(const MainSampleRequest& request)
{
    switch (request.verb) {
    case MainSampleVerb::load: load(request); return;
    case MainSampleVerb::clear: clear(request); return;
    case MainSampleVerb::query: publishStatus(request.sequence, MainSampleState::ready); return;
    case MainSampleVerb::edit: edit(request); return;
    case MainSampleVerb::assign: assign(request); return;
    case MainSampleVerb::remove: remove(request); return;
    case MainSampleVerb::waveform: waveform(request); return;
    case MainSampleVerb::spectrogram: spectrogram(request); return;
    case MainSampleVerb::arrange: arrange(request); return;
    case MainSampleVerb::range: setRange(request); return;
    case MainSampleVerb::cut: cutSource(request); return;
    case MainSampleVerb::overlap: removeOverlaps(request); return;
    }
}

void MainSampleWorkflows::load(const MainSampleRequest& request)
{
    latestLoadSequence_.store(request.sequence, std::memory_order_release);
    const auto name = fileName(request.path);
    publishStatus(request.sequence, MainSampleState::loading, name);
    // Decode without the mutation mutex: pad workflows and host saves keep
    // running while a long file is read.
    auto loaded = readPadWav(pathFromUtf8(request.path));
    if (!loaded) {
        publishStatus(request.sequence, MainSampleState::error, loaded.error);
        return;
    }
    auto audio = padData(std::move(loaded.audio));
    const std::lock_guard lock(mutationMutex_);
    if (latestLoadSequence_.load(std::memory_order_acquire) != request.sequence)
        return; // A newer load or clear owns the reply.
    const auto id = nextSourceId(session_.sourceId);
    if (!sampler_.setSource(&audio, id)) {
        publishStatusLocked(request.sequence, MainSampleState::error,
                            "Main sample storage is unavailable");
        return;
    }
    // Rates above 96 kHz are stored at 96 kHz, so slices use the stored frames.
    const auto info = sampler_.sourceInfo();
    session_.reset(id, info.frames,
                   static_cast<std::uint32_t>(std::lround(info.sampleRate)), name);
    audioCache_ = {}; // keyed by the old id; the next save encodes the new source
    audioCacheId_ = 0U;
    publishStatusLocked(request.sequence, MainSampleState::ready);
}

void MainSampleWorkflows::clear(const MainSampleRequest& request)
{
    latestLoadSequence_.store(request.sequence, std::memory_order_release);
    const std::lock_guard lock(mutationMutex_);
    if (!sampler_.setSource(nullptr, 0U)) {
        publishStatusLocked(request.sequence, MainSampleState::error,
                            "Main sample storage is unavailable");
        return;
    }
    // Pads keep their audio and stale tags; a new source never reuses the id.
    session_.reset(0U, 0U, 0U, {});
    audioCache_ = {};
    audioCacheId_ = 0U;
    publishStatusLocked(request.sequence, MainSampleState::ready);
}

bool MainSampleWorkflows::sliceHasFavoritePad(const std::uint16_t sliceId) const
{
    if (sampler_.favoriteMask() == 0U)
        return false;
    const auto pads = snapshotPads();
    for (std::uint32_t pad = 0; pad < kPadCount; ++pad)
        if (sampler_.isFavorite(pad) && pads.provenance[pad] == link(session_.sourceId, sliceId))
            return true;
    return false;
}

std::uint32_t MainSampleWorkflows::zeroCrossFrameLocked(const std::uint32_t frame,
    const std::span<const std::uint16_t> ignoreIds) const
{
    for (const auto& slice : session_.slices) {
        if (std::find(ignoreIds.begin(), ignoreIds.end(), slice.id) != ignoreIds.end())
            continue;
        if (slice.start == frame || slice.end == frame)
            return frame; // region snap already placed it
    }
    if (session_.sampleRate == 0U || frame > session_.frames)
        return frame;
    const auto radius = static_cast<std::uint32_t>(
        session_.sampleRate * kZeroCrossMilliseconds / 1000.0);
    const std::uint32_t low = frame > radius ? frame - radius : 0U;
    const std::uint32_t high = std::min(session_.frames - 1U, frame + radius);
    if (session_.frames == 0U || low > high)
        return frame;
    const std::uint32_t first = low > 0U ? low - 1U : low; // one sample earlier to see a crossing at `low`
    PadData audio;
    if (!sampler_.copySourceRange(session_.sourceId, first, high - first + 1U, audio) || audio.frames == 0U)
        return frame;
    std::vector<float> mono(audio.frames);
    for (std::size_t index = 0; index < mono.size(); ++index)
        mono[index] = 0.5f * (audio.stereo[index * 2U] + audio.stereo[index * 2U + 1U]);
    const auto found = nearestZeroCrossing(mono, frame - first, radius);
    const auto result = first + static_cast<std::uint32_t>(found);
    return result >= low && result <= high ? result : frame;
}

void MainSampleWorkflows::edit(const MainSampleRequest& request)
{
    const std::lock_guard lock(mutationMutex_);
    if (session_.empty()) {
        publishStatusLocked(request.sequence, MainSampleState::error, "Load a main sample first");
        return;
    }
    auto edited = session_;
    const std::span<const MainSampleSlice> edits{request.edits.data(), request.editCount};
    for (std::size_t index = 0; index < edits.size(); ++index)
        if (sliceHasFavoritePad(edits[index].id)) { // a favorite's own markers are locked
            publishStatusLocked(request.sequence, MainSampleState::error, kFavoriteRefusal);
            return;
        }
    bool applied = false;
    if (request.zeroCross) {
        // Only edges the drag actually moved are snapped; the same frame snaps to the same place.
        std::array<std::uint16_t, 2> editedIds{};
        for (std::size_t index = 0; index < edits.size(); ++index)
            editedIds[index] = edits[index].id;
        std::array<MainSampleSlice, 2> snapped{};
        std::array<std::uint32_t, 4> from{};
        std::array<std::uint32_t, 4> to{};
        std::size_t known = 0U;
        const auto snapEdge = [&](const std::uint32_t frame) {
            for (std::size_t i = 0; i < known; ++i)
                if (from[i] == frame) return to[i];
            const auto result = zeroCrossFrameLocked(frame, {editedIds.data(), edits.size()});
            from[known] = frame;
            to[known++] = result;
            return result;
        };
        for (std::size_t index = 0; index < edits.size(); ++index) {
            snapped[index] = edits[index];
            const auto old = std::find_if(session_.slices.begin(), session_.slices.end(),
                [&](const MainSampleSlice& slice) { return slice.id == edits[index].id; });
            if (old == session_.slices.end())
                continue;
            if (edits[index].start != old->start) snapped[index].start = snapEdge(edits[index].start);
            if (edits[index].end != old->end) snapped[index].end = snapEdge(edits[index].end);
        }
        applied = edited.editSlices({snapped.data(), edits.size()});
        if (!applied)
            edited = session_; // an invalid snapped result falls back to the exact edit
    }
    if (!applied && !edited.editSlices(edits)) {
        publishStatusLocked(request.sequence, MainSampleState::error, "Invalid slice edit");
        return;
    }
    std::array<std::uint16_t, 2> ids{};
    for (std::size_t index = 0; index < edits.size(); ++index)
        ids[index] = edits[index].id;
    std::uint64_t affected = 0U;
    if (!recutLinkedPads(edited, {ids.data(), edits.size()}, snapshotPads(), affected)) {
        publishStatusLocked(request.sequence, MainSampleState::error, "Pads changed, try again");
        return;
    }
    session_ = std::move(edited);
    publishPadSettings(affected, false);
    publishStatusLocked(request.sequence, MainSampleState::ready, {}, affected);
}

void MainSampleWorkflows::removeOverlaps(const MainSampleRequest& request)
{
    const std::lock_guard lock(mutationMutex_);
    if (session_.empty()) {
        publishStatusLocked(request.sequence, MainSampleState::error, "Load a main sample first");
        return;
    }
    const auto plan = planOverlapTrim(session_.slices, [&](const std::uint16_t id) { return sliceHasFavoritePad(id); });
    if (plan.trimmed.empty()) {
        publishStatusLocked(request.sequence, MainSampleState::ready,
            plan.left == 0U ? "No overlaps" : std::to_string(plan.left) + " overlaps left");
        return;
    }
    auto edited = session_;
    edited.slices = plan.slices;
    std::uint64_t affected = 0U;
    if (!recutLinkedPads(edited, {plan.trimmed.data(), plan.trimmed.size()}, snapshotPads(), affected)) {
        publishStatusLocked(request.sequence, MainSampleState::error, "Pads changed, try again");
        return;
    }
    session_ = std::move(edited);
    publishPadSettings(affected, false);
    std::string text = "Removed " + std::to_string(plan.trimmed.size()) + (plan.trimmed.size() == 1U ? " overlap" : " overlaps");
    if (plan.left != 0U) text += ", " + std::to_string(plan.left) + " left";
    publishStatusLocked(request.sequence, MainSampleState::ready, text, affected);
}

void MainSampleWorkflows::setRange(const MainSampleRequest& request)
{
    const std::lock_guard lock(mutationMutex_);
    if (session_.empty()) {
        publishStatusLocked(request.sequence, MainSampleState::error, "Load a main sample first");
        return;
    }
    std::uint32_t start = request.start;
    std::uint32_t end = request.end;
    if (request.zeroCross) {
        // Only an edge the drag moved is snapped, as with slice edges.
        const auto snappedStart = start != session_.rangeStart ? zeroCrossFrameLocked(start, {}) : start;
        const auto snappedEnd = end != session_.rangeEndFrame() ? zeroCrossFrameLocked(end, {}) : end;
        if (snappedStart < snappedEnd) {
            start = snappedStart;
            end = snappedEnd;
        }
    }
    auto edited = session_;
    if (!edited.setRange(start, end)) {
        publishStatusLocked(request.sequence, MainSampleState::error, "Invalid source range");
        return;
    }
    session_ = std::move(edited);
    publishStatusLocked(request.sequence, MainSampleState::ready);
}

void MainSampleWorkflows::assign(const MainSampleRequest& request)
{
    const std::lock_guard lock(mutationMutex_);
    if (session_.empty()) {
        publishStatusLocked(request.sequence, MainSampleState::error, "Load a main sample first");
        return;
    }
    const auto pad = request.pad;
    const auto bit = std::uint64_t{1} << pad;
    if (sampler_.isFavorite(pad)) {
        publishStatusLocked(request.sequence, MainSampleState::error, kFavoriteRefusal);
        return;
    }
    EngineSettings settings;
    PadMetadata metadata;
    controlDispatcher_.invoke([&]() noexcept {
        settings = sampler_.settings();
        metadata = sampler_.padMetadata(pad);
    });
    if (settings.armed || metadata.recording) {
        publishStatusLocked(request.sequence, MainSampleState::error, "Disarm recording first");
        return;
    }
    const auto pads = snapshotPads();
    const bool ownLink = pads.provenance[pad] != 0U &&
        (pads.provenance[pad] >> 16U) == session_.sourceId;
    if (metadata.occupied && !ownLink && !request.confirm) {
        publishStatusLocked(request.sequence, MainSampleState::confirm,
                            "Replace this pad's audio?", bit);
        return;
    }
    auto edited = session_;
    MainSampleSlice created;
    std::uint32_t start = request.start;
    std::uint32_t end = request.end;
    if (request.zeroCross) {
        const auto snappedStart = zeroCrossFrameLocked(start, {});
        const auto snappedEnd = zeroCrossFrameLocked(end, {});
        if (snappedStart < snappedEnd) {
            start = snappedStart;
            end = snappedEnd;
        }
    }
    if (!edited.addSlice(start, end, created)) {
        publishStatusLocked(request.sequence, MainSampleState::error,
            edited.slices.size() >= kMainSampleMaxSlices ? "All 64 slices are in use"
                                                         : "Invalid slice range");
        return;
    }
    PadData audio;
    PadImport import;
    import.pad = pad;
    import.audio = &audio;
    import.provenance = link(edited.sourceId, created.id);
    import.expectedGeneration = pads.generations[pad];
    // Like a WAV import, the new audio starts with default editor and mixer settings.
    if (!sampler_.copySourceRange(edited.sourceId, created.start, created.end - created.start,
                                  audio) ||
        !sampler_.importPads({&import, 1U}, &settings)) {
        publishStatusLocked(request.sequence, MainSampleState::error, "Pads changed, try again");
        return;
    }
    session_ = std::move(edited);
    publishPadSettings(bit, true);
    publishStatusLocked(request.sequence, MainSampleState::ready, {}, bit);
}

std::shared_ptr<const PadData> MainSampleWorkflows::sourceSnapshot(const std::uint64_t sourceId) const
{
    const std::lock_guard lock(mutationMutex_);
    auto copy = std::make_shared<PadData>();
    std::uint64_t id = 0U;
    if (!sampler_.exportSource(*copy, &id) || id != sourceId)
        return nullptr;
    return copy;
}

std::shared_ptr<const PadData> MainSampleWorkflows::takeRetiredSource(const std::uint64_t sourceId)
{
    const std::lock_guard lock(mutationMutex_);
    if (retiredSourceId_ != sourceId)
        return nullptr;
    retiredSourceId_ = 0U;
    return std::move(retiredSource_);
}

void MainSampleWorkflows::cutSource(const MainSampleRequest& request)
{
    constexpr std::uint32_t kMinSliceFrames = 64U;
    constexpr double kCrossfadeMilliseconds = 5.0;
    const std::lock_guard lock(mutationMutex_);
    const auto fail = [&](const char* text) {
        publishStatusLocked(request.sequence, MainSampleState::error, text);
    };
    if (session_.empty())
        return fail("Load a main sample first");
    const auto settings = engineSettings();
    if (settings.armed)
        return fail("Disarm recording first");
    std::uint32_t a = request.start;
    std::uint32_t b = std::min(request.end, session_.frames);
    if (a >= b)
        return fail("Invalid region");
    if (request.zeroCross) {
        const auto snappedA = zeroCrossFrameLocked(a, {});
        const auto snappedB = zeroCrossFrameLocked(b, {});
        if (snappedA < snappedB) {
            a = snappedA;
            b = snappedB;
        }
    }
    const bool crop = request.crop;
    const std::uint32_t cutFrames = b - a;
    if (!crop && cutFrames >= session_.frames)
        return fail("Cannot cut the whole sample");
    if (crop && cutFrames == session_.frames)
        return fail("Nothing to crop");
    const std::uint32_t newFrames = crop ? cutFrames : session_.frames - cutFrames;
    if (newFrames < kMinSliceFrames)
        return fail("Result would be too short");

    const auto remap = remapSlicesForCut(session_.slices, a, b, crop, kMinSliceFrames);
    for (const auto id : remap.removed)
        if (sliceHasFavoritePad(id))
            return fail(kFavoriteRefusal);
    for (const auto id : remap.changed)
        if (sliceHasFavoritePad(id))
            return fail(kFavoriteRefusal);

    PadData whole;
    std::uint64_t oldId = 0U;
    if (!sampler_.exportSource(whole, &oldId) || oldId != session_.sourceId)
        return fail("Main sample unavailable");
    if (whole.stereo.size() * sizeof(float) > undoByteLimit_)
        return fail("Sample too large to undo: cut refused");

    PadData next;
    next.sampleRate = whole.sampleRate;
    next.frames = newFrames;
    next.stereo.resize(static_cast<std::size_t>(newFrames) * 2U);
    const auto& src = whole.stereo;
    if (crop) {
        std::copy_n(src.begin() + static_cast<std::ptrdiff_t>(a) * 2, static_cast<std::size_t>(newFrames) * 2U,
                    next.stereo.begin());
    } else {
        std::copy_n(src.begin(), static_cast<std::size_t>(a) * 2U, next.stereo.begin());
        std::uint32_t fade = 0U;
        if (a != 0U && b != whole.frames)
            fade = std::min({static_cast<std::uint32_t>(whole.sampleRate * kCrossfadeMilliseconds / 1000.0),
                             cutFrames, whole.frames - b});
        // Equal-power join: the removed material fades out while the material after the cut fades in.
        for (std::uint32_t i = 0; i < fade; ++i) {
            const double theta = 1.5707963267948966 * (static_cast<double>(i) + 0.5) / fade;
            const auto fadeOut = static_cast<float>(std::cos(theta));
            const auto fadeIn = static_cast<float>(std::sin(theta));
            for (std::size_t channel = 0; channel < 2U; ++channel)
                next.stereo[(static_cast<std::size_t>(a) + i) * 2U + channel] =
                    src[(static_cast<std::size_t>(a) + i) * 2U + channel] * fadeOut +
                    src[(static_cast<std::size_t>(b) + i) * 2U + channel] * fadeIn;
        }
        std::copy(src.begin() + static_cast<std::ptrdiff_t>(b + fade) * 2, src.end(),
                  next.stereo.begin() + static_cast<std::ptrdiff_t>(a + fade) * 2);
    }
    float peak = 0.0f;
    double energy = 0.0;
    for (const float sample : next.stereo) {
        peak = std::max(peak, std::fabs(sample));
        energy += static_cast<double>(sample) * sample;
    }
    next.peak = peak;
    next.rms = static_cast<float>(std::sqrt(energy / static_cast<double>(next.stereo.size())));

    const auto mapPoint = [&](const std::uint32_t frame) {
        if (crop)
            return std::clamp(frame, a, b) - a;
        return frame <= a ? frame : frame < b ? a : frame - cutFrames;
    };
    auto edited = session_;
    edited.sourceId = nextSourceId(session_.sourceId);
    edited.frames = newFrames;
    edited.slices = remap.slices;
    {
        const auto rangeStart = mapPoint(session_.rangeStart);
        const auto rangeEnd = mapPoint(session_.rangeEndFrame());
        edited.rangeStart = rangeStart < rangeEnd ? rangeStart : 0U;
        edited.rangeEnd = rangeStart < rangeEnd ? rangeEnd : newFrames;
    }

    if (!sampler_.setSource(&next, edited.sourceId))
        return fail("Main sample storage is unavailable");
    const auto rollback = [&] { static_cast<void>(sampler_.setSource(&whole, oldId)); };

    // Every pad linked to the old source moves to the new source id in ONE transaction:
    // survivors are re-cut from the new audio, slices that no longer exist clear their pads.
    const auto pads = snapshotPads();
    std::array<PadData, kPadCount> padAudio;
    std::vector<PadImport> imports;
    std::uint64_t affected = 0U;
    for (std::uint32_t pad = 0; pad < kPadCount; ++pad) {
        const auto provenance = pads.provenance[pad];
        if (provenance == 0U || (provenance >> 16U) != oldId)
            continue;
        const auto sliceId = static_cast<std::uint16_t>(provenance & kSliceMask);
        PadImport import;
        import.pad = pad;
        const auto* const slice = edited.find(sliceId);
        if (slice != nullptr) {
            if (!sampler_.copySourceRange(edited.sourceId, slice->start, slice->end - slice->start,
                                          padAudio[pad])) {
                rollback();
                return fail("Main sample unavailable");
            }
            import.audio = &padAudio[pad];
            import.playback = sampler_.padPlaybackSettings(pad);
            if (std::find(remap.changed.begin(), remap.changed.end(), sliceId) != remap.changed.end()) {
                import.playback.start = 0.0f;
                import.playback.end = 1.0f;
            }
            import.mixer = sampler_.padMixerSettings(pad);
            import.provenance = link(edited.sourceId, sliceId);
        }
        imports.push_back(import);
        affected |= std::uint64_t{1} << pad;
    }
    if (!imports.empty() && !sampler_.importPads(imports, &settings)) {
        rollback();
        return fail("Pads changed, try again");
    }
    session_ = std::move(edited);
    audioCache_ = {};
    audioCacheId_ = 0U;
    retiredSource_ = std::make_shared<const PadData>(std::move(whole));
    retiredSourceId_ = oldId;
    publishPadSettings(affected, true);
    publishStatusLocked(request.sequence, MainSampleState::ready, {}, affected);
}

void MainSampleWorkflows::remove(const MainSampleRequest& request)
{
    const std::lock_guard lock(mutationMutex_);
    if (session_.empty()) {
        publishStatusLocked(request.sequence, MainSampleState::error, "Load a main sample first");
        return;
    }
    if (sliceHasFavoritePad(request.sliceId)) {
        publishStatusLocked(request.sequence, MainSampleState::error, kFavoriteRefusal);
        return;
    }
    auto edited = session_;
    if (!edited.removeSlice(request.sliceId)) {
        publishStatusLocked(request.sequence, MainSampleState::error, "Slice not found");
        return;
    }
    // Linked pads are cleared (audio removed, settings reset) in the same atomic transaction, so
    // one undo step restores the slice and the pad audio together.
    const auto pads = snapshotPads();
    const auto removed = link(edited.sourceId, request.sliceId);
    std::vector<PadImport> clears;
    std::uint64_t affected = 0U;
    for (std::uint32_t pad = 0; pad < kPadCount; ++pad) {
        if (pads.provenance[pad] != removed)
            continue;
        PadImport clear;
        clear.pad = pad;
        clears.push_back(clear);
        affected |= std::uint64_t{1} << pad;
    }
    if (!clears.empty()) {
        const auto settings = engineSettings();
        if (settings.armed) {
            publishStatusLocked(request.sequence, MainSampleState::error, "Disarm recording first");
            return;
        }
        if (!sampler_.importPads(clears, &settings)) {
            publishStatusLocked(request.sequence, MainSampleState::error, "Pads changed, try again");
            return;
        }
        publishPadSettings(affected, true);
    }
    session_ = std::move(edited);
    publishStatusLocked(request.sequence, MainSampleState::ready, {}, affected);
}

void MainSampleWorkflows::arrange(const MainSampleRequest& request)
{
    const std::lock_guard lock(mutationMutex_);
    if (session_.empty()) {
        publishStatusLocked(request.sequence, MainSampleState::error, "Load a main sample first");
        return;
    }
    const auto settings = engineSettings();
    if (settings.armed) {
        publishStatusLocked(request.sequence, MainSampleState::error, "Disarm recording first");
        return;
    }
    const auto pads = snapshotPads();
    // Favourite pads keep their slice and their place; everything else is arranged around them.
    std::vector<MainSampleLink> links;
    std::vector<std::uint16_t> favouriteSlices;
    std::uint64_t blocked = 0U; // favourites and pads holding audio that is not a slice of this source
    for (std::uint32_t pad = 0; pad < kPadCount; ++pad) {
        const auto provenance = pads.provenance[pad];
        const bool own = provenance != 0U && (provenance >> 16U) == session_.sourceId &&
            session_.find(static_cast<std::uint16_t>(provenance & kSliceMask)) != nullptr;
        if (sampler_.isFavorite(pad)) {
            blocked |= std::uint64_t{1} << pad;
            if (own)
                favouriteSlices.push_back(static_cast<std::uint16_t>(provenance & kSliceMask));
        } else if (own) {
            links.push_back({pad, static_cast<std::uint16_t>(provenance & kSliceMask)});
        } else {
            PadMetadata metadata;
            controlDispatcher_.invoke([&]() noexcept { metadata = sampler_.padMetadata(pad); });
            if (metadata.recording) {
                publishStatusLocked(request.sequence, MainSampleState::error, "Disarm recording first");
                return;
            }
            if (metadata.occupied)
                blocked |= std::uint64_t{1} << pad;
        }
    }
    // Consecutive visible pads from the first pad, skipping the blocked ones.
    std::vector<std::uint32_t> candidates;
    const std::uint32_t firstPad = request.pad < kPadCount ? request.pad : 0U;
    for (std::uint32_t step = 0; step < kPadCount; ++step) {
        const auto pad = advanceVisiblePad(firstPad, step, settings.padsPerBank, settings.midiBankMode);
        if (pad >= kPadCount)
            break;
        if ((blocked & (std::uint64_t{1} << pad)) == 0U)
            candidates.push_back(pad);
    }
    const auto plan = planArrange(session_.slices, links, favouriteSlices, candidates);
    if (!plan.enough) {
        publishStatusLocked(request.sequence, MainSampleState::error, "Not enough free pads to arrange every slice");
        return;
    }
    if (plan.empty()) {
        publishStatusLocked(request.sequence, MainSampleState::ready, "Pads already in order");
        return;
    }
    // Every moved pad is both a source and a destination, so one transaction replaces them all.
    const std::size_t total = plan.moves.size() + plan.fills.size() + plan.vacated.size();
    std::vector<PadData> audio(total);
    std::vector<PadImport> imports(total);
    std::uint64_t affected = 0U;
    std::size_t index = 0U;
    for (const auto& move : plan.moves) {
        auto& import = imports[index];
        import.pad = move.to;
        import.audio = &audio[index];
        if (!sampler_.exportPad(move.from, audio[index], &import.playback, &import.mixer)) {
            publishStatusLocked(request.sequence, MainSampleState::error, "Pads changed, try again");
            return;
        }
        import.provenance = pads.provenance[move.from];
        import.expectedGeneration = pads.generations[move.to];
        affected |= std::uint64_t{1} << move.to;
        ++index;
    }
    for (const auto& fill : plan.fills) {
        const auto* slice = session_.find(fill.sliceId);
        auto& import = imports[index];
        import.pad = fill.pad;
        import.audio = &audio[index];
        if (slice == nullptr ||
            !sampler_.copySourceRange(session_.sourceId, slice->start, slice->end - slice->start, audio[index])) {
            publishStatusLocked(request.sequence, MainSampleState::error, "Pads changed, try again");
            return;
        }
        import.provenance = link(session_.sourceId, fill.sliceId);
        import.expectedGeneration = pads.generations[fill.pad];
        affected |= std::uint64_t{1} << fill.pad;
        ++index;
    }
    for (const auto pad : plan.vacated) {
        auto& import = imports[index];
        import.pad = pad; // audio stays nullptr: the pad is cleared
        import.expectedGeneration = pads.generations[pad];
        affected |= std::uint64_t{1} << pad;
        ++index;
    }
    if (!sampler_.importPads(imports, &settings)) {
        publishStatusLocked(request.sequence, MainSampleState::error, "Pads changed, try again");
        return;
    }
    if (arrangeHook_)
        arrangeHook_(plan.moves, plan.vacated);
    publishPadSettings(affected, true);
    publishStatusLocked(request.sequence, MainSampleState::ready, {}, affected);
}

void MainSampleWorkflows::buildAnalysisLocked(const PadData& whole)
{
    analysisCache_ = {};
    analysisCache_.analysis = sms::analysis::analyzeOnsets(
        whole.stereo, static_cast<double>(session_.sampleRate), {}, nullptr);
    const auto tempo = sms::analysis::estimateTempo(analysisCache_.analysis.novelty,
                                                    analysisCache_.analysis.hopSeconds);
    analysisCache_.bpm = tempo.bpm;
    analysisCache_.sourceId = session_.sourceId;
    ++analysisRuns_;
}

void MainSampleWorkflows::spectrogram(const MainSampleRequest& request)
{
    // Reuses the cached onset analysis (same FFT frames); built here on first use.
    static_assert(kSpectrogramBins == sms::analysis::kSpectrogramBins);
    const std::lock_guard lock(mutationMutex_);
    if (session_.empty() || request.start >= request.end || request.end > session_.frames)
        return; // stale; the view retries against its next status
    if (analysisCache_.sourceId != session_.sourceId) {
        PadData whole;
        std::uint64_t id = 0U;
        if (!sampler_.exportSource(whole, &id) || id != session_.sourceId)
            return;
        buildAnalysisLocked(whole);
    }
    MainSampleSpectrogram reply;
    reply.sequence = request.sequence;
    reply.start = request.start;
    reply.end = request.end;
    reply.columns = request.tiles;
    reply.data.resize(static_cast<std::size_t>(request.tiles) * kSpectrogramBins);
    if (!sms::analysis::spectrogramColumns(analysisCache_.analysis, request.start, request.end,
                                           request.tiles, reply.data))
        return;
    const auto encoded = encodeMainSampleSpectrogram(reply);
    observer_.publishUiState(kMainSampleSpectrogramKey, encoded.c_str());
}

void MainSampleWorkflows::waveform(const MainSampleRequest& request)
{
    // No mutation mutex: the engine pins the source, so a long scan never
    // blocks pad workflows. A stale range or source is refused there.
    MainSampleWaveform reply;
    reply.sequence = request.sequence;
    reply.start = request.start;
    reply.end = request.end;
    reply.count = request.tiles;
    const auto id = sampler_.sourceInfo().id;
    if (id == 0U || !sampler_.summarizeSourceRange(id, request.start, request.end,
                                                   {reply.tiles.data(), request.tiles})) {
        publishStatus(request.sequence, MainSampleState::error, "Waveform unavailable");
        return;
    }
    const auto encoded = encodeMainSampleWaveform(reply);
    observer_.publishUiState(kMainSampleWaveformKey, encoded.c_str());
}

bool MainSampleWorkflows::recutLinkedPads(const MainSampleSession& target,
    const std::span<const std::uint16_t> sliceIds, const PadSnapshot& pads,
    std::uint64_t& affected)
{
    std::array<PadData, kPadCount> audio;
    std::vector<PadImport> imports;
    for (std::uint32_t pad = 0; pad < kPadCount; ++pad) {
        const auto provenance = pads.provenance[pad];
        const auto sliceId = static_cast<std::uint16_t>(provenance & kSliceMask);
        if (provenance == 0U || (provenance >> 16U) != target.sourceId ||
            std::find(sliceIds.begin(), sliceIds.end(), sliceId) == sliceIds.end())
            continue;
        const auto* const slice = target.find(sliceId);
        if (slice == nullptr ||
            !sampler_.copySourceRange(target.sourceId, slice->start, slice->end - slice->start,
                                      audio[pad]))
            return false;
        PadImport import;
        import.pad = pad;
        import.audio = &audio[pad];
        // Settings read after the generation snapshot: a newer edit fails the
        // generation check instead of being lost. The markers now define the
        // trim, so Start/End reset; envelope and mixer stay.
        import.playback = sampler_.padPlaybackSettings(pad);
        import.playback.start = 0.0f;
        import.playback.end = 1.0f;
        import.mixer = sampler_.padMixerSettings(pad);
        import.provenance = provenance;
        import.expectedGeneration = pads.generations[pad];
        imports.push_back(import);
        affected |= std::uint64_t{1} << pad;
    }
    if (imports.empty())
        return true;
    const auto settings = engineSettings();
    if (settings.armed)
        return false;
    return sampler_.importPads(imports, &settings);
}

MainSampleWorkflows::PadSnapshot MainSampleWorkflows::snapshotPads() const
{
    PadSnapshot pads;
    sampler_.padProvenance(pads.provenance, pads.generations);
    return pads;
}

EngineSettings MainSampleWorkflows::engineSettings() const
{
    EngineSettings settings;
    controlDispatcher_.invoke([&]() noexcept { settings = sampler_.settings(); });
    return settings;
}

void MainSampleWorkflows::publishPadSettings(const std::uint64_t pads, const bool mixer)
{
    // One or two slices change per request, and each slice feeds its own pad,
    // so this stays far below the UI message bus capacity.
    for (std::uint32_t pad = 0; pad < kPadCount; ++pad) {
        if ((pads & (std::uint64_t{1} << pad)) == 0U)
            continue;
        const auto editor = encodePlaybackSettings(sampler_.padPlaybackSettings(pad));
        observer_.publishUiState(kPadEditStateKeys[pad].c_str(), editor.c_str());
        if (mixer) {
            const auto encoded = encodeMixerSettings(sampler_.padMixerSettings(pad));
            observer_.publishUiState(kPadMixerStateKeys[pad].c_str(), encoded.c_str());
        }
    }
}

void MainSampleWorkflows::publishStatusLocked(const std::uint64_t sequence,
    const MainSampleState state, const std::string_view text, const std::uint64_t affected)
{
    MainSampleStatus status;
    status.sequence = sequence;
    status.state = state == MainSampleState::ready && session_.empty() ? MainSampleState::empty
                                                                       : state;
    status.affectedMask = affected;
    status.text = text.empty() && status.state == MainSampleState::ready
        ? session_.name : std::string(text);
    if (!session_.empty()) {
        status.sourceId = session_.sourceId;
        status.frames = session_.frames;
        status.sampleRate = session_.sampleRate;
        status.slices = session_.slices;
        status.rangeStart = session_.rangeStart;
        status.rangeEnd = session_.rangeEndFrame();
        const auto pads = snapshotPads();
        for (std::uint32_t pad = 0; pad < kPadCount; ++pad) {
            const auto provenance = pads.provenance[pad];
            const auto sliceId = static_cast<std::uint16_t>(provenance & kSliceMask);
            if (provenance != 0U && (provenance >> 16U) == session_.sourceId &&
                session_.find(sliceId) != nullptr)
                status.links.push_back({pad, sliceId});
        }
    }
    const auto encoded = encodeMainSampleStatus(status);
    observer_.publishUiState(kMainSampleStatusKey, encoded.c_str());
}

void MainSampleWorkflows::publishStatus(const std::uint64_t sequence,
    const MainSampleState state, const std::string_view text, const std::uint64_t affected)
{
    const std::lock_guard lock(mutationMutex_);
    publishStatusLocked(sequence, state, text, affected);
}

std::uint64_t MainSampleWorkflows::nextSourceId(const std::uint64_t current)
{
    const auto clock = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::uint64_t id = 0U;
    do {
        id = makeMainSampleSourceId(clock ^ (++sourceCounter_ * 0x9e3779b97f4a7c15ULL));
    } while (id == current);
    return id;
}

void MainSampleWorkflows::publishAutoChop(const AutoChopStatus& status)
{
    const auto encoded = encodeAutoChopStatus(status);
    observer_.publishUiState(kAutoChopStatusKey, encoded.c_str());
}

void MainSampleWorkflows::handleAutoChop(const std::string_view encoded)
{
    AutoChopRequest request;
    if (!decodeAutoChopRequest(encoded, request)) {
        AutoChopStatus status;
        status.text = "Invalid auto chop request";
        publishAutoChop(status);
        return;
    }
    try {
        autoChop(request);
    } catch (...) {
        AutoChopStatus status;
        status.sequence = request.sequence;
        status.text = "Auto chop failed";
        publishAutoChop(status);
    }
}

void MainSampleWorkflows::autoChop(const AutoChopRequest& request)
{
    AutoChopStatus reply;
    reply.sequence = request.sequence;
    auto fail = [&](const char* text) {
        reply.state = AutoChopState::error;
        reply.text = text;
        publishAutoChop(reply);
    };
    const std::lock_guard lock(mutationMutex_);
    if (session_.empty()) {
        fail("Load a main sample first");
        return;
    }
    EngineSettings settings;
    controlDispatcher_.invoke([&]() noexcept { settings = sampler_.settings(); });
    if (settings.armed) {
        fail("Disarm recording first");
        return;
    }
    // The analysis is keyed by the source id, so a reload or clear rebuilds it.
    if (analysisCache_.sourceId != session_.sourceId) {
        PadData whole;
        std::uint64_t id = 0U;
        if (!sampler_.exportSource(whole, &id) || id != session_.sourceId) {
            fail("Main sample storage is unavailable");
            return;
        }
        buildAnalysisLocked(whole);
    }
    const double host = hostBpm_ != nullptr ? hostBpm_->load(std::memory_order_relaxed) : 0.0;
    const double requested = static_cast<double>(request.bpmCenti) / 100.0;
    const double bpm = requested > 0.0 ? requested : host;
    const bool beatMode = request.mode == AutoChopMode::beat || request.mode == AutoChopMode::random;
    if (beatMode && !(bpm > 0.0)) {
        fail("No tempo");
        return;
    }
    const std::int64_t frames = session_.frames;
    // The source range confines every mode; the request may narrow it further.
    const std::int64_t rangeStart =
        std::max<std::int64_t>(std::min<std::int64_t>(request.rangeStart, frames), session_.rangeStart);
    const std::int64_t rangeEnd = std::min<std::int64_t>(
        request.rangeEnd == 0U ? frames : std::min<std::int64_t>(request.rangeEnd, frames),
        session_.rangeEndFrame());
    if (rangeStart >= rangeEnd) {
        fail("No slices found");
        return;
    }
    const std::int64_t minFrames =
        std::max<std::int64_t>(1, static_cast<std::int64_t>(0.01 * session_.sampleRate));
    const double contextBpm =
        request.mode == AutoChopMode::findSamples && !(bpm > 0.0) ? analysisCache_.bpm : bpm;
    const sms::analysis::SliceContext context{analysisCache_.analysis, rangeStart, rangeEnd,
                                              minFrames, 0, contextBpm};
    const std::size_t want = request.count;
    const double beats = static_cast<double>(request.beatsMilli) / 1000.0;
    std::vector<sms::analysis::SliceRegion> regions;
    switch (request.mode) {
    case AutoChopMode::transient:
        regions = sms::analysis::transientSlices(context, want,
            static_cast<float>(request.sensitivityPermille) / 1000.0F,
            static_cast<double>(request.minGapMs) / 1000.0);
        break;
    case AutoChopMode::spectral:
        regions = sms::analysis::spectralTransientSlices(context, want,
            static_cast<float>(request.sensitivityPermille) / 1000.0F,
            static_cast<double>(request.minGapMs) / 1000.0);
        break;
    case AutoChopMode::findSamples:
        regions = sms::analysis::findSamples(context, want, request.seed);
        break;
    case AutoChopMode::equal: regions = sms::analysis::equalSlices(context, want); break;
    case AutoChopMode::beat:
        regions = sms::analysis::beatSlices(context, want, beats,
            std::clamp<std::int64_t>(request.anchor, rangeStart, rangeEnd), request.snap);
        break;
    case AutoChopMode::random:
        regions = sms::analysis::randomSlices(context, want, beats, request.seed, request.snap);
        break;
    }
    // Favorite pads keep their slices: new slices are cut around those regions.
    std::vector<MainSampleSlice> fixedSlices;
    {
        const auto fixedPads = snapshotPads();
        for (std::uint32_t pad = 0; pad < kPadCount; ++pad) {
            const auto provenance = fixedPads.provenance[pad];
            if (!sampler_.isFavorite(pad) || provenance == 0U || (provenance >> 16U) != session_.sourceId)
                continue;
            if (const auto* slice = session_.find(static_cast<std::uint16_t>(provenance & kSliceMask)))
                fixedSlices.push_back(*slice);
        }
    }
    if (!fixedSlices.empty()) {
        std::sort(fixedSlices.begin(), fixedSlices.end(),
                  [](const auto& a, const auto& b) { return a.start < b.start; });
        std::vector<sms::analysis::SliceRegion> trimmed;
        for (const auto& region : regions) {
            std::int64_t cursor = region.start;
            const auto keep = [&](const std::int64_t from, const std::int64_t to) {
                if (to - from >= minFrames)
                    trimmed.push_back({from, to});
            };
            for (const auto& fixed : fixedSlices) {
                if (static_cast<std::int64_t>(fixed.end) <= cursor || static_cast<std::int64_t>(fixed.start) >= region.end)
                    continue;
                if (static_cast<std::int64_t>(fixed.start) > cursor)
                    keep(cursor, fixed.start);
                cursor = std::max<std::int64_t>(cursor, fixed.end);
            }
            if (cursor < region.end)
                keep(cursor, region.end);
        }
        regions = std::move(trimmed);
    }
    // Slices follow pad numbers across the visible sequence, skipping hidden slots and favorites.
    std::vector<std::uint32_t> targetPads;
    for (std::uint32_t step = 0; targetPads.size() < std::min(regions.size(), want); ++step) {
        const auto pad = advanceVisiblePad(request.firstPad, step, settings.padsPerBank, settings.midiBankMode);
        if (pad >= kPadCount)
            break;
        if (!sampler_.isFavorite(pad))
            targetPads.push_back(pad);
    }
    const auto padFor = [&](const std::size_t index) noexcept { return targetPads[index]; };
    const std::size_t reachable = targetPads.size();
    const auto targetCount = std::min<std::size_t>({regions.size(), want, reachable});
    regions.resize(targetCount);
    reply.found = static_cast<std::uint32_t>(targetCount);
    reply.requested = request.count;
    if (targetCount == 0U) {
        fail("No slices found");
        return;
    }

    const auto pads = snapshotPads();
    std::uint64_t foreign = 0U;
    std::uint64_t targets = 0U;
    for (std::size_t index = 0; index < targetCount; ++index) {
        const auto pad = padFor(index);
        const auto bit = std::uint64_t{1} << pad;
        targets |= bit;
        PadMetadata metadata;
        controlDispatcher_.invoke([&]() noexcept { metadata = sampler_.padMetadata(pad); });
        if (metadata.recording) {
            fail("Disarm recording first");
            return;
        }
        const bool own = pads.provenance[pad] != 0U &&
            (pads.provenance[pad] >> 16U) == session_.sourceId;
        if (metadata.occupied && !own)
            foreign |= bit;
    }
    if (foreign != 0U && !request.confirm) {
        reply.state = AutoChopState::confirm;
        reply.mask = foreign;
        reply.found = static_cast<std::uint32_t>(std::popcount(foreign));
        publishAutoChop(reply);
        return;
    }

    // A fresh slice set replaces the old one; its ids are never reused.
    auto edited = session_;
    std::erase_if(edited.slices, [&](const MainSampleSlice& slice) {
        return std::none_of(fixedSlices.begin(), fixedSlices.end(),
                            [&](const MainSampleSlice& fixed) { return fixed.id == slice.id; });
    });
    std::vector<MainSampleSlice> created(targetCount);
    for (std::size_t index = 0; index < targetCount; ++index) {
        if (!edited.addSlice(static_cast<std::uint32_t>(regions[index].start),
                             static_cast<std::uint32_t>(regions[index].end), created[index])) {
            fail("Invalid slice range");
            return;
        }
    }
    std::vector<PadData> audio(targetCount);
    std::vector<PadImport> imports(targetCount);
    for (std::size_t index = 0; index < targetCount; ++index) {
        const auto pad = padFor(index);
        imports[index].pad = pad;
        imports[index].audio = &audio[index];
        imports[index].provenance = link(edited.sourceId, created[index].id);
        imports[index].expectedGeneration = pads.generations[pad];
        if (!sampler_.copySourceRange(edited.sourceId, created[index].start,
                                      created[index].end - created[index].start, audio[index])) {
            fail("Pads changed, try again");
            return;
        }
    }
    if (!sampler_.importPads(imports, &settings)) {
        fail("Pads changed, try again");
        return;
    }
    // Pads tied to the replaced slices that were not refilled keep their audio, minus the tag.
    std::uint64_t affected = targets;
    for (std::uint32_t pad = 0; pad < kPadCount; ++pad) {
        const auto bit = std::uint64_t{1} << pad;
        if ((targets & bit) == 0U && pads.provenance[pad] != 0U &&
            (pads.provenance[pad] >> 16U) == session_.sourceId && !sampler_.isFavorite(pad) &&
            sampler_.setPadProvenance(pad, 0U, sampler_.padAudioFingerprint(pad)))
            affected |= bit;
    }
    session_ = std::move(edited);
    publishPadSettings(targets, true);
    publishStatusLocked(request.sequence, MainSampleState::ready, {}, affected);
    reply.state = AutoChopState::ok;
    reply.mask = targets;
    publishAutoChop(reply);
}

} // namespace midichopper::plugin
