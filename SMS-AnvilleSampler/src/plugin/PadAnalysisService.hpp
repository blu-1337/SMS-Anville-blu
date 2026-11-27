#pragma once

#include "Analysis/SoundAnalysis.hpp"
#include "PadAnalysisProtocol.hpp"
#include "PadWorkflows.hpp"
#include "SamplerEngine.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace midichopper::plugin {

/** Pad sound detection on the control worker. Copies published pad audio under
 * the mutation mutex and analyses it unlocked; never dispatches to or waits for
 * the audio thread. Results are cached per pad and never persisted. Engine,
 * mutex and observer outlive this instance; stop the worker (after cancel())
 * before destroying it. */
class PadAnalysisService {
public:
    using Analyzer = std::function<sms::analysis::SoundAnalysis(
        std::span<const float>, double, const std::atomic<bool>*)>;

    PadAnalysisService(const SamplerEngine& sampler, std::mutex& mutation,
                       PadWorkflowObserver& observer)
        : sampler_(sampler), mutation_(mutation), observer_(observer) {}

    /** Worker thread only: the cache is unsynchronised. */
    void handleRequest(std::string_view encoded);
    /** Any thread, permanent: a running analysis returns and nothing more is published. */
    void cancel() noexcept { cancel_.store(true, std::memory_order_relaxed); }
    /** Test seam; call before the first request. */
    void setAnalyzer(Analyzer analyzer) { analyzer_ = std::move(analyzer); }

private:
    struct Entry {
        std::uint64_t generation = 0U;
        float start = 0.0F;
        float end = 1.0F;
        // Staged host restores replace audio without moving the generation.
        std::uint64_t audioHash = 0U;
        sms::analysis::SoundAnalysis result{};
    };

    const SamplerEngine& sampler_;
    std::mutex& mutation_;
    PadWorkflowObserver& observer_;
    Analyzer analyzer_ = &sms::analysis::analyzeSound;
    std::array<std::optional<Entry>, kPadCount> cache_{};
    std::atomic<bool> cancel_{false};
};

} // namespace midichopper::plugin
