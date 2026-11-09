#include "FileDropEffect.hpp"
#include "FileDropTarget.hpp"

#if defined(_WIN32)
# include "FileDropWin32.hpp"
# include <shlobj.h>

# include <chrono>
# include <cstddef>
# include <cstring>
# include <filesystem>
# include <fstream>
# include <iterator>
# include <vector>
#endif

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
using namespace sms::ui;

void check(const bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

// Every allowed mask from the drag source, with and without acceptance.
void effectChooser()
{
    struct Case {
        std::uint32_t allowed;
        std::uint32_t expected;
    };
    constexpr std::array cases{
        Case{kDropNone, kDropNone},
        Case{kDropCopy, kDropCopy},
        Case{kDropMove, kDropMove},
        Case{kDropCopy | kDropMove, kDropCopy},
        Case{kDropLink, kDropLink},
        Case{kDropCopy | kDropLink, kDropCopy},
        Case{kDropMove | kDropLink, kDropMove},
        Case{kDropCopy | kDropMove | kDropLink, kDropCopy},
    };
    for (const auto& entry : cases) {
        check(chooseDropEffect(entry.allowed, true) == entry.expected,
              "accepted drops choose COPY, then MOVE, then LINK from the allowed mask");
        check(chooseDropEffect(entry.allowed, false) == kDropNone,
              "a rejected drop never reports an effect");
        check((chooseDropEffect(entry.allowed, true) & ~entry.allowed) == 0U,
              "the chosen effect is always one the source allows");
    }
    static_assert(chooseDropEffect(kDropMove, true) == kDropMove,
                  "the Edison case: a MOVE-only source is accepted as MOVE, never as COPY");
    check(chooseDropEffect(0x80000000U | kDropMove, true) == kDropMove,
          "unknown source bits (DROPEFFECT_SCROLL) are ignored");

    check(reportedDropEffect(kDropCopy) == kDropCopy, "a copy is reported as COPY");
    check(reportedDropEffect(kDropMove) == kDropNone,
          "a move is reported as NONE so the source never deletes its original");
    check(reportedDropEffect(kDropLink) == kDropNone && reportedDropEffect(kDropNone) == kDropNone,
          "link and none are reported as NONE");
    check(!dropNeedsPrivateCopy(kDropCopy) && dropNeedsPrivateCopy(kDropMove) &&
          dropNeedsPrivateCopy(kDropLink) && !dropNeedsPrivateCopy(kDropNone),
          "only MOVE and LINK sources are copied before the asynchronous import");
}

void extensionFilter()
{
    check(isSupportedAudioPath("C:\\Samples\\kick.wav"), "lowercase .wav");
    check(isSupportedAudioPath("/tmp/KICK.WAV"), "uppercase .WAV");
    check(isSupportedAudioPath("C:\\Samples\\loop.Wave"), "mixed-case .wave");
    check(isSupportedAudioPath("C:\\Samples\\音源.wav"), "non-ASCII name");
    check(!isSupportedAudioPath("C:\\Samples\\song.mp3"), "mp3 is rejected");
    check(!isSupportedAudioPath("C:\\Samples\\wav"), "no dot");
    check(!isSupportedAudioPath("C:\\Samples\\.wav"), "a bare extension is not a file name");
    check(!isSupportedAudioPath("/tmp/.wav"), "a bare extension after a slash is not a file name");
    check(!isSupportedAudioPath(".wav") && !isSupportedAudioPath(""), "empty or bare names");
    check(!isSupportedAudioPath("C:\\Samples\\kick.wav.txt"), "only the final extension counts");
    check(!isSupportedAudioPath("C:\\Samples\\kick.wa"), "truncated extension");
}

void nullTargetIsSafe()
{
#if !defined(_WIN32)
    struct Listener final : FileDropListener {
        bool fileDragOver(FileDropPoint, std::span<const std::string>) override { return true; }
        void fileDragLeave() override {}
        void filesDropped(FileDropPoint, std::span<const std::string>) override {}
    } listener;
    check(FileDropTarget::attach(1U, listener) == nullptr,
          "platforms without native drop support report no target");
#endif
    FileDropTarget::releaseDroppedFile("");
    FileDropTarget::releaseDroppedFile("C:\\not-a-private-copy.wav");
}

#if defined(_WIN32)
namespace fs = std::filesystem;

void textConversion()
{
    const std::string utf8 = "C:\\Samples\\\xE9\x9F\xB3\xE6\xBA\x90 kick.wav";
    std::wstring wide;
    std::string back;
    check(win32::wideFromUtf8(utf8, wide) && wide == L"C:\\Samples\\\u97F3\u6E90 kick.wav",
          "UTF-8 paths convert to UTF-16");
    check(win32::utf8FromWide(wide, back) && back == utf8, "UTF-16 paths convert back to UTF-8");
    check(!win32::wideFromUtf8("bad \xC3\x28 byte", wide), "invalid UTF-8 fails instead of being replaced");
    check(!win32::utf8FromWide(std::wstring(1U, static_cast<wchar_t>(0xD800)), back),
          "a lone surrogate fails instead of being replaced");
    check(win32::wideFromUtf8("", wide) && wide.empty() && win32::utf8FromWide(L"", back) && back.empty(),
          "empty text converts to empty text");
}

// A minimal CF_HDROP source, like Explorer or a host browser.
class HdropSource final : public IDataObject {
public:
    explicit HdropSource(const std::vector<std::wstring>& paths)
    {
        payload_.resize(sizeof(DROPFILES));
        for (const auto& path : paths) {
            const auto* bytes = reinterpret_cast<const std::byte*>(path.c_str());
            payload_.insert(payload_.end(), bytes, bytes + (path.size() + 1U) * sizeof(wchar_t));
        }
        payload_.resize(payload_.size() + sizeof(wchar_t));
        DROPFILES header{};
        header.pFiles = sizeof(DROPFILES);
        header.fWide = TRUE;
        std::memcpy(payload_.data(), &header, sizeof(header));
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override
    {
        if (IsEqualIID(id, IID_IUnknown) || IsEqualIID(id, IID_IDataObject)) {
            *object = static_cast<IDataObject*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1U; }
    ULONG STDMETHODCALLTYPE Release() override { return 1U; }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override
    {
        if (format->cfFormat != CF_HDROP || (format->tymed & TYMED_HGLOBAL) == 0U)
            return DV_E_FORMATETC;
        const HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, payload_.size());
        if (global == nullptr)
            return E_OUTOFMEMORY;
        std::memcpy(GlobalLock(global), payload_.data(), payload_.size());
        GlobalUnlock(global);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = global;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
    {
        return format->cfFormat == CF_HDROP ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    std::vector<std::byte> payload_;
};

struct RecordingListener final : FileDropListener {
    bool accept = true;
    int overs = 0;
    int leaves = 0;
    std::vector<std::string> seen;
    std::vector<std::string> dropped;

    bool fileDragOver(FileDropPoint, std::span<const std::string> paths) override
    {
        ++overs;
        seen.assign(paths.begin(), paths.end());
        return accept;
    }
    void fileDragLeave() override { ++leaves; }
    void filesDropped(FileDropPoint, std::span<const std::string> paths) override
    {
        dropped.assign(paths.begin(), paths.end());
    }
};

[[nodiscard]] std::string utf8(const fs::path& path)
{
    std::string text;
    check(win32::utf8FromWide(path.native(), text), "test paths convert to UTF-8");
    return text;
}

void writeFile(const fs::path& path, const std::string& bytes)
{
    std::ofstream(path, std::ios::binary) << bytes;
}

[[nodiscard]] std::string readFile(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

// Drives the COM object the way OLE does, for each source effect mask.
void oleDropTarget()
{
    const auto folder = fs::temp_directory_path() / L"sms-file-drop-test";
    fs::remove_all(folder);
    fs::create_directories(folder);
    const auto wave = folder / L"\u97F3\u6E90 loop.wav";
    const auto text = folder / L"notes.txt";
    writeFile(wave, "RIFF-test-bytes");
    writeFile(text, "not audio");
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(wave, now - std::chrono::hours(48));

    HdropSource source({wave.native(), text.native()});
    RecordingListener listener;
    IDropTarget* const target = win32::createOleDropTarget(GetDesktopWindow(), listener);
    check(target != nullptr, "the OLE target is created");
    void* unknown = nullptr;
    check(target->QueryInterface(IID_IDropTarget, &unknown) == S_OK && unknown == target,
          "the target answers IDropTarget");
    target->Release();
    check(target->QueryInterface(IID_IDataObject, &unknown) == E_NOINTERFACE && unknown == nullptr,
          "the target refuses other interfaces");

    DWORD effect = DROPEFFECT_MOVE;
    check(target->DragEnter(&source, 0, POINTL{10, 20}, &effect) == S_OK && effect == DROPEFFECT_MOVE,
          "a MOVE-only source (Edison) is answered with MOVE, not COPY");
    check(listener.overs == 1 && listener.seen.size() == 2U && listener.seen[0] == utf8(wave),
          "the listener sees every dropped path as UTF-8");
    listener.accept = false;
    effect = DROPEFFECT_MOVE;
    check(target->DragOver(0, POINTL{10, 20}, &effect) == S_OK && effect == DROPEFFECT_NONE,
          "a rejected position answers NONE");
    listener.accept = true;
    effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    check(target->DragOver(0, POINTL{10, 20}, &effect) == S_OK && effect == DROPEFFECT_COPY,
          "COPY is preferred when the source allows it");
    check(target->DragOver(0, POINTL{10, 20}, nullptr) == E_INVALIDARG, "a null effect is refused");
    check(target->DragLeave() == S_OK && listener.leaves == 1, "leaving reaches the listener");

    effect = DROPEFFECT_MOVE;
    check(target->DragEnter(&source, 0, POINTL{10, 20}, &effect) == S_OK, "the drag re-enters");
    effect = DROPEFFECT_MOVE;
    check(target->Drop(&source, 0, POINTL{10, 20}, &effect) == S_OK && effect == DROPEFFECT_NONE,
          "a completed MOVE is reported as NONE so the source keeps its original");
    check(listener.dropped.size() == 1U, "only the WAV is copied for a MOVE source");
    std::wstring copyWide;
    check(win32::wideFromUtf8(listener.dropped[0], copyWide), "the copy path is UTF-8");
    const fs::path privateCopy(copyWide);
    check(privateCopy.parent_path() == win32::privateDropDirectory(), "the copy is in the private folder");
    check(readFile(privateCopy) == "RIFF-test-bytes", "the copy has the dropped bytes");
    check(fs::last_write_time(privateCopy) > now - std::chrono::hours(1),
          "the copy is dated now, so the stale sweep keeps it");
    check(fs::exists(wave), "the source file is untouched");
    FileDropTarget::releaseDroppedFile(utf8(wave));
    check(fs::exists(wave), "release never deletes a file outside the private folder");
    FileDropTarget::releaseDroppedFile(listener.dropped[0]);
    check(!fs::exists(privateCopy), "release deletes the private copy");

    listener.dropped.clear();
    effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    check(target->Drop(&source, 0, POINTL{10, 20}, &effect) == S_OK && effect == DROPEFFECT_COPY,
          "a COPY drop is reported as COPY");
    check(listener.dropped.size() == 2U && listener.dropped[0] == utf8(wave),
          "a COPY drop passes the original paths without copying");

    listener.dropped.clear();
    listener.accept = false;
    const int leaves = listener.leaves;
    effect = DROPEFFECT_COPY;
    check(target->Drop(&source, 0, POINTL{10, 20}, &effect) == S_OK && effect == DROPEFFECT_NONE &&
              listener.dropped.empty() && listener.leaves == leaves + 1,
          "a rejected drop ends as a leave");
    target->Release();
    fs::remove_all(folder);
}

// Registration on a real window, the stale-copy sweep and revocation.
void windowRegistration()
{
    RecordingListener listener;
    check(FileDropTarget::attach(0U, listener) == nullptr, "no window, no target");

    const auto drops = win32::privateDropDirectory();
    fs::create_directories(drops);
    const auto stale = drops / L"99999999-1.wav";
    const auto foreign = drops / L"keep-me.wav";
    writeFile(stale, "old");
    writeFile(foreign, "old");
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours(48);
    fs::last_write_time(stale, old);
    fs::last_write_time(foreign, old);
    FileDropTarget::releaseDroppedFile(utf8(foreign));
    check(fs::exists(foreign), "release ignores names that are not private copies");

    const HWND window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                                        GetModuleHandleW(nullptr), nullptr);
    check(window != nullptr, "the test window is created");
    {
        auto attached = FileDropTarget::attach(reinterpret_cast<std::uintptr_t>(window), listener);
        check(attached != nullptr, "a real window accepts registration");
        check(!fs::exists(stale), "attach sweeps private copies older than a day");
        check(fs::exists(foreign), "the sweep ignores other files");
    }
    IDropTarget* const probe = win32::createOleDropTarget(window, listener);
    check(RegisterDragDrop(window, probe) == S_OK, "destroying the target revokes the window");
    RevokeDragDrop(window);
    probe->Release();
    DestroyWindow(window);
    fs::remove(foreign);
}
#endif

} // namespace

int main()
{
    effectChooser();
    extensionFilter();
    nullTargetIsSafe();
#if defined(_WIN32)
    check(SUCCEEDED(OleInitialize(nullptr)), "the test thread joins a single-threaded apartment");
    textConversion();
    oleDropTarget();
    windowRegistration();
    OleUninitialize();
#endif
    std::cout << "File drop tests passed\n";
}
