#include "UndoKeyHook.hpp"

namespace sms::ui {

std::unique_ptr<UndoKeyHook> UndoKeyHook::install(std::uintptr_t, Callback) noexcept { return nullptr; }

} // namespace sms::ui
