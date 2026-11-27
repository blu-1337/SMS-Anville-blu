#include "Analysis/SoundAnalysis.hpp"

#include "Analysis/Chroma.hpp"
#include "Analysis/KeyChord.hpp"
#include "Analysis/PitchTracker.hpp"
#include "Analysis/SoundPreprocess.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace sms::analysis {

namespace {
constexpr double kMaxSeconds = 30.0;
constexpr double kSegmentSeconds = 0.5;
constexpr double kLoopSeconds = 4.0;    // segment-wise chord check from this length on [D]
constexpr double kLabelCoverage = 0.6;

bool isCancelled(const std::atomic<bool>* cancel) noexcept
{
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

SoundAnalysis cancelledResult()
{
    SoundAnalysis result;
    result.cancelled = true;
    return result;
}

// Section 9.5: classify ~0.5 s segments; without a label covering >= 60% the chord "varies".
bool chordVaries(const std::vector<ChromaFrame>& frames, const float tuning, const ChordEstimate& whole)
{
    std::vector<std::pair<int, ChordQuality>> labels;
    std::size_t first = 0;
    while (first < frames.size()) {
        const double segmentStart = std::floor(frames[first].seconds / kSegmentSeconds);
        std::size_t last = first;
        while (last < frames.size() && std::floor(frames[last].seconds / kSegmentSeconds) == segmentStart) {
            ++last;
        }
        const bool anyUsed = std::any_of(frames.begin() + static_cast<std::ptrdiff_t>(first),
                                         frames.begin() + static_cast<std::ptrdiff_t>(last),
                                         [](const ChromaFrame& f) { return f.used; });
        if (anyUsed) {
            const ChordEstimate c = chordFrom(chroma(frames, tuning, first, last),
                                              bassPitchClass(frames, tuning, first, last), false);
            labels.emplace_back(c.valid ? c.root : -1, c.quality);
        }
        first = last;
    }
    if (labels.size() < 2) {
        return false;
    }
    std::size_t bestCount = 0;
    for (const auto& label : labels) {
        bestCount = std::max(bestCount, static_cast<std::size_t>(std::count(labels.begin(), labels.end(), label)));
    }
    const std::size_t wholeCount = whole.valid
        ? static_cast<std::size_t>(std::count(labels.begin(), labels.end(), std::make_pair(whole.root, whole.quality)))
        : bestCount;
    return static_cast<double>(wholeCount) < kLabelCoverage * static_cast<double>(labels.size());
}
} // namespace

SoundAnalysis analyzeSound(const std::span<const float> interleavedStereo, const double rate,
                           const std::atomic<bool>* cancel)
{
    if (isCancelled(cancel)) {
        return cancelledResult();
    }
    SoundAnalysis result;
    std::span<const float> input = interleavedStereo;
    if (rate > 0.0 && std::isfinite(rate)) {
        const double maxFrames = std::floor(kMaxSeconds * rate);
        if (static_cast<double>(input.size() / 2) > maxFrames) {
            input = input.first(2 * static_cast<std::size_t>(maxFrames));
        }
    }
    double fs = 0.0;
    bool silent = true;
    const std::vector<float> mono = monoDecimated(input, rate, fs, silent);
    if (silent || mono.empty()) {
        result.silent = true;
        return result;
    }
    if (isCancelled(cancel)) {
        return cancelledResult();
    }
    const PitchAnalysis pitch = analysePitch(mono, fs, cancel);
    if (pitch.cancelled) {
        return cancelledResult();
    }
    result.note = pitch.estimate;
    const ChromaFrames frames = chromaFrames(mono, fs, cancel);
    if (frames.cancelled) {
        return cancelledResult();
    }
    const float tuning = tuningOffset(frames.frames);
    result.tuningOffsetCents = tuning;
    const Chroma whole = chroma(frames.frames, tuning);
    const double seconds = static_cast<double>(mono.size()) / fs;
    if (!pitch.singleNote) {
        result.chord = chordFrom(whole, bassPitchClass(frames.frames, tuning), pitch.percussive);
        if (!pitch.percussive && seconds >= kLoopSeconds && chordVaries(frames.frames, tuning, result.chord)) {
            result.chord = ChordEstimate{};
            result.chord.varies = true;
        }
    }
    result.key = keyFrom(whole, seconds, pitch.percussive);
    return result;
}

} // namespace sms::analysis
