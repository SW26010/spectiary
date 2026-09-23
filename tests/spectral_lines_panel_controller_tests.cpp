#include "overlays/spectral_line_projection.h"

#include "ui/spectral_lines_panel_controller.h"
#include "overlays/built_in_spectral_line_state_io.h"
#include "platform/exclusive_file_lease.h"
#include "helpers/temporary_directory.h"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace spectiary;
using Intent = SpectralLineStateIntent;
using Status = SpectralLineStateResultStatus;
void Require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
void Applied(const SpectralLineStateResult& result) { Require(result.status == Status::Applied, result.message); }
SpectralLineList Base()
{
    SpectralLineList list;
    list.id = "public-spectral-lines.v1"; list.name = "Public spectral lines";
    list.markers = {{"a", "Hα", line_list::MarkerKind::Line, 6564.608, {}, {}, {}},
                    {"b", "C₂", line_list::MarkerKind::Band, {}, 5100, 5200, "band note"},
                    {"c", "C I", line_list::MarkerKind::Line, 6000, {}, {}, {}}};
    list.grouping_views = {{"base-one", "Base one", {{"base-g1", "Hydrogen", {"a"}}}},
                           {"base-two", "Base two", {{"base-g2", "Carbon", {"c", "b"}}}}};
    return list;
}
const SpectralLineGroupingView& View(const SpectralLinePanelView& state, const std::string& id)
{
    for (const auto& view : state.grouping_views) if (view.id == id) return view;
    throw std::runtime_error("missing view: " + id);
}
std::string Active(const SpectralLinePanelView& state)
{
    for (const auto& view : state.grouping_views) if (view.active) return view.id;
    return {};
}
std::string CreateView(SpectralLinesPanelController& controller)
{
    Applied(controller.Submit(Intent::CreateUserGroupingView()));
    return Active(controller.View());
}
std::string AddGroup(SpectralLinesPanelController& controller, const std::string& view)
{
    Applied(controller.Submit(Intent::AddUserGroup(view)));
    auto state = controller.View();
    const auto& groups = View(state, view).groups;
    return groups.at(groups.size() - 2).id;
}
std::string Bytes(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void TestUserLineLists(const std::filesystem::path& root)
{
    const auto managed = root / "managed";
    const auto outside = root / "documents";
    std::filesystem::create_directories(managed);
    std::filesystem::create_directories(outside);
    RuntimePaths paths;
    paths.application_data_root = managed;
    paths.package_root = managed;
    paths.storage_profile = StorageProfile::Portable;
    const auto state_path = managed / "state" / "built-in.json";
    SpectralLinesPanelController controller(Base(), state_path);
    auto snapshot = std::make_shared<SpectrumSnapshot>();
    snapshot->capabilities.can_show_spectral_lines = true;
    snapshot->capabilities.requires_rest_frame_warning = true;
    const auto builtin_view = CreateView(controller);
    Applied(controller.Submit(Intent::SetMarkerVisibility("a", false)));
    Applied(controller.Submit(Intent::SetGroupingViewSearch("builtin search")));
    Applied(controller.Submit(Intent::SetMarkerLabelsVisible(false)));
    Require(controller.OpenUserLineList(outside / "missing", paths).status == Status::Rejected,
            "failed first open preserves built-in owner");
    Require(!controller.View().user_owned && Active(controller.View()) == builtin_view &&
            ProjectSpectralLineList(controller.PlotSource(), snapshot).visible_markers.size() == 2, "built-in model/session preserved");

    auto user = Base(); user.id = "user-list"; user.name = "Same name";
    user.markers.front().coordinate = 1000;
    user.grouping_views.front().id = "__catalog_grouping_view__";
    user.grouping_views.front().name = "My authored name";
    user.color_schemes = {{"red", "Red", {{"a", "#FF0000FF"}}}, {"green", "Green", {{"a", "#00FF00FF"}}}};
    const auto file = outside / L"用户谱线.data";
    std::string error;
    const auto save = [&](const std::filesystem::path& path, const SpectralLineList& list) {
        Require(SaveSpectralLineListToPathAtomic(path, list, error), error);
    };
    save(file, user);
    const auto original_bytes = Bytes(file);
    Applied(controller.OpenUserLineList(file, paths));
    auto view = controller.View();
    Require(view.user_owned && !controller.CanCustomize() && view.line_list_id == user.id &&
            view.grouping_view_search.empty() && view.marker_labels_visible, "new owner starts independent session");
    Require(view.grouping_views.front().generated_name == GeneratedNameMetadata{} &&
            view.grouping_views.front().name == "My authored name", "user names never receive built-in localization");
    Require(ProjectSpectralLineList(controller.PlotSource(), snapshot).visible_markers.size() == 3 &&
            ProjectSpectralLineList(controller.PlotSource(), snapshot).visible_markers.front().marker->coordinate == 1000,
            "sole active user markers reach rest-warning capable plot");
    Require(controller.Flush(), "pending built-in save works while user owner is active");
    const auto durable_builtin = Bytes(state_path);
    const auto slot = ProjectSpectralLineList(controller.PlotSource(), snapshot).visible_markers.front().automatic_color_slot;
    Applied(controller.Submit(Intent::SelectColorScheme("green")));
    Require(ProjectSpectralLineList(controller.PlotSource(), snapshot).visible_markers.front().color == DecodeLineListColor("#00FF00FF"),
            "canonical color selection is session-only");
    Require(controller.Submit(Intent::SetMarkerColor("a", PlotSeriesColor::Auto())).status == Status::Rejected &&
            controller.Submit(Intent::CreateUserGroupingView()).status == Status::Rejected &&
            controller.Submit(Intent::DuplicateGroupingView("__catalog_grouping_view__")).status == Status::Rejected &&
            controller.Submit(Intent::AddUserGroup("__catalog_grouping_view__")).status == Status::Rejected &&
            controller.Submit(Intent::DeleteUserGroupingView("__catalog_grouping_view__")).status == Status::Rejected,
            "all canonical editing entry points reject user owner");
    const auto hidden = controller.Submit(Intent::SetMarkerVisibility("a", false));
    Applied(hidden); Require(!hidden.persistent_state_changed, "user visibility never schedules canonical/state mirror save");
    Applied(controller.Submit(Intent::SelectGroupingView("base-two")));
    Applied(controller.Submit(Intent::SetGroupExpanded("base-two", "base-g2", true)));
    Applied(controller.Submit(Intent::SetGroupingViewSearch("C")));
    Applied(controller.Submit(Intent::SetMarkerLabelsVisible(false)));
    const auto generation = controller.View().generation;
    const auto unchanged = [&] {
        const auto current = controller.View();
        Require(current.generation == generation && current.line_list_id == user.id && controller.PlotSource().list == user &&
                current.grouping_view_search == "C" && !current.marker_labels_visible &&
                Active(current) == "base-two" && current.active_color_scheme_id == "green" &&
                ProjectSpectralLineList(controller.PlotSource(), snapshot).visible_markers.size() == 2,
                "failed candidate leaves generation and all interaction state untouched");
    };
    for (const auto* role : {"config", "state", "logs", "unsaved", "STATE"}) {
        const auto reserved = managed / role / "list.json";
        save(reserved, user);
        Require(controller.OpenUserLineList(reserved, paths).status == Status::Rejected, "reserved namespace rejected");
        unchanged();
    }
    for (const auto& bytes : {std::string{}, std::string("{"), std::string("{}"),
            std::string("{\"format\":\"other\",\"schema_version\":1}"),
            std::string("{\"format\":\"spectiary.spectral_line_list\",\"schema_version\":99}")}) {
        const auto bad = outside / "bad.json";
        { std::ofstream stream(bad, std::ios::binary); stream << bytes; }
        Require(controller.OpenUserLineList(bad, paths).status == Status::Rejected, "invalid candidate rejected"); unchanged();
    }
    Require(controller.OpenUserLineList(outside / "absent", paths).status == Status::Rejected, "missing rejected"); unchanged();
    const HANDLE locked = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(locked != INVALID_HANDLE_VALUE, "lock fixture");
    const auto blocked = controller.OpenUserLineList(file, paths);
    CloseHandle(locked);
    Require(blocked.status == Status::Rejected, "inaccessible file rejected"); unchanged();
    Require(controller.Flush() && Bytes(state_path) == durable_builtin && Bytes(file) == original_bytes,
            "user interactions do not persist canonical contents or touch built-in state");

    Applied(controller.SelectBuiltInLineList());
    Require(controller.View().grouping_view_search == "builtin search" && !controller.View().marker_labels_visible &&
            Active(controller.View()) == builtin_view, "built-in interaction restored");
    Applied(controller.SelectOpenedUserLineList());
    Require(Active(controller.View()) == "base-two" && View(controller.View(), "base-two").groups.front().expanded,
            "switch restores user identity-scoped session");
    const auto moved = managed / "legal-at-root.anything";
    std::filesystem::rename(file, moved);
    Require(ProjectSpectralLineList(controller.PlotSource(), snapshot).visible_markers.size() == 2, "external move does not invalidate generation");
    Require(controller.OpenUserLineList(file, paths).status == Status::Rejected, "stale explicit locator does not rebind");
    Applied(controller.OpenUserLineList(moved, paths));
    Require(controller.View().grouping_view_search == "C" && controller.View().active_color_scheme_id == "green" &&
            ProjectSpectralLineList(controller.PlotSource(), snapshot).visible_markers.size() == 2, "explicit moved-path reopen resumes stable identity");
    Applied(controller.Submit(Intent::SetMarkerVisibility("a", true)));
    Require(ProjectSpectralLineList(controller.PlotSource(), snapshot).visible_markers.front().automatic_color_slot == slot, "stable colors after switches and move");
    auto other = user; other.id = "other-identity";
    save(moved, other);
    Applied(controller.OpenUserLineList(moved, paths));
    Require(controller.View().line_list_id == other.id && controller.View().grouping_view_search.empty() &&
            controller.View().marker_labels_visible && ProjectSpectralLineList(controller.PlotSource(), snapshot).visible_markers.size() == 3,
            "explicit replacement at same path/name adopts new identity without old session");
    const auto child = managed / "my-documents" / "list.json";
    save(child, user); Applied(controller.OpenUserLineList(child, paths));
    Require(controller.View().grouping_view_search == "C", "non-reserved child legal and identity resumes");
    // Even a user-authored document declaring the built-in ID cannot acquire built-in ownership.
    other.id = Base().id; other.grouping_views.clear();
    save(moved, other); Applied(controller.OpenUserLineList(moved, paths));
    Require(!controller.CanCustomize() && controller.View().grouping_views.front().groups.back().marker_references.size() == 3,
            "ungrouped user list inspectable; built-in identity cannot grant editing");
    Applied(controller.Submit(Intent::SetGroupMarkerVisibility("", "__unassigned__", false)));
    Require(ProjectSpectralLineList(controller.PlotSource(), snapshot).visible_markers.empty(), "ungrouped visibility is session-only");
    for (const auto unit : {line_list::Unit::Angstrom, line_list::Unit::Nanometer, line_list::Unit::Micrometer}) {
        for (const auto medium : {line_list::Medium::Vacuum, line_list::Medium::Air}) {
            auto coordinates = user; coordinates.id = "coordinates"; coordinates.coordinate.unit = unit; coordinates.coordinate.medium = medium;
            coordinates.description = "Preserve complete semantic content";
            coordinates.creator = "Authored by the user";
            coordinates.created_at = "2026-09-21T00:00:00Z";
            coordinates.modified_at = "2026-09-21T01:00:00Z";
            save(moved, coordinates); Applied(controller.OpenUserLineList(moved, paths));
            // The handoff is not filtered by wavelength semantics or snapshot
            // capabilities: complete canonical contents survive every combination.
            const auto source = controller.PlotSource();
            Require(source.list == coordinates && source.visible_markers.size() == coordinates.markers.size(),
                    "complete active model handed downstream unchanged for every valid unit/medium");
            for (std::size_t i = 0; i < coordinates.markers.size(); ++i)
                Require(source.visible_markers[i].marker == &source.list.markers[i] &&
                        *source.visible_markers[i].marker == coordinates.markers[i],
                        "line positions, band endpoints, IDs, names and notes retain canonical semantics");
            const auto projected = ProjectSpectralLineList(source, snapshot);
            Require(source.list == coordinates, "downstream projection never mutates canonical contents");
            const bool compatible = unit == line_list::Unit::Angstrom && medium == line_list::Medium::Vacuum;
            Require(projected.status == (compatible ? SpectralLineProjectionStatus::Available : SpectralLineProjectionStatus::UnsupportedCoordinates) &&
                    controller.View().line_list_marker_count == 3 && projected.visible_markers.size() == (compatible ? 3U : 0U),
                    "temporary downstream guard diagnoses unsupported semantics without affecting activation");
            if (compatible) {
                Require(projected.visible_markers[0].marker->coordinate == coordinates.markers[0].coordinate &&
                        projected.visible_markers[1].marker->start == coordinates.markers[1].start &&
                        projected.visible_markers[1].marker->end == coordinates.markers[1].end,
                        "compatible projection preserves line and band coordinates");
            }
            Applied(controller.Submit(Intent::SetMarkerVisibility("a", false)));
            Applied(controller.Submit(Intent::SetMarkerLabelsVisible(false)));
            const auto hidden_source = controller.PlotSource();
            Require(hidden_source.list == coordinates && hidden_source.visible_markers.size() == 2 &&
                    !hidden_source.marker_labels_visible,
                    "session visibility does not remove hidden canonical markers from complete handoff");
            const auto hidden_projection = ProjectSpectralLineList(hidden_source, snapshot);
            Require(!hidden_projection.marker_labels_visible && hidden_projection.layout_scope_id == coordinates.id,
                    "downstream result retains session labels and active identity even when unavailable");
            Applied(controller.Submit(Intent::SetMarkerVisibility("a", true)));
            Applied(controller.Submit(Intent::SetMarkerLabelsVisible(true)));
            Applied(controller.SelectBuiltInLineList());
            const auto builtin_source = controller.PlotSource();
            Require(builtin_source.list.id == Base().id && builtin_source.list.markers == Base().markers &&
                    std::any_of(builtin_source.list.grouping_views.begin(), builtin_source.list.grouping_views.end(),
                        [&](const auto& view) { return view.id == builtin_view; }),
                    "switch hands off sole built-in effective model including overlay views");
            Applied(controller.SelectOpenedUserLineList());
            Require(controller.PlotSource().list == coordinates,
                    "switch back hands off sole user model without built-in overlay contamination");

        }
    }
    save(moved, user); Applied(controller.OpenUserLineList(moved, paths));
    const auto before_unavailable = controller.PlotSource().list;
    snapshot->capabilities.can_show_spectral_lines = false;
    Require(controller.PlotSource().list == before_unavailable &&
            ProjectSpectralLineList(controller.PlotSource(), snapshot).status == SpectralLineProjectionStatus::SpectrumUnavailable &&
            ProjectSpectralLineList(controller.PlotSource(), {}).status == SpectralLineProjectionStatus::SpectrumUnavailable,
            "snapshot unavailability is downstream state and does not change the complete handoff");
    Require(ProjectSpectralLineList(controller.PlotSource(), snapshot).visible_markers.empty() && ProjectSpectralLineList(controller.PlotSource(), {}).visible_markers.empty(), "snapshot capability remains authoritative");
    auto empty = user; empty.markers.clear(); empty.grouping_views.clear(); empty.color_schemes.clear();
    save(moved, empty); Applied(controller.OpenUserLineList(moved, paths));
    Require(controller.View().user_owned && controller.View().line_list_marker_count == 0 &&
            controller.PlotSource().list == empty, "valid zero-marker file activates and hands off complete empty model");
    std::filesystem::remove(moved);
    Require(controller.View().user_owned, "external deletion leaves adopted in-memory generation intact");
    Applied(controller.SelectBuiltInLineList());
    Require(controller.CanCustomize(), "built-in capabilities restored");
    Applied(controller.Submit(Intent::AddUserGroup(builtin_view)));
    Require(controller.Flush(), "built-in editing still persists after user switching");
}

void TestExpansionIdentity(const std::filesystem::path& path)
{
    auto base = Base();
    base.grouping_views = {{"a/b", "First", {{"c", "First group", {"a"}}}},
                           {"a", "Second", {{"b/c", "Second group", {"b"}}}}};
    SpectralLinesPanelController controller(base, path);
    const auto expanded = [&](const std::string& view, const std::string& group) {
        const auto state = controller.View();
        for (const auto& candidate : View(state, view).groups)
            if (candidate.id == group) return candidate.expanded;
        throw std::runtime_error("missing expansion group");
    };
    Applied(controller.Submit(Intent::SetGroupExpanded("a/b", "c", true)));
    Require(expanded("a/b", "c") && !expanded("a", "b/c"), "opaque IDs do not collide");
    Applied(controller.Submit(Intent::SetGroupExpanded("a", "b/c", true)));
    Require(expanded("a/b", "c") && expanded("a", "b/c"), "both pairs expand independently");
    Applied(controller.Submit(Intent::SetGroupExpanded("a/b", "c", false)));
    Require(!expanded("a/b", "c") && expanded("a", "b/c"), "collapse preserves the other pair");
    Applied(controller.Submit(Intent::SetGroupExpanded("a/b", "__unassigned__", true)));
    Require(expanded("a/b", "__unassigned__") && !expanded("a", "__unassigned__"),
            "derived Unassigned expansion belongs to its view");
    Applied(controller.Submit(Intent::SetGroupExpanded("a", "__unassigned__", true)));
    Applied(controller.Submit(Intent::SetGroupExpanded("a/b", "__unassigned__", false)));
    Require(!expanded("a/b", "__unassigned__") && expanded("a", "__unassigned__"),
            "derived Unassigned collapse preserves the other view");
    Require(controller.Flush(), "structured expansion persists");
}

void TestOperations(const std::filesystem::path& path)
{
    SpectralLinesPanelController controller(Base(), path);
    auto state = controller.View();
    Require(state.grouping_views.size() == 2 && state.user_grouping_view_count == 0, "multiple base views exposed");
    for (const auto& view : state.grouping_views) {
        Require(!view.editable, "base ownership is explicit for every view");
        Require(controller.Submit(Intent::DeleteUserGroupingView(view.id)).status == Status::Rejected, "base delete rejected");
        Require(controller.Submit(Intent::AddUserGroup(view.id)).status == Status::Rejected, "base add rejected");
    }
    Require(controller.Submit(Intent::SetMarkerVisibility("missing", false)).status == Status::Rejected, "foreign marker rejected");
    Require(controller.Submit(Intent::SelectGroupingView("missing")).status == Status::Rejected, "foreign view rejected");
    auto snapshot = std::make_shared<SpectrumSnapshot>(); snapshot->capabilities.can_show_spectral_lines = true;
    Require(ProjectSpectralLineList(controller.PlotSource(), snapshot).visible_markers.size() == 3, "default marker visibility remains visible");
    Applied(controller.Submit(Intent::SetMarkerVisibility("a", false)));
    Require(ProjectSpectralLineList(controller.PlotSource(), snapshot).visible_markers.size() == 2, "visibility controls plot independently of grouping");
    const auto user = CreateView(controller);
    auto first = AddGroup(controller, user), second = AddGroup(controller, user);
    Applied(controller.Submit(Intent::MoveMarkerReference(user, "a", "__unassigned__", first)));
    Applied(controller.Submit(Intent::CopyMarkerReference(user, "a", first, second)));
    state = controller.View();
    Require(View(state, user).groups[0].marker_references[0].shared, "shared references detected");
    Applied(controller.Submit(Intent::MoveMarkerReference(user, "b", "__unassigned__", first)));
    Require(controller.Submit(Intent::MoveMarkerReference(user, "a", first, first)).status == Status::NoChange, "same-source move no-op");
    Applied(controller.Submit(Intent::SetGroupingViewSearch("Hα")));
    Require(controller.Submit(Intent::SetGroupMarkerVisibility(user, first, true)).status == Status::Rejected, "search cannot bulk toggle");
    Applied(controller.Submit(Intent::CopyMarkerReference(user, "b", first, second)));
    Applied(controller.Submit(Intent::SetGroupingViewSearch("")));
    Applied(controller.Submit(Intent::SetGroupMarkerVisibility(user, first, true)));
    Require(ProjectSpectralLineList(controller.PlotSource(), snapshot).visible_markers.size() == 3, "group visibility changes shared marker state");
    Applied(controller.Submit(Intent::ReorderUserGroupBefore(user, second, first)));
    Applied(controller.Submit(Intent::RenameUserGroup(user, first, "Unassigned", SpectralLineRenameEditState::Edited)));
    Applied(controller.Submit(Intent::SetGroupExpanded(user, first, true)));
    state = controller.View();
    Require(View(state, user).groups[0].id == second && View(state, user).groups[1].name == "Unassigned" &&
            !View(state, user).groups[1].is_unassigned, "authored Unassigned has ordinary semantics");
    Applied(controller.Submit(Intent::DuplicateGroupingView(user)));
    const auto copy = Active(controller.View());
    Require(copy != user, "duplicate has fresh identity");
    state = controller.View();
    Require(View(state, copy).groups[0].id != second, "duplicate group identities fresh");
    Applied(controller.Submit(Intent::RenameUserGroupingView(copy, "Grouping 1", SpectralLineRenameEditState::Edited)));
    Require(View(controller.View(), copy).generated_name == GeneratedNameMetadata{}, "explicit rename clears generated provenance");
    Applied(controller.Submit(Intent::DeleteUserGroupingView(copy)));
    Applied(controller.Submit(Intent::SelectGroupingView(user)));
    Applied(controller.Submit(Intent::RemoveMarkerReference(user, "a", first)));
    Require(View(controller.View(), user).groups[0].marker_references[1].marker_id == "a", "removing one shared reference preserves others");
    Applied(controller.Submit(Intent::SetMarkerColor("a", PlotSeriesColor::ExplicitColor({1, 0, 0, 1}))));
    Applied(controller.Submit(Intent::SetMarkerColor("b", PlotSeriesColor::ExplicitColor({0, 1, 0, 1}))));
    const auto slot = ProjectSpectralLineList(controller.PlotSource(), snapshot).visible_markers[0].automatic_color_slot;
    Applied(controller.Submit(Intent::SelectGroupingView("base-two")));
    Require(ProjectSpectralLineList(controller.PlotSource(), snapshot).visible_markers[0].automatic_color_slot == slot, "palette slots remain stable across view selection");
    Require(controller.Flush(), "controller saves state");
    auto loaded = LoadBuiltInSpectralLineState(path, Base());
    Require(loaded.error.empty() && loaded.state.overlay.grouping_views.size() == 1, loaded.error);
    Require(loaded.state.overlay.grouping_views[0].groups.size() == 2, "Unassigned projection not persisted");
    Require(loaded.state.overlay.grouping_views[0].groups[0].marker_ids == std::vector<std::string>{"a", "b"}, "stored order survives display and save");
    Require(View(controller.View(), user).groups[0].marker_references[0].marker_id == "b",
            "UI may project wavelength order independently of the preserved marker_ids array");
    Require(Bytes(path).find("is_unassigned") == std::string::npos && Bytes(path).find("marker_references") == std::string::npos, "new writer emits only canonical subrecords");
    SpectralLinesPanelController reopened(Base(), path);
    Require(Active(reopened.View()) == "base-two", "selection restored independently");
    Require(ProjectSpectralLineList(reopened.PlotSource(), snapshot).visible_markers[0].color == PlotSeriesColor::ExplicitColor({1, 0, 0, 1}), "explicit color persists");
    Applied(reopened.Submit(Intent::SetMarkerColor("a", PlotSeriesColor::Auto())));
    Require(reopened.Flush(), "Auto reset saves");
    loaded = LoadBuiltInSpectralLineState(path, Base());
    Require(loaded.state.overlay.color_schemes && !loaded.state.overlay.color_schemes->front().colors.contains("a") &&
            loaded.state.overlay.color_schemes->front().colors.contains("b"), "Auto reset removes only selected marker mapping");
}
void TestPersistence(const std::filesystem::path& root)
{
    const auto path = root / "persistence.json";
    SpectralLinesPanelController stale(Base(), path), peer(Base(), path);
    const auto a = CreateView(stale), b = CreateView(peer);
    Require(peer.Flush() && stale.Flush(), "stale independent views reconcile");
    auto loaded = LoadBuiltInSpectralLineState(path, Base());
    Require(loaded.state.overlay.grouping_views.size() == 2, "both additions preserved");
    SpectralLinesPanelController left(Base(), path), right(Base(), path);
    Applied(left.Submit(Intent::RenameUserGroupingView(a, "Pending", SpectralLineRenameEditState::Edited)));
    Applied(right.Submit(Intent::DeleteUserGroupingView(a)));
    Require(right.Flush() && !left.Flush(), "deleted edited view is controlled conflict");
    Require(View(left.View(), a).name == "Pending", "conflict retains in-memory edit");
    Require(LoadBuiltInSpectralLineState(path, Base()).state.overlay.grouping_views.size() == 1, "conflict does not recreate entity");

    const auto corrupt = root / "corrupt.json";
    { std::ofstream(corrupt) << "{broken"; }
    {
        SpectralLinesPanelController warning(Base(), corrupt);
        Require(warning.View().grouping_views.size() == 2 && warning.View().persistence.load_issue != SpectralLineCacheLoadIssueKind::None,
                "invalid overlay shows base with diagnostic");
        Applied(warning.Submit(Intent::SetMarkerVisibility("a", false)));
        Require(!warning.Flush() && Bytes(corrupt) == "{broken", "rejected startup state cannot be overwritten");
        std::string error; Require(SaveBuiltInSpectralLineState(corrupt, Base(), {}, error), error);
        Require(warning.Flush(), "explicit external repair permits pending write recovery");
        Require(warning.View().persistence.recovered, "recovery is presented");
    }
    const auto corrupt_latest = root / "latest.json";
    {
        SpectralLinesPanelController controller(Base(), corrupt_latest);
        Applied(controller.Submit(Intent::SetMarkerVisibility("b", false)));
        { std::ofstream(corrupt_latest) << "{\"x\":1,\"x\":2}"; }
        const auto before = Bytes(corrupt_latest);
        Require(!controller.Flush() && Bytes(corrupt_latest) == before, "latest duplicate keys fail closed");
    }
    const auto shutdown = root / "shutdown.json";
    { SpectralLinesPanelController controller(Base(), shutdown); Applied(controller.Submit(Intent::SetMarkerVisibility("c", false))); }
    Require(!LoadBuiltInSpectralLineState(shutdown, Base()).state.session.marker_visibility.at("c"), "destructor flushes pending state");
    const auto lock_path = root / "leased.json";
    auto lease_file = lock_path; lease_file += ".commit.lock";
    auto lease = TryAcquireExclusiveFileLease(lease_file);
    Require(lease.status == ExclusiveFileLeaseAcquireStatus::Acquired, "test acquires commit lease");
    SpectralLinesPanelController locked(Base(), lock_path);
    Applied(locked.Submit(Intent::SetMarkerVisibility("a", false)));
    Require(!locked.Flush() && !std::filesystem::exists(lock_path), "busy lease cannot publish");
    lease.lease = ExclusiveFileLease{};
    Require(locked.Flush(), "released lease allows retry");
    (void)b;
}
void WaitFile(const std::filesystem::path& path)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!std::filesystem::exists(path)) {
        Require(std::chrono::steady_clock::now() < deadline, "child barrier timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
int Child(const std::filesystem::path& root, const std::string& id)
{
    SpectralLinesPanelController controller(Base(), root / "shared.json");
    Applied(controller.Submit(Intent::SetMarkerColor(id, PlotSeriesColor::ExplicitColor(id == "a" ? RgbaColor{1,0,0,1} : RgbaColor{0,1,0,1}))));
    { std::ofstream(root / (id + ".ready")) << "ready"; }
    WaitFile(root / "go");
    Require(controller.Flush(), "child concurrent save failed");
    return 0;
}
void TestProcesses(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root);
    wchar_t executable[32768]{};
    Require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "test executable path");
    struct Process {
        PROCESS_INFORMATION info{};
        ~Process() { if (info.hProcess) { if (WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(info.hProcess, 10); CloseHandle(info.hProcess); } if (info.hThread) CloseHandle(info.hThread); }
    } a, b;
    const auto start = [&](Process& process, const wchar_t* id) {
        auto command = L"\"" + std::wstring(executable) + L"\" --child \"" + root.wstring() + L"\" " + id;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        Require(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process.info) != FALSE, "start child process");
    };
    start(a, L"a"); start(b, L"b");
    WaitFile(root / "a.ready"); WaitFile(root / "b.ready");
    { std::ofstream(root / "go") << "go"; }
    for (auto* process : {&a, &b}) {
        Require(WaitForSingleObject(process->info.hProcess, 15000) == WAIT_OBJECT_0, "child completes bounded save");
        DWORD code = 1; GetExitCodeProcess(process->info.hProcess, &code); Require(code == 0, "child succeeds");
    }
    const auto state = LoadBuiltInSpectralLineState(root / "shared.json", Base());
    Require(state.error.empty() && state.state.overlay.color_schemes && state.state.overlay.color_schemes->front().colors.size() == 2,
            "OS commit lease preserves concurrent first overrides across processes");
}
}
int wmain(int argc, wchar_t* argv[])
{
    try {
        if (argc == 4 && std::wstring_view(argv[1]) == L"--child") return Child(argv[2], std::wstring_view(argv[3]) == L"a" ? "a" : "b");
        spectiary::test_support::TemporaryDirectory directory;
        TestUserLineLists(directory.path() / "user-files");
        TestExpansionIdentity(directory.path() / "expansion.json");
        TestOperations(directory.path() / "operations.json"); TestPersistence(directory.path()); TestProcesses(directory.path() / "processes");
        std::cout << "Spectral-line controller tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
