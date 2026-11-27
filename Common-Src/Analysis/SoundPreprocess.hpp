// Sound-analysis preprocessing: stereo to mono (anti-phase safe), DC removal,
// -60 dBFS silence gate and integer decimation towards ~22 kHz.
#pragma once

#include <span>
#include <vector>

namespace sms::analysis {

inline constexpr double kAnalysisMinRate = 8000.0;
inline constexpr double kAnalysisMaxRate = 192000.0;

// Mono = (L+R)/2, or the louder channel when the mix loses more than 6 dB to
// phase cancellation. Non-finite samples count as 0. Decimates by
// D = max(1, floor(rate / 22050)) with a 63*D-tap Blackman windowed sinc
// (cutoff 0.45 of the output rate). Returns an empty vector and silent = true
// for an unsupported rate, empty input or a peak below -60 dBFS.
[[nodiscard]] std::vector<float> monoDecimated(std::span<const float> interleavedStereo, double rate,
                                               double& outRate, bool& silent);

} // namespace sms::analysis
