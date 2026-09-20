#include "imgui_widget_harness.h"
#include "ui/spectral_lines_panel.h"
#include "../helpers/temporary_directory.h"

#include <iostream>
#include <stdexcept>

namespace {
using namespace spectiary;
using spectiary::test::WidgetHarness;
void Require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
SpectralLineList BuiltIn()
{
    SpectralLineList list;
    list.id = "public-spectral-lines.v1"; list.name = "Built-in";
    list.markers = {{"marker", "Built-in marker", line_list::MarkerKind::Line, 5000, {}, {}, {}}};
    list.grouping_views = {{"base", "Base", {{"group", "Group", {"marker"}}}}};
    return list;
}
void TestPanel()
{
    test_support::TemporaryDirectory temporary;
    RuntimePaths paths; paths.application_data_root = temporary.path() / "managed";
    std::filesystem::create_directories(paths.application_data_root);
    SpectralLinesPanelController controller(BuiltIn(), paths.application_data_root / "state" / "built-in.json");
    SpectralLinesPanelUi panel;
    auto snapshot = std::make_shared<SpectrumSnapshot>();
    snapshot->capabilities.can_show_spectral_lines = true;
    snapshot->capabilities.requires_rest_frame_warning = true;
    std::string text;
    UiLanguage language = UiLanguage::English;
    WidgetHarness ui{[&] {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(900, 680), ImGuiCond_Always);
        ImGui::LogToBuffer();
        bool open = true;
        panel.Render(controller, snapshot, language, &open);
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    }};
    ui.Frames(3);
    ui.Click("SpectralLineList");
    ui.Click("Open...");
    Require(panel.TakeOpenRequest() && !panel.TakeOpenRequest(), "Open emits exactly one picker request");

    auto list = BuiltIn(); list.id = "user"; list.name = "My list";
    list.coordinate.unit = line_list::Unit::Nanometer;
    list.grouping_views.front().name = "User grouping";
    list.color_schemes = {{"red", "Red", {{"marker", "#FF0000FF"}}}, {"green", "Green", {{"marker", "#00FF00FF"}}}};
    const auto file = temporary.path() / "list.data";
    std::string error;
    Require(SaveSpectralLineListToPathAtomic(file, list, error), "write fixture");
    Require(controller.OpenUserLineList(file, paths).changed, "open valid incompatible user list");
    ui.Frames(3);
    Require(text.find("Overlay hidden:") != std::string::npos && text.find("Read-only file.") != std::string::npos,
            "read-only and coordinate diagnostics are rendered");
    language = UiLanguage::SimplifiedChinese; ui.Frames(2);
    Require(text.find("只读文件") != std::string::npos && text.find("已隐藏叠加标记") != std::string::npos &&
            text.find("User grouping") != std::string::npos, "translated diagnostics retain authored names");
    language = UiLanguage::English; ui.Frames(2);
    Require(!ui.Observe("+").has_value(), "canonical add action absent for user owner");
    ui.Click("LineListColorScheme");
    // Color choices share a semantic label under scheme identity scopes.
    Require(ui.Observe("scheme", "green").has_value(), "canonical schemes appear in chooser");
    ui.Click("Green###scheme");
    Require(controller.View().active_color_scheme_id == "green", "color chooser selects canonical scheme without editing");
    Require(controller.Submit(SpectralLineStateIntent::SetGroupExpanded("base", "group", true)).changed, "expand fixture group");
    ui.Frames(2);
    Require(ui.Find("##ColorButton").disabled, "user color editing is disabled");
    ui.Click("SpectralLineLabels");
    Require(!controller.View().marker_labels_visible, "read-only owner retains session label interaction");
    ui.Click("SpectralLineSearch"); ui.Text("Built-in");
    Require(controller.View().grouping_view_search == "Built-in", "search remains available");
    ui.Click("SpectralLineList"); ui.Click("Built-in public line list"); ui.Frames(2);
    Require(!controller.View().user_owned && controller.View().grouping_view_search.empty() &&
            controller.View().marker_labels_visible, "selector restores independent built-in interaction");
    Require(ui.Observe("+").has_value(), "built-in customization remains available");
    ui.Click("SpectralLineList"); ui.Click("opened_user_line_list"); ui.Frames(2);
    Require(controller.View().user_owned && controller.View().grouping_view_search == "Built-in" &&
            !controller.View().marker_labels_visible, "selector restores user interaction");
    Require(controller.OpenUserLineList(temporary.path() / "missing.json", paths).status == SpectralLineStateResultStatus::Rejected,
            "missing candidate rejected");
    ui.Frames(2);
    Require(text.find("Could not open Spectral Line List") != std::string::npos && controller.View().user_owned,
            "failed open is diagnosed beside preserved owner");
    list.id = "ungrouped"; list.grouping_views.clear(); list.coordinate = {};
    Require(SaveSpectralLineListToPathAtomic(file, list, error), "write ungrouped fixture");
    Require(controller.OpenUserLineList(file, paths).changed, "open ungrouped fixture");
    Require(controller.Submit(SpectralLineStateIntent::SetGroupExpanded("", "__unassigned__", true)).changed, "expand ungrouped markers");
    ui.Frames(3);
    Require(text.find("Built-in marker") != std::string::npos && text.find("Overlay hidden:") == std::string::npos,
            "ungrouped canonical marker is inspectable and previous diagnostic clears");
    list.markers.clear(); list.color_schemes.clear();
    Require(SaveSpectralLineListToPathAtomic(file, list, error), "write empty fixture");
    Require(controller.OpenUserLineList(file, paths).changed, "open empty fixture");
    ui.Frames(3);
    Require(text.find("This line list contains no markers.") != std::string::npos,
            "valid empty line list has appropriate panel message");
}
}
int main()
{
    try { TestPanel(); std::cout << "Spectral-line widget tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
