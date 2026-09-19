#include "platform/win32_window.h"

#include "platform/win32_application_icon.h"

#include <stdexcept>
#include <utility>

namespace specforge {
namespace {

constexpr UINT kDefaultWindowsDpi = 96;

using GetDpiForSystemFn = UINT(WINAPI*)();
using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);

UINT GetInitialWindowDpi()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        auto* get_dpi_for_system = reinterpret_cast<GetDpiForSystemFn>(GetProcAddress(user32, "GetDpiForSystem"));
        if (get_dpi_for_system != nullptr) {
            return get_dpi_for_system();
        }
    }

    HDC screen_dc = GetDC(nullptr);
    if (screen_dc != nullptr) {
        const int dpi = GetDeviceCaps(screen_dc, LOGPIXELSX);
        ReleaseDC(nullptr, screen_dc);
        if (dpi > 0) {
            return static_cast<UINT>(dpi);
        }
    }

    return kDefaultWindowsDpi;
}

BOOL AdjustWindowRectForDpi(RECT* rect, DWORD style, BOOL has_menu, DWORD extended_style, UINT dpi)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        auto* adjust_window_rect_ex_for_dpi =
            reinterpret_cast<AdjustWindowRectExForDpiFn>(GetProcAddress(user32, "AdjustWindowRectExForDpi"));
        if (adjust_window_rect_ex_for_dpi != nullptr) {
            return adjust_window_rect_ex_for_dpi(rect, style, has_menu, extended_style, dpi);
        }
    }

    return AdjustWindowRectEx(rect, style, has_menu, extended_style);
}

int ScaleForDpi(int value, UINT dpi)
{
    return MulDiv(value, static_cast<int>(dpi), static_cast<int>(kDefaultWindowsDpi));
}

}  // namespace

Win32Window::~Win32Window()
{
    Destroy();
}

bool Win32Window::Create(HINSTANCE instance, const wchar_t* title, int width, int height, MessageHandler handler)
{
    instance_ = instance;
    message_handler_ = std::move(handler);
    class_name_ = L"MainWindowV1";
    const Win32ApplicationIcons icons =
        LoadWin32ApplicationIcons(instance_);
    if (!icons) {
        return false;
    }

    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_CLASSDC;
    window_class.lpfnWndProc = &Win32Window::WindowProc;
    window_class.hInstance = instance_;
    window_class.hIcon = icons.large_icon;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = class_name_.c_str();
    window_class.hIconSm = icons.small_icon;

    if (!RegisterClassExW(&window_class)) {
        return false;
    }

    const UINT dpi = GetInitialWindowDpi();
    const DWORD window_style = WS_OVERLAPPEDWINDOW;
    const DWORD extended_style = 0;
    const int scaled_width = ScaleForDpi(width, dpi);
    const int scaled_height = ScaleForDpi(height, dpi);

    RECT window_rect = {0, 0, scaled_width, scaled_height};
    if (!AdjustWindowRectForDpi(&window_rect, window_style, FALSE, extended_style, dpi)) {
        UnregisterClassW(class_name_.c_str(), instance_);
        return false;
    }

    hwnd_ = CreateWindowExW(
        extended_style,
        class_name_.c_str(),
        title,
        window_style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        window_rect.right - window_rect.left,
        window_rect.bottom - window_rect.top,
        nullptr,
        nullptr,
        instance_,
        this);

    if (hwnd_ == nullptr) {
        UnregisterClassW(class_name_.c_str(), instance_);
        return false;
    }
    ApplyWin32ApplicationIcons(hwnd_, icons);

    RECT client_rect = {};
    if (GetClientRect(hwnd_, &client_rect)) {
        client_width_ = static_cast<UINT>(client_rect.right - client_rect.left);
        client_height_ = static_cast<UINT>(client_rect.bottom - client_rect.top);
    } else {
        client_width_ = static_cast<UINT>(scaled_width);
        client_height_ = static_cast<UINT>(scaled_height);
    }

    return true;
}

void Win32Window::Show(
    int show_command,
    Win32WindowActivation activation) const
{
    const Win32WindowShowPlan plan =
        ResolveWin32WindowShowPlan(
            show_command,
            activation);
    ShowWindow(hwnd_, plan.show_command);
    if (plan.position_flags != 0) {
        (void)SetWindowPos(
            hwnd_,
            nullptr,
            0,
            0,
            0,
            0,
            plan.position_flags);
    }
    UpdateWindow(hwnd_);
}

void Win32Window::ClearMessageHandler() noexcept
{
    message_handler_ = nullptr;
}

LRESULT CALLBACK Win32Window::WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    Win32Window* window = nullptr;

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        window = static_cast<Win32Window*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->hwnd_ = hwnd;
    } else {
        window = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window == nullptr) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    const LRESULT result = window->DispatchMessage(hwnd, message, wparam, lparam);

    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        window->hwnd_ = nullptr;
    }

    return result;
}

LRESULT Win32Window::DispatchMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_SIZE && wparam != SIZE_MINIMIZED) {
        client_width_ = LOWORD(lparam);
        client_height_ = HIWORD(lparam);
    }

    if (message_handler_) {
        return message_handler_(hwnd, message, wparam, lparam);
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void Win32Window::Destroy()
{
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    if (!class_name_.empty() && instance_ != nullptr) {
        UnregisterClassW(class_name_.c_str(), instance_);
        class_name_.clear();
    }

    ClearMessageHandler();
    instance_ = nullptr;
    client_width_ = 0;
    client_height_ = 0;
}

}  // namespace specforge
