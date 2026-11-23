#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace sms::ui {

/** What an intercepted keystroke asks for. */
enum class HistoryKey : std::uint8_t { none, undo, redo };

/** Pure decision shared by the hook and its tests: Ctrl+Z undo, Ctrl+Shift+Z and Ctrl+Y redo.
 * `virtualKey` is the Windows virtual-key code ('Z' and 'Y' are their ASCII codes). */
[[nodiscard]] constexpr HistoryKey classifyHistoryKey(const std::uint32_t virtualKey, const bool ctrl,
                                                      const bool shift, const bool alt) noexcept
{
    if (!ctrl || alt) return HistoryKey::none;
    if (virtualKey == 'Z') return shift ? HistoryKey::redo : HistoryKey::undo;
    if (virtualKey == 'Y') return HistoryKey::redo;
    return HistoryKey::none;
}

/** Thread-scoped (never global) keyboard hooks that catch Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y for one editor
 * window before the host's own accelerators see them. UI thread only; remove by destroying it.
 * The callback returns true when it handled the key (the keystroke is then swallowed) and false when
 * the plug-in does not want it (for example while a value is being typed). */
class UndoKeyHook {
public:
    using Callback = std::function<bool(HistoryKey)>;
    virtual ~UndoKeyHook() = default;
    /** nullptr when unsupported or the hook cannot be installed. */
    [[nodiscard]] static std::unique_ptr<UndoKeyHook> install(std::uintptr_t nativeWindow, Callback callback) noexcept;

protected:
    UndoKeyHook() = default;
    UndoKeyHook(const UndoKeyHook&) = delete;
    UndoKeyHook& operator=(const UndoKeyHook&) = delete;
};

} // namespace sms::ui
