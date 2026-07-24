#pragma once

#include "app/application_settings.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace specforge {

enum class SettingsSection {
    General,
    Appearance,
    Language,
    Input,
    DataAndRecovery,
    Diagnostics,
    About,
};

struct SettingsPanelEnvironment {
    std::string version;
    std::string release_profile;
    std::filesystem::path data_directory;
};

struct SettingsPanelStatus {
    bool profile_open = false;
    bool profile_stopping = false;
    const std::filesystem::path* profile_path = nullptr;
    std::string_view profile_status_message;
};

[[nodiscard]] SettingsPanelEnvironment DefaultSettingsPanelEnvironment();

class SettingsPanelUi {
public:
    SettingsPanelUi();
    explicit SettingsPanelUi(SettingsPanelEnvironment environment);

    void Open();
    void Render(
        const ApplicationSettingsView& settings,
        const SettingsPanelStatus& status = {});
    [[nodiscard]] bool TakeProfileRecordingToggleRequest();
    [[nodiscard]] bool TakeProfileOutputDirectorySelectionRequest();
    [[nodiscard]] std::optional<ApplicationSettingsIntent>
    TakeApplicationSettingsIntent();

    [[nodiscard]] bool open() const;

private:
    void RenderNavigation(UiLanguage language);
    void RenderSelectedSection(
        const ApplicationSettingsView& settings,
        const SettingsPanelStatus& status);
    void RenderGeneral();
    void RenderAppearance();
    void RenderLanguage(const ApplicationSettingsView& settings);
    void RenderInput();
    void RenderDataAndRecovery();
    void RenderDiagnostics(
        const ApplicationSettingsView& settings,
        const SettingsPanelStatus& status);
    void RenderAbout(const ApplicationSettingsView& settings);
    void ResetProfileOutputDirectory();

    void OpenDirectory(const std::filesystem::path& path, const char* label);
    void CopyPath(const std::filesystem::path& path, const char* label);
    void CopyDiagnosticInformation(
        const std::filesystem::path& profile_output_directory);

    SettingsPanelEnvironment environment_;
    SettingsSection selected_section_ = SettingsSection::General;
    bool open_ = false;
    bool focus_requested_ = false;
    bool content_scroll_reset_requested_ = false;
    bool action_failed_ = false;
    bool profile_recording_toggle_requested_ = false;
    bool profile_output_directory_selection_requested_ = false;
    std::string action_status_;
    std::optional<ApplicationSettingsIntent>
        application_settings_intent_;

    friend struct SettingsPanelUiTestAccess;
};

}  // namespace specforge
