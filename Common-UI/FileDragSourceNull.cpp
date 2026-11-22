#include "FileDragSource.hpp"

namespace sms::ui {

bool FileDragSource::pointerOutsideWindow(std::uintptr_t) noexcept { return false; }
bool FileDragSource::primaryButtonDown() noexcept { return false; }
bool FileDragSource::start(std::string_view) noexcept { return false; }
std::string FileDragSource::pointerDebug(std::uintptr_t) { return {}; }
void FileDragSource::log(std::string_view) noexcept {}
bool FileDragSource::active() noexcept { return false; }

} // namespace sms::ui
