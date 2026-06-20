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

struct SourceCollectionNavigationAction {
    SampleNavigationResult navigation;
    SourceCollectionSessionAction action;
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
    [[nodiscard]] bool AddReadOnlyAnnotationToActiveSource(
        const std::filesystem::path& path,
        std::string* message = nullptr);

    void ApplySampleFilters();
    [[nodiscard]] SampleFilterEvaluation EvaluateSampleFilters() const;
    void ClearFilters();
    void SetFilterCondition(std::string source_id, std::unordered_set<std::string> allowed_value_keys);
    void SetActiveLabelingFilterSourceSelected(bool selected);

    [[nodiscard]] bool active_labeling_filter_source_selected() const;
    [[nodiscard]] std::vector<SampleFilterSource> BuildSampleFilterSources() const;
    [[nodiscard]] bool can_add_read_only_annotation() const;

    void MaybeSaveStateCaches(std::uint64_t frame_index);
    [[nodiscard]] bool FlushStateCaches();

    [[nodiscard]] const SpectrumSnapshotHandle& snapshot() const;
    [[nodiscard]] const std::vector<SourceListEntry>& sources() const;
    [[nodiscard]] std::optional<std::size_t> current_source_index() const;
    [[nodiscard]] const SourceListEntry* current_source() const;

    [[nodiscard]] SampleNavigationController& navigation();
    [[nodiscard]] const SampleNavigationController& navigation() const;
    [[nodiscard]] SampleLabelingController& labeling();
    [[nodiscard]] const SampleLabelingController& labeling() const;
    [[nodiscard]] SampleFilterController& filters();
    [[nodiscard]] const SampleFilterController& filters() const;

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
    [[nodiscard]] std::size_t ActiveSampleCount() const;

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
