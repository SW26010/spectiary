#include "ui/sample_workflow_coordinator.h"

#include "app/local_user_state.h"
#include "domain/sample_annotation_io.h"
#include "domain/source_collection_manifest.h"
#include "ui/sample_annotation_labeling_rules.h"
#include "ui/sample_workflow_preparation.h"
#include "ui/source_collection_session.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace spectiary {
namespace {

using namespace std::chrono_literals;

constexpr auto kWorkflowStateSaveDebounce = 500ms;
constexpr auto kWorkflowStateSaveRetry = 2s;
constexpr std::size_t kMaxLabelUndoEntries = 256;

SampleLabelingController::CanonicalDocumentPublisher
DefaultCanonicalDocumentPublisher()
{
    return [](
               const SampleLabelingAsdfOpenSnapshot& snapshot,
               const SampleLabelingDocument& document,
               const SampleLabelingCanonicalSourceDescriptor& source) {
        return RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
            snapshot,
            document,
            SampleLabelingCompatibilityView(source));
    };
}

struct ActiveSampleWorkflowIdentity {
    bool present = false;
    std::string source_identity;
    std::string task_id;

    friend bool operator==(
        const ActiveSampleWorkflowIdentity& left,
        const ActiveSampleWorkflowIdentity& right) = default;
};

ActiveSampleWorkflowIdentity CaptureActiveSampleWorkflowIdentity(
    const std::optional<std::string>& source_identity,
    const SampleLabelingTask* task)
{
    return ActiveSampleWorkflowIdentity{
        .present = source_identity.has_value() && task != nullptr,
        .source_identity = source_identity.value_or(std::string{}),
        .task_id = task == nullptr ? std::string{} : task->task_id};
}

bool RefreshAttachedCanonicalAnnotation(
    SourceCollectionManifest& manifest,
    SampleAnnotationResult annotation)
{
    const auto attached = std::find_if(
        manifest.annotations.begin(),
        manifest.annotations.end(),
        [&annotation](const SampleAnnotationResult& candidate) {
            return SampleWorkflowPathsReferToSameFile(
                candidate.path,
                annotation.path);
        });
    if (attached == manifest.annotations.end()) {
        return false;
    }
    *attached = std::move(annotation);
    return true;
}

bool RestoreCanonicalOwnerAttachments(
    SourceCollectionManifest& manifest,
    const std::optional<SampleLabelingSourceState>& labeling_state,
    const SampleLabelingCanonicalSourceDescriptor& source,
    const RuntimePaths& runtime_paths)
{
    if (!labeling_state) {
        return false;
    }
    bool attachment_added = false;
    for (const SampleLabelingTask& task :
         labeling_state->tasks) {
        if (!task.persistence.output_path ||
            task.persistence.output_format !=
                SampleLabelingOutputArtifactFormat::CanonicalAsdf ||
            SourceCollectionManifestContainsAnnotation(
                manifest,
                *task.persistence.output_path)) {
            continue;
        }
        if (!runtime_paths.application_data_root.empty() &&
            CheckUserFilePath(*task.persistence.output_path, runtime_paths) !=
                UserFilePathStatus::Allowed) {
            manifest.diagnostics.push_back({
                .kind = SourceCollectionManifestDiagnosticKind::AnnotationIgnored,
                .path = *task.persistence.output_path,
                .detail = "Choose a document outside config, state, logs and unsaved.",
            });
            continue;
        }
        attachment_added =
            IngestReadOnlySampleAnnotation(
                manifest,
                *task.persistence.output_path,
                SampleLabelingCompatibilityView(source)) ||
            attachment_added;
    }
    return attachment_added;
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

enum class PathProbeResult {
    Exists,
    Missing,
    Error,
};

PathProbeResult ProbePath(
    const std::filesystem::path& path)
{
    if (path.empty()) {
        return PathProbeResult::Error;
    }
    std::error_code error;
    const bool exists =
        std::filesystem::exists(path, error);
    if (error) {
        return PathProbeResult::Error;
    }
    return exists
        ? PathProbeResult::Exists
        : PathProbeResult::Missing;
}

bool ShouldRememberLabelingPosition(SampleNavigationRequestKind kind)
{
    return kind == SampleNavigationRequestKind::Previous || kind == SampleNavigationRequestKind::Next ||
           kind == SampleNavigationRequestKind::LabelAdvance ||
           kind == SampleNavigationRequestKind::RestoreLabelUndoPosition;
}

SampleNavigationRequest BuildAutoAdvanceRequest(const SampleLabelingTask& task)
{
    if (!task.session.skip_labeled_on_advance) {
        return SampleNavigationRequest::LabelAdvance();
    }

    std::vector<bool> eligible_samples;
    eligible_samples.reserve(task.values.SampleCount());
    for (int value : task.values.Complete()) {
        eligible_samples.push_back(value == kUnlabeledSampleLabelCode);
    }
    return SampleNavigationRequest::LabelAdvanceToEligible(std::move(eligible_samples));
}

std::optional<SampleLabelResultMetadata> LoadVerifiedLabelMetadataForAnnotation(
    const SampleAnnotationResult& annotation)
{
    if (annotation.labeling_document) {
        return std::nullopt;
    }
    if (annotation.label_metadata) {
        return annotation.label_metadata;
    }
    if (annotation.kind != SampleAnnotationKind::CategoricalInteger || annotation.path.empty()) {
        return std::nullopt;
    }

    SampleLabelResultMetadataLoadResult metadata =
        SampleAnnotationIoAdapter{}.LoadLabelMetadata(
            annotation.path,
            annotation.values.size(),
            annotation.dtype_name);
    if (!metadata.warning.empty() || !metadata.metadata) {
        return std::nullopt;
    }
    return std::move(metadata.metadata);
}

SourceCollectionAnnotationValueView BuildAnnotationValueView(
    const SampleAnnotationResult& annotation,
    std::size_t current_index,
    const SampleLabelingTask* local_task,
    std::string display_name)
{
    SourceCollectionAnnotationValueView view;
    view.name = std::move(display_name);
    view.path = local_task != nullptr && local_task->persistence.output_path ? *local_task->persistence.output_path : annotation.path;
    view.relationship = local_task == nullptr
        ? annotation.relationship
        : SampleAnnotationWorkflowRelationship::LocalLabelingTask;
    if (!annotation.metadata_warning.empty()) {
        view.diagnostic =
            SourceCollectionManifestDiagnostic{
                .kind =
                    SourceCollectionManifestDiagnosticKind::
                        AnnotationMetadataIgnored,
                .path =
                    SampleAnnotationIoAdapter::
                        MetadataPathForResult(
                            annotation.path),
                .detail =
                    annotation.metadata_warning,
            };
    }
    if (local_task != nullptr &&
        annotation.labeling_document &&
        local_task->persistence.output_format ==
            SampleLabelingOutputArtifactFormat::CanonicalAsdf &&
        !local_task->values.IsComplete() &&
        current_index < annotation.labeling_document
                            ->annotation.values.size()) {
        int value = static_cast<int>(
            annotation.labeling_document
                ->annotation.values[current_index]);
        if (local_task->persistence.pending_sample_indices.contains(
                current_index) &&
            current_index < local_task->values.SampleCount()) {
            value = local_task->values.PendingValue(current_index);
        }
        if (local_task->persistence.metadata_save_pending) {
            view.display_text = FormatSampleLabelValue(
                local_task->label_set,
                value);
        } else {
            view.display_text = FormatSampleAnnotationValue(
                annotation,
                SampleAnnotationValue{
                    static_cast<std::int64_t>(value)});
        }
    } else if (local_task != nullptr && local_task->values.IsComplete() &&
               current_index < local_task->values.SampleCount()) {
        view.display_text = FormatSampleLabelValue(
            local_task->label_set,
            local_task->values.Complete()[current_index]);
    } else if (current_index < annotation.values.size()) {
        const SampleAnnotationValue& value =
            annotation.values[current_index];
        view.missing = value.missing;
        if (!view.missing) {
            view.display_text =
                FormatSampleAnnotationValue(
                    annotation,
                    value);
        }
    } else {
        view.missing = true;
    }
    view.can_activate_labeling =
        local_task != nullptr ||
        CanPromoteSampleAnnotationToLabeling(annotation) ||
        annotation.labeling_document != nullptr;
    view.can_filter_samples = local_task != nullptr || annotation.kind != SampleAnnotationKind::ContinuousFloat;
    view.can_sort_samples = local_task == nullptr &&
                            annotation.relationship == SampleAnnotationWorkflowRelationship::PlainAnnotation &&
                            !annotation.label_metadata;
    view.can_rename_annotation = !view.path.empty();
    view.can_remove_annotation = local_task == nullptr;
    if (local_task != nullptr) {
        view.labeling_owner_format = local_task->persistence.output_format;
    } else if (annotation.labeling_document) {
        view.labeling_owner_format =
            SampleLabelingOutputArtifactFormat::CanonicalAsdf;
    } else if (annotation.relationship ==
               SampleAnnotationWorkflowRelationship::
                   ExternalLabelResult) {
        view.labeling_owner_format =
            SampleLabelingOutputArtifactFormat::
                LegacyNpyWithSidecar;
    }
    if (local_task != nullptr && local_task->persistence.output_path) {
        const PathProbeResult output_probe =
            ProbePath(*local_task->persistence.output_path);
        view.output_missing =
            output_probe ==
                PathProbeResult::Missing;
        if (local_task->persistence.output_format ==
            SampleLabelingOutputArtifactFormat::
                LegacyNpyWithSidecar) {
            view.metadata_missing =
                ProbePath(
                    SampleAnnotationIoAdapter::
                        MetadataPathForResult(
                            *local_task->persistence.output_path)) ==
                PathProbeResult::Missing;
        }
        if (view.output_missing &&
            local_task->persistence.save_state.kind !=
                SampleLabelSaveStateKind::Pending &&
            local_task->persistence.save_state.kind !=
                SampleLabelSaveStateKind::Failed) {
            view.can_remove_annotation = true;
        }
        if (view.output_missing) {
            view.can_activate_labeling = false;
        }
    }
    return view;
}

SourceCollectionAnnotationValueView BuildLocalTaskAnnotationValueView(
    const SampleLabelingTask& task,
    std::size_t current_index,
    std::string display_name)
{
    SourceCollectionAnnotationValueView view;
    view.name = std::move(display_name);
    view.path = task.persistence.output_path.value_or(std::filesystem::path{});
    view.relationship = SampleAnnotationWorkflowRelationship::LocalLabelingTask;
    if (task.values.IsComplete()) {
        if (current_index < task.values.SampleCount()) {
            view.display_text = FormatSampleLabelValue(
                task.label_set,
                task.values.Complete()[current_index]);
        } else {
            view.missing = true;
        }
    }
    view.can_activate_labeling = task.persistence.output_path.has_value();
    view.can_filter_samples =
        task.persistence.output_path.has_value() &&
        task.values.IsComplete();
    view.can_sort_samples = false;
    view.can_rename_annotation = task.persistence.output_path.has_value();
    view.can_remove_annotation = false;
    view.labeling_owner_format = task.persistence.output_format;
    if (task.persistence.output_path) {
        const PathProbeResult output_probe =
            ProbePath(*task.persistence.output_path);
        view.output_missing =
            (task.persistence.output_format ==
                 SampleLabelingOutputArtifactFormat::
                     CanonicalAsdf &&
             !task.values.IsComplete()) ||
            output_probe ==
                PathProbeResult::Missing;
        if (task.persistence.output_format ==
            SampleLabelingOutputArtifactFormat::
                LegacyNpyWithSidecar) {
            view.metadata_missing =
                ProbePath(
                    SampleAnnotationIoAdapter::
                        MetadataPathForResult(
                            *task.persistence.output_path)) ==
                PathProbeResult::Missing;
        }
        if (output_probe == PathProbeResult::Missing &&
            task.persistence.save_state.kind !=
                SampleLabelSaveStateKind::Pending &&
            task.persistence.save_state.kind !=
                SampleLabelSaveStateKind::Failed) {
            view.can_remove_annotation = true;
        }
        if (view.output_missing) {
            view.can_activate_labeling = false;
        }
    }
    return view;
}

void ApplyLabelingLeaseIssue(
    SampleWorkflowTransitionOutcome& outcome,
    const SampleLabelingOperationResult& operation)
{
    outcome.labeling_issue = operation.issue;
}

}  // namespace

void MergeSampleWorkflowTransitionOutcome(
    SampleWorkflowTransitionOutcome& target,
    SampleWorkflowTransitionOutcome source)
{
    MergeSourceCollectionSessionAction(
        target.action,
        source.action);
    target.navigation = std::move(source.navigation);
    if (source.snapshot_target_updated) {
        target.snapshot_target_updated = true;
        target.snapshot_index_to_load =
            source.snapshot_index_to_load;
    }
    target.changed = target.changed || source.changed;
    target.loaded = target.loaded || source.loaded;
    target.invalidate_view =
        target.invalidate_view ||
        source.invalidate_view;
    if (source.label_write) {
        target.label_write =
            std::move(source.label_write);
    }
    if (source.labeling_issue !=
        SampleLabelingOperationResult::Issue::None) {
        target.labeling_issue =
            source.labeling_issue;
    }
    if (!source.message.empty()) {
        target.message = std::move(source.message);
    }
}

SampleWorkflowCoordinator::SampleWorkflowCoordinator()
    : SampleWorkflowCoordinator(std::filesystem::path{},
          std::filesystem::path{}, std::filesystem::path{},
          RuntimePaths{})
{
}

SampleWorkflowCoordinator::SampleWorkflowCoordinator(
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path)
    : SampleWorkflowCoordinator(std::move(navigation_state_cache_path),
          std::move(labeling_state_cache_path), std::filesystem::path{}, RuntimePaths{})
{
}
SampleWorkflowCoordinator::SampleWorkflowCoordinator(
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path,
    std::filesystem::path workflow_state_cache_path,
    const RuntimePaths& runtime_paths)
    : SampleWorkflowCoordinator(
          std::move(navigation_state_cache_path),
          std::move(labeling_state_cache_path),
          std::move(workflow_state_cache_path),
          [runtime_paths](const std::filesystem::path& path) { return LoadSampleLabelingStateCache(runtime_paths, path, {}, SampleLabelingStateCacheLoadPolicy::AllowPersistentOutputs); },
          [runtime_paths](const std::filesystem::path& path) {
              return LoadSampleWorkflowStateCache(runtime_paths, path, {});
          }, runtime_paths)
{
}

SampleWorkflowCoordinator::SampleWorkflowCoordinator(
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path,
    std::filesystem::path workflow_state_cache_path,
    SampleLabelingController::StateCacheLoader labeling_state_cache_loader,
    WorkflowStateCacheLoader workflow_state_cache_loader,
    const RuntimePaths& runtime_paths)
    : SampleWorkflowCoordinator(
          std::move(navigation_state_cache_path),
          std::move(labeling_state_cache_path),
          std::move(workflow_state_cache_path),
          std::move(labeling_state_cache_loader),
          std::move(workflow_state_cache_loader),
          SampleLabelingController::CanonicalDocumentPublisher{}, runtime_paths)
{
}

SampleWorkflowCoordinator::SampleWorkflowCoordinator(
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path,
    std::filesystem::path workflow_state_cache_path,
    SampleLabelingController::StateCacheLoader labeling_state_cache_loader,
    WorkflowStateCacheLoader workflow_state_cache_loader,
    SampleLabelingController::CanonicalDocumentPublisher
        canonical_document_publisher,
    const RuntimePaths& runtime_paths)
    : SampleWorkflowCoordinator(
          std::move(navigation_state_cache_path),
          std::move(labeling_state_cache_path),
          std::move(workflow_state_cache_path),
          std::move(labeling_state_cache_loader),
          std::move(workflow_state_cache_loader),
          std::move(canonical_document_publisher),
          SampleLabelingController::CanonicalValuesPublisher{}, runtime_paths)
{
}

SampleWorkflowCoordinator::SampleWorkflowCoordinator(
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path,
    std::filesystem::path workflow_state_cache_path,
    SampleLabelingController::StateCacheLoader labeling_state_cache_loader,
    WorkflowStateCacheLoader workflow_state_cache_loader,
    SampleLabelingController::CanonicalDocumentPublisher
        canonical_document_publisher,
    SampleLabelingController::CanonicalValuesPublisher
        canonical_values_publisher,
    const RuntimePaths& runtime_paths)
    : navigation_(std::move(navigation_state_cache_path)),
      labeling_(
          std::move(labeling_state_cache_path),
          std::move(labeling_state_cache_loader),
          canonical_document_publisher
              ? std::move(canonical_document_publisher)
              : DefaultCanonicalDocumentPublisher(),
          std::move(canonical_values_publisher), runtime_paths),
      runtime_paths_(runtime_paths),
      workflow_state_cache_path_(std::move(workflow_state_cache_path)),
      workflow_state_cache_loader_(std::move(workflow_state_cache_loader)),
      workflow_state_persistence_(kWorkflowStateSaveDebounce, kWorkflowStateSaveRetry)
{
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::Apply(
    SourceCollectionIntent intent,
    const SpectrumSnapshotHandle& snapshot)
{
    const std::uint64_t presentation_revision_before =
        labeling_.View().revision;
    SampleWorkflowTransitionOutcome outcome;
    switch (intent.kind) {
    case SourceCollectionIntentKind::AddReadOnlyAnnotationResult:
        outcome = AddReadOnlyAnnotationToActiveSource(intent.path);
        break;
    case SourceCollectionIntentKind::RemoveReadOnlyAnnotationResult:
        outcome = RemoveReadOnlyAnnotationFromActiveSource(intent.path);
        break;
    case SourceCollectionIntentKind::RenameAnnotationResultDisplayName:
        outcome = RenameAnnotationDisplayNameForActiveSource(
            std::move(intent.path),
            std::move(intent.display_name));
        break;
    case SourceCollectionIntentKind::SwitchActive:
    case SourceCollectionIntentKind::Remove:
        break;
    }
    return CompleteTransition(
        std::move(outcome),
        snapshot,
        presentation_revision_before);
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::Apply(
    SampleNavigationIntent intent,
    const SpectrumSnapshotHandle& snapshot,
    NavigationTargetResolutionReport* target_resolution)
{
    const std::uint64_t presentation_revision_before =
        labeling_.View().revision;
    SampleWorkflowTransitionOutcome outcome;
    switch (intent.kind) {
    case SampleNavigationIntentKind::Move:
        outcome = RequestSampleNavigation(
            intent.request,
            snapshot,
            std::nullopt,
            target_resolution);
        break;
    case SampleNavigationIntentKind::SetSampleNameQuery:
        outcome = SetSampleNameQuery(std::move(intent.query));
        break;
    case SampleNavigationIntentKind::CommitSampleNameSelection:
        outcome = CommitSampleNameSelection(
            intent.target_row,
            std::move(intent.matched_name),
            snapshot);
        break;
    }
    return CompleteTransition(
        std::move(outcome),
        snapshot,
        presentation_revision_before);
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::Apply(
    ActiveSampleWorkflowIntent intent,
    const SpectrumSnapshotHandle& snapshot,
    NavigationTargetResolutionReport* target_resolution)
{
    const std::uint64_t presentation_revision_before =
        labeling_.View().revision;
    const bool reconcile_external_formalization =
        intent.kind ==
            ActiveSampleWorkflowIntentKind::
                RecoverTemporaryLabelingTask ||
        intent.kind ==
            ActiveSampleWorkflowIntentKind::
                DeleteTemporaryLabelingTask;
    SampleWorkflowTransitionOutcome outcome;
    switch (intent.kind) {
    case ActiveSampleWorkflowIntentKind::StartOrResumeTemporaryLabelingTask:
        outcome = StartOrResumeTemporaryLabelingTask();
        break;
    case ActiveSampleWorkflowIntentKind::RecoverTemporaryLabelingTask:
        outcome = RecoverTemporaryLabelingTask(
            std::move(intent.source_identity),
            std::move(intent.task_id));
        break;
    case ActiveSampleWorkflowIntentKind::DeleteTemporaryLabelingTask:
        outcome = DeleteTemporaryLabelingTask(
            std::move(intent.source_identity),
            std::move(intent.task_id));
        break;
    case ActiveSampleWorkflowIntentKind::ActivateLabelingTaskFromAnnotation:
        outcome = ActivateLabelingTaskFromAnnotation(
            std::move(intent.path));
        break;
    case ActiveSampleWorkflowIntentKind::DeleteActiveLabelingTask:
        outcome = DeleteActiveLabelingTask();
        break;
    case ActiveSampleWorkflowIntentKind::RenameActiveLabelingTask:
        outcome = RenameActiveLabelingTask(
            std::move(intent.task_id),
            std::move(intent.requested_name));
        break;
    case ActiveSampleWorkflowIntentKind::UpsertActiveLabel:
        outcome = UpsertActiveLabel(std::move(intent.label));
        break;
    case ActiveSampleWorkflowIntentKind::UpdateActiveLabel:
        outcome = UpdateActiveLabel(
            intent.label_code,
            std::move(intent.label),
            intent.allow_used_label_code_change);
        break;
    case ActiveSampleWorkflowIntentKind::RemoveActiveLabel:
        outcome = RemoveActiveLabel(intent.label_code);
        break;
    case ActiveSampleWorkflowIntentKind::SetActiveLabelingAutoAdvance:
        outcome = SetActiveLabelingAutoAdvance(intent.enabled);
        break;
    case ActiveSampleWorkflowIntentKind::SetActiveLabelingSkipLabeledOnAdvance:
        outcome = SetActiveLabelingSkipLabeledOnAdvance(
            intent.enabled);
        break;
    case ActiveSampleWorkflowIntentKind::SetActiveLabelingOutputPath:
        outcome = SetActiveLabelingOutputPath(std::move(intent.path));
        break;
    case ActiveSampleWorkflowIntentKind::ExportActiveLabels:
        outcome = ExportActiveLabels(
            std::move(intent.path),
            intent.export_format);
        break;
    case ActiveSampleWorkflowIntentKind::DeactivateActiveLabelingTask:
        outcome = DeactivateActiveLabelingTask();
        break;
    case ActiveSampleWorkflowIntentKind::AssignActiveLabelToCurrentSample:
        outcome = AssignActiveLabelToCurrentSample(
            snapshot,
            intent.label_code,
            target_resolution);
        break;
    case ActiveSampleWorkflowIntentKind::ClearActiveLabelForCurrentSample:
        outcome = ClearActiveLabelForCurrentSample(
            snapshot,
            target_resolution);
        break;
    case ActiveSampleWorkflowIntentKind::UndoLastLabelWrite:
        outcome = UndoLastLabelWrite(snapshot);
        break;
    }
    if (reconcile_external_formalization &&
        SynchronizeCanonicalAsdfOwnerAttachments(
            snapshot,
            &outcome.action)) {
        outcome.changed = true;
        outcome.invalidate_view = true;
        ApplyNavigationInputEffects(
            outcome,
            ReconcileNavigationInputs(
                snapshot,
                NavigationInputReconcileRequest{
                    .filters_changed = true,
                    .sorting_changed = true}));
    }
    return CompleteTransition(
        std::move(outcome),
        snapshot,
        presentation_revision_before);
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::Apply(
    SampleFilteringIntent intent,
    const SpectrumSnapshotHandle& snapshot)
{
    const std::uint64_t presentation_revision_before =
        labeling_.View().revision;
    SampleWorkflowTransitionOutcome outcome;
    switch (intent.kind) {
    case SampleFilteringIntentKind::ClearFilters:
        outcome = ClearFilters(snapshot);
        break;
    case SampleFilteringIntentKind::AddFilterSource:
        outcome = AddFilterSource(
            snapshot,
            std::move(intent.source_id));
        break;
    case SampleFilteringIntentKind::RemoveFilterSource:
        outcome = RemoveFilterSource(
            snapshot,
            std::move(intent.source_id));
        break;
    case SampleFilteringIntentKind::SetFilterValueSelected:
        outcome = SetFilterValueSelected(
            snapshot,
            std::move(intent.source_id),
            std::move(intent.value_key),
            intent.selected);
        break;
    }
    return CompleteTransition(
        std::move(outcome),
        snapshot,
        presentation_revision_before);
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::Apply(
    SampleSortingIntent intent,
    const SpectrumSnapshotHandle& snapshot)
{
    const std::uint64_t presentation_revision_before =
        labeling_.View().revision;
    SampleWorkflowTransitionOutcome outcome;
    switch (intent.kind) {
    case SampleSortingIntentKind::ClearSorting:
        outcome = ClearSampleSorting(snapshot);
        break;
    case SampleSortingIntentKind::AddSortSource:
        outcome = AddSampleSortSource(
            snapshot,
            std::move(intent.source_id));
        break;
    case SampleSortingIntentKind::RemoveSortSource:
        outcome = RemoveSampleSortSource(
            snapshot,
            std::move(intent.source_id));
        break;
    case SampleSortingIntentKind::SetSortSource:
        outcome = SetSampleSortSource(
            snapshot,
            std::move(intent.source_id));
        break;
    case SampleSortingIntentKind::SetSortDirection:
        outcome = SetSampleSortDirection(
            snapshot,
            intent.direction);
        break;
    }
    return CompleteTransition(
        std::move(outcome),
        snapshot,
        presentation_revision_before);
}

SampleWorkflowTransitionOutcome
SampleWorkflowCoordinator::CompleteTransition(
    SampleWorkflowTransitionOutcome outcome,
    const SpectrumSnapshotHandle& snapshot,
    std::uint64_t presentation_revision_before,
    bool align_snapshot_target) const
{
    if (align_snapshot_target &&
        !outcome.snapshot_target_updated) {
        const std::optional<std::size_t> pending_index =
            navigation_.pending_index();
        const std::optional<std::size_t> target_index =
            pending_index
            ? pending_index
            : navigation_.current_index();
        if (target_index && snapshot &&
            snapshot->collection.spectrum_count > 0 &&
            snapshot->collection.current_index !=
                *target_index) {
            outcome.snapshot_target_updated = true;
            outcome.snapshot_index_to_load =
                target_index;
        }
    }
    if (outcome.snapshot_index_to_load) {
        outcome.action.navigation_inputs_changed = true;
    }
    outcome.invalidate_view =
        outcome.invalidate_view ||
        outcome.action.source_roster_changed ||
        outcome.action.snapshot_changed ||
        outcome.action.workflow_changed ||
        outcome.action.navigation_inputs_changed ||
        outcome.changed ||
        outcome.loaded ||
        labeling_.View().revision !=
            presentation_revision_before;
    return outcome;
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::SyncActiveSource(
    std::optional<std::string> source_key,
    const SpectrumSnapshotHandle& snapshot)
{
    const std::uint64_t presentation_revision_before =
        labeling_.View().revision;
    DiscardPreparedViewCaches();
    SampleWorkflowTransitionOutcome outcome;
    if (!source_key || !snapshot || snapshot->source.path.empty()) {
        navigation_.ClearActiveSource();
        ClearSampleWorkflow(outcome.action);
        return CompleteTransition(
            std::move(outcome),
            snapshot,
            presentation_revision_before,
            true);
    }

    outcome = SyncActiveSourceWithContext(
        std::move(source_key),
        snapshot,
        LoadSourceCollectionContext(*snapshot),
        std::nullopt);
    return CompleteTransition(
        std::move(outcome),
        snapshot,
        presentation_revision_before,
        true);
}

PreparedSampleWorkflowActivationResult SampleWorkflowCoordinator::SyncPreparedActiveSource(
    std::optional<std::string> source_key,
    const SpectrumSnapshotHandle& snapshot,
    SourceCollectionContext context,
    PreparedSampleWorkflowState prepared_workflow,
    bool present_explicit_member,
    bool activate)
{
    PreparedSampleWorkflowActivationResult result;
    SourceCollectionSessionAction& action = result.action;
    if (!source_key || !snapshot || snapshot->source.path.empty()) {
        navigation_.ClearActiveSource();
        ClearSampleWorkflow(action);
        return result;
    }

    const SourceCollectionIdentity identity = context.identity;
    if (!activate) {
        // Register the prepared context for later selection without touching
        // active navigation, labeling, or presentation state.
        AdoptPreparedCache(prepared_workflow.preparation_cache, result.background_retirement);
        if (auto retired = navigation_.ActivatePreparedSource(std::move(*source_key), snapshot,
                identity, std::move(context.manifest), std::move(prepared_workflow), false)) {
            result.background_retirement.push_back(std::move(retired));
        }
        return result;
    }
    const bool workflow_identity_changed =
        !active_sample_workflow_identity_ ||
        *active_sample_workflow_identity_ != identity.id;
    const bool workflow_context_changed =
        !active_sample_workflow_context_fingerprint_ ||
        *active_sample_workflow_context_fingerprint_ !=
            identity.context_fingerprint;
    std::optional<SampleLabelingCanonicalSourceDescriptor>
        labeling_source_descriptor;
    bool canonical_attachments_restored = false;
    if (workflow_identity_changed || workflow_context_changed) {
        labeling_source_descriptor =
            BuildSampleLabelingCanonicalSourceDescriptor(
                *snapshot,
                identity,
                context.manifest);
        canonical_attachments_restored =
            RestoreCanonicalOwnerAttachments(
                context.manifest,
                prepared_workflow.labeling_source_state,
                *labeling_source_descriptor,
                runtime_paths_);
        if (canonical_attachments_restored) {
            action.annotation_roster_changed = true;
            action.navigation_inputs_changed = true;
        }
    }
    AdoptPreparedCache(prepared_workflow.preparation_cache, result.background_retirement);
    if (active_sample_workflow_identity_ && *active_sample_workflow_identity_ != identity.id) {
        StoreActiveWorkflowState();
    }
    if (!active_sample_workflow_identity_ || *active_sample_workflow_identity_ != identity.id) {
        ClearLabelUndoHistory();
    }
    SampleLabelingPreparedSourceActivationResult
        labeling_activation =
            labeling_.ActivatePreparedSource(
                identity,
                std::move(
                    prepared_workflow
                        .labeling_source_state),
                std::move(
                    labeling_source_descriptor));
    if (std::optional<SampleAnnotationResult>
            canonical_attachment =
                labeling_.
                    ActiveCanonicalAsdfAnnotationProjection()) {
        action.navigation_inputs_changed =
            RefreshAttachedCanonicalAnnotation(
                context.manifest,
                std::move(*canonical_attachment)) ||
            action.navigation_inputs_changed;
    }
    if (labeling_activation.background_retirement) {
        result.background_retirement.push_back(
            std::move(
                labeling_activation
                    .background_retirement));
    }
    if (labeling_activation
            .prepared_task_projection_changed ||
        canonical_attachments_restored) {
        const std::shared_ptr<const
            SampleWorkflowPreparationCacheBundle>
            preparation_cache =
                prepared_workflow.preparation_cache;
        const SampleWorkflowPreparationCacheBundle
            empty_cache;
        const std::optional<SampleLabelingSourceState>
            latest_labeling_state =
                labeling_.SourceStateForIdentity(
                    identity.id);
        PreparedSampleWorkflowState refreshed =
            PrepareSampleWorkflowStateFromCache(
                *snapshot,
                context,
                prepared_workflow.prepared_index,
                preparation_cache
                    ? *preparation_cache
                    : empty_cache,
                &prepared_workflow
                     .workflow_source_state,
                latest_labeling_state
                    ? &*latest_labeling_state
                    : nullptr);
        refreshed.preparation_cache =
            preparation_cache;
        result.background_retirement.push_back(
            MakeBackgroundRetirementHandle(
                std::move(prepared_workflow)));
        prepared_workflow =
            std::move(refreshed);
    }
    result.background_retirement.push_back(
        MakeBackgroundRetirementHandle(std::move(workflow_sources_)));
    workflow_sources_ = SampleWorkflowSourcePolicy{};
    workflow_sources_.RestoreState(prepared_workflow.workflow_source_state);
    active_sample_workflow_identity_ = identity.id;
    active_sample_workflow_context_fingerprint_ = identity.context_fingerprint;
    if (prepared_filter_view_) {
        result.background_retirement.push_back(
            MakeBackgroundRetirementHandle(std::move(*prepared_filter_view_)));
    }
    if (prepared_sorting_view_) {
        result.background_retirement.push_back(
            MakeBackgroundRetirementHandle(std::move(*prepared_sorting_view_)));
    }
    prepared_filter_view_.emplace(std::move(prepared_workflow.filter_view));
    prepared_sorting_view_.emplace(std::move(prepared_workflow.sorting_view));
    if (BackgroundRetirementHandle retired_navigation = navigation_.ActivatePreparedSource(
            std::move(*source_key),
            snapshot,
            identity,
            std::move(context.manifest),
            std::move(prepared_workflow))) {
        result.background_retirement.push_back(std::move(retired_navigation));
    }
    if (present_explicit_member) {
        (void)navigation_.PresentExplicitSample(snapshot->collection.current_index);
        DiscardPreparedViewCaches();
    }
    action.workflow_changed = true;
    action.navigation_inputs_changed = true;
    return result;
}

bool SampleWorkflowCoordinator::CanReusePreparedKnownSource(
    std::optional<std::string> source_key,
    const SourceCollectionIdentity& identity) const
{
    if (!source_key) {
        return false;
    }
    const std::optional<SourceCollectionIdentity> known =
        navigation_.KnownSourceIdentity(*source_key);
    return known && known->id == identity.id &&
           known->context_fingerprint == identity.context_fingerprint &&
           known->spectrum_count == identity.spectrum_count;
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::SyncReusedPreparedKnownSource(
    std::optional<std::string> source_key,
    const SpectrumSnapshotHandle& snapshot,
    const SourceCollectionIdentity& identity,
    bool present_explicit_member)
{
    if (!snapshot || !CanReusePreparedKnownSource(source_key, identity)) {
        return {};
    }
    auto outcome = SyncKnownActiveSource(
        std::move(source_key),
        snapshot);
    if (present_explicit_member &&
        navigation_.PresentExplicitSample(snapshot->collection.current_index)) {
        outcome.snapshot_index_to_load.reset();
        outcome.action.navigation_inputs_changed = true;
        outcome.invalidate_view = true;
        DiscardPreparedViewCaches();
    }
    return outcome;
}

std::optional<SourceCollectionIdentity> SampleWorkflowCoordinator::ActiveSourceIdentity() const
{
    return navigation_.active_source_identity();
}

std::optional<SourceCollectionIdentity> SampleWorkflowCoordinator::KnownSourceIdentity(
    std::string_view source_key) const
{
    return navigation_.KnownSourceIdentity(source_key);
}

std::optional<std::size_t> SampleWorkflowCoordinator::KnownSourceCurrentIndex(
    std::string_view source_key) const
{
    return navigation_.KnownSourceCurrentIndex(source_key);
}

std::optional<SampleWorkflowSourceState> SampleWorkflowCoordinator::WorkflowStateForSourceIdentity(
    std::string_view source_identity)
{
    if (active_sample_workflow_identity_ && *active_sample_workflow_identity_ == source_identity) {
        return workflow_sources_.StoreState();
    }
    EnsureWorkflowStateCacheLoaded();
    const SampleWorkflowSourceState* state = CachedWorkflowState(source_identity);
    return state == nullptr ? std::nullopt : std::optional<SampleWorkflowSourceState>{*state};
}

std::optional<SampleLabelingSourceState> SampleWorkflowCoordinator::LabelingStateForSourceIdentity(
    std::string_view source_identity)
{
    return labeling_.SourceStateForIdentity(source_identity);
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::SyncKnownActiveSource(
    std::optional<std::string> source_key,
    const SpectrumSnapshotHandle& snapshot)
{
    const std::uint64_t presentation_revision_before =
        labeling_.View().revision;
    DiscardPreparedViewCaches();
    SampleWorkflowTransitionOutcome outcome;
    if (!source_key || !snapshot || snapshot->source.path.empty()) {
        navigation_.ClearActiveSource();
        ClearSampleWorkflow(outcome.action);
        return CompleteTransition(
            std::move(outcome),
            snapshot,
            presentation_revision_before,
            true);
    }

    const std::optional<SourceCollectionIdentity> identity =
        navigation_.ActivateKnownSource(*source_key);
    if (!identity) {
        navigation_.ClearActiveSource();
        ClearSampleWorkflow(outcome.action);
        return CompleteTransition(
            std::move(outcome),
            snapshot,
            presentation_revision_before,
            true);
    }

    const bool workflow_identity_changed =
        !active_sample_workflow_identity_ || *active_sample_workflow_identity_ != identity->id;
    const bool workflow_context_changed =
        !active_sample_workflow_context_fingerprint_ ||
        *active_sample_workflow_context_fingerprint_ != identity->context_fingerprint;
    if (workflow_identity_changed) {
        ClearLabelUndoHistory();
    }

    const SourceCollectionManifest* context =
        navigation_.active_context();
    if (context == nullptr) {
        ClearSampleWorkflow(outcome.action);
        return CompleteTransition(
            std::move(outcome),
            snapshot,
            presentation_revision_before,
            true);
    }
    if (workflow_identity_changed || workflow_context_changed) {
        SyncSampleWorkflowSession(
            *identity,
            BuildSampleLabelingCanonicalSourceDescriptor(
                *snapshot,
                *identity,
                *context),
            outcome.action);
    }
    if (workflow_identity_changed || workflow_context_changed) {
        ApplyNavigationInputEffects(
            outcome,
            ReconcileNavigationInputs(
                snapshot,
                NavigationInputReconcileRequest{.filters_changed = true, .sorting_changed = true}));
    }
    return CompleteTransition(
        std::move(outcome),
        snapshot,
        presentation_revision_before,
        true);
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::SyncActiveSourceWithContext(
    std::optional<std::string> source_key,
    const SpectrumSnapshotHandle& snapshot,
    SourceCollectionContext context,
    std::optional<std::size_t> prepared_index)
{
    SampleWorkflowTransitionOutcome outcome;
    if (!source_key || !snapshot || snapshot->source.path.empty()) {
        navigation_.ClearActiveSource();
        ClearSampleWorkflow(outcome.action);
        return outcome;
    }

    const SourceCollectionIdentity& identity = context.identity;
    const bool workflow_identity_changed =
        !active_sample_workflow_identity_ || *active_sample_workflow_identity_ != identity.id;
    const bool workflow_context_changed =
        !active_sample_workflow_context_fingerprint_ ||
        *active_sample_workflow_context_fingerprint_ != identity.context_fingerprint;
    std::optional<SampleLabelingCanonicalSourceDescriptor>
        labeling_source_descriptor;
    if (workflow_identity_changed || workflow_context_changed) {
        labeling_source_descriptor =
            BuildSampleLabelingCanonicalSourceDescriptor(
                *snapshot,
                identity,
                context.manifest);
    }

    if (workflow_identity_changed) {
        ClearLabelUndoHistory();
    }

    navigation_.ActivateSource(
        std::move(*source_key),
        snapshot,
        identity,
        std::move(context.manifest),
        prepared_index);
    if (labeling_source_descriptor) {
        SyncSampleWorkflowSession(
            identity,
            std::move(*labeling_source_descriptor),
            outcome.action);
    }
    if (workflow_identity_changed || workflow_context_changed) {
        ApplyNavigationInputEffects(
            outcome,
            ReconcileNavigationInputs(
                snapshot,
                NavigationInputReconcileRequest{.filters_changed = true, .sorting_changed = true}));
    }
    return outcome;
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::ClearActiveWorkflow()
{
    const std::uint64_t presentation_revision_before =
        labeling_.View().revision;
    SampleWorkflowTransitionOutcome outcome;
    ClearSampleWorkflow(outcome.action);
    return CompleteTransition(
        std::move(outcome),
        nullptr,
        presentation_revision_before);
}

void SampleWorkflowCoordinator::BeginRestoringSourceSession()
{
    restoring_source_session_ = true;
}

void SampleWorkflowCoordinator::EndRestoringSourceSession()
{
    restoring_source_session_ = false;
}

BackgroundRetirementHandle SampleWorkflowCoordinator::RemoveSource(std::string_view source_key)
{
    return navigation_.RemoveSource(source_key);
}

void SampleWorkflowCoordinator::DiscardPreparedViewCaches()
{
    prepared_filter_view_.reset();
    prepared_sorting_view_.reset();
}

std::vector<BackgroundRetirementHandle> SampleWorkflowCoordinator::ReleaseBackgroundResourcesForShutdown()
{
    std::vector<BackgroundRetirementHandle> resources =
        labeling_.ReleaseBackgroundResourcesForShutdown();
    if (BackgroundRetirementHandle navigation =
            navigation_.ReleaseBackgroundResourcesForShutdown()) {
        resources.push_back(std::move(navigation));
    }
    if (workflow_state_cache_snapshot_) {
        resources.push_back(std::move(workflow_state_cache_snapshot_));
    }
    if (!workflow_state_cache_.sources_by_identity.empty()) {
        resources.push_back(
            MakeBackgroundRetirementHandle(std::exchange(workflow_state_cache_, {})));
    }
    resources.push_back(MakeBackgroundRetirementHandle(std::move(workflow_sources_)));
    workflow_sources_ = SampleWorkflowSourcePolicy{};
    if (prepared_filter_view_) {
        resources.push_back(MakeBackgroundRetirementHandle(std::move(*prepared_filter_view_)));
        prepared_filter_view_.reset();
    }
    if (prepared_sorting_view_) {
        resources.push_back(MakeBackgroundRetirementHandle(std::move(*prepared_sorting_view_)));
        prepared_sorting_view_.reset();
    }
    return resources;
}

void SampleWorkflowCoordinator::SetDeferredSampleNavigation(bool enabled)
{
    deferred_sample_navigation_ = enabled;
    if (!enabled) {
        CancelDeferredSampleNavigation();
    }
}

bool SampleWorkflowCoordinator::CommitDeferredSampleNavigation(std::size_t spectrum_index)
{
    const bool remember_labeling_position =
        navigation_.pending_navigation_remembers_labeling_position();
    if (!navigation_.CommitDeferredNavigation(spectrum_index)) {
        return false;
    }
    if (remember_labeling_position) {
        (void)labeling_.RememberActivePosition(spectrum_index);
    }
    return true;
}

bool SampleWorkflowCoordinator::RetargetDeferredSampleNavigation(std::size_t spectrum_index)
{
    return navigation_.RetargetDeferredNavigation(spectrum_index);
}

void SampleWorkflowCoordinator::CompletePreparedDeferredSampleNavigation(
    const PendingSampleNavigation& pending)
{
    const std::optional<SourceCollectionIdentity> active_identity =
        navigation_.active_source_identity();
    if (active_identity && active_identity->id == pending.source_identity.id &&
        navigation_.current_index() == pending.spectrum_index &&
        pending.remember_labeling_position) {
        (void)labeling_.RememberActivePosition(pending.spectrum_index);
    }
}

void SampleWorkflowCoordinator::CancelDeferredSampleNavigation()
{
    navigation_.CancelDeferredNavigation();
}

std::optional<std::size_t> SampleWorkflowCoordinator::pending_sample_index() const
{
    return navigation_.pending_index();
}

std::optional<PendingSampleNavigation> SampleWorkflowCoordinator::pending_sample_navigation() const
{
    const std::optional<SourceCollectionIdentity> identity = navigation_.active_source_identity();
    const std::optional<std::size_t> index = navigation_.pending_index();
    if (!identity || !index) {
        return std::nullopt;
    }
    return PendingSampleNavigation{
        *identity,
        *index,
        navigation_.pending_navigation_remembers_labeling_position(),
    };
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::RequestSampleNavigation(
    const SampleNavigationRequest& request,
    const SpectrumSnapshotHandle& snapshot,
    std::optional<std::size_t> deferred_base_index,
    NavigationTargetResolutionReport* target_resolution)
{
    SampleWorkflowTransitionOutcome result;
    const std::optional<std::size_t> pending_index_before = navigation_.pending_index();
    const std::optional<std::size_t> current_index_before = navigation_.current_index();
    result.navigation = deferred_sample_navigation_
        ? navigation_.NavigateDeferred(
              request,
              ShouldRememberLabelingPosition(request.kind),
              deferred_base_index,
              target_resolution)
        : navigation_.Navigate(request);
    const std::optional<std::size_t> pending_index_after =
        navigation_.pending_index();
    const std::optional<std::size_t> current_index_after =
        navigation_.current_index();
    const std::optional<std::size_t> snapshot_target_before =
        deferred_sample_navigation_
        ? pending_index_before
        : current_index_before;
    const std::optional<std::size_t> snapshot_target_after =
        deferred_sample_navigation_
        ? pending_index_after
        : current_index_after;
    if (snapshot_target_after != snapshot_target_before) {
        result.snapshot_target_updated = true;
        if (snapshot_target_after &&
            (!snapshot ||
             snapshot->collection.current_index !=
                 *snapshot_target_after)) {
            result.snapshot_index_to_load =
                snapshot_target_after;
        } else {
            result.action.navigation_inputs_changed = true;
        }
    }
    if (result.navigation.has_active_source && result.navigation.target_found) {
        if (!deferred_sample_navigation_ && ShouldRememberLabelingPosition(request.kind)) {
            (void)labeling_.RememberActivePosition(result.navigation.current_index);
        }
    }
    if (!result.navigation.has_active_source || !result.navigation.target_found) {
        return result;
    }
    return result;
}

SampleWorkflowTransitionOutcome
SampleWorkflowCoordinator::AddReadOnlyAnnotationToActiveSource(
    const std::filesystem::path& path)
{
    SampleWorkflowTransitionOutcome outcome;
    if (!runtime_paths_.application_data_root.empty() &&
        CheckUserFilePath(path, runtime_paths_) != UserFilePathStatus::Allowed) {
        outcome.message = "Choose a document outside config, state, logs and unsaved.";
        return outcome;
    }
    const SourceCollectionManifest* context_before =
        navigation_.active_context();
    const std::size_t diagnostic_count_before =
        context_before == nullptr
        ? 0U
        : context_before->diagnostics.size();
    outcome.loaded =
        navigation_.AddReadOnlyAnnotationToActiveSource(
            path,
            &outcome.message);
    const SourceCollectionManifest* context_after =
        navigation_.active_context();
    outcome.invalidate_view =
        context_after != nullptr &&
        context_after->diagnostics.size() !=
            diagnostic_count_before;
    if (outcome.loaded) {
        std::optional<std::filesystem::path>
            missing_owner_path;
        std::string missing_owner_task_id;
        const SampleAnnotationResult* imported_annotation =
            context_after == nullptr
            ? nullptr
            : FindSampleWorkflowAnnotationByPath(
                  *context_after,
                  path);
        const std::vector<SampleLabelingTask>* tasks =
            labeling_.View().active_source_tasks;
        if (imported_annotation != nullptr &&
            imported_annotation->labeling_document &&
            tasks != nullptr) {
            const std::string& imported_task_id =
                imported_annotation->labeling_document
                    ->labeling.id;
            const auto missing_owner = std::find_if(
                tasks->begin(),
                tasks->end(),
                [&imported_task_id, &path](
                    const SampleLabelingTask& task) {
                    return task.task_id == imported_task_id &&
                        task.persistence.output_path &&
                        task.persistence.output_format ==
                            SampleLabelingOutputArtifactFormat::
                                CanonicalAsdf &&
                        !SampleWorkflowPathsReferToSameFile(
                            *task.persistence.output_path,
                            path) &&
                        ProbePath(*task.persistence.output_path) ==
                            PathProbeResult::Missing;
                });
            if (missing_owner != tasks->end()) {
                missing_owner_path =
                    *missing_owner->persistence.output_path;
                missing_owner_task_id =
                    missing_owner->task_id;
            }
        }

        if (missing_owner_path) {
            const SampleLabelingOperationResult relink =
                labeling_.RelinkCanonicalAsdfTask(
                    missing_owner_task_id,
                    path);
            ApplyLabelingLeaseIssue(outcome, relink);
            if (relink.accepted) {
                std::optional<std::string> removed_source_id;
                if (const SourceCollectionManifest* active_context =
                        navigation_.active_context()) {
                    if (const SampleAnnotationResult* previous =
                            FindSampleWorkflowAnnotationByPath(
                                *active_context,
                                *missing_owner_path)) {
                        removed_source_id =
                            BuildAnnotationFilterSourceId(
                                *previous);
                    }
                }
                const bool removed_old_attachment =
                    navigation_.RemoveReadOnlyAnnotationFromActiveSource(
                        *missing_owner_path);
                const bool removed_old_diagnostic =
                    navigation_.
                        RemoveAnnotationAttachmentDiagnosticsFromActiveSource(
                            *missing_owner_path);
                if (removed_source_id) {
                    (void)workflow_sources_.RemoveFilterSource(
                        *removed_source_id);
                    (void)workflow_sources_.RemoveSampleSortSource(
                        *removed_source_id);
                }
                if (removed_old_attachment) {
                    MarkActiveWorkflowStateDirty();
                    outcome.action.annotation_roster_changed = true;
                }
                outcome.changed =
                    outcome.changed ||
                    relink.changed ||
                    relink.task_projection_changed ||
                    removed_old_attachment;
                outcome.invalidate_view =
                    outcome.invalidate_view ||
                    relink.changed ||
                    relink.task_projection_changed ||
                    removed_old_attachment ||
                    removed_old_diagnostic;
            }
        }
        outcome.invalidate_view =
            navigation_.
                RemoveAnnotationAttachmentDiagnosticsFromActiveSource(
                    path) ||
            outcome.invalidate_view;
        ApplyNavigationInputEffects(
            outcome,
            ReconcileNavigationInputs(
                nullptr,
                NavigationInputReconcileRequest{.filters_changed = true, .sorting_changed = true}));
    }
    return outcome;
}

SampleWorkflowTransitionOutcome
SampleWorkflowCoordinator::RemoveReadOnlyAnnotationFromActiveSource(
    const std::filesystem::path& path)
{
    SampleWorkflowTransitionOutcome outcome;
    const std::vector<SampleLabelingTask>* tasks =
        labeling_.View().active_source_tasks;
    const SampleLabelingTask* missing_local_task =
        tasks == nullptr
        ? nullptr
        : [&]() -> const SampleLabelingTask* {
              const auto match = std::find_if(
                  tasks->begin(),
                  tasks->end(),
                  [&path](const SampleLabelingTask& task) {
                      return task.persistence.output_path &&
                          SampleWorkflowPathsReferToSameFile(
                              *task.persistence.output_path,
                              path) &&
                          ProbePath(*task.persistence.output_path) ==
                              PathProbeResult::Missing;
                  });
              return match == tasks->end()
                  ? nullptr
                  : &*match;
          }();
    const std::string removed_labeling_source_id =
        missing_local_task == nullptr
        ? std::string{}
        : BuildLabelingFilterSourceId(*missing_local_task);
    bool deleted_missing_local_task = false;
    if (missing_local_task != nullptr) {
        const std::optional<SourceCollectionIdentity> identity =
            navigation_.active_source_identity();
        if (!identity) {
            return outcome;
        }
        const SampleLabelingOperationResult deleted =
            labeling_.DeleteTask(
                identity->id,
                missing_local_task->task_id);
        ApplyLabelingLeaseIssue(outcome, deleted);
        if (!deleted.changed &&
            !deleted.task_projection_changed) {
            return outcome;
        }
        outcome.changed = true;
        outcome.invalidate_view = true;
        outcome.action.workflow_changed = true;
        deleted_missing_local_task = true;
        ClearLabelUndoHistory();
    }
    std::optional<std::string> removed_source_id;
    if (const SourceCollectionManifest* context = navigation_.active_context()) {
        if (const SampleAnnotationResult* annotation = FindSampleWorkflowAnnotationByPath(*context, path)) {
            removed_source_id = BuildAnnotationFilterSourceId(*annotation);
        }
    }
    const bool removed_annotation = navigation_.RemoveReadOnlyAnnotationFromActiveSource(path);
    const bool removed_diagnostic =
        navigation_.
            RemoveAnnotationAttachmentDiagnosticsFromActiveSource(
                path);
    if (!deleted_missing_local_task && !removed_annotation) {
        outcome.invalidate_view =
            outcome.invalidate_view ||
            removed_diagnostic;
        return outcome;
    }
    outcome.invalidate_view =
        outcome.invalidate_view ||
        removed_diagnostic;

    if (!removed_labeling_source_id.empty()) {
        (void)workflow_sources_.RemoveFilterSource(
            removed_labeling_source_id);
    }
    if (removed_source_id) {
        (void)workflow_sources_.RemoveFilterSource(*removed_source_id);
        (void)workflow_sources_.RemoveSampleSortSource(*removed_source_id);
    }
    MarkActiveWorkflowStateDirty();
    ApplyNavigationInputEffects(
        outcome,
        ReconcileNavigationInputs(
            nullptr,
            NavigationInputReconcileRequest{.filters_changed = true, .sorting_changed = true}));
    return outcome;
}

SampleWorkflowTransitionOutcome
SampleWorkflowCoordinator::RenameAnnotationDisplayNameForActiveSource(
    std::filesystem::path path,
    std::string display_name)
{
    SampleWorkflowTransitionOutcome outcome;
    if (!workflow_sources_.RenameAnnotationDisplayName(
            SourcePolicyContext(nullptr),
            path,
            std::move(display_name))) {
        return outcome;
    }
    MarkActiveWorkflowStateDirty();
    outcome.action.workflow_changed = true;
    return outcome;
}

bool SampleWorkflowCoordinator::RestoreReadOnlyAnnotationsForActiveSource(
    const std::vector<std::filesystem::path>& paths)
{
    const bool restored = navigation_.RestoreReadOnlyAnnotationsForActiveSource(paths);
    if (restored) {
        (void)ReconcileNavigationInputs(
            nullptr,
            NavigationInputReconcileRequest{.filters_changed = true, .sorting_changed = true});
    }
    return restored;
}

std::unordered_map<std::string, std::vector<std::filesystem::path>>
SampleWorkflowCoordinator::AnnotationPathsBySourceKey() const
{
    return navigation_.AnnotationPathsBySourceKey();
}

std::vector<std::filesystem::path> SampleWorkflowCoordinator::AnnotationPathsForSourceKey(
    std::string_view source_key) const
{
    return navigation_.AnnotationPathsForSourceKey(source_key);
}

SampleWorkflowTransitionOutcome
SampleWorkflowCoordinator::SetSampleNameQuery(std::string query)
{
    SampleWorkflowTransitionOutcome outcome;
    outcome.action.navigation_inputs_changed =
        navigation_.SetSampleNameQuery(
            std::move(query));
    return outcome;
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::CommitSampleNameSelection(
    std::size_t target_row,
    std::string matched_name,
    const SpectrumSnapshotHandle& snapshot)
{
    const bool query_changed =
        navigation_.SetSampleNameQuery(
            std::move(matched_name));
    std::string query{navigation_.sample_name_query()};
    SampleWorkflowTransitionOutcome result =
        RequestSampleNavigation(
            SampleNavigationRequest::LocateSampleNameMatch(
                target_row,
                std::move(query)),
            snapshot);
    result.action.navigation_inputs_changed =
        result.action.navigation_inputs_changed ||
        query_changed;
    return result;
}

SampleWorkflowTransitionOutcome
SampleWorkflowCoordinator::StartOrResumeTemporaryLabelingTask()
{
    SampleWorkflowTransitionOutcome outcome;
    const SampleLabelingOperationResult result =
        labeling_.StartOrResumeTemporaryTask();
    ApplyLabelingLeaseIssue(outcome, result);
    if (result.changed || result.task_projection_changed) {
        ClearLabelUndoHistory();
        ApplyNavigationInputEffects(
            outcome,
            ReconcileNavigationInputs(
                nullptr,
                NavigationInputReconcileRequest{
                    .workflow_changed = true,
                    .filters_changed = true,
                    .sorting_changed = true}));
    }
    return outcome;
}

SampleWorkflowTransitionOutcome
SampleWorkflowCoordinator::RecoverTemporaryLabelingTask(
    std::string source_identity,
    std::string task_id)
{
    SampleWorkflowTransitionOutcome outcome;
    const ActiveSampleWorkflowIdentity active_identity_before =
        CaptureActiveSampleWorkflowIdentity(
            active_sample_workflow_identity_,
            labeling_.View().active_task);
    const SampleLabelingOperationResult result =
        labeling_.RecoverTemporaryTask(
            source_identity,
            task_id);
    ApplyLabelingLeaseIssue(outcome, result);
    if (result.accepted || result.task_projection_changed) {
        outcome.changed =
            result.changed ||
            result.task_projection_changed;
        const ActiveSampleWorkflowIdentity active_identity_after =
            CaptureActiveSampleWorkflowIdentity(
                active_sample_workflow_identity_,
                labeling_.View().active_task);
        if (active_identity_before != active_identity_after) {
            ClearLabelUndoHistory();
            ApplyNavigationInputEffects(
                outcome,
                ReconcileNavigationInputs(
                    nullptr,
                    NavigationInputReconcileRequest{
                        .workflow_changed = true,
                        .filters_changed = true,
                        .sorting_changed = true}));
        } else if (result.task_projection_changed) {
            ApplyNavigationInputEffects(
                outcome,
                ReconcileNavigationInputs(
                    nullptr,
                    NavigationInputReconcileRequest{
                        .filters_changed = true,
                        .sorting_changed = true}));
        }
    }
    return outcome;
}

SampleWorkflowTransitionOutcome
SampleWorkflowCoordinator::DeleteTemporaryLabelingTask(
    std::string source_identity,
    std::string task_id)
{
    SampleWorkflowTransitionOutcome outcome;
    const ActiveSampleWorkflowIdentity active_identity_before =
        CaptureActiveSampleWorkflowIdentity(
            active_sample_workflow_identity_,
            labeling_.View().active_task);
    const SampleLabelingOperationResult result =
        labeling_.DeleteTemporaryTask(
            source_identity,
            task_id);
    ApplyLabelingLeaseIssue(outcome, result);
    if (result.accepted || result.task_projection_changed) {
        outcome.changed =
            result.changed ||
            result.task_projection_changed;
        const ActiveSampleWorkflowIdentity active_identity_after =
            CaptureActiveSampleWorkflowIdentity(
                active_sample_workflow_identity_,
                labeling_.View().active_task);
        if (active_identity_before != active_identity_after) {
            ClearLabelUndoHistory();
            ApplyNavigationInputEffects(
                outcome,
                ReconcileNavigationInputs(
                    nullptr,
                    NavigationInputReconcileRequest{
                        .workflow_changed = true,
                        .filters_changed = true,
                        .sorting_changed = true}));
        } else if (result.task_projection_changed) {
            ApplyNavigationInputEffects(
                outcome,
                ReconcileNavigationInputs(
                    nullptr,
                    NavigationInputReconcileRequest{
                        .filters_changed = true,
                        .sorting_changed = true}));
        }
    }
    return outcome;
}

SampleWorkflowTransitionOutcome
SampleWorkflowCoordinator::ActivateLabelingTaskFromAnnotation(
    std::filesystem::path annotation_path)
{
    SampleWorkflowTransitionOutcome outcome;
    const SourceCollectionManifest* context = navigation_.active_context();
    if (context == nullptr || annotation_path.empty()) {
        return outcome;
    }

    const std::vector<SampleLabelingTask>* active_source_tasks =
        labeling_.View().active_source_tasks;
    const SampleAnnotationResult* annotation = FindSampleWorkflowAnnotationByPath(*context, annotation_path);
    std::optional<SampleAnnotationResult> loaded_annotation;
    if (annotation == nullptr) {
        const std::optional<SampleLabelingSourceCompatibility>
            source_compatibility =
                navigation_.active_source_compatibility();
        if (!source_compatibility) {
            return outcome;
        }
        std::string load_error;
        loaded_annotation =
            SampleAnnotationIoAdapter{}.LoadForSource(
                annotation_path,
                *source_compatibility,
                &load_error);
        if (!loaded_annotation) {
            return outcome;
        }
        annotation = &*loaded_annotation;
    }

    const SampleLabelingTask* active_task = labeling_.View().active_task;
    const std::optional<SampleLabelResultMetadata> metadata = LoadVerifiedLabelMetadataForAnnotation(*annotation);
    SampleAnnotationLabelingActivationPlan plan = PlanSampleAnnotationLabelingActivation(
        SampleAnnotationLabelingActivationRequest{
            .annotation = annotation,
            .active_source_tasks = active_source_tasks,
            .metadata = metadata ? &*metadata : nullptr});
    if (plan.kind == SampleAnnotationLabelingActivationKind::None) {
        return outcome;
    }
    const std::filesystem::path target_annotation_path =
        annotation->path;
    if (plan.kind ==
            SampleAnnotationLabelingActivationKind::
                ActivateExistingTask &&
        active_task != nullptr &&
        active_task->task_id == plan.task_id) {
        return outcome;
    }
    std::optional<std::filesystem::path> previous_owner_path;
    if (plan.kind ==
            SampleAnnotationLabelingActivationKind::
                AdoptCanonicalAsdfTask &&
        active_source_tasks != nullptr) {
        const auto existing_owner = std::find_if(
            active_source_tasks->begin(),
            active_source_tasks->end(),
            [&plan](const SampleLabelingTask& task) {
                return task.task_id == plan.task_id;
            });
        if (existing_owner != active_source_tasks->end() &&
            existing_owner->persistence.output_path &&
            !SampleWorkflowPathsReferToSameFile(
                *existing_owner->persistence.output_path,
                target_annotation_path)) {
            previous_owner_path = *existing_owner->persistence.output_path;
        }
    }
    if (plan.kind ==
            SampleAnnotationLabelingActivationKind::
                ActivateExistingTask ||
        plan.kind ==
            SampleAnnotationLabelingActivationKind::
                AdoptCanonicalAsdfTask) {
        const SampleLabelingOperationResult activation =
            plan.kind ==
                    SampleAnnotationLabelingActivationKind::
                        AdoptCanonicalAsdfTask
            ? labeling_.AdoptCanonicalAsdfTask(
                  plan.task_id,
                  target_annotation_path)
            : labeling_.ActivateTask(plan.task_id);
        ApplyLabelingLeaseIssue(outcome, activation);
        bool previous_owner_attachment_removed = false;
        bool attachment_diagnostic_removed = false;
        if (activation.accepted && previous_owner_path) {
            std::optional<std::string> removed_source_id;
            if (const SourceCollectionManifest* active_context =
                    navigation_.active_context()) {
                if (const SampleAnnotationResult* previous_annotation =
                        FindSampleWorkflowAnnotationByPath(
                            *active_context,
                            *previous_owner_path)) {
                    removed_source_id =
                        BuildAnnotationFilterSourceId(
                            *previous_annotation);
                }
            }
            previous_owner_attachment_removed =
                navigation_.RemoveReadOnlyAnnotationFromActiveSource(
                    *previous_owner_path);
            attachment_diagnostic_removed =
                navigation_.
                    RemoveAnnotationAttachmentDiagnosticsFromActiveSource(
                        *previous_owner_path) ||
                attachment_diagnostic_removed;
            if (previous_owner_attachment_removed) {
                if (removed_source_id) {
                    (void)workflow_sources_.RemoveFilterSource(
                        *removed_source_id);
                    (void)workflow_sources_.RemoveSampleSortSource(
                        *removed_source_id);
                }
                MarkActiveWorkflowStateDirty();
                outcome.action.annotation_roster_changed = true;
            }
        }
        const bool attachment_refreshed =
            activation.accepted &&
            SynchronizeActiveCanonicalAsdfAttachment(
                &outcome.action);
        if (activation.accepted) {
            attachment_diagnostic_removed =
                navigation_.
                    RemoveAnnotationAttachmentDiagnosticsFromActiveSource(
                        target_annotation_path) ||
                attachment_diagnostic_removed;
        }
        if (activation.accepted ||
            activation.task_projection_changed) {
            outcome.changed =
                outcome.changed ||
                activation.task_projection_changed ||
                attachment_refreshed ||
                previous_owner_attachment_removed ||
                attachment_diagnostic_removed;
            outcome.invalidate_view =
                outcome.invalidate_view ||
                activation.task_projection_changed ||
                attachment_refreshed ||
                previous_owner_attachment_removed ||
                attachment_diagnostic_removed;
            ClearLabelUndoHistory();
            ApplyNavigationInputEffects(
                outcome,
                ReconcileNavigationInputs(
                    nullptr,
                    NavigationInputReconcileRequest{
                        .workflow_changed = true,
                        .filters_changed = true,
                        .sorting_changed = true}));
        }
        return outcome;
    }

    const SampleLabelingOperationResult create_result = labeling_.CreateTaskFromAnnotation(
        std::move(plan.task_name),
        std::move(plan.label_set),
        std::move(plan.values),
        target_annotation_path,
        std::move(plan.origin));
    ApplyLabelingLeaseIssue(outcome, create_result);
    if (!create_result.accepted) {
        if (create_result.task_projection_changed) {
            outcome.changed = true;
            outcome.invalidate_view = true;
            ApplyNavigationInputEffects(
                outcome,
                ReconcileNavigationInputs(
                    nullptr,
                    NavigationInputReconcileRequest{
                        .workflow_changed = true,
                        .filters_changed = true,
                        .sorting_changed = true}));
        }
        return outcome;
    }
    ClearLabelUndoHistory();
    ApplyNavigationInputEffects(
        outcome,
        ReconcileNavigationInputs(
            nullptr,
            NavigationInputReconcileRequest{
                .workflow_changed = true,
                .filters_changed = true,
                .sorting_changed = true}));
    return outcome;
}

SampleWorkflowTransitionOutcome
SampleWorkflowCoordinator::DeleteActiveLabelingTask()
{
    SampleWorkflowTransitionOutcome outcome;
    const SampleLabelingTask* task = labeling_.View().active_task;
    const std::string deleted_task_source_id = task == nullptr ? std::string{} : BuildLabelingFilterSourceId(*task);
    if (!labeling_.DeleteActiveTask().changed) {
        return outcome;
    }
    ClearLabelUndoHistory();

    if (!deleted_task_source_id.empty()) {
        (void)workflow_sources_.RemoveFilterSource(deleted_task_source_id);
    }
    MarkActiveWorkflowStateDirty();
    ApplyNavigationInputEffects(
        outcome,
        ReconcileNavigationInputs(
            nullptr,
            NavigationInputReconcileRequest{
                .workflow_changed = true,
                .filters_changed = true,
                .sorting_changed = true}));
    return outcome;
}

SampleWorkflowTransitionOutcome
SampleWorkflowCoordinator::RenameActiveLabelingTask(
    std::string expected_task_id,
    std::string requested_name)
{
    SampleWorkflowTransitionOutcome outcome;
    const SampleLabelingOperationResult operation =
        labeling_.RenameActiveTask(
            expected_task_id,
            std::move(requested_name));
    outcome.changed = operation.changed;
    ApplyLabelingLeaseIssue(outcome, operation);
    if (operation.output_saved) {
        (void)SynchronizeActiveCanonicalAsdfAttachment(
            &outcome.action);
    }
    return outcome;
}

SampleWorkflowTransitionOutcome
SampleWorkflowCoordinator::UpsertActiveLabel(
    SampleLabelDefinition label)
{
    SampleWorkflowTransitionOutcome outcome;
    const SampleLabelingOperationResult operation =
        labeling_.UpsertActiveLabel(std::move(label));
    outcome.changed = operation.changed;
    ApplyLabelingLeaseIssue(outcome, operation);
    if (operation.output_saved) {
        (void)SynchronizeActiveCanonicalAsdfAttachment(
            &outcome.action);
    }
    if (outcome.changed) {
        ClearLabelUndoHistory();
        ApplyNavigationInputEffects(
            outcome,
            ReconcileNavigationInputs(
                nullptr,
                NavigationInputReconcileRequest{.filters_changed = true, .sorting_changed = true}));
    }
    return outcome;
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::UpdateActiveLabel(
    int original_code,
    SampleLabelDefinition label,
    bool allow_used_code_change)
{
    SampleWorkflowTransitionOutcome outcome;
    const int updated_code = label.code;
    const SampleLabelingTask* active_task = labeling_.View().active_task;
    const std::string sample_filter_source_id =
        active_task == nullptr ? std::string{} : BuildLabelingFilterSourceId(*active_task);
    const SampleLabelingOperationResult operation =
        labeling_.UpdateActiveLabel(original_code, std::move(label), allow_used_code_change);
    outcome.changed = operation.changed;
    ApplyLabelingLeaseIssue(outcome, operation);
    if (operation.output_saved) {
        (void)SynchronizeActiveCanonicalAsdfAttachment(
            &outcome.action);
    }
    if (outcome.changed) {
        ClearLabelUndoHistory();
        if (!sample_filter_source_id.empty() &&
            workflow_sources_.ReplaceSampleFilterValue(
                sample_filter_source_id,
                std::to_string(original_code),
                std::to_string(updated_code))) {
            MarkActiveWorkflowStateDirty();
        }
        ApplyNavigationInputEffects(
            outcome,
            ReconcileNavigationInputs(
                nullptr,
                NavigationInputReconcileRequest{.filters_changed = true, .sorting_changed = true}));
    }
    return outcome;
}

SampleWorkflowTransitionOutcome
SampleWorkflowCoordinator::RemoveActiveLabel(int code)
{
    SampleWorkflowTransitionOutcome outcome;
    const SampleLabelingTask* active_task = labeling_.View().active_task;
    const std::string sample_filter_source_id =
        active_task == nullptr ? std::string{} : BuildLabelingFilterSourceId(*active_task);
    const SampleLabelingOperationResult operation = labeling_.RemoveActiveLabel(code);
    outcome.changed = operation.changed;
    ApplyLabelingLeaseIssue(outcome, operation);
    if (operation.output_saved) {
        (void)SynchronizeActiveCanonicalAsdfAttachment(
            &outcome.action);
    }
    if (outcome.changed) {
        ClearLabelUndoHistory();
        if (!sample_filter_source_id.empty() &&
            workflow_sources_.RemoveSampleFilterValue(
                sample_filter_source_id,
                std::to_string(code))) {
            MarkActiveWorkflowStateDirty();
        }
        ApplyNavigationInputEffects(
            outcome,
            ReconcileNavigationInputs(
                nullptr,
                NavigationInputReconcileRequest{.filters_changed = true, .sorting_changed = true}));
    }
    return outcome;
}

SampleWorkflowTransitionOutcome
SampleWorkflowCoordinator::SetActiveLabelingAutoAdvance(bool enabled)
{
    (void)labeling_.SetActiveAutoAdvance(enabled);
    return {};
}

SampleWorkflowTransitionOutcome
SampleWorkflowCoordinator::SetActiveLabelingSkipLabeledOnAdvance(
    bool enabled)
{
    (void)labeling_.SetActiveSkipLabeledOnAdvance(enabled);
    return {};
}

SampleWorkflowTransitionOutcome
SampleWorkflowCoordinator::SetActiveLabelingOutputPath(
    std::filesystem::path output_path)
{
    SampleWorkflowTransitionOutcome outcome;
    const std::filesystem::path selected_output_path = output_path;
    const SampleLabelingTask* active_task =
        labeling_.View().active_task;
    const SampleLabelingOperationResult operation =
        active_task != nullptr &&
            active_task->persistence.output_format ==
                SampleLabelingOutputArtifactFormat::
                    LegacyNpyWithSidecar
        ? labeling_.MigrateActiveLegacyTaskToCanonicalAsdf(
              std::move(output_path))
        : labeling_.SaveActiveTemporaryTaskToOutput(
              std::move(output_path));
    ApplyLabelingLeaseIssue(outcome, operation);
    if (operation.output_saved) {
        (void)SynchronizeActiveCanonicalAsdfAttachment(
            &outcome.action);
        if (const SourceCollectionManifest* context = navigation_.active_context()) {
            if (const SampleAnnotationResult* annotation =
                    FindSampleWorkflowAnnotationByPath(*context, selected_output_path)) {
                const std::string source_id = BuildAnnotationFilterSourceId(*annotation);
                (void)workflow_sources_.RemoveFilterSource(source_id);
                (void)workflow_sources_.RemoveSampleSortSource(source_id);
            }
        }
        MarkActiveWorkflowStateDirty();
        ApplyNavigationInputEffects(
            outcome,
            ReconcileNavigationInputs(
                nullptr,
                NavigationInputReconcileRequest{.filters_changed = true, .sorting_changed = true}));
    }
    return outcome;
}

SampleWorkflowTransitionOutcome
SampleWorkflowCoordinator::ExportActiveLabels(
    std::filesystem::path output_path,
    SampleLabelExportFormat format)
{
    SampleWorkflowTransitionOutcome outcome;
    const SampleLabelingOperationResult operation =
        labeling_.ExportActiveLabels(
            output_path,
            format);
    ApplyLabelingLeaseIssue(outcome, operation);
    return outcome;
}

SampleWorkflowTransitionOutcome
SampleWorkflowCoordinator::DeactivateActiveLabelingTask()
{
    SampleWorkflowTransitionOutcome outcome;
    if (!labeling_.DeactivateActiveTask().changed) {
        return outcome;
    }
    ClearLabelUndoHistory();

    MarkActiveWorkflowStateDirty();
    ApplyNavigationInputEffects(
        outcome,
        ReconcileNavigationInputs(
            nullptr,
            NavigationInputReconcileRequest{
                .workflow_changed = true,
                .filters_changed = true,
                .sorting_changed = true}));
    return outcome;
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::AssignActiveLabelToCurrentSample(
    const SpectrumSnapshotHandle& snapshot,
    int code,
    NavigationTargetResolutionReport* target_resolution)
{
    const std::optional<std::size_t> sample_index = ActiveSampleIndex(snapshot);
    if (!sample_index) {
        return {};
    }
    const SampleLabelingWriteOperationResult write =
        labeling_.AssignLabel(*sample_index, code);
    return ApplyLabelWriteResult(
        snapshot,
        write,
        target_resolution);
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::ClearActiveLabelForCurrentSample(
    const SpectrumSnapshotHandle& snapshot,
    NavigationTargetResolutionReport* target_resolution)
{
    const std::optional<std::size_t> sample_index = ActiveSampleIndex(snapshot);
    if (!sample_index) {
        return {};
    }
    const SampleLabelingWriteOperationResult write =
        labeling_.ClearLabel(*sample_index);
    return ApplyLabelWriteResult(
        snapshot,
        write,
        target_resolution);
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::UndoLastLabelWrite(
    const SpectrumSnapshotHandle& snapshot)
{
    if (!LabelUndoHistoryMatchesActiveTask() || label_undo_history_->entries.empty()) {
        return {};
    }

    const SampleLabelingTask* task = labeling_.View().active_task;
    const LabelUndoEntry entry = label_undo_history_->entries.back();
    if (task == nullptr || !task->values.IsComplete() || entry.sample_index >= task->values.SampleCount() ||
        task->values.Complete()[entry.sample_index] != entry.current_code) {
        ClearLabelUndoHistory();
        return {};
    }

    SampleLabelingWriteOperationResult write_operation =
        entry.previous_code == kUnlabeledSampleLabelCode
        ? labeling_.ClearLabel(entry.sample_index)
        : labeling_.AssignLabel(entry.sample_index, entry.previous_code);
    SampleLabelWriteResult& write_result = write_operation.write;
    if (!write_result.changed) {
        ClearLabelUndoHistory();
        return {};
    }

    write_result.advance_requested = false;
    label_undo_history_->entries.pop_back();
    return ApplyLabelWriteResult(
        snapshot,
        std::move(write_operation),
        nullptr,
        false,
        entry.sample_index);
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::ClearFilters(
    const SpectrumSnapshotHandle& snapshot)
{
    SampleWorkflowTransitionOutcome outcome;
    workflow_sources_.ClearFilters();
    MarkActiveWorkflowStateDirty();
    ApplyNavigationInputEffects(
        outcome,
        ReconcileNavigationInputs(
            snapshot,
            NavigationInputReconcileRequest{.filters_changed = true}));
    return outcome;
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::AddFilterSource(
    const SpectrumSnapshotHandle& snapshot,
    std::string source_id)
{
    SampleWorkflowTransitionOutcome outcome;
    if (!workflow_sources_.AddFilterSource(SourcePolicyContext(snapshot), std::move(source_id))) {
        return outcome;
    }

    MarkActiveWorkflowStateDirty();
    outcome.action.workflow_changed = true;
    return outcome;
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::RemoveFilterSource(
    const SpectrumSnapshotHandle& snapshot,
    std::string source_id)
{
    SampleWorkflowTransitionOutcome outcome;
    if (!workflow_sources_.RemoveFilterSource(source_id)) {
        return outcome;
    }

    MarkActiveWorkflowStateDirty();
    ApplyNavigationInputEffects(
        outcome,
        ReconcileNavigationInputs(
            snapshot,
            NavigationInputReconcileRequest{.filters_changed = true}));
    return outcome;
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::SetFilterValueSelected(
    const SpectrumSnapshotHandle& snapshot,
    std::string source_id,
    std::string value_key,
    bool selected)
{
    SampleWorkflowTransitionOutcome outcome;
    if (!workflow_sources_.SetFilterValueSelected(
            SourcePolicyContext(snapshot),
            std::move(source_id),
            std::move(value_key),
            selected)) {
        return outcome;
    }

    MarkActiveWorkflowStateDirty();
    ApplyNavigationInputEffects(
        outcome,
        ReconcileNavigationInputs(
            snapshot,
            NavigationInputReconcileRequest{.filters_changed = true}));
    return outcome;
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::ClearSampleSorting(
    const SpectrumSnapshotHandle& snapshot)
{
    SampleWorkflowTransitionOutcome outcome;
    workflow_sources_.ClearSampleSorting();
    MarkActiveWorkflowStateDirty();
    ApplyNavigationInputEffects(
        outcome,
        ReconcileNavigationInputs(
            snapshot,
            NavigationInputReconcileRequest{.sorting_changed = true}));
    return outcome;
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::AddSampleSortSource(
    const SpectrumSnapshotHandle& snapshot,
    std::string source_id)
{
    SampleWorkflowTransitionOutcome outcome;
    if (!workflow_sources_.AddSampleSortSource(SourcePolicyContext(snapshot), std::move(source_id))) {
        return outcome;
    }

    MarkActiveWorkflowStateDirty();
    outcome.action.workflow_changed = true;
    return outcome;
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::RemoveSampleSortSource(
    const SpectrumSnapshotHandle& snapshot,
    std::string source_id)
{
    SampleWorkflowTransitionOutcome outcome;
    if (!workflow_sources_.RemoveSampleSortSource(source_id)) {
        return outcome;
    }

    MarkActiveWorkflowStateDirty();
    ApplyNavigationInputEffects(
        outcome,
        ReconcileNavigationInputs(
            snapshot,
            NavigationInputReconcileRequest{.sorting_changed = true}));
    return outcome;
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::SetSampleSortSource(
    const SpectrumSnapshotHandle& snapshot,
    std::string source_id)
{
    SampleWorkflowTransitionOutcome outcome;
    if (source_id.empty()) {
        return ClearSampleSorting(snapshot);
    }
    if (!workflow_sources_.SetSampleSortSource(SourcePolicyContext(snapshot), std::move(source_id))) {
        return outcome;
    }

    MarkActiveWorkflowStateDirty();
    ApplyNavigationInputEffects(
        outcome,
        ReconcileNavigationInputs(
            snapshot,
            NavigationInputReconcileRequest{.sorting_changed = true}));
    return outcome;
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::SetSampleSortDirection(
    const SpectrumSnapshotHandle& snapshot,
    SampleNavigationSortDirection direction)
{
    SampleWorkflowTransitionOutcome outcome;
    workflow_sources_.SetSampleSortDirection(direction);
    MarkActiveWorkflowStateDirty();
    ApplyNavigationInputEffects(
        outcome,
        ReconcileNavigationInputs(
            snapshot,
            NavigationInputReconcileRequest{.sorting_changed = true}));
    return outcome;
}

SourceCollectionNavigationView SampleWorkflowCoordinator::NavigationView(const SpectrumSnapshotHandle& snapshot) const
{
    SourceCollectionNavigationView view;
    const SampleNavigationSequence& sequence = navigation_.current_sequence();
    view.current_index = navigation_.current_index();
    view.current_source_row = view.current_index;
    view.current_sequence_position = sequence.current_sequence_position;
    view.sample_count = navigation_.spectrum_count().value_or(snapshot ? snapshot->collection.spectrum_count : 0);
    view.has_active_source = snapshot && !snapshot->source.path.empty() && view.sample_count > 0;
    view.can_move_previous = navigation_.can_move_previous();
    view.can_move_next = navigation_.can_move_next();
    view.filter_active = navigation_.filter_active();
    view.current_sample_in_filter = navigation_.current_sample_in_filter();
    view.sequence_active = sequence.active;
    view.sequence_empty = sequence.empty;
    view.sequence_count = sequence.active ? sequence.ordered_rows.size() : view.sample_count;
    view.resolved_sequence_position.sequence_length = view.sequence_count;
    if (view.current_sequence_position &&
        *view.current_sequence_position < view.sequence_count) {
        view.resolved_sequence_position.zero_based_position =
            view.current_sequence_position;
    }
    view.sequence_topology_revision =
        navigation_.sequence_topology_revision();
    view.filtered_sample_count = view.sequence_count;
    view.row_location_available = sequence.row_location_available;
    if (snapshot && view.current_index && snapshot->collection.current_index == *view.current_index) {
        view.current_sample_display_name = snapshot->current_spectrum.name;
    }
    if (const SourceCollectionManifest* context = navigation_.active_context()) {
        view.has_sample_names = !context->sample_names.empty();
        view.annotation_diagnostics =
            context->diagnostics;
        if (view.current_index && *view.current_index < context->sample_names.size()) {
            view.current_sample_name = context->sample_names[*view.current_index];
        }

        if (view.current_index) {
            const std::size_t current_index = *view.current_index;
            view.current_annotations.reserve(context->annotations.size());
            for (const SampleAnnotationResult& annotation : context->annotations) {
                const SampleLabelingTask* local_task =
                    FindLocalTaskForLoadedAnnotation(
                        labeling_.View().active_source_tasks,
                        annotation);
                view.current_annotations.push_back(BuildAnnotationValueView(
                    annotation,
                    current_index,
                    local_task,
                    workflow_sources_.AnnotationDisplayName(annotation, local_task)));
            }

            if (const std::vector<SampleLabelingTask>* tasks =
                labeling_.View().active_source_tasks) {
                for (const SampleLabelingTask& task : *tasks) {
                    if (!task.persistence.output_path ||
                        (!task.values.IsComplete() &&
                         task.persistence.output_format !=
                             SampleLabelingOutputArtifactFormat::
                                 CanonicalAsdf)) {
                        continue;
                    }
                    const bool already_loaded = std::any_of(
                        context->annotations.begin(),
                        context->annotations.end(),
                        [&task](const SampleAnnotationResult& annotation) {
                            return task.persistence.output_path &&
                                   SampleWorkflowPathsReferToSameFile(annotation.path, *task.persistence.output_path);
                        });
                    if (!already_loaded) {
                        view.current_annotations.push_back(BuildLocalTaskAnnotationValueView(
                            task,
                            current_index,
                            workflow_sources_.LocalTaskAnnotationDisplayName(task)));
                    }
                }
            }
        }

        const std::string_view query = navigation_.sample_name_query();
        const std::vector<std::size_t>& matches = navigation_.sample_name_matches();
        if (!query.empty() && view.has_sample_names && !matches.empty()) {
            const std::string target = LowerAscii(std::string{query});
            view.sample_name_matches.reserve(matches.size());
            for (const std::size_t row : matches) {
                if (row >= context->sample_names.size()) {
                    continue;
                }
                const std::string& name = context->sample_names[row];
                if (!view.exact_sample_name_match && LowerAscii(name) == target) {
                    view.exact_sample_name_match = row;
                    view.exact_sample_name = name;
                }
                view.sample_name_matches.push_back(SourceCollectionSampleNameMatchView{row, name});
            }
            view.has_partial_sample_name_matches = !view.exact_sample_name_match && !view.sample_name_matches.empty();
        }
    }
    return view;
}

SourceCollectionLabelingView SampleWorkflowCoordinator::LabelingView(const SpectrumSnapshotHandle& snapshot) const
{
    SourceCollectionLabelingView view;
    const SampleLabelingControllerView labeling_view = labeling_.View();
    const SampleLabelingRecoveryView recovery_view = labeling_.RecoveryView();
    view.has_active_source = snapshot && !snapshot->source.path.empty() && ActiveSampleCount(snapshot) > 0;
    view.source_identity = recovery_view.source_identity;
    if (const std::optional<SampleLabelingSourceCompatibility>
            source = navigation_.active_source_compatibility()) {
        view.source_kind = source->source_kind;
    }
    view.recovery_revision = recovery_view.revision;
    view.recovery_drafts.reserve(recovery_view.temporary_drafts.size());
    for (const SampleLabelingRecoveryDraftView& draft :
         recovery_view.temporary_drafts) {
        if (draft.task == nullptr) {
            continue;
        }
        view.recovery_drafts.push_back(
            SourceCollectionLabelingRecoveryDraftView{
                .task_id = draft.task->task_id,
                .task_name = draft.task->task_name,
                .status = draft.status,
                .labeled_count = draft.task->statistics.labeled_count,
                .sample_count = draft.task->values.SampleCount(),
                .save_state = draft.task->persistence.save_state});
    }
    view.current_index = ActiveSampleIndex(snapshot);
    view.has_temporary_task = labeling_view.temporary_task != nullptr;
    if (labeling_view.active_source_tasks != nullptr) {
        view.task_ids.reserve(labeling_view.active_source_tasks->size());
        for (const SampleLabelingTask& task :
             *labeling_view.active_source_tasks) {
            view.task_ids.push_back(task.task_id);
        }
    }
    if (const SampleLabelingTask* task = labeling_view.active_task) {
        view.has_active_task = true;
        view.active_task_is_temporary = !task->persistence.output_path;
        view.task_id = task->task_id;
        view.task_name = task->task_name;
        view.label_set = task->label_set;
        view.label_usage_counts = task->statistics.label_usage_counts;
        view.labeled_count = task->statistics.labeled_count;
        view.sample_count = task->values.SampleCount();
        if (task->values.IsComplete() && view.current_index && *view.current_index < task->values.SampleCount()) {
            view.current_code = task->values.Complete()[*view.current_index];
        }
        view.auto_advance = task->session.auto_advance;
        view.skip_labeled_on_advance = task->session.skip_labeled_on_advance;
        view.remembered_position = task->session.remembered_position;
        if (view.remembered_position && *view.remembered_position < view.sample_count) {
            const SampleNavigationSequence& sequence = navigation_.current_sequence();
            view.remembered_position_resumable =
                !sequence.active || (!sequence.empty && sequence.ContainsSourceRow(*view.remembered_position));
        }
        view.output_path = task->persistence.output_path;
        view.output_format = task->persistence.output_format;
        view.save_state = task->persistence.save_state;
        view.can_export_label_values =
            task->values.IsComplete();
        view.can_deactivate_task = labeling_.CanDeactivateActiveTask();
        view.can_delete_task = labeling_.CanDeleteActiveTask();
    }
    view.state_save_failed = labeling_.state_save_failed();
    view.state_save_error = std::string{labeling_.state_save_error()};
    view.state_load_warning = std::string{labeling_.state_load_warning()};
    return view;
}

SourceCollectionFilterView SampleWorkflowCoordinator::BuildFilterView(
    const SpectrumSnapshotHandle& snapshot)
{
    SourceCollectionFilterView view;
    if (prepared_filter_view_) {
        view = std::move(*prepared_filter_view_);
        prepared_filter_view_.reset();
    } else {
        view = workflow_sources_.BuildFilterView(SourcePolicyContext(snapshot));
    }
    view.has_active_source = snapshot && !snapshot->source.path.empty() && view.sample_count > 0;
    view.navigation_filter_active = navigation_.filter_active();
    view.current_sample_in_filter = navigation_.current_sample_in_filter();
    return view;
}

SourceCollectionSampleSortingView SampleWorkflowCoordinator::BuildSortingView(
    const SpectrumSnapshotHandle& snapshot)
{
    SourceCollectionSampleSortingView view;
    if (prepared_sorting_view_) {
        view = std::move(*prepared_sorting_view_);
        prepared_sorting_view_.reset();
    } else {
        view = workflow_sources_.BuildSortingView(SourcePolicyContext(snapshot));
    }
    view.has_active_source = snapshot && !snapshot->source.path.empty() && ActiveSampleCount(snapshot) > 0;
    return view;
}

ExactSampleNameResolution
SampleWorkflowCoordinator::ResolveExactSampleName(
    std::string_view name) const
{
    return navigation_.ResolveExactSampleName(name);
}

bool SampleWorkflowCoordinator::can_add_read_only_annotation() const
{
    return navigation_.active_context() != nullptr && navigation_.spectrum_count().value_or(0) > 0;
}

std::optional<std::size_t> SampleWorkflowCoordinator::current_index() const
{
    return navigation_.current_index();
}

std::vector<std::size_t> SampleWorkflowCoordinator::AdjacentNavigationRows(
    SampleNavigationDirection direction,
    SampleNavigationPrefetchPolicy policy) const
{
    return navigation_.AdjacentRows(direction, policy);
}

SampleWorkflowTransitionOutcome
SampleWorkflowCoordinator::RunMaintenance(
    LocalUserStateSaveScheduler::TimePoint now,
    const SpectrumSnapshotHandle& snapshot)
{
    const std::uint64_t labeling_revision_before =
        labeling_.View().revision;
    const std::uint64_t labeling_generation_before =
        labeling_.active_source_tasks_generation();
    SampleWorkflowTransitionOutcome outcome;
    navigation_.RunMaintenance(now);
    const SampleLabelingMaintenanceResult
        labeling_maintenance =
            labeling_.RunMaintenance(now);
    const bool canonical_attachment_refreshed =
        labeling_maintenance.canonical_output_published &&
        SynchronizeActiveCanonicalAsdfAttachment(
            &outcome.action);
    if (labeling_.active_source_tasks_generation() !=
            labeling_generation_before ||
        labeling_maintenance.canonical_output_published) {
        DiscardPreparedViewCaches();
        outcome.action.navigation_inputs_changed =
            canonical_attachment_refreshed ||
            outcome.action.navigation_inputs_changed;
        ApplyNavigationInputEffects(
            outcome,
            ReconcileNavigationInputs(
                snapshot,
                NavigationInputReconcileRequest{
                    .filters_changed = true,
                    .sorting_changed = true}));
    }
    (void)workflow_state_persistence_.RunMaintenance(
        now,
        [this] {
            return SaveWorkflowStateCache();
        });
    return CompleteTransition(
        std::move(outcome),
        snapshot,
        labeling_revision_before);
}

std::optional<LocalUserStateSaveScheduler::TimePoint> SampleWorkflowCoordinator::NextMaintenanceDeadline() const
{
    std::optional<LocalUserStateSaveScheduler::TimePoint> deadline =
        navigation_.NextMaintenanceDeadline();
    const std::optional<LocalUserStateSaveScheduler::TimePoint> labeling_deadline =
        labeling_.NextMaintenanceDeadline();
    if (labeling_deadline && (!deadline || *labeling_deadline < *deadline)) {
        deadline = labeling_deadline;
    }
    const std::optional<LocalUserStateSaveScheduler::TimePoint> workflow_deadline =
        workflow_state_persistence_.NextMaintenanceDeadline();
    if (workflow_deadline && (!deadline || *workflow_deadline < *deadline)) {
        deadline = workflow_deadline;
    }
    return deadline;
}

bool SampleWorkflowCoordinator::FlushStateCaches()
{
    return FlushStateCachesWithStatus().all_saved();
}

bool SampleWorkflowCoordinator::PrepareLabelingForInteractiveClose()
{
    return labeling_.PrepareForInteractiveClose();
}

SampleWorkflowStateFlushResult
SampleWorkflowCoordinator::FlushStateCachesWithStatus()
{
    SampleWorkflowStateFlushResult result;
    result.navigation_saved = navigation_.FlushStateCache();
    result.labeling_saved = labeling_.FlushStateCache();
    result.workflow_saved = FlushWorkflowStateCache();
    return result;
}

SampleWorkflowPersistenceStatus
SampleWorkflowCoordinator::PersistenceStatus() const
{
    return {
        .navigation = navigation_.PersistenceStatus(),
        .labeling = labeling_.PersistenceStatus(),
        .workflow = workflow_state_persistence_.PersistenceStatus(),
    };
}

void SampleWorkflowCoordinator::SyncSampleWorkflowSession(
    const SourceCollectionIdentity& identity,
    SampleLabelingCanonicalSourceDescriptor
        source_descriptor,
    SourceCollectionSessionAction& action)
{
    if (identity.id.empty() || identity.spectrum_count == 0) {
        ClearSampleWorkflow(action);
        return;
    }
    if (!active_sample_workflow_identity_ || *active_sample_workflow_identity_ != identity.id) {
        StoreActiveWorkflowState();
        active_sample_workflow_identity_ = identity.id;
        active_sample_workflow_context_fingerprint_ = identity.context_fingerprint;
        labeling_.ActivateSource(
            identity,
            std::move(source_descriptor));
        (void)SynchronizeActiveCanonicalAsdfAttachment(
            &action);
        RestoreActiveWorkflowState(identity.id);
        action.workflow_changed = true;
    } else if (
        !active_sample_workflow_context_fingerprint_ ||
        *active_sample_workflow_context_fingerprint_ != identity.context_fingerprint) {
        active_sample_workflow_context_fingerprint_ = identity.context_fingerprint;
        labeling_.ActivateSource(
            identity,
            std::move(source_descriptor));
        (void)SynchronizeActiveCanonicalAsdfAttachment(
            &action);
        action.workflow_changed = true;
    }
}

bool SampleWorkflowCoordinator::
    SynchronizeActiveCanonicalAsdfAttachment(
        SourceCollectionSessionAction* action)
{
    std::optional<SampleAnnotationResult> projection =
        labeling_.ActiveCanonicalAsdfAnnotationProjection();
    bool attachment_added = false;
    const bool synchronized = projection &&
        navigation_.UpsertAttachedAnnotationForActiveSource(
            std::move(*projection),
            &attachment_added);
    if (synchronized && action != nullptr) {
        action->navigation_inputs_changed = true;
        action->annotation_roster_changed =
            action->annotation_roster_changed ||
            attachment_added;
    }
    return synchronized;
}

bool SampleWorkflowCoordinator::
    SynchronizeCanonicalAsdfOwnerAttachments(
        const SpectrumSnapshotHandle& snapshot,
        SourceCollectionSessionAction* action)
{
    const std::optional<SourceCollectionIdentity> identity =
        navigation_.active_source_identity();
    const SourceCollectionManifest* context =
        navigation_.active_context();
    const std::vector<SampleLabelingTask>* tasks =
        labeling_.View().active_source_tasks;
    if (!snapshot || !identity || context == nullptr ||
        tasks == nullptr) {
        return false;
    }

    const SampleLabelingCanonicalSourceDescriptor descriptor =
        BuildSampleLabelingCanonicalSourceDescriptor(
            *snapshot,
            *identity,
            *context);
    const SampleAnnotationSourceCompatibility compatibility =
        SampleLabelingCompatibilityView(descriptor);
    bool synchronized = false;
    for (const SampleLabelingTask& task : *tasks) {
        if (!task.persistence.output_path ||
            task.persistence.output_format !=
                SampleLabelingOutputArtifactFormat::CanonicalAsdf ||
            SourceCollectionManifestContainsAnnotation(
                *context,
                *task.persistence.output_path)) {
            continue;
        }
        if (!runtime_paths_.application_data_root.empty() &&
            CheckUserFilePath(*task.persistence.output_path, runtime_paths_) !=
                UserFilePathStatus::Allowed) {
            continue;
        }
        std::optional<SampleAnnotationResult> annotation =
            SampleAnnotationIoAdapter{}.LoadForSource(
                *task.persistence.output_path,
                compatibility);
        if (!annotation) {
            continue;
        }
        bool attachment_added = false;
        if (!navigation_.UpsertAttachedAnnotationForActiveSource(
                std::move(*annotation),
                &attachment_added)) {
            continue;
        }
        synchronized = true;
        if (action != nullptr) {
            action->navigation_inputs_changed = true;
            action->annotation_roster_changed =
                action->annotation_roster_changed ||
                attachment_added;
        }
    }
    return synchronized;
}

void SampleWorkflowCoordinator::ClearSampleWorkflow(SourceCollectionSessionAction& action)
{
    DiscardPreparedViewCaches();
    ClearLabelUndoHistory();
    labeling_.ClearActiveSource();
    workflow_sources_.Clear();
    active_sample_workflow_identity_.reset();
    active_sample_workflow_context_fingerprint_.reset();
    action.workflow_changed = true;
}

SampleWorkflowCoordinator::NavigationInputReconcileEffects
SampleWorkflowCoordinator::ReconcileNavigationInputs(
    const SpectrumSnapshotHandle& snapshot,
    NavigationInputReconcileRequest request)
{
    NavigationInputReconcileEffects effects;
    effects.workflow_changed = request.workflow_changed;
    effects.navigation_inputs_changed = request.filters_changed || request.sorting_changed;
    if (request.filters_changed) {
        const std::optional<std::size_t> target_before =
            deferred_sample_navigation_
            ? navigation_.pending_index()
            : navigation_.current_index();
        const std::optional<std::size_t> target =
            ApplySampleFilters(snapshot);
        const std::optional<std::size_t> target_after =
            deferred_sample_navigation_
            ? navigation_.pending_index()
            : navigation_.current_index();
        if (target || target_after != target_before) {
            effects.snapshot_target_updated = true;
            effects.snapshot_index_to_load = target;
        }
    }
    if (request.sorting_changed) {
        const std::optional<std::size_t> target_before =
            deferred_sample_navigation_
            ? navigation_.pending_index()
            : navigation_.current_index();
        const std::optional<std::size_t> target =
            ApplySampleSorting(snapshot);
        const std::optional<std::size_t> target_after =
            deferred_sample_navigation_
            ? navigation_.pending_index()
            : navigation_.current_index();
        if (target || target_after != target_before) {
            effects.snapshot_target_updated = true;
            effects.snapshot_index_to_load = target;
        }
    }
    return effects;
}

void SampleWorkflowCoordinator::ApplyNavigationInputEffects(
    SampleWorkflowTransitionOutcome& outcome,
    const NavigationInputReconcileEffects& effects)
{
    outcome.action.workflow_changed =
        outcome.action.workflow_changed ||
        effects.workflow_changed;
    outcome.action.navigation_inputs_changed =
        outcome.action.navigation_inputs_changed ||
        effects.navigation_inputs_changed;
    if (effects.snapshot_target_updated) {
        outcome.snapshot_target_updated = true;
        outcome.snapshot_index_to_load =
            effects.snapshot_index_to_load;
    }
}

std::optional<std::size_t> SampleWorkflowCoordinator::ApplySampleFilters(const SpectrumSnapshotHandle& snapshot)
{
    const std::size_t sample_count = ActiveSampleCount(snapshot);
    if (sample_count == 0) {
        return navigation_.ClearSampleFilter(deferred_sample_navigation_);
    }
    if (!workflow_sources_.has_filter_conditions()) {
        return navigation_.ClearSampleFilter(deferred_sample_navigation_);
    }

    const SampleFilterEvaluation evaluation = workflow_sources_.EvaluateFilters(SourcePolicyContext(snapshot));
    if (evaluation.active) {
        return navigation_.SetSampleFilter(evaluation.included_samples, deferred_sample_navigation_);
    }
    return navigation_.ClearSampleFilter(deferred_sample_navigation_);
}

std::optional<std::size_t> SampleWorkflowCoordinator::ApplySampleSorting(
    const SpectrumSnapshotHandle& snapshot)
{
    SampleWorkflowSortChoiceResult sort_choice =
        workflow_sources_.BuildSortChoice(SourcePolicyContext(snapshot), !restoring_source_session_);
    if (sort_choice.state_changed) {
        MarkActiveWorkflowStateDirty();
    }
    if (!sort_choice.choice) {
        return navigation_.ClearSampleSorting(deferred_sample_navigation_);
    }

    return navigation_.SetSampleSorting(
        std::move(*sort_choice.choice),
        deferred_sample_navigation_);
}

std::size_t SampleWorkflowCoordinator::ActiveSampleCount(const SpectrumSnapshotHandle& snapshot) const
{
    return navigation_.spectrum_count().value_or(snapshot ? snapshot->collection.spectrum_count : 0);
}

std::optional<std::size_t> SampleWorkflowCoordinator::ActiveSampleIndex(const SpectrumSnapshotHandle& snapshot) const
{
    if (navigation_.spectrum_count()) {
        return navigation_.current_index();
    }
    if (snapshot && !snapshot->source.path.empty() && snapshot->collection.spectrum_count > 0) {
        return snapshot->collection.current_index;
    }
    return std::nullopt;
}

SampleWorkflowSourceContext SampleWorkflowCoordinator::SourcePolicyContext(
    const SpectrumSnapshotHandle& snapshot) const
{
    const SampleLabelingControllerView labeling_view =
        labeling_.View();
    return SampleWorkflowSourceContext{
        .collection = navigation_.active_context(),
        .labeling_tasks =
            labeling_view.active_source_tasks,
        .sample_count = ActiveSampleCount(snapshot),
        .context_generation =
            navigation_.active_context_generation(),
        .labeling_generation =
            labeling_.active_source_tasks_generation(),
    };
}

void SampleWorkflowCoordinator::EnsureWorkflowStateCacheLoaded()
{
    if (workflow_state_cache_loaded_) {
        return;
    }
    workflow_state_cache_loaded_ = true;
    SampleWorkflowStateCacheLoadResult result =
        workflow_state_cache_loader_(workflow_state_cache_path_);
    workflow_state_cache_ = std::move(result.cache);
    workflow_state_persistence_.SetLoadWarning(std::move(result.warning));
}

void SampleWorkflowCoordinator::AdoptPreparedCache(
    const std::shared_ptr<const SampleWorkflowPreparationCacheBundle>& cache,
    std::vector<BackgroundRetirementHandle>& background_retirement)
{
    if (!cache) {
        return;
    }
    const bool first_workflow_load =
        !workflow_state_cache_loaded_;
    std::shared_ptr<const SampleNavigationStateCacheLoadResult>
        navigation_cache(cache, &cache->navigation);
    if (BackgroundRetirementHandle retired =
            navigation_.AdoptPreparedStateCache(
                std::move(navigation_cache))) {
        background_retirement.push_back(std::move(retired));
    }
    std::shared_ptr<const SampleWorkflowStateCache> workflow_cache(cache, &cache->workflow);
    if (workflow_cache != workflow_state_cache_snapshot_) {
        if (workflow_state_cache_snapshot_) {
            background_retirement.push_back(workflow_state_cache_snapshot_);
        }
        workflow_state_cache_snapshot_ = std::move(workflow_cache);
        if (first_workflow_load) {
            workflow_state_persistence_.SetLoadWarning(
                cache->workflow_warning);
        }
    }
    workflow_state_cache_loaded_ = true;

    std::shared_ptr<const SampleLabelingStateCacheLoadResult> labeling_cache(cache, &cache->labeling);
    if (BackgroundRetirementHandle retired =
            labeling_.AdoptPreparedStateCache(std::move(labeling_cache))) {
        background_retirement.push_back(std::move(retired));
    }
}

const SampleWorkflowSourceState* SampleWorkflowCoordinator::CachedWorkflowState(
    std::string_view source_identity) const
{
    const std::string identity{source_identity};
    if (workflow_state_tombstones_.contains(identity)) {
        return nullptr;
    }
    if (const auto state = workflow_state_cache_.sources_by_identity.find(identity);
        state != workflow_state_cache_.sources_by_identity.end()) {
        return &state->second;
    }
    if (!workflow_state_cache_snapshot_) {
        return nullptr;
    }
    const auto state = workflow_state_cache_snapshot_->sources_by_identity.find(identity);
    return state == workflow_state_cache_snapshot_->sources_by_identity.end() ? nullptr : &state->second;
}

void SampleWorkflowCoordinator::RestoreActiveWorkflowState(std::string_view source_identity)
{
    EnsureWorkflowStateCacheLoaded();

    workflow_sources_.Clear();
    const SampleWorkflowSourceState* state = CachedWorkflowState(source_identity);
    if (state == nullptr) {
        return;
    }
    workflow_sources_.RestoreState(*state);
}

void SampleWorkflowCoordinator::StoreActiveWorkflowState()
{
    if (!active_sample_workflow_identity_ || active_sample_workflow_identity_->empty()) {
        return;
    }
    EnsureWorkflowStateCacheLoaded();

    SampleWorkflowSourceState state = workflow_sources_.StoreState();
    if (workflow_sources_.HasState()) {
        workflow_state_cache_.sources_by_identity[*active_sample_workflow_identity_] = std::move(state);
        workflow_state_tombstones_.erase(*active_sample_workflow_identity_);
    } else {
        workflow_state_cache_.sources_by_identity.erase(*active_sample_workflow_identity_);
        workflow_state_tombstones_.insert(*active_sample_workflow_identity_);
    }
}

void SampleWorkflowCoordinator::MarkActiveWorkflowStateDirty()
{
    StoreActiveWorkflowState();
    if (!workflow_state_cache_path_.empty()) {
        workflow_state_persistence_.MarkDirty();
    }
}

LocalUserStatePersistenceLifecycle::SaveResult
SampleWorkflowCoordinator::SaveWorkflowStateCache()
{
    SampleWorkflowStateCache merged = workflow_state_cache_snapshot_
        ? *workflow_state_cache_snapshot_
        : SampleWorkflowStateCache{};
    for (const auto& [identity, state] : workflow_state_cache_.sources_by_identity) {
        merged.sources_by_identity[identity] = state;
    }
    for (const std::string& identity : workflow_state_tombstones_) {
        merged.sources_by_identity.erase(identity);
    }
    if (SaveSampleWorkflowStateCache(runtime_paths_, workflow_state_cache_path_, merged)) {
        return {.saved = true};
    }
    return {
        .saved = false,
        .error = "Could not save sample workflow state.",
    };
}

bool SampleWorkflowCoordinator::FlushWorkflowStateCache()
{
    return workflow_state_persistence_.Flush(
               [this] {
                   return SaveWorkflowStateCache();
               }) !=
        LocalUserStatePersistenceLifecycle::FlushOutcome::Failed;
}

SampleWorkflowTransitionOutcome SampleWorkflowCoordinator::ApplyLabelWriteResult(
    const SpectrumSnapshotHandle& snapshot,
    SampleLabelingWriteOperationResult result,
    NavigationTargetResolutionReport* target_resolution,
    bool record_undo,
    std::optional<std::size_t> restore_sample_index)
{
    DiscardPreparedViewCaches();
    SampleWorkflowTransitionOutcome outcome;
    const SampleLabelWriteResult& write =
        result.write;
    outcome.label_write = result;
    ApplyLabelingLeaseIssue(outcome, result.operation);
    if (!write.changed) {
        return outcome;
    }

    if (record_undo) {
        RecordLabelUndo(write);
    }

    if (result.operation.output_saved) {
        (void)SynchronizeActiveCanonicalAsdfAttachment(
            &outcome.action);
    }

    const SampleLabelingTask* task = labeling_.View().active_task;

    const NavigationInputReconcileEffects effects = ReconcileNavigationInputs(
        snapshot,
        NavigationInputReconcileRequest{.filters_changed = true});
    ApplyNavigationInputEffects(outcome, effects);

    if (restore_sample_index) {
        MergeSampleWorkflowTransitionOutcome(
            outcome,
            RequestSampleNavigation(
                SampleNavigationRequest::RestoreLabelUndoPosition(
                    *restore_sample_index),
                snapshot));
    } else if (write.advance_requested && task != nullptr) {
        MergeSampleWorkflowTransitionOutcome(
            outcome,
            RequestSampleNavigation(
                BuildAutoAdvanceRequest(*task),
                snapshot,
                write.sample_index,
                target_resolution));
    }
    return outcome;
}

void SampleWorkflowCoordinator::RecordLabelUndo(const SampleLabelWriteResult& result)
{
    const SampleLabelingTask* task = labeling_.View().active_task;
    if (!active_sample_workflow_identity_ || task == nullptr) {
        return;
    }
    if (!LabelUndoHistoryMatchesActiveTask()) {
        label_undo_history_ = LabelUndoHistory{
            .workflow_identity = *active_sample_workflow_identity_,
            .task_id = task->task_id};
    }
    if (label_undo_history_->entries.size() == kMaxLabelUndoEntries) {
        label_undo_history_->entries.erase(label_undo_history_->entries.begin());
    }
    label_undo_history_->entries.push_back(LabelUndoEntry{
        .sample_index = result.sample_index,
        .previous_code = result.previous_code,
        .current_code = result.current_code});
}

void SampleWorkflowCoordinator::ClearLabelUndoHistory()
{
    label_undo_history_.reset();
}

bool SampleWorkflowCoordinator::LabelUndoHistoryMatchesActiveTask() const
{
    const SampleLabelingTask* task = labeling_.View().active_task;
    return label_undo_history_ && active_sample_workflow_identity_ && task != nullptr &&
           label_undo_history_->workflow_identity == *active_sample_workflow_identity_ &&
           label_undo_history_->task_id == task->task_id;
}

}  // namespace spectiary
