#include "ui/sample_workflow_preparation.h"

#include "ui/sample_workflow_source_policy.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace specforge {
namespace {

void Checkpoint(const std::function<void()>& cancellation_checkpoint)
{
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
}

void ReconcilePreparedSequenceCurrent(SampleNavigationSequence& sequence)
{
    if (sequence.current_source_row || !sequence.active || sequence.ordered_rows.empty()) {
        return;
    }
    sequence.current_source_row = sequence.ordered_rows.front();
    sequence.current_sequence_position = 0;
    sequence.previous_target = sequence.ordered_rows.front();
    sequence.next_target = sequence.ordered_rows.size() > 1
        ? std::optional<std::size_t>{sequence.ordered_rows[1]}
        : sequence.current_source_row;
}

}  // namespace

PreparedSampleWorkflowState PrepareSampleWorkflowState(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionContext& context,
    std::size_t prepared_index,
    const SampleWorkflowPreparationPaths& paths,
    const std::function<void()>& cancellation_checkpoint)
{
    auto cache = std::make_shared<const SampleWorkflowPreparationCacheBundle>(
        LoadSampleWorkflowPreparationCacheBundle(
            paths,
            cancellation_checkpoint));
    PreparedSampleWorkflowState prepared =
        PrepareSampleWorkflowStateFromCache(
        snapshot,
        context,
        prepared_index,
        *cache,
        nullptr,
        nullptr,
        cancellation_checkpoint);
    prepared.preparation_cache = std::move(cache);
    return prepared;
}

SampleWorkflowPreparationCacheBundle LoadSampleWorkflowPreparationCacheBundle(
    const SampleWorkflowPreparationPaths& paths,
    const std::function<void()>& cancellation_checkpoint)
{
    Checkpoint(cancellation_checkpoint);
    SampleWorkflowPreparationCacheBundle bundle;
    bundle.labeling =
        LoadSampleLabelingStateCache(
            paths.labeling_state_cache_path,
            cancellation_checkpoint,
            paths.labeling_state_cache_load_policy);
    Checkpoint(cancellation_checkpoint);
    SampleWorkflowStateCacheLoadResult workflow =
        LoadSampleWorkflowStateCache(
            paths.workflow_state_cache_path,
            cancellation_checkpoint);
    bundle.workflow = std::move(workflow.cache);
    bundle.workflow_warning = std::move(workflow.warning);
    Checkpoint(cancellation_checkpoint);
    bundle.navigation =
        LoadSampleNavigationStateCache(
            paths.navigation_state_cache_path);
    Checkpoint(cancellation_checkpoint);
    return bundle;
}

PreparedSampleWorkflowState PrepareSampleWorkflowStateFromCache(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionContext& context,
    std::size_t prepared_index,
    const SampleWorkflowPreparationCacheBundle& cache,
    const SampleWorkflowSourceState* workflow_state_override,
    const SampleLabelingSourceState* labeling_state_override,
    const std::function<void()>& cancellation_checkpoint)
{
    Checkpoint(cancellation_checkpoint);
    if (snapshot.collection.spectrum_count != context.identity.spectrum_count) {
        throw std::invalid_argument("prepared workflow sample count does not match the source context");
    }
    PreparedSampleWorkflowState prepared;
    prepared.prepared_index = prepared_index;

    if (labeling_state_override != nullptr) {
        prepared.labeling_source_state = *labeling_state_override;
    }
    if (!prepared.labeling_source_state) {
        const auto source = cache.labeling.cache.sources.find(context.identity.id);
        if (source != cache.labeling.cache.sources.end()) {
            prepared.labeling_source_state = source->second;
        }
    }
    if (prepared.labeling_source_state) {
        if (prepared.labeling_source_state->sample_count != 0 &&
            prepared.labeling_source_state->sample_count != context.identity.spectrum_count) {
            prepared.labeling_source_state->tasks.clear();
            prepared.labeling_source_state->active_task_id.reset();
        }
    }
    Checkpoint(cancellation_checkpoint);

    if (workflow_state_override != nullptr) {
        prepared.workflow_source_state = *workflow_state_override;
    } else if (const auto source = cache.workflow.sources_by_identity.find(context.identity.id);
        source != cache.workflow.sources_by_identity.end()) {
        prepared.workflow_source_state = source->second;
    }
    Checkpoint(cancellation_checkpoint);

    SampleWorkflowSourcePolicy policy;
    policy.RestoreState(prepared.workflow_source_state);
    const std::vector<SampleLabelingTask>* labeling_tasks = prepared.labeling_source_state
        ? &prepared.labeling_source_state->tasks
        : nullptr;
    const SampleWorkflowSourceContext source_context{
        .collection = &context.manifest,
        .labeling_tasks = labeling_tasks,
        .sample_count = context.identity.spectrum_count,
    };
    prepared.filter_evaluation = policy.EvaluateFilters(source_context, cancellation_checkpoint);
    SampleWorkflowSortChoiceResult sorting =
        policy.BuildSortChoice(source_context, false, cancellation_checkpoint);
    prepared.sort_choice = std::move(sorting.choice);
    prepared.workflow_source_state = policy.StoreState();
    Checkpoint(cancellation_checkpoint);
    prepared.filter_view = policy.BuildFilterView(source_context, cancellation_checkpoint);
    prepared.filter_view.evaluation = prepared.filter_evaluation;
    prepared.filter_view.evaluation.included_samples.clear();
    Checkpoint(cancellation_checkpoint);
    prepared.sorting_view = policy.BuildSortingView(source_context, cancellation_checkpoint);
    Checkpoint(cancellation_checkpoint);

    const std::size_t sample_count = context.identity.spectrum_count;
    std::optional<std::size_t> requested_index;
    if (sample_count > 0) {
        requested_index = std::min(prepared_index, sample_count - 1);
    }
    SampleNavigationSequenceInput sequence_input;
    sequence_input.source_row_count = sample_count;
    sequence_input.sample_names = context.manifest.sample_names;
    sequence_input.filter_active = prepared.filter_evaluation.active;
    sequence_input.materialize_source_order = false;
    sequence_input.included_samples = &prepared.filter_evaluation.included_samples;
    sequence_input.sort_choice = prepared.sort_choice ? &*prepared.sort_choice : nullptr;
    sequence_input.current_source_row = requested_index;
    prepared.navigation_sequence =
        BuildSampleNavigationSequence(sequence_input, cancellation_checkpoint);
    ReconcilePreparedSequenceCurrent(prepared.navigation_sequence);
    prepared.current_index = prepared.navigation_sequence.current_source_row;
    if (prepared.filter_evaluation.active) {
        prepared.index_before_active_filter = requested_index;
    }
    Checkpoint(cancellation_checkpoint);
    return prepared;
}

}  // namespace specforge
