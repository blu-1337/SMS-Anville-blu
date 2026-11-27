#include "Analysis/KeyChord.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace sms::analysis {

namespace {
struct ChordShape {
    ChordQuality quality;
    std::array<int, 4> intervals;
    int tones;
    double penalty;
};

// The last row is a lone note: it competes, and winning means "no chord".
constexpr std::array<ChordShape, 11> kShapes{{
    {ChordQuality::major, {0, 4, 7, 0}, 3, 0.00},
    {ChordQuality::minor, {0, 3, 7, 0}, 3, 0.00},
    {ChordQuality::dominant7, {0, 4, 7, 10}, 4, 0.02},
    {ChordQuality::major7, {0, 4, 7, 11}, 4, 0.02},
    {ChordQuality::minor7, {0, 3, 7, 10}, 4, 0.02},
    {ChordQuality::diminished, {0, 3, 6, 0}, 3, 0.03},
    {ChordQuality::augmented, {0, 4, 8, 0}, 3, 0.04},
    {ChordQuality::sus2, {0, 2, 7, 0}, 3, 0.03},
    {ChordQuality::sus4, {0, 5, 7, 0}, 3, 0.03},
    {ChordQuality::power, {0, 7, 0, 0}, 2, 0.02},
    {ChordQuality::none, {0, 0, 0, 0}, 1, 0.00},
}};

constexpr double kBassBonus = 0.05;
constexpr double kMinChordScore = 0.75;

// Each tone adds 0.6^(h-1) at offsets {0, 0, 7, 0, 4, 7} for h = 1..6 (harmonics).
std::array<double, 12> chordTemplate(const ChordShape& shape, const int root)
{
    constexpr std::array<int, 6> kHarmonicOffset{0, 0, 7, 0, 4, 7};
    std::array<double, 12> t{};
    for (int i = 0; i < shape.tones; ++i) {
        const int tone = root + shape.intervals[static_cast<std::size_t>(i)];
        double weight = 1.0;
        for (const int offset : kHarmonicOffset) {
            t[static_cast<std::size_t>((tone + offset) % 12)] += weight;
            weight *= 0.6;
        }
    }
    return t;
}

double cosine(const Chroma& c, const std::array<double, 12>& t)
{
    double dot = 0.0;
    double nc = 0.0;
    double nt = 0.0;
    for (std::size_t i = 0; i < 12; ++i) {
        dot += static_cast<double>(c[i]) * t[i];
        nc += static_cast<double>(c[i]) * static_cast<double>(c[i]);
        nt += t[i] * t[i];
    }
    return nc > 0.0 && nt > 0.0 ? dot / std::sqrt(nc * nt) : 0.0;
}

bool tonesPresent(const Chroma& c, const ChordShape& shape, const int root, const float maxValue)
{
    for (int i = 0; i < shape.tones; ++i) {
        const float value = c[static_cast<std::size_t>((root + shape.intervals[static_cast<std::size_t>(i)]) % 12)];
        float required = 0.25F;
        if (i == 3) {
            required = 0.2F;  // sevenths
        }
        if (shape.quality == ChordQuality::power && i == 1) {
            required = 0.5F;  // the fifth must stand above harmonic leakage
        }
        if (value < required * maxValue) {
            return false;
        }
    }
    return true;
}

double clamp01(const double v)
{
    return std::clamp(v, 0.0, 1.0);
}

double pearson(const Chroma& c, const std::array<double, 12>& profile, const int tonic)
{
    double meanC = 0.0;
    double meanP = 0.0;
    for (std::size_t i = 0; i < 12; ++i) {
        meanC += static_cast<double>(c[i]);
        meanP += profile[i];
    }
    meanC /= 12.0;
    meanP /= 12.0;
    double num = 0.0;
    double dc = 0.0;
    double dp = 0.0;
    for (std::size_t i = 0; i < 12; ++i) {
        const double x = static_cast<double>(c[(i + static_cast<std::size_t>(tonic)) % 12]) - meanC;
        const double y = profile[i] - meanP;
        num += x * y;
        dc += x * x;
        dp += y * y;
    }
    return dc > 0.0 && dp > 0.0 ? num / std::sqrt(dc * dp) : 0.0;
}

// Profiles start at the tonic. Krumhansl-Kessler and Temperley (CBMS).
constexpr std::array<double, 12> kKrumhanslMajor{6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
constexpr std::array<double, 12> kKrumhanslMinor{6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
constexpr std::array<double, 12> kTemperleyMajor{5.0, 2.0, 3.5, 2.0, 4.5, 4.0, 2.0, 4.5, 2.0, 3.5, 1.5, 4.0};
constexpr std::array<double, 12> kTemperleyMinor{5.0, 2.0, 3.5, 4.5, 2.0, 4.0, 2.0, 4.5, 3.5, 2.0, 1.5, 4.0};
} // namespace

ChordEstimate chordFrom(const Chroma& chroma, const int bassPc, const bool percussive)
{
    ChordEstimate out;
    const float maxValue = *std::max_element(chroma.begin(), chroma.end());
    if (percussive || !(maxValue > 0.0F)) {
        return out;
    }
    double best = -1.0;
    double second = -1.0;
    int bestRoot = -1;
    ChordQuality bestQuality = ChordQuality::none;
    for (const ChordShape& shape : kShapes) {
        for (int root = 0; root < 12; ++root) {
            if (!tonesPresent(chroma, shape, root, maxValue)) {
                continue;
            }
            double score = cosine(chroma, chordTemplate(shape, root)) - shape.penalty;
            if (root == bassPc) {
                score += kBassBonus;
            }
            if (score > best) {
                second = best;
                best = score;
                bestRoot = root;
                bestQuality = shape.quality;
            } else if (score > second) {
                second = score;
            }
        }
    }
    if (bestQuality == ChordQuality::none || best < kMinChordScore) {
        return out;
    }
    out.valid = true;
    out.root = bestRoot;
    out.quality = bestQuality;
    out.confidence = static_cast<float>(clamp01((best - 0.7) / 0.25) * clamp01(0.5 + (best - second) / 0.1));
    return out;
}

KeyEstimate keyFrom(const Chroma& chroma, const double seconds, const bool percussive)
{
    KeyEstimate out;
    const float maxValue = *std::max_element(chroma.begin(), chroma.end());
    if (percussive || seconds < 2.0 || !(maxValue > 0.0F)) {
        return out;
    }
    int strong = 0;
    for (const float v : chroma) {
        strong += v > 0.2F * maxValue ? 1 : 0;
    }
    if (strong < 4) {
        return out;
    }
    double r1 = -2.0;
    double r2 = -2.0;
    int tonic1 = -1;
    int tonic2 = -1;
    bool minor1 = false;
    bool minor2 = false;
    for (int mode = 0; mode < 2; ++mode) {
        const bool minor = mode == 1;
        for (int tonic = 0; tonic < 12; ++tonic) {
            const double r = 0.5 * (pearson(chroma, minor ? kKrumhanslMinor : kKrumhanslMajor, tonic)
                                    + pearson(chroma, minor ? kTemperleyMinor : kTemperleyMajor, tonic));
            if (r > r1) {
                r2 = r1;
                tonic2 = tonic1;
                minor2 = minor1;
                r1 = r;
                tonic1 = tonic;
                minor1 = minor;
            } else if (r > r2) {
                r2 = r;
                tonic2 = tonic;
                minor2 = minor;
            }
        }
    }
    if (r1 < 0.55) {
        return out;
    }
    out.valid = true;
    out.tonic = tonic1;
    out.minor = minor1;
    out.runnerUpTonic = tonic2;
    out.runnerUpMinor = minor2;
    out.confidence = static_cast<float>(clamp01((r1 - 0.4) / 0.4) * clamp01(0.5 + (r1 - r2) / 0.1));
    return out;
}

} // namespace sms::analysis
