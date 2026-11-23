#include "UndoKeyHook.hpp"
#include "FileDragSource.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace sms::ui {
namespace {

class Win32UndoKeyHook;
std::mutex gMutex;
std::vector<Win32UndoKeyHook*> gHooks;

class Win32UndoKeyHook final : public UndoKeyHook {
public:
    Win32UndoKeyHook(const HWND window, Callback callback) : window_(window), callback_(std::move(callback)) {}

    bool installHooks() noexcept
    {
        const DWORD thread = GetCurrentThreadId();
        keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD, &keyboardProc, nullptr, thread);
        messageHook_ = SetWindowsHookExW(WH_GETMESSAGE, &messageProc, nullptr, thread);
        if (keyboardHook_ == nullptr && messageHook_ == nullptr) return false;
        std::lock_guard lock(gMutex);
        gHooks.push_back(this);
        return true;
    }

    ~Win32UndoKeyHook() override
    {
        {
            std::lock_guard lock(gMutex);
            gHooks.erase(std::remove(gHooks.begin(), gHooks.end(), this), gHooks.end());
        }
        if (keyboardHook_ != nullptr) UnhookWindowsHookEx(keyboardHook_);
        if (messageHook_ != nullptr) UnhookWindowsHookEx(messageHook_);
    }

    /** Our window (or a child of it) has the keyboard focus, or it is the active window under the cursor. */
    bool ours() const noexcept
    {
        if (window_ == nullptr || IsWindow(window_) == FALSE || IsWindowVisible(window_) == FALSE) return false;
        const HWND focus = GetFocus();
        if (focus != nullptr && (focus == window_ || IsChild(window_, focus) != FALSE)) return true;
        POINT cursor{};
        RECT area{};
        if (!GetCursorPos(&cursor) || !GetWindowRect(window_, &area) || PtInRect(&area, cursor) == FALSE) return false;
        const HWND root = GetAncestor(window_, GA_ROOT);
        return root != nullptr && (GetForegroundWindow() == root || GetActiveWindow() == root);
    }

    /** Returns true when the keystroke was ours and has been handled. */
    bool handle(const std::uint32_t vk, const bool down, const char*) noexcept
    {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (!down) {
            if ((vk == 'Z' || vk == 'Y') && pendingUp_ == vk) { pendingUp_ = 0U; return true; }
            return false;
        }
        const auto key = classifyHistoryKey(vk, ctrl, shift, alt);
        if (key == HistoryKey::none || !ours()) return false;
        bool handled = false;
        try {
            handled = callback_ && callback_(key);
        } catch (...) {
        }
        if (handled) pendingUp_ = vk;
        return handled;
    }

    bool swallowChar(const std::uint32_t character) noexcept
    {
        return (character == 26U || character == 25U) && (GetKeyState(VK_CONTROL) & 0x8000) != 0 && ours();
    }

    static std::vector<Win32UndoKeyHook*> snapshot()
    {
        std::lock_guard lock(gMutex);
        return gHooks;
    }

private:
    static LRESULT CALLBACK keyboardProc(const int code, const WPARAM wParam, const LPARAM lParam)
    {
        if (code == HC_ACTION) {
            const bool down = (static_cast<std::uint32_t>(lParam) & 0x80000000U) == 0U;
            for (auto* hook : snapshot())
                if (hook->handle(static_cast<std::uint32_t>(wParam), down, "WH_KEYBOARD")) return 1;
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    static LRESULT CALLBACK messageProc(const int code, const WPARAM wParam, const LPARAM lParam)
    {
        if (code == HC_ACTION && wParam == PM_REMOVE && lParam != 0) {
            auto* msg = reinterpret_cast<MSG*>(lParam);
            const bool down = msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN;
            const bool up = msg->message == WM_KEYUP || msg->message == WM_SYSKEYUP;
            for (auto* hook : snapshot()) {
                if ((down || up) && hook->handle(static_cast<std::uint32_t>(msg->wParam), down, "WH_GETMESSAGE")) {
                    msg->message = WM_NULL;
                    break;
                }
                if (msg->message == WM_CHAR && hook->swallowChar(static_cast<std::uint32_t>(msg->wParam))) {
                    msg->message = WM_NULL;
                    break;
                }
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    HWND window_;
    Callback callback_;
    HHOOK keyboardHook_ = nullptr;
    HHOOK messageHook_ = nullptr;
    std::uint32_t pendingUp_ = 0U;
};

} // namespace

std::unique_ptr<UndoKeyHook> UndoKeyHook::install(const std::uintptr_t nativeWindow, Callback callback) noexcept
{
    try {
        const auto window = reinterpret_cast<HWND>(nativeWindow);
        if (window == nullptr || IsWindow(window) == FALSE || !callback) return nullptr;
        auto hook = std::make_unique<Win32UndoKeyHook>(window, std::move(callback));
        if (!hook->installHooks()) return nullptr;
        return hook;
    } catch (...) {
        return nullptr;
    }
}

} // namespace sms::ui
