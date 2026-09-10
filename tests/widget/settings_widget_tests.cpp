#include "imgui_widget_harness.h"
#include "ui/settings_panel.h"

#include <iostream>
#include <stdexcept>

namespace {
using namespace specforge;
using specforge::test::WidgetHarness;

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct SettingsFixture {
    ApplicationSettings settings{ApplicationSettingsStorage{
        .default_profile_output_directory = "widget-default-logs", .persistent = false}};
    SettingsPanelUi panel{SettingsPanelEnvironment{}};
    SettingsPanelStatus status;
    int intents = 0;
    WidgetHarness ui{[this] {
        panel.Render(settings.View(), status);
        if (auto intent = panel.TakeApplicationSettingsIntent()) {
            ++intents;
            const auto result = settings.Apply(std::move(*intent),
                {.profile_recording_in_progress = status.profile_open || status.profile_stopping});
            Require(result.outcome != ApplicationSettingsOutcome::Rejected,
                    "Rendered control emitted a rejected settings intent");
            Require(!panel.TakeApplicationSettingsIntent(), "Intent must be one-shot");
        }
    }};
    SettingsFixture() { panel.Open(); ui.Frames(3); }
};

void TestInputCheckbox()
{
    SettingsFixture f;
    const bool initial = f.settings.View().live_numeric_navigation;
    f.ui.Click("SpecForgeSettingsInput");
    f.ui.Click("SpecForgeLiveNumericNavigation");
    Require(f.settings.View().live_numeric_navigation != initial,
            "Input checkbox must update ApplicationSettings");
    f.ui.Click("SpecForgeLiveNumericNavigation");
    Require(f.settings.View().live_numeric_navigation == initial,
            "Second checkbox click must restore the original setting");
    Require(f.intents == 2, "Checkbox interactions must emit exactly one intent each");
}

void TestGeneralAndLanguageControls()
{
    SettingsFixture f;
    f.ui.Click("SpecForgeSettingsGeneral");
    f.ui.Click("SpecForgeOpenExternalSourceAsFolder");
    Require(f.settings.View().open_external_source_as_folder && f.intents == 1,
        "External source checkbox must update the owner exactly once");
    const auto placeholder = f.ui.Find("SpecForgeIncludeExternalSubfolders");
    Require(placeholder.disabled, "Deferred subfolder control must stay disabled");
    try {
        f.ui.Click("SpecForgeIncludeExternalSubfolders");
        throw std::runtime_error("Disabled placeholder accepted input");
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()) == "Disabled widget: SpecForgeIncludeExternalSubfolders",
            "Disabled placeholder must reject widget input");
    }
    Require(f.intents == 1, "Disabled placeholder must not emit an intent");
    f.ui.Click("SpecForgeSettingsLanguage");
    f.ui.Click("SpecForgeApplicationLanguage");
    f.ui.Click("SpecForgeUiLanguageSimplifiedChinese");
    Require(f.settings.View().language == UiLanguage::SimplifiedChinese && f.intents == 2,
        "Language selector must update the owner exactly once");
    f.ui.Click("SpecForgeSettingsAppearance");
    (void)f.settings.Apply(ApplicationSettingsIntent::SetUiScale(125), {});
    f.ui.Frames(2);
    f.ui.Click("SpecForgeUiScaleReset");
    Require(f.settings.View().ui_scale_percentage == 100 && f.intents == 3,
        "Localized scale Reset must preserve its identity and submit once");
}

void TestThemeCombo()
{
    SettingsFixture f;
    f.ui.Click("SpecForgeSettingsAppearance");
    f.ui.Click("SpecForgeAppearanceTheme");
    f.ui.Click("Light");
    Require(f.settings.View().theme_selection == ThemeSelection::Explicit(BuiltInLightThemeId()),
            "Rendered theme popup must select the light theme");
    Require(f.intents == 1, "Theme selection must emit one intent");
}

void TestScaleKeyboardCommitAndReset()
{
    SettingsFixture f;
    f.ui.Click("SpecForgeSettingsAppearance");
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    f.ui.Frames();
    f.ui.Click("SpecForgeUiScale");
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
    f.ui.Frames();
    f.ui.Text("125");
    Require(f.settings.View().ui_scale_percentage == 100,
            "Scale draft must not apply before the edit finishes");
    f.ui.Key(ImGuiKey_Enter);
    f.ui.Until([&] { return f.settings.View().ui_scale_percentage == 125; }, "scale keyboard commit");
    f.ui.Click("SpecForgeUiScaleReset");
    Require(f.settings.View().ui_scale_percentage == 100, "Scale Reset must restore 100 percent");
    Require(f.intents == 2, "Scale commit and Reset must each emit one intent");
}

void TestRecordingDisablesDirectoryReset()
{
    SettingsFixture f;
    Require(f.settings.Apply(ApplicationSettingsIntent::SetProfileOutputDirectory("widget-custom-logs"), {}).applied(),
            "Custom profile directory fixture must apply");
    f.status.profile_open = true;
    f.ui.Click("SpecForgeSettingsDiagnostics");
    Require(f.ui.Find("SpecForgeRestoreProfileOutputDefault").disabled,
            "Directory reset must be disabled while recording");
    // Inject a real click even on the disabled control to prove it cannot emit an intent.
    const auto point = f.ui.Find("SpecForgeRestoreProfileOutputDefault").bounds.GetCenter();
    ImGui::GetIO().AddMousePosEvent(point.x, point.y);
    f.ui.Frames();
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    f.ui.Frames();
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    f.ui.Frames(2);
    Require(f.intents == 0, "Disabled reset must not emit an intent");
    Require(f.settings.View().profile_output_directory == "widget-custom-logs",
            "Recording must preserve the output directory");
    f.status.profile_open = false;
    f.ui.Frames(2);
    f.ui.Click("SpecForgeRestoreProfileOutputDefault");
    Require(f.settings.View().profile_output_directory == "widget-default-logs",
            "Reset after recording must restore the production owner's default directory");
    Require(f.intents == 1, "Enabled reset must emit one intent");
}

void TestBoundedFailures()
{
    WidgetHarness ui{[] {
        ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_Always);
        ImGui::Begin("Harness failures");
        ImGui::PushID(1); ImGui::Button("Duplicate"); ImGui::PopID();
        ImGui::PushID(2); ImGui::Button("Duplicate"); ImGui::PopID();
        ImGui::PushID("source/task-a"); ImGui::Button("Recover"); ImGui::PopID();
        ImGui::PushID("source/task-b"); ImGui::Button("Recover"); ImGui::PopID();
        ImGui::BeginDisabled(); ImGui::Button("Disabled"); ImGui::EndDisabled();
        ImGui::End();
    }};
    ui.Frames(3);
    const auto first_recovery = ui.Observe("Recover", "source/task-a");
    const auto second_recovery = ui.Observe("Recover", "source/task-b");
    Require(first_recovery && second_recovery && first_recovery->id != second_recovery->id,
            "Semantic scopes must disambiguate repeated row actions");
    Require(!ui.Observe("Missing"), "Immediate observation must not invent an absent widget");
    const auto expect_failure = [](auto action, const char* message) {
        try { action(); }
        catch (const std::runtime_error& error) {
            Require(std::string(error.what()) == message, "Failure diagnostic must be deterministic");
            return;
        }
        throw std::runtime_error("Expected bounded harness failure");
    };
    const int start = ui.frame_count();
    expect_failure([&] { ui.Find("Missing"); }, "Widget not found after 8 frames: Missing");
    Require(ui.frame_count() - start == 8, "Missing widget must consume exactly the bounded frame budget");
    expect_failure([&] { ui.Find("Duplicate"); }, "Ambiguous widget: Duplicate");
    expect_failure([&] { ui.Click("Disabled"); }, "Disabled widget: Disabled");
    const int settlement_start = ui.frame_count();
    expect_failure([&] { ui.Until([] { return false; }, "never settles", 3); },
                   "Widget settlement timed out: never settles");
    Require(ui.frame_count() - settlement_start == 3, "Settlement must obey the frame limit");
}
} // namespace

int main(int argc, char** argv)
{
    try {
        const std::string name = argc > 1 ? argv[1] : "all";
        Require(name == "all" || name == "failures" || name == "input" ||
                name == "theme" || name == "scale" || name == "recording", "Unknown widget test case");
        if (name == "all" || name == "failures") TestBoundedFailures();
        if (name == "all" || name == "input") { TestInputCheckbox(); TestGeneralAndLanguageControls(); }
        if (name == "all" || name == "theme") TestThemeCombo();
        if (name == "all" || name == "scale") TestScaleKeyboardCommitAndReset();
        if (name == "all" || name == "recording") TestRecordingDisablesDirectoryReset();
        std::cout << "Widget regression passed: " << name << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
