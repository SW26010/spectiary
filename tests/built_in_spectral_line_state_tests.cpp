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
    merged.session.expanded_groups = {{"a/b", "c"}, {"a", "b/c"}};
    Require(SaveBuiltInSpectralLineState(path, base, merged, error), error);
    auto loaded = LoadBuiltInSpectralLineState(path, base);
    NormalizeSpectralLineSession(merged.session, *ComposeBuiltInSpectralLineList(base, merged.overlay).list);
    Require(loaded.error.empty() && loaded.state == merged, "state round trip");
    const auto read = [&] { std::ifstream stream(path); return Json::parse(stream); };
    const auto schema_eight = read();
    Require(schema_eight.at("schema_version") == 8 &&
            schema_eight.at("catalogs").at(base.id).at("session").at("expanded_groups") ==
                Json::array({{{"view_id", "a"}, {"group_id", "b/c"}}, {{"view_id", "a/b"}, {"group_id", "c"}}}) &&
            !schema_eight.at("catalogs").at(base.id).at("session").contains("expanded_group_ids"),
            "schema 8 writes ordered pair objects without delimiter encoding");
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
    add_a.session.expanded_groups.insert({"v", "new"});
    add_a.session.group_names["new"] = {GeneratedNameSource::DefaultGroup, 2, 0, {}};
    add_b.session.group_names["new"] = {GeneratedNameSource::DefaultGroup, 3, 0, {}};
    Require(ReconcileBuiltInSpectralLineTask(base, groups, add_a, add_b, {}, loaded.state, error), error);
    Require(loaded.state.overlay.grouping_views[0].groups.size() == 3, "concurrent ID collision preserves both additions");
    Require(loaded.state.session.group_names.at("new").ordinal == 3 && loaded.state.session.group_names.at("new-2").ordinal == 2,
            "remapping must preserve each writer's localization metadata independently");

    Require(loaded.state.session.expanded_groups.contains({"v", "new-2"}) &&
            !loaded.state.session.expanded_groups.contains({"v", "new"}),
            "group remap moves expansion to the local writer's new identity");
    auto view_a = initial, view_b = initial;
    view_a.overlay.grouping_views = {{"v/x", "A", {{"g/x", "A", {"a"}}}}};
    view_b.overlay.grouping_views = {{"v/x", "B", {{"g/x", "B", {"b"}}}}};
    view_a.session.expanded_groups.insert({"v/x", "g/x"});
    Require(ReconcileBuiltInSpectralLineTask(base, initial, view_a, view_b, {}, loaded.state, error), error);
    Require(loaded.state.session.expanded_groups.contains({"v/x-2", "g/x-2"}) &&
            !loaded.state.session.expanded_groups.contains({"v/x", "g/x"}),
            "simultaneous view/group remap uses the original opaque view identity");

    Json legacy = {{"format_kind", "spectiary.catalog_user_state.cache"}, {"schema_version", 6},
        {"catalogs", {{base.id, {{"active_view_id", "v"}, {"marker_visibility", {{"a", true}}},
            {"marker_colors", {{"a", {{"mode", "explicit-color"}, {"red", "0.5"}, {"green", "0"}, {"blue", "1"}, {"alpha", "1"}}}}},
            {"grouping_views", Json::array({{{"id", "v"}, {"name", "View"}, {"name_source", "default_grouping_view"}, {"name_ordinal", 1},
                {"groups", Json::array({{{"id", "g"}, {"name", "Unassigned"}, {"is_unassigned", false},
                     {"marker_references", Json::array({{{"catalog_identity", base.id}, {"marker_id", "a"}}, {{"catalog_identity", base.id}, {"marker_id", "b"}}})}},
                    {{"id", "__unassigned__"}, {"name", "Unassigned"}, {"is_unassigned", true}, {"marker_references", Json::array()}}})}}})}}}}},
        {"catalog_panel_state", {{base.id, {{"expanded_group_ids", {"v/g"}}}}}}};
    const auto write = [&](const Json& value) { std::ofstream(path) << value.dump(2); };
    auto seven = schema_eight;
    seven["schema_version"] = 7;
    auto& seven_session = seven["catalogs"][base.id]["session"];
    seven_session.erase("expanded_groups");
    seven_session["expanded_group_ids"] = {"v/g"};
    write(seven);
    loaded = LoadBuiltInSpectralLineState(path, base);
    Require(loaded.error.empty() && loaded.requires_save &&
            loaded.state.session.expanded_groups == std::set<SpectralLineGroupExpansionKey>{{"v", "g"}},
            "schema 7 migrates expansion pairs and requests save");
    Require(SaveBuiltInSpectralLineState(path, base, loaded.state, error), error);
    Require(read().at("schema_version") == 8 &&
            read().at("catalogs").at(base.id).at("session").at("expanded_groups") ==
                Json::array({{{"view_id", "v"}, {"group_id", "g"}}}),
            "migrated schema 7 saves as schema 8");
    for (const Json& entries : std::vector<Json>{
            Json::array({"v/g"}), Json::array({{{"view_id", ""}, {"group_id", "g"}}}),
            Json::array({{{"view_id", "v"}, {"group_id", 1}}}),
            Json::array({{{"view_id", "v"}}}),
            Json::array({{{"view_id", "v"}, {"group_id", "g"}, {"extra", true}}}),
            Json::array({{{"view_id", "v"}, {"group_id", "g"}}, {{"view_id", "v"}, {"group_id", "g"}}})}) {
        auto invalid_eight = schema_eight;
        invalid_eight["catalogs"][base.id]["session"]["expanded_groups"] = entries;
        write(invalid_eight);
        Require(LoadBuiltInSpectralLineState(path, base).issue_kind == VersionedJsonCacheLoadIssueKind::InvalidDocument,
                "schema 8 rejects malformed or repeated expansion pairs");
    }
    for (const auto& key : {"v", "/g", "v/", "a/b/c"}) {
        auto invalid_seven = seven;
        invalid_seven["catalogs"][base.id]["session"]["expanded_group_ids"] = {key};
        write(invalid_seven);
        Require(!LoadBuiltInSpectralLineState(path, base).error.empty(), "schema 7 rejects ambiguous legacy keys");
        auto invalid_six = legacy;
        invalid_six["catalog_panel_state"][base.id]["expanded_group_ids"] = {key};
        write(invalid_six);
        Require(!LoadBuiltInSpectralLineState(path, base).error.empty(), "schema 6 rejects ambiguous legacy keys");
    }
    write(legacy);
    loaded = LoadBuiltInSpectralLineState(path, base);
    Require(loaded.error.empty() && loaded.requires_save, loaded.error);
    Require(loaded.state.session.expanded_groups.contains({"v", "g"}), "schema 6 migrates expansion pair");
    Require(loaded.state.overlay.grouping_views[0].groups.size() == 1 &&
            loaded.state.overlay.grouping_views[0].groups[0].marker_ids == std::vector<std::string>{"a", "b"},
            "migration preserves stored representation order and removes only system Unassigned");
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
            "schemas older than 6 are unsupported");
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
