// Spectral-flux onset analysis. Run once per source (control thread), then pick
// onsets many times from the cached candidates with pickOnsets().
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sms::analysis {

struct OnsetSettings {
    std::size_t window = 1024;       // FFT size at the analysis rate (power of two)
    std::size_t hop = 256;
    double analysisRate = 22050.0;   // decimate by an integer factor towards this
    float fluxFloor = 6.0F;          // raw flux below this never normalizes up to onset level
    float candidateFloor = 0.02F;    // normalized flux needed to be a candidate
    double refineWindowMs = 2.0;     // zero-crossing search distance
};

struct Onset {
    std::int64_t frame = 0;  // refined source frame
    float strength = 0.0F;   // normalized flux, 0..1
    float localMedian = 0.0F;
};

struct OnsetAnalysis {
    double sourceRate = 0.0;
    std::int64_t sourceFrames = 0;
    double hopSeconds = 0.0;
    bool cancelled = false;
    std::vector<float> novelty;                 // normalized flux per hop, 0..1
    std::vector<float> rms;                     // per hop, at the analysis rate
    std::vector<std::array<float, 3>> bands;    // <250 Hz, 250 Hz..4 kHz, >4 kHz magnitude
    std::vector<Onset> candidates;              // sorted by frame
};

// Interleaved stereo float. Checks `cancel` every 1024 hops; a cancelled
// analysis has `cancelled` set and must not be cached.
[[nodiscard]] OnsetAnalysis analyzeOnsets(std::span<const float> interleavedStereo, double rate,
                                          const OnsetSettings& settings = {},
                                          const std::atomic<bool>* cancel = nullptr);

// Sensitivity 0..1: threshold delta = lerp(0.35, 0.02, sensitivity). Returns
// frames in [rangeStart, rangeEnd) sorted ascending; greedy by strength with a
// minimum gap.
[[nodiscard]] std::vector<std::int64_t> pickOnsets(const OnsetAnalysis& analysis, float sensitivity,
                                                   double minGapSeconds, std::int64_t rangeStart,
                                                   std::int64_t rangeEnd);

} // namespace sms::analysis
