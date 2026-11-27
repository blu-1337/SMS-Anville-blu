// Note + cents of a mono (decimated) sound: YIN with a Beta(2,18) prior over
// thresholds, stable-region selection, harmonic summation with octave tests on
// a long spectrum, and an "approximate" dominant-peak mode for drums.
#pragma once

#include "Analysis/SoundAnalysis.hpp"
#include "Analysis/SpectralTools.hpp"

#include <atomic>
#include <span>
#include <vector>

namespace sms::analysis {

struct PitchAnalysis {
    PitchEstimate estimate;
    bool cancelled = false;
    // Short usable region (< 150 ms), approximate (drum) mode or glide: no chord or key.
    bool percussive = false;
    // The body spectrum is explained by one harmonic series (chord gate 2).
    bool singleNote = false;
    double usableSeconds = 0.0;
    float flatness = 1.0F;  // body spectrum, 40..5000 Hz
};

[[nodiscard]] PitchAnalysis analysePitch(std::span<const float> mono, double rate,
                                         const std::atomic<bool>* cancel);

[[nodiscard]] PitchEstimate trackPitch(std::span<const float> mono, double rate,
                                       const std::atomic<bool>* cancel);

// Chord gate 2: some f0 explains >= 90% of the peak amplitude with partials
// within 10 cents, one of partials 1..3 is present, and partial 1, 5 or 7 is.
[[nodiscard]] bool explainedBySingleNote(std::span<const SpectralPeak> peaks);

} // namespace sms::analysis
