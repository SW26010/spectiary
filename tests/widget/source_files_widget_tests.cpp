#include "ui/source_collection_panel.h"

#include <imgui.h>
#include "imgui_widget_harness.h"

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

namespace spectiary {
struct SourceCollectionPanelUiTestAccess {
    static const std::optional<std::string>& SourceLaunchError(const SourceCollectionPanelUi& panel)
    { return panel.source_launch_error_; }
};
}
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
    spectiary::SourceCollectionSessionView view;
    spectiary::PanelSessionInteraction interaction(
        [](spectiary::SourceCollectionSessionIntent,
           std::optional<spectiary::NavigationLatencyInputKind>) {
            return spectiary::SourceCollectionSessionResult{};
        },
        [&view]() -> const spectiary::SourceCollectionSessionView& {
            return view;
        });
    spectiary::SourceCollectionPanelUi panel;
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
            spectiary::UiLanguage::English,
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

    spectiary::test::WidgetHarness ui{render_frame,
        spectiary::test::WidgetHarness::FrameMode::ExistingContext};
    ui.Frames(2);
    ui.Click("FilesAddFile");
    Require(
        choose_file_count == 1 &&
            choose_folder_count == 0 &&
            opened_path == selected_path,
        "Files panel Add file should forward the selected CSV to its source opener");
}
void TestFilesPanelContextActionLaunchesWithoutMutatingSession()
{
    ScopedImGuiContext context;
    const std::filesystem::path source_path =
        std::filesystem::path{L"C:\\观测 data\\source file.npy"};
    auto snapshot = std::make_shared<spectiary::SpectrumSnapshot>();
    snapshot->source.path = source_path;
    snapshot->source.display_name = "source file";
    snapshot->collection.spectrum_count = 4;
    snapshot->collection.current_index = 2;

    spectiary::SourceCollectionSessionView view;
    view.snapshot = snapshot;
    view.current_sample_snapshot = snapshot;
    view.sources = {
        {
            source_path,
            "source file",
            std::string{"npy"},
            spectiary::SourceCollectionSourceState::Loaded,
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
    const spectiary::SourceCollectionSessionView before = view;

    int submit_count = 0;
    int launch_count = 0;
    std::optional<std::filesystem::path> launched_path;
    spectiary::PanelSessionInteraction interaction(
        [&](spectiary::SourceCollectionSessionIntent,
            std::optional<spectiary::NavigationLatencyInputKind>) {
            ++submit_count;
            return spectiary::SourceCollectionSessionResult{};
        },
        [&view]() -> const spectiary::SourceCollectionSessionView& {
            return view;
        });
    spectiary::SourceCollectionPanelUi panel;
    bool open = true;
    const spectiary::SourceCollectionPathLauncher launch_source =
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
            spectiary::UiLanguage::English,
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

    spectiary::test::WidgetHarness ui{render_frame,
        spectiary::test::WidgetHarness::FrameMode::ExistingContext};
    ui.Frames(2);
    ui.Click("type", ImGuiMouseButton_Right);
    Require(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
        ImGuiPopupFlags_AnyPopupLevel), "source row should open its context menu");
    ui.Click("OpenSourceInNewInstance");
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
    const spectiary::SourceCollectionSessionAction pending_action =
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
        spectiary::SourceCollectionPanelUiTestAccess::SourceLaunchError(panel) &&
            *spectiary::SourceCollectionPanelUiTestAccess::SourceLaunchError(panel) ==
                "test process creation failure",
        "a launcher failure should remain visible as a Files-panel diagnostic");
}
void TestFilesPanelContextActionIsDisabledForIneligiblePath()
{
    ScopedImGuiContext context;
    const std::filesystem::path loaded_path =
        std::filesystem::path{L"C:\\观测 data\\loaded source.npy"};
    auto snapshot = std::make_shared<spectiary::SpectrumSnapshot>();
    snapshot->source.path = loaded_path;
    snapshot->source.display_name = "loaded source";
    snapshot->collection.spectrum_count = 1;

    spectiary::SourceCollectionSessionView view;
    view.snapshot = snapshot;
    view.current_sample_snapshot = snapshot;
    view.sources = {
        {
            {},
            "unavailable source",
            std::string{"npy"},
            spectiary::SourceCollectionSourceState::Unavailable,
        },
    };
    view.current_source_index = 0;

    int launch_count = 0;
    spectiary::PanelSessionInteraction interaction(
        [](spectiary::SourceCollectionSessionIntent,
           std::optional<spectiary::NavigationLatencyInputKind>) {
            return spectiary::SourceCollectionSessionResult{};
        },
        [&view]() -> const spectiary::SourceCollectionSessionView& {
            return view;
        });
    spectiary::SourceCollectionPanelUi panel;
    bool open = true;
    bool cover_source_context_cell = false;
    const spectiary::SourceCollectionPathLauncher launch_source =
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
            spectiary::UiLanguage::English,
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
            ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_Always);
            ImGui::Begin("Source context occluder", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            ImGui::End();
        }
        ImGui::EndFrame();
    };

    spectiary::test::WidgetHarness ui{render_frame,
        spectiary::test::WidgetHarness::FrameMode::ExistingContext};
    ui.Frames(2);
    const auto type_widget = ui.Find("type");
    // Exercise the cell's left padding, outside the InvisibleButton itself.
    const float padding = ImGui::GetStyle().CellPadding.x;
    Require(padding > 0.0f, "cell padding fixture must have positive width");
    const ImVec2 cell(type_widget.raw_bounds.Min.x - padding * 0.5f,
        type_widget.bounds.GetCenter().y);
    Require(!type_widget.raw_bounds.Contains(cell),
        "padding right-click must be outside the widget bounds");
    cover_source_context_cell = true;
    ImGui::GetIO().AddMousePosEvent(cell.x, cell.y);
    ui.Frames(2);
    // Raw pointer input is intentional here: this tests occlusion, not activation.
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    ui.Frames();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    ui.Frames(2);
    Require(!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
        ImGuiPopupFlags_AnyPopupLevel), "an occluded cell must not open its context menu");
    cover_source_context_cell = false;
    ui.Frames(2);
    ImGui::GetIO().AddMousePosEvent(cell.x, cell.y);
    ui.Frames(2);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    ui.Frames();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    ui.Frames(2);
    const auto action = ui.Find("OpenSourceInNewInstance");
    Require(action.disabled, "ineligible source action must be disabled");
    const auto center = action.bounds.GetCenter();
    ImGui::GetIO().AddMousePosEvent(center.x, center.y);
    ui.Frames();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    ui.Frames();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    ui.Frames();
    Require(launch_count == 0, "disabled source action must not launch");
    Require(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
        ImGuiPopupFlags_AnyPopupLevel), "disabled action must leave the popup open");
}
}
void TestFailedSourceRowWithoutSnapshotIsDisabledAndRemovable()
{
    ScopedImGuiContext context;
    spectiary::SourceCollectionSessionView view;
    view.sources = {{
        std::filesystem::path{"missing.csv"}, "missing.csv", {},
        spectiary::SourceCollectionSourceState::Unavailable,
        spectiary::SourceCollectionLoadError{
            spectiary::SourceCollectionLoadErrorKind::BackgroundLoadingFailed,
            "The saved source no longer exists"},
    }};
    view.current_source_index = 0;
    int submissions = 0;
    spectiary::PanelSessionInteraction interaction(
        [&](auto, auto) {
            ++submissions;
            view.sources.clear();
            return spectiary::SourceCollectionSessionResult{};
        },
        [&]() -> const spectiary::SourceCollectionSessionView& { return view; });
    spectiary::SourceCollectionPanelUi panel;
    bool open = true;
    const auto render_frame = [&]() {
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::GetIO().DisplaySize = ImVec2(900, 700);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_Always);
        panel.RenderFiles(interaction, spectiary::UiLanguage::English, &open,
            []() -> std::optional<std::filesystem::path> { return {}; },
            []() -> std::optional<std::filesystem::path> { return {}; },
            [](const auto&) {}, {});
        ImGui::EndFrame();
    };
    spectiary::test::WidgetHarness ui{render_frame,
        spectiary::test::WidgetHarness::FrameMode::ExistingContext};
    ui.Frames(2);
    for (const auto* cell : {"source", "type", "state"}) {
        const auto widget = ui.Find(cell);
        Require(widget.disabled, "every failed source cell must be disabled even without a snapshot");
        const auto center = widget.bounds.GetCenter();
        ImGui::GetIO().AddMousePosEvent(center.x, center.y);
        ui.Frames(2);
        bool tooltip_visible = false;
        for (const auto* window : GImGui->Windows) {
            tooltip_visible |= window->Active && (window->Flags & ImGuiWindowFlags_Tooltip) != 0;
        }
        Require(tooltip_visible, "disabled source cells must still explain their failure on hover");
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        ui.Frames();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        ui.Frames();
        Require(submissions == 0, "clicking a failed row must not select it");
    }
    Require(!ui.Find("remove").disabled, "failed row removal must remain enabled");
    ui.Click("remove");
    Require(submissions == 1 && view.sources.empty(), "failed row removal must submit normally");
}

void TestFilesShellDropHitTesting()
{
    ScopedImGuiContext context;
    spectiary::SourceCollectionSessionView view;
    spectiary::PanelSessionInteraction interaction(
        [](auto, auto) { return spectiary::SourceCollectionSessionResult{}; },
        [&]() -> const spectiary::SourceCollectionSessionView& { return view; });
    spectiary::SourceCollectionPanelUi panel;
    bool open = true;
    const auto frame = [&](bool render_files, bool overlap, bool collapsed, bool modal = false) {
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::GetIO().DisplaySize = ImVec2(900, 700);
        ImGui::NewFrame();
        if (render_files) {
            ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_Always);
            ImGui::SetNextWindowCollapsed(collapsed, ImGuiCond_Always);
            panel.RenderFiles(interaction, spectiary::UiLanguage::English, &open,
                []() -> std::optional<std::filesystem::path> { return {}; },
                []() -> std::optional<std::filesystem::path> { return {}; },
                [](const auto&) {}, {});
        }
        if (overlap) {
            ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(200, 200), ImGuiCond_Always);
            ImGui::SetNextWindowFocus();
            ImGui::Begin("Cover");
            ImGui::End();
        }
        if (modal) {
            ImGui::OpenPopup("BlockFiles");
            ImGui::SetNextWindowPos(ImVec2(750, 100), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(100, 100), ImGuiCond_Always);
            if (ImGui::BeginPopupModal("BlockFiles")) ImGui::EndPopup();
        }
        ImGui::EndFrame();
    };
    frame(true, false, false);
    frame(true, false, false);
    Require(panel.FilesDropViewport() != 0 && panel.HitTestFilesDrop(150, 150),
        "Files content accepts shell positions without an ImGui mouse event");
    Require(!panel.HitTestFilesDrop(10, 150) && !panel.HitTestFilesDrop(150, 25),
        "panel exterior and title bar reject shell drops");
    auto* window = ImGui::FindWindowByName(panel.FilesWindowName());
    auto* original = window->Viewport;
    auto* mouse_viewport = GImGui->MouseViewport;
    ImGuiViewportP detached;
    detached.ID = 123;
    window->Viewport = &detached;
    Require(panel.FilesDropViewport() == 123 && panel.HitTestFilesDrop(150, 150) &&
        GImGui->MouseViewport == mouse_viewport,
        "detached target uses its own viewport and restores ImGui mouse state");
    window->Viewport = original;
    panel.SetFilesDropHovered(true);
    frame(true, false, false);
    Require(ImGui::GetForegroundDrawList(window->Viewport)->VtxBuffer.Size > 0,
        "compatible shell drag draws an active target outline");
    panel.SetFilesDropHovered(false);
    frame(true, true, false);
    frame(true, true, false);
    Require(!panel.HitTestFilesDrop(150, 150), "overlapping ImGui window blocks drop");
    frame(true, false, false, true);
    frame(true, false, false, true);
    Require(!panel.HitTestFilesDrop(400, 150), "modal dialog blocks even uncovered Files content");
    frame(true, false, true);
    Require(!panel.FilesDropViewport() && !panel.HitTestFilesDrop(150, 150),
        "collapsed Files panel has no target");
    frame(false, false, false);
    Require(!panel.FilesDropViewport(), "hidden Files panel discards prior target");
}

void TestLoadFailuresAreVisibleWithoutHoverAndDoNotBecomeSources()
{
    spectiary::SourceCollectionSessionView view;
    int submissions = 0;
    spectiary::PanelSessionInteraction interaction(
        [&](auto, auto) {
            ++submissions;
            return spectiary::SourceCollectionSessionResult{};
        },
        [&]() -> const spectiary::SourceCollectionSessionView& { return view; });
    spectiary::SourceCollectionPanelUi panel;
    std::vector<spectiary::SourceCollectionLoadFailure> failures{
        {std::filesystem::path{L"C:\\观测\\empty.csv"},
            {spectiary::SourceCollectionLoadErrorKind::BackgroundLoadingFailed,
                "CSV has no spectrum rows (100% empty)."}},
        {"broken.fits", {spectiary::SourceCollectionLoadErrorKind::BackgroundLoadingFailed,
            "Unsupported FITS table.\nExpected a single spectrum."}},
    };
    const std::string diagnostic = spectiary::FormatSourceCollectionLoadFailures(
        spectiary::UiLanguage::English, failures);
    bool open = true;
    int dismiss_count = 0;
    std::string text;
    const auto render = [&]() {
        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(700, 600), ImGuiCond_Always);
        ImGui::LogToBuffer();
        const bool dismiss = panel.RenderFiles(interaction, spectiary::UiLanguage::English,
            &open, {}, {}, {}, {}, failures);
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        if (dismiss) {
            ++dismiss_count;
            failures.clear();
        }
    };
    spectiary::test::WidgetHarness ui{render};
    ui.Frames(2);
    Require(text.find("empty.csv") != std::string::npos &&
            text.find("Background source loading failed.") != std::string::npos &&
            text.find("CSV has no spectrum rows (100% empty).") != std::string::npos &&
            text.find("Unsupported FITS table.") != std::string::npos &&
            text.find("No spectrum is currently displayed.") != std::string::npos,
        "Files must show failed paths, semantics and raw diagnostics without hover or a snapshot");
    ui.Frames(20);
    Require(dismiss_count == 0 && !failures.empty() && view.sources.empty(),
        "merely displaying failures must neither acknowledge them nor add source rows");

    auto snapshot = std::make_shared<spectiary::SpectrumSnapshot>();
    snapshot->source.path = "previous.csv";
    view.current_sample_snapshot = snapshot;
    ui.Frames();
    Require(text.find("Source shown in Spectrum and Information: previous.csv") != std::string::npos &&
            text.find("empty.csv") != std::string::npos,
        "a retained snapshot must be identified separately from the failed candidate");
    ui.Click("CopySourceLoadFailures");
    Require(ImGui::GetClipboardText() == diagnostic && dismiss_count == 0,
        "copy must preserve all diagnostic text without dismissing failures");
    ui.Click("DismissSourceLoadFailures");
    ui.Frames();
    Require(dismiss_count == 1 && text.find("CSV has no spectrum rows") == std::string::npos &&
            view.current_sample_snapshot == snapshot && submissions == 0,
        "dismiss must clear the diagnostic presentation without changing the active source");
}

int main()
{
    TestLoadFailuresAreVisibleWithoutHoverAndDoNotBecomeSources();
    TestFilesShellDropHitTesting();
    TestFilesPanelAddFileForwardsCsvToInAppOpener();
    TestFilesPanelContextActionLaunchesWithoutMutatingSession();
    TestFilesPanelContextActionIsDisabledForIneligiblePath();
    TestFailedSourceRowWithoutSnapshotIsDisabledAndRemovable();
    return 0;
}
