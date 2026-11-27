#pragma once

#include "DSP/SampleMixerSettings.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace midichopper::plugin {

/** Delta sent on a pad's own mixer key by the Main-view drag gestures: "QE1;<gainDb or ->;<pan or ->".
 * The plugin applies it to the pad's live settings, so no stale UI copy can overwrite other knobs. */
struct PadQuickEditDelta {
    bool hasGain = false;
    bool hasPan = false;
    float gainDecibels = 0.0f;
    float pan = 0.0f;
    /** S = single step, B = gesture begin, U = live update, E = gesture end (one undo step). */
    char phase = 'S';
};

[[nodiscard]] inline std::string encodePadQuickEdit(const PadQuickEditDelta& edit)
{
    char text[64];
    char gain[24] = "-";
    char pan[24] = "-";
    if (edit.hasGain) std::snprintf(gain, sizeof(gain), "%.2f", static_cast<double>(edit.gainDecibels));
    if (edit.hasPan) std::snprintf(pan, sizeof(pan), "%.4f", static_cast<double>(edit.pan));
    if (edit.phase != 'S')
        std::snprintf(text, sizeof(text), "QE1;%s;%s;%c", gain, pan, edit.phase);
    else
        std::snprintf(text, sizeof(text), "QE1;%s;%s", gain, pan);
    return text;
}

[[nodiscard]] inline bool isPadQuickEdit(const char* const value) noexcept
{
    return value != nullptr && std::strncmp(value, "QE1;", 4) == 0;
}

[[nodiscard]] inline bool decodePadQuickEdit(const char* const value, PadQuickEditDelta& out) noexcept
{
    if (!isPadQuickEdit(value)) return false;
    const char* const gainText = value + 4;
    const char* const split = std::strchr(gainText, ';');
    if (split == nullptr) return false;
    const char* const tail = std::strchr(split + 1, ';');
    PadQuickEditDelta edit;
    if (tail != nullptr) {
        if (tail[1] == '\0' || tail[2] != '\0' || std::strchr("BUES", tail[1]) == nullptr) return false;
        edit.phase = tail[1];
    }
    const auto parse = [](const char* begin, const char* end, bool& has, float& number) noexcept {
        if (end - begin == 1 && *begin == '-') return true;
        if (end == begin || end - begin > 20) return false;
        char buffer[24];
        std::memcpy(buffer, begin, static_cast<std::size_t>(end - begin));
        buffer[end - begin] = '\0';
        char* stop = nullptr;
        const float parsed = std::strtof(buffer, &stop);
        if (stop == buffer || *stop != '\0' || !std::isfinite(parsed)) return false;
        has = true;
        number = parsed;
        return true;
    };
    if (!parse(gainText, split, edit.hasGain, edit.gainDecibels) ||
        !parse(split + 1, tail != nullptr ? tail : split + 1 + std::strlen(split + 1), edit.hasPan, edit.pan) ||
        (!edit.hasGain && !edit.hasPan))
        return false;
    out = edit;
    return true;
}

/** Applies the delta with the same clamping as every other mixer edit. */
[[nodiscard]] inline sms::dsp::SampleMixerSettings applyPadQuickEdit(
    sms::dsp::SampleMixerSettings settings, const PadQuickEditDelta& edit) noexcept
{
    if (edit.hasGain) settings.gainDecibels = edit.gainDecibels;
    if (edit.hasPan) settings.pan = edit.pan;
    return sms::dsp::sanitize(settings);
}

/** Header TUNE, also sent on the pad's mixer key: "TN1;<semitones>", the absolute pad tune.
 * Only the tune changes, and it is always its own undo step. */
[[nodiscard]] inline std::string encodePadTune(const float semitones)
{
    char text[32];
    std::snprintf(text, sizeof(text), "TN1;%.9g", static_cast<double>(semitones));
    return text;
}

[[nodiscard]] inline bool decodePadTune(const char* const value, float& semitones) noexcept
{
    if (value == nullptr || std::strncmp(value, "TN1;", 4) != 0) return false;
    char* stop = nullptr;
    const float parsed = std::strtof(value + 4, &stop);
    if (stop == value + 4 || *stop != '\0' || !std::isfinite(parsed) ||
        parsed < sms::dsp::kMinimumTuneSemitones || parsed > sms::dsp::kMaximumTuneSemitones)
        return false;
    semitones = parsed;
    return true;
}

/** Pad GSP, also sent on the pad's mixer key, so the pad's other settings are never rewritten:
 * "GS1;?" asks for the flag, "GS1;T" flips it (one undo step); the plugin answers "GS1;0|1". */
inline constexpr const char* kPadGspQuery = "GS1;?";
inline constexpr const char* kPadGspToggle = "GS1;T";

[[nodiscard]] inline const char* encodePadGsp(const bool on) noexcept { return on ? "GS1;1" : "GS1;0"; }

} // namespace midichopper::plugin
