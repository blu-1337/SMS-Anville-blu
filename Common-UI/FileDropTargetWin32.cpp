// Win32 OLE drop target. COM calls arrive on the host UI thread (STA), the
// same thread that delivers the editor's window events.
#include "FileDropWin32.hpp"
#include "FileDropEffect.hpp"

#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <new>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace sms::ui {
namespace {

static_assert(kDropNone == DROPEFFECT_NONE && kDropCopy == DROPEFFECT_COPY &&
              kDropMove == DROPEFFECT_MOVE && kDropLink == DROPEFFECT_LINK,
              "portable drop effects mirror the OLE DROPEFFECT bits");

constexpr UINT kMaximumDroppedPaths = 1024U;
constexpr auto kStaleCopyAge = std::chrono::hours(24);

// Private copies are named <pid>-<counter>.wav.
[[nodiscard]] bool isPrivateCopyName(const std::wstring& name) noexcept
{
    constexpr std::wstring_view extension = L".wav";
    if (name.size() <= extension.size() ||
        name.compare(name.size() - extension.size(), extension.size(), extension) != 0)
        return false;
    const auto stem = std::wstring_view(name).substr(0U, name.size() - extension.size());
    const auto dash = stem.find(L'-');
    if (dash == std::wstring_view::npos || dash == 0U || dash + 1U == stem.size())
        return false;
    for (std::size_t index = 0; index < stem.size(); ++index) {
        if (index != dash && (stem[index] < L'0' || stem[index] > L'9'))
            return false;
    }
    return true;
}

[[nodiscard]] bool isPrivateCopy(const std::filesystem::path& path)
{
    const auto directory = win32::privateDropDirectory();
    if (directory.empty() || !path.has_filename() || !isPrivateCopyName(path.filename().native()))
        return false;
    const auto parent = path.parent_path().lexically_normal().native();
    const auto expected = directory.lexically_normal().native();
    return CompareStringOrdinal(parent.c_str(), -1, expected.c_str(), -1, TRUE) == CSTR_EQUAL;
}

void removeStaleCopies() noexcept
{
    try {
        const auto directory = win32::privateDropDirectory();
        if (directory.empty())
            return;
        std::error_code error;
        std::filesystem::directory_iterator entry(directory, error);
        const auto now = std::filesystem::file_time_type::clock::now();
        for (; !error && entry != std::filesystem::directory_iterator(); entry.increment(error)) {
            std::error_code fileError;
            if (!entry->is_regular_file(fileError) || !isPrivateCopyName(entry->path().filename().native()))
                continue;
            const auto written = entry->last_write_time(fileError);
            if (!fileError && now - written > kStaleCopyAge)
                std::filesystem::remove(entry->path(), fileError);
        }
    } catch (...) {
    }
}

[[nodiscard]] bool makePrivateCopy(const std::string& source, std::string& copy)
{
    std::wstring wideSource;
    if (!win32::wideFromUtf8(source, wideSource) || wideSource.empty())
        return false;
    const auto directory = win32::privateDropDirectory();
    if (directory.empty())
        return false;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;
    static std::atomic<std::uint32_t> counter{0U};
    const auto name = std::to_wstring(GetCurrentProcessId()) + L"-" +
                      std::to_wstring(counter.fetch_add(1U) + 1U) + L".wav";
    const auto target = directory / name;
    if (CopyFileW(wideSource.c_str(), target.c_str(), FALSE) == FALSE)
        return false;
    // CopyFileW keeps the source's write time; refresh it so another editor's
    // stale-copy sweep cannot delete a fresh copy of an old file.
    std::filesystem::last_write_time(target, std::filesystem::file_time_type::clock::now(), error);
    if (!win32::utf8FromWide(target.native(), copy)) {
        std::filesystem::remove(target, error);
        return false;
    }
    return true;
}

[[nodiscard]] std::vector<std::string> readDroppedPaths(IDataObject* const data)
{
    std::vector<std::string> paths;
    if (data == nullptr)
        return paths;
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&format, &medium)))
        return paths;
    struct MediumGuard {
        STGMEDIUM& medium;
        ~MediumGuard() { ReleaseStgMedium(&medium); }
    } mediumGuard{medium};
    if (medium.tymed != TYMED_HGLOBAL || medium.hGlobal == nullptr)
        return paths;
    const auto drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
    if (drop == nullptr)
        return paths;
    struct LockGuard {
        HGLOBAL global;
        ~LockGuard() { GlobalUnlock(global); }
    } lockGuard{medium.hGlobal};

    const UINT count = DragQueryFileW(drop, 0xFFFFFFFFU, nullptr, 0U);
    const UINT limit = count < kMaximumDroppedPaths ? count : kMaximumDroppedPaths;
    paths.reserve(limit);
    std::wstring wide;
    std::string utf8;
    for (UINT index = 0U; index < limit; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0U);
        if (length == 0U)
            continue;
        wide.assign(length, L'\0');
        if (DragQueryFileW(drop, index, wide.data(), length + 1U) != length)
            continue;
        if (win32::utf8FromWide(wide, utf8) && !utf8.empty())
            paths.push_back(utf8);
    }
    return paths;
}

class OleDropTarget final : public IDropTarget {
public:
    OleDropTarget(const HWND window, FileDropListener& listener) noexcept
        : window_(window), listener_(&listener) {}

    void detach() noexcept
    {
        listener_ = nullptr;
        paths_.clear();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override
    {
        if (object == nullptr)
            return E_POINTER;
        if (IsEqualIID(id, IID_IUnknown) || IsEqualIID(id, IID_IDropTarget)) {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return references_.fetch_add(1U) + 1U; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = references_.fetch_sub(1U) - 1U;
        if (remaining == 0U)
            delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL point, DWORD* effect) override
    {
        try {
            paths_ = readDroppedPaths(data);
            return dragOver(point, effect);
        } catch (...) {
            reject(effect);
            return E_UNEXPECTED;
        }
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD* effect) override
    {
        try {
            return dragOver(point, effect);
        } catch (...) {
            reject(effect);
            return E_UNEXPECTED;
        }
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        try {
            paths_.clear();
            if (listener_ != nullptr)
                listener_->fileDragLeave();
            return S_OK;
        } catch (...) {
            return E_UNEXPECTED;
        }
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL point, DWORD* effect) override
    {
        try {
            if (effect == nullptr)
                return E_INVALIDARG;
            const DWORD allowed = *effect;
            *effect = DROPEFFECT_NONE;
            paths_.clear();
            if (listener_ == nullptr)
                return S_OK;
            // Re-read from the object passed to Drop; DragEnter's list may be stale.
            auto paths = readDroppedPaths(data);
            const FileDropPoint client = clientPoint(point);
            const DWORD chosen = chooseDropEffect(
                allowed, !paths.empty() && listener_->fileDragOver(client, paths));
            if (chosen != DROPEFFECT_NONE && dropNeedsPrivateCopy(chosen)) {
                // MOVE/LINK sources (FL Studio Edison) may delete their file
                // once Drop returns, before the worker reads it.
                std::vector<std::string> copies;
                for (const auto& path : paths) {
                    std::string copy;
                    if (isSupportedAudioPath(path) && makePrivateCopy(path, copy))
                        copies.push_back(std::move(copy));
                }
                paths = std::move(copies);
            }
            if (chosen == DROPEFFECT_NONE || paths.empty() || listener_ == nullptr) {
                if (listener_ != nullptr)
                    listener_->fileDragLeave();
                return S_OK;
            }
            listener_->filesDropped(client, paths);
            *effect = reportedDropEffect(chosen);
            return S_OK;
        } catch (...) {
            reject(effect);
            return E_UNEXPECTED;
        }
    }

private:
    ~OleDropTarget() = default;

    static void reject(DWORD* const effect) noexcept
    {
        if (effect != nullptr)
            *effect = DROPEFFECT_NONE;
    }

    HRESULT dragOver(const POINTL point, DWORD* const effect)
    {
        if (effect == nullptr)
            return E_INVALIDARG;
        const bool accept = listener_ != nullptr && !paths_.empty() &&
                            listener_->fileDragOver(clientPoint(point), paths_);
        // The Edison fix: answer with an effect the source allows.
        *effect = chooseDropEffect(*effect, accept);
        return S_OK;
    }

    [[nodiscard]] FileDropPoint clientPoint(const POINTL point) const noexcept
    {
        POINT client{point.x, point.y};
        ScreenToClient(window_, &client);
        return {static_cast<double>(client.x), static_cast<double>(client.y)};
    }

    std::atomic<ULONG> references_{1U};
    const HWND window_;
    FileDropListener* listener_;
    std::vector<std::string> paths_;
};

class Win32FileDropTarget final : public FileDropTarget {
public:
    Win32FileDropTarget(const HWND window, OleDropTarget* const target) noexcept
        : window_(window), target_(target) {}

    ~Win32FileDropTarget() override
    {
        target_->detach();
        RevokeDragDrop(window_);
        target_->Release();
        OleUninitialize();
    }

private:
    const HWND window_;
    OleDropTarget* const target_;
};

} // namespace

std::filesystem::path win32::privateDropDirectory()
{
    std::wstring buffer(MAX_PATH + 1U, L'\0');
    const DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (length == 0U || length >= buffer.size())
        return {};
    buffer.resize(length);
    return std::filesystem::path(buffer) / L"SMS-Anville" / L"drops";
}

IDropTarget* win32::createOleDropTarget(const HWND window, FileDropListener& listener) noexcept
{
    return new (std::nothrow) OleDropTarget(window, listener);
}

std::unique_ptr<FileDropTarget> FileDropTarget::attach(
    const std::uintptr_t nativeWindow, FileDropListener& listener) noexcept
{
    const auto window = reinterpret_cast<HWND>(nativeWindow);
    if (window == nullptr || IsWindow(window) == FALSE)
        return nullptr;
    const HRESULT initialized = OleInitialize(nullptr);
    if (initialized == RPC_E_CHANGED_MODE) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            OutputDebugStringA("SMS file drop: the UI thread is not single-threaded COM; drop disabled\n");
        return nullptr;
    }
    if (FAILED(initialized))
        return nullptr;
    removeStaleCopies();
    auto* const target = new (std::nothrow) OleDropTarget(window, listener);
    if (target == nullptr) {
        OleUninitialize();
        return nullptr;
    }
    if (FAILED(RegisterDragDrop(window, target))) {
        target->Release();
        OleUninitialize();
        return nullptr;
    }
    std::unique_ptr<FileDropTarget> owner(new (std::nothrow) Win32FileDropTarget(window, target));
    if (owner == nullptr) {
        RevokeDragDrop(window);
        target->Release();
        OleUninitialize();
    }
    return owner;
}

void FileDropTarget::releaseDroppedFile(const std::string_view utf8Path) noexcept
{
    try {
        std::wstring wide;
        if (utf8Path.empty() || !win32::wideFromUtf8(utf8Path, wide))
            return;
        const std::filesystem::path path(wide);
        if (!isPrivateCopy(path))
            return;
        std::error_code error;
        std::filesystem::remove(path, error);
    } catch (...) {
    }
}

} // namespace sms::ui
