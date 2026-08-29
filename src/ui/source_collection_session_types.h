#pragma once

#include "app/local_user_state.h"
#include "domain/sample_filter.h"
#include "domain/sample_labeling.h"
#include "domain/source_collection_manifest.h"
#include "domain/spectrum_snapshot.h"
#include "ui/background_retirement.h"
#include "ui/sample_labeling_controller.h"
#include "ui/sample_navigation_sequence.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace specforge {

enum class SourceCollectionLoadErrorKind {
    None,
    BackgroundLoadingFailed,
    PreparedResultNotApplicable,
    PreparedReuseTargetUnavailable,
    PreparedKnownSourcePlanStale,
    PreparedNavigationUnavailable,
};

struct SourceCollectionLoadError {
    SourceCollectionLoadErrorKind kind =
        SourceCollectionLoadErrorKind::None;
    // Parser, operating-system, and other source-specific diagnostics are
    // intentionally kept separate from the application-authored semantic.
    std::string diagnostic_detail;
};

struct SourceCollectionLoadFailure {
    std::filesystem::path source_path;
    SourceCollectionLoadError error;
};

enum class SourceCollectionSnapshotChangeReason {
    None,
    SampleChangedWithinCollection,
    SnapshotReloadedWithinCollection,
    SourceCollectionChanged,
    SourceCollectionCleared,
};

[[nodiscard]] constexpr int SnapshotChangeReasonPriority(
    SourceCollectionSnapshotChangeReason reason) noexcept
{
    switch (reason) {
    case SourceCollectionSnapshotChangeReason::None:
        return 0;
    case SourceCollectionSnapshotChangeReason::SampleChangedWithinCollection:
        return 1;
    case SourceCollectionSnapshotChangeReason::SnapshotReloadedWithinCollection:
        return 2;
    case SourceCollectionSnapshotChangeReason::SourceCollectionChanged:
        return 3;
    case SourceCollectionSnapshotChangeReason::SourceCollectionCleared:
        return 4;
    }
    return 0;
}

struct SourceCollectionSessionAction {
    bool source_roster_changed = false;
    bool snapshot_changed = false;
    SourceCollectionSnapshotChangeReason snapshot_change_reason =
        SourceCollectionSnapshotChangeReason::None;
    bool workflow_changed = false;
    bool navigation_inputs_changed = false;
    // The set of annotation paths persisted with the source session changed.
    // Value-only refreshes of an existing attachment do not set this flag.
    bool annotation_roster_changed = false;
};

inline void MergeSourceCollectionSessionAction(
    SourceCollectionSessionAction& target,
    const SourceCollectionSessionAction& source)
{
    target.source_roster_changed =
        target.source_roster_changed ||
        source.source_roster_changed;
    target.snapshot_changed = target.snapshot_changed || source.snapshot_changed;
    if (SnapshotChangeReasonPriority(source.snapshot_change_reason) >
        SnapshotChangeReasonPriority(target.snapshot_change_reason)) {
        target.snapshot_change_reason = source.snapshot_change_reason;
    }
    target.workflow_changed = target.workflow_changed || source.workflow_changed;
    target.navigation_inputs_changed = target.navigation_inputs_changed || source.navigation_inputs_changed;
    target.annotation_roster_changed =
        target.annotation_roster_changed ||
        source.annotation_roster_changed;
}

struct SourceCollectionSampleNameMatchView {
    std::size_t row = 0;
    std::string name;
};

struct SourceCollectionAnnotationValueView {
    std::string name;
    std::filesystem::path path;
    SampleAnnotationWorkflowRelationship relationship =
        SampleAnnotationWorkflowRelationship::PlainAnnotation;
    std::string display_text;
    std::optional<SourceCollectionManifestDiagnostic>
        diagnostic;
    bool missing = false;
    bool output_missing = false;
    bool metadata_missing = false;
    bool can_activate_labeling = false;
    bool can_filter_samples = false;
    bool can_sort_samples = false;
    bool can_rename_annotation = false;
    bool can_remove_annotation = false;
};

// Read-only position of the current sample in the final navigation sequence.
// The position is zero-based for workflow consumers; presentation code owns
// any one-based formatting. An empty position explicitly means that the
// current sample cannot be mapped into the resolved sequence. The sequence
// length remains available in that state; the position never falls back to the
// current source-row index.
struct SourceCollectionResolvedSequencePositionView {
    std::optional<std::size_t> zero_based_position;
    std::size_t sequence_length = 0;
};

struct SourceCollectionNavigationView {
    bool has_active_source = false;
    std::optional<std::size_t> current_index;
    std::size_t sample_count = 0;
    bool can_move_previous = false;
    bool can_move_next = false;
    bool filter_active = false;
    std::size_t filtered_sample_count = 0;
    bool current_sample_in_filter = true;
    bool sequence_active = false;
    bool sequence_empty = false;
    std::size_t sequence_count = 0;
    SourceCollectionResolvedSequencePositionView resolved_sequence_position;
    // Changes only when source context, membership, or ordering changes.
    std::uint64_t sequence_topology_revision = 0;
    std::optional<std::size_t> current_sequence_position;
    std::optional<std::size_t> current_source_row;
    bool row_location_available = true;
    std::string current_sample_name;
    std::string current_sample_display_name;
    bool has_sample_names = false;
    std::optional<std::size_t> exact_sample_name_match;
    std::string exact_sample_name;
    bool has_partial_sample_name_matches = false;
    std::vector<SourceCollectionSampleNameMatchView> sample_name_matches;
    std::vector<SourceCollectionManifestDiagnostic>
        annotation_diagnostics;
    std::vector<SourceCollectionAnnotationValueView> current_annotations;
};

struct SourceCollectionLabelingRecoveryDraftView {
    std::string task_id;
    std::string task_name;
    SampleLabelingRecoveryDraftStatus status =
        SampleLabelingRecoveryDraftStatus::Stale;
    std::size_t labeled_count = 0;
    std::size_t sample_count = 0;
    SampleLabelSaveState save_state;
};

struct SourceCollectionLabelingView {
    bool has_active_source = false;
    std::string source_identity;
    std::optional<std::size_t> current_index;
    bool has_active_task = false;
    bool has_temporary_task = false;
    bool active_task_is_temporary = false;
    std::string task_id;
    std::string task_name;
    std::vector<std::string> task_ids;
    SampleLabelSet label_set;
    std::unordered_map<int, std::size_t> label_usage_counts;
    std::size_t labeled_count = 0;
    std::size_t sample_count = 0;
    int current_code = kUnlabeledSampleLabelCode;
    bool auto_advance = false;
    bool skip_labeled_on_advance = false;
    std::optional<std::size_t> remembered_position;
    bool remembered_position_resumable = false;
    std::optional<std::filesystem::path> output_path;
    SampleLabelSaveState save_state;
    bool can_deactivate_task = false;
    bool can_delete_task = false;
    bool state_save_failed = false;
    std::string state_save_error;
    std::string state_load_warning;
    std::uint64_t recovery_revision = 0;
    std::vector<SourceCollectionLabelingRecoveryDraftView>
        recovery_drafts;

    [[nodiscard]] bool HasConflictingLabelCode(int original_code, int requested_code) const
    {
        if (requested_code == original_code) {
            return false;
        }
        const auto usage = label_usage_counts.find(requested_code);
        return ContainsSampleLabelCode(label_set, requested_code) ||
               (usage != label_usage_counts.end() && usage->second > 0);
    }
};

struct SourceCollectionFilterSourceView {
    std::string id;
    std::string name;
    std::filesystem::path annotation_path;
    bool filterable = false;
    std::vector<SampleFilterValueOption> options;
    std::unordered_set<std::string> selected_value_keys;
};

struct SourceCollectionFilterView {
    bool has_active_source = false;
    std::size_t sample_count = 0;
    bool navigation_filter_active = false;
    bool current_sample_in_filter = true;
    SampleFilterEvaluation evaluation;
    std::vector<SourceCollectionFilterSourceView> sources;
    std::vector<SourceCollectionFilterSourceView> available_sources;
};

struct SourceCollectionSampleSortSourceView {
    std::string id;
    std::string name;
    std::filesystem::path annotation_path;
    bool selected = false;
    bool removable = false;
    SampleNavigationSortDirection direction = SampleNavigationSortDirection::Ascending;
};

struct SourceCollectionSampleSortingView {
    bool has_active_source = false;
    bool active = false;
    std::string active_source_id;
    SampleNavigationSortDirection direction = SampleNavigationSortDirection::Ascending;
    SampleNavigationSortDirection source_order_direction = SampleNavigationSortDirection::Ascending;
    std::vector<SourceCollectionSampleSortSourceView> sources;
    std::vector<SourceCollectionSampleSortSourceView> available_sources;
};

enum class SourceCollectionSampleTransitionReason {
    Previous,
    Next,
    LabelingAutoAdvance,
    LocateRow,
    LocateSequencePosition,
    LocateSourceRowInSequence,
    LocateSampleName,
    NavigationInputReconciliation,
    SourceActivation,
    Restore,
};

// Transient, read-only context for presenting the most recent effective sample
// transition. Source-row indexes are explicit so presentation code never has to
// infer the source sample from the current navigation position. When activation
// changes the roster source key, from_sample_index is empty because that row
// belongs to the previous source rather than the active view context.
struct SourceCollectionSampleTransitionView {
    SourceCollectionSampleTransitionReason reason =
        SourceCollectionSampleTransitionReason::SourceActivation;
    std::optional<std::size_t> from_sample_index;
    std::optional<std::size_t> current_sample_index;
    std::optional<int> accepted_label_value;
};

enum class SourceCollectionSourceState {
    Unavailable,
    Error,
    Loaded,
    LoadedWithDiagnostics,
    NotPlottable,
};

struct SourceCollectionSourceView {
    std::filesystem::path path;
    std::string display_name;
    std::optional<std::string> type;
    SourceCollectionSourceState state =
        SourceCollectionSourceState::Unavailable;
};

struct SourceCollectionSavedSource {
    std::filesystem::path path;
    std::size_t last_spectrum_index = 0;
    std::vector<std::filesystem::path> annotation_paths;
};

struct SourceCollectionDeferredRestorePlan {
    std::vector<SourceCollectionSavedSource> sources;
    std::optional<std::size_t> active_source_index;
};

struct SourceCollectionStateFlushResult {
    bool source_session_saved = true;
    bool navigation_saved = true;
    bool labeling_saved = true;
    bool workflow_saved = true;

    [[nodiscard]] bool all_saved() const
    {
        return source_session_saved && navigation_saved &&
               labeling_saved && workflow_saved;
    }
};

struct SourceCollectionSessionView {
    SpectrumSnapshotHandle snapshot;
    SpectrumSnapshotHandle current_sample_snapshot;
    std::vector<SourceCollectionSourceView> sources;
    std::optional<std::size_t> current_source_index;
    bool can_add_read_only_annotation = false;
    SourceCollectionNavigationView navigation;
    SourceCollectionLabelingView labeling;
    SourceCollectionFilterView filter;
    SourceCollectionSampleSortingView sorting;
    std::optional<SourceCollectionSampleTransitionView>
        sample_transition;
    LocalUserStateHealthView persistence;
};

}  // namespace specforge
