#include "PadAnalysisService.hpp"

#include "PadFingerprint.hpp"
#include "StateKeys.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace midichopper::plugin {

namespace {
// analyzeSound() reads no further than this.
constexpr double kAnalysedSeconds = 30.0;

// Every analysed sample: the sampled link fingerprint could miss a local edit.
[[nodiscard]] std::uint64_t hashAudio(const std::span<const float> audio,
                                      const double rate) noexcept
{
    std::uint64_t hash = midichopper::detail::kFnvOffset;
    hash = (hash ^ std::bit_cast<std::uint64_t>(rate)) * midichopper::detail::kFnvPrime;
    hash = (hash ^ audio.size()) * midichopper::detail::kFnvPrime;
    for (const float sample : audio)
        hash = (hash ^ std::bit_cast<std::uint32_t>(sample)) * midichopper::detail::kFnvPrime;
    return hash;
}
} // namespace

void PadAnalysisService::handleRequest(const std::string_view encoded)
{
    const auto cancelled = [this] { return cancel_.load(std::memory_order_relaxed); };
    PadAnalysisRequest request;
    if (cancelled() || !decodePadAnalysisRequest(encoded, request))
        return;

    PadData audio;
    sms::dsp::SamplePlaybackSettings playback;
    bool recording = false;
    {
        const std::lock_guard lock(mutation_);
        const auto metadata = sampler_.padMetadata(request.pad);
        recording = metadata.recording;
        audio.generation = metadata.generation;
        if (!recording &&
            !sampler_.exportPublishedPad(request.pad, audio, &playback))
            return;
    }

    PadAnalysisStatus status;
    status.sequence = request.sequence;
    status.pad = request.pad;
    status.generation = audio.generation;
    const auto publish = [&] {
        if (!cancelled())
            observer_.publishUiState(kPadAnalysisStatusKey,
                                     encodePadAnalysisStatus(status).c_str());
    };

    const std::size_t frames = std::min<std::size_t>(audio.frames, audio.stereo.size() / 2U);
    const auto frameAt = [frames](const float position) {
        return std::min(frames, static_cast<std::size_t>(
            static_cast<double>(std::clamp(position, 0.0F, 1.0F)) * static_cast<double>(frames)));
    };
    const std::size_t first = frameAt(playback.start);
    std::size_t length = std::max(first, frameAt(playback.end)) - first;
    if (std::isfinite(audio.sampleRate) && audio.sampleRate > 0.0)
        length = std::min(length,
            static_cast<std::size_t>(std::ceil(kAnalysedSeconds * audio.sampleRate)));
    if (recording || length == 0U) {
        status.state = recording ? PadAnalysisState::busy : PadAnalysisState::noAudio;
        publish();
        return;
    }

    // Reverse playback does not change what the region contains.
    const auto region = std::span<const float>(audio.stereo).subspan(first * 2U, length * 2U);
    const auto audioHash = hashAudio(region, audio.sampleRate);
    auto& entry = cache_[request.pad];
    if (!entry || entry->generation != audio.generation || entry->start != playback.start ||
        entry->end != playback.end || entry->audioHash != audioHash) {
        const auto result = analyzer_(region, audio.sampleRate, &cancel_);
        if (result.cancelled || cancelled())
            return;
        entry = Entry{audio.generation, playback.start, playback.end, audioHash, result};
    }
    status.state = PadAnalysisState::ready;
    status.result = entry->result;
    publish();
}

} // namespace midichopper::plugin
