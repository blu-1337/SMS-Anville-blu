#include "MainSampleWorkflows.hpp"

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
    if (!change.imports.empty() && !sampler_.importPads(change.imports, nullptr))
        return false;
    for (const auto& item : change.settings)
        sampler_.restorePadEditorSettings(item.pad, item.playback, item.mixer);
    for (const auto& item : change.links)
        static_cast<void>(sampler_.setPadProvenance(item.pad, item.provenance, item.fingerprint));
    if (change.session != nullptr && !session_.empty() && change.session->sourceId == session_.sourceId)
        session_ = *change.session;
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
    case MainSampleVerb::arrange: arrange(request); return;
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

void MainSampleWorkflows::edit(const MainSampleRequest& request)
{
    const std::lock_guard lock(mutationMutex_);
    if (session_.empty()) {
        publishStatusLocked(request.sequence, MainSampleState::error, "Load a main sample first");
        return;
    }
    auto edited = session_;
    const std::span<const MainSampleSlice> edits{request.edits.data(), request.editCount};
    if (!edited.editSlices(edits)) {
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

void MainSampleWorkflows::assign(const MainSampleRequest& request)
{
    const std::lock_guard lock(mutationMutex_);
    if (session_.empty()) {
        publishStatusLocked(request.sequence, MainSampleState::error, "Load a main sample first");
        return;
    }
    const auto pad = request.pad;
    const auto bit = std::uint64_t{1} << pad;
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
    if (!edited.addSlice(request.start, request.end, created)) {
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

void MainSampleWorkflows::remove(const MainSampleRequest& request)
{
    const std::lock_guard lock(mutationMutex_);
    if (session_.empty()) {
        publishStatusLocked(request.sequence, MainSampleState::error, "Load a main sample first");
        return;
    }
    auto edited = session_;
    if (!edited.removeSlice(request.sliceId)) {
        publishStatusLocked(request.sequence, MainSampleState::error, "Slice not found");
        return;
    }
    // Linked pads keep their audio and settings; only the tag goes.
    const auto pads = snapshotPads();
    const auto removed = link(edited.sourceId, request.sliceId);
    std::uint64_t affected = 0U;
    for (std::uint32_t pad = 0; pad < kPadCount; ++pad) {
        if (pads.provenance[pad] == removed &&
            sampler_.setPadProvenance(pad, 0U, sampler_.padAudioFingerprint(pad)))
            affected |= std::uint64_t{1} << pad;
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
    std::vector<MainSampleLink> links;
    for (std::uint32_t pad = 0; pad < kPadCount; ++pad) {
        const auto provenance = pads.provenance[pad];
        if (provenance != 0U && (provenance >> 16U) == session_.sourceId)
            links.push_back({pad, static_cast<std::uint16_t>(provenance & kSliceMask)});
    }
    const auto moves = arrangeMoves(session_.slices, links);
    if (moves.empty()) {
        publishStatusLocked(request.sequence, MainSampleState::ready, "Pads already in order");
        return;
    }
    // Every moved pad is both a source and a destination, so one transaction replaces them all.
    std::vector<PadData> audio(moves.size());
    std::vector<PadImport> imports(moves.size());
    std::uint64_t affected = 0U;
    for (std::size_t index = 0; index < moves.size(); ++index) {
        const auto from = moves[index].from;
        const auto to = moves[index].to;
        auto& import = imports[index];
        import.pad = to;
        import.audio = &audio[index];
        if (!sampler_.exportPad(from, audio[index], &import.playback, &import.mixer)) {
            publishStatusLocked(request.sequence, MainSampleState::error, "Pads changed, try again");
            return;
        }
        import.provenance = pads.provenance[from];
        import.expectedGeneration = pads.generations[to];
        affected |= std::uint64_t{1} << to;
    }
    if (!sampler_.importPads(imports, &settings)) {
        publishStatusLocked(request.sequence, MainSampleState::error, "Pads changed, try again");
        return;
    }
    publishPadSettings(affected, true);
    publishStatusLocked(request.sequence, MainSampleState::ready, {}, affected);
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
        analysisCache_ = {};
        analysisCache_.analysis = sms::analysis::analyzeOnsets(
            whole.stereo, static_cast<double>(session_.sampleRate), {}, nullptr);
        const auto tempo = sms::analysis::estimateTempo(analysisCache_.analysis.novelty,
                                                        analysisCache_.analysis.hopSeconds);
        analysisCache_.bpm = tempo.bpm;
        analysisCache_.sourceId = session_.sourceId;
        ++analysisRuns_;
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
    const std::int64_t rangeStart = std::min<std::int64_t>(request.rangeStart, frames);
    const std::int64_t rangeEnd =
        request.rangeEnd == 0U ? frames : std::min<std::int64_t>(request.rangeEnd, frames);
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
    // Slices follow pad numbers across the visible sequence, skipping hidden slots.
    const auto padFor = [&](const std::size_t index) noexcept {
        return advanceVisiblePad(request.firstPad, static_cast<std::uint32_t>(index),
                                 settings.padsPerBank, settings.midiBankMode);
    };
    std::size_t reachable = 0U;
    while (reachable < regions.size() && padFor(reachable) < kPadCount)
        ++reachable;
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
    edited.slices.clear();
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
            (pads.provenance[pad] >> 16U) == session_.sourceId &&
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
