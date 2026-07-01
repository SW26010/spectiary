#include "ui/source_collection_session.h"

#include "app/local_user_state.h"
#include "ui/sample_workflow_coordinator.h"
#include "ui/source_collection_roster.h"
#include "ui/source_collection_session_state_cache_io.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace specforge {
namespace {

constexpr std::uint64_t kSourceSessionSaveDebounceFrames = 30;
constexpr std::uint64_t kSourceSessionSaveRetryFrames = 120;

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
          save_scheduler_(kSourceSessionSaveDebounceFrames, kSourceSessionSaveRetryFrames)
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
    }

    void MarkDirty()
    {
        if (!restoring_ && !cache_path_.empty()) {
            save_scheduler_.MarkDirty();
        }
    }

    void MaybeSave(
        std::uint64_t frame_index,
        const std::vector<SourceCollectionSavedSource>& sources,
        std::optional<std::size_t> active_source_index)
    {
        if (!save_scheduler_.ShouldAttemptSave(frame_index)) {
            return;
        }
        if (Save(sources, active_source_index)) {
            save_scheduler_.MarkSaveSucceeded();
        } else {
            save_scheduler_.MarkSaveFailed(frame_index);
        }
    }

    [[nodiscard]] bool Flush(
        const std::vector<SourceCollectionSavedSource>& sources,
        std::optional<std::size_t> active_source_index)
    {
        if (!save_scheduler_.dirty()) {
            return true;
        }
        if (Save(sources, active_source_index)) {
            save_scheduler_.MarkSaveSucceeded();
            return true;
        }
        save_scheduler_.MarkSaveFailed(0);
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
};

SourceCollectionIntent SourceCollectionIntent::Open(
    std::filesystem::path path,
    std::size_t spectrum_index)
{
    SourceCollectionIntent intent;
    intent.kind = SourceCollectionIntentKind::Open;
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

ActiveSampleWorkflowIntent ActiveSampleWorkflowIntent::CreateDefaultLabelingTask()
{
    ActiveSampleWorkflowIntent intent;
    intent.kind = ActiveSampleWorkflowIntentKind::CreateDefaultLabelingTask;
    return intent;
}

ActiveSampleWorkflowIntent ActiveSampleWorkflowIntent::CreateLabelingTask(std::string task_name)
{
    ActiveSampleWorkflowIntent intent;
    intent.kind = ActiveSampleWorkflowIntentKind::CreateLabelingTask;
    intent.task_name = std::move(task_name);
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

ActiveSampleWorkflowIntent ActiveSampleWorkflowIntent::RenameActiveLabelingTask(std::string task_name)
{
    ActiveSampleWorkflowIntent intent;
    intent.kind = ActiveSampleWorkflowIntentKind::RenameActiveLabelingTask;
    intent.task_name = std::move(task_name);
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
    : roster_(std::make_unique<SourceCollectionRoster>(std::move(snapshot_loader))),
      workflow_(std::make_unique<SampleWorkflowCoordinator>()),
      source_session_state_(std::make_unique<SourceCollectionSessionStatePersistence>(
          DefaultSourceCollectionSessionStateCachePath()))
{
    RestoreSourceSessionCache();
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
    : roster_(std::make_unique<SourceCollectionRoster>(std::move(snapshot_loader))),
      workflow_(std::make_unique<SampleWorkflowCoordinator>(
          std::move(navigation_state_cache_path),
          std::move(labeling_state_cache_path),
          std::move(workflow_state_cache_path))),
      source_session_state_(std::make_unique<SourceCollectionSessionStatePersistence>(
          std::move(source_session_state_cache_path)))
{
    RestoreSourceSessionCache();
}

SourceCollectionSession::~SourceCollectionSession() = default;

SourceCollectionSession::SourceCollectionSession(SourceCollectionSession&&) noexcept = default;

SourceCollectionSession& SourceCollectionSession::operator=(SourceCollectionSession&&) noexcept = default;

SourceCollectionSessionResult SourceCollectionSession::Submit(SourceCollectionSessionIntent intent)
{
    SourceCollectionSessionResult result;
    switch (intent.kind) {
    case SourceCollectionSessionIntentKind::SourceCollection:
        switch (intent.source_collection.kind) {
        case SourceCollectionIntentKind::Open:
            result.action =
                OpenSource(intent.source_collection.path, intent.source_collection.spectrum_index);
            break;
        case SourceCollectionIntentKind::SwitchActive:
            result.action = ActivateSource(intent.source_collection.source_index);
            break;
        case SourceCollectionIntentKind::Remove:
            result.action = RemoveSource(intent.source_collection.source_index);
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
            result.action = RequestSampleNavigation(intent.sample_navigation.request, &result.navigation);
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
        case ActiveSampleWorkflowIntentKind::CreateDefaultLabelingTask:
            result.action = CreateDefaultLabelingTask();
            break;
        case ActiveSampleWorkflowIntentKind::CreateLabelingTask:
            result.action = CreateLabelingTask(std::move(intent.active_sample_workflow.task_name));
            break;
        case ActiveSampleWorkflowIntentKind::ActivateLabelingTaskFromAnnotation:
            result.action = ActivateLabelingTaskFromAnnotation(std::move(intent.active_sample_workflow.path));
            break;
        case ActiveSampleWorkflowIntentKind::RenameActiveLabelingTask:
            result.action = RenameActiveLabelingTask(std::move(intent.active_sample_workflow.task_name));
            break;
        case ActiveSampleWorkflowIntentKind::DeleteActiveLabelingTask:
            result.action = DeleteActiveLabelingTask();
            break;
        case ActiveSampleWorkflowIntentKind::UpsertActiveLabel:
            result.action = UpsertActiveLabel(std::move(intent.active_sample_workflow.label), &result.changed);
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
            result.action = AssignActiveLabelToCurrentSample(intent.active_sample_workflow.label_code);
            break;
        case ActiveSampleWorkflowIntentKind::ClearActiveLabelForCurrentSample:
            result.action = ClearActiveLabelForCurrentSample();
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
    result.view = View();
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
    view.current_sample_snapshot = view.navigation.current_index ? snapshot : nullptr;
    view.labeling = workflow_->LabelingView(snapshot);
    view.filter = workflow_->FilterView(snapshot);
    view.sorting = workflow_->SortingView(snapshot);
    return view;
}

SpectrumSnapshotHandle SourceCollectionSession::CurrentSampleSnapshot() const
{
    const SpectrumSnapshotHandle& snapshot = roster_->snapshot();
    if (!snapshot || snapshot->source.path.empty() || !workflow_->current_index()) {
        return nullptr;
    }
    return snapshot;
}

SourceCollectionSessionAction SourceCollectionSession::OpenSource(
    const std::filesystem::path& path,
    std::size_t spectrum_index)
{
    SourceCollectionSessionAction action = roster_->OpenSource(path, spectrum_index);
    MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
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

SourceCollectionSessionAction SourceCollectionSession::RemoveSource(std::size_t source_index)
{
    SourceCollectionSessionAction action;
    const SourceCollectionRosterRemoveResult remove_result = roster_->RemoveSource(source_index);
    MergeSourceCollectionSessionAction(action, remove_result.action);
    if (!remove_result.removed) {
        return action;
    }

    MarkSourceSessionCacheDirty();
    workflow_->RemoveSource(remove_result.removed_source_key);
    if (remove_result.removed_current) {
        MergeSourceCollectionSessionAction(action, workflow_->ClearActiveWorkflow());
        MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    }
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::RequestSampleNavigation(
    const SampleNavigationRequest& request,
    SampleNavigationResult* navigation_result)
{
    SourceCollectionSessionAction action;
    ApplyWorkflowCommandResult(
        action,
        workflow_->RequestSampleNavigation(request, roster_->snapshot()),
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

SourceCollectionSessionAction SourceCollectionSession::CreateDefaultLabelingTask()
{
    SourceCollectionSessionAction action = workflow_->CreateDefaultLabelingTask();
    MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::CreateLabelingTask(std::string task_name)
{
    SourceCollectionSessionAction action = workflow_->CreateLabelingTask(std::move(task_name));
    MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::ActivateLabelingTaskFromAnnotation(
    std::filesystem::path annotation_path)
{
    return workflow_->ActivateLabelingTaskFromAnnotation(std::move(annotation_path));
}

SourceCollectionSessionAction SourceCollectionSession::RenameActiveLabelingTask(std::string task_name)
{
    return workflow_->RenameActiveLabelingTask(std::move(task_name));
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

SourceCollectionSessionAction SourceCollectionSession::AssignActiveLabelToCurrentSample(int code)
{
    SourceCollectionSessionAction action;
    ApplyWorkflowCommandResult(action, workflow_->AssignActiveLabelToCurrentSample(roster_->snapshot(), code));
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::ClearActiveLabelForCurrentSample()
{
    SourceCollectionSessionAction action;
    ApplyWorkflowCommandResult(action, workflow_->ClearActiveLabelForCurrentSample(roster_->snapshot()));
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

void SourceCollectionSession::MaybeSaveStateCaches(std::uint64_t frame_index)
{
    source_session_state_->MaybeSave(frame_index, SavedSourcesWithAnnotations(), roster_->current_source_index());
    workflow_->MaybeSaveStateCaches(frame_index);
}

bool SourceCollectionSession::FlushStateCaches()
{
    const bool source_session_saved =
        source_session_state_->Flush(SavedSourcesWithAnnotations(), roster_->current_source_index());
    const bool workflow_saved = workflow_->FlushStateCaches();
    return source_session_saved && workflow_saved;
}

SourceCollectionSessionAction SourceCollectionSession::EnsureSnapshotMatchesNavigation()
{
    SourceCollectionSessionAction action =
        workflow_->SyncActiveSource(roster_->current_source_key(), roster_->snapshot());
    const std::optional<std::size_t> navigation_index = workflow_->current_index();
    const SpectrumSnapshotHandle& snapshot = roster_->snapshot();
    if (navigation_index && snapshot && snapshot->collection.spectrum_count > 0 &&
        snapshot->collection.current_index != *navigation_index) {
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

    SourceCollectionSessionAction action = roster_->LoadActiveSourceAt(spectrum_index);
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
    return sources;
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
        MergeSourceCollectionSessionAction(action, LoadActiveSourceAt(*command_result.snapshot_index_to_load));
    }
}

}  // namespace specforge
