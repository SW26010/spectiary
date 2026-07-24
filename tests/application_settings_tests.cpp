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
            specforge::UiLanguage::SimplifiedChinese));
    Require(
        language_result.applied(),
        "language intent should apply");

    const std::filesystem::path custom_directory =
        temporary.path() / "custom profiles";
    const auto directory_result = settings.Apply(
        specforge::ApplicationSettingsIntent::
            SetProfileOutputDirectory(custom_directory));
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
            RestoreDefaultProfileOutputDirectory());
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
            specforge::UiLanguage::SimplifiedChinese));
    Require(
        result.outcome ==
            specforge::ApplicationSettingsOutcome::
                PersistenceFailed,
        "save failure should be a typed persistence outcome");
    const specforge::ApplicationSettingsView view = settings.View();
    Require(
        view.language == specforge::UiLanguage::English,
        "save failure should retain the previous language");
    Require(
        view.status.kind ==
                specforge::ApplicationSettingsStatusKind::
                    PersistenceError &&
            view.status.setting ==
                specforge::ApplicationSetting::Language &&
            !view.status.detail.empty(),
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
    Require(
        loaded.status.kind ==
                specforge::ApplicationSettingsStatusKind::
                    LoadWarning &&
            loaded.status.setting ==
                specforge::ApplicationSetting::Language,
        "damaged language settings should produce a typed load warning");
    Require(
        loaded.profile_output_directory_source ==
            specforge::ProfileOutputDirectorySource::Environment,
        "environment profile directory should retain explicit precedence");

    const auto result = settings.Apply(
        specforge::ApplicationSettingsIntent::
            SetProfileOutputDirectory(
                temporary.path() / "ignored"));
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
    const specforge::PanelVisibilityState previous =
        settings.panel_visibility();
    settings.panel_visibility().annotations = false;
    settings.panel_visibility().spectral_lines = false;
    settings.CommitPanelVisibilityChange(previous);
    settings.RunMaintenance(
        specforge::LocalUserStateSaveScheduler::Clock::now() + 1s);

    specforge::ApplicationSettings reloaded(storage);
    Require(
        !reloaded.panel_visibility().annotations &&
            !reloaded.panel_visibility().spectral_lines,
        "panel visibility should persist through application settings maintenance");
}

}  // namespace

int main()
{
    TestSettingsIntentsPersistAndReloadThroughOneOwner();
    TestPersistenceFailureRetainsThePreviousValueAndStatus();
    TestLoadWarningAndEnvironmentOverrideAreTyped();
    TestPanelVisibilitySharesTheSettingsLifecycle();
    return 0;
}
