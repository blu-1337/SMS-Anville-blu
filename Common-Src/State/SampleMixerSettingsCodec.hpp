#pragma once

#include "DSP/SampleMixerSettings.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace sms::state {

/** MX8 = the twelve MX7 fields, then the eight VintageSettings fields in declaration order. */
[[nodiscard]] inline std::string encodeSampleMixerSettings(
    const dsp::SampleMixerSettings& requested)
{
    const auto settings = dsp::sanitize(requested);
    const auto& v = settings.vintage;
    char encoded[512]{};
    std::snprintf(encoded, sizeof(encoded),
                  "MX8;%.9g;%.9g;%.9g;%.9g;%.9g;%.9g;%.9g;%.9g;%.9g;%.9g;%.9g;%.9g;"
                  "%.9g;%.9g;%.9g;%.9g;%.9g;%.9g;%.9g;%.9g",
                  settings.gainDecibels, settings.pan, settings.tuneSemitones,
                  settings.lowpass, settings.highpass, settings.filterSlope, settings.dirty,
                  settings.reverse, settings.globalMixer, settings.chokeGroup,
                  settings.outputBus, settings.holdLoop,
                  v.model, v.voicing, v.bits, v.rateHz, v.driveDb, v.toneOctaves, v.mix,
                  v.resonance);
    return encoded;
}

[[nodiscard]] inline bool decodeSampleMixerSettings(
    const char* const encoded, dsp::SampleMixerSettings& destination) noexcept
{
    if (encoded == nullptr || std::strncmp(encoded, "MX", 2U) != 0 ||
        encoded[2] < '1' || encoded[2] > '8' || encoded[3] != ';')
        return false;
    const int version = encoded[2] - '0';
    const char* cursor = encoded + 4U;
    // Slot 11 = hold loop: states older than MX7 had no such field and always looped, so they decode as On.
    // Slots 12-19 = vintage FX: states older than MX8 decode Off (the defaults).
    const dsp::VintageSettings off{};
    float values[20]{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                     off.model, off.voicing, off.bits, off.rateHz, off.driveDb,
                     off.toneOctaves, off.mix, off.resonance};
    constexpr std::size_t kCounts[9]{0U, 3U, 7U, 8U, 9U, 10U, 11U, 12U, 20U};
    const std::size_t count = kCounts[version];
    for (std::size_t index = 0; index < count; ++index) {
        char* end = nullptr;
        values[index] = std::strtof(cursor, &end);
        if (end == cursor || !std::isfinite(values[index]))
            return false;
        cursor = end;
        if (index + 1U != count) {
            if (*cursor != ';')
                return false;
            ++cursor;
        }
    }
    if (*cursor != '\0')
        return false;
    dsp::SampleMixerSettings decoded;
    decoded.gainDecibels = values[0];
    decoded.pan = values[1];
    decoded.tuneSemitones = values[2];
    decoded.lowpass = values[3];
    decoded.highpass = values[4];
    decoded.filterSlope = values[5];
    decoded.dirty = values[6];
    decoded.reverse = values[7];
    decoded.globalMixer = values[8];
    decoded.chokeGroup = values[9];
    decoded.outputBus = values[10];
    decoded.holdLoop = values[11];
    decoded.vintage = {values[12], values[13], values[14], values[15], values[16], values[17],
                       values[18], values[19]};
    destination = dsp::sanitize(decoded);
    return true;
}

} // namespace sms::state
