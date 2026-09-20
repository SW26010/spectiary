#include "platform/win32_file_drop_target.h"
#include <shellapi.h>
#include <shlobj.h>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace spectiary {
struct Win32FileDropTargetTestAccess {
    static IDropTarget* Receiver(Win32FileDropTarget& target) { return target.Receiver(); }
};
}
namespace {
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class FileData final : public IDataObject {
public:
    std::vector<std::filesystem::path> paths;
    bool supported = true;
    bool fail_read = false;
    int reads = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDataObject) return E_NOINTERFACE;
        *out = static_cast<IDataObject*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
    { return supported && format->cfFormat == CF_HDROP ? S_OK : DV_E_FORMATETC; }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override
    {
        ++reads;
        if (fail_read || FAILED(QueryGetData(format))) return E_FAIL;
        std::wstring list;
        for (const auto& path : paths) { list += path.native(); list += L'\0'; }
        list += L'\0';
        const auto bytes = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GHND, bytes);
        if (!memory) return E_OUTOFMEMORY;
        auto* header = static_cast<DROPFILES*>(GlobalLock(memory));
        header->pFiles = sizeof(DROPFILES);
        header->fWide = TRUE;
        std::memcpy(reinterpret_cast<char*>(header) + sizeof(DROPFILES), list.data(),
            list.size() * sizeof(wchar_t));
        GlobalUnlock(memory);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};

void TestShellDrop()
{
    using namespace spectiary;
    bool inside = true;
    bool hover = false;
    Win32FileDropTarget target([&](float x, float y) {
        return inside && x >= 100 && x < 200 && y >= 100 && y < 200;
    }, [&](bool active) { hover = active; });
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"SpectiaryFileDropTest";
    Require(RegisterClassW(&window_class) != 0, "test window class registration");
    struct Window {
        HWND value = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"SpectiaryFileDropTest", L"Drop test",
            WS_POPUP, 100, 100, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~Window() { if (value) DestroyWindow(value); }
    } window;
    Require(window.value != nullptr, "test window creation");
    ShowWindow(window.value, SW_SHOWNOACTIVATE);
    target.SetWindow(window.value);
    auto* receiver = Win32FileDropTargetTestAccess::Receiver(target);
    FileData data;
    data.paths = {L"C:\\data with spaces\\星光.csv", L"C:\\光谱 文件夹", L"C:\\unsupported.xyz"};
    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    Require(SUCCEEDED(receiver->DragEnter(&data, 0, {150, 150}, &effect)) &&
        effect == DROPEFFECT_COPY && hover, "compatible shell drag shows copy feedback");
    effect = DROPEFFECT_COPY;
    receiver->DragOver(0, {200, 150}, &effect);
    Require(effect == DROPEFFECT_NONE && !hover, "outside target is rejected");
    effect = DROPEFFECT_COPY;
    receiver->Drop(&data, 0, {150, 150}, &effect);
    Require(effect == DROPEFFECT_COPY && !hover && data.reads == 1,
        "drop reads filesystem paths only once and clears feedback");
    effect = DROPEFFECT_COPY;
    receiver->DragEnter(&data, 0, {150, 150}, &effect);
    Require(effect == DROPEFFECT_NONE, "one bounded pending batch at a time");
    Require(target.TakePaths() == data.paths, "Unicode, spaces, folders and unsupported paths preserve order verbatim");
    Require(target.TakePaths().empty(), "batch consumed exactly once");
    inside = false;
    effect = DROPEFFECT_COPY;
    receiver->Drop(&data, 0, {150, 150}, &effect);
    Require(effect == DROPEFFECT_NONE && target.TakePaths().empty(), "drop rechecks target at release");
    inside = true;
    data.supported = false;
    effect = DROPEFFECT_COPY;
    receiver->DragEnter(&data, 0, {150, 150}, &effect);
    Require(effect == DROPEFFECT_NONE && !hover, "non-filesystem payload is rejected");
    data.supported = true;
    data.fail_read = true;
    effect = DROPEFFECT_COPY;
    receiver->DragEnter(&data, 0, {150, 150}, &effect);
    receiver->Drop(&data, 0, {150, 150}, &effect);
    Require(effect == DROPEFFECT_NONE && target.TakePaths().empty(), "failed data transfer adds nothing");
    data.fail_read = false;
    data.paths.resize(Win32FileDropTarget::MaximumPaths + 1, L"C:\\a.csv");
    effect = DROPEFFECT_COPY;
    receiver->DragEnter(&data, 0, {150, 150}, &effect);
    receiver->Drop(&data, 0, {150, 150}, &effect);
    Require(effect == DROPEFFECT_NONE && target.TakePaths().empty(), "oversized batch is rejected atomically");
    effect = DROPEFFECT_MOVE;
    receiver->DragEnter(&data, 0, {150, 150}, &effect);
    Require(effect == DROPEFFECT_NONE, "never move or delete Explorer files");
    DestroyWindow(window.value);
    window.value = nullptr;
    target.SetWindow(nullptr);
    Require(!hover, "destroying a detached Files window revokes the target");
}
}
int main()
{
    try { TestShellDrop(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
