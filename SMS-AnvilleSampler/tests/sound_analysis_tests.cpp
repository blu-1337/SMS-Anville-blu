// sms_analysis sound detection: note + cents, chord and key on synthetic signals.
// Targets are the research note's section 12 table (detection.md); none is loosened.
// Rates 44.1/48/96 kHz (192 kHz for sines only), fixed SplitMix64 seeds.
// Implementation choices that differ from the note's constants, needed to meet them:
// - Approximate (drum) mode also when the body's 20..5000 Hz flatness > 0.03: YIN over
//   80 ms read the equal-RMS snare 72-82% periodic, so periodicity < 0.5 alone failed
//   "conf <= 0.6, no chord" (it read D7/B7 or conf 0.83).
// - A spectral f0 above 1 kHz that is an octave/twelfth above YIN keeps agreement 1.0:
//   YIN's integer lags (~6 at 24 kHz) prefer two periods, which capped a 3.7 kHz sine at conf 0.3.
// - The long spectrum (refinement, octave and single-note tests) uses Blackman-Harris,
//   not Hann: Hann side lobes counted as unexplained peaks in the single-note gate.
// - The envelope "peak" is the first 10 ms block within 1 dB of the maximum, so a
//   steady tone's usable region does not start at a late ripple.
// - Per-frame spectral estimates are replaced by one long spectrum of the stable region.
// - "Varies" (segment-wise chords) is checked only from 4 s on.
#include "Analysis/KeyChord.hpp"
#include "Analysis/NoteNames.hpp"
#include "Analysis/SoundAnalysis.hpp"
#include "Analysis/SplitMix64.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

namespace {
using namespace sms::analysis;

int failures = 0;

void check(const bool value, const std::string& message)
{
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::array<double, 3> kRates{44100.0, 48000.0, 96000.0};
constexpr std::array<double, 4> kRatesWith192{44100.0, 48000.0, 96000.0, 192000.0};

double midiToHz(const double midi)
{
    return 440.0 * std::exp2((midi - 69.0) / 12.0);
}

struct Partial {
    double hz = 0.0;
    double amplitude = 0.0;
};

enum class Stereo { duplicate, unequal, antiPhase };

// Sum of stationary partials (random phases), times an envelope, as interleaved stereo.
std::vector<float> renderPartials(const double rate, const double seconds, const std::vector<Partial>& partials,
                                  const std::function<double(double)>& envelope, SplitMix64& rng,
                                  const Stereo stereo = Stereo::duplicate)
{
    const std::size_t frames = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(seconds * rate)));
    std::vector<double> mono(frames, 0.0);
    for (const Partial& p : partials) {
        const std::complex<double> step = std::polar(1.0, 2.0 * std::numbers::pi * p.hz / rate);
        std::complex<double> z = std::polar(p.amplitude, 2.0 * std::numbers::pi * rng.nextUnit());
        for (std::size_t i = 0; i < frames; ++i) {
            mono[i] += z.imag();
            z *= step;
            if ((i & 1023U) == 1023U) {
                z *= p.amplitude / std::abs(z);
            }
        }
    }
    std::vector<float> out(2 * frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const double v = mono[i] * envelope(static_cast<double>(i) / rate);
        const double left = v;
        const double right = stereo == Stereo::duplicate ? v : (stereo == Stereo::unequal ? 0.6 * v : -v);
        out[2 * i] = static_cast<float>(left);
        out[2 * i + 1] = static_cast<float>(right);
    }
    return out;
}

// Sine with a time-varying frequency (808, kick) plus optional decaying noise.
std::vector<float> renderGlide(const double rate, const double seconds, const std::function<double(double)>& hz,
                               const std::function<double(double)>& envelope, double noiseRatio, SplitMix64& rng)
{
    const std::size_t frames = static_cast<std::size_t>(std::lround(seconds * rate));
    std::vector<double> tone(frames);
    std::vector<double> noise(frames);
    double phase = 2.0 * std::numbers::pi * rng.nextUnit();
    double toneEnergy = 0.0;
    double noiseEnergy = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        tone[i] = envelope(t) * std::sin(phase);
        phase += 2.0 * std::numbers::pi * hz(t) / rate;
        noise[i] = envelope(t) * (rng.nextUnit() * 2.0 - 1.0);
        toneEnergy += tone[i] * tone[i];
        noiseEnergy += noise[i] * noise[i];
    }
    const double noiseGain = noiseEnergy > 0.0 ? noiseRatio * std::sqrt(toneEnergy / noiseEnergy) : 0.0;
    std::vector<float> out(2 * frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const auto v = static_cast<float>(0.5 * (tone[i] + noiseGain * noise[i]));
        out[2 * i] = v;
        out[2 * i + 1] = v;
    }
    return out;
}

double flat(double)
{
    return 0.5;
}

std::vector<Partial> harmonicTone(const double f0, const int partials, const double gain, const double maxHz = 9000.0)
{
    std::vector<Partial> out;
    for (int h = 1; h <= partials && f0 * h <= maxHz; ++h) {
        out.push_back({f0 * h, gain / h});
    }
    return out;
}

double centsError(const PitchEstimate& note, const double hz)
{
    return 1200.0 * std::log2(note.hz / hz);
}

std::string describe(const char* what, const double hz, const double rate, const SoundAnalysis& a)
{
    return std::string(what) + " " + std::to_string(hz) + " Hz @" + std::to_string(static_cast<int>(rate))
           + ": note " + noteText(a.note) + " (" + std::to_string(a.note.hz) + " Hz, conf "
           + std::to_string(a.note.confidence) + ") chord " + chordText(a.chord) + " key " + keyText(a.key);
}

void sinesAcrossRange()
{
    SplitMix64 rng(101);
    for (const double rate : kRatesWith192) {
        // Sine 25..50 Hz, 1 s: +-5 cents, conf >= 0.75.
        for (const double hz : {25.0, 31.3, 40.7, 50.0}) {
            const auto a = analyzeSound(renderPartials(rate, 1.0, {{hz, 0.5}}, flat, rng), rate);
            check(a.note.valid && std::abs(centsError(a.note, hz)) <= 5.0 && a.note.confidence >= 0.75F,
                  describe("low sine", hz, rate, a));
        }
        // Sine 50..4000 Hz, 0.5 s: +-2 cents (no octave error), conf >= 0.9.
        for (int i = 0; i < 10; ++i) {
            const double hz = 50.0 * std::pow(80.0, (static_cast<double>(i) + rng.nextUnit()) / 10.0);
            const auto a = analyzeSound(renderPartials(rate, 0.5, {{hz, 0.4}}, flat, rng), rate);
            check(a.note.valid && std::abs(centsError(a.note, hz)) <= 2.0 && a.note.confidence >= 0.9F
                      && !a.chord.valid && !a.key.valid,
                  describe("sine", hz, rate, a));
        }
        // Sine 440 Hz, 20 ms: +-20 cents; the clip cannot see down to 25 Hz.
        const auto a = analyzeSound(renderPartials(rate, 0.020, {{440.0, 0.5}}, flat, rng), rate);
        check(std::abs(centsError(a.note, 440.0)) <= 20.0 && a.note.rangeLimited && !a.chord.valid,
              describe("20 ms sine", 440.0, rate, a));
    }
}

void harmonicTones()
{
    SplitMix64 rng(202);
    std::size_t index = 0;
    // Saw-like (1/h, band-limited to 9 kHz) tones 41..2000 Hz: +-3 cents, no octave error, no chord.
    for (const double hz : {41.2, 65.4, 98.0, 155.6, 261.6, 466.2, 830.6, 1244.5, 2000.0}) {
        const double rate = kRates[index++ % kRates.size()];
        const auto a = analyzeSound(renderPartials(rate, 1.0, harmonicTone(hz, 200, 0.3), flat, rng), rate);
        check(a.note.valid && std::abs(centsError(a.note, hz)) <= 3.0 && !a.chord.valid,
              describe("harmonic tone", hz, rate, a));
    }
    // Missing fundamental: partials 2..6 of 110 Hz read A2 +-5 cents, no chord.
    for (const double rate : kRates) {
        std::vector<Partial> partials;
        for (int h = 2; h <= 6; ++h) {
            partials.push_back({110.0 * h, 0.15});
        }
        const auto a = analyzeSound(renderPartials(rate, 1.0, partials, flat, rng), rate);
        check(a.note.valid && std::abs(centsError(a.note, 110.0)) <= 5.0 && !a.chord.valid,
              describe("missing fundamental", 110.0, rate, a));
    }
    // F#2 detuned by -23 cents reads "F#2 -23c" +-3 cents, as a sine and as a harmonic tone.
    for (const double rate : kRates) {
        const double hz = midiToHz(42.0 - 0.23);
        for (const bool rich : {false, true}) {
            const auto partials = rich ? harmonicTone(hz, 12, 0.3) : std::vector<Partial>{{hz, 0.5}};
            const auto a = analyzeSound(renderPartials(rate, 1.0, partials, flat, rng), rate);
            const double cents = 100.0 * (a.note.midi - 42.0);
            check(a.note.valid && std::abs(cents + 23.0) <= 3.0 && noteText(a.note).rfind("F#2 -2", 0) == 0,
                  describe("detuned F#2", hz, rate, a) + " text " + noteText(a.note));
        }
    }
}

struct ChordCase {
    ChordQuality quality;
    std::vector<int> intervals;
};

std::vector<Partial> chordPartials(const int rootMidi, const std::vector<int>& intervals, const bool rich,
                                   const double offsetCents)
{
    std::vector<Partial> out;
    for (const int interval : intervals) {
        const double hz = midiToHz(static_cast<double>(rootMidi + interval) + offsetCents / 100.0);
        const auto tone = rich ? harmonicTone(hz, 6, 0.12) : std::vector<Partial>{{hz, 0.12}};
        out.insert(out.end(), tone.begin(), tone.end());
    }
    return out;
}

double chordEnvelope(const double t)
{
    return std::min(1.0, t / 0.01) * std::exp(-t / 3.0);
}

void equalTemperedChords()
{
    const std::vector<ChordCase> cases{
        {ChordQuality::major, {0, 4, 7}},      {ChordQuality::minor, {0, 3, 7}},
        {ChordQuality::diminished, {0, 3, 6}}, {ChordQuality::augmented, {0, 4, 8}},
        {ChordQuality::dominant7, {0, 4, 7, 10}}, {ChordQuality::major7, {0, 4, 7, 11}},
        {ChordQuality::minor7, {0, 3, 7, 10}},
    };
    SplitMix64 rng(303);
    std::size_t total = 0;
    std::size_t correct = 0;
    std::size_t offsetTotal = 0;
    std::size_t offsetGood = 0;
    std::size_t index = 0;
    for (const double offset : {0.0, 30.0, -30.0}) {
        for (const bool rich : {false, true}) {
            for (const ChordCase& c : cases) {
                for (int root = 0; root < 12; ++root) {
                    const double rate = kRates[index++ % kRates.size()];
                    const auto audio =
                        renderPartials(rate, 2.0, chordPartials(48 + root, c.intervals, rich, offset), chordEnvelope, rng);
                    const auto a = analyzeSound(audio, rate);
                    ++total;
                    const bool good = a.chord.valid && a.chord.root == root && a.chord.quality == c.quality;
                    correct += good ? 1 : 0;
                    if (!good) {
                        std::cerr << "  chord miss: want " << chordText({true, root, c.quality, 1.0F, false})
                                  << (rich ? " rich" : " sine") << " offset " << offset << " got "
                                  << chordText(a.chord) << '\n';
                    }
                    if (offset != 0.0) {
                        ++offsetTotal;
                        offsetGood += std::abs(a.tuningOffsetCents - offset) <= 5.0 ? 1 : 0;
                    }
                }
            }
        }
    }
    std::cout << "chords: " << correct << "/" << total << " root+quality, tuning offset " << offsetGood << "/"
              << offsetTotal << '\n';
    check(static_cast<double>(correct) >= 0.95 * static_cast<double>(total), "ET chords >= 95% root + quality");
    check(offsetGood == offsetTotal, "global offset +-30 c measured within 5 c");

    // Sus chords need the bass to tell Csus2 from Gsus4 (same pitch classes).
    for (const auto& [quality, intervals] : std::vector<ChordCase>{{ChordQuality::sus2, {0, 2, 7}},
                                                                   {ChordQuality::sus4, {0, 5, 7}}}) {
        const auto a = analyzeSound(
            renderPartials(48000.0, 2.0, chordPartials(50, intervals, true, 0.0), chordEnvelope, rng), 48000.0);
        check(a.chord.valid && a.chord.root == 2 && a.chord.quality == quality,
              "sus chord root from bass, got " + chordText(a.chord));
    }
    // Power chord C3 + G3 reads "C5" (sines and harmonic tones).
    for (const bool rich : {false, true}) {
        const auto a = analyzeSound(
            renderPartials(44100.0, 2.0, chordPartials(48, {0, 7}, rich, 0.0), chordEnvelope, rng), 44100.0);
        check(chordText(a.chord) == "C5", std::string("power chord reads C5, got ") + chordText(a.chord));
    }
}

void keysFromProgressions()
{
    SplitMix64 rng(404);
    int correct = 0;
    int related = 0;
    int cases = 0;
    std::size_t index = 0;
    for (const bool minor : {false, true}) {
        for (int tonic = 0; tonic < 12; ++tonic) {
            // I-IV-V-I or i-iv-V-i, 2 s per chord, harmonic tones.
            const std::array<int, 4> degrees{0, 5, 7, 0};
            const double rate = kRates[index++ % kRates.size()];
            std::vector<float> audio;
            for (std::size_t c = 0; c < degrees.size(); ++c) {
                const int root = 48 + (tonic + degrees[c]) % 12;
                const bool minorChord = minor && degrees[c] != 7;
                const std::vector<int> intervals{0, minorChord ? 3 : 4, 7};
                const auto part = renderPartials(rate, 2.0, chordPartials(root, intervals, true, 0.0),
                                                 [](const double t) { return chordEnvelope(t) * std::min(1.0, (2.0 - t) / 0.02); },
                                                 rng);
                audio.insert(audio.end(), part.begin(), part.end());
            }
            const auto a = analyzeSound(audio, rate);
            ++cases;
            const bool exact = a.key.valid && a.key.tonic == tonic && a.key.minor == minor;
            const int relativeTonic = minor ? (tonic + 3) % 12 : (tonic + 9) % 12;
            const bool relative = a.key.valid && a.key.minor != minor
                                  && (a.key.tonic == relativeTonic || a.key.tonic == tonic);
            correct += exact ? 1 : 0;
            related += relative ? 1 : 0;
            if (!exact) {
                std::cerr << "  key miss: want " << keyText({true, tonic, minor, 1.0F, -1, false}) << " got "
                          << keyText(a.key) << '\n';
            }
            check(!a.chord.valid && a.chord.varies, "a progression's chord varies, got " + chordText(a.chord));
        }
    }
    std::cout << "keys: " << correct << "/" << cases << " exact, " << related << " relative/parallel\n";
    check(correct >= 22, "progressions >= 90% key");  // 0.9 * 24 = 21.6
    check(correct + related == cases, "key misses are relative or parallel only");
}

void drumsAndGlides()
{
    SplitMix64 rng(505);
    for (const double rate : kRates) {
        // 808: 110 -> 55 Hz glide (tau 40 ms), 1.5 s decay: A1 +-15 cents, no chord or key.
        const auto boom = analyzeSound(
            renderGlide(rate, 1.5, [](const double t) { return 55.0 + 55.0 * std::exp(-t / 0.04); },
                        [](const double t) { return std::min(1.0, t / 0.002) * std::exp(-t / 0.3); }, 0.0, rng),
            rate);
        check(boom.note.valid && std::abs(centsError(boom.note, 55.0)) <= 15.0 && !boom.chord.valid && !boom.key.valid,
              describe("808", 55.0, rate, boom));

        // Kick: 150 -> 50 Hz, 300 ms: +-50 cents of 50 Hz.
        const auto kick = analyzeSound(
            renderGlide(rate, 0.3, [](const double t) { return 50.0 + 100.0 * std::exp(-t / 0.03); },
                        [](const double t) { return std::min(1.0, t / 0.001) * std::exp(-t / 0.08); }, 0.0, rng),
            rate);
        check(std::abs(centsError(kick.note, 50.0)) <= 50.0 && !kick.chord.valid && !kick.key.valid,
              describe("kick", 50.0, rate, kick));

        // Tom: decaying 120 Hz + inharmonic partial at 1.59x: +-30 cents, no chord.
        const auto tom = analyzeSound(
            renderPartials(rate, 0.8, {{120.0, 0.5}, {120.0 * 1.59, 0.3}},
                           [](const double t) { return std::min(1.0, t / 0.002) * std::exp(-t / 0.15); }, rng),
            rate);
        check(std::abs(centsError(tom.note, 120.0)) <= 30.0 && !tom.chord.valid, describe("tom", 120.0, rate, tom));

        // Snare: 190 Hz decay + white noise at equal RMS: +-50 cents, conf <= 0.6, no chord or key.
        const auto snare = analyzeSound(
            renderGlide(rate, 0.5, [](double) { return 190.0; },
                        [](const double t) { return std::min(1.0, t / 0.001) * std::exp(-t / 0.08); }, 1.0, rng),
            rate);
        check(std::abs(centsError(snare.note, 190.0)) <= 50.0 && snare.note.confidence <= 0.6F && !snare.chord.valid
                  && !snare.key.valid,
              describe("snare", 190.0, rate, snare));
    }
}

bool allNone(const SoundAnalysis& a)
{
    return !a.cancelled && !a.note.valid && !a.chord.valid && !a.key.valid;
}

void degenerateInputs()
{
    SplitMix64 rng(606);
    for (const double rate : kRates) {
        std::vector<float> noise(static_cast<std::size_t>(2.0 * rate) * 2);
        for (float& v : noise) {
            v = static_cast<float>(0.5 * (rng.nextUnit() * 2.0 - 1.0));
        }
        const auto a = analyzeSound(noise, rate);
        check(allNone(a), describe("white noise", 0.0, rate, a));
    }
    const std::vector<float> silence(2 * 48000, 0.0F);
    check(allNone(analyzeSound(silence, 48000.0)) && analyzeSound(silence, 48000.0).silent, "silence is silent");
    check(allNone(analyzeSound(std::vector<float>{0.5F, 0.5F}, 48000.0)), "one sample");
    check(allNone(analyzeSound(std::vector<float>{}, 48000.0)), "empty input");
    check(allNone(analyzeSound(std::vector<float>{0.5F}, 48000.0)), "half a frame");
    std::vector<float> nan(2 * 48000, std::numeric_limits<float>::quiet_NaN());
    check(allNone(analyzeSound(nan, 48000.0)), "NaN input");
    // Partly NaN/inf input: non-finite samples count as silence, the tone still reads.
    auto tone = renderPartials(48000.0, 1.0, {{220.0, 0.5}}, flat, rng);
    tone[1001] = std::numeric_limits<float>::infinity();
    tone[5000] = std::numeric_limits<float>::quiet_NaN();
    const auto partly = analyzeSound(tone, 48000.0);
    check(partly.note.valid && std::abs(centsError(partly.note, 220.0)) <= 5.0, "partly non-finite tone still reads");
    for (const double badRate : {0.0, -1.0, 4000.0, 400000.0, std::numeric_limits<double>::quiet_NaN()}) {
        check(allNone(analyzeSound(tone, badRate)), "unsupported rate reads as silent");
    }
    // A cancel flag set before the call returns a cancelled result.
    std::atomic<bool> cancel{true};
    const auto cancelled = analyzeSound(tone, 48000.0, &cancel);
    check(cancelled.cancelled && !cancelled.note.valid, "cancel before call");
}

void stereoInputs()
{
    SplitMix64 rng(707);
    const double hz = midiToHz(57.0);
    for (const Stereo mode : {Stereo::unequal, Stereo::antiPhase}) {
        for (const double rate : kRates) {
            const auto a = analyzeSound(renderPartials(rate, 1.0, harmonicTone(hz, 8, 0.3), flat, rng, mode), rate);
            check(a.note.valid && std::abs(centsError(a.note, hz)) <= 3.0,
                  describe(mode == Stereo::antiPhase ? "anti-phase stereo" : "unequal stereo", hz, rate, a));
        }
    }
}

void textFormatting()
{
    PitchEstimate note;
    note.valid = true;
    note.midi = 42.0 - 0.23;
    note.confidence = 0.9F;
    check(noteText(note) == "F#2 -23c", "note text " + noteText(note));
    note.midi = 60.0;
    check(noteText(note) == "C4 +0c", "C4 = MIDI 60");
    note.approximate = true;
    check(noteText(note) == "~C4 +0c", "approximate prefix");
    note.valid = false;
    check(noteText(note) == "-", "invalid note");
    check(chordText({true, 0, ChordQuality::major7, 1.0F, false}) == "Cmaj7", "Cmaj7");
    check(chordText({true, 9, ChordQuality::minor, 1.0F, false}) == "Am", "Am");
    check(chordText({true, 0, ChordQuality::power, 1.0F, false}) == "C5", "C5");
    check(chordText({}) == "-", "no chord");
    check(keyText({true, 9, true, 1.0F, 0, false}) == "A min", "A min");
    check(keyText({}) == "-", "no key");
    // Gates: percussive material and a lone pitch class never make a chord; short clips have no key.
    Chroma triad{};
    triad[0] = 1.0F;
    triad[4] = 0.9F;
    triad[7] = 0.9F;
    check(chordFrom(triad, 0, false).valid && !chordFrom(triad, 0, true).valid, "percussive gate");
    Chroma lone{};
    lone[0] = 1.0F;
    check(!chordFrom(lone, 0, false).valid, "single pitch class is no chord");
    Chroma scale{1.0F, 0.0F, 0.6F, 0.0F, 0.8F, 0.5F, 0.0F, 0.9F, 0.0F, 0.5F, 0.0F, 0.4F};
    check(keyFrom(scale, 4.0, false).valid && keyFrom(scale, 4.0, false).tonic == 0, "C major scale key");
    check(!keyFrom(scale, 1.5, false).valid, "no key under 2 s");
}

} // namespace

int main()
{
    const auto started = std::chrono::steady_clock::now();
    textFormatting();
    sinesAcrossRange();
    harmonicTones();
    stereoInputs();
    drumsAndGlides();
    degenerateInputs();
    equalTemperedChords();
    keysFromProgressions();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cout << "sound analysis tests: " << seconds << " s, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
