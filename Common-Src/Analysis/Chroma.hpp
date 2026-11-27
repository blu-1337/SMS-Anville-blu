// HPCP-style chroma (12 bins, index 0 = C) and global tuning offset from
// spectral peaks of 8192-point Hann frames (hop 4096) of a mono decimated sound.
#pragma once

#include "Analysis/SpectralTools.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

namespace sms::analysis {

using Chroma = std::array<float, 12>;

struct ChromaFrame {
    double seconds = 0.0;  // frame start
    float rms = 0.0F;
    float flatness = 1.0F;
    bool used = true;      // false: noisy (flatness > 0.3) or within 50 ms after an onset
    std::vector<SpectralPeak> peaks;  // 55..5000 Hz, <= 60, sorted by frequency
};

struct ChromaFrames {
    bool cancelled = false;
    std::vector<ChromaFrame> frames;  // <= 2000
};

[[nodiscard]] ChromaFrames chromaFrames(std::span<const float> mono, double rate,
                                        const std::atomic<bool>* cancel);

// Circular mean of peak pitch (weighted by magnitude) over used frames, in cents
// against A440; 0 when the reliability |z| / sum w is below 0.4.
[[nodiscard]] float tuningOffset(std::span<const ChromaFrame> frames);

// RMS-weighted mean of per-frame unit-max chroma (L = 4/3 semitone cos^2 kernel)
// over used frames with first <= index < last; normalised to unit max (all 0 if none).
[[nodiscard]] Chroma chroma(std::span<const ChromaFrame> frames, float tuningCents,
                            std::size_t first = 0, std::size_t last = static_cast<std::size_t>(-1));

// Pitch class of the lowest peak within 20 dB of each frame's maximum, voted by
// frame RMS; -1 when no used frame has peaks.
[[nodiscard]] int bassPitchClass(std::span<const ChromaFrame> frames, float tuningCents,
                                 std::size_t first = 0, std::size_t last = static_cast<std::size_t>(-1));

} // namespace sms::analysis
