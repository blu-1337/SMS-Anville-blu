// Engine support for the Main Sample view: storage provenance (pad links),
// ranged reads, atomic multi-pad imports, the source storage and its preview.
#include "../src/core/SamplerEngine.hpp"
#include "../src/core/PadClipboard.hpp"
#include "../src/core/PadFingerprint.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <vector>

namespace {
using midichopper::kPadCount;
using midichopper::PadData;
using midichopper::SamplerEngine;
using Links = std::array<std::uint64_t, kPadCount>;

void check(const bool value, const char* const message)
{
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

PadData ramp(const std::uint32_t frames, const float offset, const double rate = 1000.0)
{
    PadData data;
    data.sampleRate = rate;
    data.frames = frames;
    data.stereo.resize(static_cast<std::size_t>(frames) * 2U);
    for (std::uint32_t frame = 0U; frame < frames; ++frame) {
        const float value = offset + static_cast<float>(frame) * 0.001f;
        data.stereo[static_cast<std::size_t>(frame) * 2U] = value;
        data.stereo[static_cast<std::size_t>(frame) * 2U + 1U] = -value;
        data.peak = std::max(data.peak, std::abs(value));
    }
    data.rms = data.peak * 0.5f;
    return data;
}

void processBlock(SamplerEngine& engine, const std::uint32_t frames = 1U)
{
    std::array<float, 64> left{};
    std::array<float, 64> right{};
    engine.process(nullptr, nullptr, left.data(), right.data(), std::min(frames, 64U));
}

void capture_descriptor_reuse_resets_link()
{
    midichopper::SampleStoragePool pool(2U);
    auto* first = pool.acquireCapture();
    check(first != nullptr, "acquire a capture descriptor");
    first->provenance.store(42U);
    first->references.fetch_sub(1U);
    auto* again = pool.acquireCapture();
    check(again == first && again->provenance.load() == 0U,
          "a reused capture descriptor starts unlinked");
    again->references.fetch_sub(1U);
}

void ranged_copy_is_bit_exact()
{
    midichopper::SampleStoragePool pool(2U);
    const auto data = ramp(10U, 0.2f);
    auto* imported = pool.prepareImport(data, 1000.0, 1024U);
    check(imported != nullptr, "prepare imported storage");
    std::array<float, 8> range{};
    range.fill(9.0f);
    pool.copySamples(*imported, 7U, range);
    check(range[0] == data.stereo[14] && range[1] == data.stereo[15] &&
          range[5] == data.stereo[19] && range[6] == 0.0f && range[7] == 0.0f,
          "imported range copy is bit-exact and zero-fills past the end");
    pool.copySamples(*imported, 12U, range);
    check(std::all_of(range.begin(), range.end(), [](float v) { return v == 0.0f; }),
          "a range past the end is silent");

    auto* capture = pool.acquireCapture();
    check(capture != nullptr && pool.ensureCaptureBlock(*capture, 0U) &&
          pool.ensureCaptureBlock(*capture, 1U), "map two capture blocks");
    constexpr auto kBlock = midichopper::SampleStoragePool::kBlockFrames;
    for (std::uint32_t frame = kBlock - 2U; frame < kBlock + 2U; ++frame)
        pool.writeCapture(*capture, frame, static_cast<float>(frame), -static_cast<float>(frame));
    capture->frames = kBlock + 2U;
    std::array<float, 8> crossing{};
    pool.copySamples(*capture, kBlock - 2U, crossing);
    check(crossing[0] == static_cast<float>(kBlock - 2U) &&
          crossing[5] == -static_cast<float>(kBlock) &&
          crossing[6] == static_cast<float>(kBlock + 1U),
          "capture range copy crosses a block boundary");
    capture->references.fetch_sub(1U);
    pool.collectImported();
}

void fingerprint_identity()
{
    const auto data = ramp(5000U, 0.1f, 44100.0);
    auto copy = data;
    check(midichopper::fingerprint(data) == midichopper::fingerprint(copy),
          "identical audio has one fingerprint");
    copy.stereo[0] = std::nextafter(copy.stereo[0], 1.0f);
    check(midichopper::fingerprint(data) != midichopper::fingerprint(copy),
          "one changed sampled bit changes the fingerprint");
    copy = data;
    copy.sampleRate = 48000.0;
    check(midichopper::fingerprint(data) != midichopper::fingerprint(copy),
          "the rate is part of the fingerprint");
}

void provenance_follows_pcm()
{
    SamplerEngine engine(1000.0, 1.0);
    const auto a = ramp(8U, 0.1f);
    constexpr std::uint64_t tagA = (0x123456ULL << 16U) | 1U;
    check(engine.restorePad(1U, &a, tagA), "restore a linked pad");
    Links links{};
    Links generations{};
    engine.padProvenance(links);
    check(links[1] == tagA, "a pending restore exposes its link");
    check(engine.padAudioFingerprint(1U) == midichopper::fingerprint(a),
          "the pending fingerprint matches the restored data");
    processBlock(engine);
    engine.padProvenance(links, generations);
    check(links[1] == tagA && links[0] == 0U, "a published restore keeps its link");
    check(engine.padAudioFingerprint(1U) == midichopper::fingerprint(a),
          "the published fingerprint matches the restored data");

    sms::dsp::SamplePlaybackSettings playback;
    playback.start = 0.25f;
    engine.setPadPlaybackSettings(1U, playback);
    sms::dsp::SampleMixerSettings mixer;
    mixer.gainDecibels = -3.0f;
    engine.setPadMixerSettings(1U, mixer);
    Links later{};
    engine.padProvenance(links, later);
    check(links[1] == tagA && later[1] != generations[1],
          "settings edits keep the link and bump the generation");

    check(engine.collapsePadGap(0U, 16U, 0U), "collapse moves the linked pad left");
    engine.padProvenance(links);
    check(links[0] == tagA && links[1] == 0U, "a whole-pad move keeps the storage and its link");

    check(!engine.setPadProvenance(0U, 7U, midichopper::fingerprint(a) + 1U),
          "a fingerprint mismatch is refused");
    check(engine.setPadProvenance(0U, 0U, midichopper::fingerprint(a)), "a matching unlink");
    engine.padProvenance(links);
    check(links[0] == 0U, "provenance 0 unlinks");
    check(engine.setPadProvenance(0U, tagA, midichopper::fingerprint(a)), "relink");
    check(!engine.setPadProvenance(5U, tagA, midichopper::fingerprint(a)),
          "an empty pad cannot be linked");

    const auto b = ramp(6U, 0.3f);
    check(engine.importPad(0U, b), "import replaces linked audio");
    engine.padProvenance(links);
    check(links[0] == 0U, "imported audio starts unlinked");

    check(engine.restorePad(2U, &b, tagA + 1U) && engine.restorePad(3U, &b, tagA + 2U),
          "restore two more linked pads");
    processBlock(engine);
    midichopper::PadClipboard clipboard;
    check(clipboard.copyFrom(engine, 0U) && clipboard.pasteTo(engine, 2U), "paste over a link");
    engine.clearPad(3U);
    engine.padProvenance(links);
    check(links[2] == 0U && links[3] == 0U, "paste and clear drop links");

    check(engine.restorePad(4U, &b, tagA + 3U), "restore a pad to record over");
    processBlock(engine);
    auto settings = engine.settings();
    settings.armed = true;
    settings.monitorInput = false;
    engine.setSettings(settings);
    engine.selectCaptureTarget(4U);
    std::array<float, 8> input{0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    std::array<float, 8> output{};
    const midichopper::MidiEvent on{0U, 40U, 100U, midichopper::MidiEventType::NoteOn};
    engine.process(input.data(), input.data(), output.data(), output.data(), 8U, &on, 1U);
    settings.armed = false;
    engine.setSettings(settings);
    engine.process(nullptr, nullptr, output.data(), output.data(), 0U);
    engine.padProvenance(links);
    check(engine.padMetadata(4U).occupied && links[4] == 0U, "recording drops the link");
}

} // namespace

int main()
{
    capture_descriptor_reuse_resets_link();
    ranged_copy_is_bit_exact();
    fingerprint_identity();
    provenance_follows_pcm();
    std::cout << "main sample core tests passed\n";
    return 0;
}
