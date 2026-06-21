#include "ui/spectral_lines_panel_controller.h"

#include "overlays/spectral_line_user_state_cache_io.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace specforge {
namespace {

constexpr std::uint64_t kSaveDebounceFrames = 30;
constexpr std::uint64_t kSaveRetryFrames = 600;

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

}  // namespace

SpectralLinesPanelController::SpectralLinesPanelController()
    : catalog_(LoadDefaultSpectralLineCatalog()),
      catalog_identity_(PublicSpectralLineCatalogIdentity()),
      catalog_grouping_view_(BuildCatalogGroupingView(catalog_, catalog_identity_)),
      user_state_cache_path_(DefaultCatalogUserStateCachePath()),
      cache_save_scheduler_(kSaveDebounceFrames, kSaveRetryFrames)
{
    CatalogUserStateCacheLoadResult load_result = LoadCatalogUserStateCache(user_state_cache_path_);
    user_state_cache_ = std::move(load_result.cache);
    warning_ = std::move(load_result.warning);
    user_state_ = EnsureCatalogUserState(user_state_cache_, catalog_identity_);
    panel_state_ = EnsureCatalogPanelState(user_state_cache_, catalog_identity_);
    next_view_index_ = static_cast<int>(user_state_.grouping_views.size()) + 1;
    for (const GroupingView& view : user_state_.grouping_views) {
        next_group_index_ += static_cast<int>(view.groups.size());
    }
    NormalizeViewSelection();
}

SpectralLinesPanelController::~SpectralLinesPanelController()
{
    FlushCache();
}

void SpectralLinesPanelController::SetFrameIndex(std::uint64_t frame_index)
{
    frame_index_ = frame_index;
}

bool SpectralLinesPanelController::IsMarkerEnabled(const SpectralLineMarker& marker) const
{
    return IsMarkerVisible(user_state_, marker.id);
}

void SpectralLinesPanelController::SetMarkerEnabled(const SpectralLineMarker& marker, bool enabled)
{
    if (IsMarkerEnabled(marker) == enabled) {
        return;
    }
    SetMarkerVisible(user_state_, marker.id, enabled);
    MarkCacheDirty();
}

void SpectralLinesPanelController::RequestTabSelection()
{
    tab_selection_requested_ = true;
}

bool SpectralLinesPanelController::ShouldSelectTab(std::string_view view_id) const
{
    return tab_selection_requested_ && user_state_.active_view_id == view_id;
}

void SpectralLinesPanelController::AcknowledgeTabSelection(std::string_view view_id)
{
    if (ShouldSelectTab(view_id)) {
        tab_selection_requested_ = false;
    }
}

std::vector<const SpectralLineMarker*> SpectralLinesPanelController::FilteredMarkers(
    const SpectrumSnapshotHandle& snapshot,
    bool include_disabled) const
{
    std::vector<const SpectralLineMarker*> markers;
    if (!snapshot || !snapshot->capabilities.can_show_spectral_lines || catalog_.markers.empty()) {
        return markers;
    }

    for (const SpectralLineMarker& marker : catalog_.markers) {
        if (!include_disabled && !IsMarkerEnabled(marker)) {
            continue;
        }
        markers.push_back(&marker);
    }
    return markers;
}

void SpectralLinesPanelController::NormalizeViewSelection()
{
    const bool active_is_catalog =
        catalog_grouping_view_ && user_state_.active_view_id == catalog_grouping_view_->id;
    if (active_is_catalog) {
        return;
    }
    if (ActiveUserGroupingView() != nullptr) {
        return;
    }
    if (catalog_grouping_view_) {
        user_state_.active_view_id = catalog_grouping_view_->id;
        RequestTabSelection();
        return;
    }
    if (!user_state_.grouping_views.empty()) {
        user_state_.active_view_id = user_state_.grouping_views.front().id;
        RequestTabSelection();
        return;
    }
    if (!user_state_.active_view_id.empty()) {
        user_state_.active_view_id.clear();
        RequestTabSelection();
    }
}

bool SpectralLinesPanelController::SetActiveView(std::string_view view_id)
{
    if (user_state_.active_view_id == view_id) {
        return false;
    }
    user_state_.active_view_id = std::string(view_id);
    MarkCacheDirty();
    return true;
}

void SpectralLinesPanelController::CreateUserGroupingView()
{
    const std::string id = NextGroupingViewId();
    const std::string name = "Grouping " + std::to_string(user_state_.grouping_views.size() + 1);
    user_state_.grouping_views.push_back(
        CreateUserGroupingViewFromCatalog(catalog_, catalog_identity_, id, name));
    user_state_.active_view_id = id;
    RequestTabSelection();
    MarkCacheDirty();
}

void SpectralLinesPanelController::DuplicateUserGroupingView(const GroupingView& source)
{
    const std::string id = NextGroupingViewId();
    const std::string name = source.name + " copy";
    user_state_.grouping_views.push_back(DuplicateGroupingView(source, catalog_, catalog_identity_, id, name));
    user_state_.active_view_id = id;
    RequestTabSelection();
    MarkCacheDirty();
}

bool SpectralLinesPanelController::RenameUserGroupingView(std::string_view view_id, std::string_view name)
{
    const std::string trimmed_name = TrimWhitespace(name);
    if (view_id.empty() || trimmed_name.empty()) {
        return false;
    }
    for (GroupingView& view : user_state_.grouping_views) {
        if (view.id != view_id || view.read_only) {
            continue;
        }
        if (view.name == trimmed_name) {
            return true;
        }
        view.name = trimmed_name;
        MarkCacheDirty();
        return true;
    }
    return false;
}

bool SpectralLinesPanelController::DeleteUserGroupingView(std::string_view view_id)
{
    const auto match = std::find_if(
        user_state_.grouping_views.begin(),
        user_state_.grouping_views.end(),
        [view_id](const GroupingView& view) {
            return view.id == view_id && !view.read_only;
        });
    if (match == user_state_.grouping_views.end()) {
        return false;
    }
    user_state_.grouping_views.erase(match);
    NormalizeViewSelection();
    MarkCacheDirty();
    return true;
}

bool SpectralLinesPanelController::AddUserGroupToView(GroupingView& view)
{
    const std::string group_id = NextUserGroupId();
    const std::string group_name = "Group " + group_id.substr(std::string("group-").size());
    if (!AddUserGroup(view, group_id, group_name)) {
        return false;
    }
    MarkCacheDirty();
    return true;
}

bool SpectralLinesPanelController::AddUserGroupWithMarkerReferenceToView(
    GroupingView& view,
    std::string_view marker_id,
    std::string_view source_group_id,
    bool copy)
{
    if (!copy && source_group_id != UnassignedUserGroupId()) {
        return false;
    }

    const std::string group_id = NextUserGroupId();
    const std::string group_name = "Group " + group_id.substr(std::string("group-").size());
    if (!AddUserGroup(view, group_id, group_name)) {
        return false;
    }

    const bool reference_added =
        copy ? CopyMarkerReference(view, catalog_identity_, marker_id, group_id)
             : MoveMarkerReference(view, catalog_identity_, marker_id, source_group_id, group_id);
    if (!reference_added) {
        RemoveUserGroup(view, group_id);
        return false;
    }

    panel_state_.expanded_group_ids.insert(GroupExpansionKey(view.id, group_id));
    MarkCacheDirty();
    return true;
}

bool SpectralLinesPanelController::RenameUserGroupInView(
    GroupingView& view,
    std::string_view group_id,
    std::string_view name)
{
    const std::string trimmed_name = TrimWhitespace(name);
    if (view.read_only || group_id.empty() || trimmed_name.empty()) {
        return false;
    }

    for (UserGroup& group : view.groups) {
        if (group.id != group_id || group.is_unassigned || group.id == UnassignedUserGroupId()) {
            continue;
        }
        if (group.name == trimmed_name) {
            return true;
        }
        group.name = trimmed_name;
        MarkCacheDirty();
        return true;
    }
    return false;
}

bool SpectralLinesPanelController::DeleteUserGroupFromView(GroupingView& view, std::string_view group_id)
{
    if (!RemoveUserGroup(view, group_id)) {
        return false;
    }
    panel_state_.expanded_group_ids.erase(GroupExpansionKey(view.id, group_id));
    MarkCacheDirty();
    return true;
}

bool SpectralLinesPanelController::SetGroupVisibility(
    const UserGroup& group,
    bool visible,
    bool search_active)
{
    if (!SetGroupMarkerVisibility(user_state_, group, catalog_, catalog_identity_, visible, search_active)) {
        return false;
    }
    MarkCacheDirty();
    return true;
}

void SpectralLinesPanelController::SetGroupExpanded(
    std::string_view view_id,
    std::string_view group_id,
    bool expanded)
{
    const std::string expansion_key = GroupExpansionKey(view_id, group_id);
    if (expanded) {
        panel_state_.expanded_group_ids.insert(expansion_key);
    } else {
        panel_state_.expanded_group_ids.erase(expansion_key);
    }
    MarkCacheDirty();
}

bool SpectralLinesPanelController::ReorderUserGroupBeforeInView(
    GroupingView& view,
    std::string_view source_group_id,
    std::string_view target_group_id)
{
    if (!ReorderUserGroupBefore(view, source_group_id, target_group_id)) {
        return false;
    }
    MarkCacheDirty();
    return true;
}

bool SpectralLinesPanelController::MoveOrCopyMarkerReferenceToGroup(
    GroupingView& view,
    std::string_view marker_id,
    std::string_view source_group_id,
    std::string_view target_group_id,
    bool copy)
{
    const bool changed = copy
                             ? CopyMarkerReference(view, catalog_identity_, marker_id, target_group_id)
                             : MoveMarkerReference(
                                   view,
                                   catalog_identity_,
                                   marker_id,
                                   source_group_id,
                                   target_group_id);
    if (changed) {
        MarkCacheDirty();
    }
    return changed;
}

bool SpectralLinesPanelController::RemoveMarkerReferenceFromGroup(
    GroupingView& view,
    std::string_view marker_id,
    std::string_view group_id)
{
    if (!specforge::RemoveMarkerReferenceFromGroup(view, catalog_identity_, marker_id, group_id)) {
        return false;
    }
    MarkCacheDirty();
    return true;
}

bool SpectralLinesPanelController::CopyMarkerReferenceToGroup(
    GroupingView& view,
    std::string_view marker_id,
    std::string_view target_group_id)
{
    if (!CopyMarkerReference(view, catalog_identity_, marker_id, target_group_id)) {
        return false;
    }
    MarkCacheDirty();
    return true;
}

GroupingView* SpectralLinesPanelController::ActiveUserGroupingView()
{
    for (GroupingView& view : user_state_.grouping_views) {
        if (view.id == user_state_.active_view_id) {
            return &view;
        }
    }
    return nullptr;
}

const GroupingView* SpectralLinesPanelController::ActiveUserGroupingView() const
{
    for (const GroupingView& view : user_state_.grouping_views) {
        if (view.id == user_state_.active_view_id) {
            return &view;
        }
    }
    return nullptr;
}

std::string SpectralLinesPanelController::NextGroupingViewId()
{
    for (;;) {
        std::string id = "view-" + std::to_string(next_view_index_++);
        const bool exists = std::any_of(
            user_state_.grouping_views.begin(),
            user_state_.grouping_views.end(),
            [&id](const auto& view) {
                return view.id == id;
            });
        if (!exists && id != CatalogGroupingViewId()) {
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
            exists = exists || std::any_of(view.groups.begin(), view.groups.end(), [&id](const auto& group) {
                return group.id == id;
            });
        }
        if (!exists && id != UnassignedUserGroupId()) {
            return id;
        }
    }
}

void SpectralLinesPanelController::MarkCacheDirty()
{
    cache_save_scheduler_.MarkDirty(frame_index_);
}

void SpectralLinesPanelController::MaybeSaveCache(std::uint64_t frame_index)
{
    if (!cache_save_scheduler_.ShouldAttemptSave(frame_index)) {
        return;
    }
    FlushCache();
}

void SpectralLinesPanelController::FlushCache()
{
    if (!cache_save_scheduler_.dirty()) {
        return;
    }
    user_state_cache_.catalogs[catalog_identity_.id] = user_state_;
    user_state_cache_.catalog_panel_state[catalog_identity_.id] = panel_state_;
    std::string error;
    if (!SaveCatalogUserStateCache(user_state_cache_path_, user_state_cache_, error)) {
        cache_save_scheduler_.MarkSaveFailed(
            frame_index_,
            cache_save_status_,
            "Could not save spectral-line grouping cache: " + error);
        return;
    }
    warning_.clear();
    cache_save_scheduler_.MarkSaveSucceeded(cache_save_status_);
}

const SpectralLineCatalog& SpectralLinesPanelController::catalog() const
{
    return catalog_;
}

const CatalogIdentity& SpectralLinesPanelController::catalog_identity() const
{
    return catalog_identity_;
}

const std::optional<GroupingView>& SpectralLinesPanelController::catalog_grouping_view() const
{
    return catalog_grouping_view_;
}

CatalogUserState& SpectralLinesPanelController::user_state()
{
    return user_state_;
}

const CatalogUserState& SpectralLinesPanelController::user_state() const
{
    return user_state_;
}

CatalogPanelState& SpectralLinesPanelController::panel_state()
{
    return panel_state_;
}

const CatalogPanelState& SpectralLinesPanelController::panel_state() const
{
    return panel_state_;
}

std::array<char, 96>& SpectralLinesPanelController::filter_buffer()
{
    return filter_;
}

const std::array<char, 96>& SpectralLinesPanelController::filter_buffer() const
{
    return filter_;
}

bool& SpectralLinesPanelController::show_labels()
{
    return show_labels_;
}

bool SpectralLinesPanelController::show_labels() const
{
    return show_labels_;
}

const std::string& SpectralLinesPanelController::warning() const
{
    if (cache_save_status_.failed()) {
        return cache_save_status_.message();
    }
    return warning_;
}

}  // namespace specforge
