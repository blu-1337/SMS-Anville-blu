#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>

#include <string>

namespace sms::ui::win32 {

/** The CF_HDROP data object a pad drag offers; one reference owned by the caller. For tests. */
[[nodiscard]] IDataObject* createFileDataObject(const std::wstring& path) noexcept;

} // namespace sms::ui::win32
