#include "MainSampleSession.hpp"
#include "PadFingerprint.hpp"
#include "Parameters.hpp"
#include "StateCodec.hpp"
#include "StateKeys.hpp"
#include "src/DistrhoPluginInternal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace midichopper::plugin;
using DISTRHO::PluginExporter;
constexpr std::uint32_t kFrames = 64U;
using Audio = std::array<std::array<float, kFrames>, 2>;

void check(const bool condition, const char* const message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool sameBits(const float a, const float b)
{
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

Audio process(PluginExporter& plugin, const Audio& input, const std::uint32_t frames,
              const bool trigger = false)
{
    Audio output{};
    const float* inputs[]{input[0].data(), input[1].data()};
    float* outputs[]{output[0].data(), output[1].data()};
    DISTRHO::MidiEvent note{};
    note.size = 3U;
    note.data[0] = 0x90U;
    note.data[1] = midichopper::kDefaultBaseMidiNote;
    note.data[2] = 127U;
    plugin.run(inputs, outputs, frames, trigger ? &note : nullptr, trigger ? 1U : 0U);
    return output;
}

void checkNoMainSample(PluginExporter& plugin, const char* const message)
{
    check(plugin.getStateValue(kMainSampleAudioKey) == "" &&
          plugin.getStateValue(kMainSampleSessionKey) == "" &&
          plugin.getStateValue(kMainSampleLinksKey) == "", message);
}

struct SavedState {
    std::array<std::string, 3> values;
    Audio playback;
};
const std::array<const char*, 3> kKeys{
    kPadStateKeys[0].c_str(), kPadEditStateKeys[0].c_str(), kPadMixerStateKeys[0].c_str()};

SavedState captureAndSave()
{
    PluginExporter plugin(nullptr, nullptr, nullptr, nullptr);
    plugin.setParameterValue(kParameterInputMonitor, 0.0f);
    plugin.setParameterValue(kParameterMode, 1.0f);
    Audio input{};
    constexpr float samples[]{1.5f, -2.25f, 1.0e-8f, -1.0e-8f, 0.1f, -0.3f, 0.0f, -0.0f};
    for (std::uint32_t frame = 0U; frame < kFrames; ++frame) {
        input[0][frame] = samples[frame % 8U];
        input[1][frame] = samples[(frame + 3U) % 8U];
    }
    static_cast<void>(process(plugin, input, kFrames, true));
    plugin.setParameterValue(kParameterMode, 0.0f);
    static_cast<void>(process(plugin, {}, 0U)); // Publish the complete recording.

    sms::dsp::SamplePlaybackSettings edit;
    edit.start = 0.125f;
    edit.end = 0.875f;
    edit.attackSeconds = 0.0001f;
    edit.releaseSeconds = 0.0001f;
    const auto encodedEdit = encodePlaybackSettings(edit);
    plugin.setState(kKeys[1], encodedEdit.c_str());
    sms::dsp::SampleMixerSettings mixer;
    mixer.gainDecibels = -6.0f;
    mixer.pan = 0.25f;
    mixer.tuneSemitones = 0.37f;
    const auto encodedMixer = encodeMixerSettings(mixer);
    plugin.setState(kKeys[2], encodedMixer.c_str());

    SavedState saved;
    for (std::size_t i = 0U; i < kKeys.size(); ++i)
        saved.values[i] = plugin.getStateValue(kKeys[i]).buffer();
    DecodedPadState decoded;
    check(decodePadState(saved.values[0].c_str(), decoded) && decoded.pad.frames == kFrames &&
          decoded.sourceSampleRate == 48000U, "adapter saves captured stereo PCM at its source rate");
    for (std::uint32_t frame = 0U; frame < kFrames; ++frame)
        for (std::uint32_t channel = 0U; channel < 2U; ++channel)
            check(sameBits(decoded.pad.stereo[frame * 2U + channel], input[channel][frame]),
                  "capture and host save preserve quiet audio, headroom, channel order and signed zero");
    check(saved.values[1] == encodedEdit && saved.values[2] == encodedMixer,
          "host save includes desired region, ADSR and mixer settings before another callback");
    saved.playback = process(plugin, {}, kFrames, true);
    bool audible = false;
    for (const float sample : saved.playback[0]) audible = audible || sample != 0.0f;
    check(audible, "reference playback contains the saved recording");
    plugin.deactivateIfNeeded();
    return saved;
}

void restoreAndCompare(const SavedState& saved, const bool active)
{
    PluginExporter plugin(nullptr, nullptr, nullptr, nullptr);
    plugin.setParameterValue(kParameterInputMonitor, 0.0f);
    if (active) plugin.activate(); // An active host can suspend callbacks while restoring.
    for (std::size_t i = 0U; i < kKeys.size(); ++i)
        plugin.setState(kKeys[i], saved.values[i].c_str());
    for (std::size_t i = 0U; i < kKeys.size(); ++i)
        check(plugin.getStateValue(kKeys[i]) == saved.values[i].c_str(),
              "host can read restored PCM and settings before processing resumes");
    const auto output = process(plugin, {}, kFrames, true);
    for (std::uint32_t frame = 0U; frame < kFrames; ++frame)
        for (std::uint32_t channel = 0U; channel < 2U; ++channel)
            check(sameBits(output[channel][frame], saved.playback[channel][frame]),
                  "recreated adapter plays the saved edited recording identically");
    check(plugin.getStateValue(kKeys[0]) == saved.values[0].c_str(),
          "processing leaves the restored source audio unchanged");
    checkNoMainSample(plugin, "a project without main-sample keys restores no main sample");
    plugin.deactivateIfNeeded();
}

void legacyRestore()
{
    // Literal released PCM16 state; its decoded samples must retain the old scaling.
    constexpr char legacy[] = "U1NQMQEAAgCAuwAAAgAAAAgAAAAy9vZ8AEAA4P9/AYA=";
    constexpr float expected[]{16384.0f / 32767.0f, -8192.0f / 32767.0f, 1.0f, -1.0f};
    PluginExporter plugin(nullptr, nullptr, nullptr, nullptr);
    plugin.setParameterValue(kParameterInputMonitor, 0.0f);
    plugin.setState(kKeys[0], legacy);
    const auto upgraded = plugin.getStateValue(kKeys[0]);
    DecodedPadState decoded;
    check(decodePadState(upgraded.buffer(), decoded) && decoded.pad.stereo.size() == 4U,
          "adapter reads a released PCM16 project and saves float32 state");
    for (std::size_t i = 0U; i < 4U; ++i)
        check(sameBits(decoded.pad.stereo[i], expected[i]), "upgrading preserves released PCM16 sample values");
    const auto output = process(plugin, {}, kFrames, true);
    for (std::uint32_t frame = 0U; frame < 2U; ++frame)
        for (std::uint32_t channel = 0U; channel < 2U; ++channel)
            check(sameBits(output[channel][frame], expected[frame * 2U + channel]),
                  "legacy project playback retains its released PCM16 amplitude");
    checkNoMainSample(plugin, "a released project restores no main sample");
    plugin.deactivateIfNeeded();
}

midichopper::PadData tone(const std::uint32_t frames, const float scale)
{
    midichopper::PadData data;
    data.sampleRate = 48000.0;
    data.frames = frames;
    data.stereo.resize(static_cast<std::size_t>(frames) * 2U);
    for (std::uint32_t frame = 0U; frame < frames; ++frame) {
        const float value = scale * static_cast<float>(static_cast<int>(frame % 97U) - 48) / 64.0f;
        data.stereo[frame * 2U] = value;
        data.stereo[frame * 2U + 1U] = -0.5f * value;
        data.peak = std::max(data.peak, std::abs(value));
    }
    return data;
}

midichopper::PadData range(const midichopper::PadData& source, const std::uint32_t first,
                           const std::uint32_t count)
{
    auto data = source;
    data.frames = count;
    data.stereo.assign(source.stereo.begin() + first * 2U,
                       source.stereo.begin() + (first + count) * 2U);
    return data;
}

using KeyValues = std::vector<std::pair<std::string, std::string>>;
struct MainSampleProject {
    KeyValues values; // pads 1..3, then audio, session, links (index order)
    std::string links; // what a save writes back: the two matching links
};

MainSampleProject mainSampleProject()
{
    const auto source = tone(4800U, 0.9f);
    MainSampleSession session;
    session.reset(0xabc123U, 4800U, 48000U, "loop.wav");
    MainSampleSlice first;
    MainSampleSlice second;
    check(session.addSlice(100U, 1100U, first) && session.addSlice(2000U, 2600U, second),
          "fixture slices");
    const auto padA = range(source, 100U, 1000U);
    const auto padB = range(source, 2000U, 600U);
    const auto other = tone(500U, 0.3f);
    const auto id = session.sourceId;
    const std::vector<MainSampleLinkRecord> links{
        {0U, (id << 16U) | first.id, 1000U, midichopper::fingerprint(padA)},
        {1U, (id << 16U) | second.id, 600U, midichopper::fingerprint(padB)},
        // The pad was edited after the link was saved: its fingerprint differs.
        {2U, (id << 16U) | first.id, 500U, midichopper::fingerprint(other) ^ 1U},
    };
    MainSampleProject project;
    project.values = {
        {kPadStateKeys[0], encodePadState(padA, 48000U)},
        {kPadStateKeys[1], encodePadState(padB, 48000U)},
        {kPadStateKeys[2], encodePadState(other, 48000U)},
        {kMainSampleAudioKey, encodePadState(source, 48000U)},
        {kMainSampleSessionKey, encodeMainSampleSession(session)},
        {kMainSampleLinksKey, encodeMainSampleLinks(links)},
    };
    project.links = encodeMainSampleLinks(std::span(links).first(2U));
    return project;
}

void restoreMainSample(const MainSampleProject& project, const KeyValues& order,
                       const bool active, const char* const name)
{
    PluginExporter plugin(nullptr, nullptr, nullptr, nullptr);
    plugin.setParameterValue(kParameterInputMonitor, 0.0f);
    if (active) plugin.activate(); // Callbacks suspended: the source is queued, not dispatched.
    for (const auto& [key, value] : order)
        plugin.setState(key.c_str(), value.c_str());
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& [key, value] : project.values) {
            if (key == kMainSampleLinksKey) continue;
            if (plugin.getStateValue(key.c_str()) != value.c_str()) {
                std::cerr << name << ": " << key << '\n';
                check(false, "main-sample source, session and pads restore in any key order");
            }
        }
        if (plugin.getStateValue(kMainSampleLinksKey) != project.links.c_str()) {
            std::cerr << name << '\n';
            check(false, "matching links restore in any key order; an edited pad drops its link");
        }
        static_cast<void>(process(plugin, {}, kFrames)); // Install queued state; saves stay equal.
    }
    plugin.deactivateIfNeeded();
}

void mainSampleKeyOrders()
{
    const auto project = mainSampleProject();
    auto alphabetical = project.values; // VST3 and CLAP: main_* before pad_*
    std::sort(alphabetical.begin(), alphabetical.end());
    check(alphabetical.front().first == kMainSampleAudioKey, "alphabetical fixture order");
    restoreMainSample(project, alphabetical, false, "alphabetical");
    restoreMainSample(project, alphabetical, true, "alphabetical, callbacks suspended");
    restoreMainSample(project, project.values, false, "index order"); // LV2: pads first
    const auto& v = project.values;
    restoreMainSample(project, {v[1], v[5], v[0], v[4], v[2], v[3]}, true, "shuffled");
}

void mainSampleClearAndMismatch()
{
    const auto project = mainSampleProject();
    PluginExporter plugin(nullptr, nullptr, nullptr, nullptr);
    plugin.setParameterValue(kParameterInputMonitor, 0.0f);
    for (const auto& [key, value] : project.values)
        plugin.setState(key.c_str(), value.c_str());
    check(plugin.getStateValue(kMainSampleSessionKey) != "", "fixture restores a main sample");
    plugin.setState(kMainSampleAudioKey, "");
    checkNoMainSample(plugin, "an empty durable value clears a stale main sample and its links");
    check(plugin.getStateValue(kPadStateKeys[0].c_str()) == project.values[0].second.c_str(),
          "clearing the main sample keeps pad audio");

    // A session whose audio does not match it is never adopted.
    plugin.setState(kMainSampleSessionKey, project.values[4].second.c_str());
    const auto shorter = encodePadState(tone(4000U, 0.9f), 48000U);
    plugin.setState(kMainSampleAudioKey, shorter.c_str());
    checkNoMainSample(plugin, "mismatched frames keep the main sample empty");
    plugin.setState(kMainSampleSessionKey, "MSS1;broken");
    plugin.setState(kMainSampleAudioKey, project.values[3].second.c_str());
    checkNoMainSample(plugin, "an invalid session is ignored");

    // The save cache is seeded by the restored text, so a save does not
    // re-encode: a PCM16 (version 1) blob comes back unchanged, not as float32.
    constexpr char legacy[] = "U1NQMQEAAgCAuwAAAgAAAAgAAAAy9vZ8AEAA4P9/AYA=";
    MainSampleSession session;
    session.reset(0x51U, 2U, 48000U, "short.wav");
    const auto encodedSession = encodeMainSampleSession(session);
    plugin.setState(kMainSampleAudioKey, legacy); // replaces the staged unmatched audio
    plugin.setState(kMainSampleSessionKey, encodedSession.c_str());
    check(plugin.getStateValue(kMainSampleAudioKey) == legacy &&
          plugin.getStateValue(kMainSampleSessionKey) == encodedSession.c_str(),
          "a save reuses the cached Base64 of the restored source");
    plugin.setState(kMainSampleSessionKey, "");
    checkNoMainSample(plugin, "an empty session clears the main sample");
    plugin.deactivateIfNeeded();
}
} // namespace

int main()
{
    DISTRHO::d_nextBufferSize = kFrames;
    DISTRHO::d_nextSampleRate = 48000.0;
    // Suppress user preferences during construction; state and run use the real adapter/engine.
    DISTRHO::d_nextPluginIsDummy = true;
    const auto saved = captureAndSave();
    restoreAndCompare(saved, false);
    restoreAndCompare(saved, true);
    legacyRestore();
    mainSampleKeyOrders();
    mainSampleClearAndMismatch();
    DISTRHO::d_nextPluginIsDummy = false;
    std::cout << "Project state tests passed\n";
}
