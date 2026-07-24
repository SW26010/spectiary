#include "ui/spectral_lines_panel_controller.h"

#include "overlays/spectral_line_user_state_cache_io.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <utility>

namespace specforge {
namespace {

using namespace std::chrono_literals;

constexpr auto kSaveDebounce = 500ms;
constexpr auto kSaveRetry = 10s;

std::string TrimWhitespace(std::string_view value)
{
    const auto is_space = [](unsigned char character) {
        return std::isspace(character) != 0;
    };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::string MarkerWavelengthText(const SpectralLineMarker& marker)
{
    std::array<char, 64> buffer = {};
    if (marker.kind == SpectralLineMarkerKind::Line && marker.vacuum_angstrom) {
        std::snprintf(buffer.data(), buffer.size(), "%.3f", *marker.vacuum_angstrom);
        return buffer.data();
    }
    if (marker.kind == SpectralLineMarkerKind::Band && marker.start_vacuum_angstrom &&
        marker.end_vacuum_angstrom) {
        std::snprintf(
            buffer.data(),
            buffer.size(),
            "%.3f-%.3f",
            *marker.start_vacuum_angstrom,
            *marker.end_vacuum_angstrom);
        return buffer.data();
    }
    return {};
}

bool GroupContainsMarker(
    const UserGroup& group,
    const CatalogIdentity& identity,
    std::string_view marker_id)
{
    return std::any_of(group.marker_references.begin(), group.marker_references.end(), [&](const auto& reference) {
        return SameCatalogIdentity(reference.catalog_identity, identity) && reference.marker_id == marker_id;
    });
}

}  // namespace

CatalogUserStateIntent::CatalogUserStateIntent(Kind kind) : kind_(kind) {}

CatalogUserStateIntent CatalogUserStateIntent::SetGroupingViewSearch(std::string query)
{
    CatalogUserStateIntent intent(Kind::SetGroupingViewSearch);
    intent.text_ = std::move(query);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::SetMarkerLabelsVisible(bool visible)
{
    CatalogUserStateIntent intent(Kind::SetMarkerLabelsVisible);
    intent.enabled_ = visible;
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::SelectGroupingView(std::string view_id)
{
    CatalogUserStateIntent intent(Kind::SelectGroupingView);
    intent.view_id_ = std::move(view_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::AcknowledgeGroupingViewSelection(std::string view_id)
{
    CatalogUserStateIntent intent(Kind::AcknowledgeGroupingViewSelection);
    intent.view_id_ = std::move(view_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::CreateUserGroupingView()
{
    return CatalogUserStateIntent(Kind::CreateUserGroupingView);
}

CatalogUserStateIntent CatalogUserStateIntent::DuplicateGroupingView(std::string view_id)
{
    CatalogUserStateIntent intent(Kind::DuplicateGroupingView);
    intent.view_id_ = std::move(view_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::RenameUserGroupingView(
    std::string view_id,
    std::string name)
{
    CatalogUserStateIntent intent(Kind::RenameUserGroupingView);
    intent.view_id_ = std::move(view_id);
    intent.text_ = std::move(name);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::DeleteUserGroupingView(std::string view_id)
{
    CatalogUserStateIntent intent(Kind::DeleteUserGroupingView);
    intent.view_id_ = std::move(view_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::AddUserGroup(std::string view_id)
{
    CatalogUserStateIntent intent(Kind::AddUserGroup);
    intent.view_id_ = std::move(view_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::MoveMarkerReferenceToNewGroup(
    std::string view_id,
    std::string marker_id,
    std::string source_group_id)
{
    CatalogUserStateIntent intent(Kind::MoveMarkerReferenceToNewGroup);
    intent.view_id_ = std::move(view_id);
    intent.marker_id_ = std::move(marker_id);
    intent.source_group_id_ = std::move(source_group_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::CopyMarkerReferenceToNewGroup(
    std::string view_id,
    std::string marker_id,
    std::string source_group_id)
{
    CatalogUserStateIntent intent(Kind::CopyMarkerReferenceToNewGroup);
    intent.view_id_ = std::move(view_id);
    intent.marker_id_ = std::move(marker_id);
    intent.source_group_id_ = std::move(source_group_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::RenameUserGroup(
    std::string view_id,
    std::string group_id,
    std::string name)
{
    CatalogUserStateIntent intent(Kind::RenameUserGroup);
    intent.view_id_ = std::move(view_id);
    intent.group_id_ = std::move(group_id);
    intent.text_ = std::move(name);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::DeleteUserGroup(std::string view_id, std::string group_id)
{
    CatalogUserStateIntent intent(Kind::DeleteUserGroup);
    intent.view_id_ = std::move(view_id);
    intent.group_id_ = std::move(group_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::SetGroupMarkerVisibility(
    std::string view_id,
    std::string group_id,
    bool visible)
{
    CatalogUserStateIntent intent(Kind::SetGroupMarkerVisibility);
    intent.view_id_ = std::move(view_id);
    intent.group_id_ = std::move(group_id);
    intent.enabled_ = visible;
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::SetGroupExpanded(
    std::string view_id,
    std::string group_id,
    bool expanded)
{
    CatalogUserStateIntent intent(Kind::SetGroupExpanded);
    intent.view_id_ = std::move(view_id);
    intent.group_id_ = std::move(group_id);
    intent.enabled_ = expanded;
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::ReorderUserGroupBefore(
    std::string view_id,
    std::string source_group_id,
    std::string target_group_id)
{
    CatalogUserStateIntent intent(Kind::ReorderUserGroupBefore);
    intent.view_id_ = std::move(view_id);
    intent.source_group_id_ = std::move(source_group_id);
    intent.target_group_id_ = std::move(target_group_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::MoveMarkerReference(
    std::string view_id,
    std::string marker_id,
    std::string source_group_id,
    std::string target_group_id)
{
    CatalogUserStateIntent intent(Kind::MoveMarkerReference);
    intent.view_id_ = std::move(view_id);
    intent.marker_id_ = std::move(marker_id);
    intent.source_group_id_ = std::move(source_group_id);
    intent.target_group_id_ = std::move(target_group_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::CopyMarkerReference(
    std::string view_id,
    std::string marker_id,
    std::string source_group_id,
    std::string target_group_id)
{
    CatalogUserStateIntent intent(Kind::CopyMarkerReference);
    intent.view_id_ = std::move(view_id);
    intent.marker_id_ = std::move(marker_id);
    intent.source_group_id_ = std::move(source_group_id);
    intent.target_group_id_ = std::move(target_group_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::RemoveMarkerReference(
    std::string view_id,
    std::string marker_id,
    std::string group_id)
{
    CatalogUserStateIntent intent(Kind::RemoveMarkerReference);
    intent.view_id_ = std::move(view_id);
    intent.marker_id_ = std::move(marker_id);
    intent.group_id_ = std::move(group_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::SetMarkerVisibility(std::string marker_id, bool visible)
{
    CatalogUserStateIntent intent(Kind::SetMarkerVisibility);
    intent.marker_id_ = std::move(marker_id);
    intent.enabled_ = visible;
    return intent;
}

SpectralLinesPanelController::SpectralLinesPanelController(
    std::filesystem::path packaged_catalog_path)
    : SpectralLinesPanelController(
          LoadPackagedPublicSpectralLineCatalog(packaged_catalog_path),
          PublicSpectralLineCatalogIdentity(),
          DefaultCatalogUserStateCachePath())
{
}

SpectralLinesPanelController::SpectralLinesPanelController(
    SpectralLineCatalog catalog,
    CatalogIdentity catalog_identity,
    std::filesystem::path user_state_cache_path)
    : catalog_(std::move(catalog)),
      catalog_identity_(std::move(catalog_identity)),
      catalog_grouping_view_(BuildCatalogGroupingView(catalog_, catalog_identity_)),
      user_state_cache_path_(std::move(user_state_cache_path)),
      cache_save_scheduler_(kSaveDebounce, kSaveRetry)
{
    CatalogUserStateCacheLoadResult load_result = LoadCatalogUserStateCache(user_state_cache_path_);
    user_state_cache_ = std::move(load_result.cache);
    load_warning_ = std::move(load_result.warning);
    user_state_ = EnsureCatalogUserState(user_state_cache_, catalog_identity_);
    panel_state_ = EnsureCatalogPanelState(user_state_cache_, catalog_identity_);
    const CatalogUserStateCanonicalizationResult canonicalization =
        CanonicalizeCatalogUserState(
            user_state_,
            panel_state_,
            catalog_,
            catalog_identity_,
            catalog_grouping_view_);
    if (canonicalization.active_view_changed) {
        RequestGroupingViewSelection();
    }
    if (canonicalization.changed) {
        MarkCacheDirty();
    }
    next_view_index_ = static_cast<int>(user_state_.grouping_views.size()) + 1;
    for (const GroupingView& view : user_state_.grouping_views) {
        next_group_index_ += static_cast<int>(view.groups.size());
    }
}

SpectralLinesPanelController::~SpectralLinesPanelController()
{
    (void)Flush();
}

CatalogUserStateResult SpectralLinesPanelController::Submit(CatalogUserStateIntent intent)
{
    switch (intent.kind_) {
    case CatalogUserStateIntent::Kind::SetGroupingViewSearch:
        if (grouping_view_search_ == intent.text_) {
            return NoChange();
        }
        grouping_view_search_ = std::move(intent.text_);
        return Applied(false);

    case CatalogUserStateIntent::Kind::SetMarkerLabelsVisible:
        if (marker_labels_visible_ == intent.enabled_) {
            return NoChange();
        }
        marker_labels_visible_ = intent.enabled_;
        return Applied(false);

    case CatalogUserStateIntent::Kind::SelectGroupingView:
        if (!ViewExists(intent.view_id_)) {
            return Rejected("Grouping view identity does not belong to this catalog user state.");
        }
        if (user_state_.active_view_id == intent.view_id_) {
            return NoChange();
        }
        user_state_.active_view_id = std::move(intent.view_id_);
        return Applied(true);

    case CatalogUserStateIntent::Kind::AcknowledgeGroupingViewSelection:
        if (!grouping_view_selection_requested_ || user_state_.active_view_id != intent.view_id_) {
            return NoChange();
        }
        grouping_view_selection_requested_ = false;
        return Applied(false);

    case CatalogUserStateIntent::Kind::CreateUserGroupingView: {
        const std::string id = NextGroupingViewId();
        const std::string name = "Grouping " + std::to_string(user_state_.grouping_views.size() + 1);
        user_state_.grouping_views.push_back(
            CreateUserGroupingViewFromCatalog(catalog_, catalog_identity_, id, name));
        user_state_.active_view_id = id;
        RequestGroupingViewSelection();
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::DuplicateGroupingView: {
        std::optional<GroupingView> source = EffectiveGroupingView(intent.view_id_);
        if (!source) {
            return Rejected("Grouping view identity does not belong to this catalog user state.");
        }
        const std::string id = NextGroupingViewId();
        const std::string name = source->name + " copy";
        user_state_.grouping_views.push_back(
            DuplicateGroupingView(*source, catalog_, catalog_identity_, id, name));
        user_state_.active_view_id = id;
        RequestGroupingViewSelection();
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::RenameUserGroupingView: {
        GroupingView* view = FindUserGroupingView(intent.view_id_);
        const std::string name = TrimWhitespace(intent.text_);
        if (view == nullptr || name.empty()) {
            return Rejected("Editable grouping view identity and a non-empty name are required.");
        }
        if (view->name == name) {
            return NoChange();
        }
        view->name = name;
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::DeleteUserGroupingView: {
        const auto match = std::find_if(
            user_state_.grouping_views.begin(),
            user_state_.grouping_views.end(),
            [&](const GroupingView& view) {
                return view.id == intent.view_id_;
            });
        if (match == user_state_.grouping_views.end()) {
            return Rejected("Editable grouping view identity does not belong to this catalog user state.");
        }
        const std::string deleted_view_id = match->id;
        user_state_.grouping_views.erase(match);
        for (auto iterator = panel_state_.expanded_group_ids.begin();
             iterator != panel_state_.expanded_group_ids.end();) {
            if (iterator->starts_with(deleted_view_id + "/")) {
                iterator = panel_state_.expanded_group_ids.erase(iterator);
            } else {
                ++iterator;
            }
        }
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::AddUserGroup: {
        GroupingView* view = FindUserGroupingView(intent.view_id_);
        if (view == nullptr) {
            return Rejected("Editable grouping view identity does not belong to this catalog user state.");
        }
        const std::string group_id = NextUserGroupId();
        const std::string group_name = "Group " + group_id.substr(std::string("group-").size());
        if (!AddUserGroup(*view, group_id, group_name)) {
            return Rejected("The user group could not be added without violating grouping view invariants.");
        }
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::MoveMarkerReferenceToNewGroup:
    case CatalogUserStateIntent::Kind::CopyMarkerReferenceToNewGroup: {
        GroupingView* view = FindUserGroupingView(intent.view_id_);
        if (view == nullptr) {
            return Rejected("Editable grouping view identity does not belong to this catalog user state.");
        }
        const UserGroup* source_group = FindUserGroup(*view, intent.source_group_id_);
        if (source_group == nullptr ||
            !GroupContainsMarker(*source_group, catalog_identity_, intent.marker_id_)) {
            return Rejected("Source group and marker reference identities do not belong to the grouping view.");
        }
        const bool copy = intent.kind_ == CatalogUserStateIntent::Kind::CopyMarkerReferenceToNewGroup;
        if (!copy && intent.source_group_id_ != UnassignedUserGroupId()) {
            return Rejected("Only an unassigned marker reference can be moved directly into a new group.");
        }

        const std::string group_id = NextUserGroupId();
        const std::string group_name = "Group " + group_id.substr(std::string("group-").size());
        if (!AddUserGroup(*view, group_id, group_name)) {
            return Rejected("The user group could not be added without violating grouping view invariants.");
        }
        const bool changed = copy
                                 ? specforge::CopyMarkerReference(
                                       *view,
                                       catalog_identity_,
                                       intent.marker_id_,
                                       group_id)
                                 : MoveMarkerReference(
                                       *view,
                                       catalog_identity_,
                                       intent.marker_id_,
                                       intent.source_group_id_,
                                       group_id);
        if (!changed) {
            (void)RemoveUserGroup(*view, group_id);
            return Rejected("The marker reference could not be placed in the new group.");
        }
        panel_state_.expanded_group_ids.insert(GroupExpansionKey(view->id, group_id));
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::RenameUserGroup: {
        GroupingView* view = FindUserGroupingView(intent.view_id_);
        UserGroup* group = view == nullptr ? nullptr : FindUserGroup(*view, intent.group_id_);
        const std::string name = TrimWhitespace(intent.text_);
        if (group == nullptr || group->is_unassigned || group->id == UnassignedUserGroupId() || name.empty()) {
            return Rejected("Editable user group identities and a non-empty name are required.");
        }
        if (group->name == name) {
            return NoChange();
        }
        group->name = name;
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::DeleteUserGroup: {
        GroupingView* view = FindUserGroupingView(intent.view_id_);
        if (view == nullptr || !RemoveUserGroup(*view, intent.group_id_)) {
            return Rejected("Editable user group identity does not belong to the grouping view.");
        }
        panel_state_.expanded_group_ids.erase(GroupExpansionKey(view->id, intent.group_id_));
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::SetGroupMarkerVisibility: {
        std::optional<GroupingView> view = EffectiveGroupingView(intent.view_id_);
        const UserGroup* group = view ? FindUserGroup(*view, intent.group_id_) : nullptr;
        if (group == nullptr) {
            return Rejected("Grouping view and group identities do not belong to this catalog user state.");
        }
        if (!grouping_view_search_.empty()) {
            return Rejected("Group marker visibility cannot be changed while grouping view search is active.");
        }
        if (!SetGroupMarkerVisibility(
                user_state_,
                *group,
                catalog_,
                catalog_identity_,
                intent.enabled_,
                false)) {
            return NoChange();
        }
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::SetGroupExpanded: {
        std::optional<GroupingView> view = EffectiveGroupingView(intent.view_id_);
        if (!view || FindUserGroup(*view, intent.group_id_) == nullptr) {
            return Rejected("Grouping view and group identities do not belong to this catalog user state.");
        }
        const std::string key = GroupExpansionKey(intent.view_id_, intent.group_id_);
        const bool expanded = panel_state_.expanded_group_ids.contains(key);
        if (expanded == intent.enabled_) {
            return NoChange();
        }
        if (intent.enabled_) {
            panel_state_.expanded_group_ids.insert(key);
        } else {
            panel_state_.expanded_group_ids.erase(key);
        }
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::ReorderUserGroupBefore: {
        GroupingView* view = FindUserGroupingView(intent.view_id_);
        if (view == nullptr ||
            !ReorderUserGroupBefore(*view, intent.source_group_id_, intent.target_group_id_)) {
            return Rejected("User group identities cannot be reordered in this grouping view.");
        }
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::MoveMarkerReference:
    case CatalogUserStateIntent::Kind::CopyMarkerReference: {
        GroupingView* view = FindUserGroupingView(intent.view_id_);
        const UserGroup* source_group = view == nullptr ? nullptr : FindUserGroup(*view, intent.source_group_id_);
        if (view == nullptr || source_group == nullptr ||
            !GroupContainsMarker(*source_group, catalog_identity_, intent.marker_id_)) {
            return Rejected("Source group and marker reference identities do not belong to the grouping view.");
        }
        const bool copy = intent.kind_ == CatalogUserStateIntent::Kind::CopyMarkerReference;
        const bool changed = copy
                                 ? specforge::CopyMarkerReference(
                                       *view,
                                       catalog_identity_,
                                       intent.marker_id_,
                                       intent.target_group_id_)
                                 : MoveMarkerReference(
                                       *view,
                                       catalog_identity_,
                                       intent.marker_id_,
                                       intent.source_group_id_,
                                       intent.target_group_id_);
        if (!changed) {
            return Rejected("Marker reference identities cannot be moved or copied to the target group.");
        }
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::RemoveMarkerReference: {
        GroupingView* view = FindUserGroupingView(intent.view_id_);
        if (view == nullptr || !specforge::RemoveMarkerReferenceFromGroup(
                                   *view,
                                   catalog_identity_,
                                   intent.marker_id_,
                                   intent.group_id_)) {
            return Rejected("Marker reference and group identities do not belong to the editable grouping view.");
        }
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::SetMarkerVisibility:
        if (!MarkerExists(intent.marker_id_)) {
            return Rejected("Marker identity does not belong to the current spectral-line catalog.");
        }
        if (IsMarkerVisible(user_state_, intent.marker_id_) == intent.enabled_) {
            return NoChange();
        }
        SetMarkerVisible(user_state_, intent.marker_id_, intent.enabled_);
        return Applied(true);
    }

    return Rejected("Unknown catalog user state intent.");
}

CatalogUserStateView SpectralLinesPanelController::View() const
{
    CatalogUserStateView result;
    result.catalog_id = catalog_identity_.id;
    result.catalog_display_name = catalog_identity_.display_name;
    result.catalog_load_error = catalog_.load_error;
    result.warning = cache_save_status_.failed() ? cache_save_status_.message() : load_warning_;
    result.grouping_view_search = grouping_view_search_;
    result.marker_labels_visible = marker_labels_visible_;
    result.has_catalog_grouping_view = catalog_grouping_view_.has_value();
    result.user_grouping_view_count = user_state_.grouping_views.size();
    result.catalog_marker_count = catalog_.markers.size();
    result.grouping_views.reserve(user_state_.grouping_views.size() + (catalog_grouping_view_ ? 1U : 0U));

    const auto append_view = [&](const GroupingView& view, bool editable) {
        SpectralLineGroupingView grouping_view;
        grouping_view.id = view.id;
        grouping_view.name = view.name;
        grouping_view.editable = editable;
        grouping_view.active = user_state_.active_view_id == view.id;
        grouping_view.selection_requested = grouping_view.active && grouping_view_selection_requested_;
        grouping_view.search_active = !grouping_view_search_.empty();
        grouping_view.groups.reserve(view.groups.size());

        const std::unordered_map<std::string, int> reference_counts =
            MarkerReferenceCounts(view, catalog_identity_);
        for (const UserGroup& group : view.groups) {
            SpectralLineGroupView group_view;
            group_view.id = group.id;
            group_view.name = group.name;
            group_view.is_unassigned = group.is_unassigned || group.id == UnassignedUserGroupId();
            group_view.expanded =
                panel_state_.expanded_group_ids.contains(GroupExpansionKey(view.id, group.id));
            group_view.visibility = VisibilityStateForGroup(
                user_state_,
                group,
                catalog_,
                catalog_identity_,
                grouping_view.search_active);

            for (const MarkerReference& reference : group.marker_references) {
                if (!MarkerMatchesSearch(catalog_, catalog_identity_, reference, grouping_view_search_)) {
                    continue;
                }
                const SpectralLineMarker* marker = FindCatalogMarker(catalog_, catalog_identity_, reference);
                SpectralLineMarkerReferenceView marker_view;
                marker_view.marker_id = reference.marker_id;
                marker_view.resolved = marker != nullptr;
                marker_view.visible = marker != nullptr && IsMarkerVisible(user_state_, reference.marker_id);
                marker_view.shared = IsSharedMarkerReference(reference_counts, reference);
                if (marker != nullptr) {
                    marker_view.label = marker->label;
                    marker_view.wavelength_text = MarkerWavelengthText(*marker);
                    marker_view.notes = marker->notes;
                } else {
                    marker_view.label = reference.marker_id;
                    marker_view.wavelength_text = "unresolved";
                }
                group_view.marker_references.push_back(std::move(marker_view));
            }
            group_view.dimmed_by_search =
                grouping_view.search_active && group_view.marker_references.empty();
            grouping_view.groups.push_back(std::move(group_view));
        }
        result.grouping_views.push_back(std::move(grouping_view));
    };

    if (catalog_grouping_view_) {
        append_view(*catalog_grouping_view_, false);
    }
    for (const GroupingView& stored_view : user_state_.grouping_views) {
        append_view(stored_view, true);
    }
    return result;
}

SpectralLinePlotView SpectralLinesPanelController::PlotView(
    const SpectrumSnapshotHandle& snapshot) const
{
    SpectralLinePlotView result;
    result.marker_labels_visible = marker_labels_visible_;
    result.layout_scope_id = catalog_identity_.id;
    if (!snapshot || !snapshot->capabilities.can_show_spectral_lines || catalog_.markers.empty()) {
        return result;
    }
    result.visible_markers.reserve(catalog_.markers.size());
    for (const SpectralLineMarker& marker : catalog_.markers) {
        if (IsMarkerVisible(user_state_, marker.id)) {
            result.visible_markers.push_back(&marker);
        }
    }
    return result;
}

void SpectralLinesPanelController::RunMaintenance(LocalUserStateSaveScheduler::TimePoint now)
{
    if (cache_save_scheduler_.ShouldAttemptSave(now)) {
        (void)Flush();
    }
}

std::optional<LocalUserStateSaveScheduler::TimePoint>
SpectralLinesPanelController::NextMaintenanceDeadline() const
{
    return cache_save_scheduler_.next_attempt_time();
}

bool SpectralLinesPanelController::Flush()
{
    if (!cache_save_scheduler_.dirty()) {
        return true;
    }
    user_state_cache_.catalogs[catalog_identity_.id] = user_state_;
    user_state_cache_.catalog_panel_state[catalog_identity_.id] = panel_state_;
    std::string error;
    if (!SaveCatalogUserStateCache(user_state_cache_path_, user_state_cache_, error)) {
        cache_save_scheduler_.MarkSaveFailed(
            cache_save_status_,
            "Could not save spectral-line grouping cache: " + error);
        return false;
    }
    load_warning_.clear();
    cache_save_scheduler_.MarkSaveSucceeded(cache_save_status_);
    return true;
}

CatalogUserStateResult SpectralLinesPanelController::Applied(bool persistent_state_changed)
{
    if (persistent_state_changed) {
        const CatalogUserStateCanonicalizationResult canonicalization =
            CanonicalizeCatalogUserState(
                user_state_,
                panel_state_,
                catalog_,
                catalog_identity_,
                catalog_grouping_view_);
        if (canonicalization.active_view_changed) {
            RequestGroupingViewSelection();
        }
        MarkCacheDirty();
    }
    CatalogUserStateResult result;
    result.status = CatalogUserStateResultStatus::Applied;
    result.changed = true;
    result.persistent_state_changed = persistent_state_changed;
    return result;
}

CatalogUserStateResult SpectralLinesPanelController::NoChange()
{
    CatalogUserStateResult result;
    result.status = CatalogUserStateResultStatus::NoChange;
    return result;
}

CatalogUserStateResult SpectralLinesPanelController::Rejected(std::string message)
{
    CatalogUserStateResult result;
    result.status = CatalogUserStateResultStatus::Rejected;
    result.message = std::move(message);
    return result;
}

bool SpectralLinesPanelController::ViewExists(std::string_view view_id) const
{
    if (view_id.empty()) {
        return false;
    }
    if (catalog_grouping_view_ && catalog_grouping_view_->id == view_id) {
        return true;
    }
    return FindUserGroupingView(view_id) != nullptr;
}

bool SpectralLinesPanelController::MarkerExists(std::string_view marker_id) const
{
    return !marker_id.empty() &&
           std::any_of(catalog_.markers.begin(), catalog_.markers.end(), [&](const SpectralLineMarker& marker) {
               return marker.id == marker_id;
           });
}

GroupingView* SpectralLinesPanelController::FindUserGroupingView(std::string_view view_id)
{
    const auto match = std::find_if(
        user_state_.grouping_views.begin(),
        user_state_.grouping_views.end(),
        [&](const GroupingView& view) {
            return view.id == view_id;
        });
    return match == user_state_.grouping_views.end() ? nullptr : &(*match);
}

const GroupingView* SpectralLinesPanelController::FindUserGroupingView(std::string_view view_id) const
{
    const auto match = std::find_if(
        user_state_.grouping_views.begin(),
        user_state_.grouping_views.end(),
        [&](const GroupingView& view) {
            return view.id == view_id;
        });
    return match == user_state_.grouping_views.end() ? nullptr : &(*match);
}

std::optional<GroupingView> SpectralLinesPanelController::EffectiveGroupingView(std::string_view view_id) const
{
    if (catalog_grouping_view_ && catalog_grouping_view_->id == view_id) {
        return catalog_grouping_view_;
    }
    const GroupingView* view = FindUserGroupingView(view_id);
    if (view == nullptr) {
        return std::nullopt;
    }
    return EffectiveUserGroupingView(*view, catalog_, catalog_identity_);
}

UserGroup* SpectralLinesPanelController::FindUserGroup(GroupingView& view, std::string_view group_id)
{
    const auto match = std::find_if(view.groups.begin(), view.groups.end(), [&](const UserGroup& group) {
        return group.id == group_id;
    });
    return match == view.groups.end() ? nullptr : &(*match);
}

const UserGroup* SpectralLinesPanelController::FindUserGroup(
    const GroupingView& view,
    std::string_view group_id)
{
    const auto match = std::find_if(view.groups.begin(), view.groups.end(), [&](const UserGroup& group) {
        return group.id == group_id;
    });
    return match == view.groups.end() ? nullptr : &(*match);
}

std::string SpectralLinesPanelController::NextGroupingViewId()
{
    for (;;) {
        std::string id = "view-" + std::to_string(next_view_index_++);
        if (!ViewExists(id) && id != CatalogGroupingViewId()) {
            return id;
        }
    }
}

std::string SpectralLinesPanelController::NextUserGroupId()
{
    for (;;) {
        std::string id = "group-" + std::to_string(next_group_index_++);
        bool exists = false;
        for (const GroupingView& view : user_state_.grouping_views) {
            exists = exists || FindUserGroup(view, id) != nullptr;
        }
        if (!exists && id != UnassignedUserGroupId()) {
            return id;
        }
    }
}

void SpectralLinesPanelController::MarkCacheDirty()
{
    cache_save_scheduler_.MarkDirty();
}

void SpectralLinesPanelController::RequestGroupingViewSelection()
{
    grouping_view_selection_requested_ = true;
}

}  // namespace specforge
