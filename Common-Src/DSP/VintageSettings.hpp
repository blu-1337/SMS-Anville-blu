#pragma once

#include <algorithm>
#include <cmath>

namespace sms::dsp {

/** Vintage 12-bit colour (Docs/AI/VINTAGE-FX.md). UI names: drum12 = "GSP1200 Sampler",
 * rack12 = "GS950 Unit". Values are floats so codecs and atomics stay uniform. */
enum class VintageModel : int { off = 0, drum12 = 1, rack12 = 2 };
/** Drum-model output filter; ignored by the rack model. */
enum class VintageVoicing : int { sweep = 0, dark = 1, mid = 2, open = 3, bright = 4, raw = 5 };

inline constexpr float kVintageDrumRateHz = 26041.67f; // 20 MHz / 768
inline constexpr float kVintageRackRateHz = 40000.0f;
inline constexpr float kVintageMinimumBits = 4.0f;
inline constexpr float kVintageMaximumBits = 16.0f;
inline constexpr float kVintageMinimumRateHz = 4000.0f;
inline constexpr float kVintageMaximumRateHz = 48000.0f;
inline constexpr float kVintageMaximumDriveDb = 24.0f;
inline constexpr float kVintageMaximumToneOctaves = 2.0f;
inline constexpr int kVintageVoicingCount = 6;

struct VintageSettings {
    float model = 0.0f;      // VintageModel
    float voicing = 3.0f;    // VintageVoicing, default Open
    float bits = 12.0f;      // 4..16, fractional while smoothing
    float rateHz = kVintageDrumRateHz;
    float driveDb = 0.0f;    // 0..24
    float toneOctaves = 0.0f; // -2..2, scales every model filter cutoff
    float mix = 1.0f;        // 0..1
    float resonance = 0.0f;  // 0..1, rack model only; 0 = authentic non-resonant Butterworth

    friend bool operator==(const VintageSettings&, const VintageSettings&) = default;
};

[[nodiscard]] inline VintageModel vintageModel(const VintageSettings& settings) noexcept
{
    return static_cast<VintageModel>(static_cast<int>(settings.model));
}

[[nodiscard]] inline VintageVoicing vintageVoicing(const VintageSettings& settings) noexcept
{
    return static_cast<VintageVoicing>(static_cast<int>(settings.voicing));
}

/** Rate a model starts from; the UI resets Rate to this on a model change. */
[[nodiscard]] inline float defaultRateHz(const VintageModel model) noexcept
{
    return model == VintageModel::rack12 ? kVintageRackRateHz : kVintageDrumRateHz;
}

[[nodiscard]] inline VintageSettings sanitize(VintageSettings value) noexcept
{
    const VintageSettings defaults{};
    const auto finiteOr = [](const float candidate, const float fallback) noexcept {
        return std::isfinite(candidate) ? candidate : fallback;
    };
    value.model = std::clamp(std::round(finiteOr(value.model, defaults.model)), 0.0f, 2.0f);
    value.voicing = std::clamp(std::round(finiteOr(value.voicing, defaults.voicing)), 0.0f,
                               static_cast<float>(kVintageVoicingCount - 1));
    value.bits = std::clamp(finiteOr(value.bits, defaults.bits),
                            kVintageMinimumBits, kVintageMaximumBits);
    value.rateHz = std::clamp(finiteOr(value.rateHz, defaults.rateHz),
                              kVintageMinimumRateHz, kVintageMaximumRateHz);
    value.driveDb = std::clamp(finiteOr(value.driveDb, defaults.driveDb), 0.0f,
                               kVintageMaximumDriveDb);
    value.toneOctaves = std::clamp(finiteOr(value.toneOctaves, defaults.toneOctaves),
                                   -kVintageMaximumToneOctaves, kVintageMaximumToneOctaves);
    value.mix = std::clamp(finiteOr(value.mix, defaults.mix), 0.0f, 1.0f);
    value.resonance = std::clamp(finiteOr(value.resonance, defaults.resonance), 0.0f, 1.0f);
    return value;
}

/** True when the processor can produce anything but the dry input. */
[[nodiscard]] inline bool vintageAudible(const VintageSettings& settings) noexcept
{
    return settings.model >= 0.5f && settings.mix > 0.0f;
}

} // namespace sms::dsp
