#pragma once

// Vintage FX popup: layout and pure value mapping (Docs/AI/VINTAGE-FX.md).

#include "DSP/VintageSettings.hpp"
#include "MidichopperLayout.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>

namespace midichopper::ui {

namespace layout {

inline constexpr sms::ui::Rect globalVintage{994.0f, 608.0f, 222.0f, 32.0f};
inline constexpr sms::ui::Rect padVintage{650.0f, 604.0f, 150.0f, 34.0f};
inline constexpr sms::ui::Rect vintagePanel{300.0f, 236.0f, 520.0f, 250.0f};
inline constexpr sms::ui::Rect vintageClose{784.0f, 244.0f, 28.0f, 24.0f};

[[nodiscard]] constexpr sms::ui::Rect vintageModel(const int index) noexcept
{
    return {316.0f + static_cast<float>(index) * 164.0f, 276.0f, 156.0f, 28.0f};
}

[[nodiscard]] constexpr sms::ui::Rect vintageVoicing(const int index) noexcept
{
    return {316.0f + static_cast<float>(index) * 82.0f, 314.0f, 76.0f, 24.0f};
}

[[nodiscard]] constexpr sms::ui::Rect vintageKnob(const int index) noexcept
{
    return {316.0f + static_cast<float>(index) * 84.0f, 348.0f, 76.0f, 72.0f};
}

[[nodiscard]] constexpr sms::ui::Rect vintageValueLabel(const int index) noexcept
{
    const auto knob = vintageKnob(index);
    return {knob.x, knob.y + 52.0f, knob.width, 18.0f};
}

} // namespace layout

/** Popup target: a global pad index, or the global (bus) settings. -1 is closed. */
inline constexpr int kVintageGlobalTarget = 64;
/** Knob order: bits, rate, drive, tone, mix, resonance. */
inline constexpr int kVintageKnobCount = 6;

[[nodiscard]] inline const char* vintageModelName(const int model) noexcept
{
    return model == 1 ? "GSP1200 Sampler" : model == 2 ? "GS950 Unit" : "Off";
}

/** No knobs while Off; resonance belongs to the GS950 Unit. */
[[nodiscard]] inline bool vintageKnobShown(const sms::dsp::VintageSettings& v,
                                           const int knob) noexcept
{
    const auto model = sms::dsp::vintageModel(v);
    return model != sms::dsp::VintageModel::off && knob >= 0 && knob < kVintageKnobCount &&
           (knob != 5 || model == sms::dsp::VintageModel::rack12);
}

[[nodiscard]] inline float vintageKnobFraction(const sms::dsp::VintageSettings& v,
                                               const int knob) noexcept
{
    using namespace sms::dsp;
    switch (knob) {
    case 0: return (v.bits - kVintageMinimumBits) / (kVintageMaximumBits - kVintageMinimumBits);
    case 1: return std::log(v.rateHz / kVintageMinimumRateHz) /
                   std::log(kVintageMaximumRateHz / kVintageMinimumRateHz);
    case 2: return v.driveDb / kVintageMaximumDriveDb;
    case 3: return (v.toneOctaves + kVintageMaximumToneOctaves) / (2.0f * kVintageMaximumToneOctaves);
    case 4: return v.mix;
    default: return v.resonance;
    }
}

[[nodiscard]] inline sms::dsp::VintageSettings vintageWithKnob(sms::dsp::VintageSettings v,
                                                               const int knob,
                                                               float fraction) noexcept
{
    using namespace sms::dsp;
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    switch (knob) {
    case 0:
        v.bits = std::round(kVintageMinimumBits +
                            fraction * (kVintageMaximumBits - kVintageMinimumBits));
        break;
    case 1:
        v.rateHz = kVintageMinimumRateHz *
                   std::pow(kVintageMaximumRateHz / kVintageMinimumRateHz, fraction);
        break;
    case 2: v.driveDb = fraction * kVintageMaximumDriveDb; break;
    case 3: v.toneOctaves = (2.0f * fraction - 1.0f) * kVintageMaximumToneOctaves; break;
    case 4: v.mix = fraction; break;
    default: v.resonance = fraction; break;
    }
    return sanitize(v);
}

/** One wheel notch: a whole bit, otherwise a fraction of the knob travel. */
[[nodiscard]] inline sms::dsp::VintageSettings vintageStepped(sms::dsp::VintageSettings v,
    const int knob, const float notches, const bool fine) noexcept
{
    if (knob == 0) {
        v.bits = std::round(v.bits) + (notches > 0.0f ? 1.0f : -1.0f);
        return sms::dsp::sanitize(v);
    }
    return vintageWithKnob(v, knob,
        vintageKnobFraction(v, knob) + notches * (fine ? 0.005f : 0.025f));
}

[[nodiscard]] inline sms::dsp::VintageSettings vintageWithDefault(sms::dsp::VintageSettings v,
                                                                  const int knob) noexcept
{
    const sms::dsp::VintageSettings defaults{};
    switch (knob) {
    case 0: v.bits = defaults.bits; break;
    case 1: v.rateHz = sms::dsp::defaultRateHz(sms::dsp::vintageModel(v)); break;
    case 2: v.driveDb = defaults.driveDb; break;
    case 3: v.toneOctaves = defaults.toneOctaves; break;
    case 4: v.mix = defaults.mix; break;
    default: v.resonance = defaults.resonance; break;
    }
    return v;
}

/** A model change resets Rate to that model's default; the same model changes nothing. */
[[nodiscard]] inline sms::dsp::VintageSettings vintageWithModel(sms::dsp::VintageSettings v,
                                                                const int model) noexcept
{
    if (static_cast<int>(v.model) == model)
        return v;
    v.model = static_cast<float>(model);
    v.rateHz = sms::dsp::defaultRateHz(sms::dsp::vintageModel(v));
    return sms::dsp::sanitize(v);
}

inline void vintageKnobText(char* const out, const std::size_t size,
                            const sms::dsp::VintageSettings& v, const int knob) noexcept
{
    switch (knob) {
    case 0: std::snprintf(out, size, "%.0f", v.bits); break;
    case 1: std::snprintf(out, size, "%.1f kHz", v.rateHz / 1000.0f); break;
    case 2: std::snprintf(out, size, "%.1f dB", v.driveDb); break;
    case 3: std::snprintf(out, size, "%+.2f oct", v.toneOctaves); break;
    case 4: std::snprintf(out, size, "%.0f%%", v.mix * 100.0f); break;
    default: std::snprintf(out, size, "%.0f%%", v.resonance * 100.0f); break;
    }
}

/** The pad button is narrower, so a chosen model gets the shorter prefix. */
inline void vintageButtonText(char* const out, const std::size_t size,
                              const sms::dsp::VintageSettings& v, const bool pad) noexcept
{
    const int model = static_cast<int>(v.model);
    if (model == 0)
        std::snprintf(out, size, "VINTAGE: OFF");
    else
        std::snprintf(out, size, "%s: %s", pad ? "FX" : "VINTAGE", vintageModelName(model));
}

} // namespace midichopper::ui
