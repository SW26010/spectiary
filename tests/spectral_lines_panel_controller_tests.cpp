#include "ui/spectral_lines_panel_controller.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
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
                "Balmer review")),
            "user grouping view rename should be applied");
        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::RenameUserGroup(
                user_view_id,
                first_group_id,
                "Hydrogen")),
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

}  // namespace

int main()
{
    TestForeignIdentitiesAreRejectedWithoutPersistence();
    TestModificationSelectionAndPersistenceRoundTrip();
    return 0;
}
