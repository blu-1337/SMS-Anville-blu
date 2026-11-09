#include "Analysis/TempoEstimate.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace sms::analysis {

TempoEstimate estimateTempo(const std::span<const float> novelty, const double hopSeconds,
                            const double minBpm, const double maxBpm)
{
    TempoEstimate result;
    if (!(hopSeconds > 0.0) || !(minBpm > 0.0) || !(maxBpm > minBpm) || novelty.size() < 16) {
        return result;
    }
    const double maxLagD = 60.0 / minBpm / hopSeconds;
    const std::size_t maxLag = static_cast<std::size_t>(std::ceil(maxLagD)) + 2;
    if (novelty.size() < 2 * maxLag) {
        return result;
    }
    const double mean = std::accumulate(novelty.begin(), novelty.end(), 0.0) / static_cast<double>(novelty.size());
    std::vector<double> x(novelty.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<double>(novelty[i]) - mean;
    }
    std::vector<double> ac(maxLag + 1, 0.0);
    for (std::size_t lag = 0; lag <= maxLag; ++lag) {
        double sum = 0.0;
        for (std::size_t i = lag; i < x.size(); ++i) {
            sum += x[i] * x[i - lag];
        }
        ac[lag] = sum / static_cast<double>(x.size() - lag);
    }
    if (!(ac[0] > 1e-12)) {
        return result;
    }
    double bestScore = -1.0;
    double bestBpm = 0.0;
    double bestAc = 0.0;
    for (double bpm = minBpm; bpm <= maxBpm; bpm += 0.25) {
        const double lag = 60.0 / bpm / hopSeconds;
        const std::size_t i0 = static_cast<std::size_t>(lag);
        const double frac = lag - static_cast<double>(i0);
        const double a = ac[i0] * (1.0 - frac) + ac[std::min(i0 + 1, maxLag)] * frac;
        const double octaves = std::log2(bpm / 120.0);
        const double prior = std::exp(-0.5 * octaves * octaves);  // sigma = 1 octave
        const double score = a * prior;
        if (score > bestScore) {
            bestScore = score;
            bestBpm = bpm;
            bestAc = a;
        }
    }
    result.bpm = bestBpm;
    result.confidence = std::clamp(bestAc / ac[0], 0.0, 1.0);
    return result;
}

} // namespace sms::analysis
