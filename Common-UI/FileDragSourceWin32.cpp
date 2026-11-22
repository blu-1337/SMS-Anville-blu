#include "FileDragSource.hpp"
#include "FileDragWin32.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <ole2.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>
#include <string>

namespace sms::ui {
namespace {

std::atomic<bool> gActive{false};

CLIPFORMAT preferredEffectFormat() noexcept
{
    static const CLIPFORMAT format = static_cast<CLIPFORMAT>(RegisterClipboardFormatA(CFSTR_PREFERREDDROPEFFECT));
    return format;
}

bool wideFromUtf8(const std::string_view utf8, std::wstring& wide)
{
    if (utf8.empty()) return false;
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) return false;
    wide.assign(static_cast<std::size_t>(size), L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                               static_cast<int>(utf8.size()), wide.data(), size) == size;
}

std::string hex(const unsigned long value)
{
    char buffer[16]{};
    std::snprintf(buffer, sizeof buffer, "%08lX", value);
    return buffer;
}

class DragSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override
    {
        if (object == nullptr) return E_POINTER;
        if (IsEqualIID(id, IID_IUnknown) || IsEqualIID(id, IID_IDropSource)) {
            *object = static_cast<IDropSource*>(this);
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
        if (remaining == 0U) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(const BOOL escape, const DWORD keys) override
    {
        if (escape) return DRAGDROP_S_CANCEL;
        if ((keys & MK_LBUTTON) == 0U) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    std::atomic<ULONG> references_{1U};
};

/** One CF_HDROP file; every other format is refused. */
class FileDataObject final : public IDataObject {
public:
    explicit FileDataObject(std::wstring path) : path_(std::move(path)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override
    {
        if (object == nullptr) return E_POINTER;
        if (IsEqualIID(id, IID_IUnknown) || IsEqualIID(id, IID_IDataObject)) {
            *object = static_cast<IDataObject*>(this);
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
        if (remaining == 0U) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override
    {
        if (format == nullptr || medium == nullptr) return E_POINTER;
        if (QueryGetData(format) != S_OK) return DV_E_FORMATETC;
        if (format->cfFormat == preferredEffectFormat()) {
            HGLOBAL effect = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
            if (effect == nullptr) return E_OUTOFMEMORY;
            if (auto* value = static_cast<DWORD*>(GlobalLock(effect))) { *value = DROPEFFECT_COPY; GlobalUnlock(effect); }
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = effect;
            medium->pUnkForRelease = nullptr;
            return S_OK;
        }
        const SIZE_T bytes = sizeof(DROPFILES) + (path_.size() + 2U) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        if (memory == nullptr) return E_OUTOFMEMORY;
        auto* files = static_cast<DROPFILES*>(GlobalLock(memory));
        if (files == nullptr) { GlobalFree(memory); return E_OUTOFMEMORY; }
        files->pFiles = sizeof(DROPFILES);
        files->fWide = TRUE;
        std::memcpy(reinterpret_cast<char*>(files) + sizeof(DROPFILES), path_.c_str(),
                    path_.size() * sizeof(wchar_t)); // the zeroed tail gives the double terminator
        GlobalUnlock(memory);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
    {
        if (format == nullptr) return E_POINTER;
        if ((format->cfFormat != CF_HDROP && format->cfFormat != preferredEffectFormat()) ||
            (format->tymed & TYMED_HGLOBAL) == 0 || format->dwAspect != DVASPECT_CONTENT)
            return DV_E_FORMATETC;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override
    {
        if (out != nullptr) out->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(const DWORD direction, IEnumFORMATETC** result) override
    {
        if (direction != DATADIR_GET) return E_NOTIMPL;
        FORMATETC formats[2] = {{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
                                {preferredEffectFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}};
        return SHCreateStdEnumFmtEtc(2U, formats, result);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
    { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    std::atomic<ULONG> references_{1U};
    std::wstring path_;
};

} // namespace

IDataObject* win32::createFileDataObject(const std::wstring& path) noexcept
{
    return new (std::nothrow) FileDataObject(path);
}

void FileDragSource::log(const std::string_view file, const std::string_view message) noexcept
{
    try {
        std::error_code error;
        const auto base = std::filesystem::temp_directory_path(error);
        if (error) return;
        const auto directory = base / "SMS-Anville" / "drag";
        std::filesystem::create_directories(directory, error);
        std::ofstream out(directory / std::string(file), std::ios::app);
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char stamp[32]{};
        std::tm local{};
        localtime_s(&local, &now);
        std::strftime(stamp, sizeof stamp, "%H:%M:%S", &local);
        out << stamp << ' ' << message << '\n';
    } catch (...) {
    }
}

bool FileDragSource::pointerOutsideWindow(const std::uintptr_t nativeWindow) noexcept
{
    const auto window = reinterpret_cast<HWND>(nativeWindow);
    POINT cursor{};
    RECT client{};
    if (window == nullptr || IsWindow(window) == FALSE || !GetCursorPos(&cursor) ||
        !GetClientRect(window, &client) || !ScreenToClient(window, &cursor))
        return false;
    return cursor.x < client.left || cursor.y < client.top || cursor.x >= client.right ||
           cursor.y >= client.bottom;
}

bool FileDragSource::primaryButtonDown() noexcept
{
    return (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0;
}

bool FileDragSource::active() noexcept { return gActive.load(); }

bool FileDragSource::start(const std::string_view utf8Path) noexcept
{
    try {
        std::wstring path;
        if (gActive.load() || !wideFromUtf8(utf8Path, path)) return false;
        const HRESULT initialized = OleInitialize(nullptr);
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return false;
        auto* const data = new (std::nothrow) FileDataObject(std::move(path));
        auto* const source = new (std::nothrow) DragSource();
        bool dropped = false;
        if (data != nullptr && source != nullptr) {
            ReleaseCapture(); // the window owns the mouse until the OLE loop takes over
            gActive.store(true);
            DWORD effect = DROPEFFECT_NONE;
            const HRESULT result = DoDragDrop(data, source, DROPEFFECT_COPY, &effect);
            gActive.store(false);
            dropped = result == DRAGDROP_S_DROP && (effect & DROPEFFECT_COPY) != 0U;
        }
        if (data != nullptr) data->Release();
        if (source != nullptr) source->Release();
        if (SUCCEEDED(initialized)) OleUninitialize();
        return dropped;
    } catch (...) {
        gActive.store(false);
        return false;
    }
}

} // namespace sms::ui
