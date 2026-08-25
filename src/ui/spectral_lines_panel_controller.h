#pragma once

#include "app/local_user_state.h"
#include "domain/spectrum_snapshot.h"
#include "overlays/catalog_user_state_reconciliation.h"
#include "overlays/spectral_line_catalog.h"
#include "overlays/spectral_line_plot_marker.h"
#include "overlays/spectral_line_user_state.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace specforge {

class SpectralLinesPanelController;

enum class CatalogUserRenameEditState {
    Unedited,
    Edited,
};

struct CatalogUserStateIntent {
    [[nodiscard]] static CatalogUserStateIntent SetGroupingViewSearch(std::string query);
    [[nodiscard]] static CatalogUserStateIntent SetMarkerLabelsVisible(bool visible);
    [[nodiscard]] static CatalogUserStateIntent SelectGroupingView(std::string view_id);
    [[nodiscard]] static CatalogUserStateIntent AcknowledgeGroupingViewSelection(std::string view_id);
    [[nodiscard]] static CatalogUserStateIntent CreateUserGroupingView();
    [[nodiscard]] static CatalogUserStateIntent DuplicateGroupingView(std::string view_id);
    [[nodiscard]] static CatalogUserStateIntent RenameUserGroupingView(
        std::string view_id,
        std::string name,
        CatalogUserRenameEditState edit_state);
    [[nodiscard]] static CatalogUserStateIntent DeleteUserGroupingView(std::string view_id);
    [[nodiscard]] static CatalogUserStateIntent AddUserGroup(std::string view_id);
    [[nodiscard]] static CatalogUserStateIntent MoveMarkerReferenceToNewGroup(
        std::string view_id,
        std::string marker_id,
        std::string source_group_id);
    [[nodiscard]] static CatalogUserStateIntent CopyMarkerReferenceToNewGroup(
        std::string view_id,
        std::string marker_id,
        std::string source_group_id);
    [[nodiscard]] static CatalogUserStateIntent RenameUserGroup(
        std::string view_id,
        std::string group_id,
        std::string name,
        CatalogUserRenameEditState edit_state);
    [[nodiscard]] static CatalogUserStateIntent DeleteUserGroup(
        std::string view_id,
        std::string group_id);
    [[nodiscard]] static CatalogUserStateIntent SetGroupMarkerVisibility(
        std::string view_id,
        std::string group_id,
        bool visible);
    [[nodiscard]] static CatalogUserStateIntent SetGroupExpanded(
        std::string view_id,
        std::string group_id,
        bool expanded);
    [[nodiscard]] static CatalogUserStateIntent ReorderUserGroupBefore(
        std::string view_id,
        std::string source_group_id,
        std::string target_group_id);
    [[nodiscard]] static CatalogUserStateIntent MoveMarkerReference(
        std::string view_id,
        std::string marker_id,
        std::string source_group_id,
        std::string target_group_id);
    [[nodiscard]] static CatalogUserStateIntent CopyMarkerReference(
        std::string view_id,
        std::string marker_id,
        std::string source_group_id,
        std::string target_group_id);
    [[nodiscard]] static CatalogUserStateIntent RemoveMarkerReference(
        std::string view_id,
        std::string marker_id,
        std::string group_id);
    [[nodiscard]] static CatalogUserStateIntent SetMarkerVisibility(
        std::string marker_id,
        bool visible);
    [[nodiscard]] static CatalogUserStateIntent SetMarkerColor(
        std::string marker_id,
        PlotSeriesColor color);

private:
    friend class SpectralLinesPanelController;

    enum class Kind {
        SetGroupingViewSearch,
        SetMarkerLabelsVisible,
        SelectGroupingView,
        AcknowledgeGroupingViewSelection,
        CreateUserGroupingView,
        DuplicateGroupingView,
        RenameUserGroupingView,
        DeleteUserGroupingView,
        AddUserGroup,
        MoveMarkerReferenceToNewGroup,
        CopyMarkerReferenceToNewGroup,
        RenameUserGroup,
        DeleteUserGroup,
        SetGroupMarkerVisibility,
        SetGroupExpanded,
        ReorderUserGroupBefore,
        MoveMarkerReference,
        CopyMarkerReference,
        RemoveMarkerReference,
        SetMarkerVisibility,
        SetMarkerColor,
    };

    explicit CatalogUserStateIntent(Kind kind);

    Kind kind_;
    std::string view_id_;
    std::string group_id_;
    std::string source_group_id_;
    std::string target_group_id_;
    std::string marker_id_;
    std::string text_;
    bool enabled_ = false;
    PlotSeriesColor marker_color_ = PlotSeriesColor::Auto();
    CatalogUserRenameEditState rename_edit_state_ =
        CatalogUserRenameEditState::Unedited;
};

enum class CatalogUserStateResultStatus {
    Applied,
    NoChange,
    Rejected,
};

struct CatalogUserStateResult {
    CatalogUserStateResultStatus status = CatalogUserStateResultStatus::NoChange;
    bool changed = false;
    bool persistent_state_changed = false;
    std::string message;
};

struct SpectralLineMarkerReferenceView {
    std::string marker_id;
    std::string label;
    std::string wavelength_text;
    std::string notes;
    bool resolved = false;
    bool visible = false;
    bool shared = false;
    PlotSeriesColor color = PlotSeriesColor::Auto();
    std::size_t automatic_color_slot = 0;
};

struct SpectralLineGroupView {
    std::string id;
    std::string name;
    GeneratedNameMetadata generated_name;
    bool is_unassigned = false;
    bool expanded = false;
    bool dimmed_by_search = false;
    GroupVisibilityState visibility = GroupVisibilityState::Empty;
    std::vector<SpectralLineMarkerReferenceView> marker_references;
};

struct SpectralLineGroupingView {
    std::string id;
    std::string name;
    GeneratedNameMetadata generated_name;
    bool editable = false;
    bool active = false;
    bool selection_requested = false;
    bool search_active = false;
    std::vector<SpectralLineGroupView> groups;
};

enum class SpectralLineCacheLoadIssueKind {
    None,
    ReadFailed,
    InvalidDocument,
    UnsupportedFormatOrSchema,
};

struct SpectralLinePersistenceView {
    bool retrying = false;
    bool recovered = false;
    SpectralLineCacheLoadIssueKind load_issue =
        SpectralLineCacheLoadIssueKind::None;
    std::string load_diagnostic_detail;
    std::string save_diagnostic_detail;
};

struct CatalogUserStateView {
    std::string catalog_id;
    std::string catalog_display_name;
    std::string catalog_load_error;
    SpectralLinePersistenceView persistence;
    std::string grouping_view_search;
    bool marker_labels_visible = true;
    bool has_catalog_grouping_view = false;
    std::size_t user_grouping_view_count = 0;
    std::size_t catalog_marker_count = 0;
    std::vector<SpectralLineGroupingView> grouping_views;
};

struct SpectralLinePlotView {
    std::vector<SpectralLinePlotMarker> visible_markers;
    bool marker_labels_visible = true;
    std::string_view layout_scope_id;
};

class SpectralLinesPanelController {
public:
    explicit SpectralLinesPanelController(
        std::filesystem::path packaged_catalog_path);
    SpectralLinesPanelController(
        std::filesystem::path packaged_catalog_path,
        std::filesystem::path user_state_cache_path);
    SpectralLinesPanelController(
        SpectralLineCatalog catalog,
        CatalogIdentity catalog_identity,
        std::filesystem::path user_state_cache_path);
    ~SpectralLinesPanelController();

    SpectralLinesPanelController(const SpectralLinesPanelController&) = delete;
    SpectralLinesPanelController& operator=(const SpectralLinesPanelController&) = delete;

    [[nodiscard]] CatalogUserStateResult Submit(CatalogUserStateIntent intent);
    [[nodiscard]] CatalogUserStateView View() const;
    [[nodiscard]] SpectralLinePlotView PlotView(
        const SpectrumSnapshotHandle& snapshot) const;
    [[nodiscard]] LocalUserStatePersistenceStatus
        PersistenceStatus() const;

    void RunMaintenance(LocalUserStateSaveScheduler::TimePoint now);
    [[nodiscard]] std::optional<LocalUserStateSaveScheduler::TimePoint> NextMaintenanceDeadline() const;
    [[nodiscard]] bool Flush();

private:
    [[nodiscard]] CatalogUserStateResult Applied(bool persistent_state_changed);
    [[nodiscard]] static CatalogUserStateResult NoChange();
    [[nodiscard]] static CatalogUserStateResult Rejected(std::string message);
    [[nodiscard]] bool ViewExists(std::string_view view_id) const;
    [[nodiscard]] bool MarkerExists(std::string_view marker_id) const;
    [[nodiscard]] std::size_t MarkerAutomaticColorSlot(
        std::string_view marker_id) const;
    [[nodiscard]] GroupingView* FindUserGroupingView(std::string_view view_id);
    [[nodiscard]] const GroupingView* FindUserGroupingView(std::string_view view_id) const;
    [[nodiscard]] std::optional<GroupingView> EffectiveGroupingView(std::string_view view_id) const;
    [[nodiscard]] static UserGroup* FindUserGroup(GroupingView& view, std::string_view group_id);
    [[nodiscard]] static const UserGroup* FindUserGroup(
        const GroupingView& view,
        std::string_view group_id);
    [[nodiscard]] std::string NextGroupingViewId();
    [[nodiscard]] std::string NextUserGroupId();
    [[nodiscard]] LocalUserStatePersistenceLifecycle::SaveResult
        SaveCatalogUserState();
    void MarkCacheDirty();
    void RequestGroupingViewSelection();

    SpectralLineCatalog catalog_;
    CatalogIdentity catalog_identity_;
    std::optional<GroupingView> catalog_grouping_view_;
    CatalogUserStateCache user_state_cache_;
    CatalogUserState user_state_;
    CatalogPanelState panel_state_;
    StablePlotSeriesColorAssignments marker_color_assignments_;
    std::unordered_map<std::string, std::size_t> marker_auto_slots_;
    std::filesystem::path user_state_cache_path_;
    CatalogUserState reconciliation_base_state_;
    CatalogPanelState reconciliation_base_panel_state_;
    LocalUserStatePersistenceLifecycle cache_persistence_;
    SpectralLineCacheLoadIssueKind load_issue_kind_ =
        SpectralLineCacheLoadIssueKind::None;
    std::string grouping_view_search_;
    bool marker_labels_visible_ = true;
    bool grouping_view_selection_requested_ = true;
    std::uint64_t next_view_sequence_ = 1;
    std::uint64_t next_group_sequence_ = 1;
    // Canonicalization may change active_view_id after a deletion without
    // representing a user selection. Keep task provenance separate from the
    // durable scalar so stale fallback cannot win a real peer selection.
    bool explicit_selection_intent_pending_ = false;
    std::unordered_set<std::string> explicit_group_ordering_view_ids_;
    bool explicit_task_delta_pending_ = false;
};

}  // namespace specforge
