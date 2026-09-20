#include "platform/win32_file_drop_target.h"

#include <ole2.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace spectiary {

struct Win32FileDropTarget::Impl final : IDropTarget {
    ULONG references = 1;
    std::vector<HWND> windows;
    HitTest hit_test;
    Feedback feedback;
    bool compatible = false;
    bool all_files = false;
    unsigned int hovered = 0;
    DropBatch pending;

    static FORMATETC Format()
    {
        return {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    }
    static bool ContainsOnlyFiles(IDataObject* data)
    {
        Microsoft::WRL::ComPtr<IShellItemArray> items;
        if (!data || FAILED(SHCreateShellItemArrayFromDataObject(data, IID_PPV_ARGS(&items)))) return false;
        SFGAOF attributes = 0;
        return SUCCEEDED(items->GetAttributes(SIATTRIBFLAGS_OR, SFGAO_FOLDER, &attributes)) &&
            !(attributes & SFGAO_FOLDER);
    }
    void Hover(unsigned int value) noexcept
    {
        if (hovered == value) return;
        hovered = value;
        try { feedback(value); } catch (...) { /* Never unwind through OLE. */ }
    }
    unsigned int Accept(POINTL point, DWORD effects)
    {
        const HWND window = WindowFromPoint({point.x, point.y});
        if (!compatible || !pending.paths.empty() || !(effects & DROPEFFECT_COPY) ||
            !window || std::find(windows.begin(), windows.end(), window) == windows.end() ||
            !IsWindowVisible(window) || IsIconic(window)) return 0;
        return hit_test(window, static_cast<float>(point.x), static_cast<float>(point.y), all_files);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDropTarget) return E_NOINTERFACE;
        *out = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --references;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL point, DWORD* effect) override
    {
        auto format = Format();
        compatible = data && SUCCEEDED(data->QueryGetData(&format));
        all_files = compatible && ContainsOnlyFiles(data);
        return DragOver(0, point, effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD* effect) override
    {
        if (!effect) return E_POINTER;
        try {
            const unsigned int accepted = Accept(point, *effect);
            *effect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            Hover(accepted);
            return S_OK;
        } catch (...) {
            *effect = DROPEFFECT_NONE;
            Hover(0);
            return E_FAIL;
        }
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        compatible = false;
        all_files = false;
        Hover(0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL point, DWORD* effect) override
    {
        if (!effect) return E_POINTER;
        const DWORD allowed = *effect;
        *effect = DROPEFFECT_NONE;
        struct Medium {
            STGMEDIUM value{};
            ~Medium() { if (value.tymed) ReleaseStgMedium(&value); }
        } medium;
        try {
            auto format = Format();
            const unsigned int destination = Accept(point, allowed);
            if (data && destination &&
                SUCCEEDED(data->GetData(&format, &medium.value)) &&
                medium.value.tymed == TYMED_HGLOBAL && medium.value.hGlobal) {
                const HDROP drop = static_cast<HDROP>(medium.value.hGlobal);
                const UINT count = DragQueryFileW(drop, 0xffffffffU, nullptr, 0);
                if (count == 0 || count > MaximumPaths) {
                    DragLeave();
                    return S_OK;
                }
                std::vector<std::filesystem::path> paths;
                paths.reserve(count);
                for (UINT index = 0; index < count; ++index) {
                    const UINT length = DragQueryFileW(drop, index, nullptr, 0);
                    if (length == 0 || length > 32767) {
                        DragLeave();
                        return S_OK;
                    }
                    std::wstring path(length + 1, L'\0');
                    if (DragQueryFileW(drop, index, path.data(), length + 1) != length) {
                        DragLeave();
                        return S_OK;
                    }
                    path.resize(length);
                    paths.emplace_back(std::move(path));
                }
                pending = {destination, std::move(paths)};
                *effect = DROPEFFECT_COPY;
            }
        } catch (...) {
            DragLeave();
            return E_FAIL;
        }
        DragLeave();
        return S_OK;
    }
    void Detach(HWND window) noexcept
    {
        if (window) {
            std::erase(windows, window);
            RemoveWindowSubclass(window, WindowProc, reinterpret_cast<UINT_PTR>(this));
            RevokeDragDrop(window);
        }
        DragLeave();
    }
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam,
        LPARAM lparam, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<Impl*>(data);
        if (message == WM_NCDESTROY) self->Detach(hwnd);
        return DefSubclassProc(hwnd, message, wparam, lparam);
    }
};

Win32FileDropTarget::Win32FileDropTarget(HitTest hit_test, Feedback feedback)
    : impl_(nullptr)
{
    const HRESULT result = OleInitialize(nullptr);
    if (FAILED(result)) throw std::runtime_error("Could not initialize filesystem drag and drop.");
    try {
        impl_ = new Impl;
        impl_->hit_test = std::move(hit_test);
        impl_->feedback = std::move(feedback);
    } catch (...) {
        if (impl_) impl_->Release();
        OleUninitialize();
        throw;
    }
}

Win32FileDropTarget::~Win32FileDropTarget()
{
    while (!impl_->windows.empty()) impl_->Detach(impl_->windows.back());
    impl_->Release();
    OleUninitialize();
}

void Win32FileDropTarget::SetWindow(HWND window)
{
    SetWindows(std::span<const HWND>(&window, 1));
}

void Win32FileDropTarget::SetWindows(std::span<const HWND> windows)
{
    // Two panels can share an HWND. Register it once, while keeping both
    // detached viewports live when the panels are in separate windows.
    for (std::size_t index = impl_->windows.size(); index > 0; --index) {
        const HWND window = impl_->windows[index - 1];
        if (std::find(windows.begin(), windows.end(), window) == windows.end()) impl_->Detach(window);
    }
    impl_->windows.reserve(windows.size());
    for (const HWND window : windows) {
        if (!window || std::find(impl_->windows.begin(), impl_->windows.end(), window) != impl_->windows.end()) continue;
        if (!SetWindowSubclass(window, Impl::WindowProc, reinterpret_cast<UINT_PTR>(impl_),
            reinterpret_cast<DWORD_PTR>(impl_))) {
            throw std::runtime_error("Could not attach filesystem drop target to window.");
        }
        const HRESULT result = RegisterDragDrop(window, Receiver());
        if (FAILED(result)) {
            RemoveWindowSubclass(window, Impl::WindowProc, reinterpret_cast<UINT_PTR>(impl_));
            throw std::runtime_error("Could not register filesystem drop target.");
        }
        impl_->windows.push_back(window);
    }
}

Win32FileDropTarget::DropBatch Win32FileDropTarget::TakeDrop()
{
    return std::exchange(impl_->pending, {});
}

IDropTarget* Win32FileDropTarget::Receiver() const noexcept
{
    return impl_;
}

} // namespace spectiary
