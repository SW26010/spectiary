#include "app/runtime_paths.h"
#include "ui/shell_ui.h"
#include "ui/ui_language_settings.h"

#include <Windows.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

std::string ReadFile(
    const std::filesystem::path& path)
{
    std::ifstream input(
        path,
        std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::filesystem::path UniqueRoot()
{
    const auto suffix =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
    return std::filesystem::temp_directory_path() /
           ("specforge-automation-state-isolation-" +
            std::to_string(GetCurrentProcessId()) +
            "-" + std::to_string(suffix));
}

}  // namespace

int main()
{
    const std::filesystem::path fixture_root =
        UniqueRoot();
    const std::filesystem::path ordinary_root =
        fixture_root / "ordinary";
    const std::filesystem::path automation_root =
        fixture_root / "automation";
    std::error_code error;
    std::filesystem::create_directories(
        ordinary_root,
        error);
    Require(!error, "ordinary fixture root should be created");
    std::filesystem::create_directories(
        automation_root,
        error);
    Require(!error, "automation fixture root should be created");

    const std::filesystem::path ordinary_language =
        ordinary_root / "ui-language.json";
    std::string save_error;
    Require(
        specforge::SaveUiLanguageSettings(
            ordinary_language,
            specforge::UiLanguage::
                SimplifiedChinese,
            &save_error),
        save_error);
    const std::string ordinary_before =
        ReadFile(ordinary_language);
    const auto ordinary_write_time =
        std::filesystem::last_write_time(
            ordinary_language);

    specforge::RuntimePathInputs inputs;
    inputs.executable_path =
        specforge::CurrentExecutablePath();
    inputs.local_app_data_user_state_root =
        ordinary_root;
    inputs.local_user_state_root_override =
        automation_root;
    const specforge::SpecForgeStartup startup =
        specforge::PrepareSpecForgeStartup(
            std::move(inputs));
    Require(
        startup.runtime_paths()
                .local_user_state_root ==
            automation_root &&
            startup.runtime_paths()
                    .ui_language_settings_path ==
                automation_root /
                    "ui-language.json" &&
            startup.runtime_paths()
                    .source_session_state_path ==
                automation_root /
                    "source-session.json",
        "automation override should remap the complete local-state path family");

    {
        specforge::ShellUi shell(startup);
        Require(
            shell.ui_language() ==
                specforge::UiLanguage::English,
            "Shell should not import the ordinary root's Chinese language setting");
        Require(
            !shell.current_snapshot(),
            "isolated Shell should not restore an ordinary user session");
        const auto flush = shell.FlushLocalState();
        Require(
            flush.all_saved(),
            "isolated Shell state should flush normally");
    }

    Require(
        ReadFile(ordinary_language) ==
                ordinary_before &&
            std::filesystem::last_write_time(
                ordinary_language) ==
                ordinary_write_time,
        "automation Shell construction and flush must not modify the ordinary state root");
    Require(
        std::filesystem::exists(
            automation_root),
        "automation state should remain scoped to the launcher-provided root");

    std::filesystem::remove_all(
        fixture_root,
        error);
    std::cout
        << "automation state isolation tests passed\n";
    return 0;
}
