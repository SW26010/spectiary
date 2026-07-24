#include "ui/settings_panel.h"
#include "ui/shell_ui.h"
#include "ui/ui_language_settings.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#ifndef SPECFORGE_EXPECTED_VERSION
#error "SPECFORGE_EXPECTED_VERSION must be provided by the build configuration."
#endif

namespace specforge {

struct SettingsPanelUiTestAccess {
    static void Close(SettingsPanelUi& panel) { panel.open_ = false; }
    static void SetTransientFeedback(SettingsPanelUi& panel, std::string status, bool failed)
    {
        panel.action_status_ = std::move(status);
        panel.action_failed_ = failed;
    }
    static const std::string& TransientFeedback(const SettingsPanelUi& panel) { return panel.action_status_; }
    static bool ActionFailed(const SettingsPanelUi& panel) { return panel.action_failed_; }
    static void SelectSection(SettingsPanelUi& panel, SettingsSection section) { panel.selected_section_ = section; }
    static const SettingsPanelEnvironment& Environment(const SettingsPanelUi& panel)
    {
        return panel.environment_;
    }
    static void ResetProfileOutputDirectory(SettingsPanelUi& panel)
    {
        panel.ResetProfileOutputDirectory();
    }
    static SettingsPanelLanguageFeedbackKind LanguageFeedbackKind(
        const SettingsPanelUi& panel)
    {
        return panel.language_feedback_kind_;
    }
    static const std::string& LanguageFeedbackDetail(
        const SettingsPanelUi& panel)
    {
        return panel.language_feedback_detail_;
    }
};

struct ShellUiTestAccess {
    static std::unique_ptr<ShellUi> Create()
    {
        SourceCollectionSession session(
            [](const std::filesystem::path&, std::size_t)
                -> SpectrumSnapshotHandle {
                return {};
            },
            std::filesystem::path{},
            std::filesystem::path{},
            std::filesystem::path{},
            std::filesystem::path{},
            SourceCollectionSessionRestoreMode::Immediate);
        return std::unique_ptr<ShellUi>(
            new ShellUi(
                std::move(session),
                SourceCollectionLoadQueue{}));
    }

    static void LoadLanguageFrom(
        ShellUi& shell,
        std::filesystem::path path)
    {
        shell.ui_language_settings_path_ = std::move(path);
        shell.LoadCurrentUiLanguage();
    }

    static void ApplyLanguageChange(
        ShellUi& shell,
        UiLanguage language)
    {
        shell.ApplyUiLanguageChange(language);
    }

    static UiLanguage CurrentLanguage(const ShellUi& shell)
    {
        return shell.ui_language_;
    }

    static SettingsPanelUi& SettingsPanel(ShellUi& shell)
    {
        return shell.settings_panel_ui_;
    }
};

}  // namespace specforge

namespace {

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const char* name, const char* value)
        : name_(name)
    {
        char* raw_previous = nullptr;
        std::size_t previous_size = 0;
        if (_dupenv_s(&raw_previous, &previous_size, name_.c_str()) == 0 && raw_previous != nullptr) {
            std::unique_ptr<char, decltype(&std::free)> previous(raw_previous, std::free);
            previous_ = std::string(previous.get());
        }
        Require(_putenv_s(name_.c_str(), value) == 0, "test environment variable should be configurable");
    }

    ~ScopedEnvironmentVariable()
    {
        (void)_putenv_s(name_.c_str(), previous_ ? previous_->c_str() : "");
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
    std::string name_;
    std::optional<std::string> previous_;
};

class ScopedImGuiContext {
public:
    ScopedImGuiContext()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        unsigned char* font_pixels = nullptr;
        int font_width = 0;
        int font_height = 0;
        io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_width, &font_height);
        Require(font_pixels != nullptr && font_width > 0 && font_height > 0, "ImGui font atlas should build");
    }

    ~ScopedImGuiContext()
    {
        ImGui::DestroyContext();
    }

    ScopedImGuiContext(const ScopedImGuiContext&) = delete;
    ScopedImGuiContext& operator=(const ScopedImGuiContext&) = delete;
};

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        const auto suffix = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        path_ = std::filesystem::temp_directory_path() /
                ("specforge-settings-panel-tests-" +
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

struct LanguageRenderObservation {
    bool selector_hovered = false;
    bool simplified_chinese_hovered = false;
    bool popup_open = false;
    ImVec2 popup_content_start;
};

LanguageRenderObservation RenderLanguageFrame(
    specforge::SettingsPanelUi& panel,
    specforge::UiLanguage language)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    ImGui::NewFrame();
    panel.Render(language);

    LanguageRenderObservation observation;
    const ImGuiID hovered_id = GImGui->HoveredId;
    for (ImGuiWindow* window : GImGui->Windows) {
        observation.selector_hovered =
            observation.selector_hovered ||
            hovered_id == window->GetID(
                "Application language###SpecForgeApplicationLanguage");
    }
    observation.popup_open = ImGui::IsPopupOpen(
        nullptr,
        ImGuiPopupFlags_AnyPopupId |
            ImGuiPopupFlags_AnyPopupLevel);
    if (!GImGui->OpenPopupStack.empty()) {
        if (ImGuiWindow* popup_window =
                GImGui->OpenPopupStack.back().Window) {
            observation.popup_content_start =
                popup_window->DC.CursorStartPos;
            observation.simplified_chinese_hovered =
                hovered_id ==
                popup_window->GetID(
                    "Simplified Chinese###SpecForgeUiLanguageSimplifiedChinese");
        }
    }
    ImGui::EndFrame();
    return observation;
}

void TestDefaultEnvironmentDescribesThisBuild()
{
    ScopedEnvironmentVariable profile_directory("SPECFORGE_PROFILE_DIR", "");
    const specforge::SettingsPanelEnvironment environment =
        specforge::DefaultSettingsPanelEnvironment();

    Require(
        environment.version == SPECFORGE_EXPECTED_VERSION,
        "settings should expose the CMake project version");
    Require(!environment.release_profile.empty(), "settings should expose the release profile");
    Require(!environment.data_directory.empty(), "settings should expose the application data directory");
    Require(!environment.log_directory.empty(), "settings should expose the profile output directory");
    Require(
        environment.default_profile_output_directory.parent_path() == environment.data_directory,
        "the default diagnostics directory should remain below application data");
}

void TestEnvironmentOverrideControlsDisplayedLogDirectory()
{
    const std::filesystem::path override_path = "C:/SpecForge/settings-profile-override";
    ScopedEnvironmentVariable profile_directory("SPECFORGE_PROFILE_DIR", override_path.string().c_str());

    const specforge::SettingsPanelEnvironment environment =
        specforge::DefaultSettingsPanelEnvironment();

    Require(
        environment.log_directory == override_path,
        "settings should display the same overridden directory used by ProfileSink");
    Require(
        environment.profile_output_directory_source ==
            specforge::ProfileOutputDirectorySource::Environment,
        "settings should identify an environment-controlled output directory");
}

void TestProfileOutputDirectorySelectionPersistsAndResets()
{
    const std::filesystem::path test_root =
        std::filesystem::temp_directory_path() / "specforge-settings-panel-profile-output";
    const std::filesystem::path settings_path = test_root / "profile-settings.json";
    const std::filesystem::path default_directory = test_root / "default";
    const std::filesystem::path selected_directory =
        test_root / L"selected-\u65E5\u5FD7";
    std::error_code ignored;
    std::filesystem::remove_all(test_root, ignored);

    specforge::SettingsPanelUi panel({
        .version = "test",
        .release_profile = "Portable",
        .data_directory = test_root,
        .log_directory = default_directory,
        .default_profile_output_directory = default_directory,
        .profile_settings_path = settings_path,
    });

    panel.ApplyProfileOutputDirectorySelection(selected_directory);
    Require(
        specforge::SettingsPanelUiTestAccess::Environment(panel).log_directory ==
            selected_directory,
        "choosing a profile output directory should update the settings view");
    Require(
        specforge::LoadProfileSettings(settings_path).output_directory ==
            selected_directory,
        "choosing a profile output directory should persist for future recordings");

    specforge::SettingsPanelUiTestAccess::ResetProfileOutputDirectory(panel);
    Require(
        specforge::SettingsPanelUiTestAccess::Environment(panel).log_directory ==
            default_directory,
        "restoring the default should update the settings view");
    Require(
        !specforge::LoadProfileSettings(settings_path).output_directory,
        "restoring the default should clear the custom directory");

    std::filesystem::remove_all(test_root, ignored);
}

void TestOpenIsIdempotent()
{
    specforge::SettingsPanelUi panel({
        .version = "test",
        .release_profile = "Portable",
        .data_directory = "Data",
        .log_directory = "Data/logs",
    });

    Require(!panel.open(), "settings should start closed");
    panel.Open();
    panel.Open();
    Require(panel.open(), "opening settings repeatedly should retain one open panel");
}

void TestClosedToOpenClearsTransientFeedback()
{
    specforge::SettingsPanelUi panel({
        .version = "test",
        .release_profile = "Portable",
        .data_directory = "Data",
        .log_directory = "Data/logs",
    });

    panel.Open();
    specforge::SettingsPanelUiTestAccess::SetTransientFeedback(panel, "old feedback", true);
    panel.Open();
    Require(
        specforge::SettingsPanelUiTestAccess::TransientFeedback(panel) == "old feedback",
        "refocusing an open panel should preserve current feedback");

    specforge::SettingsPanelUiTestAccess::Close(panel);
    panel.Open();
    Require(
        specforge::SettingsPanelUiTestAccess::TransientFeedback(panel).empty(),
        "reopening a closed panel should clear stale feedback");
    Require(
        !specforge::SettingsPanelUiTestAccess::ActionFailed(panel),
        "reopening a closed panel should clear stale failure state");
}

void TestLanguageSelectorEmitsOneShotIntent()
{
    ScopedImGuiContext imgui;
    specforge::SettingsPanelUi panel({
        .version = "test",
        .release_profile = "Portable",
        .data_directory = "Data",
        .log_directory = "Data/logs",
    });
    specforge::SettingsPanelUiTestAccess::SelectSection(
        panel,
        specforge::SettingsSection::Language);
    panel.Open();

    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    LanguageRenderObservation observation =
        RenderLanguageFrame(
            panel,
            specforge::UiLanguage::English);

    ImVec2 selector_position;
    for (float y = 100.0f;
         y <= 280.0f && !observation.selector_hovered;
         y += 2.0f) {
        selector_position = ImVec2(500.0f, y);
        ImGui::GetIO().AddMousePosEvent(
            selector_position.x,
            selector_position.y);
        observation = RenderLanguageFrame(
            panel,
            specforge::UiLanguage::English);
    }
    Require(
        observation.selector_hovered,
        "integration fixture should locate the application language selector");

    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    observation = RenderLanguageFrame(
        panel,
        specforge::UiLanguage::English);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    observation = RenderLanguageFrame(
        panel,
        specforge::UiLanguage::English);
    Require(
        observation.popup_open,
        "the application language selector should open");

    ImVec2 simplified_chinese_position;
    const float option_x =
        observation.popup_content_start.x + 50.0f;
    for (float y = observation.popup_content_start.y - 10.0f;
         y <= observation.popup_content_start.y + 100.0f &&
         !observation.simplified_chinese_hovered;
         y += 2.0f) {
        simplified_chinese_position = ImVec2(option_x, y);
        ImGui::GetIO().AddMousePosEvent(
            simplified_chinese_position.x,
            simplified_chinese_position.y);
        observation = RenderLanguageFrame(
            panel,
            specforge::UiLanguage::English);
    }
    Require(
        observation.simplified_chinese_hovered,
        "integration fixture should locate the Simplified Chinese option");

    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderLanguageFrame(
        panel,
        specforge::UiLanguage::English);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    (void)RenderLanguageFrame(
        panel,
        specforge::UiLanguage::English);
    Require(
        panel.TakeLanguageChangeRequest() ==
            specforge::UiLanguage::SimplifiedChinese,
        "selecting Simplified Chinese should emit that intent");
    Require(
        !panel.TakeLanguageChangeRequest(),
        "the selected language intent should be consumed once");
}

void TestLanguageRenderKeepsStableImGuiIds()
{
    ScopedImGuiContext imgui;
    specforge::SettingsPanelUi panel({
        .version = "test",
        .release_profile = "Portable",
        .data_directory = "Data",
        .log_directory = "Data/logs",
    });
    specforge::SettingsPanelUiTestAccess::SelectSection(
        panel,
        specforge::SettingsSection::Language);
    panel.Open();

    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    ImGui::NewFrame();
    panel.Render(specforge::UiLanguage::English);
    ImGuiWindow* english_window =
        ImGui::FindWindowByName(
            "Settings###SpecForgeSettingsV1");
    Require(
        english_window != nullptr,
        "English settings render should create the settings window");
    const ImGuiID english_window_id = english_window->ID;
    ImGui::EndFrame();

    ImGui::NewFrame();
    panel.Render(specforge::UiLanguage::SimplifiedChinese);
    ImGuiWindow* chinese_window =
        ImGui::FindWindowByName(
            "设置###SpecForgeSettingsV1");
    Require(
        chinese_window != nullptr,
        "Chinese settings render should reuse the settings window");
    Require(
        chinese_window->ID == english_window_id,
        "English and Chinese settings titles should resolve to the same ImGui window ID");
    ImGui::EndFrame();

    Require(
        ImHashStr(
            "Language###SpecForgeSettingsLanguage") ==
            ImHashStr(
                "语言###SpecForgeSettingsLanguage"),
        "localized Language navigation labels should retain one ImGui ID");
    Require(
        ImHashStr(
            "Application language###SpecForgeApplicationLanguage") ==
            ImHashStr(
                "应用语言###SpecForgeApplicationLanguage"),
        "localized application language labels should retain one ImGui ID");
    Require(
        ImHashStr(
            "English###SpecForgeUiLanguageEnglish") ==
            ImHashStr(
                "英语###SpecForgeUiLanguageEnglish"),
        "localized English options should retain one ImGui ID");
}

void TestShellLanguageChangePersistsAndReloads()
{
    TemporaryDirectory temporary;
    const std::filesystem::path settings_path =
        temporary.path() / "ui-language.json";

    std::unique_ptr<specforge::ShellUi> shell =
        specforge::ShellUiTestAccess::Create();
    specforge::ShellUiTestAccess::LoadLanguageFrom(
        *shell,
        settings_path);
    Require(
        specforge::ShellUiTestAccess::CurrentLanguage(*shell) ==
            specforge::UiLanguage::English,
        "a shell with no language file should start in English");

    specforge::ShellUiTestAccess::ApplyLanguageChange(
        *shell,
        specforge::UiLanguage::SimplifiedChinese);
    Require(
        specforge::ShellUiTestAccess::CurrentLanguage(*shell) ==
            specforge::UiLanguage::SimplifiedChinese,
        "the shell should update its language after a successful save");
    Require(
        specforge::LoadUiLanguageSettings(settings_path).language ==
            specforge::UiLanguage::SimplifiedChinese,
        "the shell language change should be persisted");

    std::unique_ptr<specforge::ShellUi> restarted_shell =
        specforge::ShellUiTestAccess::Create();
    specforge::ShellUiTestAccess::LoadLanguageFrom(
        *restarted_shell,
        settings_path);
    Require(
        specforge::ShellUiTestAccess::CurrentLanguage(
            *restarted_shell) ==
            specforge::UiLanguage::SimplifiedChinese,
        "a restarted shell should restore the saved language");
}

void TestShellSaveFailureRetainsLanguageAndShowsFeedback()
{
    TemporaryDirectory temporary;
    const std::filesystem::path blocker =
        temporary.path() / "not-a-directory";
    {
        std::ofstream stream(blocker);
        stream << "block language settings directory creation";
    }
    const std::filesystem::path settings_path =
        blocker / "ui-language.json";

    std::unique_ptr<specforge::ShellUi> shell =
        specforge::ShellUiTestAccess::Create();
    specforge::ShellUiTestAccess::LoadLanguageFrom(
        *shell,
        settings_path);
    specforge::ShellUiTestAccess::ApplyLanguageChange(
        *shell,
        specforge::UiLanguage::SimplifiedChinese);

    Require(
        specforge::ShellUiTestAccess::CurrentLanguage(*shell) ==
            specforge::UiLanguage::English,
        "a failed save should retain the shell's previous language");
    specforge::SettingsPanelUi& panel =
        specforge::ShellUiTestAccess::SettingsPanel(*shell);
    Require(
        specforge::SettingsPanelUiTestAccess::LanguageFeedbackKind(
            panel) ==
            specforge::SettingsPanelLanguageFeedbackKind::SaveError,
        "a failed save should be exposed as settings feedback");
    Require(
        !specforge::SettingsPanelUiTestAccess::LanguageFeedbackDetail(
             panel)
             .empty(),
        "a failed save should retain a non-empty error detail");
}

void TestShellLoadWarningFallsBackToEnglish()
{
    TemporaryDirectory temporary;
    const std::filesystem::path settings_path =
        temporary.path() / "ui-language.json";
    {
        std::ofstream stream(settings_path);
        stream << R"({"format_kind":)";
    }

    std::unique_ptr<specforge::ShellUi> shell =
        specforge::ShellUiTestAccess::Create();
    specforge::ShellUiTestAccess::LoadLanguageFrom(
        *shell,
        settings_path);

    Require(
        specforge::ShellUiTestAccess::CurrentLanguage(*shell) ==
            specforge::UiLanguage::English,
        "a damaged language file should leave the shell in English");
    specforge::SettingsPanelUi& panel =
        specforge::ShellUiTestAccess::SettingsPanel(*shell);
    Require(
        specforge::SettingsPanelUiTestAccess::LanguageFeedbackKind(
            panel) ==
            specforge::SettingsPanelLanguageFeedbackKind::LoadWarning,
        "a language load warning should be available to the settings page");
    Require(
        !specforge::SettingsPanelUiTestAccess::LanguageFeedbackDetail(
             panel)
             .empty(),
        "a language load warning should retain non-empty detail");
}

void TestRenderSmoke()
{
    ScopedImGuiContext imgui;
    specforge::SettingsPanelUi panel({
        .version = "test",
        .release_profile = "Portable",
        .data_directory = "Data",
        .log_directory = "Data/logs",
    });
    panel.Open();

    constexpr specforge::SettingsSection sections[] = {
        specforge::SettingsSection::General,
        specforge::SettingsSection::Appearance,
        specforge::SettingsSection::Language,
        specforge::SettingsSection::Input,
        specforge::SettingsSection::DataAndRecovery,
        specforge::SettingsSection::Diagnostics,
        specforge::SettingsSection::About,
    };
    for (const specforge::SettingsSection section : sections) {
        specforge::SettingsPanelUiTestAccess::SelectSection(panel, section);
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.DisplaySize = ImVec2(1280.0f, 720.0f);
        ImGui::NewFrame();
        panel.Render(specforge::UiLanguage::English);
        Require(
            ImGui::FindWindowByName("Settings###SpecForgeSettingsV1") != nullptr,
            "rendering an open settings panel should create its ImGui window");
        Require(panel.open(), "rendering every settings section should keep the non-modal panel open");
        ImGui::EndFrame();
    }
}

}  // namespace

int main()
{
    TestDefaultEnvironmentDescribesThisBuild();
    TestEnvironmentOverrideControlsDisplayedLogDirectory();
    TestProfileOutputDirectorySelectionPersistsAndResets();
    TestOpenIsIdempotent();
    TestClosedToOpenClearsTransientFeedback();
    TestLanguageSelectorEmitsOneShotIntent();
    TestLanguageRenderKeepsStableImGuiIds();
    TestShellLanguageChangePersistsAndReloads();
    TestShellSaveFailureRetainsLanguageAndShowsFeedback();
    TestShellLoadWarningFallsBackToEnglish();
    TestRenderSmoke();
    std::cout << "settings panel tests passed\n";
    return 0;
}
