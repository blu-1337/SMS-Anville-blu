#pragma once

#include <algorithm>
#include <cmath>

namespace sms::dsp {

inline constexpr float kMinimumSampleGainDecibels = -60.0f;
inline constexpr float kMaximumSampleGainDecibels = 12.0f;
inline constexpr float kMinimumSamplePan = -1.0f;
inline constexpr float kMaximumSamplePan = 1.0f;
inline constexpr float kMinimumTuneSemitones = -24.0f;
inline constexpr float kMaximumTuneSemitones = 24.0f;
inline constexpr float kMinimumFilterAmount = 0.0f;
inline constexpr float kMaximumFilterAmount = 1.0f;
inline constexpr int kChokeGroupCount = 8;
inline constexpr int kAuxOutputCount = 16; // pad outputs 1..16 after Main (0)

/** Per-sample level, balance, varispeed, color, direction, and global routing. */
struct SampleMixerSettings {
    float gainDecibels = 0.0f;
    float pan = 0.0f;
    float tuneSemitones = 0.0f;
    float lowpass = 0.0f;
    float highpass = 0.0f;
    float filterSlope = 0.0f; // 0 = 6, 1 = 12, 2 = 24 dB/octave
    float dirty = 0.0f;
    float reverse = 0.0f;
    float globalMixer = 1.0f;
    float chokeGroup = 0.0f; // 0 = off, 1..8 = pads in one group cut each other
    float outputBus = 0.0f;  // 0 = main stereo out, 1..16 = auxiliary stereo out
    float holdLoop = 1.0f;   // HOLD mode: 1 = loop while held (default), 0 = play once
};

[[nodiscard]] inline SampleMixerSettings sanitize(SampleMixerSettings value) noexcept
{
    const auto finiteOr = [](const float candidate, const float fallback) noexcept {
        return std::isfinite(candidate) ? candidate : fallback;
    };
    value.gainDecibels = std::clamp(finiteOr(value.gainDecibels, 0.0f),
                                    kMinimumSampleGainDecibels,
                                    kMaximumSampleGainDecibels);
    value.pan = std::clamp(finiteOr(value.pan, 0.0f),
                           kMinimumSamplePan, kMaximumSamplePan);
    value.tuneSemitones = std::clamp(finiteOr(value.tuneSemitones, 0.0f),
                                     kMinimumTuneSemitones,
                                     kMaximumTuneSemitones);
    value.lowpass = std::clamp(finiteOr(value.lowpass, 0.0f),
                               kMinimumFilterAmount, kMaximumFilterAmount);
    value.highpass = std::clamp(finiteOr(value.highpass, 0.0f),
                                kMinimumFilterAmount, kMaximumFilterAmount);
    value.filterSlope = std::clamp(std::round(finiteOr(value.filterSlope, 0.0f)), 0.0f, 2.0f);
    value.dirty = finiteOr(value.dirty, 0.0f) >= 0.5f ? 1.0f : 0.0f;
    value.reverse = finiteOr(value.reverse, 0.0f) >= 0.5f ? 1.0f : 0.0f;
    value.globalMixer = finiteOr(value.globalMixer, 1.0f) >= 0.5f ? 1.0f : 0.0f;
    value.chokeGroup = std::clamp(std::round(finiteOr(value.chokeGroup, 0.0f)), 0.0f,
                                  static_cast<float>(kChokeGroupCount));
    value.outputBus = std::clamp(std::round(finiteOr(value.outputBus, 0.0f)), 0.0f,
                                 static_cast<float>(kAuxOutputCount));
    value.holdLoop = finiteOr(value.holdLoop, 1.0f) >= 0.5f ? 1.0f : 0.0f;
    return value;
}

[[nodiscard]] inline float sampleGain(const SampleMixerSettings& requested) noexcept
{
    return std::pow(10.0f, sanitize(requested).gainDecibels / 20.0f);
}

[[nodiscard]] inline double tuneRatio(const SampleMixerSettings& requested) noexcept
{
    return std::exp2(static_cast<double>(sanitize(requested).tuneSemitones) / 12.0);
}

} // namespace sms::dsp
