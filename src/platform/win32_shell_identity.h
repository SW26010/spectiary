#pragma once
#include <Windows.h>
#include <filesystem>
#include <string>

namespace spectiary {
[[nodiscard]] std::wstring ShellAppUserModelId(const std::filesystem::path& config_root);
[[nodiscard]] HRESULT ConfigureShellWindowIdentity(HWND window,
    const std::wstring& app_id, const std::filesystem::path& executable);
} // namespace spectiary
