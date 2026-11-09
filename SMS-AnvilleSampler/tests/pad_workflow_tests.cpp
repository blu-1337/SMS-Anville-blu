#include "PadWorkflows.hpp"
#include "ChopEditorProtocol.hpp"
#include "MainSampleWorkflows.hpp"
#include "PadFileActions.hpp"
#include "PadStructureProtocol.hpp"
#include "StateCodec.hpp"
#include "StateKeys.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace midichopper;
using namespace midichopper::plugin;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct Observer final : PadWorkflowObserver {
    std::vector<std::pair<std::string, std::string>> messages;
    PadClipboardResultCode clipboardResult = PadClipboardResultCode::none;
    PadFileResultCode fileResult = PadFileResultCode::none;
    bool available = false;
    void publishUiState(const char* key, const char* value) override
    { messages.emplace_back(key, value); }
    void publishFileResult(PadFileResultCode result) noexcept override { fileResult = result; }
    void publishClipboardResult(PadClipboardResultCode result) noexcept override
    { clipboardResult = result; }
    void clipboardAvailable(const bool value) noexcept override { available = value; }
    std::string latest(const char* key) const
    {
        for (auto i = messages.rbegin(); i != messages.rend(); ++i)
            if (i->first == key) return i->second;
        return {};
    }
};

PadData sample()
{
    PadData result;
    result.frames = 8;
    result.peak = 0.5f;
    result.stereo = {0.1f,-0.1f,0.2f,-0.2f,0.3f,-0.3f,0.4f,-0.4f,
                     0.5f,-0.5f,0.4f,-0.4f,0.3f,-0.3f,0.2f,-0.2f};
    return result;
}

bool samePlayback(const sms::dsp::SamplePlaybackSettings& a,
                  const sms::dsp::SamplePlaybackSettings& b)
{
    return a.start == b.start && a.end == b.end && a.attackSeconds == b.attackSeconds &&
        a.decaySeconds == b.decaySeconds && a.sustainLevel == b.sustainLevel &&
        a.releaseSeconds == b.releaseSeconds;
}

bool sameMixer(const sms::dsp::SampleMixerSettings& a,
               const sms::dsp::SampleMixerSettings& b)
{
    return a.gainDecibels == b.gainDecibels && a.pan == b.pan &&
        a.tuneSemitones == b.tuneSemitones && a.lowpass == b.lowpass &&
        a.highpass == b.highpass && a.filterSlope == b.filterSlope && a.dirty == b.dirty &&
        a.reverse == b.reverse && a.globalMixer == b.globalMixer;
}

void checkSettingsReply(const Observer& observer, const SamplerEngine& engine,
                        const std::uint32_t pad,
                        const sms::dsp::SamplePlaybackSettings& expectedPlayback,
                        const sms::dsp::SampleMixerSettings& expectedMixer,
                        const char* message)
{
    sms::dsp::SamplePlaybackSettings playbackReply;
    sms::dsp::SampleMixerSettings mixerReply;
    const auto playbackText = observer.latest(kPadEditStateKeys[pad].c_str());
    const auto mixerText = observer.latest(kPadMixerStateKeys[pad].c_str());
    check(!playbackText.empty() && !mixerText.empty() &&
          decodePlaybackSettings(playbackText.c_str(), playbackReply) &&
          decodeMixerSettings(mixerText.c_str(), mixerReply) &&
          samePlayback(playbackReply, expectedPlayback) &&
          sameMixer(mixerReply, expectedMixer) &&
          samePlayback(engine.padPlaybackSettings(pad), expectedPlayback) &&
          sameMixer(engine.padMixerSettings(pad), expectedMixer), message);
}

struct PadSnapshot {
    PadData audio;
    sms::dsp::SamplePlaybackSettings playback;
    sms::dsp::SampleMixerSettings mixer;
};

PadSnapshot snapshotPad(const SamplerEngine& engine, const std::uint32_t pad)
{
    PadSnapshot result;
    check(engine.exportPublishedPad(pad, result.audio, &result.playback, &result.mixer),
          "snapshot published pad");
    return result;
}

bool sameSnapshot(const PadSnapshot& a, const PadSnapshot& b)
{
    return a.audio.frames == b.audio.frames && a.audio.sampleRate == b.audio.sampleRate &&
        a.audio.peak == b.audio.peak && a.audio.rms == b.audio.rms &&
        a.audio.generation == b.audio.generation && a.audio.stereo == b.audio.stereo &&
        samePlayback(a.playback, b.playback) && sameMixer(a.mixer, b.mixer);
}

template <std::size_t Count>
std::array<PadSnapshot, Count> snapshotRange(const SamplerEngine& engine,
                                            const std::uint32_t firstPad)
{
    std::array<PadSnapshot, Count> result;
    for (std::size_t i = 0; i < Count; ++i)
        result[i] = snapshotPad(engine, firstPad + static_cast<std::uint32_t>(i));
    return result;
}

template <std::size_t Count>
bool sameSnapshots(const std::array<PadSnapshot, Count>& a,
                   const std::array<PadSnapshot, Count>& b)
{
    for (std::size_t i = 0; i < Count; ++i)
        if (!sameSnapshot(a[i], b[i])) return false;
    return true;
}

void clipboardSnapshotAndReplies()
{
    SamplerEngine engine(48000, 0.001);
    sms::audio::RealtimeCommandDispatcher dispatcher;
    engine.setControlDispatcher(&dispatcher,
        &sms::audio::RealtimeCommandDispatcher::dispatchFromEngine);
    Observer observer;
    PadWorkflows workflows(engine, dispatcher, observer);
    const auto original = sample();
    check(engine.importPad(0, original), "seed clipboard source");
    auto playback = engine.padPlaybackSettings(0);
    playback.start = 0.25f;
    engine.setPadPlaybackSettings(0, playback);
    auto mixer = engine.padMixerSettings(0);
    mixer.gainDecibels = -6;
    mixer.globalMixer = 0.0f;
    engine.setPadMixerSettings(0, mixer);
    workflows.handlePadClipboardRequest(encodePadClipboardRequest(PadClipboardAction::copy, 0));
    check(observer.available && observer.clipboardResult == PadClipboardResultCode::copied,
          "copy publishes availability and completion");
    engine.clearPad(0);
    workflows.handlePadClipboardRequest(encodePadClipboardRequest(PadClipboardAction::paste, 1));
    PadData restored;
    sms::dsp::SamplePlaybackSettings actualPlayback;
    sms::dsp::SampleMixerSettings actualMixer;
    check(engine.exportPublishedPad(1, restored, &actualPlayback, &actualMixer) &&
          restored.stereo == original.stereo && actualPlayback.start == playback.start &&
          sameMixer(actualMixer, mixer),
          "paste retains copy-time PCM and settings after source clear");
    check(observer.clipboardResult == PadClipboardResultCode::pasted &&
          !observer.latest(kWaveformDataKey).empty() &&
          !observer.latest(kPadEditStateKeys[1].c_str()).empty() &&
          !observer.latest(kPadMixerStateKeys[1].c_str()).empty(),
          "paste publishes waveform, both settings, and completion");
    workflows.handlePadClipboardRequest("invalid");
    check(observer.clipboardResult == PadClipboardResultCode::failed,
          "invalid clipboard action reports failure");
    workflows.handlePadFileRequest("invalid");
    check(observer.fileResult == PadFileResultCode::failed &&
          observer.latest(kPadFileStatusKey) == "Invalid pad file request",
          "invalid WAV action reports failure to adapter");
}

void resetClearsPadsAndClipboard()
{
    SamplerEngine engine(48000, 0.001);
    sms::audio::RealtimeCommandDispatcher dispatcher;
    engine.setControlDispatcher(&dispatcher,
        &sms::audio::RealtimeCommandDispatcher::dispatchFromEngine);
    Observer observer;
    PadWorkflows workflows(engine, dispatcher, observer);
    for (std::uint32_t pad = 0; pad < kPadCount; ++pad) {
        check(engine.importPad(pad, sample()), "seed every pad before reset");
        auto playback = engine.padPlaybackSettings(pad);
        playback.start = 0.25f;
        playback.releaseSeconds = 0.2f;
        engine.setPadPlaybackSettings(pad, playback);
        auto mixer = engine.padMixerSettings(pad);
        mixer.gainDecibels = -6;
        mixer.reverse = 1;
        mixer.globalMixer = 0;
        engine.setPadMixerSettings(pad, mixer);
    }
    workflows.handlePadClipboardRequest(encodePadClipboardRequest(PadClipboardAction::copy, 0));
    check(observer.available, "seed clipboard before reset");
    engine.clearPad(1);
    workflows.handlePadStructureRequest(encodePadStructureRequest(
        {PadStructureAction::prepareSplit, 0, 16, 0}));
    SplitPlanReady plan;
    check(decodeSplitPlanReady(observer.latest(kPadStructureStatusKey), plan),
          "prepare split plan before reset");
    engine.auditionPad(0, true);
    check(engine.anyPlaybackActive(), "reset begins with active playback");
    workflows.reset();
    check(!observer.available && !engine.anyPlaybackActive(), "reset stops playback and clears clipboard");
    for (std::uint32_t pad = 0; pad < kPadCount; ++pad) {
        const auto snapshot = snapshotPad(engine, pad);
        check(snapshot.audio.frames == 0 && samePlayback(snapshot.playback, {}) &&
              sameMixer(snapshot.mixer, {}), "reset empties all banks and restores every pad setting");
    }
    workflows.handlePadClipboardRequest(encodePadClipboardRequest(PadClipboardAction::paste, 0));
    check(observer.clipboardResult == PadClipboardResultCode::failed &&
          snapshotPad(engine, 0).audio.frames == 0, "old clipboard audio cannot return after reset");
    workflows.handlePadStructureRequest(encodePadStructureRequest(
        {PadStructureAction::applySplit, 0, 16, 0, plan.planId, 4}));
    check(snapshotPad(engine, 0).audio.frames == 0 &&
          observer.latest(kPadStructureStatusKey).starts_with("PS1;E;"),
          "reset invalidates an outstanding split plan");
}

void clipboardEmptyAndSamePadReplies()
{
    SamplerEngine engine(48000, 0.001);
    sms::audio::RealtimeCommandDispatcher dispatcher;
    engine.setControlDispatcher(&dispatcher,
        &sms::audio::RealtimeCommandDispatcher::dispatchFromEngine);
    Observer observer;
    PadWorkflows workflows(engine, dispatcher, observer);

    workflows.handlePadClipboardRequest(encodePadClipboardRequest(PadClipboardAction::paste, 2));
    check(observer.clipboardResult == PadClipboardResultCode::failed && !observer.available,
          "paste with an empty clipboard fails without advertising availability");
    check(engine.importPad(0, sample()), "seed pad for clipboard copy failure");
    auto sourcePlayback = engine.padPlaybackSettings(0);
    sourcePlayback.start = 0.2f;
    sourcePlayback.releaseSeconds = 0.15f;
    engine.setPadPlaybackSettings(0, sourcePlayback);
    auto sourceMixer = engine.padMixerSettings(0);
    sourceMixer.gainDecibels = -4.0f;
    sourceMixer.pan = 0.25f;
    sourceMixer.globalMixer = 0.0f;
    engine.setPadMixerSettings(0, sourceMixer);
    workflows.handlePadClipboardRequest(encodePadClipboardRequest(PadClipboardAction::copy, 0));
    check(observer.clipboardResult == PadClipboardResultCode::copied && observer.available,
          "occupied pad creates clipboard snapshot");
    engine.clearPad(0);
    workflows.handlePadClipboardRequest(encodePadClipboardRequest(PadClipboardAction::copy, 0));
    check(observer.clipboardResult == PadClipboardResultCode::failed,
          "copying an empty pad fails");
    workflows.handlePadClipboardRequest(encodePadClipboardRequest(PadClipboardAction::paste, 0));
    PadData restored;
    check(observer.clipboardResult == PadClipboardResultCode::pasted &&
          engine.exportPublishedPad(0, restored) && restored.stereo == sample().stereo,
          "failed empty-pad copy leaves clipboard snapshot available for same-pad paste");
    checkSettingsReply(observer, engine, 0, sourcePlayback, sourceMixer,
                       "same-pad paste publishes the complete playback and mixer settings");

    workflows.handlePadClipboardRequest(encodePadClipboardRequest(PadClipboardAction::paste, 16));
    PadData crossBank;
    check(observer.clipboardResult == PadClipboardResultCode::pasted &&
          engine.exportPublishedPad(16, crossBank) && crossBank.stereo == restored.stereo,
          "clipboard paste works across banks and publishes target settings");
    checkSettingsReply(observer, engine, 16, sourcePlayback, sourceMixer,
                       "cross-bank paste publishes the complete playback and mixer settings");
}

void splitPlanAndConflict()
{
    SamplerEngine engine(48000, 0.001);
    sms::audio::RealtimeCommandDispatcher dispatcher;
    engine.setControlDispatcher(&dispatcher,
        &sms::audio::RealtimeCommandDispatcher::dispatchFromEngine);
    Observer observer;
    PadWorkflows workflows(engine, dispatcher, observer);
    const auto original = sample();
    check(engine.importPad(0, original), "seed split source");
    const PadStructureRequest prepare{PadStructureAction::prepareSplit, 0, 16, 0};
    workflows.handlePadStructureRequest(encodePadStructureRequest(prepare));
    SplitPlanReady plan;
    check(decodeSplitPlanReady(observer.latest(kPadStructureStatusKey), plan),
          "prepare publishes a usable split plan");
    const PadStructureRequest apply{PadStructureAction::applySplit, 0, 0, 0, plan.planId, 4};
    workflows.handlePadStructureRequest(encodePadStructureRequest(apply));
    PadData prefix, suffix;
    check(engine.exportPublishedPad(0, prefix) && engine.exportPublishedPad(1, suffix) &&
          prefix.frames == 4 && suffix.frames == 4,
          "split workflow commits both halves");
    std::vector<float> joined = prefix.stereo;
    joined.insert(joined.end(), suffix.stereo.begin(), suffix.stereo.end());
    check(joined == original.stereo, "split workflow preserves stereo PCM order");
    const auto committedSplit = snapshotRange<2>(engine, 0);
    workflows.handlePadStructureRequest(encodePadStructureRequest(apply));
    check(observer.latest(kPadStructureStatusKey).starts_with("PS1;E;") &&
          sameSnapshots(committedSplit, snapshotRange<2>(engine, 0)),
          "successful split plan is consumed and cannot be replayed");
    workflows.handlePadStructureRequest(encodePadStructureRequest(prepare));
    check(decodeSplitPlanReady(observer.latest(kPadStructureStatusKey), plan),
          "prepare another split plan");
    auto mixer = engine.padMixerSettings(0);
    mixer.pan = 0.5f;
    engine.setPadMixerSettings(0, mixer);
    workflows.handlePadStructureRequest(encodePadStructureRequest(
        {PadStructureAction::applySplit, 0, 0, 0, plan.planId, 2}));
    check(observer.latest(kPadStructureStatusKey).starts_with("PS1;E;") &&
          engine.padMetadata(0).frames == 4 && engine.padMetadata(1).frames == 4,
          "settings change invalidates split without modifying PCM");
}

void splitPlanLifecycle()
{
    SamplerEngine engine(48000, 0.001);
    sms::audio::RealtimeCommandDispatcher dispatcher;
    engine.setControlDispatcher(&dispatcher,
        &sms::audio::RealtimeCommandDispatcher::dispatchFromEngine);
    Observer observer;
    PadWorkflows workflows(engine, dispatcher, observer);
    check(engine.importPad(0, sample()), "seed split plan lifecycle source");
    const PadStructureRequest prepare{PadStructureAction::prepareSplit, 0, 16, 0};
    workflows.handlePadStructureRequest(encodePadStructureRequest(prepare));
    SplitPlanReady replaced;
    check(decodeSplitPlanReady(observer.latest(kPadStructureStatusKey), replaced),
          "first split plan is prepared");
    workflows.handlePadStructureRequest(encodePadStructureRequest(prepare));
    SplitPlanReady active;
    check(decodeSplitPlanReady(observer.latest(kPadStructureStatusKey), active) &&
          active.planId != replaced.planId, "new preparation replaces the prior plan");
    const auto beforeRejectedApply = snapshotRange<2>(engine, 0);
    workflows.handlePadStructureRequest(encodePadStructureRequest(
        {PadStructureAction::applySplit, 0, 0, 0, replaced.planId, 2}));
    check(observer.latest(kPadStructureStatusKey).starts_with("PS1;E;") &&
          sameSnapshots(beforeRejectedApply, snapshotRange<2>(engine, 0)),
          "replaced split plan cannot be applied");
    workflows.handlePadStructureRequest(encodePadStructureRequest(
        {PadStructureAction::cancelSplit, 0, 0, 0, active.planId}));
    workflows.handlePadStructureRequest(encodePadStructureRequest(
        {PadStructureAction::applySplit, 0, 0, 0, active.planId, 2}));
    check(observer.latest(kPadStructureStatusKey).starts_with("PS1;E;") &&
          sameSnapshots(beforeRejectedApply, snapshotRange<2>(engine, 0)),
          "cancelled split plan cannot be applied");

    workflows.handlePadStructureRequest(encodePadStructureRequest(prepare));
    SplitPlanReady invalid;
    check(decodeSplitPlanReady(observer.latest(kPadStructureStatusKey), invalid),
          "prepare plan for invalid split frame");
    const auto beforeInvalidApply = snapshotRange<2>(engine, 0);
    workflows.handlePadStructureRequest(encodePadStructureRequest(
        {PadStructureAction::applySplit, 0, 0, 0, invalid.planId, sample().frames}));
    check(observer.latest(kPadStructureStatusKey).starts_with("PS1;E;") &&
          sameSnapshots(beforeInvalidApply, snapshotRange<2>(engine, 0)),
          "out-of-range split frame fails without changing pads");
    workflows.handlePadStructureRequest(encodePadStructureRequest(
        {PadStructureAction::applySplit, 0, 0, 0, invalid.planId, 2}));
    check(observer.latest(kPadStructureStatusKey).starts_with("PS1;E;") &&
          sameSnapshots(beforeInvalidApply, snapshotRange<2>(engine, 0)),
          "failed split plan is consumed and cannot be replayed");
}

void collapseGapPublishesMovedSettings()
{
    SamplerEngine engine(48000, 0.001);
    sms::audio::RealtimeCommandDispatcher dispatcher;
    engine.setControlDispatcher(&dispatcher,
        &sms::audio::RealtimeCommandDispatcher::dispatchFromEngine);
    Observer observer;
    PadWorkflows workflows(engine, dispatcher, observer);
    const auto movedAudio = sample();
    check(engine.importPad(0, sample()) && engine.importPad(1, movedAudio),
          "seed pads around a gap");
    auto movedPlayback = engine.padPlaybackSettings(1);
    movedPlayback.start = 0.3f;
    engine.setPadPlaybackSettings(1, movedPlayback);
    auto movedMixer = engine.padMixerSettings(1);
    movedMixer.pan = -0.4f;
    engine.setPadMixerSettings(1, movedMixer);
    engine.clearPad(0);

    workflows.handlePadStructureRequest(encodePadStructureRequest(
        {PadStructureAction::collapse, 0, 16, 0}));
    PadData collapsed;
    sms::dsp::SamplePlaybackSettings playback;
    sms::dsp::SampleMixerSettings mixer;
    check(engine.exportPublishedPad(0, collapsed, &playback, &mixer) &&
          collapsed.stereo == movedAudio.stereo && playback.start == movedPlayback.start &&
          mixer.pan == movedMixer.pan && !engine.padMetadata(1).occupied,
          "collapse moves following audio and settings into the cleared slot");
    check(observer.latest(kPadStructureStatusKey) == "PS1;O;0;C",
          "collapse publishes success and moved pad settings");
    checkSettingsReply(observer, engine, 0, movedPlayback, movedMixer,
                       "collapse publishes the complete moved playback and mixer settings");
}

void collapseAcrossBankBoundary()
{
    for (const auto layout : kPadsPerBankByLayout) {
        for (const auto mode : {MidiBankMode::SelectedBank, MidiBankMode::AllBanks}) {
            for (std::uint8_t bank = 0; bank < kBankCount; ++bank) {
                for (const auto leadingGap : {0U, 1U}) {
                    for (const bool fullRun : {false, true}) {
                        SamplerEngine engine(48000, 0.02);
                        auto settings = engine.settings();
                        settings.padsPerBank = layout;
                        settings.midiBankMode = mode;
                        settings.activeBank = bank;
                        engine.setSettings(settings);
                        sms::audio::RealtimeCommandDispatcher dispatcher;
                        engine.setControlDispatcher(&dispatcher,
                            &sms::audio::RealtimeCommandDispatcher::dispatchFromEngine);
                        Observer observer;
                        PadWorkflows workflows(engine, dispatcher, observer);
                        const auto first = bank * bankStride(layout, mode);
                        const auto target = first + layout - 1U;
                        const auto gapIndex = bank * layout + layout - 2U;
                        const auto runIndex = (bank + 1U) * layout + leadingGap;
                        const auto count = bank + 1U == kBankCount ? 0U :
                            (fullRun ? layout - leadingGap : 2U);
                        // Occupied hidden slots and later banks must stay untouched.
                        for (std::uint32_t pad = 0; pad < kPadCount; ++pad) {
                            const auto index = visiblePadIndex(pad, layout, mode);
                            if (index < kPadCount && (index == gapIndex || index == gapIndex + 1U ||
                                (bankForPad(pad, layout, mode) == bank + 1U &&
                                 (index < runIndex || index == runIndex + count))))
                                continue;
                            auto audio = sample();
                            audio.sampleRate = 32000.0 + pad * 100.0;
                            for (auto& value : audio.stereo) value *= 0.2f + pad * 0.01f;
                            check(engine.importPad(pad, audio), "seed boundary collapse PCM");
                            engine.setPadPlaybackSettings(pad, {0.1f, 0.9f, 0.01f, 0.02f, 0.6f, 0.03f});
                            auto mixer = engine.padMixerSettings(pad);
                            mixer.pan = -0.4f + pad * 0.01f;
                            mixer.reverse = 1.0f;
                            engine.setPadMixerSettings(pad, mixer);
                        }
                        const auto before = snapshotRange<kPadCount>(engine, 0);
                        workflows.handlePadStructureRequest(encodePadStructureRequest(
                            {PadStructureAction::queryCollapse, first, layout, target}));
                        check(observer.latest(kPadStructureStatusKey) ==
                              "PS1;G;" + std::to_string(target) + ";" + std::to_string(count) &&
                              sameSnapshots(before, snapshotRange<kPadCount>(engine, 0)),
                              "collapse query counts the following bank run without changing pads");
                        workflows.handlePadStructureRequest(encodePadStructureRequest(
                            {PadStructureAction::collapse, first, layout, target - 1U}));
                        check(observer.latest(kPadStructureStatusKey).starts_with("PS1;E;") &&
                              sameSnapshots(before, snapshotRange<kPadCount>(engine, 0)),
                              "an earlier slot in the same trailing gap cannot collapse across banks");
                        auto armed = settings;
                        armed.armed = true;
                        engine.setSettings(armed);
                        workflows.handlePadStructureRequest(encodePadStructureRequest(
                            {PadStructureAction::collapse, first, layout, target}));
                        check(observer.latest(kPadStructureStatusKey).starts_with("PS1;E;") &&
                              sameSnapshots(before, snapshotRange<kPadCount>(engine, 0)),
                              "armed collapse cannot mutate pads");
                        engine.setSettings(settings);
                        observer.messages.clear();
                        workflows.handlePadStructureRequest(encodePadStructureRequest(
                            {PadStructureAction::collapse, first, layout, target}));
                        if (count == 0U) {
                            check(observer.latest(kPadStructureStatusKey).starts_with("PS1;E;") &&
                                  sameSnapshots(before, snapshotRange<kPadCount>(engine, 0)),
                                  "Bank D trailing gap never wraps to Bank A");
                            continue;
                        }
                        check(observer.latest(kPadStructureStatusKey) == "PS1;O;0;C",
                              "last-slot collapse commits across the bank boundary");
                        const auto after = snapshotRange<kPadCount>(engine, 0);
                        for (std::uint32_t pad = 0; pad < kPadCount; ++pad) {
                            const auto index = visiblePadIndex(pad, layout, mode);
                            if (index >= gapIndex && index < gapIndex + count) {
                                const auto source = padForVisibleIndex(
                                    runIndex + index - gapIndex, layout, mode);
                                check(after[pad].audio.stereo == before[source].audio.stereo &&
                                      after[pad].audio.sampleRate == before[source].audio.sampleRate &&
                                      after[pad].audio.frames == before[source].audio.frames,
                                      "cross-bank collapse preserves exact ordered PCM and source rates");
                                checkSettingsReply(observer, engine, pad,
                                    before[source].playback, before[source].mixer,
                                    "cross-bank collapse publishes complete moved settings");
                            } else if (index >= gapIndex + count && index < runIndex + count) {
                                check(!engine.padMetadata(pad).occupied && after[pad].audio.frames == 0U,
                                      "cross-bank collapse clears every vacated slot");
                                checkSettingsReply(observer, engine, pad, {}, {},
                                    "cross-bank collapse publishes reset settings for vacated slots");
                            } else {
                                check(sameSnapshot(before[pad], after[pad]),
                                      "collapse preserves hidden slots, later runs and all other banks");
                            }
                        }
                    }
                }
            }
        }
    }
}

void collapseAvailabilityRevalidates()
{
    SamplerEngine engine(48000, 0.02);
    auto settings = engine.settings();
    settings.midiBankMode = MidiBankMode::SelectedBank;
    engine.setSettings(settings);
    sms::audio::RealtimeCommandDispatcher dispatcher;
    engine.setControlDispatcher(&dispatcher,
        &sms::audio::RealtimeCommandDispatcher::dispatchFromEngine);
    Observer observer;
    PadWorkflows workflows(engine, dispatcher, observer);
    for (std::uint32_t pad = 0; pad <= 32U; ++pad)
        if (pad != 1U) check(engine.importPad(pad, sample()), "seed adjacent full banks");
    const auto nextBank = snapshotRange<16>(engine, 16);
    workflows.handlePadStructureRequest(encodePadStructureRequest(
        {PadStructureAction::collapse, 0, 16, 1}));
    check(observer.latest(kPadStructureStatusKey) == "PS1;O;0;C" &&
          !engine.padMetadata(15).occupied && sameSnapshots(nextBank, snapshotRange<16>(engine, 16)),
          "ordinary collapse stops at its bank boundary even when the next bank is occupied");
    workflows.handlePadStructureRequest(encodePadStructureRequest(
        {PadStructureAction::queryCollapse, 0, 16, 15}));
    check(observer.latest(kPadStructureStatusKey) == "PS1;G;15;16",
          "last-slot query sees the full next bank");
    settings.activeBank = 1;
    engine.setSettings(settings);
    const auto beforeBankChange = snapshotRange<kPadCount>(engine, 0);
    workflows.handlePadStructureRequest(encodePadStructureRequest(
        {PadStructureAction::collapse, 0, 16, 15}));
    check(observer.latest(kPadStructureStatusKey).starts_with("PS1;E;") &&
          sameSnapshots(beforeBankChange, snapshotRange<kPadCount>(engine, 0)),
          "confirmation revalidates the active bank after an availability query");
    settings.activeBank = 0;
    engine.setSettings(settings);
    for (std::uint32_t pad = 16; pad < 32; ++pad) engine.clearPad(pad);
    const auto beforeClearedRun = snapshotRange<kPadCount>(engine, 0);
    workflows.handlePadStructureRequest(encodePadStructureRequest(
        {PadStructureAction::collapse, 0, 16, 15}));
    check(observer.latest(kPadStructureStatusKey).starts_with("PS1;E;") &&
          sameSnapshots(beforeClearedRun, snapshotRange<kPadCount>(engine, 0)),
          "a removed run invalidates collapse; an empty next bank cannot pull from Bank C");
}

void chopApplyRevisionAndReplies()
{
    SamplerEngine engine(48000, 0.001);
    sms::audio::RealtimeCommandDispatcher dispatcher;
    engine.setControlDispatcher(&dispatcher,
        &sms::audio::RealtimeCommandDispatcher::dispatchFromEngine);
    Observer observer;
    PadWorkflows workflows(engine, dispatcher, observer);
    const sms::dsp::SamplePlaybackSettings shaped{0.25f, 0.75f, 0.1f, 0.2f, 0.4f, 0.3f};
    for (std::uint32_t pad = 0; pad < 3; ++pad) {
        check(engine.importPad(pad, sample()), "seed chop apply range");
        engine.setPadPlaybackSettings(pad, shaped);
    }

    std::array<sms::audio::WaveformSummary, 3> summaries{};
    std::array<std::uint64_t, 3> generations{};
    check(engine.getChopSnapshot(0, 3, summaries, generations),
          "capture coherent chop baseline");
    ChopApplyRequest apply;
    apply.firstPad = 0;
    apply.padCount = 3;
    apply.sequence = 17;
    apply.revisionChecked = true;
    for (std::size_t i = 0; i < generations.size(); ++i)
        apply.expectedGenerations[i] = generations[i];
    apply.boundaryOffsets[0] = 1;
    std::vector<float> originalJoined;
    for (std::uint32_t pad = 0; pad < 3; ++pad) {
        PadData source;
        check(engine.exportPublishedPad(pad, source), "capture original chop PCM");
        originalJoined.insert(originalJoined.end(), source.stereo.begin(), source.stereo.end());
    }
    workflows.handleChopApplyRequest(encodeChopApplyRequest(apply));
    check(observer.latest(kChopStatusKey) == "CH2;17;OK" && engine.padMetadata(0).frames == 9 &&
          engine.padMetadata(1).frames == 7 && engine.padMetadata(2).frames == 8,
          "revision-checked chop apply commits the requested boundary");
    std::vector<float> choppedJoined;
    for (std::uint32_t pad = 0; pad < 3; ++pad) {
        PadData chopped;
        check(engine.exportPublishedPad(pad, chopped), "export chopped PCM");
        choppedJoined.insert(choppedJoined.end(), chopped.stereo.begin(), chopped.stereo.end());
        auto expected = shaped;
        if (pad == 0) expected = {2.0f / 9.0f, 6.0f / 9.0f};
        if (pad == 1) expected = {1.0f / 7.0f, 5.0f / 7.0f};
        checkSettingsReply(observer, engine, pad, expected,
                           engine.padMixerSettings(pad),
                           "successful chop publishes rebased markers and settings for every pad");
    }
    check(choppedJoined == originalJoined,
          "successful chop preserves concatenated source PCM exactly");

    check(engine.getChopSnapshot(0, 3, summaries, generations),
          "capture baseline for stale chop apply");
    apply.sequence = 18;
    for (std::size_t i = 0; i < generations.size(); ++i)
        apply.expectedGenerations[i] = generations[i];
    const auto replacement = sample();
    check(engine.importPad(1, replacement), "change chop range after baseline");
    const auto beforeConflict = snapshotRange<3>(engine, 0);
    workflows.handleChopApplyRequest(encodeChopApplyRequest(apply));
    check(observer.latest(kChopStatusKey).starts_with("CH2;18;ERROR;") &&
          sameSnapshots(beforeConflict, snapshotRange<3>(engine, 0)),
          "stale chop revision reports conflict and preserves replacement audio");

    check(engine.getChopSnapshot(0, 3, summaries, generations),
          "capture baseline for invalid chop boundary");
    apply.sequence = 19;
    for (std::size_t i = 0; i < generations.size(); ++i)
        apply.expectedGenerations[i] = generations[i];
    apply.boundaryOffsets[0] = -100;
    const auto beforeInvalidChop = snapshotRange<3>(engine, 0);
    workflows.handleChopApplyRequest(encodeChopApplyRequest(apply));
    check(observer.latest(kChopStatusKey).starts_with("CH2;19;ERROR;") &&
          sameSnapshots(beforeInvalidChop, snapshotRange<3>(engine, 0)),
          "invalid chop boundary reports failure without changing the pad range");

    apply.sequence = 20;
    apply.revisionChecked = false;
    const auto beforeLegacyChop = snapshotRange<3>(engine, 0);
    workflows.handleChopApplyRequest(encodeChopApplyRequest(apply));
    check(observer.latest(kChopStatusKey) == "CH2;0;ERROR;Reopen Adjust Cut Points" &&
          sameSnapshots(beforeLegacyChop, snapshotRange<3>(engine, 0)),
          "legacy chop apply without coherent revisions is rejected");
}

void crossBankWorkflowReplies()
{
    for (const auto layout : kPadsPerBankByLayout) {
        for (const auto mode : {MidiBankMode::SelectedBank, MidiBankMode::AllBanks}) {
            SamplerEngine engine(48000, 0.01);
            auto settings = engine.settings();
            settings.padsPerBank = layout;
            settings.midiBankMode = mode;
            engine.setSettings(settings);
            sms::audio::RealtimeCommandDispatcher dispatcher;
            engine.setControlDispatcher(&dispatcher,
                &sms::audio::RealtimeCommandDispatcher::dispatchFromEngine);
            Observer observer;
            PadWorkflows workflows(engine, dispatcher, observer);
            const auto first = padForVisibleIndex(layout - 2U, layout, mode);
            const auto second = advanceVisiblePad(first, 1U, layout, mode);
            const auto third = advanceVisiblePad(first, 2U, layout, mode);
            const auto empty = advanceVisiblePad(first, 3U, layout, mode);
            for (std::uint32_t pad = 0; pad < kPadCount; ++pad) {
                if (pad == first || pad == second || pad == third ||
                    visiblePadIndex(pad, layout, mode) == kPadCount) {
                    auto audio = sample();
                    for (auto& value : audio.stereo) value *= 0.25f + pad * 0.01f;
                    check(engine.importPad(pad, audio), "seed cross-bank workflow pads");
                    auto playback = engine.padPlaybackSettings(pad);
                    playback.attackSeconds = 0.05f + pad * 0.001f;
                    engine.setPadPlaybackSettings(pad, playback);
                    auto mixer = engine.padMixerSettings(pad);
                    mixer.pan = -0.5f + pad * 0.01f;
                    engine.setPadMixerSettings(pad, mixer);
                }
            }
            const auto before = snapshotRange<kPadCount>(engine, 0);
            const PadStructureRequest prepare{PadStructureAction::prepareSplit, 0, layout, first};
            workflows.handlePadStructureRequest(encodePadStructureRequest(prepare));
            SplitPlanReady plan;
            check(decodeSplitPlanReady(observer.latest(kPadStructureStatusKey), plan) &&
                  plan.targetPad == first && plan.emptyPad == empty,
                  "split plan finds nearest empty visible pad across a bank boundary");
            workflows.handlePadStructureRequest(encodePadStructureRequest(
                {PadStructureAction::querySplit, 0, layout, first}));
            check(observer.latest(kPadStructureStatusKey) ==
                  "PS1;A;" + std::to_string(first) + ";1",
                  "split availability sees empty pads in later banks");
            observer.messages.clear();
            workflows.handlePadStructureRequest(encodePadStructureRequest(
                {PadStructureAction::applySplit, 0, 0, 0, plan.planId, 4}));
            check(observer.latest(kPadStructureStatusKey) ==
                  "PS1;O;" + std::to_string(plan.planId) + ";S",
                  "availability query preserves pending cross-bank split plan");
            const auto prefix = snapshotPad(engine, first);
            const auto suffix = snapshotPad(engine, second);
            auto joined = prefix.audio.stereo;
            joined.insert(joined.end(), suffix.audio.stereo.begin(), suffix.audio.stereo.end());
            check(prefix.audio.frames == 4 && suffix.audio.frames == 4 &&
                  joined == before[first].audio.stereo,
                  "cross-bank split preserves both source halves exactly");
            checkSettingsReply(observer, engine, third, before[second].playback,
                before[second].mixer, "split publishes settings for mapped destination in next bank");
            checkSettingsReply(observer, engine, empty, before[third].playback,
                before[third].mixer, "split publishes settings through its final mapped destination");
            check(snapshotPad(engine, third).audio.stereo == before[second].audio.stereo &&
                  snapshotPad(engine, empty).audio.stereo == before[third].audio.stereo,
                  "cross-bank split shifts intervening visible audio in order");
            for (std::uint32_t pad = 0; pad < kPadCount; ++pad) {
                if (pad == first || pad == second || pad == third || pad == empty)
                    continue;
                check(sameSnapshot(before[pad], snapshotPad(engine, pad)),
                      "split preserves every unaffected or hidden storage slot");
                check(observer.latest(kPadEditStateKeys[pad].c_str()).empty() &&
                      observer.latest(kPadMixerStateKeys[pad].c_str()).empty(),
                      "split publishes settings only for mapped affected slots");
            }

            // Rechop crosses the same storage gap and must notify visible slots.
            std::array<sms::audio::WaveformSummary, 3> summaries{};
            std::array<std::uint64_t, 3> generations{};
            check(engine.getChopSnapshot(second, 3, summaries, generations),
                  "capture cross-bank chop revisions");
            ChopApplyRequest chop;
            chop.firstPad = second;
            chop.padCount = 3;
            chop.sequence = 31;
            chop.revisionChecked = true;
            chop.padsPerBank = layout;
            chop.midiBankMode = mode;
            chop.boundaryOffsets[0] = 1;
            for (std::size_t i = 0; i < generations.size(); ++i)
                chop.expectedGenerations[i] = generations[i];
            const auto beforeChop = snapshotRange<kPadCount>(engine, 0);
            auto changedMapping = settings;
            changedMapping.midiBankMode = mode == MidiBankMode::SelectedBank
                ? MidiBankMode::AllBanks : MidiBankMode::SelectedBank;
            engine.setSettings(changedMapping);
            workflows.handleChopApplyRequest(encodeChopApplyRequest(chop));
            check(observer.latest(kChopStatusKey).starts_with("CH2;31;ERROR;") &&
                  sameSnapshots(beforeChop, snapshotRange<kPadCount>(engine, 0)),
                  "cut save rejects a mapping change after its baseline without touching pads");
            engine.setSettings(settings);
            observer.messages.clear();
            workflows.handleChopApplyRequest(encodeChopApplyRequest(chop));
            check(observer.latest(kChopStatusKey) == "CH2;31;OK",
                  "cross-bank cut apply commits visible range");
            for (const auto pad : {second, third, empty})
                checkSettingsReply(observer, engine, pad, engine.padPlaybackSettings(pad),
                    engine.padMixerSettings(pad), "chop publishes every mapped destination setting");
            for (std::uint32_t pad = 0; pad < kPadCount; ++pad) {
                if (visiblePadIndex(pad, layout, mode) == kPadCount) {
                    check(sameSnapshot(before[pad], snapshotPad(engine, pad)) &&
                          observer.latest(kPadEditStateKeys[pad].c_str()).empty() &&
                          observer.latest(kPadMixerStateKeys[pad].c_str()).empty(),
                          "cross-bank chop preserves hidden audio/settings and skips hidden notifications");
                }
            }

            workflows.handlePadStructureRequest(encodePadStructureRequest(prepare));
            check(decodeSplitPlanReady(observer.latest(kPadStructureStatusKey), plan),
                  "prepare cross-bank split before mapping change");
            settings.midiBankMode = mode == MidiBankMode::SelectedBank
                ? MidiBankMode::AllBanks : MidiBankMode::SelectedBank;
            engine.setSettings(settings);
            const auto beforeChangedModeApply = snapshotRange<kPadCount>(engine, 0);
            workflows.handlePadStructureRequest(encodePadStructureRequest(
                {PadStructureAction::applySplit, 0, 0, 0, plan.planId, 2}));
            check(observer.latest(kPadStructureStatusKey).starts_with("PS1;E;") &&
                  sameSnapshots(beforeChangedModeApply, snapshotRange<kPadCount>(engine, 0)),
                  "mode change invalidates split plan even with unchanged active page first/count");

            settings.midiBankMode = mode;
            engine.setSettings(settings);
            workflows.handlePadStructureRequest(encodePadStructureRequest(prepare));
            check(decodeSplitPlanReady(observer.latest(kPadStructureStatusKey), plan),
                  "prepare split before layout change");
            settings.padsPerBank = layout == 8 ? 12 : 8;
            engine.setSettings(settings);
            const auto beforeLayoutApply = snapshotRange<kPadCount>(engine, 0);
            workflows.handlePadStructureRequest(encodePadStructureRequest(
                {PadStructureAction::applySplit, 0, 0, 0, plan.planId, 2}));
            check(observer.latest(kPadStructureStatusKey).starts_with("PS1;E;") &&
                  sameSnapshots(beforeLayoutApply, snapshotRange<kPadCount>(engine, 0)),
                  "layout change invalidates cross-bank split without changing storage");
        }
    }
}

void splitPlanCoversAllVisibleBanks()
{
    SamplerEngine engine(48000, 0.01);
    sms::audio::RealtimeCommandDispatcher dispatcher;
    engine.setControlDispatcher(&dispatcher,
        &sms::audio::RealtimeCommandDispatcher::dispatchFromEngine);
    Observer observer;
    PadWorkflows workflows(engine, dispatcher, observer);
    for (std::uint32_t pad = 0; pad < kPadCount - 1U; ++pad)
        check(engine.importPad(pad, sample()), "seed full split shift range");
    workflows.handlePadStructureRequest(encodePadStructureRequest(
        {PadStructureAction::prepareSplit, 0, 16, 0}));
    SplitPlanReady plan;
    check(decodeSplitPlanReady(observer.latest(kPadStructureStatusKey), plan) &&
          plan.emptyPad == kPadCount - 1U, "split plan captures all 64 visible generations");
    auto mixer = engine.padMixerSettings(kPadCount - 2U);
    mixer.pan = 0.5f;
    engine.setPadMixerSettings(kPadCount - 2U, mixer);
    const auto beforeStale = snapshotRange<kPadCount>(engine, 0);
    workflows.handlePadStructureRequest(encodePadStructureRequest(
        {PadStructureAction::applySplit, 0, 0, 0, plan.planId, 4}));
    check(observer.latest(kPadStructureStatusKey).starts_with("PS1;E;") &&
          sameSnapshots(beforeStale, snapshotRange<kPadCount>(engine, 0)),
          "changed destination in final bank rejects whole split atomically");
    workflows.handlePadStructureRequest(encodePadStructureRequest(
        {PadStructureAction::prepareSplit, 0, 16, 0}));
    check(decodeSplitPlanReady(observer.latest(kPadStructureStatusKey), plan),
          "prepare full-range split after refreshing revisions");
    workflows.handlePadStructureRequest(encodePadStructureRequest(
        {PadStructureAction::applySplit, 0, 0, 0, plan.planId, 4}));
    check(observer.latest(kPadStructureStatusKey) ==
          "PS1;O;" + std::to_string(plan.planId) + ";S" &&
          engine.padMetadata(kPadCount - 1U).occupied,
          "split can shift visible pads through the final bank");
    workflows.handlePadStructureRequest(encodePadStructureRequest(
        {PadStructureAction::querySplit, 0, 16, 0}));
    check(observer.latest(kPadStructureStatusKey) == "PS1;A;0;0",
          "split availability is false when no later visible pad is empty");
}


struct MainSampleFixture {
    SamplerEngine engine{48000, 0.001};
    sms::audio::RealtimeCommandDispatcher dispatcher;
    Observer observer;
    PadWorkflows pads{engine, dispatcher, observer};
    MainSampleWorkflows main{engine, dispatcher, observer, pads.mutationMutex()};
    std::uint64_t sequence = 0U;

    MainSampleFixture()
    {
        engine.setControlDispatcher(&dispatcher,
            &sms::audio::RealtimeCommandDispatcher::dispatchFromEngine);
    }

    MainSampleStatus send(MainSampleRequest request, const std::size_t expectedStatuses = 1U)
    {
        request.sequence = ++sequence;
        const auto before = statusCount();
        main.handleRequest(encodeMainSampleRequest(request));
        check(statusCount() - before == expectedStatuses, "one status per main-sample operation");
        return status();
    }

    [[nodiscard]] std::size_t statusCount() const
    {
        return static_cast<std::size_t>(std::count_if(observer.messages.begin(),
            observer.messages.end(), [](const auto& message) {
                return message.first == kMainSampleStatusKey;
            }));
    }

    [[nodiscard]] MainSampleStatus status() const
    {
        MainSampleStatus decoded;
        check(decodeMainSampleStatus(observer.latest(kMainSampleStatusKey), decoded),
              "main-sample status decodes");
        return decoded;
    }

    [[nodiscard]] std::uint64_t provenance(const std::uint32_t pad) const
    {
        std::array<std::uint64_t, kPadCount> tags{};
        engine.padProvenance(tags);
        return tags[pad];
    }
};

MainSampleRequest mainRequest(const MainSampleVerb verb)
{
    MainSampleRequest request;
    request.verb = verb;
    return request;
}

MainSampleRequest assignRequest(const std::uint32_t pad, const std::uint32_t start,
                                const std::uint32_t end, const bool confirm = false)
{
    auto request = mainRequest(MainSampleVerb::assign);
    request.pad = pad;
    request.start = start;
    request.end = end;
    request.confirm = confirm;
    return request;
}

MainSampleRequest editRequest(const MainSampleSlice first, const MainSampleSlice second = {})
{
    auto request = mainRequest(MainSampleVerb::edit);
    request.edits = {first, second};
    request.editCount = second.id == 0U ? 1U : 2U;
    return request;
}

std::string utf8(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

/** Source sample i is (i % 512) / 1024 left and its negation right: exact in any WAV format. */
float sourceSample(const std::uint32_t frame) { return static_cast<float>(frame % 512U) / 1024.0f; }

std::filesystem::path writeSourceWav(const char* name, const std::uint32_t frames)
{
    sms::audio::WavAudio audio;
    audio.sampleRate = 48000U;
    audio.frames = frames;
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        audio.stereo.push_back(sourceSample(frame));
        audio.stereo.push_back(-sourceSample(frame));
    }
    const auto path = std::filesystem::temp_directory_path() / name;
    check(writePadWav(path, audio).empty(), "write main-sample source WAV");
    return path;
}

bool padHoldsSlice(const SamplerEngine& engine, const std::uint32_t pad,
                   const std::uint32_t start, const std::uint32_t end)
{
    PadData audio;
    if (!engine.exportPublishedPad(pad, audio) || audio.frames != end - start)
        return false;
    for (std::uint32_t frame = 0; frame < audio.frames; ++frame) {
        if (audio.stereo[2U * frame] != sourceSample(start + frame) ||
            audio.stereo[2U * frame + 1U] != -sourceSample(start + frame))
            return false;
    }
    return true;
}

void mainSampleLoadQueryWaveformAndClear()
{
    MainSampleFixture fixture;
    auto status = fixture.send(mainRequest(MainSampleVerb::query));
    check(status.state == MainSampleState::empty && status.sourceId == 0U && status.sequence == 1U,
          "query reports an empty main sample");

    fixture.main.handleRequest("MS1;bad");
    status = fixture.status();
    check(status.state == MainSampleState::error && status.sequence == 0U &&
          status.text == "Invalid main sample request", "malformed request is reported");

    auto load = mainRequest(MainSampleVerb::load);
    load.path = utf8(std::filesystem::temp_directory_path() / "sms-missing-main-sample.wav");
    status = fixture.send(load, 2U);
    check(status.state == MainSampleState::error && fixture.main.session().empty() &&
          fixture.engine.sourceInfo().id == 0U, "unreadable WAV leaves no main sample");

    const auto path = writeSourceWav("sms-main-sample-workflow.wav", 1000U);
    load.path = utf8(path);
    status = fixture.send(load, 2U);
    MainSampleStatus loading;
    const auto loadingIndex = fixture.observer.messages.size() - 2U;
    check(fixture.observer.messages[loadingIndex].first == kMainSampleStatusKey &&
          decodeMainSampleStatus(fixture.observer.messages[loadingIndex].second, loading) &&
          loading.state == MainSampleState::loading &&
          loading.text == "sms-main-sample-workflow.wav", "load publishes LOADING first");
    const auto session = fixture.main.session();
    check(status.state == MainSampleState::ready && status.sourceId == session.sourceId &&
          status.sourceId == fixture.engine.sourceInfo().id && status.frames == 1000U &&
          status.sampleRate == 48000U && status.slices.empty() &&
          status.text == "sms-main-sample-workflow.wav", "load publishes READY with the source");

    auto waveform = mainRequest(MainSampleVerb::waveform);
    waveform.start = 100U;
    waveform.end = 900U;
    waveform.tiles = 2U;
    waveform.sequence = ++fixture.sequence;
    fixture.main.handleRequest(encodeMainSampleRequest(waveform));
    MainSampleWaveform expected;
    expected.sequence = waveform.sequence;
    expected.start = 100U;
    expected.end = 900U;
    expected.count = 2U;
    MainSampleWaveform reply;
    const auto published = fixture.observer.latest(kMainSampleWaveformKey);
    check(fixture.engine.summarizeSourceRange(status.sourceId, 100U, 900U,
              {expected.tiles.data(), 2U}) &&
          published == encodeMainSampleWaveform(expected) &&
          decodeMainSampleWaveform(published, reply) && reply.tiles[0].frames == 400U,
          "waveform tiles summarize the source");
    waveform.end = 1001U;
    status = fixture.send(waveform);
    check(status.state == MainSampleState::error && status.text == "Waveform unavailable" &&
          status.sourceId == session.sourceId, "out-of-range waveform is refused");

    status = fixture.send(load, 2U);
    check(status.state == MainSampleState::ready && status.sourceId != session.sourceId,
          "reloading assigns a new source id");

    status = fixture.send(mainRequest(MainSampleVerb::clear));
    check(status.state == MainSampleState::empty && fixture.main.session().empty() &&
          fixture.engine.sourceInfo().id == 0U, "clear removes the main sample");
    std::filesystem::remove(path);
}

void mainSampleAssignEditAndDelete()
{
    MainSampleFixture fixture;
    auto status = fixture.send(assignRequest(0U, 0U, 10U));
    check(status.state == MainSampleState::error && status.text == "Load a main sample first",
          "assign needs a main sample");
    const auto path = writeSourceWav("sms-main-sample-assign.wav", 1000U);
    auto load = mainRequest(MainSampleVerb::load);
    load.path = utf8(path);
    const auto sourceId = fixture.send(load, 2U).sourceId;

    status = fixture.send(assignRequest(0U, 100U, 300U));
    check(status.state == MainSampleState::ready && status.affectedMask == 1U &&
          status.slices.size() == 1U && status.links.size() == 1U &&
          status.links[0].pad == 0U && status.links[0].sliceId == status.slices[0].id &&
          padHoldsSlice(fixture.engine, 0U, 100U, 300U) &&
          fixture.provenance(0U) == ((sourceId << 16U) | status.slices[0].id),
          "assign cuts the slice onto an empty pad and links it");
    checkSettingsReply(fixture.observer, fixture.engine, 0U, {}, {},
                       "assign publishes default pad settings");
    const auto first = status.slices[0].id;

    auto playback = fixture.engine.padPlaybackSettings(0U);
    playback.start = 0.2f;
    playback.end = 0.8f;
    playback.attackSeconds = 0.1f;
    fixture.engine.setPadPlaybackSettings(0U, playback);
    auto mixer = fixture.engine.padMixerSettings(0U);
    mixer.gainDecibels = -6;
    fixture.engine.setPadMixerSettings(0U, mixer);
    const auto mixerReplies = std::count_if(fixture.observer.messages.begin(),
        fixture.observer.messages.end(), [](const auto& message) {
            return message.first == kPadMixerStateKeys[0];
        });
    status = fixture.send(editRequest({first, 150U, 350U}));
    playback.start = 0.0f;
    playback.end = 1.0f;
    check(status.state == MainSampleState::ready && status.affectedMask == 1U &&
          status.slices[0].start == 150U && status.slices[0].end == 350U &&
          padHoldsSlice(fixture.engine, 0U, 150U, 350U) &&
          fixture.provenance(0U) == ((sourceId << 16U) | first) &&
          samePlayback(fixture.engine.padPlaybackSettings(0U), playback) &&
          sameMixer(fixture.engine.padMixerSettings(0U), mixer),
          "moving a marker re-cuts the linked pad, resets Start/End and keeps envelope and mixer");
    sms::dsp::SamplePlaybackSettings playbackReply;
    check(decodePlaybackSettings(
              fixture.observer.latest(kPadEditStateKeys[0].c_str()).c_str(), playbackReply) && samePlayback(playbackReply, playback) &&
          std::count_if(fixture.observer.messages.begin(), fixture.observer.messages.end(),
              [](const auto& message) { return message.first == kPadMixerStateKeys[0]; }) ==
              mixerReplies, "a re-cut publishes the editor settings only");

    check(fixture.engine.importPad(1U, sample()), "seed a foreign pad");
    status = fixture.send(assignRequest(1U, 400U, 500U));
    check(status.state == MainSampleState::confirm && status.affectedMask == 2U &&
          status.slices.size() == 1U && snapshotPad(fixture.engine, 1U).audio.frames == 8U,
          "assigning over foreign audio asks first and changes nothing");
    status = fixture.send(assignRequest(1U, 400U, 500U, true));
    check(status.state == MainSampleState::ready && status.slices.size() == 2U &&
          status.links.size() == 2U && padHoldsSlice(fixture.engine, 1U, 400U, 500U),
          "a confirmed assign replaces foreign audio");
    const auto second = status.slices[1].id;

    status = fixture.send(assignRequest(0U, 0U, 50U));
    check(status.state == MainSampleState::ready && status.slices.size() == 3U &&
          padHoldsSlice(fixture.engine, 0U, 0U, 50U),
          "a pad linked to this source is reassigned without confirmation");
    const auto third = status.slices[0].id;
    check(third != first && status.slices[1].id == first, "slices stay sorted by start");

    status = fixture.send(editRequest({third, 0U, 60U}, {first, 60U, 350U}));
    check(status.state == MainSampleState::ready && status.affectedMask == 1U &&
          padHoldsSlice(fixture.engine, 0U, 0U, 60U),
          "a shared boundary edit re-cuts only linked slices");
    status = fixture.send(editRequest({999U, 0U, 10U}));
    check(status.state == MainSampleState::error && status.text == "Invalid slice edit" &&
          status.slices.size() == 3U, "an unknown slice is refused");

    auto settings = fixture.engine.settings();
    settings.armed = true;
    fixture.engine.setSettings(settings);
    status = fixture.send(editRequest({second, 450U, 550U}));
    check(status.state == MainSampleState::error && status.text == "Pads changed, try again" &&
          status.slices[2].start == 400U && padHoldsSlice(fixture.engine, 1U, 400U, 500U),
          "a refused re-cut keeps the session and the pad");
    status = fixture.send(assignRequest(2U, 0U, 10U));
    check(status.state == MainSampleState::error && status.text == "Disarm recording first",
          "assign is refused while armed");
    settings.armed = false;
    fixture.engine.setSettings(settings);

    auto remove = mainRequest(MainSampleVerb::remove);
    remove.sliceId = second;
    status = fixture.send(remove);
    check(status.state == MainSampleState::ready && status.affectedMask == 2U &&
          status.slices.size() == 2U && status.links.size() == 1U &&
          fixture.provenance(1U) == 0U && padHoldsSlice(fixture.engine, 1U, 400U, 500U),
          "delete unlinks the pad and keeps its audio");
    status = fixture.send(remove);
    check(status.state == MainSampleState::error && status.text == "Slice not found",
          "a deleted slice cannot be deleted twice");

    for (std::uint32_t index = 2U; index < kMainSampleMaxSlices; ++index)
        check(fixture.send(assignRequest(0U, index, index + 1U)).state == MainSampleState::ready,
              "fill every slice");
    status = fixture.send(assignRequest(0U, 900U, 901U));
    check(status.state == MainSampleState::error && status.text == "All 64 slices are in use" &&
          status.slices.size() == kMainSampleMaxSlices, "the 65th slice is refused");

    status = fixture.send(load, 2U);
    check(status.state == MainSampleState::ready && status.slices.empty() &&
          status.links.empty() && padHoldsSlice(fixture.engine, 0U, 63U, 64U),
          "a new source starts without slices; pads keep their audio");
    std::filesystem::remove(path);
}
}

int main()
{
    clipboardSnapshotAndReplies();
    resetClearsPadsAndClipboard();
    clipboardEmptyAndSamePadReplies();
    splitPlanAndConflict();
    splitPlanLifecycle();
    collapseGapPublishesMovedSettings();
    collapseAcrossBankBoundary();
    collapseAvailabilityRevalidates();
    chopApplyRevisionAndReplies();
    crossBankWorkflowReplies();
    splitPlanCoversAllVisibleBanks();
    mainSampleLoadQueryWaveformAndClear();
    mainSampleAssignEditAndDelete();
    std::cout << "Pad workflow tests passed\n";
}
