#pragma once

// DPF-free Main Sample session: the source identity, its slices, and the
// durable MSS1 (session) and LK1 (pad links) codecs.

#include "MainSampleProtocol.hpp"

#include <algorithm>
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
    std::uint32_t rangeStart = 0U; // non-destructive source range [rangeStart, rangeEnd)
    std::uint32_t rangeEnd = 0U;   // equals frames when the whole source is used
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
    /** Sets the working range; fails (unchanged) unless start < end <= frames. */
    [[nodiscard]] bool setRange(std::uint32_t start, std::uint32_t end) noexcept;
    /** rangeEnd 0 (a session built by hand) also means the whole source. */
    [[nodiscard]] std::uint32_t rangeEndFrame() const noexcept
    { return rangeEnd == 0U || rangeEnd > frames ? frames : rangeEnd; }
    [[nodiscard]] bool rangeIsFull() const noexcept { return rangeStart == 0U && rangeEndFrame() == frames; }
};

/** Result of re-indexing the slices after a destructive cut or crop of the source. */
struct SliceRemap {
    std::vector<MainSampleSlice> slices;   // survivors, new positions, sorted
    std::vector<std::uint16_t> removed;    // dropped (inside the cut, outside the crop, or too short)
    std::vector<std::uint16_t> changed;    // survivors whose length changed
};

/**
 * Remaps slices for removing [a, b) (crop = false) or keeping only [a, b) (crop = true).
 * Cut: before stays, after shifts left by b-a, a straddling slice is shortened, inside is removed.
 * A result shorter than minFrames is removed. Pure function; no I/O.
 */
[[nodiscard]] inline SliceRemap remapSlicesForCut(const std::vector<MainSampleSlice>& input,
    const std::uint32_t a, const std::uint32_t b, const bool crop, const std::uint32_t minFrames)
{
    SliceRemap result;
    const std::uint32_t cut = b - a;
    for (const auto& slice : input) {
        std::uint32_t start = slice.start;
        std::uint32_t end = slice.end;
        bool gone = false;
        if (crop) {
            start = std::max(slice.start, a);
            end = std::min(slice.end, b);
            gone = start >= end;
            if (!gone) {
                start -= a;
                end -= a;
            }
        } else if (slice.end <= a) {
            // before the cut: unchanged
        } else if (slice.start >= b) {
            start -= cut;
            end -= cut;
        } else if (slice.start >= a && slice.end <= b) {
            gone = true;
        } else if (slice.start < a && slice.end <= b) {
            end = a;
        } else if (slice.start >= a) { // straddles the cut end
            start = a;
            end = slice.end - cut;
        } else {                        // spans the whole cut
            end = slice.end - cut;
        }
        if (gone || end - start < minFrames) {
            result.removed.push_back(slice.id);
            continue;
        }
        if (end - start != slice.end - slice.start)
            result.changed.push_back(slice.id);
        result.slices.push_back({slice.id, start, end});
    }
    return result;
}

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
