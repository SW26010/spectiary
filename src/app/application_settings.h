#pragma once

#include "profile/profile_settings.h"
#include "ui/input_settings.h"
#include "ui/panel_visibility_state_cache_io.h"
#include "ui/ui_scale_settings.h"
#include "ui/ui_text.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace specforge {

struct RuntimePaths;

enum class ApplicationSetting {
    None,
    Language,
    ProfileOutputDirectory,
    UiScale,
    Input,
    PanelVisibility,
};

inline constexpr std::size_t kApplicationSettingCount =
    static_cast<std::size_t>(
        ApplicationSetting::PanelVisibility) +
    1;

enum class ApplicationSettingsStatusKind {
    Ready,
    LoadWarning,
    PersistenceError,
    Rejected,
};

enum class ApplicationSettingsStatusReason {
    None,
    SavedValueUnreadable,
    SettingsWriteFailed,
    UnsupportedLanguage,
    UiScaleOutOfRange,
    EnvironmentOverrideActive,
    RecordingInProgress,
    EmptyProfileOutputDirectory,
};

struct ApplicationSettingsStatus {
    ApplicationSettingsStatusKind kind = ApplicationSettingsStatusKind::Ready;
    ApplicationSetting setting = ApplicationSetting::None;
    ApplicationSettingsStatusReason reason =
        ApplicationSettingsStatusReason::None;
    std::string detail;
};

struct ApplicationSettingsView {
    UiLanguage language = UiLanguage::English;
    int ui_scale_percentage = kDefaultUiScalePercentage;
    bool live_numeric_navigation =
        kDefaultLiveNumericNavigation;
    std::filesystem::path profile_output_directory;
    std::filesystem::path default_profile_output_directory;
    ProfileOutputDirectorySource profile_output_directory_source =
        ProfileOutputDirectorySource::Default;
    PanelVisibilityState panel_visibility;
    std::array<
        ApplicationSettingsStatus,
        kApplicationSettingCount>
        statuses;

    [[nodiscard]] const ApplicationSettingsStatus& StatusFor(
        ApplicationSetting setting) const noexcept
    {
        return statuses[static_cast<std::size_t>(setting)];
    }
};

enum class ApplicationPanel {
    Files,
    Navigation,
    Annotations,
    Labeling,
    Filters,
    Sorting,
    Smoothing,
    Information,
    SpectralLines,
};

inline constexpr std::size_t kApplicationPanelCount =
    static_cast<std::size_t>(
        ApplicationPanel::SpectralLines) +
    1;

[[nodiscard]] bool ApplicationPanelVisible(
    const PanelVisibilityState& visibility,
    ApplicationPanel panel) noexcept;
void SetApplicationPanelVisible(
    PanelVisibilityState& visibility,
    ApplicationPanel panel,
    bool visible) noexcept;

enum class ApplicationSettingsIntentKind {
    SetLanguage,
    SetUiScale,
    SetLiveNumericNavigation,
    SetProfileOutputDirectory,
    RestoreDefaultProfileOutputDirectory,
    SetPanelVisibility,
    TogglePanelVisibility,
    ShowAllPanels,
};

struct ApplicationSettingsIntent {
    ApplicationSettingsIntentKind kind =
        ApplicationSettingsIntentKind::SetLanguage;
    UiLanguage language = UiLanguage::English;
    int ui_scale_percentage = kDefaultUiScalePercentage;
    bool live_numeric_navigation =
        kDefaultLiveNumericNavigation;
    std::filesystem::path directory;
    ApplicationPanel panel = ApplicationPanel::Files;
    bool visible = true;

    [[nodiscard]] static ApplicationSettingsIntent SetLanguage(
        UiLanguage language);
    [[nodiscard]] static ApplicationSettingsIntent SetUiScale(
        int percentage);
    [[nodiscard]] static ApplicationSettingsIntent
    SetLiveNumericNavigation(bool enabled);
    [[nodiscard]] static ApplicationSettingsIntent SetProfileOutputDirectory(
        std::filesystem::path directory);
    [[nodiscard]] static ApplicationSettingsIntent
    RestoreDefaultProfileOutputDirectory();
    [[nodiscard]] static ApplicationSettingsIntent SetPanelVisibility(
        ApplicationPanel panel,
        bool visible);
    [[nodiscard]] static ApplicationSettingsIntent TogglePanelVisibility(
        ApplicationPanel panel);
    [[nodiscard]] static ApplicationSettingsIntent ShowAllPanels();
};

enum class ApplicationSettingsOutcome {
    Unchanged,
    Applied,
    Rejected,
    PersistenceFailed,
};

struct ApplicationSettingsResult {
    ApplicationSettingsOutcome outcome =
        ApplicationSettingsOutcome::Unchanged;
    ApplicationSetting setting = ApplicationSetting::None;
    std::string detail;

    [[nodiscard]] bool applied() const noexcept
    {
        return outcome == ApplicationSettingsOutcome::Applied;
    }
};

struct ApplicationSettingsRuntimeState {
    bool profile_recording_in_progress = false;
};

struct ApplicationSettingsStorage {
    std::filesystem::path language_settings_path;
    std::filesystem::path ui_scale_settings_path;
    std::filesystem::path input_settings_path;
    std::filesystem::path profile_settings_path;
    std::filesystem::path panel_visibility_path;
    std::filesystem::path default_profile_output_directory;
    std::optional<std::filesystem::path> profile_output_environment_override;
    bool persistent = true;
};

[[nodiscard]] ApplicationSettingsStorage
ApplicationSettingsStorageForRuntimePaths(
    const RuntimePaths& paths);

class ApplicationSettings {
public:
    explicit ApplicationSettings(ApplicationSettingsStorage storage);

    [[nodiscard]] ApplicationSettingsView View() const;
    [[nodiscard]] ApplicationSettingsResult Apply(
        ApplicationSettingsIntent intent,
        ApplicationSettingsRuntimeState runtime);

    void RunMaintenance(LocalUserStateSaveScheduler::TimePoint now);
    [[nodiscard]] std::optional<LocalUserStateSaveScheduler::TimePoint>
    NextMaintenanceDeadline() const;
    [[nodiscard]] bool Flush();
    [[nodiscard]] LocalUserStatePersistenceStatus
        PersistenceStatus(ApplicationSetting setting) const;

private:
    [[nodiscard]] ApplicationSettingsResult ApplyLanguage(
        UiLanguage language);
    [[nodiscard]] ApplicationSettingsResult ApplyUiScale(
        int percentage);
    [[nodiscard]] ApplicationSettingsResult
    ApplyLiveNumericNavigation(bool enabled);
    [[nodiscard]] ApplicationSettingsResult ApplyProfileOutputDirectory(
        std::optional<std::filesystem::path> directory,
        const ApplicationSettingsRuntimeState& runtime);
    [[nodiscard]] ApplicationSettingsResult ApplyPanelVisibility(
        ApplicationPanel panel,
        bool visible);
    [[nodiscard]] ApplicationSettingsResult ShowAllPanels();
    void SetStatus(
        ApplicationSettingsStatusKind kind,
        ApplicationSetting setting,
        ApplicationSettingsStatusReason reason,
        std::string detail = {});
    void ClearStatus(ApplicationSetting setting);
    void AdoptLoadWarning(
        ApplicationSetting setting,
        std::string warning);
    void PrepareSave(ApplicationSetting setting);
    void MarkSaveFailed(
        ApplicationSetting setting,
        const std::string& message);
    void MarkSaveSucceeded(ApplicationSetting setting);

    ApplicationSettingsStorage storage_;
    UiLanguage language_ = UiLanguage::English;
    int ui_scale_percentage_ = kDefaultUiScalePercentage;
    bool live_numeric_navigation_ =
        kDefaultLiveNumericNavigation;
    ProfileOutputDirectoryResolution profile_output_directory_;
    PanelVisibilityStatePersistence panel_visibility_persistence_;
    PanelVisibilityState panel_visibility_;
    std::array<
        ApplicationSettingsStatus,
        kApplicationSettingCount>
        statuses_;
    std::array<std::string, kApplicationSettingCount>
        load_warnings_;
    std::array<
        LocalUserStateSaveStatus,
        kApplicationSettingCount>
        save_statuses_;
};

}  // namespace specforge
