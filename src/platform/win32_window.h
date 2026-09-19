#pragma once

#include <Windows.h>

#include <functional>
#include <string>

namespace spectiary {

enum class Win32WindowActivation {
    Default,
    NoActivate,
};

struct Win32WindowShowPlan {
    int show_command = SW_SHOWNORMAL;
    UINT position_flags = 0;
};

[[nodiscard]] inline Win32WindowShowPlan
ResolveWin32WindowShowPlan(
    int requested_show_command,
    Win32WindowActivation activation) noexcept
{
    if (activation ==
        Win32WindowActivation::NoActivate) {
        return {
            .show_command = SW_SHOWNOACTIVATE,
            .position_flags =
                SWP_NOMOVE | SWP_NOSIZE |
                SWP_NOZORDER | SWP_NOACTIVATE |
                SWP_SHOWWINDOW,
        };
    }
    return {
        .show_command = requested_show_command,
    };
}

class Win32Window {
public:
    using MessageHandler = std::function<LRESULT(HWND, UINT, WPARAM, LPARAM)>;

    Win32Window() = default;
    ~Win32Window();

    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;

    bool Create(HINSTANCE instance, const wchar_t* title, int width, int height, MessageHandler handler);
    void Show(
        int show_command,
        Win32WindowActivation activation =
            Win32WindowActivation::Default) const;
    void Destroy();
    void ClearMessageHandler() noexcept;

    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }
    [[nodiscard]] UINT client_width() const noexcept { return client_width_; }
    [[nodiscard]] UINT client_height() const noexcept { return client_height_; }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    LRESULT DispatchMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    std::wstring class_name_;
    MessageHandler message_handler_;
    UINT client_width_ = 0;
    UINT client_height_ = 0;
};

}  // namespace spectiary
