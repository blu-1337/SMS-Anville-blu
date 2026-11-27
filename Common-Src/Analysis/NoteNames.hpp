// Display text for sound-analysis results. Sharps only; C4 = MIDI 60; cents
// are against A440. Header-only.
#pragma once

#include "Analysis/SoundAnalysis.hpp"

#include <array>
#include <cmath>
#include <string>

namespace sms::analysis {

inline constexpr std::array<const char*, 12> kPitchClassNames{
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

[[nodiscard]] inline std::string pitchClassName(const int pc)
{
    return pc >= 0 && pc < 12 ? kPitchClassNames[static_cast<std::size_t>(pc)] : "-";
}

// "F#2 -23c"; "~" prefix when approximate or below firm confidence (0.75); "-" when invalid.
[[nodiscard]] inline std::string noteText(const PitchEstimate& note)
{
    if (!note.valid || !std::isfinite(note.midi)) {
        return "-";
    }
    const long nearest = std::lround(note.midi);
    long cents = std::lround(100.0 * (note.midi - static_cast<double>(nearest)));
    cents = cents > 50 ? 50 : (cents < -50 ? -50 : cents);
    const long pc = ((nearest % 12) + 12) % 12;
    const long octave = (nearest - pc) / 12 - 1;
    std::string text = (note.approximate || note.confidence < 0.75F) ? "~" : "";
    text += kPitchClassNames[static_cast<std::size_t>(pc)];
    text += std::to_string(octave);
    text += cents < 0 ? " -" : " +";
    text += std::to_string(cents < 0 ? -cents : cents);
    text += "c";
    return text;
}

// "Cmaj7", "Am", "C5", "varies" or "-".
[[nodiscard]] inline std::string chordText(const ChordEstimate& chord)
{
    if (!chord.valid) {
        return chord.varies ? "varies" : "-";
    }
    const char* suffix = "";
    switch (chord.quality) {
    case ChordQuality::none:
        return "-";
    case ChordQuality::major:
        suffix = "";
        break;
    case ChordQuality::minor:
        suffix = "m";
        break;
    case ChordQuality::dominant7:
        suffix = "7";
        break;
    case ChordQuality::major7:
        suffix = "maj7";
        break;
    case ChordQuality::minor7:
        suffix = "m7";
        break;
    case ChordQuality::diminished:
        suffix = "dim";
        break;
    case ChordQuality::augmented:
        suffix = "aug";
        break;
    case ChordQuality::sus2:
        suffix = "sus2";
        break;
    case ChordQuality::sus4:
        suffix = "sus4";
        break;
    case ChordQuality::power:
        suffix = "5";
        break;
    }
    return pitchClassName(chord.root) + suffix;
}

// "A min", "C maj" or "-".
[[nodiscard]] inline std::string keyText(const KeyEstimate& key)
{
    if (!key.valid) {
        return "-";
    }
    return pitchClassName(key.tonic) + (key.minor ? " min" : " maj");
}

} // namespace sms::analysis
