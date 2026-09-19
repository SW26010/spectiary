#include "ui/source_collection_session.h"

#include "app/local_user_state.h"
#include "domain/source_path_identity.h"
#include "profile/navigation_latency_trace.h"
#include "ui/sample_labeling_state_cache_io.h"
#include "ui/sample_navigation_state_cache_io.h"
#include "ui/sample_workflow_coordinator.h"
#include "ui/sample_workflow_preparation.h"
#include "ui/sample_workflow_state_cache_io.h"
#include "ui/source_collection_roster.h"
#include "ui/source_collection_session_state_cache_io.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace spectiary {
namespace {

using namespace std::chrono_literals;

constexpr auto kSourceSessionSaveDebounce = 500ms;
constexpr auto kSourceSessionSaveRetry = 2s;

std::int64_t ElapsedNavigationResolutionNanoseconds(
    NavigationLatencyTimePoint started_at)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               NavigationLatencyTrace::Now() - started_at)
        .count();
}

std::string PathText(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::optional<SourceCollectionSampleTransitionReason>
PresentationReasonForNavigationRequest(
    SampleNavigationRequestKind kind)
{
    switch (kind) {
    case SampleNavigationRequestKind::Previous:
        return SourceCollectionSampleTransitionReason::Previous;
    case SampleNavigationRequestKind::Next:
        return SourceCollectionSampleTransitionReason::Next;
    case SampleNavigationRequestKind::LocateRow:
        return SourceCollectionSampleTransitionReason::LocateRow;
    case SampleNavigationRequestKind::LocateSequencePosition:
        return SourceCollectionSampleTransitionReason::
            LocateSequencePosition;
    case SampleNavigationRequestKind::LocateSourceRowInSequence:
        return SourceCollectionSampleTransitionReason::
            LocateSourceRowInSequence;
    case SampleNavigationRequestKind::LocateSampleName:
    case SampleNavigationRequestKind::LocateSampleNameMatch:
        return SourceCollectionSampleTransitionReason::LocateSampleName;
    case SampleNavigationRequestKind::RestoreLabelUndoPosition:
        return SourceCollectionSampleTransitionReason::Restore;
    case SampleNavigationRequestKind::LabelAdvance:
        // Labeling auto-advance is only presentation-valid when paired with
        // the label-write outcome that requested it.
        return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

class SourceCollectionSessionStatePersistence {
public:
    explicit SourceCollectionSessionStatePersistence(std::filesystem::path cache_path,
    const RuntimePaths& runtime_paths)
        : runtime_paths_(runtime_paths), cache_path_(std::move(cache_path)),
          persistence_(kSourceSessionSaveDebounce, kSourceSessionSaveRetry)
    {
    }

    [[nodiscard]] SourceCollectionSessionStateCache Load()
    {
        if (cache_path_.empty()) {
            return {};
        }
        SourceCollectionSessionStateCacheLoadResult result =
            LoadSourceCollectionSessionStateCache(runtime_paths_, cache_path_);
        persistence_.SetLoadWarning(std::move(result.warning));
        return std::move(result.cache);
    }

    void BeginRestore()
    {
        restoring_ = true;
    }

    void EndRestore()
    {
        restoring_ = false;
        if (dirty_after_restore_ && !cache_path_.empty()) {
            dirty_after_restore_ = false;
            persistence_.MarkDirty();
        }
    }

    void MarkDirty()
    {
        if (!restoring_ && !cache_path_.empty()) {
            persistence_.MarkDirty();
        }
    }

    void RunMaintenance(
        LocalUserStateSaveScheduler::TimePoint now,
        const std::vector<SourceCollectionSavedSource>& sources,
        std::optional<std::size_t> active_source_index)
    {
        (void)persistence_.RunMaintenance(
            now,
            [this, &sources, active_source_index] {
                return Save(sources, active_source_index);
            });
    }

    void MarkDirtyAfterRestore()
    {
        if (cache_path_.empty()) {
            return;
        }
        if (restoring_) {
            dirty_after_restore_ = true;
        } else {
            persistence_.MarkDirty();
        }
    }

    [[nodiscard]] std::optional<LocalUserStateSaveScheduler::TimePoint> NextMaintenanceDeadline() const
    {
        return persistence_.NextMaintenanceDeadline();
    }

    [[nodiscard]] bool Flush(
        const std::vector<SourceCollectionSavedSource>& sources,
        std::optional<std::size_t> active_source_index)
    {
        if (dirty_after_restore_) {
            persistence_.MarkDirty();
            if (persistence_.Flush(
                    [this, &sources, active_source_index] {
                        return Save(sources, active_source_index);
                    }) ==
                LocalUserStatePersistenceLifecycle::FlushOutcome::Failed) {
                return false;
            }
            dirty_after_restore_ = false;
        }
        return persistence_.Flush(
                   [this, &sources, active_source_index] {
                       return Save(sources, active_source_index);
                   }) !=
            LocalUserStatePersistenceLifecycle::FlushOutcome::Failed;
    }

    [[nodiscard]] LocalUserStatePersistenceStatus PersistenceStatus() const
    {
        return persistence_.PersistenceStatus();
    }

private:
    [[nodiscard]] LocalUserStatePersistenceLifecycle::SaveResult Save(
        const std::vector<SourceCollectionSavedSource>& sources,
        std::optional<std::size_t> active_source_index) const
    {
        SourceCollectionSessionStateCache cache;
        cache.sources = sources;
        cache.active_source_index = active_source_index;
        if (SaveSourceCollectionSessionStateCache(runtime_paths_, cache_path_, cache)) {
            return {.saved = true};
        }
        return {
            .saved = false,
            .error = "Could not save source session state.",
        };
    }

    RuntimePaths runtime_paths_;
    std::filesystem::path cache_path_;
    LocalUserStatePersistenceLifecycle persistence_;
    bool restoring_ = false;
    bool dirty_after_restore_ = false;
};

SourceCollectionIntent SourceCollectionIntent::SwitchActive(std::size_t source_index)
{
    SourceCollectionIntent intent;
    intent.kind = SourceCollectionIntentKind::SwitchActive;
    intent.source_index = source_index;
    return intent;
}

SourceCollectionIntent SourceCollectionIntent::Remove(std::size_t source_index)
{
    SourceCollectionIntent intent;
    intent.kind = SourceCollectionIntentKind::Remove;
    intent.source_index = source_index;
    return intent;
}

SourceCollectionIntent SourceCollectionIntent::AddReadOnlyAnnotationResult(std::filesystem::path path)
{
    SourceCollectionIntent intent;
    intent.kind = SourceCollectionIntentKind::AddReadOnlyAnnotationResult;
    intent.path = std::move(path);
    return intent;
}

SourceCollectionIntent SourceCollectionIntent::RemoveReadOnlyAnnotationResult(std::filesystem::path path)
{
    SourceCollectionIntent intent;
    intent.kind = SourceCollectionIntentKind::RemoveReadOnlyAnnotationResult;
    intent.path = std::move(path);
    return intent;
}

SourceCollectionIntent SourceCollectionIntent::RenameAnnotationResultDisplayName(
    std::filesystem::path path,
    std::string display_name)
{
    SourceCollectionIntent intent;
    intent.kind = SourceCollectionIntentKind::RenameAnnotationResultDisplayName;
    intent.path = std::move(path);
    intent.display_name = std::move(display_name);
    return intent;
}

SampleNavigationIntent SampleNavigationIntent::Move(SampleNavigationRequest request)
{
    SampleNavigationIntent intent;
    intent.kind = SampleNavigationIntentKind::Move;
    intent.request = std::move(request);
    return intent;
}

SampleNavigationIntent SampleNavigationIntent::SetSampleNameQuery(std::string query)
{
    SampleNavigationIntent intent;
    intent.kind = SampleNavigationIntentKind::SetSampleNameQuery;
    intent.query = std::move(query);
    return intent;
}

SampleNavigationIntent SampleNavigationIntent::CommitSampleNameSelection(
    std::size_t target_row,
    std::string matched_name)
{
    SampleNavigationIntent intent;
    intent.kind = SampleNavigationIntentKind::CommitSampleNameSelection;
    intent.target_row = target_row;
    intent.matched_name = std::move(matched_name);
    return intent;
}

ActiveSampleWorkflowIntent ActiveSampleWorkflowIntent::StartOrResumeTemporaryLabelingTask()
{
    ActiveSampleWorkflowIntent intent;
    intent.kind = ActiveSampleWorkflowIntentKind::StartOrResumeTemporaryLabelingTask;
    return intent;
}

ActiveSampleWorkflowIntent ActiveSampleWorkflowIntent::RecoverTemporaryLabelingTask(
    std::string source_identity,
    std::string task_id)
{
    ActiveSampleWorkflowIntent intent;
    intent.kind = ActiveSampleWorkflowIntentKind::RecoverTemporaryLabelingTask;
    intent.source_identity = std::move(source_identity);
    intent.task_id = std::move(task_id);
    return intent;
}

ActiveSampleWorkflowIntent ActiveSampleWorkflowIntent::DeleteTemporaryLabelingTask(
    std::string source_identity,
    std::string task_id)
{
    ActiveSampleWorkflowIntent intent;
    intent.kind = ActiveSampleWorkflowIntentKind::DeleteTemporaryLabelingTask;
    intent.source_identity = std::move(source_identity);
    intent.task_id = std::move(task_id);
    return intent;
}

ActiveSampleWorkflowIntent ActiveSampleWorkflowIntent::ActivateLabelingTaskFromAnnotation(
    std::filesystem::path annotation_path)
{
    ActiveSampleWorkflowIntent intent;
    intent.kind = ActiveSampleWorkflowIntentKind::ActivateLabelingTaskFromAnnotation;
    intent.path = std::move(annotation_path);
    return intent;
}

ActiveSampleWorkflowIntent ActiveSampleWorkflowIntent::DeleteActiveLabelingTask()
{
    ActiveSampleWorkflowIntent intent;
    intent.kind = ActiveSampleWorkflowIntentKind::DeleteActiveLabelingTask;
    return intent;
}

ActiveSampleWorkflowIntent ActiveSampleWorkflowIntent::RenameActiveLabelingTask(
    std::string expected_task_id,
    std::string requested_name)
{
    ActiveSampleWorkflowIntent intent;
    intent.kind =
        ActiveSampleWorkflowIntentKind::RenameActiveLabelingTask;
    intent.task_id = std::move(expected_task_id);
    intent.requested_name = std::move(requested_name);
    return intent;
}

ActiveSampleWorkflowIntent ActiveSampleWorkflowIntent::UpsertActiveLabel(SampleLabelDefinition label)
{
    ActiveSampleWorkflowIntent intent;
    intent.kind = ActiveSampleWorkflowIntentKind::UpsertActiveLabel;
    intent.label = std::move(label);
    return intent;
}

ActiveSampleWorkflowIntent ActiveSampleWorkflowIntent::UpdateActiveLabel(
    int original_code,
    SampleLabelDefinition label,
    bool allow_used_code_change)
{
    ActiveSampleWorkflowIntent intent;
    intent.kind = ActiveSampleWorkflowIntentKind::UpdateActiveLabel;
    intent.label_code = original_code;
    intent.label = std::move(label);
    intent.allow_used_label_code_change = allow_used_code_change;
    return intent;
}

ActiveSampleWorkflowIntent ActiveSampleWorkflowIntent::RemoveActiveLabel(int code)
{
    ActiveSampleWorkflowIntent intent;
    intent.kind = ActiveSampleWorkflowIntentKind::RemoveActiveLabel;
    intent.label_code = code;
    return intent;
}

ActiveSampleWorkflowIntent ActiveSampleWorkflowIntent::SetActiveLabelingAutoAdvance(bool enabled)
{
    ActiveSampleWorkflowIntent intent;
    intent.kind = ActiveSampleWorkflowIntentKind::SetActiveLabelingAutoAdvance;
    intent.enabled = enabled;
    return intent;
}

ActiveSampleWorkflowIntent ActiveSampleWorkflowIntent::SetActiveLabelingSkipLabeledOnAdvance(bool enabled)
{
    ActiveSampleWorkflowIntent intent;
    intent.kind = ActiveSampleWorkflowIntentKind::SetActiveLabelingSkipLabeledOnAdvance;
    intent.enabled = enabled;
    return intent;
}

ActiveSampleWorkflowIntent ActiveSampleWorkflowIntent::SetActiveLabelingOutputPath(
    std::filesystem::path output_path)
{
    ActiveSampleWorkflowIntent intent;
    intent.kind = ActiveSampleWorkflowIntentKind::SetActiveLabelingOutputPath;
    intent.path = std::move(output_path);
    return intent;
}

ActiveSampleWorkflowIntent ActiveSampleWorkflowIntent::ExportActiveLabels(
    std::filesystem::path output_path,
    SampleLabelExportFormat format)
{
    ActiveSampleWorkflowIntent intent;
    intent.kind =
        ActiveSampleWorkflowIntentKind::
            ExportActiveLabels;
    intent.path = std::move(output_path);
    intent.export_format = format;
    return intent;
}

ActiveSampleWorkflowIntent ActiveSampleWorkflowIntent::DeactivateActiveLabelingTask()
{
    ActiveSampleWorkflowIntent intent;
    intent.kind = ActiveSampleWorkflowIntentKind::DeactivateActiveLabelingTask;
    return intent;
}

ActiveSampleWorkflowIntent ActiveSampleWorkflowIntent::AssignActiveLabelToCurrentSample(int code)
{
    ActiveSampleWorkflowIntent intent;
    intent.kind = ActiveSampleWorkflowIntentKind::AssignActiveLabelToCurrentSample;
    intent.label_code = code;
    return intent;
}

ActiveSampleWorkflowIntent ActiveSampleWorkflowIntent::ClearActiveLabelForCurrentSample()
{
    ActiveSampleWorkflowIntent intent;
    intent.kind = ActiveSampleWorkflowIntentKind::ClearActiveLabelForCurrentSample;
    return intent;
}

ActiveSampleWorkflowIntent ActiveSampleWorkflowIntent::UndoLastLabelWrite()
{
    ActiveSampleWorkflowIntent intent;
    intent.kind = ActiveSampleWorkflowIntentKind::UndoLastLabelWrite;
    return intent;
}

SampleFilteringIntent SampleFilteringIntent::Clear()
{
    SampleFilteringIntent intent;
    intent.kind = SampleFilteringIntentKind::ClearFilters;
    return intent;
}

SampleFilteringIntent SampleFilteringIntent::AddSource(std::string source_id)
{
    SampleFilteringIntent intent;
    intent.kind = SampleFilteringIntentKind::AddFilterSource;
    intent.source_id = std::move(source_id);
    return intent;
}

SampleFilteringIntent SampleFilteringIntent::RemoveSource(std::string source_id)
{
    SampleFilteringIntent intent;
    intent.kind = SampleFilteringIntentKind::RemoveFilterSource;
    intent.source_id = std::move(source_id);
    return intent;
}

SampleFilteringIntent SampleFilteringIntent::SetFilterValueSelected(
    std::string source_id,
    std::string value_key,
    bool selected)
{
    SampleFilteringIntent intent;
    intent.kind = SampleFilteringIntentKind::SetFilterValueSelected;
    intent.source_id = std::move(source_id);
    intent.value_key = std::move(value_key);
    intent.selected = selected;
    return intent;
}

SampleSortingIntent SampleSortingIntent::Clear()
{
    SampleSortingIntent intent;
    intent.kind = SampleSortingIntentKind::ClearSorting;
    return intent;
}

SampleSortingIntent SampleSortingIntent::AddSource(std::string source_id)
{
    SampleSortingIntent intent;
    intent.kind = SampleSortingIntentKind::AddSortSource;
    intent.source_id = std::move(source_id);
    return intent;
}

SampleSortingIntent SampleSortingIntent::RemoveSource(std::string source_id)
{
    SampleSortingIntent intent;
    intent.kind = SampleSortingIntentKind::RemoveSortSource;
    intent.source_id = std::move(source_id);
    return intent;
}

SampleSortingIntent SampleSortingIntent::SetSortSource(std::string source_id)
{
    SampleSortingIntent intent;
    intent.kind = SampleSortingIntentKind::SetSortSource;
    intent.source_id = std::move(source_id);
    return intent;
}

SampleSortingIntent SampleSortingIntent::SetSortDirection(SampleNavigationSortDirection direction)
{
    SampleSortingIntent intent;
    intent.kind = SampleSortingIntentKind::SetSortDirection;
    intent.direction = direction;
    return intent;
}

SourceCollectionSessionIntent SourceCollectionSessionIntent::EditSourceCollection(SourceCollectionIntent intent)
{
    SourceCollectionSessionIntent session_intent;
    session_intent.kind = SourceCollectionSessionIntentKind::SourceCollection;
    session_intent.source_collection = std::move(intent);
    return session_intent;
}

SourceCollectionSessionIntent SourceCollectionSessionIntent::UpdateSampleNavigation(SampleNavigationIntent intent)
{
    SourceCollectionSessionIntent session_intent;
    session_intent.kind = SourceCollectionSessionIntentKind::SampleNavigation;
    session_intent.sample_navigation = std::move(intent);
    return session_intent;
}

SourceCollectionSessionIntent SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
    ActiveSampleWorkflowIntent intent)
{
    SourceCollectionSessionIntent session_intent;
    session_intent.kind = SourceCollectionSessionIntentKind::ActiveSampleWorkflow;
    session_intent.active_sample_workflow = std::move(intent);
    return session_intent;
}

SourceCollectionSessionIntent SourceCollectionSessionIntent::ApplySampleFiltering(SampleFilteringIntent intent)
{
    SourceCollectionSessionIntent session_intent;
    session_intent.kind = SourceCollectionSessionIntentKind::SampleFiltering;
    session_intent.sample_filtering = std::move(intent);
    return session_intent;
}

SourceCollectionSessionIntent SourceCollectionSessionIntent::ApplySampleSorting(SampleSortingIntent intent)
{
    SourceCollectionSessionIntent session_intent;
    session_intent.kind = SourceCollectionSessionIntentKind::SampleSorting;
    session_intent.sample_sorting = std::move(intent);
    return session_intent;
}

SourceCollectionSession::SourceCollectionSession()
    : SourceCollectionSession(
          std::filesystem::path{},
          std::filesystem::path{},
          std::filesystem::path{},
          std::filesystem::path{}, SampleLabelingStateCacheLoadPolicy::AllowPersistentOutputs, RuntimePaths{})
{
}

SourceCollectionSession::SourceCollectionSession(
    std::filesystem::path source_session_state_cache_path,
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path,
    std::filesystem::path workflow_state_cache_path,
    SampleLabelingStateCacheLoadPolicy
        labeling_state_cache_load_policy,
    const RuntimePaths& runtime_paths)
    : SourceCollectionSession(
          std::move(source_session_state_cache_path),
          std::move(navigation_state_cache_path),
          std::move(labeling_state_cache_path),
          std::move(workflow_state_cache_path),
          labeling_state_cache_load_policy,
          {}, runtime_paths)
{
}

SourceCollectionSession::SourceCollectionSession(
    std::filesystem::path source_session_state_cache_path,
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path,
    std::filesystem::path workflow_state_cache_path,
    SampleLabelingStateCacheLoadPolicy
        labeling_state_cache_load_policy,
    SampleLabelingController::CanonicalDocumentPublisher
        canonical_document_publisher,
    const RuntimePaths& runtime_paths)
    : SourceCollectionSession(
          std::move(source_session_state_cache_path),
          std::move(navigation_state_cache_path),
          std::move(labeling_state_cache_path),
          std::move(workflow_state_cache_path),
          labeling_state_cache_load_policy,
          std::move(canonical_document_publisher),
          SampleLabelingController::CanonicalValuesPublisher{}, runtime_paths)
{
}

SourceCollectionSession::SourceCollectionSession(
    std::filesystem::path source_session_state_cache_path,
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path,
    std::filesystem::path workflow_state_cache_path,
    SampleLabelingStateCacheLoadPolicy
        labeling_state_cache_load_policy,
    SampleLabelingController::CanonicalDocumentPublisher
        canonical_document_publisher,
    SampleLabelingController::CanonicalValuesPublisher
        canonical_values_publisher,
    const RuntimePaths& runtime_paths)
    : roster_(std::make_unique<SourceCollectionRoster>()),
      workflow_(std::make_unique<SampleWorkflowCoordinator>(
          std::move(navigation_state_cache_path),
          std::move(labeling_state_cache_path),
          std::move(workflow_state_cache_path),
          [labeling_state_cache_load_policy, runtime_paths](
              const std::filesystem::path& path) {
              return LoadSampleLabelingStateCache(
                  runtime_paths,
                  path,
                  {},
                  labeling_state_cache_load_policy);
          },
          [runtime_paths](const std::filesystem::path& path) {
              return LoadSampleWorkflowStateCache(
                  runtime_paths,
                  path, {});
          },
          std::move(canonical_document_publisher),
          std::move(canonical_values_publisher), runtime_paths)),
      source_session_state_(std::make_unique<SourceCollectionSessionStatePersistence>(
          std::move(source_session_state_cache_path), runtime_paths))
{
    workflow_->SetDeferredSampleNavigation(true);
    PrepareDeferredSourceSessionRestore();
}

SourceCollectionSession::~SourceCollectionSession() = default;

SourceCollectionSession::SourceCollectionSession(SourceCollectionSession&&) noexcept = default;

SourceCollectionSession& SourceCollectionSession::operator=(SourceCollectionSession&&) noexcept = default;

bool SourceCollectionSession::SupersedesPendingSourceActivation(
    const SourceCollectionSessionIntent& intent) const
{
    if (intent.kind != SourceCollectionSessionIntentKind::SourceCollection) {
        return false;
    }

    switch (intent.source_collection.kind) {
    case SourceCollectionIntentKind::SwitchActive: {
        const std::size_t target_index = intent.source_collection.source_index;
        const std::optional<std::size_t> current_index = roster_->current_source_index();
        return roster_->can_activate_source(target_index) &&
               (!current_index || *current_index != target_index);
    }
    case SourceCollectionIntentKind::Remove:
        return roster_->current_source_index() == intent.source_collection.source_index;
    case SourceCollectionIntentKind::AddReadOnlyAnnotationResult:
    case SourceCollectionIntentKind::RemoveReadOnlyAnnotationResult:
    case SourceCollectionIntentKind::RenameAnnotationResultDisplayName:
        return false;
    }
    return false;
}

SourceCollectionSessionResult SourceCollectionSession::Submit(
    SourceCollectionSessionIntent intent,
    NavigationTargetResolutionReport* target_resolution)
{
    const std::optional<std::size_t> presented_sample_before =
        PresentedSampleIndex();
    const std::optional<std::size_t> effective_sample_before =
        EffectiveSampleNavigationIndex();
    const std::optional<std::string> source_key_before =
        roster_->current_source_key();
    std::optional<SourceCollectionSampleTransitionReason>
        requested_navigation_reason;
    bool source_activation_intent = false;
    bool navigation_reconciliation_intent = false;
    switch (intent.kind) {
    case SourceCollectionSessionIntentKind::SourceCollection:
        source_activation_intent =
            intent.source_collection.kind ==
                SourceCollectionIntentKind::SwitchActive ||
            intent.source_collection.kind ==
                SourceCollectionIntentKind::Remove;
        navigation_reconciliation_intent =
            intent.source_collection.kind ==
                SourceCollectionIntentKind::AddReadOnlyAnnotationResult ||
            intent.source_collection.kind ==
                SourceCollectionIntentKind::RemoveReadOnlyAnnotationResult;
        break;
    case SourceCollectionSessionIntentKind::SampleNavigation:
        if (intent.sample_navigation.kind ==
            SampleNavigationIntentKind::Move) {
            requested_navigation_reason =
                PresentationReasonForNavigationRequest(
                    intent.sample_navigation.request.kind);
        } else if (intent.sample_navigation.kind ==
                   SampleNavigationIntentKind::
                       CommitSampleNameSelection) {
            requested_navigation_reason =
                SourceCollectionSampleTransitionReason::
                    LocateSampleName;
        }
        break;
    case SourceCollectionSessionIntentKind::ActiveSampleWorkflow:
        navigation_reconciliation_intent = true;
        if (intent.active_sample_workflow.kind ==
            ActiveSampleWorkflowIntentKind::UndoLastLabelWrite) {
            requested_navigation_reason =
                SourceCollectionSampleTransitionReason::Restore;
        }
        break;
    case SourceCollectionSessionIntentKind::SampleFiltering:
    case SourceCollectionSessionIntentKind::SampleSorting:
        navigation_reconciliation_intent = true;
        break;
    }
    const NavigationLatencyTimePoint pending_activation_started_at =
        target_resolution != nullptr ? NavigationLatencyTrace::Now()
                                     : NavigationLatencyTimePoint{};
    const std::optional<std::size_t> pending_sample_index_before =
        workflow_->pending_sample_index();
    if (target_resolution != nullptr) {
        target_resolution->pending_present =
            pending_sample_index_before.has_value();
    }
    const std::optional<std::filesystem::path> pending_source_path_before =
        pending_sample_index_before && roster_->snapshot()
        ? std::optional<std::filesystem::path>{roster_->snapshot()->source.path}
        : std::nullopt;
    pending_background_spectrum_index_.reset();
    if (SupersedesPendingSourceActivation(intent)) {
        workflow_->CancelDeferredSampleNavigation();
    }
    if (target_resolution != nullptr) {
        target_resolution->pending_activation_supersede_ns +=
            ElapsedNavigationResolutionNanoseconds(
                pending_activation_started_at);
    }
    if (intent.kind != SourceCollectionSessionIntentKind::SampleNavigation) {
        workflow_->DiscardPreparedViewCaches();
    }
    SourceCollectionSessionResult result;
    SampleWorkflowTransitionOutcome transition;
    switch (intent.kind) {
    case SourceCollectionSessionIntentKind::SourceCollection:
        switch (intent.source_collection.kind) {
        case SourceCollectionIntentKind::SwitchActive:
            transition =
                ActivateSource(
                    intent.source_collection.source_index);
            break;
        case SourceCollectionIntentKind::Remove:
            transition = RemoveSource(
                intent.source_collection.source_index,
                result.background_retirement,
                &result.canceled_source_follow_up_path);
            break;
        case SourceCollectionIntentKind::AddReadOnlyAnnotationResult: {
            transition = workflow_->Apply(
                std::move(intent.source_collection),
                roster_->snapshot());
            if (transition.loaded) {
                MarkSourceSessionCacheDirty();
            }
            break;
        }
        case SourceCollectionIntentKind::RemoveReadOnlyAnnotationResult: {
            transition = workflow_->Apply(
                std::move(intent.source_collection),
                roster_->snapshot());
            if (transition.action.navigation_inputs_changed ||
                transition.action.workflow_changed ||
                transition.action.snapshot_changed) {
                MarkSourceSessionCacheDirty();
            }
            break;
        }
        case SourceCollectionIntentKind::RenameAnnotationResultDisplayName:
            transition = workflow_->Apply(
                std::move(intent.source_collection),
                roster_->snapshot());
            break;
        }
        break;
    case SourceCollectionSessionIntentKind::SampleNavigation:
        transition = workflow_->Apply(
            std::move(intent.sample_navigation),
            roster_->snapshot(),
            target_resolution);
        break;
    case SourceCollectionSessionIntentKind::ActiveSampleWorkflow:
        transition = workflow_->Apply(
            std::move(intent.active_sample_workflow),
            roster_->snapshot(),
            target_resolution);
        break;
    case SourceCollectionSessionIntentKind::SampleFiltering:
        transition = workflow_->Apply(
            std::move(intent.sample_filtering),
            roster_->snapshot());
        break;
    case SourceCollectionSessionIntentKind::SampleSorting:
        transition = workflow_->Apply(
            std::move(intent.sample_sorting),
            roster_->snapshot());
        break;
    }
    ApplyWorkflowTransitionOutcome(
        result,
        std::move(transition));
    const NavigationLatencyTimePoint pending_follow_up_started_at =
        target_resolution != nullptr ? NavigationLatencyTrace::Now()
                                     : NavigationLatencyTimePoint{};
    FinalizePendingSourceFollowUp(
        result,
        pending_sample_index_before,
        pending_source_path_before);
    if (target_resolution != nullptr) {
        target_resolution->pending_activation_supersede_ns +=
            ElapsedNavigationResolutionNanoseconds(
                pending_follow_up_started_at);
    }
    if (const std::optional<SourceCollectionIdentity> active_identity =
            workflow_->ActiveSourceIdentity();
        active_identity && !active_identity->id.empty()) {
        ++live_workflow_revisions_[active_identity->id];
    }
    const std::optional<std::size_t> effective_sample_after =
        EffectiveSampleNavigationIndex();
    const std::optional<std::string> source_key_after =
        roster_->current_source_key();
    bool presentation_transition_recorded = false;
    const bool explicit_navigation_selected_target =
        requested_navigation_reason &&
        *requested_navigation_reason !=
            SourceCollectionSampleTransitionReason::Previous &&
        *requested_navigation_reason !=
            SourceCollectionSampleTransitionReason::Next;
    if (result.label_write &&
        result.label_write->write.changed &&
        result.label_write->write.advance_requested &&
        result.navigation.has_active_source &&
        result.navigation.target_found &&
        result.label_write->write.sample_index !=
            result.navigation.current_index) {
        const SampleLabelWriteResult& write =
            result.label_write->write;
        RecordSampleTransition(
            SourceCollectionSampleTransitionReason::
                LabelingAutoAdvance,
            write.sample_index,
            result.navigation.current_index,
            write.current_code);
        presentation_transition_recorded = true;
    } else if (requested_navigation_reason &&
               result.navigation.has_active_source &&
               result.navigation.target_found &&
               (effective_sample_after != effective_sample_before ||
                explicit_navigation_selected_target)) {
        if (presented_sample_before ==
            result.navigation.current_index) {
            presentation_transition_recorded =
                sample_transition_.has_value();
            sample_transition_.reset();
        } else {
            const bool already_recorded =
                sample_transition_ &&
                sample_transition_->reason ==
                    *requested_navigation_reason &&
                sample_transition_->from_sample_index ==
                    presented_sample_before &&
                sample_transition_->current_sample_index ==
                    result.navigation.current_index &&
                !sample_transition_->accepted_label_value;
            if (!already_recorded) {
                RecordSampleTransition(
                    *requested_navigation_reason,
                    presented_sample_before,
                    result.navigation.current_index);
                presentation_transition_recorded = true;
            }
        }
    } else if (source_activation_intent &&
               source_key_after != source_key_before) {
        RecordSampleTransition(
            deferred_restore_active_
                ? SourceCollectionSampleTransitionReason::Restore
                : SourceCollectionSampleTransitionReason::
                      SourceActivation,
            std::nullopt,
            PresentedSampleIndex());
        presentation_transition_recorded = true;
    } else if (navigation_reconciliation_intent &&
               result.action.navigation_inputs_changed) {
        RecordSampleTransition(
            SourceCollectionSampleTransitionReason::
                NavigationInputReconciliation,
            presented_sample_before,
            effective_sample_after);
        presentation_transition_recorded = true;
    } else if (result.action.navigation_inputs_changed &&
               effective_sample_after != effective_sample_before) {
        RecordSampleTransition(
            SourceCollectionSampleTransitionReason::
                NavigationInputReconciliation,
            presented_sample_before,
            effective_sample_after);
        presentation_transition_recorded = true;
    }
    result.view_invalidated =
        result.view_invalidated ||
        presentation_transition_recorded ||
        result.action.source_roster_changed ||
        result.action.snapshot_changed ||
        result.action.workflow_changed ||
        result.action.navigation_inputs_changed;
    if (result.view_invalidated) {
        InvalidateView();
    }
    AppendPendingBackgroundRetirement(
        result.background_retirement);
    return result;
}

void SourceCollectionSession::FinalizePendingSourceFollowUp(
    SourceCollectionSessionResult& result,
    std::optional<std::size_t> pending_sample_index_before,
    const std::optional<std::filesystem::path>&
        pending_source_path_before)
{
    const std::optional<std::size_t> pending_sample_index_after =
        workflow_->pending_sample_index();
    if (pending_sample_index_after == pending_sample_index_before &&
        pending_background_spectrum_index_ ==
            pending_sample_index_before) {
        pending_background_spectrum_index_.reset();
    }
    result.follow_up_spectrum_index = std::exchange(
        pending_background_spectrum_index_,
        std::nullopt);
    const SpectrumSnapshotHandle active_snapshot_after =
        roster_->snapshot();
    const bool previous_source_follow_up_retained =
        pending_sample_index_before &&
        pending_sample_index_after ==
            pending_sample_index_before &&
        pending_source_path_before &&
        active_snapshot_after &&
        SourcePathIdentityKey(*pending_source_path_before) ==
            SourcePathIdentityKey(
                active_snapshot_after->source.path);
    if (pending_sample_index_before &&
        !previous_source_follow_up_retained) {
        result.canceled_source_follow_up_path =
            pending_source_path_before;
    }
}

const SourceCollectionSessionView& SourceCollectionSession::View()
{
    if (session_view_cache_ &&
        cached_session_view_revision_ ==
            session_view_revision_) {
        return *session_view_cache_;
    }
    if (session_view_cache_) {
        pending_session_view_retirement_.push_back(
            std::move(session_view_cache_));
    }
    const SpectrumSnapshotHandle& snapshot = roster_->snapshot();
    SourceCollectionSessionView view;
    view.snapshot = snapshot;
    view.current_source_index = roster_->current_source_index();
    view.sources = roster_->SourceViews();
    view.can_add_read_only_annotation = workflow_->can_add_read_only_annotation();
    view.navigation = workflow_->NavigationView(snapshot);
    const bool snapshot_matches_navigation =
        snapshot && view.navigation.current_index && snapshot->collection.spectrum_count > 0 &&
        snapshot->collection.current_index == *view.navigation.current_index;
    view.current_sample_snapshot = snapshot_matches_navigation ? snapshot : nullptr;
    view.labeling = workflow_->LabelingView(view.current_sample_snapshot);
    view.filter = workflow_->BuildFilterView(snapshot);
    view.sorting = workflow_->BuildSortingView(snapshot);
    if (sample_transition_ &&
        (!sample_transition_->current_sample_index ||
         (view.current_sample_snapshot &&
          view.current_sample_snapshot->collection.current_index ==
              *sample_transition_->current_sample_index))) {
        view.sample_transition = sample_transition_;
    }
    view.persistence = PersistenceHealth();
    session_view_cache_ =
        std::make_shared<SourceCollectionSessionView>(
            std::move(view));
    cached_session_view_revision_ =
        session_view_revision_;
    return *session_view_cache_;
}

ExactSampleNameResolution
SourceCollectionSession::ResolveExactSampleName(
    std::string_view name) const
{
    return workflow_->ResolveExactSampleName(name);
}

SpectrumSnapshotHandle SourceCollectionSession::CurrentSampleSnapshot() const
{
    const SpectrumSnapshotHandle& snapshot = roster_->snapshot();
    const std::optional<std::size_t> current_index = workflow_->current_index();
    if (!snapshot || snapshot->source.path.empty() || !current_index ||
        snapshot->collection.spectrum_count == 0 || snapshot->collection.current_index != *current_index) {
        return nullptr;
    }
    return snapshot;
}

std::optional<std::size_t> SourceCollectionSession::EffectiveSampleNavigationIndex() const
{
    if (!workflow_) {
        return std::nullopt;
    }
    if (const std::optional<std::size_t> pending = workflow_->pending_sample_index()) {
        return pending;
    }
    return workflow_->current_index();
}

SpectrumSnapshotHandle SourceCollectionSession::CurrentSourceSnapshot() const
{
    return roster_->snapshot();
}

std::optional<std::string>
SourceCollectionSession::CurrentSourceCollectionIdentity() const
{
    return roster_->current_source_collection_identity();
}

std::vector<std::filesystem::path> SourceCollectionSession::AnnotationPathsForSource(
    const std::filesystem::path& path) const
{
    const std::string source_key = SourcePathIdentityKey(path);
    std::vector<std::filesystem::path> paths =
        workflow_->AnnotationPathsForSourceKey(source_key);
    for (const SourceCollectionSavedSource& unresolved :
         unresolved_deferred_restore_sources_) {
        if (SourcePathIdentityKey(unresolved.path) != source_key) {
            continue;
        }
        for (const std::filesystem::path& annotation_path :
             unresolved.annotation_paths) {
            if (annotation_path.empty() ||
                std::any_of(
                    paths.begin(),
                    paths.end(),
                    [&annotation_path](
                        const std::filesystem::path& existing) {
                        return SourcePathIdentityKey(existing) ==
                               SourcePathIdentityKey(annotation_path);
                    })) {
                continue;
            }
            paths.push_back(annotation_path);
        }
    }
    return paths;
}

std::optional<SourceCollectionLoadHint> SourceCollectionSession::LoadHintForSource(
    const std::filesystem::path& path,
    std::optional<std::size_t> spectrum_index)
{
    const std::string source_key = SourcePathIdentityKey(path);
    const std::optional<SourceCollectionIdentity> identity = workflow_->KnownSourceIdentity(source_key);
    const std::optional<std::size_t> current_index = workflow_->KnownSourceCurrentIndex(source_key);
    if (!identity || !current_index) {
        return std::nullopt;
    }
    const auto revision = live_workflow_revisions_.find(identity->id);
    std::optional<SourceCollectionResidentSnapshot> resident_snapshot;
    if (spectrum_index) {
        resident_snapshot =
            roster_->ResidentSnapshot(path, *spectrum_index, *identity);
    }
    const std::uint64_t live_workflow_revision =
        revision == live_workflow_revisions_.end() ? 0 : revision->second;
    SourceCollectionFolderListingGenerationHandle folder_generation =
        roster_->FolderListingGeneration(path);
    const std::optional<SourceCollectionContextReuseProof> proof =
        roster_->ContextReuseProof(path);
    SourceCollectionReuseCandidate reuse =
        proof
        ? SourceCollectionReuseCandidate::Verified(
              *proof,
              live_workflow_revision,
              std::move(folder_generation),
              std::move(resident_snapshot))
        : SourceCollectionReuseCandidate::Known(
              *identity,
              live_workflow_revision,
              std::move(folder_generation));
    return SourceCollectionLoadHint{
        std::move(reuse),
        *current_index,
    };
}

std::optional<SourceCollectionSnapshotPrefetchPlan>
SourceCollectionSession::PlanSnapshotPrefetch(
    SampleNavigationDirection direction,
    SampleNavigationPrefetchPolicy policy)
{
    const SpectrumSnapshotHandle snapshot = CurrentSampleSnapshot();
    if (!snapshot || snapshot->source.path.empty() ||
        workflow_->pending_sample_index()) {
        return std::nullopt;
    }

    const std::vector<std::size_t> rows =
        workflow_->AdjacentNavigationRows(direction, policy);
    if (rows.empty()) {
        return std::nullopt;
    }

    const std::size_t spectrum_index = rows.front();
    std::optional<SourceCollectionLoadHint> hint =
        LoadHintForSource(snapshot->source.path, spectrum_index);
    if (!hint || !hint->reuse.context_reuse_proof() ||
        hint->reuse.resident_snapshot()) {
        return std::nullopt;
    }
    return SourceCollectionSnapshotPrefetchPlan{
        snapshot->source.path,
        spectrum_index,
        AnnotationPathsForSource(snapshot->source.path),
        std::move(*hint),
    };
}

SourceCollectionSnapshotPrefetchStoreResult
SourceCollectionSession::StorePrefetchedSnapshot(
    const std::filesystem::path& path,
    SourceCollectionResidentSnapshot resident)
{
    SourceCollectionSnapshotPrefetchStoreResult result;
    const std::optional<SourceCollectionIdentity> active_identity =
        workflow_->ActiveSourceIdentity();
    const SpectrumSnapshotHandle active_snapshot = CurrentSampleSnapshot();
    if (!active_identity || !active_snapshot ||
        SourcePathIdentityKey(active_snapshot->source.path) !=
            SourcePathIdentityKey(path) ||
        *active_identity != resident.context_reuse_proof.identity) {
        if (resident.snapshot) {
            result.background_retirement.push_back(
                std::move(resident.snapshot));
        }
        return result;
    }

    SourceCollectionRosterResidentRetainResult retain =
        roster_->RetainPrefetchedSnapshot(
            path,
            std::move(resident));
    result.stored = retain.retained;
    if (!retain.retired_snapshots.empty()) {
        result.background_retirement.push_back(
            MakeBackgroundRetirementHandle(
                std::move(retain.retired_snapshots)));
    }
    return result;
}

SampleWorkflowTransitionOutcome
SourceCollectionSession::ActivateSource(
    std::size_t source_index)
{
    if (!roster_->can_activate_source(source_index)) {
        return {};
    }

    SampleWorkflowTransitionOutcome outcome;
    outcome.action = roster_->ActivateSource(source_index);
    MergeSampleWorkflowTransitionOutcome(
        outcome,
        workflow_->SyncKnownActiveSource(
            roster_->current_source_key(),
            roster_->snapshot()));
    MarkSourceSessionCacheDirty();
    return outcome;
}

SampleWorkflowTransitionOutcome SourceCollectionSession::RemoveSource(
    std::size_t source_index,
    std::vector<BackgroundRetirementHandle>& background_retirement,
    std::optional<std::filesystem::path>* canceled_source_follow_up_path)
{
    SampleWorkflowTransitionOutcome outcome;
    SourceCollectionRosterRemoveResult remove_result = roster_->RemoveSource(source_index);
    MergeSourceCollectionSessionAction(
        outcome.action,
        remove_result.action);
    if (!remove_result.removed) {
        return outcome;
    }
    outcome.action.source_roster_changed = true;
    (void)ForgetUnresolvedSourceIntent(remove_result.removed_path);

    if (canceled_source_follow_up_path != nullptr) {
        *canceled_source_follow_up_path = remove_result.removed_path;
    }

    MarkSourceSessionCacheDirty();
    if (!remove_result.retired_snapshots.empty()) {
        background_retirement.push_back(
            MakeBackgroundRetirementHandle(std::move(remove_result.retired_snapshots)));
    }
    if (remove_result.retired_folder_listing_generation) {
        background_retirement.push_back(
            std::move(remove_result.retired_folder_listing_generation));
    }
    if (BackgroundRetirementHandle retired_workflow =
            workflow_->RemoveSource(remove_result.removed_source_key)) {
        background_retirement.push_back(std::move(retired_workflow));
    }
    if (remove_result.removed_current) {
        MergeSampleWorkflowTransitionOutcome(
            outcome,
            workflow_->ClearActiveWorkflow());
        MergeSampleWorkflowTransitionOutcome(
            outcome,
            workflow_->SyncKnownActiveSource(
                roster_->current_source_key(),
                roster_->snapshot()));
    }
    return outcome;
}

SourceCollectionSessionResult SourceCollectionSession::OpenPreparedSource(
    std::filesystem::path path,
    std::size_t spectrum_index,
    SpectrumSnapshotHandle snapshot,
    PreparedSourceCollectionPayload payload,
    SourceCollectionFolderListingGenerationHandle folder_listing_generation,
    std::optional<SourceCollectionContextReuseProof> context_reuse_proof)
{
    SourceCollectionSessionResult result;
    const auto retire_folder_listing_generation = [&]() {
        if (folder_listing_generation) {
            result.background_retirement.push_back(
                std::move(folder_listing_generation));
        }
    };
    if (const auto* reuse = std::get_if<PreparedSourceCollectionReuse>(&payload);
        reuse != nullptr && !workflow_->CanReusePreparedKnownSource(
                                SourcePathIdentityKey(path),
                                reuse->identity)) {
        if (snapshot) {
            result.background_retirement.push_back(std::move(snapshot));
        }
        retire_folder_listing_generation();
        result.load_error.kind =
            SourceCollectionLoadErrorKind::
                PreparedReuseTargetUnavailable;
        return result;
    }
    SpectrumSnapshotHandle previous_snapshot = roster_->snapshot();
    const std::optional<std::size_t> previous_sample_index =
        PresentedSampleIndex();
    std::optional<PendingSampleNavigation> pending_navigation =
        workflow_->pending_sample_navigation();
    const std::string prepared_path_key = SourcePathIdentityKey(path);
    const bool prepared_for_presented_source =
        previous_snapshot &&
        SourcePathIdentityKey(previous_snapshot->source.path) == prepared_path_key;
    const bool completes_pending_navigation =
        pending_navigation && pending_navigation->spectrum_index == spectrum_index &&
        prepared_for_presented_source;
    auto* prepared_plan = std::get_if<PreparedSourceCollectionPlan>(&payload);
    const std::optional<SourceCollectionIdentity> known_identity =
        workflow_->KnownSourceIdentity(prepared_path_key);
    const auto live_revision = prepared_plan == nullptr
        ? live_workflow_revisions_.end()
        : live_workflow_revisions_.find(prepared_plan->context.identity.id);
    const std::uint64_t current_live_revision =
        live_revision == live_workflow_revisions_.end() ? 0 : live_revision->second;

    const bool derives_from_known_source =
        prepared_plan != nullptr && prepared_plan->base_live_workflow_revision.has_value();
    const bool known_source_plan_is_current =
        derives_from_known_source && snapshot && known_identity &&
        known_identity->id == prepared_plan->context.identity.id &&
        known_identity->spectrum_count == prepared_plan->context.identity.spectrum_count &&
        current_live_revision >= *prepared_plan->base_live_workflow_revision;
    if (derives_from_known_source && !known_source_plan_is_current) {
        if (snapshot) {
            result.background_retirement.push_back(std::move(snapshot));
        }
        result.background_retirement.push_back(
            MakeBackgroundRetirementHandle(std::move(payload)));
        retire_folder_listing_generation();
        result.load_error.kind =
            SourceCollectionLoadErrorKind::
                PreparedKnownSourcePlanStale;
        return result;
    }

    if (known_source_plan_is_current) {
        std::optional<SampleWorkflowSourceState> live_workflow_state =
            workflow_->WorkflowStateForSourceIdentity(prepared_plan->context.identity.id);
        std::optional<SampleLabelingSourceState> live_labeling_state =
            workflow_->LabelingStateForSourceIdentity(prepared_plan->context.identity.id);
        std::shared_ptr<const SampleWorkflowPreparationCacheBundle> preparation_cache =
            prepared_plan->workflow.preparation_cache;
        const SampleWorkflowPreparationCacheBundle empty_cache;
        PreparedSampleWorkflowState reconciled_workflow =
            PrepareSampleWorkflowStateFromCache(
                *snapshot,
                prepared_plan->context,
                spectrum_index,
                preparation_cache ? *preparation_cache : empty_cache,
                live_workflow_state ? &*live_workflow_state : nullptr,
                live_labeling_state ? &*live_labeling_state : nullptr);
        reconciled_workflow.preparation_cache = std::move(preparation_cache);
        result.background_retirement.push_back(
            MakeBackgroundRetirementHandle(std::move(prepared_plan->workflow)));
        prepared_plan->workflow = std::move(reconciled_workflow);
    }

    if (prepared_plan != nullptr &&
        prepared_plan->preferred_member_path &&
        (!prepared_plan->workflow.current_index ||
         *prepared_plan->workflow.current_index != spectrum_index)) {
        const std::string preferred_member_path =
            PathText(*prepared_plan->preferred_member_path);
        if (snapshot) {
            result.background_retirement.push_back(std::move(snapshot));
        }
        result.background_retirement.push_back(
            MakeBackgroundRetirementHandle(std::move(payload)));
        retire_folder_listing_generation();
        result.load_error.kind =
            SourceCollectionLoadErrorKind::
                BackgroundLoadingFailed;
        result.load_error.diagnostic_detail =
            "The requested external source member is excluded by "
            "the active sample filter: " +
            preferred_member_path;
        return result;
    }

    if (prepared_plan != nullptr && completes_pending_navigation &&
        prepared_plan->workflow.current_index != spectrum_index) {
        const std::optional<std::size_t> reconciled_index =
            prepared_plan->workflow.current_index;
        if (snapshot) {
            result.background_retirement.push_back(std::move(snapshot));
        }
        result.background_retirement.push_back(
            MakeBackgroundRetirementHandle(std::move(payload)));
        if (reconciled_index &&
            workflow_->RetargetDeferredSampleNavigation(*reconciled_index)) {
            result.follow_up_spectrum_index = *reconciled_index;
            result.loaded = true;
        } else {
            workflow_->CancelDeferredSampleNavigation();
            pending_background_spectrum_index_.reset();
            result.canceled_source_follow_up_path = previous_snapshot->source.path;
            result.load_error.kind =
                SourceCollectionLoadErrorKind::
                    PreparedNavigationUnavailable;
        }
        if (reconciled_index) {
            RecordSampleTransition(
                SourceCollectionSampleTransitionReason::
                    NavigationInputReconciliation,
                previous_sample_index,
                reconciled_index);
        } else {
            sample_transition_.reset();
        }
        result.view_invalidated = true;
        InvalidateView();
        retire_folder_listing_generation();
        return result;
    }

    SourceCollectionRosterOpenResult roster_result =
        roster_->OpenPreparedSource(
            path,
            spectrum_index,
            std::move(snapshot),
            std::move(folder_listing_generation),
            std::move(context_reuse_proof));
    MergeSourceCollectionSessionAction(result.action, roster_result.action);
    const bool is_prepared_plan =
        std::holds_alternative<PreparedSourceCollectionPlan>(payload);
    if (completes_pending_navigation && !is_prepared_plan) {
        (void)workflow_->CommitDeferredSampleNavigation(spectrum_index);
    }
    if (!roster_result.retired_snapshots.empty()) {
        result.background_retirement.push_back(
            MakeBackgroundRetirementHandle(
                std::move(roster_result.retired_snapshots)));
    }
    if (previous_snapshot && previous_snapshot != roster_->snapshot()) {
        result.background_retirement.push_back(std::move(previous_snapshot));
    }
    if (roster_result.replaced_folder_listing_generation) {
        result.background_retirement.push_back(
            std::move(roster_result.replaced_folder_listing_generation));
    }
    if (auto* plan = std::get_if<PreparedSourceCollectionPlan>(&payload)) {
        PreparedSampleWorkflowActivationResult activation =
            workflow_->SyncPreparedActiveSource(
                roster_->current_source_key(),
                roster_->snapshot(),
                std::move(plan->context),
                std::move(plan->workflow));
        MergeSourceCollectionSessionAction(
            result.action,
            activation.action);
        result.background_retirement.insert(
            result.background_retirement.end(),
            std::make_move_iterator(activation.background_retirement.begin()),
            std::make_move_iterator(activation.background_retirement.end()));
        if (completes_pending_navigation) {
            workflow_->CompletePreparedDeferredSampleNavigation(*pending_navigation);
        }
    } else {
        const auto& reuse = std::get<PreparedSourceCollectionReuse>(payload);
        SampleWorkflowTransitionOutcome transition =
            workflow_->SyncReusedPreparedKnownSource(
                roster_->current_source_key(),
                roster_->snapshot(),
                reuse.identity);
        MergeSourceCollectionSessionAction(
            result.action,
            transition.action);
        result.view_invalidated =
            result.view_invalidated ||
            transition.invalidate_view;
        if (transition.snapshot_index_to_load) {
            result.follow_up_spectrum_index =
                transition.snapshot_index_to_load;
        }
    }
    if (result.action.annotation_roster_changed) {
        source_session_state_->MarkDirtyAfterRestore();
    }
    const SpectrumSnapshotHandle& active_snapshot = roster_->snapshot();
    const std::optional<std::size_t> active_index = workflow_->current_index();
    if (active_snapshot && active_index && active_snapshot->collection.spectrum_count > 0 &&
        active_snapshot->collection.current_index != *active_index) {
        result.follow_up_spectrum_index = *active_index;
        roster_->RememberActiveSourceIndex(*active_index);
    }
    if (!completes_pending_navigation) {
        const std::optional<std::size_t> activation_from_sample =
            prepared_for_presented_source
                ? previous_sample_index
                : std::nullopt;
        RecordSampleTransition(
            deferred_restore_active_
                ? SourceCollectionSampleTransitionReason::Restore
                : SourceCollectionSampleTransitionReason::
                      SourceActivation,
            activation_from_sample,
            active_index);
    }
    result.action.navigation_inputs_changed = true;
    result.loaded = true;
    std::erase_if(
        unresolved_deferred_restore_sources_,
        [&prepared_path_key](const SourceCollectionSavedSource& source) {
            return SourcePathIdentityKey(source.path) == prepared_path_key;
        });
    MarkSourceSessionCacheDirty();
    result.view_invalidated = true;
    InvalidateView();
    AppendPendingBackgroundRetirement(
        result.background_retirement);
    return result;
}

SourceCollectionSessionResult SourceCollectionSession::OpenPreparedSource(
    std::filesystem::path path,
    std::size_t spectrum_index,
    SpectrumSnapshotHandle snapshot,
    SourceCollectionContext context,
    PreparedSampleWorkflowState prepared_workflow)
{
    return OpenPreparedSource(
        std::move(path),
        spectrum_index,
        std::move(snapshot),
        PreparedSourceCollectionPlan{std::move(context), std::move(prepared_workflow)});
}

std::optional<SourceCollectionDeferredRestorePlan> SourceCollectionSession::TakeDeferredRestorePlan()
{
    return std::exchange(deferred_restore_plan_, std::nullopt);
}

void SourceCollectionSession::FinishDeferredRestore()
{
    if (!deferred_restore_active_) {
        return;
    }
    workflow_->EndRestoringSourceSession();
    source_session_state_->EndRestore();
    deferred_restore_active_ = false;
}

SourceCollectionSessionResult SourceCollectionSession::RecordRestoreFailure(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    SourceCollectionLoadError error)
{
    SourceCollectionSessionResult result;
    auto update = roster_->RecordRestoreFailure(path, spectrum_index, std::move(error));
    result.action = update.action;
    if (!update.retired_snapshots.empty()) {
        result.background_retirement.push_back(
            MakeBackgroundRetirementHandle(std::move(update.retired_snapshots)));
    }
    if (update.replaced_folder_listing_generation) {
        result.background_retirement.push_back(std::move(update.replaced_folder_listing_generation));
    }
    if (result.action.snapshot_changed) {
        ApplyWorkflowTransitionOutcome(
            result,
            workflow_->SyncKnownActiveSource(roster_->current_source_key(), roster_->snapshot()));
    }
    MarkSourceSessionCacheDirty();
    InvalidateView();
    return result;
}

SourceCollectionSessionResult SourceCollectionSession::RestoreEmptyActiveSource(
    std::optional<std::size_t> source_index)
{
    SourceCollectionSessionResult result;
    if (source_index && (!roster_->has_source(*source_index) || roster_->can_activate_source(*source_index))) {
        return result;
    }
    result.action = source_index ? roster_->ActivateSource(*source_index)
                                : roster_->ClearActiveSourceForRestore();
    ApplyWorkflowTransitionOutcome(
        result,
        workflow_->SyncKnownActiveSource(roster_->current_source_key(), roster_->snapshot()));
    MarkSourceSessionCacheDirty();
    InvalidateView();
    return result;
}

bool SourceCollectionSession::HasUnresolvedSourceIntent(const std::filesystem::path& path) const
{
    const std::string path_key = SourcePathIdentityKey(path);
    return std::any_of(
        unresolved_deferred_restore_sources_.begin(),
        unresolved_deferred_restore_sources_.end(),
        [&path_key](const SourceCollectionSavedSource& source) {
            return SourcePathIdentityKey(source.path) == path_key;
        });
}

bool SourceCollectionSession::ForgetUnresolvedSourceIntent(const std::filesystem::path& path)
{
    const std::string path_key = SourcePathIdentityKey(path);
    const std::size_t previous_size = unresolved_deferred_restore_sources_.size();
    std::erase_if(
        unresolved_deferred_restore_sources_,
        [&path_key](const SourceCollectionSavedSource& source) {
            return SourcePathIdentityKey(source.path) == path_key;
        });
    if (unresolved_deferred_restore_sources_.size() == previous_size) {
        return false;
    }
    source_session_state_->MarkDirtyAfterRestore();
    return true;
}

bool SourceCollectionSession::CancelPendingSampleNavigation(
    const std::filesystem::path& path,
    std::size_t spectrum_index)
{
    const SpectrumSnapshotHandle& snapshot = roster_->snapshot();
    if (!snapshot || SourcePathIdentityKey(snapshot->source.path) != SourcePathIdentityKey(path) ||
        workflow_->pending_sample_index() != spectrum_index) {
        return false;
    }
    workflow_->CancelDeferredSampleNavigation();
    pending_background_spectrum_index_.reset();
    sample_transition_.reset();
    InvalidateView();
    return true;
}

bool SourceCollectionSession::CancelActivePendingSampleNavigation()
{
    const std::optional<std::size_t> pending_index =
        workflow_->pending_sample_index();
    if (!pending_index) {
        return false;
    }
    workflow_->CancelDeferredSampleNavigation();
    pending_background_spectrum_index_.reset();
    sample_transition_.reset();
    InvalidateView();
    return true;
}

SourceCollectionSessionResult
SourceCollectionSession::RunMaintenance(
    LocalUserStateSaveScheduler::TimePoint now)
{
    SourceCollectionSessionResult result;
    const std::optional<std::size_t> presented_sample_before =
        PresentedSampleIndex();
    const LocalUserStateHealthView persistence_before =
        PersistenceHealth();
    const std::optional<std::size_t>
        pending_sample_index_before =
            workflow_->pending_sample_index();
    const std::optional<std::filesystem::path>
        pending_source_path_before =
            pending_sample_index_before &&
                roster_->snapshot()
            ? std::optional<std::filesystem::path>{
                  roster_->snapshot()->source.path}
            : std::nullopt;
    pending_background_spectrum_index_.reset();
    source_session_state_->RunMaintenance(now, SavedSourcesWithAnnotations(), roster_->current_source_index());
    ApplyWorkflowTransitionOutcome(
        result,
        workflow_->RunMaintenance(
            now,
            roster_->snapshot()));
    FinalizePendingSourceFollowUp(
        result,
        pending_sample_index_before,
        pending_source_path_before);
    if (result.action.navigation_inputs_changed) {
        RecordSampleTransition(
            SourceCollectionSampleTransitionReason::
                NavigationInputReconciliation,
            presented_sample_before,
            EffectiveSampleNavigationIndex());
        result.view_invalidated = true;
    }
    if (result.view_invalidated) {
        InvalidateView();
    }
    const LocalUserStateHealthView persistence_after =
        PersistenceHealth();
    if (persistence_before.kind != persistence_after.kind ||
        persistence_before.messages != persistence_after.messages) {
        result.view_invalidated = true;
        InvalidateView();
    }
    AppendPendingBackgroundRetirement(
        result.background_retirement);
    return result;
}

std::optional<LocalUserStateSaveScheduler::TimePoint> SourceCollectionSession::NextMaintenanceDeadline() const
{
    std::optional<LocalUserStateSaveScheduler::TimePoint> deadline =
        source_session_state_->NextMaintenanceDeadline();
    const std::optional<LocalUserStateSaveScheduler::TimePoint> workflow_deadline =
        workflow_->NextMaintenanceDeadline();
    if (workflow_deadline && (!deadline || *workflow_deadline < *deadline)) {
        deadline = workflow_deadline;
    }
    return deadline;
}

bool SourceCollectionSession::FlushStateCaches()
{
    return FlushStateCachesWithStatus().all_saved();
}

bool SourceCollectionSession::PrepareLabelingForInteractiveClose()
{
    return workflow_->PrepareLabelingForInteractiveClose();
}

SourceCollectionStateFlushResult
SourceCollectionSession::FlushStateCachesWithStatus()
{
    const LocalUserStateHealthView persistence_before =
        PersistenceHealth();
    SourceCollectionStateFlushResult result;
    result.source_session_saved =
        source_session_state_->Flush(
            SavedSourcesWithAnnotations(),
            roster_->current_source_index());
    const SampleWorkflowStateFlushResult workflow =
        workflow_->FlushStateCachesWithStatus();
    result.navigation_saved = workflow.navigation_saved;
    result.labeling_saved = workflow.labeling_saved;
    result.workflow_saved = workflow.workflow_saved;
    const LocalUserStateHealthView persistence_after =
        PersistenceHealth();
    if (persistence_before.kind != persistence_after.kind ||
        persistence_before.messages != persistence_after.messages) {
        InvalidateView();
    }
    return result;
}

LocalUserStateHealthView
SourceCollectionSession::PersistenceHealth() const
{
    LocalUserStateHealthView health;
    const auto append = [&](LocalUserStateArea area,
                            const LocalUserStatePersistenceStatus& status) {
        AppendLocalUserStateHealth(health, area, status);
    };

    append(
        LocalUserStateArea::SourceSession,
        source_session_state_->PersistenceStatus());
    const SampleWorkflowPersistenceStatus workflow =
        workflow_->PersistenceStatus();
    append(
        LocalUserStateArea::SampleNavigation,
        workflow.navigation);
    append(
        LocalUserStateArea::SampleLabeling,
        workflow.labeling);
    append(
        LocalUserStateArea::SampleWorkflow,
        workflow.workflow);

    return health;
}

std::vector<SourceCollectionSavedSource> SourceCollectionSession::SavedSourcesWithAnnotations() const
{
    std::vector<SourceCollectionSavedSource> sources = roster_->SavedSources();
    const std::vector<std::string> source_keys = roster_->SavedSourceKeys();
    const std::unordered_map<std::string, std::vector<std::filesystem::path>> annotation_paths_by_source_key =
        workflow_->AnnotationPathsBySourceKey();

    const std::size_t count = std::min(sources.size(), source_keys.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto paths = annotation_paths_by_source_key.find(source_keys[index]);
        if (paths != annotation_paths_by_source_key.end()) {
            sources[index].annotation_paths = paths->second;
        }
    }

    std::unordered_set<std::string> saved_source_keys;
    saved_source_keys.reserve(sources.size() + unresolved_deferred_restore_sources_.size());
    for (const SourceCollectionSavedSource& source : sources) {
        saved_source_keys.insert(SourcePathIdentityKey(source.path));
    }
    for (const SourceCollectionSavedSource& unresolved : unresolved_deferred_restore_sources_) {
        if (saved_source_keys.insert(SourcePathIdentityKey(unresolved.path)).second) {
            sources.push_back(unresolved);
        } else {
            // Failed restore rows are in the roster, but their annotation
            // associations still belong to the saved, unresolved intent.
            for (auto& source : sources) {
                if (SourcePathIdentityKey(source.path) == SourcePathIdentityKey(unresolved.path)) {
                    source.annotation_paths = unresolved.annotation_paths;
                    break;
                }
            }
        }
    }
    return sources;
}

std::vector<BackgroundRetirementHandle> SourceCollectionSession::ReleaseBackgroundResourcesForShutdown()
{
    std::vector<BackgroundRetirementHandle> resources =
        workflow_->ReleaseBackgroundResourcesForShutdown();
    if (session_view_cache_) {
        resources.push_back(
            std::move(session_view_cache_));
    }
    for (BackgroundRetirementHandle& resource :
         pending_session_view_retirement_) {
        resources.push_back(std::move(resource));
    }
    pending_session_view_retirement_.clear();
    AppendPendingBackgroundRetirement(resources);
    return resources;
}

std::vector<BackgroundRetirementHandle>
SourceCollectionSession::TakeViewRetirement()
{
    return std::exchange(
        pending_session_view_retirement_,
        {});
}

void SourceCollectionSession::PrepareDeferredSourceSessionRestore()
{
    const SourceCollectionSessionStateCache state = source_session_state_->Load();
    SourceCollectionDeferredRestorePlan plan;
    for (const SourceCollectionSavedSource& source : state.sources) {
        // Availability is a worker concern. Probing saved OneDrive/network paths
        // here would put startup back on the UI thread.
        if (!source.path.empty()) {
            plan.sources.push_back(source);
        }
    }
    if (plan.sources.empty()) {
        return;
    }

    if (state.active_source_index && *state.active_source_index < state.sources.size()) {
        const std::filesystem::path& active_path = state.sources[*state.active_source_index].path;
        const std::string active_path_key = SourcePathIdentityKey(active_path);
        const auto active = std::find_if(
            plan.sources.begin(),
            plan.sources.end(),
            [&active_path_key](const auto& source) {
                return SourcePathIdentityKey(source.path) == active_path_key;
            });
        if (active != plan.sources.end()) {
            plan.active_source_index = static_cast<std::size_t>(std::distance(plan.sources.begin(), active));
        }
    }

    source_session_state_->BeginRestore();
    workflow_->BeginRestoringSourceSession();
    deferred_restore_active_ = true;
    unresolved_deferred_restore_sources_ = plan.sources;
    deferred_restore_plan_ = std::move(plan);
}

void SourceCollectionSession::MarkSourceSessionCacheDirty()
{
    source_session_state_->MarkDirty();
}

void SourceCollectionSession::InvalidateView()
{
    ++session_view_revision_;
}

std::optional<std::size_t>
SourceCollectionSession::PresentedSampleIndex() const
{
    const SpectrumSnapshotHandle snapshot =
        CurrentSampleSnapshot();
    if (!snapshot) {
        return std::nullopt;
    }
    return snapshot->collection.current_index;
}

void SourceCollectionSession::RecordSampleTransition(
    SourceCollectionSampleTransitionReason reason,
    std::optional<std::size_t> from_sample_index,
    std::optional<std::size_t> current_sample_index,
    std::optional<int> accepted_label_value)
{
    sample_transition_ =
        SourceCollectionSampleTransitionView{
            .reason = reason,
            .from_sample_index = from_sample_index,
            .current_sample_index = current_sample_index,
            .accepted_label_value = accepted_label_value,
        };
}

void SourceCollectionSession::AppendPendingBackgroundRetirement(
    std::vector<BackgroundRetirementHandle>& retirement)
{
    retirement.insert(
        retirement.end(),
        std::make_move_iterator(
            pending_background_retirement_.begin()),
        std::make_move_iterator(
            pending_background_retirement_.end()));
    pending_background_retirement_.clear();
}

void SourceCollectionSession::ApplyWorkflowTransitionOutcome(
    SourceCollectionSessionResult& result,
    SampleWorkflowTransitionOutcome outcome)
{
    if (outcome.action.annotation_roster_changed) {
        MarkSourceSessionCacheDirty();
    }
    MergeSourceCollectionSessionAction(
        result.action,
        outcome.action);
    result.navigation = std::move(outcome.navigation);
    result.changed = result.changed || outcome.changed;
    result.loaded = result.loaded || outcome.loaded;
    result.view_invalidated =
        result.view_invalidated ||
        outcome.invalidate_view;
    if (outcome.label_write) {
        result.label_write =
            std::move(outcome.label_write);
    }
    if (outcome.labeling_issue !=
        SampleLabelingOperationResult::Issue::None) {
        result.labeling_issue =
            outcome.labeling_issue;
    }
    if (!outcome.message.empty()) {
        result.message = std::move(outcome.message);
    }
    if (outcome.snapshot_index_to_load) {
        pending_background_spectrum_index_ =
            *outcome.snapshot_index_to_load;
    }
}

SourceCollectionSessionAction SourceCollectionSession::AdoptRosterOpenResult(
    SourceCollectionRosterOpenResult result)
{
    if (!result.retired_snapshots.empty()) {
        pending_background_retirement_.push_back(
            MakeBackgroundRetirementHandle(
                std::move(result.retired_snapshots)));
    }
    if (result.replaced_folder_listing_generation) {
        pending_background_retirement_.push_back(
            std::move(result.replaced_folder_listing_generation));
    }
    return result.action;
}

}  // namespace spectiary
