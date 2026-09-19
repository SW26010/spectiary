#include "app/application_settings.h"
#include "automation/automation_panel_mutation_chain.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>

namespace {

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        const auto suffix = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        path_ = std::filesystem::temp_directory_path() /
                ("spectiary-application-settings-tests-" +
                 std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

spectiary::ApplicationSettingsStorage MakeStorage(
    const std::filesystem::path& root,
    std::optional<std::filesystem::path> environment_override =
        std::nullopt)
{
    return {
        .language_settings_path = root / "ui-language.json",
        .appearance_settings_path =
            root / "appearance-settings.json",
        .ui_scale_settings_path = root / "ui-scale.json",
        .input_settings_path = root / "input-settings.json",
        .external_source_settings_path =
            root / "external-source-settings.json",
        .profile_settings_path = root / "profile-settings.json",
        .panel_visibility_path = root / "panel-visibility.json",
        .default_profile_output_directory = root / "profiles",
        .profile_output_environment_override =
            std::move(environment_override),
    };
}

void TestSettingsIntentsPersistAndReloadThroughOneOwner()
{
    TemporaryDirectory temporary;
    const auto storage = MakeStorage(temporary.path());
    spectiary::ApplicationSettings settings(storage);

    const spectiary::ApplicationSettingsView initial =
        settings.View();
    Require(
        initial.language == spectiary::UiLanguage::English,
        "missing language settings should default to English");
    Require(
        initial.theme_selection ==
                spectiary::ThemeSelection::FollowSystem() &&
            initial
                    .StatusFor(
                        spectiary::ApplicationSetting::Appearance)
                    .kind ==
                spectiary::ApplicationSettingsStatusKind::Ready,
        "missing appearance settings should follow the system theme");
    Require(
        initial.ui_scale_percentage == 100,
        "missing UI scale settings should default to 100%");
    Require(
        initial.live_numeric_navigation,
        "missing input settings should enable live numeric navigation");
    Require(
        !initial.open_external_source_as_folder,
        "missing external source settings should disable external spectrum folder opening");
    Require(
        initial.profile_output_directory ==
            storage.default_profile_output_directory,
        "missing profile settings should use the default directory");

    const auto language_result = settings.Apply(
        spectiary::ApplicationSettingsIntent::SetLanguage(
            spectiary::UiLanguage::SimplifiedChinese),
        {});
    Require(
        language_result.applied(),
        "language intent should apply");

    const auto appearance_result = settings.Apply(
        spectiary::ApplicationSettingsIntent::SetThemeSelection(
            spectiary::ThemeSelection::Explicit(
                spectiary::BuiltInDarkThemeId())),
        {});
    Require(
        appearance_result.applied(),
        "explicit theme intent should apply");

    const auto ui_scale_result = settings.Apply(
        spectiary::ApplicationSettingsIntent::SetUiScale(125),
        {});
    Require(
        ui_scale_result.applied(),
        "UI scale intent should apply");

    const auto input_result = settings.Apply(
        spectiary::ApplicationSettingsIntent::
            SetLiveNumericNavigation(false),
        {});
    Require(
        input_result.applied(),
        "live numeric navigation intent should apply");

    const auto external_source_result = settings.Apply(
        spectiary::ApplicationSettingsIntent::
            SetOpenExternalSourceAsFolder(true),
        {});
    Require(
        external_source_result.applied(),
        "external source folder intent should apply");

    const std::filesystem::path custom_directory =
        temporary.path() / "custom profiles";
    const auto directory_result = settings.Apply(
        spectiary::ApplicationSettingsIntent::
            SetProfileOutputDirectory(custom_directory),
        {});
    Require(
        directory_result.applied(),
        "profile output directory intent should apply");

    Require(
        ReadFile(storage.language_settings_path).find(
            R"("language": "zh-Hans")") != std::string::npos,
        "language should retain its stable persisted value");
    const std::string appearance_document =
        ReadFile(storage.appearance_settings_path);
    Require(
        appearance_document.find(
            R"("selection_policy": "explicit")") !=
                std::string::npos &&
            appearance_document.find(
                R"("theme_id": "builtin.theme.dark")") !=
                std::string::npos,
        "appearance should persist selection policy and stable theme ID separately");
    Require(
        ReadFile(storage.ui_scale_settings_path).find(
            R"("percentage": 125)") != std::string::npos,
        "UI scale should retain its integer persisted value");
    Require(
        ReadFile(storage.input_settings_path).find(
            R"("live_numeric_navigation": false)") !=
            std::string::npos,
        "live numeric navigation should retain its boolean persisted value");
    const std::string external_source_document =
        ReadFile(storage.external_source_settings_path);
    Require(
        external_source_document.find(
            R"("open_external_source_as_folder": true)") !=
                std::string::npos &&
            external_source_document.find(
                "open_external_fits_as_folder") ==
                std::string::npos,
        "external source settings should retain the generic persisted key");

    spectiary::ApplicationSettings reloaded(storage);
    const spectiary::ApplicationSettingsView reloaded_view =
        reloaded.View();
    Require(
        reloaded_view.language ==
            spectiary::UiLanguage::SimplifiedChinese,
        "language should reload through the application settings owner");
    Require(
        reloaded_view.theme_selection ==
            spectiary::ThemeSelection::Explicit(
                spectiary::BuiltInDarkThemeId()),
        "explicit theme identity should reload through the application settings owner");
    Require(
        reloaded_view.ui_scale_percentage == 125,
        "UI scale should reload through the application settings owner");
    Require(
        !reloaded_view.live_numeric_navigation,
        "live numeric navigation should reload through the application settings owner");
    Require(
        reloaded_view.open_external_source_as_folder,
        "external source folder preference should reload through the application settings owner");
    Require(
        reloaded_view.profile_output_directory == custom_directory,
        "profile directory should reload through the application settings owner");
    Require(
        reloaded_view.profile_output_directory_source ==
            spectiary::ProfileOutputDirectorySource::UserSetting,
        "reloaded custom profile directory should retain its source");

    const auto restore_result = reloaded.Apply(
        spectiary::ApplicationSettingsIntent::
            RestoreDefaultProfileOutputDirectory(),
        {});
    Require(
        restore_result.applied(),
        "restore-default intent should apply");
    Require(
        reloaded.View().profile_output_directory ==
            storage.default_profile_output_directory,
        "restore-default should update the owned view");
}

void TestLegacyExternalSourcePreferenceLoadsThroughApplicationSettings()
{
    TemporaryDirectory temporary;
    const auto storage = MakeStorage(temporary.path());
    {
        std::ofstream stream(storage.external_source_settings_path);
        stream << R"({"format_kind":"spectiary.external_source.settings","schema_version":1,"open_external_fits_as_folder":true})";
    }

    const spectiary::ApplicationSettings settings(storage);
    Require(
        settings.View().open_external_source_as_folder &&
            settings.View()
                    .StatusFor(spectiary::ApplicationSetting::ExternalSource)
                    .kind == spectiary::ApplicationSettingsStatusKind::Ready,
        "the application settings owner should retain an enabled legacy external source preference");

    spectiary::ApplicationSettings mutable_settings(storage);
    Require(
        mutable_settings.Apply(
            spectiary::ApplicationSettingsIntent::
                SetOpenExternalSourceAsFolder(false),
            {})
            .applied(),
        "changing a legacy external source preference should persist through the owner");
    const std::string migrated =
        ReadFile(storage.external_source_settings_path);
    Require(
        migrated.find(
            R"("open_external_source_as_folder": false)") !=
                std::string::npos &&
            migrated.find("open_external_fits_as_folder") ==
                std::string::npos,
        "the next owner write should migrate the legacy external source member");
}

void TestCompactSingleValueFilesRemainReadableThroughApplicationSettings()
{
    TemporaryDirectory temporary;
    const auto storage = MakeStorage(temporary.path());
    {
        std::ofstream stream(storage.language_settings_path);
        stream << R"({"format_kind":"spectiary.ui_language.settings","schema_version":1,"language":"zh-Hans"})";
    }
    {
        std::ofstream stream(storage.ui_scale_settings_path);
        stream << R"({"format_kind":"spectiary.ui_scale.settings","schema_version":1,"percentage":150})";
    }
    {
        std::ofstream stream(storage.input_settings_path);
        stream << R"({"format_kind":"spectiary.input.settings","schema_version":1,"live_numeric_navigation":false})";
    }
    {
        std::ofstream stream(storage.external_source_settings_path);
        stream << R"({"format_kind":"spectiary.external_source.settings","schema_version":1,"open_external_source_as_folder":true})";
    }

    const spectiary::ApplicationSettings settings(storage);
    const spectiary::ApplicationSettingsView view = settings.View();
    Require(
        view.language == spectiary::UiLanguage::SimplifiedChinese &&
            view.ui_scale_percentage == 150 &&
            !view.live_numeric_navigation &&
            view.open_external_source_as_folder,
        "compact version-1 setting files should remain readable through ApplicationSettings");
    for (const spectiary::ApplicationSetting setting : {
             spectiary::ApplicationSetting::Language,
             spectiary::ApplicationSetting::UiScale,
             spectiary::ApplicationSetting::Input,
             spectiary::ApplicationSetting::ExternalSource}) {
        Require(
            view.StatusFor(setting).kind ==
                spectiary::ApplicationSettingsStatusKind::Ready,
            "valid compact setting files should load without warnings");
    }
}

void TestInvalidSingleValuesFallBackThroughApplicationSettings()
{
    TemporaryDirectory temporary;
    const auto storage = MakeStorage(temporary.path());
    {
        std::ofstream stream(storage.language_settings_path);
        stream << R"({"format_kind":"spectiary.ui_language.settings","schema_version":1,"language":"fr"})";
    }
    {
        std::ofstream stream(storage.ui_scale_settings_path);
        stream << R"({"format_kind":"spectiary.ui_scale.settings","schema_version":1,"percentage":151})";
    }
    {
        std::ofstream stream(storage.input_settings_path);
        stream << R"({"format_kind":"spectiary.input.settings","schema_version":1,"live_numeric_navigation":"false"})";
    }
    {
        std::ofstream stream(storage.external_source_settings_path);
        stream << R"({"format_kind":"spectiary.external_source.settings","schema_version":1,"open_external_source_as_folder":"true"})";
    }

    const spectiary::ApplicationSettings settings(storage);
    const spectiary::ApplicationSettingsView view = settings.View();
    Require(
        view.language == spectiary::UiLanguage::English &&
            view.ui_scale_percentage == 100 &&
            view.live_numeric_navigation &&
            !view.open_external_source_as_folder,
        "invalid stored values should retain the established defaults");
    for (const spectiary::ApplicationSetting setting : {
             spectiary::ApplicationSetting::Language,
             spectiary::ApplicationSetting::UiScale,
             spectiary::ApplicationSetting::Input,
             spectiary::ApplicationSetting::ExternalSource}) {
        Require(
            view.StatusFor(setting).kind ==
                spectiary::ApplicationSettingsStatusKind::LoadWarning,
            "invalid stored values should surface through owner load warnings");
    }
}

void TestUnsupportedSingleValueSchemasFallBackThroughApplicationSettings()
{
    TemporaryDirectory temporary;
    const auto storage = MakeStorage(temporary.path());
    {
        std::ofstream stream(storage.language_settings_path);
        stream << R"({"format_kind":"spectiary.ui_language.settings","schema_version":2,"language":"zh-Hans"})";
    }
    {
        std::ofstream stream(storage.ui_scale_settings_path);
        stream << R"({"format_kind":"spectiary.ui_scale.settings","schema_version":2,"percentage":125})";
    }

    const spectiary::ApplicationSettings settings(storage);
    const spectiary::ApplicationSettingsView view = settings.View();
    Require(
        view.language == spectiary::UiLanguage::English &&
            view.ui_scale_percentage == 100,
        "unsupported setting schemas should retain the established defaults");
    for (const spectiary::ApplicationSetting setting : {
             spectiary::ApplicationSetting::Language,
             spectiary::ApplicationSetting::UiScale}) {
        Require(
            view.StatusFor(setting).kind ==
                    spectiary::ApplicationSettingsStatusKind::LoadWarning &&
                !settings.PersistenceStatus(setting)
                     .load_warning.empty(),
            "unsupported setting schemas should surface through owner load warnings");
    }
}

void TestPersistenceFailureRetainsThePreviousValueAndStatus()
{
    TemporaryDirectory temporary;
    const std::filesystem::path blocker =
        temporary.path() / "not-a-directory";
    {
        std::ofstream stream(blocker);
        stream << "block settings directory creation";
    }

    spectiary::ApplicationSettings settings({
        .language_settings_path = blocker / "ui-language.json",
        .ui_scale_settings_path = blocker / "ui-scale.json",
        .input_settings_path = blocker / "input-settings.json",
        .external_source_settings_path =
            blocker / "external-source-settings.json",
        .profile_settings_path = blocker / "profile-settings.json",
        .panel_visibility_path = blocker / "panel-visibility.json",
        .default_profile_output_directory =
            temporary.path() / "profiles",
    });

    const auto result = settings.Apply(
        spectiary::ApplicationSettingsIntent::SetLanguage(
            spectiary::UiLanguage::SimplifiedChinese),
        {});
    Require(
        result.outcome ==
            spectiary::ApplicationSettingsOutcome::
                PersistenceFailed,
        "save failure should be a typed persistence outcome");
    const spectiary::ApplicationSettingsView view = settings.View();
    const spectiary::ApplicationSettingsStatus& language_status =
        view.StatusFor(spectiary::ApplicationSetting::Language);
    Require(
        view.language == spectiary::UiLanguage::English,
        "save failure should retain the previous language");
    Require(
        language_status.kind ==
                spectiary::ApplicationSettingsStatusKind::
                    PersistenceError &&
            language_status.setting ==
                spectiary::ApplicationSetting::Language &&
            language_status.reason ==
                spectiary::ApplicationSettingsStatusReason::
                    SettingsWriteFailed &&
            !language_status.detail.empty(),
        "save failure should remain structured on the owner view");
    Require(
        !settings
             .PersistenceStatus(
                 spectiary::ApplicationSetting::Language)
             .save_message.empty(),
        "application persistence health should retain a failed setting save");
    Require(
        !settings
             .PersistenceStatus(
                 spectiary::ApplicationSetting::Language)
             .retrying,
        "a terminal setting failure should not remain scheduled for retry");
    Require(
        !settings.NextMaintenanceDeadline().has_value(),
        "a terminal setting failure should cancel its maintenance deadline");

    std::filesystem::remove(blocker);
    std::filesystem::create_directories(blocker);
    Require(
        settings.Apply(
            spectiary::ApplicationSettingsIntent::SetLanguage(
                spectiary::UiLanguage::SimplifiedChinese),
            {})
            .applied(),
        "the failed language save should succeed after repairing its path");
    Require(
        settings
            .PersistenceStatus(
                spectiary::ApplicationSetting::Language)
            .recovered,
        "a successful setting retry should expose recovery");
}

void TestLanguageValidationUsesApplicationSettingsInterface()
{
    TemporaryDirectory temporary;
    const auto storage = MakeStorage(temporary.path());
    spectiary::ApplicationSettings settings(storage);

    const spectiary::ApplicationSettingsResult result =
        settings.Apply(
            spectiary::ApplicationSettingsIntent::SetLanguage(
                spectiary::UiLanguage::Count),
            {});
    const spectiary::ApplicationSettingsView view = settings.View();
    Require(
        result.outcome ==
                spectiary::ApplicationSettingsOutcome::Rejected &&
            result.setting == spectiary::ApplicationSetting::Language &&
            view.language == spectiary::UiLanguage::English &&
            view.StatusFor(spectiary::ApplicationSetting::Language)
                    .reason ==
                spectiary::ApplicationSettingsStatusReason::
                    UnsupportedLanguage &&
            !std::filesystem::exists(storage.language_settings_path),
        "unsupported languages should be rejected by ApplicationSettings before persistence");
}

void TestUiScaleValidationAndPersistenceFirstBehavior()
{
    TemporaryDirectory temporary;
    const auto storage = MakeStorage(temporary.path());
    spectiary::ApplicationSettings settings(storage);

    const auto unchanged = settings.Apply(
        spectiary::ApplicationSettingsIntent::SetUiScale(100),
        {});
    Require(
        unchanged.outcome ==
                spectiary::ApplicationSettingsOutcome::Unchanged &&
            unchanged.setting ==
                spectiary::ApplicationSetting::UiScale,
        "unchanged UI scale intent should be typed");

    for (const int percentage : {80, 100, 150}) {
        const auto result = settings.Apply(
            spectiary::ApplicationSettingsIntent::SetUiScale(
                percentage),
            {});
        Require(
            result.outcome ==
                    spectiary::ApplicationSettingsOutcome::Applied ||
                result.outcome ==
                    spectiary::ApplicationSettingsOutcome::Unchanged,
            "supported UI scale should apply");
        const spectiary::ApplicationSettings reloaded(storage);
        Require(
            reloaded.View().ui_scale_percentage == percentage,
            "supported UI scale should round-trip");
    }

    const auto rejected = settings.Apply(
        spectiary::ApplicationSettingsIntent::SetUiScale(151),
        {});
    Require(
        rejected.outcome ==
                spectiary::ApplicationSettingsOutcome::Rejected &&
            settings.View().ui_scale_percentage == 150 &&
            settings.View()
                    .StatusFor(
                        spectiary::ApplicationSetting::UiScale)
                    .reason ==
                spectiary::ApplicationSettingsStatusReason::
                    UiScaleOutOfRange,
        "out-of-range UI scale should be rejected without changing state");

    const std::filesystem::path blocker =
        temporary.path() / "not-a-directory";
    {
        std::ofstream stream(blocker);
        stream << "block UI scale settings directory creation";
    }
    auto failing_storage = MakeStorage(temporary.path());
    failing_storage.ui_scale_settings_path =
        blocker / "ui-scale.json";
    spectiary::ApplicationSettings failing_settings(
        failing_storage);
    const auto failure = failing_settings.Apply(
        spectiary::ApplicationSettingsIntent::SetUiScale(125),
        {});
    const spectiary::ApplicationSettingsView failed_view =
        failing_settings.View();
    Require(
        failure.outcome ==
                spectiary::ApplicationSettingsOutcome::
                    PersistenceFailed &&
            failed_view.ui_scale_percentage == 100,
        "UI scale save failure should retain the previous runtime value");
    Require(
        failed_view.StatusFor(
                spectiary::ApplicationSetting::UiScale)
                .kind ==
            spectiary::ApplicationSettingsStatusKind::
                PersistenceError,
        "UI scale save failure should remain visible");
}

void TestLiveNumericNavigationPersistenceFailureRetainsEnabledValue()
{
    TemporaryDirectory temporary;
    const std::filesystem::path blocker =
        temporary.path() / "not-a-directory";
    {
        std::ofstream stream(blocker);
        stream << "block input settings directory creation";
    }

    auto storage = MakeStorage(temporary.path());
    storage.input_settings_path =
        blocker / "input-settings.json";
    spectiary::ApplicationSettings settings(storage);
    const spectiary::ApplicationSettingsResult result =
        settings.Apply(
            spectiary::ApplicationSettingsIntent::
                SetLiveNumericNavigation(false),
            {});
    const spectiary::ApplicationSettingsView view =
        settings.View();
    Require(
        result.outcome ==
                spectiary::ApplicationSettingsOutcome::
                    PersistenceFailed &&
            view.live_numeric_navigation,
        "input settings save failure should retain the enabled runtime value");
    Require(
        view.StatusFor(spectiary::ApplicationSetting::Input).kind ==
            spectiary::ApplicationSettingsStatusKind::
                PersistenceError,
        "input settings save failure should remain visible on the owner view");
}

void TestExternalSourceFolderPersistenceFailureRetainsDisabledValue()
{
    using namespace std::chrono_literals;

    TemporaryDirectory temporary;
    const std::filesystem::path blocker =
        temporary.path() / "not-a-directory";
    {
        std::ofstream stream(blocker);
        stream << "block external source settings directory creation";
    }

    auto storage = MakeStorage(temporary.path());
    storage.external_source_settings_path =
        blocker / "external-source-settings.json";
    spectiary::ApplicationSettings settings(storage);
    const spectiary::ApplicationSettingsResult result =
        settings.Apply(
            spectiary::ApplicationSettingsIntent::
                SetOpenExternalSourceAsFolder(true),
            {});
    const spectiary::ApplicationSettingsView view =
        settings.View();
    Require(
        result.outcome ==
                spectiary::ApplicationSettingsOutcome::
                    PersistenceFailed &&
            !view.open_external_source_as_folder,
        "external source save failure should retain the disabled runtime value");
    Require(
        view.StatusFor(
                spectiary::ApplicationSetting::ExternalSource)
                .kind ==
            spectiary::ApplicationSettingsStatusKind::
                PersistenceError,
        "external source save failure should remain visible on the owner view");

    std::filesystem::remove(blocker);
    std::filesystem::create_directories(blocker);
    settings.RunMaintenance(
        spectiary::LocalUserStateSaveScheduler::Clock::now() + 10s);
    Require(
        !settings.View().open_external_source_as_folder,
        "external source save failure should remain disabled after crossing the maintenance deadline");

    const spectiary::ApplicationSettingsResult retry =
        settings.Apply(
            spectiary::ApplicationSettingsIntent::
                SetOpenExternalSourceAsFolder(true),
            {});
    Require(
        retry.applied() &&
            settings.View().open_external_source_as_folder,
        "only an explicit external source retry should enable the setting after repair");
}

void TestUiScaleResetRepairsDamagedFallbackState()
{
    TemporaryDirectory temporary;
    const auto storage = MakeStorage(temporary.path());
    {
        std::ofstream stream(storage.ui_scale_settings_path);
        stream << R"({"format_kind":)";
    }
    spectiary::ApplicationSettings settings(storage);
    const spectiary::ApplicationSettingsView fallback =
        settings.View();
    Require(
        fallback.ui_scale_percentage == 100 &&
            fallback.StatusFor(
                    spectiary::ApplicationSetting::UiScale)
                    .kind ==
                spectiary::ApplicationSettingsStatusKind::
                    LoadWarning,
        "damaged UI scale settings should establish a warned 100% fallback");

    const auto repair = settings.Apply(
        spectiary::ApplicationSettingsIntent::SetUiScale(100),
        {});
    Require(
        repair.outcome ==
                spectiary::ApplicationSettingsOutcome::Applied &&
            settings.View()
                    .StatusFor(
                        spectiary::ApplicationSetting::UiScale)
                    .kind ==
                spectiary::ApplicationSettingsStatusKind::Ready,
        "resetting the warned fallback should rewrite and clear its status");

    const spectiary::ApplicationSettings reloaded(storage);
    Require(
        reloaded.View().ui_scale_percentage == 100 &&
            reloaded.View()
                    .StatusFor(
                        spectiary::ApplicationSetting::UiScale)
                    .kind ==
                spectiary::ApplicationSettingsStatusKind::Ready,
        "the repaired 100% UI scale should reload without warning");
}

void TestLanguageAndProfileFallbacksCanBeReapplied()
{
    TemporaryDirectory temporary;
    const auto storage = MakeStorage(temporary.path());
    {
        std::ofstream stream(storage.language_settings_path);
        stream << R"({"format_kind":)";
    }
    {
        std::ofstream stream(storage.profile_settings_path);
        stream << R"({"format_kind":)";
    }

    spectiary::ApplicationSettings settings(storage);
    const spectiary::ApplicationSettingsView fallback =
        settings.View();
    Require(
        fallback.language == spectiary::UiLanguage::English &&
            fallback.StatusFor(
                    spectiary::ApplicationSetting::Language)
                    .kind ==
                spectiary::ApplicationSettingsStatusKind::
                    LoadWarning,
        "damaged language settings should establish a warned English fallback");
    Require(
        fallback.profile_output_directory ==
                storage.default_profile_output_directory &&
            fallback.StatusFor(
                    spectiary::ApplicationSetting::
                        ProfileOutputDirectory)
                    .kind ==
                spectiary::ApplicationSettingsStatusKind::
                    LoadWarning,
        "damaged profile settings should establish a warned default fallback");

    const auto language_repair = settings.Apply(
        spectiary::ApplicationSettingsIntent::SetLanguage(
            spectiary::UiLanguage::English),
        {});
    const auto profile_repair = settings.Apply(
        spectiary::ApplicationSettingsIntent::
            RestoreDefaultProfileOutputDirectory(),
        {});
    Require(
        language_repair.applied() &&
            settings.View()
                    .StatusFor(
                        spectiary::ApplicationSetting::Language)
                    .kind ==
                spectiary::ApplicationSettingsStatusKind::Ready,
        "reapplying the warned language fallback should rewrite and clear its status");
    Require(
        profile_repair.applied() &&
            settings.View()
                    .StatusFor(
                        spectiary::ApplicationSetting::
                            ProfileOutputDirectory)
                    .kind ==
                spectiary::ApplicationSettingsStatusKind::Ready,
        "restoring the warned profile fallback should rewrite and clear its status");

    const spectiary::ApplicationSettings reloaded(storage);
    Require(
        reloaded.View()
                    .StatusFor(
                        spectiary::ApplicationSetting::Language)
                    .kind ==
                spectiary::ApplicationSettingsStatusKind::Ready &&
            reloaded.View()
                    .StatusFor(
                        spectiary::ApplicationSetting::
                            ProfileOutputDirectory)
                    .kind ==
                spectiary::ApplicationSettingsStatusKind::Ready,
        "repaired language and profile defaults should reload without warning");
}

void TestLoadWarningAndEnvironmentOverrideAreTyped()
{
    TemporaryDirectory temporary;
    auto storage = MakeStorage(
        temporary.path(),
        temporary.path() / "environment profiles");
    {
        std::ofstream stream(storage.language_settings_path);
        stream << R"({"format_kind":)";
    }
    {
        std::ofstream stream(storage.ui_scale_settings_path);
        stream << R"({"format_kind":)";
    }
    {
        std::ofstream stream(storage.input_settings_path);
        stream << R"({"format_kind":)";
    }
    {
        std::ofstream stream(storage.external_source_settings_path);
        stream << R"({"format_kind":)";
    }
    {
        std::ofstream stream(storage.profile_settings_path);
        stream << R"({"format_kind":)";
    }
    {
        std::ofstream stream(storage.panel_visibility_path);
        stream << R"({"format_kind":)";
    }

    spectiary::ApplicationSettings settings(storage);
    const spectiary::ApplicationSettingsView loaded = settings.View();
    const spectiary::ApplicationSettingsStatus& language_status =
        loaded.StatusFor(spectiary::ApplicationSetting::Language);
    Require(
        language_status.kind ==
                spectiary::ApplicationSettingsStatusKind::
                    LoadWarning &&
            language_status.setting ==
                spectiary::ApplicationSetting::Language &&
            language_status.reason ==
                spectiary::ApplicationSettingsStatusReason::
                    SavedValueUnreadable,
        "damaged language settings should produce a typed load warning");
    const spectiary::ApplicationSettingsStatus& ui_scale_status =
        loaded.StatusFor(spectiary::ApplicationSetting::UiScale);
    Require(
        loaded.ui_scale_percentage == 100 &&
            ui_scale_status.kind ==
                spectiary::ApplicationSettingsStatusKind::
                    LoadWarning &&
            ui_scale_status.setting ==
                spectiary::ApplicationSetting::UiScale,
        "damaged UI scale settings should fall back with a typed load warning");
    Require(
        loaded.live_numeric_navigation &&
            loaded
                    .StatusFor(
                        spectiary::ApplicationSetting::Input)
                    .kind ==
                spectiary::ApplicationSettingsStatusKind::
                    LoadWarning,
        "damaged input settings should enable live navigation with a typed load warning");
    Require(
        !loaded.open_external_source_as_folder &&
            loaded
                    .StatusFor(
                        spectiary::ApplicationSetting::
                            ExternalSource)
                    .kind ==
                spectiary::ApplicationSettingsStatusKind::
                    LoadWarning,
        "damaged external source settings should disable the preference with a typed load warning");
    Require(
        loaded
                .StatusFor(
                    spectiary::ApplicationSetting::
                        ProfileOutputDirectory)
                .kind ==
            spectiary::ApplicationSettingsStatusKind::
                LoadWarning,
        "damaged profile settings should produce a typed load warning");
    Require(
        !settings
             .PersistenceStatus(
                 spectiary::ApplicationSetting::
                     ProfileOutputDirectory)
             .load_warning.empty(),
        "profile load warning should reach application persistence health");
    Require(
        loaded
                .StatusFor(
                    spectiary::ApplicationSetting::
                        PanelVisibility)
                .kind ==
            spectiary::ApplicationSettingsStatusKind::
                LoadWarning,
        "damaged panel visibility should produce a typed load warning");
    Require(
        !settings
             .PersistenceStatus(
                 spectiary::ApplicationSetting::
                     PanelVisibility)
             .load_warning.empty(),
        "panel visibility warning should reach application persistence health");
    Require(
        loaded.profile_output_directory_source ==
            spectiary::ProfileOutputDirectorySource::Environment,
        "environment profile directory should retain explicit precedence");

    const auto result = settings.Apply(
        spectiary::ApplicationSettingsIntent::
            SetProfileOutputDirectory(
                temporary.path() / "ignored"),
        {});
    Require(
        result.outcome ==
                spectiary::ApplicationSettingsOutcome::Rejected &&
            settings.View()
                    .StatusFor(
                        spectiary::ApplicationSetting::
                            ProfileOutputDirectory)
                    .reason ==
                spectiary::ApplicationSettingsStatusReason::
                    EnvironmentOverrideActive,
        "environment-owned profile directory should reject edits");
    Require(
        settings.View().profile_output_directory ==
            *storage.profile_output_environment_override,
        "a rejected edit should retain the environment directory");
}

void TestPanelVisibilitySharesTheSettingsLifecycle()
{
    using namespace std::chrono_literals;

    TemporaryDirectory temporary;
    const auto storage = MakeStorage(temporary.path());
    spectiary::ApplicationSettings settings(storage);
    Require(
        settings.Apply(
            spectiary::ApplicationSettingsIntent::SetPanelVisibility(
                spectiary::ApplicationPanel::Annotations,
                false),
            {})
            .applied(),
        "panel close intent should apply");
    Require(
        settings.Apply(
            spectiary::ApplicationSettingsIntent::TogglePanelVisibility(
                spectiary::ApplicationPanel::SpectralLines),
            {})
            .applied(),
        "panel toggle intent should apply");
    settings.RunMaintenance(
        spectiary::LocalUserStateSaveScheduler::Clock::now() + 1s);

    spectiary::ApplicationSettings reloaded(storage);
    Require(
        !reloaded.View().panel_visibility.annotations &&
            !reloaded.View().panel_visibility.spectral_lines,
        "panel visibility should persist through application settings maintenance");

    Require(
        reloaded.Apply(
            spectiary::ApplicationSettingsIntent::ShowAllPanels(),
            {})
            .applied(),
        "show-all intent should apply atomically");
    Require(
        reloaded.View().panel_visibility ==
            spectiary::PanelVisibilityState{},
        "show-all intent should update the complete projection");
    reloaded.RunMaintenance(
        spectiary::LocalUserStateSaveScheduler::Clock::now() + 1s);
    const spectiary::ApplicationSettings restored(storage);
    Require(
        restored.View().panel_visibility ==
            spectiary::PanelVisibilityState{},
        "show-all intent should persist through maintenance and reload");
}

void TestUnpresentedPanelMutationChainRestoresItsOriginalBaseline()
{
    TemporaryDirectory temporary;
    const auto storage = MakeStorage(temporary.path());
    spectiary::ApplicationSettings settings(storage);
    spectiary::AutomationPanelMutationChain chain;

    const bool initial_visible =
        settings.View().panel_visibility.files;
    Require(
        initial_visible,
        "panel rollback fixture should start from the production visible default");

    Require(
        settings.Apply(
            spectiary::ApplicationSettingsIntent::
                SetPanelVisibility(
                    spectiary::ApplicationPanel::Files,
                    false),
            {})
            .applied(),
        "the first unpresented panel generation should apply");
    chain.RecordAppliedMutation(
        initial_visible,
        false,
        1,
        10);

    const bool second_previous_visible =
        settings.View().panel_visibility.files;
    Require(
        settings.Apply(
            spectiary::ApplicationSettingsIntent::
                SetPanelVisibility(
                    spectiary::ApplicationPanel::Files,
                    true),
            {})
            .applied(),
        "the opposite unpresented panel generation should apply");
    chain.RecordAppliedMutation(
        second_previous_visible,
        true,
        2,
        10);

    Require(
        chain.active() &&
            chain.generation() == 2 &&
            chain.baseline_visible(),
        "superseding an unpresented generation must preserve the pre-chain rollback baseline");

    (void)settings.Apply(
        spectiary::ApplicationSettingsIntent::
            SetPanelVisibility(
                spectiary::ApplicationPanel::Files,
                chain.baseline_visible()),
        {});
    chain.Clear();
    Require(
        settings.View().panel_visibility.files,
        "an unrenderable terminal should restore the live production state to the pre-chain value");
    Require(
        settings.Flush().all_saved(),
        "the restored production state should flush through the real panel cache owner");

    const spectiary::ApplicationSettings reloaded(storage);
    Require(
        reloaded.View().panel_visibility.files,
        "the final panel cache must retain the pre-chain value after rollback and shutdown-style flush");

    const auto applied_storage =
        MakeStorage(temporary.path() / "applied-rollback");
    spectiary::ApplicationSettings applied_settings(
        applied_storage);
    spectiary::AutomationPanelMutationChain
        applied_chain;
    Require(
        applied_settings.Apply(
            spectiary::ApplicationSettingsIntent::
                SetPanelVisibility(
                    spectiary::ApplicationPanel::Files,
                    false),
            {})
            .applied(),
        "the applied rollback fixture should mutate the production state away from its baseline");
    applied_chain.RecordAppliedMutation(
        true,
        false,
        1,
        20);
    const auto applied_rollback =
        applied_settings.Apply(
            spectiary::ApplicationSettingsIntent::
                SetPanelVisibility(
                    spectiary::ApplicationPanel::Files,
                    applied_chain.baseline_visible()),
            {});
    applied_chain.Clear();
    Require(
        applied_rollback.outcome ==
            spectiary::ApplicationSettingsOutcome::Applied,
        "a single unpresented generation must exercise a real Applied restoration mutation");
    Require(
        applied_settings.Flush().all_saved(),
        "an Applied restoration should mark the real panel cache dirty and flush successfully");
    const spectiary::ApplicationSettings
        applied_reloaded(applied_storage);
    Require(
        applied_reloaded.View().panel_visibility.files,
        "an Applied restoration must survive cache reload at the pre-chain baseline");
}

void TestProfileDirectoryChangeIsRejectedWhileRecording()
{
    TemporaryDirectory temporary;
    const auto storage = MakeStorage(temporary.path());
    spectiary::ApplicationSettings settings(storage);

    const auto result = settings.Apply(
        spectiary::ApplicationSettingsIntent::
            SetProfileOutputDirectory(
                temporary.path() / "recording-target"),
        {.profile_recording_in_progress = true});

    Require(
        result.outcome ==
            spectiary::ApplicationSettingsOutcome::Rejected,
        "recording should reject a profile directory change in the owner");
    Require(
        settings.View().profile_output_directory ==
            storage.default_profile_output_directory,
        "recording rejection should preserve the current profile directory");
    Require(
        settings.View()
                .StatusFor(
                    spectiary::ApplicationSetting::
                        ProfileOutputDirectory)
                .reason ==
            spectiary::ApplicationSettingsStatusReason::
                RecordingInProgress,
        "recording rejection should remain visible on the profile setting");

    const std::filesystem::path custom_directory =
        temporary.path() / "custom-target";
    Require(
        settings.Apply(
            spectiary::ApplicationSettingsIntent::
                SetProfileOutputDirectory(custom_directory),
            {})
            .applied(),
        "profile directory should remain editable when recording is inactive");
    const auto restore_result = settings.Apply(
        spectiary::ApplicationSettingsIntent::
            RestoreDefaultProfileOutputDirectory(),
        {.profile_recording_in_progress = true});
    Require(
        restore_result.outcome ==
            spectiary::ApplicationSettingsOutcome::Rejected,
        "recording should also reject restoring the profile directory");
    Require(
        settings.View().profile_output_directory ==
            custom_directory,
        "rejected restore should preserve the custom profile directory");
}

void TestSuccessfulSettingDoesNotClearAnotherSettingsStatus()
{
    TemporaryDirectory temporary;
    const std::filesystem::path blocker =
        temporary.path() / "not-a-directory";
    {
        std::ofstream stream(blocker);
        stream << "block language settings directory creation";
    }
    auto storage = MakeStorage(temporary.path());
    storage.language_settings_path =
        blocker / "ui-language.json";
    spectiary::ApplicationSettings settings(storage);

    Require(
        settings.Apply(
            spectiary::ApplicationSettingsIntent::SetLanguage(
                spectiary::UiLanguage::SimplifiedChinese),
            {})
                .outcome ==
            spectiary::ApplicationSettingsOutcome::
                PersistenceFailed,
        "language persistence failure should be observable");
    Require(
        settings.Apply(
            spectiary::ApplicationSettingsIntent::
                SetProfileOutputDirectory(
                    temporary.path() / "profiles-2"),
            {})
            .applied(),
        "independent profile directory change should apply");

    const spectiary::ApplicationSettingsView view = settings.View();
    Require(
        view.StatusFor(spectiary::ApplicationSetting::Language)
                .kind ==
            spectiary::ApplicationSettingsStatusKind::
                PersistenceError,
        "successful profile change must not clear language failure");
    Require(
        view.StatusFor(
                spectiary::ApplicationSetting::
                    ProfileOutputDirectory)
                .kind ==
            spectiary::ApplicationSettingsStatusKind::Ready,
        "successful profile change should clear only its own status");
}

void TestSettingsFlushKeepsIndependentOwnersAndCancelsTransactionalFailure()
{
    using namespace std::chrono_literals;

    TemporaryDirectory temporary;
    const std::filesystem::path blocker =
        temporary.path() / "not-a-directory";
    {
        std::ofstream stream(blocker);
        stream << "block language settings directory creation";
    }
    auto storage = MakeStorage(temporary.path());
    storage.language_settings_path =
        blocker / "ui-language.json";
    spectiary::ApplicationSettings settings(storage);

    Require(
        settings.Apply(
            spectiary::ApplicationSettingsIntent::SetLanguage(
                spectiary::UiLanguage::SimplifiedChinese),
            {})
                .outcome ==
            spectiary::ApplicationSettingsOutcome::
                PersistenceFailed,
        "a failed language owner should become a terminal setting failure");
    Require(
        settings.Apply(
            spectiary::ApplicationSettingsIntent::SetPanelVisibility(
                spectiary::ApplicationPanel::Annotations,
                false),
            {})
            .applied(),
        "an independent panel owner should still apply after a language failure");

    const spectiary::ApplicationSettingsFlushResult flushed =
        settings.Flush();
    Require(
        flushed.language_saved &&
            flushed.appearance_saved &&
            flushed.ui_scale_saved &&
            flushed.input_saved &&
            flushed.profile_output_directory_saved &&
            flushed.panel_visibility_saved,
        "settings shutdown flush should not retry a terminal transactional setting failure");
    Require(
        !spectiary::LoadPanelVisibilityStateCache(
             storage.panel_visibility_path)
             .state.annotations,
        "a successful panel owner must not be swallowed by a language failure");
    Require(
        !settings.PersistenceStatus(
                     spectiary::ApplicationSetting::Language)
             .retrying &&
            !settings.PersistenceStatus(
                     spectiary::ApplicationSetting::Language)
                 .save_message.empty(),
        "a terminal language failure should remain a warning without retrying after shutdown flush");

    std::filesystem::remove(blocker);
    std::filesystem::create_directories(blocker);
    Require(
        !settings.NextMaintenanceDeadline().has_value(),
        "repairing the path must not resurrect a terminal language retry deadline");
    settings.RunMaintenance(
        spectiary::LocalUserStateSaveScheduler::Clock::now() + 10s);
    Require(
        settings.View().language == spectiary::UiLanguage::English,
        "maintenance after a repaired path must not publish the failed language setting");
    Require(
        settings.Apply(
            spectiary::ApplicationSettingsIntent::SetLanguage(
                spectiary::UiLanguage::SimplifiedChinese),
            {})
            .applied(),
        "a new explicit language request should be allowed to retry after repair");
    Require(
        settings.View().language ==
                spectiary::UiLanguage::SimplifiedChinese &&
            settings.PersistenceStatus(
                        spectiary::ApplicationSetting::Language)
                .recovered,
        "the language owner should commit and report recovery after an explicit reapply");
}

void TestProfileDirectoryPersistenceFailureDoesNotRetryIntoRecording()
{
    using namespace std::chrono_literals;

    TemporaryDirectory temporary;
    const std::filesystem::path blocker =
        temporary.path() / "not-a-directory";
    {
        std::ofstream stream(blocker);
        stream << "block profile settings directory creation";
    }
    auto storage = MakeStorage(temporary.path());
    storage.profile_settings_path =
        blocker / "profile-settings.json";
    spectiary::ApplicationSettings settings(storage);
    const std::filesystem::path requested_directory =
        temporary.path() / "recording-target";

    Require(
        settings.Apply(
            spectiary::ApplicationSettingsIntent::
                SetProfileOutputDirectory(requested_directory),
            {})
            .outcome ==
            spectiary::ApplicationSettingsOutcome::PersistenceFailed,
        "a blocked profile directory save should fail before publishing");
    Require(
        settings.View().profile_output_directory ==
            storage.default_profile_output_directory &&
            !settings.NextMaintenanceDeadline().has_value(),
        "a failed profile directory save should retain the old value without a retry deadline");

    std::filesystem::remove(blocker);
    std::filesystem::create_directories(blocker);
    settings.RunMaintenance(
        spectiary::LocalUserStateSaveScheduler::Clock::now() + 10s);
    Require(
        settings.View().profile_output_directory ==
            storage.default_profile_output_directory,
        "maintenance after repair must not publish the failed profile directory");
    Require(
        settings.Apply(
            spectiary::ApplicationSettingsIntent::
                SetProfileOutputDirectory(requested_directory),
            {.profile_recording_in_progress = true})
            .outcome ==
            spectiary::ApplicationSettingsOutcome::Rejected &&
            settings.View().profile_output_directory ==
                storage.default_profile_output_directory,
        "a new profile directory request must still honor recording protection");
    Require(
        settings.Apply(
            spectiary::ApplicationSettingsIntent::
                SetProfileOutputDirectory(requested_directory),
            {})
            .applied() &&
            settings.View().profile_output_directory == requested_directory,
        "an explicit profile directory request after recording stops should apply normally");
}

void TestPanelVisibilityFailureRetriesThroughApplicationSettingsOwner()
{
    using namespace std::chrono_literals;

    TemporaryDirectory temporary;
    auto storage = MakeStorage(temporary.path());
    std::filesystem::create_directory(storage.panel_visibility_path);
    spectiary::ApplicationSettings settings(storage);

    Require(
        settings.View()
                .StatusFor(spectiary::ApplicationSetting::PanelVisibility)
                .kind == spectiary::ApplicationSettingsStatusKind::LoadWarning,
        "a blocked panel cache should establish an owner load warning");
    Require(
        settings.Apply(
            spectiary::ApplicationSettingsIntent::SetPanelVisibility(
                spectiary::ApplicationPanel::Annotations,
                false),
            {})
            .applied(),
        "panel visibility should remain live-editable before persistence succeeds");

    const auto debounce_deadline = settings.NextMaintenanceDeadline();
    Require(
        debounce_deadline.has_value(),
        "a panel visibility edit should expose its debounce deadline");
    settings.RunMaintenance(*debounce_deadline - 1ms);
    Require(
        std::filesystem::is_directory(storage.panel_visibility_path),
        "panel visibility should not attempt its save before the debounce deadline");

    const spectiary::ApplicationSettingsFlushResult failed =
        settings.Flush();
    Require(
        failed.language_saved &&
            failed.appearance_saved &&
            failed.ui_scale_saved &&
            failed.input_saved &&
            failed.profile_output_directory_saved &&
            !failed.panel_visibility_saved,
        "panel failure should remain an independent ApplicationSettings flush result");
    const spectiary::LocalUserStatePersistenceStatus retrying =
        settings.PersistenceStatus(
            spectiary::ApplicationSetting::PanelVisibility);
    Require(
        retrying.retrying &&
            !retrying.save_message.empty() &&
            !retrying.load_warning.empty(),
        "panel failure should retain retry, save diagnostic, and load warning health");

    std::filesystem::remove_all(storage.panel_visibility_path);
    const auto retry_deadline = settings.NextMaintenanceDeadline();
    Require(
        retry_deadline.has_value(),
        "panel failure should replace debounce with a retry deadline");
    settings.RunMaintenance(*retry_deadline - 1ms);
    Require(
        !std::filesystem::exists(storage.panel_visibility_path),
        "panel retry should not run before its retry deadline");
    settings.RunMaintenance(*retry_deadline);

    const spectiary::LocalUserStatePersistenceStatus recovered =
        settings.PersistenceStatus(
            spectiary::ApplicationSetting::PanelVisibility);
    Require(
        recovered.recovered &&
            !recovered.retrying &&
            recovered.load_warning.empty() &&
            !settings.NextMaintenanceDeadline().has_value() &&
            settings.View()
                    .StatusFor(spectiary::ApplicationSetting::PanelVisibility)
                    .kind == spectiary::ApplicationSettingsStatusKind::Ready,
        "panel retry should recover the real owner status and clear its deadline");
    Require(
        std::filesystem::exists(storage.panel_visibility_path),
        "panel retry should write the cache through the real owner");

    const spectiary::ApplicationSettings reloaded(storage);
    Require(
        !reloaded.View().panel_visibility.annotations &&
            reloaded.View()
                    .StatusFor(spectiary::ApplicationSetting::PanelVisibility)
                    .kind == spectiary::ApplicationSettingsStatusKind::Ready,
        "reloaded ApplicationSettings should retain the recovered panel visibility");
}

void TestAppearanceFallbackWarningsAndRepair()
{
    TemporaryDirectory temporary;
    const auto storage = MakeStorage(temporary.path());
    {
        std::ofstream stream(storage.appearance_settings_path);
        stream << "{invalid-json";
    }

    {
        spectiary::ApplicationSettings damaged(storage);
        const spectiary::ApplicationSettingsView fallback =
            damaged.View();
        Require(
            fallback.theme_selection ==
                    spectiary::ThemeSelection::FollowSystem() &&
                fallback
                        .StatusFor(
                            spectiary::ApplicationSetting::Appearance)
                        .kind ==
                    spectiary::ApplicationSettingsStatusKind::LoadWarning &&
                !damaged
                     .PersistenceStatus(
                         spectiary::ApplicationSetting::Appearance)
                     .load_warning.empty(),
            "corrupt appearance settings should warn and fall back to following the system");

        const spectiary::ApplicationSettingsResult repaired =
            damaged.Apply(
                spectiary::ApplicationSettingsIntent::
                    SetThemeSelection(
                        spectiary::ThemeSelection::FollowSystem()),
                {});
        Require(
            repaired.applied() &&
                damaged.View()
                        .StatusFor(
                            spectiary::ApplicationSetting::Appearance)
                        .kind ==
                    spectiary::ApplicationSettingsStatusKind::Ready,
            "re-selecting a warned follow-system fallback should repair its settings file");
        const std::string repaired_document =
            ReadFile(storage.appearance_settings_path);
        Require(
            repaired_document.find(
                R"("selection_policy": "follow_system")") !=
                    std::string::npos &&
                repaired_document.find("theme_id") ==
                    std::string::npos,
            "follow-system persistence should remain a policy rather than a concrete theme identity");
    }

    {
        std::ofstream stream(storage.appearance_settings_path);
        stream
            << R"({"format_kind":"spectiary.appearance.settings","schema_version":1,"selection_policy":"explicit","theme_id":"spectiary.theme.future"})";
    }
    spectiary::ApplicationSettings unknown(storage);
    Require(
        unknown.View().theme_selection ==
                spectiary::ThemeSelection::FollowSystem() &&
            unknown.View()
                    .StatusFor(
                        spectiary::ApplicationSetting::Appearance)
                    .kind ==
                spectiary::ApplicationSettingsStatusKind::LoadWarning,
        "an unknown persisted theme ID should warn and fall back to following the system");

    const spectiary::ApplicationSettingsResult rejected =
        unknown.Apply(
            spectiary::ApplicationSettingsIntent::
                SetThemeSelection(
                    spectiary::ThemeSelection::Explicit(
                        spectiary::ThemeId(
                            std::string_view(
                                "spectiary.theme.future")))),
            {});
    Require(
        rejected.outcome ==
                spectiary::ApplicationSettingsOutcome::Rejected &&
            rejected.setting ==
                spectiary::ApplicationSetting::Appearance &&
            unknown.View().theme_selection ==
                spectiary::ThemeSelection::FollowSystem(),
        "unsupported explicit theme intents should be rejected without changing the fallback");
}

void TestAppearancePersistenceFailureRetainsPreviousSelection()
{
    TemporaryDirectory temporary;
    const std::filesystem::path blocker =
        temporary.path() / "blocked";
    {
        std::ofstream stream(blocker);
        stream << "not a directory";
    }
    auto storage = MakeStorage(temporary.path());
    storage.appearance_settings_path =
        blocker / "appearance-settings.json";
    spectiary::ApplicationSettings settings(storage);

    const spectiary::ApplicationSettingsResult result =
        settings.Apply(
            spectiary::ApplicationSettingsIntent::
                SetThemeSelection(
                    spectiary::ThemeSelection::Explicit(
                        spectiary::BuiltInLightThemeId())),
            {});
    Require(
        result.outcome ==
                spectiary::ApplicationSettingsOutcome::
                    PersistenceFailed &&
            settings.View().theme_selection ==
                spectiary::ThemeSelection::FollowSystem() &&
            settings.View()
                    .StatusFor(
                        spectiary::ApplicationSetting::Appearance)
                    .kind ==
                spectiary::ApplicationSettingsStatusKind::
                    PersistenceError,
        "a failed appearance write should retain the previous selection and expose the shared persistence error");
}

}  // namespace

void TestExternalOpenInstancePolicy()
{
    using namespace spectiary;
    TemporaryDirectory temporary;
    const auto storage = MakeStorage(temporary.path());
    ApplicationSettings settings(storage);
    Require(settings.View().external_open_instance_policy == ExternalOpenInstancePolicy::NewInstance,
        "missing routing preference must retain new-instance startup");
    ApplicationSettingsIntent intent;
    intent.kind = ApplicationSettingsIntentKind::SetExternalOpenInstancePolicy;
    intent.external_open_instance_policy = ExternalOpenInstancePolicy::RecentInstance;
    Require(settings.Apply(intent, {}).outcome == ApplicationSettingsOutcome::Applied,
        "recent-instance preference must persist");
    Require(settings.Apply(ApplicationSettingsIntent::SetOpenExternalSourceAsFolder(true), {}).outcome ==
        ApplicationSettingsOutcome::Applied, "folder preference must save alongside routing");
    ApplicationSettings reloaded(storage);
    Require(reloaded.View().external_open_instance_policy == ExternalOpenInstancePolicy::RecentInstance &&
        reloaded.View().open_external_source_as_folder, "both external-open preferences must survive reload");
    intent.external_open_instance_policy = ExternalOpenInstancePolicy::NewInstance;
    Require(reloaded.Apply(intent, {}).outcome == ApplicationSettingsOutcome::Applied,
        "new-instance preference must persist");
    Require(ApplicationSettings(storage).View().open_external_source_as_folder,
        "routing changes must retain folder preference");
    std::ofstream(storage.external_source_settings_path, std::ios::trunc)
        << R"({"format_kind":"spectiary.external_source.settings","schema_version":1,"open_external_source_as_folder":true,"instance_policy":"unknown"})";
    ApplicationSettings invalid(storage);
    Require(invalid.View().external_open_instance_policy == ExternalOpenInstancePolicy::NewInstance &&
        invalid.View().open_external_source_as_folder &&
        invalid.View().StatusFor(ApplicationSetting::ExternalSource).kind == ApplicationSettingsStatusKind::LoadWarning,
        "invalid routing must default safely without discarding folder preference");
    const auto blocked = temporary.path() / "blocker";
    std::ofstream(blocked) << "file";
    auto blocked_storage = storage;
    blocked_storage.external_source_settings_path = blocked / "settings.json";
    ApplicationSettings failure(blocked_storage);
    intent.external_open_instance_policy = ExternalOpenInstancePolicy::RecentInstance;
    Require(failure.Apply(intent, {}).outcome == ApplicationSettingsOutcome::PersistenceFailed &&
        failure.View().external_open_instance_policy == ExternalOpenInstancePolicy::NewInstance,
        "failed routing writes must report failure and preserve previous policy");
}

int main()
{
    TestExternalOpenInstancePolicy();
    TestSettingsIntentsPersistAndReloadThroughOneOwner();
    TestLegacyExternalSourcePreferenceLoadsThroughApplicationSettings();
    TestCompactSingleValueFilesRemainReadableThroughApplicationSettings();
    TestInvalidSingleValuesFallBackThroughApplicationSettings();
    TestUnsupportedSingleValueSchemasFallBackThroughApplicationSettings();
    TestPersistenceFailureRetainsThePreviousValueAndStatus();
    TestLanguageValidationUsesApplicationSettingsInterface();
    TestUiScaleValidationAndPersistenceFirstBehavior();
    TestLiveNumericNavigationPersistenceFailureRetainsEnabledValue();
    TestExternalSourceFolderPersistenceFailureRetainsDisabledValue();
    TestUiScaleResetRepairsDamagedFallbackState();
    TestLanguageAndProfileFallbacksCanBeReapplied();
    TestLoadWarningAndEnvironmentOverrideAreTyped();
    TestPanelVisibilitySharesTheSettingsLifecycle();
    TestUnpresentedPanelMutationChainRestoresItsOriginalBaseline();
    TestProfileDirectoryChangeIsRejectedWhileRecording();
    TestProfileDirectoryPersistenceFailureDoesNotRetryIntoRecording();
    TestSuccessfulSettingDoesNotClearAnotherSettingsStatus();
    TestSettingsFlushKeepsIndependentOwnersAndCancelsTransactionalFailure();
    TestPanelVisibilityFailureRetriesThroughApplicationSettingsOwner();
    TestAppearanceFallbackWarningsAndRepair();
    TestAppearancePersistenceFailureRetainsPreviousSelection();
    return 0;
}
