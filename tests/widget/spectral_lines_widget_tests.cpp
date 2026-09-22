#include "imgui_widget_harness.h"
#include "ui/spectral_lines_panel.h"
#include "ui/spectral_lines_ui_identity.h"
#include "ui/spectral_lines_plain_text.h"
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
    Require(ui.Observe("scheme", SpectralLineUiId(a)).value().id != ui.Observe("scheme", SpectralLineUiId(b)).value().id, "color choices have distinct widget identities");
    ClickScoped(ui, "scheme", b);
    Require(controller.View().active_color_scheme_id == b, "second scheme selects original opaque identity");
    ui.Click("LineListColorScheme"); ClickScoped(ui, "scheme", a);
    Require(controller.View().active_color_scheme_id == a, "first scheme remains independently selectable");
    const auto tab_a = "###" + SpectralLineUiId(a);
    const auto tab_b = "###" + SpectralLineUiId(b);
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

void TestPlainTextDrawOutput()
{
    const std::string name = "Carbon ## label ### reference";
    WidgetHarness ui{[&] {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(900, 680), ImGuiCond_Always);
        ImGui::Begin("Plain text draw output");
        auto* draw = ImGui::GetWindowDrawList();
        const auto before = draw->VtxBuffer.Size;
        (void)SpectralLineTextSelectable("###choice", name);
        const auto actual = draw->VtxBuffer.Size - before;
        const auto item_id = ImGui::GetItemID();
        const auto reference_start = draw->VtxBuffer.Size;
        draw->AddText(ImVec2(30, 100), ImGui::GetColorU32(ImGuiCol_Text), name.data(), name.data() + name.size());
        Require(actual == draw->VtxBuffer.Size - reference_start,
                "selectable emits all plain-text glyphs, not just an untruncated log entry");
        Require(item_id == ImGui::GetID("###choice"), "plain drawing retains the selectable identity");
        ImGui::End();
    }};
    ui.Frames(3);
}

void TestPlainTextSelectableHitArea()
{
    bool chosen = false;
    WidgetHarness ui{[&] {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_Always);
        ImGui::Begin("Selectable hit area");
        if (ImGui::BeginCombo("Choices", "R")) {
            if (SpectralLineTextSelectable("###short", "R")) chosen = true;
            (void)SpectralLineTextSelectable("###long", "Carbon ## longer ### reference");
            ImGui::EndCombo();
        }
        (void)SpectralLineTextSelectable("###fixed", "R", false, 0, ImVec2(80, 0));
        ImGui::End();
    }};
    ui.Frames(3);
    Require(ui.Find("fixed").raw_bounds.GetWidth() < 100, "explicit selectable width stays bounded");
    ui.Click("Choices");
    const auto short_row = ui.Find("short");
    const auto long_row = ui.Find("long");
    const ImVec2 blank(long_row.bounds.Max.x - 12, short_row.bounds.GetCenter().y);
    Require(blank.x > short_row.bounds.Min.x + 80, "click is well beyond the short label");
    ImGui::GetIO().AddMousePosEvent(blank.x, blank.y); ui.Frames(2);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true); ui.Frames();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false); ui.Frames(2);
    Require(chosen, "clicking trailing blank space selects the short plain-text menu item");
    Require(!ui.Observe("short").has_value(), "successful blank-space selection closes the popup");
}

void TestPlainTextNames()
{
    test_support::TemporaryDirectory temporary;
    RuntimePaths paths; paths.application_data_root = temporary.path() / "managed";
    std::filesystem::create_directories(paths.application_data_root);
    auto list = BuiltIn(); list.id = "plain-text";
    list.name = "Carbon ## list ### reference";
    list.markers[0].name = "Carbon ## marker ### reference";
    list.grouping_views[0].name = "Carbon ## view ### reference";
    list.grouping_views[0].groups[0].name = "Carbon ## group ### reference";
    list.color_schemes = {{"red", "Carbon ## red ### reference", {{"marker", "#FF0000FF"}}},
                          {"green", "Carbon ## green ### reference", {{"marker", "#00FF00FF"}}}};
    const auto file = temporary.path() / "plain.json";
    std::string error;
    Require(SaveSpectralLineListToPathAtomic(file, list, error), "plain authored names are valid canonical data");
    SpectralLinesPanelController controller(BuiltIn(), {});
    Require(controller.OpenUserLineList(file, paths).changed, "open plain text fixture");
    (void)controller.Submit(SpectralLineStateIntent::SetGroupExpanded("base", "group", true));
    SpectralLinesPanelUi panel;
    auto snapshot = std::make_shared<SpectrumSnapshot>(); snapshot->capabilities.can_show_spectral_lines = true;
    std::string text;
    WidgetHarness ui{[&] {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(1100, 740), ImGuiCond_Always);
        ImGui::LogToBuffer();
        bool open = true; panel.Render(controller, snapshot, UiLanguage::English, &open);
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    }};
    ui.Frames(3);
    for (const auto& name : {list.name, list.markers[0].name, list.grouping_views[0].name,
                            list.grouping_views[0].groups[0].name, list.color_schemes[0].name})
        Require(text.find(name) != std::string::npos, "closed previews, tabs, groups and marker rows render full authored names");
    const auto tab = ui.Find("###" + SpectralLineUiId("base"));
    Require(tab.raw_bounds.GetWidth() >= ImGui::CalcTextSize(list.grouping_views[0].name.c_str(), nullptr, false).x,
            "tab width includes the authored suffix");
    ui.Click("LineListColorScheme");
    Require(text.find(list.color_schemes[1].name) != std::string::npos, "scheme popup renders full authored name");
    ClickScoped(ui, "scheme", "green");
    Require(controller.View().active_color_scheme_id == "green" && text.find(list.color_schemes[1].name) != std::string::npos,
            "plain scheme choice is interactive and its preview retains the full name");
    ui.Click("SpectralLineList");
    Require(text.find(list.name, text.find(list.name) + list.name.size()) != std::string::npos,
            "list popup and preview both render full authored name");
    ui.Click("opened_user_line_list");
    Require(controller.PlotSource().list == list && LoadSpectralLineListFromPath(file).list == list,
            "plain rendering preserves canonical data and the source file");

    // The editable built-in customization has a separate copy-target menu.
    Require(controller.SelectBuiltInLineList().changed, "return to editable built-in owner");
    const auto catalog_view = controller.View().grouping_views.front().id;
    Require(controller.Submit(SpectralLineStateIntent::DuplicateGroupingView(catalog_view)).changed, "duplicate built-in view");
    std::string view_id;
    for (const auto& view : controller.View().grouping_views) if (view.editable) view_id = view.id;
    Require(!view_id.empty(), "editable view exists");
    Require(controller.Submit(SpectralLineStateIntent::AddUserGroup(view_id)).changed, "add copy target");
    std::string source_id, target_id;
    for (const auto& view : controller.View().grouping_views) if (view.id == view_id)
        for (const auto& group : view.groups) {
            if (!group.marker_references.empty()) source_id = group.id;
            else if (!group.is_unassigned) target_id = group.id;
        }
    const std::string target_name = "Carbon ## target ### reference";
    Require(controller.Submit(SpectralLineStateIntent::RenameUserGroup(view_id, target_id, target_name,
        SpectralLineRenameEditState::Edited)).changed, "rename copy target");
    (void)controller.Submit(SpectralLineStateIntent::SetGroupExpanded(view_id, source_id, true));
    ui.Frames(3);
    ui.Click("marker", ImGuiMouseButton_Right);
    ui.Click("CopySpectralLineMarkerToGroup");
    Require(text.find(target_name, text.find(target_name) + target_name.size()) != std::string::npos,
            "group row and copy target menu both retain authored markup-like text");
    ui.Click("###" + SpectralLineUiId(target_id));
    bool copied = false;
    for (const auto& view : controller.View().grouping_views) if (view.id == view_id)
        for (const auto& group : view.groups) if (group.id == target_id)
            copied = group.marker_references.size() == 1 && group.marker_references[0].marker_id == "marker";
    Require(copied, "plain target label keeps the copy action bound to its canonical identity");
}

void TestGroupingViewEditing()
{
    test_support::TemporaryDirectory temporary;
    SpectralLinesPanelController controller(BuiltIn(), temporary.path() / "state.json");
    SpectralLinesPanelUi panel;
    WidgetHarness ui{[&] {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(900, 680), ImGuiCond_Always);
        bool open = true;
        panel.Render(controller, {}, UiLanguage::English, &open);
    }};
    ui.Frames(3);
    ui.Click("+");
    auto state = controller.View();
    Require(state.user_grouping_view_count == 1, "Add tab must create exactly one user view");
    std::string id;
    std::string original_name;
    for (const auto& view : state.grouping_views) {
        if (view.editable) { id = view.id; original_name = view.name; }
    }
    Require(!id.empty(), "New user view must have a stable identity");
    const std::string tab = "###" + SpectralLineUiId(id);
    const auto name = [&]() {
        for (const auto& view : controller.View().grouping_views)
            if (view.id == id) return view.name;
        throw std::runtime_error("Expected user view disappeared");
    };
    const auto rename = [&](const char* text, bool cancel) {
        ui.Click(tab, ImGuiMouseButton_Right);
        ui.Click("RenameSpectralLineGroupingView");
        ui.Click("SpectralLineGroupingViewName");
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
        ui.Key(ImGuiKey_A);
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false); ui.Frames();
        ui.Text(text);
        ui.Click(cancel ? "CancelRenameSpectralLineGroupingView" : "ConfirmRenameSpectralLineGroupingView");
    };
    rename("Discarded name", true);
    Require(name() == original_name, "Cancel rename must preserve the selected view");
    rename("Authored grouping", false);
    Require(name() == "Authored grouping", "Confirm rename must target the same stable view ID");
    ui.Click(tab, ImGuiMouseButton_Right);
    ui.Click("DuplicateSpectralLineGroupingView");
    Require(controller.View().user_grouping_view_count == 2, "Duplicate must create one independent user view");
    ui.Click(tab);
    for (bool cancel : {true, false}) {
        ui.Click(tab, ImGuiMouseButton_Right);
        ui.Click("DeleteSpectralLineGroupingView");
        ui.Click(cancel ? "CancelDeleteSpectralLineGroupingView" : "ConfirmDeleteSpectralLineGroupingView");
        Require(controller.View().user_grouping_view_count == (cancel ? 2u : 1u),
            "Delete confirmation must control whether the selected view is removed");
        if (cancel) Require(name() == "Authored grouping", "Cancel delete must retain the target identity");
    }
    for (const auto& view : controller.View().grouping_views)
        Require(view.id != id, "Delete must remove the requested view, preserving the duplicate");
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
    ClickScoped(ui, "scheme", "green");
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
    try { TestOpaqueIdentities(); TestPlainTextDrawOutput(); TestPlainTextSelectableHitArea(); TestPlainTextNames(); TestGroupingViewEditing(); TestPanel(); std::cout << "Spectral-line widget tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
