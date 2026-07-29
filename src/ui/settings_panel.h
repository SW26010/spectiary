#pragma once

#include "app/application_settings.h"
#include "app/runtime_paths.h"

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

struct BuildSourceIdentity {
    std::string mode;
    std::string revision;
};

struct SettingsPanelEnvironment {
    std::string version;
    std::string distribution;
    std::string configuration;
    std::string target_architecture;
    BuildSourceIdentity build_source;
    BuildMetadataReadResult build_metadata;
    std::filesystem::path data_directory;
};

struct SettingsPanelStatus {
    bool profile_open = false;
    bool profile_stopping = false;
    const std::filesystem::path* profile_path = nullptr;
    std::string_view profile_status_message;
    bool frame_capture_enabled = false;
    bool frame_capture_pending = false;
    bool window_renderable = true;
    const std::filesystem::path*
        frame_capture_output_directory = nullptr;
    const std::filesystem::path*
        last_frame_capture_path = nullptr;
    std::string_view frame_capture_status_message;
};

[[nodiscard]] SettingsPanelEnvironment
SettingsPanelEnvironmentForStartup(
    const SpecForgeStartup& startup);
[[nodiscard]] std::string FormatBuildSourceForAbout(
    const BuildSourceIdentity& build_source);
[[nodiscard]] std::string_view FormatBuildMetadataStatusForAbout(
    BuildMetadataStatus status);
[[nodiscard]] std::string_view
FormatProfileOutputDirectoryStatus(
    ApplicationSettingsStatusKind kind);
[[nodiscard]] std::string FormatDiagnosticInformation(
    const SettingsPanelEnvironment& environment,
    const std::filesystem::path& profile_output_directory);

class SettingsPanelUi {
public:
    explicit SettingsPanelUi(SettingsPanelEnvironment environment);

    void Open();
    void Render(
        const ApplicationSettingsView& settings,
        const SettingsPanelStatus& status = {});
    [[nodiscard]] bool TakeProfileRecordingToggleRequest();
    [[nodiscard]] bool TakeFrameCaptureRequest();
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
    void RenderAppearance(
        const ApplicationSettingsView& settings);
    void RenderLanguage(const ApplicationSettingsView& settings);
    void RenderInput();
    void RenderDataAndRecovery();
    void RenderDiagnostics(
        const ApplicationSettingsView& settings,
        const SettingsPanelStatus& status);
    void RenderAbout(const ApplicationSettingsView& settings);
    void ResetProfileOutputDirectory();
    void SetUiScalePercentage(int percentage);
    [[nodiscard]] static bool ShouldSubmitLanguageSelection(
        const ApplicationSettingsView& settings,
        UiLanguage candidate);
    [[nodiscard]] static bool
    CanRestoreProfileOutputDirectory(
        const ApplicationSettingsView& settings,
        const SettingsPanelStatus& status);

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
    bool frame_capture_requested_ = false;
    bool profile_output_directory_selection_requested_ = false;
    unsigned int settings_viewport_id_ = 0;
    std::string action_status_;
    std::optional<ApplicationSettingsIntent>
        application_settings_intent_;
    std::optional<int> ui_scale_draft_percentage_;

    friend struct SettingsPanelUiTestAccess;
};

}  // namespace specforge
