#include "ui/settings_panel.h"

#include "app/embedded_legal_documents.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <thread>
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
    static SettingsSection SelectedSection(
        const SettingsPanelUi& panel)
    {
        return panel.selected_section_;
    }
    static void SetViewportId(
        SettingsPanelUi& panel,
        unsigned int viewport_id)
    {
        panel.settings_viewport_id_ = viewport_id;
    }
    static void RenderGeneral(
        SettingsPanelUi& panel,
        const ApplicationSettingsView& settings)
    {
        panel.RenderGeneral(settings);
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
    static void RequestPlatformWindowFocus(
        ImGuiViewport& viewport)
    {
        SettingsPanelUi::RequestPlatformWindowFocus(
            viewport);
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
    static bool ArtifactIdentityEvaluated(
        const SettingsPanelUi& panel)
    {
        return panel.artifact_identity_.has_value();
    }
    static ArtifactIdentityStatus ArtifactIdentityStatusForTest(
        const SettingsPanelUi& panel)
    {
        return panel.artifact_identity_
            ? panel.artifact_identity_->status
            : ArtifactIdentityStatus::Unavailable;
    }
    static ArtifactIdentityResult ArtifactIdentityResultForTest(
        const SettingsPanelUi& panel)
    {
        return panel.artifact_identity_.value_or(
            ArtifactIdentityResult{});
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
        ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
        platform_io.Platform_ClipboardUserData = &clipboard_text_;
        platform_io.Platform_SetClipboardTextFn =
            [](ImGuiContext* context, const char* text) {
                auto* clipboard_text = static_cast<std::string*>(
                    context->PlatformIO.Platform_ClipboardUserData);
                *clipboard_text = text != nullptr ? text : "";
            };
    }

    ~ScopedImGuiContext()
    {
        ImGui::DestroyContext();
    }

    ScopedImGuiContext(const ScopedImGuiContext&) = delete;
    ScopedImGuiContext& operator=(
        const ScopedImGuiContext&) = delete;

    const std::string& clipboard_text() const
    {
        return clipboard_text_;
    }

private:
    std::string clipboard_text_;
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

void RenderSettingsFrame(specforge::SettingsPanelUi& panel)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    ImGui::NewFrame();
    panel.Render(MakeSettingsView());
    ImGui::EndFrame();
}

specforge::BuildMetadataReadResult MakeArtifactMetadata(
    std::string sha256)
{
    specforge::BuildMetadata metadata;
    metadata.finalized_artifact = specforge::FinalizedArtifactMetadata{
        .completed_at_utc = "2026-08-05T09:21:32Z",
        .artifact = specforge::BuildArtifactMetadata{
            .file = "SpecForge.exe",
            .sha256 = std::move(sha256),
        },
    };
    return {
        .status = specforge::BuildMetadataStatus::Available,
        .metadata = std::move(metadata),
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

struct GeneralRenderObservation {
    bool open_external_source_as_folder_hovered = false;
    bool include_external_subfolders_found = false;
    bool include_external_subfolders_disabled = false;
    ImVec2 include_external_subfolders_center;
};

struct LegalDocumentUiFixture {
    specforge::LegalDocument document;
    const char* entry_label;
    const char* content_child_id;
};

constexpr LegalDocumentUiFixture kLegalDocumentUiFixtures[] = {
    {
        specforge::LegalDocument::ThirdPartyNotices,
        "Third-Party Notices###SpecForgeOpenThirdPartyNotices",
        "##SpecForgeThirdPartyNoticesContent",
    },
    {
        specforge::LegalDocument::DataSources,
        "Data Sources###SpecForgeOpenDataSources",
        "##SpecForgeDataSourcesContent",
    },
};

struct LegalRenderObservation {
    ImGuiID entry_id = 0;
    bool entry_hovered = false;
    ImGuiID copy_id = 0;
    bool copy_hovered = false;
    bool any_popup_open = false;
    int open_disclosure_count = 0;
    ImVec2 document_content_center;
    float document_content_width = 0.0f;
    float document_content_height = 0.0f;
    bool document_content_found = false;
    bool document_content_hovered = false;
    bool document_content_borderless = false;
    bool any_input_text_state = false;
    float document_scroll_y = 0.0f;
    float document_scroll_max_y = 0.0f;
    ImGuiWindow* settings_navigation_window = nullptr;
    ImGuiWindow* settings_content_window = nullptr;
    ImRect settings_content_clip_rect;
    float settings_content_scroll_max_y = 0.0f;
};

LegalRenderObservation RenderLegalFrame(
    specforge::SettingsPanelUi& panel,
    const LegalDocumentUiFixture& fixture,
    ImVec2 display_size = ImVec2(700.0f, 500.0f))
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = display_size;
    ImGui::NewFrame();
    specforge::ApplicationSettingsView settings =
        MakeSettingsView();
    settings.ui_scale_percentage = 150;
    panel.Render(settings);

    LegalRenderObservation observation;
    ImGuiWindow* settings_window = ImGui::FindWindowByName(
        "Settings###SpecForgeSettingsV1");
    if (settings_window != nullptr) {
        const ImGuiID navigation_child_id =
            settings_window->GetID("##SettingsNavigation");
        const ImGuiID content_child_id =
            settings_window->GetID("##SettingsContent");
        for (ImGuiWindow* window : GImGui->Windows) {
            if (window->ParentWindow != settings_window) {
                continue;
            }
            if (window->ChildId == navigation_child_id) {
                observation.settings_navigation_window = window;
            }
            if (window->ChildId == content_child_id) {
                observation.settings_content_window = window;
                observation.settings_content_clip_rect =
                    window->InnerClipRect;
                observation.settings_content_scroll_max_y =
                    window->ScrollMax.y;
                observation.entry_id =
                    window->GetID(fixture.entry_label);
                observation.entry_hovered =
                    GImGui->HoveredId == observation.entry_id;
                observation.copy_id = window->GetID(
                    "Copy Document###SpecForgeCopyLegalDocument");
                observation.copy_hovered =
                    GImGui->HoveredId == observation.copy_id;
                break;
            }
        }
    }

    observation.any_popup_open = ImGui::IsPopupOpen(
        nullptr,
        ImGuiPopupFlags_AnyPopupId |
            ImGuiPopupFlags_AnyPopupLevel);
    observation.any_input_text_state =
        GImGui->InputTextState.ID != 0;

    if (observation.settings_content_window != nullptr) {
        const ImGuiID document_child_id =
            observation.settings_content_window->GetID(
                fixture.content_child_id);
        for (ImGuiWindow* window : GImGui->Windows) {
            if (window->ParentWindow !=
                    observation.settings_content_window ||
                !window->Active) {
                continue;
            }
            for (const LegalDocumentUiFixture& candidate :
                 kLegalDocumentUiFixtures) {
                if (window->ChildId ==
                    observation.settings_content_window->GetID(
                        candidate.content_child_id)) {
                    ++observation.open_disclosure_count;
                }
            }
            if (window->ChildId == document_child_id) {
                ImRect visible_bounds = window->Rect();
                visible_bounds.ClipWith(
                    observation.settings_content_clip_rect);
                observation.document_content_center =
                    visible_bounds.GetCenter();
                observation.document_content_width = window->Size.x;
                observation.document_content_height = window->Size.y;
                observation.document_content_found = true;
                observation.document_content_borderless =
                    (window->ChildFlags &
                     ImGuiChildFlags_Borders) == 0;
                observation.document_content_hovered =
                    GImGui->HoveredWindow == window;
                observation.document_scroll_y = window->Scroll.y;
                observation.document_scroll_max_y =
                    window->ScrollMax.y;
                break;
            }
        }
    }

    ImGui::EndFrame();
    return observation;
}

ImVec2 FindLegalEntryPosition(
    specforge::SettingsPanelUi& panel,
    const LegalDocumentUiFixture& fixture)
{
    LegalRenderObservation observation =
        RenderLegalFrame(panel, fixture);
    observation = RenderLegalFrame(panel, fixture);
    Require(
        observation.settings_content_window != nullptr,
        "the About test fixture should expose the Settings content child");

    const float viewport_height = std::max(
        1.0f,
        observation.settings_content_clip_rect.GetHeight());
    const float scroll_step = viewport_height * 0.5f;
    for (float scroll_y = 0.0f;;
         scroll_y = std::min(
             scroll_y + scroll_step,
             observation.settings_content_scroll_max_y)) {
        observation.settings_content_window->Scroll.y = scroll_y;
        observation = RenderLegalFrame(panel, fixture);

        for (float y =
                 observation.settings_content_clip_rect.Min.y + 1.0f;
             y < observation.settings_content_clip_rect.Max.y;
             y += 3.0f) {
            for (float x =
                     observation.settings_content_clip_rect.Min.x + 1.0f;
                 x < observation.settings_content_clip_rect.Max.x;
                 x += 24.0f) {
                ImGui::GetIO().AddMousePosEvent(x, y);
                observation = RenderLegalFrame(panel, fixture);
                if (observation.entry_hovered) {
                    return ImVec2(x, y);
                }
            }
        }

        if (scroll_y >=
            observation.settings_content_scroll_max_y) {
            break;
        }
    }

    Require(
        false,
        "each Legal entry should remain pointer-accessible in the narrow About content area");
    return ImVec2();
}

ImVec2 FindLegalCopyPosition(
    specforge::SettingsPanelUi& panel,
    const LegalDocumentUiFixture& fixture)
{
    LegalRenderObservation observation =
        RenderLegalFrame(panel, fixture);
    Require(
        observation.settings_content_window != nullptr,
        "the expanded Legal disclosure should remain inside Settings content");

    const float viewport_height = std::max(
        1.0f,
        observation.settings_content_clip_rect.GetHeight());
    const float scroll_step = viewport_height * 0.5f;
    for (float scroll_y = 0.0f;;
         scroll_y = std::min(
             scroll_y + scroll_step,
             observation.settings_content_scroll_max_y)) {
        observation.settings_content_window->Scroll.y = scroll_y;
        observation = RenderLegalFrame(panel, fixture);

        for (float y =
                 observation.settings_content_clip_rect.Min.y + 1.0f;
             y < observation.settings_content_clip_rect.Max.y;
             y += 3.0f) {
            for (float x =
                     observation.settings_content_clip_rect.Min.x + 1.0f;
                 x < observation.settings_content_clip_rect.Max.x;
                 x += 24.0f) {
                ImGui::GetIO().AddMousePosEvent(x, y);
                observation = RenderLegalFrame(panel, fixture);
                if (observation.copy_hovered) {
                    return ImVec2(x, y);
                }
            }
        }

        if (scroll_y >=
            observation.settings_content_scroll_max_y) {
            break;
        }
    }

    Require(
        false,
        "Copy Document should remain pointer-accessible in the expanded disclosure");
    return ImVec2();
}

LegalRenderObservation ClickLegalPosition(
    specforge::SettingsPanelUi& panel,
    const LegalDocumentUiFixture& fixture,
    ImVec2 position)
{
    ImGui::GetIO().AddMousePosEvent(position.x, position.y);
    (void)RenderLegalFrame(panel, fixture);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderLegalFrame(panel, fixture);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    return RenderLegalFrame(panel, fixture);
}

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

GeneralRenderObservation RenderGeneralFrame(
    specforge::SettingsPanelUi& panel,
    bool open_external_source_as_folder = false)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    ImGui::NewFrame();
    specforge::ApplicationSettingsView settings =
        MakeSettingsView();
    settings.open_external_source_as_folder =
        open_external_source_as_folder;
    panel.Render(settings);

    GeneralRenderObservation observation;
    const ImGuiID hovered_id = GImGui->HoveredId;
    for (ImGuiWindow* window : GImGui->Windows) {
        observation.open_external_source_as_folder_hovered =
            observation.open_external_source_as_folder_hovered ||
            hovered_id == window->GetID(
                "Open external spectrum files as folder sources###"
                "SpecForgeOpenExternalSourceAsFolder");
    }
    ImGui::EndFrame();
    return observation;
}

GeneralRenderObservation RenderGeneralContentFrame(
    specforge::SettingsPanelUi& panel)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    ImGui::NewFrame();
    ImGui::Begin("General fixture###SpecForgeGeneralFixture");
    const ImGuiID include_external_subfolders_id =
        GImGui->CurrentWindow->GetID(
            "Include subfolders (not implemented)###"
            "SpecForgeIncludeExternalSubfolders");
    specforge::SettingsPanelUiTestAccess::RenderGeneral(
        panel,
        MakeSettingsView());

    GeneralRenderObservation observation;
    observation.include_external_subfolders_found =
        GImGui->LastItemData.ID ==
        include_external_subfolders_id;
    if (observation.include_external_subfolders_found) {
        observation.include_external_subfolders_disabled =
            (GImGui->LastItemData.ItemFlags &
             ImGuiItemFlags_Disabled) != 0;
        observation.include_external_subfolders_center =
            GImGui->LastItemData.Rect.GetCenter();
    }
    ImGui::End();
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
        !environment.executable_path.empty(),
        "settings should expose the current executable path");
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
            "Source: 0123456789abcdef0123456789abcdef01234567",
        "HEAD About text should show the full Git revision without a "
        "relative HEAD label");
    Require(
        specforge::FormatBuildSourceForAbout(
            environment.build_source,
            specforge::UiLanguage::SimplifiedChinese) ==
            "源码：0123456789abcdef0123456789abcdef01234567",
        "Chinese HEAD About text should show the full Git revision without "
        "a relative HEAD label");

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

void TestArtifactIdentityVerification()
{
    constexpr std::string_view kAbcSha256 =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge-settings-artifact-identity";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
    const std::filesystem::path executable_path =
        root / "SpecForge.exe";
    {
        std::ofstream stream(executable_path, std::ios::binary);
        Require(stream.good(), "artifact identity fixture should open");
        stream << "abc";
    }

    const specforge::ArtifactIdentityResult available =
        specforge::VerifyExecutableArtifactIdentity(
            executable_path,
            MakeArtifactMetadata(std::string(kAbcSha256)));
    Require(
        available.status ==
                specforge::ArtifactIdentityStatus::Available &&
            available.completed_at_utc ==
                "2026-08-05T09:21:32Z" &&
            available.executable_sha256 == kAbcSha256,
        "matching executable and metadata digests should be available");

    const std::string mismatched_sha256(64, '0');
    const specforge::ArtifactIdentityResult mismatch =
        specforge::VerifyExecutableArtifactIdentity(
            executable_path,
            MakeArtifactMetadata(mismatched_sha256));
    Require(
        mismatch.status ==
                specforge::ArtifactIdentityStatus::Mismatch &&
            mismatch.completed_at_utc.empty() &&
            mismatch.executable_sha256 == kAbcSha256,
        "different executable and metadata digests should reject metadata "
        "without exposing an unverified completion time");

    const specforge::ArtifactIdentityResult unavailable =
        specforge::VerifyExecutableArtifactIdentity(
            root / "missing.exe",
            MakeArtifactMetadata(std::string(kAbcSha256)));
    Require(
        unavailable.status ==
                specforge::ArtifactIdentityStatus::Unavailable &&
            unavailable.completed_at_utc.empty() &&
            unavailable.executable_sha256.empty(),
        "an unreadable executable should make identity unavailable without "
        "exposing an unverified completion time");

    const specforge::ArtifactIdentityResult missing_metadata =
        specforge::VerifyExecutableArtifactIdentity(
            executable_path,
            {});
    Require(
        missing_metadata.status ==
                specforge::ArtifactIdentityStatus::Unavailable &&
            missing_metadata.completed_at_utc.empty() &&
            missing_metadata.executable_sha256 == kAbcSha256,
        "missing metadata should hide metadata fields while retaining the "
        "running executable digest");

    specforge::BuildMetadataReadResult malformed_metadata =
        MakeArtifactMetadata(std::string(kAbcSha256));
    malformed_metadata.metadata->finalized_artifact->artifact.sha256 =
        "not-a-sha256";
    const specforge::ArtifactIdentityResult malformed =
        specforge::VerifyExecutableArtifactIdentity(
            executable_path,
            malformed_metadata);
    Require(
        malformed.status ==
                specforge::ArtifactIdentityStatus::Unavailable &&
            malformed.completed_at_utc.empty() &&
            malformed.executable_sha256 == kAbcSha256,
        "malformed metadata should hide metadata fields while retaining the "
        "running executable digest");

    std::filesystem::remove_all(root, cleanup_error);
}

void TestAboutArtifactPresentationMatrix()
{
    constexpr std::string_view kAbcSha256 =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

    const specforge::ArtifactIdentityResult matching_identity = {
        .status = specforge::ArtifactIdentityStatus::Available,
        .completed_at_utc = "2026-08-05T09:21:32Z",
        .executable_sha256 = std::string(kAbcSha256),
    };
    const specforge::AboutArtifactPresentation matching =
        specforge::AboutArtifactPresentationFor(
            matching_identity,
            MakeArtifactMetadata(std::string(kAbcSha256)));
    Require(
        matching.show_executable_sha256 &&
            matching.show_metadata_derived_fields &&
            matching.show_completed_at_utc &&
            matching.show_third_party_versions &&
            !matching.show_third_party_fallback,
        "matching metadata should expose the executable hash and trusted metadata fields");

    const specforge::ArtifactIdentityResult missing_identity = {
        .status = specforge::ArtifactIdentityStatus::Unavailable,
        .executable_sha256 = std::string(kAbcSha256),
    };
    const specforge::AboutArtifactPresentation missing =
        specforge::AboutArtifactPresentationFor(
            missing_identity,
            {});
    Require(
        missing.show_executable_sha256 &&
            !missing.show_metadata_derived_fields &&
            !missing.show_completed_at_utc &&
            !missing.show_third_party_versions &&
            missing.show_third_party_fallback,
        "missing metadata should retain the executable hash and static component fallback without status text");

    specforge::BuildMetadataReadResult malformed_metadata =
        MakeArtifactMetadata(std::string(kAbcSha256));
    malformed_metadata.metadata->finalized_artifact->artifact.sha256 =
        "not-a-sha256";
    const specforge::ArtifactIdentityResult malformed_identity = {
        .status = specforge::ArtifactIdentityStatus::Unavailable,
        .executable_sha256 = std::string(kAbcSha256),
    };
    const specforge::AboutArtifactPresentation malformed =
        specforge::AboutArtifactPresentationFor(
            malformed_identity,
            malformed_metadata);
    Require(
        malformed.show_executable_sha256 &&
            !malformed.show_metadata_derived_fields &&
            !malformed.show_completed_at_utc &&
            !malformed.show_third_party_versions &&
            malformed.show_third_party_fallback,
        "malformed metadata should omit metadata-derived and verification UI while retaining static component information");

    const specforge::ArtifactIdentityResult mismatch_identity = {
        .status = specforge::ArtifactIdentityStatus::Mismatch,
        .executable_sha256 = std::string(kAbcSha256),
    };
    const specforge::AboutArtifactPresentation mismatch =
        specforge::AboutArtifactPresentationFor(
            mismatch_identity,
            MakeArtifactMetadata(std::string(64, '0')));
    Require(
        mismatch.show_executable_sha256 &&
            !mismatch.show_metadata_derived_fields &&
            !mismatch.show_completed_at_utc &&
            !mismatch.show_third_party_versions &&
            mismatch.show_third_party_fallback,
        "mismatched metadata should present like unavailable metadata without a mismatch or recorded-hash row");

    Require(
        specforge::UiText(
            specforge::UiLanguage::English,
            specforge::UiTextId::DearImGuiComponentFallback) ==
            "Dear ImGui (docking / Win32 / DirectX 11) - MIT License" &&
            specforge::UiText(
                specforge::UiLanguage::English,
                specforge::UiTextId::ImPlotComponentFallback) ==
            "ImPlot - MIT License" &&
            specforge::UiText(
                specforge::UiLanguage::English,
                specforge::UiTextId::ZlibComponentFallback) ==
            "zlib - zlib License",
        "untrusted metadata should retain static component and license information");

    constexpr std::array<std::string_view, 14> kForbiddenAboutText = {
        "Build metadata unavailable",
        "Build metadata mismatch",
        "Verifying executable identity",
        "Executable matches metadata",
        "Executable identity unavailable",
        "Executable identity mismatch",
        "Recorded executable SHA-256",
        "构建元数据不可用",
        "构建元数据不匹配",
        "正在验证可执行文件身份",
        "可执行文件与元数据匹配",
        "可执行文件身份不可用",
        "可执行文件身份不匹配",
        "元数据记录的可执行文件 SHA-256",
    };
    for (std::size_t text_index = 0;
         text_index < static_cast<std::size_t>(
             specforge::UiTextId::Count);
         ++text_index) {
        const auto text_id = static_cast<specforge::UiTextId>(text_index);
        const std::string_view english = specforge::UiText(
            specforge::UiLanguage::English,
            text_id);
        const std::string_view simplified_chinese = specforge::UiText(
            specforge::UiLanguage::SimplifiedChinese,
            text_id);
        for (const std::string_view forbidden : kForbiddenAboutText) {
            Require(
                english != forbidden && simplified_chinese != forbidden,
                "About catalog should not contain verification or metadata-hash status text");
        }
    }
}

void TestArtifactIdentityRetainsHashWhenMetadataUnavailable()
{
    ScopedImGuiContext imgui;
    constexpr std::string_view kAbcSha256 =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge-settings-artifact-identity-metadata-gate";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
    const std::filesystem::path executable_path =
        root / "SpecForge.exe";
    {
        std::ofstream stream(executable_path, std::ios::binary);
        Require(stream.good(), "metadata gate fixture should open");
        stream << "abc";
    }

    specforge::SettingsPanelUi panel({
        .version = "test",
        .distribution = "Portable",
        .configuration = "Debug",
        .target_architecture = "amd64",
        .executable_path = executable_path,
        .build_source = {
            .mode = "working_tree",
            .revision = "",
        },
        .build_metadata = {},
        .data_directory = "Data",
    });
    panel.Open();
    specforge::SettingsPanelUiTestAccess::SelectSection(
        panel,
        specforge::SettingsSection::About);
    RenderSettingsFrame(panel);
    const specforge::ArtifactIdentityResult first_frame =
        specforge::SettingsPanelUiTestAccess::ArtifactIdentityResultForTest(
            panel);
    Require(
        first_frame.executable_sha256 == kAbcSha256,
        "the first About frame should already show the running executable hash");

    bool metadata_unavailable = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        RenderSettingsFrame(panel);
        const specforge::ArtifactIdentityResult identity =
            specforge::SettingsPanelUiTestAccess::ArtifactIdentityResultForTest(
                panel);
        if (identity.status ==
            specforge::ArtifactIdentityStatus::Unavailable) {
            metadata_unavailable = true;
            Require(
                identity.executable_sha256 == kAbcSha256,
                "metadata rejection should retain the successful executable hash");
            break;
        }
    }
    Require(
        metadata_unavailable,
        "missing metadata should complete as unavailable without hiding the executable hash");

    {
        std::ofstream stream(executable_path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "metadata gate rewrite fixture should open");
        stream << "def";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{350});
    RenderSettingsFrame(panel);
    const specforge::ArtifactIdentityResult after_metadata_retry_window =
        specforge::SettingsPanelUiTestAccess::ArtifactIdentityResultForTest(
            panel);
    Require(
        after_metadata_retry_window.status ==
                specforge::ArtifactIdentityStatus::Unavailable &&
            after_metadata_retry_window.executable_sha256 == kAbcSha256,
        "metadata-only rejection should not trigger periodic executable rehashing");

    std::filesystem::remove_all(root, cleanup_error);
}

void TestArtifactIdentityIsComputedOnAboutDemand()
{
    ScopedImGuiContext imgui;
    constexpr std::string_view kAbcSha256 =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge-settings-artifact-identity-demand";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
    const std::filesystem::path executable_path =
        root / "SpecForge.exe";
    {
        std::ofstream stream(executable_path, std::ios::binary);
        Require(stream.good(), "on-demand identity fixture should open");
        stream << "abc";
    }

    specforge::SettingsPanelUi panel({
        .version = "test",
        .distribution = "Portable",
        .configuration = "Debug",
        .target_architecture = "amd64",
        .executable_path = executable_path,
        .build_source = {
            .mode = "working_tree",
            .revision = "",
        },
        .build_metadata = MakeArtifactMetadata(
            std::string(kAbcSha256)),
        .data_directory = "Data",
    });
    panel.Open();

    RenderSettingsFrame(panel);
    Require(
        !specforge::SettingsPanelUiTestAccess::
            ArtifactIdentityEvaluated(panel),
        "non-About rendering should not hash the executable");

    specforge::SettingsPanelUiTestAccess::SelectSection(
        panel,
        specforge::SettingsSection::About);
    RenderSettingsFrame(panel);
    Require(
        specforge::SettingsPanelUiTestAccess::
            ArtifactIdentityEvaluated(panel),
        "About rendering should evaluate artifact identity on demand");
    Require(
        specforge::SettingsPanelUiTestAccess::ArtifactIdentityStatusForTest(
            panel) == specforge::ArtifactIdentityStatus::Pending,
        "the first About frame should publish pending identity verification");
    Require(
        specforge::SettingsPanelUiTestAccess::ArtifactIdentityResultForTest(
            panel)
                .executable_sha256 == kAbcSha256,
        "the first About frame should expose the running executable hash");

    bool identity_available = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        RenderSettingsFrame(panel);
        if (specforge::SettingsPanelUiTestAccess::
                ArtifactIdentityStatusForTest(panel) ==
            specforge::ArtifactIdentityStatus::Available) {
            identity_available = true;
            break;
        }
    }
    Require(
        identity_available,
        "background identity verification should eventually complete");

    const specforge::ArtifactIdentityResult identity =
        specforge::SettingsPanelUiTestAccess::ArtifactIdentityResultForTest(
            panel);
    Require(
        identity.executable_sha256 == kAbcSha256 &&
        identity.completed_at_utc == "2026-08-05T09:21:32Z",
        "background identity verification should publish the verified completion time");

    RenderSettingsFrame(panel);
    const specforge::ArtifactIdentityResult identity_after_next_frame =
        specforge::SettingsPanelUiTestAccess::ArtifactIdentityResultForTest(
            panel);
    Require(
        identity_after_next_frame.status ==
                specforge::ArtifactIdentityStatus::Available &&
            identity_after_next_frame.completed_at_utc ==
                identity.completed_at_utc &&
            identity_after_next_frame.executable_sha256 ==
                identity.executable_sha256,
        "verified identity should persist across subsequent About frames");

    std::filesystem::remove_all(root, cleanup_error);
}

void TestArtifactIdentityRetriesAfterHashFailure()
{
    ScopedImGuiContext imgui;
    constexpr std::string_view kAbcSha256 =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge-settings-artifact-identity-retry";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
    const std::filesystem::path executable_path =
        root / "SpecForge.exe";

    specforge::SettingsPanelUi panel({
        .version = "test",
        .distribution = "Portable",
        .configuration = "Debug",
        .target_architecture = "amd64",
        .executable_path = executable_path,
        .build_source = {
            .mode = "working_tree",
            .revision = "",
        },
        .build_metadata = MakeArtifactMetadata(
            std::string(kAbcSha256)),
        .data_directory = "Data",
    });
    panel.Open();
    specforge::SettingsPanelUiTestAccess::SelectSection(
        panel,
        specforge::SettingsSection::About);
    RenderSettingsFrame(panel);
    Require(
        specforge::SettingsPanelUiTestAccess::ArtifactIdentityStatusForTest(
            panel) == specforge::ArtifactIdentityStatus::Pending,
        "a failed identity check should begin in the pending state");

    bool identity_unavailable = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        RenderSettingsFrame(panel);
        if (specforge::SettingsPanelUiTestAccess::
                ArtifactIdentityStatusForTest(panel) ==
            specforge::ArtifactIdentityStatus::Unavailable) {
            identity_unavailable = true;
            break;
        }
    }
    Require(
        identity_unavailable,
        "an unreadable executable should publish a retryable unavailable state");

    {
        std::ofstream stream(executable_path, std::ios::binary);
        Require(stream.good(), "retry identity fixture should open");
        stream << "abc";
    }

    bool identity_available = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        RenderSettingsFrame(panel);
        if (specforge::SettingsPanelUiTestAccess::
                ArtifactIdentityStatusForTest(panel) ==
            specforge::ArtifactIdentityStatus::Available) {
            identity_available = true;
            break;
        }
    }
    Require(
        identity_available,
        "a later About render should recover after the executable appears");

    std::filesystem::remove_all(root, cleanup_error);
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

ImGuiViewport* focused_settings_platform_viewport = nullptr;

void RecordSettingsPlatformFocus(ImGuiViewport* viewport)
{
    focused_settings_platform_viewport = viewport;
}

void TestDetachedPlatformWindowFocusUsesBackendCallback()
{
    ScopedImGuiContext imgui;

    ImGuiViewport detached_viewport;
    detached_viewport.ID =
        ImHashStr("SpecForgeSettingsFocusViewport");
    detached_viewport.PlatformWindowCreated = true;

    focused_settings_platform_viewport = nullptr;
    ImGui::GetPlatformIO().Platform_SetWindowFocus =
        RecordSettingsPlatformFocus;
    specforge::SettingsPanelUiTestAccess::
        RequestPlatformWindowFocus(detached_viewport);
    Require(
        focused_settings_platform_viewport ==
            &detached_viewport,
        "detached Settings focus should use the native platform callback");
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

void TestExternalSourceFolderCheckboxAndDeferredSubfolderPlaceholder()
{
    ScopedImGuiContext imgui;
    specforge::SettingsPanelUi panel = MakePanel();
    specforge::SettingsPanelUiTestAccess::SelectSection(
        panel,
        specforge::SettingsSection::General);
    panel.Open();

    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    GeneralRenderObservation observation =
        RenderGeneralFrame(panel);
    ImVec2 checkbox_position;
    for (float y = 80.0f;
         y <= 900.0f &&
         !observation.open_external_source_as_folder_hovered;
         y += 2.0f) {
        for (float x = 300.0f;
             x <= 1300.0f &&
             !observation.open_external_source_as_folder_hovered;
             x += 40.0f) {
            checkbox_position = ImVec2(x, y);
            ImGui::GetIO().AddMousePosEvent(
                checkbox_position.x,
                checkbox_position.y);
            observation = RenderGeneralFrame(panel);
        }
    }
    Require(
        observation.open_external_source_as_folder_hovered,
        "fixture should locate the external spectrum folder checkbox");

    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderGeneralFrame(panel);
    Require(
        !panel.TakeApplicationSettingsIntent(),
        "pressing the external spectrum folder checkbox should wait for click release");
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    (void)RenderGeneralFrame(panel);
    const std::optional<specforge::ApplicationSettingsIntent> intent =
        panel.TakeApplicationSettingsIntent();
    Require(
        intent &&
            intent->kind ==
                specforge::ApplicationSettingsIntentKind::
                    SetOpenExternalSourceAsFolder &&
            intent->open_external_source_as_folder,
        "clicking the external spectrum folder checkbox should emit the enabled setting");
    Require(
        !panel.TakeApplicationSettingsIntent(),
        "external spectrum folder intent should be consumed once");

    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    observation = RenderGeneralContentFrame(panel);
    Require(
        observation.include_external_subfolders_found &&
            observation.include_external_subfolders_disabled,
        "the deferred subfolder placeholder should be rendered disabled");

    ImGui::GetIO().AddMousePosEvent(
        observation.include_external_subfolders_center.x,
        observation.include_external_subfolders_center.y);
    observation = RenderGeneralContentFrame(panel);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderGeneralContentFrame(panel);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    (void)RenderGeneralContentFrame(panel);
    Require(
        !panel.TakeApplicationSettingsIntent(),
        "the disabled subfolder placeholder should never emit a setting intent");
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
    Require(
        ImHashStr(
            "Third-Party Notices###SpecForgeOpenThirdPartyNotices") ==
            ImHashStr(
                "第三方声明###SpecForgeOpenThirdPartyNotices"),
        "localized third-party notice disclosure headers should retain one ImGui ID");
    Require(
        ImHashStr(
            "Data Sources###SpecForgeOpenDataSources") ==
            ImHashStr(
                "数据来源###SpecForgeOpenDataSources"),
        "localized data-source disclosure headers should retain one ImGui ID");
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

void TestSettingsWindowCanGrowAfterDetachedViewportShrink()
{
    ScopedImGuiContext imgui;
    specforge::SettingsPanelUi panel = MakePanel();
    panel.Open();

    ImGuiPlatformMonitor monitor;
    monitor.MainPos = ImVec2(0.0f, 0.0f);
    monitor.MainSize = ImVec2(1600.0f, 1000.0f);
    monitor.WorkPos = monitor.MainPos;
    monitor.WorkSize = monitor.MainSize;
    monitor.DpiScale = 1.0f;
    ImGui::GetPlatformIO().Monitors.push_back(monitor);

    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = monitor.MainSize;
    ImGui::NewFrame();
    panel.Render(MakeSettingsView());
    ImGuiWindow* window = ImGui::FindWindowByName(
        "Settings###SpecForgeSettingsV1");
    Require(
        window != nullptr,
        "detached resize fixture should create the Settings window");
    ImGui::EndFrame();

    ImGuiViewportP detached_viewport;
    detached_viewport.ID =
        ImHashStr("SpecForgeSettingsDetachedViewport");
    detached_viewport.Pos = ImVec2(200.0f, 100.0f);
    detached_viewport.Size = ImVec2(1000.0f, 700.0f);
    detached_viewport.WorkPos = detached_viewport.Pos;
    detached_viewport.WorkSize = detached_viewport.Size;
    detached_viewport.DpiScale = 1.0f;
    detached_viewport.Idx = GImGui->Viewports.Size;

    ImGui::NewFrame();
    GImGui->Viewports.push_back(&detached_viewport);
    specforge::SettingsPanelUiTestAccess::SetViewportId(
        panel,
        detached_viewport.ID);
    ImGui::SetWindowSize(
        "Settings###SpecForgeSettingsV1",
        detached_viewport.Size,
        ImGuiCond_Always);
    panel.Render(MakeSettingsView());
    ImGui::EndFrame();
    GImGui->Viewports.pop_back();

    constexpr ImVec2 enlarged_size(1200.0f, 800.0f);
    ImGui::NewFrame();
    GImGui->Viewports.push_back(&detached_viewport);
    specforge::SettingsPanelUiTestAccess::SetViewportId(
        panel,
        detached_viewport.ID);
    ImGui::SetWindowSize(
        "Settings###SpecForgeSettingsV1",
        enlarged_size,
        ImGuiCond_Always);
    panel.Render(MakeSettingsView());
    Require(
        window->Size.x >= enlarged_size.x &&
            window->Size.y >= enlarged_size.y,
        "a detached Settings window should remain growable after it shrinks");
    ImGui::EndFrame();
    GImGui->Viewports.pop_back();
}

void TestSettingsDetachedConstraintsFollowClosestMonitor()
{
    ImGuiViewport detached_viewport;
    detached_viewport.Pos = ImVec2(2200.0f, 100.0f);
    detached_viewport.Size = ImVec2(700.0f, 500.0f);
    detached_viewport.WorkPos = detached_viewport.Pos;
    detached_viewport.WorkSize = detached_viewport.Size;

    std::array<ImGuiPlatformMonitor, 2> monitors;
    monitors[0].MainPos = ImVec2(0.0f, 0.0f);
    monitors[0].MainSize = ImVec2(1920.0f, 1080.0f);
    monitors[0].WorkPos = ImVec2(0.0f, 0.0f);
    monitors[0].WorkSize = ImVec2(1920.0f, 1040.0f);
    monitors[1].MainPos = ImVec2(1920.0f, 0.0f);
    monitors[1].MainSize = ImVec2(1280.0f, 720.0f);
    monitors[1].WorkPos = ImVec2(1920.0f, 0.0f);
    monitors[1].WorkSize = ImVec2(1280.0f, 680.0f);

    const specforge::PlatformWorkArea area =
        specforge::ResolvePlatformWorkArea(
            detached_viewport,
            std::span<const ImGuiPlatformMonitor>(monitors));

    Require(
        area.position.x == 1920.0f &&
            area.position.y == 0.0f,
        "detached Settings should resolve the closest monitor work position");
    Require(
        area.size.x == 1280.0f &&
            area.size.y == 680.0f,
        "detached Settings constraints should follow the closest monitor work size");
}

void TestEmbeddedLegalDocumentInlineDisclosures()
{
    ScopedImGuiContext imgui;
    ImGui::GetStyle().FontScaleMain = 1.5f;

    specforge::SettingsPanelUi panel = MakePanel();
    specforge::SettingsPanelUiTestAccess::SelectSection(
        panel,
        specforge::SettingsSection::About);
    panel.Open();

    LegalRenderObservation observation =
        RenderLegalFrame(panel, kLegalDocumentUiFixtures[0]);
    observation =
        RenderLegalFrame(panel, kLegalDocumentUiFixtures[0]);
    Require(
        observation.open_disclosure_count == 0 &&
            !observation.any_popup_open,
        "embedded legal disclosures should start collapsed without a popup");
    for (const LegalDocumentUiFixture& fixture :
         kLegalDocumentUiFixtures) {
        (void)FindLegalEntryPosition(panel, fixture);
        observation = RenderLegalFrame(panel, fixture);
        Require(
            observation.entry_id != 0 &&
                !observation.document_content_found,
            "each legal document should be presented by an inline collapsed header");
    }

    const LegalDocumentUiFixture& third_party =
        kLegalDocumentUiFixtures[0];
    observation = ClickLegalPosition(
        panel,
        third_party,
        FindLegalEntryPosition(panel, third_party));
    Require(
        observation.document_content_found &&
            observation.document_content_borderless &&
            observation.document_scroll_max_y > 0.0f &&
            observation.open_disclosure_count == 1 &&
            !observation.any_input_text_state &&
            !observation.any_popup_open,
        "expanding a legal disclosure should render one borderless scrollable text child");
    Require(
        observation.document_content_height < 500.0f,
        "the expanded legal document child should remain bounded at a narrow viewport");

    ImGui::GetIO().AddMousePosEvent(
        observation.document_content_center.x,
        observation.document_content_center.y);
    observation = RenderLegalFrame(panel, third_party);
    const float initial_scroll_y = observation.document_scroll_y;
    ImGui::GetIO().AddMouseWheelEvent(0.0f, -8.0f);
    observation = RenderLegalFrame(panel, third_party);
    Require(
        observation.document_scroll_y > initial_scroll_y &&
            observation.open_disclosure_count == 1,
        "scrolling inside a legal disclosure should not collapse it");

    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    observation = RenderLegalFrame(panel, third_party);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    observation = RenderLegalFrame(panel, third_party);
    Require(
        observation.open_disclosure_count == 1,
        "a primary click inside the legal document should not collapse it");

    const ImVec2 copy_position =
        FindLegalCopyPosition(panel, third_party);
    constexpr char kClipboardSentinel[] =
        "SpecForge Legal copy button sentinel";
    ImGui::SetClipboardText(kClipboardSentinel);
    observation = ClickLegalPosition(panel, third_party, copy_position);
    Require(
        imgui.clipboard_text() ==
            specforge::EmbeddedLegalDocumentContent(
                third_party.document) &&
            observation.open_disclosure_count == 1,
        "Copy Document should copy complete legal content without collapsing the disclosure");

    const LegalDocumentUiFixture& data_sources =
        kLegalDocumentUiFixtures[1];
    observation = ClickLegalPosition(
        panel,
        data_sources,
        FindLegalEntryPosition(panel, data_sources));
    Require(
        observation.document_content_found &&
            observation.document_scroll_max_y > 0.0f &&
            observation.open_disclosure_count == 1 &&
            !observation.any_popup_open,
        "clicking the other legal header should switch directly to that document");
    const LegalRenderObservation third_party_after_switch =
        RenderLegalFrame(panel, third_party);
    Require(
        third_party_after_switch.open_disclosure_count == 1 &&
            !third_party_after_switch.document_content_found,
        "switching legal documents should leave only the selected disclosure open");

    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    (void)RenderLegalFrame(panel, data_sources);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    observation = RenderLegalFrame(panel, data_sources);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    observation = RenderLegalFrame(panel, data_sources);
    Require(
        observation.open_disclosure_count == 0,
        "a primary click outside the expanded legal disclosure should collapse it");

    observation = ClickLegalPosition(
        panel,
        third_party,
        FindLegalEntryPosition(panel, third_party));
    Require(
        observation.open_disclosure_count == 1,
        "the legal disclosure should be expandable again after auto-collapse");

    specforge::SettingsPanelUiTestAccess::Close(panel);
    (void)RenderLegalFrame(panel, third_party);
    panel.Open();
    observation = RenderLegalFrame(panel, third_party);
    Require(
        observation.open_disclosure_count == 0 &&
            !observation.document_content_found,
        "closing Settings and reopening it should reset legal disclosure state");

    observation = ClickLegalPosition(
        panel,
        data_sources,
        FindLegalEntryPosition(panel, data_sources));
    Require(
        observation.open_disclosure_count == 1 &&
            observation.document_content_found,
        "the legal disclosure should be open before the Settings-window outside-click check");
    observation = RenderLegalFrame(panel, data_sources);
    Require(
        observation.settings_navigation_window != nullptr,
        "the Settings navigation child should be available for the outside-click check");
    Require(
        observation.settings_content_window != nullptr &&
            observation.settings_navigation_window->RootWindow ==
                observation.settings_content_window->RootWindow,
        "the outside-click fixture should use a child of the Settings root window");
    const ImVec2 settings_window_outside_position(
        observation.settings_navigation_window->Pos.x +
            observation.settings_navigation_window->Size.x * 0.5f,
        observation.settings_navigation_window->Pos.y +
            observation.settings_navigation_window->Size.y - 4.0f);
    observation = ClickLegalPosition(
        panel,
        data_sources,
        settings_window_outside_position);
    Require(
        observation.open_disclosure_count == 0 &&
            !observation.document_content_found &&
            specforge::SettingsPanelUiTestAccess::SelectedSection(panel) ==
                specforge::SettingsSection::About,
        "a click inside Settings but outside the disclosure should collapse it");

    specforge::SettingsPanelUiTestAccess::SelectSection(
        panel,
        specforge::SettingsSection::Diagnostics);
    (void)RenderLegalFrame(panel, third_party);
    specforge::SettingsPanelUiTestAccess::SelectSection(
        panel,
        specforge::SettingsSection::About);
    observation = RenderLegalFrame(panel, third_party);
    Require(
        observation.open_disclosure_count == 0 &&
            !observation.document_content_found,
        "leaving About should clear transient legal disclosure state");
}

}  // namespace

int main()
{
    TestDefaultEnvironmentDescribesThisBuild();
    TestWorkingTreeBuildSourcePresentation();
    TestHeadBuildSourcePresentation();
    TestChineseBuildAndDiagnosticsPresentation();
    TestArtifactIdentityVerification();
    TestAboutArtifactPresentationMatrix();
    TestArtifactIdentityRetainsHashWhenMetadataUnavailable();
    TestArtifactIdentityIsComputedOnAboutDemand();
    TestArtifactIdentityRetriesAfterHashFailure();
    TestOpenIsIdempotent();
    TestDetachedPlatformWindowFocusUsesBackendCallback();
    TestClosedToOpenClearsTransientFeedback();
    TestProfileResetEmitsOneShotSettingsIntent();
    TestWarnedFallbacksRemainDirectlyRepairable();
    TestUiScaleControlEmitsOneShotSettingsIntent();
    TestLiveNumericNavigationCheckboxEmitsOneShotSettingsIntent();
    TestExternalSourceFolderCheckboxAndDeferredSubfolderPlaceholder();
    TestUiScaleSliderCommitsOnlyAfterEditDeactivation();
    TestLocalizedUiScaleResetEmitsDefaultIntent();
    TestLanguageSelectorEmitsOneShotIntent();
    TestLanguageRenderKeepsStableImGuiIds();
    TestRenderSmoke();
    TestSettingsWindowMinimumSizeTracksUiScale();
    TestSettingsWindowConstraintsFollowCurrentViewport();
    TestSettingsWindowCanGrowAfterDetachedViewportShrink();
    TestSettingsDetachedConstraintsFollowClosestMonitor();
    TestEmbeddedLegalDocumentInlineDisclosures();
    std::cout << "settings panel tests passed\n";
    return 0;
}
