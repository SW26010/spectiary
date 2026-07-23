#pragma once

#include "profile/profile_settings.h"

#include <filesystem>
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
    std::filesystem::path log_directory;
    std::filesystem::path default_profile_output_directory;
    std::filesystem::path profile_settings_path;
    ProfileOutputDirectorySource profile_output_directory_source =
        ProfileOutputDirectorySource::Default;
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
    void Render(const SettingsPanelStatus& status = {});
    [[nodiscard]] bool TakeProfileRecordingToggleRequest();
    [[nodiscard]] bool TakeProfileOutputDirectorySelectionRequest();
    void ApplyProfileOutputDirectorySelection(std::filesystem::path directory);

    [[nodiscard]] bool open() const;

private:
    void RenderNavigation();
    void RenderSelectedSection(const SettingsPanelStatus& status);
    void RenderGeneral();
    void RenderAppearance();
    void RenderLanguage();
    void RenderInput();
    void RenderDataAndRecovery();
    void RenderDiagnostics(const SettingsPanelStatus& status);
    void RenderAbout();
    void ResetProfileOutputDirectory();

    void OpenDirectory(const std::filesystem::path& path, const char* label);
    void CopyPath(const std::filesystem::path& path, const char* label);
    void CopyDiagnosticInformation();

    SettingsPanelEnvironment environment_;
    SettingsSection selected_section_ = SettingsSection::General;
    bool open_ = false;
    bool focus_requested_ = false;
    bool content_scroll_reset_requested_ = false;
    bool action_failed_ = false;
    bool profile_recording_toggle_requested_ = false;
    bool profile_output_directory_selection_requested_ = false;
    std::string action_status_;

    friend struct SettingsPanelUiTestAccess;
};

}  // namespace specforge
