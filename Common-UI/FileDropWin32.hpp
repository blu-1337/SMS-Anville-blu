#pragma once

// Win32-only pieces of the file-drop target, exposed so tests can drive the
// COM object directly. Include only from Windows sources.
#if defined(_WIN32)

#ifndef NOMINMAX
# define NOMINMAX
#endif
#include <windows.h>
#include <ole2.h>

#include "FileDropTarget.hpp"

#include <climits>
#include <filesystem>
#include <string>
#include <string_view>

namespace sms::ui::win32 {

// Invalid UTF-16/UTF-8 fails rather than being replaced: a replaced path could
// never open the dropped file.
[[nodiscard]] inline bool utf8FromWide(const std::wstring_view wide, std::string& utf8)
{
    utf8.clear();
    if (wide.empty())
        return true;
    if (wide.size() > static_cast<std::size_t>(INT_MAX))
        return false;
    const int count = static_cast<int>(wide.size());
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), count,
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return false;
    utf8.resize(static_cast<std::size_t>(length));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), count,
                               utf8.data(), length, nullptr, nullptr) == length;
}

[[nodiscard]] inline bool wideFromUtf8(const std::string_view utf8, std::wstring& wide)
{
    wide.clear();
    if (utf8.empty())
        return true;
    if (utf8.size() > static_cast<std::size_t>(INT_MAX))
        return false;
    const int count = static_cast<int>(utf8.size());
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), count,
                                           nullptr, 0);
    if (length <= 0)
        return false;
    wide.resize(static_cast<std::size_t>(length));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), count,
                               wide.data(), length) == length;
}

/** `%TEMP%\SMS-Anville\drops`, or empty when the temp folder is unavailable. */
[[nodiscard]] std::filesystem::path privateDropDirectory();

/** The COM object behind FileDropTarget::attach, with one reference owned by
 * the caller. The window is used only for ScreenToClient.
 */
[[nodiscard]] IDropTarget* createOleDropTarget(HWND window, FileDropListener& listener) noexcept;

} // namespace sms::ui::win32

#endif
