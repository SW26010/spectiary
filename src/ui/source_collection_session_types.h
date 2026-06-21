#pragma once

#include "domain/sample_filter.h"
#include "domain/sample_labeling.h"
#include "domain/spectrum_snapshot.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
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
    std::string display_text;
    bool missing = false;
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
    std::string task_name;
    SampleLabelSet label_set;
    std::size_t labeled_count = 0;
    std::size_t sample_count = 0;
    int current_code = kUnlabeledSampleLabelCode;
    bool auto_advance = false;
    bool skip_labeled_on_advance = false;
    std::optional<std::size_t> remembered_position;
    std::optional<std::filesystem::path> output_path;
    SampleLabelSaveState save_state;
    bool state_save_failed = false;
    std::string state_save_error;
    std::string state_load_warning;
};

struct SourceCollectionFilterSourceView {
    std::string id;
    std::string name;
    bool filterable = false;
    std::vector<SampleFilterValueOption> options;
    std::unordered_set<std::string> selected_value_keys;
};

struct SourceCollectionFilterView {
    bool has_active_source = false;
    std::size_t sample_count = 0;
    bool has_active_labeling_task = false;
    bool active_labeling_filter_source_selected = false;
    bool navigation_filter_active = false;
    bool current_sample_in_filter = true;
    SampleFilterEvaluation evaluation;
    std::vector<SourceCollectionFilterSourceView> sources;
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
};

struct SourceCollectionSessionView {
    SpectrumSnapshotHandle snapshot;
    std::vector<SourceCollectionSourceView> sources;
    std::optional<std::size_t> current_source_index;
    bool can_add_read_only_annotation = false;
    SourceCollectionNavigationView navigation;
    SourceCollectionLabelingView labeling;
    SourceCollectionFilterView filter;
};

}  // namespace specforge
