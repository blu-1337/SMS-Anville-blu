// Chord (template matching with false-chord gates) and key (Krumhansl-Kessler
// and Temperley CBMS profiles) from a 12-bin chroma, index 0 = C.
#pragma once

#include "Analysis/Chroma.hpp"
#include "Analysis/SoundAnalysis.hpp"

namespace sms::analysis {

// bassPc -1 when unknown. percussive: no chord. The single-note gate is the
// caller's (it needs the spectrum, see explainedBySingleNote()).
[[nodiscard]] ChordEstimate chordFrom(const Chroma& chroma, int bassPc, bool percussive);

// No key below 2 s, with < 4 pitch classes above 0.2 of the maximum, a best
// mean correlation below 0.55, or percussive material.
[[nodiscard]] KeyEstimate keyFrom(const Chroma& chroma, double seconds, bool percussive);

} // namespace sms::analysis
