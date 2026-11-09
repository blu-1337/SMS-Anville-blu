// sms_analysis: FFT, PRNG, onsets, tempo and slice strategies.
#include "Analysis/OnsetAnalysis.hpp"
#include "Analysis/RealFft.hpp"
#include "Analysis/SplitMix64.hpp"
#include "Analysis/TempoEstimate.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <vector>

namespace {
using namespace sms::analysis;

void check(const bool value, const char* const message)
{
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void fftMatchesNaiveDft()
{
    for (std::size_t n = 8; n <= 1024; n <<= 1U) {
        SplitMix64 rng(n);
        std::vector<float> x(n);
        for (float& v : x) {
            v = static_cast<float>(rng.nextUnit() * 2.0 - 1.0);
        }
        RealFft fft(n);
        std::vector<float> bins(n / 2 + 1);
        fft.magnitudes(x, bins);
        double worst = 0.0;
        for (std::size_t k = 0; k <= n / 2; ++k) {
            double re = 0.0;
            double im = 0.0;
            for (std::size_t t = 0; t < n; ++t) {
                const double a = -2.0 * std::numbers::pi * static_cast<double>(k * t % n) / static_cast<double>(n);
                re += static_cast<double>(x[t]) * std::cos(a);
                im += static_cast<double>(x[t]) * std::sin(a);
            }
            worst = std::max(worst, std::abs(std::hypot(re, im) - static_cast<double>(bins[k])));
        }
        check(worst < 1e-4, "FFT magnitudes match a naive DFT");
    }
}

void fftIgnoresShortSpans()
{
    RealFft fft(16);
    std::vector<float> in(8, 1.0F);
    std::vector<float> out(9, -1.0F);
    fft.magnitudes(in, out);
    check(out[0] == -1.0F, "short input leaves output untouched");
    check(fft.size() == 16, "size");
    check(RealFft(10).size() == 16, "size rounds up to a power of two");
}

void splitMixIsDeterministic()
{
    SplitMix64 a(42);
    SplitMix64 b(42);
    SplitMix64 c(43);
    check(SplitMix64(0).next() == 0xE220A8397B1DCDAFULL, "known SplitMix64 vector for seed 0");
    bool differs = false;
    for (int i = 0; i < 16; ++i) {
        const auto va = a.next();
        check(va == b.next(), "same seed, same stream");
        differs = differs || va != c.next();
    }
    check(differs, "different seed, different stream");
    SplitMix64 d(7);
    for (int i = 0; i < 1000; ++i) {
        const double u = d.nextUnit();
        check(u >= 0.0 && u < 1.0, "nextUnit range");
        check(d.nextBelow(5) < 5, "nextBelow range");
    }
    check(d.nextBelow(0) == 0, "nextBelow(0)");
}

std::vector<float> noiseBed(const std::size_t frames, const float amplitude, const std::uint64_t seed)
{
    SplitMix64 rng(seed);
    std::vector<float> out(frames * 2);
    for (float& v : out) {
        v = amplitude * static_cast<float>(rng.nextUnit() * 2.0 - 1.0);
    }
    return out;
}

// Decaying 200 Hz burst starting at frame `at` (phase 0).
void addBurst(std::vector<float>& stereo, const double rate, const std::int64_t at, const float amplitude)
{
    const std::int64_t length = static_cast<std::int64_t>(0.12 * rate);
    for (std::int64_t i = 0; i < length; ++i) {
        const std::size_t f = static_cast<std::size_t>(at + i);
        if (f * 2 + 1 >= stereo.size()) {
            break;
        }
        const double t = static_cast<double>(i) / rate;
        const auto v = static_cast<float>(amplitude * std::exp(-t * 30.0) * std::sin(2.0 * std::numbers::pi * 200.0 * t));
        stereo[f * 2] += v;
        stereo[f * 2 + 1] += v;
    }
}

void addClick(std::vector<float>& stereo, const std::int64_t at)
{
    for (std::int64_t i = 0; i < 4; ++i) {
        const std::size_t f = static_cast<std::size_t>(at + i);
        if (f * 2 + 1 < stereo.size()) {
            stereo[f * 2] += i % 2 == 0 ? 0.8F : -0.6F;
            stereo[f * 2 + 1] += i % 2 == 0 ? 0.8F : -0.6F;
        }
    }
}

std::vector<float> clickTrack(const double rate, const double seconds, const double bpm)
{
    auto x = noiseBed(static_cast<std::size_t>(rate * seconds), 0.001F, 3);
    const double period = 60.0 / bpm * rate;
    for (double t = 0.5 * rate; t < rate * seconds - 100; t += period) {
        addClick(x, static_cast<std::int64_t>(std::llround(t)));
    }
    return x;
}

void onsetsFoundWithinTolerance()
{
    for (const double rate : {44100.0, 48000.0, 96000.0}) {
        for (const bool clicks : {true, false}) {
            auto x = noiseBed(static_cast<std::size_t>(rate * 6.0), 0.001F, 11);
            std::vector<std::int64_t> truth;
            for (int i = 0; i < 8; ++i) {
                const auto at = static_cast<std::int64_t>(rate * (0.4 + 0.7 * i));
                truth.push_back(at);
                clicks ? addClick(x, at) : addBurst(x, rate, at, 0.7F);
            }
            const auto a = analyzeOnsets(x, rate);
            const auto picked = pickOnsets(a, 0.6F, 0.05, 0, a.sourceFrames);
            check(picked.size() == truth.size(), "every onset found, no extras");
            for (std::size_t i = 0; i < truth.size() && i < picked.size(); ++i) {
                const double err = static_cast<double>(picked[i] - truth[i]) / rate;
                check(std::abs(err) <= 0.002, "onset within 2 ms");
                check(err <= 0.0005, "refinement lands at or before the attack");
            }
        }
    }
}

void silenceDcAndSineHaveNoOnsets()
{
    const double rate = 44100.0;
    const std::size_t n = 44100 * 4;
    std::vector<float> silence(n * 2, 0.0F);
    std::vector<float> dc(n * 2, 0.5F);
    std::vector<float> sine(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        sine[i * 2] = sine[i * 2 + 1] = 0.5F * static_cast<float>(std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / rate));
    }
    for (const auto* v : {&silence, &dc, &sine}) {
        const auto a = analyzeOnsets(*v, rate);
        check(pickOnsets(a, 1.0F, 0.0, 0, a.sourceFrames).empty(), "no onsets in silence, DC or a steady sine");
    }
}

void sensitivityAndGap()
{
    const double rate = 44100.0;
    auto x = noiseBed(static_cast<std::size_t>(rate * 8), 0.01F, 5);
    SplitMix64 rng(9);
    for (int i = 0; i < 24; ++i) {
        addBurst(x, rate, static_cast<std::int64_t>(rate * (0.3 + 0.3 * i)), static_cast<float>(0.05 + 0.6 * rng.nextUnit()));
    }
    const auto a = analyzeOnsets(x, rate);
    std::size_t previous = 0;
    for (int step = 0; step <= 10; ++step) {
        const auto picked = pickOnsets(a, static_cast<float>(step) / 10.0F, 0.05, 0, a.sourceFrames);
        check(picked.size() >= previous, "onset count is monotonic in sensitivity");
        previous = picked.size();
    }
    const auto wide = pickOnsets(a, 1.0F, 0.5, 0, a.sourceFrames);
    for (std::size_t i = 1; i < wide.size(); ++i) {
        check(static_cast<double>(wide[i] - wide[i - 1]) >= 0.5 * rate, "minimum gap holds");
    }
    const auto ranged = pickOnsets(a, 1.0F, 0.05, static_cast<std::int64_t>(rate), static_cast<std::int64_t>(2 * rate));
    for (const auto f : ranged) {
        check(f >= static_cast<std::int64_t>(rate) && f < static_cast<std::int64_t>(2 * rate), "range respected");
    }
}

void determinismCancelAndEdges()
{
    const double rate = 44100.0;
    const auto x = clickTrack(rate, 5.0, 120.0);
    const auto a = analyzeOnsets(x, rate);
    const auto b = analyzeOnsets(x, rate);
    check(a.candidates.size() == b.candidates.size() && a.novelty == b.novelty, "analysis is deterministic");
    for (std::size_t i = 0; i < a.candidates.size(); ++i) {
        check(a.candidates[i].frame == b.candidates[i].frame, "candidates identical");
    }
    check(a.rms.size() == a.novelty.size() && a.bands.size() == a.novelty.size(), "per-hop vectors align");
    const std::atomic<bool> stop{true};
    const auto c = analyzeOnsets(x, rate, {}, &stop);
    check(c.cancelled && c.candidates.empty() && c.novelty.empty(), "cancel returns an empty cancelled result");
    check(analyzeOnsets({}, rate).candidates.empty(), "empty input");
    const std::vector<float> one(2, 0.5F);
    check(analyzeOnsets(one, rate).novelty.empty(), "one-frame input");
    check(analyzeOnsets(x, 0.0).novelty.empty(), "zero rate");
    check(pickOnsets(OnsetAnalysis{}, 0.5F, 0.05, 0, 100).empty(), "empty analysis picks nothing");
}

void tempoOnClickTracks()
{
    for (const double rate : {44100.0, 48000.0}) {
        for (const double bpm : {90.0, 120.0, 140.0}) {
            const auto a = analyzeOnsets(clickTrack(rate, 30.0, bpm), rate);
            const auto t = estimateTempo(a.novelty, a.hopSeconds);
            check(std::abs(t.bpm - bpm) <= 1.0, "tempo within 1 BPM");
            check(t.confidence > 0.1, "tempo confidence");
        }
    }
    check(estimateTempo({}, 0.01).bpm == 0.0, "no tempo without material");
}

void analysisSpeed()
{
#ifdef NDEBUG
    const double rate = 44100.0;
    const auto x = clickTrack(rate, 180.0, 128.0);
    const auto start = std::chrono::steady_clock::now();
    const auto a = analyzeOnsets(x, rate);
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    check(!a.candidates.empty(), "speed test finds onsets");
    check(seconds < 1.0, "3 minutes analyze in under 1 s");
#endif
}
} // namespace

int main()
{
    fftMatchesNaiveDft();
    fftIgnoresShortSpans();
    splitMixIsDeterministic();
    onsetsFoundWithinTolerance();
    silenceDcAndSineHaveNoOnsets();
    sensitivityAndGap();
    determinismCancelAndEdges();
    tempoOnClickTracks();
    analysisSpeed();
    return EXIT_SUCCESS;
}
