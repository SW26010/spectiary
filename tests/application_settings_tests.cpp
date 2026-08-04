#include "app/application_settings.h"
#include "automation/automation_panel_mutation_chain.h"
#include "ui/ui_language_settings.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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
                ("specforge-application-settings-tests-" +
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

specforge::ApplicationSettingsStorage MakeStorage(
    const std::filesystem::path& root,
    std::optional<std::filesystem::path> environment_override =
        std::nullopt)
{
    return {
        .language_settings_path = root / "ui-language.json",
        .ui_scale_settings_path = root / "ui-scale.json",
        .input_settings_path = root / "input-settings.json",
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
    specforge::ApplicationSettings settings(storage);

    const specforge::ApplicationSettingsView initial =
        settings.View();
    Require(
        initial.language == specforge::UiLanguage::English,
        "missing language settings should default to English");
    Require(
        initial.ui_scale_percentage == 100,
        "missing UI scale settings should default to 100%");
    Require(
        initial.live_numeric_navigation,
        "missing input settings should enable live numeric navigation");
    Require(
        initial.profile_output_directory ==
            storage.default_profile_output_directory,
        "missing profile settings should use the default directory");

    const auto language_result = settings.Apply(
        specforge::ApplicationSettingsIntent::SetLanguage(
            specforge::UiLanguage::SimplifiedChinese),
        {});
    Require(
        language_result.applied(),
        "language intent should apply");

    const auto ui_scale_result = settings.Apply(
        specforge::ApplicationSettingsIntent::SetUiScale(125),
        {});
    Require(
        ui_scale_result.applied(),
        "UI scale intent should apply");

    const auto input_result = settings.Apply(
        specforge::ApplicationSettingsIntent::
            SetLiveNumericNavigation(false),
        {});
    Require(
        input_result.applied(),
        "live numeric navigation intent should apply");

    const std::filesystem::path custom_directory =
        temporary.path() / "custom profiles";
    const auto directory_result = settings.Apply(
        specforge::ApplicationSettingsIntent::
            SetProfileOutputDirectory(custom_directory),
        {});
    Require(
        directory_result.applied(),
        "profile output directory intent should apply");

    specforge::ApplicationSettings reloaded(storage);
    const specforge::ApplicationSettingsView reloaded_view =
        reloaded.View();
    Require(
        reloaded_view.language ==
            specforge::UiLanguage::SimplifiedChinese,
        "language should reload through the application settings owner");
    Require(
        reloaded_view.ui_scale_percentage == 125,
        "UI scale should reload through the application settings owner");
    Require(
        !reloaded_view.live_numeric_navigation,
        "live numeric navigation should reload through the application settings owner");
    Require(
        reloaded_view.profile_output_directory == custom_directory,
        "profile directory should reload through the application settings owner");
    Require(
        reloaded_view.profile_output_directory_source ==
            specforge::ProfileOutputDirectorySource::UserSetting,
        "reloaded custom profile directory should retain its source");

    const auto restore_result = reloaded.Apply(
        specforge::ApplicationSettingsIntent::
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

void TestPersistenceFailureRetainsThePreviousValueAndStatus()
{
    TemporaryDirectory temporary;
    const std::filesystem::path blocker =
        temporary.path() / "not-a-directory";
    {
        std::ofstream stream(blocker);
        stream << "block settings directory creation";
    }

    specforge::ApplicationSettings settings({
        .language_settings_path = blocker / "ui-language.json",
        .ui_scale_settings_path = blocker / "ui-scale.json",
        .input_settings_path = blocker / "input-settings.json",
        .profile_settings_path = blocker / "profile-settings.json",
        .panel_visibility_path = blocker / "panel-visibility.json",
        .default_profile_output_directory =
            temporary.path() / "profiles",
    });

    const auto result = settings.Apply(
        specforge::ApplicationSettingsIntent::SetLanguage(
            specforge::UiLanguage::SimplifiedChinese),
        {});
    Require(
        result.outcome ==
            specforge::ApplicationSettingsOutcome::
                PersistenceFailed,
        "save failure should be a typed persistence outcome");
    const specforge::ApplicationSettingsView view = settings.View();
    const specforge::ApplicationSettingsStatus& language_status =
        view.StatusFor(specforge::ApplicationSetting::Language);
    Require(
        view.language == specforge::UiLanguage::English,
        "save failure should retain the previous language");
    Require(
        language_status.kind ==
                specforge::ApplicationSettingsStatusKind::
                    PersistenceError &&
            language_status.setting ==
                specforge::ApplicationSetting::Language &&
            language_status.reason ==
                specforge::ApplicationSettingsStatusReason::
                    SettingsWriteFailed &&
            !language_status.detail.empty(),
        "save failure should remain structured on the owner view");
    Require(
        !settings
             .PersistenceStatus(
                 specforge::ApplicationSetting::Language)
             .save_message.empty(),
        "application persistence health should retain a failed setting save");

    std::filesystem::remove(blocker);
    std::filesystem::create_directories(blocker);
    Require(
        settings.Apply(
            specforge::ApplicationSettingsIntent::SetLanguage(
                specforge::UiLanguage::SimplifiedChinese),
            {})
            .applied(),
        "the failed language save should succeed after repairing its path");
    Require(
        settings
            .PersistenceStatus(
                specforge::ApplicationSetting::Language)
            .recovered,
        "a successful setting retry should expose recovery");
}

void TestUiScaleValidationAndPersistenceFirstBehavior()
{
    TemporaryDirectory temporary;
    const auto storage = MakeStorage(temporary.path());
    specforge::ApplicationSettings settings(storage);

    const auto unchanged = settings.Apply(
        specforge::ApplicationSettingsIntent::SetUiScale(100),
        {});
    Require(
        unchanged.outcome ==
                specforge::ApplicationSettingsOutcome::Unchanged &&
            unchanged.setting ==
                specforge::ApplicationSetting::UiScale,
        "unchanged UI scale intent should be typed");

    for (const int percentage : {80, 100, 150}) {
        const auto result = settings.Apply(
            specforge::ApplicationSettingsIntent::SetUiScale(
                percentage),
            {});
        Require(
            result.outcome ==
                    specforge::ApplicationSettingsOutcome::Applied ||
                result.outcome ==
                    specforge::ApplicationSettingsOutcome::Unchanged,
            "supported UI scale should apply");
        const specforge::ApplicationSettings reloaded(storage);
        Require(
            reloaded.View().ui_scale_percentage == percentage,
            "supported UI scale should round-trip");
    }

    const auto rejected = settings.Apply(
        specforge::ApplicationSettingsIntent::SetUiScale(151),
        {});
    Require(
        rejected.outcome ==
                specforge::ApplicationSettingsOutcome::Rejected &&
            settings.View().ui_scale_percentage == 150 &&
            settings.View()
                    .StatusFor(
                        specforge::ApplicationSetting::UiScale)
                    .reason ==
                specforge::ApplicationSettingsStatusReason::
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
    specforge::ApplicationSettings failing_settings(
        failing_storage);
    const auto failure = failing_settings.Apply(
        specforge::ApplicationSettingsIntent::SetUiScale(125),
        {});
    const specforge::ApplicationSettingsView failed_view =
        failing_settings.View();
    Require(
        failure.outcome ==
                specforge::ApplicationSettingsOutcome::
                    PersistenceFailed &&
            failed_view.ui_scale_percentage == 100,
        "UI scale save failure should retain the previous runtime value");
    Require(
        failed_view.StatusFor(
                specforge::ApplicationSetting::UiScale)
                .kind ==
            specforge::ApplicationSettingsStatusKind::
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
    specforge::ApplicationSettings settings(storage);
    const specforge::ApplicationSettingsResult result =
        settings.Apply(
            specforge::ApplicationSettingsIntent::
                SetLiveNumericNavigation(false),
            {});
    const specforge::ApplicationSettingsView view =
        settings.View();
    Require(
        result.outcome ==
                specforge::ApplicationSettingsOutcome::
                    PersistenceFailed &&
            view.live_numeric_navigation,
        "input settings save failure should retain the enabled runtime value");
    Require(
        view.StatusFor(specforge::ApplicationSetting::Input).kind ==
            specforge::ApplicationSettingsStatusKind::
                PersistenceError,
        "input settings save failure should remain visible on the owner view");
}

void TestUiScaleResetRepairsDamagedFallbackState()
{
    TemporaryDirectory temporary;
    const auto storage = MakeStorage(temporary.path());
    {
        std::ofstream stream(storage.ui_scale_settings_path);
        stream << R"({"format_kind":)";
    }
    specforge::ApplicationSettings settings(storage);
    const specforge::ApplicationSettingsView fallback =
        settings.View();
    Require(
        fallback.ui_scale_percentage == 100 &&
            fallback.StatusFor(
                    specforge::ApplicationSetting::UiScale)
                    .kind ==
                specforge::ApplicationSettingsStatusKind::
                    LoadWarning,
        "damaged UI scale settings should establish a warned 100% fallback");

    const auto repair = settings.Apply(
        specforge::ApplicationSettingsIntent::SetUiScale(100),
        {});
    Require(
        repair.outcome ==
                specforge::ApplicationSettingsOutcome::Applied &&
            settings.View()
                    .StatusFor(
                        specforge::ApplicationSetting::UiScale)
                    .kind ==
                specforge::ApplicationSettingsStatusKind::Ready,
        "resetting the warned fallback should rewrite and clear its status");

    const specforge::ApplicationSettings reloaded(storage);
    Require(
        reloaded.View().ui_scale_percentage == 100 &&
            reloaded.View()
                    .StatusFor(
                        specforge::ApplicationSetting::UiScale)
                    .kind ==
                specforge::ApplicationSettingsStatusKind::Ready,
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

    specforge::ApplicationSettings settings(storage);
    const specforge::ApplicationSettingsView fallback =
        settings.View();
    Require(
        fallback.language == specforge::UiLanguage::English &&
            fallback.StatusFor(
                    specforge::ApplicationSetting::Language)
                    .kind ==
                specforge::ApplicationSettingsStatusKind::
                    LoadWarning,
        "damaged language settings should establish a warned English fallback");
    Require(
        fallback.profile_output_directory ==
                storage.default_profile_output_directory &&
            fallback.StatusFor(
                    specforge::ApplicationSetting::
                        ProfileOutputDirectory)
                    .kind ==
                specforge::ApplicationSettingsStatusKind::
                    LoadWarning,
        "damaged profile settings should establish a warned default fallback");

    const auto language_repair = settings.Apply(
        specforge::ApplicationSettingsIntent::SetLanguage(
            specforge::UiLanguage::English),
        {});
    const auto profile_repair = settings.Apply(
        specforge::ApplicationSettingsIntent::
            RestoreDefaultProfileOutputDirectory(),
        {});
    Require(
        language_repair.applied() &&
            settings.View()
                    .StatusFor(
                        specforge::ApplicationSetting::Language)
                    .kind ==
                specforge::ApplicationSettingsStatusKind::Ready,
        "reapplying the warned language fallback should rewrite and clear its status");
    Require(
        profile_repair.applied() &&
            settings.View()
                    .StatusFor(
                        specforge::ApplicationSetting::
                            ProfileOutputDirectory)
                    .kind ==
                specforge::ApplicationSettingsStatusKind::Ready,
        "restoring the warned profile fallback should rewrite and clear its status");

    const specforge::ApplicationSettings reloaded(storage);
    Require(
        reloaded.View()
                    .StatusFor(
                        specforge::ApplicationSetting::Language)
                    .kind ==
                specforge::ApplicationSettingsStatusKind::Ready &&
            reloaded.View()
                    .StatusFor(
                        specforge::ApplicationSetting::
                            ProfileOutputDirectory)
                    .kind ==
                specforge::ApplicationSettingsStatusKind::Ready,
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
        std::ofstream stream(storage.profile_settings_path);
        stream << R"({"format_kind":)";
    }
    {
        std::ofstream stream(storage.panel_visibility_path);
        stream << R"({"format_kind":)";
    }

    specforge::ApplicationSettings settings(storage);
    const specforge::ApplicationSettingsView loaded = settings.View();
    const specforge::ApplicationSettingsStatus& language_status =
        loaded.StatusFor(specforge::ApplicationSetting::Language);
    Require(
        language_status.kind ==
                specforge::ApplicationSettingsStatusKind::
                    LoadWarning &&
            language_status.setting ==
                specforge::ApplicationSetting::Language &&
            language_status.reason ==
                specforge::ApplicationSettingsStatusReason::
                    SavedValueUnreadable,
        "damaged language settings should produce a typed load warning");
    const specforge::ApplicationSettingsStatus& ui_scale_status =
        loaded.StatusFor(specforge::ApplicationSetting::UiScale);
    Require(
        loaded.ui_scale_percentage == 100 &&
            ui_scale_status.kind ==
                specforge::ApplicationSettingsStatusKind::
                    LoadWarning &&
            ui_scale_status.setting ==
                specforge::ApplicationSetting::UiScale,
        "damaged UI scale settings should fall back with a typed load warning");
    Require(
        loaded.live_numeric_navigation &&
            loaded
                    .StatusFor(
                        specforge::ApplicationSetting::Input)
                    .kind ==
                specforge::ApplicationSettingsStatusKind::
                    LoadWarning,
        "damaged input settings should enable live navigation with a typed load warning");
    Require(
        loaded
                .StatusFor(
                    specforge::ApplicationSetting::
                        ProfileOutputDirectory)
                .kind ==
            specforge::ApplicationSettingsStatusKind::
                LoadWarning,
        "damaged profile settings should produce a typed load warning");
    Require(
        !settings
             .PersistenceStatus(
                 specforge::ApplicationSetting::
                     ProfileOutputDirectory)
             .load_warning.empty(),
        "profile load warning should reach application persistence health");
    Require(
        loaded
                .StatusFor(
                    specforge::ApplicationSetting::
                        PanelVisibility)
                .kind ==
            specforge::ApplicationSettingsStatusKind::
                LoadWarning,
        "damaged panel visibility should produce a typed load warning");
    Require(
        !settings
             .PersistenceStatus(
                 specforge::ApplicationSetting::
                     PanelVisibility)
             .load_warning.empty(),
        "panel visibility warning should reach application persistence health");
    Require(
        loaded.profile_output_directory_source ==
            specforge::ProfileOutputDirectorySource::Environment,
        "environment profile directory should retain explicit precedence");

    const auto result = settings.Apply(
        specforge::ApplicationSettingsIntent::
            SetProfileOutputDirectory(
                temporary.path() / "ignored"),
        {});
    Require(
        result.outcome ==
                specforge::ApplicationSettingsOutcome::Rejected &&
            settings.View()
                    .StatusFor(
                        specforge::ApplicationSetting::
                            ProfileOutputDirectory)
                    .reason ==
                specforge::ApplicationSettingsStatusReason::
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
    specforge::ApplicationSettings settings(storage);
    Require(
        settings.Apply(
            specforge::ApplicationSettingsIntent::SetPanelVisibility(
                specforge::ApplicationPanel::Annotations,
                false),
            {})
            .applied(),
        "panel close intent should apply");
    Require(
        settings.Apply(
            specforge::ApplicationSettingsIntent::TogglePanelVisibility(
                specforge::ApplicationPanel::SpectralLines),
            {})
            .applied(),
        "panel toggle intent should apply");
    settings.RunMaintenance(
        specforge::LocalUserStateSaveScheduler::Clock::now() + 1s);

    specforge::ApplicationSettings reloaded(storage);
    Require(
        !reloaded.View().panel_visibility.annotations &&
            !reloaded.View().panel_visibility.spectral_lines,
        "panel visibility should persist through application settings maintenance");

    Require(
        reloaded.Apply(
            specforge::ApplicationSettingsIntent::ShowAllPanels(),
            {})
            .applied(),
        "show-all intent should apply atomically");
    Require(
        reloaded.View().panel_visibility ==
            specforge::PanelVisibilityState{},
        "show-all intent should update the complete projection");
    reloaded.RunMaintenance(
        specforge::LocalUserStateSaveScheduler::Clock::now() + 1s);
    const specforge::ApplicationSettings restored(storage);
    Require(
        restored.View().panel_visibility ==
            specforge::PanelVisibilityState{},
        "show-all intent should persist through maintenance and reload");
}

void TestUnpresentedPanelMutationChainRestoresItsOriginalBaseline()
{
    TemporaryDirectory temporary;
    const auto storage = MakeStorage(temporary.path());
    specforge::ApplicationSettings settings(storage);
    specforge::AutomationPanelMutationChain chain;

    const bool initial_visible =
        settings.View().panel_visibility.files;
    Require(
        initial_visible,
        "panel rollback fixture should start from the production visible default");

    Require(
        settings.Apply(
            specforge::ApplicationSettingsIntent::
                SetPanelVisibility(
                    specforge::ApplicationPanel::Files,
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
            specforge::ApplicationSettingsIntent::
                SetPanelVisibility(
                    specforge::ApplicationPanel::Files,
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
        specforge::ApplicationSettingsIntent::
            SetPanelVisibility(
                specforge::ApplicationPanel::Files,
                chain.baseline_visible()),
        {});
    chain.Clear();
    Require(
        settings.View().panel_visibility.files,
        "an unrenderable terminal should restore the live production state to the pre-chain value");
    Require(
        settings.Flush().all_saved(),
        "the restored production state should flush through the real panel cache owner");

    const specforge::ApplicationSettings reloaded(storage);
    Require(
        reloaded.View().panel_visibility.files,
        "the final panel cache must retain the pre-chain value after rollback and shutdown-style flush");

    const auto applied_storage =
        MakeStorage(temporary.path() / "applied-rollback");
    specforge::ApplicationSettings applied_settings(
        applied_storage);
    specforge::AutomationPanelMutationChain
        applied_chain;
    Require(
        applied_settings.Apply(
            specforge::ApplicationSettingsIntent::
                SetPanelVisibility(
                    specforge::ApplicationPanel::Files,
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
            specforge::ApplicationSettingsIntent::
                SetPanelVisibility(
                    specforge::ApplicationPanel::Files,
                    applied_chain.baseline_visible()),
            {});
    applied_chain.Clear();
    Require(
        applied_rollback.outcome ==
            specforge::ApplicationSettingsOutcome::Applied,
        "a single unpresented generation must exercise a real Applied restoration mutation");
    Require(
        applied_settings.Flush().all_saved(),
        "an Applied restoration should mark the real panel cache dirty and flush successfully");
    const specforge::ApplicationSettings
        applied_reloaded(applied_storage);
    Require(
        applied_reloaded.View().panel_visibility.files,
        "an Applied restoration must survive cache reload at the pre-chain baseline");
}

void TestProfileDirectoryChangeIsRejectedWhileRecording()
{
    TemporaryDirectory temporary;
    const auto storage = MakeStorage(temporary.path());
    specforge::ApplicationSettings settings(storage);

    const auto result = settings.Apply(
        specforge::ApplicationSettingsIntent::
            SetProfileOutputDirectory(
                temporary.path() / "recording-target"),
        {.profile_recording_in_progress = true});

    Require(
        result.outcome ==
            specforge::ApplicationSettingsOutcome::Rejected,
        "recording should reject a profile directory change in the owner");
    Require(
        settings.View().profile_output_directory ==
            storage.default_profile_output_directory,
        "recording rejection should preserve the current profile directory");
    Require(
        settings.View()
                .StatusFor(
                    specforge::ApplicationSetting::
                        ProfileOutputDirectory)
                .reason ==
            specforge::ApplicationSettingsStatusReason::
                RecordingInProgress,
        "recording rejection should remain visible on the profile setting");

    const std::filesystem::path custom_directory =
        temporary.path() / "custom-target";
    Require(
        settings.Apply(
            specforge::ApplicationSettingsIntent::
                SetProfileOutputDirectory(custom_directory),
            {})
            .applied(),
        "profile directory should remain editable when recording is inactive");
    const auto restore_result = settings.Apply(
        specforge::ApplicationSettingsIntent::
            RestoreDefaultProfileOutputDirectory(),
        {.profile_recording_in_progress = true});
    Require(
        restore_result.outcome ==
            specforge::ApplicationSettingsOutcome::Rejected,
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
    specforge::ApplicationSettings settings(storage);

    Require(
        settings.Apply(
            specforge::ApplicationSettingsIntent::SetLanguage(
                specforge::UiLanguage::SimplifiedChinese),
            {})
                .outcome ==
            specforge::ApplicationSettingsOutcome::
                PersistenceFailed,
        "language persistence failure should be observable");
    Require(
        settings.Apply(
            specforge::ApplicationSettingsIntent::
                SetProfileOutputDirectory(
                    temporary.path() / "profiles-2"),
            {})
            .applied(),
        "independent profile directory change should apply");

    const specforge::ApplicationSettingsView view = settings.View();
    Require(
        view.StatusFor(specforge::ApplicationSetting::Language)
                .kind ==
            specforge::ApplicationSettingsStatusKind::
                PersistenceError,
        "successful profile change must not clear language failure");
    Require(
        view.StatusFor(
                specforge::ApplicationSetting::
                    ProfileOutputDirectory)
                .kind ==
            specforge::ApplicationSettingsStatusKind::Ready,
        "successful profile change should clear only its own status");
}

void TestSettingsFlushKeepsIndependentOwnersAndRetries()
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
    specforge::ApplicationSettings settings(storage);

    Require(
        settings.Apply(
            specforge::ApplicationSettingsIntent::SetLanguage(
                specforge::UiLanguage::SimplifiedChinese),
            {})
                .outcome ==
            specforge::ApplicationSettingsOutcome::
                PersistenceFailed,
        "a failed language owner should retain a retryable pending save");
    Require(
        settings.Apply(
            specforge::ApplicationSettingsIntent::SetPanelVisibility(
                specforge::ApplicationPanel::Annotations,
                false),
            {})
            .applied(),
        "an independent panel owner should still apply after a language failure");

    const specforge::ApplicationSettingsFlushResult flushed =
        settings.Flush();
    Require(
        !flushed.language_saved &&
            flushed.ui_scale_saved &&
            flushed.input_saved &&
            flushed.profile_output_directory_saved &&
            flushed.panel_visibility_saved,
        "settings shutdown flush should preserve each owner's independent result");
    Require(
        !specforge::LoadPanelVisibilityStateCache(
             storage.panel_visibility_path)
             .state.annotations,
        "a successful panel owner must not be swallowed by a language failure");
    Require(
        settings.PersistenceStatus(
                    specforge::ApplicationSetting::Language)
                .retrying,
        "a failed language owner should remain retrying after shutdown flush");

    std::filesystem::remove(blocker);
    std::filesystem::create_directories(blocker);
    const auto deadline = settings.NextMaintenanceDeadline();
    Require(
        deadline.has_value(),
        "a failed language owner should expose its maintenance deadline");
    settings.RunMaintenance(*deadline - 1ms);
    Require(
        settings.View().language == specforge::UiLanguage::English,
        "language should remain unchanged before its retry deadline");
    settings.RunMaintenance(*deadline);
    Require(
        settings.View().language ==
                specforge::UiLanguage::SimplifiedChinese &&
            settings.PersistenceStatus(
                        specforge::ApplicationSetting::Language)
                .recovered,
        "the language owner should commit and report recovery at its deadline");
}

void TestPanelVisibilityFailureRetriesThroughApplicationSettingsOwner()
{
    using namespace std::chrono_literals;

    TemporaryDirectory temporary;
    auto storage = MakeStorage(temporary.path());
    std::filesystem::create_directory(storage.panel_visibility_path);
    specforge::ApplicationSettings settings(storage);

    Require(
        settings.View()
                .StatusFor(specforge::ApplicationSetting::PanelVisibility)
                .kind == specforge::ApplicationSettingsStatusKind::LoadWarning,
        "a blocked panel cache should establish an owner load warning");
    Require(
        settings.Apply(
            specforge::ApplicationSettingsIntent::SetPanelVisibility(
                specforge::ApplicationPanel::Annotations,
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

    const specforge::ApplicationSettingsFlushResult failed =
        settings.Flush();
    Require(
        failed.language_saved &&
            failed.ui_scale_saved &&
            failed.input_saved &&
            failed.profile_output_directory_saved &&
            !failed.panel_visibility_saved,
        "panel failure should remain an independent ApplicationSettings flush result");
    const specforge::LocalUserStatePersistenceStatus retrying =
        settings.PersistenceStatus(
            specforge::ApplicationSetting::PanelVisibility);
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

    const specforge::LocalUserStatePersistenceStatus recovered =
        settings.PersistenceStatus(
            specforge::ApplicationSetting::PanelVisibility);
    Require(
        recovered.recovered &&
            !recovered.retrying &&
            recovered.load_warning.empty() &&
            !settings.NextMaintenanceDeadline().has_value() &&
            settings.View()
                    .StatusFor(specforge::ApplicationSetting::PanelVisibility)
                    .kind == specforge::ApplicationSettingsStatusKind::Ready,
        "panel retry should recover the real owner status and clear its deadline");
    Require(
        std::filesystem::exists(storage.panel_visibility_path),
        "panel retry should write the cache through the real owner");

    const specforge::ApplicationSettings reloaded(storage);
    Require(
        !reloaded.View().panel_visibility.annotations &&
            reloaded.View()
                    .StatusFor(specforge::ApplicationSetting::PanelVisibility)
                    .kind == specforge::ApplicationSettingsStatusKind::Ready,
        "reloaded ApplicationSettings should retain the recovered panel visibility");
}

}  // namespace

int main()
{
    TestSettingsIntentsPersistAndReloadThroughOneOwner();
    TestPersistenceFailureRetainsThePreviousValueAndStatus();
    TestUiScaleValidationAndPersistenceFirstBehavior();
    TestLiveNumericNavigationPersistenceFailureRetainsEnabledValue();
    TestUiScaleResetRepairsDamagedFallbackState();
    TestLanguageAndProfileFallbacksCanBeReapplied();
    TestLoadWarningAndEnvironmentOverrideAreTyped();
    TestPanelVisibilitySharesTheSettingsLifecycle();
    TestUnpresentedPanelMutationChainRestoresItsOriginalBaseline();
    TestProfileDirectoryChangeIsRejectedWhileRecording();
    TestSuccessfulSettingDoesNotClearAnotherSettingsStatus();
    TestSettingsFlushKeepsIndependentOwnersAndRetries();
    TestPanelVisibilityFailureRetriesThroughApplicationSettingsOwner();
    return 0;
}
