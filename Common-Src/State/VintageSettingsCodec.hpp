#pragma once

#include "DSP/VintageSettings.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace sms::state {

/** Global vintage FX: "VF1;model;voicing;bits;rate;drive;tone;mix;resonance". */
[[nodiscard]] inline std::string encodeVintageSettings(const dsp::VintageSettings& requested)
{
    const auto v = dsp::sanitize(requested);
    char encoded[256]{};
    std::snprintf(encoded, sizeof(encoded), "VF1;%.9g;%.9g;%.9g;%.9g;%.9g;%.9g;%.9g;%.9g",
                  v.model, v.voicing, v.bits, v.rateHz, v.driveDb, v.toneOctaves, v.mix,
                  v.resonance);
    return encoded;
}

/** Rejects anything malformed; the destination is untouched on failure. */
[[nodiscard]] inline bool decodeVintageSettings(const char* const encoded,
                                                dsp::VintageSettings& destination) noexcept
{
    if (encoded == nullptr || std::strncmp(encoded, "VF1;", 4U) != 0)
        return false;
    const char* cursor = encoded + 4U;
    float values[8]{};
    for (std::size_t index = 0; index < 8U; ++index) {
        char* end = nullptr;
        values[index] = std::strtof(cursor, &end);
        if (end == cursor || !std::isfinite(values[index]))
            return false;
        cursor = end;
        if (index + 1U != 8U) {
            if (*cursor != ';')
                return false;
            ++cursor;
        }
    }
    if (*cursor != '\0')
        return false;
    destination = dsp::sanitize(dsp::VintageSettings{values[0], values[1], values[2], values[3],
                                                     values[4], values[5], values[6], values[7]});
    return true;
}

} // namespace sms::state
