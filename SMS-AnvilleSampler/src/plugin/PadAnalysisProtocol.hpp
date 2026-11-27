#pragma once

// DPF-free pad sound detection codecs: AQ1 request (UI to worker) and AR1
// status (worker to UI). ';'-separated ASCII (Docs/AI/SOUND-ANALYSIS.md).

#include "Analysis/SoundAnalysis.hpp"
#include "MainSampleProtocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace midichopper::plugin {

struct PadAnalysisRequest {
    std::uint64_t sequence = 0U;
    std::uint32_t pad = 0U;
};

enum class PadAnalysisState : char { ready = 'R', busy = 'B', noAudio = 'N', error = 'E' };

struct PadAnalysisStatus {
    std::uint64_t sequence = 0U;
    std::uint32_t pad = 0U;
    std::uint64_t generation = 0U;
    PadAnalysisState state = PadAnalysisState::error;
    // The wire omits silent, cancelled, hz (derived from midi) and the tuning offset.
    sms::analysis::SoundAnalysis result{};
    float tune = 0.0F; // the pad's TUNE in semitones; the result is of the untuned audio
};

namespace pad_analysis_detail {

inline constexpr std::string_view kNoteFlags = "agrv"; // approximate, gliding, range, chord varies

[[nodiscard]] inline std::string decimal(const double value)
{
    char text[32]{};
    std::snprintf(text, sizeof(text), "%.4f", value);
    return text;
}

[[nodiscard]] inline bool number(main_sample_detail::Fields& fields, const double minimum,
                                 const double maximum, double& value)
{
    std::string_view field;
    if (!fields.next(field) || field.empty() || field.size() > 24U)
        return false;
    const std::string text(field);
    char* end = nullptr;
    value = std::strtod(text.c_str(), &end);
    return end == text.c_str() + text.size() && std::isfinite(value) &&
        value >= minimum && value <= maximum;
}

[[nodiscard]] inline bool confidence(main_sample_detail::Fields& fields, float& value)
{
    double parsed = 0.0;
    if (!number(fields, 0.0, 1.0, parsed))
        return false;
    value = static_cast<float>(parsed);
    return true;
}

[[nodiscard]] inline bool pitchClass(main_sample_detail::Fields& fields, int& value)
{
    return fields.number(value) && value >= -1 && value <= 11;
}

[[nodiscard]] inline bool flag(main_sample_detail::Fields& fields, bool& value)
{
    unsigned parsed = 2U;
    if (!fields.number(parsed) || parsed > 1U)
        return false;
    value = parsed == 1U;
    return true;
}

} // namespace pad_analysis_detail

[[nodiscard]] inline std::string encodePadAnalysisRequest(const PadAnalysisRequest& request)
{
    return "AQ1;" + std::to_string(request.sequence) + ";" + std::to_string(request.pad);
}

[[nodiscard]] inline bool decodePadAnalysisRequest(const std::string_view encoded,
                                                   PadAnalysisRequest& request)
{
    if (!encoded.starts_with("AQ1;"))
        return false;
    main_sample_detail::Fields fields(encoded.substr(4U));
    PadAnalysisRequest decoded;
    if (!fields.number(decoded.sequence) || !fields.number(decoded.pad) || !fields.finished() ||
        decoded.pad >= kPadCount)
        return false;
    request = decoded;
    return true;
}

[[nodiscard]] inline std::string encodePadAnalysisStatus(const PadAnalysisStatus& status)
{
    using pad_analysis_detail::decimal;
    const auto& note = status.result.note;
    const auto& chord = status.result.chord;
    const auto& key = status.result.key;
    const auto unit = [](const float value) {
        return decimal(std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) : 0.0F);
    };
    std::string flags;
    const bool set[]{note.approximate, note.gliding, note.rangeLimited, chord.varies};
    for (std::size_t i = 0U; i < pad_analysis_detail::kNoteFlags.size(); ++i)
        if (set[i])
            flags += pad_analysis_detail::kNoteFlags[i];

    std::string out = "AR1;" + std::to_string(status.sequence) + ";" + std::to_string(status.pad) +
        ";" + std::to_string(status.generation) + ";";
    out += static_cast<char>(status.state);
    out += ";" + (note.valid && std::isfinite(note.midi) ? decimal(note.midi) : std::string("-"));
    out += ";" + unit(note.confidence) + ";" + (flags.empty() ? std::string("-") : flags);
    out += ";" + std::to_string(chord.valid ? chord.root : -1) + ";" +
        std::to_string(chord.valid ? static_cast<int>(chord.quality) : 0) + ";" +
        unit(chord.confidence);
    out += ";" + std::to_string(key.valid ? key.tonic : -1) + ";" + (key.minor ? "1" : "0") + ";" +
        unit(key.confidence) + ";" + std::to_string(key.runnerUpTonic) + ";" +
        (key.runnerUpMinor ? "1" : "0");
    out += ";" + decimal(std::isfinite(status.tune) ? std::clamp(status.tune, -24.0F, 24.0F) : 0.0F);
    return out;
}

[[nodiscard]] inline bool decodePadAnalysisStatus(const std::string_view encoded,
                                                  PadAnalysisStatus& status)
{
    using namespace pad_analysis_detail;
    using sms::analysis::ChordQuality;
    if (!encoded.starts_with("AR1;"))
        return false;
    main_sample_detail::Fields fields(encoded.substr(4U));
    PadAnalysisStatus decoded;
    auto& note = decoded.result.note;
    auto& chord = decoded.result.chord;
    auto& key = decoded.result.key;
    std::string_view state;
    std::string_view midi;
    if (!fields.number(decoded.sequence) || !fields.number(decoded.pad) ||
        decoded.pad >= kPadCount || !fields.number(decoded.generation) || !fields.next(state) ||
        state.size() != 1U || std::string_view("RBNE").find(state[0]) == std::string_view::npos ||
        !fields.next(midi))
        return false;
    decoded.state = static_cast<PadAnalysisState>(state[0]);
    if (midi != "-") {
        main_sample_detail::Fields value(midi);
        if (!number(value, -24.0, 160.0, note.midi))
            return false;
        note.valid = true;
        note.hz = 440.0 * std::pow(2.0, (note.midi - 69.0) / 12.0);
    }
    std::string_view flags;
    if (!confidence(fields, note.confidence) || !fields.next(flags) || flags.empty())
        return false;
    if (flags != "-") {
        bool set[4]{};
        std::size_t first = 0U; // letters appear at most once, in order
        for (const char letter : flags) {
            const auto index = kNoteFlags.find(letter, first);
            if (index == std::string_view::npos)
                return false;
            set[index] = true;
            first = index + 1U;
        }
        note.approximate = set[0];
        note.gliding = set[1];
        note.rangeLimited = set[2];
        chord.varies = set[3];
    }
    int quality = 0;
    double tune = 0.0;
    if (!pitchClass(fields, chord.root) || !fields.number(quality) || quality < 0 ||
        quality > static_cast<int>(ChordQuality::power) || (chord.root >= 0) != (quality != 0) ||
        !confidence(fields, chord.confidence) || !pitchClass(fields, key.tonic) ||
        !flag(fields, key.minor) || !confidence(fields, key.confidence) ||
        !pitchClass(fields, key.runnerUpTonic) || !flag(fields, key.runnerUpMinor) ||
        !number(fields, -24.0, 24.0, tune) || !fields.finished())
        return false;
    decoded.tune = static_cast<float>(tune);
    chord.quality = static_cast<ChordQuality>(quality);
    chord.valid = chord.root >= 0;
    key.valid = key.tonic >= 0;
    status = decoded;
    return true;
}

} // namespace midichopper::plugin
