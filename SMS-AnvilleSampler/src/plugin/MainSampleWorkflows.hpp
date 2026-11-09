#pragma once

#include "Audio/RealtimeCommandDispatcher.hpp"
#include "MainSampleProtocol.hpp"
#include "MainSampleSession.hpp"
#include "PadWorkflows.hpp"
#include "SamplerEngine.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
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
    /** A copy taken under the mutation mutex. */
    [[nodiscard]] MainSampleSession session() const;

private:
    struct PadSnapshot {
        std::array<std::uint64_t, kPadCount> provenance{};
        std::array<std::uint64_t, kPadCount> generations{};
    };

    void handleRequestImpl(const MainSampleRequest& request);
    void load(const MainSampleRequest& request);
    void clear(const MainSampleRequest& request);
    void edit(const MainSampleRequest& request);
    void assign(const MainSampleRequest& request);
    void remove(const MainSampleRequest& request);
    void waveform(const MainSampleRequest& request);

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
};

} // namespace midichopper::plugin
