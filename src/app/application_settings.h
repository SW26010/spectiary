#pragma once

#include "profile/profile_settings.h"
#include "ui/panel_visibility_state_cache_io.h"
#include "ui/ui_text.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace specforge {

enum class ApplicationSetting {
    None,
    Language,
    ProfileOutputDirectory,
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

struct ApplicationSettingsStatus {
    ApplicationSettingsStatusKind kind = ApplicationSettingsStatusKind::Ready;
    ApplicationSetting setting = ApplicationSetting::None;
    std::string detail;
};

struct ApplicationSettingsView {
    UiLanguage language = UiLanguage::English;
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

enum class ApplicationSettingsIntentKind {
    SetLanguage,
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
    std::filesystem::path directory;
    ApplicationPanel panel = ApplicationPanel::Files;
    bool visible = true;

    [[nodiscard]] static ApplicationSettingsIntent SetLanguage(
        UiLanguage language);
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
    std::filesystem::path profile_settings_path;
    std::filesystem::path panel_visibility_path;
    std::filesystem::path default_profile_output_directory;
    std::optional<std::filesystem::path> profile_output_environment_override;
    bool persistent = true;
};

[[nodiscard]] ApplicationSettingsStorage DefaultApplicationSettingsStorage();

class ApplicationSettings {
public:
    ApplicationSettings();
    explicit ApplicationSettings(ApplicationSettingsStorage storage);

    [[nodiscard]] ApplicationSettingsView View() const;
    [[nodiscard]] ApplicationSettingsResult Apply(
        ApplicationSettingsIntent intent,
        ApplicationSettingsRuntimeState runtime);

    void RunMaintenance(LocalUserStateSaveScheduler::TimePoint now);
    [[nodiscard]] std::optional<LocalUserStateSaveScheduler::TimePoint>
    NextMaintenanceDeadline() const;
    [[nodiscard]] bool Flush();

private:
    [[nodiscard]] ApplicationSettingsResult ApplyLanguage(
        UiLanguage language);
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
        std::string detail = {});
    void ClearStatus(ApplicationSetting setting);

    ApplicationSettingsStorage storage_;
    UiLanguage language_ = UiLanguage::English;
    ProfileOutputDirectoryResolution profile_output_directory_;
    PanelVisibilityStatePersistence panel_visibility_persistence_;
    PanelVisibilityState panel_visibility_;
    std::array<
        ApplicationSettingsStatus,
        kApplicationSettingCount>
        statuses_;
};

}  // namespace specforge
