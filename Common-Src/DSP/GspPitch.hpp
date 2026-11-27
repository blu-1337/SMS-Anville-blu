#pragma once

#include <algorithm>
#include <cmath>

namespace sms::dsp {

/** GSP pitch mode (SP-1200 style): the source is read as if it had been resampled to a fixed
 * 26041.67 Hz machine clock without an anti-alias filter, 12-bit mid-tread quantised, repitched
 * by drop/repeat (no interpolation) by the pitch ratio, and zero-order held up to the host rate.
 * Everything is a pure function of the voice's elapsed source distance, so it needs no state. */
inline constexpr double kGspMachineRateHz = 26041.67;
inline constexpr float kGspQuantizeSteps = 2048.0f; // 12-bit signed: -2048 .. 2047

/** Whole source frames from the region start for `elapsed` source frames of playback at
 * `ratio` (> 0) from a source recorded at `sourceRate` Hz. Monotonic in `elapsed`. */
[[nodiscard]] inline double gspSourceOffset(const double elapsed, const double ratio,
                                            const double sourceRate) noexcept
{
    constexpr double kEpsilon = 1.0e-7; // exact grid points stay on their own step
    const double perMachine = sourceRate / kGspMachineRateHz; // source frames per stored sample
    const double ticks = std::floor(std::max(elapsed, 0.0) / (perMachine * ratio) + kEpsilon);
    const double stored = std::floor(ticks * ratio + kEpsilon);  // drop/repeat
    return std::floor(stored * perMachine + kEpsilon);           // decimated read, no filter
}

[[nodiscard]] inline float gspQuantize(const float value) noexcept
{
    const float steps = std::round(value * kGspQuantizeSteps);
    return std::clamp(steps, -kGspQuantizeSteps, kGspQuantizeSteps - 1.0f) / kGspQuantizeSteps;
}

} // namespace sms::dsp
