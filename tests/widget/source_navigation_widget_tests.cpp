#include "imgui_widget_harness.h"
#include "ui/source_collection_panel.h"

#include <iostream>
#include <stdexcept>

namespace spectiary {
struct SourceCollectionPanelUiTestAccess {
    static SampleNavigationRequest NavigationRequest(const SourceCollectionSessionIntent& intent)
    {
        if (intent.kind != SourceCollectionSessionIntentKind::SampleNavigation ||
            intent.sample_navigation.kind != SampleNavigationIntentKind::Move)
            throw std::runtime_error("Numeric input emitted an unexpected intent");
        return intent.sample_navigation.request;
    }
};
}

namespace {
using namespace spectiary;
using test::WidgetHarness;
void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

struct NavigationFixture {
    SourceCollectionSessionView view;
    SourceCollectionPanelUi panel;
    std::vector<SampleNavigationRequest> requests;
    bool live;
    PanelSessionInteraction interaction{
        [this](SourceCollectionSessionIntent intent, std::optional<NavigationLatencyInputKind>) {
            requests.push_back(SourceCollectionPanelUiTestAccess::NavigationRequest(intent));
            return SourceCollectionSessionResult{};
        },
        [this]() -> const SourceCollectionSessionView& { return view; }};
    WidgetHarness ui{[this] {
        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(520, 300), ImGuiCond_Always);
        bool open = true;
        SampleWorkflowShortcut shortcut;
        panel.RenderNavigation(interaction, UiLanguage::English, live, &open, shortcut);
        ImGui::SetNextWindowPos(ImVec2(560, 20), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(200, 100), ImGuiCond_Always);
        ImGui::Begin("Other controls");
        ImGui::Button("Finish editing");
        ImGui::End();
        panel.FinalizeNavigationInputEdits(interaction);
    }};

    NavigationFixture(bool sequence, bool live_input) : live(live_input)
    {
        auto& nav = view.navigation;
        nav.has_active_source = true;
        nav.current_index = 19;
        nav.current_source_row = 19;
        nav.sample_count = 100;
        nav.row_location_available = !sequence;
        nav.sequence_active = sequence;
        nav.sequence_count = sequence ? 100 : 0;
        nav.sequence_topology_revision = 1;
        if (sequence) nav.current_sequence_position = 19;
        panel.SyncNavigationInputs(nav);
        ui.Frames(3);
    }

    void Edit(bool sequence, const char* text)
    {
        ui.Click(sequence ? "##SampleNavigationSequence" : "##SampleNavigationSample");
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
        ui.Key(ImGuiKey_A);
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
        ui.Frames();
        ui.Text(text);
    }

    void Check(bool sequence, std::size_t index)
    {
        Require(!requests.empty(), "Numeric editing must submit a request");
        const auto& request = requests.back();
        Require(request.kind == (sequence ? SampleNavigationRequestKind::LocateSequencePosition :
            SampleNavigationRequestKind::LocateRow), "Numeric input must preserve location kind");
        Require((sequence ? request.sequence_position : request.row_index) == index,
            "UI positions must convert to zero-based request indices");
    }
};

void TestNumericControls(bool sequence)
{
    for (bool blur : {false, true}) {
        NavigationFixture f(sequence, false);
        f.Edit(sequence, "45");
        Require(f.requests.empty(), "Non-live editing must wait for commit");
        if (blur) f.ui.Click("Finish editing");
        else f.ui.Key(ImGuiKey_Enter);
        f.Check(sequence, 44);
        f.ui.Frames(3);
        Require(f.requests.size() == 1, "Enter or blur must commit exactly once");
    }
    {
        NavigationFixture f(sequence, false);
        f.Edit(sequence, "45");
        f.ui.Key(ImGuiKey_Escape);
        f.ui.Click("Finish editing");
        Require(f.requests.empty(), "Escape must cancel an uncommitted draft");
    }
    {
        NavigationFixture f(sequence, true);
        f.Edit(sequence, "45");
        Require(f.requests.size() == 2, "Live input must submit each valid prefix in the input frame");
        const auto first = f.requests.front();
        Require((sequence ? first.sequence_position : first.row_index) == 3,
            "First live prefix must locate UI position 4");
        f.Check(sequence, 44);
        f.ui.Key(ImGuiKey_Escape);
        f.ui.Click("Finish editing");
        Require(f.requests.size() == 2, "Escape and blur must not repeat or reverse live navigation");
    }
}
}

int main()
{
    try {
        TestNumericControls(false);
        TestNumericControls(true);
        std::cout << "Navigation widget tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
