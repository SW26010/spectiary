#include "platform/win32_file_drop_target.h"

#include <ole2.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdexcept>
#include <utility>

namespace spectiary {

struct Win32FileDropTarget::Impl final : IDropTarget {
    ULONG references = 1;
    HWND window = nullptr;
    HitTest hit_test;
    Feedback feedback;
    bool compatible = false;
    bool hovered = false;
    std::vector<std::filesystem::path> pending;

    static FORMATETC Format()
    {
        return {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    }
    void Hover(bool value) noexcept
    {
        if (hovered == value) return;
        hovered = value;
        try { feedback(value); } catch (...) { /* Never unwind through OLE. */ }
    }
    bool Accept(POINTL point, DWORD effects)
    {
        return compatible && pending.empty() && (effects & DROPEFFECT_COPY) &&
            window && IsWindowVisible(window) && !IsIconic(window) &&
            WindowFromPoint({point.x, point.y}) == window &&
            hit_test(static_cast<float>(point.x), static_cast<float>(point.y));
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
        return DragOver(0, point, effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD* effect) override
    {
        if (!effect) return E_POINTER;
        try {
            const bool accepted = Accept(point, *effect);
            *effect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            Hover(accepted);
            return S_OK;
        } catch (...) {
            *effect = DROPEFFECT_NONE;
            Hover(false);
            return E_FAIL;
        }
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        compatible = false;
        Hover(false);
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
            if (data && Accept(point, allowed) &&
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
                pending = std::move(paths);
                *effect = DROPEFFECT_COPY;
            }
        } catch (...) {
            DragLeave();
            return E_FAIL;
        }
        DragLeave();
        return S_OK;
    }
    void Detach() noexcept
    {
        if (window) {
            const HWND old = std::exchange(window, nullptr);
            RemoveWindowSubclass(old, WindowProc, reinterpret_cast<UINT_PTR>(this));
            RevokeDragDrop(old);
        }
        DragLeave();
    }
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam,
        LPARAM lparam, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<Impl*>(data);
        if (message == WM_NCDESTROY) self->Detach();
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
    impl_->Detach();
    impl_->Release();
    OleUninitialize();
}

void Win32FileDropTarget::SetWindow(HWND window)
{
    if (impl_->window == window) return;
    impl_->Detach();
    if (!window) return;
    if (!SetWindowSubclass(window, Impl::WindowProc, reinterpret_cast<UINT_PTR>(impl_),
            reinterpret_cast<DWORD_PTR>(impl_))) {
        throw std::runtime_error("Could not attach filesystem drop target to window.");
    }
    const HRESULT result = RegisterDragDrop(window, Receiver());
    if (FAILED(result)) {
        RemoveWindowSubclass(window, Impl::WindowProc, reinterpret_cast<UINT_PTR>(impl_));
        throw std::runtime_error("Could not register filesystem drop target.");
    }
    impl_->window = window;
}

std::vector<std::filesystem::path> Win32FileDropTarget::TakePaths()
{
    return std::exchange(impl_->pending, {});
}

IDropTarget* Win32FileDropTarget::Receiver() const noexcept
{
    return impl_;
}

} // namespace spectiary
