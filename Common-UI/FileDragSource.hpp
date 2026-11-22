#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sms::ui {

/** Native drag-out of one file (CF_HDROP, copy effect). Windows only; other platforms are no-ops. */
class FileDragSource {
public:
    /** True when the cursor is outside the window's client area (false when unknown). */
    [[nodiscard]] static bool pointerOutsideWindow(std::uintptr_t nativeWindow) noexcept;
    /** True while the left mouse button is held. */
    [[nodiscard]] static bool primaryButtonDown() noexcept;
    /** Blocks in the OS drag loop until the drop or cancel. True when a target accepted the copy. */
    static bool start(std::string_view utf8Path) noexcept;
    /** TEMPORARY diagnostics: appends a timestamped line to %TEMP%\SMS-Anville\drag\<file>. */
    static void log(std::string_view file, std::string_view message) noexcept;
    /** True while start() runs; drop targets in this process ignore such a drag. */
    [[nodiscard]] static bool active() noexcept;
};

} // namespace sms::ui
