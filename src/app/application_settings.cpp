#include "app/application_settings.h"

#include "app/runtime_paths.h"
#include "ui/ui_language_settings.h"

#include <utility>

namespace specforge {

ApplicationSettingsIntent ApplicationSettingsIntent::SetLanguage(
    UiLanguage language)
{
    return {
        .kind = ApplicationSettingsIntentKind::SetLanguage,
        .language = language,
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

ApplicationSettingsStorage DefaultApplicationSettingsStorage()
{
    const RuntimePaths paths = DefaultRuntimePaths();
    return {
        .language_settings_path = DefaultUiLanguageSettingsPath(),
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
        .profile_output_directory =
            profile_output_directory_.directory,
        .default_profile_output_directory =
            storage_.default_profile_output_directory,
        .profile_output_directory_source =
            profile_output_directory_.source,
        .status = status_,
    };
}

ApplicationSettingsResult ApplicationSettings::Apply(
    ApplicationSettingsIntent intent)
{
    switch (intent.kind) {
    case ApplicationSettingsIntentKind::SetLanguage:
        return ApplyLanguage(intent.language);
    case ApplicationSettingsIntentKind::SetProfileOutputDirectory:
        return ApplyProfileOutputDirectory(std::move(intent.directory));
    case ApplicationSettingsIntentKind::
        RestoreDefaultProfileOutputDirectory:
        return ApplyProfileOutputDirectory(std::nullopt);
    }
    return {};
}

PanelVisibilityState& ApplicationSettings::panel_visibility() noexcept
{
    return panel_visibility_;
}

const PanelVisibilityState&
ApplicationSettings::panel_visibility() const noexcept
{
    return panel_visibility_;
}

void ApplicationSettings::CommitPanelVisibilityChange(
    const PanelVisibilityState& previous)
{
    if (!storage_.persistent) {
        return;
    }
    panel_visibility_persistence_.MarkDirtyIfChanged(
        previous,
        panel_visibility_);
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
        if (status_.setting == ApplicationSetting::PanelVisibility) {
            ClearStatus();
        }
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
        return true;
    }
    SetStatus(
        ApplicationSettingsStatusKind::PersistenceError,
        ApplicationSetting::PanelVisibility,
        "Could not save panel visibility.");
    return false;
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
    ClearStatus();
    return {
        .outcome = ApplicationSettingsOutcome::Applied,
        .setting = kSetting,
    };
}

ApplicationSettingsResult
ApplicationSettings::ApplyProfileOutputDirectory(
    std::optional<std::filesystem::path> directory)
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
    ClearStatus();
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
    status_ = {
        .kind = kind,
        .setting = setting,
        .detail = std::move(detail),
    };
}

void ApplicationSettings::ClearStatus()
{
    status_ = {};
}

}  // namespace specforge
