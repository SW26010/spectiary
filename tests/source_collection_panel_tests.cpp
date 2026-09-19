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

namespace spectiary {

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






};

}  // namespace spectiary

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



void TestReopenableSourcePathEligibility()
{
    Require(
        !spectiary::IsReopenableSourcePath({}),
        "an empty source path must not be reopenable");
    Require(
        spectiary::IsReopenableSourcePath(
            std::filesystem::path{L"relative folder\\观测 file.npy"}),
        "relative filesystem source paths should be reopenable");
    Require(
        spectiary::IsReopenableSourcePath(
            std::filesystem::path{L"C:\\观测 data\\source file.npy"}),
        "Unicode and space-containing filesystem source paths should be reopenable");

    const std::filesystem::path::string_type invalid_native{
        L"source\0path",
        11};
    Require(
        !spectiary::IsReopenableSourcePath(
            std::filesystem::path{invalid_native}),
        "a source path containing an embedded NUL must not be reopenable");
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
                  spectiary::SourceCollectionSessionIntent intent,
                  std::optional<
                      spectiary::NavigationLatencyInputKind>) {
                  ++submission_count;
                  Require(
                      spectiary::SourceCollectionPanelUiTestAccess::
                          IsNavigationNumberMove(intent),
                      "numeric navigation input should submit a direct navigation request");
                  submitted_requests.push_back(
                      spectiary::SourceCollectionPanelUiTestAccess::
                          MoveRequest(intent));
                  spectiary::SourceCollectionSessionResult result;
                  result.action.navigation_inputs_changed = true;
                  return result;
              },
              [this]()
                  -> const spectiary::
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
        spectiary::SampleWorkflowShortcut shortcut;
        bool open = true;
        if (presentation !=
            NavigationFramePresentation::Hidden) {
            ImGui::SetNextWindowCollapsed(
                presentation ==
                    NavigationFramePresentation::Collapsed,
                ImGuiCond_Always);
            panel.RenderNavigation(
                interaction,
                spectiary::UiLanguage::English,
                live_numeric_navigation,
                &open,
                shortcut);
        }
        if (after_navigation) {
            after_navigation();
        }
        panel.FinalizeNavigationInputEdits(
            interaction);
        const spectiary::SourceCollectionSessionAction action =
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
                spectiary::SourceCollectionPanelUi::
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
            : spectiary::SourceCollectionPanelUi::
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

        spectiary::SampleWorkflowShortcut shortcut;
        bool open = true;
        panel.RenderNavigation(
            interaction,
            spectiary::UiLanguage::English,
            live_numeric_navigation,
            &open,
            shortcut);
        ImGuiWindow* navigation_window =
            ImGui::FindWindowByName(
                spectiary::SourceCollectionPanelUi::
                    NavigationWindowName());
        navigation_tab_visible_last_frame =
            navigation_window != nullptr &&
            navigation_window->DockTabIsVisible;

        if (!select_sibling_before_navigation) {
            ImGui::Begin(kDockSiblingWindow);
            ImGui::End();
        }

        panel.FinalizeNavigationInputEdits(interaction);
        const spectiary::SourceCollectionSessionAction action =
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
                spectiary::SourceCollectionPanelUi::
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
                spectiary::SourceCollectionPanelUi::
                    NavigationWindowName());
        Require(
            window != nullptr,
            "Navigation window should exist");
        return window->GetID(
            "##SampleNavigationSample");
    }

    spectiary::SourceCollectionSessionView view;
    int submission_count = 0;
    std::vector<spectiary::SampleNavigationRequest>
        submitted_requests;
    spectiary::SourceCollectionPanelUi panel;
    spectiary::PanelSessionInteraction interaction;
    bool navigation_tab_visible_last_frame = false;
    bool live_numeric_navigation = false;

private:
    static constexpr const char* kDockHostWindow =
        "Navigation Test Dock Host";
    static constexpr const char* kDockspaceId =
        "NavigationTestDockspace";
    static constexpr const char* kDockSiblingWindow =
        "Other###SpectiaryNavigationTestSibling";
    bool docking_initialized_ = false;
};

template <typename RenderFrame>
void ReplaceActiveInputText(RenderFrame render_frame, std::string_view text)
{
    auto& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_A, true);
    render_frame();
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    render_frame();
    if (text.empty()) {
        io.AddKeyEvent(ImGuiKey_Backspace, true);
        render_frame();
        io.AddKeyEvent(ImGuiKey_Backspace, false);
    } else {
        io.AddInputCharactersUTF8(std::string(text).c_str());
    }
    render_frame();
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
                spectiary::SampleNavigationRequestKind::LocateRow &&
            fixture.submitted_requests[0].row_index == 3 &&
            fixture.submitted_requests[1].kind ==
                spectiary::SampleNavigationRequestKind::LocateRow &&
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
                spectiary::SampleNavigationRequestKind::
                    LocateSequencePosition &&
            fixture.submitted_requests[0].sequence_position == 3 &&
            fixture.submitted_requests[1].kind ==
                spectiary::SampleNavigationRequestKind::
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
    for (const std::string_view value : {"", "0"}) {
        ReplaceActiveInputText([&] { fixture.RenderFrame(); }, value);
        fixture.RenderFrame();
    }
    Require(
        fixture.submission_count == 0,
        "empty and zero live source values must not submit");

    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "101");
    fixture.RenderFrame();
    Require(fixture.submitted_requests.size() == 2 &&
        fixture.submitted_requests[0].row_index == 0 &&
        fixture.submitted_requests[1].row_index == 9,
        "typing 101 must submit valid prefixes 1 and 10, but reject out-of-range 101");

    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "4");
    fixture.RenderFrame();
    Require(
        fixture.submission_count == 3,
        "a valid live source value should submit before topology replacement");
    fixture.view.navigation.current_index = 7;
    fixture.view.navigation.current_source_row = 7;
    ++fixture.view.navigation.sequence_topology_revision;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "45");
    fixture.RenderFrame();
    Require(
        fixture.submission_count == 3 &&
            GImGui->ActiveId != source_input_id,
        "topology replacement should deactivate the old live edit and prevent further requests");

    fixture.view.navigation.row_location_available = false;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    ImGui::ActivateItemByID(source_input_id);
    fixture.RenderFrame();
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "45");
    fixture.RenderFrame();
    Require(
        fixture.submission_count == 3,
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
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "6");
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
    ReplaceActiveInputText([&] { fixture.RenderDockedFrame(); }, "6");
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

    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "4");
    fixture.RenderFrame();
    Require(
        fixture.submission_count == 0,
        "the first valid digit of a multi-digit source draft must not navigate");

    fixture.view.navigation.current_index = 29;
    fixture.view.navigation.current_source_row = 29;
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "45");
    ImGui::ClearActiveID();
    fixture.RenderFrame();

    Require(
        fixture.submitted_requests.size() == 1 &&
            fixture.submitted_requests.front().kind ==
                spectiary::SampleNavigationRequestKind::LocateRow &&
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
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "45");
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
                spectiary::SampleNavigationRequestKind::LocateRow &&
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
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "45");
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
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "2");
    fixture.RenderFrame();
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "20");
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
                spectiary::SampleNavigationRequestKind::LocateRow &&
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
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "45");
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
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "45");
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
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "45");
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
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "0");
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
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "101");
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
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "6");
    fixture.RenderFrame();

    fixture.RenderFrame({}, presentation);

    Require(
        fixture.submitted_requests.size() == 1 &&
            fixture.submitted_requests.front().kind ==
                spectiary::SampleNavigationRequestKind::LocateRow &&
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
    ReplaceActiveInputText([&] { fixture.RenderDockedFrame(); }, "6");
    fixture.RenderDockedFrame();
    fixture.RenderDockedFrame(true);

    Require(
        !fixture.navigation_tab_visible_last_frame,
        "the sibling window should cover the Navigation dock tab");
    Require(
        fixture.submitted_requests.size() == 1 &&
            fixture.submitted_requests.front().kind ==
                spectiary::SampleNavigationRequestKind::LocateRow &&
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
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "6");
    fixture.RenderFrame();

    ImGui::ActivateItemByID(sequence_input_id);
    fixture.RenderFrame();
    fixture.RenderFrame();
    Require(
        fixture.submitted_requests.size() == 1 &&
            fixture.submitted_requests.front().kind ==
                spectiary::SampleNavigationRequestKind::LocateRow &&
            fixture.submitted_requests.front().row_index == 5,
        "moving focus from source to sequence should route the source blur commit to LocateRow");
    Require(
        GImGui->ActiveId == sequence_input_id,
        "the sequence input should remain active after the source blur commit is finalized");

    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "7");
    ImGui::ClearActiveID();
    fixture.RenderFrame();
    Require(
        fixture.submitted_requests.size() == 2 &&
            fixture.submitted_requests.back().kind ==
                spectiary::SampleNavigationRequestKind::
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
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "4");
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

    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "4");
    fixture.RenderFrame();
    Require(
        fixture.submission_count == 0,
        "the first valid digit of a multi-digit sequence draft must not submit deferred navigation");
    fixture.panel.SyncNavigationInputs(
        fixture.view.navigation);

    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "45");
    ImGui::ClearActiveID();
    fixture.RenderFrame();
    Require(
        fixture.submission_count == 1,
        "the complete multi-digit sequence draft should submit exactly once on deactivation");
    Require(
        fixture.submitted_requests.size() == 1 &&
            fixture.submitted_requests.front().kind ==
                spectiary::SampleNavigationRequestKind::
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
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "45");
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
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "45");
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
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "2");
    fixture.RenderFrame();
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "20");
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
                spectiary::SampleNavigationRequestKind::
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
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "45");
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
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "6");
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
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "6");
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
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "6");
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
    ReplaceActiveInputText([&] { fixture.RenderFrame(); }, "6");
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
    ReplaceActiveInputText([&] { fixture.RenderDockedFrame(); }, "6");
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
    TestReopenableSourcePathEligibility();
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
