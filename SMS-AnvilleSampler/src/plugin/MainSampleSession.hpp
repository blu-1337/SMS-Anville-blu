#pragma once

// DPF-free Main Sample session: the source identity, its slices, and the
// durable MSS1 (session) and LK1 (pad links) codecs.

#include "MainSampleProtocol.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace midichopper::plugin {

struct MainSampleSession {
    std::uint64_t sourceId = 0U; // 48 bits; 0 = no source
    std::uint32_t frames = 0U;
    std::uint32_t sampleRate = 0U; // whole hertz of the stored source
    std::uint16_t nextSliceId = 1U; // 1..65535
    std::vector<MainSampleSlice> slices; // at most 64, sorted by start
    std::string name; // at most 128 bytes, cut on a UTF-8 boundary

    [[nodiscard]] bool empty() const noexcept { return sourceId == 0U; }
    void reset(std::uint64_t id, std::uint32_t frameCount, std::uint32_t rate,
               std::string_view sourceName);
    [[nodiscard]] const MainSampleSlice* find(std::uint16_t id) const noexcept;
    /** Adds [start, end) under a fresh id; fails when full or out of range. */
    [[nodiscard]] bool addSlice(std::uint32_t start, std::uint32_t end, MainSampleSlice& created);
    /** Applies one or two edits to existing slices together, or none of them. */
    [[nodiscard]] bool editSlices(std::span<const MainSampleSlice> edits);
    [[nodiscard]] bool removeSlice(std::uint16_t id);
};

/** Pad link saved with a project: restored only onto audio with this fingerprint. */
struct MainSampleLinkRecord {
    std::uint32_t pad = 0U;
    std::uint64_t provenance = 0U;
    std::uint32_t frames = 0U;
    std::uint64_t fingerprint = 0U;
    friend bool operator==(const MainSampleLinkRecord&, const MainSampleLinkRecord&) = default;
};

/** Empty for an empty session, which the durable key treats as "clear". */
[[nodiscard]] std::string encodeMainSampleSession(const MainSampleSession& session);
[[nodiscard]] bool decodeMainSampleSession(std::string_view encoded, MainSampleSession& session);
[[nodiscard]] std::string encodeMainSampleLinks(std::span<const MainSampleLinkRecord> links);
[[nodiscard]] bool decodeMainSampleLinks(std::string_view encoded,
                                         std::vector<MainSampleLinkRecord>& links);
/** SplitMix64 of the seed, masked to a nonzero 48-bit source id. */
[[nodiscard]] std::uint64_t makeMainSampleSourceId(std::uint64_t seed) noexcept;

} // namespace midichopper::plugin
