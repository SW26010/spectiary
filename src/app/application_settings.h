#pragma once

#include "profile/profile_settings.h"
#include "ui/panel_visibility_state_cache_io.h"
#include "ui/ui_text.h"

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
    ApplicationSettingsStatus status;
};

enum class ApplicationSettingsIntentKind {
    SetLanguage,
    SetProfileOutputDirectory,
    RestoreDefaultProfileOutputDirectory,
};

struct ApplicationSettingsIntent {
    ApplicationSettingsIntentKind kind =
        ApplicationSettingsIntentKind::SetLanguage;
    UiLanguage language = UiLanguage::English;
    std::filesystem::path directory;

    [[nodiscard]] static ApplicationSettingsIntent SetLanguage(
        UiLanguage language);
    [[nodiscard]] static ApplicationSettingsIntent SetProfileOutputDirectory(
        std::filesystem::path directory);
    [[nodiscard]] static ApplicationSettingsIntent
    RestoreDefaultProfileOutputDirectory();
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
        ApplicationSettingsIntent intent);

    [[nodiscard]] PanelVisibilityState& panel_visibility() noexcept;
    [[nodiscard]] const PanelVisibilityState& panel_visibility() const noexcept;
    void CommitPanelVisibilityChange(
        const PanelVisibilityState& previous);

    void RunMaintenance(LocalUserStateSaveScheduler::TimePoint now);
    [[nodiscard]] std::optional<LocalUserStateSaveScheduler::TimePoint>
    NextMaintenanceDeadline() const;
    [[nodiscard]] bool Flush();

private:
    [[nodiscard]] ApplicationSettingsResult ApplyLanguage(
        UiLanguage language);
    [[nodiscard]] ApplicationSettingsResult ApplyProfileOutputDirectory(
        std::optional<std::filesystem::path> directory);
    void SetStatus(
        ApplicationSettingsStatusKind kind,
        ApplicationSetting setting,
        std::string detail = {});
    void ClearStatus();

    ApplicationSettingsStorage storage_;
    UiLanguage language_ = UiLanguage::English;
    ProfileOutputDirectoryResolution profile_output_directory_;
    PanelVisibilityStatePersistence panel_visibility_persistence_;
    PanelVisibilityState panel_visibility_;
    ApplicationSettingsStatus status_;
};

}  // namespace specforge
