#include "Audio/RealtimeLatestMailbox.hpp"
#include "ChopEditorProtocol.hpp"
#include "MainSampleProtocol.hpp"
#include "Parameters.hpp"
#include "PluginControls.hpp"
#include "SamplerEngine.hpp"
#include "StateKeys.hpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {
using midichopper::plugin::ChopPreviewRequest;
using Mailbox = sms::audio::RealtimeLatestMailbox<ChopPreviewRequest>;
void check(const bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

ChopPreviewRequest request(const std::uint64_t revision)
{
    return {revision % 2U != 0U, static_cast<std::uint32_t>(revision % 61U),
            static_cast<std::uint32_t>(1U + revision % 3U), revision * 1000U,
            revision * 1000U + 999U};
}

bool coherent(const ChopPreviewRequest& value)
{
    const auto revision = value.sourceFrame / 1000U;
    const auto expected = request(revision);
    return value.play == expected.play && value.firstPad == expected.firstPad &&
           value.padCount == expected.padCount && value.sourceFrame == expected.sourceFrame &&
           value.sourceEndFrame == expected.sourceEndFrame;
}

void supersededCommands()
{
    Mailbox mailbox;
    ChopPreviewRequest value;
    check(!mailbox.tryConsume(value), "empty mailbox has no command");
    mailbox.publish(request(1));
    mailbox.publish(request(2)); // stop supersedes play including its entire range
    check(mailbox.tryConsume(value) && coherent(value) && value.sourceFrame == 2000U,
          "latest stop/range supersedes unread play as one value");
    check(!mailbox.tryConsume(value), "consumed command is not replayed");
    mailbox.publish(request(3));
    check(mailbox.tryConsume(value) && coherent(value) && value.sourceFrame == 3000U,
          "next publication remains observable after consumption");
}

void overlappingWritersAndAudio()
{
    Mailbox mailbox;
    std::atomic<bool> start{false};
    std::atomic<unsigned> completed{0};
    constexpr std::uint64_t count = 100000U;
    auto writer = [&](const std::uint64_t offset) {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::uint64_t revision = 1U; revision <= count; ++revision)
            mailbox.publish(request(offset + revision));
        completed.fetch_add(1U, std::memory_order_release);
    };
    std::thread first(writer, 0U), second(writer, count);
    start.store(true, std::memory_order_release);
    ChopPreviewRequest value;
    unsigned consumed = 0U;
    while (completed.load(std::memory_order_acquire) < 2U) {
        if (mailbox.tryConsume(value)) {
            check(coherent(value), "overlap never mixes command, pad, start or end revisions");
            ++consumed;
        }
    }
    first.join();
    second.join();
    if (mailbox.tryConsume(value)) {
        check(coherent(value), "final pending publication remains coherent");
        ++consumed;
    }
    check(consumed != 0U, "reader consumed concurrent publications");
    mailbox.publish(request(count * 2U + 1U));
    check(mailbox.tryConsume(value) && value.sourceFrame == (count * 2U + 1U) * 1000U &&
          coherent(value), "latest value is delivered after overlap");
    check(!mailbox.tryConsume(value), "final threaded value is consumed only once");
}

midichopper::PadData sourceRamp(const std::uint32_t frames)
{
    midichopper::PadData data;
    data.sampleRate = 1000.0;
    data.frames = frames;
    data.stereo.resize(static_cast<std::size_t>(frames) * 2U);
    for (std::uint32_t frame = 0U; frame < frames; ++frame) {
        data.stereo[static_cast<std::size_t>(frame) * 2U] = 0.001f * static_cast<float>(frame);
        data.stereo[static_cast<std::size_t>(frame) * 2U + 1U] = -0.001f * static_cast<float>(frame);
    }
    data.peak = 0.001f * static_cast<float>(frames);
    data.rms = data.peak * 0.5f;
    return data;
}

// main_preview_request takes the chop preview's control-to-audio path.
void mainSamplePreviewThroughControls()
{
    using namespace midichopper::plugin;
    midichopper::SamplerEngine engine(1000.0, 1.0);
    PluginControls controls(0, 0);
    const PluginControls::GlobalSettings globals{};
    const auto source = sourceRamp(100U);
    check(engine.setSource(&source, 9U), "store a source to preview");
    const auto queue = [&](const std::string& value) {
        return controls.queueStateCommand(kMainPreviewRequestKey, value.c_str());
    };

    check(queue(encodeSourcePreviewRequest({true, false, 9U, 10U, 60U})),
          "the controls own main_preview_request");
    check(!engine.sourcePreviewActive(), "nothing starts before the audio owner applies it");
    controls.applyToEngine(engine, globals);
    check(engine.sourcePreviewActive() && engine.sourcePreviewPosition() == 1.0 + 10.0 / 100.0,
          "a queued play starts the source preview");

    check(queue(encodeSourcePreviewRequest({})), "a stop is accepted");
    controls.applyToEngine(engine, globals);
    check(!engine.sourcePreviewActive(), "a queued stop stops the source preview");

    check(queue(encodeSourcePreviewRequest({true, true, 9U, 0U, 50U})) &&
          queue(encodeSourcePreviewRequest({})), "play then stop queue");
    controls.applyToEngine(engine, globals);
    check(!engine.sourcePreviewActive(), "the latest request wins");

    check(queue("MP1;1;0;9;20;10") && queue("garbage") && queue(""),
          "invalid requests are consumed by the controls");
    controls.applyToEngine(engine, globals);
    check(!engine.sourcePreviewActive(), "invalid requests start nothing");

    check(queue(encodeSourcePreviewRequest({true, false, 8U, 0U, 50U})), "stale id queues");
    controls.applyToEngine(engine, globals);
    check(!engine.sourcePreviewActive(), "a stale source id starts nothing");

    check(queue(encodeSourcePreviewRequest({true, false, 9U, 0U, 50U})), "play queues again");
    controls.applyToEngine(engine, globals);
    controls.applyToEngine(engine, globals);
    check(engine.sourcePreviewActive(), "a consumed request is not replayed as a stop");
}

void mainSamplePreviewParameter()
{
    using namespace midichopper::plugin;
    check(kParameterMainSamplePreviewPosition == kParameterCaptureStorageExhausted + 1U &&
          kParameterMainSamplePreviewPosition == 72U && kParameterCount == 73U,
          "the preview position is appended after every existing parameter");
    const auto range = parameterRange(kParameterMainSamplePreviewPosition);
    check(range.defaultValue == 0.0f && range.minimum == 0.0f && range.maximum == 2.0f,
          "0 = idle, else 1 + frame / frames");
}
}

int main()
{
    supersededCommands();
    overlappingWritersAndAudio();
    mainSamplePreviewThroughControls();
    mainSamplePreviewParameter();
    std::cout << "Preview mailbox tests passed\n";
}
