// Offline pad sound detection: note + cents, chord and key of a sample region.
// Runs on a worker thread; allocates. Algorithm and constants:
// handoff research note "detection.md" (YIN multi-threshold, harmonic summation,
// HPCP chroma, Krumhansl-Kessler + Temperley key profiles, chord templates).
#pragma once

#include <atomic>
#include <cstdint>
#include <span>

namespace sms::analysis {

enum class ChordQuality : std::uint8_t {
    none, major, minor, dominant7, major7, minor7, diminished, augmented, sus2, sus4, power
};

struct PitchEstimate {
    bool valid = false;
    double midi = 0.0;  // fractional, A4 = 69; cents are always against A440
    double hz = 0.0;
    float confidence = 0.0F;
    bool approximate = false;  // inharmonic/noisy (drum) sound: dominant spectral peak
    bool gliding = false;      // no stable region; the tail of the glide is reported
    bool rangeLimited = false; // clip too short to see down to 25 Hz
};

struct ChordEstimate {
    bool valid = false;
    int root = -1;  // pitch class, 0 = C
    ChordQuality quality = ChordQuality::none;
    float confidence = 0.0F;
    bool varies = false;  // long region whose segments disagree (valid is false then)
};

struct KeyEstimate {
    bool valid = false;
    int tonic = -1;  // pitch class, 0 = C
    bool minor = false;
    float confidence = 0.0F;
    int runnerUpTonic = -1;
    bool runnerUpMinor = false;
};

struct SoundAnalysis {
    bool cancelled = false;
    bool silent = false;
    PitchEstimate note;
    ChordEstimate chord;
    KeyEstimate key;
    float tuningOffsetCents = 0.0F;  // global tuning of the material vs A440, -50..50
};

// Interleaved stereo, any rate 8k-192k (others read as silent). Only the first
// 30 s are analysed. Checks `cancel` per frame; a cancelled result carries
// cancelled = true and nothing else, and must not be cached.
[[nodiscard]] SoundAnalysis analyzeSound(std::span<const float> interleavedStereo, double rate,
                                         const std::atomic<bool>* cancel = nullptr);

} // namespace sms::analysis
