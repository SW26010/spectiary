#include <Windows.h>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr UINT kActivateWindowMessage = WM_APP + 1;
constexpr wchar_t kWindowClassName[] =
    L"SpecForgeWin32TestPeer";

std::string FormatHandle(HWND window)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::setw(
        static_cast<int>(sizeof(std::uintptr_t) * 2))
           << std::setfill('0')
           << reinterpret_cast<std::uintptr_t>(window);
    return stream.str();
}

bool ActivateWindowForTest(HWND target)
{
    if (!IsWindow(target)) {
        return false;
    }

    (void)ShowWindowAsync(target, SW_RESTORE);

    const DWORD current_thread = GetCurrentThreadId();
    const HWND foreground = GetForegroundWindow();
    const DWORD foreground_thread = foreground != nullptr
        ? GetWindowThreadProcessId(foreground, nullptr)
        : 0;
    const DWORD target_thread =
        GetWindowThreadProcessId(target, nullptr);

    const bool attached_foreground =
        foreground_thread != 0 &&
        foreground_thread != current_thread &&
        AttachThreadInput(
            current_thread,
            foreground_thread,
            TRUE) != FALSE;
    const bool attached_target =
        target_thread != 0 &&
        target_thread != current_thread &&
        target_thread != foreground_thread &&
        AttachThreadInput(
            current_thread,
            target_thread,
            TRUE) != FALSE;

    (void)BringWindowToTop(target);
    (void)SetForegroundWindow(target);
    (void)SetActiveWindow(target);
    (void)SetFocus(target);

    if (attached_target) {
        (void)AttachThreadInput(
            current_thread,
            target_thread,
            FALSE);
    }
    if (attached_foreground) {
        (void)AttachThreadInput(
            current_thread,
            foreground_thread,
            FALSE);
    }
    return GetForegroundWindow() == target;
}

LRESULT CALLBACK WindowProc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam)
{
    switch (message) {
    case kActivateWindowMessage: {
        HWND target = reinterpret_cast<HWND>(wparam);
        const bool activated = ActivateWindowForTest(target);
        std::cout << "activated " << FormatHandle(target)
                  << ' ' << (activated ? "true" : "false")
                  << std::endl;
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(
            window,
            message,
            wparam,
            lparam);
    }
}

}  // namespace

int main()
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&window_class) == 0) {
        return 1;
    }

    HWND window = CreateWindowExW(
        0,
        kWindowClassName,
        L"SpecForge Win32 Test Peer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        640,
        480,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (window == nullptr) {
        return 2;
    }

    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    std::cout << "ready " << FormatHandle(window)
              << std::endl;

    std::thread([window]() {
        std::string command;
        while (std::getline(std::cin, command)) {
            if (command == "activate") {
                (void)PostMessageW(
                    window,
                    kActivateWindowMessage,
                    reinterpret_cast<WPARAM>(window),
                    0);
                continue;
            }
            constexpr std::string_view prefix =
                "activate-window ";
            if (command.starts_with(prefix)) {
                try {
                    const std::uintptr_t target =
                        std::stoull(
                            command.substr(prefix.size()),
                            nullptr,
                            0);
                    (void)PostMessageW(
                        window,
                        kActivateWindowMessage,
                        static_cast<WPARAM>(target),
                        0);
                }
                catch (...) {
                }
                continue;
            }
            if (command == "quit") {
                (void)PostMessageW(
                    window,
                    WM_CLOSE,
                    0,
                    0);
                return;
            }
        }
        (void)PostMessageW(window, WM_CLOSE, 0, 0);
    }).detach();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
