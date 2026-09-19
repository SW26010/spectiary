#include "app/application_settings.h"
#include "app/local_user_state.h"
#include "app/specforge_app.h"
#include "app/runtime_paths.h"
#include "automation/automation_startup.h"
#include "platform/win32_text.h"
#include "ui/ui_text.h"

#include <Windows.h>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

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

std::string PathToUtf8(
    const std::filesystem::path& path)
{
    const std::u8string utf8 = path.u8string();
    return std::string(
        utf8.begin(),
        utf8.end());
}

std::filesystem::path StartupErrorFileName(
    bool automation)
{
    return automation
        ? std::filesystem::path(
              "automation-startup-error.txt")
        : std::filesystem::path(
              "runtime-resource-startup-error.txt");
}

specforge::SpecForgeStartup PrepareStartup(
    const specforge::SpecForgeCommandLine& command_line)
{
    specforge::RuntimePathInputs inputs =
        specforge::CurrentProcessRuntimePathInputs(
            specforge::CurrentExecutablePath());
    if (!command_line.automation) {
        return specforge::PrepareSpecForgeStartup(
            std::move(inputs));
    }

    const std::filesystem::path ordinary_root =
        specforge::OrdinaryUserStateRootForExecutable(
            specforge::CurrentExecutablePath());
    if (!specforge::AutomationStateRootIsIndependent(
            command_line.automation->state_root,
            ordinary_root)) {
        throw std::runtime_error(
            "Automation state root must be independent from the ordinary user state root ('" +
            PathToUtf8(ordinary_root) +
            "').");
    }
    inputs.application_data_root_override =
        command_line.automation->state_root;
    inputs.legacy_application_data_root = command_line.automation->state_root;
    return specforge::PrepareSpecForgeStartup(
        std::move(inputs));
}

bool AutomatedStartupEnabled(
    bool automation_requested,
    const std::optional<std::filesystem::path>&
        automation_state_root)
{
    return automation_requested ||
           automation_state_root.has_value() ||
           RuntimeResourceWorkloadEnabled();
}

void WriteAutomatedStartupError(
    const std::filesystem::path& state_directory,
    bool automation,
    std::string_view diagnostic)
{
    std::error_code directory_error;
    std::filesystem::create_directories(
        state_directory,
        directory_error);
    if (directory_error) {
        return;
    }
    std::ofstream output(
        state_directory /
            StartupErrorFileName(automation),
        std::ios::binary | std::ios::trunc);
    if (output) {
        output << diagnostic;
    }
}

void ReportStartupError(
    std::string_view message,
    specforge::UiLanguage language,
    bool automation_requested,
    const std::optional<std::filesystem::path>&
        automation_state_root)
{
    if (!AutomatedStartupEnabled(
            automation_requested,
            automation_state_root)) {
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
    const std::optional<std::filesystem::path>
        state_directory =
            automation_state_root
            ? automation_state_root
            : (automation_requested
                   ? std::nullopt
                   : EnvironmentPath(
                         L"SPECFORGE_RUNTIME_RESOURCE_STATE_DIR"));
    if (state_directory) {
        WriteAutomatedStartupError(
            *state_directory,
            automation_state_root.has_value(),
            diagnostic);
    }
}

/*
 * EnvironmentPath and RuntimeResourceWorkloadEnabled remain the legacy
 * resource-stability startup contract. The explicit automation command line
 * above is independent and takes its state root from the launcher.
 */

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command)
{
    specforge::UiLanguage startup_error_language =
        specforge::UiLanguage::English;
    const specforge::SpecForgeCommandLine command_line =
        specforge::ParseCurrentProcessSpecForgeCommandLine();
    std::optional<std::filesystem::path>
        automation_diagnostic_root;
    try {
        if (!command_line.error_message.empty()) {
            throw std::runtime_error(
                command_line.error_message);
        }
        if (command_line.automation) {
            if (const auto conflict =
                    specforge::
                        ActiveIncompatibleAutomationEnvironmentVariable()) {
                std::string conflict_name;
                conflict_name.reserve(
                    conflict->size());
                for (const wchar_t character :
                     *conflict) {
                    conflict_name.push_back(
                        static_cast<char>(
                            character));
                }
                throw std::runtime_error(
                    "Automation startup rejects inherited legacy environment variable '" +
                    conflict_name +
                    "'.");
            }
        }
        specforge::SpecForgeStartup startup =
            PrepareStartup(command_line);
        specforge::MigrateLegacyApplicationStorage(startup.runtime_paths());
        // Hidden is presentation only; checkpoint I/O owns and reports failures.
        std::error_code unsaved_directory_error;
        std::filesystem::create_directories(startup.runtime_paths().unsaved_root, unsaved_directory_error);
        specforge::HideUnsavedCheckpointDirectory(startup.runtime_paths().unsaved_root);
        if (command_line.automation) {
            automation_diagnostic_root =
                command_line.automation->state_root;
        }
        startup_error_language =
            specforge::ApplicationSettings(
                specforge::ApplicationSettingsStorageForRuntimePaths(
                    startup.runtime_paths()))
                .View()
                .language;
        specforge::SpecForgeApp app(
            startup,
            command_line.automation);
        return app.Run(
            instance,
            show_command,
            command_line.initial_source);
    } catch (const std::exception& error) {
        ReportStartupError(
            error.what(),
            startup_error_language,
            command_line.automation_requested,
            automation_diagnostic_root);
    } catch (...) {
        ReportStartupError(
            specforge::UiText(
                startup_error_language,
                specforge::UiTextId::
                    UnknownStartupError),
            startup_error_language,
            command_line.automation_requested,
            automation_diagnostic_root);
    }

    return 1;
}
