#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace sms::ui {

/** Physical client pixels: the same space as framework mouse events. */
struct FileDropPoint {
    double x = 0.0;
    double y = 0.0;
};

/** Receives native file-drag events on the window's UI thread. Paths are UTF-8. */
class FileDropListener {
public:
    virtual ~FileDropListener() = default;
    /** Return true when a drop at this point would be accepted. */
    virtual bool fileDragOver(FileDropPoint point, std::span<const std::string> utf8Paths) = 0;
    virtual void fileDragLeave() = 0;
    /** The paths stay readable until releaseDroppedFile; queue work, do not block. */
    virtual void filesDropped(FileDropPoint point, std::span<const std::string> utf8Paths) = 0;
};

/** Native drop registration for one editor window; destroy before the window.
 * The listener must outlive this object.
 */
class FileDropTarget {
public:
    virtual ~FileDropTarget() = default;

    /** Returns nullptr when the platform has no implementation or registration fails. */
    [[nodiscard]] static std::unique_ptr<FileDropTarget> attach(
        std::uintptr_t nativeWindow, FileDropListener& listener) noexcept;

    /** Delete a private copy made for a source that allowed only MOVE or LINK.
     * Paths outside the private drop folder are never touched.
     */
    static void releaseDroppedFile(std::string_view utf8Path) noexcept;

protected:
    FileDropTarget() = default;
    FileDropTarget(const FileDropTarget&) = delete;
    FileDropTarget& operator=(const FileDropTarget&) = delete;
};

} // namespace sms::ui
