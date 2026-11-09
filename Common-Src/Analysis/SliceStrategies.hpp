// Slice selection over a cached OnsetAnalysis. Every function returns regions
// sorted by start, clamped to the context range, each at least `minFrames` and
// at most `maxFrames` long. Fewer regions than requested is a valid result.
#pragma once

#include "Analysis/OnsetAnalysis.hpp"

#include <cstdint>
#include <vector>

namespace sms::analysis {

struct SliceRegion {
    std::int64_t start = 0;
    std::int64_t end = 0;
};

struct SliceContext {
    const OnsetAnalysis& analysis;
    std::int64_t rangeStart = 0;
    std::int64_t rangeEnd = 0;   // exclusive
    std::int64_t minFrames = 1;
    std::int64_t maxFrames = 0;  // 0 = unlimited
    double bpm = 0.0;            // 0 = unknown (beat-based modes return nothing)
};

// Transient: first `maxSlices` onsets in time order; slice i = [onset_i, onset_i+1),
// the last one ends at the range end. Never invents cuts.
[[nodiscard]] std::vector<SliceRegion> transientSlices(const SliceContext& ctx, std::size_t maxSlices,
                                                       float sensitivity, double minGapSeconds);

// Find Samples: musically useful regions of 1, 2 or 4 beats picked by score and
// diversity, spread across the range. A nonzero seed jitters scores by +-10%.
[[nodiscard]] std::vector<SliceRegion> findSamples(const SliceContext& ctx, std::size_t count,
                                                   std::uint64_t seed);

// Equal: `count` equal parts of the range; the remainder is spread across them.
[[nodiscard]] std::vector<SliceRegion> equalSlices(const SliceContext& ctx, std::size_t count);

// Beat: slices of beatsPerSlice beats from `anchorFrame`, boundaries computed
// cumulatively in double precision (no drift), optionally snapped to onsets within 30 ms.
[[nodiscard]] std::vector<SliceRegion> beatSlices(const SliceContext& ctx, std::size_t count,
                                                  double beatsPerSlice, std::int64_t anchorFrame,
                                                  bool snapToTransients);

// Random: `count` non-overlapping regions of `lengthBeats` at seeded positions,
// optionally snapped to an onset within a quarter beat.
[[nodiscard]] std::vector<SliceRegion> randomSlices(const SliceContext& ctx, std::size_t count,
                                                    double lengthBeats, std::uint64_t seed,
                                                    bool snapToTransients);

} // namespace sms::analysis
