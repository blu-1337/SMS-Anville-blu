// Tempo from autocorrelation of the novelty curve, weighted by a log-Gaussian
// prior centred on 120 BPM.
#pragma once

#include <span>

namespace sms::analysis {

struct TempoEstimate {
    double bpm = 0.0;         // 0 when there is not enough material
    double confidence = 0.0;  // 0..1
};

[[nodiscard]] TempoEstimate estimateTempo(std::span<const float> novelty, double hopSeconds,
                                          double minBpm = 70.0, double maxBpm = 180.0);

} // namespace sms::analysis
