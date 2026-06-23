#pragma once

#include "domain/sample_filter.h"
#include "domain/spectrum_snapshot.h"
#include "ui/sample_labeling_controller.h"
#include "ui/sample_navigation_controller.h"
#include "ui/source_collection_session_types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace specforge {

struct SampleWorkflowCommandResult {
    SourceCollectionSessionAction action;
    SampleNavigationResult navigation;
    std::optional<std::size_t> snapshot_index_to_load;
};

class SampleWorkflowCoordinator {
public:
    SampleWorkflowCoordinator();
    SampleWorkflowCoordinator(
        std::filesystem::path navigation_state_cache_path,
        std::filesystem::path labeling_state_cache_path);

    [[nodiscard]] SourceCollectionSessionAction SyncActiveSource(
        std::optional<std::string> source_key,
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SourceCollectionSessionAction ClearActiveWorkflow();
    void RemoveSource(std::string_view source_key);

    [[nodiscard]] SampleWorkflowCommandResult RequestSampleNavigation(
        const SampleNavigationRequest& request,
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SourceCollectionSessionAction AddReadOnlyAnnotationToActiveSource(
        const std::filesystem::path& path,
        bool* loaded = nullptr,
        std::string* message = nullptr);
    [[nodiscard]] SourceCollectionSessionAction SetSampleNameQuery(std::string query);
    [[nodiscard]] SampleWorkflowCommandResult CommitSampleNameSelection(
        std::size_t target_row,
        std::string matched_name,
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SourceCollectionSessionAction CreateDefaultLabelingTask();
    [[nodiscard]] SourceCollectionSessionAction UpsertActiveLabel(
        SampleLabelDefinition label,
        bool* changed = nullptr);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingAutoAdvance(bool enabled);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingSkipLabeledOnAdvance(bool enabled);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingOutputPath(std::filesystem::path output_path);
    [[nodiscard]] SourceCollectionSessionAction DeactivateActiveLabelingTask();
    [[nodiscard]] SampleWorkflowCommandResult AssignActiveLabelToCurrentSample(
        const SpectrumSnapshotHandle& snapshot,
        int code);
    [[nodiscard]] SampleWorkflowCommandResult ClearActiveLabelForCurrentSample(
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SourceCollectionSessionAction ClearFilters(const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SourceCollectionSessionAction SetFilterValueSelected(
        const SpectrumSnapshotHandle& snapshot,
        std::string source_id,
        std::string value_key,
        bool selected);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingFilterSourceSelected(
        const SpectrumSnapshotHandle& snapshot,
        bool selected);

    [[nodiscard]] SourceCollectionNavigationView NavigationView(const SpectrumSnapshotHandle& snapshot) const;
    [[nodiscard]] SourceCollectionLabelingView LabelingView(const SpectrumSnapshotHandle& snapshot) const;
    [[nodiscard]] SourceCollectionFilterView FilterView(const SpectrumSnapshotHandle& snapshot) const;
    [[nodiscard]] bool can_add_read_only_annotation() const;
    [[nodiscard]] std::optional<std::size_t> current_index() const;

    void MaybeSaveStateCaches(std::uint64_t frame_index);
    [[nodiscard]] bool FlushStateCaches();

private:
    void SyncSampleWorkflowSession(const SpectrumSnapshotHandle& snapshot, SourceCollectionSessionAction& action);
    void ClearSampleWorkflow(SourceCollectionSessionAction& action);
    void ApplySampleFilters(const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] std::size_t ActiveSampleCount(const SpectrumSnapshotHandle& snapshot) const;
    [[nodiscard]] std::optional<std::size_t> ActiveSampleIndex(const SpectrumSnapshotHandle& snapshot) const;
    [[nodiscard]] SampleFilterEvaluation EvaluateSampleFilters(const SpectrumSnapshotHandle& snapshot) const;
    [[nodiscard]] bool active_labeling_filter_source_selected() const;
    [[nodiscard]] std::vector<SampleFilterSource> BuildSampleFilterSources() const;
    [[nodiscard]] SampleWorkflowCommandResult ApplyLabelWriteResult(
        const SpectrumSnapshotHandle& snapshot,
        const SampleLabelWriteResult& result);

    SampleNavigationController navigation_;
    SampleLabelingController labeling_;
    SampleFilterController filters_;
    std::optional<std::string> active_sample_workflow_identity_;
    std::optional<std::string> selected_labeling_filter_source_id_;
};

}  // namespace specforge
