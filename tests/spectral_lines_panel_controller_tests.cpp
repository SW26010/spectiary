#include "overlays/spectral_line_user_state_cache_io.h"
#include "ui/spectral_lines_panel_controller.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

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

specforge::SpectrumSnapshotHandle Snapshot(bool can_show_spectral_lines)
{
    auto snapshot = std::make_shared<specforge::SpectrumSnapshot>();
    snapshot->capabilities.can_show_spectral_lines = can_show_spectral_lines;
    return snapshot;
}

std::filesystem::path TestCachePath(std::string_view test_name)
{
    return std::filesystem::temp_directory_path() /
           ("specforge_catalog_user_state_session_" + std::string(test_name) + ".json");
}

void RemoveTestCache(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + ".tmp", error);
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void WriteNonCanonicalCache(const std::filesystem::path& path)
{
    std::ofstream stream(path);
    stream << R"json({
  "format_kind": "specforge.catalog_user_state.cache",
  "schema_version": 2,
  "catalogs": {
    "specforge.public": {
      "active_view_id": "missing-view",
      "marker_visibility": {"": false},
      "grouping_views": [{
        "id": "view-1",
        "name": "  Review  ",
        "read_only": true,
        "groups": [{
          "id": "group-1",
          "name": "  Hydrogen  ",
          "is_unassigned": false,
          "marker_references": [
            {"catalog_identity": "specforge.public", "marker_id": "h_alpha"},
            {"catalog_identity": "specforge.public", "marker_id": "missing-marker"},
            {"catalog_identity": "foreign.catalog", "marker_id": "h_beta"}
          ]
        }, {
          "id": "__unassigned__",
          "name": "Wrong",
          "is_unassigned": false,
          "marker_references": [
            {"catalog_identity": "specforge.public", "marker_id": "h_beta"}
          ]
        }, {
          "id": "",
          "name": "",
          "is_unassigned": false,
          "marker_references": []
        }]
      }, {
        "id": "view-1",
        "name": "Duplicate",
        "groups": []
      }]
    }
  },
  "catalog_panel_state": {
    "specforge.public": {
      "expanded_group_ids": ["missing/group", "view-1/group-1"]
    }
  }
})json";
}

void WriteMalformedLegacyCacheBody(
    const std::filesystem::path& path,
    std::string_view catalogs_json)
{
    std::ofstream stream(path, std::ios::binary);
    stream << R"json({
  "format_kind": "specforge.catalog_user_state.cache",
  "schema_version": 2,
  "catalogs": )json"
           << catalogs_json
           << R"json(,
  "catalog_panel_state": {}
})json";
}

void WriteLegacyExactShapeUserNames(
    const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::binary);
    stream << R"json({
  "format_kind": "specforge.catalog_user_state.cache",
  "schema_version": 2,
  "catalogs": {
    "specforge.public": {
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
      }]
    }
  },
  "catalog_panel_state": {}
})json";
}

const specforge::SpectralLineGroupingView* FindGroupingView(
    const specforge::CatalogUserStateView& state,
    std::string_view view_id)
{
    const auto match = std::find_if(
        state.grouping_views.begin(),
        state.grouping_views.end(),
        [view_id](const specforge::SpectralLineGroupingView& view) {
            return view.id == view_id;
        });
    return match == state.grouping_views.end() ? nullptr : &(*match);
}

const specforge::SpectralLineGroupingView* FindActiveEditableView(
    const specforge::CatalogUserStateView& state)
{
    const auto match = std::find_if(
        state.grouping_views.begin(),
        state.grouping_views.end(),
        [](const specforge::SpectralLineGroupingView& view) {
            return view.active && view.editable;
        });
    return match == state.grouping_views.end() ? nullptr : &(*match);
}

const specforge::SpectralLineGroupView* FindGroup(
    const specforge::SpectralLineGroupingView& view,
    std::string_view group_id)
{
    const auto match = std::find_if(
        view.groups.begin(),
        view.groups.end(),
        [group_id](const specforge::SpectralLineGroupView& group) {
            return group.id == group_id;
        });
    return match == view.groups.end() ? nullptr : &(*match);
}

const specforge::SpectralLineGroupView* FindUnassignedGroup(
    const specforge::SpectralLineGroupingView& view)
{
    const auto match = std::find_if(
        view.groups.begin(),
        view.groups.end(),
        [](const specforge::SpectralLineGroupView& group) {
            return group.is_unassigned;
        });
    return match == view.groups.end() ? nullptr : &(*match);
}

const specforge::SpectralLineMarkerReferenceView* FindMarker(
    const specforge::SpectralLineGroupView& group,
    std::string_view marker_id)
{
    const auto match = std::find_if(
        group.marker_references.begin(),
        group.marker_references.end(),
        [marker_id](const specforge::SpectralLineMarkerReferenceView& marker) {
            return marker.marker_id == marker_id;
        });
    return match == group.marker_references.end() ? nullptr : &(*match);
}

void RequireApplied(const specforge::CatalogUserStateResult& result, std::string_view message)
{
    Require(result.status == specforge::CatalogUserStateResultStatus::Applied, message);
    Require(result.changed, "applied result should report a change");
}

void RequireRejected(const specforge::CatalogUserStateResult& result, std::string_view message)
{
    Require(result.status == specforge::CatalogUserStateResultStatus::Rejected, message);
    Require(!result.changed, "rejected result must not report a change");
    Require(!result.message.empty(), "rejected result should explain the invalid identity or invariant");
}

void RequireNoChange(
    const specforge::CatalogUserStateResult& result,
    std::string_view message)
{
    Require(
        result.status ==
            specforge::CatalogUserStateResultStatus::NoChange,
        message);
    Require(
        !result.changed &&
            !result.persistent_state_changed,
        "no-change result must not report a persistent mutation");
}

void TestForeignIdentitiesAreRejectedWithoutPersistence()
{
    const std::filesystem::path path = TestCachePath("identity_rejection");
    RemoveTestCache(path);
    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);

        RequireRejected(
            session.Submit(specforge::CatalogUserStateIntent::SelectGroupingView("missing-view")),
            "unknown active grouping view should be rejected");
        RequireRejected(
            session.Submit(specforge::CatalogUserStateIntent::AddUserGroup("missing-view")),
            "unknown grouping view mutation should be rejected");
        RequireRejected(
            session.Submit(specforge::CatalogUserStateIntent::SetMarkerVisibility("missing-marker", false)),
            "unknown catalog marker should be rejected");

        const specforge::CatalogUserStateView state = session.View();
        Require(state.user_grouping_view_count == 0, "rejected intents must leave user grouping views unchanged");
        Require(session.Flush(), "a clean session should flush successfully");
    }
    Require(!std::filesystem::exists(path), "rejected intents must not create a cache file");
    RemoveTestCache(path);
}

void TestPlotViewProjectsOnlyPlotOverlayState()
{
    const std::filesystem::path path = TestCachePath("plot_view");
    RemoveTestCache(path);
    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);

        const specforge::SpectralLinePlotView no_snapshot = session.PlotView(nullptr);
        Require(no_snapshot.visible_markers.empty(), "a missing snapshot must not expose plot markers");
        Require(no_snapshot.marker_labels_visible, "plot label visibility should be available without a snapshot");
        Require(
            no_snapshot.layout_scope_id == specforge::PublicSpectralLineCatalogIdentity().id,
            "plot layout scope should use the stable catalog identity");

        const specforge::SpectralLinePlotView unsupported = session.PlotView(Snapshot(false));
        Require(unsupported.visible_markers.empty(), "an unsupported snapshot must not expose plot markers");

        specforge::SpectralLinePlotView plot_view = session.PlotView(Snapshot(true));
        Require(plot_view.visible_markers.size() == 3, "a supported snapshot should expose visible catalog markers");
        Require(
            plot_view.visible_markers[0]->id == "h_beta" &&
                plot_view.visible_markers[1]->id == "h_alpha" &&
                plot_view.visible_markers[2]->id == "ca_ii_8500",
            "plot markers should preserve catalog order");

        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::SetMarkerLabelsVisible(false)),
            "plot label visibility intent should be applied");
        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::SetMarkerVisibility("h_alpha", false)),
            "plot marker visibility intent should be applied");

        plot_view = session.PlotView(Snapshot(true));
        Require(!plot_view.marker_labels_visible, "plot view should reflect current marker label visibility");
        Require(plot_view.visible_markers.size() == 2, "plot view should omit hidden markers");
        Require(
            std::none_of(
                plot_view.visible_markers.begin(),
                plot_view.visible_markers.end(),
                [](const specforge::SpectralLineMarker* marker) {
                    return marker != nullptr && marker->id == "h_alpha";
                }),
            "plot view should not expose a marker hidden through catalog user state");
    }
    RemoveTestCache(path);
}

void TestCacheLoadUsesDomainCanonicalizationAndPreservesUnresolvedMarkers()
{
    const std::filesystem::path path =
        TestCachePath("canonicalization");
    RemoveTestCache(path);
    WriteNonCanonicalCache(path);

    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        const specforge::CatalogUserStateView state = session.View();
        Require(
            state.user_grouping_view_count == 1,
            "domain canonicalization should remove duplicate view identities");

        const specforge::SpectralLineGroupingView* catalog_view =
            FindGroupingView(state, specforge::CatalogGroupingViewId());
        Require(
            catalog_view != nullptr && catalog_view->active &&
                catalog_view->selection_requested,
            "domain canonicalization should repair invalid active selection and request presentation");

        const specforge::SpectralLineGroupingView* user_view =
            FindGroupingView(state, "view-1");
        Require(
            user_view != nullptr && user_view->editable &&
                user_view->name == "Review",
            "domain canonicalization should own persisted view flags and names");
        const specforge::SpectralLineGroupView* group =
            FindGroup(*user_view, "group-1");
        const specforge::SpectralLineGroupView* unassigned =
            FindUnassignedGroup(*user_view);
        Require(
            group != nullptr && group->name == "Hydrogen" &&
                group->expanded,
            "domain canonicalization should trim group names and retain valid panel state");
        Require(
            unassigned != nullptr &&
                unassigned->id == specforge::UnassignedUserGroupId() &&
                unassigned->name == "Unassigned",
            "domain canonicalization should create exactly the canonical unassigned group");

        const specforge::SpectralLineMarkerReferenceView* unresolved =
            FindMarker(*group, "missing-marker");
        Require(
            unresolved != nullptr && !unresolved->resolved &&
                unresolved->label == "missing-marker" &&
                unresolved->wavelength_text.empty(),
            "unresolved persisted markers should survive the canonicalization path");
        Require(
            FindMarker(*group, "h_beta") == nullptr &&
                FindMarker(*unassigned, "h_beta") != nullptr,
            "foreign references should be removed while current catalog markers remain assigned");

        Require(
            session.Flush(),
            "canonical state repaired during cache load should persist");
    }

    const specforge::CatalogUserStateCacheLoadResult persisted =
        specforge::LoadCatalogUserStateCache(path);
    const specforge::CatalogUserState& state =
        persisted.cache.catalogs.at("specforge.public");
    const specforge::CatalogPanelState& panel =
        persisted.cache.catalog_panel_state.at("specforge.public");
    Require(
        !state.marker_visibility.contains("") &&
            state.grouping_views.size() == 1 &&
            !state.grouping_views.front().read_only,
        "persisted cache should contain the domain-canonical form");
    Require(
        panel.expanded_group_ids.size() == 1 &&
            panel.expanded_group_ids.contains("view-1/group-1"),
        "domain canonicalization should discard unresolved panel expansion keys");

    RemoveTestCache(path);
}

void TestPersistentIntentUsesDomainCanonicalizationForSelection()
{
    const std::filesystem::path path =
        TestCachePath("intent_canonicalization");
    RemoveTestCache(path);

    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    CreateUserGroupingView()),
            "creating a view should apply");
        specforge::CatalogUserStateView state = session.View();
        const specforge::SpectralLineGroupingView* user_view =
            FindActiveEditableView(state);
        Require(
            user_view != nullptr,
            "created view should become active");
        const std::string user_view_id = user_view->id;
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    AcknowledgeGroupingViewSelection(user_view_id)),
            "selection acknowledgement should apply");

        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    DeleteUserGroupingView(user_view_id)),
            "deleting the active view should apply");
        state = session.View();
        const specforge::SpectralLineGroupingView* catalog_view =
            FindGroupingView(state, specforge::CatalogGroupingViewId());
        Require(
            catalog_view != nullptr && catalog_view->active &&
                catalog_view->selection_requested,
            "persistent intent should use domain canonicalization to select the fallback view");
    }

    RemoveTestCache(path);
}

void TestGeneratedNamesPreserveStoredValuesAndOrigins()
{
    const std::filesystem::path path =
        TestCachePath("generated_names");
    RemoveTestCache(path);

    std::string view_id;
    std::string group_id;
    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    CreateUserGroupingView()),
            "generated-name fixture should create a user view");

        specforge::CatalogUserStateView state =
            session.View();
        const specforge::SpectralLineGroupingView* view =
            FindActiveEditableView(state);
        Require(
            view != nullptr &&
                view->name == "Grouping 1" &&
                view->generated_name.source ==
                    specforge::GeneratedNameSource::
                        DefaultGroupingView &&
                view->generated_name.ordinal == 1,
            "created grouping views should carry explicit generated-name metadata");
        view_id = view->id;

        RequireNoChange(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    RenameUserGroupingView(
                        view_id,
                        view->name,
                        specforge::
                            CatalogUserRenameEditState::
                                Unedited)),
            "confirming an unedited grouping view name should preserve the stored value");

        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    AddUserGroup(view_id)),
            "generated-name fixture should add a user group");
        state = session.View();
        view = FindGroupingView(state, view_id);
        const specforge::SpectralLineGroupView* group =
            view == nullptr || view->groups.empty()
                ? nullptr
                : &view->groups.front();
        Require(
            group != nullptr &&
                !group->is_unassigned &&
                group->name == "Group 1" &&
                group->generated_name.source ==
                    specforge::GeneratedNameSource::
                        DefaultGroup &&
                group->generated_name.ordinal == 1,
            "created groups should carry explicit generated-name metadata");
        group_id = group->id;

        RequireNoChange(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    RenameUserGroup(
                        view_id,
                        group_id,
                        group->name,
                        specforge::
                            CatalogUserRenameEditState::
                                Unedited)),
            "confirming an unedited group name should preserve the stored value");
        Require(
            session.Flush(),
            "generated-name metadata should persist");
    }

    {
        specforge::SpectralLinesPanelController restored(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        specforge::CatalogUserStateView state =
            restored.View();
        const specforge::SpectralLineGroupingView* view =
            FindGroupingView(state, view_id);
        const specforge::SpectralLineGroupView* group =
            view == nullptr
                ? nullptr
                : FindGroup(*view, group_id);
        Require(
            view != nullptr &&
                view->name == "Grouping 1" &&
                view->generated_name.source ==
                    specforge::GeneratedNameSource::
                        DefaultGroupingView &&
                group != nullptr &&
                group->name == "Group 1" &&
                group->generated_name.source ==
                    specforge::GeneratedNameSource::
                        DefaultGroup,
            "generated-name metadata should survive the cache round trip");

        RequireApplied(
            restored.Submit(
                specforge::CatalogUserStateIntent::
                    RenameUserGroupingView(
                        view_id,
                        "Grouping 7",
                        specforge::
                            CatalogUserRenameEditState::
                                Edited)),
            "same-shaped user grouping names should be accepted");
        RequireApplied(
            restored.Submit(
                specforge::CatalogUserStateIntent::
                    RenameUserGroup(
                        view_id,
                        group_id,
                        "Group 7",
                        specforge::
                            CatalogUserRenameEditState::
                                Edited)),
            "same-shaped user group names should be accepted");
        state = restored.View();
        view = FindGroupingView(state, view_id);
        group =
            view == nullptr
                ? nullptr
                : FindGroup(*view, group_id);
        Require(
            view != nullptr &&
                view->name == "Grouping 7" &&
                view->generated_name.source ==
                    specforge::GeneratedNameSource::None &&
                group != nullptr &&
                group->name == "Group 7" &&
                group->generated_name.source ==
                    specforge::GeneratedNameSource::None,
            "user renames should clear generated-name semantics even when their shape matches a default");

        RequireApplied(
            restored.Submit(
                specforge::CatalogUserStateIntent::
                    RenameUserGroupingView(
                        view_id,
                        "Draft copy",
                        specforge::
                            CatalogUserRenameEditState::
                                Edited)),
            "user names ending in copy should be accepted verbatim");
        RequireApplied(
            restored.Submit(
                specforge::CatalogUserStateIntent::
                    DuplicateGroupingView(view_id)),
            "duplicating a user-named view should apply");
        state = restored.View();
        const specforge::SpectralLineGroupingView*
            duplicated = FindActiveEditableView(state);
        Require(
            duplicated != nullptr &&
                duplicated->id != view_id &&
                duplicated->name == "Draft copy copy" &&
                duplicated->generated_name.source ==
                    specforge::GeneratedNameSource::None &&
                duplicated->generated_name.copy_count == 1 &&
                duplicated->generated_name.copy_base_name ==
                    "Draft copy",
            "generated copies should carry an explicit base name instead of inferring suffixes");
    }

    RemoveTestCache(path);
}

void TestExplicitStoredValueRenamesClearGeneratedMetadata()
{
    const std::filesystem::path path =
        TestCachePath("explicit_stored_value_rename");
    RemoveTestCache(path);

    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    CreateUserGroupingView()),
            "explicit-rename fixture should create a user view");
        specforge::CatalogUserStateView state =
            session.View();
        const specforge::SpectralLineGroupingView* view =
            FindActiveEditableView(state);
        Require(
            view != nullptr,
            "explicit-rename fixture should expose the created view");
        const std::string view_id = view->id;

        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    AddUserGroup(view_id)),
            "explicit-rename fixture should create a group");
        state = session.View();
        view = FindGroupingView(state, view_id);
        const specforge::SpectralLineGroupView* group =
            view == nullptr || view->groups.empty()
                ? nullptr
                : &view->groups.front();
        Require(
            group != nullptr,
            "explicit-rename fixture should expose the created group");
        const std::string group_id = group->id;

        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    RenameUserGroupingView(
                        view_id,
                        "Grouping 1",
                        specforge::
                            CatalogUserRenameEditState::
                                Edited)),
            "an explicit rename to the stored grouping-view text should clear generated-name semantics");
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    RenameUserGroup(
                        view_id,
                        group_id,
                        "Group 1",
                        specforge::
                            CatalogUserRenameEditState::
                                Edited)),
            "an explicit rename to the stored group text should clear generated-name semantics");

        state = session.View();
        view = FindGroupingView(state, view_id);
        group =
            view == nullptr
                ? nullptr
                : FindGroup(*view, group_id);
        Require(
            view != nullptr &&
                view->name == "Grouping 1" &&
                view->generated_name.source ==
                    specforge::GeneratedNameSource::None &&
                group != nullptr &&
                group->name == "Group 1" &&
                group->generated_name.source ==
                    specforge::GeneratedNameSource::None,
            "explicit same-text renames should preserve text while clearing both generated-name markers");
        Require(
            session.Flush(),
            "explicit same-text renames should be persisted");
    }

    {
        specforge::SpectralLinesPanelController restored(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        const specforge::CatalogUserStateView state =
            restored.View();
        const specforge::SpectralLineGroupingView* view =
            FindActiveEditableView(state);
        const specforge::SpectralLineGroupView* group =
            view == nullptr || view->groups.empty()
                ? nullptr
                : &view->groups.front();
        Require(
            view != nullptr &&
                view->generated_name.source ==
                    specforge::GeneratedNameSource::None &&
                group != nullptr &&
                group->generated_name.source ==
                    specforge::GeneratedNameSource::None,
            "cleared generated-name markers should survive the cache round trip");
    }

    RemoveTestCache(path);
}

void TestLegacyExactShapeNamesRemainUserOwnedAcrossRestart()
{
    const std::filesystem::path path =
        TestCachePath("legacy_exact_shape_user_names");
    RemoveTestCache(path);
    WriteLegacyExactShapeUserNames(path);

    const auto require_user_owned_names =
        [](const specforge::CatalogUserStateView& state,
           std::string_view context) {
            const specforge::SpectralLineGroupingView* view =
                FindGroupingView(state, "view-1");
            const specforge::SpectralLineGroupView* group =
                view == nullptr
                    ? nullptr
                    : FindGroup(*view, "group-1");
            Require(
                view != nullptr &&
                    view->name == "Grouping 1" &&
                    view->generated_name.source ==
                        specforge::GeneratedNameSource::None &&
                    group != nullptr &&
                    group->name == "Group 1" &&
                    group->generated_name.source ==
                        specforge::GeneratedNameSource::None,
                context);
        };

    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        require_user_owned_names(
            session.View(),
            "schema-2 exact-shape names must load as user-owned");
    }

    const std::string rewritten = ReadFile(path);
    Require(
        rewritten.find("\"schema_version\": 3") !=
                std::string::npos &&
            rewritten.find("\"name_source\"") ==
                std::string::npos,
        "schema rewrite must not invent generated-name provenance");

    {
        specforge::SpectralLinesPanelController restarted(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        require_user_owned_names(
            restarted.View(),
            "restart must preserve user ownership of exact-shape names");
    }

    RemoveTestCache(path);
}

void TestMalformedLegacyCacheBodyIsNotSilentlyOverwritten()
{
    const auto require_preserved =
        [](const std::filesystem::path& path) {
            const std::string original =
                ReadFile(path);
            {
                specforge::
                    SpectralLinesPanelController
                        session(
                            GroupedCatalog(),
                            specforge::
                                PublicSpectralLineCatalogIdentity(),
                            path);
                Require(
                    session.View()
                            .persistence
                            .load_issue ==
                        specforge::
                            SpectralLineCacheLoadIssueKind::
                                InvalidDocument,
                    "a malformed legacy cache body should expose an invalid-document warning");
            }
            Require(
                ReadFile(path) == original,
                "a malformed legacy cache body must remain byte-identical until an explicit user mutation");
        };

    for (const auto& [test_name, catalogs_json] :
         std::initializer_list<
             std::pair<std::string_view, std::string_view>>{
             {"null_catalogs", "null"},
             {"array_catalogs", "[]"},
             {"non_object_catalog", R"json({"specforge.public": []})json"},
             {"object_grouping_views",
              R"json({"specforge.public": {"active_view_id": "", "marker_visibility": {}, "grouping_views": {}}})json"},
             {"object_groups",
              R"json({"specforge.public": {"active_view_id": "view-1", "marker_visibility": {}, "grouping_views": [{"id": "view-1", "name": "Grouping 1", "groups": {}}]}})json"},
         }) {
        const std::filesystem::path path =
            TestCachePath(test_name);
        RemoveTestCache(path);
        WriteMalformedLegacyCacheBody(
            path,
            catalogs_json);
        require_preserved(path);
        RemoveTestCache(path);
    }

    const std::filesystem::path missing_path =
        TestCachePath("missing_catalogs");
    RemoveTestCache(missing_path);
    {
        std::ofstream stream(
            missing_path,
            std::ios::binary);
        stream << R"json({
  "format_kind": "specforge.catalog_user_state.cache",
  "schema_version": 2,
  "catalog_panel_state": {}
})json";
    }
    require_preserved(missing_path);
    RemoveTestCache(missing_path);
}

void TestModificationSelectionAndPersistenceRoundTrip()
{
    const std::filesystem::path path = TestCachePath("round_trip");
    RemoveTestCache(path);

    std::string user_view_id;
    std::string first_group_id;
    std::string second_group_id;
    std::string first_flush_contents;
    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);

        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::CreateUserGroupingView()),
            "creating a user grouping view should be applied");
        specforge::CatalogUserStateView state = session.View();
        const specforge::SpectralLineGroupingView* user_view = FindActiveEditableView(state);
        Require(user_view != nullptr, "created user grouping view should become active");
        user_view_id = user_view->id;
        const specforge::SpectralLineGroupView* unassigned = FindUnassignedGroup(*user_view);
        Require(unassigned != nullptr, "created user grouping view should have an unassigned group");
        const std::string unassigned_group_id = unassigned->id;

        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::AddUserGroup(user_view_id)),
            "first user group should be added");
        state = session.View();
        user_view = FindGroupingView(state, user_view_id);
        Require(user_view != nullptr, "user grouping view should still exist after adding a group");
        first_group_id = user_view->groups.front().id;

        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::MoveMarkerReference(
                user_view_id,
                "h_alpha",
                unassigned_group_id,
                first_group_id)),
            "marker reference should move from unassigned into the first user group");
        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::CopyMarkerReference(
                user_view_id,
                "h_beta",
                unassigned_group_id,
                first_group_id)),
            "copying from unassigned should organize the marker without persisting a stale unassigned reference");
        state = session.View();
        user_view = FindGroupingView(state, user_view_id);
        unassigned = user_view == nullptr ? nullptr : FindUnassignedGroup(*user_view);
        const specforge::SpectralLineGroupView* first_group =
            user_view == nullptr ? nullptr : FindGroup(*user_view, first_group_id);
        Require(
            unassigned != nullptr && FindMarker(*unassigned, "h_beta") == nullptr,
            "a marker copied into an ordinary group must leave the derived unassigned group");
        Require(
            first_group != nullptr && FindMarker(*first_group, "h_beta") != nullptr,
            "the ordinary group should contain the marker copied from unassigned");
        RequireRejected(
            session.Submit(specforge::CatalogUserStateIntent::CopyMarkerReference(
                user_view_id,
                "h_alpha",
                unassigned_group_id,
                first_group_id)),
            "copy should reject a source group that no longer owns the marker reference");

        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::AddUserGroup(user_view_id)),
            "second user group should be added");
        state = session.View();
        user_view = FindGroupingView(state, user_view_id);
        Require(user_view != nullptr && user_view->groups.size() >= 3, "two ordinary groups should be visible");
        second_group_id = user_view->groups[1].id;

        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::CopyMarkerReference(
                user_view_id,
                "h_alpha",
                first_group_id,
                second_group_id)),
            "marker reference should copy between owned user groups");
        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::RenameUserGroupingView(
                user_view_id,
                "Balmer review",
                specforge::CatalogUserRenameEditState::Edited)),
            "user grouping view rename should be applied");
        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::RenameUserGroup(
                user_view_id,
                first_group_id,
                "Hydrogen",
                specforge::CatalogUserRenameEditState::Edited)),
            "user group rename should be applied");
        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::SetGroupExpanded(
                user_view_id,
                first_group_id,
                true)),
            "expanded state should be persisted by the Module");
        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::SetGroupMarkerVisibility(
                user_view_id,
                first_group_id,
                false)),
            "group visibility should update shared marker visibility");

        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::SetGroupingViewSearch("alpha")),
            "grouping view search should update the read view");
        RequireRejected(
            session.Submit(specforge::CatalogUserStateIntent::SetGroupMarkerVisibility(
                user_view_id,
                first_group_id,
                true)),
            "bulk group visibility should be rejected while grouping view search is active");
        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::SetGroupingViewSearch("")),
            "clearing grouping view search should be applied");

        const std::string catalog_view_id = specforge::CatalogGroupingViewId();
        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::SelectGroupingView(catalog_view_id)),
            "catalog grouping view selection should be applied");
        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::SelectGroupingView(user_view_id)),
            "user grouping view should be selected before persistence");
        Require(session.Flush(), "modified catalog user state should flush successfully");
        Require(std::filesystem::exists(path), "modified catalog user state should create a cache file");
        first_flush_contents = ReadFile(path);
        Require(!first_flush_contents.empty(), "first flush should write a non-empty cache file");
    }

    {
        specforge::SpectralLinesPanelController restored(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        const specforge::CatalogUserStateView state = restored.View();
        const specforge::SpectralLineGroupingView* user_view = FindGroupingView(state, user_view_id);
        Require(user_view != nullptr, "persisted user grouping view should be restored");
        Require(user_view->active, "persisted active grouping view selection should be restored");
        Require(user_view->name == "Balmer review", "persisted grouping view name should be restored");

        const specforge::SpectralLineGroupView* first_group = FindGroup(*user_view, first_group_id);
        const specforge::SpectralLineGroupView* second_group = FindGroup(*user_view, second_group_id);
        Require(first_group != nullptr && second_group != nullptr, "persisted user groups should be restored");
        Require(first_group->name == "Hydrogen", "persisted user group name should be restored");
        Require(first_group->expanded, "persisted expanded state should be restored");

        const specforge::SpectralLineMarkerReferenceView* first_marker = FindMarker(*first_group, "h_alpha");
        const specforge::SpectralLineMarkerReferenceView* second_marker = FindMarker(*second_group, "h_alpha");
        Require(first_marker != nullptr && second_marker != nullptr, "shared marker references should be restored");
        Require(first_marker->shared && second_marker->shared, "restored marker references should remain shared");
        Require(!first_marker->visible && !second_marker->visible, "shared marker visibility should be restored");
        Require(restored.Flush(), "restored clean session should flush successfully");
        Require(
            ReadFile(path) == first_flush_contents,
            "restoring and flushing should not rewrite a non-canonical first save");
    }

    RemoveTestCache(path);
}

void TestPersistenceViewReportsLoadWarningRetryAndRecovery()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge_catalog_user_state_health";
    const std::filesystem::path blocker =
        root / "not-a-directory";
    const std::filesystem::path path =
        blocker / "catalog-user-state.json";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    const std::filesystem::path warning_path =
        root / "corrupt-catalog-user-state.json";
    {
        std::ofstream stream(warning_path);
        stream << "{ invalid json";
    }
    {
        specforge::SpectralLinesPanelController warned(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            warning_path);
        Require(
            warned.View().persistence.load_issue ==
                    specforge::SpectralLineCacheLoadIssueKind::
                        InvalidDocument &&
                !warned.View()
                     .persistence
                     .load_diagnostic_detail.empty(),
            "spectral-line view should expose a typed cache load issue and parser detail");
    }
    specforge::SpectralLinesPanelController session(
        GroupedCatalog(),
        specforge::PublicSpectralLineCatalogIdentity(),
        path);
    RequireApplied(
        session.Submit(
            specforge::CatalogUserStateIntent::
                SetMarkerVisibility(
                    "h_alpha",
                    false)),
        "spectral-line state mutation should become dirty");
    Require(
        session.Flush(),
        "the recovery fixture should establish an initial cache");
    std::filesystem::remove_all(blocker, error);
    {
        std::ofstream stream(blocker);
        stream << "block cache directory creation";
    }
    RequireApplied(
        session.Submit(
            specforge::CatalogUserStateIntent::
                SetMarkerVisibility(
                    "h_alpha",
                    true)),
        "a second spectral-line mutation should become dirty");
    Require(
        !session.Flush(),
        "blocked spectral-line cache path should fail to flush");
    Require(
        session.View().persistence.retrying &&
            !session.View()
                 .persistence
                 .save_diagnostic_detail.empty() &&
            session.View()
                    .persistence
                    .save_diagnostic_detail.find(
                        "Could not save spectral-line grouping cache:") ==
                std::string::npos,
        "spectral-line view should expose retry state and raw diagnostics without an English application prefix");

    std::filesystem::remove(blocker);
    std::filesystem::create_directories(blocker);
    Require(
        session.Flush(),
        "spectral-line cache should retry after repairing its path");
    Require(
        session.View().persistence.recovered,
        "spectral-line view should expose successful recovery");

    RequireApplied(
        session.Submit(
            specforge::CatalogUserStateIntent::
                SetMarkerVisibility(
                    "h_alpha",
                    false)),
        "a later spectral-line mutation should apply");
    Require(
        !session.View().persistence.recovered,
        "a later spectral-line mutation should clear recovery");
    Require(
        session.Flush(),
        "the final spectral-line state should flush");

    std::filesystem::remove_all(root, error);
}

}  // namespace

int main()
{
    try {
        TestForeignIdentitiesAreRejectedWithoutPersistence();
        TestPlotViewProjectsOnlyPlotOverlayState();
        TestCacheLoadUsesDomainCanonicalizationAndPreservesUnresolvedMarkers();
        TestPersistentIntentUsesDomainCanonicalizationForSelection();
        TestGeneratedNamesPreserveStoredValuesAndOrigins();
        TestExplicitStoredValueRenamesClearGeneratedMetadata();
        TestLegacyExactShapeNamesRemainUserOwnedAcrossRestart();
        TestMalformedLegacyCacheBodyIsNotSilentlyOverwritten();
        TestModificationSelectionAndPersistenceRoundTrip();
        TestPersistenceViewReportsLoadWarningRetryAndRecovery();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
