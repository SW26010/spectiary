#pragma once

#include <Windows.h>

namespace spectiary {

struct Win32ApplicationIcons {
    HICON large_icon = nullptr;
    HICON small_icon = nullptr;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return large_icon != nullptr &&
            small_icon != nullptr;
    }
};

[[nodiscard]] Win32ApplicationIcons
LoadWin32ApplicationIcons(HINSTANCE instance) noexcept;

void ApplyWin32ApplicationIcons(
    HWND hwnd,
    const Win32ApplicationIcons& icons) noexcept;

[[nodiscard]] bool
InstallImGuiPlatformWindowIconHook() noexcept;

}  // namespace spectiary
