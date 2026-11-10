#pragma once

#include "Analysis/OnsetAnalysis.hpp"
#include "Audio/RealtimeCommandDispatcher.hpp"
#include "AutoChopProtocol.hpp"
#include "MainSampleProtocol.hpp"
#include "MainSampleSession.hpp"
#include "PadWorkflows.hpp"
#include "SamplerEngine.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace midichopper::plugin {

/**
 * Control-thread Main Sample workflows (MS1 requests). They share the pad
 * mutation mutex with PadWorkflows, so a re-cut never interleaves with a pad
 * workflow, a durable state read or a sample-rate rebuild. Every request
 * publishes exactly one main_sample_status or main_sample_waveform reply
 * (a load also publishes LOADING first). Audio processing must never call
 * this controller. Engine, dispatcher, observer and mutex outlive it.
 */
class MainSampleWorkflows {
public:
    MainSampleWorkflows(SamplerEngine& sampler, sms::audio::RealtimeCommandDispatcher& dispatcher,
                        PadWorkflowObserver& observer, std::mutex& mutationMutex) noexcept
        : sampler_(sampler), controlDispatcher_(dispatcher), observer_(observer),
          mutationMutex_(mutationMutex) {}

    void handleRequest(std::string_view encoded);
    /**
     * AC1: re-chop the source into pads. Publishes one main_sample_status and
     * one auto_chop_status (CONFIRM and ERROR publish only the latter).
     */
    void handleAutoChop(std::string_view encoded);
    /** Host tempo in BPM (0 = unknown), read for mode B/R with bpm 0. Outlives this. */
    void setHostBpm(const std::atomic<double>* bpm) noexcept { hostBpm_ = bpm; }
    /** Called under the mutation lock after Arrange moved pads, so colours (kept per pad) move with them. */
    void setArrangeHook(std::function<void(const std::vector<ArrangeMove>&)> hook) { arrangeHook_ = std::move(hook); }
    /** Number of full analyses run since construction (cache tests). */
    [[nodiscard]] std::uint32_t analysisRuns() const noexcept { return analysisRuns_; }
    /** A copy taken under the mutation mutex. */
    [[nodiscard]] MainSampleSession session() const;

    // Durable keys (host state thread; each call takes the mutation mutex).
    /** SSP1 v2 Base64 of the source, cached per source id; "" without a source. */
    [[nodiscard]] std::string saveAudio() const;
    /** MSS1, or "" without a source. */
    [[nodiscard]] std::string saveSession() const;
    /** LK1 of the pads linked to a slice of the current source; "" when none. */
    [[nodiscard]] std::string saveLinks() const;
    /**
     * Order-independent restore. Audio and session are staged until both are
     * present with matching frames and rate; then the source is queued
     * (restoreSource) and the session adopted. An empty value clears the source
     * and session. Each links value replaces the staged LK1 entries and tags the
     * pads whose audio already matches; restorePad tags later pads on a match.
     * Invalid values change nothing.
     */
    void restoreAudio(const char* value);
    void restoreSession(const char* value);
    void restoreLinks(const char* value);
    /** Undo/redo: apply a snapshot diff under the mutation mutex. */
    struct HistoryChange {
        std::vector<PadImport> imports; // changed audio, one atomic importPads
        struct Settings {
            std::uint32_t pad = 0U;
            sms::dsp::SamplePlaybackSettings playback{};
            sms::dsp::SampleMixerSettings mixer{};
        };
        std::vector<Settings> settings; // settings-only pads
        struct Link { std::uint32_t pad = 0U; std::uint64_t provenance = 0U; std::uint64_t fingerprint = 0U; };
        std::vector<Link> links;        // link-only pads
        const MainSampleSession* session = nullptr; // adopted if the source id still matches
        std::uint64_t affected = 0U;
    };
    /** False (nothing changed) when the pad transaction is refused, e.g. while armed. */
    [[nodiscard]] bool applyHistory(const HistoryChange& change);
    /** Restore pad audio (nullptr clears), with a staged link if its fingerprint matches. */
    void restorePad(std::uint32_t pad, const PadData* audio);

private:
    struct PadSnapshot {
        std::array<std::uint64_t, kPadCount> provenance{};
        std::array<std::uint64_t, kPadCount> generations{};
    };

    void handleRequestImpl(const MainSampleRequest& request);
    void load(const MainSampleRequest& request);
    void clear(const MainSampleRequest& request);
    void edit(const MainSampleRequest& request);
    [[nodiscard]] bool sliceHasFavoritePad(std::uint16_t sliceId) const;
    void assign(const MainSampleRequest& request);
    void remove(const MainSampleRequest& request);
    void arrange(const MainSampleRequest& request);
    void waveform(const MainSampleRequest& request);
    void spectrogram(const MainSampleRequest& request);
    void buildAnalysisLocked(const PadData& whole);
    void autoChop(const AutoChopRequest& request);
    void publishAutoChop(const AutoChopStatus& status);

    /** Re-cut every pad linked to one of sliceIds from target's slices. */
    [[nodiscard]] bool recutLinkedPads(const MainSampleSession& target,
        std::span<const std::uint16_t> sliceIds, const PadSnapshot& pads,
        std::uint64_t& affected);
    [[nodiscard]] PadSnapshot snapshotPads() const;
    [[nodiscard]] EngineSettings engineSettings() const;
    void publishPadSettings(std::uint64_t pads, bool mixer);
    /** Caller holds the mutation mutex. Ready on an empty session reads EMPTY. */
    void publishStatusLocked(std::uint64_t sequence, MainSampleState state,
                             std::string_view text = {}, std::uint64_t affected = 0U);
    void publishStatus(std::uint64_t sequence, MainSampleState state,
                       std::string_view text = {}, std::uint64_t affected = 0U);
    [[nodiscard]] std::uint64_t nextSourceId(std::uint64_t current);

    SamplerEngine& sampler_;
    sms::audio::RealtimeCommandDispatcher& controlDispatcher_;
    PadWorkflowObserver& observer_;
    std::mutex& mutationMutex_;
    /** The newest load or clear; an older load that finishes decoding later is dropped. */
    std::atomic<std::uint64_t> latestLoadSequence_{0U};
    std::uint64_t sourceCounter_ = 0U;  // guarded by mutationMutex_
    MainSampleSession session_;         // guarded by mutationMutex_

    // Onset analysis of the current source, built on first use and never saved.
    struct AnalysisCache {
        std::uint64_t sourceId = 0U;
        sms::analysis::OnsetAnalysis analysis;
        double bpm = 0.0;
    };
    AnalysisCache analysisCache_;       // guarded by mutationMutex_
    std::uint32_t analysisRuns_ = 0U;   // guarded by mutationMutex_
    const std::atomic<double>* hostBpm_ = nullptr;
    std::function<void(const std::vector<ArrangeMove>&)> arrangeHook_;

    // Restore staging and the save cache, all guarded by mutationMutex_.
    struct StagedAudio {
        PadData audio;
        std::uint32_t sampleRate = 0U;
        std::string encoded; // seeds the save cache once the source is adopted
    };
    void applyStagedSourceLocked();
    void clearRestoredSourceLocked();
    std::optional<StagedAudio> stagedAudio_;
    std::optional<MainSampleSession> stagedSession_;
    std::vector<MainSampleLinkRecord> stagedLinks_;
    mutable std::string audioCache_;
    mutable std::uint64_t audioCacheId_ = 0U;
};

} // namespace midichopper::plugin
