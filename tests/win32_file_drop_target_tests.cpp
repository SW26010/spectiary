#include "platform/win32_file_drop_target.h"
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <cstring>
#include <fstream>
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

Microsoft::WRL::ComPtr<IDataObject> ShellData(std::span<const std::filesystem::path> paths)
{
    struct IdLists {
        std::vector<PIDLIST_ABSOLUTE> values;
        ~IdLists() { for (const auto value : values) CoTaskMemFree(value); }
    } ids;
    for (const auto& path : paths) {
        PIDLIST_ABSOLUTE id = nullptr;
        Require(SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &id, 0, nullptr)), "parse shell test item");
        ids.values.push_back(id);
    }
    Microsoft::WRL::ComPtr<IShellItemArray> items;
    Require(SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.values.size()),
        const_cast<PCIDLIST_ABSOLUTE*>(ids.values.data()), &items)), "create shell test selection");
    Microsoft::WRL::ComPtr<IDataObject> data;
    Require(SUCCEEDED(items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data))), "create Explorer-style data object");
    return data;
}

void TestShellDrop()
{
    using namespace spectiary;
    bool inside = true;
    unsigned int hover = 0;
    unsigned int destination = 1;
    bool file_only_panel = false;
    HWND hit_window = nullptr;
    Win32FileDropTarget target([&](HWND window, float x, float y, bool all_files) {
        hit_window = window;
        return inside && (!file_only_panel || all_files) &&
            x >= 100 && x < 400 && y >= 100 && y < 200 ? destination : 0U;
    }, [&](unsigned int active) { hover = active; });
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
    receiver->DragOver(0, {400, 150}, &effect);
    Require(effect == DROPEFFECT_NONE && !hover, "outside target is rejected");
    const auto reads_before_drop = data.reads;
    effect = DROPEFFECT_COPY;
    receiver->Drop(&data, 0, {150, 150}, &effect);
    Require(effect == DROPEFFECT_COPY && !hover && data.reads == reads_before_drop + 1,
        "drop reads filesystem paths only once and clears feedback");
    effect = DROPEFFECT_COPY;
    receiver->DragEnter(&data, 0, {150, 150}, &effect);
    Require(effect == DROPEFFECT_NONE, "one bounded pending batch at a time");
    Require(target.TakeDrop().paths == data.paths, "Unicode, spaces, folders and unsupported paths preserve order verbatim");
    Require(target.TakeDrop().paths.empty(), "batch consumed exactly once");
    inside = false;
    effect = DROPEFFECT_COPY;
    receiver->Drop(&data, 0, {150, 150}, &effect);
    Require(effect == DROPEFFECT_NONE && target.TakeDrop().paths.empty(), "drop rechecks target at release");
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
    Require(effect == DROPEFFECT_NONE && target.TakeDrop().paths.empty(), "failed data transfer adds nothing");
    data.fail_read = false;
    data.paths.resize(Win32FileDropTarget::MaximumPaths + 1, L"C:\\a.csv");
    effect = DROPEFFECT_COPY;
    receiver->DragEnter(&data, 0, {150, 150}, &effect);
    receiver->Drop(&data, 0, {150, 150}, &effect);
    Require(effect == DROPEFFECT_NONE && target.TakeDrop().paths.empty(), "oversized batch is rejected atomically");
    effect = DROPEFFECT_MOVE;
    receiver->DragEnter(&data, 0, {150, 150}, &effect);
    Require(effect == DROPEFFECT_NONE, "never move or delete Explorer files");
    data.paths.resize(1);
    effect = DROPEFFECT_COPY;
    receiver->DragEnter(&data, 0, {150, 150}, &effect);
    Require(hover == 1, "source panel feedback");
    destination = 2;
    effect = DROPEFFECT_COPY;
    receiver->DragOver(0, {150, 150}, &effect);
    Require(hover == 2, "moving to annotations within the same HWND changes feedback");
    receiver->Drop(&data, 0, {150, 150}, &effect);
    destination = 1;
    const auto annotation_drop = target.TakeDrop();
    Require(annotation_drop.destination == 2 && annotation_drop.paths == data.paths && !hover,
        "release destination survives cleared feedback and subsequent hit-test changes");
    Window detached;
    SetWindowPos(detached.value, HWND_TOPMOST, 300, 100, 100, 100, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    const HWND both[] = {window.value, detached.value, window.value};
    target.SetWindows(both);
    target.SetWindows(both);
    effect = DROPEFFECT_COPY;
    receiver->DragEnter(&data, 0, {350, 150}, &effect);
    receiver->Drop(&data, 0, {350, 150}, &effect);
    Require(target.TakeDrop().destination == 1 && hit_window == detached.value,
        "separate viewport registers while shared HWND is deduplicated; hit test receives the top native window");
    DestroyWindow(detached.value);
    detached.value = nullptr;
    effect = DROPEFFECT_COPY;
    receiver->DragEnter(&data, 0, {150, 150}, &effect);
    Require(effect == DROPEFFECT_COPY, "destroying one viewport retains the other target");
    receiver->DragLeave();
    Require(!hover && target.TakeDrop().paths.empty(), "canceling a drag imports nothing");
    const auto root = std::filesystem::temp_directory_path() /
        (L"spectiary-shell-annotation-drop-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root / L"folder");
    const auto file = root / L"标注 file.csv";
    { std::ofstream stream(file); stream << "sample,label\na,A\n"; }
    file_only_panel = true;
    destination = 2;
    for (const std::vector<std::filesystem::path>& selection : {
            std::vector<std::filesystem::path>{file},
            std::vector<std::filesystem::path>{root / L"folder"},
            std::vector<std::filesystem::path>{file, root / L"folder"}}) {
        const auto shell_data = ShellData(selection);
        effect = DROPEFFECT_COPY;
        receiver->DragEnter(shell_data.Get(), 0, {150, 150}, &effect);
        const bool files_only = selection.size() == 1 && selection.front() == file;
        Require(effect == (files_only ? DROPEFFECT_COPY : DROPEFFECT_NONE) && hover == (files_only ? 2U : 0U),
            "file-only target rejects folder and mixed shell selections during hover");
        effect = DROPEFFECT_COPY;
        receiver->Drop(shell_data.Get(), 0, {150, 150}, &effect);
        const auto batch = target.TakeDrop();
        Require(files_only ? batch.destination == 2 && batch.paths == selection : batch.paths.empty(),
            "folder and mixed shell drops import nothing; file drop preserves Unicode path");
    }
    std::filesystem::remove_all(root);
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
