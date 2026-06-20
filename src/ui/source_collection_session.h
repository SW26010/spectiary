#pragma once

#include "domain/sample_filter.h"
#include "domain/spectrum_snapshot.h"
#include "ui/sample_labeling_controller.h"
#include "ui/sample_navigation_controller.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
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

struct SourceCollectionNavigationAction {
    SampleNavigationResult navigation;
    SourceCollectionSessionAction action;
};

struct SourceCollectionCommandResult {
    bool changed = false;
    SourceCollectionSessionAction action;
};

struct SourceCollectionAnnotationAction {
    bool loaded = false;
    SourceCollectionSessionAction action;
};

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

class SourceCollectionSession {
public:
    struct SourceListEntry {
        std::filesystem::path path;
        std::string key;
        std::string display_name;
        std::string type_label;
        std::string state_label;
        // Stores the last domain snapshot for this source so reactivation can use
        // an explicit cache instead of reloading. Do not remove as a summary-only
        // optimization without retesting CSV/folder error snapshots: that change
        // reproduced 0xc0000005 shared_ptr refcount crashes.
        SpectrumSnapshotHandle cached_snapshot;
        std::size_t last_spectrum_index = 0;
    };

    using SnapshotLoader = std::function<SpectrumSnapshotHandle(const std::filesystem::path&, std::size_t)>;

    explicit SourceCollectionSession(SnapshotLoader snapshot_loader);
    SourceCollectionSession(
        SnapshotLoader snapshot_loader,
        std::filesystem::path navigation_state_cache_path,
        std::filesystem::path labeling_state_cache_path);

    [[nodiscard]] SourceCollectionSessionAction OpenSource(
        const std::filesystem::path& path,
        std::size_t spectrum_index = 0);
    [[nodiscard]] SourceCollectionSessionAction ActivateSource(std::size_t source_index);
    [[nodiscard]] SourceCollectionSessionAction RemoveSource(std::size_t source_index);
    [[nodiscard]] SourceCollectionNavigationAction RequestSampleNavigation(const SampleNavigationRequest& request);
    [[nodiscard]] SourceCollectionAnnotationAction AddReadOnlyAnnotationToActiveSource(
        const std::filesystem::path& path,
        std::string* message = nullptr);

    [[nodiscard]] SourceCollectionSessionAction SetSampleNameQuery(std::string query);
    [[nodiscard]] SourceCollectionNavigationAction CommitSampleNameSelection(
        std::size_t target_row,
        std::string matched_name);
    [[nodiscard]] SourceCollectionSessionAction CreateDefaultLabelingTask();
    [[nodiscard]] SourceCollectionCommandResult UpsertActiveLabel(SampleLabelDefinition label);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingAutoAdvance(bool enabled);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingSkipLabeledOnAdvance(bool enabled);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingOutputPath(std::filesystem::path output_path);
    [[nodiscard]] SourceCollectionSessionAction AssignActiveLabelToCurrentSample(int code);
    [[nodiscard]] SourceCollectionSessionAction ClearActiveLabelForCurrentSample();
    [[nodiscard]] SourceCollectionSessionAction ClearFilters();
    [[nodiscard]] SourceCollectionSessionAction SetFilterValueSelected(
        std::string source_id,
        std::string value_key,
        bool selected);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingFilterSourceSelected(bool selected);

    [[nodiscard]] SourceCollectionNavigationView NavigationView() const;
    [[nodiscard]] SourceCollectionLabelingView LabelingView() const;
    [[nodiscard]] SourceCollectionFilterView FilterView() const;
    [[nodiscard]] bool can_add_read_only_annotation() const;

    void MaybeSaveStateCaches(std::uint64_t frame_index);
    [[nodiscard]] bool FlushStateCaches();

    [[nodiscard]] const SpectrumSnapshotHandle& snapshot() const;
    [[nodiscard]] const std::vector<SourceListEntry>& sources() const;
    [[nodiscard]] std::optional<std::size_t> current_source_index() const;
    [[nodiscard]] const SourceListEntry* current_source() const;

private:
    [[nodiscard]] std::size_t AddOrUpdateSource(
        const std::filesystem::path& path,
        SpectrumSnapshotHandle snapshot,
        std::size_t spectrum_index);
    [[nodiscard]] SourceCollectionSessionAction EnsureSnapshotMatchesNavigation();
    [[nodiscard]] SourceCollectionSessionAction LoadActiveSourceAt(std::size_t spectrum_index);
    void SyncSampleNavigationSession(SourceCollectionSessionAction& action);
    void SyncSampleWorkflowSession(SourceCollectionSessionAction& action);
    void ClearSampleWorkflow(SourceCollectionSessionAction& action);
    void SetSnapshot(SpectrumSnapshotHandle snapshot, SourceCollectionSessionAction& action);
    void ApplySampleFilters();
    [[nodiscard]] std::size_t ActiveSampleCount() const;
    [[nodiscard]] std::optional<std::size_t> ActiveSampleIndex() const;
    [[nodiscard]] SampleFilterEvaluation EvaluateSampleFilters() const;
    [[nodiscard]] bool active_labeling_filter_source_selected() const;
    [[nodiscard]] std::vector<SampleFilterSource> BuildSampleFilterSources() const;
    [[nodiscard]] SourceCollectionSessionAction ApplyLabelWriteResult(const SampleLabelWriteResult& result);

    SampleNavigationController navigation_;
    SampleLabelingController labeling_;
    SampleFilterController filters_;
    SnapshotLoader snapshot_loader_;
    SpectrumSnapshotHandle snapshot_;
    std::vector<SourceListEntry> sources_;
    std::optional<std::size_t> current_source_index_;
    std::optional<std::string> active_sample_workflow_identity_;
    std::optional<std::string> selected_labeling_filter_source_id_;
};

}  // namespace specforge
