#include "imgui_widget_harness.h"
#include "ui/settings_panel.h"
#include "app/embedded_legal_documents.h"
#include "../helpers/temporary_directory.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace spectiary;
using spectiary::test::WidgetHarness;

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
    f.ui.Click("SettingsInput");
    f.ui.Click("LiveNumericNavigation");
    Require(f.settings.View().live_numeric_navigation != initial,
            "Input checkbox must update ApplicationSettings");
    f.ui.Click("LiveNumericNavigation");
    Require(f.settings.View().live_numeric_navigation == initial,
            "Second checkbox click must restore the original setting");
    Require(f.intents == 2, "Checkbox interactions must emit exactly one intent each");
}

void TestGeneralAndLanguageControls()
{
    SettingsFixture f;
    f.ui.Click("SettingsGeneral");
    f.ui.Click("OpenExternalSourceAsFolder");
    Require(f.settings.View().open_external_source_as_folder && f.intents == 1,
        "External source checkbox must update the owner exactly once");
    const auto placeholder = f.ui.Find("IncludeExternalSubfolders");
    Require(placeholder.disabled, "Deferred subfolder control must stay disabled");
    try {
        f.ui.Click("IncludeExternalSubfolders");
        throw std::runtime_error("Disabled placeholder accepted input");
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()) == "Disabled widget: IncludeExternalSubfolders",
            "Disabled placeholder must reject widget input");
    }
    Require(f.intents == 1, "Disabled placeholder must not emit an intent");
    f.ui.Click("SettingsLanguage");
    f.ui.Click("ApplicationLanguage");
    f.ui.Click("UiLanguageSimplifiedChinese");
    Require(f.settings.View().language == UiLanguage::SimplifiedChinese && f.intents == 2,
        "Language selector must update the owner exactly once");
    f.ui.Click("SettingsAppearance");
    (void)f.settings.Apply(ApplicationSettingsIntent::SetUiScale(125), {});
    f.ui.Frames(2);
    f.ui.Click("UiScaleReset");
    Require(f.settings.View().ui_scale_percentage == 100 && f.intents == 3,
        "Localized scale Reset must preserve its identity and submit once");
}

void TestThemeCombo()
{
    SettingsFixture f;
    f.ui.Click("SettingsAppearance");
    f.ui.Click("AppearanceTheme");
    f.ui.Click("Light");
    Require(f.settings.View().theme_selection == ThemeSelection::Explicit(BuiltInLightThemeId()),
            "Rendered theme popup must select the light theme");
    Require(f.intents == 1, "Theme selection must emit one intent");
}

void TestWarnedFallbackRepairThroughControls()
{
    test_support::TemporaryDirectory temporary;
    const auto& root = temporary.path();
    ApplicationSettingsStorage storage{
        .language_settings_path = root / "language.json",
        .appearance_settings_path = root / "appearance.json",
        .ui_scale_settings_path = root / "scale.json",
        .input_settings_path = root / "input.json",
        .external_source_settings_path = root / "external.json",
        .profile_settings_path = root / "profile.json",
        .panel_visibility_path = root / "panels.json",
        .default_profile_output_directory = root / "profiles"};
    for (const auto& path : {storage.language_settings_path, storage.profile_settings_path}) {
        std::ofstream file(path);
        file << "{invalid";
    }
    ApplicationSettings settings(storage);
    for (auto setting : {ApplicationSetting::Language, ApplicationSetting::ProfileOutputDirectory})
        Require(settings.View().StatusFor(setting).kind == ApplicationSettingsStatusKind::LoadWarning,
            "Malformed persisted settings must start with a load warning");
    const auto fallback_language = settings.View().language;
    SettingsPanelUi panel{SettingsPanelEnvironment{}};
    std::vector<ApplicationSettingsIntentKind> intents;
    WidgetHarness ui{[&] {
        panel.Render(settings.View(), {});
        if (auto intent = panel.TakeApplicationSettingsIntent()) {
            intents.push_back(intent->kind);
            Require(settings.Apply(std::move(*intent), {}).applied(),
                "Reselecting a warned fallback must repair the settings owner");
            Require(!panel.TakeApplicationSettingsIntent(), "Repair intent must be one-shot");
        }
    }};
    panel.Open(); ui.Frames(3);
    ui.Click("SettingsLanguage");
    ui.Click("ApplicationLanguage");
    ui.Click(fallback_language == UiLanguage::English ? "UiLanguageEnglish" : "UiLanguageSimplifiedChinese");
    Require(settings.View().language == fallback_language &&
        settings.View().StatusFor(ApplicationSetting::Language).kind == ApplicationSettingsStatusKind::Ready,
        "Selecting the current fallback language must clear its warning without changing its value");
    ui.Click("SettingsDiagnostics");
    Require(!ui.Find("RestoreProfileOutputDefault").disabled,
        "Warned default directory must remain repairable");
    ui.Click("RestoreProfileOutputDefault");
    Require(settings.View().profile_output_directory == storage.default_profile_output_directory &&
        settings.View().StatusFor(ApplicationSetting::ProfileOutputDirectory).kind == ApplicationSettingsStatusKind::Ready,
        "Restoring the current default directory must clear its warning");
    Require(intents == std::vector<ApplicationSettingsIntentKind>{ApplicationSettingsIntentKind::SetLanguage,
        ApplicationSettingsIntentKind::RestoreDefaultProfileOutputDirectory},
        "Each repair must submit exactly its own settings intent once");
    Require(settings.Flush().all_saved(), "Repaired settings must persist");
    ApplicationSettings reopened(storage);
    Require(reopened.View().StatusFor(ApplicationSetting::Language).kind == ApplicationSettingsStatusKind::Ready &&
        reopened.View().StatusFor(ApplicationSetting::ProfileOutputDirectory).kind == ApplicationSettingsStatusKind::Ready,
        "Repaired settings must reload without warnings");
}

void TestLegalDocumentControls()
{
    SettingsFixture f;
    std::string clipboard;
    int copies = 0;
    struct ClipboardCapture { std::string& text; int& count; } capture{clipboard, copies};
    auto& platform = ImGui::GetPlatformIO();
    platform.Platform_ClipboardUserData = &capture;
    platform.Platform_SetClipboardTextFn = [](ImGuiContext* context, const char* text) {
        auto& output = *static_cast<ClipboardCapture*>(context->PlatformIO.Platform_ClipboardUserData);
        output.text = text;
        ++output.count;
    };
    f.ui.Click("SettingsAbout");
    const auto reveal = [&](std::string_view label) {
        // Scroll the real content pane; do not scan coordinates to discover a control.
        for (int frame = 0; frame < 40 && !f.ui.Observe(label); ++frame) {
            ImGuiWindow* settings = ImGui::FindWindowByName("Settings###SettingsV1");
            Require(settings != nullptr, "Settings must be visible");
            ImGuiWindow* content = nullptr;
            for (auto* window : GImGui->Windows)
                if (window->ParentWindow == settings && window->ChildId == settings->GetID("##SettingsContent"))
                    content = window;
            Require(content != nullptr, "Settings content pane must exist");
            const auto point = ImVec2(content->InnerClipRect.Max.x - 2, content->InnerClipRect.Min.y + 10);
            ImGui::GetIO().AddMousePosEvent(point.x, point.y); f.ui.Frames(2);
            ImGui::GetIO().AddMouseWheelEvent(0, -2); f.ui.Frames(2);
        }
        (void)f.ui.Find(label);
    };
    const auto check_disclosure = [&](LegalDocument expected) {
        int count = 0;
        for (auto* window : GImGui->Windows) {
            if (!window->Active || !window->ParentWindow) continue;
            for (const auto& [child, document] : {
                     std::pair{"##ThirdPartyNoticesContent", LegalDocument::ThirdPartyNotices},
                     std::pair{"##DataSourcesContent", LegalDocument::DataSources}}) {
                if (window->ChildId == window->ParentWindow->GetID(child)) {
                    ++count;
                    Require(document == expected, "Switching disclosures must close the previous document");
                }
            }
        }
        Require(count == 1, "Exactly the selected legal disclosure must remain open");
    };
    for (const auto& [label, document] : {
             std::pair{"OpenThirdPartyNotices", LegalDocument::ThirdPartyNotices},
             std::pair{"OpenDataSources", LegalDocument::DataSources}}) {
        reveal(label);
        f.ui.Click(label);
        reveal("CopyLegalDocument");
        check_disclosure(document);
        Require(!EmbeddedLegalDocumentContent(document).empty(), "Legal fixture must embed production resources");
        const int before = copies;
        f.ui.Click("CopyLegalDocument");
        Require(copies == before + 1 && clipboard == EmbeddedLegalDocumentContent(document),
            "Copy must emit exactly the complete selected legal document once");
        check_disclosure(document);
    }
    platform.Platform_ClipboardUserData = nullptr;
    platform.Platform_SetClipboardTextFn = nullptr;
}

void TestExternalOpenInstanceCombo()
{
    SettingsFixture f;
    f.ui.Click("SettingsGeneral");
    Require(f.settings.View().external_open_instance_policy == ExternalOpenInstancePolicy::NewInstance,
        "instance routing must default to a new process");
    f.ui.Click("ExternalOpenInstancePolicy");
    f.ui.Click("Add to recent instance and activate");
    Require(f.settings.View().external_open_instance_policy == ExternalOpenInstancePolicy::RecentInstance && f.intents == 1,
        "recent-instance selection must emit one owner intent");
    f.ui.Click("ExternalOpenInstancePolicy");
    f.ui.Click("Open in a new instance");
    Require(f.settings.View().external_open_instance_policy == ExternalOpenInstancePolicy::NewInstance && f.intents == 2,
        "new-instance selection must restore the original policy");
}

void TestScaleKeyboardCommitAndReset()
{
    SettingsFixture f;
    f.ui.Click("SettingsAppearance");
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    f.ui.Frames();
    f.ui.Click("UiScale");
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
    f.ui.Frames();
    f.ui.Text("125");
    Require(f.settings.View().ui_scale_percentage == 100,
            "Scale draft must not apply before the edit finishes");
    f.ui.Key(ImGuiKey_Enter);
    f.ui.Until([&] { return f.settings.View().ui_scale_percentage == 125; }, "scale keyboard commit");
    f.ui.Click("UiScaleReset");
    Require(f.settings.View().ui_scale_percentage == 100, "Scale Reset must restore 100 percent");
    Require(f.intents == 2, "Scale commit and Reset must each emit one intent");
}

void TestRecordingDisablesDirectoryReset()
{
    SettingsFixture f;
    Require(f.settings.Apply(ApplicationSettingsIntent::SetProfileOutputDirectory("widget-custom-logs"), {}).applied(),
            "Custom profile directory fixture must apply");
    f.status.profile_open = true;
    f.ui.Click("SettingsDiagnostics");
    Require(f.ui.Find("RestoreProfileOutputDefault").disabled,
            "Directory reset must be disabled while recording");
    // Inject a real click even on the disabled control to prove it cannot emit an intent.
    const auto point = f.ui.Find("RestoreProfileOutputDefault").bounds.GetCenter();
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
    f.ui.Click("RestoreProfileOutputDefault");
    Require(f.settings.View().profile_output_directory == "widget-default-logs",
            "Reset after recording must restore the production owner's default directory");
    Require(f.intents == 1, "Enabled reset must emit one intent");
}

void TestClippedWidgetBounds()
{
    int clicks = 0;
    WidgetHarness ui{[&] {
        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(240, 160), ImGuiCond_Always);
        ImGui::Begin("Harness clipping", nullptr,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::SetCursorScreenPos(ImVec2(220, 70));
        if (ImGui::Button("Partially clipped", ImVec2(100, 30))) ++clicks;
        ImGui::End();
    }};
    ui.Frames(3);
    const auto widget = ui.Find("Partially clipped");
    Require(widget.raw_bounds.GetWidth() == 100.0f &&
            widget.raw_bounds.Max.x > widget.bounds.Max.x &&
            widget.bounds.GetWidth() > 0.0f,
        "Geometry observations must preserve overflow beyond the visible bounds");
    Require(!widget.bounds.Contains(widget.raw_bounds.GetCenter()),
        "Clipping fixture must put the raw center outside the visible region");
    ui.Click("Partially clipped");
    Require(clicks == 1, "Pointer input must use the visible part of a clipped widget");
}

void TestLayoutRecoveryControlAndSettingsPlacement()
{
    SettingsFixture f;
    f.ui.Click("SettingsDataAndRecovery");
    f.ui.Click("ResetWindowLayout");
    Require(f.panel.TakeRestoreDefaultLayoutRequest(),
        "Layout recovery button must emit a request");
    Require(!f.panel.TakeRestoreDefaultLayoutRequest() && f.intents == 0,
        "Recovery is one-shot and must not emit unrelated settings intents");
    ImGuiWindow* window = ImGui::FindWindowByName("###SettingsV1");
    Require(window != nullptr, "Settings window should exist");
    ImGui::SetWindowPos(window, ImVec2(30000, -20000));
    ImGui::SetWindowSize(window, ImVec2(4000, 3000));
    f.panel.CloseForLayoutRecovery();
    f.ui.Frames(2);
    Require(!f.panel.open(), "Settings is hidden in the default layout");
    ImGui::GetIO().DisplaySize = ImVec2(800, 600);
    f.panel.Open();
    f.ui.Frames(3);
    const auto* viewport = ImGui::GetMainViewport();
    Require(window->Pos.x >= viewport->WorkPos.x &&
            window->Pos.y >= viewport->WorkPos.y &&
            window->Pos.x + window->Size.x <= viewport->WorkPos.x + viewport->WorkSize.x + 1 &&
            window->Pos.y + window->Size.y <= viewport->WorkPos.y + viewport->WorkSize.y + 1,
        "Settings must reopen within the reduced current work area");
    Require(window->DockId == 0, "Settings must reopen undocked");
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
        if (name == "all" || name == "failures") { TestBoundedFailures(); TestClippedWidgetBounds(); }
        if (name == "all" || name == "input") { TestInputCheckbox(); TestGeneralAndLanguageControls(); TestExternalOpenInstanceCombo(); TestLayoutRecoveryControlAndSettingsPlacement(); TestLegalDocumentControls(); }
        if (name == "all" || name == "theme") TestThemeCombo();
        if (name == "all" || name == "scale") TestScaleKeyboardCommitAndReset();
        if (name == "all" || name == "recording") { TestRecordingDisablesDirectoryReset(); TestWarnedFallbackRepairThroughControls(); }
        std::cout << "Widget regression passed: " << name << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
