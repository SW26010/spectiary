#include "platform/win32_application_icon.h"
#include "platform/win32_window.h"

#include <imgui.h>
#include <imgui_impl_win32.h>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

HICON WindowIcon(HWND hwnd, WPARAM icon_kind)
{
    return reinterpret_cast<HICON>(
        SendMessageW(hwnd, WM_GETICON, icon_kind, 0));
}

HICON WindowClassIcon(HWND hwnd, int index)
{
    return reinterpret_cast<HICON>(
        GetClassLongPtrW(hwnd, index));
}

void RequireMainWindowIcons(HWND hwnd)
{
    Require(
        WindowClassIcon(hwnd, GCLP_HICON) != nullptr,
        "main HWND class should expose a large application icon");
    Require(
        WindowClassIcon(hwnd, GCLP_HICONSM) != nullptr,
        "main HWND class should expose a small application icon");
    Require(
        WindowIcon(hwnd, ICON_BIG) != nullptr,
        "main HWND should answer WM_GETICON for its large icon");
    Require(
        WindowIcon(hwnd, ICON_SMALL) != nullptr,
        "main HWND should answer WM_GETICON for its small icon");
}

void TestImGuiPlatformWindowIcons(HWND main_hwnd)
{
    ImGuiContext* previous_context =
        ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    bool backend_initialized = false;
    ImGuiViewport viewport;

    auto cleanup = [&]() {
        if (context == nullptr) {
            return;
        }

        ImGui::SetCurrentContext(context);
        ImGuiPlatformIO& platform_io =
            ImGui::GetPlatformIO();
        if (viewport.PlatformUserData != nullptr &&
            platform_io.Platform_DestroyWindow != nullptr) {
            platform_io.Platform_DestroyWindow(&viewport);
        }
        if (backend_initialized) {
            ImGui_ImplWin32_Shutdown();
            backend_initialized = false;
        }
        ImGui::DestroyContext(context);
        context = nullptr;
        ImGui::SetCurrentContext(previous_context);
    };

    try {
        ImGui::SetCurrentContext(context);
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        backend_initialized =
            ImGui_ImplWin32_Init(main_hwnd);
        Require(
            backend_initialized,
            "Dear ImGui Win32 backend should initialize for icon integration test");
        Require(
            specforge::InstallImGuiPlatformWindowIconHook(),
            "application icon hook should wrap Platform_CreateWindow");

        ImGuiViewport* main_viewport =
            ImGui::GetMainViewport();
        viewport.ID = 0x53464943U;
        viewport.Pos = ImVec2(100.0f, 100.0f);
        viewport.Size = ImVec2(320.0f, 200.0f);
        viewport.DpiScale = 1.0f;
        viewport.ParentViewportId = main_viewport->ID;
        viewport.ParentViewport = main_viewport;

        ImGuiPlatformIO& platform_io =
            ImGui::GetPlatformIO();
        Require(
            platform_io.Platform_CreateWindow != nullptr,
            "wrapped Platform_CreateWindow callback should remain installed");
        platform_io.Platform_CreateWindow(&viewport);

        HWND detached_hwnd = static_cast<HWND>(
            viewport.PlatformHandleRaw);
        Require(
            detached_hwnd != nullptr &&
                IsWindow(detached_hwnd),
            "wrapped Platform_CreateWindow should create a real secondary HWND");
        Require(
            WindowIcon(detached_hwnd, ICON_BIG) != nullptr,
            "secondary HWND should answer WM_GETICON for its large icon");
        Require(
            WindowIcon(detached_hwnd, ICON_SMALL) != nullptr,
            "secondary HWND should answer WM_GETICON for its small icon");
        Require(
            WindowClassIcon(detached_hwnd, GCLP_HICON) == nullptr &&
                WindowClassIcon(detached_hwnd, GCLP_HICONSM) == nullptr,
            "secondary HWND icons should be per-window without modifying the ImGui backend class");

        cleanup();
    }
    catch (...) {
        cleanup();
        throw;
    }
}

}  // namespace

int main()
{
    try {
        specforge::Win32Window main_window;
        Require(
            main_window.Create(
                GetModuleHandleW(nullptr),
                L"SpecForge application icon integration test",
                640,
                480,
                [](HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
                    return DefWindowProcW(
                        hwnd,
                        message,
                        wparam,
                        lparam);
                }),
            "main integration-test HWND should be created");

        RequireMainWindowIcons(main_window.hwnd());
        TestImGuiPlatformWindowIcons(main_window.hwnd());
        std::cout
            << "Win32 application icon integration checks passed.\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
