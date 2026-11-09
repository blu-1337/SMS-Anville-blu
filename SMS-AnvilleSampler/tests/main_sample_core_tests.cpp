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

bool samePcm(const SamplerEngine& engine, const std::uint32_t pad, const PadData& expected)
{
    PadData exported;
    return engine.exportPad(pad, exported) && exported.frames == expected.frames &&
           exported.stereo == expected.stereo;
}

void import_pads_transaction()
{
    SamplerEngine engine(1000.0, 1.0); // 64 storage blocks of 1024 frames
    const auto a = ramp(8U, 0.1f);
    const auto b = ramp(12U, 0.4f);
    sms::dsp::SamplePlaybackSettings trimmed;
    trimmed.start = 0.5f;
    sms::dsp::SampleMixerSettings louder;
    louder.gainDecibels = 3.0f;
    std::array<midichopper::PadImport, 2> pair{{
        {3U, &a, trimmed, louder, 0x10001U, {}},
        {9U, &b, {}, {}, 0x10002U, {}}}};
    check(engine.importPads(pair), "import two pads in one transaction");
    Links links{};
    Links generations{};
    engine.padProvenance(links, generations);
    check(links[3] == 0x10001U && links[9] == 0x10002U, "imports carry their links");
    check(samePcm(engine, 3U, a) && samePcm(engine, 9U, b), "imported PCM is exact");
    check(engine.padPlaybackSettings(3U).start == 0.5f &&
          engine.padMixerSettings(3U).gainDecibels == 3.0f, "imports take the supplied settings");

    // A stale generation anywhere fails everything.
    std::array<midichopper::PadImport, 2> stale{{
        {3U, &b, {}, {}, 0x20001U, generations[3]},
        {9U, &a, {}, {}, 0x20002U, generations[9] + 1U}}};
    check(!engine.importPads(stale), "a stale generation rejects the transaction");
    engine.padProvenance(links);
    check(links[3] == 0x10001U && samePcm(engine, 3U, a) && samePcm(engine, 9U, b),
          "a rejected transaction changes nothing");

    std::array<midichopper::PadImport, 2> duplicate{{{4U, &a}, {4U, &b}}};
    check(!engine.importPads(duplicate), "duplicate pads are rejected");
    check(!engine.importPads({}), "an empty transaction is rejected");

    const auto large = ramp(40000U, 0.0f); // 40 blocks each, 80 > 64 available
    std::array<midichopper::PadImport, 2> tooLarge{{{20U, &large}, {21U, &large}}};
    check(!engine.importPads(tooLarge), "a capacity shortfall is rejected");
    check(!engine.padMetadata(20U).occupied && !engine.padMetadata(21U).occupied,
          "a capacity failure publishes no pad");

    auto expected = engine.settings();
    expected.padsPerBank = 12U;
    std::array<midichopper::PadImport, 1> one{{{5U, &a}}};
    check(!engine.importPads(one, &expected), "a changed layout rejects the plan");
    auto settings = engine.settings();
    settings.armed = true;
    engine.setSettings(settings);
    check(!engine.importPads(one), "Arm rejects imports");
    settings.armed = false;
    engine.setSettings(settings);
    processBlock(engine);

    std::array<midichopper::PadImport, 2> clearAndKeep{{
        {3U, nullptr, trimmed, louder, 0x30001U, {}},
        {9U, &a, {}, {}, 0x30002U, {}}}};
    check(engine.importPads(clearAndKeep), "clear one pad and replace another");
    engine.padProvenance(links);
    check(!engine.padMetadata(3U).occupied && links[3] == 0U &&
          engine.padPlaybackSettings(3U).start == 0.0f, "nullptr clears and resets settings");
    check(links[9] == 0x30002U && samePcm(engine, 9U, a), "the other pad is replaced");
}

} // namespace

int main()
{
    capture_descriptor_reuse_resets_link();
    ranged_copy_is_bit_exact();
    fingerprint_identity();
    provenance_follows_pcm();
    import_pads_transaction();
    std::cout << "main sample core tests passed\n";
    return 0;
}
