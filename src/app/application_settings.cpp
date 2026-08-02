#include "app/application_settings.h"

#include "app/runtime_paths.h"
#include "ui/ui_language_settings.h"

#include <utility>

namespace specforge {
namespace {

template <typename Visibility>
decltype(auto) PanelVisibilityValue(
    Visibility& visibility,
    ApplicationPanel panel)
{
    switch (panel) {
    case ApplicationPanel::Files:
        return (visibility.files);
    case ApplicationPanel::Navigation:
        return (visibility.navigation);
    case ApplicationPanel::Annotations:
        return (visibility.annotations);
    case ApplicationPanel::Labeling:
        return (visibility.labeling);
    case ApplicationPanel::Filters:
        return (visibility.filters);
    case ApplicationPanel::Sorting:
        return (visibility.sorting);
    case ApplicationPanel::Smoothing:
        return (visibility.smoothing);
    case ApplicationPanel::Information:
        return (visibility.information);
    case ApplicationPanel::SpectralLines:
        return (visibility.spectral_lines);
    }
    return (visibility.files);
}

}  // namespace

bool ApplicationPanelVisible(
    const PanelVisibilityState& visibility,
    ApplicationPanel panel) noexcept
{
    return PanelVisibilityValue(visibility, panel);
}

void SetApplicationPanelVisible(
    PanelVisibilityState& visibility,
    ApplicationPanel panel,
    bool visible) noexcept
{
    PanelVisibilityValue(visibility, panel) = visible;
}

ApplicationSettingsIntent ApplicationSettingsIntent::SetLanguage(
    UiLanguage language)
{
    return {
        .kind = ApplicationSettingsIntentKind::SetLanguage,
        .language = language,
    };
}

ApplicationSettingsIntent ApplicationSettingsIntent::SetUiScale(
    int percentage)
{
    return {
        .kind = ApplicationSettingsIntentKind::SetUiScale,
        .ui_scale_percentage = percentage,
    };
}

ApplicationSettingsIntent
ApplicationSettingsIntent::SetLiveNumericNavigation(bool enabled)
{
    return {
        .kind =
            ApplicationSettingsIntentKind::
                SetLiveNumericNavigation,
        .live_numeric_navigation = enabled,
    };
}

ApplicationSettingsIntent
ApplicationSettingsIntent::SetProfileOutputDirectory(
    std::filesystem::path directory)
{
    return {
        .kind =
            ApplicationSettingsIntentKind::SetProfileOutputDirectory,
        .directory = std::move(directory),
    };
}

ApplicationSettingsIntent
ApplicationSettingsIntent::RestoreDefaultProfileOutputDirectory()
{
    return {
        .kind =
            ApplicationSettingsIntentKind::
                RestoreDefaultProfileOutputDirectory,
    };
}

ApplicationSettingsIntent ApplicationSettingsIntent::SetPanelVisibility(
    ApplicationPanel panel,
    bool visible)
{
    return {
        .kind = ApplicationSettingsIntentKind::SetPanelVisibility,
        .panel = panel,
        .visible = visible,
    };
}

ApplicationSettingsIntent ApplicationSettingsIntent::TogglePanelVisibility(
    ApplicationPanel panel)
{
    return {
        .kind = ApplicationSettingsIntentKind::TogglePanelVisibility,
        .panel = panel,
    };
}

ApplicationSettingsIntent ApplicationSettingsIntent::ShowAllPanels()
{
    return {
        .kind = ApplicationSettingsIntentKind::ShowAllPanels,
    };
}

ApplicationSettingsStorage
ApplicationSettingsStorageForRuntimePaths(
    const RuntimePaths& paths)
{
    return {
        .language_settings_path =
            paths.ui_language_settings_path,
        .ui_scale_settings_path =
            paths.ui_scale_settings_path,
        .input_settings_path =
            paths.input_settings_path,
        .profile_settings_path =
            paths.profile_settings_path,
        .panel_visibility_path =
            paths.panel_visibility_state_path,
        .default_profile_output_directory =
            paths.profile_log_directory,
        .profile_output_environment_override =
            ProfileOutputDirectoryEnvironmentOverride(),
    };
}

ApplicationSettings::ApplicationSettings(
    ApplicationSettingsStorage storage)
    : storage_(std::move(storage)),
      panel_visibility_persistence_(storage_.panel_visibility_path)
{
    for (std::size_t index = 0;
         index < statuses_.size();
         ++index) {
        statuses_[index].setting =
            static_cast<ApplicationSetting>(index);
    }
    if (!storage_.persistent) {
        profile_output_directory_ = ResolveProfileOutputDirectory(
            {},
            storage_.default_profile_output_directory,
            storage_.profile_output_environment_override);
        return;
    }

    UiLanguageSettingsLoadResult language_settings =
        LoadUiLanguageSettings(storage_.language_settings_path);
    language_ = language_settings.language;
    AdoptLoadWarning(
        ApplicationSetting::Language,
        std::move(language_settings.warning));

    UiScaleSettingsLoadResult ui_scale_settings =
        LoadUiScaleSettings(storage_.ui_scale_settings_path);
    ui_scale_percentage_ = ui_scale_settings.percentage;
    AdoptLoadWarning(
        ApplicationSetting::UiScale,
        std::move(ui_scale_settings.warning));

    InputSettingsLoadResult input_settings =
        LoadInputSettings(storage_.input_settings_path);
    live_numeric_navigation_ =
        input_settings.settings.live_numeric_navigation;
    AdoptLoadWarning(
        ApplicationSetting::Input,
        std::move(input_settings.warning));

    ProfileSettingsLoadResult profile_settings =
        LoadProfileSettings(storage_.profile_settings_path);
    profile_output_directory_ = ResolveProfileOutputDirectory(
        profile_settings.settings,
        storage_.default_profile_output_directory,
        storage_.profile_output_environment_override);
    AdoptLoadWarning(
        ApplicationSetting::ProfileOutputDirectory,
        std::move(profile_settings.warning));
    panel_visibility_ = panel_visibility_persistence_.Load();
    AdoptLoadWarning(
        ApplicationSetting::PanelVisibility,
        panel_visibility_persistence_
            .PersistenceStatus()
            .load_warning);
}

ApplicationSettingsView ApplicationSettings::View() const
{
    return {
        .language = language_,
        .ui_scale_percentage = ui_scale_percentage_,
        .live_numeric_navigation =
            live_numeric_navigation_,
        .profile_output_directory =
            profile_output_directory_.directory,
        .default_profile_output_directory =
            storage_.default_profile_output_directory,
        .profile_output_directory_source =
            profile_output_directory_.source,
        .panel_visibility = panel_visibility_,
        .statuses = statuses_,
    };
}

ApplicationSettingsResult ApplicationSettings::Apply(
    ApplicationSettingsIntent intent,
    ApplicationSettingsRuntimeState runtime)
{
    switch (intent.kind) {
    case ApplicationSettingsIntentKind::SetLanguage:
        return ApplyLanguage(intent.language);
    case ApplicationSettingsIntentKind::SetUiScale:
        return ApplyUiScale(intent.ui_scale_percentage);
    case ApplicationSettingsIntentKind::
        SetLiveNumericNavigation:
        return ApplyLiveNumericNavigation(
            intent.live_numeric_navigation);
    case ApplicationSettingsIntentKind::SetProfileOutputDirectory:
        return ApplyProfileOutputDirectory(
            std::move(intent.directory),
            runtime);
    case ApplicationSettingsIntentKind::
        RestoreDefaultProfileOutputDirectory:
        return ApplyProfileOutputDirectory(std::nullopt, runtime);
    case ApplicationSettingsIntentKind::SetPanelVisibility:
        return ApplyPanelVisibility(intent.panel, intent.visible);
    case ApplicationSettingsIntentKind::TogglePanelVisibility:
        return ApplyPanelVisibility(
            intent.panel,
            !PanelVisibilityValue(
                panel_visibility_,
                intent.panel));
    case ApplicationSettingsIntentKind::ShowAllPanels:
        return ShowAllPanels();
    }
    return {};
}

void ApplicationSettings::RunMaintenance(
    LocalUserStateSaveScheduler::TimePoint now)
{
    if (!storage_.persistent) {
        return;
    }
    const std::optional<bool> saved =
        panel_visibility_persistence_.RunMaintenance(
            panel_visibility_,
            now);
    if (!saved) {
        return;
    }
    if (*saved) {
        ClearStatus(ApplicationSetting::PanelVisibility);
        return;
    }
    SetStatus(
        ApplicationSettingsStatusKind::PersistenceError,
        ApplicationSetting::PanelVisibility,
        ApplicationSettingsStatusReason::SettingsWriteFailed,
        "Could not save panel visibility.");
}

std::optional<LocalUserStateSaveScheduler::TimePoint>
ApplicationSettings::NextMaintenanceDeadline() const
{
    if (!storage_.persistent) {
        return std::nullopt;
    }
    return panel_visibility_persistence_.NextMaintenanceDeadline();
}

bool ApplicationSettings::Flush()
{
    if (!storage_.persistent) {
        return true;
    }
    if (panel_visibility_persistence_.Flush(panel_visibility_)) {
        if (panel_visibility_persistence_
                .PersistenceStatus()
                .load_warning.empty()) {
            ClearStatus(ApplicationSetting::PanelVisibility);
        }
        return true;
    }
    SetStatus(
        ApplicationSettingsStatusKind::PersistenceError,
        ApplicationSetting::PanelVisibility,
        ApplicationSettingsStatusReason::SettingsWriteFailed,
        "Could not save panel visibility.");
    return false;
}

LocalUserStatePersistenceStatus
ApplicationSettings::PersistenceStatus(
    ApplicationSetting setting) const
{
    if (setting == ApplicationSetting::PanelVisibility) {
        return panel_visibility_persistence_.PersistenceStatus();
    }
    const std::size_t index =
        static_cast<std::size_t>(setting);
    if (index >= kApplicationSettingCount) {
        return {};
    }
    const LocalUserStateSaveStatus& save_status =
        save_statuses_[index];
    const ApplicationSettingsStatus& status =
        statuses_[index];
    return {
        .recovered = save_status.recovered(),
        .load_warning = load_warnings_[index],
        .save_message = save_status.message(),
        .load_diagnostic_detail =
            status.kind ==
                    ApplicationSettingsStatusKind::
                        LoadWarning
                ? status.detail
                : std::string{},
        .save_diagnostic_detail =
            save_status.message(),
    };
}

ApplicationSettingsResult ApplicationSettings::ApplyUiScale(
    int percentage)
{
    constexpr ApplicationSetting kSetting =
        ApplicationSetting::UiScale;
    if (percentage == ui_scale_percentage_ &&
        statuses_[static_cast<std::size_t>(kSetting)].kind ==
            ApplicationSettingsStatusKind::Ready) {
        return {
            .outcome = ApplicationSettingsOutcome::Unchanged,
            .setting = kSetting,
        };
    }
    if (!IsValidUiScalePercentage(percentage)) {
        const std::string detail =
            "The UI scale must be from 80% through 150%.";
        SetStatus(
            ApplicationSettingsStatusKind::Rejected,
            kSetting,
            ApplicationSettingsStatusReason::UiScaleOutOfRange,
            detail);
        return {
            .outcome = ApplicationSettingsOutcome::Rejected,
            .setting = kSetting,
            .detail = detail,
        };
    }

    std::string error;
    PrepareSave(kSetting);
    if (storage_.persistent &&
        !SaveUiScaleSettings(
            storage_.ui_scale_settings_path,
            percentage,
            &error)) {
        MarkSaveFailed(kSetting, error);
        return {
            .outcome =
                ApplicationSettingsOutcome::PersistenceFailed,
            .setting = kSetting,
            .detail = std::move(error),
        };
    }

    ui_scale_percentage_ = percentage;
    MarkSaveSucceeded(kSetting);
    return {
        .outcome = ApplicationSettingsOutcome::Applied,
        .setting = kSetting,
    };
}

ApplicationSettingsResult ApplicationSettings::ApplyLanguage(
    UiLanguage language)
{
    constexpr ApplicationSetting kSetting = ApplicationSetting::Language;
    if (language == language_ &&
        statuses_[static_cast<std::size_t>(kSetting)].kind ==
            ApplicationSettingsStatusKind::Ready) {
        return {
            .outcome = ApplicationSettingsOutcome::Unchanged,
            .setting = kSetting,
        };
    }
    if (language == UiLanguage::Count) {
        const std::string detail =
            "The application language is not supported.";
        SetStatus(
            ApplicationSettingsStatusKind::Rejected,
            kSetting,
            ApplicationSettingsStatusReason::UnsupportedLanguage,
            detail);
        return {
            .outcome = ApplicationSettingsOutcome::Rejected,
            .setting = kSetting,
            .detail = detail,
        };
    }

    std::string error;
    PrepareSave(kSetting);
    if (storage_.persistent &&
        !SaveUiLanguageSettings(
            storage_.language_settings_path,
            language,
            &error)) {
        MarkSaveFailed(kSetting, error);
        return {
            .outcome =
                ApplicationSettingsOutcome::PersistenceFailed,
            .setting = kSetting,
            .detail = std::move(error),
        };
    }

    language_ = language;
    MarkSaveSucceeded(kSetting);
    return {
        .outcome = ApplicationSettingsOutcome::Applied,
        .setting = kSetting,
    };
}

ApplicationSettingsResult
ApplicationSettings::ApplyLiveNumericNavigation(bool enabled)
{
    constexpr ApplicationSetting kSetting =
        ApplicationSetting::Input;
    if (enabled == live_numeric_navigation_ &&
        statuses_[static_cast<std::size_t>(kSetting)].kind ==
            ApplicationSettingsStatusKind::Ready) {
        return {
            .outcome = ApplicationSettingsOutcome::Unchanged,
            .setting = kSetting,
        };
    }

    std::string error;
    PrepareSave(kSetting);
    if (storage_.persistent &&
        !SaveInputSettings(
            storage_.input_settings_path,
            {.live_numeric_navigation = enabled},
            &error)) {
        MarkSaveFailed(kSetting, error);
        return {
            .outcome =
                ApplicationSettingsOutcome::PersistenceFailed,
            .setting = kSetting,
            .detail = std::move(error),
        };
    }

    live_numeric_navigation_ = enabled;
    MarkSaveSucceeded(kSetting);
    return {
        .outcome = ApplicationSettingsOutcome::Applied,
        .setting = kSetting,
    };
}

ApplicationSettingsResult
ApplicationSettings::ApplyProfileOutputDirectory(
    std::optional<std::filesystem::path> directory,
    const ApplicationSettingsRuntimeState& runtime)
{
    constexpr ApplicationSetting kSetting =
        ApplicationSetting::ProfileOutputDirectory;
    if (profile_output_directory_.source ==
        ProfileOutputDirectorySource::Environment) {
        const std::string detail =
            "The output directory is controlled by "
            "SPECFORGE_PROFILE_DIR.";
        SetStatus(
            ApplicationSettingsStatusKind::Rejected,
            kSetting,
            ApplicationSettingsStatusReason::
                EnvironmentOverrideActive,
            detail);
        return {
            .outcome = ApplicationSettingsOutcome::Rejected,
            .setting = kSetting,
            .detail = detail,
        };
    }
    if (runtime.profile_recording_in_progress) {
        const std::string detail =
            "Stop the current recording before changing its output directory.";
        SetStatus(
            ApplicationSettingsStatusKind::Rejected,
            kSetting,
            ApplicationSettingsStatusReason::RecordingInProgress,
            detail);
        return {
            .outcome = ApplicationSettingsOutcome::Rejected,
            .setting = kSetting,
            .detail = detail,
        };
    }
    if (directory && directory->empty()) {
        const std::string detail =
            "The profile output directory cannot be empty.";
        SetStatus(
            ApplicationSettingsStatusKind::Rejected,
            kSetting,
            ApplicationSettingsStatusReason::
                EmptyProfileOutputDirectory,
            detail);
        return {
            .outcome = ApplicationSettingsOutcome::Rejected,
            .setting = kSetting,
            .detail = detail,
        };
    }

    ProfileSettings settings{.output_directory = directory};
    const ProfileOutputDirectoryResolution requested =
        ResolveProfileOutputDirectory(
            settings,
            storage_.default_profile_output_directory,
            std::nullopt);
    if (requested == profile_output_directory_ &&
        statuses_[static_cast<std::size_t>(kSetting)].kind ==
            ApplicationSettingsStatusKind::Ready) {
        return {
            .outcome = ApplicationSettingsOutcome::Unchanged,
            .setting = kSetting,
        };
    }

    std::string error;
    PrepareSave(kSetting);
    if (storage_.persistent &&
        !SaveProfileSettings(
            storage_.profile_settings_path,
            settings,
            &error)) {
        MarkSaveFailed(kSetting, error);
        return {
            .outcome =
                ApplicationSettingsOutcome::PersistenceFailed,
            .setting = kSetting,
            .detail = std::move(error),
        };
    }

    profile_output_directory_ = requested;
    MarkSaveSucceeded(kSetting);
    return {
        .outcome = ApplicationSettingsOutcome::Applied,
        .setting = kSetting,
    };
}

ApplicationSettingsResult ApplicationSettings::ApplyPanelVisibility(
    ApplicationPanel panel,
    bool visible)
{
    constexpr ApplicationSetting kSetting =
        ApplicationSetting::PanelVisibility;
    bool& current = PanelVisibilityValue(
        panel_visibility_,
        panel);
    if (current == visible) {
        return {
            .outcome = ApplicationSettingsOutcome::Unchanged,
            .setting = kSetting,
        };
    }

    const PanelVisibilityState previous = panel_visibility_;
    current = visible;
    if (storage_.persistent) {
        panel_visibility_persistence_.MarkDirtyIfChanged(
            previous,
            panel_visibility_);
    }
    return {
        .outcome = ApplicationSettingsOutcome::Applied,
        .setting = kSetting,
    };
}

ApplicationSettingsResult ApplicationSettings::ShowAllPanels()
{
    constexpr ApplicationSetting kSetting =
        ApplicationSetting::PanelVisibility;
    const PanelVisibilityState visible;
    if (panel_visibility_ == visible) {
        return {
            .outcome = ApplicationSettingsOutcome::Unchanged,
            .setting = kSetting,
        };
    }

    const PanelVisibilityState previous = panel_visibility_;
    panel_visibility_ = visible;
    if (storage_.persistent) {
        panel_visibility_persistence_.MarkDirtyIfChanged(
            previous,
            panel_visibility_);
    }
    return {
        .outcome = ApplicationSettingsOutcome::Applied,
        .setting = kSetting,
    };
}

void ApplicationSettings::SetStatus(
    ApplicationSettingsStatusKind kind,
    ApplicationSetting setting,
    ApplicationSettingsStatusReason reason,
    std::string detail)
{
    statuses_[static_cast<std::size_t>(setting)] = {
        .kind = kind,
        .setting = setting,
        .reason = reason,
        .detail = std::move(detail),
    };
}

void ApplicationSettings::ClearStatus(ApplicationSetting setting)
{
    statuses_[static_cast<std::size_t>(setting)] = {
        .setting = setting,
    };
}

void ApplicationSettings::AdoptLoadWarning(
    ApplicationSetting setting,
    std::string warning)
{
    const std::size_t index =
        static_cast<std::size_t>(setting);
    load_warnings_[index] = std::move(warning);
    if (!load_warnings_[index].empty()) {
        SetStatus(
            ApplicationSettingsStatusKind::LoadWarning,
            setting,
            ApplicationSettingsStatusReason::SavedValueUnreadable,
            load_warnings_[index]);
    }
}

void ApplicationSettings::PrepareSave(
    ApplicationSetting setting)
{
    save_statuses_[static_cast<std::size_t>(setting)]
        .ClearRecovered();
}

void ApplicationSettings::MarkSaveFailed(
    ApplicationSetting setting,
    const std::string& message)
{
    save_statuses_[static_cast<std::size_t>(setting)]
        .MarkFailed(message);
    SetStatus(
        ApplicationSettingsStatusKind::PersistenceError,
        setting,
        ApplicationSettingsStatusReason::SettingsWriteFailed,
        message);
}

void ApplicationSettings::MarkSaveSucceeded(
    ApplicationSetting setting)
{
    const std::size_t index =
        static_cast<std::size_t>(setting);
    load_warnings_[index].clear();
    save_statuses_[index].MarkSaveSucceeded();
    ClearStatus(setting);
}

}  // namespace specforge
