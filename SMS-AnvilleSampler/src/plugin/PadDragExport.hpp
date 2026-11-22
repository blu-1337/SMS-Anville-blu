#pragma once

#include "Audio/WavCodec.hpp"
#include "DSP/SamplePlaybackSettings.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace midichopper::plugin {

/** Dragging a pad out of the window exports its trimmed region as a float32 WAV. */
inline constexpr std::uint64_t kMaxDragExportBytes = 256ULL * 1024U * 1024U;
inline constexpr std::chrono::hours kDragFileLifetime{24};

[[nodiscard]] inline std::filesystem::path dragExportDirectory()
{
    std::error_code error;
    auto base = std::filesystem::temp_directory_path(error);
    if (error) return {};
    return base / "SMS-Anville" / "drag";
}

/** A file stem safe on every file system: no extension, no separators or reserved characters. */
[[nodiscard]] inline std::string dragFileStem(std::string_view name)
{
    if (const auto slash = name.find_last_of("/\\"); slash != std::string_view::npos)
        name.remove_prefix(slash + 1U);
    if (const auto dot = name.find_last_of('.'); dot != std::string_view::npos && dot > 0U)
        name = name.substr(0U, dot);
    std::string stem;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        const bool bad = u < 0x20U || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos;
        stem.push_back(bad ? '_' : c);
        if (stem.size() >= 48U) break;
    }
    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.')) stem.pop_back();
    while (!stem.empty() && stem.front() == ' ') stem.erase(stem.begin());
    return stem.empty() ? std::string("Pad") : stem;
}

/** "SampleName_pad05.wav" for global pad index 4 (pads are shown 1-based). */
[[nodiscard]] inline std::string dragFileName(const std::string_view sourceName, const std::uint32_t pad)
{
    const std::uint32_t shown = pad + 1U;
    return dragFileStem(sourceName) + "_pad" + (shown < 10U ? "0" : "") + std::to_string(shown) + ".wav";
}

/** The pad's Start/End region at the stored rate (no envelope, no tuning, no mixer). */
[[nodiscard]] inline sms::audio::WavAudio trimToPlaybackRegion(
    const sms::audio::WavAudio& source, const sms::dsp::SamplePlaybackSettings& requested)
{
    sms::audio::WavAudio out;
    if (source.sampleRate == 0U || source.frames == 0U ||
        source.stereo.size() < static_cast<std::size_t>(source.frames) * 2U)
        return out;
    const auto settings = sms::dsp::sanitize(requested);
    const std::uint32_t start = std::min(source.frames - 1U, static_cast<std::uint32_t>(
        std::floor(static_cast<double>(settings.start) * source.frames)));
    const std::uint32_t end = std::clamp(static_cast<std::uint32_t>(
        std::ceil(static_cast<double>(settings.end) * source.frames)), start + 1U, source.frames);
    out.sampleRate = source.sampleRate;
    out.frames = end - start;
    out.stereo.assign(source.stereo.begin() + static_cast<std::ptrdiff_t>(start) * 2,
                      source.stereo.begin() + static_cast<std::ptrdiff_t>(end) * 2);
    return out;
}

[[nodiscard]] inline bool dragExportTooLarge(const std::uint32_t frames) noexcept
{
    return static_cast<std::uint64_t>(frames) * 8U > kMaxDragExportBytes;
}

/** Only a file older than the lifetime is stale; the age is measured against `now`. */
[[nodiscard]] inline bool isStaleDragFile(const std::filesystem::file_time_type modified,
                                          const std::filesystem::file_time_type now) noexcept
{
    return now > modified && now - modified > kDragFileLifetime;
}

/** Removes stale .wav files from the drag folder. Call on attach, never during a drag. */
inline std::size_t cleanDragDirectory(const std::filesystem::path& directory,
                                      const std::filesystem::file_time_type now)
{
    std::size_t removed = 0U;
    std::error_code error;
    if (directory.empty() || !std::filesystem::is_directory(directory, error)) return 0U;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        std::error_code entryError;
        if (!it->is_regular_file(entryError) || it->path().extension() != ".wav") continue;
        const auto modified = std::filesystem::last_write_time(it->path(), entryError);
        if (entryError || !isStaleDragFile(modified, now)) continue;
        if (std::filesystem::remove(it->path(), entryError)) ++removed;
    }
    return removed;
}

/** Gesture: armed on a pad press; once the pointer leaves the window the export is requested,
 * and when it is ready the OS drag starts. Pure so it can be tested without a window. */
class PadDragOutGesture {
public:
    enum class Action : std::uint8_t { none, request, startDrag };
    enum class State : std::uint8_t { idle, armed, requested, ready, failed, dragging };

    void arm(const int pad) noexcept { fPad = pad; fState = pad >= 0 ? State::armed : State::idle; }
    void release() noexcept { fPad = -1; fState = State::idle; }
    [[nodiscard]] State state() const noexcept { return fState; }
    [[nodiscard]] int pad() const noexcept { return fPad; }
    [[nodiscard]] bool active() const noexcept { return fState != State::idle; }

    /** Call from idle: `outside` = pointer outside the window, `buttonDown` = left button held. */
    [[nodiscard]] Action poll(const bool outside, const bool buttonDown) noexcept
    {
        if (fState == State::idle || fState == State::dragging) return Action::none;
        if (!buttonDown) { release(); return Action::none; }
        if (fState == State::armed && outside) { fState = State::requested; return Action::request; }
        if (fState == State::ready && outside) { fState = State::dragging; return Action::startDrag; }
        return Action::none;
    }
    /** The plug-in answered the export request. */
    void exported(const bool ok) noexcept
    {
        if (fState == State::requested) fState = ok ? State::ready : State::failed;
    }
    /** The OS drag returned (dropped or cancelled). */
    void dragFinished() noexcept { release(); }

private:
    int fPad = -1;
    State fState = State::idle;
};

} // namespace midichopper::plugin
