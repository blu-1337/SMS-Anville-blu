#pragma once

// TUNE math (DPF-free). The sounding note is raw analysed MIDI + pad tune; both
// functions return the new absolute pad tune, so applying one twice changes nothing.

#include "DSP/SampleMixerSettings.hpp"

#include <algorithm>
#include <cmath>

namespace midichopper::ui {

namespace tune_detail {

// Out of range steps back by octaves, so the pitch class survives.
[[nodiscard]] inline float inRange(double tune) noexcept
{
    if (!std::isfinite(tune)) return 0.0F;
    // The tolerance keeps float noise from throwing a limit value an octave away.
    while (tune > sms::dsp::kMaximumTuneSemitones + 1.0e-4) tune -= 12.0;
    while (tune < sms::dsp::kMinimumTuneSemitones - 1.0e-4) tune += 12.0;
    return std::clamp(static_cast<float>(tune), sms::dsp::kMinimumTuneSemitones,
                      sms::dsp::kMaximumTuneSemitones);
}

} // namespace tune_detail

/** Removes the cents: the sounding note moves to the nearest semitone. */
[[nodiscard]] inline float tuneToNearest(const double rawMidi, const float current) noexcept
{
    return tune_detail::inRange(std::round(rawMidi + current) - rawMidi);
}

/** Moves the sounding note to `pitchClass` (0 = C) in the nearest octave; ties go down. */
[[nodiscard]] inline float tuneToPitchClass(const double rawMidi, const float current,
                                            const int pitchClass) noexcept
{
    const double sounding = rawMidi + current;
    double step = std::fmod(static_cast<double>(pitchClass) - sounding, 12.0);
    if (step < 0.0) step += 12.0;
    if (step >= 6.0 - 1.0e-4) step -= 12.0;
    return tune_detail::inRange(sounding + step - rawMidi);
}

} // namespace midichopper::ui
