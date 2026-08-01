#include "ui/settings_panel.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

#ifndef SPECFORGE_EXPECTED_VERSION
#error "SPECFORGE_EXPECTED_VERSION must be provided by the build configuration."
#endif

#ifndef SPECFORGE_EXPECTED_SOURCE_MODE
#error "SPECFORGE_EXPECTED_SOURCE_MODE must be provided by the build configuration."
#endif

#ifndef SPECFORGE_EXPECTED_CONFIGURATION
#error "SPECFORGE_EXPECTED_CONFIGURATION must be provided by the build configuration."
#endif

#ifndef SPECFORGE_EXPECTED_ARCHITECTURE
#error "SPECFORGE_EXPECTED_ARCHITECTURE must be provided by the build configuration."
#endif

#ifndef SPECFORGE_EXPECTED_SOURCE_REVISION
#error "SPECFORGE_EXPECTED_SOURCE_REVISION must be provided by the build configuration."
#endif

namespace specforge {

struct SettingsPanelUiTestAccess {
    static void Close(SettingsPanelUi& panel) { panel.open_ = false; }
    static void SetTransientFeedback(
        SettingsPanelUi& panel,
        std::string status,
        bool failed)
    {
        panel.action_status_ = std::move(status);
        panel.action_failed_ = failed;
    }
    static const std::string& TransientFeedback(
        const SettingsPanelUi& panel)
    {
        return panel.action_status_;
    }
    static bool ActionFailed(const SettingsPanelUi& panel)
    {
        return panel.action_failed_;
    }
    static void SelectSection(
        SettingsPanelUi& panel,
        SettingsSection section)
    {
        panel.selected_section_ = section;
    }
    static void SetViewportId(
        SettingsPanelUi& panel,
        unsigned int viewport_id)
    {
        panel.settings_viewport_id_ = viewport_id;
    }
    static void ResetProfileOutputDirectory(SettingsPanelUi& panel)
    {
        panel.ResetProfileOutputDirectory();
    }
    static void SetUiScalePercentage(
        SettingsPanelUi& panel,
        int percentage)
    {
        panel.SetUiScalePercentage(percentage);
    }
    static std::string SectionLabel(
        SettingsSection section,
        UiLanguage language)
    {
        return SettingsPanelUi::SectionLabel(
            section,
            language);
    }
    static std::string AppearanceThemeLabel(
        UiLanguage language)
    {
        return SettingsPanelUi::
            AppearanceThemeLabel(language);
    }
    static std::string AppearanceAccentColorLabel(
        UiLanguage language)
    {
        return SettingsPanelUi::
            AppearanceAccentColorLabel(language);
    }
    static float VisibleLabelWidth(std::string_view label)
    {
        return SettingsPanelUi::VisibleLabelWidth(label);
    }
    static bool ShouldSubmitLanguageSelection(
        const ApplicationSettingsView& settings,
        UiLanguage candidate)
    {
        return SettingsPanelUi::
            ShouldSubmitLanguageSelection(
                settings,
                candidate);
    }
    static bool CanRestoreProfileOutputDirectory(
        const ApplicationSettingsView& settings,
        const SettingsPanelStatus& status = {})
    {
        return SettingsPanelUi::
            CanRestoreProfileOutputDirectory(
                settings,
                status);
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
        io.Fonts->GetTexDataAsRGBA32(
            &font_pixels,
            &font_width,
            &font_height);
        Require(
            font_pixels != nullptr &&
                font_width > 0 &&
                font_height > 0,
            "ImGui font atlas should build");
    }

    ~ScopedImGuiContext()
    {
        ImGui::DestroyContext();
    }

    ScopedImGuiContext(const ScopedImGuiContext&) = delete;
    ScopedImGuiContext& operator=(
        const ScopedImGuiContext&) = delete;
};

specforge::SettingsPanelUi MakePanel()
{
    return specforge::SettingsPanelUi({
        .version = "test",
        .distribution = "Portable",
        .configuration = "Debug",
        .target_architecture = "amd64",
        .build_source = {
            .mode = "working_tree",
            .revision = "",
        },
        .data_directory = "Data",
    });
}

specforge::ApplicationSettingsView MakeSettingsView(
    specforge::UiLanguage language =
        specforge::UiLanguage::English)
{
    return {
        .language = language,
        .profile_output_directory = "Data/logs",
        .default_profile_output_directory = "Data/logs",
    };
}

struct LanguageRenderObservation {
    bool selector_hovered = false;
    bool simplified_chinese_hovered = false;
    bool popup_open = false;
    ImVec2 popup_content_start;
};

struct UiScaleRenderObservation {
    bool slider_hovered = false;
    bool slider_active = false;
    bool reset_hovered = false;
};

struct InputRenderObservation {
    bool live_numeric_navigation_hovered = false;
};

LanguageRenderObservation RenderLanguageFrame(
    specforge::SettingsPanelUi& panel,
    specforge::UiLanguage language)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    ImGui::NewFrame();
    panel.Render(MakeSettingsView(language));

    LanguageRenderObservation observation;
    const ImGuiID hovered_id = GImGui->HoveredId;
    for (ImGuiWindow* window : GImGui->Windows) {
        observation.selector_hovered =
            observation.selector_hovered ||
            hovered_id == window->GetID(
                "Application language###"
                "SpecForgeApplicationLanguage");
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
                    "Simplified Chinese###"
                    "SpecForgeUiLanguageSimplifiedChinese");
        }
    }
    ImGui::EndFrame();
    return observation;
}

UiScaleRenderObservation RenderUiScaleFrame(
    specforge::SettingsPanelUi& panel,
    int percentage = 100,
    specforge::UiLanguage language =
        specforge::UiLanguage::English)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    ImGui::NewFrame();
    specforge::ApplicationSettingsView settings =
        MakeSettingsView(language);
    settings.ui_scale_percentage = percentage;
    panel.Render(settings);

    UiScaleRenderObservation observation;
    for (ImGuiWindow* window : GImGui->Windows) {
        const ImGuiID slider_id =
            window->GetID(
                "UI scale###SpecForgeUiScale");
        observation.slider_hovered =
            observation.slider_hovered ||
            GImGui->HoveredId == slider_id;
        observation.slider_active =
            observation.slider_active ||
            GImGui->ActiveId == slider_id;
        observation.reset_hovered =
            observation.reset_hovered ||
            GImGui->HoveredId ==
                window->GetID(
                    "Reset###SpecForgeUiScaleReset");
    }
    ImGui::EndFrame();
    return observation;
}

InputRenderObservation RenderInputFrame(
    specforge::SettingsPanelUi& panel,
    bool live_numeric_navigation = true)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    ImGui::NewFrame();
    specforge::ApplicationSettingsView settings =
        MakeSettingsView();
    settings.live_numeric_navigation =
        live_numeric_navigation;
    panel.Render(settings);

    InputRenderObservation observation;
    for (ImGuiWindow* window : GImGui->Windows) {
        observation.live_numeric_navigation_hovered =
            observation.live_numeric_navigation_hovered ||
            GImGui->HoveredId == window->GetID(
                "Live numeric navigation###"
                "SpecForgeLiveNumericNavigation");
    }
    ImGui::EndFrame();
    return observation;
}

void TestDefaultEnvironmentDescribesThisBuild()
{
    const specforge::SettingsPanelEnvironment environment =
        specforge::SettingsPanelEnvironmentForStartup(
            specforge::DefaultSpecForgeStartup());

    Require(
        environment.version == SPECFORGE_EXPECTED_VERSION,
        "settings should expose the CMake project version");
    Require(
        !environment.distribution.empty(),
        "settings should expose the distribution");
    Require(
        environment.configuration == SPECFORGE_EXPECTED_CONFIGURATION,
        "settings should expose the actual build configuration");
    Require(
        environment.target_architecture == SPECFORGE_EXPECTED_ARCHITECTURE,
        "settings should expose the target architecture");
    Require(
        environment.build_source.mode ==
            SPECFORGE_EXPECTED_SOURCE_MODE,
        "settings should expose the configured build source mode");
    Require(
        environment.build_source.revision ==
            SPECFORGE_EXPECTED_SOURCE_REVISION,
        "settings should expose the configured build source revision");
    Require(
        !environment.data_directory.empty(),
        "settings should expose the application data directory");
}

void TestWorkingTreeBuildSourcePresentation()
{
    const specforge::SettingsPanelEnvironment environment = {
        .version = "test-version",
        .distribution = "Portable",
        .configuration = "Debug",
        .target_architecture = "amd64",
        .build_source = {
            .mode = "working_tree",
            .revision = "",
        },
        .data_directory = "Data",
    };

    Require(
        specforge::FormatBuildSourceForAbout(
            environment.build_source) ==
            "Source: Working tree",
        "working-tree About text should identify the working tree");

    const std::string diagnostics =
        specforge::FormatDiagnosticInformation(
            environment,
            "Data/logs");
    Require(
        diagnostics ==
            "SpecForge test-version\n"
            "Distribution: Portable\n"
            "Source mode: working_tree\n"
            "Graphics: Direct3D 11 / SDR\n"
            "Data directory: Data\n"
            "Log directory: Data/logs",
        "working-tree diagnostics should include the mode "
        "without a source revision");
}

void TestHeadBuildSourcePresentation()
{
    constexpr const char kRevision[] =
        "0123456789abcdef0123456789abcdef01234567";
    const specforge::SettingsPanelEnvironment environment = {
        .version = "test-version",
        .distribution = "Portable",
        .configuration = "Debug",
        .target_architecture = "amd64",
        .build_source = {
            .mode = "head",
            .revision = kRevision,
        },
        .data_directory = "Data",
    };

    Require(
        specforge::FormatBuildSourceForAbout(
            environment.build_source) ==
            "Source: HEAD 0123456789ab",
        "HEAD About text should use the 12-character revision");

    const std::string diagnostics =
        specforge::FormatDiagnosticInformation(
            environment,
            "Data/logs");
    Require(
        diagnostics ==
            "SpecForge test-version\n"
            "Distribution: Portable\n"
            "Source mode: head\n"
            "Source revision: "
            "0123456789abcdef0123456789abcdef01234567\n"
            "Graphics: Direct3D 11 / SDR\n"
            "Data directory: Data\n"
            "Log directory: Data/logs",
        "HEAD diagnostics should include the mode and full revision");
}

void TestChineseBuildAndDiagnosticsPresentation()
{
    const specforge::SettingsPanelEnvironment environment = {
        .version = "test-version",
        .distribution = "Portable",
        .configuration = "Debug",
        .target_architecture = "amd64",
        .build_source = {
            .mode = "working_tree",
            .revision = "",
        },
        .data_directory = "Data",
    };

    Require(
        specforge::FormatBuildSourceForAbout(
            environment.build_source,
            specforge::UiLanguage::
                SimplifiedChinese) ==
            "源码：工作树",
        "Chinese About source text should be exact");
    Require(
        specforge::FormatBuildMetadataStatusForAbout(
            specforge::BuildMetadataStatus::Mismatch,
            specforge::UiLanguage::
                SimplifiedChinese) ==
            "构建元数据不匹配",
        "Chinese build metadata status should be exact");
    Require(
        specforge::FormatProfileOutputDirectoryStatus(
            specforge::ApplicationSettingsStatusKind::
                PersistenceError,
            specforge::UiLanguage::
                SimplifiedChinese) ==
            "无法保存性能分析输出目录。",
        "Chinese profile persistence status should be exact");
    Require(
        specforge::FormatApplicationSettingsStatusReason(
            specforge::ApplicationSettingsStatusReason::
                SavedValueUnreadable,
            specforge::UiLanguage::
                SimplifiedChinese) ==
            "已保存的值无效或无法读取。",
        "Chinese settings failure reason should be exact");
    Require(
        specforge::FormatApplicationSettingsStatusReason(
            specforge::ApplicationSettingsStatusReason::
                SettingsWriteFailed,
            specforge::UiLanguage::
                SimplifiedChinese) ==
            "无法写入设置文件。",
        "Chinese settings persistence reason should be exact");

    const std::string diagnostics =
        specforge::FormatDiagnosticInformation(
            environment,
            "Data/logs",
            specforge::UiLanguage::
                SimplifiedChinese);
    Require(
        diagnostics ==
            "SpecForge test-version\n"
            "分发方式：Portable\n"
            "源码模式：working_tree\n"
            "图形：Direct3D 11 / SDR\n"
            "数据目录：Data\n"
            "日志目录：Data/logs",
        "copied diagnostics should use localized labels");
}

void TestBuildMetadataStatusPresentation()
{
    Require(
        specforge::FormatBuildMetadataStatusForAbout(
            specforge::BuildMetadataStatus::Available).empty(),
        "available metadata should not render a fallback status");
    Require(
        specforge::FormatBuildMetadataStatusForAbout(
            specforge::BuildMetadataStatus::Unavailable) ==
            "Build metadata unavailable",
        "unavailable metadata should render its fallback status");
    Require(
        specforge::FormatBuildMetadataStatusForAbout(
            specforge::BuildMetadataStatus::Mismatch) ==
            "Build metadata mismatch",
        "mismatched metadata should render its fallback status");
}

void TestOpenIsIdempotent()
{
    specforge::SettingsPanelUi panel = MakePanel();

    Require(!panel.open(), "settings should start closed");
    panel.Open();
    panel.Open();
    Require(
        panel.open(),
        "opening settings repeatedly should retain one open panel");
}

void TestClosedToOpenClearsTransientFeedback()
{
    specforge::SettingsPanelUi panel = MakePanel();

    panel.Open();
    specforge::SettingsPanelUiTestAccess::SetTransientFeedback(
        panel,
        "old feedback",
        true);
    panel.Open();
    Require(
        specforge::SettingsPanelUiTestAccess::
            TransientFeedback(panel) == "old feedback",
        "refocusing an open panel should preserve current feedback");

    specforge::SettingsPanelUiTestAccess::Close(panel);
    panel.Open();
    Require(
        specforge::SettingsPanelUiTestAccess::
            TransientFeedback(panel).empty(),
        "reopening a closed panel should clear stale feedback");
    Require(
        !specforge::SettingsPanelUiTestAccess::
            ActionFailed(panel),
        "reopening a closed panel should clear stale failure state");
}

void TestProfileResetEmitsOneShotSettingsIntent()
{
    specforge::SettingsPanelUi panel = MakePanel();
    specforge::SettingsPanelUiTestAccess::
        ResetProfileOutputDirectory(panel);

    const std::optional<specforge::ApplicationSettingsIntent> intent =
        panel.TakeApplicationSettingsIntent();
    Require(
        intent &&
            intent->kind ==
                specforge::ApplicationSettingsIntentKind::
                    RestoreDefaultProfileOutputDirectory,
        "profile reset should emit an application-settings intent");
    Require(
        !panel.TakeApplicationSettingsIntent(),
        "profile reset intent should be consumed once");
}

void TestWarnedFallbacksRemainDirectlyRepairable()
{
    specforge::ApplicationSettingsView settings =
        MakeSettingsView();
    settings.statuses[static_cast<std::size_t>(
        specforge::ApplicationSetting::Language)] = {
        .kind =
            specforge::ApplicationSettingsStatusKind::
                LoadWarning,
        .setting = specforge::ApplicationSetting::Language,
    };
    settings.statuses[static_cast<std::size_t>(
        specforge::ApplicationSetting::
            ProfileOutputDirectory)] = {
        .kind =
            specforge::ApplicationSettingsStatusKind::
                LoadWarning,
        .setting =
            specforge::ApplicationSetting::
                ProfileOutputDirectory,
    };

    Require(
        specforge::SettingsPanelUiTestAccess::
            ShouldSubmitLanguageSelection(
                settings,
                specforge::UiLanguage::English),
        "the selected warned language fallback should remain directly selectable for repair");
    Require(
        specforge::SettingsPanelUiTestAccess::
            CanRestoreProfileOutputDirectory(settings),
        "the warned default profile fallback should keep Restore Default enabled");
    Require(
        specforge::FormatProfileOutputDirectoryStatus(
            specforge::ApplicationSettingsStatusKind::
                LoadWarning)
                .find("could not be loaded") !=
            std::string_view::npos,
        "profile load warnings should describe fallback loading rather than an update failure");
    Require(
        specforge::FormatProfileOutputDirectoryStatus(
            specforge::ApplicationSettingsStatusKind::
                PersistenceError)
                .find("could not be saved") !=
            std::string_view::npos,
        "profile persistence failures should retain distinct save wording");
}

void TestUiScaleControlEmitsOneShotSettingsIntent()
{
    specforge::SettingsPanelUi panel = MakePanel();
    specforge::SettingsPanelUiTestAccess::
        SetUiScalePercentage(panel, 125);

    const std::optional<specforge::ApplicationSettingsIntent> intent =
        panel.TakeApplicationSettingsIntent();
    Require(
        intent &&
            intent->kind ==
                specforge::ApplicationSettingsIntentKind::
                    SetUiScale &&
            intent->ui_scale_percentage == 125,
        "UI scale control should emit the selected percentage");
    Require(
        !panel.TakeApplicationSettingsIntent(),
        "UI scale intent should be consumed once");

    specforge::SettingsPanelUiTestAccess::
        SetUiScalePercentage(panel, 100);
    const std::optional<specforge::ApplicationSettingsIntent>
        reset_intent = panel.TakeApplicationSettingsIntent();
    Require(
        reset_intent &&
            reset_intent->kind ==
                specforge::ApplicationSettingsIntentKind::
                    SetUiScale &&
            reset_intent->ui_scale_percentage == 100,
        "UI scale reset should emit 100%");
}

void TestLiveNumericNavigationCheckboxEmitsOneShotSettingsIntent()
{
    ScopedImGuiContext imgui;
    specforge::SettingsPanelUi panel = MakePanel();
    specforge::SettingsPanelUiTestAccess::SelectSection(
        panel,
        specforge::SettingsSection::Input);
    panel.Open();

    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    InputRenderObservation observation =
        RenderInputFrame(panel);
    ImVec2 checkbox_position;
    for (float y = 80.0f;
         y <= 900.0f &&
         !observation.live_numeric_navigation_hovered;
         y += 2.0f) {
        for (float x = 300.0f;
             x <= 1300.0f &&
             !observation.live_numeric_navigation_hovered;
             x += 40.0f) {
            checkbox_position = ImVec2(x, y);
            ImGui::GetIO().AddMousePosEvent(
                checkbox_position.x,
                checkbox_position.y);
            observation = RenderInputFrame(panel);
        }
    }
    Require(
        observation.live_numeric_navigation_hovered,
        "fixture should locate the checked live numeric navigation checkbox");

    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderInputFrame(panel);
    Require(
        !panel.TakeApplicationSettingsIntent(),
        "pressing the checkbox should wait for click release");
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    (void)RenderInputFrame(panel);

    const std::optional<specforge::ApplicationSettingsIntent> intent =
        panel.TakeApplicationSettingsIntent();
    Require(
        intent &&
            intent->kind ==
                specforge::ApplicationSettingsIntentKind::
                    SetLiveNumericNavigation &&
            !intent->live_numeric_navigation,
        "clicking the checked live numeric navigation checkbox should emit the disabled setting");
    Require(
        !panel.TakeApplicationSettingsIntent(),
        "live numeric navigation intent should be consumed once");
}

void TestUiScaleSliderCommitsOnlyAfterEditDeactivation()
{
    ScopedImGuiContext imgui;
    specforge::SettingsPanelUi panel = MakePanel();
    specforge::SettingsPanelUiTestAccess::SelectSection(
        panel,
        specforge::SettingsSection::Appearance);
    panel.Open();

    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    UiScaleRenderObservation observation =
        RenderUiScaleFrame(panel);

    ImVec2 slider_position;
    for (float y = 220.0f;
         y <= 780.0f && !observation.slider_hovered;
         y += 2.0f) {
        for (float x = 520.0f;
             x <= 1080.0f &&
             !observation.slider_hovered;
             x += 80.0f) {
            slider_position = ImVec2(x, y);
            ImGui::GetIO().AddMousePosEvent(
                slider_position.x,
                slider_position.y);
            observation = RenderUiScaleFrame(panel);
        }
    }
    Require(
        observation.slider_hovered,
        "fixture should locate the UI scale slider");

    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    observation = RenderUiScaleFrame(panel);
    Require(
        observation.slider_active,
        "pressing the UI scale slider should begin an active edit");
    Require(
        !panel.TakeApplicationSettingsIntent(),
        "pressing the active slider should not submit a setting");

    ImGui::GetIO().AddMousePosEvent(
        slider_position.x + 120.0f,
        slider_position.y);
    observation = RenderUiScaleFrame(
        panel,
        100,
        specforge::UiLanguage::SimplifiedChinese);
    Require(
        observation.slider_active,
        "dragging should keep the UI scale slider active");
    Require(
        !panel.TakeApplicationSettingsIntent(),
        "dragging should retain only a panel-local draft");

    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    observation = RenderUiScaleFrame(
        panel,
        100,
        specforge::UiLanguage::SimplifiedChinese);
    Require(
        !observation.slider_active,
        "releasing should deactivate the UI scale slider");

    const std::optional<specforge::ApplicationSettingsIntent> intent =
        panel.TakeApplicationSettingsIntent();
    Require(
        intent &&
            intent->kind ==
                specforge::ApplicationSettingsIntentKind::
                    SetUiScale &&
            intent->ui_scale_percentage > 100 &&
            intent->ui_scale_percentage <= 150,
        "releasing an edited slider should submit its final draft once");
    Require(
        !panel.TakeApplicationSettingsIntent(),
        "the released UI scale intent should be consumed once");
}

void TestLocalizedUiScaleResetEmitsDefaultIntent()
{
    ScopedImGuiContext imgui;
    specforge::SettingsPanelUi panel = MakePanel();
    specforge::SettingsPanelUiTestAccess::SelectSection(
        panel,
        specforge::SettingsSection::Appearance);
    panel.Open();

    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    UiScaleRenderObservation observation =
        RenderUiScaleFrame(
            panel,
            125,
            specforge::UiLanguage::SimplifiedChinese);

    float slider_y = 0.0f;
    for (float y = 220.0f;
         y <= 780.0f && !observation.slider_hovered;
         y += 2.0f) {
        for (float x = 520.0f;
             x <= 1080.0f &&
             !observation.slider_hovered;
             x += 80.0f) {
            slider_y = y;
            ImGui::GetIO().AddMousePosEvent(x, y);
            observation = RenderUiScaleFrame(
                panel,
                125,
                specforge::UiLanguage::
                    SimplifiedChinese);
        }
    }
    Require(
        observation.slider_hovered,
        "fixture should locate the localized UI scale row");

    ImVec2 reset_position;
    for (float x = 600.0f;
         x <= 1150.0f && !observation.reset_hovered;
         x += 2.0f) {
        reset_position = ImVec2(x, slider_y);
        ImGui::GetIO().AddMousePosEvent(
            reset_position.x,
            reset_position.y);
        observation = RenderUiScaleFrame(
            panel,
            125,
            specforge::UiLanguage::SimplifiedChinese);
    }
    Require(
        observation.reset_hovered,
        "fixture should locate the localized UI scale reset action");

    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderUiScaleFrame(
        panel,
        125,
        specforge::UiLanguage::SimplifiedChinese);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    (void)RenderUiScaleFrame(
        panel,
        125,
        specforge::UiLanguage::SimplifiedChinese);

    const std::optional<specforge::ApplicationSettingsIntent> intent =
        panel.TakeApplicationSettingsIntent();
    Require(
        intent &&
            intent->kind ==
                specforge::ApplicationSettingsIntentKind::
                    SetUiScale &&
            intent->ui_scale_percentage == 100,
        "localized UI scale reset should emit 100%");
    Require(
        !panel.TakeApplicationSettingsIntent(),
        "localized UI scale reset should be consumed once");
}

void TestLanguageSelectorEmitsOneShotIntent()
{
    ScopedImGuiContext imgui;
    specforge::SettingsPanelUi panel = MakePanel();
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
    for (float y = 80.0f;
         y <= 650.0f && !observation.selector_hovered;
         y += 2.0f) {
        for (float x = 360.0f;
             x <= 1000.0f &&
             !observation.selector_hovered;
             x += 80.0f) {
            selector_position = ImVec2(x, y);
            ImGui::GetIO().AddMousePosEvent(
                selector_position.x,
                selector_position.y);
            observation = RenderLanguageFrame(
                panel,
                specforge::UiLanguage::English);
        }
    }
    Require(
        observation.selector_hovered,
        "fixture should locate the application language selector");

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
    for (float y =
             observation.popup_content_start.y - 10.0f;
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
        "fixture should locate the Simplified Chinese option");

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

    const std::optional<specforge::ApplicationSettingsIntent> intent =
        panel.TakeApplicationSettingsIntent();
    Require(
        intent &&
            intent->kind ==
                specforge::ApplicationSettingsIntentKind::
                    SetLanguage &&
            intent->language ==
                specforge::UiLanguage::SimplifiedChinese,
        "selecting Simplified Chinese should emit that settings intent");
    Require(
        !panel.TakeApplicationSettingsIntent(),
        "the selected language intent should be consumed once");
}

void TestLanguageRenderKeepsStableImGuiIds()
{
    ScopedImGuiContext imgui;
    specforge::SettingsPanelUi panel = MakePanel();
    specforge::SettingsPanelUiTestAccess::SelectSection(
        panel,
        specforge::SettingsSection::Language);
    panel.Open();

    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    ImGui::NewFrame();
    panel.Render(
        MakeSettingsView(specforge::UiLanguage::English));
    ImGuiWindow* english_window =
        ImGui::FindWindowByName(
            "Settings###SpecForgeSettingsV1");
    Require(
        english_window != nullptr,
        "English render should create the settings window");
    const ImGuiID english_window_id = english_window->ID;
    ImGui::EndFrame();

    ImGui::NewFrame();
    panel.Render(
        MakeSettingsView(
            specforge::UiLanguage::SimplifiedChinese));
    ImGuiWindow* chinese_window =
        ImGui::FindWindowByName(
            "设置###SpecForgeSettingsV1");
    Require(
        chinese_window != nullptr &&
            chinese_window->ID == english_window_id,
        "localized titles should retain one ImGui window ID");
    ImGui::EndFrame();

    Require(
        ImHashStr(
            "Language###SpecForgeSettingsLanguage") ==
            ImHashStr(
                "语言###SpecForgeSettingsLanguage"),
        "localized Language labels should retain one ImGui ID");
    const std::string appearance_english =
        specforge::SettingsPanelUiTestAccess::SectionLabel(
            specforge::SettingsSection::Appearance,
            specforge::UiLanguage::English);
    const std::string appearance_chinese =
        specforge::SettingsPanelUiTestAccess::SectionLabel(
            specforge::SettingsSection::Appearance,
            specforge::UiLanguage::SimplifiedChinese);
    Require(
        appearance_english ==
                "Appearance###SpecForgeSettingsAppearance" &&
            appearance_chinese ==
                "外观###SpecForgeSettingsAppearance",
        "Appearance navigation should use the production stable suffix");
    Require(
        ImHashStr(appearance_english.c_str()) ==
            ImHashStr(appearance_chinese.c_str()),
        "localized Appearance labels should retain one ImGui ID");
    const std::string data_and_recovery_english =
        specforge::SettingsPanelUiTestAccess::SectionLabel(
            specforge::SettingsSection::DataAndRecovery,
            specforge::UiLanguage::English);
    constexpr std::string_view kVisibleDataAndRecovery =
        "Data & Recovery";
    const float visible_width =
        specforge::SettingsPanelUiTestAccess::
            VisibleLabelWidth(data_and_recovery_english);
    Require(
        visible_width ==
            ImGui::CalcTextSize(
                kVisibleDataAndRecovery.data(),
                kVisibleDataAndRecovery.data() +
                    kVisibleDataAndRecovery.size())
                .x,
        "navigation width should measure only the visible label text");
    Require(
        visible_width <
            ImGui::CalcTextSize(
                data_and_recovery_english.c_str())
                .x,
        "navigation width should exclude the stable ID suffix");
    constexpr std::array kRemainingSections = {
        specforge::SettingsSection::General,
        specforge::SettingsSection::Input,
        specforge::SettingsSection::DataAndRecovery,
        specforge::SettingsSection::Diagnostics,
        specforge::SettingsSection::About,
    };
    for (const specforge::SettingsSection section :
         kRemainingSections) {
        const std::string english =
            specforge::SettingsPanelUiTestAccess::
                SectionLabel(
                    section,
                    specforge::UiLanguage::English);
        const std::string chinese =
            specforge::SettingsPanelUiTestAccess::
                SectionLabel(
                    section,
                    specforge::UiLanguage::
                        SimplifiedChinese);
        Require(
            ImHashStr(english.c_str()) ==
                ImHashStr(chinese.c_str()),
            "localized settings sections should retain stable ImGui IDs");
    }

    const std::string theme_english =
        specforge::SettingsPanelUiTestAccess::
            AppearanceThemeLabel(
                specforge::UiLanguage::English);
    const std::string theme_chinese =
        specforge::SettingsPanelUiTestAccess::
            AppearanceThemeLabel(
                specforge::UiLanguage::SimplifiedChinese);
    Require(
        theme_english ==
                "Theme###SpecForgeAppearanceTheme" &&
            theme_chinese ==
                "主题###SpecForgeAppearanceTheme",
        "theme controls should use the production stable suffix");
    Require(
        ImHashStr(theme_english.c_str()) ==
            ImHashStr(theme_chinese.c_str()),
        "localized theme controls should retain one ImGui ID");

    const std::string accent_english =
        specforge::SettingsPanelUiTestAccess::
            AppearanceAccentColorLabel(
                specforge::UiLanguage::English);
    const std::string accent_chinese =
        specforge::SettingsPanelUiTestAccess::
            AppearanceAccentColorLabel(
                specforge::UiLanguage::SimplifiedChinese);
    Require(
        accent_english ==
                "Accent color###"
                "SpecForgeAppearanceAccentColor" &&
            accent_chinese ==
                "强调色###"
                "SpecForgeAppearanceAccentColor",
        "accent color controls should use the production stable suffix");
    Require(
        ImHashStr(accent_english.c_str()) ==
            ImHashStr(accent_chinese.c_str()),
        "localized accent color controls should retain one ImGui ID");
    Require(
        ImHashStr(
            "UI scale###SpecForgeUiScale") ==
            ImHashStr(
                "界面缩放###SpecForgeUiScale"),
        "localized UI scale controls should retain one ImGui ID");
    Require(
        ImHashStr(
            "Reset###SpecForgeUiScaleReset") ==
            ImHashStr(
                "重置###SpecForgeUiScaleReset"),
        "localized reset actions should retain one ImGui ID");
    Require(
        ImHashStr(
            "Application language###"
            "SpecForgeApplicationLanguage") ==
            ImHashStr(
                "应用语言###"
                "SpecForgeApplicationLanguage"),
        "localized language selectors should retain one ImGui ID");
    Require(
        ImHashStr(
            "English###SpecForgeUiLanguageEnglish") ==
            ImHashStr(
                "英语###SpecForgeUiLanguageEnglish"),
        "localized language options should retain one ImGui ID");
}

void TestRenderSmoke()
{
    ScopedImGuiContext imgui;
    specforge::SettingsPanelUi panel = MakePanel();
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
        specforge::SettingsPanelUiTestAccess::SelectSection(
            panel,
            section);
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.DisplaySize = ImVec2(1280.0f, 720.0f);
        ImGui::NewFrame();
        panel.Render(MakeSettingsView());
        Require(
            ImGui::FindWindowByName(
                "Settings###SpecForgeSettingsV1") != nullptr,
            "rendering an open settings panel should create its window");
        Require(
            panel.open(),
            "rendering every section should keep the panel open");
        ImGui::EndFrame();
    }
}

void TestSettingsWindowMinimumSizeTracksUiScale()
{
    ScopedImGuiContext imgui;
    specforge::SettingsPanelUi panel = MakePanel();
    panel.Open();

    specforge::ApplicationSettingsView settings =
        MakeSettingsView();
    settings.ui_scale_percentage = 150;

    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    ImGui::NewFrame();
    panel.Render(settings);
    ImGuiWindow* window = ImGui::FindWindowByName(
        "Settings###SpecForgeSettingsV1");
    Require(
        window != nullptr &&
            window->Size.x >= 1290.0f &&
            window->Size.y >= 840.0f,
        "150% UI scale should enlarge the Settings window minimum size");
    ImGui::EndFrame();
}

void TestSettingsWindowConstraintsFollowCurrentViewport()
{
    ScopedImGuiContext imgui;
    specforge::SettingsPanelUi panel = MakePanel();
    panel.Open();

    specforge::ApplicationSettingsView settings =
        MakeSettingsView();
    settings.ui_scale_percentage = 150;

    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    ImGui::NewFrame();
    panel.Render(settings);
    ImGuiWindow* window = ImGui::FindWindowByName(
        "Settings###SpecForgeSettingsV1");
    Require(
        window != nullptr &&
            window->Size.x >= 1290.0f &&
            window->Size.y >= 840.0f,
        "fixture should begin with the 150% main-viewport size");
    ImGui::EndFrame();

    ImGuiViewportP secondary_viewport;
    secondary_viewport.ID =
        ImHashStr("SpecForgeSettingsSecondaryViewport");
    secondary_viewport.Pos =
        ImVec2(2000.0f, 100.0f);
    secondary_viewport.Size =
        ImVec2(700.0f, 500.0f);
    secondary_viewport.WorkPos =
        secondary_viewport.Pos;
    secondary_viewport.WorkSize =
        secondary_viewport.Size;
    secondary_viewport.DpiScale = 1.0f;
    secondary_viewport.Idx = GImGui->Viewports.Size;

    ImGui::NewFrame();
    GImGui->Viewports.push_back(
        &secondary_viewport);
    specforge::SettingsPanelUiTestAccess::SetViewportId(
        panel,
        secondary_viewport.ID);
    panel.Render(settings);
    Require(
        window->Size.x <=
                secondary_viewport.WorkSize.x &&
            window->Size.y <=
                secondary_viewport.WorkSize.y,
        "Settings constraints should fit its current viewport work area");
    ImGui::EndFrame();
    GImGui->Viewports.pop_back();
}

}  // namespace

int main()
{
    TestDefaultEnvironmentDescribesThisBuild();
        TestWorkingTreeBuildSourcePresentation();
        TestHeadBuildSourcePresentation();
        TestChineseBuildAndDiagnosticsPresentation();
    TestBuildMetadataStatusPresentation();
    TestOpenIsIdempotent();
    TestClosedToOpenClearsTransientFeedback();
    TestProfileResetEmitsOneShotSettingsIntent();
    TestWarnedFallbacksRemainDirectlyRepairable();
    TestUiScaleControlEmitsOneShotSettingsIntent();
    TestLiveNumericNavigationCheckboxEmitsOneShotSettingsIntent();
    TestUiScaleSliderCommitsOnlyAfterEditDeactivation();
    TestLocalizedUiScaleResetEmitsDefaultIntent();
    TestLanguageSelectorEmitsOneShotIntent();
    TestLanguageRenderKeepsStableImGuiIds();
    TestRenderSmoke();
    TestSettingsWindowMinimumSizeTracksUiScale();
    TestSettingsWindowConstraintsFollowCurrentViewport();
    std::cout << "settings panel tests passed\n";
    return 0;
}
