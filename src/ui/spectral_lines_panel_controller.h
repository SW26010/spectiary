#pragma once

#include "domain/spectrum_snapshot.h"
#include "overlays/spectral_line_catalog.h"
#include "overlays/spectral_line_user_state.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace specforge {

class SpectralLinesPanelController {
public:
    SpectralLinesPanelController();
    ~SpectralLinesPanelController();

    void SetFrameIndex(std::uint64_t frame_index);
    void MaybeSaveCache(std::uint64_t frame_index);
    void FlushCache();
    void MarkCacheDirty();
    void RequestTabSelection();
    [[nodiscard]] bool ShouldSelectTab(std::string_view view_id) const;
    void AcknowledgeTabSelection(std::string_view view_id);

    [[nodiscard]] std::vector<const SpectralLineMarker*> FilteredMarkers(
        const SpectrumSnapshotHandle& snapshot,
        bool include_disabled) const;
    [[nodiscard]] bool IsMarkerEnabled(const SpectralLineMarker& marker) const;
    void SetMarkerEnabled(const SpectralLineMarker& marker, bool enabled);

    void NormalizeViewSelection();
    bool SetActiveView(std::string_view view_id);
    void CreateUserGroupingView();
    void DuplicateUserGroupingView(const GroupingView& source);
    bool RenameUserGroupingView(std::string_view view_id, std::string_view name);
    bool DeleteUserGroupingView(std::string_view view_id);
    bool AddUserGroupToView(GroupingView& view);
    bool AddUserGroupWithMarkerReferenceToView(
        GroupingView& view,
        std::string_view marker_id,
        std::string_view source_group_id,
        bool copy);
    bool RenameUserGroupInView(GroupingView& view, std::string_view group_id, std::string_view name);
    bool DeleteUserGroupFromView(GroupingView& view, std::string_view group_id);
    bool SetGroupVisibility(const UserGroup& group, bool visible, bool search_active);
    void SetGroupExpanded(std::string_view view_id, std::string_view group_id, bool expanded);
    bool ReorderUserGroupBeforeInView(
        GroupingView& view,
        std::string_view source_group_id,
        std::string_view target_group_id);
    bool MoveOrCopyMarkerReferenceToGroup(
        GroupingView& view,
        std::string_view marker_id,
        std::string_view source_group_id,
        std::string_view target_group_id,
        bool copy);
    bool RemoveMarkerReferenceFromGroup(
        GroupingView& view,
        std::string_view marker_id,
        std::string_view group_id);
    bool CopyMarkerReferenceToGroup(
        GroupingView& view,
        std::string_view marker_id,
        std::string_view target_group_id);
    [[nodiscard]] GroupingView* ActiveUserGroupingView();
    [[nodiscard]] const GroupingView* ActiveUserGroupingView() const;
    [[nodiscard]] std::string NextGroupingViewId();
    [[nodiscard]] std::string NextUserGroupId();

    [[nodiscard]] const SpectralLineCatalog& catalog() const;
    [[nodiscard]] const CatalogIdentity& catalog_identity() const;
    [[nodiscard]] const std::optional<GroupingView>& catalog_grouping_view() const;
    [[nodiscard]] CatalogUserState& user_state();
    [[nodiscard]] const CatalogUserState& user_state() const;
    [[nodiscard]] CatalogPanelState& panel_state();
    [[nodiscard]] const CatalogPanelState& panel_state() const;
    [[nodiscard]] std::array<char, 96>& filter_buffer();
    [[nodiscard]] const std::array<char, 96>& filter_buffer() const;
    [[nodiscard]] bool& show_labels();
    [[nodiscard]] bool show_labels() const;
    [[nodiscard]] const std::string& warning() const;

private:
    SpectralLineCatalog catalog_;
    CatalogIdentity catalog_identity_;
    std::optional<GroupingView> catalog_grouping_view_;
    CatalogUserStateCache user_state_cache_;
    CatalogUserState user_state_;
    CatalogPanelState panel_state_;
    std::filesystem::path user_state_cache_path_;
    std::string warning_;
    bool cache_dirty_ = false;
    bool tab_selection_requested_ = true;
    std::uint64_t cache_dirty_frame_ = 0;
    std::uint64_t cache_next_save_frame_ = 0;
    std::uint64_t frame_index_ = 0;
    int next_view_index_ = 1;
    int next_group_index_ = 1;
    bool show_labels_ = true;
    std::array<char, 96> filter_ = {};
};

}  // namespace specforge
