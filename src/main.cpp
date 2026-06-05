#include "app/specforge_app.h"

#include <Windows.h>
#include <shellapi.h>

#include <exception>
#include <filesystem>
#include <optional>
#include <string>

namespace {

std::optional<std::filesystem::path> InitialSourceFromCommandLine()
{
    int argument_count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments == nullptr) {
        return std::nullopt;
    }

    std::optional<std::filesystem::path> source;
    for (int index = 1; index < argument_count; ++index) {
        if (arguments[index] != nullptr && arguments[index][0] != L'\0' && arguments[index][0] != L'-') {
            source = std::filesystem::path(arguments[index]);
            break;
        }
    }

    LocalFree(arguments);
    return source;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command)
{
    try {
        specforge::SpecForgeApp app;
        return app.Run(instance, show_command, InitialSourceFromCommandLine());
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "SpecForge startup error", MB_OK | MB_ICONERROR);
    } catch (...) {
        MessageBoxA(nullptr, "Unknown startup error.", "SpecForge startup error", MB_OK | MB_ICONERROR);
    }

    return 1;
}
