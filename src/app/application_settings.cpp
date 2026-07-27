#include "app/application_settings.h"

#include "app/runtime_paths.h"
#include "ui/ui_language_settings.h"

#include <utility>

namespace specforge {
namespace {

bool& PanelVisibilityValue(
    PanelVisibilityState& visibility,
    ApplicationPanel panel)
{
    switch (panel) {
    case ApplicationPanel::Files:
        return visibility.files;
    case ApplicationPanel::Navigation:
        return visibility.navigation;
    case ApplicationPanel::Annotations:
        return visibility.annotations;
    case ApplicationPanel::Labeling:
        return visibility.labeling;
    case ApplicationPanel::Filters:
        return visibility.filters;
    case ApplicationPanel::Sorting:
        return visibility.sorting;
    case ApplicationPanel::Smoothing:
        return visibility.smoothing;
    case ApplicationPanel::Information:
        return visibility.information;
    case ApplicationPanel::SpectralLines:
        return visibility.spectral_lines;
    }
    return visibility.files;
}

}  // namespace

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

ApplicationSettingsStorage DefaultApplicationSettingsStorage()
{
    const RuntimePaths paths = DefaultRuntimePaths();
    return {
        .language_settings_path = DefaultUiLanguageSettingsPath(),
        .ui_scale_settings_path = DefaultUiScaleSettingsPath(),
        .profile_settings_path = DefaultProfileSettingsPath(),
        .panel_visibility_path =
            DefaultPanelVisibilityStateCachePath(),
        .default_profile_output_directory =
            paths.profile_log_directory,
        .profile_output_environment_override =
            ProfileOutputDirectoryEnvironmentOverride(),
    };
}

ApplicationSettings::ApplicationSettings()
    : ApplicationSettings(DefaultApplicationSettingsStorage())
{
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
    if (!language_settings.warning.empty()) {
        SetStatus(
            ApplicationSettingsStatusKind::LoadWarning,
            ApplicationSetting::Language,
            std::move(language_settings.warning));
    }

    UiScaleSettingsLoadResult ui_scale_settings =
        LoadUiScaleSettings(storage_.ui_scale_settings_path);
    ui_scale_percentage_ = ui_scale_settings.percentage;
    if (!ui_scale_settings.warning.empty()) {
        SetStatus(
            ApplicationSettingsStatusKind::LoadWarning,
            ApplicationSetting::UiScale,
            std::move(ui_scale_settings.warning));
    }

    profile_output_directory_ = ResolveProfileOutputDirectory(
        LoadProfileSettings(storage_.profile_settings_path),
        storage_.default_profile_output_directory,
        storage_.profile_output_environment_override);
    panel_visibility_ = panel_visibility_persistence_.Load();
}

ApplicationSettingsView ApplicationSettings::View() const
{
    return {
        .language = language_,
        .ui_scale_percentage = ui_scale_percentage_,
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
        ClearStatus(ApplicationSetting::PanelVisibility);
        return true;
    }
    SetStatus(
        ApplicationSettingsStatusKind::PersistenceError,
        ApplicationSetting::PanelVisibility,
        "Could not save panel visibility.");
    return false;
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
            detail);
        return {
            .outcome = ApplicationSettingsOutcome::Rejected,
            .setting = kSetting,
            .detail = detail,
        };
    }

    std::string error;
    if (storage_.persistent &&
        !SaveUiScaleSettings(
            storage_.ui_scale_settings_path,
            percentage,
            &error)) {
        SetStatus(
            ApplicationSettingsStatusKind::PersistenceError,
            kSetting,
            error);
        return {
            .outcome =
                ApplicationSettingsOutcome::PersistenceFailed,
            .setting = kSetting,
            .detail = std::move(error),
        };
    }

    ui_scale_percentage_ = percentage;
    ClearStatus(kSetting);
    return {
        .outcome = ApplicationSettingsOutcome::Applied,
        .setting = kSetting,
    };
}

ApplicationSettingsResult ApplicationSettings::ApplyLanguage(
    UiLanguage language)
{
    constexpr ApplicationSetting kSetting = ApplicationSetting::Language;
    if (language == language_) {
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
            detail);
        return {
            .outcome = ApplicationSettingsOutcome::Rejected,
            .setting = kSetting,
            .detail = detail,
        };
    }

    std::string error;
    if (storage_.persistent &&
        !SaveUiLanguageSettings(
            storage_.language_settings_path,
            language,
            &error)) {
        SetStatus(
            ApplicationSettingsStatusKind::PersistenceError,
            kSetting,
            error);
        return {
            .outcome =
                ApplicationSettingsOutcome::PersistenceFailed,
            .setting = kSetting,
            .detail = std::move(error),
        };
    }

    language_ = language;
    ClearStatus(kSetting);
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
    if (requested == profile_output_directory_) {
        return {
            .outcome = ApplicationSettingsOutcome::Unchanged,
            .setting = kSetting,
        };
    }

    std::string error;
    if (storage_.persistent &&
        !SaveProfileSettings(
            storage_.profile_settings_path,
            settings,
            &error)) {
        SetStatus(
            ApplicationSettingsStatusKind::PersistenceError,
            kSetting,
            error);
        return {
            .outcome =
                ApplicationSettingsOutcome::PersistenceFailed,
            .setting = kSetting,
            .detail = std::move(error),
        };
    }

    profile_output_directory_ = requested;
    ClearStatus(kSetting);
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
    std::string detail)
{
    statuses_[static_cast<std::size_t>(setting)] = {
        .kind = kind,
        .setting = setting,
        .detail = std::move(detail),
    };
}

void ApplicationSettings::ClearStatus(ApplicationSetting setting)
{
    statuses_[static_cast<std::size_t>(setting)] = {
        .setting = setting,
    };
}

}  // namespace specforge
