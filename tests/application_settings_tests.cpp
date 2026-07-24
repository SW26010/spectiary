#include "app/application_settings.h"
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
            !language_status.detail.empty(),
        "save failure should remain visible on the owner view");
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

    specforge::ApplicationSettings settings(storage);
    const specforge::ApplicationSettingsView loaded = settings.View();
    const specforge::ApplicationSettingsStatus& language_status =
        loaded.StatusFor(specforge::ApplicationSetting::Language);
    Require(
        language_status.kind ==
                specforge::ApplicationSettingsStatusKind::
                    LoadWarning &&
            language_status.setting ==
                specforge::ApplicationSetting::Language,
        "damaged language settings should produce a typed load warning");
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
            specforge::ApplicationSettingsOutcome::Rejected,
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
                .kind ==
            specforge::ApplicationSettingsStatusKind::Rejected,
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

}  // namespace

int main()
{
    TestSettingsIntentsPersistAndReloadThroughOneOwner();
    TestPersistenceFailureRetainsThePreviousValueAndStatus();
    TestLoadWarningAndEnvironmentOverrideAreTyped();
    TestPanelVisibilitySharesTheSettingsLifecycle();
    TestProfileDirectoryChangeIsRejectedWhileRecording();
    TestSuccessfulSettingDoesNotClearAnotherSettingsStatus();
    return 0;
}
