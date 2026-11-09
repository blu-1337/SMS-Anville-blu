#include "Analysis/OnsetAnalysis.hpp"

#include "Analysis/RealFft.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numbers>

namespace sms::analysis {

namespace {
constexpr std::size_t kCancelInterval = 1024;
constexpr std::size_t kMedianRadius = 8;
constexpr std::size_t kPeakRadius = 3;
constexpr std::int64_t kRefineBlock = 64;

float medianOf(const std::span<const float> values)
{
    std::array<float, 2 * kMedianRadius + 1> tmp{};
    const std::size_t n = std::min(values.size(), tmp.size());
    if (n == 0) {
        return 0.0F;
    }
    std::copy_n(values.begin(), n, tmp.begin());
    std::nth_element(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(n / 2),
                     tmp.begin() + static_cast<std::ptrdiff_t>(n));
    return tmp[n / 2];
}

float peakAbs(const std::span<const float> stereo, const std::int64_t frame)
{
    const std::size_t i = static_cast<std::size_t>(frame) * 2;
    return std::max(std::fabs(stereo[i]), std::fabs(stereo[i + 1]));
}

float monoAt(const std::span<const float> stereo, const std::int64_t frame)
{
    const std::size_t i = static_cast<std::size_t>(frame) * 2;
    return stereo[i] + stereo[i + 1];
}

// Moves a coarse estimate to the first block of the attack at full rate, then
// back to the previous zero crossing.
std::int64_t refine(const std::span<const float> stereo, const std::int64_t frames, const std::int64_t estimate,
                    const std::int64_t before, const std::int64_t after, const std::int64_t zeroSearch)
{
    const std::int64_t lo = std::clamp<std::int64_t>(estimate - before, 0, frames - 1);
    const std::int64_t hi = std::clamp<std::int64_t>(estimate + after, lo, frames - 1);
    float floorLevel = 1e30F;
    float peak = 0.0F;
    for (std::int64_t b = lo; b <= hi; b += kRefineBlock) {
        float env = 0.0F;
        const std::int64_t end = std::min(b + kRefineBlock, hi + 1);
        for (std::int64_t i = b; i < end; ++i) {
            env = std::max(env, peakAbs(stereo, i));
        }
        floorLevel = std::min(floorLevel, env);
        peak = std::max(peak, env);
    }
    if (peak - floorLevel < 1e-6F) {
        return std::clamp<std::int64_t>(estimate, 0, frames - 1);
    }
    const float level = floorLevel + 0.2F * (peak - floorLevel);
    std::int64_t found = lo;
    bool hit = false;
    for (std::int64_t b = lo; b <= hi && !hit; b += kRefineBlock) {
        const std::int64_t end = std::min(b + kRefineBlock, hi + 1);
        for (std::int64_t i = b; i < end; ++i) {
            if (peakAbs(stereo, i) > level) {
                found = b;
                hit = true;
                break;
            }
        }
    }
    const std::int64_t stop = std::max<std::int64_t>(0, found - zeroSearch);
    for (std::int64_t i = found; i > stop; --i) {
        if ((monoAt(stereo, i) >= 0.0F) != (monoAt(stereo, i - 1) >= 0.0F)) {
            return i;
        }
    }
    return found;
}
} // namespace

OnsetAnalysis analyzeOnsets(const std::span<const float> interleavedStereo, const double rate,
                            const OnsetSettings& settings, const std::atomic<bool>* const cancel)
{
    OnsetAnalysis out;
    out.sourceRate = rate;
    const std::int64_t frames = static_cast<std::int64_t>(interleavedStereo.size() / 2);
    out.sourceFrames = frames;
    const std::size_t window = std::max<std::size_t>(settings.window, 16);
    const std::size_t hop = std::max<std::size_t>(settings.hop, 1);
    if (!(rate > 0.0) || frames < static_cast<std::int64_t>(window)) {
        return out;
    }
    const std::size_t decimation =
        static_cast<std::size_t>(std::max(1.0, std::floor(rate / settings.analysisRate + 0.25)));
    const double analysisRate = rate / static_cast<double>(decimation);
    out.hopSeconds = static_cast<double>(hop) / analysisRate;

    const std::size_t monoCount = static_cast<std::size_t>(frames) / decimation;
    if (monoCount < window) {
        return out;
    }
    std::vector<float> mono(monoCount);
    const float norm = 0.5F / static_cast<float>(decimation);
    for (std::size_t i = 0; i < monoCount; ++i) {
        float sum = 0.0F;
        for (std::size_t j = 0; j < decimation; ++j) {
            const std::size_t f = i * decimation + j;
            sum += interleavedStereo[f * 2] + interleavedStereo[f * 2 + 1];
        }
        mono[i] = sum * norm;
    }
    const std::size_t hops = (monoCount - window) / hop + 1;

    std::vector<float> hann(window);
    for (std::size_t i = 0; i < window; ++i) {
        hann[i] = static_cast<float>(
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(window)));
    }
    RealFft fft(window);
    const std::size_t binCount = window / 2 + 1;
    const float scale = 4.0F / static_cast<float>(window);
    const std::size_t lowEnd =
        std::min(binCount, static_cast<std::size_t>(250.0 * static_cast<double>(window) / analysisRate) + 1);
    const std::size_t midEnd =
        std::min(binCount, static_cast<std::size_t>(4000.0 * static_cast<double>(window) / analysisRate) + 1);

    std::vector<float> windowed(window);
    std::vector<float> mags(binCount);
    std::vector<float> previous(binCount, 0.0F);
    std::vector<float> flux(hops, 0.0F);
    out.rms.resize(hops);
    out.bands.resize(hops);
    for (std::size_t n = 0; n < hops; ++n) {
        if (n % kCancelInterval == 0 && cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            out = OnsetAnalysis{};
            out.sourceRate = rate;
            out.sourceFrames = frames;
            out.cancelled = true;
            return out;
        }
        const float* const seg = &mono[n * hop];
        float energy = 0.0F;
        for (std::size_t i = 0; i < window; ++i) {
            windowed[i] = seg[i] * hann[i];
            energy += seg[i] * seg[i];
        }
        out.rms[n] = std::sqrt(energy / static_cast<float>(window));
        fft.magnitudes(windowed, mags);
        float sum = 0.0F;
        float low = 0.0F;
        float mid = 0.0F;
        float high = 0.0F;
        for (std::size_t k = 0; k < binCount; ++k) {
            const float m = mags[k] * scale;
            const float y = std::log1p(100.0F * m);
            if (n > 0) {
                sum += std::max(0.0F, y - previous[k]);
            }
            previous[k] = y;
            (k < lowEnd ? low : (k < midEnd ? mid : high)) += m * m;
        }
        flux[n] = sum;
        out.bands[n] = {std::sqrt(low), std::sqrt(mid), std::sqrt(high)};
    }

    // Normalize by the 99th percentile, but never amplify a quiet signal.
    std::vector<float> sorted = flux;
    const std::size_t p99 = std::min(hops - 1, static_cast<std::size_t>(0.99 * static_cast<double>(hops)));
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(p99), sorted.end());
    const float denom = std::max(sorted[p99], settings.fluxFloor);
    out.novelty.resize(hops);
    for (std::size_t n = 0; n < hops; ++n) {
        out.novelty[n] = std::clamp(flux[n] / denom, 0.0F, 1.0F);
    }

    const std::int64_t d = static_cast<std::int64_t>(decimation);
    const std::int64_t zeroSearch = static_cast<std::int64_t>(settings.refineWindowMs * 0.001 * rate);
    for (std::size_t n = 1; n < hops; ++n) {
        const float v = out.novelty[n];
        if (v < settings.candidateFloor) {
            continue;
        }
        bool isPeak = true;
        for (std::size_t r = 1; r <= kPeakRadius && isPeak; ++r) {
            if (n >= r && out.novelty[n - r] > v) {
                isPeak = false;
            }
            if (n + r < hops && out.novelty[n + r] >= v) {
                isPeak = false;
            }
        }
        if (!isPeak) {
            continue;
        }
        const std::size_t from = n >= kMedianRadius ? n - kMedianRadius : 0;
        const std::size_t to = std::min(hops, n + kMedianRadius + 1);
        const float med = medianOf(std::span<const float>(out.novelty).subspan(from, to - from));
        const std::int64_t estimate =
            (static_cast<std::int64_t>(n * hop) + static_cast<std::int64_t>(window / 2)) * d;
        // The log-compressed flux peaks when an attack first enters the window's
        // right edge, so search from one hop before the centre to the right edge.
        const std::int64_t before = static_cast<std::int64_t>(hop) * d;
        const std::int64_t after = static_cast<std::int64_t>(window / 2) * d;
        out.candidates.push_back({refine(interleavedStereo, frames, estimate, before, after, zeroSearch), v, med});
    }
    std::sort(out.candidates.begin(), out.candidates.end(),
              [](const Onset& a, const Onset& b) { return a.frame < b.frame; });
    return out;
}

std::vector<std::int64_t> pickOnsets(const OnsetAnalysis& analysis, const float sensitivity,
                                     const double minGapSeconds, const std::int64_t rangeStart,
                                     const std::int64_t rangeEnd)
{
    const float s = std::clamp(sensitivity, 0.0F, 1.0F);
    const float delta = 0.35F + (0.02F - 0.35F) * s;
    std::vector<const Onset*> eligible;
    for (const Onset& o : analysis.candidates) {
        if (o.frame >= rangeStart && o.frame < rangeEnd && o.strength >= delta + o.localMedian) {
            eligible.push_back(&o);
        }
    }
    std::stable_sort(eligible.begin(), eligible.end(),
                     [](const Onset* a, const Onset* b) { return a->strength > b->strength; });
    const std::int64_t gap = static_cast<std::int64_t>(std::max(0.0, minGapSeconds) * analysis.sourceRate);
    std::vector<std::int64_t> picked;
    for (const Onset* o : eligible) {
        const bool clear = std::none_of(picked.begin(), picked.end(), [&](const std::int64_t f) {
            return std::llabs(f - o->frame) < gap;
        });
        if (clear) {
            picked.push_back(o->frame);
        }
    }
    std::sort(picked.begin(), picked.end());
    return picked;
}

} // namespace sms::analysis
