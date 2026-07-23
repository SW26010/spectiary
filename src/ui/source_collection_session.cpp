#include "ui/source_collection_session.h"

#include "app/local_user_state.h"
#include "domain/source_path_identity.h"
#include "profile/navigation_latency_trace.h"
#include "ui/sample_workflow_coordinator.h"
#include "ui/sample_workflow_preparation.h"
#include "ui/source_collection_roster.h"
#include "ui/source_collection_session_state_cache_io.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <memory>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace specforge {
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

bool IsRestorableSourcePath(const std::filesystem::path& path)
{
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

}  // namespace

class SourceCollectionSessionStatePersistence {
public:
    explicit SourceCollectionSessionStatePersistence(std::filesystem::path cache_path)
        : cache_path_(std::move(cache_path)),
          save_scheduler_(kSourceSessionSaveDebounce, kSourceSessionSaveRetry)
    {
    }

    [[nodiscard]] SourceCollectionSessionStateCache Load() const
    {
        if (cache_path_.empty()) {
            return {};
        }
        return LoadSourceCollectionSessionStateCache(cache_path_);
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
            save_scheduler_.MarkDirty();
        }
    }

    void MarkDirty()
    {
        if (!restoring_ && !cache_path_.empty()) {
            save_scheduler_.MarkDirty();
        }
    }

    void RunMaintenance(
        LocalUserStateSaveScheduler::TimePoint now,
        const std::vector<SourceCollectionSavedSource>& sources,
        std::optional<std::size_t> active_source_index)
    {
        if (!save_scheduler_.ShouldAttemptSave(now)) {
            return;
        }
        if (Save(sources, active_source_index)) {
            save_scheduler_.MarkSaveSucceeded();
        } else {
            save_scheduler_.MarkSaveFailed();
        }
    }

    void MarkDirtyAfterRestore()
    {
        if (cache_path_.empty()) {
            return;
        }
        if (restoring_) {
            dirty_after_restore_ = true;
        } else {
            save_scheduler_.MarkDirty();
        }
    }

    [[nodiscard]] std::optional<LocalUserStateSaveScheduler::TimePoint> NextMaintenanceDeadline() const
    {
        return save_scheduler_.next_attempt_time();
    }

    [[nodiscard]] bool Flush(
        const std::vector<SourceCollectionSavedSource>& sources,
        std::optional<std::size_t> active_source_index)
    {
        if (dirty_after_restore_) {
            if (!Save(sources, active_source_index)) {
                return false;
            }
            dirty_after_restore_ = false;
        }
        if (!save_scheduler_.dirty()) {
            return true;
        }
        if (Save(sources, active_source_index)) {
            save_scheduler_.MarkSaveSucceeded();
            return true;
        }
        save_scheduler_.MarkSaveFailed();
        return false;
    }

private:
    [[nodiscard]] bool Save(
        const std::vector<SourceCollectionSavedSource>& sources,
        std::optional<std::size_t> active_source_index) const
    {
        SourceCollectionSessionStateCache cache;
        cache.sources = sources;
        cache.active_source_index = active_source_index;
        return SaveSourceCollectionSessionStateCache(cache_path_, cache);
    }

    std::filesystem::path cache_path_;
    LocalUserStateSaveScheduler save_scheduler_;
    bool restoring_ = false;
    bool dirty_after_restore_ = false;
};

SourceCollectionIntent SourceCollectionIntent::OpenSynchronously(
    std::filesystem::path path,
    std::size_t spectrum_index)
{
    SourceCollectionIntent intent;
    intent.kind = SourceCollectionIntentKind::OpenSynchronously;
    intent.path = std::move(path);
    intent.spectrum_index = spectrum_index;
    return intent;
}

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

SourceCollectionSession::SourceCollectionSession(SnapshotLoader snapshot_loader)
    : SourceCollectionSession(
          std::move(snapshot_loader),
          SourceCollectionSessionRestoreMode::Immediate)
{
}

SourceCollectionSession::SourceCollectionSession(
    SnapshotLoader snapshot_loader,
    SourceCollectionSessionRestoreMode restore_mode)
    : roster_(std::make_unique<SourceCollectionRoster>(std::move(snapshot_loader))),
      workflow_(std::make_unique<SampleWorkflowCoordinator>()),
      source_session_state_(std::make_unique<SourceCollectionSessionStatePersistence>(
          DefaultSourceCollectionSessionStateCachePath())),
      background_loads_required_(restore_mode == SourceCollectionSessionRestoreMode::Deferred)
{
    workflow_->SetDeferredSampleNavigation(background_loads_required_);
    if (restore_mode == SourceCollectionSessionRestoreMode::Deferred) {
        PrepareDeferredSourceSessionRestore();
    } else {
        RestoreSourceSessionCache();
    }
}

SourceCollectionSession::SourceCollectionSession(
    SnapshotLoader snapshot_loader,
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path)
    : roster_(std::make_unique<SourceCollectionRoster>(std::move(snapshot_loader))),
      workflow_(std::make_unique<SampleWorkflowCoordinator>(
          std::move(navigation_state_cache_path),
          std::move(labeling_state_cache_path))),
      source_session_state_(std::make_unique<SourceCollectionSessionStatePersistence>(std::filesystem::path{}))
{
}

SourceCollectionSession::SourceCollectionSession(
    SnapshotLoader snapshot_loader,
    std::filesystem::path source_session_state_cache_path,
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path)
    : roster_(std::make_unique<SourceCollectionRoster>(std::move(snapshot_loader))),
      workflow_(std::make_unique<SampleWorkflowCoordinator>(
          std::move(navigation_state_cache_path),
          std::move(labeling_state_cache_path))),
      source_session_state_(std::make_unique<SourceCollectionSessionStatePersistence>(
          std::move(source_session_state_cache_path)))
{
    RestoreSourceSessionCache();
}

SourceCollectionSession::SourceCollectionSession(
    SnapshotLoader snapshot_loader,
    std::filesystem::path source_session_state_cache_path,
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path,
    std::filesystem::path workflow_state_cache_path)
    : SourceCollectionSession(
          std::move(snapshot_loader),
          std::move(source_session_state_cache_path),
          std::move(navigation_state_cache_path),
          std::move(labeling_state_cache_path),
          std::move(workflow_state_cache_path),
          SourceCollectionSessionRestoreMode::Immediate)
{
}

SourceCollectionSession::SourceCollectionSession(
    SnapshotLoader snapshot_loader,
    std::filesystem::path source_session_state_cache_path,
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path,
    std::filesystem::path workflow_state_cache_path,
    SourceCollectionSessionRestoreMode restore_mode)
    : roster_(std::make_unique<SourceCollectionRoster>(std::move(snapshot_loader))),
      workflow_(std::make_unique<SampleWorkflowCoordinator>(
          std::move(navigation_state_cache_path),
          std::move(labeling_state_cache_path),
          std::move(workflow_state_cache_path))),
      source_session_state_(std::make_unique<SourceCollectionSessionStatePersistence>(
          std::move(source_session_state_cache_path))),
      background_loads_required_(restore_mode == SourceCollectionSessionRestoreMode::Deferred)
{
    workflow_->SetDeferredSampleNavigation(background_loads_required_);
    if (restore_mode == SourceCollectionSessionRestoreMode::Deferred) {
        PrepareDeferredSourceSessionRestore();
    } else {
        RestoreSourceSessionCache();
    }
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
    case SourceCollectionIntentKind::OpenSynchronously:
        return true;
    case SourceCollectionIntentKind::SwitchActive: {
        const std::size_t target_index = intent.source_collection.source_index;
        const std::optional<std::size_t> current_index = roster_->current_source_index();
        return roster_->has_source(target_index) &&
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
    switch (intent.kind) {
    case SourceCollectionSessionIntentKind::SourceCollection:
        switch (intent.source_collection.kind) {
        case SourceCollectionIntentKind::OpenSynchronously:
            result.action =
                OpenSource(intent.source_collection.path, intent.source_collection.spectrum_index);
            break;
        case SourceCollectionIntentKind::SwitchActive:
            result.action = ActivateSource(intent.source_collection.source_index);
            break;
        case SourceCollectionIntentKind::Remove:
            result.action = RemoveSource(
                intent.source_collection.source_index,
                result.background_retirement,
                &result.canceled_source_follow_up_path);
            break;
        case SourceCollectionIntentKind::AddReadOnlyAnnotationResult:
            result.action = AddReadOnlyAnnotationToActiveSource(
                intent.source_collection.path,
                &result.loaded,
                &result.message);
            break;
        case SourceCollectionIntentKind::RemoveReadOnlyAnnotationResult:
            result.action = RemoveReadOnlyAnnotationFromActiveSource(intent.source_collection.path);
            break;
        case SourceCollectionIntentKind::RenameAnnotationResultDisplayName:
            result.action = RenameAnnotationDisplayName(
                std::move(intent.source_collection.path),
                std::move(intent.source_collection.display_name));
            break;
        }
        break;
    case SourceCollectionSessionIntentKind::SampleNavigation:
        switch (intent.sample_navigation.kind) {
        case SampleNavigationIntentKind::Move:
            result.action = RequestSampleNavigation(
                intent.sample_navigation.request,
                &result.navigation,
                target_resolution);
            break;
        case SampleNavigationIntentKind::SetSampleNameQuery:
            result.action = SetSampleNameQuery(std::move(intent.sample_navigation.query));
            break;
        case SampleNavigationIntentKind::CommitSampleNameSelection:
            result.action = CommitSampleNameSelection(
                intent.sample_navigation.target_row,
                std::move(intent.sample_navigation.matched_name),
                &result.navigation);
            break;
        }
        break;
    case SourceCollectionSessionIntentKind::ActiveSampleWorkflow:
        switch (intent.active_sample_workflow.kind) {
        case ActiveSampleWorkflowIntentKind::StartOrResumeTemporaryLabelingTask:
            result.action = StartOrResumeTemporaryLabelingTask();
            break;
        case ActiveSampleWorkflowIntentKind::ActivateLabelingTaskFromAnnotation:
            result.action = ActivateLabelingTaskFromAnnotation(std::move(intent.active_sample_workflow.path));
            break;
        case ActiveSampleWorkflowIntentKind::DeleteActiveLabelingTask:
            result.action = DeleteActiveLabelingTask();
            break;
        case ActiveSampleWorkflowIntentKind::UpsertActiveLabel:
            result.action = UpsertActiveLabel(std::move(intent.active_sample_workflow.label), &result.changed);
            break;
        case ActiveSampleWorkflowIntentKind::UpdateActiveLabel:
            result.action = UpdateActiveLabel(
                intent.active_sample_workflow.label_code,
                std::move(intent.active_sample_workflow.label),
                intent.active_sample_workflow.allow_used_label_code_change,
                &result.changed);
            break;
        case ActiveSampleWorkflowIntentKind::RemoveActiveLabel:
            result.action = RemoveActiveLabel(intent.active_sample_workflow.label_code, &result.changed);
            break;
        case ActiveSampleWorkflowIntentKind::SetActiveLabelingAutoAdvance:
            result.action = SetActiveLabelingAutoAdvance(intent.active_sample_workflow.enabled);
            break;
        case ActiveSampleWorkflowIntentKind::SetActiveLabelingSkipLabeledOnAdvance:
            result.action = SetActiveLabelingSkipLabeledOnAdvance(intent.active_sample_workflow.enabled);
            break;
        case ActiveSampleWorkflowIntentKind::SetActiveLabelingOutputPath:
            result.action = SetActiveLabelingOutputPath(std::move(intent.active_sample_workflow.path));
            break;
        case ActiveSampleWorkflowIntentKind::DeactivateActiveLabelingTask:
            result.action = DeactivateActiveLabelingTask();
            break;
        case ActiveSampleWorkflowIntentKind::AssignActiveLabelToCurrentSample:
            result.action = AssignActiveLabelToCurrentSample(
                intent.active_sample_workflow.label_code,
                target_resolution);
            break;
        case ActiveSampleWorkflowIntentKind::ClearActiveLabelForCurrentSample:
            result.action = ClearActiveLabelForCurrentSample(target_resolution);
            break;
        case ActiveSampleWorkflowIntentKind::UndoLastLabelWrite:
            result.action = UndoLastLabelWrite();
            break;
        }
        break;
    case SourceCollectionSessionIntentKind::SampleFiltering:
        switch (intent.sample_filtering.kind) {
        case SampleFilteringIntentKind::ClearFilters:
            result.action = ClearFilters();
            break;
        case SampleFilteringIntentKind::AddFilterSource:
            result.action = AddFilterSource(std::move(intent.sample_filtering.source_id));
            break;
        case SampleFilteringIntentKind::RemoveFilterSource:
            result.action = RemoveFilterSource(std::move(intent.sample_filtering.source_id));
            break;
        case SampleFilteringIntentKind::SetFilterValueSelected:
            result.action = SetFilterValueSelected(
                std::move(intent.sample_filtering.source_id),
                std::move(intent.sample_filtering.value_key),
                intent.sample_filtering.selected);
            break;
        }
        break;
    case SourceCollectionSessionIntentKind::SampleSorting:
        switch (intent.sample_sorting.kind) {
        case SampleSortingIntentKind::ClearSorting:
            result.action = ClearSampleSorting();
            break;
        case SampleSortingIntentKind::AddSortSource:
            result.action = AddSampleSortSource(std::move(intent.sample_sorting.source_id));
            break;
        case SampleSortingIntentKind::RemoveSortSource:
            result.action = RemoveSampleSortSource(std::move(intent.sample_sorting.source_id));
            break;
        case SampleSortingIntentKind::SetSortSource:
            result.action = SetSampleSortSource(std::move(intent.sample_sorting.source_id));
            break;
        case SampleSortingIntentKind::SetSortDirection:
            result.action = SetSampleSortDirection(intent.sample_sorting.direction);
            break;
        }
        break;
    }
    PreserveRequiredBackgroundSnapshotLoad();
    const NavigationLatencyTimePoint pending_follow_up_started_at =
        target_resolution != nullptr ? NavigationLatencyTrace::Now()
                                     : NavigationLatencyTimePoint{};
    const std::optional<std::size_t> pending_sample_index_after =
        workflow_->pending_sample_index();
    if (pending_sample_index_after == pending_sample_index_before &&
        pending_background_spectrum_index_ == pending_sample_index_before) {
        pending_background_spectrum_index_.reset();
    }
    result.follow_up_spectrum_index = std::exchange(pending_background_spectrum_index_, std::nullopt);
    const SpectrumSnapshotHandle active_snapshot_after = roster_->snapshot();
    const bool previous_source_follow_up_retained =
        pending_sample_index_before &&
        pending_sample_index_after == pending_sample_index_before &&
        pending_source_path_before && active_snapshot_after &&
        SourcePathIdentityKey(*pending_source_path_before) ==
            SourcePathIdentityKey(active_snapshot_after->source.path);
    if (pending_sample_index_before && !previous_source_follow_up_retained) {
        result.canceled_source_follow_up_path = pending_source_path_before;
    }
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
    for (BackgroundRetirementHandle& resource :
         pending_background_retirement_) {
        result.background_retirement.push_back(std::move(resource));
    }
    pending_background_retirement_.clear();
    return result;
}

SourceCollectionSessionView SourceCollectionSession::View() const
{
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
    view.filter = workflow_->FilterView(snapshot);
    view.sorting = workflow_->SortingView(snapshot);
    return view;
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

std::vector<std::filesystem::path> SourceCollectionSession::AnnotationPathsForSource(
    const std::filesystem::path& path) const
{
    return workflow_->AnnotationPathsForSourceKey(SourcePathIdentityKey(path));
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
    return SourceCollectionLoadHint{
        *identity,
        *current_index,
        revision == live_workflow_revisions_.end() ? 0 : revision->second,
        roster_->FolderListingGeneration(path),
        roster_->ContextReuseProof(path),
        std::move(resident_snapshot)};
}

SourceCollectionSessionAction SourceCollectionSession::OpenSource(
    const std::filesystem::path& path,
    std::size_t spectrum_index)
{
    SourceCollectionSessionAction action =
        AdoptRosterOpenResult(roster_->OpenSource(path, spectrum_index));
    MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation(true));
    MarkSourceSessionCacheDirty();
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::ActivateSource(std::size_t source_index)
{
    if (!roster_->has_source(source_index)) {
        return {};
    }

    SourceCollectionSessionAction action = roster_->ActivateSource(source_index);
    MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    MarkSourceSessionCacheDirty();
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::RemoveSource(
    std::size_t source_index,
    std::vector<BackgroundRetirementHandle>& background_retirement,
    std::optional<std::filesystem::path>* canceled_source_follow_up_path)
{
    SourceCollectionSessionAction action;
    SourceCollectionRosterRemoveResult remove_result = roster_->RemoveSource(source_index);
    MergeSourceCollectionSessionAction(action, remove_result.action);
    if (!remove_result.removed) {
        return action;
    }

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
        MergeSourceCollectionSessionAction(action, workflow_->ClearActiveWorkflow());
        MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    }
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::RequestSampleNavigation(
    const SampleNavigationRequest& request,
    SampleNavigationResult* navigation_result,
    NavigationTargetResolutionReport* target_resolution)
{
    SourceCollectionSessionAction action;
    ApplyWorkflowCommandResult(
        action,
        workflow_->RequestSampleNavigation(
            request,
            roster_->snapshot(),
            std::nullopt,
            target_resolution),
        navigation_result);
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::AddReadOnlyAnnotationToActiveSource(
    const std::filesystem::path& path,
    bool* loaded,
    std::string* message)
{
    bool annotation_loaded = false;
    SourceCollectionSessionAction action =
        workflow_->AddReadOnlyAnnotationToActiveSource(path, &annotation_loaded, message);
    if (loaded != nullptr) {
        *loaded = annotation_loaded;
    }
    if (annotation_loaded) {
        MarkSourceSessionCacheDirty();
        MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    }
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::RemoveReadOnlyAnnotationFromActiveSource(
    const std::filesystem::path& path)
{
    SourceCollectionSessionAction action = workflow_->RemoveReadOnlyAnnotationFromActiveSource(path);
    if (action.navigation_inputs_changed || action.workflow_changed || action.snapshot_changed) {
        MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
        MarkSourceSessionCacheDirty();
    }
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::RenameAnnotationDisplayName(
    std::filesystem::path path,
    std::string display_name)
{
    return workflow_->RenameAnnotationDisplayNameForActiveSource(
        std::move(path),
        std::move(display_name));
}

SourceCollectionSessionAction SourceCollectionSession::SetSampleNameQuery(std::string query)
{
    return workflow_->SetSampleNameQuery(std::move(query));
}

SourceCollectionSessionAction SourceCollectionSession::CommitSampleNameSelection(
    std::size_t target_row,
    std::string matched_name,
    SampleNavigationResult* navigation_result)
{
    SourceCollectionSessionAction action;
    ApplyWorkflowCommandResult(
        action,
        workflow_->CommitSampleNameSelection(target_row, std::move(matched_name), roster_->snapshot()),
        navigation_result);
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::StartOrResumeTemporaryLabelingTask()
{
    SourceCollectionSessionAction action = workflow_->StartOrResumeTemporaryLabelingTask();
    MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    return action;
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
        result.message = "The prepared source reuse target is no longer available.";
        return result;
    }
    SpectrumSnapshotHandle previous_snapshot = roster_->snapshot();
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
        result.message = "The prepared known-source plan is no longer current.";
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
            result.message = "Prepared navigation no longer has a selectable final spectrum.";
        }
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
        MergeSourceCollectionSessionAction(
            result.action,
            workflow_->SyncReusedPreparedKnownSource(
                roster_->current_source_key(),
                roster_->snapshot(),
                reuse.identity));
    }
    const SpectrumSnapshotHandle& active_snapshot = roster_->snapshot();
    const std::optional<std::size_t> active_index = workflow_->current_index();
    if (active_snapshot && active_index && active_snapshot->collection.spectrum_count > 0 &&
        active_snapshot->collection.current_index != *active_index) {
        result.follow_up_spectrum_index = *active_index;
        roster_->RememberActiveSourceIndex(*active_index);
    }
    result.action.navigation_inputs_changed = true;
    result.loaded = true;
    std::erase_if(
        unresolved_deferred_restore_sources_,
        [&prepared_path_key](const SourceCollectionSavedSource& source) {
            return SourcePathIdentityKey(source.path) == prepared_path_key;
        });
    MarkSourceSessionCacheDirty();
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
    return true;
}

bool SourceCollectionSession::CancelActivePendingSampleNavigation()
{
    if (!workflow_->pending_sample_index()) {
        return false;
    }
    workflow_->CancelDeferredSampleNavigation();
    pending_background_spectrum_index_.reset();
    return true;
}

SourceCollectionSessionAction SourceCollectionSession::ActivateLabelingTaskFromAnnotation(
    std::filesystem::path annotation_path)
{
    return workflow_->ActivateLabelingTaskFromAnnotation(std::move(annotation_path));
}

SourceCollectionSessionAction SourceCollectionSession::DeleteActiveLabelingTask()
{
    SourceCollectionSessionAction action = workflow_->DeleteActiveLabelingTask();
    MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::UpsertActiveLabel(SampleLabelDefinition label, bool* changed)
{
    return workflow_->UpsertActiveLabel(std::move(label), changed);
}

SourceCollectionSessionAction SourceCollectionSession::UpdateActiveLabel(
    int original_code,
    SampleLabelDefinition label,
    bool allow_used_code_change,
    bool* changed)
{
    SourceCollectionSessionAction action = workflow_->UpdateActiveLabel(
        original_code,
        std::move(label),
        allow_used_code_change,
        changed);
    if (action.navigation_inputs_changed) {
        MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    }
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::RemoveActiveLabel(int code, bool* changed)
{
    SourceCollectionSessionAction action = workflow_->RemoveActiveLabel(code, changed);
    if (action.navigation_inputs_changed) {
        MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    }
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::SetActiveLabelingAutoAdvance(bool enabled)
{
    return workflow_->SetActiveLabelingAutoAdvance(enabled);
}

SourceCollectionSessionAction SourceCollectionSession::SetActiveLabelingSkipLabeledOnAdvance(bool enabled)
{
    return workflow_->SetActiveLabelingSkipLabeledOnAdvance(enabled);
}

SourceCollectionSessionAction SourceCollectionSession::SetActiveLabelingOutputPath(std::filesystem::path output_path)
{
    SourceCollectionSessionAction action = workflow_->SetActiveLabelingOutputPath(std::move(output_path));
    if (action.navigation_inputs_changed) {
        MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    }
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::DeactivateActiveLabelingTask()
{
    SourceCollectionSessionAction action = workflow_->DeactivateActiveLabelingTask();
    MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::AssignActiveLabelToCurrentSample(
    int code,
    NavigationTargetResolutionReport* target_resolution)
{
    SourceCollectionSessionAction action;
    ApplyWorkflowCommandResult(
        action,
        workflow_->AssignActiveLabelToCurrentSample(
            roster_->snapshot(),
            code,
            target_resolution));
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::ClearActiveLabelForCurrentSample(
    NavigationTargetResolutionReport* target_resolution)
{
    SourceCollectionSessionAction action;
    ApplyWorkflowCommandResult(
        action,
        workflow_->ClearActiveLabelForCurrentSample(
            roster_->snapshot(),
            target_resolution));
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::UndoLastLabelWrite()
{
    SourceCollectionSessionAction action;
    ApplyWorkflowCommandResult(action, workflow_->UndoLastLabelWrite(roster_->snapshot()));
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::ClearFilters()
{
    SourceCollectionSessionAction action = workflow_->ClearFilters(roster_->snapshot());
    MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::AddFilterSource(std::string source_id)
{
    return workflow_->AddFilterSource(roster_->snapshot(), std::move(source_id));
}

SourceCollectionSessionAction SourceCollectionSession::RemoveFilterSource(std::string source_id)
{
    SourceCollectionSessionAction action =
        workflow_->RemoveFilterSource(roster_->snapshot(), std::move(source_id));
    MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::SetFilterValueSelected(
    std::string source_id,
    std::string value_key,
    bool selected)
{
    SourceCollectionSessionAction action = workflow_->SetFilterValueSelected(
        roster_->snapshot(),
        std::move(source_id),
        std::move(value_key),
        selected);
    MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::ClearSampleSorting()
{
    SourceCollectionSessionAction action = workflow_->ClearSampleSorting(roster_->snapshot());
    MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::AddSampleSortSource(std::string source_id)
{
    SourceCollectionSessionAction action =
        workflow_->AddSampleSortSource(roster_->snapshot(), std::move(source_id));
    MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::RemoveSampleSortSource(std::string source_id)
{
    SourceCollectionSessionAction action =
        workflow_->RemoveSampleSortSource(roster_->snapshot(), std::move(source_id));
    MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::SetSampleSortSource(std::string source_id)
{
    SourceCollectionSessionAction action =
        workflow_->SetSampleSortSource(roster_->snapshot(), std::move(source_id));
    MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::SetSampleSortDirection(
    SampleNavigationSortDirection direction)
{
    SourceCollectionSessionAction action =
        workflow_->SetSampleSortDirection(roster_->snapshot(), direction);
    MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    return action;
}

void SourceCollectionSession::RunMaintenance(LocalUserStateSaveScheduler::TimePoint now)
{
    source_session_state_->RunMaintenance(now, SavedSourcesWithAnnotations(), roster_->current_source_index());
    workflow_->RunMaintenance(now);
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
    const bool source_session_saved =
        source_session_state_->Flush(SavedSourcesWithAnnotations(), roster_->current_source_index());
    const bool workflow_saved = workflow_->FlushStateCaches();
    return source_session_saved && workflow_saved;
}

SourceCollectionSessionAction SourceCollectionSession::EnsureSnapshotMatchesNavigation(
    bool refresh_source_context)
{
    SourceCollectionSessionAction action = refresh_source_context
        ? workflow_->SyncActiveSource(roster_->current_source_key(), roster_->snapshot())
        : workflow_->SyncKnownActiveSource(roster_->current_source_key(), roster_->snapshot());
    const std::optional<std::size_t> pending_index = workflow_->pending_sample_index();
    const std::optional<std::size_t> navigation_index = pending_index
        ? pending_index
        : workflow_->current_index();
    const SpectrumSnapshotHandle& snapshot = roster_->snapshot();
    if (navigation_index && snapshot && snapshot->collection.spectrum_count > 0 &&
        snapshot->collection.current_index != *navigation_index) {
        if (background_loads_required_) {
            pending_background_spectrum_index_ = *navigation_index;
            action.navigation_inputs_changed = true;
            return action;
        }
        MergeSourceCollectionSessionAction(action, LoadActiveSourceAt(*navigation_index));
        return action;
    }
    action.navigation_inputs_changed = true;
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::LoadActiveSourceAt(std::size_t spectrum_index)
{
    if (!roster_->current_source_key()) {
        return {};
    }

    SourceCollectionSessionAction action =
        AdoptRosterOpenResult(
            roster_->LoadActiveSourceAt(spectrum_index));
    action.navigation_inputs_changed = true;
    MarkSourceSessionCacheDirty();
    return action;
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
        }
    }
    return sources;
}

std::vector<BackgroundRetirementHandle> SourceCollectionSession::ReleaseBackgroundResourcesForShutdown()
{
    std::vector<BackgroundRetirementHandle> resources =
        workflow_->ReleaseBackgroundResourcesForShutdown();
    for (BackgroundRetirementHandle& resource :
         pending_background_retirement_) {
        resources.push_back(std::move(resource));
    }
    pending_background_retirement_.clear();
    return resources;
}

void SourceCollectionSession::PreserveRequiredBackgroundSnapshotLoad()
{
    if (!background_loads_required_) {
        return;
    }
    if (const std::optional<std::size_t> pending_index = workflow_->pending_sample_index()) {
        pending_background_spectrum_index_ = *pending_index;
        return;
    }
    const std::optional<std::size_t> navigation_index = workflow_->current_index();
    const SpectrumSnapshotHandle& snapshot = roster_->snapshot();
    if (navigation_index && snapshot && snapshot->collection.spectrum_count > 0 &&
        snapshot->collection.current_index != *navigation_index) {
        pending_background_spectrum_index_ = *navigation_index;
    }
}

void SourceCollectionSession::RestoreSourceSessionCache()
{
    const SourceCollectionSessionStateCache state = source_session_state_->Load();
    if (state.sources.empty()) {
        return;
    }

    source_session_state_->BeginRestore();
    workflow_->BeginRestoringSourceSession();
    std::optional<std::size_t> restored_active_source_index;
    for (std::size_t source_index = 0; source_index < state.sources.size(); ++source_index) {
        const SourceCollectionSavedSource& source = state.sources[source_index];
        if (!IsRestorableSourcePath(source.path)) {
            continue;
        }

        (void)OpenSource(source.path, source.last_spectrum_index);
        (void)workflow_->RestoreReadOnlyAnnotationsForActiveSource(source.annotation_paths);
        if (state.active_source_index && *state.active_source_index == source_index) {
            restored_active_source_index = roster_->current_source_index();
        }
    }

    if (restored_active_source_index && roster_->has_source(*restored_active_source_index)) {
        (void)ActivateSource(*restored_active_source_index);
    }
    workflow_->EndRestoringSourceSession();
    source_session_state_->EndRestore();
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

void SourceCollectionSession::ApplyWorkflowCommandResult(
    SourceCollectionSessionAction& action,
    const SampleWorkflowCommandResult& command_result,
    SampleNavigationResult* navigation_result)
{
    if (navigation_result != nullptr) {
        *navigation_result = command_result.navigation;
    }
    MergeSourceCollectionSessionAction(action, command_result.action);
    if (command_result.snapshot_index_to_load) {
        if (background_loads_required_) {
            pending_background_spectrum_index_ = *command_result.snapshot_index_to_load;
            action.navigation_inputs_changed = true;
        } else {
            MergeSourceCollectionSessionAction(action, LoadActiveSourceAt(*command_result.snapshot_index_to_load));
        }
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

}  // namespace specforge
