#include "../src/plugin/PadColorState.hpp"
#include "PadWorkflows.hpp"
#include "ChopEditorProtocol.hpp"
#include "MainSampleWorkflows.hpp"
#include "PadFileActions.hpp"
#include "PadStructureProtocol.hpp"
#include "StateCodec.hpp"
#include "StateKeys.hpp"
#include "UndoController.hpp"
#include "SamplerUiSession.hpp"

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
    SamplerEngine engine;
    sms::audio::RealtimeCommandDispatcher dispatcher;
    Observer observer;
    PadWorkflows pads{engine, dispatcher, observer};
    MainSampleWorkflows main{engine, dispatcher, observer, pads.mutationMutex()};
    std::uint64_t sequence = 0U;

    explicit MainSampleFixture(const double poolSeconds = 0.001) : engine(48000, poolSeconds)
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

AutoChopStatus autoChop(MainSampleFixture& fixture, AutoChopRequest request)
{
    request.sequence = ++fixture.sequence;
    fixture.main.handleAutoChop(encodeAutoChopRequest(request));
    AutoChopStatus status;
    check(decodeAutoChopStatus(fixture.observer.latest(kAutoChopStatusKey), status) &&
          status.sequence == request.sequence, "auto-chop status decodes with its sequence");
    return status;
}

std::size_t autoChopStatusCount(const MainSampleFixture& fixture)
{
    return static_cast<std::size_t>(std::count_if(fixture.observer.messages.begin(),
        fixture.observer.messages.end(), [](const auto& message) {
            return message.first == kAutoChopStatusKey;
        }));
}

void autoChopWorkflow()
{
    MainSampleFixture fixture(12.0);
    AutoChopRequest equal;
    equal.mode = AutoChopMode::equal;
    equal.count = 4U;
    auto status = autoChop(fixture, equal);
    check(status.state == AutoChopState::error && status.text == "Load a main sample first",
          "auto chop needs a main sample");

    const auto path = writeSourceWav("sms-auto-chop-workflow.wav", 192000U);
    auto load = mainRequest(MainSampleVerb::load);
    load.path = utf8(path);
    static_cast<void>(fixture.send(load, 2U));
    const auto sourceId = fixture.main.session().sourceId;

    const auto mainBefore = fixture.statusCount();
    status = autoChop(fixture, equal);
    check(status.state == AutoChopState::ok && status.mask == 0xfU && status.found == 4U &&
          status.requested == 4U && fixture.statusCount() == mainBefore + 1U,
          "equal chop fills four pads with one status of each kind");
    auto session = fixture.main.session();
    check(session.slices.size() == 4U && fixture.main.analysisRuns() == 1U, "four slices, one analysis");
    PadData pad;
    for (std::uint32_t index = 0; index < 4U; ++index) {
        const auto tag = fixture.provenance(index);
        check((tag >> 16U) == sourceId && (tag & 0xffffU) == session.slices[index].id &&
              fixture.engine.exportPublishedPad(index, pad) && pad.frames == 48000U &&
              padHoldsSlice(fixture.engine, index, session.slices[index].start,
                            session.slices[index].end), "chopped pad carries its slice and tag");
    }
    const auto firstIds = session.slices[0].id;

    status = autoChop(fixture, equal);
    session = fixture.main.session();
    check(status.state == AutoChopState::ok && fixture.main.analysisRuns() == 1U &&
          session.slices.size() == 4U && session.slices[0].id != firstIds,
          "re-chopping own pads never asks, reuses the cache and issues new ids");

    // A pad holding other audio needs a confirmation.
    std::array<std::uint64_t, kPadCount> tags{};
    std::array<std::uint64_t, kPadCount> generations{};
    fixture.engine.padProvenance(tags, generations);
    auto foreign = sample();
    PadImport import;
    import.pad = 5U;
    import.audio = &foreign;
    import.expectedGeneration = generations[5];
    check(fixture.engine.importPads({&import, 1U}), "put foreign audio on pad 5");
    equal.firstPad = 4U;
    const auto before = fixture.statusCount();
    status = autoChop(fixture, equal);
    check(status.state == AutoChopState::confirm && status.mask == (1ULL << 5U) &&
          status.found == 1U && fixture.statusCount() == before &&
          fixture.provenance(5U) == 0U, "foreign audio asks before any change");
    equal.confirm = true;
    status = autoChop(fixture, equal);
    check(status.state == AutoChopState::ok && status.mask == 0xf0U &&
          (fixture.provenance(5U) >> 16U) == sourceId, "confirmed chop replaces the pad");
    check(fixture.provenance(0U) == 0U, "pads left out lose their stale tag");

    // Beat modes need a tempo.
    AutoChopRequest beat;
    beat.mode = AutoChopMode::beat;
    beat.count = 4U;
    beat.firstPad = 8U;
    status = autoChop(fixture, beat);
    check(status.state == AutoChopState::error && status.text == "No tempo", "beat mode without tempo is refused");
    std::atomic<double> host{120.0};
    fixture.main.setHostBpm(&host);
    status = autoChop(fixture, beat);
    check(status.state == AutoChopState::ok && fixture.engine.exportPublishedPad(8U, pad) &&
          pad.frames == 24000U, "host tempo gives half-second beat slices");
    beat.bpmCenti = 6000U;
    status = autoChop(fixture, beat);
    check(status.state == AutoChopState::ok && fixture.engine.exportPublishedPad(8U, pad) &&
          pad.frames == 48000U, "an explicit tempo overrides the host");

    // A reload invalidates the cache.
    static_cast<void>(fixture.send(load, 2U));
    equal.firstPad = 0U;
    status = autoChop(fixture, equal);
    check(status.state == AutoChopState::ok && fixture.main.analysisRuns() == 2U,
          "a new source is analysed again");

    fixture.main.handleAutoChop("AC1;bad");
    check(autoChopStatusCount(fixture) > 0U && fixture.observer.latest(kAutoChopStatusKey).find("ERROR") !=
          std::string::npos, "malformed auto-chop request is reported");
    std::filesystem::remove(path);

    // Eight slices land on pads 01..08 in number order.
    AutoChopRequest eight = equal;
    eight.count = 8U;
    eight.firstPad = 0U;
    status = autoChop(fixture, eight);
    session = fixture.main.session();
    check(status.state == AutoChopState::ok && status.mask == 0xffU && session.slices.size() == 8U,
          "eight slices fill pads 01..08");
    for (std::uint32_t index = 0; index < 8U; ++index)
        check((fixture.provenance(index) & 0xffffU) == session.slices[index].id,
              "slice N lands on the pad labelled N");
}

void mainSampleArrange()
{
    MainSampleFixture fixture;
    const auto path = writeSourceWav("sms-main-sample-arrange.wav", 1000U);
    auto load = mainRequest(MainSampleVerb::load);
    load.path = utf8(path);
    const auto sourceId = fixture.send(load, 2U).sourceId;
    check(fixture.send(assignRequest(0U, 600U, 800U)).state == MainSampleState::ready, "assign late slice");
    check(fixture.send(assignRequest(1U, 0U, 300U)).state == MainSampleState::ready, "assign early slice");
    check(fixture.send(assignRequest(3U, 300U, 600U)).state == MainSampleState::ready, "assign middle slice");
    auto mixer = fixture.engine.padMixerSettings(0U);
    mixer.gainDecibels = -9;
    fixture.engine.setPadMixerSettings(0U, mixer);
    const auto lateProvenance = fixture.provenance(0U);

    auto status = fixture.send(mainRequest(MainSampleVerb::arrange));
    check(status.state == MainSampleState::ready && status.affectedMask == 0xBU &&
          padHoldsSlice(fixture.engine, 0U, 0U, 300U) && padHoldsSlice(fixture.engine, 1U, 300U, 600U) &&
          padHoldsSlice(fixture.engine, 3U, 600U, 800U),
          "arrange moves the audio so pad order follows time order in one transaction");
    check(fixture.provenance(3U) == lateProvenance && (fixture.provenance(0U) >> 16U) == sourceId &&
          sameMixer(fixture.engine.padMixerSettings(3U), mixer),
          "links and mixer settings travel with the audio");
    status = fixture.send(mainRequest(MainSampleVerb::arrange));
    check(status.state == MainSampleState::ready && status.affectedMask == 0U, "a second arrange changes nothing");
}

void arrangeKeepsColours()
{
    MainSampleFixture fixture;
    std::array<int, kPadCount> colors{};
    fixture.main.setArrangeHook([&](const std::vector<ArrangeMove>& moves) {
        std::vector<int> values;
        for (const auto& move : moves) values.push_back(colors[move.from]);
        for (std::size_t i = 0; i < moves.size(); ++i) colors[moves[i].to] = values[i];
    });
    const auto path = writeSourceWav("sms-arrange-colours.wav", 1000U);
    auto load = mainRequest(MainSampleVerb::load);
    load.path = utf8(path);
    fixture.send(load, 2U);
    fixture.send(assignRequest(0U, 700U, 900U));
    fixture.send(assignRequest(1U, 0U, 200U));
    fixture.send(assignRequest(2U, 500U, 700U));
    fixture.send(assignRequest(3U, 200U, 500U));
    // AUTO COLOR keys the colour by slice: tag each pad with its slice id.
    std::array<int, kPadCount> bySlice{};
    for (std::uint32_t pad = 0; pad < 4U; ++pad) {
        colors[pad] = 1 + static_cast<int>(fixture.provenance(pad) & 0xFFFFU);
        bySlice[fixture.provenance(pad) & 0xFFFFU] = colors[pad];
    }
    for (int press = 0; press < 3; ++press) {
        fixture.send(mainRequest(MainSampleVerb::arrange));
        for (std::uint32_t pad = 0; pad < 4U; ++pad) {
            const auto slice = fixture.provenance(pad) & 0xFFFFU;
            check(colors[pad] == bySlice[slice], "each pad keeps its slice's colour after every arrange");
        }
    }
    check(padHoldsSlice(fixture.engine, 0U, 0U, 200U) && padHoldsSlice(fixture.engine, 3U, 700U, 900U),
          "the audio is in time order");
}

void zeroCrossSnap()
{
    MainSampleFixture fixture;
    constexpr std::uint32_t frames = 4000U;
    sms::audio::WavAudio audio;
    audio.sampleRate = 48000U;
    audio.frames = frames;
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const float value = 0.5f * std::sin(6.2831853f * static_cast<float>(frame) / 100.0f);
        audio.stereo.push_back(value);
        audio.stereo.push_back(value);
    }
    const auto path = std::filesystem::temp_directory_path() / "sms-zero-cross.wav";
    check(writePadWav(path, audio).empty(), "write sine source");
    auto load = mainRequest(MainSampleVerb::load);
    load.path = utf8(path);
    fixture.send(load, 2U);
    PadData source;
    check(fixture.engine.exportSource(source), "read the source back");
    const auto nearCrossing = [&](const std::uint32_t f) {
        const auto at = [&](const std::uint32_t i) { return source.stereo[2U * i]; };
        return (at(f - 1U) < 0.0f) != (at(f) < 0.0f) || (at(f) < 0.0f) != (at(f + 1U) < 0.0f) ||
               std::fabs(at(f)) < 0.02f;
    };
    const std::uint32_t radius = 144U; // 3 ms at 48 kHz

    auto snapped = assignRequest(0U, 1013U, 2037U);
    snapped.zeroCross = true;
    auto status = fixture.send(snapped);
    check(status.state == MainSampleState::ready && status.slices.size() == 1U, "snapped assign succeeds");
    const auto slice = status.slices.front();
    check(nearCrossing(slice.start) && nearCrossing(slice.end), "both edges land on a sign change");
    check(slice.start != 1013U && slice.end != 2037U, "the edges moved");
    check(std::abs(static_cast<int>(slice.start) - 1013) <= static_cast<int>(radius) &&
          std::abs(static_cast<int>(slice.end) - 2037) <= static_cast<int>(radius),
          "the edges never leave the window");

    // Bypass (flag off) leaves the edges exact.
    status = fixture.send(assignRequest(1U, 2513U, 3537U));
    check(status.slices.back().start == 2513U && status.slices.back().end == 3537U, "bypass keeps the exact frames");

    // A drag edit snaps only the edge it moved, and region snap wins over zero crossing.
    const auto second = status.slices.back();
    auto edit = editRequest({second.id, 2530U, second.end});
    edit.zeroCross = true;
    status = fixture.send(edit);
    const auto moved = std::find_if(status.slices.begin(), status.slices.end(),
        [&](const MainSampleSlice& s) { return s.id == second.id; });
    check(moved != status.slices.end() && moved->end == 3537U && nearCrossing(moved->start) &&
          std::abs(static_cast<int>(moved->start) - 2530) <= static_cast<int>(radius),
          "an edit snaps the moved edge only");
    edit = editRequest({second.id, slice.end, second.end}); // lands exactly on the other slice's end
    edit.zeroCross = true;
    status = fixture.send(edit);
    const auto onBoundary = std::find_if(status.slices.begin(), status.slices.end(),
        [&](const MainSampleSlice& s) { return s.id == second.id; });
    check(onBoundary != status.slices.end() && onBoundary->start == slice.end,
          "an edge on another slice boundary stays (region snap wins)");
}

float gainOf(const SamplerEngine& engine, const std::uint32_t pad)
{
    sms::dsp::SamplePlaybackSettings playback;
    sms::dsp::SampleMixerSettings mixer;
    engine.snapshotPadSettings(pad, playback, mixer);
    return mixer.gainDecibels;
}

void undoRedoRestoresState()
{
    MainSampleFixture fixture;
    std::array<std::atomic<int>, kPadCount> colors{};
    UndoController undo(fixture.engine, fixture.main, fixture.observer, colors);
    using Kind = UndoController::Kind;
    const auto now = UndoController::Clock::now();
    const auto path = writeSourceWav("sms-undo-source.wav", 1000U);
    auto load = mainRequest(MainSampleVerb::load);
    load.path = utf8(path);
    fixture.send(load, 2U);
    check(undo.undoDepth() == 0U, "nothing recorded yet");
    // Action: assign a slice to pad 2.
    undo.record(Kind::action, now, [&] { fixture.send(assignRequest(2U, 100U, 300U)); });
    check(undo.undoDepth() == 1U && padHoldsSlice(fixture.engine, 2U, 100U, 300U), "assign is one step");
    // A no-op request leaves no step.
    undo.record(Kind::action, now, [&] { fixture.send(mainRequest(MainSampleVerb::query)); });
    check(undo.undoDepth() == 1U, "queries leave no step");
    // Settings edits within the coalescing window are one step.
    auto mixer = fixture.engine.padMixerSettings(2U);
    mixer.gainDecibels = -6;
    undo.record(Kind::settings, now + std::chrono::seconds(5), [&] { fixture.engine.setPadMixerSettings(2U, mixer); });
    mixer.gainDecibels = -9;
    undo.record(Kind::settings, now + std::chrono::seconds(5) + std::chrono::milliseconds(100),
                [&] { fixture.engine.setPadMixerSettings(2U, mixer); });
    check(undo.undoDepth() == 2U, "a drag of settings is one step");
    // Colour edit.
    undo.record(Kind::colors, now + std::chrono::seconds(10), [&] { colors[2].store(3); });
    check(undo.undoDepth() == 3U, "colour is a step");
    undo.undo();
    check(colors[2].load() == 0 && undo.redoDepth() == 1U, "undo colour");
    undo.undo();
    check(gainOf(fixture.engine, 2U) == 0 && padHoldsSlice(fixture.engine, 2U, 100U, 300U),
          "undo settings keeps audio");
    undo.undo();
    check(!fixture.engine.padMetadata(2U).occupied && fixture.provenance(2U) == 0U &&
          fixture.main.session().slices.empty(), "undo assign clears pad and link");
    check(!fixture.main.session().empty(), "source stays loaded");
    undo.redo();
    check(padHoldsSlice(fixture.engine, 2U, 100U, 300U) && (fixture.provenance(2U) >> 16U) != 0U,
          "redo restores audio and link");
    undo.redo();
    undo.redo();
    check(gainOf(fixture.engine, 2U) == -9 && colors[2].load() == 3 &&
          undo.redoDepth() == 0U, "redo everything");
    check(fixture.observer.latest(kUndoStatusKey).rfind("UD1;", 0) == 0, "status published");
    undo.undo();
    undo.record(Kind::colors, now + std::chrono::seconds(20), [&] { colors[5].store(1); });
    check(undo.redoDepth() == 0U, "new edit truncates redo");
    undo.hostRestored(now + std::chrono::seconds(30));
    check(undo.undoDepth() == 0U, "host restore clears the history");
    undo.record(Kind::colors, now + std::chrono::seconds(30), [&] { colors[6].store(1); });
    check(undo.undoDepth() == 0U, "no steps right after a host restore");
}

struct RoundTripHost final : midichopper::ui::SamplerUiHost {
    std::vector<std::pair<std::string, std::string>> sent;
    void sendUiParameter(std::uint32_t, float) override {}
    void sendUiState(const char* key, const std::string& value) override { sent.emplace_back(key, value); }
    void sendUiNote(std::uint8_t, std::uint8_t) override {}
    void repaintUi() override {}
    bool showUiPadFileBrowser(bool, const char*) override { return false; }
    void invalidateUiInteraction(midichopper::ui::UiInteractionInvalidation) override {}
};

// One AUTO COLOR click over 16 linked pads: UI -> plugin -> UI, replies processed in arrival order.
void autoColorRoundTrip()
{
    for (const bool hotCoalescingWindow : {false, true}) {
        MainSampleFixture fixture;
        std::array<std::atomic<int>, kPadCount> colors{};
        UndoController undo(fixture.engine, fixture.main, fixture.observer, colors);
        RoundTripHost host;
        midichopper::ui::SamplerUiSession session(host);
        session.openMainSample();
        MainSampleStatus status;
        status.state = MainSampleState::ready;
        status.sourceId = 0xABCU;
        status.frames = 16000U;
        status.sampleRate = 48000U;
        status.text = "loop.wav";
        for (std::uint16_t i = 0U; i < 16U; ++i) {
            status.slices.push_back({static_cast<std::uint16_t>(i + 1U), i * 1000U, (i + 1U) * 1000U});
            status.links.push_back({i, static_cast<std::uint16_t>(i + 1U)});
        }
        status.sequence = 1U;
        check(session.acceptState(kMainSampleStatusKey, encodeMainSampleStatus(status).c_str()), "links reach the UI");

        const auto now = UndoController::Clock::now();
        std::size_t delivered = fixture.observer.messages.size();
        const auto deliver = [&] {
            for (; delivered < fixture.observer.messages.size(); ++delivered) {
                const auto& [key, value] = fixture.observer.messages[delivered];
                session.acceptState(key.c_str(), value.c_str());
            }
        };
        if (hotCoalescingWindow) {
            // A colour edit just before the click: the coalescing window is open.
            undo.record(UndoController::Kind::colors, now, [&] { colors[0].store(0); });
            deliver();
        }
        // An older status that is still in flight when the click happens.
        const std::string stale = fixture.observer.latest(kUndoStatusKey);
        session.autoColorPads();
        const auto depth = undo.undoDepth();
        std::string batch;
        for (const auto& [key, value] : host.sent)
            if (key == kUndoRequestKey && value.starts_with("colors:")) batch = value;
        check(!batch.empty() && std::count_if(host.sent.begin(), host.sent.end(), [](const auto& m) {
                  return m.first == kUndoRequestKey; }) == 1, "AUTO COLOR sends exactly one batched request");
        if (!stale.empty())
            session.acceptState(kUndoStatusKey, stale.c_str()); // arrives before the plugin's reply
        check(undo.applyColorBatch(std::string_view(batch).substr(7), now + std::chrono::milliseconds(hotCoalescingWindow ? 100 : 0)),
              "the plugin applies the batch");
        deliver();
        check(undo.undoDepth() == depth + 1U, "the whole batch is one undo step");
        for (std::uint32_t pad = 0U; pad < 16U; ++pad) {
            const int expected = midichopper::ui::MainSampleController::spreadColor(
                pad, static_cast<std::size_t>(8), session.mainSample().colorPress());
            check(session.state().padColors[pad] == expected && colors[pad].load() == expected,
                  "every pad colour (UI state and plugin) equals its slice colour after one click");
        }
        undo.undo();
        deliver();
        check(colors[5].load() == 0 && session.state().padColors[5] == 0, "undo reverts all pads in one step");
    }
}

void undoColorStatusCarriesNewColours()
{
    MainSampleFixture fixture;
    std::array<std::atomic<int>, kPadCount> colors{};
    UndoController undo(fixture.engine, fixture.main, fixture.observer, colors);
    const auto now = UndoController::Clock::now();
    undo.record(UndoController::Kind::colors, now + std::chrono::seconds(10), [&] {
        colors[0].store(2);
        colors[1].store(3);
    });
    const auto status = fixture.observer.latest(kUndoStatusKey);
    check(status.size() >= 2U && status.find(';', status.find(';', status.find(';', 4) + 1) + 1) != std::string::npos,
          "status has a header");
    const auto digits = status.substr(status.rfind(';') + 1);
    check(digits.size() >= 2U && digits[0] == '2' && digits[1] == '3',
          "the colour status published for a step shows the colours after the edit");
}

void undoGestureCoalescesLiveUpdates()
{
    MainSampleFixture fixture;
    std::array<std::atomic<int>, kPadCount> colors{};
    UndoController undo(fixture.engine, fixture.main, fixture.observer, colors);
    const auto now = UndoController::Clock::now();
    const auto setGain = [&](const float db) {
        auto mixer = fixture.engine.padMixerSettings(3U);
        mixer.gainDecibels = db;
        fixture.engine.setPadMixerSettings(3U, mixer);
    };
    undo.recordGesture('B', now, [&] { setGain(-3.0f); });
    for (int i = 0; i < 10; ++i)
        undo.recordGesture('U', now, [&] { setGain(-3.0f - static_cast<float>(i)); });
    check(undo.undoDepth() == 0U && gainOf(fixture.engine, 3U) == -12.0f, "live updates make no step");
    undo.recordGesture('E', now, [&] { setGain(-20.0f); });
    check(undo.undoDepth() == 1U, "the whole drag is one undo step");
    undo.undo();
    check(gainOf(fixture.engine, 3U) == 0.0f, "undo restores the pre-drag value");
    undo.redo();
    check(gainOf(fixture.engine, 3U) == -20.0f, "redo restores the final value");
    undo.recordGesture('B', now, [&] { setGain(-20.0f); });
    undo.recordGesture('E', now, [&] { setGain(-20.0f); });
    check(undo.undoDepth() == 1U, "an unchanged gesture leaves no step");
    undo.recordGesture('S', now, [&] { setGain(0.0f); });
    check(undo.undoDepth() == 2U, "a single-shot (Ctrl+click reset) is one step");

    // A live slice drag re-cuts the linked pad on every message and is still one step.
    const auto path = writeSourceWav("sms-undo-live-source.wav", 1000U);
    auto load = mainRequest(MainSampleVerb::load);
    load.path = utf8(path);
    fixture.send(load, 2U);
    undo.record(UndoController::Kind::action, now, [&] { fixture.send(assignRequest(2U, 100U, 300U)); });
    const auto depth = undo.undoDepth();
    const auto slice = fixture.main.session().slices.front();
    undo.recordGesture('B', now, [&] { fixture.send(editRequest({slice.id, 100U, 320U})); });
    check(padHoldsSlice(fixture.engine, 2U, 100U, 320U), "the pad plays the dragged range mid-drag");
    undo.recordGesture('U', now, [&] { fixture.send(editRequest({slice.id, 100U, 340U})); });
    check(padHoldsSlice(fixture.engine, 2U, 100U, 340U) && undo.undoDepth() == depth, "updates add no step");
    undo.recordGesture('E', now, [&] { fixture.send(editRequest({slice.id, 100U, 360U})); });
    check(undo.undoDepth() == depth + 1U && padHoldsSlice(fixture.engine, 2U, 100U, 360U), "one step for the drag");
    undo.undo();
    check(padHoldsSlice(fixture.engine, 2U, 100U, 300U), "undo returns to the range before the drag");
}

void padSwap()
{
    PadStructureRequest roundTrip;
    check(decodePadStructureRequest("PS1;W;2;5", roundTrip) && roundTrip.action == PadStructureAction::swap &&
          roundTrip.firstPad == 2U && roundTrip.targetPad == 5U &&
          encodePadStructureRequest(roundTrip) == "PS1;W;2;5", "swap request round-trips");
    for (const char* bad : {"PS1;W;2;2", "PS1;W;2", "PS1;W;2;999", "PS1;W;2;5;1", "PS1;W;x;5"})
        check(!decodePadStructureRequest(bad, roundTrip), "malformed swap request is refused");

    MainSampleFixture fixture;
    std::array<std::atomic<int>, kPadCount> colors{};
    colors[2] = 3;
    colors[5] = 7 | 0x100; // any colour plus a flag bit that must travel with the pad
    std::vector<std::pair<std::uint32_t, std::uint32_t>> hooked;
    fixture.pads.setSwapHook([&](const std::uint32_t a, const std::uint32_t b) {
        hooked.emplace_back(a, b);
        const int first = colors[a];
        colors[a] = colors[b].load();
        colors[b] = first;
    });
    const auto path = writeSourceWav("sms-swap-source.wav", 1000U);
    auto load = mainRequest(MainSampleVerb::load);
    load.path = utf8(path);
    fixture.send(load, 2U);
    fixture.send(assignRequest(2U, 100U, 300U));
    fixture.send(assignRequest(5U, 400U, 500U));
    auto mixer = fixture.engine.padMixerSettings(2U);
    mixer.gainDecibels = -6.0f;
    mixer.pan = -0.5f;
    fixture.engine.setPadMixerSettings(2U, mixer);
    const auto tag2 = fixture.provenance(2U);
    const auto tag5 = fixture.provenance(5U);
    check(tag2 != 0U && tag5 != 0U && tag2 != tag5, "two linked pads before the swap");

    fixture.pads.setSwapPublishedHook([&](const std::uint32_t x, const std::uint32_t y) {
        fixture.main.republishStatus((1ULL << x) | (1ULL << y));
    });
    const auto linkedSlice = [&](const std::uint32_t pad) {
        MainSampleStatus status;
        check(decodeMainSampleStatus(fixture.observer.latest(kMainSampleStatusKey), status), "status decodes");
        for (const auto& link : status.links)
            if (link.pad == pad) return static_cast<int>(link.sliceId);
        return -1;
    };
    const int slice2 = static_cast<int>(tag2 & 0xFFFFU);
    const int slice5 = static_cast<int>(tag5 & 0xFFFFU);
    UndoController undo(fixture.engine, fixture.main, fixture.observer, colors);
    const auto now = UndoController::Clock::now();
    const auto depth = undo.undoDepth();
    undo.record(UndoController::Kind::action, now, [&] {
        fixture.pads.handlePadStructureRequest("PS1;W;2;5");
    });
    check(fixture.observer.latest(kPadStructureStatusKey) == "PS1;O;0;W", "swap reports success");
    check(padHoldsSlice(fixture.engine, 5U, 100U, 300U) && padHoldsSlice(fixture.engine, 2U, 400U, 500U),
          "audio moves with the pad (swap, no shifting)");
    check(fixture.provenance(2U) == tag5 && fixture.provenance(5U) == tag2, "slice link and provenance swap");
    check(gainOf(fixture.engine, 5U) == -6.0f && fixture.engine.padMixerSettings(5U).pan == -0.5f &&
          gainOf(fixture.engine, 2U) == 0.0f, "volume and pan travel with the pad");
    check(hooked.size() == 1U && colors[2] == (7 | 0x100) && colors[5] == 3, "colour and favourite flag swap");
    check(linkedSlice(2U) == slice5 && linkedSlice(5U) == slice2,
          "the published main-sample links swap with the pads (labels, badges, zone colours)");
    check(undo.undoDepth() == depth + 1U, "one undo step");
    undo.undo();
    check(padHoldsSlice(fixture.engine, 2U, 100U, 300U) && padHoldsSlice(fixture.engine, 5U, 400U, 500U) &&
          fixture.provenance(2U) == tag2 && gainOf(fixture.engine, 2U) == -6.0f, "undo restores both pads");

    // A pad may be swapped with an empty pad (the content simply moves).
    fixture.pads.handlePadStructureRequest("PS1;W;2;9");
    check(linkedSlice(9U) == slice2 && linkedSlice(2U) == -1, "a slice moved to an empty pad keeps its link there");
    check(padHoldsSlice(fixture.engine, 9U, 100U, 300U) && !fixture.engine.padMetadata(2U).occupied &&
          fixture.provenance(2U) == 0U && fixture.provenance(9U) == tag2, "swap with an empty pad moves the pad");
    {
        // Cross-bank swap (bank A pad 9 <-> bank C pad 40): global indices, one undo step.
        const auto tag9 = fixture.provenance(9U);
        const auto crossDepth = undo.undoDepth();
        undo.record(UndoController::Kind::action, now + std::chrono::seconds(5), [&] {
            fixture.pads.handlePadStructureRequest("PS1;W;9;40");
        });
        check(fixture.observer.latest(kPadStructureStatusKey) == "PS1;O;0;W", "cross-bank swap succeeds");
        check(fixture.provenance(40U) == tag9 && !fixture.engine.padMetadata(9U).occupied,
              "cross-bank swap moves the pad to its global index");
        check(undo.undoDepth() == crossDepth + 1U, "cross-bank swap is one undo step");
        undo.undo();
        check(fixture.provenance(9U) == tag9 && !fixture.engine.padMetadata(40U).occupied,
              "undo restores the cross-bank swap");
    }

    // Refusals change nothing.
    const auto before = fixture.provenance(9U);
    const auto messages = hooked.size();
    fixture.pads.handlePadStructureRequest("PS1;W;9;9");
    check(fixture.observer.latest(kPadStructureStatusKey).starts_with("PS1;E;") &&
          hooked.size() == messages && fixture.provenance(9U) == before, "same-pad swap is refused");
    auto armed = fixture.engine.settings();
    armed.armed = true;
    fixture.engine.setSettings(armed);
    fixture.pads.handlePadStructureRequest("PS1;W;9;5");
    check(fixture.observer.latest(kPadStructureStatusKey).starts_with("PS1;E;") &&
          hooked.size() == messages && fixture.provenance(9U) == before, "armed swap is refused");
}

void favoriteGuards()
{
    MainSampleFixture fixture(12.0);
    const auto path = writeSourceWav("sms-favorites.wav", 192000U);
    auto load = mainRequest(MainSampleVerb::load);
    load.path = utf8(path);
    static_cast<void>(fixture.send(load, 2U));
    AutoChopRequest equal;
    equal.mode = AutoChopMode::equal;
    equal.count = 4U;
    check(autoChop(fixture, equal).state == AutoChopState::ok, "baseline chop");
    const auto before = fixture.main.session();
    const auto favoriteSlice = before.slices[1];
    const auto favoriteTag = fixture.provenance(1U);
    const auto favoriteGeneration = fixture.engine.padMetadata(1U).generation;
    fixture.engine.setFavoriteMask(1ULL << 1U);

    check(fixture.engine.isFavorite(1U) && !fixture.engine.isFavorite(0U), "favorite mask reads back");
    // Auto-chop skips the favorite, keeps its slice fixed and cuts around it.
    equal.count = 3U;
    const auto status = autoChop(fixture, equal);
    const auto after = fixture.main.session();
    check(status.state == AutoChopState::ok && (status.mask & 2U) == 0U,
          "auto-chop never targets a favorite pad");
    check(fixture.provenance(1U) == favoriteTag &&
          fixture.engine.padMetadata(1U).generation == favoriteGeneration &&
          padHoldsSlice(fixture.engine, 1U, favoriteSlice.start, favoriteSlice.end),
          "favorite pad keeps audio, tag and generation through auto-chop");
    bool kept = false;
    for (const auto& slice : after.slices) {
        if (slice.id == favoriteSlice.id) {
            kept = slice.start == favoriteSlice.start && slice.end == favoriteSlice.end;
            continue;
        }
        check(slice.end <= favoriteSlice.start || slice.start >= favoriteSlice.end,
              "new slices never overlap a favorite region");
    }
    check(kept, "the favorite slice stays fixed");

    // Marker edit, delete and assign of a favorite are refused with the shared message.
    auto request = mainRequest(MainSampleVerb::remove);
    request.sliceId = favoriteSlice.id;
    auto refused = fixture.send(request);
    check(refused.state == MainSampleState::error && refused.text == midichopper::plugin::kFavoriteRefusal,
          "deleting a favorite's slice is refused");
    refused = fixture.send(assignRequest(1U, 10U, 20U));
    check(refused.state == MainSampleState::error && refused.text == midichopper::plugin::kFavoriteRefusal,
          "assign onto a favorite is refused");
    refused = fixture.send(editRequest({favoriteSlice.id, favoriteSlice.start + 5U, favoriteSlice.end}));
    check(refused.state == MainSampleState::error && refused.text == midichopper::plugin::kFavoriteRefusal &&
          fixture.main.session().find(favoriteSlice.id)->start == favoriteSlice.start,
          "marker edits of a favorite's slice are refused");

    // Paste and WAV import onto a favorite are refused with the message.
    fixture.pads.handlePadClipboardRequest(encodePadClipboardRequest(PadClipboardAction::paste, 1U));
    check(fixture.observer.latest(kPadFileStatusKey) == midichopper::plugin::kFavoriteRefusal &&
          fixture.engine.padMetadata(1U).generation == favoriteGeneration, "paste onto a favorite is refused");
    fixture.pads.handlePadFileRequest(encodePadFileRequest(PadFileAction::import, 1U, utf8(path)));
    check(fixture.observer.latest(kPadFileStatusKey) == midichopper::plugin::kFavoriteRefusal &&
          fixture.engine.padMetadata(1U).generation == favoriteGeneration, "import onto a favorite is refused");

    // Unfavoriting makes the pad editable again.
    fixture.engine.setFavoriteMask(0U);
    check(fixture.send(assignRequest(1U, 10U, 20U)).state == MainSampleState::ready,
          "an unfavorited pad can be assigned");
    std::filesystem::remove(path);
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
    autoChopWorkflow();
    mainSampleArrange();
    favoriteGuards();
    undoRedoRestoresState();
    undoColorStatusCarriesNewColours();
    autoColorRoundTrip();
    undoGestureCoalescesLiveUpdates();
    padSwap();
    arrangeKeepsColours();
    zeroCrossSnap();
    std::cout << "Pad workflow tests passed\n";
}
