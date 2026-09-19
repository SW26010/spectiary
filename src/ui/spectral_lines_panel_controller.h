#pragma once

#include "app/local_user_state.h"
#include "domain/spectrum_snapshot.h"
#include "overlays/built_in_spectral_line_adapter.h"

#include "overlays/spectral_line_plot_marker.h"


#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spectiary {

enum class GroupVisibilityState { Empty, AllVisible, AllHidden, Mixed, SearchFiltered };
class SpectralLinesPanelController;

enum class SpectralLineRenameEditState {
    Unedited,
    Edited,
};

struct SpectralLineStateIntent {
    [[nodiscard]] static SpectralLineStateIntent SetGroupingViewSearch(std::string query);
    [[nodiscard]] static SpectralLineStateIntent SetMarkerLabelsVisible(bool visible);
    [[nodiscard]] static SpectralLineStateIntent SelectGroupingView(std::string view_id);
    [[nodiscard]] static SpectralLineStateIntent AcknowledgeGroupingViewSelection(std::string view_id);
    [[nodiscard]] static SpectralLineStateIntent CreateUserGroupingView();
    [[nodiscard]] static SpectralLineStateIntent DuplicateGroupingView(std::string view_id);
    [[nodiscard]] static SpectralLineStateIntent RenameUserGroupingView(
        std::string view_id,
        std::string name,
        SpectralLineRenameEditState edit_state);
    [[nodiscard]] static SpectralLineStateIntent DeleteUserGroupingView(std::string view_id);
    [[nodiscard]] static SpectralLineStateIntent AddUserGroup(std::string view_id);
    [[nodiscard]] static SpectralLineStateIntent MoveMarkerReferenceToNewGroup(
        std::string view_id,
        std::string marker_id,
        std::string source_group_id);
    [[nodiscard]] static SpectralLineStateIntent CopyMarkerReferenceToNewGroup(
        std::string view_id,
        std::string marker_id,
        std::string source_group_id);
    [[nodiscard]] static SpectralLineStateIntent RenameUserGroup(
        std::string view_id,
        std::string group_id,
        std::string name,
        SpectralLineRenameEditState edit_state);
    [[nodiscard]] static SpectralLineStateIntent DeleteUserGroup(
        std::string view_id,
        std::string group_id);
    [[nodiscard]] static SpectralLineStateIntent SetGroupMarkerVisibility(
        std::string view_id,
        std::string group_id,
        bool visible);
    [[nodiscard]] static SpectralLineStateIntent SetGroupExpanded(
        std::string view_id,
        std::string group_id,
        bool expanded);
    [[nodiscard]] static SpectralLineStateIntent ReorderUserGroupBefore(
        std::string view_id,
        std::string source_group_id,
        std::string target_group_id);
    [[nodiscard]] static SpectralLineStateIntent MoveMarkerReference(
        std::string view_id,
        std::string marker_id,
        std::string source_group_id,
        std::string target_group_id);
    [[nodiscard]] static SpectralLineStateIntent CopyMarkerReference(
        std::string view_id,
        std::string marker_id,
        std::string source_group_id,
        std::string target_group_id);
    [[nodiscard]] static SpectralLineStateIntent RemoveMarkerReference(
        std::string view_id,
        std::string marker_id,
        std::string group_id);
    [[nodiscard]] static SpectralLineStateIntent SetMarkerVisibility(
        std::string marker_id,
        bool visible);
    [[nodiscard]] static SpectralLineStateIntent SetMarkerColor(
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

    explicit SpectralLineStateIntent(Kind kind);

    Kind kind_;
    std::string view_id_;
    std::string group_id_;
    std::string source_group_id_;
    std::string target_group_id_;
    std::string marker_id_;
    std::string text_;
    bool enabled_ = false;
    PlotSeriesColor marker_color_ = PlotSeriesColor::Auto();
    SpectralLineRenameEditState rename_edit_state_ =
        SpectralLineRenameEditState::Unedited;
};

enum class SpectralLineStateResultStatus {
    Applied,
    NoChange,
    Rejected,
};

struct SpectralLineStateResult {
    SpectralLineStateResultStatus status = SpectralLineStateResultStatus::NoChange;
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

struct SpectralLinePanelView {
    std::string line_list_id;
    std::string line_list_display_name;
    std::string line_list_load_error;
    SpectralLinePersistenceView persistence;
    std::string grouping_view_search;
    bool marker_labels_visible = true;
    bool has_base_grouping_view = false;
    std::size_t user_grouping_view_count = 0;
    std::size_t line_list_marker_count = 0;
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
        std::filesystem::path packaged_line_list_path);
    SpectralLinesPanelController(
        std::filesystem::path packaged_line_list_path,
        std::filesystem::path user_state_cache_path);
    SpectralLinesPanelController(
        SpectralLineList list,
        std::filesystem::path user_state_cache_path);
    ~SpectralLinesPanelController();

    SpectralLinesPanelController(const SpectralLinesPanelController&) = delete;
    SpectralLinesPanelController& operator=(const SpectralLinesPanelController&) = delete;

    [[nodiscard]] SpectralLineStateResult Submit(SpectralLineStateIntent intent);
    [[nodiscard]] SpectralLinePanelView View() const;
    [[nodiscard]] SpectralLinePlotView PlotView(
        const SpectrumSnapshotHandle& snapshot) const;
    [[nodiscard]] LocalUserStatePersistenceStatus
        PersistenceStatus() const;

    void RunMaintenance(LocalUserStateSaveScheduler::TimePoint now);
    [[nodiscard]] std::optional<LocalUserStateSaveScheduler::TimePoint> NextMaintenanceDeadline() const;
    [[nodiscard]] bool Flush();

private:
    SpectralLinesPanelController(SpectralLineListParseResult parsed, std::filesystem::path state_path);
    [[nodiscard]] SpectralLineStateResult Applied(bool persistent_state_changed);
    [[nodiscard]] static SpectralLineStateResult NoChange();
    [[nodiscard]] static SpectralLineStateResult Rejected(std::string message);
    [[nodiscard]] const line_list::GroupingView* FindView(std::string_view id) const;
    [[nodiscard]] const line_list::Marker* FindMarker(std::string_view id) const;
    [[nodiscard]] std::string NextId(bool group) const;
    [[nodiscard]] std::vector<std::string> GroupMembers(const line_list::GroupingView& view, std::string_view id) const;
    [[nodiscard]] bool Visible(std::string_view id) const;
    [[nodiscard]] std::string UnassignedId(const line_list::GroupingView& view) const;
    [[nodiscard]] LocalUserStatePersistenceLifecycle::SaveResult SaveState();
    BuiltInSpectralLineAdapter adapter_;
    StablePlotSeriesColorAssignments marker_color_assignments_;
    std::unordered_map<std::string, std::size_t> marker_auto_slots_;
    LocalUserStatePersistenceLifecycle cache_persistence_;
    std::string grouping_view_search_;
    bool marker_labels_visible_ = true;
    bool grouping_view_selection_requested_ = true;
};
} // namespace spectiary
