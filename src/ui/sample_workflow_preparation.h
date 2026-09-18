#pragma once

#include "domain/sample_filter.h"
#include "domain/source_collection_manifest.h"
#include "domain/spectrum_snapshot.h"
#include "ui/sample_labeling_state_cache_io.h"
#include "ui/sample_navigation_sequence.h"
#include "ui/sample_navigation_state_cache_io.h"
#include "ui/sample_workflow_state_cache_io.h"
#include "ui/source_collection_session_types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace specforge {

struct SampleWorkflowPreparationPaths {
    std::filesystem::path labeling_state_cache_path;
    std::filesystem::path workflow_state_cache_path;
    std::filesystem::path navigation_state_cache_path;
    SampleLabelingStateCacheLoadPolicy
        labeling_state_cache_load_policy =
            SampleLabelingStateCacheLoadPolicy::
                AllowPersistentOutputs;
    RuntimePaths runtime_paths;
};

struct SampleWorkflowPreparationCacheBundle {
    SampleLabelingStateCacheLoadResult labeling;
    SampleWorkflowStateCache workflow;
    std::string workflow_warning;
    SampleNavigationStateCacheLoadResult navigation;
};

[[nodiscard]] SampleWorkflowPreparationCacheBundle LoadSampleWorkflowPreparationCacheBundle(
    const SampleWorkflowPreparationPaths& paths,
    const std::function<void()>& cancellation_checkpoint = {});

struct PreparedSampleWorkflowState {
    // Keeps the immutable worker-loaded caches alive so the UI owner can adopt
    // them as an in-memory base without reopening either cache file.
    std::shared_ptr<const SampleWorkflowPreparationCacheBundle> preparation_cache;
    std::optional<SampleLabelingSourceState> labeling_source_state;
    SampleWorkflowSourceState workflow_source_state;
    SampleFilterEvaluation filter_evaluation;
    std::optional<SampleNavigationSortChoice> sort_choice;
    SampleNavigationSequence navigation_sequence;
    SourceCollectionFilterView filter_view;
    SourceCollectionSampleSortingView sorting_view;
    std::size_t prepared_index = 0;
    std::optional<std::size_t> current_index;
    std::optional<std::size_t> index_before_active_filter;
};

struct PreparedSourceCollectionPlan {
    SourceCollectionContext context;
    PreparedSampleWorkflowState workflow;
    std::optional<std::uint64_t> base_live_workflow_revision;
    // A startup external source-folder request remains authoritative through live
    // workflow reconciliation at session admission.
    std::optional<std::filesystem::path> preferred_member_path;
};

struct PreparedSourceCollectionReuse {
    SourceCollectionIdentity identity;
};

using PreparedSourceCollectionPayload =
    std::variant<PreparedSourceCollectionPlan, PreparedSourceCollectionReuse>;

[[nodiscard]] PreparedSampleWorkflowState PrepareSampleWorkflowState(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionContext& context,
    std::size_t prepared_index,
    const SampleWorkflowPreparationPaths& paths,
    const std::function<void()>& cancellation_checkpoint = {});
[[nodiscard]] PreparedSampleWorkflowState PrepareSampleWorkflowStateFromCache(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionContext& context,
    std::size_t prepared_index,
    const SampleWorkflowPreparationCacheBundle& cache,
    const SampleWorkflowSourceState* workflow_state_override = nullptr,
    const SampleLabelingSourceState* labeling_state_override = nullptr,
    const std::function<void()>& cancellation_checkpoint = {});

}  // namespace specforge
