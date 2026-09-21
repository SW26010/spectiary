#include "imgui_widget_harness.h"
#include "ui/spectral_lines_panel.h"
#include "ui/spectral_lines_ui_identity.h"
#include "../helpers/temporary_directory.h"

#include <iostream>
#include <stdexcept>
#include <unordered_set>

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
void ClickScoped(WidgetHarness& ui, std::string_view label, std::string_view canonical_id)
{
    const auto click_position = [&] {
        const auto widget = ui.Observe(label, SpectralLineUiId(canonical_id));
        Require(widget.has_value() && !widget->disabled, "scoped widget exists and is enabled");
        auto position = widget->bounds.GetCenter();
        return position;
    };
    auto position = click_position();
    ImGui::GetIO().AddMousePosEvent(position.x, position.y); ui.Frames(2);
    position = click_position();
    ImGui::GetIO().AddMousePosEvent(position.x, position.y); ui.Frames();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true); ui.Frames();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false); ui.Frames(2);
}
void TestOpaqueIdentities()
{
    Require(ImHashStr("a###shared") == ImHashStr("b###shared"), "fixture reproduces ImGui syntax collision");
    std::unordered_set<ImGuiID> hashes;
    for (const auto& id : std::vector<std::string>{"", "a###shared", "b###shared", "a##shared", "用户###shared",
            "line-id-", std::string("x\0a", 3), std::string("x\0b", 3)}) {
        const auto encoded = SpectralLineUiId(id);
        Require(encoded.find('#') == std::string::npos && encoded.find('\0') == std::string::npos &&
                hashes.insert(ImHashStr(encoded.c_str())).second,
                "opaque IDs remain distinct without ImGui syntax or NUL truncation");
    }
    test_support::TemporaryDirectory temporary;
    RuntimePaths paths; paths.application_data_root = temporary.path() / "managed";
    std::filesystem::create_directories(paths.application_data_root);
    auto list = BuiltIn(); list.id = "user###shared";
    const std::string a = "a###shared", b = "b###shared", c = "c###shared";
    const std::string marker_a = "marker-a###shared", marker_b = "marker-b###shared";
    list.markers = {{marker_a, "Marker A", line_list::MarkerKind::Line, 5000, {}, {}, {}},
                    {marker_b, "Marker B", line_list::MarkerKind::Line, 6000, {}, {}, {}}};
    list.grouping_views = {{a, "View A", {{a, "Group A", {marker_a, marker_b}}, {b, "Group B", {}}}},
                          {b, "View B", {{c, "Group C", {marker_a, marker_b}}}}};
    list.color_schemes = {{a, "Red", {{marker_a, "#FF0000FF"}}}, {b, "Green", {{marker_a, "#00FF00FF"}}}};
    const auto file = temporary.path() / "opaque.json";
    std::string error;
    Require(SaveSpectralLineListToPathAtomic(file, list, error), "opaque IDs are valid canonical data");
    SpectralLinesPanelController controller(BuiltIn(), {});
    Require(controller.OpenUserLineList(file, paths).changed, "open opaque identity fixture");
    SpectralLinesPanelUi panel;
    auto snapshot = std::make_shared<SpectrumSnapshot>(); snapshot->capabilities.can_show_spectral_lines = true;
    WidgetHarness ui{[&] {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(900, 680), ImGuiCond_Always);
        bool open = true; panel.Render(controller, snapshot, UiLanguage::English, &open);
    }};
    ui.Frames(3);
    ui.Click("LineListColorScheme");
    Require(ui.Find("Red###scheme").id != ui.Find("Green###scheme").id, "color choices have distinct widget identities");
    ui.Click("Green###scheme");
    Require(controller.View().active_color_scheme_id == b, "second scheme selects original opaque identity");
    ui.Click("LineListColorScheme"); ui.Click("Red###scheme");
    Require(controller.View().active_color_scheme_id == a, "first scheme remains independently selectable");
    const auto tab_a = "View A###" + SpectralLineUiId(a);
    const auto tab_b = "View B###" + SpectralLineUiId(b);
    Require(ui.Find(tab_a).id != ui.Find(tab_b).id, "grouping tabs have distinct identities");
    ui.Click(tab_b); ui.Frames(2);
    Require(controller.View().grouping_views[1].active, "second grouping tab selects canonical ID");
    ui.Click(tab_a); ui.Frames(2);
    Require(controller.View().grouping_views[0].active, "first grouping tab remains selectable");
    Require(ui.Observe("Group A (2)", SpectralLineUiId(a)).value().id != ui.Observe("Group B (0)", SpectralLineUiId(b)).value().id,
            "groups with common ImGui suffix have distinct identities");
    ClickScoped(ui, "Group A (2)", a);
    Require(controller.View().grouping_views[0].groups[0].expanded && !controller.View().grouping_views[0].groups[1].expanded,
            "expanding first group leaves second collapsed");
    ClickScoped(ui, "Group B (0)", b);
    Require(controller.View().grouping_views[0].groups[1].expanded, "second group expands independently");
    Require(ui.Observe("##marker_visibility", SpectralLineUiId(marker_a)).value().id != ui.Observe("##marker_visibility", SpectralLineUiId(marker_b)).value().id,
            "marker visibility controls have distinct identities");
    ClickScoped(ui, "##marker_visibility", marker_b);
    auto source = controller.PlotSource();
    Require(source.visible_markers.size() == 1 && source.visible_markers[0].marker->id == marker_a,
            "second marker checkbox hides only its original canonical ID");
    ClickScoped(ui, "##marker_visibility", marker_a);
    Require(controller.PlotSource().visible_markers.empty(), "first marker checkbox toggles independently");
    ClickScoped(ui, "##marker_visibility", marker_b);
    const auto restored_source = controller.PlotSource();
    Require(restored_source.visible_markers.size() == 1 && restored_source.visible_markers[0].marker->id == marker_b,
            "second marker can be restored independently");
    Require(restored_source.list == list && LoadSpectralLineListFromPath(file).list == list,
            "widget encoding never rewrites canonical identities or file contents");
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
    Require(ui.Observe("scheme", SpectralLineUiId("green")).has_value(), "canonical schemes appear in chooser");
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
    try { TestOpaqueIdentities(); TestPanel(); std::cout << "Spectral-line widget tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
