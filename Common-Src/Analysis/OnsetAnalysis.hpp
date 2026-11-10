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
    std::uint8_t band = 0;   // spectral candidates: source band (high bands use a shorter gap)
};

// Spectral (TRANSIENT 2) analysis splits the spectrum into log-spaced bands:
// <150, 150-600, 600-2.5k, 2.5k-4.5k, 4.5k-6.5k, 6.5k-10k and >10k Hz. Each band's flux is normalized on
// its own, so a quiet high band counts as much as a loud low band.
inline constexpr std::size_t kSpectralBands = 7;
// Bands from this index up (>= 2.5 kHz) are normalized against their local envelope
// and may use a shorter minimum gap, so soft hats next to loud hits still count.
inline constexpr std::size_t kFirstHighBand = 3;
inline constexpr double kHighBandMinGapSeconds = 0.030;

// Low-resolution log-frequency spectrogram kept with the analysis (one row of
// kSpectrogramBins bytes per hop): bin b spans 30 Hz * (10000/30)^(b/48) upward,
// level 0..255 maps -84..0 dBFS (a full-scale sine reads about 255).
inline constexpr std::size_t kSpectrogramBins = 48;
inline constexpr double kSpectrogramLowHz = 30.0;
inline constexpr double kSpectrogramHighHz = 10000.0;
[[nodiscard]] std::size_t spectrogramBinForHz(double hz) noexcept;

struct OnsetAnalysis {
    double sourceRate = 0.0;
    std::int64_t sourceFrames = 0;
    double hopSeconds = 0.0;
    bool cancelled = false;
    std::vector<float> novelty;                 // normalized flux per hop, 0..1
    std::vector<float> rms;                     // per hop, at the analysis rate
    std::vector<std::array<float, 3>> bands;    // <250 Hz, 250 Hz..4 kHz, >4 kHz magnitude
    std::vector<Onset> candidates;              // sorted by frame
    // Per-band normalized flux per hop (0..1) and the merged per-band candidates
    // (sorted by frame, strength relative to the band's own median).
    std::vector<std::array<float, kSpectralBands>> bandNovelty;
    std::vector<Onset> spectralCandidates;
    std::vector<std::uint8_t> spectrogram;      // hops x kSpectrogramBins, row per hop
};

// Resamples the cached spectrogram to `columns` columns over [start, end) source
// frames (max over the hops of each column, so transients stay visible). Writes
// columns x kSpectrogramBins bytes, column-major. Returns false for an empty
// analysis or range.
[[nodiscard]] bool spectrogramColumns(const OnsetAnalysis& analysis, std::int64_t start, std::int64_t end,
                                      std::size_t columns, std::span<std::uint8_t> out);

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

// Like pickOnsets() but over the per-band candidates (TRANSIENT 2).
[[nodiscard]] std::vector<std::int64_t> pickSpectralOnsets(const OnsetAnalysis& analysis, float sensitivity,
                                                           double minGapSeconds, std::int64_t rangeStart,
                                                           std::int64_t rangeEnd);

} // namespace sms::analysis
