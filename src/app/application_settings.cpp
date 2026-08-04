#include "app/application_settings.h"

#include "app/runtime_paths.h"
#include "ui/ui_language_settings.h"

#include <chrono>
#include <string>
#include <utility>

namespace specforge {
namespace {

using namespace std::chrono_literals;

constexpr auto kApplicationSettingsSaveRetry = 2s;
constexpr auto kPanelVisibilitySaveDebounce = 500ms;

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
    : storage_(std::move(storage))
{
    for (std::size_t index = 0;
         index < statuses_.size();
         ++index) {
        statuses_[index].setting =
            static_cast<ApplicationSetting>(index);
        persistence_[index] = LocalUserStatePersistenceLifecycle(
            static_cast<ApplicationSetting>(index) ==
                    ApplicationSetting::PanelVisibility
                ? kPanelVisibilitySaveDebounce
                : LocalUserStatePersistenceLifecycle::Duration::zero(),
            kApplicationSettingsSaveRetry);
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
    PanelVisibilityStateCacheLoadResult panel_visibility =
        LoadPanelVisibilityStateCache(
            storage_.panel_visibility_path);
    panel_visibility_ = std::move(panel_visibility.state);
    AdoptLoadWarning(
        ApplicationSetting::PanelVisibility,
        std::move(panel_visibility.warning));
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
    RunSettingMaintenance(ApplicationSetting::Language, now);
    RunSettingMaintenance(ApplicationSetting::UiScale, now);
    RunSettingMaintenance(ApplicationSetting::Input, now);
    RunSettingMaintenance(
        ApplicationSetting::ProfileOutputDirectory,
        now);
    RunSettingMaintenance(ApplicationSetting::PanelVisibility, now);
}

std::optional<LocalUserStateSaveScheduler::TimePoint>
ApplicationSettings::NextMaintenanceDeadline() const
{
    if (!storage_.persistent) {
        return std::nullopt;
    }
    std::optional<LocalUserStateSaveScheduler::TimePoint> deadline;
    for (const LocalUserStatePersistenceLifecycle& persistence :
         persistence_) {
        const std::optional<LocalUserStateSaveScheduler::TimePoint>
            candidate = persistence.NextMaintenanceDeadline();
        if (candidate && (!deadline || *candidate < *deadline)) {
            deadline = candidate;
        }
    }
    return deadline;
}

ApplicationSettingsFlushResult ApplicationSettings::Flush()
{
    ApplicationSettingsFlushResult result;
    if (!storage_.persistent) {
        return result;
    }
    result.language_saved =
        FlushSetting(ApplicationSetting::Language) !=
        LocalUserStatePersistenceLifecycle::FlushOutcome::Failed;
    result.ui_scale_saved =
        FlushSetting(ApplicationSetting::UiScale) !=
        LocalUserStatePersistenceLifecycle::FlushOutcome::Failed;
    result.input_saved =
        FlushSetting(ApplicationSetting::Input) !=
        LocalUserStatePersistenceLifecycle::FlushOutcome::Failed;
    result.profile_output_directory_saved =
        FlushSetting(
            ApplicationSetting::ProfileOutputDirectory) !=
        LocalUserStatePersistenceLifecycle::FlushOutcome::Failed;
    result.panel_visibility_saved =
        FlushSetting(ApplicationSetting::PanelVisibility) !=
        LocalUserStatePersistenceLifecycle::FlushOutcome::Failed;
    return result;
}

LocalUserStatePersistenceStatus
ApplicationSettings::PersistenceStatus(
    ApplicationSetting setting) const
{
    const std::size_t index =
        static_cast<std::size_t>(setting);
    if (index >= kApplicationSettingCount) {
        return {};
    }
    return persistence_[index].PersistenceStatus();
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

    pending_ui_scale_percentage_ = percentage;
    if (!storage_.persistent) {
        ui_scale_percentage_ = percentage;
        pending_ui_scale_percentage_.reset();
        ClearStatus(kSetting);
        return {
            .outcome = ApplicationSettingsOutcome::Applied,
            .setting = kSetting,
        };
    }

    PersistenceFor(kSetting).MarkDirty();
    if (FlushSetting(kSetting) ==
        LocalUserStatePersistenceLifecycle::FlushOutcome::Failed) {
        const std::string error =
            PersistenceStatus(kSetting).save_message;
        return {
            .outcome =
                ApplicationSettingsOutcome::PersistenceFailed,
            .setting = kSetting,
            .detail = std::move(error),
        };
    }

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

    pending_language_ = language;
    if (!storage_.persistent) {
        language_ = language;
        pending_language_.reset();
        ClearStatus(kSetting);
        return {
            .outcome = ApplicationSettingsOutcome::Applied,
            .setting = kSetting,
        };
    }

    PersistenceFor(kSetting).MarkDirty();
    if (FlushSetting(kSetting) ==
        LocalUserStatePersistenceLifecycle::FlushOutcome::Failed) {
        const std::string error =
            PersistenceStatus(kSetting).save_message;
        return {
            .outcome =
                ApplicationSettingsOutcome::PersistenceFailed,
            .setting = kSetting,
            .detail = std::move(error),
        };
    }

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

    pending_live_numeric_navigation_ = enabled;
    if (!storage_.persistent) {
        live_numeric_navigation_ = enabled;
        pending_live_numeric_navigation_.reset();
        ClearStatus(kSetting);
        return {
            .outcome = ApplicationSettingsOutcome::Applied,
            .setting = kSetting,
        };
    }

    PersistenceFor(kSetting).MarkDirty();
    if (FlushSetting(kSetting) ==
        LocalUserStatePersistenceLifecycle::FlushOutcome::Failed) {
        const std::string error =
            PersistenceStatus(kSetting).save_message;
        return {
            .outcome =
                ApplicationSettingsOutcome::PersistenceFailed,
            .setting = kSetting,
            .detail = std::move(error),
        };
    }

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

    pending_profile_settings_ = settings;
    pending_profile_output_directory_ = requested;
    if (!storage_.persistent) {
        profile_output_directory_ = requested;
        pending_profile_settings_.reset();
        pending_profile_output_directory_.reset();
        ClearStatus(kSetting);
        return {
            .outcome = ApplicationSettingsOutcome::Applied,
            .setting = kSetting,
        };
    }

    PersistenceFor(kSetting).MarkDirty();
    if (FlushSetting(kSetting) ==
        LocalUserStatePersistenceLifecycle::FlushOutcome::Failed) {
        const std::string error =
            PersistenceStatus(kSetting).save_message;
        return {
            .outcome =
                ApplicationSettingsOutcome::PersistenceFailed,
            .setting = kSetting,
            .detail = std::move(error),
        };
    }

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
        if (!(panel_visibility_ == previous)) {
            PersistenceFor(kSetting).MarkDirty();
        }
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
        if (!(panel_visibility_ == previous)) {
            PersistenceFor(kSetting).MarkDirty();
        }
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
    PersistenceFor(setting).SetLoadWarning(warning, warning);
    if (!warning.empty()) {
        SetStatus(
            ApplicationSettingsStatusKind::LoadWarning,
            setting,
            ApplicationSettingsStatusReason::SavedValueUnreadable,
            std::move(warning));
    }
}

LocalUserStatePersistenceLifecycle& ApplicationSettings::PersistenceFor(
    ApplicationSetting setting)
{
    return persistence_[static_cast<std::size_t>(setting)];
}

const LocalUserStatePersistenceLifecycle&
ApplicationSettings::PersistenceFor(
    ApplicationSetting setting) const
{
    return persistence_[static_cast<std::size_t>(setting)];
}

LocalUserStatePersistenceLifecycle::SaveResult
ApplicationSettings::SavePendingSetting(ApplicationSetting setting)
{
    switch (setting) {
    case ApplicationSetting::Language: {
        if (!pending_language_) {
            return {.error = "No pending language setting save."};
        }
        std::string error;
        if (SaveUiLanguageSettings(
                storage_.language_settings_path,
                *pending_language_,
                &error)) {
            return {.saved = true};
        }
        return {
            .error = error.empty()
                ? "Could not save language settings."
                : std::move(error),
        };
    }
    case ApplicationSetting::UiScale: {
        if (!pending_ui_scale_percentage_) {
            return {.error = "No pending UI scale setting save."};
        }
        std::string error;
        if (SaveUiScaleSettings(
                storage_.ui_scale_settings_path,
                *pending_ui_scale_percentage_,
                &error)) {
            return {.saved = true};
        }
        return {
            .error = error.empty()
                ? "Could not save UI scale settings."
                : std::move(error),
        };
    }
    case ApplicationSetting::Input: {
        if (!pending_live_numeric_navigation_) {
            return {.error = "No pending input setting save."};
        }
        std::string error;
        if (SaveInputSettings(
                storage_.input_settings_path,
                {.live_numeric_navigation =
                     *pending_live_numeric_navigation_},
                &error)) {
            return {.saved = true};
        }
        return {
            .error = error.empty()
                ? "Could not save input settings."
                : std::move(error),
        };
    }
    case ApplicationSetting::ProfileOutputDirectory: {
        if (!pending_profile_settings_) {
            return {.error = "No pending profile settings save."};
        }
        std::string error;
        if (SaveProfileSettings(
                storage_.profile_settings_path,
                *pending_profile_settings_,
                &error)) {
            return {.saved = true};
        }
        return {
            .error = error.empty()
                ? "Could not save profile settings."
                : std::move(error),
        };
    }
    case ApplicationSetting::PanelVisibility:
        if (SavePanelVisibilityStateCache(
                storage_.panel_visibility_path,
                panel_visibility_)) {
            return {.saved = true};
        }
        return {.error = "Could not save panel visibility."};
    case ApplicationSetting::None:
        return {.error = "Unknown application setting."};
    }
    return {.error = "Unknown application setting."};
}

LocalUserStatePersistenceLifecycle::FlushOutcome
ApplicationSettings::FlushSetting(ApplicationSetting setting)
{
    LocalUserStatePersistenceLifecycle& persistence =
        PersistenceFor(setting);
    if (!persistence.dirty()) {
        return LocalUserStatePersistenceLifecycle::FlushOutcome::NotNeeded;
    }
    const LocalUserStatePersistenceLifecycle::FlushOutcome outcome =
        persistence.Flush([this, setting] {
            return SavePendingSetting(setting);
        });
    if (outcome ==
        LocalUserStatePersistenceLifecycle::FlushOutcome::Saved) {
        CommitPendingSetting(setting);
        ClearStatus(setting);
    } else if (outcome ==
               LocalUserStatePersistenceLifecycle::FlushOutcome::Failed) {
        SetPersistenceFailureStatus(setting);
    }
    return outcome;
}

void ApplicationSettings::RunSettingMaintenance(
    ApplicationSetting setting,
    LocalUserStateSaveScheduler::TimePoint now)
{
    LocalUserStatePersistenceLifecycle& persistence =
        PersistenceFor(setting);
    if (!persistence.dirty() || !HasPendingSetting(setting)) {
        return;
    }
    const LocalUserStatePersistenceLifecycle::FlushOutcome outcome =
        persistence.RunMaintenance(
            now,
            [this, setting] {
                return SavePendingSetting(setting);
            });
    if (outcome ==
        LocalUserStatePersistenceLifecycle::FlushOutcome::Saved) {
        CommitPendingSetting(setting);
        ClearStatus(setting);
    } else if (outcome ==
               LocalUserStatePersistenceLifecycle::FlushOutcome::Failed) {
        SetPersistenceFailureStatus(setting);
    }
}

bool ApplicationSettings::HasPendingSetting(
    ApplicationSetting setting) const
{
    switch (setting) {
    case ApplicationSetting::Language:
        return pending_language_.has_value();
    case ApplicationSetting::UiScale:
        return pending_ui_scale_percentage_.has_value();
    case ApplicationSetting::Input:
        return pending_live_numeric_navigation_.has_value();
    case ApplicationSetting::ProfileOutputDirectory:
        return pending_profile_settings_.has_value() &&
               pending_profile_output_directory_.has_value();
    case ApplicationSetting::PanelVisibility:
        return true;
    case ApplicationSetting::None:
        return false;
    }
    return false;
}

void ApplicationSettings::CommitPendingSetting(ApplicationSetting setting)
{
    switch (setting) {
    case ApplicationSetting::Language:
        if (pending_language_) {
            language_ = *pending_language_;
            pending_language_.reset();
        }
        return;
    case ApplicationSetting::UiScale:
        if (pending_ui_scale_percentage_) {
            ui_scale_percentage_ = *pending_ui_scale_percentage_;
            pending_ui_scale_percentage_.reset();
        }
        return;
    case ApplicationSetting::Input:
        if (pending_live_numeric_navigation_) {
            live_numeric_navigation_ =
                *pending_live_numeric_navigation_;
            pending_live_numeric_navigation_.reset();
        }
        return;
    case ApplicationSetting::ProfileOutputDirectory:
        if (pending_profile_output_directory_) {
            profile_output_directory_ =
                *pending_profile_output_directory_;
            pending_profile_output_directory_.reset();
            pending_profile_settings_.reset();
        }
        return;
    case ApplicationSetting::PanelVisibility:
    case ApplicationSetting::None:
        return;
    }
}

void ApplicationSettings::SetPersistenceFailureStatus(
    ApplicationSetting setting)
{
    std::string detail = PersistenceStatus(setting).save_message;
    if (detail.empty()) {
        detail = "Could not save application settings.";
    }
    SetStatus(
        ApplicationSettingsStatusKind::PersistenceError,
        setting,
        ApplicationSettingsStatusReason::SettingsWriteFailed,
        std::move(detail));
}

}  // namespace specforge
