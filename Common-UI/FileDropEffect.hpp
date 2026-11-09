#pragma once

#include <cstdint>
#include <string_view>

namespace sms::ui {

// Platform-neutral mirrors of the OLE DROPEFFECT bits. The Win32 target
// static_asserts that these values match DROPEFFECT_NONE/COPY/MOVE/LINK.
inline constexpr std::uint32_t kDropNone = 0U;
inline constexpr std::uint32_t kDropCopy = 1U;
inline constexpr std::uint32_t kDropMove = 2U;
inline constexpr std::uint32_t kDropLink = 4U;

/** Choose an effect the drag source actually allows: COPY, then MOVE, then LINK.
 * Writing COPY unconditionally makes OLE treat a MOVE-only source (FL Studio
 * Edison) as "no drop", so the drag ends in DragLeave instead of Drop.
 */
[[nodiscard]] constexpr std::uint32_t chooseDropEffect(
    const std::uint32_t allowed, const bool accept) noexcept
{
    if (!accept)
        return kDropNone;
    if ((allowed & kDropCopy) != 0U)
        return kDropCopy;
    if ((allowed & kDropMove) != 0U)
        return kDropMove;
    if ((allowed & kDropLink) != 0U)
        return kDropLink;
    return kDropNone;
}

/** Effect reported back from Drop. A reported MOVE asks the source to delete
 * its original ("unoptimized move"), so a completed MOVE is reported as NONE,
 * the documented "optimized move" answer that leaves the source file intact.
 */
[[nodiscard]] constexpr std::uint32_t reportedDropEffect(const std::uint32_t chosen) noexcept
{
    return chosen == kDropCopy ? kDropCopy : kDropNone;
}

/** True when the chosen effect means the source may delete or reuse its file,
 * so the target must copy it before the asynchronous import reads it.
 */
[[nodiscard]] constexpr bool dropNeedsPrivateCopy(const std::uint32_t chosen) noexcept
{
    return chosen == kDropMove || chosen == kDropLink;
}

[[nodiscard]] constexpr bool isSupportedAudioPath(const std::string_view path) noexcept
{
    const auto lower = [](const char c) noexcept {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    const auto endsWith = [&](const std::string_view suffix) noexcept {
        if (path.size() <= suffix.size())
            return false;
        const auto tail = path.substr(path.size() - suffix.size());
        for (std::size_t index = 0; index < suffix.size(); ++index) {
            if (lower(tail[index]) != suffix[index])
                return false;
        }
        const char before = path[path.size() - suffix.size() - 1U];
        return before != '/' && before != '\\';
    };
    return endsWith(".wav") || endsWith(".wave");
}

} // namespace sms::ui
