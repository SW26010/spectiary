#include "app/specforge_app.h"
#include "app/runtime_paths.h"
#include "platform/win32_text.h"
#include "ui/ui_language_settings.h"
#include "ui/ui_text.h"

#include <Windows.h>
#include <shellapi.h>

#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

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

std::optional<std::filesystem::path> EnvironmentPath(
    const wchar_t* name)
{
    const DWORD required =
        GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return std::nullopt;
    }
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        name,
        value.data(),
        required);
    if (written == 0 || written >= required) {
        return std::nullopt;
    }
    value.resize(written);
    return value.empty()
        ? std::nullopt
        : std::optional<std::filesystem::path>{
              std::move(value)};
}

bool RuntimeResourceWorkloadEnabled()
{
    return EnvironmentPath(
               L"SPECFORGE_RUNTIME_RESOURCE_WORKLOAD")
        .has_value();
}

void ReportStartupError(
    std::string_view message,
    specforge::UiLanguage language)
{
    if (!RuntimeResourceWorkloadEnabled()) {
        const std::wstring wide_message =
            specforge::Utf8ToWide(message);
        const std::wstring wide_title =
            specforge::Utf8ToWide(
                specforge::UiText(
                    language,
                    specforge::UiTextId::
                        StartupErrorTitle));
        MessageBoxW(
            nullptr,
            wide_message.c_str(),
            wide_title.c_str(),
            MB_OK | MB_ICONERROR);
        return;
    }

    const std::string diagnostic =
        "SpecForge startup error: " +
        std::string(message) + "\n";
    OutputDebugStringA(diagnostic.c_str());
    const std::optional<std::filesystem::path> state_directory =
        EnvironmentPath(
            L"SPECFORGE_RUNTIME_RESOURCE_STATE_DIR");
    if (!state_directory) {
        return;
    }

    std::error_code directory_error;
    std::filesystem::create_directories(
        *state_directory,
        directory_error);
    if (directory_error) {
        return;
    }
    std::ofstream output(
        *state_directory /
            "runtime-resource-startup-error.txt",
        std::ios::binary | std::ios::trunc);
    if (output) {
        output << diagnostic;
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command)
{
    specforge::UiLanguage startup_error_language =
        specforge::UiLanguage::English;
    try {
        const specforge::SpecForgeStartup& startup =
            specforge::DefaultSpecForgeStartup();
        startup_error_language =
            specforge::LoadUiLanguageSettings(
                startup.runtime_paths()
                    .ui_language_settings_path)
                .language;
        specforge::SpecForgeApp app(startup);
        return app.Run(instance, show_command, InitialSourceFromCommandLine());
    } catch (const std::exception& error) {
        ReportStartupError(
            error.what(),
            startup_error_language);
    } catch (...) {
        ReportStartupError(
            specforge::UiText(
                startup_error_language,
                specforge::UiTextId::
                    UnknownStartupError),
            startup_error_language);
    }

    return 1;
}
