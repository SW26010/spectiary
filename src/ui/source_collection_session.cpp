#include "ui/source_collection_session.h"

#include "app/local_user_state.h"
#include "ui/sample_workflow_coordinator.h"
#include "ui/source_collection_roster.h"
#include "ui/source_collection_session_state_cache_io.h"

#include <cstdint>
#include <memory>
#include <system_error>
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
        return SaveSourceCollectionSessionStateCache(cache_path_, sources, active_source_index);
    }

    std::filesystem::path cache_path_;
    LocalUserStateSaveScheduler save_scheduler_;
    bool restoring_ = false;
};

SourceCollectionSessionCommand SourceCollectionSessionCommand::OpenSource(
    std::filesystem::path path,
    std::size_t spectrum_index)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::OpenSource;
    command.path = std::move(path);
    command.spectrum_index = spectrum_index;
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::ActivateSource(std::size_t source_index)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::ActivateSource;
    command.source_index = source_index;
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::RemoveSource(std::size_t source_index)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::RemoveSource;
    command.source_index = source_index;
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::NavigateSample(SampleNavigationRequest request)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::NavigateSample;
    command.navigation_request = std::move(request);
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::AddReadOnlyAnnotation(std::filesystem::path path)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::AddReadOnlyAnnotation;
    command.path = std::move(path);
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::SetSampleNameQuery(std::string query)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::SetSampleNameQuery;
    command.query = std::move(query);
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::CommitSampleNameSelection(
    std::size_t target_row,
    std::string matched_name)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::CommitSampleNameSelection;
    command.target_row = target_row;
    command.matched_name = std::move(matched_name);
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::CreateDefaultLabelingTask()
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::CreateDefaultLabelingTask;
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::UpsertActiveLabel(SampleLabelDefinition label)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::UpsertActiveLabel;
    command.label = std::move(label);
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::SetActiveLabelingAutoAdvance(bool enabled)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::SetActiveLabelingAutoAdvance;
    command.enabled = enabled;
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::SetActiveLabelingSkipLabeledOnAdvance(bool enabled)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::SetActiveLabelingSkipLabeledOnAdvance;
    command.enabled = enabled;
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::SetActiveLabelingOutputPath(
    std::filesystem::path output_path)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::SetActiveLabelingOutputPath;
    command.path = std::move(output_path);
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::AssignActiveLabelToCurrentSample(int code)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::AssignActiveLabelToCurrentSample;
    command.label_code = code;
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::ClearActiveLabelForCurrentSample()
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::ClearActiveLabelForCurrentSample;
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::ClearFilters()
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::ClearFilters;
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::SetFilterValueSelected(
    std::string source_id,
    std::string value_key,
    bool selected)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::SetFilterValueSelected;
    command.filter_source_id = std::move(source_id);
    command.filter_value_key = std::move(value_key);
    command.selected = selected;
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::SetActiveLabelingFilterSourceSelected(bool selected)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::SetActiveLabelingFilterSourceSelected;
    command.selected = selected;
    return command;
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

SourceCollectionSession::~SourceCollectionSession() = default;

SourceCollectionSession::SourceCollectionSession(SourceCollectionSession&&) noexcept = default;

SourceCollectionSession& SourceCollectionSession::operator=(SourceCollectionSession&&) noexcept = default;

SourceCollectionSessionResult SourceCollectionSession::Submit(SourceCollectionSessionCommand command)
{
    SourceCollectionSessionResult result;
    switch (command.kind) {
    case SourceCollectionSessionCommandKind::OpenSource:
        result.action = OpenSource(command.path, command.spectrum_index);
        break;
    case SourceCollectionSessionCommandKind::ActivateSource:
        result.action = ActivateSource(command.source_index);
        break;
    case SourceCollectionSessionCommandKind::RemoveSource:
        result.action = RemoveSource(command.source_index);
        break;
    case SourceCollectionSessionCommandKind::NavigateSample:
        result.action = RequestSampleNavigation(command.navigation_request, &result.navigation);
        break;
    case SourceCollectionSessionCommandKind::AddReadOnlyAnnotation:
        result.action = AddReadOnlyAnnotationToActiveSource(command.path, &result.loaded, &result.message);
        break;
    case SourceCollectionSessionCommandKind::SetSampleNameQuery:
        result.action = SetSampleNameQuery(std::move(command.query));
        break;
    case SourceCollectionSessionCommandKind::CommitSampleNameSelection:
        result.action =
            CommitSampleNameSelection(command.target_row, std::move(command.matched_name), &result.navigation);
        break;
    case SourceCollectionSessionCommandKind::CreateDefaultLabelingTask:
        result.action = CreateDefaultLabelingTask();
        break;
    case SourceCollectionSessionCommandKind::UpsertActiveLabel:
        result.action = UpsertActiveLabel(std::move(command.label), &result.changed);
        break;
    case SourceCollectionSessionCommandKind::SetActiveLabelingAutoAdvance:
        result.action = SetActiveLabelingAutoAdvance(command.enabled);
        break;
    case SourceCollectionSessionCommandKind::SetActiveLabelingSkipLabeledOnAdvance:
        result.action = SetActiveLabelingSkipLabeledOnAdvance(command.enabled);
        break;
    case SourceCollectionSessionCommandKind::SetActiveLabelingOutputPath:
        result.action = SetActiveLabelingOutputPath(std::move(command.path));
        break;
    case SourceCollectionSessionCommandKind::AssignActiveLabelToCurrentSample:
        result.action = AssignActiveLabelToCurrentSample(command.label_code);
        break;
    case SourceCollectionSessionCommandKind::ClearActiveLabelForCurrentSample:
        result.action = ClearActiveLabelForCurrentSample();
        break;
    case SourceCollectionSessionCommandKind::ClearFilters:
        result.action = ClearFilters();
        break;
    case SourceCollectionSessionCommandKind::SetFilterValueSelected:
        result.action = SetFilterValueSelected(
            std::move(command.filter_source_id),
            std::move(command.filter_value_key),
            command.selected);
        break;
    case SourceCollectionSessionCommandKind::SetActiveLabelingFilterSourceSelected:
        result.action = SetActiveLabelingFilterSourceSelected(command.selected);
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
    view.labeling = workflow_->LabelingView(snapshot);
    view.filter = workflow_->FilterView(snapshot);
    return view;
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
    return workflow_->AddReadOnlyAnnotationToActiveSource(path, loaded, message);
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
    return workflow_->CreateDefaultLabelingTask();
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
    return workflow_->SetActiveLabelingOutputPath(std::move(output_path));
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
    return workflow_->ClearFilters(roster_->snapshot());
}

SourceCollectionSessionAction SourceCollectionSession::SetFilterValueSelected(
    std::string source_id,
    std::string value_key,
    bool selected)
{
    return workflow_->SetFilterValueSelected(
        roster_->snapshot(),
        std::move(source_id),
        std::move(value_key),
        selected);
}

SourceCollectionSessionAction SourceCollectionSession::SetActiveLabelingFilterSourceSelected(bool selected)
{
    return workflow_->SetActiveLabelingFilterSourceSelected(roster_->snapshot(), selected);
}

void SourceCollectionSession::MaybeSaveStateCaches(std::uint64_t frame_index)
{
    source_session_state_->MaybeSave(frame_index, roster_->SavedSources(), roster_->current_source_index());
    workflow_->MaybeSaveStateCaches(frame_index);
}

bool SourceCollectionSession::FlushStateCaches()
{
    const bool source_session_saved =
        source_session_state_->Flush(roster_->SavedSources(), roster_->current_source_index());
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
    MergeSourceCollectionSessionAction(
        action,
        workflow_->SyncActiveSource(roster_->current_source_key(), roster_->snapshot()));
    action.navigation_inputs_changed = true;
    MarkSourceSessionCacheDirty();
    return action;
}

void SourceCollectionSession::RestoreSourceSessionCache()
{
    const SourceCollectionSessionStateCache state = source_session_state_->Load();
    if (state.sources.empty()) {
        return;
    }

    source_session_state_->BeginRestore();
    std::optional<std::size_t> restored_active_source_index;
    for (std::size_t source_index = 0; source_index < state.sources.size(); ++source_index) {
        const SourceCollectionSavedSource& source = state.sources[source_index];
        if (!IsRestorableSourcePath(source.path)) {
            continue;
        }

        (void)OpenSource(source.path, source.last_spectrum_index);
        if (state.active_source_index && *state.active_source_index == source_index) {
            restored_active_source_index = roster_->current_source_index();
        }
    }

    if (restored_active_source_index && roster_->has_source(*restored_active_source_index)) {
        (void)ActivateSource(*restored_active_source_index);
    }
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
