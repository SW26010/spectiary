#pragma once

#include "domain/sample_filter.h"
#include "domain/sample_labeling.h"
#include "domain/spectrum_snapshot.h"
#include "ui/background_retirement.h"
#include "ui/sample_navigation_sequence.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace specforge {

struct SourceCollectionSessionAction {
    bool snapshot_changed = false;
    bool workflow_changed = false;
    bool navigation_inputs_changed = false;
};

inline void MergeSourceCollectionSessionAction(
    SourceCollectionSessionAction& target,
    const SourceCollectionSessionAction& source)
{
    target.snapshot_changed = target.snapshot_changed || source.snapshot_changed;
    target.workflow_changed = target.workflow_changed || source.workflow_changed;
    target.navigation_inputs_changed = target.navigation_inputs_changed || source.navigation_inputs_changed;
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
    std::string relationship_label;
    std::string display_text;
    std::string message;
    bool missing = false;
    bool output_missing = false;
    bool metadata_missing = false;
    bool can_activate_labeling = false;
    bool can_filter_samples = false;
    bool can_sort_samples = false;
    bool can_rename_annotation = false;
    bool can_remove_annotation = false;
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
    std::vector<std::size_t> sequence_rows;
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
    std::vector<std::string> annotation_messages;
    std::vector<SourceCollectionAnnotationValueView> current_annotations;
};

struct SourceCollectionLabelingView {
    bool has_active_source = false;
    std::optional<std::size_t> current_index;
    bool has_active_task = false;
    bool has_temporary_task = false;
    bool active_task_is_temporary = false;
    std::string task_id;
    std::string task_name;
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

struct SourceCollectionSourceView {
    std::filesystem::path path;
    std::string display_name;
    std::string type_label;
    std::string state_label;
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
};

}  // namespace specforge
