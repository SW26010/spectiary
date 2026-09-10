#include "app/application_settings.h"

#include "app/local_user_state_json.h"
#include "app/runtime_paths.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace specforge {
namespace {

using namespace std::chrono_literals;

constexpr auto kApplicationSettingsSaveRetry = 2s;
constexpr auto kPanelVisibilitySaveDebounce = 500ms;

constexpr const char* kUiLanguageSettingsFormatKind =
    "specforge.ui_language.settings";
constexpr const char* kAppearanceSettingsFormatKind =
    "specforge.appearance.settings";
constexpr const char* kUiScaleSettingsFormatKind =
    "specforge.ui_scale.settings";
constexpr const char* kInputSettingsFormatKind =
    "specforge.input.settings";
constexpr const char* kExternalSourceSettingsFormatKind =
    "specforge.external_source.settings";
constexpr int kSettingsSchemaVersion = 1;
constexpr const char* kThemeSelectionPolicyMember =
    "selection_policy";
constexpr const char* kExplicitThemeIdMember = "theme_id";
constexpr const char* kFollowSystemThemePolicyValue =
    "follow_system";
constexpr const char* kExplicitThemePolicyValue = "explicit";
constexpr const char* kLiveNumericNavigationMember =
    "live_numeric_navigation";
constexpr const char* kOpenExternalSourceAsFolderMember =
    "open_external_source_as_folder";
constexpr const char* kLegacyOpenExternalFitsAsFolderMember =
    "open_external_fits_as_folder";

struct StoredUiLanguageLoadResult {
    UiLanguage language = UiLanguage::English;
    std::string warning;
};

struct StoredUiScaleLoadResult {
    int percentage = kDefaultUiScalePercentage;
    std::string warning;
};

struct StoredThemeSelectionLoadResult {
    ThemeSelection selection = ThemeSelection::FollowSystem();
    std::string warning;
};

struct StoredBooleanLoadResult {
    bool value = false;
    std::string warning;
};

StoredUiLanguageLoadResult LoadStoredUiLanguage(
    const std::filesystem::path& path)
{
    StoredUiLanguageLoadResult loaded;
    VersionedJsonCacheLoadResult cache = LoadVersionedJsonCacheFile(
        path,
        kUiLanguageSettingsFormatKind,
        {kSettingsSchemaVersion},
        "UI language settings");
    if (!cache.document) {
        loaded.warning = std::move(cache.warning);
        return loaded;
    }

    const std::optional<std::string> language =
        ReadJsonStringMember(cache.document->root, "language");
    if (!language) {
        loaded.warning =
            "Ignored UI language settings: the language value is missing or invalid.";
        return loaded;
    }
    if (const std::optional<UiLanguage> parsed =
            ParseUiLanguageSettingValue(*language)) {
        loaded.language = *parsed;
        return loaded;
    }

    loaded.warning =
        "Ignored UI language settings: the language value is not supported.";
    return loaded;
}

bool SaveStoredUiLanguage(
    const std::filesystem::path& path,
    UiLanguage language,
    std::string* error_message)
{
    const std::string_view stable_value =
        UiLanguageSettingValue(language);
    if (stable_value.empty()) {
        if (error_message != nullptr) {
            *error_message = "The application language is not supported.";
        }
        return false;
    }

    return WriteVersionedJsonCacheDocument(
        path,
        kUiLanguageSettingsFormatKind,
        kSettingsSchemaVersion,
        "UI language settings",
        nlohmann::json::object({
            {"language", nlohmann::json(stable_value)},
        }),
        error_message);
}

StoredThemeSelectionLoadResult LoadStoredThemeSelection(
    const std::filesystem::path& path)
{
    StoredThemeSelectionLoadResult loaded;
    VersionedJsonCacheLoadResult cache =
        LoadVersionedJsonCacheFile(
            path,
            kAppearanceSettingsFormatKind,
            {kSettingsSchemaVersion},
            "appearance settings");
    if (!cache.document) {
        loaded.warning = std::move(cache.warning);
        return loaded;
    }

    const std::optional<std::string> policy =
        ReadJsonStringMember(
            cache.document->root,
            kThemeSelectionPolicyMember);
    if (!policy) {
        loaded.warning =
            "Ignored appearance settings: selection_policy is missing or invalid.";
        return loaded;
    }
    if (*policy == kFollowSystemThemePolicyValue) {
        return loaded;
    }
    if (*policy != kExplicitThemePolicyValue) {
        loaded.warning =
            "Ignored appearance settings: selection_policy is not supported.";
        return loaded;
    }

    const std::optional<std::string> stable_theme_id =
        ReadJsonStringMember(
            cache.document->root,
            kExplicitThemeIdMember);
    if (!stable_theme_id) {
        loaded.warning =
            "Ignored appearance settings: explicit selection requires a valid theme_id.";
        return loaded;
    }

    ThemeSelection selection = ThemeSelection::Explicit(
        ThemeId(*stable_theme_id));
    if (!IsSupportedThemeSelection(selection)) {
        loaded.warning =
            "Ignored appearance settings: theme_id is not supported.";
        return loaded;
    }
    loaded.selection = std::move(selection);
    return loaded;
}

bool SaveStoredThemeSelection(
    const std::filesystem::path& path,
    const ThemeSelection& selection,
    std::string* error_message)
{
    if (!IsSupportedThemeSelection(selection)) {
        if (error_message != nullptr) {
            *error_message =
                "The selected application theme is not supported.";
        }
        return false;
    }

    if (selection.policy ==
        ThemeSelectionPolicy::FollowSystem) {
        return WriteVersionedJsonCacheDocument(
            path,
            kAppearanceSettingsFormatKind,
            kSettingsSchemaVersion,
            "appearance settings",
            nlohmann::json::object({
                {kThemeSelectionPolicyMember,
                 nlohmann::json(
                     kFollowSystemThemePolicyValue)},
            }),
            error_message);
    }

    return WriteVersionedJsonCacheDocument(
        path,
        kAppearanceSettingsFormatKind,
        kSettingsSchemaVersion,
        "appearance settings",
        nlohmann::json::object({
            {kThemeSelectionPolicyMember,
             nlohmann::json(kExplicitThemePolicyValue)},
            {kExplicitThemeIdMember,
             nlohmann::json(
                 selection.explicit_theme_id.value())},
        }),
        error_message);
}

StoredUiScaleLoadResult LoadStoredUiScale(
    const std::filesystem::path& path)
{
    StoredUiScaleLoadResult loaded;
    VersionedJsonCacheLoadResult cache =
        LoadVersionedJsonCacheFile(
            path,
            kUiScaleSettingsFormatKind,
            {kSettingsSchemaVersion},
            "UI scale settings");
    if (!cache.document) {
        loaded.warning = std::move(cache.warning);
        return loaded;
    }

    const std::optional<int> percentage =
        ReadJsonIntMember(cache.document->root, "percentage");
    if (!percentage ||
        !IsValidUiScalePercentage(*percentage)) {
        loaded.warning =
            "Ignored UI scale settings: the percentage "
            "must be an integer from 80 through 150.";
        return loaded;
    }

    loaded.percentage = *percentage;
    return loaded;
}

bool SaveStoredUiScale(
    const std::filesystem::path& path,
    int percentage,
    std::string* error_message)
{
    if (!IsValidUiScalePercentage(percentage)) {
        if (error_message != nullptr) {
            *error_message =
                "The UI scale must be from 80% through 150%.";
        }
        return false;
    }

    return WriteVersionedJsonCacheDocument(
        path,
        kUiScaleSettingsFormatKind,
        kSettingsSchemaVersion,
        "UI scale settings",
        nlohmann::json::object({
            {"percentage", nlohmann::json(percentage)},
        }),
        error_message);
}

StoredBooleanLoadResult LoadStoredLiveNumericNavigation(
    const std::filesystem::path& path)
{
    StoredBooleanLoadResult loaded{
        .value = kDefaultLiveNumericNavigation,
    };
    VersionedJsonCacheLoadResult cache =
        LoadVersionedJsonCacheFile(
            path,
            kInputSettingsFormatKind,
            {kSettingsSchemaVersion},
            "input settings");
    if (!cache.document) {
        loaded.warning = std::move(cache.warning);
        return loaded;
    }

    const nlohmann::json* live_numeric_navigation =
        JsonObjectMember(
            cache.document->root,
            kLiveNumericNavigationMember);
    if (live_numeric_navigation == nullptr ||
        live_numeric_navigation->type() != nlohmann::json::value_t::boolean) {
        loaded.warning =
            "Ignored input settings: "
            "live_numeric_navigation must be boolean.";
        return loaded;
    }

    loaded.value = live_numeric_navigation->get<bool>();
    return loaded;
}

bool SaveStoredLiveNumericNavigation(
    const std::filesystem::path& path,
    bool enabled,
    std::string* error_message)
{
    return WriteVersionedJsonCacheDocument(
        path,
        kInputSettingsFormatKind,
        kSettingsSchemaVersion,
        "input settings",
        nlohmann::json::object({
            {kLiveNumericNavigationMember,
             nlohmann::json(enabled)},
        }),
        error_message);
}

StoredBooleanLoadResult LoadStoredOpenExternalSourceAsFolder(
    const std::filesystem::path& path)
{
    StoredBooleanLoadResult loaded{
        .value = kDefaultOpenExternalSourceAsFolder,
    };
    VersionedJsonCacheLoadResult cache =
        LoadVersionedJsonCacheFile(
            path,
            kExternalSourceSettingsFormatKind,
            {kSettingsSchemaVersion},
            "external source settings");
    if (!cache.document) {
        loaded.warning = std::move(cache.warning);
        return loaded;
    }

    const nlohmann::json* open_external_source_as_folder =
        JsonObjectMember(
            cache.document->root,
            kOpenExternalSourceAsFolderMember);
    const char* setting_member =
        kOpenExternalSourceAsFolderMember;
    if (open_external_source_as_folder == nullptr) {
        open_external_source_as_folder = JsonObjectMember(
            cache.document->root,
            kLegacyOpenExternalFitsAsFolderMember);
        setting_member = kLegacyOpenExternalFitsAsFolderMember;
    }
    if (open_external_source_as_folder == nullptr ||
        open_external_source_as_folder->type() !=
            nlohmann::json::value_t::boolean) {
        loaded.warning =
            "Ignored external source settings: "
            + std::string(setting_member) +
            " must be boolean.";
        return loaded;
    }

    loaded.value = open_external_source_as_folder->get<bool>();
    return loaded;
}

bool SaveStoredOpenExternalSourceAsFolder(
    const std::filesystem::path& path,
    bool enabled,
    std::string* error_message)
{
    return WriteVersionedJsonCacheDocument(
        path,
        kExternalSourceSettingsFormatKind,
        kSettingsSchemaVersion,
        "external source settings",
        nlohmann::json::object({
            {kOpenExternalSourceAsFolderMember,
             nlohmann::json(enabled)},
        }),
        error_message);
}

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

ApplicationSettingsIntent
ApplicationSettingsIntent::SetThemeSelection(
    ThemeSelection selection)
{
    return {
        .kind =
            ApplicationSettingsIntentKind::SetThemeSelection,
        .theme_selection = std::move(selection),
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
ApplicationSettingsIntent::SetOpenExternalSourceAsFolder(bool enabled)
{
    return {
        .kind = ApplicationSettingsIntentKind::
            SetOpenExternalSourceAsFolder,
        .open_external_source_as_folder = enabled,
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
        .appearance_settings_path =
            paths.appearance_settings_path,
        .ui_scale_settings_path =
            paths.ui_scale_settings_path,
        .input_settings_path =
            paths.input_settings_path,
        .external_source_settings_path =
            paths.external_source_settings_path,
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

    StoredUiLanguageLoadResult language_settings =
        LoadStoredUiLanguage(storage_.language_settings_path);
    language_ = language_settings.language;
    AdoptLoadWarning(
        ApplicationSetting::Language,
        std::move(language_settings.warning));

    StoredThemeSelectionLoadResult appearance_settings =
        LoadStoredThemeSelection(
            storage_.appearance_settings_path);
    theme_selection_ =
        std::move(appearance_settings.selection);
    AdoptLoadWarning(
        ApplicationSetting::Appearance,
        std::move(appearance_settings.warning));

    StoredUiScaleLoadResult ui_scale_settings =
        LoadStoredUiScale(storage_.ui_scale_settings_path);
    ui_scale_percentage_ = ui_scale_settings.percentage;
    AdoptLoadWarning(
        ApplicationSetting::UiScale,
        std::move(ui_scale_settings.warning));

    StoredBooleanLoadResult input_settings =
        LoadStoredLiveNumericNavigation(
            storage_.input_settings_path);
    live_numeric_navigation_ =
        input_settings.value;
    AdoptLoadWarning(
        ApplicationSetting::Input,
        std::move(input_settings.warning));

    StoredBooleanLoadResult external_source_settings =
        LoadStoredOpenExternalSourceAsFolder(
            storage_.external_source_settings_path);
    open_external_source_as_folder_ =
        external_source_settings.value;
    AdoptLoadWarning(
        ApplicationSetting::ExternalSource,
        std::move(external_source_settings.warning));

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
        .theme_selection = theme_selection_,
        .ui_scale_percentage = ui_scale_percentage_,
        .live_numeric_navigation =
            live_numeric_navigation_,
        .open_external_source_as_folder =
            open_external_source_as_folder_,
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
    case ApplicationSettingsIntentKind::SetThemeSelection:
        return ApplyThemeSelection(
            std::move(intent.theme_selection));
    case ApplicationSettingsIntentKind::SetUiScale:
        return ApplyUiScale(intent.ui_scale_percentage);
    case ApplicationSettingsIntentKind::
        SetLiveNumericNavigation:
        return ApplyLiveNumericNavigation(
            intent.live_numeric_navigation);
    case ApplicationSettingsIntentKind::
        SetOpenExternalSourceAsFolder:
        return ApplyOpenExternalSourceAsFolder(
            intent.open_external_source_as_folder);
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
    RunSettingMaintenance(
        ApplicationSetting::Appearance,
        now);
    RunSettingMaintenance(ApplicationSetting::UiScale, now);
    RunSettingMaintenance(ApplicationSetting::Input, now);
    RunSettingMaintenance(
        ApplicationSetting::ExternalSource,
        now);
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
    result.appearance_saved =
        FlushSetting(ApplicationSetting::Appearance) !=
        LocalUserStatePersistenceLifecycle::FlushOutcome::Failed;
    result.ui_scale_saved =
        FlushSetting(ApplicationSetting::UiScale) !=
        LocalUserStatePersistenceLifecycle::FlushOutcome::Failed;
    result.input_saved =
        FlushSetting(ApplicationSetting::Input) !=
        LocalUserStatePersistenceLifecycle::FlushOutcome::Failed;
    result.external_source_saved =
        FlushSetting(ApplicationSetting::ExternalSource) !=
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
        CancelPendingSetting(kSetting);
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
        CancelPendingSetting(kSetting);
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
ApplicationSettings::ApplyThemeSelection(
    ThemeSelection selection)
{
    constexpr ApplicationSetting kSetting =
        ApplicationSetting::Appearance;
    if (selection == theme_selection_ &&
        statuses_[static_cast<std::size_t>(kSetting)].kind ==
            ApplicationSettingsStatusKind::Ready) {
        return {
            .outcome = ApplicationSettingsOutcome::Unchanged,
            .setting = kSetting,
        };
    }
    if (!IsSupportedThemeSelection(selection)) {
        const std::string detail =
            "The selected application theme is not supported.";
        SetStatus(
            ApplicationSettingsStatusKind::Rejected,
            kSetting,
            ApplicationSettingsStatusReason::UnsupportedTheme,
            detail);
        return {
            .outcome = ApplicationSettingsOutcome::Rejected,
            .setting = kSetting,
            .detail = detail,
        };
    }

    pending_theme_selection_ = std::move(selection);
    if (!storage_.persistent) {
        theme_selection_ = *pending_theme_selection_;
        pending_theme_selection_.reset();
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
        CancelPendingSetting(kSetting);
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
        CancelPendingSetting(kSetting);
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
ApplicationSettings::ApplyOpenExternalSourceAsFolder(bool enabled)
{
    constexpr ApplicationSetting kSetting =
        ApplicationSetting::ExternalSource;
    if (enabled == open_external_source_as_folder_ &&
        statuses_[static_cast<std::size_t>(kSetting)].kind ==
            ApplicationSettingsStatusKind::Ready) {
        return {
            .outcome = ApplicationSettingsOutcome::Unchanged,
            .setting = kSetting,
        };
    }

    pending_open_external_source_as_folder_ = enabled;
    if (!storage_.persistent) {
        open_external_source_as_folder_ = enabled;
        pending_open_external_source_as_folder_.reset();
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
        CancelPendingSetting(kSetting);
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
        CancelPendingSetting(kSetting);
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
        if (SaveStoredUiLanguage(
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
    case ApplicationSetting::Appearance: {
        if (!pending_theme_selection_) {
            return {
                .error =
                    "No pending appearance setting save."};
        }
        std::string error;
        if (SaveStoredThemeSelection(
                storage_.appearance_settings_path,
                *pending_theme_selection_,
                &error)) {
            return {.saved = true};
        }
        return {
            .error = error.empty()
                ? "Could not save appearance settings."
                : std::move(error),
        };
    }
    case ApplicationSetting::UiScale: {
        if (!pending_ui_scale_percentage_) {
            return {.error = "No pending UI scale setting save."};
        }
        std::string error;
        if (SaveStoredUiScale(
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
        if (SaveStoredLiveNumericNavigation(
                storage_.input_settings_path,
                *pending_live_numeric_navigation_,
                &error)) {
            return {.saved = true};
        }
        return {
            .error = error.empty()
                ? "Could not save input settings."
                : std::move(error),
        };
    }
    case ApplicationSetting::ExternalSource: {
        if (!pending_open_external_source_as_folder_) {
            return {
                .error =
                    "No pending external source setting save."};
        }
        std::string error;
        if (SaveStoredOpenExternalSourceAsFolder(
                storage_.external_source_settings_path,
                *pending_open_external_source_as_folder_,
                &error)) {
            return {.saved = true};
        }
        return {
            .error = error.empty()
                ? "Could not save external source settings."
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
    case ApplicationSetting::Appearance:
        return pending_theme_selection_.has_value();
    case ApplicationSetting::UiScale:
        return pending_ui_scale_percentage_.has_value();
    case ApplicationSetting::Input:
        return pending_live_numeric_navigation_.has_value();
    case ApplicationSetting::ExternalSource:
        return pending_open_external_source_as_folder_.has_value();
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
    case ApplicationSetting::Appearance:
        if (pending_theme_selection_) {
            theme_selection_ =
                *pending_theme_selection_;
            pending_theme_selection_.reset();
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
    case ApplicationSetting::ExternalSource:
        if (pending_open_external_source_as_folder_) {
            open_external_source_as_folder_ =
                *pending_open_external_source_as_folder_;
            pending_open_external_source_as_folder_.reset();
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

void ApplicationSettings::CancelPendingSetting(
    ApplicationSetting setting)
{
    switch (setting) {
    case ApplicationSetting::Language:
        pending_language_.reset();
        break;
    case ApplicationSetting::Appearance:
        pending_theme_selection_.reset();
        break;
    case ApplicationSetting::UiScale:
        pending_ui_scale_percentage_.reset();
        break;
    case ApplicationSetting::Input:
        pending_live_numeric_navigation_.reset();
        break;
    case ApplicationSetting::ExternalSource:
        pending_open_external_source_as_folder_.reset();
        break;
    case ApplicationSetting::ProfileOutputDirectory:
        pending_profile_settings_.reset();
        pending_profile_output_directory_.reset();
        break;
    case ApplicationSetting::PanelVisibility:
    case ApplicationSetting::None:
        return;
    }
    PersistenceFor(setting).CancelPendingSave();
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
