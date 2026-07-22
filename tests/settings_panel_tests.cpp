#include "ui/settings_panel.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#ifndef SPECFORGE_EXPECTED_VERSION
#error "SPECFORGE_EXPECTED_VERSION must be provided by the build configuration."
#endif

namespace specforge {

struct SettingsPanelUiTestAccess {
    static void Close(SettingsPanelUi& panel) { panel.open_ = false; }
    static void SetTransientFeedback(SettingsPanelUi& panel, std::string status, bool failed)
    {
        panel.action_status_ = std::move(status);
        panel.action_failed_ = failed;
    }
    static const std::string& TransientFeedback(const SettingsPanelUi& panel) { return panel.action_status_; }
    static bool ActionFailed(const SettingsPanelUi& panel) { return panel.action_failed_; }
    static void SelectSection(SettingsPanelUi& panel, SettingsSection section) { panel.selected_section_ = section; }
};

}  // namespace specforge

namespace {

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const char* name, const char* value)
        : name_(name)
    {
        char* raw_previous = nullptr;
        std::size_t previous_size = 0;
        if (_dupenv_s(&raw_previous, &previous_size, name_.c_str()) == 0 && raw_previous != nullptr) {
            std::unique_ptr<char, decltype(&std::free)> previous(raw_previous, std::free);
            previous_ = std::string(previous.get());
        }
        Require(_putenv_s(name_.c_str(), value) == 0, "test environment variable should be configurable");
    }

    ~ScopedEnvironmentVariable()
    {
        (void)_putenv_s(name_.c_str(), previous_ ? previous_->c_str() : "");
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
    std::string name_;
    std::optional<std::string> previous_;
};

class ScopedImGuiContext {
public:
    ScopedImGuiContext()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        unsigned char* font_pixels = nullptr;
        int font_width = 0;
        int font_height = 0;
        io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_width, &font_height);
        Require(font_pixels != nullptr && font_width > 0 && font_height > 0, "ImGui font atlas should build");
    }

    ~ScopedImGuiContext()
    {
        ImGui::DestroyContext();
    }

    ScopedImGuiContext(const ScopedImGuiContext&) = delete;
    ScopedImGuiContext& operator=(const ScopedImGuiContext&) = delete;
};

void TestDefaultEnvironmentDescribesThisBuild()
{
    ScopedEnvironmentVariable profile_directory("SPECFORGE_PROFILE_DIR", "");
    const specforge::SettingsPanelEnvironment environment =
        specforge::DefaultSettingsPanelEnvironment();

    Require(
        environment.version == SPECFORGE_EXPECTED_VERSION,
        "settings should expose the CMake project version");
    Require(!environment.release_profile.empty(), "settings should expose the release profile");
    Require(!environment.data_directory.empty(), "settings should expose the application data directory");
    Require(
        environment.log_directory.parent_path() == environment.data_directory,
        "the diagnostics directory should remain below application data");
}

void TestEnvironmentOverrideControlsDisplayedLogDirectory()
{
    const std::filesystem::path override_path = "C:/SpecForge/settings-profile-override";
    ScopedEnvironmentVariable profile_directory("SPECFORGE_PROFILE_DIR", override_path.string().c_str());

    const specforge::SettingsPanelEnvironment environment =
        specforge::DefaultSettingsPanelEnvironment();

    Require(
        environment.log_directory == override_path,
        "settings should display the same overridden directory used by ProfileSink");
}

void TestOpenIsIdempotent()
{
    specforge::SettingsPanelUi panel({
        .version = "test",
        .release_profile = "Portable",
        .data_directory = "Data",
        .log_directory = "Data/logs",
    });

    Require(!panel.open(), "settings should start closed");
    panel.Open();
    panel.Open();
    Require(panel.open(), "opening settings repeatedly should retain one open panel");
}

void TestClosedToOpenClearsTransientFeedback()
{
    specforge::SettingsPanelUi panel({
        .version = "test",
        .release_profile = "Portable",
        .data_directory = "Data",
        .log_directory = "Data/logs",
    });

    panel.Open();
    specforge::SettingsPanelUiTestAccess::SetTransientFeedback(panel, "old feedback", true);
    panel.Open();
    Require(
        specforge::SettingsPanelUiTestAccess::TransientFeedback(panel) == "old feedback",
        "refocusing an open panel should preserve current feedback");

    specforge::SettingsPanelUiTestAccess::Close(panel);
    panel.Open();
    Require(
        specforge::SettingsPanelUiTestAccess::TransientFeedback(panel).empty(),
        "reopening a closed panel should clear stale feedback");
    Require(
        !specforge::SettingsPanelUiTestAccess::ActionFailed(panel),
        "reopening a closed panel should clear stale failure state");
}

void TestRenderSmoke()
{
    ScopedImGuiContext imgui;
    specforge::SettingsPanelUi panel({
        .version = "test",
        .release_profile = "Portable",
        .data_directory = "Data",
        .log_directory = "Data/logs",
    });
    panel.Open();

    constexpr specforge::SettingsSection sections[] = {
        specforge::SettingsSection::General,
        specforge::SettingsSection::Appearance,
        specforge::SettingsSection::Language,
        specforge::SettingsSection::Input,
        specforge::SettingsSection::DataAndRecovery,
        specforge::SettingsSection::About,
    };
    for (const specforge::SettingsSection section : sections) {
        specforge::SettingsPanelUiTestAccess::SelectSection(panel, section);
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.DisplaySize = ImVec2(1280.0f, 720.0f);
        ImGui::NewFrame();
        panel.Render();
        Require(
            ImGui::FindWindowByName("Settings###SpecForgeSettingsV1") != nullptr,
            "rendering an open settings panel should create its ImGui window");
        Require(panel.open(), "rendering every settings section should keep the non-modal panel open");
        ImGui::EndFrame();
    }
}

}  // namespace

int main()
{
    TestDefaultEnvironmentDescribesThisBuild();
    TestEnvironmentOverrideControlsDisplayedLogDirectory();
    TestOpenIsIdempotent();
    TestClosedToOpenClearsTransientFeedback();
    TestRenderSmoke();
    std::cout << "settings panel tests passed\n";
    return 0;
}
