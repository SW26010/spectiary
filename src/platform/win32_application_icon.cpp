#include "platform/win32_application_icon.h"

#include "platform/specforge_resource.h"

#include <imgui.h>

namespace specforge {
namespace {

using ImGuiPlatformCreateWindowCallback =
    void (*)(ImGuiViewport* viewport);

ImGuiPlatformCreateWindowCallback
    original_imgui_platform_create_window = nullptr;

HICON LoadIconForSystemMetric(
    HINSTANCE instance,
    int width_metric,
    int height_metric) noexcept
{
    return static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(SPECFORGE_RESOURCE_APPLICATION_ICON),
        IMAGE_ICON,
        GetSystemMetrics(width_metric),
        GetSystemMetrics(height_metric),
        LR_SHARED));
}

void CreateImGuiPlatformWindowWithApplicationIcon(
    ImGuiViewport* viewport)
{
    if (original_imgui_platform_create_window == nullptr) {
        return;
    }

    original_imgui_platform_create_window(viewport);
    if (viewport == nullptr) {
        return;
    }

    HWND hwnd = static_cast<HWND>(
        viewport->PlatformHandleRaw != nullptr
            ? viewport->PlatformHandleRaw
            : viewport->PlatformHandle);
    ApplyWin32ApplicationIcons(
        hwnd,
        LoadWin32ApplicationIcons(GetModuleHandleW(nullptr)));
}

}  // namespace

Win32ApplicationIcons
LoadWin32ApplicationIcons(HINSTANCE instance) noexcept
{
    if (instance == nullptr) {
        return {};
    }

    return {
        .large_icon = LoadIconForSystemMetric(
            instance,
            SM_CXICON,
            SM_CYICON),
        .small_icon = LoadIconForSystemMetric(
            instance,
            SM_CXSMICON,
            SM_CYSMICON),
    };
}

void ApplyWin32ApplicationIcons(
    HWND hwnd,
    const Win32ApplicationIcons& icons) noexcept
{
    if (hwnd == nullptr || !icons) {
        return;
    }

    (void)SendMessageW(
        hwnd,
        WM_SETICON,
        ICON_BIG,
        reinterpret_cast<LPARAM>(icons.large_icon));
    (void)SendMessageW(
        hwnd,
        WM_SETICON,
        ICON_SMALL,
        reinterpret_cast<LPARAM>(icons.small_icon));
}

bool InstallImGuiPlatformWindowIconHook() noexcept
{
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    if (platform_io.Platform_CreateWindow == nullptr ||
        platform_io.Platform_CreateWindow ==
            &CreateImGuiPlatformWindowWithApplicationIcon) {
        return false;
    }

    original_imgui_platform_create_window =
        platform_io.Platform_CreateWindow;
    platform_io.Platform_CreateWindow =
        &CreateImGuiPlatformWindowWithApplicationIcon;
    return true;
}

}  // namespace specforge
