#pragma once

#include "app/application_settings.h"
#include "app/on_demand_frame_capture.h"
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

enum class ArtifactIdentityStatus {
    Available,
    Unavailable,
    Mismatch,
};

struct ArtifactIdentityResult {
    ArtifactIdentityStatus status =
        ArtifactIdentityStatus::Unavailable;
    std::string completed_at_utc;
    std::string metadata_sha256;
    std::string executable_sha256;
};

struct SettingsPanelEnvironment {
    std::string version;
    std::string distribution;
    std::string configuration;
    std::string target_architecture;
    std::filesystem::path executable_path;
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
    OnDemandFrameCaptureStatus frame_capture_status =
        OnDemandFrameCaptureStatus::None;
    std::string_view frame_capture_status_operation;
    std::string_view frame_capture_status_result;
};

[[nodiscard]] SettingsPanelEnvironment
SettingsPanelEnvironmentForStartup(
    const SpecForgeStartup& startup);
[[nodiscard]] std::string FormatBuildSourceForAbout(
    const BuildSourceIdentity& build_source,
    UiLanguage language = UiLanguage::English);
[[nodiscard]] std::string_view FormatBuildMetadataStatusForAbout(
    BuildMetadataStatus status,
    UiLanguage language = UiLanguage::English);
[[nodiscard]] ArtifactIdentityResult
VerifyExecutableArtifactIdentity(
    const std::filesystem::path& executable_path,
    const BuildMetadataReadResult& build_metadata);
[[nodiscard]] std::string_view
FormatArtifactIdentityStatusForAbout(
    ArtifactIdentityStatus status,
    UiLanguage language = UiLanguage::English);
[[nodiscard]] std::string_view
FormatProfileOutputDirectoryStatus(
    ApplicationSettingsStatusKind kind,
    UiLanguage language = UiLanguage::English);
[[nodiscard]] std::string_view
FormatApplicationSettingsStatusReason(
    ApplicationSettingsStatusReason reason,
    UiLanguage language = UiLanguage::English);
[[nodiscard]] std::string FormatDiagnosticInformation(
    const SettingsPanelEnvironment& environment,
    const std::filesystem::path& profile_output_directory,
    UiLanguage language = UiLanguage::English);

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
    [[nodiscard]] static std::string SectionLabel(
        SettingsSection section,
        UiLanguage language);
    [[nodiscard]] static std::string
    AppearanceThemeLabel(UiLanguage language);
    [[nodiscard]] static std::string
    AppearanceAccentColorLabel(UiLanguage language);
    [[nodiscard]] static float VisibleLabelWidth(
        std::string_view label);
    void RenderNavigation(UiLanguage language);
    void RenderSelectedSection(
        const ApplicationSettingsView& settings,
        const SettingsPanelStatus& status);
    void RenderGeneral(
        const ApplicationSettingsView& settings);
    void RenderAppearance(
        const ApplicationSettingsView& settings);
    void RenderLanguage(const ApplicationSettingsView& settings);
    void RenderInput(
        const ApplicationSettingsView& settings);
    void RenderDataAndRecovery(UiLanguage language);
    void RenderDiagnostics(
        const ApplicationSettingsView& settings,
        const SettingsPanelStatus& status);
    void RenderAbout(const ApplicationSettingsView& settings);
    [[nodiscard]] const ArtifactIdentityResult&
    ArtifactIdentityForAbout();
    void ResetProfileOutputDirectory();
    void SetUiScalePercentage(int percentage);
    void SetLiveNumericNavigation(bool enabled);
    void SetOpenExternalSourceAsFolder(bool enabled);
    [[nodiscard]] static bool ShouldSubmitLanguageSelection(
        const ApplicationSettingsView& settings,
        UiLanguage candidate);
    [[nodiscard]] static bool
    CanRestoreProfileOutputDirectory(
        const ApplicationSettingsView& settings,
        const SettingsPanelStatus& status);

    void OpenDirectory(
        const std::filesystem::path& path,
        UiLanguage language,
        UiTextId prepare_error,
        UiTextId open_error,
        UiTextId opened);
    void CopyPath(
        const std::filesystem::path& path,
        UiLanguage language,
        UiTextId copied);
    void CopyDiagnosticInformation(
        const std::filesystem::path& profile_output_directory,
        UiLanguage language);

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
    std::optional<ArtifactIdentityResult> artifact_identity_;

    friend struct SettingsPanelUiTestAccess;
};

}  // namespace specforge
