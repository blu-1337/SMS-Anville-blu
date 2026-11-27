#include "../src/core/PlaybackVoice.hpp"
#include "reference_resampler.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
using midichopper::PlaybackVoice;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct Source {
    midichopper::SampleStoragePool pool{1U};
    midichopper::SampleStoragePool::Storage* storage;
    PlaybackVoice::Parameters parameters;

    Source() {
        midichopper::PadData data;
        data.sampleRate = 1000.0;
        data.frames = 4U;
        data.stereo = {1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f, 4.0f, -4.0f};
        storage = pool.prepareImport(data, 2000.0, 4U);
        check(storage != nullptr, "prepare immutable source");
        storage->references.store(1U); // The engine owns the storage reference.
        parameters.mixer.holdLoop = 1.0f; // Hold tests loop
        parameters.frames = data.frames;
        parameters.sourceSampleRate = data.sampleRate;
        parameters.sampleRate = 2000.0;
    }

    void render(PlaybackVoice& voice, const float expected) {
        check(voice.playing(), "caller only renders an active voice");
        const bool integerPhase = voice.position() == std::floor(voice.position());
        float left = 8.0f, right = -16.0f;
        const bool active = voice.render(pool, storage, parameters.frames,
                                        parameters.sourceSampleRate, left, right);
        check(active == voice.playing(), "render reports resulting activity");
        if (integerPhase)
            check(left == 8.0f + expected && right == -16.0f - expected,
                  "integer phases preserve exact source PCM in the mix");
        // Fractional phases compare the lookup kernel against an analytic oracle.
        check(std::abs(left - (8.0f + expected)) < 1.0e-5f &&
              std::abs(right - (-16.0f - expected)) < 1.0e-5f,
              "interpolated stereo adds to the existing mix");
        check(storage->references.load() == 1U, "DSP leaves storage ownership with the engine");
    }
};

void fractional_loop_and_direction() {
    Source source;
    PlaybackVoice voice;
    voice.start(source.parameters, 127U, PlaybackVoice::Mode::Loop, -1, 7U);
    constexpr std::array<float, 8> pcm{1,-1, 2,-2, 3,-3, 4,-4};
    const auto sample = [&](double position, bool reverse = false) {
        return reference_resampler::sample(pcm, position, 0.5, 0U, 4U, true, reverse);
    };
    for (unsigned frame = 0U; frame < 9U; ++frame)
        source.render(voice, sample(std::fmod(frame * 0.5, 4.0)));
    check(voice.position() == 0.5, "loop retains fractional source phase");
    voice.redirect(source.parameters.frames, true);
    check(voice.reverse() && voice.position() == 2.5,
          "direction turn mirrors the active fractional cursor");
    source.render(voice, sample(2.5, true));
    source.render(voice, sample(3.0, true));
    source.render(voice, sample(3.5, true));
    source.render(voice, sample(0.0, true));
}

void cut_edits_report_engine_actions() {
    Source source;
    PlaybackVoice voice;
    voice.start(source.parameters, 127U, PlaybackVoice::Mode::Hold, 36, 1U);
    source.render(voice, 1.0f);
    constexpr std::array<float, 8> pcm{1,-1, 2,-2, 3,-3, 4,-4};
    source.render(voice, reference_resampler::sample(pcm, 0.5, 0.5, 0U, 4U, true));
    source.render(voice, 2.0f);
    source.parameters.playback.start = 0.5f;
    check(voice.refresh(source.parameters), "loop accepts edited Start");
    check(voice.position() == 3.5, "edited loop wraps an earlier cursor fractionally");
    source.render(voice, reference_resampler::sample(pcm, 3.5, 0.5, 2U, 4U, true));

    source.parameters.playback = {};
    voice.start(source.parameters, 127U, PlaybackVoice::Mode::OneShot, 36, 2U);
    source.render(voice, 1.0f);
    source.parameters.playback.start = 0.5f;
    check(voice.refresh(source.parameters), "one-shot accepts settings without moving Start");
    source.render(voice, reference_resampler::sample(pcm, 0.5, 0.5, 0U, 4U, false));
    source.parameters.playback.start = 0.0f;
    source.parameters.playback.end = 0.25f;
    check(!voice.refresh(source.parameters), "one-shot cut past its cursor asks the engine to stop");
    check(voice.playing(), "refresh leaves ownership cleanup to the engine");
    voice.hardStop();
    check(!voice.playing() && voice.triggerNote() == -1, "engine stop clears MIDI ownership");
}

void lifecycle_preserves_distinct_reset_contracts() {
    Source source;
    PlaybackVoice voice;
    source.parameters.mixer.reverse = 1.0f;
    voice.start(source.parameters, 127U, PlaybackVoice::Mode::Hold, 60, 9U);
    source.render(voice, 4.0f);
    voice.suspend();
    check(!voice.playing() && voice.order() == 9U && voice.triggerNote() == 60 &&
          voice.position() == 0.5 && voice.reverse() && voice.mode() == PlaybackVoice::Mode::Hold,
          "rate suspension preserves DSP and trigger metadata");

    voice.start(source.parameters, 127U, PlaybackVoice::Mode::Hold, 61, 10U);
    source.render(voice, 4.0f);
    voice.reset();
    check(!voice.playing() && voice.order() == 0U && voice.triggerNote() == 61 &&
          voice.position() == 0.5 && voice.reverse() && voice.mode() == PlaybackVoice::Mode::Hold,
          "capture and clear reset activity and age while preserving trigger metadata");

    voice.start(source.parameters, 127U, PlaybackVoice::Mode::Hold, 62, 11U);
    check(voice.release(source.parameters.playback), "zero release requests stop publication");
    check(!voice.playing() && voice.triggerNote() == 62 && voice.order() == 11U,
          "natural release retains trigger ownership and age");
    check(!voice.release(source.parameters.playback), "repeated release requires no publication");
    voice.hardStop();
    check(!voice.playing() && voice.triggerNote() == -1 && voice.order() == 0U,
          "hard stop clears ownership and age");
    check(source.storage->references.load() == 1U, "voice lifecycle never releases engine storage");
}

// Pad vintage FX: Off (any other vintage fields) is bit-identical; On changes the
// sound and stays finite; switching back Off returns to the exact original output.
void vintage_off_is_exact_and_on_is_finite() {
    Source source;
    source.parameters.mixer.tuneSemitones = 0.37f; // fractional reads exercise the sinc path
    auto offVariant = source.parameters;
    offVariant.mixer.vintage = {0.0f, 1.0f, 6.0f, 9000.0f, 18.0f, 1.0f, 0.5f, 1.0f};
    auto on = source.parameters;
    on.mixer.vintage.model = 1.0f; // drum model, Open
    on.mixer.vintage.bits = 6.0f;
    PlaybackVoice reference, off, wet, toggled;
    reference.start(source.parameters, 127U, PlaybackVoice::Mode::Loop, -1, 1U);
    off.start(offVariant, 127U, PlaybackVoice::Mode::Loop, -1, 1U);
    wet.start(on, 127U, PlaybackVoice::Mode::Loop, -1, 1U);
    toggled.start(on, 127U, PlaybackVoice::Mode::Loop, -1, 1U);
    const auto renderOne = [&](PlaybackVoice& voice, float& left, float& right) {
        left = right = 0.0f;
        static_cast<void>(voice.render(source.pool, source.storage, source.parameters.frames,
                                       source.parameters.sourceSampleRate, left, right));
    };
    bool differs = false;
    for (unsigned frame = 0U; frame < 400U; ++frame) {
        if (frame == 100U)
            check(toggled.refresh(source.parameters), "vintage Off refresh keeps the voice");
        float l0, r0, l1, r1, l2, r2, l3, r3;
        renderOne(reference, l0, r0);
        renderOne(off, l1, r1);
        renderOne(wet, l2, r2);
        renderOne(toggled, l3, r3);
        check(std::bit_cast<std::uint32_t>(l0) == std::bit_cast<std::uint32_t>(l1) &&
              std::bit_cast<std::uint32_t>(r0) == std::bit_cast<std::uint32_t>(r1),
              "vintage Off is bit-identical whatever its other fields hold");
        check(std::isfinite(l2) && std::isfinite(r2) && std::abs(l2) < 16.0f,
              "vintage On output stays finite");
        differs = differs || l2 != l0;
        if (frame >= 200U) // 10 ms fade at 2 kHz = 20 frames; 100 frames is ample
            check(l3 == l0 && r3 == r0, "switching vintage Off returns to the exact dry output");
    }
    check(differs, "vintage On changes the pad's sound");
}
// A settled vintage read stays on the linear read; it must not drift back to the sinc.
void vintage_read_blend_settles() {
    Source source;
    source.parameters.mixer.vintage.model = 1.0f;
    PlaybackVoice voice;
    voice.start(source.parameters, 127U, PlaybackVoice::Mode::Loop, -1, 1U);
    for (unsigned frame = 0U; frame < 50U; ++frame) {
        float left = 0.0f, right = 0.0f;
        static_cast<void>(voice.render(source.pool, source.storage, source.parameters.frames,
                                       source.parameters.sourceSampleRate, left, right));
        check(voice.vintageReadBlend() == 1.0f, "a settled vintage read blend stays at 1");
    }
}
} // namespace

int main() {
    fractional_loop_and_direction();
    cut_edits_report_engine_actions();
    lifecycle_preserves_distinct_reset_contracts();
    vintage_off_is_exact_and_on_is_finite();
    vintage_read_blend_settles();
    std::cout << "Playback voice tests passed\n";
}
