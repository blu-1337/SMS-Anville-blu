#include "../src/core/PlaybackVoice.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <vector>

// GSP pitch mode (Docs/AI/VINTAGE-FX.md): 26041.67 Hz machine clock, 12-bit, drop/repeat, held.
namespace {
using midichopper::PlaybackVoice;

void check(const bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

constexpr double kRate = 48000.0;

struct Source {
    midichopper::SampleStoragePool pool{1U};
    midichopper::SampleStoragePool::Storage* storage = nullptr;
    PlaybackVoice::Parameters parameters;

    explicit Source(const std::uint32_t frames, const double hz)
    {
        midichopper::PadData data;
        data.sampleRate = kRate;
        data.frames = frames;
        data.stereo.resize(frames * 2U);
        for (std::uint32_t i = 0; i < frames; ++i) {
            const auto value = static_cast<float>(
                0.5 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
            data.stereo[i * 2U] = value;
            data.stereo[i * 2U + 1U] = -value;
        }
        storage = pool.prepareImport(data, kRate, frames);
        check(storage != nullptr, "prepare source");
        storage->references.store(1U);
        parameters.frames = frames;
        parameters.sourceSampleRate = kRate;
        parameters.sampleRate = kRate;
    }

    std::vector<float> render(PlaybackVoice::Parameters p, const std::uint32_t count,
                              std::vector<float>* right = nullptr)
    {
        PlaybackVoice voice;
        voice.start(p, 127U, PlaybackVoice::Mode::Loop, -1, 1U);
        std::vector<float> out(count, 0.0f);
        for (auto& sample : out) {
            float l = 0.0f, r = 0.0f;
            static_cast<void>(voice.render(pool, storage, p.frames, p.sourceSampleRate, l, r));
            sample = l;
            if (right != nullptr)
                right->push_back(r);
        }
        return out;
    }
};

// Relative magnitude of `hz` in the signal (Goertzel), skipping the attack.
double magnitude(const std::vector<float>& x, const double hz)
{
    const double w = 2.0 * std::numbers::pi * hz / kRate;
    double re = 0.0, im = 0.0;
    for (std::size_t i = 4800U; i < x.size(); ++i) {
        const double window = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi *
            static_cast<double>(i - 4800U) / static_cast<double>(x.size() - 4800U));
        re += x[i] * window * std::cos(w * static_cast<double>(i));
        im -= x[i] * window * std::sin(w * static_cast<double>(i));
    }
    return std::hypot(re, im) / static_cast<double>(x.size() - 4800U);
}

void offIsBitIdentical()
{
    Source source(48000U, 1000.0);
    source.parameters.mixer.tuneSemitones = 0.37f;
    auto toggled = source.parameters;
    toggled.mixer.gsp = 1.0f;
    PlaybackVoice reference, back;
    reference.start(source.parameters, 127U, PlaybackVoice::Mode::Loop, -1, 1U);
    back.start(toggled, 127U, PlaybackVoice::Mode::Loop, -1, 1U);
    for (unsigned frame = 0U; frame < 2000U; ++frame) {
        if (frame == 100U)
            check(back.refresh(source.parameters), "GSP Off refresh keeps the voice");
        float l0 = 0, r0 = 0, l1 = 0, r1 = 0;
        static_cast<void>(reference.render(source.pool, source.storage, 48000U, kRate, l0, r0));
        static_cast<void>(back.render(source.pool, source.storage, 48000U, kRate, l1, r1));
        if (frame >= 700U) // 10 ms fade = 480 frames
            check(std::bit_cast<std::uint32_t>(l0) == std::bit_cast<std::uint32_t>(l1) &&
                      std::bit_cast<std::uint32_t>(r0) == std::bit_cast<std::uint32_t>(r1),
                  "switching GSP Off returns to the exact interpolated output");
    }
}

void heldAndQuantised()
{
    Source source(48000U, 1000.0);
    auto on = source.parameters;
    on.mixer.gsp = 1.0f;
    const auto out = source.render(on, 48000U);
    std::size_t changes = 0U;
    for (std::size_t i = 4800U; i < out.size(); ++i) {
        const float steps = out[i] * 2048.0f;
        check(std::abs(steps - std::round(steps)) < 1.0e-3f, "GSP output sits on the 12-bit grid");
        changes += out[i] != out[i - 1U] ? 1U : 0U;
    }
    // 43200 host frames at a 26041.67 Hz machine clock hold each value ~1.84 frames.
    check(changes > 20000U && changes <= 23438U, "GSP holds each machine sample (zero-order hold)");
}

void aliasingUpAndDown()
{
    Source source(48000U, 10000.0);
    for (const float tune : {12.0f, -12.0f}) {
        auto off = source.parameters;
        off.mixer.tuneSemitones = tune;
        auto on = off;
        on.mixer.gsp = 1.0f;
        // Up: 20 kHz at a 26041.67 Hz clock folds to 6041.67 Hz.
        // Down: repeats make a 13020.83 Hz stored rate; 5 kHz images at 8020.83 Hz.
        const double alias = tune > 0.0f ? 6041.67 : 8020.83;
        const double wet = magnitude(source.render(on, 48000U), alias);
        const double dry = magnitude(source.render(off, 48000U), alias);
        check(wet > 0.01 && wet > 20.0 * dry, tune > 0.0f ? "pitching up with GSP aliases"
                                                          : "pitching down with GSP aliases");
    }
}

void globalTuneAndFlagDriveIt()
{
    Source source(48000U, 3000.0);
    auto pad = source.parameters;
    pad.mixer.gsp = 1.0f;
    pad.mixer.tuneSemitones = 7.0f;
    auto global = source.parameters;
    global.global.gsp = true;
    global.global.tuneSemitones = 7.0f;
    check(source.render(pad, 6000U) == source.render(global, 6000U),
          "global TUNE drives GSP exactly like pad TUNE; the global flag works like the pad flag");
    auto bypassed = global;
    bypassed.mixer.globalMixer = 0.0f;
    auto untuned = pad;
    untuned.mixer.tuneSemitones = 0.0f;
    check(source.render(bypassed, 6000U) == source.render(untuned, 6000U),
          "with GLOBAL MIXER off the global TUNE does not apply; global GSP still does");
}

void reverseLoopBoundsAndLiveTune()
{
    Source source(37U, 2000.0);
    for (const float reverse : {0.0f, 1.0f}) {
        auto p = source.parameters;
        p.mixer.gsp = 1.0f;
        p.mixer.reverse = reverse;
        p.playback.start = 0.2f;
        p.playback.end = 0.9f;
        p.playback.attackSeconds = 0.0f;
        PlaybackVoice voice;
        voice.start(p, 127U, PlaybackVoice::Mode::Loop, -1, 1U);
        for (unsigned frame = 0U; frame < 20000U; ++frame) {
            p.mixer.tuneSemitones = static_cast<float>(static_cast<int>(frame % 97U) - 48) * 0.5f;
            if (frame % 7U == 0U)
                check(voice.refresh(p), "live tune keeps the looping GSP voice");
            float l = 0.0f, r = 0.0f;
            check(voice.render(source.pool, source.storage, 37U, kRate, l, r), "loop keeps playing");
            check(std::isfinite(l) && std::abs(l) <= 0.5f && std::abs(l + r) < 1.0e-6f,
                  "GSP reads stay inside the source in reverse and loop");
            check(voice.position() >= 7.0 && voice.position() < 34.0, "loop bounds hold");
        }
    }
    check(sms::dsp::gspSourceOffset(0.0, 0.25, kRate) == 0.0 &&
              sms::dsp::gspSourceOffset(-3.0, 2.0, kRate) == 0.0,
          "the GSP grid starts at the region start");
    check(sms::dsp::gspQuantize(1.0f) == 2047.0f / 2048.0f && sms::dsp::gspQuantize(-1.0f) == -1.0f &&
              sms::dsp::gspQuantize(0.0001f) == 0.0f,
          "12-bit mid-tread quantiser clamps to -2048..2047");
}
} // namespace

int main()
{
    offIsBitIdentical();
    heldAndQuantised();
    aliasingUpAndDown();
    globalTuneAndFlagDriveIt();
    reverseLoopBoundsAndLiveTune();
    std::cout << "GSP pitch tests passed\n";
}
