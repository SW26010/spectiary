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

#ifndef SPECTIARY_EXPECTED_VERSION
#error "SPECTIARY_EXPECTED_VERSION must be provided by the build configuration."
#endif

#ifndef SPECTIARY_EXPECTED_SOURCE_MODE
#error "SPECTIARY_EXPECTED_SOURCE_MODE must be provided by the build configuration."
#endif

#ifndef SPECTIARY_EXPECTED_CONFIGURATION
#error "SPECTIARY_EXPECTED_CONFIGURATION must be provided by the build configuration."
#endif

#ifndef SPECTIARY_EXPECTED_ARCHITECTURE
#error "SPECTIARY_EXPECTED_ARCHITECTURE must be provided by the build configuration."
#endif

#ifndef SPECTIARY_EXPECTED_SOURCE_REVISION
#error "SPECTIARY_EXPECTED_SOURCE_REVISION must be provided by the build configuration."
#endif

namespace spectiary {

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

}  // namespace spectiary

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

spectiary::SettingsPanelUi MakePanel()
{
    return spectiary::SettingsPanelUi({
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

spectiary::ApplicationSettingsView MakeSettingsView(
    spectiary::UiLanguage language =
        spectiary::UiLanguage::English)
{
    return {
        .language = language,
        .profile_output_directory = "Data/logs",
        .default_profile_output_directory = "Data/logs",
    };
}

void RenderSettingsFrame(spectiary::SettingsPanelUi& panel)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    ImGui::NewFrame();
    panel.Render(MakeSettingsView());
    ImGui::EndFrame();
}

spectiary::BuildMetadataReadResult MakeArtifactMetadata(
    std::string sha256)
{
    spectiary::BuildMetadata metadata;
    metadata.finalized_artifact = spectiary::FinalizedArtifactMetadata{
        .completed_at_utc = "2026-08-05T09:21:32Z",
        .artifact = spectiary::BuildArtifactMetadata{
            .file = "Spectiary.exe",
            .sha256 = std::move(sha256),
        },
    };
    return {
        .status = spectiary::BuildMetadataStatus::Available,
        .metadata = std::move(metadata),
    };
}





struct LegalDocumentUiFixture {
    spectiary::LegalDocument document;
    const char* entry_label;
    const char* content_child_id;
};

constexpr LegalDocumentUiFixture kLegalDocumentUiFixtures[] = {
    {
        spectiary::LegalDocument::ThirdPartyNotices,
        "Third-Party Notices###OpenThirdPartyNotices",
        "##ThirdPartyNoticesContent",
    },
    {
        spectiary::LegalDocument::DataSources,
        "Data Sources###OpenDataSources",
        "##DataSourcesContent",
    },
};

struct LegalRenderObservation {
    ImGuiID entry_id = 0;
    bool entry_hovered = false;
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
    spectiary::SettingsPanelUi& panel,
    const LegalDocumentUiFixture& fixture,
    ImVec2 display_size = ImVec2(700.0f, 500.0f))
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = display_size;
    ImGui::NewFrame();
    spectiary::ApplicationSettingsView settings =
        MakeSettingsView();
    settings.ui_scale_percentage = 150;
    panel.Render(settings);

    LegalRenderObservation observation;
    ImGuiWindow* settings_window = ImGui::FindWindowByName(
        "Settings###SettingsV1");
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
    spectiary::SettingsPanelUi& panel,
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

LegalRenderObservation ClickLegalPosition(
    spectiary::SettingsPanelUi& panel,
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

void TestDefaultEnvironmentDescribesThisBuild()
{
    const spectiary::SettingsPanelEnvironment environment =
        spectiary::SettingsPanelEnvironmentForStartup(
            spectiary::DefaultSpectiaryStartup());

    Require(
        environment.version == SPECTIARY_EXPECTED_VERSION,
        "settings should expose the CMake project version");
    Require(
        !environment.distribution.empty(),
        "settings should expose the distribution");
    Require(
        environment.configuration == SPECTIARY_EXPECTED_CONFIGURATION,
        "settings should expose the actual build configuration");
    Require(
        environment.target_architecture == SPECTIARY_EXPECTED_ARCHITECTURE,
        "settings should expose the target architecture");
    Require(
        !environment.executable_path.empty(),
        "settings should expose the current executable path");
    Require(
        environment.build_source.mode ==
            SPECTIARY_EXPECTED_SOURCE_MODE,
        "settings should expose the configured build source mode");
    Require(
        environment.build_source.revision ==
            SPECTIARY_EXPECTED_SOURCE_REVISION,
        "settings should expose the configured build source revision");
    Require(
        !environment.data_directory.empty(),
        "settings should expose the application data directory");
}

void TestWorkingTreeBuildSourcePresentation()
{
    const spectiary::SettingsPanelEnvironment environment = {
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
        spectiary::FormatBuildSourceForAbout(
            environment.build_source) ==
            "Source: Working tree",
        "working-tree About text should identify the working tree");

    const std::string diagnostics =
        spectiary::FormatDiagnosticInformation(
            environment,
            "Data/logs");
    Require(
        diagnostics ==
            "Spectiary test-version\n"
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
    const spectiary::SettingsPanelEnvironment environment = {
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
        spectiary::FormatBuildSourceForAbout(
            environment.build_source) ==
            "Source: 0123456789abcdef0123456789abcdef01234567",
        "HEAD About text should show the full Git revision without a "
        "relative HEAD label");
    Require(
        spectiary::FormatBuildSourceForAbout(
            environment.build_source,
            spectiary::UiLanguage::SimplifiedChinese) ==
            "源码：0123456789abcdef0123456789abcdef01234567",
        "Chinese HEAD About text should show the full Git revision without "
        "a relative HEAD label");

    const std::string diagnostics =
        spectiary::FormatDiagnosticInformation(
            environment,
            "Data/logs");
    Require(
        diagnostics ==
            "Spectiary test-version\n"
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
    const spectiary::SettingsPanelEnvironment environment = {
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
        spectiary::FormatBuildSourceForAbout(
            environment.build_source,
            spectiary::UiLanguage::
                SimplifiedChinese) ==
            "源码：工作树",
        "Chinese About source text should be exact");
    Require(
        spectiary::FormatProfileOutputDirectoryStatus(
            spectiary::ApplicationSettingsStatusKind::
                PersistenceError,
            spectiary::UiLanguage::
                SimplifiedChinese) ==
            "无法保存性能分析输出目录。",
        "Chinese profile persistence status should be exact");
    Require(
        spectiary::FormatApplicationSettingsStatusReason(
            spectiary::ApplicationSettingsStatusReason::
                SavedValueUnreadable,
            spectiary::UiLanguage::
                SimplifiedChinese) ==
            "已保存的值无效或无法读取。",
        "Chinese settings failure reason should be exact");
    Require(
        spectiary::FormatApplicationSettingsStatusReason(
            spectiary::ApplicationSettingsStatusReason::
                SettingsWriteFailed,
            spectiary::UiLanguage::
                SimplifiedChinese) ==
            "无法写入设置文件。",
        "Chinese settings persistence reason should be exact");

    const std::string diagnostics =
        spectiary::FormatDiagnosticInformation(
            environment,
            "Data/logs",
            spectiary::UiLanguage::
                SimplifiedChinese);
    Require(
        diagnostics ==
            "Spectiary test-version\n"
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
        "spectiary-settings-artifact-identity";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
    const std::filesystem::path executable_path =
        root / "Spectiary.exe";
    {
        std::ofstream stream(executable_path, std::ios::binary);
        Require(stream.good(), "artifact identity fixture should open");
        stream << "abc";
    }

    const spectiary::ArtifactIdentityResult available =
        spectiary::VerifyExecutableArtifactIdentity(
            executable_path,
            MakeArtifactMetadata(std::string(kAbcSha256)));
    Require(
        available.status ==
                spectiary::ArtifactIdentityStatus::Available &&
            available.completed_at_utc ==
                "2026-08-05T09:21:32Z" &&
            available.executable_sha256 == kAbcSha256,
        "matching executable and metadata digests should be available");

    const std::string mismatched_sha256(64, '0');
    const spectiary::ArtifactIdentityResult mismatch =
        spectiary::VerifyExecutableArtifactIdentity(
            executable_path,
            MakeArtifactMetadata(mismatched_sha256));
    Require(
        mismatch.status ==
                spectiary::ArtifactIdentityStatus::Mismatch &&
            mismatch.completed_at_utc.empty() &&
            mismatch.executable_sha256 == kAbcSha256,
        "different executable and metadata digests should reject metadata "
        "without exposing an unverified completion time");

    const spectiary::ArtifactIdentityResult unavailable =
        spectiary::VerifyExecutableArtifactIdentity(
            root / "missing.exe",
            MakeArtifactMetadata(std::string(kAbcSha256)));
    Require(
        unavailable.status ==
                spectiary::ArtifactIdentityStatus::Unavailable &&
            unavailable.completed_at_utc.empty() &&
            unavailable.executable_sha256.empty(),
        "an unreadable executable should make identity unavailable without "
        "exposing an unverified completion time");

    const spectiary::ArtifactIdentityResult missing_metadata =
        spectiary::VerifyExecutableArtifactIdentity(
            executable_path,
            {});
    Require(
        missing_metadata.status ==
                spectiary::ArtifactIdentityStatus::Unavailable &&
            missing_metadata.completed_at_utc.empty() &&
            missing_metadata.executable_sha256 == kAbcSha256,
        "missing metadata should hide metadata fields while retaining the "
        "running executable digest");

    spectiary::BuildMetadataReadResult malformed_metadata =
        MakeArtifactMetadata(std::string(kAbcSha256));
    malformed_metadata.metadata->finalized_artifact->artifact.sha256 =
        "not-a-sha256";
    const spectiary::ArtifactIdentityResult malformed =
        spectiary::VerifyExecutableArtifactIdentity(
            executable_path,
            malformed_metadata);
    Require(
        malformed.status ==
                spectiary::ArtifactIdentityStatus::Unavailable &&
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

    const spectiary::ArtifactIdentityResult matching_identity = {
        .status = spectiary::ArtifactIdentityStatus::Available,
        .completed_at_utc = "2026-08-05T09:21:32Z",
        .executable_sha256 = std::string(kAbcSha256),
    };
    const spectiary::AboutArtifactPresentation matching =
        spectiary::AboutArtifactPresentationFor(
            matching_identity,
            MakeArtifactMetadata(std::string(kAbcSha256)));
    Require(
        matching.show_executable_sha256 &&
            matching.show_metadata_derived_fields &&
            matching.show_completed_at_utc &&
            matching.show_third_party_versions &&
            !matching.show_third_party_fallback,
        "matching metadata should expose the executable hash and trusted metadata fields");

    const spectiary::ArtifactIdentityResult missing_identity = {
        .status = spectiary::ArtifactIdentityStatus::Unavailable,
        .executable_sha256 = std::string(kAbcSha256),
    };
    const spectiary::AboutArtifactPresentation missing =
        spectiary::AboutArtifactPresentationFor(
            missing_identity,
            {});
    Require(
        missing.show_executable_sha256 &&
            !missing.show_metadata_derived_fields &&
            !missing.show_completed_at_utc &&
            !missing.show_third_party_versions &&
            missing.show_third_party_fallback,
        "missing metadata should retain the executable hash and static component fallback without status text");

    spectiary::BuildMetadataReadResult malformed_metadata =
        MakeArtifactMetadata(std::string(kAbcSha256));
    malformed_metadata.metadata->finalized_artifact->artifact.sha256 =
        "not-a-sha256";
    const spectiary::ArtifactIdentityResult malformed_identity = {
        .status = spectiary::ArtifactIdentityStatus::Unavailable,
        .executable_sha256 = std::string(kAbcSha256),
    };
    const spectiary::AboutArtifactPresentation malformed =
        spectiary::AboutArtifactPresentationFor(
            malformed_identity,
            malformed_metadata);
    Require(
        malformed.show_executable_sha256 &&
            !malformed.show_metadata_derived_fields &&
            !malformed.show_completed_at_utc &&
            !malformed.show_third_party_versions &&
            malformed.show_third_party_fallback,
        "malformed metadata should omit metadata-derived and verification UI while retaining static component information");

    const spectiary::ArtifactIdentityResult mismatch_identity = {
        .status = spectiary::ArtifactIdentityStatus::Mismatch,
        .executable_sha256 = std::string(kAbcSha256),
    };
    const spectiary::AboutArtifactPresentation mismatch =
        spectiary::AboutArtifactPresentationFor(
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
        spectiary::UiText(
            spectiary::UiLanguage::English,
            spectiary::UiTextId::DearImGuiComponentFallback) ==
            "Dear ImGui (docking / Win32 / DirectX 11) - MIT License" &&
            spectiary::UiText(
                spectiary::UiLanguage::English,
                spectiary::UiTextId::ImPlotComponentFallback) ==
            "ImPlot - MIT License" &&
            spectiary::UiText(
                spectiary::UiLanguage::English,
                spectiary::UiTextId::CfitsioComponentFallback) ==
            "CFITSIO - NASA License" &&
            spectiary::UiText(
                spectiary::UiLanguage::English,
                spectiary::UiTextId::YamlCppComponentFallback) ==
            "yaml-cpp - MIT License" &&
            spectiary::UiText(
                spectiary::UiLanguage::English,
                spectiary::UiTextId::ZlibComponentFallback) ==
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
             spectiary::UiTextId::Count);
         ++text_index) {
        const auto text_id = static_cast<spectiary::UiTextId>(text_index);
        const std::string_view english = spectiary::UiText(
            spectiary::UiLanguage::English,
            text_id);
        const std::string_view simplified_chinese = spectiary::UiText(
            spectiary::UiLanguage::SimplifiedChinese,
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
        "spectiary-settings-artifact-identity-metadata-gate";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
    const std::filesystem::path executable_path =
        root / "Spectiary.exe";
    {
        std::ofstream stream(executable_path, std::ios::binary);
        Require(stream.good(), "metadata gate fixture should open");
        stream << "abc";
    }

    spectiary::SettingsPanelUi panel({
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
    spectiary::SettingsPanelUiTestAccess::SelectSection(
        panel,
        spectiary::SettingsSection::About);
    RenderSettingsFrame(panel);
    const spectiary::ArtifactIdentityResult first_frame =
        spectiary::SettingsPanelUiTestAccess::ArtifactIdentityResultForTest(
            panel);
    Require(
        first_frame.executable_sha256 == kAbcSha256,
        "the first About frame should already show the running executable hash");

    bool metadata_unavailable = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        RenderSettingsFrame(panel);
        const spectiary::ArtifactIdentityResult identity =
            spectiary::SettingsPanelUiTestAccess::ArtifactIdentityResultForTest(
                panel);
        if (identity.status ==
            spectiary::ArtifactIdentityStatus::Unavailable) {
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
    const spectiary::ArtifactIdentityResult after_metadata_retry_window =
        spectiary::SettingsPanelUiTestAccess::ArtifactIdentityResultForTest(
            panel);
    Require(
        after_metadata_retry_window.status ==
                spectiary::ArtifactIdentityStatus::Unavailable &&
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
        "spectiary-settings-artifact-identity-demand";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
    const std::filesystem::path executable_path =
        root / "Spectiary.exe";
    {
        std::ofstream stream(executable_path, std::ios::binary);
        Require(stream.good(), "on-demand identity fixture should open");
        stream << "abc";
    }

    spectiary::SettingsPanelUi panel({
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
        !spectiary::SettingsPanelUiTestAccess::
            ArtifactIdentityEvaluated(panel),
        "non-About rendering should not hash the executable");

    spectiary::SettingsPanelUiTestAccess::SelectSection(
        panel,
        spectiary::SettingsSection::About);
    RenderSettingsFrame(panel);
    Require(
        spectiary::SettingsPanelUiTestAccess::
            ArtifactIdentityEvaluated(panel),
        "About rendering should evaluate artifact identity on demand");
    Require(
        spectiary::SettingsPanelUiTestAccess::ArtifactIdentityStatusForTest(
            panel) == spectiary::ArtifactIdentityStatus::Pending,
        "the first About frame should publish pending identity verification");
    Require(
        spectiary::SettingsPanelUiTestAccess::ArtifactIdentityResultForTest(
            panel)
                .executable_sha256 == kAbcSha256,
        "the first About frame should expose the running executable hash");

    bool identity_available = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        RenderSettingsFrame(panel);
        if (spectiary::SettingsPanelUiTestAccess::
                ArtifactIdentityStatusForTest(panel) ==
            spectiary::ArtifactIdentityStatus::Available) {
            identity_available = true;
            break;
        }
    }
    Require(
        identity_available,
        "background identity verification should eventually complete");

    const spectiary::ArtifactIdentityResult identity =
        spectiary::SettingsPanelUiTestAccess::ArtifactIdentityResultForTest(
            panel);
    Require(
        identity.executable_sha256 == kAbcSha256 &&
        identity.completed_at_utc == "2026-08-05T09:21:32Z",
        "background identity verification should publish the verified completion time");

    RenderSettingsFrame(panel);
    const spectiary::ArtifactIdentityResult identity_after_next_frame =
        spectiary::SettingsPanelUiTestAccess::ArtifactIdentityResultForTest(
            panel);
    Require(
        identity_after_next_frame.status ==
                spectiary::ArtifactIdentityStatus::Available &&
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
        "spectiary-settings-artifact-identity-retry";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
    const std::filesystem::path executable_path =
        root / "Spectiary.exe";

    spectiary::SettingsPanelUi panel({
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
    spectiary::SettingsPanelUiTestAccess::SelectSection(
        panel,
        spectiary::SettingsSection::About);
    RenderSettingsFrame(panel);
    Require(
        spectiary::SettingsPanelUiTestAccess::ArtifactIdentityStatusForTest(
            panel) == spectiary::ArtifactIdentityStatus::Pending,
        "a failed identity check should begin in the pending state");

    bool identity_unavailable = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        RenderSettingsFrame(panel);
        if (spectiary::SettingsPanelUiTestAccess::
                ArtifactIdentityStatusForTest(panel) ==
            spectiary::ArtifactIdentityStatus::Unavailable) {
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
        if (spectiary::SettingsPanelUiTestAccess::
                ArtifactIdentityStatusForTest(panel) ==
            spectiary::ArtifactIdentityStatus::Available) {
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
    spectiary::SettingsPanelUi panel = MakePanel();

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
        ImHashStr("SpectiarySettingsFocusViewport");
    detached_viewport.PlatformWindowCreated = true;

    focused_settings_platform_viewport = nullptr;
    ImGui::GetPlatformIO().Platform_SetWindowFocus =
        RecordSettingsPlatformFocus;
    spectiary::SettingsPanelUiTestAccess::
        RequestPlatformWindowFocus(detached_viewport);
    Require(
        focused_settings_platform_viewport ==
            &detached_viewport,
        "detached Settings focus should use the native platform callback");
}

void TestClosedToOpenClearsTransientFeedback()
{
    spectiary::SettingsPanelUi panel = MakePanel();

    panel.Open();
    spectiary::SettingsPanelUiTestAccess::SetTransientFeedback(
        panel,
        "old feedback",
        true);
    panel.Open();
    Require(
        spectiary::SettingsPanelUiTestAccess::
            TransientFeedback(panel) == "old feedback",
        "refocusing an open panel should preserve current feedback");

    spectiary::SettingsPanelUiTestAccess::Close(panel);
    panel.Open();
    Require(
        spectiary::SettingsPanelUiTestAccess::
            TransientFeedback(panel).empty(),
        "reopening a closed panel should clear stale feedback");
    Require(
        !spectiary::SettingsPanelUiTestAccess::
            ActionFailed(panel),
        "reopening a closed panel should clear stale failure state");
}

void TestWarnedFallbacksRemainDirectlyRepairable()
{
    spectiary::ApplicationSettingsView settings =
        MakeSettingsView();
    settings.statuses[static_cast<std::size_t>(
        spectiary::ApplicationSetting::Language)] = {
        .kind =
            spectiary::ApplicationSettingsStatusKind::
                LoadWarning,
        .setting = spectiary::ApplicationSetting::Language,
    };
    settings.statuses[static_cast<std::size_t>(
        spectiary::ApplicationSetting::
            ProfileOutputDirectory)] = {
        .kind =
            spectiary::ApplicationSettingsStatusKind::
                LoadWarning,
        .setting =
            spectiary::ApplicationSetting::
                ProfileOutputDirectory,
    };

    Require(
        spectiary::SettingsPanelUiTestAccess::
            ShouldSubmitLanguageSelection(
                settings,
                spectiary::UiLanguage::English),
        "the selected warned language fallback should remain directly selectable for repair");
    Require(
        spectiary::SettingsPanelUiTestAccess::
            CanRestoreProfileOutputDirectory(settings),
        "the warned default profile fallback should keep Restore Default enabled");
    Require(
        spectiary::FormatProfileOutputDirectoryStatus(
            spectiary::ApplicationSettingsStatusKind::
                LoadWarning)
                .find("could not be loaded") !=
            std::string_view::npos,
        "profile load warnings should describe fallback loading rather than an update failure");
    Require(
        spectiary::FormatProfileOutputDirectoryStatus(
            spectiary::ApplicationSettingsStatusKind::
                PersistenceError)
                .find("could not be saved") !=
            std::string_view::npos,
        "profile persistence failures should retain distinct save wording");
}

void TestAppearanceThemeOptionsKeepStableOrderAndMapping()
{
    const std::span<const spectiary::AppearanceThemeOption> options =
        spectiary::AppearanceThemeOptions();
    const std::array expected_text_ids = {
        spectiary::UiTextId::FollowSystemTheme,
        spectiary::UiTextId::DarkTheme,
        spectiary::UiTextId::LightTheme,
    };
    const std::array expected_selections = {
        spectiary::ThemeSelection::FollowSystem(),
        spectiary::ThemeSelection::Explicit(
            spectiary::BuiltInDarkThemeId()),
        spectiary::ThemeSelection::Explicit(
            spectiary::BuiltInLightThemeId()),
    };
    Require(
        options.size() == expected_selections.size(),
        "appearance should expose follow-system, dark, and light options");

    for (std::size_t index = 0;
         index < expected_selections.size();
         ++index) {
        const std::optional<int> mapped_index =
            spectiary::AppearanceThemeOptionIndex(
                expected_selections[index]);
        const std::optional<spectiary::ThemeSelection> mapped_selection =
            spectiary::AppearanceThemeSelectionAt(
                static_cast<int>(index));
        Require(
            options[index].text_id == expected_text_ids[index] &&
                options[index].selection ==
                    expected_selections[index] &&
                mapped_index == static_cast<int>(index) &&
                mapped_selection == expected_selections[index],
            "appearance theme labels and bidirectional index mapping should share one stable order");
    }

    Require(
        !spectiary::AppearanceThemeSelectionAt(-1) &&
            !spectiary::AppearanceThemeSelectionAt(
                static_cast<int>(options.size())) &&
            !spectiary::AppearanceThemeOptionIndex(
                spectiary::ThemeSelection::Explicit(
                    spectiary::ThemeId(
                        "spectiary.theme.synthetic"))),
        "appearance theme mapping should reject indexes and identities outside the option table");
}






void TestLanguageRenderKeepsStableImGuiIds()
{
    ScopedImGuiContext imgui;
    spectiary::SettingsPanelUi panel = MakePanel();
    spectiary::SettingsPanelUiTestAccess::SelectSection(
        panel,
        spectiary::SettingsSection::Language);
    panel.Open();

    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    ImGui::NewFrame();
    panel.Render(
        MakeSettingsView(spectiary::UiLanguage::English));
    ImGuiWindow* english_window =
        ImGui::FindWindowByName(
            "Settings###SettingsV1");
    Require(
        english_window != nullptr,
        "English render should create the settings window");
    const ImGuiID english_window_id = english_window->ID;
    ImGui::EndFrame();

    ImGui::NewFrame();
    panel.Render(
        MakeSettingsView(
            spectiary::UiLanguage::SimplifiedChinese));
    ImGuiWindow* chinese_window =
        ImGui::FindWindowByName(
            "设置###SettingsV1");
    Require(
        chinese_window != nullptr &&
            chinese_window->ID == english_window_id,
        "localized titles should retain one ImGui window ID");
    ImGui::EndFrame();

    Require(
        ImHashStr(
            "Language###SettingsLanguage") ==
            ImHashStr(
                "语言###SettingsLanguage"),
        "localized Language labels should retain one ImGui ID");
    const std::string appearance_english =
        spectiary::SettingsPanelUiTestAccess::SectionLabel(
            spectiary::SettingsSection::Appearance,
            spectiary::UiLanguage::English);
    const std::string appearance_chinese =
        spectiary::SettingsPanelUiTestAccess::SectionLabel(
            spectiary::SettingsSection::Appearance,
            spectiary::UiLanguage::SimplifiedChinese);
    Require(
        appearance_english ==
                "Appearance###SettingsAppearance" &&
            appearance_chinese ==
                "外观###SettingsAppearance",
        "Appearance navigation should use the production stable suffix");
    Require(
        ImHashStr(appearance_english.c_str()) ==
            ImHashStr(appearance_chinese.c_str()),
        "localized Appearance labels should retain one ImGui ID");
    const std::string data_and_recovery_english =
        spectiary::SettingsPanelUiTestAccess::SectionLabel(
            spectiary::SettingsSection::DataAndRecovery,
            spectiary::UiLanguage::English);
    constexpr std::string_view kVisibleDataAndRecovery =
        "Data & Recovery";
    const float visible_width =
        spectiary::SettingsPanelUiTestAccess::
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
        spectiary::SettingsSection::General,
        spectiary::SettingsSection::Input,
        spectiary::SettingsSection::DataAndRecovery,
        spectiary::SettingsSection::Diagnostics,
        spectiary::SettingsSection::About,
    };
    for (const spectiary::SettingsSection section :
         kRemainingSections) {
        const std::string english =
            spectiary::SettingsPanelUiTestAccess::
                SectionLabel(
                    section,
                    spectiary::UiLanguage::English);
        const std::string chinese =
            spectiary::SettingsPanelUiTestAccess::
                SectionLabel(
                    section,
                    spectiary::UiLanguage::
                        SimplifiedChinese);
        Require(
            ImHashStr(english.c_str()) ==
                ImHashStr(chinese.c_str()),
            "localized settings sections should retain stable ImGui IDs");
    }

    const std::string theme_english =
        spectiary::SettingsPanelUiTestAccess::
            AppearanceThemeLabel(
                spectiary::UiLanguage::English);
    const std::string theme_chinese =
        spectiary::SettingsPanelUiTestAccess::
            AppearanceThemeLabel(
                spectiary::UiLanguage::SimplifiedChinese);
    Require(
        theme_english ==
                "Theme###AppearanceTheme" &&
            theme_chinese ==
                "主题###AppearanceTheme",
        "theme controls should use the production stable suffix");
    Require(
        ImHashStr(theme_english.c_str()) ==
            ImHashStr(theme_chinese.c_str()),
        "localized theme controls should retain one ImGui ID");

    const std::string accent_english =
        spectiary::SettingsPanelUiTestAccess::
            AppearanceAccentColorLabel(
                spectiary::UiLanguage::English);
    const std::string accent_chinese =
        spectiary::SettingsPanelUiTestAccess::
            AppearanceAccentColorLabel(
                spectiary::UiLanguage::SimplifiedChinese);
    Require(
        accent_english ==
                "Accent color###"
                "AppearanceAccentColor" &&
            accent_chinese ==
                "强调色###"
                "AppearanceAccentColor",
        "accent color controls should use the production stable suffix");
    Require(
        ImHashStr(accent_english.c_str()) ==
            ImHashStr(accent_chinese.c_str()),
        "localized accent color controls should retain one ImGui ID");
    Require(
        ImHashStr(
            "UI scale###UiScale") ==
            ImHashStr(
                "界面缩放###UiScale"),
        "localized UI scale controls should retain one ImGui ID");
    Require(
        ImHashStr(
            "Reset###UiScaleReset") ==
            ImHashStr(
                "重置###UiScaleReset"),
        "localized reset actions should retain one ImGui ID");
    Require(
        ImHashStr(
            "Application language###"
            "ApplicationLanguage") ==
            ImHashStr(
                "应用语言###"
                "ApplicationLanguage"),
        "localized language selectors should retain one ImGui ID");
    Require(
        ImHashStr(
            "English###UiLanguageEnglish") ==
            ImHashStr(
                "英语###UiLanguageEnglish"),
        "localized language options should retain one ImGui ID");
    Require(
        ImHashStr(
            "Third-Party Notices###OpenThirdPartyNotices") ==
            ImHashStr(
                "第三方声明###OpenThirdPartyNotices"),
        "localized third-party notice disclosure headers should retain one ImGui ID");
    Require(
        ImHashStr(
            "Data Sources###OpenDataSources") ==
            ImHashStr(
                "数据来源###OpenDataSources"),
        "localized data-source disclosure headers should retain one ImGui ID");
}

void TestRenderSmoke()
{
    ScopedImGuiContext imgui;
    spectiary::SettingsPanelUi panel = MakePanel();
    panel.Open();

    constexpr spectiary::SettingsSection sections[] = {
        spectiary::SettingsSection::General,
        spectiary::SettingsSection::Appearance,
        spectiary::SettingsSection::Language,
        spectiary::SettingsSection::Input,
        spectiary::SettingsSection::DataAndRecovery,
        spectiary::SettingsSection::Diagnostics,
        spectiary::SettingsSection::About,
    };
    for (const spectiary::SettingsSection section : sections) {
        spectiary::SettingsPanelUiTestAccess::SelectSection(
            panel,
            section);
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.DisplaySize = ImVec2(1280.0f, 720.0f);
        ImGui::NewFrame();
        panel.Render(MakeSettingsView());
        Require(
            ImGui::FindWindowByName(
                "Settings###SettingsV1") != nullptr,
            "rendering an open settings panel should create its window");
        Require(
            panel.open(),
            "rendering every section should keep the panel open");
        ImGui::EndFrame();
    }
}

void TestSettingsViewportStableAcrossOpeningFrames()
{
    ScopedImGuiContext imgui;
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigViewportsNoAutoMerge = true;
    io.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports |
                       ImGuiBackendFlags_RendererHasViewports;
    io.DisplaySize = ImVec2(1600, 1000);
    io.DeltaTime = 1.0f / 60.0f;
    auto& platform = ImGui::GetPlatformIO();
    platform.Platform_CreateWindow = [](ImGuiViewport* v) { v->PlatformHandle = v; };
    platform.Platform_DestroyWindow = [](ImGuiViewport* v) { v->PlatformHandle = nullptr; };
    platform.Platform_ShowWindow = [](ImGuiViewport*) {};
    platform.Platform_SetWindowTitle = [](ImGuiViewport*, const char*) {};
    platform.Platform_SetWindowPos = [](ImGuiViewport*, ImVec2) {};
    platform.Platform_GetWindowPos = [](ImGuiViewport* v) { return v->Pos; };
    platform.Platform_SetWindowSize = [](ImGuiViewport*, ImVec2) {};
    platform.Platform_GetWindowSize = [](ImGuiViewport* v) { return v->Size; };
    ImGui::GetMainViewport()->PlatformHandle = ImGui::GetMainViewport();
    ImGuiPlatformMonitor monitor;
    monitor.MainSize = monitor.WorkSize = ImVec2(1920, 1080);
    monitor.DpiScale = 1.0f;
    platform.Monitors.push_back(monitor);

    auto panel = MakePanel();
    const auto settings = MakeSettingsView();
    const auto frame = [&] {
        ImGui::NewFrame();
        panel.Render(settings);
        ImGui::EndFrame();
        ImGui::UpdatePlatformWindows();
    };
    panel.Open();
    ImGuiID settings_viewport = 0;
    for (int i = 0; i < 3; ++i) {
        frame();
        const auto* window = ImGui::FindWindowByName("###SettingsV1");
        Require(window && window->ViewportId != ImGui::GetMainViewport()->ID,
            "Settings must own its detached viewport from its opening frame");
        if (i == 0) settings_viewport = window->ViewportId;
        Require(window->ViewportId == settings_viewport,
            "Opening Settings must not migrate between viewports on successive frames");
    }
    panel.Open();
    frame();
    Require(ImGui::FindWindowByName("###SettingsV1")->ViewportId == settings_viewport,
        "Focusing an open Settings panel must retain its detached viewport");
    panel.CloseForLayoutRecovery();
    frame();
    frame();
    panel.Open();
    frame();
    Require(ImGui::FindWindowByName("###SettingsV1")->ViewportId == settings_viewport,
        "Reopening Settings after recovery must immediately use its detached viewport");
    ImGui::DestroyPlatformWindows();
}

void TestSettingsWindowMinimumSizeTracksUiScale()
{
    ScopedImGuiContext imgui;
    spectiary::SettingsPanelUi panel = MakePanel();
    panel.Open();

    spectiary::ApplicationSettingsView settings =
        MakeSettingsView();
    settings.ui_scale_percentage = 150;

    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    ImGui::NewFrame();
    panel.Render(settings);
    ImGuiWindow* window = ImGui::FindWindowByName(
        "Settings###SettingsV1");
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
    spectiary::SettingsPanelUi panel = MakePanel();
    panel.Open();

    spectiary::ApplicationSettingsView settings =
        MakeSettingsView();
    settings.ui_scale_percentage = 150;

    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    ImGui::NewFrame();
    panel.Render(settings);
    ImGuiWindow* window = ImGui::FindWindowByName(
        "Settings###SettingsV1");
    Require(
        window != nullptr &&
            window->Size.x >= 1290.0f &&
            window->Size.y >= 840.0f,
        "fixture should begin with the 150% main-viewport size");
    ImGui::EndFrame();

    ImGuiViewportP secondary_viewport;
    secondary_viewport.ID =
        ImHashStr("SpectiarySettingsSecondaryViewport");
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
    spectiary::SettingsPanelUiTestAccess::SetViewportId(
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
    spectiary::SettingsPanelUi panel = MakePanel();
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
        "Settings###SettingsV1");
    Require(
        window != nullptr,
        "detached resize fixture should create the Settings window");
    ImGui::EndFrame();

    ImGuiViewportP detached_viewport;
    detached_viewport.ID =
        ImHashStr("SpectiarySettingsDetachedViewport");
    detached_viewport.Pos = ImVec2(200.0f, 100.0f);
    detached_viewport.Size = ImVec2(1000.0f, 700.0f);
    detached_viewport.WorkPos = detached_viewport.Pos;
    detached_viewport.WorkSize = detached_viewport.Size;
    detached_viewport.DpiScale = 1.0f;
    detached_viewport.Idx = GImGui->Viewports.Size;

    ImGui::NewFrame();
    GImGui->Viewports.push_back(&detached_viewport);
    spectiary::SettingsPanelUiTestAccess::SetViewportId(
        panel,
        detached_viewport.ID);
    ImGui::SetWindowSize(
        "Settings###SettingsV1",
        detached_viewport.Size,
        ImGuiCond_Always);
    panel.Render(MakeSettingsView());
    ImGui::EndFrame();
    GImGui->Viewports.pop_back();

    constexpr ImVec2 enlarged_size(1200.0f, 800.0f);
    ImGui::NewFrame();
    GImGui->Viewports.push_back(&detached_viewport);
    spectiary::SettingsPanelUiTestAccess::SetViewportId(
        panel,
        detached_viewport.ID);
    ImGui::SetWindowSize(
        "Settings###SettingsV1",
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

    const spectiary::PlatformWorkArea area =
        spectiary::ResolvePlatformWorkArea(
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

    spectiary::SettingsPanelUi panel = MakePanel();
    spectiary::SettingsPanelUiTestAccess::SelectSection(
        panel,
        spectiary::SettingsSection::About);
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

    spectiary::SettingsPanelUiTestAccess::Close(panel);
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
            spectiary::SettingsPanelUiTestAccess::SelectedSection(panel) ==
                spectiary::SettingsSection::About,
        "a click inside Settings but outside the disclosure should collapse it");

    spectiary::SettingsPanelUiTestAccess::SelectSection(
        panel,
        spectiary::SettingsSection::Diagnostics);
    (void)RenderLegalFrame(panel, third_party);
    spectiary::SettingsPanelUiTestAccess::SelectSection(
        panel,
        spectiary::SettingsSection::About);
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
    TestWarnedFallbacksRemainDirectlyRepairable();
    TestAppearanceThemeOptionsKeepStableOrderAndMapping();
    TestLanguageRenderKeepsStableImGuiIds();
    TestRenderSmoke();
    TestSettingsViewportStableAcrossOpeningFrames();
    TestSettingsWindowMinimumSizeTracksUiScale();
    TestSettingsWindowConstraintsFollowCurrentViewport();
    TestSettingsWindowCanGrowAfterDetachedViewportShrink();
    TestSettingsDetachedConstraintsFollowClosestMonitor();
    TestEmbeddedLegalDocumentInlineDisclosures();
    std::cout << "settings panel tests passed\n";
    return 0;
}
