#include "overlays/spectral_line_user_state.h"
#include "overlays/spectral_line_user_state_cache_io.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

specforge::SpectralLineMarker Line(
    std::string id,
    std::string label,
    std::string group,
    double vacuum_angstrom)
{
    specforge::SpectralLineMarker marker;
    marker.id = std::move(id);
    marker.label = std::move(label);
    marker.kind = specforge::SpectralLineMarkerKind::Line;
    marker.group = std::move(group);
    marker.vacuum_angstrom = vacuum_angstrom;
    marker.display_label = marker.label;
    marker.source_ref = "test";
    return marker;
}

specforge::SpectralLineCatalog GroupedCatalog()
{
    specforge::SpectralLineCatalog catalog;
    catalog.markers.push_back(Line("h_beta", "H beta", "Balmer", 4862.683));
    catalog.markers.push_back(Line("h_alpha", "H alpha", "Balmer", 6564.614));
    catalog.markers.push_back(Line("ca_ii_8500", "Ca II", "Ca II", 8500.360));
    return catalog;
}

specforge::SpectralLineCatalog UngroupedCatalog()
{
    specforge::SpectralLineCatalog catalog;
    catalog.markers.push_back(Line("marker_a", "Marker A", "", 4100.0));
    catalog.markers.push_back(Line("marker_b", "Marker B", "", 4200.0));
    return catalog;
}

specforge::MarkerReference Reference(std::string marker_id)
{
    specforge::MarkerReference reference;
    reference.catalog_identity = specforge::PublicSpectralLineCatalogIdentity();
    reference.marker_id = std::move(marker_id);
    return reference;
}

specforge::MarkerReference ReferenceFor(specforge::CatalogIdentity identity, std::string marker_id)
{
    specforge::MarkerReference reference;
    reference.catalog_identity = std::move(identity);
    reference.marker_id = std::move(marker_id);
    return reference;
}

specforge::UserGroup* FindGroupById(specforge::GroupingView& view, std::string_view group_id)
{
    const auto match = std::find_if(view.groups.begin(), view.groups.end(), [group_id](const auto& group) {
        return group.id == group_id;
    });
    return match == view.groups.end() ? nullptr : &(*match);
}

const specforge::UserGroup* FindGroupById(const specforge::GroupingView& view, std::string_view group_id)
{
    const auto match = std::find_if(view.groups.begin(), view.groups.end(), [group_id](const auto& group) {
        return group.id == group_id;
    });
    return match == view.groups.end() ? nullptr : &(*match);
}

bool GroupContainsReference(const specforge::UserGroup& group, std::string_view marker_id)
{
    return std::any_of(group.marker_references.begin(), group.marker_references.end(), [marker_id](const auto& reference) {
        return reference.marker_id == marker_id;
    });
}

void TestPublicCatalogIdentityIsStable()
{
    const specforge::CatalogIdentity& identity = specforge::PublicSpectralLineCatalogIdentity();
    Require(identity.id == "specforge.public", "public catalog identity should be fixed");
}

void TestDefaultMarkerVisibilityIsVisible()
{
    const specforge::CatalogUserState state =
        specforge::MakeCatalogUserState(specforge::PublicSpectralLineCatalogIdentity());

    Require(specforge::IsMarkerVisible(state, "h_alpha"), "missing marker visibility should default to visible");
}

void TestSearchStateDoesNotBulkToggleMarkers()
{
    const specforge::SpectralLineCatalog catalog = GroupedCatalog();
    specforge::CatalogUserState state =
        specforge::MakeCatalogUserState(specforge::PublicSpectralLineCatalogIdentity());
    const specforge::GroupingView view = specforge::CreateUserGroupingViewFromCatalog(
        catalog,
        specforge::PublicSpectralLineCatalogIdentity(),
        "view-1",
        "Working view");
    const specforge::UserGroup& unassigned = view.groups.front();

    const bool changed = specforge::SetGroupMarkerVisibility(
        state,
        unassigned,
        catalog,
        specforge::PublicSpectralLineCatalogIdentity(),
        false,
        true);

    Require(!changed, "bulk visibility should be non-actionable while search is active");
    Require(specforge::IsMarkerVisible(state, "h_alpha"), "search-state bulk toggle should not hide h_alpha");
    Require(
        specforge::VisibilityStateForGroup(
            state,
            unassigned,
            catalog,
            specforge::PublicSpectralLineCatalogIdentity(),
            true) ==
            specforge::GroupVisibilityState::SearchFiltered,
        "search state should expose the fourth group visibility state");
}

void TestUnresolvedReferenceIsPreserved()
{
    const specforge::SpectralLineCatalog catalog = GroupedCatalog();
    specforge::GroupingView view;
    view.id = "view-1";
    view.name = "Working view";

    specforge::UserGroup group;
    group.id = "group-1";
    group.name = "Interesting";
    group.marker_references.push_back(Reference("missing_marker"));
    view.groups.push_back(std::move(group));

    const specforge::GroupingView effective =
        specforge::EffectiveUserGroupingView(view, catalog, specforge::PublicSpectralLineCatalogIdentity());
    const specforge::MarkerReference& unresolved = effective.groups.front().marker_references.front();

    Require(
        specforge::FindCatalogMarker(catalog, specforge::PublicSpectralLineCatalogIdentity(), unresolved) == nullptr,
        "unresolved marker reference should stay unresolved");
    Require(
        specforge::VisibilityStateForGroup(
            specforge::MakeCatalogUserState(specforge::PublicSpectralLineCatalogIdentity()),
            effective.groups.front(),
            catalog,
            specforge::PublicSpectralLineCatalogIdentity(),
            false) == specforge::GroupVisibilityState::Empty,
        "unresolved-only groups should not produce actionable bulk visibility");
}

void TestReferenceIdentityMustMatchCurrentCatalog()
{
    const specforge::SpectralLineCatalog catalog = GroupedCatalog();
    const specforge::CatalogIdentity imported_identity{"imported.catalog", "Imported"};
    const specforge::MarkerReference wrong_identity_reference = ReferenceFor(imported_identity, "h_alpha");

    Require(
        specforge::FindCatalogMarker(catalog, specforge::PublicSpectralLineCatalogIdentity(), wrong_identity_reference) ==
            nullptr,
        "same marker id from another catalog identity should not resolve against the active catalog");
}

void TestSharedReferencesAreDetected()
{
    specforge::GroupingView view;
    view.id = "view-1";
    view.name = "Working view";

    specforge::UserGroup first;
    first.id = "group-1";
    first.name = "First";
    first.marker_references.push_back(Reference("h_alpha"));
    view.groups.push_back(std::move(first));

    specforge::UserGroup second;
    second.id = "group-2";
    second.name = "Second";
    second.marker_references.push_back(Reference("h_alpha"));
    view.groups.push_back(std::move(second));

    const std::unordered_map<std::string, int> counts =
        specforge::MarkerReferenceCounts(view, specforge::PublicSpectralLineCatalogIdentity());

    Require(counts.at("h_alpha") == 2, "shared marker reference should count both occurrences");
    Require(
        specforge::IsSharedMarkerReference(counts, view.groups.front().marker_references.front()),
        "shared marker reference should be marked shared");
}

void TestMoveAndCopyMarkerReferences()
{
    const specforge::SpectralLineCatalog catalog = GroupedCatalog();
    specforge::GroupingView view = specforge::CreateUserGroupingViewFromCatalog(
        catalog,
        specforge::PublicSpectralLineCatalogIdentity(),
        "view-1",
        "Working view");
    Require(specforge::AddUserGroup(view, "group-1", "Interesting"), "first user group should be added");
    Require(specforge::AddUserGroup(view, "group-2", "Compare"), "second user group should be added");

    Require(
        specforge::MoveMarkerReference(
            view,
            specforge::PublicSpectralLineCatalogIdentity(),
            "h_alpha",
            specforge::UnassignedUserGroupId(),
            "group-1"),
        "move should transfer a marker reference into the target group");
    const specforge::GroupingView after_move =
        specforge::EffectiveUserGroupingView(view, catalog, specforge::PublicSpectralLineCatalogIdentity());
    const specforge::UserGroup& unassigned = after_move.groups.back();
    Require(unassigned.is_unassigned, "Unassigned should remain the last group after ordinary groups");
    for (const specforge::MarkerReference& reference : unassigned.marker_references) {
        Require(reference.marker_id != "h_alpha", "moved marker should not remain in Unassigned");
    }

    Require(
        specforge::CopyMarkerReference(view, specforge::PublicSpectralLineCatalogIdentity(), "h_alpha", "group-2"),
        "copy should add a shared marker reference to the target group");
    Require(
        !specforge::CopyMarkerReference(
            view,
            specforge::PublicSpectralLineCatalogIdentity(),
            "h_alpha",
            specforge::UnassignedUserGroupId()),
        "copy to Unassigned should be rejected because Unassigned is derived from ordinary groups");
    const std::unordered_map<std::string, int> counts =
        specforge::MarkerReferenceCounts(view, specforge::PublicSpectralLineCatalogIdentity());
    Require(counts.at("h_alpha") == 2, "copied marker should be shared across two user groups");
}

void TestRemoveUserGroupAndMarkerReferences()
{
    const specforge::SpectralLineCatalog catalog = GroupedCatalog();
    specforge::GroupingView view = specforge::CreateUserGroupingViewFromCatalog(
        catalog,
        specforge::PublicSpectralLineCatalogIdentity(),
        "view-1",
        "Working view");
    Require(specforge::AddUserGroup(view, "group-1", "Interesting"), "user group should be added");
    specforge::UserGroup* editable_group = FindGroupById(view, "group-1");
    Require(editable_group != nullptr, "new user group should be addressable by id");
    editable_group->marker_references.push_back(Reference("missing_marker"));

    Require(
        specforge::MoveMarkerReference(
            view,
            specforge::PublicSpectralLineCatalogIdentity(),
            "h_alpha",
            specforge::UnassignedUserGroupId(),
            "group-1"),
        "marker reference should move into the user group");
    Require(
        specforge::RemoveMarkerReferenceFromGroup(
            view,
            specforge::PublicSpectralLineCatalogIdentity(),
            "h_alpha",
            "group-1"),
        "marker reference should be removable from its user group");
    Require(
        !specforge::RemoveMarkerReferenceFromGroup(
            view,
            specforge::PublicSpectralLineCatalogIdentity(),
            "h_alpha",
            specforge::UnassignedUserGroupId()),
        "Unassigned marker references should not be directly removable");
    Require(
        specforge::RemoveMarkerReferenceFromGroup(
            view,
            specforge::PublicSpectralLineCatalogIdentity(),
            "missing_marker",
            "group-1"),
        "unresolved marker reference should be removable from its user group");
    const specforge::UserGroup* source_unassigned_after_unresolved_remove =
        FindGroupById(view, specforge::UnassignedUserGroupId());
    Require(
        source_unassigned_after_unresolved_remove != nullptr &&
            GroupContainsReference(*source_unassigned_after_unresolved_remove, "missing_marker"),
        "removed unresolved marker reference should be preserved in source Unassigned");

    const specforge::GroupingView after_reference_remove =
        specforge::EffectiveUserGroupingView(view, catalog, specforge::PublicSpectralLineCatalogIdentity());
    const specforge::UserGroup& unassigned_after_reference_remove = after_reference_remove.groups.back();
    Require(unassigned_after_reference_remove.is_unassigned, "Unassigned should remain the last group");
    Require(
        std::any_of(
            unassigned_after_reference_remove.marker_references.begin(),
            unassigned_after_reference_remove.marker_references.end(),
            [](const specforge::MarkerReference& reference) {
                return reference.marker_id == "h_alpha";
            }),
        "removed marker reference should return to Unassigned in the effective view");
    Require(
        GroupContainsReference(unassigned_after_reference_remove, "missing_marker"),
        "removed unresolved marker reference should remain in effective Unassigned");

    Require(
        specforge::MoveMarkerReference(
            view,
            specforge::PublicSpectralLineCatalogIdentity(),
            "h_beta",
            specforge::UnassignedUserGroupId(),
            "group-1"),
        "second marker reference should move into the user group");
    editable_group = FindGroupById(view, "group-1");
    Require(editable_group != nullptr, "user group should still be addressable before deletion");
    editable_group->marker_references.push_back(Reference("missing_from_deleted_group"));
    Require(specforge::RemoveUserGroup(view, "group-1"), "ordinary user group should be removable");
    Require(
        !specforge::RemoveUserGroup(view, specforge::UnassignedUserGroupId()),
        "Unassigned user group should not be removable");
    const specforge::UserGroup* source_unassigned_after_group_remove =
        FindGroupById(view, specforge::UnassignedUserGroupId());
    Require(
        source_unassigned_after_group_remove != nullptr &&
            GroupContainsReference(*source_unassigned_after_group_remove, "missing_from_deleted_group"),
        "unresolved marker references from a removed group should be preserved in source Unassigned");

    const specforge::GroupingView after_group_remove =
        specforge::EffectiveUserGroupingView(view, catalog, specforge::PublicSpectralLineCatalogIdentity());
    Require(
        std::none_of(after_group_remove.groups.begin(), after_group_remove.groups.end(), [](const auto& group) {
            return group.id == "group-1";
        }),
        "removed user group should not remain in the effective view");
    const specforge::UserGroup& unassigned_after_group_remove = after_group_remove.groups.back();
    Require(
        std::any_of(
            unassigned_after_group_remove.marker_references.begin(),
            unassigned_after_group_remove.marker_references.end(),
            [](const specforge::MarkerReference& reference) {
                return reference.marker_id == "h_beta";
            }),
        "marker references from a removed user group should return to Unassigned");
    Require(
        GroupContainsReference(unassigned_after_group_remove, "missing_from_deleted_group"),
        "unresolved marker references from a removed group should remain in effective Unassigned");
}

void TestReorderUserGroups()
{
    const specforge::SpectralLineCatalog catalog = GroupedCatalog();
    specforge::GroupingView view = specforge::CreateUserGroupingViewFromCatalog(
        catalog,
        specforge::PublicSpectralLineCatalogIdentity(),
        "view-1",
        "Working view");
    Require(specforge::AddUserGroup(view, "group-1", "First"), "first user group should be added");
    Require(specforge::AddUserGroup(view, "group-2", "Second"), "second user group should be added");
    Require(specforge::AddUserGroup(view, "group-3", "Third"), "third user group should be added");

    Require(
        !specforge::ReorderUserGroupBefore(view, "group-1", "group-2"),
        "moving a group before its immediate next group should be a no-op");
    Require(view.groups[0].id == "group-1", "group-1 should remain first after adjacent no-op");
    Require(view.groups[1].id == "group-2", "group-2 should remain second after adjacent no-op");

    Require(
        specforge::ReorderUserGroupBefore(view, "group-3", "group-1"),
        "group-3 should move before group-1");
    Require(view.groups[0].id == "group-3", "group-3 should become first");
    Require(view.groups[1].id == "group-1", "group-1 should shift after group-3");

    Require(
        specforge::ReorderUserGroupBefore(view, "group-3", specforge::UnassignedUserGroupId()),
        "dropping on the Unassigned insertion line should move the group to the last ordinary position");
    Require(view.groups[0].id == "group-1", "group-1 should become first after group-3 moves down");
    Require(view.groups[1].id == "group-2", "group-2 should become second after group-3 moves down");
    Require(view.groups[2].id == "group-3", "group-3 should move before Unassigned");
    Require(view.groups.back().is_unassigned, "Unassigned should remain last after reordering");

    Require(
        !specforge::ReorderUserGroupBefore(view, "missing", "group-1"),
        "missing source group should be rejected");
    Require(
        !specforge::ReorderUserGroupBefore(view, "group-1", "missing"),
        "missing target group should be rejected");
    Require(view.groups[0].id == "group-1", "missing groups should not change ordering");
    Require(view.groups[2].id == "group-3", "missing groups should not move the last ordinary group");

    Require(
        !specforge::ReorderUserGroupBefore(view, specforge::UnassignedUserGroupId(), "group-1"),
        "Unassigned should not be reorderable");
    Require(view.groups.back().is_unassigned, "Unassigned should remain last when its move is rejected");
}

void TestMarkerVisibilityIsSharedAcrossViews()
{
    const specforge::SpectralLineCatalog catalog = GroupedCatalog();
    specforge::CatalogUserState state =
        specforge::MakeCatalogUserState(specforge::PublicSpectralLineCatalogIdentity());
    specforge::SetMarkerVisible(state, "h_alpha", false);

    specforge::GroupingView first;
    first.id = "view-1";
    first.name = "First";
    specforge::UserGroup first_group;
    first_group.id = "group-1";
    first_group.name = "First group";
    first_group.marker_references.push_back(Reference("h_alpha"));
    first.groups.push_back(std::move(first_group));

    specforge::GroupingView second;
    second.id = "view-2";
    second.name = "Second";
    specforge::UserGroup second_group;
    second_group.id = "group-2";
    second_group.name = "Second group";
    second_group.marker_references.push_back(Reference("h_alpha"));
    second.groups.push_back(std::move(second_group));

    Require(
        specforge::VisibilityStateForGroup(
            state,
            first.groups.front(),
            catalog,
            specforge::PublicSpectralLineCatalogIdentity(),
            false) == specforge::GroupVisibilityState::AllHidden,
        "hidden marker visibility should apply in the first grouping view");
    Require(
        specforge::VisibilityStateForGroup(
            state,
            second.groups.front(),
            catalog,
            specforge::PublicSpectralLineCatalogIdentity(),
            false) == specforge::GroupVisibilityState::AllHidden,
        "hidden marker visibility should apply in another grouping view for the same catalog identity");
}

void TestUngroupedCatalogHasNoCatalogGroupingView()
{
    const specforge::SpectralLineCatalog catalog = UngroupedCatalog();
    const std::optional<specforge::GroupingView> catalog_view =
        specforge::BuildCatalogGroupingView(catalog, specforge::PublicSpectralLineCatalogIdentity());
    const specforge::GroupingView user_view = specforge::CreateUserGroupingViewFromCatalog(
        catalog,
        specforge::PublicSpectralLineCatalogIdentity(),
        "view-1",
        "Working view");

    Require(!specforge::CatalogHasGrouping(catalog), "ungrouped catalog should report no catalog grouping");
    Require(!catalog_view, "ungrouped catalog should not create a catalog grouping view");
    Require(user_view.groups.size() == 1, "new user view should contain the Unassigned group");
    Require(user_view.groups.front().is_unassigned, "new user view group should be Unassigned");
    Require(user_view.groups.front().marker_references.size() == 2, "Unassigned should contain current catalog markers");
}

void TestCatalogGroupingViewUsesUniqueGroupIds()
{
    specforge::SpectralLineCatalog catalog;
    catalog.markers.push_back(Line("marker_a", "Marker A", "C H", 4100.0));
    catalog.markers.push_back(Line("marker_b", "Marker B", "C-H", 4200.0));
    catalog.markers.push_back(Line("marker_c", "Marker C", "C_H", 4300.0));

    const std::optional<specforge::GroupingView> catalog_view =
        specforge::BuildCatalogGroupingView(catalog, specforge::PublicSpectralLineCatalogIdentity());

    Require(catalog_view.has_value(), "grouped catalog should create a catalog grouping view");
    Require(catalog_view->groups.size() == 3, "colliding sanitized groups should remain separate");
    std::unordered_set<std::string> group_ids;
    for (const specforge::UserGroup& group : catalog_view->groups) {
        Require(group_ids.insert(group.id).second, "catalog grouping ids should be unique after sanitization");
    }
    Require(
        group_ids.find("catalog_c_h") != group_ids.end(),
        "first sanitized catalog group should keep the base id");
    Require(
        group_ids.find("catalog_c_h_2") != group_ids.end(),
        "second sanitized catalog group should receive a deterministic suffix");
    Require(
        group_ids.find("catalog_c_h_3") != group_ids.end(),
        "third sanitized catalog group should receive a deterministic suffix");
}

void TestCacheRoundTrip()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_spectral_line_user_state_cache_test.json";
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);

    specforge::CatalogUserState state =
        specforge::MakeCatalogUserState(specforge::PublicSpectralLineCatalogIdentity());
    state.active_view_id = "view-1";
    state.marker_visibility["h_alpha"] = false;

    specforge::GroupingView view;
    view.id = "view-1";
    view.name = "Working view";
    specforge::UserGroup group;
    group.id = "group-1";
    group.name = "Interesting";
    group.marker_references.push_back(Reference("h_alpha"));
    view.groups.push_back(std::move(group));
    state.grouping_views.push_back(std::move(view));

    specforge::CatalogUserStateCache cache;
    cache.catalogs.emplace(state.catalog_identity.id, state);
    cache.catalog_panel_state["specforge.public"].expanded_group_ids.insert(
        specforge::GroupExpansionKey("view-1", "group-1"));

    std::string error;
    Require(specforge::SaveCatalogUserStateCache(path, cache, error), error);
    const specforge::CatalogUserStateCacheLoadResult loaded = specforge::LoadCatalogUserStateCache(path);
    std::filesystem::remove(path, remove_error);

    Require(loaded.warning.empty(), loaded.warning);
    const auto catalog = loaded.cache.catalogs.find("specforge.public");
    Require(catalog != loaded.cache.catalogs.end(), "cache should preserve public catalog state");
    Require(catalog->second.active_view_id == "view-1", "cache should preserve active view");
    Require(!specforge::IsMarkerVisible(catalog->second, "h_alpha"), "cache should preserve marker visibility");
    Require(catalog->second.grouping_views.size() == 1, "cache should preserve user grouping views");
    const auto panel = loaded.cache.catalog_panel_state.find("specforge.public");
    Require(panel != loaded.cache.catalog_panel_state.end(), "cache should preserve public panel state separately");
    Require(
        panel->second.expanded_group_ids.find(specforge::GroupExpansionKey("view-1", "group-1")) !=
            panel->second.expanded_group_ids.end(),
        "cache should preserve expanded group ids");
}

void TestCacheSaveReplacesExistingFileWithoutLeavingTempFile()
{
    specforge::CatalogUserState state =
        specforge::MakeCatalogUserState(specforge::PublicSpectralLineCatalogIdentity());
    state.active_view_id = "view-1";
    state.marker_visibility["h_alpha"] = false;

    specforge::CatalogUserStateCache cache;
    cache.catalogs.emplace(state.catalog_identity.id, state);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_spectral_line_user_state_atomic_save_test.json";
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
    {
        std::ofstream stream(path);
        stream << "{ invalid old cache";
    }

    std::string error;
    Require(specforge::SaveCatalogUserStateCache(path, cache, error), error);
    const specforge::CatalogUserStateCacheLoadResult loaded = specforge::LoadCatalogUserStateCache(path);

    const std::filesystem::path parent = path.parent_path();
    const std::string temporary_prefix = path.filename().string() + ".tmp.";
    bool found_temporary_file = false;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(parent)) {
        if (entry.path().filename().string().starts_with(temporary_prefix)) {
            found_temporary_file = true;
            break;
        }
    }

    std::filesystem::remove(path, remove_error);

    Require(loaded.warning.empty(), loaded.warning);
    Require(loaded.cache.catalogs.find("specforge.public") != loaded.cache.catalogs.end(), "saved cache should load");
    Require(!found_temporary_file, "atomic cache save should not leave temporary files behind");
}

void TestCacheSeparatesCatalogIdentities()
{
    const specforge::CatalogIdentity imported_identity{"imported.catalog", "Imported"};
    specforge::CatalogUserState public_state =
        specforge::MakeCatalogUserState(specforge::PublicSpectralLineCatalogIdentity());
    public_state.active_view_id = "public-view";
    public_state.marker_visibility["h_alpha"] = false;

    specforge::CatalogUserState imported_state = specforge::MakeCatalogUserState(imported_identity);
    imported_state.active_view_id = "imported-view";
    imported_state.marker_visibility["h_alpha"] = true;

    specforge::CatalogUserStateCache cache;
    cache.catalogs.emplace(public_state.catalog_identity.id, public_state);
    cache.catalogs.emplace(imported_state.catalog_identity.id, imported_state);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_spectral_line_user_state_identity_cache_test.json";
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
    std::string error;
    Require(specforge::SaveCatalogUserStateCache(path, cache, error), error);
    const specforge::CatalogUserStateCacheLoadResult loaded = specforge::LoadCatalogUserStateCache(path);
    std::filesystem::remove(path, remove_error);

    Require(loaded.warning.empty(), loaded.warning);
    const auto public_match = loaded.cache.catalogs.find("specforge.public");
    const auto imported_match = loaded.cache.catalogs.find("imported.catalog");
    Require(public_match != loaded.cache.catalogs.end(), "public catalog state should round-trip");
    Require(imported_match != loaded.cache.catalogs.end(), "imported catalog state should round-trip separately");
    Require(public_match->second.active_view_id == "public-view", "public active view should stay isolated");
    Require(imported_match->second.active_view_id == "imported-view", "imported active view should stay isolated");
    Require(!specforge::IsMarkerVisible(public_match->second, "h_alpha"), "public visibility should stay isolated");
    Require(specforge::IsMarkerVisible(imported_match->second, "h_alpha"), "imported visibility should stay isolated");
}

void TestLegacyExpandedGroupsMigrateToPanelState()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_spectral_line_user_state_legacy_panel_test.json";
    {
        std::ofstream stream(path);
        stream << "{\n";
        stream << "  \"format_kind\": \"specforge.catalog_user_state.cache\",\n";
        stream << "  \"schema_version\": 1,\n";
        stream << "  \"catalogs\": {\n";
        stream << "    \"specforge.public\": {\n";
        stream << "      \"active_view_id\": \"view-1\",\n";
        stream << "      \"marker_visibility\": {},\n";
        stream << "      \"expanded_group_ids\": [\"view-1/group-1\"],\n";
        stream << "      \"grouping_views\": []\n";
        stream << "    }\n";
        stream << "  }\n";
        stream << "}\n";
    }

    const specforge::CatalogUserStateCacheLoadResult loaded = specforge::LoadCatalogUserStateCache(path);
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);

    Require(loaded.warning.empty(), loaded.warning);
    const auto panel = loaded.cache.catalog_panel_state.find("specforge.public");
    Require(panel != loaded.cache.catalog_panel_state.end(), "legacy expanded state should migrate to panel state");
    Require(
        panel->second.expanded_group_ids.find("view-1/group-1") != panel->second.expanded_group_ids.end(),
        "legacy expanded group id should be preserved");
}

void TestCacheReadsUnicodeEscapes()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_spectral_line_user_state_unicode_cache_test.json";
    {
        std::ofstream stream(path);
        stream << "{\n";
        stream << "  \"format_kind\": \"specforge.catalog_user_state.cache\",\n";
        stream << "  \"schema_version\": 2,\n";
        stream << "  \"catalogs\": {\n";
        stream << "    \"specforge.public\": {\n";
        stream << "      \"active_view_id\": \"view-\\u0031\",\n";
        stream << "      \"marker_visibility\": {},\n";
        stream << "      \"grouping_views\": [\n";
        stream << "        {\n";
        stream << "          \"id\": \"view-1\",\n";
        stream << "          \"name\": \"Unicode view\",\n";
        stream << "          \"read_only\": false,\n";
        stream << "          \"groups\": [\n";
        stream << "            {\n";
        stream << "              \"id\": \"group-1\",\n";
        stream << "              \"name\": \"\\u4e2d\\u6587\",\n";
        stream << "              \"is_unassigned\": false,\n";
        stream << "              \"marker_references\": [\"h_alpha\"]\n";
        stream << "            }\n";
        stream << "          ]\n";
        stream << "        }\n";
        stream << "      ]\n";
        stream << "    }\n";
        stream << "  }\n";
        stream << "}\n";
    }

    const specforge::CatalogUserStateCacheLoadResult loaded = specforge::LoadCatalogUserStateCache(path);
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);

    Require(loaded.warning.empty(), loaded.warning);
    const auto catalog = loaded.cache.catalogs.find("specforge.public");
    Require(catalog != loaded.cache.catalogs.end(), "unicode cache should load public catalog state");
    Require(catalog->second.active_view_id == "view-1", "unicode escape should decode ASCII code points");
    Require(catalog->second.grouping_views.size() == 1, "unicode cache should load grouping view");
    Require(catalog->second.grouping_views.front().groups.size() >= 1, "unicode cache should load group");
    Require(
        catalog->second.grouping_views.front().groups.front().name == "\xE4\xB8\xAD\xE6\x96\x87",
        "unicode escapes should decode to UTF-8 group names");
}

void TestCorruptCacheIsWarningOnly()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_spectral_line_user_state_corrupt_cache_test.json";
    {
        std::ofstream stream(path);
        stream << "{ invalid json";
    }

    const specforge::CatalogUserStateCacheLoadResult loaded = specforge::LoadCatalogUserStateCache(path);
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);

    Require(!loaded.warning.empty(), "corrupt cache should produce a non-blocking warning");
    Require(loaded.cache.catalogs.empty(), "corrupt cache should be ignored");
}

void TestUnsupportedCacheSchemaIsWarningOnly()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_spectral_line_user_state_schema_cache_test.json";
    {
        std::ofstream stream(path);
        stream << "{\n";
        stream << "  \"format_kind\": \"specforge.catalog_user_state.cache\",\n";
        stream << "  \"schema_version\": 999,\n";
        stream << "  \"catalogs\": {}\n";
        stream << "}\n";
    }

    const specforge::CatalogUserStateCacheLoadResult loaded = specforge::LoadCatalogUserStateCache(path);
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);

    Require(!loaded.warning.empty(), "unsupported cache schema should produce a non-blocking warning");
    Require(loaded.cache.catalogs.empty(), "unsupported cache schema should be ignored");
}

}  // namespace

int main()
{
    TestPublicCatalogIdentityIsStable();
    TestDefaultMarkerVisibilityIsVisible();
    TestSearchStateDoesNotBulkToggleMarkers();
    TestUnresolvedReferenceIsPreserved();
    TestReferenceIdentityMustMatchCurrentCatalog();
    TestSharedReferencesAreDetected();
    TestMoveAndCopyMarkerReferences();
    TestRemoveUserGroupAndMarkerReferences();
    TestReorderUserGroups();
    TestMarkerVisibilityIsSharedAcrossViews();
    TestUngroupedCatalogHasNoCatalogGroupingView();
    TestCatalogGroupingViewUsesUniqueGroupIds();
    TestCacheRoundTrip();
    TestCacheSaveReplacesExistingFileWithoutLeavingTempFile();
    TestCacheSeparatesCatalogIdentities();
    TestLegacyExpandedGroupsMigrateToPanelState();
    TestCacheReadsUnicodeEscapes();
    TestCorruptCacheIsWarningOnly();
    TestUnsupportedCacheSchemaIsWarningOnly();
    return 0;
}
