#include "ui/settings_panel.h"

#include "app/runtime_paths.h"
#include "profile/profile_sink.h"

#include <Windows.h>
#include <shellapi.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>

#ifndef SPECFORGE_VERSION
#define SPECFORGE_VERSION "development"
#endif

namespace specforge {
namespace {

constexpr const char* kSettingsWindow = "Settings###SpecForgeSettingsV1";
constexpr float kMinimumNavigationWidth = 190.0f;
constexpr float kInitialSettingsWidth = 860.0f;
constexpr float kInitialSettingsHeight = 560.0f;

constexpr std::array<SettingsSection, 6> kSettingsSections = {
    SettingsSection::General,
    SettingsSection::Appearance,
    SettingsSection::Language,
    SettingsSection::Input,
    SettingsSection::DataAndRecovery,
    SettingsSection::About,
};

const char* SectionLabel(SettingsSection section)
{
    switch (section) {
    case SettingsSection::General:
        return "General";
    case SettingsSection::Appearance:
        return "Appearance";
    case SettingsSection::Language:
        return "Language";
    case SettingsSection::Input:
        return "Input";
    case SettingsSection::DataAndRecovery:
        return "Data & Recovery";
    case SettingsSection::About:
        return "About";
    }
    return "Settings";
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

void RenderSectionHeading(const char* title, const char* description)
{
    ImGui::TextUnformatted(title);
    ImGui::Separator();
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled("%s", description);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
}

void RenderUnavailableNote(const char* explanation)
{
    ImGui::Spacing();
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled("Not available yet. %s", explanation);
    ImGui::PopTextWrapPos();
}

void RenderReadOnlyValue(const char* label, const char* value)
{
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine();
    ImGui::TextUnformatted(value);
}

}  // namespace

SettingsPanelEnvironment DefaultSettingsPanelEnvironment()
{
    const RuntimePaths paths = DefaultRuntimePaths();
    return {
        .version = SPECFORGE_VERSION,
        .release_profile = ReleaseProfileName(paths.release_profile),
        .data_directory = paths.local_user_state_root,
        .log_directory = ProfileSink::EffectiveOutputDirectory(),
    };
}

SettingsPanelUi::SettingsPanelUi()
    : SettingsPanelUi(DefaultSettingsPanelEnvironment())
{
}

SettingsPanelUi::SettingsPanelUi(SettingsPanelEnvironment environment)
    : environment_(std::move(environment))
{
}

void SettingsPanelUi::Open()
{
    if (!open_) {
        action_failed_ = false;
        action_status_.clear();
    }
    open_ = true;
    focus_requested_ = true;
}

void SettingsPanelUi::Render()
{
    if (!open_) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 initial_size(kInitialSettingsWidth, kInitialSettingsHeight);
    const ImVec2 initial_position(
        viewport->WorkPos.x + std::max(0.0f, viewport->WorkSize.x - initial_size.x) * 0.5f,
        viewport->WorkPos.y + std::max(0.0f, viewport->WorkSize.y - initial_size.y) * 0.5f);
    ImGui::SetNextWindowSize(initial_size, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        initial_position,
        ImGuiCond_FirstUseEver);
    if (focus_requested_) {
        ImGui::SetNextWindowFocus();
        focus_requested_ = false;
    }

    if (!ImGui::Begin(kSettingsWindow, &open_, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    const float navigation_width = std::max(
        kMinimumNavigationWidth,
        ImGui::CalcTextSize(SectionLabel(SettingsSection::DataAndRecovery)).x +
            ImGui::GetStyle().WindowPadding.x * 2.0f);
    if (ImGui::BeginChild("##SettingsNavigation", ImVec2(navigation_width, 0.0f), true)) {
        RenderNavigation();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    if (ImGui::BeginChild("##SettingsContent", ImVec2(0.0f, 0.0f), false)) {
        if (content_scroll_reset_requested_) {
            ImGui::SetScrollY(0.0f);
            content_scroll_reset_requested_ = false;
        }
        RenderSelectedSection();
    }
    ImGui::EndChild();
    ImGui::End();
}

bool SettingsPanelUi::open() const
{
    return open_;
}

void SettingsPanelUi::RenderNavigation()
{
    ImGui::TextDisabled("SPECFORGE");
    ImGui::Spacing();
    for (const SettingsSection section : kSettingsSections) {
        const bool selected = selected_section_ == section;
        if (ImGui::Selectable(SectionLabel(section), selected)) {
            selected_section_ = section;
            content_scroll_reset_requested_ = true;
            action_status_.clear();
        }
    }
}

void SettingsPanelUi::RenderSelectedSection()
{
    switch (selected_section_) {
    case SettingsSection::General:
        RenderGeneral();
        return;
    case SettingsSection::Appearance:
        RenderAppearance();
        return;
    case SettingsSection::Language:
        RenderLanguage();
        return;
    case SettingsSection::Input:
        RenderInput();
        return;
    case SettingsSection::DataAndRecovery:
        RenderDataAndRecovery();
        return;
    case SettingsSection::About:
        RenderAbout();
        return;
    }
}

void SettingsPanelUi::RenderGeneral()
{
    RenderSectionHeading("General", "Choose how SpecForge starts and restores your local workspace.");

    bool restore_previous_session = true;
    ImGui::BeginDisabled();
    ImGui::Checkbox("Restore the previous session at startup", &restore_previous_session);
    ImGui::EndDisabled();
    RenderUnavailableNote("Session restoration is currently managed automatically.");
}

void SettingsPanelUi::RenderAppearance()
{
    RenderSectionHeading("Appearance", "Adjust the application theme without changing scientific plot semantics.");

    int theme = 2;
    float ui_scale = 100.0f;
    float accent_color[3] = {0.24f, 0.55f, 0.86f};
    ImGui::BeginDisabled();
    ImGui::Combo("Theme", &theme, "Follow system\0Light\0Dark\0");
    ImGui::ColorEdit3("Accent color", accent_color, ImGuiColorEditFlags_NoInputs);
    ImGui::SliderFloat("UI scale", &ui_scale, 80.0f, 150.0f, "%.0f%%", ImGuiSliderFlags_None);
    ImGui::EndDisabled();
    RenderUnavailableNote("The current UI uses the built-in dark style.");
}

void SettingsPanelUi::RenderLanguage()
{
    RenderSectionHeading("Language", "Select the language used by menus, panels, messages, and diagnostics.");

    int language = 0;
    ImGui::BeginDisabled();
    ImGui::Combo("Application language", &language, "English\0Simplified Chinese\0");
    ImGui::EndDisabled();
    RenderUnavailableNote("The interface is not localized yet.");
}

void SettingsPanelUi::RenderInput()
{
    RenderSectionHeading("Input", "Tune mouse, touchpad, and keyboard behavior for spectrum inspection.");

    float mouse_zoom_sensitivity = 1.0f;
    float touchpad_zoom_sensitivity = 1.0f;
    bool reverse_zoom_direction = false;
    ImGui::BeginDisabled();
    ImGui::SliderFloat("Mouse zoom sensitivity", &mouse_zoom_sensitivity, 0.5f, 2.0f, "%.1fx");
    ImGui::SliderFloat("Touchpad zoom sensitivity", &touchpad_zoom_sensitivity, 0.5f, 2.0f, "%.1fx");
    ImGui::Checkbox("Reverse zoom direction", &reverse_zoom_direction);
    ImGui::Button("View keyboard shortcuts");
    ImGui::EndDisabled();
    RenderUnavailableNote("Input behavior currently follows the built-in interaction model.");
}

void SettingsPanelUi::RenderDataAndRecovery()
{
    RenderSectionHeading(
        "Data & Recovery",
        "Inspect local application storage. Scientific source files and label result files remain user-owned.");

    const std::string data_path = PathToUtf8(environment_.data_directory);
    ImGui::TextUnformatted("Application data");
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled("%s", data_path.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Open Data Folder")) {
        OpenDirectory(environment_.data_directory, "data folder");
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy Path##Data")) {
        CopyPath(environment_.data_directory, "Data path copied.");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Configuration portability");
    ImGui::BeginDisabled();
    ImGui::Button("Import Settings...");
    ImGui::SameLine();
    ImGui::Button("Export Settings...");
    ImGui::EndDisabled();
    RenderUnavailableNote("A public, versioned settings-file format has not been defined.");

    ImGui::Spacing();
    ImGui::SeparatorText("Recovery and reset");
    ImGui::BeginDisabled();
    ImGui::Button("Reset Window Layout");
    ImGui::Button("Reset Application Settings");
    ImGui::Button("Erase All Application State...");
    ImGui::EndDisabled();
    RenderUnavailableNote("Reset operations need explicit data boundaries and confirmation behavior.");

    if (!action_status_.empty()) {
        ImGui::Spacing();
        if (action_failed_) {
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 1.0f), "%s", action_status_.c_str());
        } else {
            ImGui::TextDisabled("%s", action_status_.c_str());
        }
    }
}

void SettingsPanelUi::RenderAbout()
{
    RenderSectionHeading("About", "Version, release, storage, and diagnostic information for this build.");

    ImGui::TextUnformatted("SpecForge");
    ImGui::TextDisabled("Local astronomical spectrum inspection and labeling.");
    ImGui::Spacing();
    RenderReadOnlyValue("Version", environment_.version.c_str());
    RenderReadOnlyValue("Release profile", environment_.release_profile.c_str());
    RenderReadOnlyValue("Graphics", "Direct3D 11 / SDR");

    ImGui::Spacing();
    ImGui::SeparatorText("Diagnostics");
    const std::string log_path = PathToUtf8(environment_.log_directory);
    ImGui::TextDisabled("Performance logs");
    ImGui::PushTextWrapPos();
    ImGui::TextUnformatted(log_path.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Open Log Folder")) {
        OpenDirectory(environment_.log_directory, "log folder");
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy Diagnostic Information")) {
        CopyDiagnosticInformation();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Performance recording is available from the Performance menu.");
    if (!action_status_.empty()) {
        ImGui::Spacing();
        if (action_failed_) {
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 1.0f), "%s", action_status_.c_str());
        } else {
            ImGui::TextDisabled("%s", action_status_.c_str());
        }
    }
}

void SettingsPanelUi::OpenDirectory(const std::filesystem::path& path, const char* label)
{
    std::error_code directory_error;
    std::filesystem::create_directories(path, directory_error);
    if (directory_error) {
        action_failed_ = true;
        action_status_ = std::string("Could not prepare the ") + label + ".";
        return;
    }

    const HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<std::intptr_t>(result) <= 32) {
        action_failed_ = true;
        action_status_ = std::string("Could not open the ") + label + ".";
        return;
    }

    action_failed_ = false;
    action_status_ = std::string("Opened the ") + label + ".";
}

void SettingsPanelUi::CopyPath(const std::filesystem::path& path, const char* label)
{
    const std::string path_text = PathToUtf8(path);
    ImGui::SetClipboardText(path_text.c_str());
    action_failed_ = false;
    action_status_ = label;
}

void SettingsPanelUi::CopyDiagnosticInformation()
{
    std::string diagnostics;
    diagnostics.reserve(256);
    diagnostics += "SpecForge ";
    diagnostics += environment_.version;
    diagnostics += "\nRelease profile: ";
    diagnostics += environment_.release_profile;
    diagnostics += "\nGraphics: Direct3D 11 / SDR";
    diagnostics += "\nData directory: ";
    diagnostics += PathToUtf8(environment_.data_directory);
    diagnostics += "\nLog directory: ";
    diagnostics += PathToUtf8(environment_.log_directory);
    ImGui::SetClipboardText(diagnostics.c_str());
    action_failed_ = false;
    action_status_ = "Diagnostic information copied.";
}

}  // namespace specforge
