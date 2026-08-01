#include "ui/source_collection_panel.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

namespace specforge {

struct SourceCollectionPanelUiTestAccess {
    [[nodiscard]] static bool IsSequencePositionMove(
        const SourceCollectionSessionIntent& intent)
    {
        return intent.kind ==
                   SourceCollectionSessionIntentKind::
                       SampleNavigation &&
               intent.sample_navigation.kind ==
                   SampleNavigationIntentKind::Move &&
               intent.sample_navigation.request.kind ==
                   SampleNavigationRequestKind::
                       LocateSequencePosition;
    }

    [[nodiscard]] static const SampleNavigationRequest&
    MoveRequest(const SourceCollectionSessionIntent& intent)
    {
        return intent.sample_navigation.request;
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
                          IsSequencePositionMove(intent),
                      "sequence input should submit a sequence-position navigation request");
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

    specforge::SourceCollectionSessionView view;
    int submission_count = 0;
    std::vector<specforge::SampleNavigationRequest>
        submitted_requests;
    specforge::SourceCollectionPanelUi panel;
    specforge::PanelSessionInteraction interaction;
    bool navigation_tab_visible_last_frame = false;

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
