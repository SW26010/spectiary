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
        .release_profile = "Portable",
        .configuration = "Debug",
        .target_architecture = "x64",
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
    int percentage = 100)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    ImGui::NewFrame();
    specforge::ApplicationSettingsView settings =
        MakeSettingsView();
    settings.ui_scale_percentage = percentage;
    panel.Render(settings);

    UiScaleRenderObservation observation;
    for (ImGuiWindow* window : GImGui->Windows) {
        const ImGuiID slider_id =
            window->GetID("UI scale");
        observation.slider_hovered =
            observation.slider_hovered ||
            GImGui->HoveredId == slider_id;
        observation.slider_active =
            observation.slider_active ||
            GImGui->ActiveId == slider_id;
    }
    ImGui::EndFrame();
    return observation;
}

void TestDefaultEnvironmentDescribesThisBuild()
{
    const specforge::SettingsPanelEnvironment environment =
        specforge::DefaultSettingsPanelEnvironment();

    Require(
        environment.version == SPECFORGE_EXPECTED_VERSION,
        "settings should expose the CMake project version");
    Require(
        !environment.release_profile.empty(),
        "settings should expose the release profile");
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
        .release_profile = "Portable",
        .configuration = "Debug",
        .target_architecture = "x64",
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
            "Release profile: Portable\n"
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
        .release_profile = "Portable",
        .configuration = "Debug",
        .target_architecture = "x64",
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
            "Release profile: Portable\n"
            "Source mode: head\n"
            "Source revision: "
            "0123456789abcdef0123456789abcdef01234567\n"
            "Graphics: Direct3D 11 / SDR\n"
            "Data directory: Data\n"
            "Log directory: Data/logs",
        "HEAD diagnostics should include the mode and full revision");
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

    ImVec2 slider_position(700.0f, 0.0f);
    for (float y = 260.0f;
         y <= 520.0f && !observation.slider_hovered;
         y += 2.0f) {
        slider_position.y = y;
        ImGui::GetIO().AddMousePosEvent(
            slider_position.x,
            slider_position.y);
        observation = RenderUiScaleFrame(panel);
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
    observation = RenderUiScaleFrame(panel);
    Require(
        observation.slider_active,
        "dragging should keep the UI scale slider active");
    Require(
        !panel.TakeApplicationSettingsIntent(),
        "dragging should retain only a panel-local draft");

    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    observation = RenderUiScaleFrame(panel);
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
    TestBuildMetadataStatusPresentation();
    TestOpenIsIdempotent();
    TestClosedToOpenClearsTransientFeedback();
    TestProfileResetEmitsOneShotSettingsIntent();
    TestUiScaleControlEmitsOneShotSettingsIntent();
    TestUiScaleSliderCommitsOnlyAfterEditDeactivation();
    TestLanguageSelectorEmitsOneShotIntent();
    TestLanguageRenderKeepsStableImGuiIds();
    TestRenderSmoke();
    TestSettingsWindowMinimumSizeTracksUiScale();
    TestSettingsWindowConstraintsFollowCurrentViewport();
    std::cout << "settings panel tests passed\n";
    return 0;
}
