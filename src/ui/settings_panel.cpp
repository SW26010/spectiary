#include "ui/settings_panel.h"

#include "app/runtime_paths.h"
#include "ui/profile_recording_ui_state.h"
#include "specforge/third_party_versions.h"

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

constexpr float kMinimumNavigationWidth = 190.0f;
constexpr float kInitialSettingsWidth = 860.0f;
constexpr float kInitialSettingsHeight = 560.0f;

constexpr std::array<SettingsSection, 7> kSettingsSections = {
    SettingsSection::General,
    SettingsSection::Appearance,
    SettingsSection::Language,
    SettingsSection::Input,
    SettingsSection::DataAndRecovery,
    SettingsSection::Diagnostics,
    SettingsSection::About,
};

std::string StableUiLabel(
    UiLanguage language,
    UiTextId text_id,
    std::string_view stable_id)
{
    std::string label(UiText(language, text_id));
    label += "###";
    label += stable_id;
    return label;
}

std::string SettingsWindowLabel(UiLanguage language)
{
    return StableUiLabel(
        language,
        UiTextId::Settings,
        "SpecForgeSettingsV1");
}

std::string SectionLabel(
    SettingsSection section,
    UiLanguage language)
{
    switch (section) {
    case SettingsSection::General:
        return "General";
    case SettingsSection::Appearance:
        return "Appearance";
    case SettingsSection::Language:
        return StableUiLabel(
            language,
            UiTextId::Language,
            "SpecForgeSettingsLanguage");
    case SettingsSection::Input:
        return "Input";
    case SettingsSection::DataAndRecovery:
        return "Data & Recovery";
    case SettingsSection::Diagnostics:
        return "Diagnostics";
    case SettingsSection::About:
        return "About";
    }
    return "Settings";
}

UiTextId LanguageNameTextId(UiLanguage language)
{
    switch (language) {
    case UiLanguage::English:
        return UiTextId::EnglishLanguageName;
    case UiLanguage::SimplifiedChinese:
        return UiTextId::SimplifiedChineseLanguageName;
    case UiLanguage::Count:
        return UiTextId::EnglishLanguageName;
    }
    return UiTextId::EnglishLanguageName;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

void RenderSectionHeading(
    std::string_view title,
    std::string_view description)
{
    ImGui::TextUnformatted(
        title.data(),
        title.data() + title.size());
    ImGui::Separator();
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(description.size()),
        description.data());
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

void SettingsPanelUi::Render(
    const ApplicationSettingsView& settings,
    const SettingsPanelStatus& status)
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

    const std::string settings_window =
        SettingsWindowLabel(settings.language);
    if (!ImGui::Begin(
            settings_window.c_str(),
            &open_,
            ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    const std::string widest_navigation_label =
        SectionLabel(
            SettingsSection::DataAndRecovery,
            settings.language);
    const float navigation_width = std::max(
        kMinimumNavigationWidth,
        ImGui::CalcTextSize(widest_navigation_label.c_str()).x +
            ImGui::GetStyle().WindowPadding.x * 2.0f);
    if (ImGui::BeginChild("##SettingsNavigation", ImVec2(navigation_width, 0.0f), true)) {
        RenderNavigation(settings.language);
    }
    ImGui::EndChild();

    ImGui::SameLine();
    if (ImGui::BeginChild("##SettingsContent", ImVec2(0.0f, 0.0f), false)) {
        if (content_scroll_reset_requested_) {
            ImGui::SetScrollY(0.0f);
            content_scroll_reset_requested_ = false;
        }
        RenderSelectedSection(settings, status);
    }
    ImGui::EndChild();
    ImGui::End();
}

bool SettingsPanelUi::open() const
{
    return open_;
}

bool SettingsPanelUi::TakeProfileRecordingToggleRequest()
{
    const bool requested = profile_recording_toggle_requested_;
    profile_recording_toggle_requested_ = false;
    return requested;
}

bool SettingsPanelUi::TakeProfileOutputDirectorySelectionRequest()
{
    const bool requested = profile_output_directory_selection_requested_;
    profile_output_directory_selection_requested_ = false;
    return requested;
}

std::optional<ApplicationSettingsIntent>
SettingsPanelUi::TakeApplicationSettingsIntent()
{
    std::optional<ApplicationSettingsIntent> requested =
        std::exchange(application_settings_intent_, std::nullopt);
    return requested;
}

void SettingsPanelUi::RenderNavigation(UiLanguage language)
{
    ImGui::TextDisabled("SPECFORGE");
    ImGui::Spacing();
    for (const SettingsSection section : kSettingsSections) {
        const bool selected = selected_section_ == section;
        const std::string label = SectionLabel(section, language);
        if (ImGui::Selectable(label.c_str(), selected)) {
            selected_section_ = section;
            content_scroll_reset_requested_ = true;
            action_status_.clear();
        }
    }
}

void SettingsPanelUi::RenderSelectedSection(
    const ApplicationSettingsView& settings,
    const SettingsPanelStatus& status)
{
    switch (selected_section_) {
    case SettingsSection::General:
        RenderGeneral();
        return;
    case SettingsSection::Appearance:
        RenderAppearance();
        return;
    case SettingsSection::Language:
        RenderLanguage(settings);
        return;
    case SettingsSection::Input:
        RenderInput();
        return;
    case SettingsSection::DataAndRecovery:
        RenderDataAndRecovery();
        return;
    case SettingsSection::Diagnostics:
        RenderDiagnostics(settings, status);
        return;
    case SettingsSection::About:
        RenderAbout(settings);
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

void SettingsPanelUi::RenderLanguage(
    const ApplicationSettingsView& settings)
{
    const UiLanguage language = settings.language;
    RenderSectionHeading(
        UiText(language, UiTextId::Language),
        UiText(language, UiTextId::LanguagePageDescription));

    const std::string selector_label = StableUiLabel(
        language,
        UiTextId::ApplicationLanguage,
        "SpecForgeApplicationLanguage");
    const char* preview =
        UiText(language, LanguageNameTextId(language)).data();
    if (ImGui::BeginCombo(selector_label.c_str(), preview)) {
        constexpr std::array kLanguages = {
            UiLanguage::English,
            UiLanguage::SimplifiedChinese,
        };
        for (const UiLanguage candidate : kLanguages) {
            const bool selected = language == candidate;
            const std::string option_label = StableUiLabel(
                language,
                LanguageNameTextId(candidate),
                candidate == UiLanguage::English
                    ? "SpecForgeUiLanguageEnglish"
                    : "SpecForgeUiLanguageSimplifiedChinese");
            if (ImGui::Selectable(option_label.c_str(), selected) &&
                !selected) {
                application_settings_intent_ =
                    ApplicationSettingsIntent::SetLanguage(
                        candidate);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();
    ImGui::PushTextWrapPos();
    const std::string_view coverage =
        UiText(language, UiTextId::LocalizationInProgress);
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(coverage.size()),
        coverage.data());
    ImGui::PopTextWrapPos();

    const ApplicationSettingsStatus& setting_status =
        settings.StatusFor(ApplicationSetting::Language);
    if (setting_status.kind ==
        ApplicationSettingsStatusKind::Ready) {
        return;
    }

    ImGui::Spacing();
    const bool failed =
        setting_status.kind !=
        ApplicationSettingsStatusKind::LoadWarning;
    const UiTextId feedback_text_id = failed
        ? UiTextId::LanguageSaveError
        : UiTextId::LanguageLoadWarning;
    const std::string_view feedback =
        UiText(language, feedback_text_id);
    const ImVec4 feedback_color = failed
        ? ImVec4(0.95f, 0.35f, 0.30f, 1.0f)
        : ImVec4(0.95f, 0.75f, 0.30f, 1.0f);
    ImGui::PushTextWrapPos();
    ImGui::TextColored(
        feedback_color,
        "%.*s",
        static_cast<int>(feedback.size()),
        feedback.data());
    if (!setting_status.detail.empty()) {
        ImGui::TextDisabled(
            "%s",
            setting_status.detail.c_str());
    }
    ImGui::PopTextWrapPos();
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

void SettingsPanelUi::RenderDiagnostics(
    const ApplicationSettingsView& settings,
    const SettingsPanelStatus& status)
{
    RenderSectionHeading(
        "Diagnostics",
        "Record bounded performance profiles for investigating interaction and loading latency.");

    const ProfileRecordingUiPresentation presentation =
        ResolveProfileRecordingUiPresentation(
            status.profile_open,
            status.profile_stopping);
    ImGui::TextDisabled("Performance profile");
    ImGui::SameLine();
    ImGui::TextUnformatted(
        status.profile_open
            ? "Recording"
            : (status.profile_stopping ? "Finishing..." : "Not recording"));

    if (!presentation.menu_action_enabled) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(presentation.menu_action.data())) {
        profile_recording_toggle_requested_ = true;
    }
    if (!presentation.menu_action_enabled) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Automatically stops after 5 minutes or 100 MiB.");

    if (status.profile_path != nullptr) {
        ImGui::Spacing();
        ImGui::TextDisabled("Current profile");
        ImGui::PushTextWrapPos();
        const std::string active_path = PathToUtf8(*status.profile_path);
        ImGui::TextUnformatted(active_path.c_str());
        ImGui::PopTextWrapPos();
    }
    if (!status.profile_status_message.empty()) {
        ImGui::PushTextWrapPos();
        ImGui::TextDisabled(
            "%.*s",
            static_cast<int>(status.profile_status_message.size()),
            status.profile_status_message.data());
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Profile output directory");
    const std::string output_path =
        PathToUtf8(settings.profile_output_directory);
    ImGui::PushTextWrapPos();
    ImGui::TextUnformatted(output_path.c_str());
    ImGui::PopTextWrapPos();

    switch (settings.profile_output_directory_source) {
    case ProfileOutputDirectorySource::Default:
        ImGui::TextDisabled("Source: release-profile default");
        break;
    case ProfileOutputDirectorySource::UserSetting:
        ImGui::TextDisabled("Source: saved setting");
        break;
    case ProfileOutputDirectorySource::Environment:
        ImGui::TextDisabled("Source: SPECFORGE_PROFILE_DIR environment override");
        break;
    }

    const bool directory_editing_disabled =
        status.profile_open ||
        status.profile_stopping ||
        settings.profile_output_directory_source ==
            ProfileOutputDirectorySource::Environment;
    if (directory_editing_disabled) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Choose Folder...")) {
        profile_output_directory_selection_requested_ = true;
    }
    if (directory_editing_disabled) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    const bool reset_disabled =
        directory_editing_disabled ||
        settings.profile_output_directory_source !=
            ProfileOutputDirectorySource::UserSetting;
    if (reset_disabled) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Restore Default")) {
        ResetProfileOutputDirectory();
    }
    if (reset_disabled) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (ImGui::Button("Open Output Folder")) {
        OpenDirectory(
            settings.profile_output_directory,
            "profile output folder");
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy Path##ProfileOutput")) {
        CopyPath(
            settings.profile_output_directory,
            "Profile output path copied.");
    }

    if (settings.profile_output_directory_source ==
        ProfileOutputDirectorySource::Environment) {
        ImGui::PushTextWrapPos();
        ImGui::TextDisabled(
            "Remove SPECFORGE_PROFILE_DIR before changing this path in Settings.");
        ImGui::PopTextWrapPos();
    } else if (status.profile_open || status.profile_stopping) {
        ImGui::PushTextWrapPos();
        ImGui::TextDisabled(
            "Stop the current recording before changing its output directory.");
        ImGui::PopTextWrapPos();
    }

    const ApplicationSettingsStatus& setting_status =
        settings.StatusFor(
            ApplicationSetting::ProfileOutputDirectory);
    if (setting_status.kind !=
        ApplicationSettingsStatusKind::Ready) {
        ImGui::Spacing();
        ImGui::TextColored(
            ImVec4(0.95f, 0.35f, 0.30f, 1.0f),
            "Could not update the profile output directory.");
        if (!setting_status.detail.empty()) {
            ImGui::TextDisabled(
                "%s",
                setting_status.detail.c_str());
        }
    }

    if (!action_status_.empty()) {
        ImGui::Spacing();
        if (action_failed_) {
            ImGui::TextColored(
                ImVec4(0.95f, 0.35f, 0.30f, 1.0f),
                "%s",
                action_status_.c_str());
        } else {
            ImGui::TextDisabled("%s", action_status_.c_str());
        }
    }
}

void SettingsPanelUi::RenderAbout(
    const ApplicationSettingsView& settings)
{
    RenderSectionHeading("About", "Version, licensing, and diagnostic information for this build.");

    ImGui::TextUnformatted("SpecForge");
    ImGui::TextDisabled("Local astronomical spectrum inspection and labeling.");
    ImGui::PushTextWrapPos();
    ImGui::TextUnformatted("Copyright (c) 2026 SpecForge.");
    ImGui::TextDisabled("Proprietary software. Use is subject to the SpecForge EULA.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    RenderReadOnlyValue("Version", environment_.version.c_str());
    RenderReadOnlyValue("Release profile", environment_.release_profile.c_str());
    RenderReadOnlyValue("Graphics", "Direct3D 11 / SDR");

    ImGui::Spacing();
    ImGui::SeparatorText("Third-party components");
    ImGui::BulletText(
        "Dear ImGui %s (docking / Win32 / DirectX 11) - MIT License",
        build_info::kDearImGuiVersion);
    ImGui::BulletText("ImPlot %s - MIT License", build_info::kImPlotVersion);
    ImGui::BulletText("zlib %s - zlib License", build_info::kZlibVersion);
    ImGui::BulletText("Modified stb headers bundled with Dear ImGui - MIT License");
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled("Full terms: Legal/EULA.txt and Legal/THIRD_PARTY_NOTICES.txt.");
    ImGui::TextDisabled("Scientific data attribution: Legal/DATA_SOURCES.txt.");
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    ImGui::SeparatorText("Diagnostics");
    const std::string log_path =
        PathToUtf8(settings.profile_output_directory);
    ImGui::TextDisabled("Performance logs");
    ImGui::PushTextWrapPos();
    ImGui::TextUnformatted(log_path.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Open Log Folder")) {
        OpenDirectory(
            settings.profile_output_directory,
            "log folder");
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy Diagnostic Information")) {
        CopyDiagnosticInformation(
            settings.profile_output_directory);
    }

    if (!action_status_.empty()) {
        ImGui::Spacing();
        if (action_failed_) {
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 1.0f), "%s", action_status_.c_str());
        } else {
            ImGui::TextDisabled("%s", action_status_.c_str());
        }
    }
}

void SettingsPanelUi::ResetProfileOutputDirectory()
{
    application_settings_intent_ =
        ApplicationSettingsIntent::
            RestoreDefaultProfileOutputDirectory();
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

void SettingsPanelUi::CopyDiagnosticInformation(
    const std::filesystem::path& profile_output_directory)
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
    diagnostics += PathToUtf8(profile_output_directory);
    ImGui::SetClipboardText(diagnostics.c_str());
    action_failed_ = false;
    action_status_ = "Diagnostic information copied.";
}

}  // namespace specforge
