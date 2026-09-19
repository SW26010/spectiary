#include "overlays/spectral_line_user_state.h"
#include "overlays/catalog_user_state_reconciliation.h"
#include "app/local_user_state_json.h"
#include "overlays/spectral_line_user_state_cache_io.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    Require(identity.id == "public-spectral-lines.v1", "public catalog identity should be fixed");
}

void TestMarkerColorsCanonicalizeAutoAndValidateExplicitRgba()
{
    specforge::CatalogUserState state =
        specforge::MakeCatalogUserState(
            specforge::PublicSpectralLineCatalogIdentity());
    Require(
        specforge::MarkerColor(state, "h_alpha").mode() ==
            specforge::PlotSeriesColorMode::Auto,
        "a marker without an override should resolve from canonical Auto state");

    const specforge::PlotSeriesColor custom =
        specforge::PlotSeriesColor::ExplicitColor({
            .red = 0.12f,
            .green = 0.34f,
            .blue = 0.56f,
            .alpha = 0.78f,
        });
    specforge::SetMarkerColor(state, "h_alpha", custom);
    Require(
        specforge::MarkerColor(state, "h_alpha") == custom &&
            state.marker_colors.size() == 1,
        "an explicit marker color should be stored by stable marker identity");

    specforge::SetMarkerColor(
        state,
        "h_alpha",
        specforge::PlotSeriesColor::Auto());
    Require(
        state.marker_colors.empty() &&
            specforge::MarkerColor(state, "h_alpha").mode() ==
                specforge::PlotSeriesColorMode::Auto,
        "Reset to Auto should erase the override instead of retaining an Auto payload");

    state.marker_colors.emplace(
        "h_beta",
        specforge::PlotSeriesColor::Auto());
    specforge::CatalogPanelState panel_state;
    const auto canonicalized =
        specforge::CanonicalizeCatalogUserState(
            state,
            panel_state,
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            specforge::BuildCatalogGroupingView(
                GroupedCatalog(),
                specforge::PublicSpectralLineCatalogIdentity()));
    Require(
        canonicalized.changed && state.marker_colors.empty(),
        "canonicalization should remove legacy or malformed Auto override entries");
}

void TestEmptyCachedGroupNameRecoversWithGeneratedProvenance()
{
    const specforge::SpectralLineCatalog catalog =
        GroupedCatalog();
    const specforge::CatalogIdentity identity =
        specforge::PublicSpectralLineCatalogIdentity();
    specforge::CatalogUserState state =
        specforge::MakeCatalogUserState(identity);
    specforge::GroupingView view;
    view.id = "view-1";
    view.name = "Review";
    specforge::UserGroup group;
    group.id = "group-1";
    group.name = "   ";
    view.groups.push_back(std::move(group));
    state.grouping_views.push_back(std::move(view));
    specforge::CatalogPanelState panel_state;

    const specforge::CatalogUserStateCanonicalizationResult result =
        specforge::CanonicalizeCatalogUserState(
            state,
            panel_state,
            catalog,
            identity,
            specforge::BuildCatalogGroupingView(
                catalog,
                identity));
    const specforge::UserGroup* recovered =
        FindGroupById(
            state.grouping_views.front(),
            "group-1");

    Require(
        result.changed &&
            recovered != nullptr &&
            recovered->name == "Group 1" &&
            recovered->generated_name.source ==
                specforge::GeneratedNameSource::
                    DefaultGroup &&
            recovered->generated_name.ordinal == 1,
        "a valid cached group with an empty name should recover through localizable generated-name provenance");
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
    const specforge::PlotSeriesColor line_color =
        specforge::PlotSeriesColor::ExplicitColor({
            .red = 0.125f,
            .green = 0.25f,
            .blue = 0.5f,
            .alpha = 0.75f,
        });
    const specforge::PlotSeriesColor band_color =
        specforge::PlotSeriesColor::ExplicitColor({
            .red = 0.8f,
            .green = 0.6f,
            .blue = 0.4f,
            .alpha = 0.2f,
        });
    specforge::SetMarkerColor(state, "h_alpha", line_color);
    specforge::SetMarkerColor(state, "molecular_band", band_color);
    specforge::SetMarkerColor(
        state,
        "auto_marker",
        specforge::PlotSeriesColor::Auto());

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
    cache.catalog_panel_state["public-spectral-lines.v1"].expanded_group_ids.insert(
        specforge::GroupExpansionKey("view-1", "group-1"));

    std::string error;
    Require(specforge::SaveCatalogUserStateCache(path, cache, error), error);
    const specforge::CatalogUserStateCacheLoadResult loaded = specforge::LoadCatalogUserStateCache(path);
    std::filesystem::remove(path, remove_error);

    Require(loaded.warning.empty(), loaded.warning);
    const auto catalog = loaded.cache.catalogs.find("public-spectral-lines.v1");
    Require(catalog != loaded.cache.catalogs.end(), "cache should preserve public catalog state");
    Require(catalog->second.active_view_id == "view-1", "cache should preserve active view");
    Require(!specforge::IsMarkerVisible(catalog->second, "h_alpha"), "cache should preserve marker visibility");
    Require(
        specforge::MarkerColor(catalog->second, "h_alpha") ==
                line_color &&
            specforge::MarkerColor(
                catalog->second,
                "molecular_band") == band_color &&
            !catalog->second.marker_colors.contains("auto_marker"),
        "cache should round-trip independent line and band overrides while keeping Auto absent");
    Require(catalog->second.grouping_views.size() == 1, "cache should preserve user grouping views");
    const auto panel = loaded.cache.catalog_panel_state.find("public-spectral-lines.v1");
    Require(panel != loaded.cache.catalog_panel_state.end(), "cache should preserve public panel state separately");
    Require(
        panel->second.expanded_group_ids.find(specforge::GroupExpansionKey("view-1", "group-1")) !=
            panel->second.expanded_group_ids.end(),
        "cache should preserve expanded group ids");
}

void TestSchemaFourMarkerColorsMigrateToAuto()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "specforge_spectral_line_schema_four_color_migration_test.json";
    {
        std::ofstream stream(path);
        stream << R"json({
  "format_kind": "spectiary.catalog_user_state.cache",
  "schema_version": 4,
  "catalogs": {
    "public-spectral-lines.v1": {
      "active_view_id": "",
      "next_view_sequence": 1,
      "next_group_sequence": 1,
      "reserved_view_ids": [],
      "reserved_group_ids": [],
      "marker_visibility": {},
      "grouping_views": []
    }
  },
  "catalog_panel_state": {}
})json";
    }

    const auto loaded =
        specforge::LoadCatalogUserStateCache(path);
    std::error_code error;
    std::filesystem::remove(path, error);
    Require(loaded.warning.empty(), loaded.warning);
    Require(
        loaded.schema_version == 4 && loaded.requires_save,
        "schema four should load as a supported one-time migration");
    const auto match =
        loaded.cache.catalogs.find("public-spectral-lines.v1");
    Require(
        match != loaded.cache.catalogs.end() &&
            match->second.marker_colors.empty() &&
            specforge::MarkerColor(match->second, "h_alpha").mode() ==
                specforge::PlotSeriesColorMode::Auto,
        "legacy catalogs without color state should migrate every marker to Auto");
}

void TestSchemaFiveRejectsCorruptMarkerColor()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "specforge_spectral_line_corrupt_color_test.json";
    {
        std::ofstream stream(path);
        stream << R"json({
  "format_kind": "spectiary.catalog_user_state.cache",
  "schema_version": 5,
  "catalogs": {
    "public-spectral-lines.v1": {
      "active_view_id": "",
      "next_view_sequence": 1,
      "next_group_sequence": 1,
      "reserved_view_ids": [],
      "reserved_group_ids": [],
      "marker_visibility": {},
      "marker_colors": {
        "h_alpha": {
          "mode": "explicit-color",
          "red": "1.5",
          "green": "0.2",
          "blue": "0.3",
          "alpha": "1"
        }
      },
      "grouping_views": []
    }
  },
  "catalog_panel_state": {}
})json";
    }

    const auto loaded =
        specforge::LoadCatalogUserStateCache(path);
    std::error_code error;
    std::filesystem::remove(path, error);
    Require(
        loaded.issue_kind ==
                specforge::CatalogUserStateCacheLoadIssueKind::InvalidDocument &&
            loaded.cache.catalogs.empty() &&
            loaded.diagnostic_detail.find("RGBA channels") !=
                std::string::npos,
        "a corrupt current-schema marker color should fail closed with a diagnostic");
}

void TestCacheLoadLeavesCanonicalizationToTheDomain()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "specforge_spectral_line_user_state_raw_cache_test.json";
    {
        std::ofstream stream(path);
        stream << R"json({
  "format_kind": "spectiary.catalog_user_state.cache",
  "schema_version": 2,
  "catalogs": {
    "public-spectral-lines.v1": {
      "active_view_id": "",
      "marker_visibility": {"": false},
      "grouping_views": [{
        "id": "view-1",
        "name": "  Review  ",
        "read_only": true,
        "groups": [{
          "id": "",
          "name": "",
          "is_unassigned": false,
          "marker_references": [{
            "catalog_identity": "",
            "marker_id": ""
          }]
        }]
      }]
    }
  },
  "catalog_panel_state": {
    "public-spectral-lines.v1": {"expanded_group_ids": [""]}
  }
})json";
    }
    const specforge::CatalogUserStateCacheLoadResult loaded =
        specforge::LoadCatalogUserStateCache(path);

    Require(loaded.warning.empty(), loaded.warning);
    Require(
        loaded.requires_save,
        "legacy schema two should request a one-time rewrite to the current schema");
    const auto catalog =
        loaded.cache.catalogs.find("public-spectral-lines.v1");
    Require(
        catalog != loaded.cache.catalogs.end(),
        "raw cache should preserve the parsed catalog entry");
    const specforge::CatalogUserState& state = catalog->second;
    Require(
        state.active_view_id.empty() &&
            state.marker_visibility.contains(""),
        "cache parsing should preserve raw selection and visibility keys");
    Require(
        state.grouping_views.size() == 1 &&
            state.grouping_views.front().read_only &&
            state.grouping_views.front().name == "  Review  ",
        "cache parsing should not canonicalize view flags or names");
    Require(
        state.grouping_views.front().groups.size() == 1 &&
            state.grouping_views.front().groups.front().id.empty(),
        "cache parsing should preserve invalid groups for domain canonicalization");
    const specforge::UserGroup& raw_group =
        state.grouping_views.front().groups.front();
    Require(
        raw_group.marker_references.size() == 1 &&
            raw_group.marker_references.front().marker_id.empty(),
        "cache parsing should preserve invalid references for domain canonicalization");
    const auto panel =
        loaded.cache.catalog_panel_state.find("public-spectral-lines.v1");
    Require(
        panel != loaded.cache.catalog_panel_state.end() &&
            panel->second.expanded_group_ids.contains(""),
        "cache parsing should preserve raw panel keys");
}

void TestLegacySchemaTwoEditableNamesRemainUserOwned()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "specforge_spectral_line_user_state_legacy_generated_names_test.json";
    {
        std::ofstream stream(path);
        stream << R"json({
  "format_kind": "spectiary.catalog_user_state.cache",
  "schema_version": 2,
  "catalogs": {
    "public-spectral-lines.v1": {
      "active_view_id": "view-1",
      "marker_visibility": {},
      "grouping_views": [{
        "id": "view-1",
        "name": "Grouping 1",
        "groups": [{
          "id": "group-1",
          "name": "Group 1",
          "is_unassigned": false,
          "marker_references": []
        }]
      }, {
        "id": "view-2",
        "name": "Grouping 1 copy",
        "groups": [{
          "id": "group-1",
          "name": "Group 1",
          "is_unassigned": false,
          "marker_references": []
        }]
      }, {
        "id": "view-3",
        "name": "Grouping 7",
        "groups": [{
          "id": "group-9",
          "name": "Group 7",
          "is_unassigned": false,
          "marker_references": []
        }]
      }, {
        "id": "view-4",
        "name": "Draft copy",
        "groups": []
      }, {
        "id": "view-6",
        "name": "Grouping 5",
        "groups": []
      }, {
        "id": "view-7",
        "name": "Catalog grouping view copy",
        "groups": []
      }]
    }
  },
  "catalog_panel_state": {}
})json";
    }
    const specforge::CatalogUserStateCacheLoadResult loaded =
        specforge::LoadCatalogUserStateCache(path);

    Require(loaded.warning.empty(), loaded.warning);
    Require(
        loaded.requires_save,
        "legacy caches should request a one-time schema rewrite");
    const auto catalog =
        loaded.cache.catalogs.find("public-spectral-lines.v1");
    Require(
        catalog != loaded.cache.catalogs.end() &&
            catalog->second.grouping_views.size() == 6,
        "legacy generated-name fixture should load all user views");
    const std::vector<specforge::GroupingView>& views =
        catalog->second.grouping_views;
    Require(
        views[0].generated_name.source ==
                specforge::GeneratedNameSource::None &&
            views[0].groups[0].generated_name.source ==
                specforge::GeneratedNameSource::None,
        "legacy exact-shape default view and group names must remain user-owned");
    Require(
        views[1].generated_name.source ==
                specforge::GeneratedNameSource::None &&
            views[1].generated_name.copy_count == 0 &&
            views[1].groups[0].generated_name.source ==
                specforge::GeneratedNameSource::None,
        "legacy editable copies must not acquire generated-name metadata");
    Require(
        views[2].generated_name.source ==
                specforge::GeneratedNameSource::None &&
            views[2].groups[0].generated_name.source ==
                specforge::GeneratedNameSource::None &&
            views[3].generated_name.source ==
                specforge::GeneratedNameSource::None &&
            views[3].generated_name.copy_count == 0,
        "legacy migration must not infer same-shaped custom names from editable strings alone");
    Require(
        views[4].generated_name.source ==
                specforge::GeneratedNameSource::None,
        "legacy names must remain user-owned even when their ordinal matches their old append position");
    Require(
        views[5].generated_name.source ==
                specforge::GeneratedNameSource::None &&
            views[5].generated_name.copy_count == 0,
        "legacy catalog-view copies are editable names and must remain user-owned");

    std::string save_error;
    Require(
        specforge::SaveCatalogUserStateCache(
            path,
            loaded.cache,
            save_error),
        save_error);
    const specforge::CatalogUserStateCacheLoadResult
        reloaded =
            specforge::LoadCatalogUserStateCache(
                path);
    Require(
        !reloaded.requires_save,
        "rewritten generated-name metadata should use the current cache schema");
    const std::vector<specforge::GroupingView>&
        reloaded_views =
            reloaded.cache.catalogs
                .at("public-spectral-lines.v1")
                .grouping_views;
    Require(
        reloaded_views[0].generated_name.source ==
                specforge::GeneratedNameSource::None &&
            reloaded_views[0].groups[0].generated_name
                    .source ==
                specforge::GeneratedNameSource::None &&
            reloaded_views[1].generated_name.source ==
                specforge::GeneratedNameSource::None &&
            reloaded_views[2].generated_name.source ==
                specforge::GeneratedNameSource::None &&
            reloaded_views[3].generated_name
                    .copy_count ==
                0 &&
            reloaded_views[4].generated_name.source ==
                specforge::GeneratedNameSource::None &&
            reloaded_views[5].generated_name.source ==
                specforge::GeneratedNameSource::None,
        "current-schema reload must preserve conservative user ownership for every legacy editable name");

    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
}

void TestCacheSaveReplacesExistingFileWithoutLeavingTempFile()
{
    specforge::CatalogUserState state =
        specforge::MakeCatalogUserState(specforge::PublicSpectralLineCatalogIdentity());
    state.active_view_id = "view-1";
    state.marker_visibility["h_alpha"] = false;

    specforge::CatalogUserStateCache cache;
    cache.catalogs.emplace(state.catalog_identity.id, state);

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "specforge_spectral_line_user_state_atomic_save_test";
    const std::filesystem::path path = root / "state.json";
    std::error_code remove_error;
    std::filesystem::remove_all(root, remove_error);
    std::filesystem::create_directories(root);
    {
        std::ofstream stream(path);
        stream << "{ invalid old cache";
    }

    std::string error;
    Require(specforge::SaveCatalogUserStateCache(path, cache, error), error);
    const specforge::CatalogUserStateCacheLoadResult loaded = specforge::LoadCatalogUserStateCache(path);

    const std::string temporary_prefix = path.filename().string() + ".tmp.";
    bool found_temporary_file = false;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().filename().string().starts_with(temporary_prefix)) {
            found_temporary_file = true;
            break;
        }
    }

    std::filesystem::remove_all(root, remove_error);

    Require(loaded.warning.empty(), loaded.warning);
    Require(loaded.cache.catalogs.find("public-spectral-lines.v1") != loaded.cache.catalogs.end(), "saved cache should load");
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
    const auto public_match = loaded.cache.catalogs.find("public-spectral-lines.v1");
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
        stream << "  \"format_kind\": \"spectiary.catalog_user_state.cache\",\n";
        stream << "  \"schema_version\": 1,\n";
        stream << "  \"catalogs\": {\n";
        stream << "    \"public-spectral-lines.v1\": {\n";
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
    const auto panel = loaded.cache.catalog_panel_state.find("public-spectral-lines.v1");
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
        stream << "  \"format_kind\": \"spectiary.catalog_user_state.cache\",\n";
        stream << "  \"schema_version\": 2,\n";
        stream << "  \"catalogs\": {\n";
        stream << "    \"public-spectral-lines.v1\": {\n";
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
    const auto catalog = loaded.cache.catalogs.find("public-spectral-lines.v1");
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
    Require(
        loaded.issue_kind ==
                specforge::CatalogUserStateCacheLoadIssueKind::
                    InvalidDocument &&
            !loaded.diagnostic_detail.empty() &&
            loaded.warning ==
                loaded.diagnostic_detail,
        "corrupt cache should expose typed parser detail without an English application prefix");
    Require(loaded.cache.catalogs.empty(), "corrupt cache should be ignored");
}

void TestDuplicateJsonObjectKeysAreRejected()
{
    std::string error;
    const std::optional<nlohmann::json> parsed = specforge::ParseJson(
        R"json({
  "catalogs": {
    "public-spectral-lines.v1": {},
    "public-spectral-lines.v1": {}
  }
})json",
        error);
    Require(
        !parsed.has_value() &&
            error.find("duplicate JSON object member") != std::string::npos,
        "the JSON parser must reject duplicate object keys with an actionable diagnostic");
}

void TestUnsupportedCacheSchemaIsWarningOnly()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_spectral_line_user_state_schema_cache_test.json";
    {
        std::ofstream stream(path);
        stream << "{\n";
        stream << "  \"format_kind\": \"spectiary.catalog_user_state.cache\",\n";
        stream << "  \"schema_version\": 999,\n";
        stream << "  \"catalogs\": {}\n";
        stream << "}\n";
    }

    const specforge::CatalogUserStateCacheLoadResult loaded = specforge::LoadCatalogUserStateCache(path);
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);

    Require(!loaded.warning.empty(), "unsupported cache schema should produce a non-blocking warning");
    Require(
        loaded.issue_kind ==
                specforge::CatalogUserStateCacheLoadIssueKind::
                    UnsupportedFormatOrSchema &&
            loaded.diagnostic_detail.find(
                "schema_version=999") !=
                std::string::npos &&
            loaded.warning ==
                loaded.diagnostic_detail,
        "unsupported cache should expose typed schema detail without an English application prefix");
    Require(loaded.cache.catalogs.empty(), "unsupported cache schema should be ignored");
}

void TestSchemaThreeRejectsExcessiveGeneratedCopyCount()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "specforge_spectral_line_user_state_excessive_copy_count_test.json";
    {
        std::ofstream stream(path);
        stream << R"json({
  "format_kind": "spectiary.catalog_user_state.cache",
  "schema_version": 3,
  "catalogs": {
    "public-spectral-lines.v1": {
      "active_view_id": "view-1",
      "marker_visibility": {},
      "grouping_views": [{
        "id": "view-1",
        "name": "Grouping 1",
        "name_source": "default_grouping_view",
        "name_ordinal": 1,
        "generated_copy_count": )json"
               << (specforge::kMaximumGeneratedNameCopyCount + 1)
               << R"json(,
        "groups": []
      }]
    }
  },
  "catalog_panel_state": {}
})json";
    }

    const specforge::CatalogUserStateCacheLoadResult loaded =
        specforge::LoadCatalogUserStateCache(path);
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);

    Require(
        loaded.issue_kind ==
                specforge::CatalogUserStateCacheLoadIssueKind::
                    InvalidDocument &&
            loaded.diagnostic_detail.find(
                "generated_copy_count") !=
                std::string::npos &&
            loaded.cache.catalogs.empty(),
        "schema-three caches must reject generated copy counts above the rendering safety limit");
}

specforge::GroupingView ReconciliationView(
    std::string id,
    std::string name)
{
    specforge::GroupingView view;
    view.id = std::move(id);
    view.name = std::move(name);
    specforge::UserGroup unassigned;
    unassigned.id = specforge::UnassignedUserGroupId();
    unassigned.name = "Unassigned";
    unassigned.is_unassigned = true;
    view.groups.push_back(std::move(unassigned));
    return view;
}

void TestCatalogTaskReconciliationMergesDisjointChanges()
{
    const specforge::CatalogIdentity identity =
        specforge::PublicSpectralLineCatalogIdentity();
    specforge::CatalogUserState base =
        specforge::MakeCatalogUserState(identity);
    base.active_view_id = "view-1";
    base.grouping_views.push_back(
        ReconciliationView("view-1", "Base one"));
    base.grouping_views.push_back(
        ReconciliationView("view-2", "Base two"));
    base.grouping_views.front().groups.push_back(
        specforge::UserGroup{
            .id = "group-1",
            .name = "One",
        });
    base.grouping_views.front().groups.push_back(
        specforge::UserGroup{
            .id = "group-2",
            .name = "Two",
        });

    specforge::CatalogUserState local = base;
    local.marker_visibility["h_alpha"] = false;
    local.active_view_id = "view-2";
    local.grouping_views.erase(local.grouping_views.begin());

    specforge::CatalogUserState latest = base;
    latest.marker_visibility["h_beta"] = false;
    latest.grouping_views[1].name = "Remote two";
    latest.grouping_views.push_back(
        ReconciliationView("view-3", "Remote addition"));

    specforge::CatalogPanelState base_panel;
    specforge::CatalogPanelState local_panel = base_panel;
    local_panel.expanded_group_ids.insert("view-2/__unassigned__");
    specforge::CatalogPanelState latest_panel = base_panel;
    latest_panel.expanded_group_ids.insert("view-3/__unassigned__");

    specforge::CatalogUserStateReconciliationResult result;
    std::string diagnostic;
    Require(
        specforge::ReconcileCatalogUserStateTask(
            base,
            local,
            latest,
            base_panel,
            local_panel,
            latest_panel,
            result,
            diagnostic),
        diagnostic);

    Require(
        !result.state.marker_visibility.at("h_alpha") &&
            !result.state.marker_visibility.at("h_beta"),
        "task reconciliation should merge disjoint marker visibility changes");
    Require(
        result.state.active_view_id == "view-2" &&
            result.state.grouping_views.size() == 2,
        "task deletion and explicit selection should not erase the unrelated durable view");
    Require(
        result.state.grouping_views.front().name == "Remote two" &&
            result.state.grouping_views.front().id == "view-2" &&
            result.state.grouping_views.back().id == "view-3",
        "task reconciliation should retain the latest edit of a surviving view");
    Require(
        result.panel_state.expanded_group_ids.contains("view-2/__unassigned__") &&
            result.panel_state.expanded_group_ids.contains("view-3/__unassigned__"),
        "task reconciliation should merge independent panel expansion keys");
}

void TestCatalogTaskReconciliationMergesColorOverridesAndReset()
{
    const specforge::CatalogIdentity identity =
        specforge::PublicSpectralLineCatalogIdentity();
    specforge::CatalogUserState base =
        specforge::MakeCatalogUserState(identity);
    specforge::SetMarkerColor(
        base,
        "h_alpha",
        specforge::PlotSeriesColor::ExplicitColor({
            .red = 0.1f,
            .green = 0.2f,
            .blue = 0.3f,
            .alpha = 0.4f,
        }));

    specforge::CatalogUserState local = base;
    specforge::SetMarkerColor(
        local,
        "h_alpha",
        specforge::PlotSeriesColor::Auto());
    specforge::CatalogUserState latest = base;
    const specforge::PlotSeriesColor peer_color =
        specforge::PlotSeriesColor::ExplicitColor({
            .red = 0.9f,
            .green = 0.8f,
            .blue = 0.7f,
            .alpha = 0.6f,
        });
    specforge::SetMarkerColor(latest, "h_beta", peer_color);

    specforge::CatalogUserStateReconciliationResult result;
    std::string diagnostic;
    Require(
        specforge::ReconcileCatalogUserStateTask(
            base,
            local,
            latest,
            {},
            {},
            {},
            result,
            diagnostic),
        diagnostic);
    Require(
        !result.state.marker_colors.contains("h_alpha") &&
            specforge::MarkerColor(result.state, "h_beta") ==
                peer_color,
        "a local Reset to Auto and a peer marker override should both survive three-way merge");
}

void TestCatalogTaskReconciliationResolvesAddedIdAndSelectionConflict()
{
    const specforge::CatalogIdentity identity =
        specforge::PublicSpectralLineCatalogIdentity();
    specforge::CatalogUserState base =
        specforge::MakeCatalogUserState(identity);
    specforge::CatalogUserState local = base;
    local.grouping_views.push_back(
        ReconciliationView("view-1", "Local addition"));
    local.active_view_id = "view-1";
    specforge::CatalogUserState latest = base;
    latest.grouping_views.push_back(
        ReconciliationView("view-1", "Durable addition"));
    latest.active_view_id = "view-1";

    specforge::CatalogUserStateReconciliationResult result;
    std::string diagnostic;
    Require(
        specforge::ReconcileCatalogUserStateTask(
            base,
            local,
            latest,
            {},
            {},
            {},
            result,
            diagnostic),
        diagnostic);

    Require(
        result.state.grouping_views.size() == 2,
        "different concurrent additions with the same id should both survive");
    Require(
        result.state.grouping_views[0].name == "Durable addition" &&
            result.state.grouping_views[1].name == "Local addition" &&
            result.state.active_view_id == result.state.grouping_views[1].id &&
            result.state.grouping_views[1].id != "view-1",
        "a colliding task addition should receive a fresh id and retain task selection");
}

void TestCatalogTaskReconciliationPreservesExplicitOrderAndAdditions()
{
    const specforge::CatalogIdentity identity =
        specforge::PublicSpectralLineCatalogIdentity();
    specforge::CatalogUserState base =
        specforge::MakeCatalogUserState(identity);
    base.active_view_id = "view-1";
    base.grouping_views.push_back(
        ReconciliationView("view-1", "Base one"));
    base.grouping_views.push_back(
        ReconciliationView("view-2", "Base two"));
    base.grouping_views[0].groups.push_back(
        specforge::UserGroup{.id = "group-1", .name = "One"});
    base.grouping_views[0].groups.push_back(
        specforge::UserGroup{.id = "group-2", .name = "Two"});

    specforge::CatalogUserState local = base;
    std::swap(local.grouping_views[0], local.grouping_views[1]);
    local.active_view_id = "view-2";
    local.grouping_views[1].groups.insert(
        local.grouping_views[1].groups.begin() + 1,
        local.grouping_views[1].groups.back());
    local.grouping_views[1].groups.pop_back();
    local.grouping_views[1].groups.push_back(
        specforge::UserGroup{.id = "group-3", .name = "Task group"});
    local.grouping_views.push_back(
        ReconciliationView("view-3", "Task addition"));

    specforge::CatalogUserState latest = base;
    latest.grouping_views[0].groups.push_back(
        specforge::UserGroup{.id = "group-4", .name = "Durable group"});
    latest.grouping_views.push_back(
        ReconciliationView("view-4", "Durable addition"));
    latest.active_view_id = "view-1";

    specforge::CatalogUserStateReconciliationResult result;
    std::string diagnostic;
    Require(
        specforge::ReconcileCatalogUserStateTask(
            base,
            local,
            latest,
            {},
            {},
            {},
            result,
            diagnostic),
        diagnostic);

    Require(
        result.state.grouping_views.size() == 4 &&
            result.state.grouping_views[0].id == "view-2" &&
            result.state.grouping_views[1].id == "view-1" &&
            result.state.grouping_views[2].id == "view-4" &&
            result.state.grouping_views[3].id == "view-3" &&
            result.state.active_view_id == "view-2",
        "local explicit view order and selection must win while durable additions precede task additions");
    const auto& groups = result.state.grouping_views[1].groups;
    Require(
        groups.size() == 5 &&
            groups[0].id == specforge::UnassignedUserGroupId() &&
            groups[1].id == "group-2" &&
            groups[2].id == "group-1" &&
            groups[3].id == "group-4" &&
            groups[4].id == "group-3",
        "local explicit group order must win while durable additions precede task additions");

}

void TestCatalogTaskReconciliationRemapsIdsDeterministically()
{
    const specforge::CatalogIdentity identity =
        specforge::PublicSpectralLineCatalogIdentity();
    specforge::CatalogUserState base =
        specforge::MakeCatalogUserState(identity);
    base.active_view_id = "view-base";
    specforge::GroupingView base_view =
        ReconciliationView("view-base", "Base");
    base_view.groups.push_back(
        specforge::UserGroup{.id = "group-base", .name = "Base group"});
    base.grouping_views.push_back(std::move(base_view));

    specforge::CatalogUserState local = base;
    local.grouping_views.front().groups.push_back(
        specforge::UserGroup{.id = "group-1-2", .name = "Task group 1-2"});
    local.grouping_views.front().groups.push_back(
        specforge::UserGroup{.id = "group-1", .name = "Task group 1"});
    // Deliberately reverse the colliding requests in the task vector. The
    // allocator contract sorts requested identities before assigning fresh
    // suffixes, while retaining the task's order in the merged output.
    local.grouping_views.push_back(
        ReconciliationView("view-1-2", "Task view 1-2"));
    local.grouping_views.push_back(
        ReconciliationView("view-1", "Task view 1"));

    specforge::CatalogUserState latest = base;
    latest.grouping_views.front().groups.push_back(
        specforge::UserGroup{.id = "group-1", .name = "Durable group 1"});
    latest.grouping_views.push_back(
        ReconciliationView("view-1", "Durable view 1"));

    specforge::CatalogUserStateReconciliationResult result;
    std::string diagnostic;
    Require(
        specforge::ReconcileCatalogUserStateTask(
            base,
            local,
            latest,
            {},
            {},
            {},
            result,
            diagnostic),
        diagnostic);

    const auto task_view_one = std::find_if(
        result.state.grouping_views.begin(),
        result.state.grouping_views.end(),
        [](const auto& view) { return view.name == "Task view 1"; });
    const auto task_view_one_two = std::find_if(
        result.state.grouping_views.begin(),
        result.state.grouping_views.end(),
        [](const auto& view) { return view.name == "Task view 1-2"; });
    Require(
        task_view_one != result.state.grouping_views.end() &&
            task_view_one_two != result.state.grouping_views.end() &&
            task_view_one->id == "view-1-2" &&
            task_view_one_two->id == "view-1-2-2",
        "view identity remaps must use sorted requested identities");

    const auto group_one = std::find_if(
        result.state.grouping_views.front().groups.begin(),
        result.state.grouping_views.front().groups.end(),
        [](const auto& group) { return group.name == "Task group 1"; });
    const auto group_one_two = std::find_if(
        result.state.grouping_views.front().groups.begin(),
        result.state.grouping_views.front().groups.end(),
        [](const auto& group) { return group.name == "Task group 1-2"; });
    Require(
        group_one != result.state.grouping_views.front().groups.end() &&
            group_one_two != result.state.grouping_views.front().groups.end() &&
            group_one->id == "group-1-2" &&
            group_one_two->id == "group-1-2-2",
        "group identity remaps must use sorted requested identities");
}

void TestCatalogReconciliationRejectsInvalidSemanticIdentities()
{
    const specforge::CatalogIdentity identity =
        specforge::PublicSpectralLineCatalogIdentity();
    specforge::CatalogUserStateCache cache;
    specforge::CatalogUserState state =
        specforge::MakeCatalogUserState(identity);
    state.grouping_views.push_back(
        ReconciliationView("", "Empty identity"));
    cache.catalogs.emplace(identity.id, state);

    std::string diagnostic;
    Require(
        !specforge::ValidateCatalogUserStateCacheForReconciliation(
            cache,
            diagnostic) &&
            diagnostic.find("must not be empty") != std::string::npos,
        "reconciliation trust validation must reject an empty view identity");

    state.grouping_views.clear();
    state.grouping_views.push_back(
        ReconciliationView("view-1", "First"));
    state.grouping_views.push_back(
        ReconciliationView("view-1", "Duplicate"));
    cache.catalogs.at(identity.id) = state;
    Require(
        !specforge::ValidateCatalogUserStateCacheForReconciliation(
            cache,
            diagnostic) &&
            diagnostic.find("duplicate grouping view identity") !=
                std::string::npos,
        "reconciliation trust validation must reject duplicate view identities");
}

}  // namespace

int main()
{
    try {
        TestPublicCatalogIdentityIsStable();
        TestMarkerColorsCanonicalizeAutoAndValidateExplicitRgba();
        TestEmptyCachedGroupNameRecoversWithGeneratedProvenance();
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
        TestSchemaFourMarkerColorsMigrateToAuto();
        TestSchemaFiveRejectsCorruptMarkerColor();
        TestCacheLoadLeavesCanonicalizationToTheDomain();
        TestLegacySchemaTwoEditableNamesRemainUserOwned();
        TestCacheSaveReplacesExistingFileWithoutLeavingTempFile();
        TestCacheSeparatesCatalogIdentities();
        TestLegacyExpandedGroupsMigrateToPanelState();
        TestCacheReadsUnicodeEscapes();
        TestCorruptCacheIsWarningOnly();
        TestDuplicateJsonObjectKeysAreRejected();
        TestUnsupportedCacheSchemaIsWarningOnly();
        TestSchemaThreeRejectsExcessiveGeneratedCopyCount();
        TestCatalogTaskReconciliationMergesDisjointChanges();
        TestCatalogTaskReconciliationMergesColorOverridesAndReset();
        TestCatalogTaskReconciliationResolvesAddedIdAndSelectionConflict();
        TestCatalogTaskReconciliationPreservesExplicitOrderAndAdditions();
        TestCatalogTaskReconciliationRemapsIdsDeterministically();
        TestCatalogReconciliationRejectsInvalidSemanticIdentities();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
