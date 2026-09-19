#include "overlays/built_in_spectral_line_state_io.h"
#include "overlays/built_in_spectral_line_reconciliation.h"
#include "helpers/temporary_directory.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
void Run()
{
    using namespace spectiary;
    using Json = nlohmann::json;
    SpectralLineList base;
    base.id = "public-spectral-lines.v1"; base.name = "Base";
    base.markers = {{"a", "A", line_list::MarkerKind::Line, 200, {}, {}, {}},
                    {"b", "B", line_list::MarkerKind::Line, 100, {}, {}, {}}};
    base.color_schemes = {{"s", "Default", {{"a", "#111111FF"}, {"b", "#222222FF"}}}};
    test_support::TemporaryDirectory directory;
    const auto path = directory.path() / "state.json";
    Require(LoadBuiltInSpectralLineState(path, base).error.empty(), "missing state is first write");
    BuiltInSpectralLineState initial, a, b, merged;
    std::string error;
    Require(SetBuiltInMarkerColor(base, a.overlay, "s", "a", "#FF0000FF", error), error);
    Require(SetBuiltInMarkerColor(base, b.overlay, "s", "b", "#00FF00FF", error), error);
    BuiltInSpectralLineTaskIntent intent;
    intent.marker_colors.insert({"s", "a"});
    Require(ReconcileBuiltInSpectralLineTask(base, initial, a, b, intent, merged, error), error);
    const auto& colors = merged.overlay.color_schemes->front().colors;
    Require(colors.at("a") == "#FF0000FF" && colors.at("b") == "#00FF00FF", "concurrent first overrides preserve disjoint fields");
    Require(SaveBuiltInSpectralLineState(path, base, merged, error), error);
    auto loaded = LoadBuiltInSpectralLineState(path, base);
    NormalizeSpectralLineSession(merged.session, *ComposeBuiltInSpectralLineList(base, merged.overlay).list);
    Require(loaded.error.empty() && loaded.state == merged, "state round trip");
    auto restored = merged;
    restored.overlay.color_schemes.reset();
    intent.whole_color_collection = true;
    Require(ReconcileBuiltInSpectralLineTask(base, merged, restored, b, intent, loaded.state, error) && !loaded.state.overlay.color_schemes,
            "whole restore owns entire collection");
    intent = {};
    auto edited = merged;
    Require(SetBuiltInMarkerColor(base, edited.overlay, "s", "a", "#0000FFFF", error), error);
    intent.marker_colors.insert({"s", "a"});
    Require(ReconcileBuiltInSpectralLineTask(base, merged, edited, restored, intent, loaded.state, error), error);
    Require(loaded.state.overlay.color_schemes->front().colors.at("b") == "#222222FF", "stale ordinary edit after restore cannot resurrect untouched fields");
    auto cleared = merged; cleared.overlay.color_schemes = std::vector<line_list::ColorScheme>{};
    Require(!ReconcileBuiltInSpectralLineTask(base, merged, edited, cleared, intent, loaded.state, error), "deleted scheme conflicts and preserves edit");
    auto groups = initial;
    groups.overlay.grouping_views = {{"v", "View", {{"g", "Group", {"a"}}}}};
    auto edit_group = groups; edit_group.overlay.grouping_views[0].groups[0].name = "Renamed";
    auto delete_group = groups; delete_group.overlay.grouping_views[0].groups.clear();
    Require(!ReconcileBuiltInSpectralLineTask(base, groups, edit_group, delete_group, {}, loaded.state, error), "deleted group conflicts");
    auto add_a = groups, add_b = groups;
    add_a.overlay.grouping_views[0].groups.push_back({"new", "A", {"a"}});
    add_b.overlay.grouping_views[0].groups.push_back({"new", "B", {"b"}});
    Require(ReconcileBuiltInSpectralLineTask(base, groups, add_a, add_b, {}, loaded.state, error), error);
    Require(loaded.state.overlay.grouping_views[0].groups.size() == 3, "concurrent ID collision preserves both additions");

    Json legacy = {{"format_kind", "spectiary.catalog_user_state.cache"}, {"schema_version", 6},
        {"catalogs", {{base.id, {{"active_view_id", "v"}, {"marker_visibility", {{"a", true}}},
            {"marker_colors", {{"a", {{"mode", "explicit-color"}, {"red", "0.5"}, {"green", "0"}, {"blue", "1"}, {"alpha", "1"}}}}},
            {"grouping_views", Json::array({{{"id", "v"}, {"name", "View"}, {"name_source", "default_grouping_view"}, {"name_ordinal", 1},
                {"groups", Json::array({{{"id", "g"}, {"name", "Unassigned"}, {"is_unassigned", false},
                     {"marker_references", Json::array({{{"catalog_identity", base.id}, {"marker_id", "a"}}, {{"catalog_identity", base.id}, {"marker_id", "b"}}})}},
                    {{"id", "__unassigned__"}, {"name", "Unassigned"}, {"is_unassigned", true}, {"marker_references", Json::array()}}})}}})}}}}},
        {"catalog_panel_state", {{base.id, {{"expanded_group_ids", {"v/g"}}}}}}};
    const auto write = [&](const Json& value) { std::ofstream(path) << value.dump(2); };
    write(legacy);
    loaded = LoadBuiltInSpectralLineState(path, base);
    Require(loaded.error.empty() && loaded.requires_save, loaded.error);
    Require(loaded.state.overlay.grouping_views[0].groups.size() == 1 &&
            loaded.state.overlay.grouping_views[0].groups[0].marker_ids == std::vector<std::string>{"b", "a"},
            "migration materializes display order and removes only system Unassigned");
    Require(loaded.state.overlay.color_schemes->front().colors.at("a") == "#8000FFFF", "legacy color quantization");
    Require(loaded.state.session.view_names.at("v").source == GeneratedNameSource::DefaultGroupingView, "generated name provenance retained outside canonical views");
    Require(SaveBuiltInSpectralLineState(path, base, loaded.state, error), error);
    Require(!LoadBuiltInSpectralLineState(path, base).requires_save, "new state is sole active owner");
    auto invalid = legacy;
    invalid["catalogs"][base.id]["grouping_views"][0]["groups"][0]["marker_references"][0]["marker_id"] = "missing";
    write(invalid);
    Require(!LoadBuiltInSpectralLineState(path, base).error.empty(), "unresolved legacy references rejected");
    invalid = legacy; invalid["schema_version"] = 5; write(invalid);
    Require(LoadBuiltInSpectralLineState(path, base).issue_kind == VersionedJsonCacheLoadIssueKind::UnsupportedFormatOrSchema,
            "only immediately preceding schema supported");
    write(legacy);
    const auto old_size = std::filesystem::file_size(path);
    Require(!SaveBuiltInSpectralLineState(path, base, merged, error, [](const auto&, const auto&) { throw std::runtime_error("interruption"); }),
            "interrupted replacement fails");
    Require(std::filesystem::file_size(path) == old_size && LoadBuiltInSpectralLineState(path, base).requires_save,
            "interrupted publication leaves complete prior schema");
    auto no_colors = legacy; no_colors["catalogs"][base.id]["marker_colors"] = Json::object(); write(no_colors);
    Require(!LoadBuiltInSpectralLineState(path, base).state.overlay.color_schemes, "colorless migration does not create override");
}
}
int main()
{
    try { Run(); std::cout << "Built-in state tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
