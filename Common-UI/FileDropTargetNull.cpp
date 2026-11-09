// Platforms without a native drop implementation: dropping files is unavailable.
#include "FileDropTarget.hpp"

namespace sms::ui {

std::unique_ptr<FileDropTarget> FileDropTarget::attach(
    const std::uintptr_t nativeWindow, FileDropListener& listener) noexcept
{
    static_cast<void>(nativeWindow);
    static_cast<void>(listener);
    return nullptr;
}

void FileDropTarget::releaseDroppedFile(const std::string_view utf8Path) noexcept
{
    static_cast<void>(utf8Path);
}

} // namespace sms::ui
