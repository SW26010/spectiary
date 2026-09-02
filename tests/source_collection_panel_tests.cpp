#include "ui/source_collection_panel.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace specforge {

struct SourceCollectionPanelUiTestAccess {
    [[nodiscard]] static bool IsNavigationNumberMove(
        const SourceCollectionSessionIntent& intent)
    {
        return intent.kind ==
                   SourceCollectionSessionIntentKind::
                       SampleNavigation &&
               intent.sample_navigation.kind ==
                   SampleNavigationIntentKind::Move &&
               (intent.sample_navigation.request.kind ==
                    SampleNavigationRequestKind::LocateRow ||
                intent.sample_navigation.request.kind ==
                    SampleNavigationRequestKind::
                        LocateSequencePosition);
    }

    [[nodiscard]] static const SampleNavigationRequest&
    MoveRequest(const SourceCollectionSessionIntent& intent)
    {
        return intent.sample_navigation.request;
    }

    [[nodiscard]] static const std::optional<std::string>&
    SourceLaunchError(const SourceCollectionPanelUi& panel)
    {
        return panel.source_launch_error_;
    }

    [[nodiscard]] static const std::optional<std::array<float, 4>>&
    FirstSourceContextCellRect(
        const SourceCollectionPanelUi& panel)
    {
        return panel.first_source_context_cell_rect_;
    }

    [[nodiscard]] static const std::optional<std::array<float, 4>>&
    SourceContextActionRect(const SourceCollectionPanelUi& panel)
    {
        return panel.source_context_action_rect_;
    }

    [[nodiscard]] static const std::optional<std::array<float, 4>>&
    AnnotationAddFileRect(
        const SourceCollectionPanelUi& panel)
    {
        return panel.annotation_add_file_rect_;
    }

    [[nodiscard]] static const std::vector<std::array<float, 4>>&
    AnnotationDiagnosticDismissRects(
        const SourceCollectionPanelUi& panel)
    {
        return panel.annotation_diagnostic_dismiss_rects_;
    }

    [[nodiscard]] static const std::vector<std::array<float, 4>>&
    AnnotationRemoveRects(
        const SourceCollectionPanelUi& panel)
    {
        return panel.annotation_remove_rects_;
    }

    [[nodiscard]] static const std::optional<std::array<float, 4>>&
    MissingLocalAnnotationRemoveConfirmRect(
        const SourceCollectionPanelUi& panel)
    {
        return panel.missing_local_annotation_remove_confirm_rect_;
    }
};

}  // namespace specforge

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
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
};

void TestFilesPanelAddFileForwardsCsvToInAppOpener()
{
    ScopedImGuiContext context;
    specforge::SourceCollectionSessionView view;
    specforge::PanelSessionInteraction interaction(
        [](specforge::SourceCollectionSessionIntent,
           std::optional<specforge::NavigationLatencyInputKind>) {
            return specforge::SourceCollectionSessionResult{};
        },
        [&view]() -> const specforge::SourceCollectionSessionView& {
            return view;
        });
    specforge::SourceCollectionPanelUi panel;
    const std::filesystem::path selected_path =
        std::filesystem::path{"selected.CSV"};
    std::optional<std::filesystem::path> opened_path;
    int choose_file_count = 0;
    int choose_folder_count = 0;
    bool open = true;

    const auto render_frame = [&]() {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.DisplaySize = ImVec2(900.0f, 700.0f);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(
            ImVec2(20.0f, 20.0f),
            ImGuiCond_Always);
        ImGui::SetNextWindowSize(
            ImVec2(700.0f, 500.0f),
            ImGuiCond_Always);
        panel.RenderFiles(
            interaction,
            specforge::UiLanguage::English,
            &open,
            [&]() -> std::optional<std::filesystem::path> {
                ++choose_file_count;
                return selected_path;
            },
            [&]() -> std::optional<std::filesystem::path> {
                ++choose_folder_count;
                return std::nullopt;
            },
            [&](const std::filesystem::path& path) {
                opened_path = path;
            },
            [](const std::filesystem::path&) -> std::optional<std::string> {
                return std::nullopt;
            });
        ImGui::EndFrame();
    };

    render_frame();
    ImGuiWindow* window = ImGui::FindWindowByName(
        specforge::SourceCollectionPanelUi::FilesWindowName());
    Require(
        window != nullptr,
        "Files panel should render its window for the Add file test");
    const ImGuiID add_file_id = window->GetID(
        "Add file...###SpecForgeFilesAddFile");
    bool hovered = false;
    for (float y = 20.0f;
         y <= 220.0f && !hovered;
         y += 2.0f) {
        for (float x = 20.0f;
             x <= 420.0f && !hovered;
             x += 4.0f) {
            ImGui::GetIO().AddMousePosEvent(x, y);
            render_frame();
            hovered = GImGui->HoveredId == add_file_id;
        }
    }
    Require(
        hovered,
        "Files panel should expose a clickable Add file button");

    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    render_frame();
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    render_frame();
    Require(
        choose_file_count == 1 &&
            choose_folder_count == 0 &&
            opened_path == selected_path,
        "Files panel Add file should forward the selected CSV to its source opener");
}

ImVec2 RectCenter(const std::array<float, 4>& rect)
{
    return ImVec2(
        (rect[0] + rect[2]) * 0.5f,
        (rect[1] + rect[3]) * 0.5f);
}

struct AnnotationPanelFrameObservation {
    std::string logged_text;
    bool popup_open = false;
};

class AnnotationPanelFixture {
public:
    AnnotationPanelFixture()
    {
        snapshot = std::make_shared<specforge::SpectrumSnapshot>();
        snapshot->source.path = "source-a.npy";
        snapshot->collection.spectrum_count = 1;
        snapshot->collection.current_index = 0;
        view.snapshot = snapshot;
        view.current_sample_snapshot = snapshot;
        view.navigation.has_active_source = true;
        view.navigation.current_index = 0;
        view.navigation.sample_count = 1;
        view.labeling.has_active_source = true;
        view.labeling.source_identity = "source-a";
    }

    AnnotationPanelFrameObservation RenderFrame(
        bool capture_text = true)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.DisplaySize = ImVec2(900.0f, 700.0f);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(
            ImVec2(20.0f, 20.0f),
            ImGuiCond_Always);
        ImGui::SetNextWindowSize(
            ImVec2(560.0f, 520.0f),
            ImGuiCond_Always);
        if (capture_text) {
            ImGui::LogToBuffer();
        }
        bool open = true;
        specforge::PanelSessionInteraction interaction(
            [this](
                specforge::SourceCollectionSessionIntent intent,
                std::optional<
                    specforge::NavigationLatencyInputKind>) {
                if (intent.intent_kind() ==
                    specforge::SourceCollectionSessionIntentKind::
                        SourceCollection) {
                    ++submit_count;
                    if (after_submit) {
                        after_submit();
                    }
                }
                specforge::SourceCollectionSessionResult result;
                result.loaded = submit_loaded;
                return result;
            },
            [this]() -> const specforge::SourceCollectionSessionView& {
                return view;
            });
        panel.RenderAnnotations(
            interaction,
            specforge::UiLanguage::English,
            &open,
            [this]() {
                ++choose_file_count;
                return selected_path;
            });

        AnnotationPanelFrameObservation observation;
        if (capture_text) {
            observation.logged_text =
                GImGui->LogBuffer.c_str();
            ImGui::LogFinish();
        }
        observation.popup_open = ImGui::IsPopupOpen(
            nullptr,
            ImGuiPopupFlags_AnyPopupId |
                ImGuiPopupFlags_AnyPopupLevel);
        ImGui::EndFrame();
        return observation;
    }

    std::shared_ptr<specforge::SpectrumSnapshot> snapshot;
    specforge::SourceCollectionSessionView view;
    specforge::SourceCollectionPanelUi panel;
    std::optional<std::filesystem::path> selected_path;
    std::function<void()> after_submit;
    bool submit_loaded = false;
    int choose_file_count = 0;
    int submit_count = 0;
};

void ClickAnnotationPanelRect(
    AnnotationPanelFixture& fixture,
    const std::array<float, 4>& rect)
{
    const ImVec2 center = RectCenter(rect);
    ImGui::GetIO().AddMousePosEvent(center.x, center.y);
    (void)fixture.RenderFrame(false);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)fixture.RenderFrame(false);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    (void)fixture.RenderFrame(false);
}

std::size_t CountOccurrences(
    std::string_view text,
    std::string_view needle)
{
    std::size_t count = 0;
    for (std::size_t offset = 0;
         (offset = text.find(needle, offset)) !=
             std::string_view::npos;
         offset += needle.size()) {
        ++count;
    }
    return count;
}

specforge::SourceCollectionManifestDiagnostic
MakeAnnotationImportFailure(
    std::filesystem::path path,
    std::string detail)
{
    return {
        .kind = specforge::
            SourceCollectionManifestDiagnosticKind::
                AnnotationIgnored,
        .path = std::move(path),
        .detail = std::move(detail),
    };
}

void TestAnnotationImportFailureRendersInlineWithoutHoverOrPopup()
{
    ScopedImGuiContext context;
    AnnotationPanelFixture fixture;
    fixture.view.navigation.annotation_diagnostics.push_back(
        MakeAnnotationImportFailure(
            "initial-labels.csv",
            "CSV roster does not match the active source collection.\nExpected sample B at row 2."));
    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);

    const AnnotationPanelFrameObservation observation =
        fixture.RenderFrame();
    Require(
        observation.logged_text.find("initial-labels.csv") !=
                std::string::npos &&
            observation.logged_text.find(
                "CSV roster does not match the active source collection.") !=
                std::string::npos &&
            observation.logged_text.find(
                "Expected sample B at row 2.") !=
                std::string::npos &&
            observation.logged_text.find("Dismiss") !=
                std::string::npos,
        "annotation import notice should draw filename, full multiline reason, and Dismiss without hover");
    Require(
        !observation.popup_open,
        "annotation import failure should remain inline rather than opening a modal");
}

void TestMissingLocalAnnotationRemovalRequiresConfirmation()
{
    ScopedImGuiContext context;
    AnnotationPanelFixture fixture;
    const std::filesystem::path missing_path =
        std::filesystem::path{"moved-away-labels.asdf"};
    fixture.view.navigation.current_annotations.push_back({
        .name = "Quality review",
        .path = missing_path,
        .relationship = specforge::
            SampleAnnotationWorkflowRelationship::
                LocalLabelingTask,
        .output_missing = true,
        .can_remove_annotation = true,
        .labeling_owner_format = specforge::
            SampleLabelingOutputArtifactFormat::
                CanonicalAsdf,
    });

    (void)fixture.RenderFrame();
    const auto& remove_rects =
        specforge::SourceCollectionPanelUiTestAccess::
            AnnotationRemoveRects(fixture.panel);
    Require(
        remove_rects.size() == 1,
        "a removable missing local task should expose one remove action");
    const std::array<float, 4> remove_rect =
        remove_rects.front();
    ClickAnnotationPanelRect(fixture, remove_rect);

    const AnnotationPanelFrameObservation warning =
        fixture.RenderFrame();
    Require(
        fixture.submit_count == 0 &&
            warning.popup_open &&
            warning.logged_text.find(
                "Delete local task \"Quality review\"") !=
                std::string::npos &&
            warning.logged_text.find(
                "Output files are not deleted.") !=
                std::string::npos,
        "the first remove click should show an explicit warning without abandoning the task");

    const auto confirm_rect =
        specforge::SourceCollectionPanelUiTestAccess::
            MissingLocalAnnotationRemoveConfirmRect(
                fixture.panel);
    Require(
        confirm_rect.has_value(),
        "the missing-local warning should expose a confirmation action");
    ClickAnnotationPanelRect(fixture, *confirm_rect);
    Require(
        fixture.submit_count == 1,
        "confirming the warning should submit the missing local task removal exactly once");
}

void TestSourceSwitchDismissesMissingLocalRemovalWarning()
{
    ScopedImGuiContext context;
    AnnotationPanelFixture fixture;
    fixture.view.navigation.current_annotations.push_back({
        .name = "Old source review",
        .path = "old-source-labels.asdf",
        .relationship = specforge::
            SampleAnnotationWorkflowRelationship::
                LocalLabelingTask,
        .output_missing = true,
        .can_remove_annotation = true,
        .labeling_owner_format = specforge::
            SampleLabelingOutputArtifactFormat::
                CanonicalAsdf,
    });

    (void)fixture.RenderFrame();
    const auto& remove_rects =
        specforge::SourceCollectionPanelUiTestAccess::
            AnnotationRemoveRects(fixture.panel);
    Require(
        remove_rects.size() == 1,
        "the old source should expose its missing-task remove action");
    const std::array<float, 4> remove_rect =
        remove_rects.front();
    ClickAnnotationPanelRect(
        fixture,
        remove_rect);
    Require(
        fixture.RenderFrame().popup_open,
        "the old source removal warning should be open before switching sources");

    fixture.view.labeling.source_identity = "source-b";
    fixture.snapshot->source.path = "source-b.npy";
    fixture.view.navigation.current_annotations.clear();
    const AnnotationPanelFrameObservation switched =
        fixture.RenderFrame();
    Require(
        fixture.submit_count == 0 &&
            !switched.popup_open &&
            switched.logged_text.find(
                "Old source review") ==
                std::string::npos,
        "switching sources should dismiss the stale missing-task warning without submitting its deletion");
}

void TestAnnotationImportFailureDismissalTracksExactDetail()
{
    ScopedImGuiContext context;
    AnnotationPanelFixture fixture;
    fixture.view.navigation.annotation_diagnostics.push_back(
        MakeAnnotationImportFailure(
            "initial-labels.csv",
            "First roster mismatch detail."));
    (void)fixture.RenderFrame();
    const auto& dismiss_rects =
        specforge::SourceCollectionPanelUiTestAccess::
            AnnotationDiagnosticDismissRects(fixture.panel);
    Require(
        dismiss_rects.size() == 1,
        "one visible import failure should expose one Dismiss action");
    const std::array<float, 4> dismiss_rect =
        dismiss_rects.front();
    ClickAnnotationPanelRect(fixture, dismiss_rect);
    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    const AnnotationPanelFrameObservation dismissed =
        fixture.RenderFrame();
    Require(
        dismissed.logged_text.find("initial-labels.csv") ==
                std::string::npos &&
            dismissed.logged_text.find(
                "First roster mismatch detail.") ==
                std::string::npos,
        "dismissed diagnostic should stay hidden for this panel session");

    fixture.view.navigation.annotation_diagnostics.front().detail =
        "Changed roster mismatch detail.";
    const AnnotationPanelFrameObservation changed =
        fixture.RenderFrame();
    Require(
        changed.logged_text.find("initial-labels.csv") !=
                std::string::npos &&
            changed.logged_text.find(
                "Changed roster mismatch detail.") !=
                std::string::npos,
        "a changed diagnostic detail should produce a new visible dismissal key");
}

void TestReimportSameAnnotationClearsDismissalForNewFailure()
{
    ScopedImGuiContext context;
    AnnotationPanelFixture fixture;
    const std::filesystem::path diagnostic_path =
        std::filesystem::path{"imports"} /
        "nested" / ".." / "initial-labels.csv";
    const std::filesystem::path selected_path =
        std::filesystem::path{"imports"} /
        "initial-labels.csv";
    const auto diagnostic = MakeAnnotationImportFailure(
        diagnostic_path,
        "Roster mismatch on retry.");
    fixture.view.navigation.annotation_diagnostics.push_back(
        diagnostic);
    (void)fixture.RenderFrame();
    const std::array<float, 4> dismiss_rect =
        specforge::SourceCollectionPanelUiTestAccess::
            AnnotationDiagnosticDismissRects(fixture.panel)
                .front();
    ClickAnnotationPanelRect(fixture, dismiss_rect);
    Require(
        fixture.RenderFrame().logged_text.find(
            "Roster mismatch on retry.") ==
            std::string::npos,
        "precondition: retry diagnostic should be dismissed");

    fixture.selected_path = selected_path;
    fixture.submit_loaded = false;
    fixture.after_submit = [&fixture, diagnostic]() {
        fixture.view.navigation.annotation_diagnostics.push_back(
            diagnostic);
    };
    const auto add_file_rect =
        specforge::SourceCollectionPanelUiTestAccess::
            AnnotationAddFileRect(fixture.panel);
    Require(
        add_file_rect.has_value(),
        "Annotations panel should expose its Add file action");
    ClickAnnotationPanelRect(fixture, *add_file_rect);
    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    const AnnotationPanelFrameObservation retried =
        fixture.RenderFrame();
    Require(
        fixture.choose_file_count == 1 &&
            fixture.submit_count == 1 &&
            CountOccurrences(
                retried.logged_text,
                "Roster mismatch on retry.") == 1,
        "same normalized path retry should submit once and reveal one latest failure card");

    const std::array<float, 4> retried_dismiss_rect =
        specforge::SourceCollectionPanelUiTestAccess::
            AnnotationDiagnosticDismissRects(fixture.panel)
                .front();
    ClickAnnotationPanelRect(
        fixture,
        retried_dismiss_rect);
    fixture.submit_loaded = true;
    fixture.after_submit = {};
    const auto success_add_rect =
        specforge::SourceCollectionPanelUiTestAccess::
            AnnotationAddFileRect(fixture.panel);
    Require(
        success_add_rect.has_value(),
        "successful retry should retain the Add file action");
    ClickAnnotationPanelRect(fixture, *success_add_rect);
    const AnnotationPanelFrameObservation successful =
        fixture.RenderFrame();
    Require(
        successful.logged_text.find(
            "Roster mismatch on retry.") ==
                std::string::npos &&
            !successful.popup_open,
        "successful retry should keep historical failure hidden without a modal or success toast");
}

void TestAnnotationDismissalsResetAcrossSourceSwitch()
{
    ScopedImGuiContext context;
    AnnotationPanelFixture fixture;
    const auto source_a_failure = MakeAnnotationImportFailure(
        "source-a.csv",
        "Source A failure.");
    fixture.view.navigation.annotation_diagnostics = {
        source_a_failure};
    (void)fixture.RenderFrame();
    const std::array<float, 4> dismiss_rect =
        specforge::SourceCollectionPanelUiTestAccess::
            AnnotationDiagnosticDismissRects(fixture.panel)
                .front();
    ClickAnnotationPanelRect(fixture, dismiss_rect);

    fixture.view.labeling.source_identity = "source-b";
    fixture.snapshot->source.path = "source-b.npy";
    fixture.view.navigation.annotation_diagnostics = {
        MakeAnnotationImportFailure(
            "source-b.csv",
            "Source B failure.")};
    const AnnotationPanelFrameObservation source_b =
        fixture.RenderFrame();
    Require(
        source_b.logged_text.find("Source B failure.") !=
                std::string::npos &&
            source_b.logged_text.find("Source A failure.") ==
                std::string::npos,
        "source switch should show only the active source diagnostic");

    fixture.view.labeling.source_identity = "source-a";
    fixture.snapshot->source.path = "source-a.npy";
    fixture.view.navigation.annotation_diagnostics = {
        source_a_failure};
    const AnnotationPanelFrameObservation source_a_again =
        fixture.RenderFrame();
    Require(
        source_a_again.logged_text.find("Source A failure.") !=
            std::string::npos,
        "switching sources should clear prior-source transient dismissal state");
}

void TestHiddenAnnotationPanelObservesIntermediateSourceSwitch()
{
    ScopedImGuiContext context;
    AnnotationPanelFixture fixture;
    const auto source_a_failure = MakeAnnotationImportFailure(
        "source-a.csv",
        "Source A hidden-panel failure.");
    fixture.view.navigation.annotation_diagnostics = {
        source_a_failure};
    (void)fixture.RenderFrame();
    const std::array<float, 4> dismiss_rect =
        specforge::SourceCollectionPanelUiTestAccess::
            AnnotationDiagnosticDismissRects(fixture.panel)
                .front();
    ClickAnnotationPanelRect(fixture, dismiss_rect);
    Require(
        fixture.RenderFrame().logged_text.find(
            "Source A hidden-panel failure.") ==
            std::string::npos,
        "precondition: source A diagnostic should be dismissed");

    fixture.view.labeling.source_identity = "source-b";
    fixture.snapshot->source.path = "source-b.npy";
    fixture.view.navigation.annotation_diagnostics = {
        MakeAnnotationImportFailure(
            "source-b.csv",
            "Source B hidden-panel failure.")};
    fixture.panel.SyncAnnotationDiagnosticSource(
        fixture.view);

    fixture.view.labeling.source_identity = "source-a";
    fixture.snapshot->source.path = "source-a.npy";
    fixture.view.navigation.annotation_diagnostics = {
        source_a_failure};
    fixture.panel.SyncAnnotationDiagnosticSource(
        fixture.view);

    const AnnotationPanelFrameObservation source_a_again =
        fixture.RenderFrame();
    Require(
        source_a_again.logged_text.find(
            "Source A hidden-panel failure.") !=
            std::string::npos,
        "an unrendered A-to-B-to-A source transition should clear source A dismissal state");
}

void TestReopenableSourcePathEligibility()
{
    Require(
        !specforge::IsReopenableSourcePath({}),
        "an empty source path must not be reopenable");
    Require(
        specforge::IsReopenableSourcePath(
            std::filesystem::path{L"relative folder\\观测 file.npy"}),
        "relative filesystem source paths should be reopenable");
    Require(
        specforge::IsReopenableSourcePath(
            std::filesystem::path{L"C:\\观测 data\\source file.npy"}),
        "Unicode and space-containing filesystem source paths should be reopenable");

    const std::filesystem::path::string_type invalid_native{
        L"source\0path",
        11};
    Require(
        !specforge::IsReopenableSourcePath(
            std::filesystem::path{invalid_native}),
        "a source path containing an embedded NUL must not be reopenable");
}

void TestFilesPanelContextActionLaunchesWithoutMutatingSession()
{
    ScopedImGuiContext context;
    const std::filesystem::path source_path =
        std::filesystem::path{L"C:\\观测 data\\source file.npy"};
    auto snapshot = std::make_shared<specforge::SpectrumSnapshot>();
    snapshot->source.path = source_path;
    snapshot->source.display_name = "source file";
    snapshot->collection.spectrum_count = 4;
    snapshot->collection.current_index = 2;

    specforge::SourceCollectionSessionView view;
    view.snapshot = snapshot;
    view.current_sample_snapshot = snapshot;
    view.sources = {
        {
            source_path,
            "source file",
            std::string{"npy"},
            specforge::SourceCollectionSourceState::Loaded,
        },
    };
    view.current_source_index = 0;
    view.navigation.has_active_source = true;
    view.navigation.current_index = 2;
    view.navigation.current_source_row = 2;
    view.navigation.sample_count = 4;
    view.labeling.has_active_source = true;
    view.labeling.source_identity = "source identity";
    view.sorting.has_active_source = true;
    view.sorting.active_source_id = "source identity";
    const specforge::SourceCollectionSessionView before = view;

    int submit_count = 0;
    int launch_count = 0;
    std::optional<std::filesystem::path> launched_path;
    specforge::PanelSessionInteraction interaction(
        [&](specforge::SourceCollectionSessionIntent,
            std::optional<specforge::NavigationLatencyInputKind>) {
            ++submit_count;
            return specforge::SourceCollectionSessionResult{};
        },
        [&view]() -> const specforge::SourceCollectionSessionView& {
            return view;
        });
    specforge::SourceCollectionPanelUi panel;
    bool open = true;
    const specforge::SourceCollectionPathLauncher launch_source =
        [&](const std::filesystem::path& path)
        -> std::optional<std::string> {
        ++launch_count;
        launched_path = path;
        return std::string{"test process creation failure"};
    };

    const auto render_frame = [&]() {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.DisplaySize = ImVec2(900.0f, 700.0f);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(
            ImVec2(20.0f, 20.0f),
            ImGuiCond_Always);
        ImGui::SetNextWindowSize(
            ImVec2(700.0f, 500.0f),
            ImGuiCond_Always);
        panel.RenderFiles(
            interaction,
            specforge::UiLanguage::English,
            &open,
            []() -> std::optional<std::filesystem::path> {
                return std::nullopt;
            },
            []() -> std::optional<std::filesystem::path> {
                return std::nullopt;
            },
            [](const std::filesystem::path&) {},
            launch_source);
        ImGui::EndFrame();
    };

    render_frame();
    ImGuiWindow* files_window = ImGui::FindWindowByName(
        specforge::SourceCollectionPanelUi::FilesWindowName());
    Require(
        files_window != nullptr,
        "Files panel should render its window for the context-menu test");

    const auto source_context_cell =
        specforge::SourceCollectionPanelUiTestAccess::
            FirstSourceContextCellRect(panel);
    Require(
        source_context_cell.has_value(),
        "Files panel should expose the rendered type-cell rectangle for its context menu");
    const ImVec2 source_context_position =
        RectCenter(*source_context_cell);
    ImGui::GetIO().AddMousePosEvent(
        source_context_position.x,
        source_context_position.y);
    render_frame();
    Require(
        GImGui->HoveredWindow == files_window,
        "the rendered type-cell rectangle should target the Files panel");

    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Right,
        true);
    render_frame();
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Right,
        false);
    render_frame();
    render_frame();
    Require(
        ImGui::IsPopupOpen(
            nullptr,
            ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel),
        "right-clicking a source row should open its context menu");
    ImGuiWindow* popup = GImGui->OpenPopupStack.back().Window;
    Require(
        popup != nullptr,
        "source context menu should expose a popup window");

    const auto action_rect =
        specforge::SourceCollectionPanelUiTestAccess::
            SourceContextActionRect(panel);
    Require(
        action_rect.has_value(),
        "source context menu should expose the rendered action rectangle");
    const ImVec2 action_position = RectCenter(*action_rect);
    ImGui::GetIO().AddMousePosEvent(
        action_position.x,
        action_position.y);
    render_frame();
    Require(
        GImGui->HoveredWindow == popup &&
            GImGui->HoveredId != 0 &&
            !GImGui->HoveredIdIsDisabled,
        "the eligible source context menu action should be enabled before activation");
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    render_frame();
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    render_frame();
    Require(
        launch_count == 1 && launched_path == source_path,
        "activating the source context menu should launch the selected filesystem path");
    Require(
        submit_count == 0,
        "opening a source in a new instance must not submit a session action");
    Require(
        view.snapshot == before.snapshot &&
            view.current_sample_snapshot == before.current_sample_snapshot &&
            view.current_source_index == before.current_source_index &&
            view.navigation.current_index == before.navigation.current_index &&
            view.navigation.current_source_row == before.navigation.current_source_row &&
            view.labeling.source_identity == before.labeling.source_identity &&
            view.sorting.active_source_id == before.sorting.active_source_id,
        "the original source, selection, and workflow state must remain unchanged");
    const specforge::SourceCollectionSessionAction pending_action =
        interaction.TakeAction();
    Require(
        !pending_action.source_roster_changed &&
            !pending_action.snapshot_changed &&
            !pending_action.workflow_changed &&
            !pending_action.navigation_inputs_changed &&
            !interaction.PendingAction().source_roster_changed &&
            !interaction.PendingAction().snapshot_changed &&
            !interaction.PendingAction().workflow_changed &&
            !interaction.PendingAction().navigation_inputs_changed,
        "opening a source in a new instance must not change pending session state");
    Require(
        specforge::SourceCollectionPanelUiTestAccess::SourceLaunchError(panel) &&
            *specforge::SourceCollectionPanelUiTestAccess::SourceLaunchError(panel) ==
                "test process creation failure",
        "a launcher failure should remain visible as a Files-panel diagnostic");
}

void TestFilesPanelContextActionIsDisabledForIneligiblePath()
{
    ScopedImGuiContext context;
    const std::filesystem::path loaded_path =
        std::filesystem::path{L"C:\\观测 data\\loaded source.npy"};
    auto snapshot = std::make_shared<specforge::SpectrumSnapshot>();
    snapshot->source.path = loaded_path;
    snapshot->source.display_name = "loaded source";
    snapshot->collection.spectrum_count = 1;

    specforge::SourceCollectionSessionView view;
    view.snapshot = snapshot;
    view.current_sample_snapshot = snapshot;
    view.sources = {
        {
            {},
            "unavailable source",
            std::string{"npy"},
            specforge::SourceCollectionSourceState::Unavailable,
        },
    };
    view.current_source_index = 0;

    int launch_count = 0;
    specforge::PanelSessionInteraction interaction(
        [](specforge::SourceCollectionSessionIntent,
           std::optional<specforge::NavigationLatencyInputKind>) {
            return specforge::SourceCollectionSessionResult{};
        },
        [&view]() -> const specforge::SourceCollectionSessionView& {
            return view;
        });
    specforge::SourceCollectionPanelUi panel;
    bool open = true;
    bool cover_source_context_cell = false;
    const specforge::SourceCollectionPathLauncher launch_source =
        [&launch_count](const std::filesystem::path&)
        -> std::optional<std::string> {
        ++launch_count;
        return std::nullopt;
    };

    const auto render_frame = [&]() {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.DisplaySize = ImVec2(900.0f, 700.0f);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(
            ImVec2(20.0f, 20.0f),
            ImGuiCond_Always);
        ImGui::SetNextWindowSize(
            ImVec2(700.0f, 500.0f),
            ImGuiCond_Always);
        panel.RenderFiles(
            interaction,
            specforge::UiLanguage::English,
            &open,
            []() -> std::optional<std::filesystem::path> {
                return std::nullopt;
            },
            []() -> std::optional<std::filesystem::path> {
                return std::nullopt;
            },
            [](const std::filesystem::path&) {},
            launch_source);
        if (cover_source_context_cell) {
            const auto cell =
                specforge::SourceCollectionPanelUiTestAccess::
                    FirstSourceContextCellRect(panel);
            if (cell) {
                ImGui::SetNextWindowPos(
                    ImVec2((*cell)[0] - 20.0f,
                           (*cell)[1] - 20.0f),
                    ImGuiCond_Always);
                ImGui::SetNextWindowSize(
                    ImVec2((*cell)[2] - (*cell)[0] + 40.0f,
                           (*cell)[3] - (*cell)[1] + 40.0f),
                    ImGuiCond_Always);
                ImGui::Begin(
                    "Source context occluder",
                    nullptr,
                    ImGuiWindowFlags_NoDecoration |
                        ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoSavedSettings);
                ImGui::End();
            }
        }
        ImGui::EndFrame();
    };

    render_frame();
    ImGuiWindow* files_window = ImGui::FindWindowByName(
        specforge::SourceCollectionPanelUi::FilesWindowName());
    Require(
        files_window != nullptr,
        "Files panel should render its window for the disabled context-menu test");

    const auto source_context_cell =
        specforge::SourceCollectionPanelUiTestAccess::
            FirstSourceContextCellRect(panel);
    Require(
        source_context_cell.has_value(),
        "Files panel should expose the ineligible type-cell rectangle for the disabled context-menu test");
    const ImVec2 source_context_position(
        (*source_context_cell)[0] + 1.0f,
        ((*source_context_cell)[1] +
         (*source_context_cell)[3]) * 0.5f);
    ImGui::GetIO().AddMousePosEvent(
        source_context_position.x,
        source_context_position.y);
    cover_source_context_cell = true;
    render_frame();
    render_frame();
    ImGuiWindow* occluder = ImGui::FindWindowByName(
        "Source context occluder");
    Require(
        occluder != nullptr &&
            GImGui->HoveredWindow == occluder,
        "the context-menu test should cover the source cell with a foreground window");

    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Right,
        true);
    render_frame();
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Right,
        false);
    render_frame();
    render_frame();
    Require(
        !ImGui::IsPopupOpen(
            nullptr,
            ImGuiPopupFlags_AnyPopupId |
                ImGuiPopupFlags_AnyPopupLevel),
        "right-clicking an occluded source cell must not open the underlying context menu");

    cover_source_context_cell = false;
    render_frame();
    render_frame();
    Require(
        GImGui->HoveredWindow == files_window,
        "the uncovered type-cell padding should target the Files panel");

    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Right,
        true);
    render_frame();
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Right,
        false);
    render_frame();
    render_frame();
    Require(
        ImGui::IsPopupOpen(
            nullptr,
            ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel),
        "right-clicking an ineligible row should still open its context menu");
    ImGuiWindow* popup = GImGui->OpenPopupStack.back().Window;
    Require(
        popup != nullptr,
        "the ineligible source context menu should expose a popup window");

    const auto action_rect =
        specforge::SourceCollectionPanelUiTestAccess::
            SourceContextActionRect(panel);
    Require(
        action_rect.has_value(),
        "the ineligible source context menu should expose the rendered action rectangle");
    const ImVec2 action_position = RectCenter(*action_rect);
    ImGui::GetIO().AddMousePosEvent(
        action_position.x,
        action_position.y);
    render_frame();
    Require(
        GImGui->HoveredWindow == popup &&
            GImGui->HoveredId != 0 &&
            GImGui->HoveredIdIsDisabled,
        "the ineligible source context menu action should be disabled");
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    render_frame();
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    render_frame();

    Require(
        launch_count == 0,
        "activating the disabled source context menu item must not launch an ineligible path");
    Require(
        ImGui::IsPopupOpen(
            nullptr,
            ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel),
        "a disabled source context menu item should not close its popup when clicked");
}

class NavigationFixture {
public:
    enum class NavigationFramePresentation {
        Visible,
        Collapsed,
        Hidden,
    };

    NavigationFixture()
        : interaction(
              [this](
                  specforge::SourceCollectionSessionIntent intent,
                  std::optional<
                      specforge::NavigationLatencyInputKind>) {
                  ++submission_count;
                  Require(
                      specforge::SourceCollectionPanelUiTestAccess::
                          IsNavigationNumberMove(intent),
                      "numeric navigation input should submit a direct navigation request");
                  submitted_requests.push_back(
                      specforge::SourceCollectionPanelUiTestAccess::
                          MoveRequest(intent));
                  specforge::SourceCollectionSessionResult result;
                  result.action.navigation_inputs_changed = true;
                  return result;
              },
              [this]()
                  -> const specforge::
                      SourceCollectionSessionView& {
                  return view;
              })
    {
        view.navigation.has_active_source = true;
        view.navigation.current_index = 2;
        view.navigation.sample_count = 10;
        view.navigation.sequence_active = true;
        view.navigation.sequence_count = 10;
        view.navigation.sequence_topology_revision = 1;
        view.navigation.current_sequence_position = 2;
        view.navigation.current_source_row = 2;
        view.navigation.row_location_available = false;
    }

    void RenderFrame(
        const std::function<void()>& after_navigation = {},
        NavigationFramePresentation presentation =
            NavigationFramePresentation::Visible)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.DisplaySize = ImVec2(800.0f, 600.0f);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(
            ImVec2(20.0f, 20.0f),
            ImGuiCond_Always);
        ImGui::SetNextWindowSize(
            ImVec2(420.0f, 240.0f),
            ImGuiCond_Always);
        specforge::SampleWorkflowShortcut shortcut;
        bool open = true;
        if (presentation !=
            NavigationFramePresentation::Hidden) {
            ImGui::SetNextWindowCollapsed(
                presentation ==
                    NavigationFramePresentation::Collapsed,
                ImGuiCond_Always);
            panel.RenderNavigation(
                interaction,
                specforge::UiLanguage::English,
                live_numeric_navigation,
                &open,
                shortcut);
        }
        if (after_navigation) {
            after_navigation();
        }
        panel.FinalizeNavigationInputEdits(
            interaction);
        const specforge::SourceCollectionSessionAction action =
            interaction.TakeAction();
        if (action.navigation_inputs_changed) {
            panel.SyncNavigationInputs(view.navigation);
        }
        ImGui::EndFrame();
    }

    void RenderDockedFrame(bool select_sibling_before_navigation = false)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.DisplaySize = ImVec2(800.0f, 600.0f);
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(
            ImVec2(0.0f, 0.0f),
            ImGuiCond_Always);
        ImGui::SetNextWindowSize(
            io.DisplaySize,
            ImGuiCond_Always);
        constexpr ImGuiWindowFlags host_flags =
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoDocking;
        ImGui::Begin(kDockHostWindow, nullptr, host_flags);
        const ImGuiID dockspace_id =
            ImGui::GetID(kDockspaceId);
        if (!docking_initialized_) {
            docking_initialized_ = true;
            ImGui::DockBuilderAddNode(
                dockspace_id,
                ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(
                dockspace_id,
                io.DisplaySize);
            ImGui::DockBuilderDockWindow(
                kDockSiblingWindow,
                dockspace_id);
            ImGui::DockBuilderDockWindow(
                specforge::SourceCollectionPanelUi::
                    NavigationWindowName(),
                dockspace_id);
            ImGui::DockBuilderFinish(dockspace_id);
        }
        ImGui::DockSpace(
            dockspace_id,
            ImGui::GetContentRegionAvail());
        ImGui::End();

        const char* selected_window_name =
            select_sibling_before_navigation
            ? kDockSiblingWindow
            : specforge::SourceCollectionPanelUi::
                  NavigationWindowName();
        if (ImGuiWindow* selected_window =
                ImGui::FindWindowByName(
                    selected_window_name);
            selected_window != nullptr &&
            selected_window->DockNode != nullptr) {
            ImGuiDockNode* node =
                selected_window->DockNode;
            node->SelectedTabId =
                selected_window->TabId;
            node->VisibleWindow = selected_window;
            if (node->TabBar != nullptr) {
                node->TabBar->SelectedTabId =
                    selected_window->TabId;
                node->TabBar->NextSelectedTabId =
                    selected_window->TabId;
            }
        }

        if (select_sibling_before_navigation) {
            ImGui::SetNextWindowFocus();
            ImGui::Begin(kDockSiblingWindow);
            ImGui::End();
        } else {
            ImGui::SetNextWindowFocus();
        }

        specforge::SampleWorkflowShortcut shortcut;
        bool open = true;
        panel.RenderNavigation(
            interaction,
            specforge::UiLanguage::English,
            live_numeric_navigation,
            &open,
            shortcut);
        ImGuiWindow* navigation_window =
            ImGui::FindWindowByName(
                specforge::SourceCollectionPanelUi::
                    NavigationWindowName());
        navigation_tab_visible_last_frame =
            navigation_window != nullptr &&
            navigation_window->DockTabIsVisible;

        if (!select_sibling_before_navigation) {
            ImGui::Begin(kDockSiblingWindow);
            ImGui::End();
        }

        panel.FinalizeNavigationInputEdits(interaction);
        const specforge::SourceCollectionSessionAction action =
            interaction.TakeAction();
        if (action.navigation_inputs_changed) {
            panel.SyncNavigationInputs(view.navigation);
        }
        ImGui::EndFrame();
    }

    [[nodiscard]] ImGuiID SequenceInputId() const
    {
        ImGuiWindow* window =
            ImGui::FindWindowByName(
                specforge::SourceCollectionPanelUi::
                    NavigationWindowName());
        Require(
            window != nullptr,
            "Navigation window should exist");
        return window->GetID(
            "##SampleNavigationSequence");
    }

    [[nodiscard]] ImGuiID SourceInputId() const
    {
        ImGuiWindow* window =
            ImGui::FindWindowByName(
                specforge::SourceCollectionPanelUi::
                    NavigationWindowName());
        Require(
            window != nullptr,
            "Navigation window should exist");
        return window->GetID(
            "##SampleNavigationSample");
    }

    specforge::SourceCollectionSessionView view;
    int submission_count = 0;
    std::vector<specforge::SampleNavigationRequest>
        submitted_requests;
    specforge::SourceCollectionPanelUi panel;
    specforge::PanelSessionInteraction interaction;
    bool navigation_tab_visible_last_frame = false;
    bool live_numeric_navigation = false;

private:
    static constexpr const char* kDockHostWindow =
        "Navigation Test Dock Host";
    static constexpr const char* kDockspaceId =
        "NavigationTestDockspace";
    static constexpr const char* kDockSiblingWindow =
        "Other###SpecForgeNavigationTestSibling";
    bool docking_initialized_ = false;
};

void SetActiveInputTextValue(std::string_view text)
{
    ImGuiInputTextState& state =
        GImGui->InputTextState;
    state.TextA.resize(
        static_cast<int>(text.size() + 1));
    std::memcpy(
        state.TextA.Data,
        text.data(),
        text.size());
    state.TextA[
        static_cast<int>(text.size())] = '\0';
    state.TextLen =
        static_cast<int>(text.size());
    GImGui->ActiveIdHasBeenEditedBefore = true;
}

void ConfigureEditableSourceInput(
    NavigationFixture& fixture)
{
    fixture.view.navigation.current_index = 19;
    fixture.view.navigation.current_source_row = 19;
    fixture.view.navigation.sample_count = 100;
    fixture.view.navigation.row_location_available = true;
    fixture.view.navigation.sequence_active = false;
    fixture.view.navigation.sequence_count = 0;
    fixture.view.navigation.current_sequence_position.reset();
}

void TestLiveSourceInputSubmitsEveryValidPrefixAndSurvivesCursorSync()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    fixture.live_numeric_navigation = true;
    ConfigureEditableSourceInput(fixture);
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID source_input_id =
        fixture.SourceInputId();
    ImGui::ActivateItemByID(source_input_id);
    fixture.RenderFrame();
    ImGui::GetIO().AddInputCharactersUTF8("45");
    fixture.RenderFrame();
    Require(
        fixture.submitted_requests.size() == 2 &&
            fixture.submitted_requests[0].kind ==
                specforge::SampleNavigationRequestKind::LocateRow &&
            fixture.submitted_requests[0].row_index == 3 &&
            fixture.submitted_requests[1].kind ==
                specforge::SampleNavigationRequestKind::LocateRow &&
            fixture.submitted_requests[1].row_index == 44,
        "live source input should submit 4 and 45 in order when both characters arrive in one frame");

    fixture.view.navigation.current_index = 29;
    fixture.view.navigation.current_source_row = 29;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    Require(
        GImGui->InputTextState.TextA.Data != nullptr &&
            std::string_view{
                GImGui->InputTextState.TextA.Data} == "45",
        "same-topology cursor synchronization must preserve the completed active source buffer");
}

void TestLiveSequenceInputSubmitsEveryValidPrefixAndEscapeKeepsLatestIntent()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    fixture.live_numeric_navigation = true;
    fixture.view.navigation.sample_count = 100;
    fixture.view.navigation.sequence_count = 100;
    fixture.view.navigation.current_index = 19;
    fixture.view.navigation.current_source_row = 19;
    fixture.view.navigation.current_sequence_position = 19;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID sequence_input_id =
        fixture.SequenceInputId();
    ImGui::ActivateItemByID(sequence_input_id);
    fixture.RenderFrame();
    ImGui::GetIO().AddInputCharactersUTF8("45");
    fixture.RenderFrame();

    Require(
        fixture.submitted_requests.size() == 2 &&
            fixture.submitted_requests[0].kind ==
                specforge::SampleNavigationRequestKind::
                    LocateSequencePosition &&
            fixture.submitted_requests[0].sequence_position == 3 &&
            fixture.submitted_requests[1].kind ==
                specforge::SampleNavigationRequestKind::
                    LocateSequencePosition &&
            fixture.submitted_requests[1].sequence_position == 44,
        "live sequence input should submit valid prefixes 4 and 45 as 0-based latest intents");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    fixture.RenderFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    fixture.RenderFrame();
    Require(
        fixture.submitted_requests.size() == 2,
        "Escape in live mode should end editing without reversing or repeating the latest navigation intent");
}

void TestLiveInputRejectsInvalidTargetsAndStopsAfterTopologyChange()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    fixture.live_numeric_navigation = true;
    ConfigureEditableSourceInput(fixture);
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID source_input_id =
        fixture.SourceInputId();
    ImGui::ActivateItemByID(source_input_id);
    fixture.RenderFrame();
    for (const std::string_view value : {"", "0", "101"}) {
        SetActiveInputTextValue(value);
        fixture.RenderFrame();
    }
    Require(
        fixture.submission_count == 0,
        "empty, zero, and out-of-range live source values must not submit");

    SetActiveInputTextValue("4");
    fixture.RenderFrame();
    Require(
        fixture.submission_count == 1,
        "a valid live source value should submit before topology replacement");
    fixture.view.navigation.current_index = 7;
    fixture.view.navigation.current_source_row = 7;
    ++fixture.view.navigation.sequence_topology_revision;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    SetActiveInputTextValue("45");
    fixture.RenderFrame();
    Require(
        fixture.submission_count == 1 &&
            GImGui->ActiveId != source_input_id,
        "topology replacement should deactivate the old live edit and prevent further requests");

    fixture.view.navigation.row_location_available = false;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    ImGui::ActivateItemByID(source_input_id);
    fixture.RenderFrame();
    SetActiveInputTextValue("45");
    fixture.RenderFrame();
    Require(
        fixture.submission_count == 1,
        "an unavailable source-row locator must reject live requests");
}

void TestLiveExplicitCommitOfDisplayedTargetSubmitsLatestIntent()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    fixture.live_numeric_navigation = true;
    ConfigureEditableSourceInput(fixture);
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID source_input_id =
        fixture.SourceInputId();
    ImGui::ActivateItemByID(source_input_id);
    fixture.RenderFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    fixture.RenderFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    fixture.RenderFrame();

    Require(
        fixture.submitted_requests.size() == 1 &&
            fixture.submitted_requests.front().row_index == 19,
        "explicitly submitting the displayed target in live mode should still express a latest intent");
}

void TestEnterOwnedByAnotherItemDoesNotCommitNumericInput()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    ConfigureEditableSourceInput(fixture);
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID source_input_id =
        fixture.SourceInputId();
    ImGui::ActivateItemByID(source_input_id);
    fixture.RenderFrame();

    std::array<char, 16> other_buffer = {};
    const auto render_other_input = [&other_buffer]() {
        ImGui::Begin("Other input###NumericEnterOwnership");
        (void)ImGui::InputText(
            "##OtherInput",
            other_buffer.data(),
            other_buffer.size(),
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::End();
    };
    fixture.RenderFrame(render_other_input);
    ImGuiWindow* other_window =
        ImGui::FindWindowByName(
            "Other input###NumericEnterOwnership");
    Require(
        other_window != nullptr,
        "fixture should create the other input window");
    const ImGuiID other_input_id =
        other_window->GetID("##OtherInput");
    ImGui::ActivateItemByID(other_input_id);
    fixture.RenderFrame(render_other_input);
    Require(
        GImGui->ActiveId == other_input_id,
        "fixture should transfer keyboard ownership to the other input");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    fixture.RenderFrame(render_other_input);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    fixture.RenderFrame();

    Require(
        fixture.submission_count == 0,
        "Enter owned by another active item must not commit the stale numeric edit state");
}

void RequireLiveSourceEditDoesNotRepeatWhenNotRendered(
    NavigationFixture::NavigationFramePresentation presentation,
    std::string_view message)
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    fixture.live_numeric_navigation = true;
    ConfigureEditableSourceInput(fixture);
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID source_input_id =
        fixture.SourceInputId();
    ImGui::ActivateItemByID(source_input_id);
    fixture.RenderFrame();
    SetActiveInputTextValue("6");
    fixture.RenderFrame();
    Require(
        fixture.submission_count == 1,
        "live source edit should submit once while visible");

    fixture.RenderFrame({}, presentation);
    Require(
        fixture.submission_count == 1,
        message);
}

void TestHiddenAndCollapsedLiveEditsDoNotRepeat()
{
    RequireLiveSourceEditDoesNotRepeatWhenNotRendered(
        NavigationFixture::NavigationFramePresentation::Hidden,
        "hiding Navigation should not repeat an already submitted live edit");
    RequireLiveSourceEditDoesNotRepeatWhenNotRendered(
        NavigationFixture::NavigationFramePresentation::Collapsed,
        "collapsing Navigation should not repeat an already submitted live edit");
}

void TestCoveredDockTabDoesNotRepeatLiveEdit()
{
    ScopedImGuiContext context;
    ImGui::GetIO().ConfigFlags |=
        ImGuiConfigFlags_DockingEnable;
    NavigationFixture fixture;
    fixture.live_numeric_navigation = true;
    ConfigureEditableSourceInput(fixture);
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderDockedFrame();
    fixture.RenderDockedFrame();

    const ImGuiID source_input_id =
        fixture.SourceInputId();
    ImGui::ActivateItemByID(source_input_id);
    fixture.RenderDockedFrame();
    SetActiveInputTextValue("6");
    fixture.RenderDockedFrame();
    Require(
        fixture.submission_count == 1,
        "live docked edit should submit once while its tab is visible");
    fixture.RenderDockedFrame(true);
    Require(
        fixture.submission_count == 1,
        "covering the Navigation dock tab should not repeat an already submitted live edit");
}

void TestSourceDraftWaitsForBlurAndSurvivesCursorSync()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    ConfigureEditableSourceInput(fixture);
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID source_input_id =
        fixture.SourceInputId();
    ImGui::ActivateItemByID(source_input_id);
    fixture.RenderFrame();
    Require(
        GImGui->ActiveId == source_input_id,
        "source input should activate for a multi-digit edit");

    SetActiveInputTextValue("4");
    fixture.RenderFrame();
    Require(
        fixture.submission_count == 0,
        "the first valid digit of a multi-digit source draft must not navigate");

    fixture.view.navigation.current_index = 29;
    fixture.view.navigation.current_source_row = 29;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    SetActiveInputTextValue("45");
    ImGui::ClearActiveID();
    fixture.RenderFrame();

    Require(
        fixture.submitted_requests.size() == 1 &&
            fixture.submitted_requests.front().kind ==
                specforge::SampleNavigationRequestKind::LocateRow &&
            fixture.submitted_requests.front().row_index == 44,
        "source input 45 should submit one 0-based LocateRow target after blur despite a same-topology cursor sync");
}

void TestEnterCommitsSourceDraftOnce()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    ConfigureEditableSourceInput(fixture);
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID source_input_id =
        fixture.SourceInputId();
    ImGui::ActivateItemByID(source_input_id);
    fixture.RenderFrame();
    SetActiveInputTextValue("45");
    fixture.RenderFrame();
    Require(
        fixture.submission_count == 0,
        "editing a complete source draft must wait for explicit commit");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    fixture.RenderFrame([&fixture]() {
        Require(
            fixture.submission_count == 1,
            "Enter should submit the source draft immediately");
    });
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    fixture.RenderFrame();

    Require(
        fixture.submitted_requests.size() == 1 &&
            fixture.submitted_requests.front().kind ==
                specforge::SampleNavigationRequestKind::LocateRow &&
            fixture.submitted_requests.front().row_index == 44,
        "Enter should submit the complete source draft exactly once with a 0-based target");
}

void TestExplicitCommittedSourceRowCancelsPendingTarget()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    ConfigureEditableSourceInput(fixture);
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID source_input_id =
        fixture.SourceInputId();
    ImGui::ActivateItemByID(source_input_id);
    fixture.RenderFrame();
    SetActiveInputTextValue("45");
    fixture.RenderFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    fixture.RenderFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    fixture.RenderFrame();
    Require(
        fixture.submitted_requests.size() == 1 &&
            fixture.submitted_requests.front().row_index == 44,
        "the first explicit source row should queue the pending target");

    ImGui::ActivateItemByID(source_input_id);
    fixture.RenderFrame();
    SetActiveInputTextValue("2");
    fixture.RenderFrame();
    SetActiveInputTextValue("20");
    fixture.RenderFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    fixture.RenderFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    fixture.RenderFrame();

    Require(
        fixture.submitted_requests.size() == 2,
        "explicitly committing the displayed source row must submit a latest-intent request");
    Require(
        fixture.submitted_requests.back().kind ==
                specforge::SampleNavigationRequestKind::LocateRow &&
            fixture.submitted_requests.back().row_index == 19,
        "the pending-cancel source request should preserve the 1-based to 0-based row contract");
}

void TestEscapeCancelsSourceDraftAgainstLatestCommittedValue()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    ConfigureEditableSourceInput(fixture);
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID source_input_id =
        fixture.SourceInputId();
    ImGui::ActivateItemByID(source_input_id);
    fixture.RenderFrame();
    SetActiveInputTextValue("45");
    fixture.RenderFrame();

    fixture.view.navigation.current_index = 29;
    fixture.view.navigation.current_source_row = 29;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);

    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    fixture.RenderFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    fixture.RenderFrame();

    Require(
        fixture.submission_count == 0,
        "Escape should cancel the source draft without navigation");

    ImGui::ActivateItemByID(source_input_id);
    fixture.RenderFrame();
    Require(
        GImGui->InputTextState.TextA.Data != nullptr &&
            std::string_view{
                GImGui->InputTextState.TextA.Data} == "30",
        "Escape should restore the latest committed source row");
}

void TestTopologyChangeDiscardsActiveSourceDraft()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    ConfigureEditableSourceInput(fixture);
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID source_input_id =
        fixture.SourceInputId();
    ImGui::ActivateItemByID(source_input_id);
    fixture.RenderFrame();
    SetActiveInputTextValue("45");
    fixture.RenderFrame();

    fixture.view.navigation.current_index = 7;
    fixture.view.navigation.current_source_row = 7;
    Require(
        fixture.view.navigation.row_location_available,
        "source input should remain available so topology revision is the only draft invalidation guard");
    ++fixture.view.navigation.sequence_topology_revision;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    Require(
        fixture.submission_count == 0,
        "a source draft must not submit after its topology revision changes");
    Require(
        GImGui->ActiveId != source_input_id,
        "topology synchronization should deactivate the stale source draft");
    Require(
        GImGui->InputTextState.TextA.Data != nullptr &&
            std::string_view{
                GImGui->InputTextState.TextA.Data} == "8",
        "topology synchronization should reload the source input with the latest committed row");

    fixture.RenderFrame();
    Require(
        fixture.submission_count == 0,
        "the reloaded source row must not return as a deactivation edit");
}

void TestInvalidAndUnavailableSourceTargetsDoNotSubmit()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    ConfigureEditableSourceInput(fixture);
    fixture.view.navigation.row_location_available = false;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID source_input_id =
        fixture.SourceInputId();
    ImGui::ActivateItemByID(source_input_id);
    fixture.RenderFrame();
    SetActiveInputTextValue("45");
    fixture.RenderFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    fixture.RenderFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    fixture.RenderFrame();
    Require(
        fixture.submission_count == 0,
        "an unavailable source-row input must not submit even when the test forces activation and Enter");

    fixture.view.navigation.row_location_available = true;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();
    ImGui::ActivateItemByID(source_input_id);
    fixture.RenderFrame();
    SetActiveInputTextValue("0");
    fixture.RenderFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    fixture.RenderFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    fixture.RenderFrame();
    Require(
        fixture.submission_count == 0,
        "source row zero should not submit a navigation request");

    ImGui::ActivateItemByID(source_input_id);
    fixture.RenderFrame();
    SetActiveInputTextValue("101");
    ImGui::ClearActiveID();
    fixture.RenderFrame();
    Require(
        fixture.submission_count == 0,
        "an out-of-range source row should not submit on blur");
}

void RequireSourceDraftFinalizesWhenNotRendered(
    NavigationFixture::NavigationFramePresentation presentation,
    std::string_view message)
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    ConfigureEditableSourceInput(fixture);
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID source_input_id =
        fixture.SourceInputId();
    ImGui::ActivateItemByID(source_input_id);
    fixture.RenderFrame();
    SetActiveInputTextValue("6");
    fixture.RenderFrame();

    fixture.RenderFrame({}, presentation);

    Require(
        fixture.submitted_requests.size() == 1 &&
            fixture.submitted_requests.front().kind ==
                specforge::SampleNavigationRequestKind::LocateRow &&
            fixture.submitted_requests.front().row_index == 5,
        message);
}

void TestHiddenAndCollapsedNavigationFinalizeSourceDraft()
{
    RequireSourceDraftFinalizesWhenNotRendered(
        NavigationFixture::NavigationFramePresentation::Hidden,
        "hiding Navigation should finalize the dirty source draft through the frame-end commit");
    RequireSourceDraftFinalizesWhenNotRendered(
        NavigationFixture::NavigationFramePresentation::Collapsed,
        "collapsing Navigation should finalize the dirty source draft through the frame-end commit");
}

void TestCoveredDockTabFinalizesSourceDraft()
{
    ScopedImGuiContext context;
    ImGui::GetIO().ConfigFlags |=
        ImGuiConfigFlags_DockingEnable;
    NavigationFixture fixture;
    ConfigureEditableSourceInput(fixture);
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderDockedFrame();
    fixture.RenderDockedFrame();
    Require(
        fixture.navigation_tab_visible_last_frame,
        "Navigation should start as the visible dock tab");

    const ImGuiID source_input_id =
        fixture.SourceInputId();
    ImGui::ActivateItemByID(source_input_id);
    fixture.RenderDockedFrame();
    SetActiveInputTextValue("6");
    fixture.RenderDockedFrame();
    fixture.RenderDockedFrame(true);

    Require(
        !fixture.navigation_tab_visible_last_frame,
        "the sibling window should cover the Navigation dock tab");
    Require(
        fixture.submitted_requests.size() == 1 &&
            fixture.submitted_requests.front().kind ==
                specforge::SampleNavigationRequestKind::LocateRow &&
            fixture.submitted_requests.front().row_index == 5,
        "covering the Navigation dock tab should finalize the dirty source draft");
}

void TestSourceAndSequenceEditsKeepIndependentCommitRouting()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    fixture.view.navigation.row_location_available = true;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID source_input_id =
        fixture.SourceInputId();
    const ImGuiID sequence_input_id =
        fixture.SequenceInputId();
    ImGui::ActivateItemByID(source_input_id);
    fixture.RenderFrame();
    SetActiveInputTextValue("6");
    fixture.RenderFrame();

    ImGui::ActivateItemByID(sequence_input_id);
    fixture.RenderFrame();
    fixture.RenderFrame();
    Require(
        fixture.submitted_requests.size() == 1 &&
            fixture.submitted_requests.front().kind ==
                specforge::SampleNavigationRequestKind::LocateRow &&
            fixture.submitted_requests.front().row_index == 5,
        "moving focus from source to sequence should route the source blur commit to LocateRow");
    Require(
        GImGui->ActiveId == sequence_input_id,
        "the sequence input should remain active after the source blur commit is finalized");

    SetActiveInputTextValue("7");
    ImGui::ClearActiveID();
    fixture.RenderFrame();
    Require(
        fixture.submitted_requests.size() == 2 &&
            fixture.submitted_requests.back().kind ==
                specforge::SampleNavigationRequestKind::
                    LocateSequencePosition &&
            fixture.submitted_requests.back().sequence_position == 6,
        "the following sequence blur should keep its independent LocateSequencePosition routing");
}

void TestCurrentSequenceEditStillSubmitsOnDeactivation()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID sequence_input_id =
        fixture.SequenceInputId();
    ImGui::ActivateItemByID(sequence_input_id);
    fixture.RenderFrame();
    Require(
        GImGui->ActiveId == sequence_input_id,
        "sequence input should activate for a current edit");
    SetActiveInputTextValue("4");
    ImGui::ClearActiveID();

    fixture.RenderFrame();

    Require(
        fixture.submission_count == 1,
        "a sequence edit from the current synchronization generation should still submit");
}

void TestDeferredShellSyncDoesNotSubmitAMultiDigitPrefix()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    fixture.view.navigation.sample_count = 100;
    fixture.view.navigation.sequence_count = 100;
    fixture.view.navigation.current_index = 19;
    fixture.view.navigation.current_source_row = 19;
    fixture.view.navigation.current_sequence_position = 19;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID sequence_input_id =
        fixture.SequenceInputId();
    ImGui::ActivateItemByID(sequence_input_id);
    fixture.RenderFrame();
    Require(
        GImGui->ActiveId == sequence_input_id,
        "sequence input should activate for a multi-digit edit");

    SetActiveInputTextValue("4");
    fixture.RenderFrame();
    Require(
        fixture.submission_count == 0,
        "the first valid digit of a multi-digit sequence draft must not submit deferred navigation");
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);

    SetActiveInputTextValue("45");
    ImGui::ClearActiveID();
    fixture.RenderFrame();
    Require(
        fixture.submission_count == 1,
        "the complete multi-digit sequence draft should submit exactly once on deactivation");
    Require(
        fixture.submitted_requests.size() == 1 &&
            fixture.submitted_requests.front().kind ==
                specforge::SampleNavigationRequestKind::
                    LocateSequencePosition &&
            fixture.submitted_requests.front().sequence_position ==
                44,
        "sequence input 45 should submit LocateSequencePosition with the 0-based target 44");
}

void TestEnterCommitsSequenceDraftOnce()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    fixture.view.navigation.sample_count = 100;
    fixture.view.navigation.sequence_count = 100;
    fixture.view.navigation.current_index = 19;
    fixture.view.navigation.current_source_row = 19;
    fixture.view.navigation.current_sequence_position = 19;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID sequence_input_id =
        fixture.SequenceInputId();
    ImGui::ActivateItemByID(sequence_input_id);
    fixture.RenderFrame();
    SetActiveInputTextValue("45");
    fixture.RenderFrame();
    Require(
        fixture.submission_count == 0,
        "editing a complete sequence draft must still wait for explicit commit");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    fixture.RenderFrame([&fixture]() {
        Require(
            fixture.submission_count == 1,
            "Enter should submit immediately before later panel actions are finalized");
    });
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    fixture.RenderFrame();

    Require(
        fixture.submission_count == 1 &&
            fixture.submitted_requests.front().sequence_position ==
                44,
        "Enter should submit the complete sequence draft exactly once with a 0-based target");

    ImGui::ActivateItemByID(sequence_input_id);
    fixture.RenderFrame();
    Require(
        GImGui->InputTextState.TextA.Data != nullptr &&
            std::string_view{
                GImGui->InputTextState.TextA.Data} == "20",
        "deferred Shell synchronization should restore the committed value after Enter");
}

void TestExplicitCommittedSequencePositionCancelsPendingTarget()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    fixture.view.navigation.sample_count = 100;
    fixture.view.navigation.sequence_count = 100;
    fixture.view.navigation.current_index = 19;
    fixture.view.navigation.current_source_row = 19;
    fixture.view.navigation.current_sequence_position = 19;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID sequence_input_id =
        fixture.SequenceInputId();
    ImGui::ActivateItemByID(sequence_input_id);
    fixture.RenderFrame();
    SetActiveInputTextValue("45");
    fixture.RenderFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    fixture.RenderFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    fixture.RenderFrame();
    Require(
        fixture.submission_count == 1 &&
            fixture.submitted_requests.front().sequence_position ==
                44,
        "the first explicit position should queue the pending target");

    ImGui::ActivateItemByID(sequence_input_id);
    fixture.RenderFrame();
    SetActiveInputTextValue("2");
    fixture.RenderFrame();
    SetActiveInputTextValue("20");
    fixture.RenderFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    fixture.RenderFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    fixture.RenderFrame();

    Require(
        fixture.submitted_requests.size() == 2,
        "explicitly committing the displayed position must submit a latest-intent request that can cancel the pending target");
    Require(
        fixture.submitted_requests.back().kind ==
                specforge::SampleNavigationRequestKind::
                    LocateSequencePosition &&
            fixture.submitted_requests.back().sequence_position ==
                19,
        "the pending-cancel request should preserve the 1-based to 0-based sequence-position contract");
}

void TestEscapeCancelsSequenceDraftAgainstLatestCommittedValue()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    fixture.view.navigation.sample_count = 100;
    fixture.view.navigation.sequence_count = 100;
    fixture.view.navigation.current_index = 19;
    fixture.view.navigation.current_source_row = 19;
    fixture.view.navigation.current_sequence_position = 19;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID sequence_input_id =
        fixture.SequenceInputId();
    ImGui::ActivateItemByID(sequence_input_id);
    fixture.RenderFrame();
    SetActiveInputTextValue("45");
    fixture.RenderFrame();

    fixture.view.navigation.current_index = 29;
    fixture.view.navigation.current_source_row = 29;
    fixture.view.navigation.current_sequence_position = 29;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);

    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    fixture.RenderFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    fixture.RenderFrame();

    Require(
        fixture.submission_count == 0,
        "Escape should cancel the draft instead of navigating to either the draft or the activation-time value");

    ImGui::ActivateItemByID(sequence_input_id);
    fixture.RenderFrame();
    Require(
        GImGui->InputTextState.TextA.Data != nullptr &&
            std::string_view{
                GImGui->InputTextState.TextA.Data} == "30",
        "Escape should restore the latest committed sequence position");
}

void TestStaleSequenceDraftIsNotReplayedAfterExternalSync()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID sequence_input_id =
        fixture.SequenceInputId();
    ImGui::ActivateItemByID(sequence_input_id);
    fixture.RenderFrame();
    Require(
        GImGui->ActiveId == sequence_input_id,
        "sequence input should be active before the external synchronization");
    SetActiveInputTextValue("6");
    fixture.RenderFrame();
    Require(
        GImGui->InputTextState.TextA.Data != nullptr &&
            std::string_view{
                GImGui->InputTextState.TextA.Data} == "6",
        "active sequence edit state should contain the pre-topology draft");

    fixture.view.navigation.current_sequence_position = 7;
    ++fixture.view.navigation.sequence_topology_revision;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);

    fixture.RenderFrame();

    Require(
        fixture.submission_count == 0,
        "an active sequence draft must not be submitted after external synchronization changes the displayed position");
    Require(
        GImGui->ActiveId != sequence_input_id,
        "topology synchronization should explicitly deactivate the stale active sequence draft");
    Require(
        GImGui->InputTextState.TextA.Data != nullptr &&
            std::string_view{
                GImGui->InputTextState.TextA.Data} == "8",
        "topology synchronization should reload the active ImGui text state with the recomputed position");
    Require(
        fixture.view.navigation.current_index == 2 &&
            fixture.view.navigation.current_sequence_position == 7,
        "external topology synchronization should retain the source row and its recomputed sequence position");

    fixture.RenderFrame();
    Require(
        fixture.submission_count == 0,
        "the reloaded topology value must not return as a deactivation edit on the following frame");
}

void TestBlurDraftIsDiscardedWhenLaterPanelChangesTopology()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID sequence_input_id =
        fixture.SequenceInputId();
    ImGui::ActivateItemByID(sequence_input_id);
    fixture.RenderFrame();
    SetActiveInputTextValue("6");
    fixture.RenderFrame();
    ImGui::ClearActiveID();

    fixture.RenderFrame([&fixture]() {
        Require(
            fixture.submission_count == 0,
            "sequence blur must wait until later panel actions have settled");
        fixture.view.navigation.current_sequence_position = 7;
        ++fixture.view.navigation.sequence_topology_revision;
        fixture.panel.SyncNavigationInputs(
            fixture.view.navigation);
    });

    Require(
        fixture.submission_count == 0,
        "a blur draft must be discarded when a later panel replaces its topology");

    fixture.RenderFrame();
    Require(
        GImGui->InputTextState.TextA.Data != nullptr &&
            std::string_view{
                GImGui->InputTextState.TextA.Data} == "8",
        "later-panel topology synchronization should replace ImGui's saved blur draft with the recomputed position");

    Require(
        fixture.submission_count == 0,
        "the stale blur draft must remain discarded after the deactivation feedback frame");
}

void TestCollapsedNavigationFinalizesSequenceDraft()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID sequence_input_id =
        fixture.SequenceInputId();
    ImGui::ActivateItemByID(sequence_input_id);
    fixture.RenderFrame();
    SetActiveInputTextValue("6");
    fixture.RenderFrame();

    fixture.RenderFrame(
        {},
        NavigationFixture::NavigationFramePresentation::
            Collapsed);

    Require(
        fixture.submitted_requests.size() == 1 &&
            fixture.submitted_requests.front().sequence_position ==
                5,
        "collapsing Navigation should finalize the dirty sequence draft through the frame-end commit");
}

void TestHiddenNavigationFinalizesDraftAfterSameTopologyCursorSync()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID sequence_input_id =
        fixture.SequenceInputId();
    ImGui::ActivateItemByID(sequence_input_id);
    fixture.RenderFrame();
    SetActiveInputTextValue("6");
    fixture.RenderFrame();

    fixture.view.navigation.current_sequence_position = 7;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame(
        {},
        NavigationFixture::NavigationFramePresentation::
            Hidden);

    Require(
        fixture.submitted_requests.size() == 1 &&
            fixture.submitted_requests.front().sequence_position ==
                5,
        "hiding Navigation should commit the dirty draft even after a same-topology cursor sync");

    fixture.RenderFrame();
    Require(
        GImGui->InputTextState.TextA.Data != nullptr &&
            std::string_view{
                GImGui->InputTextState.TextA.Data} == "8",
        "reopening Navigation should show the latest synchronized cursor instead of the finalized draft");
}

void TestHiddenNavigationCancelsCleanEditAndSynchronizesLatestCursor()
{
    ScopedImGuiContext context;
    NavigationFixture fixture;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame();

    const ImGuiID sequence_input_id =
        fixture.SequenceInputId();
    ImGui::ActivateItemByID(sequence_input_id);
    fixture.RenderFrame();

    fixture.view.navigation.current_sequence_position = 7;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderFrame(
        {},
        NavigationFixture::NavigationFramePresentation::
            Hidden);

    Require(
        fixture.submission_count == 0,
        "hiding Navigation should not submit an unedited sequence field");

    fixture.RenderFrame();
    Require(
        GImGui->InputTextState.TextA.Data != nullptr &&
            std::string_view{
                GImGui->InputTextState.TextA.Data} == "8",
        "reopening a clean hidden edit should show the latest synchronized cursor");
}

void TestCoveredDockTabFinalizesSequenceDraft()
{
    ScopedImGuiContext context;
    ImGui::GetIO().ConfigFlags |=
        ImGuiConfigFlags_DockingEnable;
    NavigationFixture fixture;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    fixture.RenderDockedFrame();
    fixture.RenderDockedFrame();
    Require(
        fixture.navigation_tab_visible_last_frame,
        "Navigation should start as the visible dock tab");

    const ImGuiID sequence_input_id =
        fixture.SequenceInputId();
    ImGui::ActivateItemByID(sequence_input_id);
    fixture.RenderDockedFrame();
    SetActiveInputTextValue("6");
    fixture.RenderDockedFrame();

    fixture.RenderDockedFrame(true);

    Require(
        !fixture.navigation_tab_visible_last_frame,
        "the sibling window should cover the Navigation dock tab");
    Require(
        fixture.submitted_requests.size() == 1 &&
            fixture.submitted_requests.front().sequence_position ==
                5,
        "covering the Navigation dock tab should finalize the dirty sequence draft");
}

}  // namespace

int main()
{
    TestFilesPanelAddFileForwardsCsvToInAppOpener();
    TestAnnotationImportFailureRendersInlineWithoutHoverOrPopup();
    TestMissingLocalAnnotationRemovalRequiresConfirmation();
    TestSourceSwitchDismissesMissingLocalRemovalWarning();
    TestAnnotationImportFailureDismissalTracksExactDetail();
    TestReimportSameAnnotationClearsDismissalForNewFailure();
    TestAnnotationDismissalsResetAcrossSourceSwitch();
    TestHiddenAnnotationPanelObservesIntermediateSourceSwitch();
    TestReopenableSourcePathEligibility();
    TestFilesPanelContextActionLaunchesWithoutMutatingSession();
    TestFilesPanelContextActionIsDisabledForIneligiblePath();
    TestLiveSourceInputSubmitsEveryValidPrefixAndSurvivesCursorSync();
    TestLiveSequenceInputSubmitsEveryValidPrefixAndEscapeKeepsLatestIntent();
    TestLiveInputRejectsInvalidTargetsAndStopsAfterTopologyChange();
    TestLiveExplicitCommitOfDisplayedTargetSubmitsLatestIntent();
    TestEnterOwnedByAnotherItemDoesNotCommitNumericInput();
    TestHiddenAndCollapsedLiveEditsDoNotRepeat();
    TestCoveredDockTabDoesNotRepeatLiveEdit();
    TestSourceDraftWaitsForBlurAndSurvivesCursorSync();
    TestEnterCommitsSourceDraftOnce();
    TestExplicitCommittedSourceRowCancelsPendingTarget();
    TestEscapeCancelsSourceDraftAgainstLatestCommittedValue();
    TestTopologyChangeDiscardsActiveSourceDraft();
    TestInvalidAndUnavailableSourceTargetsDoNotSubmit();
    TestHiddenAndCollapsedNavigationFinalizeSourceDraft();
    TestCoveredDockTabFinalizesSourceDraft();
    TestSourceAndSequenceEditsKeepIndependentCommitRouting();
    TestCurrentSequenceEditStillSubmitsOnDeactivation();
    TestDeferredShellSyncDoesNotSubmitAMultiDigitPrefix();
    TestEnterCommitsSequenceDraftOnce();
    TestEscapeCancelsSequenceDraftAgainstLatestCommittedValue();
    TestStaleSequenceDraftIsNotReplayedAfterExternalSync();
    TestBlurDraftIsDiscardedWhenLaterPanelChangesTopology();
    TestHiddenNavigationFinalizesDraftAfterSameTopologyCursorSync();
    TestHiddenNavigationCancelsCleanEditAndSynchronizesLatestCursor();
    TestCollapsedNavigationFinalizesSequenceDraft();
    TestCoveredDockTabFinalizesSequenceDraft();
    TestExplicitCommittedSequencePositionCancelsPendingTarget();
    return 0;
}
