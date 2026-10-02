#include "app/project_identity.h"
#include "ui/sample_labeling_controller.h"

#include "app/local_user_state.h"
#include "domain/sample_annotation_io.h"
#include "domain/sample_label_export.h"
#include "domain/source_collection_manifest.h"
#include "domain/source_path_identity.h"
#include "domain/stable_sha256.h"
#include "domain/utf8.h"
#include "domain/uuid_v4.h"
#include "ui/sample_annotation_labeling_rules.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace spectiary {
namespace {

using namespace std::chrono_literals;

constexpr auto kStateSaveDebounce = 500ms;
constexpr auto kStateSaveRetry = 2s;
constexpr auto kSynchronousCommitLockWait = 2s;

bool HasPendingOutputSave(const SampleLabelingTask& task)
{
    return !task.persistence.pending_sample_indices.empty() || task.persistence.metadata_save_pending;
}

bool IsCanonicalAsdfOutputPath(
    const std::filesystem::path& path)
{
    const std::filesystem::path::string_type extension =
        path.extension().native();
    if (extension.size() != 5U || extension[0] != '.') {
        return false;
    }
    const auto lower_ascii = [](
        std::filesystem::path::value_type character) {
        return character >= 'A' && character <= 'Z'
            ? static_cast<std::filesystem::path::value_type>(
                  character - 'A' + 'a')
            : character;
    };
    return lower_ascii(extension[1]) == 'a' &&
        lower_ascii(extension[2]) == 's' &&
        lower_ascii(extension[3]) == 'd' &&
        lower_ascii(extension[4]) == 'f';
}

bool IsValidAnnotationPromotionOrigin(
    const SampleLabelingOrigin& origin)
{
    if (origin.kind != "annotation_promotion" ||
        !origin.annotation ||
        !IsValidSampleLabelingAnnotationOriginName(
            origin.annotation->name) ||
        (origin.annotation->format != "csv" &&
         origin.annotation->format != "npy")) {
        return false;
    }
    if (!origin.annotation->fingerprint) {
        return true;
    }
    constexpr std::string_view kPrefix = "sha256:";
    const std::string_view fingerprint =
        *origin.annotation->fingerprint;
    if (!fingerprint.starts_with(kPrefix) ||
        fingerprint.size() != kPrefix.size() + 64U) {
        return false;
    }
    const std::string_view digest =
        fingerprint.substr(kPrefix.size());
    return std::all_of(
        digest.begin(),
        digest.end(),
        [](char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f');
        });
}

bool SameLabelSet(
    const SampleLabelSet& left,
    const SampleLabelSet& right)
{
    return left.labels.size() == right.labels.size() &&
        std::equal(
            left.labels.begin(),
            left.labels.end(),
            right.labels.begin(),
            [](const SampleLabelDefinition& left_label,
               const SampleLabelDefinition& right_label) {
                return left_label.code == right_label.code &&
                    left_label.name == right_label.name &&
                    left_label.shortcut == right_label.shortcut;
            });
}

bool PromotionArtifactMatchesPlannedGeneration(
    const std::filesystem::path& path,
    std::size_t sample_count,
    const SampleLabelingOrigin& origin,
    std::string_view task_name,
    const SampleLabelSet& label_set,
    std::span<const int> values,
    const SampleLabelingCanonicalSourceDescriptor*
        source_descriptor)
{
    if (!origin.annotation ||
        !origin.annotation->fingerprint) {
        return true;
    }

    SampleAnnotationIoAdapter adapter;
    std::string reload_error;
    std::optional<SampleAnnotationResult> reloaded =
        source_descriptor == nullptr
        ? adapter.Load(
              path,
              sample_count,
              &reload_error)
        : adapter.LoadForSource(
              path,
              SampleLabelingCompatibilityView(
                  *source_descriptor),
              &reload_error);
    if (!reloaded ||
        !reloaded->artifact_provenance ||
        *reloaded->artifact_provenance !=
            *origin.annotation) {
        return false;
    }

    const SampleLabelResultMetadata* metadata =
        reloaded->label_metadata
        ? &*reloaded->label_metadata
        : nullptr;
    const SampleAnnotationLabelingActivationPlan reloaded_plan =
        PlanSampleAnnotationLabelingActivation(
            SampleAnnotationLabelingActivationRequest{
                .annotation = &*reloaded,
                .metadata = metadata,
            });
    return reloaded_plan.kind ==
            SampleAnnotationLabelingActivationKind::
                CreateTaskFromAnnotation &&
        reloaded_plan.origin == origin &&
        reloaded_plan.task_name == task_name &&
        SameLabelSet(
            reloaded_plan.label_set,
            label_set) &&
        std::ranges::equal(
            reloaded_plan.values,
            values);
}

bool ShouldRetryOutputSave(const SampleLabelingTask& task)
{
    if (!task.persistence.output_path || !HasPendingOutputSave(task)) {
        return false;
    }
    switch (task.persistence.output_format) {
    case SampleLabelingOutputArtifactFormat::
        LegacyNpyWithSidecar:
        return false; // Retain recovery state until explicit ASDF migration.
    case SampleLabelingOutputArtifactFormat::CanonicalAsdf:
        return (task.persistence.metadata_save_pending ||
                !task.persistence.pending_sample_indices.empty()) &&
            (task.persistence.save_state.kind ==
                 SampleLabelSaveStateKind::Pending ||
             task.persistence.save_state.kind ==
                 SampleLabelSaveStateKind::Failed);
    case SampleLabelingOutputArtifactFormat::None:
        return false;
    }
    return false;
}

bool OutputPathMatches(const std::filesystem::path& left, const std::filesystem::path& right)
{
    if (left.empty() || right.empty()) {
        return false;
    }
    std::error_code left_exists_error;
    std::error_code right_exists_error;
    const bool left_exists = std::filesystem::exists(left, left_exists_error) && !left_exists_error;
    const bool right_exists = std::filesystem::exists(right, right_exists_error) && !right_exists_error;
    std::error_code equivalent_error;
    if (left_exists && right_exists &&
        std::filesystem::equivalent(left, right, equivalent_error) && !equivalent_error) {
        return true;
    }
    const std::string left_key =
        OutputPathIdentityKey(left);
    const std::string right_key =
        OutputPathIdentityKey(right);
    if (!left_key.empty() && !right_key.empty()) {
        return left_key == right_key;
    }
    return left.lexically_normal() ==
        right.lexically_normal();
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

std::string TaskIdentityEditLeaseKey(
    std::string_view source_identity,
    std::string_view task_id)
{
    if (source_identity.empty() || task_id.empty()) {
        return {};
    }
    return "task\n" + std::string(source_identity) +
        "\n" + std::string(task_id);
}

std::string TemporarySlotEditLeaseKey(
    std::string_view source_identity)
{
    if (source_identity.empty()) {
        return {};
    }
    return "temporary-slot\n" +
        std::string(source_identity);
}

std::vector<std::string> OutputEditLeaseKeys(
    const SampleAnnotationArtifactIdentitySet& identities)
{
    std::vector<std::string> keys;
    keys.reserve(
        identities.stable_path_keys.size() +
        identities.physical_path_keys.size());
    for (const std::string& key :
         identities.stable_path_keys) {
        keys.push_back("artifact-path\n" + key);
    }
    for (const std::string& key :
         identities.physical_path_keys) {
        keys.push_back("artifact\n" + key);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

std::vector<std::string> OutputEditLeaseKeys(
    const SampleLabelingTask& task,
    bool resolve_physical_paths = true)
{
    if (!task.persistence.output_path) {
        return {};
    }
    return OutputEditLeaseKeys(
        SampleAnnotationArtifactIdentities(
            *task.persistence.output_path,
            task.persistence.output_format,
            resolve_physical_paths));
}

std::vector<std::string> NpyExportEditLeaseKeys(
    const std::filesystem::path& output_path)
{
    // A stateless NPY export writes only the array, but it must coordinate
    // with both paths that form an existing legacy NPY owner.
    return OutputEditLeaseKeys(
        SampleAnnotationArtifactIdentities(
            output_path,
            SampleLabelingOutputArtifactFormat::
                LegacyNpyWithSidecar));
}

std::vector<std::string> SingleFileExportEditLeaseKeys(
    const std::filesystem::path& output_path)
{
    SampleAnnotationArtifactIdentitySet identities;
    identities.stable_path_keys.push_back(
        SourcePathIdentityKey(output_path));
    identities.physical_path_keys =
        OutputPathIdentityKeys(output_path);
    return OutputEditLeaseKeys(identities);
}

bool HasManagedOutputArtifactConflict(
    const std::unordered_map<
        std::string,
        SampleLabelingController::SourceState>& sources,
    std::span<const std::string> requested)
{
    for (const auto& [source_identity, state] : sources) {
        (void)source_identity;
        for (const SampleLabelingTask& task : state.tasks) {
            if (!task.persistence.output_path) {
                continue;
            }
            const std::vector<std::string> managed =
                OutputEditLeaseKeys(task);
            if (std::any_of(
                    requested.begin(),
                    requested.end(),
                    [&managed](const std::string& key) {
                        return std::binary_search(
                            managed.begin(),
                            managed.end(),
                            key);
                    })) {
                return true;
            }
        }
    }
    return false;
}

bool SameTaskProjection(
    const SampleLabelingTask& left,
    const SampleLabelingTask& right)
{
    const bool labels_match =
        left.label_set.labels.size() ==
            right.label_set.labels.size() &&
        std::equal(
            left.label_set.labels.begin(),
            left.label_set.labels.end(),
            right.label_set.labels.begin(),
            [](const SampleLabelDefinition& first,
               const SampleLabelDefinition& second) {
                return first.code == second.code &&
                    first.name == second.name &&
                    first.shortcut == second.shortcut;
            });
    return left.task_id == right.task_id &&
        left.task_name == right.task_name &&
        left.canonical_metadata == right.canonical_metadata &&
        labels_match && left.values == right.values &&
        left.session.auto_advance == right.session.auto_advance &&
        left.session.skip_labeled_on_advance ==
            right.session.skip_labeled_on_advance &&
        left.session.remembered_position ==
            right.session.remembered_position &&
        left.persistence.output_path == right.persistence.output_path &&
        left.persistence.output_format == right.persistence.output_format &&
        left.values.IsComplete() ==
            right.values.IsComplete() &&
        left.persistence.pending_sample_indices ==
            right.persistence.pending_sample_indices &&
        left.persistence.metadata_save_pending ==
            right.persistence.metadata_save_pending &&
        left.persistence.save_state.kind == right.persistence.save_state.kind &&
        left.persistence.save_state.pending_count ==
            right.persistence.save_state.pending_count &&
        left.persistence.save_state.message_kind ==
            right.persistence.save_state.message_kind &&
        left.persistence.save_state.message ==
            right.persistence.save_state.message;
}

bool SameSourceTaskProjection(
    const SampleLabelingController::SourceState& left,
    const SampleLabelingController::SourceState& right)
{
    return left.active_task_id == right.active_task_id &&
        left.tasks.size() == right.tasks.size() &&
        std::equal(
            left.tasks.begin(),
            left.tasks.end(),
            right.tasks.begin(),
            SameTaskProjection);
}

std::vector<std::filesystem::path> TaskEditLeasePaths(
    const std::filesystem::path& state_cache_path,
    std::string_view lease_key)
{
    std::vector<std::filesystem::path> paths;
    if (lease_key.empty()) {
        return paths;
    }
    StableSha256 digest;
    digest.Append(project_identity::kLabelingEditLeaseDomain);
    digest.Append(lease_key);
    const std::string file_name =
        digest.FinishHex() + ".lock";
    for (const std::filesystem::path& coordination_directory :
         SampleLabelingStateCoordinationDirectories(
             state_cache_path)) {
        paths.push_back(
            coordination_directory /
            "targets" /
            file_name);
    }
    return paths;
}

SampleLabelResultMetadataSource SourceMetadataFromState(const SampleLabelingController::SourceState& state)
{
    SampleLabelResultMetadataSource source;
    source.source_name = state.source_name;
    source.source_fingerprint = state.source_fingerprint;
    source.context_fingerprint = state.context_fingerprint;
    source.spectrum_count = state.sample_count;
    return source;
}

std::optional<SampleLabelingDocument>
BuildCanonicalDocumentReplacement(
    const SampleLabelingContentView& task,
    const SampleLabelingAsdfOpenSnapshot& snapshot,
    std::string* error_message)
{
    const SampleLabelingDocument& current = snapshot.document();
    if (task.task_id != current.labeling.id ||
        task.values.size() != current.annotation.values.size()) {
        if (error_message != nullptr) {
            *error_message =
                "canonical ASDF document publication does not match the opened owner generation";
        }
        return std::nullopt;
    }

    SampleLabelingDocument replacement = current;
    replacement.annotation.values.clear();
    replacement.annotation.values.reserve(task.values.size());
    for (const int value : task.values) {
        const std::int64_t widened =
            static_cast<std::int64_t>(value);
        if (widened <
                std::numeric_limits<std::int32_t>::min() ||
            widened >
                std::numeric_limits<std::int32_t>::max()) {
            if (error_message != nullptr) {
                *error_message =
                    "canonical ASDF label value is outside the int32 range";
            }
            return std::nullopt;
        }
        replacement.annotation.values.push_back(
            static_cast<std::int32_t>(value));
    }

    replacement.labeling.name = task.task_name;
    replacement.labeling.canonical_metadata = task.canonical_metadata;
    replacement.labeling.labels.clear();
    replacement.labeling.labels.reserve(
        task.label_set.labels.size());
    for (const SampleLabelDefinition& label :
         task.label_set.labels) {
        const std::int64_t widened =
            static_cast<std::int64_t>(label.code);
        if (widened <
                std::numeric_limits<std::int32_t>::min() ||
            widened >
                std::numeric_limits<std::int32_t>::max()) {
            if (error_message != nullptr) {
                *error_message =
                    "canonical ASDF label code is outside the int32 range";
            }
            return std::nullopt;
        }
        replacement.labeling.labels.push_back(
            SampleLabelingDocumentLabel{
                .code = static_cast<std::int32_t>(label.code),
                .name = label.name,
                .shortcut = label.shortcut == '\0'
                    ? std::string{}
                    : std::string(1, label.shortcut)});
    }

    if (!ValidateSampleLabelingDocumentFailFast(
             replacement)
             .valid()) {
        if (error_message != nullptr) {
            *error_message =
                "canonical ASDF metadata publication produced an invalid document";
        }
        return std::nullopt;
    }
    return replacement;
}

bool CanDeleteTask(const SampleLabelingTask& task)
{
    if (!task.persistence.output_path) {
        return true;
    }
    return task.persistence.save_state.kind != SampleLabelSaveStateKind::Pending &&
           task.persistence.save_state.kind != SampleLabelSaveStateKind::Failed;
}

bool RecoverySnapshotTrusted(
    const SampleLabelingStateCacheLoadResult& snapshot)
{
    return snapshot.issue_kind ==
            SampleLabelingStateCacheLoadIssueKind::None &&
        snapshot.warning.empty();
}

std::unordered_set<std::string> TaskIds(
    const SampleLabelingController::SourceState& state)
{
    std::unordered_set<std::string> ids;
    ids.reserve(state.tasks.size());
    for (const SampleLabelingTask& task : state.tasks) {
        if (!task.task_id.empty()) {
            ids.insert(task.task_id);
        }
    }
    return ids;
}

std::unordered_set<std::string> TaskIdsMatchingSnapshot(
    const SampleLabelingController::SourceState& state,
    const SampleLabelingController::SourceState& snapshot_state)
{
    std::unordered_set<std::string> ids;
    ids.reserve(state.tasks.size());
    for (const SampleLabelingTask& task : state.tasks) {
        if (task.task_id.empty()) {
            continue;
        }
        const auto snapshot_task = std::find_if(
            snapshot_state.tasks.begin(),
            snapshot_state.tasks.end(),
            [&task](const SampleLabelingTask& candidate) {
                return candidate.task_id == task.task_id;
            });
        if (snapshot_task != snapshot_state.tasks.end() &&
            SameTaskProjection(task, *snapshot_task)) {
            ids.insert(task.task_id);
        }
    }
    return ids;
}

}  // namespace

SampleLabelingController::SampleLabelingController()
    : SampleLabelingController(std::filesystem::path{}, RuntimePaths{})
{
}

SampleLabelingController::SampleLabelingController(std::filesystem::path state_cache_path,
    const RuntimePaths& runtime_paths)
    : SampleLabelingController(
          std::move(state_cache_path),
          [runtime_paths](const std::filesystem::path& path) { return LoadSampleLabelingStateCache(runtime_paths, path, {}, SampleLabelingStateCacheLoadPolicy::AllowPersistentOutputs); }, runtime_paths)
{
}

SampleLabelingController::SampleLabelingController(
    std::filesystem::path state_cache_path,
    StateCacheLoader state_cache_loader,
    const RuntimePaths& runtime_paths)
    : SampleLabelingController(
          std::move(state_cache_path),
          std::move(state_cache_loader),
          [](const SampleLabelingAsdfOpenSnapshot& snapshot,
             const SampleLabelingDocument& document,
             const SampleLabelingCanonicalSourceDescriptor& source) {
              return RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
                  snapshot,
                  document,
                  SampleLabelingCompatibilityView(source));
          }, runtime_paths)
{
}

SampleLabelingController::SampleLabelingController(
    std::filesystem::path state_cache_path,
    StateCacheLoader state_cache_loader,
    CanonicalDocumentPublisher canonical_document_publisher,
    const RuntimePaths& runtime_paths)
    : SampleLabelingController(
          std::move(state_cache_path),
          std::move(state_cache_loader),
          std::move(canonical_document_publisher),
          [](const std::filesystem::path& path,
             const SampleLabelingDocument& document,
             const SampleLabelingCanonicalSourceDescriptor& source) {
              return WriteSampleLabelingAsdfDocumentAndOpenAtomically(
                  path,
                  document,
                  SampleLabelingCompatibilityView(source));
          }, runtime_paths)
{
}

SampleLabelingController::SampleLabelingController(
    std::filesystem::path state_cache_path,
    StateCacheLoader state_cache_loader,
    CanonicalDocumentPublisher canonical_document_publisher,
    CanonicalValuesPublisher canonical_values_publisher,
    const RuntimePaths& runtime_paths)
    : SampleLabelingController(
          std::move(state_cache_path),
          std::move(state_cache_loader),
          canonical_document_publisher
              ? std::move(canonical_document_publisher)
              : CanonicalDocumentPublisher{
                    [](const SampleLabelingAsdfOpenSnapshot& snapshot,
                       const SampleLabelingDocument& document,
                       const SampleLabelingCanonicalSourceDescriptor& source) {
                        return RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
                            snapshot,
                            document,
                            SampleLabelingCompatibilityView(source));
                    }},
          canonical_values_publisher
              ? std::move(canonical_values_publisher)
              : CanonicalValuesPublisher{
                    [](SampleLabelingAsdfOpenSnapshot& snapshot,
                       const SampleLabelingDocument& replacement) {
                        return RewriteSampleLabelingAsdfValuesAtomically(
                            snapshot,
                            replacement);
                    }},
          [](const std::filesystem::path& path,
             const SampleLabelingDocument& document,
             const SampleLabelingCanonicalSourceDescriptor& source) {
              return WriteSampleLabelingAsdfDocumentAndOpenAtomically(
                  path,
                  document,
                  SampleLabelingCompatibilityView(source));
          },
          []() { return GenerateUuidV4(); },
          []() { return CurrentCanonicalTimestamp(); }, runtime_paths)
{
}

SampleLabelingController::SampleLabelingController(
    std::filesystem::path state_cache_path,
    StateCacheLoader state_cache_loader,
    CanonicalDocumentPublisher canonical_document_publisher,
    CanonicalCreationPublisher canonical_creation_publisher,
    const RuntimePaths& runtime_paths)
    : SampleLabelingController(
          std::move(state_cache_path),
          std::move(state_cache_loader),
          std::move(canonical_document_publisher),
          std::move(canonical_creation_publisher),
          []() { return GenerateUuidV4(); },
          []() { return CurrentCanonicalTimestamp(); }, runtime_paths)
{
}

SampleLabelingController::SampleLabelingController(
    std::filesystem::path state_cache_path,
    StateCacheLoader state_cache_loader,
    CanonicalDocumentPublisher canonical_document_publisher,
    CanonicalCreationPublisher canonical_creation_publisher,
    TaskIdGenerator task_id_generator,
    TaskClock task_clock,
    const RuntimePaths& runtime_paths)
    : SampleLabelingController(
          std::move(state_cache_path),
          std::move(state_cache_loader),
          std::move(canonical_document_publisher),
          [](SampleLabelingAsdfOpenSnapshot& snapshot,
             const SampleLabelingDocument& replacement) {
              return RewriteSampleLabelingAsdfValuesAtomically(
                  snapshot,
                  replacement);
          },
          std::move(canonical_creation_publisher),
          std::move(task_id_generator),
          std::move(task_clock), runtime_paths)
{
}

SampleLabelingController::SampleLabelingController(
    std::filesystem::path state_cache_path,
    StateCacheLoader state_cache_loader,
    CanonicalDocumentPublisher canonical_document_publisher,
    CanonicalValuesPublisher canonical_values_publisher,
    CanonicalCreationPublisher canonical_creation_publisher,
    TaskIdGenerator task_id_generator,
    TaskClock task_clock,
    const RuntimePaths& runtime_paths)
    : runtime_paths_(runtime_paths),
      state_cache_path_(std::move(state_cache_path)),
      state_cache_loader_(std::move(state_cache_loader)),
      canonical_document_publisher_(
          std::move(canonical_document_publisher)),
      canonical_values_publisher_(
          std::move(canonical_values_publisher)),
      canonical_creation_publisher_(
          std::move(canonical_creation_publisher)),
      task_id_generator_(std::move(task_id_generator)),
      task_clock_(std::move(task_clock)),
      state_cache_save_scheduler_(kStateSaveDebounce, kStateSaveRetry),
      output_retry_scheduler_(kStateSaveRetry, kStateSaveRetry)
{
}

SampleLabelingController::SourceState
SampleLabelingController::MergeRefreshedSourceState(
    std::string_view source_identity,
    const SourceState& local,
    SourceState refreshed) const
{
    const auto pending_source =
        pending_cache_patch_.sources.find(
            std::string(source_identity));
    const SampleLabelingSourceStatePatch* patch =
        pending_source == pending_cache_patch_.sources.end()
        ? nullptr
        : &pending_source->second;
    const auto is_tombstoned =
        [patch](std::string_view task_id) {
            return patch != nullptr &&
                std::find(
                    patch->task_tombstones.begin(),
                    patch->task_tombstones.end(),
                    task_id) != patch->task_tombstones.end();
        };
    const auto replace_or_append =
        [&refreshed](const SampleLabelingTask& task) {
            const auto current = std::find_if(
                refreshed.tasks.begin(),
                refreshed.tasks.end(),
                [&task](const SampleLabelingTask& candidate) {
                    return candidate.task_id == task.task_id;
                });
            if (current == refreshed.tasks.end()) {
                refreshed.tasks.push_back(task);
            } else {
                *current = task;
            }
        };
    const auto task_lease_held =
        [this, &source_identity](std::string_view task_id) {
            if (TaskIdentityLeaseHeld(
                    active_task_leases_,
                    source_identity,
                    task_id)) {
                return true;
            }
            return std::any_of(
                deferred_task_leases_.begin(),
                deferred_task_leases_.end(),
                [this, &source_identity, task_id](
                    const TaskEditLeaseSet& leases) {
                    return TaskIdentityLeaseHeld(
                        leases,
                        source_identity,
                        task_id);
                });
        };

    if (patch != nullptr) {
        refreshed.tasks.erase(
            std::remove_if(
                refreshed.tasks.begin(),
                refreshed.tasks.end(),
                [&is_tombstoned](
                    const SampleLabelingTask& task) {
                    return is_tombstoned(task.task_id);
                }),
            refreshed.tasks.end());
        for (const SampleLabelingTask& pending_task :
             patch->task_upserts) {
            if (!is_tombstoned(pending_task.task_id)) {
                replace_or_append(pending_task);
            }
        }
    }

    for (const SampleLabelingTask& local_task : local.tasks) {
        if (is_tombstoned(local_task.task_id) ||
            PendingTaskUpsert(
                source_identity,
                local_task.task_id) != nullptr) {
            continue;
        }
        if (task_lease_held(local_task.task_id)) {
            replace_or_append(local_task);
        }
    }

    if (patch != nullptr &&
        patch->active_task_selection_changed) {
        refreshed.active_task_id = patch->active_task_id;
    } else if (local.active_task_id &&
               !is_tombstoned(*local.active_task_id) &&
               (PendingTaskUpsert(
                    source_identity,
                    *local.active_task_id) != nullptr ||
                task_lease_held(*local.active_task_id))) {
        refreshed.active_task_id = local.active_task_id;
    }
    if (refreshed.active_task_id &&
        std::none_of(
            refreshed.tasks.begin(),
            refreshed.tasks.end(),
            [&refreshed](const SampleLabelingTask& task) {
                return task.task_id ==
                    *refreshed.active_task_id;
            })) {
        refreshed.active_task_id.reset();
    }
    return refreshed;
}

void SampleLabelingController::ActivateSource(std::string source_identity, std::size_t sample_count)
{
    ActivateSourceInternal(
        std::move(source_identity),
        sample_count,
        std::nullopt);
}

void SampleLabelingController::ActivateSourceInternal(
    std::string source_identity,
    std::size_t sample_count,
    std::optional<SampleLabelingCanonicalSourceDescriptor>
        source_descriptor)
{
    EnsureStateCacheLoaded();
    if (source_identity.empty() || sample_count == 0) {
        ClearActiveSource();
        return;
    }

    const bool active_source_changed =
        !active_source_identity_ ||
        *active_source_identity_ != source_identity;
    if (active_source_changed) {
        ReplaceActiveSourceDescriptor(std::nullopt);
    }
    if (source_descriptor) {
        if (source_descriptor->base_identity ==
                source_identity &&
            source_descriptor->sample_count == sample_count) {
            ReplaceActiveSourceDescriptor(
                std::move(source_descriptor));
        } else {
            ReplaceActiveSourceDescriptor(std::nullopt);
        }
    }
    bool tasks_replaced = false;
    std::unordered_set<std::string> prepared_task_ids;
    std::unordered_set<std::string> preserved_local_task_ids;
    std::optional<bool> prepared_snapshot_trusted;
    if (active_source_changed) {
        ReleaseActiveTaskLeaseForTransition();
    }
    if (lease_unavailable_source_identities_.contains(
            source_identity) &&
        state_cache_loader_ &&
        !state_cache_path_.empty()) {
        SampleLabelingStateCacheLoadResult latest =
            state_cache_loader_(state_cache_path_);
        if (latest.issue_kind ==
            SampleLabelingStateCacheLoadIssueKind::None) {
            state_cache_load_warning_ = latest.warning;
            state_cache_load_diagnostic_detail_ = latest.diagnostic_detail;
            state_cache_snapshot_ =
                std::make_shared<
                    const SampleLabelingStateCacheLoadResult>(
                    std::move(latest));
            const auto refreshed =
                state_cache_snapshot_->cache.sources.find(
                    source_identity);
            if (refreshed !=
                state_cache_snapshot_->cache.sources.end()) {
                prepared_task_ids = TaskIds(refreshed->second);
                prepared_snapshot_trusted =
                    RecoverySnapshotTrusted(
                        *state_cache_snapshot_);
                const auto existing =
                    sources_.find(source_identity);
                if (existing == sources_.end()) {
                    sources_.emplace(
                        source_identity,
                        refreshed->second);
                    tasks_replaced = true;
                } else {
                    for (const SampleLabelingTask& task :
                         existing->second.tasks) {
                        if (LocalTaskProjectionProtected(
                                source_identity,
                                task.task_id)) {
                            preserved_local_task_ids.insert(
                                task.task_id);
                        }
                    }
                    const SourceState local = existing->second;
                    SourceState merged =
                        MergeRefreshedSourceState(
                            source_identity,
                            local,
                            refreshed->second);
                    tasks_replaced =
                        !SameSourceTaskProjection(local, merged);
                    existing->second = std::move(merged);
                }
            }
        }
    }
    const bool source_was_present =
        sources_.contains(source_identity);
    SourceState* materialized = MaterializeSource(source_identity);
    if (!source_was_present &&
        !prepared_snapshot_trusted &&
        state_cache_snapshot_) {
        const auto cached =
            state_cache_snapshot_->cache.sources.find(
                source_identity);
        if (cached != state_cache_snapshot_->cache.sources.end()) {
            prepared_task_ids = TaskIds(cached->second);
            prepared_snapshot_trusted =
                RecoverySnapshotTrusted(
                    *state_cache_snapshot_);
        }
    }
    SourceState& state = materialized == nullptr ? sources_[source_identity] : *materialized;
    if (state.sample_count != 0 && state.sample_count != sample_count) {
        std::vector<std::string> replaced_task_ids;
        replaced_task_ids.reserve(state.tasks.size());
        for (const SampleLabelingTask& task : state.tasks) {
            replaced_task_ids.push_back(task.task_id);
        }
        state.tasks.clear();
        state.active_task_id.reset();
        state.sample_count = sample_count;
        for (std::string& task_id : replaced_task_ids) {
            MarkTaskTombstone(
                source_identity,
                state,
                std::move(task_id));
        }
        MarkActiveTaskSelection(
            source_identity,
            state);
        QueueStateSave();
        DeferActiveTaskLeases();
        tasks_replaced = true;
    }
    state.sample_count = sample_count;
    active_source_identity_ = std::move(source_identity);
    if (prepared_snapshot_trusted == true) {
        ExpireTaskLeaseConflictsOnTrustedRefresh(
            *active_source_identity_);
    }
    ReconcileRecoveryTaskTrust(
        *active_source_identity_,
        state,
        prepared_task_ids,
        preserved_local_task_ids,
        prepared_snapshot_trusted);
    const bool task_projection_changed =
        RestoreActiveTaskLease();
    if (std::any_of(state.tasks.begin(), state.tasks.end(), ShouldRetryOutputSave)) {
        QueueOutputRetry();
    }
    if (active_source_changed || tasks_replaced ||
        task_projection_changed) {
        BumpActiveSourceTasksGeneration();
    }
    Touch();
}

void SampleLabelingController::ActivateSource(const SourceCollectionIdentity& identity)
{
    ActivateSourceInternal(
        identity.id,
        identity.spectrum_count,
        std::nullopt);
    SourceState* state = ActiveSource();
    if (state == nullptr) {
        return;
    }
    state->source_name = identity.source_name;
    state->source_fingerprint = identity.source_fingerprint;
    state->context_fingerprint = identity.context_fingerprint;
    MarkSourceMetadataUpsert(identity.id, *state);
    QueueStateSave();
    Touch();
}

void SampleLabelingController::ActivateSource(
    const SourceCollectionIdentity& identity,
    SampleLabelingCanonicalSourceDescriptor source_descriptor)
{
    if (source_descriptor.base_identity != identity.id ||
        source_descriptor.source_name != identity.source_name ||
        source_descriptor.source_fingerprint !=
            identity.source_fingerprint ||
        source_descriptor.sample_count != identity.spectrum_count) {
        source_descriptor = {};
    }
    ActivateSourceInternal(
        identity.id,
        identity.spectrum_count,
        std::optional<
            SampleLabelingCanonicalSourceDescriptor>{
            std::move(source_descriptor)});
    SourceState* state = ActiveSource();
    if (state == nullptr) {
        return;
    }
    state->source_name = identity.source_name;
    state->source_fingerprint = identity.source_fingerprint;
    state->context_fingerprint = identity.context_fingerprint;
    MarkSourceMetadataUpsert(identity.id, *state);
    QueueStateSave();
    Touch();
}

SampleLabelingPreparedSourceActivationResult
SampleLabelingController::ActivatePreparedSource(
    const SourceCollectionIdentity& identity,
    std::optional<SourceState> prepared_state,
    std::optional<SampleLabelingCanonicalSourceDescriptor>
        source_descriptor)
{
    SampleLabelingPreparedSourceActivationResult result;
    if (identity.id.empty() || identity.spectrum_count == 0) {
        if (prepared_state) {
            result.background_retirement =
                MakeBackgroundRetirementHandle(
                    std::move(*prepared_state));
        }
        ClearActiveSource();
        return result;
    }
    if (prepared_state) {
        for (SampleLabelingTask& task :
             prepared_state->tasks) {
            static_cast<void>(
                DowngradeCanonicalSampleLabelingTaskToStructural(
                    task));
        }
    }
    const bool active_source_changed =
        !active_source_identity_ ||
        *active_source_identity_ != identity.id;
    if (active_source_changed) {
        ReplaceActiveSourceDescriptor(std::nullopt);
    }
    if (source_descriptor) {
        if (source_descriptor->base_identity == identity.id &&
            source_descriptor->source_name ==
                identity.source_name &&
            source_descriptor->source_fingerprint ==
                identity.source_fingerprint &&
            source_descriptor->sample_count ==
                identity.spectrum_count) {
            ReplaceActiveSourceDescriptor(
                std::move(source_descriptor));
        } else {
            ReplaceActiveSourceDescriptor(std::nullopt);
        }
    }
    if (active_source_changed) {
        ReleaseActiveTaskLeaseForTransition();
    }
    if (lease_unavailable_source_identities_.contains(
            identity.id) &&
        state_cache_loader_ &&
        !state_cache_path_.empty()) {
        SampleLabelingStateCacheLoadResult latest =
            state_cache_loader_(state_cache_path_);
        if (latest.issue_kind ==
            SampleLabelingStateCacheLoadIssueKind::None) {
            state_cache_load_warning_ = latest.warning;
            state_cache_load_diagnostic_detail_ = latest.diagnostic_detail;
            state_cache_snapshot_ =
                std::make_shared<
                    const SampleLabelingStateCacheLoadResult>(
                    std::move(latest));
            const auto refreshed =
                state_cache_snapshot_->cache.sources.find(
                    identity.id);
            if (refreshed !=
                state_cache_snapshot_->cache.sources.end()) {
                prepared_state = refreshed->second;
            }
        }
    }
    std::unordered_set<std::string> prepared_task_ids;
    std::optional<bool> prepared_snapshot_trusted;
    if (prepared_state && state_cache_snapshot_) {
        const auto snapshot_source =
            state_cache_snapshot_->cache.sources.find(
                identity.id);
        if (snapshot_source !=
            state_cache_snapshot_->cache.sources.end()) {
            prepared_task_ids =
                TaskIdsMatchingSnapshot(
                    *prepared_state,
                    snapshot_source->second);
            prepared_snapshot_trusted =
                RecoverySnapshotTrusted(
                    *state_cache_snapshot_);
        }
    }
    std::unordered_set<std::string> preserved_local_task_ids;
    auto existing = sources_.find(identity.id);
    BackgroundRetirementHandle retired;
    bool tasks_replaced = false;
    bool reconciled_sample_count_mismatch = false;
    if (existing == sources_.end()) {
        SourceState state = prepared_state ? std::move(*prepared_state) : SourceState{};
        existing = sources_.emplace(identity.id, std::move(state)).first;
        tasks_replaced = true;
    } else if (existing->second.sample_count != 0 &&
               existing->second.sample_count != identity.spectrum_count) {
        for (const SampleLabelingTask& task :
             existing->second.tasks) {
            MarkTaskTombstone(
                identity.id,
                existing->second,
                task.task_id);
        }
        auto retired_state = std::make_shared<SourceState>();
        *retired_state = std::move(existing->second);
        retired = std::move(retired_state);
        existing->second = prepared_state ? std::move(*prepared_state) : SourceState{};
        DeferActiveTaskLeases();
        tasks_replaced = true;
        reconciled_sample_count_mismatch = true;
    } else if (prepared_state) {
        for (const SampleLabelingTask& task :
             existing->second.tasks) {
            if (LocalTaskProjectionProtected(
                    identity.id,
                    task.task_id)) {
                preserved_local_task_ids.insert(
                    task.task_id);
            }
        }
        const SourceState& local = existing->second;
        SourceState merged =
            MergeRefreshedSourceState(
                identity.id,
                local,
                std::move(*prepared_state));
        result.prepared_task_projection_changed =
            !SameSourceTaskProjection(local, merged);
        retired = MakeBackgroundRetirementHandle(
            std::move(existing->second));
        existing->second = std::move(merged);
        tasks_replaced = true;
    }
    SourceState& state = existing->second;
    state.sample_count = identity.spectrum_count;
    state.source_name = identity.source_name;
    state.source_fingerprint = identity.source_fingerprint;
    state.context_fingerprint = identity.context_fingerprint;
    active_source_identity_ = identity.id;
    if (prepared_snapshot_trusted == true) {
        ExpireTaskLeaseConflictsOnTrustedRefresh(
            identity.id);
    }
    ReconcileRecoveryTaskTrust(
        identity.id,
        state,
        prepared_task_ids,
        preserved_local_task_ids,
        prepared_snapshot_trusted);
    result.prepared_task_projection_changed =
        RestoreActiveTaskLease() ||
        result.prepared_task_projection_changed;
    if (reconciled_sample_count_mismatch) {
        MarkSourceMetadataUpsert(identity.id, state);
        for (const SampleLabelingTask& task : state.tasks) {
            MarkTaskUpsert(
                identity.id,
                state,
                task);
        }
        MarkActiveTaskSelection(identity.id, state);
        QueueStateSave();
    }
    if (std::any_of(state.tasks.begin(), state.tasks.end(), ShouldRetryOutputSave)) {
        QueueOutputRetry();
    }
    if (active_source_changed || tasks_replaced ||
        result.prepared_task_projection_changed) {
        BumpActiveSourceTasksGeneration();
    }
    Touch();
    result.background_retirement =
        std::move(retired);
    return result;
}

BackgroundRetirementHandle SampleLabelingController::AdoptPreparedStateCache(
    std::shared_ptr<const SampleLabelingStateCacheLoadResult> cache_snapshot)
{
    if (!cache_snapshot || cache_snapshot == state_cache_snapshot_) {
        return {};
    }
    const bool first_load = !state_cache_loaded_;
    std::shared_ptr<const SampleLabelingStateCacheLoadResult> retired =
        std::exchange(state_cache_snapshot_, std::move(cache_snapshot));
    state_cache_loaded_ = true;
    if (first_load) {
        state_cache_load_warning_ = state_cache_snapshot_->warning;
        state_cache_load_diagnostic_detail_ = state_cache_snapshot_->diagnostic_detail;
    }
    Touch();
    return retired;
}

std::vector<BackgroundRetirementHandle> SampleLabelingController::ReleaseBackgroundResourcesForShutdown()
{
    ReleaseActiveTaskLease();
    deferred_task_leases_.clear();
    const bool changed =
        state_cache_snapshot_ != nullptr || !sources_.empty() || active_source_identity_.has_value();
    std::vector<BackgroundRetirementHandle> resources;
    if (state_cache_snapshot_) {
        resources.push_back(std::move(state_cache_snapshot_));
    }
    if (!sources_.empty()) {
        resources.push_back(MakeBackgroundRetirementHandle(std::exchange(sources_, {})));
    }
    active_source_identity_.reset();
    ReplaceActiveSourceDescriptor(std::nullopt);
    if (changed) {
        BumpActiveSourceTasksGeneration();
        Touch();
    }
    return resources;
}

void SampleLabelingController::ClearActiveSource()
{
    if (!active_source_identity_) {
        ReplaceActiveSourceDescriptor(std::nullopt);
        return;
    }
    ReleaseActiveTaskLeaseForTransition();
    active_source_identity_.reset();
    ReplaceActiveSourceDescriptor(std::nullopt);
    BumpActiveSourceTasksGeneration();
    Touch();
}

void SampleLabelingController::RemoveSource(std::string_view source_identity)
{
    if (active_source_identity_ && *active_source_identity_ == source_identity) {
        ReleaseActiveTaskLeaseForTransition();
        active_source_identity_.reset();
        ReplaceActiveSourceDescriptor(std::nullopt);
        BumpActiveSourceTasksGeneration();
        Touch();
    }
}

SampleLabelingControllerView SampleLabelingController::View() const
{
    const SourceState* state = ActiveSource();
    return SampleLabelingControllerView{
        .active_task = ActiveTask(),
        .temporary_task = TemporaryTask(),
        .active_source_tasks = state == nullptr ? nullptr : &state->tasks,
        .revision = revision_};
}

SampleLabelingRecoveryView SampleLabelingController::RecoveryView() const
{
    SampleLabelingRecoveryView view{
        .source_identity = active_source_identity_.value_or(std::string{}),
        .revision = revision_};
    const SourceState* state = ActiveSource();
    if (state == nullptr) {
        return view;
    }

    std::unordered_map<std::string, std::size_t> task_id_counts;
    std::size_t temporary_task_count = 0;
    for (const SampleLabelingTask& task : state->tasks) {
        ++task_id_counts[task.task_id];
        if (!task.persistence.output_path) {
            ++temporary_task_count;
        }
    }

    view.temporary_drafts.reserve(temporary_task_count);
    for (const SampleLabelingTask& task : state->tasks) {
        if (task.persistence.output_path) {
            continue;
        }

        const auto untrusted_source =
            recovery_untrusted_task_ids_by_source_.find(
                view.source_identity);
        const bool stale =
            (!task.task_id.empty() &&
             untrusted_source !=
                 recovery_untrusted_task_ids_by_source_.end() &&
             untrusted_source->second.contains(task.task_id)) ||
            task.task_id.empty() ||
            task.values.SampleCount() != state->sample_count;
        const bool duplicate_task_id =
            task_id_counts.at(task.task_id) > 1;
        const std::string task_identity_key =
            TaskIdentityEditLeaseKey(
                view.source_identity,
                task.task_id);
        const bool known_task_lease_conflict =
            !task_identity_key.empty() &&
            lease_unavailable_task_targets_.contains(
                task_identity_key);
        const bool is_current =
            state->active_task_id &&
            *state->active_task_id == task.task_id;

        SampleLabelingRecoveryDraftStatus status =
            SampleLabelingRecoveryDraftStatus::Recoverable;
        if (stale) {
            status = SampleLabelingRecoveryDraftStatus::Stale;
        } else if (duplicate_task_id) {
            status = SampleLabelingRecoveryDraftStatus::Conflicting;
        } else if (known_task_lease_conflict) {
            status = SampleLabelingRecoveryDraftStatus::Conflicting;
        } else if (is_current) {
            status = SampleLabelingRecoveryDraftStatus::Current;
        } else if (temporary_task_count > 1) {
            status = SampleLabelingRecoveryDraftStatus::Conflicting;
        }
        view.temporary_drafts.push_back(
            SampleLabelingRecoveryDraftView{
                .task = &task,
                .status = status});
    }
    return view;
}

std::uint64_t
SampleLabelingController::active_source_tasks_generation() const
{
    return active_source_tasks_generation_;
}

SampleLabelingTask* SampleLabelingController::ActiveTask()
{
    SourceState* state = ActiveSource();
    if (state == nullptr || !state->active_task_id) {
        return nullptr;
    }

    const auto match = std::find_if(state->tasks.begin(), state->tasks.end(), [state](const auto& task) {
        return task.task_id == *state->active_task_id;
    });
    return match == state->tasks.end() ? nullptr : &*match;
}

const SampleLabelingTask* SampleLabelingController::ActiveTask() const
{
    const SourceState* state = ActiveSource();
    if (state == nullptr || !state->active_task_id) {
        return nullptr;
    }

    const auto match = std::find_if(state->tasks.begin(), state->tasks.end(), [state](const auto& task) {
        return task.task_id == *state->active_task_id;
    });
    return match == state->tasks.end() ? nullptr : &*match;
}

SampleLabelingTask* SampleLabelingController::TemporaryTask()
{
    SourceState* state = ActiveSource();
    if (state == nullptr) {
        return nullptr;
    }
    const auto match = std::find_if(state->tasks.begin(), state->tasks.end(), [](const auto& task) {
        return !task.persistence.output_path;
    });
    return match == state->tasks.end() ? nullptr : &*match;
}

const SampleLabelingTask* SampleLabelingController::TemporaryTask() const
{
    const SourceState* state = ActiveSource();
    if (state == nullptr) {
        return nullptr;
    }
    const auto match = std::find_if(state->tasks.begin(), state->tasks.end(), [](const auto& task) {
        return !task.persistence.output_path;
    });
    return match == state->tasks.end() ? nullptr : &*match;
}

std::optional<SampleLabelingController::SourceState> SampleLabelingController::SourceStateForIdentity(
    std::string_view source_identity)
{
    EnsureStateCacheLoaded();
    const SourceState* state = MaterializeSource(source_identity);
    if (state == nullptr) {
        return std::nullopt;
    }
    SourceState exported = *state;
    for (SampleLabelingTask& task : exported.tasks) {
        static_cast<void>(
            DowngradeCanonicalSampleLabelingTaskToStructural(
                task));
    }
    return exported;
}

std::optional<SampleAnnotationResult>
SampleLabelingController::
    ActiveCanonicalAsdfAnnotationProjection() const
{
    const SampleLabelingTask* task = ActiveTask();
    if (task == nullptr ||
        !task->values.IsComplete() ||
        task->persistence.output_format !=
            SampleLabelingOutputArtifactFormat::CanonicalAsdf ||
        !task->persistence.output_path ||
        !active_asdf_snapshot_ ||
        !OutputPathMatches(
            *task->persistence.output_path,
            active_asdf_snapshot_->path()) ||
        active_asdf_snapshot_->document().labeling.id !=
            task->task_id) {
        return std::nullopt;
    }

    const std::shared_ptr<const SampleLabelingDocument>
        document =
            active_asdf_snapshot_->document_handle();
    SampleAnnotationResult projection;
    projection.name = document->labeling.name;
    projection.path = active_asdf_snapshot_->path();
    projection.kind =
        SampleAnnotationKind::CategoricalInteger;
    projection.dtype = "<i4";
    projection.dtype_name = "int32";
    projection.relationship =
        SampleAnnotationWorkflowRelationship::
            ExternalLabelResult;
    projection.labeling_document = document;
    projection.values.reserve(
        document->annotation.values.size());
    for (const std::int32_t value :
         document->annotation.values) {
        projection.values.push_back(
            SampleAnnotationValue{
                static_cast<std::int64_t>(value)});
    }
    return projection;
}

SampleLabelingOperationResult SampleLabelingController::CreateTask(
    std::string task_name)
{
    SourceState* state = ActiveSource();
    if (state == nullptr ||
        !IsValidUtf8WithNonWhitespace(task_name) ||
        !task_id_generator_ ||
        !task_clock_) {
        return RejectOperation();
    }
    const std::optional<std::string> requested_task_id =
        task_id_generator_();
    if (!requested_task_id ||
        !IsCanonicalUuidV4(*requested_task_id)) {
        return RejectOperation();
    }

    bool refreshed_task_projection_changed = false;
    const auto preserve_refreshed_projection =
        [&refreshed_task_projection_changed](
            SampleLabelingOperationResult result) {
            result.task_projection_changed =
                result.task_projection_changed ||
                refreshed_task_projection_changed;
            return result;
        };
    if (SampleLabelingTask* existing_temporary_task = TemporaryTask()) {
        if (state->active_task_id && *state->active_task_id == existing_temporary_task->task_id) {
            SampleLabelingOperationResult result = RejectOperation();
            result.accepted = true;
            return result;
        }
        const std::string stale_task_id =
            existing_temporary_task->task_id;
        SampleLabelingOperationResult resumed =
            ActivateTaskWithExpectation(
                stale_task_id,
                TaskActivationExpectation::
                    TemporaryTask);
        refreshed_task_projection_changed =
            resumed.task_projection_changed;
        if (resumed.issue !=
                SampleLabelingOperationResult::Issue::
                    EditTargetChanged ||
            TemporaryTask() != nullptr) {
            return resumed;
        }
        state = ActiveSource();
        if (state == nullptr) {
            return preserve_refreshed_projection(
                RejectOperation());
        }
    }

    const SampleLabelingTask* active_task = ActiveTask();
    if (active_task != nullptr &&
        !CanDeleteTask(*active_task)) {
        return preserve_refreshed_projection(
            RejectOperation());
    }

    TaskCreationPreparation preparation =
        PrepareTaskCreation(
            *active_source_identity_,
            *requested_task_id,
            state->sample_count,
            state->tasks,
            true);
    if (preparation.lease_status !=
        ExclusiveFileLeaseAcquireStatus::Acquired) {
        return preserve_refreshed_projection(
            RejectLeaseAcquireStatus(
                preparation.lease_status));
    }
    if (!preparation.ready &&
        !state_cache_load_warning_.empty() &&
        state->tasks.empty()) {
        constexpr std::size_t kMaximumIdAttempts = 64;
        for (std::size_t attempt = 0;
             attempt < kMaximumIdAttempts;
             ++attempt) {
            const std::optional<std::string> generated =
                attempt == 0
                ? requested_task_id
                : task_id_generator_();
            if (generated &&
                IsCanonicalUuidV4(*generated) &&
                std::none_of(
                    state->tasks.begin(),
                    state->tasks.end(),
                    [&generated](const SampleLabelingTask& task) {
                        return task.task_id == *generated;
                    })) {
                preparation.task_id = *generated;
                break;
            }
        }
        if (preparation.task_id.empty()) {
            return preserve_refreshed_projection(
                RejectEditTargetChanged());
        }
        preparation.leases.task_identity_key =
            TaskIdentityEditLeaseKey(
                *active_source_identity_,
                preparation.task_id);
        if (!state_cache_path_.empty()) {
            TaskEditLeaseAcquireResult task_lease =
                TryAcquireTaskEditLease(
                    preparation.leases
                        .task_identity_key);
            if (task_lease.status !=
                ExclusiveFileLeaseAcquireStatus::Acquired) {
                return preserve_refreshed_projection(
                    RejectLeaseAcquireStatus(
                        task_lease.status));
            }
            preparation.leases.task_identity =
                std::move(task_lease.component);
        }
        preparation.ready = true;
    }
    if (!preparation.ready ||
        preparation.task_id.empty()) {
        return preserve_refreshed_projection(
            RejectEditTargetChanged());
    }

    const CanonicalTimestamp created_at = task_clock_();
    SampleLabelingTaskCanonicalMetadata canonical_metadata;
    canonical_metadata.created_at = created_at;
    canonical_metadata.modified_at = created_at;
    canonical_metadata.origin.kind = "manual";
    state->tasks.push_back(CreateSampleLabelingTask(
        std::move(preparation.task_id),
        std::move(task_name),
        state->sample_count,
        std::move(canonical_metadata)));
    SampleLabelingTask& created = state->tasks.back();
    state->active_task_id = created.task_id;
    MarkTaskUpsert(
        *active_source_identity_,
        *state,
        created,
        true);
    MarkActiveTaskSelection(
        *active_source_identity_,
        *state);
    SampleLabelingOperationResult result = CompleteMutation(
        &created,
        PersistencePolicy::FlushStateSave,
        TaskProjectionEffect::Unchanged);
    TransitionActiveTaskLeases(
        std::move(preparation.leases),
        std::nullopt,
        result.state_saved);
    return preserve_refreshed_projection(
        std::move(result));
}

SampleLabelingOperationResult
SampleLabelingController::StartOrResumeTemporaryTask()
{
    const auto next_task_name = [this]() {
        const SourceState* current = ActiveSource();
        const std::string base{kTemporarySampleLabelingTaskName};
        std::string candidate = base;
        for (std::size_t suffix = 1; current != nullptr &&
             std::ranges::any_of(current->tasks, [&](const SampleLabelingTask& task) {
                 return task.task_name == candidate;
             }); ++suffix) {
            candidate = base + " " + std::to_string(suffix);
        }
        return candidate;
    };
    SourceState* state = ActiveSource();
    if (state == nullptr) {
        return RejectOperation();
    }

    const SampleLabelingTask* active_task = ActiveTask();
    const SampleLabelingTask* temporary_task =
        TemporaryTask();
    if (active_task != nullptr &&
        temporary_task != nullptr &&
        active_task->task_id ==
            temporary_task->task_id) {
        SampleLabelingOperationResult result =
            RejectOperation();
        result.accepted = true;
        return result;
    }
    if (active_task != nullptr &&
        !CanDeleteTask(*active_task)) {
        return RejectOperation();
    }

    if (temporary_task != nullptr) {
        const std::string stale_task_id =
            temporary_task->task_id;
        SampleLabelingOperationResult result =
            ActivateTaskWithExpectation(
                stale_task_id,
                TaskActivationExpectation::
                    TemporaryTask);
        if (result.issue !=
                SampleLabelingOperationResult::Issue::
                    EditTargetChanged ||
            TemporaryTask() != nullptr) {
            return result;
        }
        SampleLabelingOperationResult created = CreateTask(
            next_task_name());
        created.task_projection_changed =
            created.task_projection_changed ||
            result.task_projection_changed;
        return created;
    }
    return CreateTask(
        next_task_name());
}

SampleLabelingOperationResult SampleLabelingController::CreateTaskFromAnnotation(
    std::string task_name,
    SampleLabelSet label_set,
    std::vector<int> values,
    std::filesystem::path annotation_path)
{
    SampleLabelingOrigin origin{.kind = "annotation_promotion"};
    const std::filesystem::path filename = annotation_path.filename();
    const std::u8string name = filename.u8string();
    std::string extension = annotation_path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const std::string format = extension == ".csv"
        ? "csv"
        : extension == ".npy" ? "npy" : std::string{};
    origin.annotation = SampleLabelingAnnotationOrigin{
        .name = std::string{
            reinterpret_cast<const char*>(name.data()), name.size()},
        .format = format};
    return CreateTaskFromAnnotation(
        std::move(task_name),
        std::move(label_set),
        std::move(values),
        std::move(annotation_path),
        std::move(origin));
}

SampleLabelingOperationResult SampleLabelingController::CreateTaskFromAnnotation(
    std::string task_name,
    SampleLabelSet label_set,
    std::vector<int> values,
    std::filesystem::path annotation_path,
    SampleLabelingOrigin origin)
{
    SourceState* state = ActiveSource();
    if (state == nullptr ||
        !IsValidUtf8WithNonWhitespace(task_name) ||
        !task_id_generator_ ||
        !task_clock_ ||
        !IsValidAnnotationPromotionOrigin(origin) ||
        annotation_path.empty() ||
        values.size() != state->sample_count) {
        return RejectOperation();
    }

    if (TemporaryTask() != nullptr) {
        return RejectOperation();
    }

    const std::optional<std::string> requested_task_id =
        task_id_generator_();
    if (!requested_task_id ||
        !IsCanonicalUuidV4(*requested_task_id)) {
        return RejectOperation();
    }

    const SampleLabelingTask* current_active_task =
        ActiveTask();
    if (current_active_task != nullptr &&
        !CanDeleteTask(*current_active_task)) {
        return RejectOperation();
    }

    TaskCreationPreparation preparation =
        PrepareTaskCreation(
            *active_source_identity_,
            *requested_task_id,
            state->sample_count,
            state->tasks,
            true);
    if (preparation.lease_status !=
        ExclusiveFileLeaseAcquireStatus::Acquired) {
        return RejectLeaseAcquireStatus(
            preparation.lease_status);
    }
    if (!preparation.ready ||
        preparation.task_id.empty()) {
        return RejectEditTargetChanged();
    }
    const CanonicalTimestamp created_at = task_clock_();
    SampleLabelingTaskCanonicalMetadata canonical_metadata;
    canonical_metadata.created_at = created_at;
    canonical_metadata.modified_at = created_at;
    canonical_metadata.origin = std::move(origin);
    SampleLabelingTask task = CreateSampleLabelingTask(
        std::move(preparation.task_id),
        std::move(task_name),
        state->sample_count,
        std::move(canonical_metadata));
    task.label_set = std::move(label_set);
    task.values.Complete() = std::move(values);
    RebuildSampleLabelingTaskStatistics(task);
    if (!PromotionArtifactMatchesPlannedGeneration(
            annotation_path,
            state->sample_count,
            task.canonical_metadata.origin,
            task.task_name,
            task.label_set,
            task.values.Complete(),
            active_source_descriptor_
                ? &*active_source_descriptor_
                : nullptr)) {
        return RejectEditTargetChanged();
    }

    // CSV and NPY are import sources. Their bytes are never an autosave target.
    MarkSampleLabelTaskPersisted(task, SampleLabelSaveStateKind::InternalDraftOnly);

    state->tasks.push_back(std::move(task));
    state->active_task_id = state->tasks.back().task_id;
    MarkTaskUpsert(
        *active_source_identity_,
        *state,
        state->tasks.back(),
        true);
    MarkActiveTaskSelection(
        *active_source_identity_,
        *state);
    SampleLabelingOperationResult result = CompleteMutation(
        &state->tasks.back(),
        PersistencePolicy::FlushStateSave,
        TaskProjectionEffect::Changed);
    TransitionActiveTaskLeases(
        std::move(preparation.leases),
        std::nullopt,
        result.state_saved);
    return result;
}

SampleLabelingOperationResult
SampleLabelingController::AdoptCanonicalAsdfTask(
    std::string expected_task_id,
    std::filesystem::path output_path)
{
    return ConnectCanonicalAsdfTask(
        std::move(expected_task_id),
        std::move(output_path),
        true,
        true);
}

SampleLabelingOperationResult
SampleLabelingController::RelinkCanonicalAsdfTask(
    std::string expected_task_id,
    std::filesystem::path output_path)
{
    return ConnectCanonicalAsdfTask(
        std::move(expected_task_id),
        std::move(output_path),
        false,
        false);
}

SampleLabelingOperationResult
SampleLabelingController::ConnectCanonicalAsdfTask(
    std::string expected_task_id,
    std::filesystem::path output_path,
    bool allow_new_adoption,
    bool activate_task)
{
    if (!runtime_paths_.application_data_root.empty() &&
        CheckUserFilePath(output_path, runtime_paths_) != UserFilePathStatus::Allowed) {
        return {.revision = revision_, .issue = SampleLabelingOperationResult::Issue::UserFilePathRejected};
    }

    SourceState* state = ActiveSource();
    if (state == nullptr ||
        !active_source_identity_ ||
        !IsCanonicalUuidV4(expected_task_id) ||
        output_path.empty() ||
        !IsCanonicalAsdfOutputPath(output_path) ||
        !active_source_descriptor_ ||
        active_source_descriptor_->base_identity !=
            *active_source_identity_) {
        return RejectOperation();
    }

    const auto id_match = std::find_if(
        state->tasks.begin(),
        state->tasks.end(),
        [&expected_task_id](const SampleLabelingTask& task) {
            return task.task_id == expected_task_id;
        });
    const auto output_match = std::find_if(
        state->tasks.begin(),
        state->tasks.end(),
        [&output_path](const SampleLabelingTask& task) {
            return task.persistence.output_path &&
                OutputPathMatches(
                    *task.persistence.output_path,
                    output_path);
        });
    const bool relinking_missing_owner =
        id_match != state->tasks.end() &&
        output_match == state->tasks.end() &&
        id_match->persistence.output_path &&
        id_match->persistence.output_format ==
            SampleLabelingOutputArtifactFormat::CanonicalAsdf;
    if (id_match != state->tasks.end() ||
        output_match != state->tasks.end()) {
        if (id_match != state->tasks.end() &&
            output_match == id_match &&
            id_match->persistence.output_format ==
                SampleLabelingOutputArtifactFormat::
                    CanonicalAsdf) {
            if (activate_task) {
                return ActivateTask(expected_task_id);
            }
            SampleLabelingOperationResult result =
                RejectOperation();
            result.accepted = true;
            return result;
        }
        if (!relinking_missing_owner) {
            return RejectEditTargetChanged();
        }
        if (ProbePath(*id_match->persistence.output_path) !=
            PathProbeResult::Missing) {
            // A same-id document at another path is a copy/conflict while the
            // established owner still exists. Automatic relink is reserved
            // for a confirmed missing owner.
            return RejectEditTargetChanged();
        }
    }
    if (!relinking_missing_owner && !allow_new_adoption) {
        return RejectEditTargetChanged();
    }

    const SampleLabelingTask* current_active_task =
        ActiveTask();
    if (relinking_missing_owner &&
        current_active_task != nullptr &&
        current_active_task->task_id == expected_task_id) {
        // The active owner retains its identity and artifact leases. Pause it
        // before reconnecting so this path cannot acquire a second lease set
        // for the same task identity.
        return RejectEditTargetChanged();
    }
    if (activate_task &&
        current_active_task != nullptr &&
        !CanDeleteTask(*current_active_task)) {
        return RejectOperation();
    }

    SampleLabelingTask local_state = relinking_missing_owner
        ? *id_match
        : CreateSampleLabelingTask(
              expected_task_id,
              expected_task_id,
              state->sample_count);
    const std::optional<std::filesystem::path> previous_output_path =
        relinking_missing_owner
        ? local_state.persistence.output_path
        : std::nullopt;
    local_state.persistence.output_path = output_path;
    local_state.persistence.output_format =
        SampleLabelingOutputArtifactFormat::CanonicalAsdf;
    if (!relinking_missing_owner) {
        MarkSampleLabelTaskPersisted(
            local_state,
            SampleLabelSaveStateKind::AutosavedToOutput);
    }

    TaskEditLeaseSet leases;
    leases.task_identity_key =
        TaskIdentityEditLeaseKey(
            *active_source_identity_,
            expected_task_id);
    if (leases.task_identity_key.empty()) {
        return RejectOperation();
    }
    if (!state_cache_path_.empty()) {
        TaskEditLeaseAcquireResult task_lease =
            TryAcquireTaskEditLease(
                leases.task_identity_key);
        if (task_lease.status !=
            ExclusiveFileLeaseAcquireStatus::Acquired) {
            return RejectLeaseAcquireStatus(
                task_lease.status);
        }
        leases.task_identity =
            std::move(task_lease.component);
    }

    std::vector<std::string> output_lease_keys =
        OutputEditLeaseKeys(local_state);
    if (previous_output_path) {
        SampleLabelingTask previous_owner = local_state;
        previous_owner.persistence.output_path = *previous_output_path;
        std::vector<std::string> previous_keys =
            OutputEditLeaseKeys(previous_owner);
        output_lease_keys.insert(
            output_lease_keys.end(),
            previous_keys.begin(),
            previous_keys.end());
        std::sort(
            output_lease_keys.begin(),
            output_lease_keys.end());
        output_lease_keys.erase(
            std::unique(
                output_lease_keys.begin(),
                output_lease_keys.end()),
            output_lease_keys.end());
    }
    ExclusiveFileLeaseAcquireResult output_lease =
        TryAttachArtifactLeases(
            leases,
            std::move(output_lease_keys));
    if (output_lease.status !=
        ExclusiveFileLeaseAcquireStatus::Acquired) {
        return RejectLeaseAcquireStatus(
            output_lease.status);
    }

    bool expected_absent = true;
    if (!state_cache_path_.empty()) {
        const SampleLabelingStateCacheLoadResult latest =
            LoadSampleLabelingStateCache(
                runtime_paths_,
                state_cache_path_,
                {},
                SampleLabelingStateCacheLoadPolicy::
                    AllowPersistentOutputsWithoutResultHydration);
        if (latest.issue_kind !=
            SampleLabelingStateCacheLoadIssueKind::None) {
            return RejectEditTargetChanged();
        }
        const auto latest_source = latest.cache.sources.find(
            *active_source_identity_);
        if (latest_source != latest.cache.sources.end()) {
            if (latest_source->second.sample_count !=
                state->sample_count) {
                return RejectEditTargetChanged();
            }
            const auto latest_id_match = std::find_if(
                latest_source->second.tasks.begin(),
                latest_source->second.tasks.end(),
                [&expected_task_id](
                    const SampleLabelingTask& task) {
                    return task.task_id == expected_task_id;
                });
            if (latest_id_match !=
                latest_source->second.tasks.end()) {
                if (!latest_id_match->persistence.output_path ||
                    latest_id_match->persistence.output_format !=
                        SampleLabelingOutputArtifactFormat::
                            CanonicalAsdf ||
                    latest_id_match->values.SampleCount() !=
                        state->sample_count) {
                    return RejectEditTargetChanged();
                }
                local_state = *latest_id_match;
                if (!OutputPathMatches(
                        *latest_id_match->persistence.output_path,
                        output_path)) {
                    if (!relinking_missing_owner ||
                        !previous_output_path ||
                        !OutputPathMatches(
                            *latest_id_match->persistence.output_path,
                            *previous_output_path)) {
                        return RejectEditTargetChanged();
                    }
                    if (ProbePath(
                            *latest_id_match->persistence.output_path) !=
                        PathProbeResult::Missing) {
                        return RejectEditTargetChanged();
                    }
                    local_state.persistence.output_path = output_path;
                }
                expected_absent = false;
            } else if (relinking_missing_owner) {
                // Another editor deleted the local record before this editor
                // obtained the identity lease; never resurrect it as an
                // apparently new adoption.
                return RejectEditTargetChanged();
            }
        } else if (relinking_missing_owner) {
            return RejectEditTargetChanged();
        }
        if (HasSampleLabelingOutputPathConflict(
                latest.cache,
                local_state,
                *active_source_identity_)) {
            return RejectEditTargetChanged();
        }
    }

    std::optional<SampleLabelingAsdfOpenSnapshot>
        asdf_snapshot;
    std::string hydration_error;
    std::optional<SampleLabelingTask> hydrated =
        HydrateCanonicalAsdfTask(
            local_state,
            &asdf_snapshot,
            &hydration_error);
    if (!hydrated || !asdf_snapshot) {
        return RejectEditTargetChanged();
    }

    SampleLabelingTask* connected_task = nullptr;
    if (relinking_missing_owner) {
        *id_match = std::move(*hydrated);
        connected_task = &*id_match;
    } else {
        state->tasks.push_back(std::move(*hydrated));
        connected_task = &state->tasks.back();
    }
    if (!activate_task) {
        static_cast<void>(
            DowngradeCanonicalSampleLabelingTaskToStructural(
                *connected_task));
    } else {
        state->active_task_id = connected_task->task_id;
    }
    MarkTaskUpsert(
        *active_source_identity_,
        *state,
        *connected_task,
        expected_absent);
    if (activate_task) {
        MarkActiveTaskSelection(
            *active_source_identity_,
            *state);
    }
    SampleLabelingOperationResult result =
        CompleteMutation(
            connected_task,
            PersistencePolicy::FlushStateSave,
            TaskProjectionEffect::Changed);
    if (activate_task) {
        TransitionActiveTaskLeases(
            std::move(leases),
            std::move(asdf_snapshot),
            result.state_saved);
    } else if (!result.state_saved) {
        deferred_task_leases_.push_back(
            std::move(leases));
    }
    return result;
}

SampleLabelingOperationResult SampleLabelingController::ActivateTask(std::string_view task_id)
{
    return ActivateTaskWithExpectation(
        task_id,
        TaskActivationExpectation::AnyTask);
}

SampleLabelingOperationResult
SampleLabelingController::RecoverTemporaryTask(
    std::string_view source_identity,
    std::string_view task_id)
{
    const std::string owned_task_id{task_id};
    if (source_identity.empty() ||
        owned_task_id.empty() ||
        !active_source_identity_ ||
        *active_source_identity_ != source_identity ||
        ActiveSource() == nullptr) {
        return RejectEditTargetChanged();
    }
    const SourceState* state = ActiveSource();
    const auto match = std::find_if(
        state->tasks.begin(),
        state->tasks.end(),
        [&owned_task_id](const SampleLabelingTask& task) {
            return task.task_id == owned_task_id;
        });
    if (match == state->tasks.end()) {
        return RejectEditTargetChanged();
    }
    return ActivateTaskWithExpectation(
        owned_task_id,
        TaskActivationExpectation::TemporaryTask,
        true);
}

SampleLabelingOperationResult
SampleLabelingController::DeleteTemporaryTask(
    std::string_view source_identity,
    std::string_view task_id)
{
    const std::string owned_task_id{task_id};
    if (source_identity.empty() ||
        owned_task_id.empty() ||
        !active_source_identity_ ||
        *active_source_identity_ != source_identity) {
        return RejectEditTargetChanged();
    }

    SourceState* state = ActiveSource();
    if (state == nullptr) {
        return RejectEditTargetChanged();
    }
    auto match = std::find_if(
        state->tasks.begin(),
        state->tasks.end(),
        [&owned_task_id](const SampleLabelingTask& task) {
            return task.task_id == owned_task_id;
        });
    if (match == state->tasks.end() || match->persistence.output_path) {
        return RejectEditTargetChanged();
    }

    if (state->active_task_id &&
        *state->active_task_id == owned_task_id) {
        return DeleteActiveTask();
    }

    if (!CanDeleteTask(*match)) {
        return RejectOperation();
    }

    TaskActivationPreparation preparation =
        PrepareTaskActivation(
            source_identity,
            *match,
            state->sample_count,
            true,
            true);
    if (preparation.lease_status !=
        ExclusiveFileLeaseAcquireStatus::Acquired) {
        if (preparation.lease_status ==
            ExclusiveFileLeaseAcquireStatus::Unavailable) {
            NoteTaskLeaseUnavailable(
                source_identity,
                owned_task_id);
        }
        return RejectLeaseAcquireStatus(
            preparation.lease_status);
    }
    if (preparation.refresh_status ==
        TaskRefreshStatus::Missing) {
        const bool cancel_pending_create =
            PendingTaskExpectedAbsent(
                source_identity,
                owned_task_id);
        ClearRecoveryTaskTrust(
            source_identity,
            owned_task_id);
        lease_unavailable_task_targets_.erase(
            TaskIdentityEditLeaseKey(
                source_identity,
                owned_task_id));
        state->tasks.erase(match);
        if (cancel_pending_create) {
            MarkTaskTombstone(
                source_identity,
                *state,
                owned_task_id);
        }
        BumpActiveSourceTasksGeneration();
        Touch();
        SampleLabelingOperationResult result =
            RejectEditTargetChanged();
        result.task_projection_changed = true;
        return result;
    }
    if (preparation.refresh_status !=
            TaskRefreshStatus::Ready ||
        !preparation.task) {
        return RejectEditTargetChanged();
    }

    const bool latest_task_is_formal =
        preparation.task->persistence.output_path.has_value();
    const bool task_projection_changed =
        !SameTaskProjection(
            *match,
            *preparation.task);
    *match = std::move(*preparation.task);
    ClearRecoveryTaskTrust(
        source_identity,
        owned_task_id);
    if (latest_task_is_formal) {
        if (task_projection_changed) {
            BumpActiveSourceTasksGeneration();
        }
        Touch();
        SampleLabelingOperationResult result =
            RejectEditTargetChanged();
        result.task_projection_changed =
            task_projection_changed;
        return result;
    }
    state->tasks.erase(match);
    MarkTaskTombstone(
        source_identity,
        *state,
        owned_task_id);
    SampleLabelingOperationResult result = CompleteMutation(
        nullptr,
        PersistencePolicy::FlushStateSave,
        TaskProjectionEffect::Unchanged);
    if (!result.state_saved) {
        deferred_task_leases_.push_back(
            std::move(preparation.leases));
    }
    return result;
}

SampleLabelingOperationResult
SampleLabelingController::DeleteTask(
    std::string_view source_identity,
    std::string_view task_id)
{
    const std::string owned_task_id{task_id};
    if (source_identity.empty() ||
        owned_task_id.empty() ||
        !active_source_identity_ ||
        *active_source_identity_ != source_identity) {
        return RejectEditTargetChanged();
    }

    SourceState* state = ActiveSource();
    if (state == nullptr) {
        return RejectEditTargetChanged();
    }
    auto match = std::find_if(
        state->tasks.begin(),
        state->tasks.end(),
        [&owned_task_id](const SampleLabelingTask& task) {
            return task.task_id == owned_task_id;
        });
    if (match == state->tasks.end()) {
        return RejectEditTargetChanged();
    }
    if (!match->persistence.output_path ||
        ProbePath(*match->persistence.output_path) !=
            PathProbeResult::Missing) {
        return RejectEditTargetChanged();
    }
    if (state->active_task_id &&
        *state->active_task_id == owned_task_id) {
        if (!state_cache_path_.empty()) {
            const SampleLabelingStateCacheLoadResult latest =
                LoadSampleLabelingStateCache(
                    runtime_paths_,
                    state_cache_path_,
                    {},
                    SampleLabelingStateCacheLoadPolicy::
                        AllowPersistentOutputsWithoutResultHydration);
            const auto latest_source =
                latest.cache.sources.find(
                    std::string(source_identity));
            if (latest.issue_kind !=
                    SampleLabelingStateCacheLoadIssueKind::None ||
                latest_source == latest.cache.sources.end() ||
                latest_source->second.sample_count !=
                    state->sample_count) {
                return RejectEditTargetChanged();
            }
            const auto latest_task = std::find_if(
                latest_source->second.tasks.begin(),
                latest_source->second.tasks.end(),
                [&owned_task_id](
                    const SampleLabelingTask& task) {
                    return task.task_id == owned_task_id;
                });
            if (latest_task ==
                    latest_source->second.tasks.end() ||
                !CanDeleteTask(*latest_task) ||
                !latest_task->persistence.output_path ||
                OutputEditLeaseKeys(*latest_task) !=
                    OutputEditLeaseKeys(*match) ||
                ProbePath(*latest_task->persistence.output_path) !=
                    PathProbeResult::Missing) {
                return RejectEditTargetChanged();
            }
        }
        return DeleteActiveTask();
    }
    if (!CanDeleteTask(*match)) {
        return RejectOperation();
    }

    TaskEditLeaseSet leases;
    leases.task_identity_key =
        TaskIdentityEditLeaseKey(
            source_identity,
            owned_task_id);
    if (leases.task_identity_key.empty()) {
        return RejectOperation();
    }
    if (!state_cache_path_.empty()) {
        TaskEditLeaseAcquireResult task_lease =
            TryAcquireTaskEditLease(
                leases.task_identity_key);
        if (task_lease.status !=
            ExclusiveFileLeaseAcquireStatus::Acquired) {
            if (task_lease.status ==
                ExclusiveFileLeaseAcquireStatus::Unavailable) {
                NoteTaskLeaseUnavailable(
                    source_identity,
                    owned_task_id);
            }
            return RejectLeaseAcquireStatus(
                task_lease.status);
        }
        leases.task_identity =
            std::move(task_lease.component);
    }
    const ExclusiveFileLeaseAcquireResult output_lease =
        TryAttachOutputLease(
            leases,
            *match);
    if (output_lease.status !=
        ExclusiveFileLeaseAcquireStatus::Acquired) {
        return RejectLeaseAcquireStatus(
            output_lease.status);
    }

    if (!state_cache_path_.empty()) {
        SampleLabelingStateCacheLoadResult latest =
            LoadSampleLabelingStateCache(
                runtime_paths_,
                state_cache_path_,
                {},
                SampleLabelingStateCacheLoadPolicy::
                    AllowPersistentOutputsWithoutResultHydration);
        if (latest.issue_kind !=
            SampleLabelingStateCacheLoadIssueKind::None) {
            return RejectEditTargetChanged();
        }
        const auto latest_source = latest.cache.sources.find(
            std::string(source_identity));
        if (latest_source == latest.cache.sources.end()) {
            state->tasks.erase(match);
            BumpActiveSourceTasksGeneration();
            Touch();
            SampleLabelingOperationResult result =
                RejectEditTargetChanged();
            result.task_projection_changed = true;
            return result;
        }
        if (latest_source->second.sample_count !=
            state->sample_count) {
            return RejectEditTargetChanged();
        }
        const auto latest_task = std::find_if(
            latest_source->second.tasks.begin(),
            latest_source->second.tasks.end(),
            [&owned_task_id](const SampleLabelingTask& task) {
                return task.task_id == owned_task_id;
            });
        if (latest_task ==
            latest_source->second.tasks.end()) {
            state->tasks.erase(match);
            BumpActiveSourceTasksGeneration();
            Touch();
            SampleLabelingOperationResult result =
                RejectEditTargetChanged();
            result.task_projection_changed = true;
            return result;
        }
        if (OutputEditLeaseKeys(*latest_task) !=
                leases.output_artifact_keys ||
            !CanDeleteTask(*latest_task) ||
            !latest_task->persistence.output_path ||
            ProbePath(*latest_task->persistence.output_path) !=
                PathProbeResult::Missing) {
            return RejectEditTargetChanged();
        }
        *match = *latest_task;
    }

    if (!match->persistence.output_path ||
        ProbePath(*match->persistence.output_path) !=
            PathProbeResult::Missing) {
        return RejectEditTargetChanged();
    }

    const TaskProjectionEffect projection_effect =
        match->persistence.output_path
        ? TaskProjectionEffect::Changed
        : TaskProjectionEffect::Unchanged;
    state->tasks.erase(match);
    ClearRecoveryTaskTrust(
        source_identity,
        owned_task_id);
    lease_unavailable_task_targets_.erase(
        leases.task_identity_key);
    MarkTaskTombstone(
        source_identity,
        *state,
        owned_task_id);
    SampleLabelingOperationResult result =
        CompleteMutation(
            nullptr,
            PersistencePolicy::FlushStateSave,
            projection_effect);
    if (!result.state_saved) {
        deferred_task_leases_.push_back(
            std::move(leases));
    }
    return result;
}

SampleLabelingOperationResult
SampleLabelingController::ActivateTaskWithExpectation(
    std::string_view task_id,
    TaskActivationExpectation expectation,
    bool allow_pending_task_recovery)
{
    SourceState* state = ActiveSource();
    if (state == nullptr || task_id.empty()) {
        return RejectOperation();
    }

    const auto match = std::find_if(state->tasks.begin(), state->tasks.end(), [task_id](const auto& task) {
        return task.task_id == task_id;
    });
    if (match == state->tasks.end()) {
        return RejectOperation();
    }
    if (expectation == TaskActivationExpectation::TemporaryTask &&
        match->persistence.output_path) {
        return RejectEditTargetChanged();
    }
    if (state->active_task_id && *state->active_task_id == match->task_id) {
        SampleLabelingOperationResult result = RejectOperation();
        result.accepted = true;
        return result;
    }

    const SampleLabelingTask* active_task =
        ActiveTask();
    if (active_task != nullptr &&
        !CanDeleteTask(*active_task)) {
        return RejectOperation();
    }
    TaskActivationPreparation preparation =
        PrepareTaskActivation(
            *active_source_identity_,
            *match,
            state->sample_count,
            true,
            true,
            allow_pending_task_recovery);
    if (preparation.lease_status !=
        ExclusiveFileLeaseAcquireStatus::Acquired) {
        if (active_source_identity_ &&
            preparation.lease_status ==
                ExclusiveFileLeaseAcquireStatus::Unavailable) {
            NoteTaskLeaseUnavailable(
                *active_source_identity_,
                match->task_id);
        }
        return RejectLeaseAcquireStatus(
            preparation.lease_status);
    }
    if (preparation.refresh_status ==
        TaskRefreshStatus::Missing) {
        ClearRecoveryTaskTrust(
            *active_source_identity_,
            task_id);
        lease_unavailable_task_targets_.erase(
            TaskIdentityEditLeaseKey(
                *active_source_identity_,
                task_id));
        const bool cleared_selection =
            state->active_task_id &&
            *state->active_task_id == task_id;
        state->tasks.erase(match);
        if (cleared_selection) {
            state->active_task_id.reset();
        }
        BumpActiveSourceTasksGeneration();
        Touch();
        SampleLabelingOperationResult result =
            RejectEditTargetChanged();
        result.task_projection_changed = true;
        return result;
    }
    if (preparation.refresh_status !=
            TaskRefreshStatus::Ready ||
        !preparation.task) {
        return RejectEditTargetChanged();
    }

    const bool task_projection_changed =
        !SameTaskProjection(
            *match,
            *preparation.task);
    *match = std::move(*preparation.task);
    ClearRecoveryTaskTrust(
        *active_source_identity_,
        match->task_id);
    if (expectation ==
            TaskActivationExpectation::TemporaryTask &&
        match->persistence.output_path) {
        if (task_projection_changed) {
            BumpActiveSourceTasksGeneration();
        }
        Touch();
        SampleLabelingOperationResult result =
            RejectEditTargetChanged();
        result.task_projection_changed =
            task_projection_changed;
        return result;
    }
    state->active_task_id = match->task_id;
    MarkActiveTaskSelection(
        *active_source_identity_,
        *state);
    SampleLabelingOperationResult result = CompleteMutation(
        nullptr,
        PersistencePolicy::FlushStateSave,
        task_projection_changed
            ? TaskProjectionEffect::Changed
            : TaskProjectionEffect::Unchanged);
    if (preparation.reuses_active_temporary_slot) {
        TransferActiveTemporarySlotLease(
            preparation.leases);
    }
    TransitionActiveTaskLeases(
        std::move(preparation.leases),
        std::move(preparation.asdf_snapshot),
        result.state_saved);
    return result;
}

SampleLabelingOperationResult SampleLabelingController::UpsertActiveLabel(SampleLabelDefinition label)
{
    SampleLabelingTask* task = ActiveTask();
    if (task != nullptr && task->persistence.output_format ==
            SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar) {
        return RejectOperation();
    }
    if (task == nullptr || !UpsertSampleLabel(task->label_set, std::move(label))) {
        SampleLabelingOperationResult result = RejectOperation();
        result.accepted = task != nullptr;
        return result;
    }
    MarkCanonicalSemanticMutation(*task, task_clock_());
    return CompleteMutation(
        task,
        PersistencePolicy::PersistOutputIfSelected,
        task->persistence.output_path
            ? TaskProjectionEffect::Changed
            : TaskProjectionEffect::Unchanged);
}

SampleLabelingOperationResult SampleLabelingController::UpdateActiveLabel(
    int original_code,
    SampleLabelDefinition label,
    bool allow_used_code_change)
{
    SampleLabelingTask* task = ActiveTask();
    if (task != nullptr && task->persistence.output_format ==
            SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar) {
        return RejectOperation();
    }
    if (task == nullptr ||
        !UpdateSampleLabel(*task, original_code, std::move(label), allow_used_code_change)) {
        SampleLabelingOperationResult result = RejectOperation();
        result.accepted = task != nullptr;
        return result;
    }
    MarkCanonicalSemanticMutation(*task, task_clock_());
    return CompleteMutation(
        task,
        PersistencePolicy::PersistOutputIfSelected,
        task->persistence.output_path
            ? TaskProjectionEffect::Changed
            : TaskProjectionEffect::Unchanged);
}

SampleLabelingOperationResult SampleLabelingController::RemoveActiveLabel(int code)
{
    SampleLabelingTask* task = ActiveTask();
    if (task != nullptr && task->persistence.output_format ==
            SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar) {
        return RejectOperation();
    }
    if (task == nullptr || !RemoveSampleLabel(*task, code)) {
        SampleLabelingOperationResult result = RejectOperation();
        result.accepted = task != nullptr;
        return result;
    }
    MarkCanonicalSemanticMutation(*task, task_clock_());
    return CompleteMutation(
        task,
        PersistencePolicy::PersistOutputIfSelected,
        task->persistence.output_path
            ? TaskProjectionEffect::Changed
            : TaskProjectionEffect::Unchanged);
}

SampleLabelingOperationResult SampleLabelingController::RenameActiveTask(
    std::string_view expected_task_id,
    std::string task_name)
{
    SampleLabelingTask* task = ActiveTask();
    if (task != nullptr && task->persistence.output_format ==
            SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar) {
        return RejectOperation();
    }
    if (task == nullptr || task->task_id != expected_task_id) {
        return RejectEditTargetChanged();
    }

    const bool valid_name =
        IsValidUtf8WithNonWhitespace(task_name);
    if (!valid_name ||
        task->task_name == task_name) {
        SampleLabelingOperationResult result = RejectOperation();
        result.accepted = valid_name;
        return result;
    }

    task->task_name = std::move(task_name);
    MarkCanonicalSemanticMutation(*task, task_clock_());
    return CompleteMutation(
        task,
        PersistencePolicy::PersistOutputIfSelected,
        task->persistence.output_path
            ? TaskProjectionEffect::Changed
            : TaskProjectionEffect::Unchanged);
}

SampleLabelingOperationResult SampleLabelingController::SetActiveAutoAdvance(bool enabled)
{
    SampleLabelingTask* task = ActiveTask();
    if (task == nullptr || task->session.auto_advance == enabled) {
        SampleLabelingOperationResult result = RejectOperation();
        result.accepted = task != nullptr;
        return result;
    }
    task->session.auto_advance = enabled;
    return CompleteMutation(
        task,
        PersistencePolicy::FlushStateSave,
        TaskProjectionEffect::Unchanged);
}

SampleLabelingOperationResult SampleLabelingController::SetActiveSkipLabeledOnAdvance(bool enabled)
{
    SampleLabelingTask* task = ActiveTask();
    if (task == nullptr || task->session.skip_labeled_on_advance == enabled) {
        SampleLabelingOperationResult result = RejectOperation();
        result.accepted = task != nullptr;
        return result;
    }
    task->session.skip_labeled_on_advance = enabled;
    return CompleteMutation(
        task,
        PersistencePolicy::FlushStateSave,
        TaskProjectionEffect::Unchanged);
}

SampleLabelingOperationResult SampleLabelingController::SaveActiveTemporaryTaskToOutput(
    std::filesystem::path output_path)
{
    if (!runtime_paths_.application_data_root.empty() &&
        CheckUserFilePath(output_path, runtime_paths_) != UserFilePathStatus::Allowed) {
        return {.revision = revision_, .issue = SampleLabelingOperationResult::Issue::UserFilePathRejected};
    }

    SampleLabelingTask* task = ActiveTask();
    SourceState* state = ActiveSource();
    if (task == nullptr || state == nullptr || task->persistence.output_path ||
        output_path.empty() ||
        !IsCanonicalAsdfOutputPath(output_path)) {
        return RejectOperation();
    }

    const auto conflict = std::find_if(state->tasks.begin(), state->tasks.end(), [&](const auto& existing) {
        return existing.task_id != task->task_id && existing.persistence.output_path &&
               OutputPathMatches(*existing.persistence.output_path, output_path);
    });
    if (conflict != state->tasks.end()) {
        task->persistence.save_state.message_kind =
            SampleLabelSaveMessageKind::OutputPathAlreadyUsed;
        task->persistence.save_state.message.clear();
        SampleLabelingOperationResult result =
            CompleteMutation(
                task,
                PersistencePolicy::ScheduleStateSave,
                TaskProjectionEffect::Unchanged);
        result.accepted = false;
        return result;
    }

    SampleLabelingTask candidate = *task;
    candidate.persistence.output_path = std::move(output_path);
    candidate.persistence.output_format =
        SampleLabelingOutputArtifactFormat::CanonicalAsdf;

    for (std::size_t index = 0;
         index < candidate.values.SampleCount();
         ++index) {
        if (candidate.values.Complete()[index] !=
            kUnlabeledSampleLabelCode) {
            candidate.persistence.pending_sample_indices.insert(index);
        }
    }
    MarkSampleLabelTaskMetadataPending(candidate);
    TaskEditLeaseSet candidate_output_lease;
    ExclusiveFileLeaseAcquireResult candidate_lease =
        TryAttachOutputLease(
            candidate_output_lease,
            candidate);
    if (candidate_lease.status !=
        ExclusiveFileLeaseAcquireStatus::Acquired) {
        return RejectLeaseAcquireStatus(
            candidate_lease.status);
    }
    const std::optional<bool> latest_output_conflict =
        LatestCacheHasOutputConflict(candidate);
    if (!latest_output_conflict) {
        return RejectEditTargetChanged();
    }
    if (*latest_output_conflict) {
        task->persistence.save_state.message_kind =
            SampleLabelSaveMessageKind::OutputPathAlreadyUsed;
        task->persistence.save_state.message.clear();
        SampleLabelingOperationResult result =
            CompleteMutation(
                task,
                PersistencePolicy::ScheduleStateSave,
                TaskProjectionEffect::Unchanged);
        result.accepted = false;
        return result;
    }
    std::optional<SampleLabelingAsdfOpenSnapshot>
        candidate_asdf_snapshot;
    TaskOutputPersistenceAttempt persistence_attempt = PublishCanonicalTaskCreation(
        candidate,
        candidate_output_lease,
        &candidate_asdf_snapshot);
    const SampleLabelOutputPublicationResult& publication =
        persistence_attempt.publication;

    const auto adopt_formal_output_lease = [this,
                                            &candidate_output_lease]() {
        active_task_leases_.temporary_slot.Reset();
        active_task_leases_.temporary_slot_key.clear();
        active_task_leases_.output_artifacts =
            std::move(
                candidate_output_lease
                    .output_artifacts);
        active_task_leases_.output_artifact_keys =
            std::move(
                candidate_output_lease
                    .output_artifact_keys);
    };
    const auto mark_persistence_failure =
        [&candidate, &publication](SampleLabelingTask& destination) {
            destination.persistence.save_state.kind =
                SampleLabelSaveStateKind::Failed;
            destination.persistence.save_state.pending_count =
                candidate.persistence.save_state.pending_count;
            if (candidate.persistence.save_state.message_kind !=
                SampleLabelSaveMessageKind::None) {
                destination.persistence.save_state.message_kind =
                    candidate.persistence.save_state.message_kind;
            } else {
                destination.persistence.save_state.message_kind =
                    publication.message.empty()
                    ? SampleLabelSaveMessageKind::OutputSaveFailed
                    : SampleLabelSaveMessageKind::SystemDetail;
            }
            if (destination.persistence.save_state.message_kind ==
                SampleLabelSaveMessageKind::SystemDetail) {
                destination.persistence.save_state.message =
                    !candidate.persistence.save_state.message.empty()
                    ? candidate.persistence.save_state.message
                    : publication.message;
            } else {
                destination.persistence.save_state.message.clear();
            }
        };

    const bool formal_owner_adopted =
        persistence_attempt.publication.published &&
        persistence_attempt.lease_status ==
            ExclusiveFileLeaseAcquireStatus::Acquired &&
        candidate_asdf_snapshot.has_value();
    if (formal_owner_adopted) {
        *task = std::move(candidate);
        adopt_formal_output_lease();
        ReplaceActiveAsdfSnapshot(
            std::move(candidate_asdf_snapshot));
    } else {
        mark_persistence_failure(*task);
    }

    if (formal_owner_adopted) {
        BumpActiveSourceTasksGeneration();
    }
    Touch();
    MarkTaskUpsert(
        *active_source_identity_,
        *state,
        *task);
    QueueStateSave();
    SampleLabelingOperationResult result;
    result.accepted = true;
    result.changed = true;
    result.output_save_attempted =
        persistence_attempt.publication.attempted;
    result.output_saved = formal_owner_adopted;
    result.output_retry_scheduled = false;
    if (persistence_attempt.lease_status !=
        ExclusiveFileLeaseAcquireStatus::Acquired) {
        result.issue =
            persistence_attempt.lease_status ==
                ExclusiveFileLeaseAcquireStatus::Unavailable
            ? SampleLabelingOperationResult::Issue::
                  EditLeaseUnavailable
            : SampleLabelingOperationResult::Issue::
                  EditLeaseFailed;
    }
    result.state_save_scheduled = true;
    result.state_save_attempted = true;
    result.state_saved = FlushStateCache();
    result.revision = revision_;
    return result;
}

SampleLabelingOperationResult
SampleLabelingController::MigrateActiveLegacyTaskToCanonicalAsdf(
    std::filesystem::path output_path)
{
    if (!runtime_paths_.application_data_root.empty() &&
        CheckUserFilePath(output_path, runtime_paths_) != UserFilePathStatus::Allowed) {
        return {.revision = revision_, .issue = SampleLabelingOperationResult::Issue::UserFilePathRejected};
    }

    SampleLabelingTask* task = ActiveTask();
    SourceState* state = ActiveSource();
    if (task == nullptr || state == nullptr ||
        !active_source_identity_ ||
        !active_source_descriptor_ ||
        !task->persistence.output_path ||
        task->persistence.output_format !=
            SampleLabelingOutputArtifactFormat::
                LegacyNpyWithSidecar ||
        !task->values.IsComplete() ||
        output_path.empty() ||
        !IsCanonicalAsdfOutputPath(output_path) ||
        OutputPathMatches(
            *task->persistence.output_path,
            output_path) ||
        active_source_descriptor_->base_identity !=
            *active_source_identity_ ||
        active_source_descriptor_->sample_count !=
            task->values.SampleCount() ||
        !ActiveTaskLeaseMatches(
            *active_source_identity_,
            *task)) {
        return RejectOperation();
    }

    const auto local_conflict = std::find_if(
        state->tasks.begin(),
        state->tasks.end(),
        [task, &output_path](
            const SampleLabelingTask& existing) {
            return existing.task_id != task->task_id &&
                existing.persistence.output_path &&
                OutputPathMatches(
                    *existing.persistence.output_path,
                    output_path);
        });
    if (local_conflict != state->tasks.end()) {
        return RejectOutputPathAlreadyUsed();
    }

    SampleLabelingTask candidate = *task;
    candidate.persistence.output_path = std::move(output_path);
    candidate.persistence.output_format =
        SampleLabelingOutputArtifactFormat::CanonicalAsdf;


    TaskEditLeaseSet candidate_output_leases;
    ExclusiveFileLeaseAcquireResult candidate_lease =
        TryAttachOutputLease(
            candidate_output_leases,
            candidate);
    if (candidate_lease.status !=
        ExclusiveFileLeaseAcquireStatus::Acquired) {
        return RejectLeaseAcquireStatus(
            candidate_lease.status);
    }
    const std::optional<bool> latest_output_conflict =
        LatestCacheHasOutputConflict(candidate);
    if (!latest_output_conflict) {
        return RejectEditTargetChanged();
    }
    if (*latest_output_conflict) {
        return RejectOutputPathAlreadyUsed();
    }

    std::optional<SampleLabelingAsdfOpenSnapshot>
        candidate_asdf_snapshot;
    TaskOutputPersistenceAttempt persistence_attempt =
        PublishCanonicalTaskCreation(
            candidate,
            candidate_output_leases,
            &candidate_asdf_snapshot);
    const bool canonical_generation_ready =
        persistence_attempt.publication.published &&
        persistence_attempt.lease_status ==
            ExclusiveFileLeaseAcquireStatus::Acquired &&
        candidate_asdf_snapshot.has_value();
    if (!canonical_generation_ready) {
        SampleLabelingOperationResult failed;
        failed.accepted = true;
        failed.output_save_attempted =
            persistence_attempt.publication.attempted;
        failed.output_saved = false;
        failed.state_save_attempted =
            !state_cache_path_.empty();
        failed.state_saved = false;
        failed.issue =
            SampleLabelingOperationResult::Issue::
                OutputMigrationPublicationFailed;
        failed.diagnostic =
            persistence_attempt.publication.message.empty()
            ? "could not publish and open the migrated canonical ASDF owner"
            : persistence_attempt.publication.message;
        if (persistence_attempt.lease_status !=
            ExclusiveFileLeaseAcquireStatus::Acquired) {
            failed.issue =
                persistence_attempt.lease_status ==
                    ExclusiveFileLeaseAcquireStatus::
                        Unavailable
                ? SampleLabelingOperationResult::Issue::
                      EditLeaseUnavailable
                : SampleLabelingOperationResult::Issue::
                      EditLeaseFailed;
        }
        failed.revision = revision_;
        return failed;
    }

    // The owner switch itself is durable before the controller releases any
    // legacy artifact lease. If this commit fails, the newly written ASDF is
    // left unowned and the unchanged NPY+sidecar pair remains authoritative.
    const auto registration = state_cache_path_.empty()
        ? SampleLabelingStateCacheSaveResult{}
        : CommitTaskRegistration(
            *active_source_identity_, *state, candidate, false);
    const bool canonical_owner_saved =
        state_cache_path_.empty() ||
        registration.RegistrationSucceeded();
    if (!canonical_owner_saved) {
        SampleLabelingOperationResult failed;
        failed.accepted = true;
        failed.output_save_attempted = true;
        failed.output_saved = false;
        failed.state_save_scheduled = true;
        failed.state_save_attempted = true;
        failed.issue =
            SampleLabelingOperationResult::Issue::
                OutputMigrationOwnerSwitchFailed;
        failed.diagnostic = registration.diagnostic_detail.empty()
            ? "could not register the canonical owner after ASDF migration"
            : registration.diagnostic_detail;
        failed.revision = revision_;
        return failed;
    }

    std::vector<TaskEditLeaseSet::Component>
        legacy_output_leases = std::move(
            active_task_leases_.output_artifacts);
    active_task_leases_.output_artifact_keys.clear();
    *task = std::move(candidate);
    active_task_leases_.output_artifacts =
        std::move(
            candidate_output_leases.output_artifacts);
    active_task_leases_.output_artifact_keys =
        std::move(
            candidate_output_leases.output_artifact_keys);
    ReplaceActiveAsdfSnapshot(
        std::move(candidate_asdf_snapshot));
    // Both the in-memory and durable owner now point at the opened ASDF
    // generation. Releasing these components cannot expose a half-switched
    // task to another instance.
    legacy_output_leases.clear();

    BumpActiveSourceTasksGeneration();
    Touch();
    MarkTaskUpsert(
        *active_source_identity_,
        *state,
        *task);
    QueueStateSave();

    SampleLabelingOperationResult result;
    result.accepted = true;
    result.changed = true;
    result.task_projection_changed = true;
    result.output_save_attempted = true;
    result.output_saved = true;
    result.state_save_scheduled = true;
    result.state_save_attempted = true;
    result.state_saved = FlushStateCache();
    result.revision = revision_;
    return result;
}

SampleLabelingOperationResult
SampleLabelingController::ExportActiveLabelValuesToNpy(
    const std::filesystem::path& output_path) const
{
    return ExportActiveLabels(
        output_path,
        SampleLabelExportFormat::Npy);
}

SampleLabelingOperationResult
SampleLabelingController::ExportActiveLabels(
    const std::filesystem::path& output_path,
    SampleLabelExportFormat format) const
{
    if (!runtime_paths_.application_data_root.empty() &&
        CheckUserFilePath(output_path, runtime_paths_) != UserFilePathStatus::Allowed) {
        return {.revision = revision_, .issue = SampleLabelingOperationResult::Issue::UserFilePathRejected};
    }

    const SampleLabelingTask* task = ActiveTask();
    if (task == nullptr || !task->values.IsComplete() ||
        !active_source_descriptor_) {
        return RejectOperation();
    }
    if (!IsSampleLabelExportPath(
            output_path,
            format)) {
        SampleLabelingOperationResult rejected = RejectOperation();
        rejected.issue =
            SampleLabelingOperationResult::Issue::
                LabelValuesExportInvalidPath;
        return rejected;
    }

    std::string snapshot_error;
    const std::optional<SampleLabelExportSnapshot> snapshot =
        BuildSampleLabelExportSnapshot(
            format,
            *task,
            *active_source_descriptor_,
            &snapshot_error);
    if (!snapshot) {
        SampleLabelingOperationResult rejected = RejectOperation();
        rejected.issue =
            SampleLabelingOperationResult::Issue::
                LabelValuesExportFailed;
        rejected.diagnostic = std::move(snapshot_error);
        return rejected;
    }

    const auto reject_protected = [this]() {
        SampleLabelingOperationResult rejected =
            RejectOperation();
        rejected.issue =
            SampleLabelingOperationResult::Issue::
                LabelValuesExportTargetProtected;
        return rejected;
    };
    const auto reject_guard_failure = [this](
                                                std::string diagnostic) {
        SampleLabelingOperationResult rejected =
            RejectOperation();
        rejected.issue =
            SampleLabelingOperationResult::Issue::
                LabelValuesExportFailed;
        rejected.diagnostic = std::move(diagnostic);
        return rejected;
    };

    const std::vector<std::string> export_artifact_keys =
        format == SampleLabelExportFormat::Npy
        ? NpyExportEditLeaseKeys(output_path)
        : SingleFileExportEditLeaseKeys(output_path);
    TaskEditLeaseSet export_artifact_guard;
    ExclusiveFileLeaseAcquireResult guard =
        TryAttachArtifactLeases(
            export_artifact_guard,
            export_artifact_keys);
    if (guard.status ==
        ExclusiveFileLeaseAcquireStatus::Unavailable) {
        return reject_protected();
    }
    if (guard.status !=
        ExclusiveFileLeaseAcquireStatus::Acquired) {
        return reject_guard_failure(
            guard.error.empty()
            ? "could not guard the label export target"
            : std::move(guard.error));
    }

    // The guard closes the race with another instance creating or activating
    // an owner while both the live local projection and latest durable cache
    // are checked. It is released when this one call returns and is never
    // adopted as task state.
    if (HasManagedOutputArtifactConflict(
            sources_,
            export_artifact_keys)) {
        return reject_protected();
    }
    if (!state_cache_path_.empty()) {
        const SampleLabelingStateCacheLoadResult latest =
            LoadSampleLabelingStateCache(
                runtime_paths_,
                state_cache_path_,
                {},
                SampleLabelingStateCacheLoadPolicy::
                    AllowPersistentOutputsWithoutResultHydration);
        if (latest.issue_kind !=
            SampleLabelingStateCacheLoadIssueKind::None) {
            std::string diagnostic = latest.warning.empty()
                ? "could not verify the latest labeling owners before label export"
                : latest.warning;
            if (!latest.diagnostic_detail.empty()) {
                diagnostic += ": " +
                    latest.diagnostic_detail;
            }
            return reject_guard_failure(
                std::move(diagnostic));
        }
        if (HasManagedOutputArtifactConflict(
                latest.cache.sources,
                export_artifact_keys)) {
            return reject_protected();
        }
    }
    if (format == SampleLabelExportFormat::Npy) {
        std::error_code sidecar_probe_error;
        const bool legacy_sidecar_exists =
            std::filesystem::exists(
                SampleAnnotationIoAdapter::
                    MetadataPathForResult(output_path),
                sidecar_probe_error);
        if (sidecar_probe_error) {
            return reject_guard_failure(
                "could not verify the adjacent label metadata sidecar: " +
                sidecar_probe_error.message());
        }
        if (legacy_sidecar_exists) {
            return reject_protected();
        }
    }

    SampleLabelingOperationResult result;
    result.accepted = true;
    result.export_attempted = true;
    result.revision = revision_;
    std::string export_error;
    result.exported = ExportSampleLabelSnapshot(
        output_path,
        *snapshot,
        &export_error);
    if (!result.exported) {
        std::error_code sidecar_error;
        const bool failure_sidecar_exists =
            format == SampleLabelExportFormat::Npy &&
            std::filesystem::exists(
                SampleAnnotationIoAdapter::
                    MetadataPathForResult(output_path),
                sidecar_error) &&
            !sidecar_error;
        result.issue = failure_sidecar_exists
            ? SampleLabelingOperationResult::Issue::
                  LabelValuesExportTargetProtected
            : SampleLabelingOperationResult::Issue::
                  LabelValuesExportFailed;
        if (failure_sidecar_exists) {
            result.accepted = false;
            result.export_attempted = false;
        }
        result.diagnostic = std::move(export_error);
    }
    return result;
}

bool SampleLabelingController::CanDeactivateActiveTask() const
{
    const SampleLabelingTask* task = ActiveTask();
    if (task == nullptr) {
        return false;
    }
    return CanDeleteTask(*task);
}

bool SampleLabelingController::PrepareForInteractiveClose()
{
    bool has_draft = false;
    for (const auto& [identity, source] : sources_) {
        for (const auto& task : source.tasks) {
            if (task.persistence.output_path && HasPendingOutputSave(task)) return false;
            has_draft = has_draft || !task.persistence.output_path;
        }
    }
    // Closing retains paused drafts via their best-effort checkpoint. A known
    // failure must not silently abandon the only current in-memory contents.
    return !has_draft || FlushStateCache();
}

bool SampleLabelingController::CanDeleteActiveTask() const
{
    return CanDeactivateActiveTask();
}

SampleLabelingOperationResult SampleLabelingController::DeactivateActiveTask()
{
    SourceState* state = ActiveSource();
    if (state == nullptr || !state->active_task_id || !CanDeactivateActiveTask()) {
        return RejectOperation();
    }
    state->active_task_id.reset();
    MarkActiveTaskSelection(
        *active_source_identity_,
        *state);
    SampleLabelingOperationResult result = CompleteMutation(
        nullptr,
        PersistencePolicy::FlushStateSave,
        TaskProjectionEffect::Unchanged);
    if (result.state_saved) {
        ReleaseActiveTaskLease();
    } else {
        DeferActiveTaskLeases();
    }
    return result;
}

SampleLabelingOperationResult SampleLabelingController::DeleteActiveTask()
{
    SourceState* state = ActiveSource();
    if (state == nullptr || !state->active_task_id || !CanDeleteActiveTask()) {
        return RejectOperation();
    }

    const SampleLabelingTask* active_task = ActiveTask();
    const TaskProjectionEffect projection_effect =
        active_task != nullptr && active_task->persistence.output_path
        ? TaskProjectionEffect::Changed
        : TaskProjectionEffect::Unchanged;
    const std::string task_id = *state->active_task_id;
    state->tasks.erase(
        std::remove_if(state->tasks.begin(), state->tasks.end(), [&task_id](const auto& task) {
            return task.task_id == task_id;
        }),
        state->tasks.end());
    state->active_task_id.reset();
    MarkTaskTombstone(
        *active_source_identity_,
        *state,
        task_id);
    MarkActiveTaskSelection(
        *active_source_identity_,
        *state);
    SampleLabelingOperationResult result = CompleteMutation(
        nullptr,
        PersistencePolicy::FlushStateSave,
        projection_effect);
    if (result.state_saved) {
        ReleaseActiveTaskLease();
    } else {
        DeferActiveTaskLeases();
    }
    return result;
}

SampleLabelingOperationResult SampleLabelingController::RememberActivePosition(std::size_t sample_index)
{
    SampleLabelingTask* task = ActiveTask();
    if (task == nullptr || sample_index >= task->values.SampleCount()) {
        return RejectOperation();
    }
    if (task->session.remembered_position && *task->session.remembered_position == sample_index) {
        SampleLabelingOperationResult result = RejectOperation();
        result.accepted = true;
        return result;
    }
    task->session.remembered_position = sample_index;
    return CompleteMutation(
        task,
        PersistencePolicy::ScheduleStateSave,
        TaskProjectionEffect::Unchanged);
}

SampleLabelingOperationResult SampleLabelingController::RejectOperation() const
{
    return SampleLabelingOperationResult{.revision = revision_};
}

SampleLabelingOperationResult
SampleLabelingController::RejectEditLeaseUnavailable() const
{
    SampleLabelingOperationResult result =
        RejectOperation();
    result.issue =
        SampleLabelingOperationResult::Issue::
            EditLeaseUnavailable;
    return result;
}

SampleLabelingOperationResult
SampleLabelingController::RejectEditLeaseFailed() const
{
    SampleLabelingOperationResult result =
        RejectOperation();
    result.issue =
        SampleLabelingOperationResult::Issue::
            EditLeaseFailed;
    return result;
}

SampleLabelingOperationResult
SampleLabelingController::RejectEditTargetChanged() const
{
    SampleLabelingOperationResult result =
        RejectOperation();
    result.issue =
        SampleLabelingOperationResult::Issue::
            EditTargetChanged;
    return result;
}

SampleLabelingOperationResult
SampleLabelingController::RejectOutputPathAlreadyUsed() const
{
    SampleLabelingOperationResult result =
        RejectOperation();
    result.issue =
        SampleLabelingOperationResult::Issue::
            OutputPathAlreadyUsed;
    return result;
}

SampleLabelingOperationResult
SampleLabelingController::RejectLeaseAcquireStatus(
    ExclusiveFileLeaseAcquireStatus status) const
{
    return status ==
            ExclusiveFileLeaseAcquireStatus::Unavailable
        ? RejectEditLeaseUnavailable()
        : RejectEditLeaseFailed();
}

void SampleLabelingController::NoteTaskLeaseUnavailable(
    std::string_view source_identity,
    std::string_view task_id)
{
    lease_unavailable_source_identities_.insert(
        std::string(source_identity));
    const std::string task_identity_key =
        TaskIdentityEditLeaseKey(
            source_identity,
            task_id);
    if (!task_identity_key.empty() &&
        lease_unavailable_task_targets_.insert(
            task_identity_key)
            .second) {
        Touch();
    }
}

SampleLabelingOperationResult SampleLabelingController::CompleteMutation(
    SampleLabelingTask* task,
    PersistencePolicy persistence,
    TaskProjectionEffect projection_effect)
{
    if (projection_effect == TaskProjectionEffect::Changed) {
        BumpActiveSourceTasksGeneration();
    }
    Touch();
    QueueStateSave();

    SampleLabelingOperationResult result;
    result.accepted = true;
    result.changed = true;
    result.task_projection_changed =
        projection_effect ==
        TaskProjectionEffect::Changed;
    result.state_save_scheduled = true;
    const auto mark_task_upsert = [this, task]() {
        if (task == nullptr || !active_source_identity_) {
            return;
        }
        const SourceState* state = ActiveSource();
        if (state != nullptr) {
            MarkTaskUpsert(
                *active_source_identity_,
                *state,
                *task);
        }
    };
    const bool requests_output_persistence =
        persistence == PersistencePolicy::
            PersistOutputIfSelected ||
        persistence == PersistencePolicy::
            PersistOutputIfSelectedInteractive;
    if (requests_output_persistence &&
        task != nullptr && task->persistence.output_path) {
        const bool wait_for_commit_lock =
            persistence == PersistencePolicy::PersistOutputIfSelected;
        TaskOutputPersistenceAttempt attempt =
            PersistTaskOutput(
                *task,
                ActiveSource(),
                active_task_leases_,
                &active_asdf_snapshot_);
        result.output_save_attempted =
            attempt.publication.attempted;
        result.output_saved =
            attempt.publication.published;
        result.output_retry_scheduled =
            attempt.publication.retryable &&
            ShouldRetryOutputSave(*task);
        if (!attempt.publication.attempted) {
            result.revision = revision_;
            return result;
        }
        if (attempt.lease_status !=
            ExclusiveFileLeaseAcquireStatus::Acquired) {
            SourceState* state = ActiveSource();
            if (state != nullptr && state->active_task_id &&
                *state->active_task_id == task->task_id) {
                state->active_task_id.reset();
                MarkActiveTaskSelection(
                    *active_source_identity_,
                    *state);
            }
            DeferActiveTaskLeases();
            result.issue =
                attempt.lease_status ==
                    ExclusiveFileLeaseAcquireStatus::Unavailable
                ? SampleLabelingOperationResult::Issue::
                      EditLeaseUnavailable
                : SampleLabelingOperationResult::Issue::
                      EditLeaseFailed;
        }
        mark_task_upsert();
        QueueStateSave();
        result.state_save_attempted = true;
        result.state_saved = TrySaveStateCache(wait_for_commit_lock);
    } else if (persistence == PersistencePolicy::FlushStateSave) {
        mark_task_upsert();
        result.state_save_attempted = true;
        result.state_saved = FlushStateCache();
    } else {
        mark_task_upsert();
    }
    result.revision = revision_;
    return result;
}

SampleLabelingController::TaskOutputPersistenceAttempt
SampleLabelingController::PersistTaskOutput(
    SampleLabelingTask& task,
    const SourceState* source_state,
    TaskEditLeaseSet& leases,
    std::optional<SampleLabelingAsdfOpenSnapshot>*
        asdf_snapshot)
{
    TaskOutputPersistenceAttempt attempt;
    switch (task.persistence.output_format) {
    case SampleLabelingOutputArtifactFormat::
        LegacyNpyWithSidecar:
        // Legacy state is a read-only migration source, never a save target.
        break;
    case SampleLabelingOutputArtifactFormat::CanonicalAsdf:
        attempt.publication =
            PersistCanonicalTaskOutput(
                task,
                source_state,
                asdf_snapshot);
        break;
    case SampleLabelingOutputArtifactFormat::None:
        attempt.publication.message =
            "labeling task has no formal output owner";
        break;
    }
    if (attempt.publication.artifacts_replaced) {
        const ExclusiveFileLeaseAcquireResult refreshed_lease =
            TryAttachOutputLease(
                leases,
                task,
                false);
        if (refreshed_lease.status !=
            ExclusiveFileLeaseAcquireStatus::Acquired) {
            attempt.lease_status =
                refreshed_lease.status;
            attempt.lease_error =
                refreshed_lease.error;
        }
    }
    if (attempt.publication.retryable &&
        ShouldRetryOutputSave(task)) {
        QueueOutputRetry();
    }
    return attempt;
}

SampleLabelOutputPublicationResult
SampleLabelingController::PersistCanonicalTaskOutput(
    SampleLabelingTask& task,
    const SourceState* source_state,
    std::optional<SampleLabelingAsdfOpenSnapshot>*
        asdf_snapshot)
{
    SampleLabelOutputPublicationResult result;
    if (!task.persistence.metadata_save_pending &&
        task.persistence.pending_sample_indices.empty()) {
        return result;
    }

    result.attempted = true;
    const auto fail = [&task, &result](
                          std::string message,
                          bool retryable = true) {
        result.retryable = retryable;
        result.message = std::move(message);
        MarkSampleLabelTaskSaveFailed(
            task,
            result.message);
    };
    if (!task.persistence.output_path ||
        (!runtime_paths_.application_data_root.empty() &&
         CheckUserFilePath(*task.persistence.output_path, runtime_paths_) != UserFilePathStatus::Allowed) ||
        !task.values.IsComplete() ||
        asdf_snapshot == nullptr ||
        !*asdf_snapshot ||
        !OutputPathMatches(
            *task.persistence.output_path,
            (*asdf_snapshot)->path()) ||
        (*asdf_snapshot)->document().labeling.id !=
            task.task_id ||
        (*asdf_snapshot)->document().annotation.values.size() !=
            task.values.SampleCount() ||
        (source_state != nullptr &&
         source_state->sample_count != task.values.SampleCount())) {
        fail(
            "canonical ASDF publication does not have a matching durable owner generation");
        return result;
    }

    SampleLabelingAsdfOpenSnapshot& snapshot =
        **asdf_snapshot;
    if (!active_source_descriptor_ ||
        !active_source_identity_ ||
        active_source_descriptor_->base_identity !=
            *active_source_identity_ ||
        active_source_descriptor_->base_identity !=
            snapshot.document().source.base_identity ||
        active_source_descriptor_->sample_count !=
            task.values.SampleCount()) {
        fail(
            "canonical ASDF publication does not have a matching source generation");
        return result;
    }

    std::string replacement_error;
    std::optional<SampleLabelingDocument> replacement =
        BuildCanonicalDocumentReplacement(
            task.Content().value(),
            snapshot,
            &replacement_error);
    if (!replacement) {
        fail(
            replacement_error.empty()
                ? "could not build the canonical ASDF replacement generation"
                : std::move(replacement_error),
            false);
        return result;
    }

    if (task.persistence.metadata_save_pending) {
        SampleLabelingAsdfStoreGenerationWriteResult generation =
            canonical_document_publisher_(
                snapshot,
                *replacement,
                *active_source_descriptor_);
        const bool published =
            generation.document_replaced &&
            generation.succeeded();
        result.artifacts_replaced = generation.document_replaced;
        if (generation.document_replaced) {
            *asdf_snapshot = std::move(generation.snapshot);
        }
        if (!published) {
            fail(
                generation.error.message.empty()
                    ? "could not publish and reopen canonical ASDF document"
                    : std::move(generation.error.message));
            return result;
        }
    } else {
        SampleLabelingAsdfStoreWriteResult write =
            canonical_values_publisher_(
                snapshot,
                *replacement);
        result.artifacts_replaced = write.written;
        if (!write.succeeded()) {
            fail(
                write.error.message.empty()
                    ? "could not publish canonical ASDF values"
                    : std::move(write.error.message));
            return result;
        }
    }

    result.published = true;
    MarkSampleLabelTaskPersisted(
        task,
        SampleLabelSaveStateKind::AutosavedToOutput);
    return result;
}

SampleLabelingController::TaskOutputPersistenceAttempt
SampleLabelingController::PublishCanonicalTaskCreation(
    SampleLabelingTask& task,
    TaskEditLeaseSet& leases,
    std::optional<SampleLabelingAsdfOpenSnapshot>*
        asdf_snapshot)
{
    TaskOutputPersistenceAttempt attempt;
    SampleLabelOutputPublicationResult& publication =
        attempt.publication;
    publication.attempted = true;
    const auto fail = [&task, &publication](std::string message) {
        publication.retryable = true;
        publication.message = std::move(message);
        MarkSampleLabelTaskSaveFailed(
            task,
            publication.message);
    };

    if (!task.persistence.output_path ||
        task.persistence.output_format !=
            SampleLabelingOutputArtifactFormat::CanonicalAsdf ||
        !task.values.IsComplete() ||
        asdf_snapshot == nullptr ||
        !active_source_descriptor_ ||
        !active_source_identity_ ||
        active_source_descriptor_->base_identity !=
            *active_source_identity_ ||
        active_source_descriptor_->sample_count !=
            task.values.SampleCount()) {
        fail(
            "canonical ASDF owner creation does not have a matching source descriptor");
        return attempt;
    }

    SampleLabelingDocument document =
        BuildSampleLabelingDocument(
            *active_source_descriptor_,
            task.Content().value());
    if (!ValidateSampleLabelingDocumentFailFast(document).valid()) {
        fail(
            "labeling task cannot form a valid canonical ASDF document");
        publication.retryable = false;
        return attempt;
    }

    SampleLabelingAsdfStoreGenerationWriteResult generation =
        canonical_creation_publisher_(
            *task.persistence.output_path,
            document,
            *active_source_descriptor_);
    publication.artifacts_replaced =
        generation.document_replaced;
    if (!generation.document_replaced ||
        !generation.succeeded()) {
        fail(
            generation.error.message.empty()
                ? "could not publish and open the canonical ASDF owner"
                : std::move(generation.error.message));
        return attempt;
    }

    *asdf_snapshot = std::move(generation.snapshot);
    publication.published = true;
    publication.retryable = false;

    const ExclusiveFileLeaseAcquireResult refreshed_lease =
        TryAttachOutputLease(
            leases,
            task,
            false);
    if (refreshed_lease.status !=
        ExclusiveFileLeaseAcquireStatus::Acquired) {
        attempt.lease_status = refreshed_lease.status;
        attempt.lease_error = refreshed_lease.error;
        return attempt;
    }
    MarkSampleLabelTaskPersisted(
        task,
        SampleLabelSaveStateKind::AutosavedToOutput);
    return attempt;
}

SampleLabelingStateCacheSaveResult SampleLabelingController::CommitTaskRegistration(
    std::string_view source_identity,
    const SourceState& state,
    const SampleLabelingTask& task,
    bool expected_absent)
{
    SampleLabelingStateCachePatch checkpoint;
    SampleLabelingSourceStatePatch& source_patch =
        checkpoint.sources[std::string(source_identity)];
    source_patch.metadata =
        SampleLabelingSourceMetadataPatch{
            .sample_count = state.sample_count,
            .source_name = state.source_name,
            .source_fingerprint = state.source_fingerprint,
            .context_fingerprint = state.context_fingerprint};
    source_patch.task_upserts.push_back(task);
    if (expected_absent) {
        source_patch.task_ids_expected_absent.insert(
            task.task_id);
    }
    const auto result = CommitSampleLabelingStateCachePatch(
        runtime_paths_,
        state_cache_path_,
        checkpoint,
        nullptr,
        kSynchronousCommitLockWait);
    if (result.RegistrationSucceeded()) {
        ClearRecoveryTaskTrust(
            source_identity,
            task.task_id);
    }
    if (result.CheckpointCleanupPending()) {
        MarkTaskUpsert(source_identity, state, task);
        QueueStateSave();
    }
    return result;
}

void SampleLabelingController::
    BumpActiveSourceTasksGeneration()
{
    ++active_source_tasks_generation_;
}

void SampleLabelingController::Touch()
{
    ++revision_;
}

void SampleLabelingController::ClearRecoveryTaskTrust(
    std::string_view source_identity,
    std::string_view task_id)
{
    if (source_identity.empty() || task_id.empty()) {
        return;
    }
    const auto source =
        recovery_untrusted_task_ids_by_source_.find(
            std::string(source_identity));
    if (source == recovery_untrusted_task_ids_by_source_.end()) {
        return;
    }
    source->second.erase(std::string(task_id));
    if (source->second.empty()) {
        recovery_untrusted_task_ids_by_source_.erase(source);
    }
}

void SampleLabelingController::ExpireTaskLeaseConflictsOnTrustedRefresh(
    std::string_view source_identity)
{
    if (source_identity.empty()) {
        return;
    }
    const std::string key_prefix =
        "task\n" + std::string(source_identity) + "\n";
    // Lease availability is not persisted in the task cache. A trusted
    // refresh is therefore the next observation boundary; an old failed
    // acquisition must not classify the refreshed task as conflicting until
    // a new activation attempt observes another unavailable lease.
    for (auto marker = lease_unavailable_task_targets_.begin();
         marker != lease_unavailable_task_targets_.end();) {
        if (marker->rfind(key_prefix, 0) != 0) {
            ++marker;
            continue;
        }
        marker = lease_unavailable_task_targets_.erase(marker);
    }
}

void SampleLabelingController::ReconcileRecoveryTaskTrust(
    std::string_view source_identity,
    const SourceState& state,
    const std::unordered_set<std::string>& prepared_task_ids,
    const std::unordered_set<std::string>& preserved_local_task_ids,
    std::optional<bool> prepared_snapshot_trusted)
{
    if (source_identity.empty()) {
        return;
    }

    if (prepared_snapshot_trusted) {
        if (!*prepared_snapshot_trusted) {
            auto& untrusted_task_ids =
                recovery_untrusted_task_ids_by_source_[
                    std::string(source_identity)];
            for (const SampleLabelingTask& task : state.tasks) {
                if (!task.task_id.empty() &&
                    prepared_task_ids.contains(task.task_id) &&
                    !preserved_local_task_ids.contains(task.task_id)) {
                    untrusted_task_ids.insert(task.task_id);
                }
            }
        } else {
            const auto source =
                recovery_untrusted_task_ids_by_source_.find(
                    std::string(source_identity));
            if (source !=
                recovery_untrusted_task_ids_by_source_.end()) {
                for (const SampleLabelingTask& task : state.tasks) {
                    if (!task.task_id.empty() &&
                        prepared_task_ids.contains(task.task_id) &&
                        !preserved_local_task_ids.contains(task.task_id)) {
                        source->second.erase(task.task_id);
                    }
                }
            }
        }
    }

    const auto source =
        recovery_untrusted_task_ids_by_source_.find(
            std::string(source_identity));
    if (source == recovery_untrusted_task_ids_by_source_.end()) {
        return;
    }
    const std::unordered_set<std::string> current_task_ids =
        TaskIds(state);
    for (auto task = source->second.begin();
         task != source->second.end();) {
        if (!current_task_ids.contains(*task)) {
            task = source->second.erase(task);
        } else {
            ++task;
        }
    }
    if (source->second.empty()) {
        recovery_untrusted_task_ids_by_source_.erase(source);
    }
}

void SampleLabelingController::UpdateRecoveryTaskTrustFromSnapshot(
    const SampleLabelingStateCacheLoadResult& snapshot)
{
    const bool trusted = RecoverySnapshotTrusted(snapshot);
    for (const auto& [identity, state] : snapshot.cache.sources) {
        const std::unordered_set<std::string> task_ids = TaskIds(state);
        ReconcileRecoveryTaskTrust(
            identity,
            state,
            task_ids,
            {},
            trusted);
    }
}

SampleLabelingWriteOperationResult SampleLabelingController::AssignLabel(
    std::size_t sample_index,
    int code)
{
    SampleLabelingTask* task = ActiveTask();
    if (task == nullptr || task->persistence.output_format ==
            SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar) {
        SampleLabelingWriteOperationResult result;
        result.operation = RejectOperation();
        return result;
    }
    SampleLabelingWriteOperationResult result;
    result.write = AssignSampleLabel(*task, sample_index, code);
    if (result.write.changed) {
        MarkCanonicalValueMutation(*task, task_clock_());
        result.operation =
            CompleteMutation(
                task,
                PersistencePolicy::
                    PersistOutputIfSelectedInteractive,
                task->persistence.output_path
                    ? TaskProjectionEffect::Changed
                    : TaskProjectionEffect::Unchanged);
    } else {
        result.operation = RejectOperation();
        result.operation.accepted = result.write.accepted;
    }
    return result;
}

SampleLabelingWriteOperationResult SampleLabelingController::ClearLabel(std::size_t sample_index)
{
    SampleLabelingTask* task = ActiveTask();
    if (task == nullptr || task->persistence.output_format ==
            SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar) {
        SampleLabelingWriteOperationResult result;
        result.operation = RejectOperation();
        return result;
    }
    SampleLabelingWriteOperationResult result;
    result.write = ClearSampleLabel(*task, sample_index);
    if (result.write.changed) {
        MarkCanonicalValueMutation(*task, task_clock_());
        result.operation =
            CompleteMutation(
                task,
                PersistencePolicy::
                    PersistOutputIfSelectedInteractive,
                task->persistence.output_path
                    ? TaskProjectionEffect::Changed
                    : TaskProjectionEffect::Unchanged);
    } else {
        result.operation = RejectOperation();
        result.operation.accepted = result.write.accepted;
    }
    return result;
}

SampleLabelingController::SourceState* SampleLabelingController::ActiveSource()
{
    if (!active_source_identity_) {
        return nullptr;
    }
    const auto match = sources_.find(*active_source_identity_);
    return match == sources_.end() ? nullptr : &match->second;
}

const SampleLabelingController::SourceState* SampleLabelingController::ActiveSource() const
{
    if (!active_source_identity_) {
        return nullptr;
    }
    const auto match = sources_.find(*active_source_identity_);
    return match == sources_.end() ? nullptr : &match->second;
}

SampleLabelingController::SourceState* SampleLabelingController::MaterializeSource(
    std::string_view source_identity)
{
    const std::string identity{source_identity};
    if (const auto state = sources_.find(identity); state != sources_.end()) {
        return &state->second;
    }
    if (!state_cache_snapshot_) {
        return nullptr;
    }
    const auto cached = state_cache_snapshot_->cache.sources.find(identity);
    if (cached == state_cache_snapshot_->cache.sources.end()) {
        return nullptr;
    }
    return &sources_.emplace(identity, cached->second).first->second;
}

void SampleLabelingController::EnsureStateCacheLoaded()
{
    if (state_cache_loaded_) {
        return;
    }
    state_cache_loaded_ = true;
    SampleLabelingStateCacheLoadResult result = state_cache_loader_(state_cache_path_);
    UpdateRecoveryTaskTrustFromSnapshot(result);
    for (auto& [identity, state] : result.cache.sources) {
        sources_.try_emplace(std::move(identity), std::move(state));
    }
    state_cache_load_warning_ = std::move(result.warning);
    state_cache_load_diagnostic_detail_ = std::move(result.diagnostic_detail);
    Touch();
}

void SampleLabelingController::QueueStateSave()
{
    state_cache_save_scheduler_.MarkDirty();
    state_cache_save_status_.ClearRecovered();
}

void SampleLabelingController::QueueOutputRetry()
{
    output_retry_scheduler_.MarkDirty();
}

SampleLabelingController::TaskOutputRetryResult
SampleLabelingController::TryRetryOutputSaves()
{
    EnsureStateCacheLoaded();
    // A metadata rewrite invalidates its input snapshot as soon as atomic
    // replacement succeeds. If the mandatory reopen failed, restore the
    // selected owner from the current file generation before any retry can
    // publish again.
    static_cast<void>(RestoreActiveTaskLease());

    TaskOutputRetryResult result;
    bool cache_changed = false;
    for (auto& [identity, state] : sources_) {
        for (std::size_t task_index = 0;
             task_index < state.tasks.size();) {
            SampleLabelingTask& task =
                state.tasks[task_index];
            if (!ShouldRetryOutputSave(task)) {
                ++task_index;
                continue;
            }
            // A canonical owner can only be reopened against the descriptor
            // of its active source. Do not feed an inactive source through
            // the current source's descriptor or keep the global scheduler
            // retrying that guaranteed mismatch. Source activation re-arms
            // pending owners after installing their descriptor.
            if (task.persistence.output_format ==
                    SampleLabelingOutputArtifactFormat::
                        CanonicalAsdf &&
                (!active_source_identity_ ||
                 *active_source_identity_ != identity ||
                 !active_source_descriptor_ ||
                 active_source_descriptor_->base_identity !=
                     identity ||
                 active_source_descriptor_->sample_count !=
                     state.sample_count)) {
                ++task_index;
                continue;
            }
            result.attempted = true;
            const bool is_active_task =
                active_source_identity_ &&
                *active_source_identity_ == identity &&
                state.active_task_id &&
                *state.active_task_id == task.task_id &&
                ActiveTaskLeaseMatches(identity, task);
            TaskActivationPreparation preparation;
            if (!is_active_task) {
                preparation = PrepareTaskActivation(
                    identity,
                    task,
                    state.sample_count,
                    false,
                    false);
                if (preparation.lease_status !=
                    ExclusiveFileLeaseAcquireStatus::Acquired) {
                    result.all_succeeded = false;
                    ++task_index;
                    continue;
                }
                if (preparation.refresh_status ==
                    TaskRefreshStatus::Missing) {
                    state.tasks.erase(
                        state.tasks.begin() +
                        static_cast<std::ptrdiff_t>(
                            task_index));
                    if (active_source_identity_ &&
                        *active_source_identity_ == identity) {
                        BumpActiveSourceTasksGeneration();
                    }
                    continue;
                }
                if (preparation.refresh_status !=
                        TaskRefreshStatus::Ready ||
                    !preparation.task) {
                    result.all_succeeded = false;
                    ++task_index;
                    continue;
                }
                const bool task_projection_changed =
                    !SameTaskProjection(
                        task,
                        *preparation.task);
                task = std::move(*preparation.task);
                if (task_projection_changed &&
                    active_source_identity_ &&
                    *active_source_identity_ ==
                        identity) {
                    BumpActiveSourceTasksGeneration();
                }
                if (!ShouldRetryOutputSave(task)) {
                    ++task_index;
                    continue;
                }
            }
            TaskEditLeaseSet& persistence_leases =
                is_active_task
                ? active_task_leases_
                : preparation.leases;
            const TaskOutputPersistenceAttempt attempt =
                PersistTaskOutput(
                    task,
                    &state,
                    persistence_leases,
                    is_active_task
                        ? &active_asdf_snapshot_
                        : &preparation.asdf_snapshot);
            const bool lease_is_current =
                attempt.lease_status ==
                ExclusiveFileLeaseAcquireStatus::Acquired;
            result.all_succeeded =
                (attempt.publication.published ||
                 !attempt.publication.retryable) &&
                lease_is_current &&
                result.all_succeeded;
            result.canonical_output_published =
                result.canonical_output_published ||
                (task.persistence.output_format ==
                     SampleLabelingOutputArtifactFormat::
                         CanonicalAsdf &&
                 attempt.publication.published);
            if (!lease_is_current &&
                is_active_task) {
                state.active_task_id.reset();
                MarkActiveTaskSelection(
                    identity,
                    state);
                DeferActiveTaskLeases();
            }
            MarkTaskUpsert(
                identity,
                state,
                task);
            cache_changed = true;
            if (!is_active_task) {
                deferred_task_leases_.push_back(
                    std::move(preparation.leases));
            }
            ++task_index;
        }
    }

    if (!result.attempted) {
        return result;
    }

    if (cache_changed) {
        QueueStateSave();
    }
    Touch();
    return result;
}

std::optional<SampleLabelingTask>
SampleLabelingController::HydrateCanonicalAsdfTask(
    const SampleLabelingTask& cached_task,
    std::optional<SampleLabelingAsdfOpenSnapshot>* snapshot,
    std::string* error_message) const
{
    if (!cached_task.persistence.output_path ||
        (!runtime_paths_.application_data_root.empty() &&
         CheckUserFilePath(*cached_task.persistence.output_path, runtime_paths_) != UserFilePathStatus::Allowed) ||
        cached_task.persistence.output_format !=
            SampleLabelingOutputArtifactFormat::CanonicalAsdf) {
        if (error_message != nullptr) {
            *error_message =
                "canonical ASDF task owner is missing its document path";
        }
        return std::nullopt;
    }
    if (!active_source_descriptor_ ||
        !active_source_identity_ ||
        active_source_descriptor_->base_identity !=
            *active_source_identity_) {
        if (error_message != nullptr) {
            *error_message =
                "canonical source descriptor is unavailable for ASDF task hydration";
        }
        return std::nullopt;
    }

    const auto deferred = std::find_if(
        deferred_task_leases_.begin(), deferred_task_leases_.end(),
        [&](const TaskEditLeaseSet& leases) {
            return leases.task_identity_key == TaskIdentityEditLeaseKey(
                *active_source_identity_, cached_task.task_id);
        });
    SampleLabelingAsdfStoreOpenResult opened;
    if (HasPendingOutputSave(cached_task) &&
        deferred != deferred_task_leases_.end() &&
        deferred->pending_asdf_snapshot) {
        opened.snapshot = deferred->pending_asdf_snapshot;
        if (const auto mismatch = CheckSampleLabelingSourceCompatibility(
                opened.snapshot->document(),
                SampleLabelingCompatibilityView(*active_source_descriptor_))) {
            if (error_message != nullptr) {
                *error_message = mismatch->message;
            }
            return std::nullopt;
        }
    } else {
        opened = OpenSampleLabelingAsdfDocumentStore(
            *cached_task.persistence.output_path,
            SampleLabelingCompatibilityView(*active_source_descriptor_));
    }
    if (!opened.succeeded()) {
        if (error_message != nullptr) {
            *error_message = opened.error.message.empty()
                ? "could not open canonical ASDF task owner"
                : opened.error.message;
        }
        return std::nullopt;
    }

    SampleLabelingAsdfOpenSnapshot opened_snapshot =
        std::move(*opened.snapshot);
    const SampleLabelingDocument& document =
        opened_snapshot.document();
    if (document.labeling.id != cached_task.task_id) {
        if (error_message != nullptr) {
            *error_message =
                "ASDF labeling task identity does not match the local owner record";
        }
        return std::nullopt;
    }

    std::optional<SampleLabelingTask> hydrated =
        ProjectSampleLabelingDocumentTask(
            document,
            cached_task);
    if (!hydrated) {
        if (error_message != nullptr) {
            *error_message =
                "ASDF labeling pending overlay is incompatible with the canonical task metadata or sample roster";
        }
        return std::nullopt;
    }
    if (snapshot != nullptr) {
        *snapshot = std::move(opened_snapshot);
    }
    return hydrated;
}

SampleLabelingController::TaskActivationPreparation
SampleLabelingController::PrepareTaskActivation(
    std::string_view source_identity,
    const SampleLabelingTask& known_task,
    std::size_t sample_count,
    bool reuse_deferred_lease,
    bool reuse_active_temporary_slot,
    bool allow_pending_task_recovery)
{
    TaskActivationPreparation preparation;
    const std::string task_identity_key =
        TaskIdentityEditLeaseKey(
            source_identity,
            known_task.task_id);
    if (task_identity_key.empty()) {
        preparation.error =
            "labeling task identity is empty";
        return preparation;
    }

    const SampleLabelingTask* active_task =
        ActiveTask();
    const std::string temporary_slot_key =
        TemporarySlotEditLeaseKey(source_identity);
    const bool can_reuse_active_temporary_slot =
        reuse_active_temporary_slot &&
        !known_task.persistence.output_path &&
        !state_cache_path_.empty() &&
        active_source_identity_ &&
        *active_source_identity_ == source_identity &&
        active_task != nullptr &&
        !active_task->persistence.output_path &&
        active_task_leases_.temporary_slot_key ==
            temporary_slot_key &&
        active_task_leases_.temporary_slot.Held();
    preparation.reuses_active_temporary_slot =
        can_reuse_active_temporary_slot;

    auto deferred =
        deferred_task_leases_.end();
    if (reuse_deferred_lease) {
        deferred = std::find_if(
            deferred_task_leases_.begin(),
            deferred_task_leases_.end(),
            [this, &task_identity_key](
                const TaskEditLeaseSet& leases) {
                return leases.task_identity_key ==
                        task_identity_key &&
                    (state_cache_path_.empty() ||
                     leases.task_identity.Held());
            });
    }
    TaskEditLeaseSet* working_leases =
        &preparation.leases;
    if (deferred != deferred_task_leases_.end()) {
        working_leases = &*deferred;
        preparation.lease_status =
            ExclusiveFileLeaseAcquireStatus::Acquired;
    } else {
        preparation.leases.task_identity_key =
            task_identity_key;
    }
    const auto adopt_deferred_leases = [&]() {
        if (deferred == deferred_task_leases_.end()) {
            return;
        }
        preparation.leases = std::move(*deferred);
        deferred_task_leases_.erase(deferred);
        working_leases = &preparation.leases;
    };
    const auto restore_pending_temporary_task = [&]() {
        // A failed first save can leave a locally owned draft only in the
        // pending patch. Recovery may adopt that patch after the identity and
        // temporary-slot leases above have been revalidated.
        if (!allow_pending_task_recovery) {
            return false;
        }
        const SampleLabelingTask* pending =
            PendingTaskUpsert(
                source_identity,
                known_task.task_id);
        if (pending == nullptr ||
            pending->persistence.output_path ||
            pending->values.SampleCount() != sample_count) {
            return false;
        }
        preparation.task = *pending;
        preparation.refresh_status =
            TaskRefreshStatus::Ready;
        adopt_deferred_leases();
        return true;
    };

    if (!state_cache_path_.empty()) {
        if (deferred ==
            deferred_task_leases_.end()) {
            TaskEditLeaseAcquireResult identity_lease =
                TryAcquireTaskEditLease(
                    task_identity_key);
            preparation.lease_status =
                identity_lease.status;
            preparation.error =
                std::move(identity_lease.error);
            if (identity_lease.status !=
                ExclusiveFileLeaseAcquireStatus::Acquired) {
                return preparation;
            }
            preparation.leases.task_identity =
                std::move(identity_lease.component);
        }
        if (!preparation.reuses_active_temporary_slot) {
            ExclusiveFileLeaseAcquireResult temporary_slot_lease =
                TryAttachTemporarySlotLease(
                    *working_leases,
                    source_identity,
                    known_task);
            if (temporary_slot_lease.status !=
                ExclusiveFileLeaseAcquireStatus::Acquired) {
                preparation.lease_status =
                    temporary_slot_lease.status;
                preparation.error =
                    std::move(
                        temporary_slot_lease.error);
                return preparation;
            }
        }
    } else {
        preparation.lease_status =
            ExclusiveFileLeaseAcquireStatus::Acquired;
        preparation.task = known_task;
        const ExclusiveFileLeaseAcquireResult
            temporary_slot_lease =
                TryAttachTemporarySlotLease(
                    *working_leases,
                    source_identity,
                    known_task);
        if (temporary_slot_lease.status !=
            ExclusiveFileLeaseAcquireStatus::Acquired) {
            preparation.lease_status =
                temporary_slot_lease.status;
            preparation.error =
                temporary_slot_lease.error;
            return preparation;
        }
        const ExclusiveFileLeaseAcquireResult output_lease =
            TryAttachOutputLease(
                *working_leases,
                known_task);
        if (output_lease.status !=
            ExclusiveFileLeaseAcquireStatus::Acquired) {
            preparation.lease_status =
                output_lease.status;
            preparation.error = output_lease.error;
            return preparation;
        }
        if (known_task.persistence.output_format ==
            SampleLabelingOutputArtifactFormat::CanonicalAsdf) {
            preparation.task = HydrateCanonicalAsdfTask(
                known_task,
                &preparation.asdf_snapshot,
                &preparation.error);
            if (!preparation.task) {
                return preparation;
            }
        }
        preparation.refresh_status =
            TaskRefreshStatus::Ready;
        adopt_deferred_leases();
        return preparation;
    }

    SampleLabelingStateCacheLoadResult structural =
        LoadSampleLabelingStateCache(
            runtime_paths_,
            state_cache_path_,
            {},
            SampleLabelingStateCacheLoadPolicy::
                AllowPersistentOutputsWithoutResultHydration);
    if (structural.issue_kind !=
        SampleLabelingStateCacheLoadIssueKind::None) {
        preparation.error = structural.warning;
        if (!structural.diagnostic_detail.empty()) {
            preparation.error +=
                preparation.error.empty() ? "" : ": ";
            preparation.error +=
                structural.diagnostic_detail;
        }
        return preparation;
    }
    preparation.latest_cache = structural.cache;
    const auto source =
        structural.cache.sources.find(
            std::string(source_identity));
    if (source == structural.cache.sources.end()) {
        if (!restore_pending_temporary_task()) {
            preparation.refresh_status =
                TaskRefreshStatus::Missing;
        }
        return preparation;
    }
    if (source->second.sample_count != sample_count) {
        preparation.error =
            "labeling source sample count changed";
        return preparation;
    }
    const auto structural_task =
        std::find_if(
            source->second.tasks.begin(),
            source->second.tasks.end(),
            [&known_task](const SampleLabelingTask& task) {
                return task.task_id ==
                    known_task.task_id;
            });
    if (structural_task == source->second.tasks.end()) {
        if (!restore_pending_temporary_task()) {
            preparation.refresh_status =
                TaskRefreshStatus::Missing;
        }
        return preparation;
    }

    const SampleLabelingTask* pending_overlay =
        deferred == deferred_task_leases_.end()
        ? nullptr
        : &known_task;
    bool pending_canonical_owner_switch = false;
    if (pending_overlay != nullptr &&
        OutputEditLeaseKeys(*pending_overlay) !=
            OutputEditLeaseKeys(*structural_task)) {
        pending_canonical_owner_switch =
            structural_task->persistence.output_path &&
            pending_overlay->persistence.output_path &&
            known_task.persistence.output_path &&
            structural_task->persistence.output_format ==
                SampleLabelingOutputArtifactFormat::
                    CanonicalAsdf &&
            pending_overlay->persistence.output_format ==
                SampleLabelingOutputArtifactFormat::
                    CanonicalAsdf &&
            pending_overlay->values.SampleCount() == sample_count &&
            OutputPathMatches(
                *pending_overlay->persistence.output_path,
                *known_task.persistence.output_path) &&
            !OutputPathMatches(
                *pending_overlay->persistence.output_path,
                *structural_task->persistence.output_path);
        if (!pending_canonical_owner_switch) {
            preparation.error =
                "labeling task identity changed while acquiring its edit lease";
            return preparation;
        }
    }

    if (!preparation.reuses_active_temporary_slot) {
        ExclusiveFileLeaseAcquireResult temporary_slot_lease =
            TryAttachTemporarySlotLease(
                *working_leases,
                source_identity,
                *structural_task);
        if (temporary_slot_lease.status !=
            ExclusiveFileLeaseAcquireStatus::Acquired) {
            preparation.lease_status =
                temporary_slot_lease.status;
            preparation.error =
                std::move(
                    temporary_slot_lease.error);
            return preparation;
        }
    }

    ExclusiveFileLeaseAcquireResult output_lease;
    if (pending_canonical_owner_switch) {
        std::vector<std::string> output_keys =
            OutputEditLeaseKeys(*structural_task);
        const std::vector<std::string> pending_output_keys =
            OutputEditLeaseKeys(*pending_overlay);
        output_keys.insert(
            output_keys.end(),
            pending_output_keys.begin(),
            pending_output_keys.end());
        std::sort(
            output_keys.begin(),
            output_keys.end());
        output_keys.erase(
            std::unique(
                output_keys.begin(),
                output_keys.end()),
            output_keys.end());
        const bool union_leases_held =
            std::all_of(
                output_keys.begin(),
                output_keys.end(),
                [this, working_leases](
                    const std::string& output_key) {
                    const auto held = std::find(
                        working_leases->output_artifact_keys.begin(),
                        working_leases->output_artifact_keys.end(),
                        output_key);
                    if (held ==
                        working_leases->output_artifact_keys.end()) {
                        return false;
                    }
                    const std::size_t index =
                        static_cast<std::size_t>(
                            std::distance(
                                working_leases
                                    ->output_artifact_keys.begin(),
                                held));
                    return state_cache_path_.empty() ||
                        (index < working_leases
                                     ->output_artifacts.size() &&
                         working_leases
                             ->output_artifacts[index]
                             .Held());
                });
        if (!union_leases_held) {
            preparation.error =
                "pending labeling owner switch lost an artifact lease";
            return preparation;
        }
        output_lease.status =
            ExclusiveFileLeaseAcquireStatus::Acquired;
    } else {
        output_lease = TryAttachOutputLease(
            *working_leases,
            *structural_task);
    }
    if (output_lease.status !=
        ExclusiveFileLeaseAcquireStatus::Acquired) {
        preparation.lease_status =
            output_lease.status;
        preparation.error =
            std::move(output_lease.error);
        return preparation;
    }

    if (structural_task->persistence.output_format ==
            SampleLabelingOutputArtifactFormat::CanonicalAsdf) {
        const SampleLabelingTask* overlay_task =
            &*structural_task;
        if (pending_overlay != nullptr) {
            if (!pending_canonical_owner_switch &&
                OutputEditLeaseKeys(*pending_overlay) !=
                    OutputEditLeaseKeys(*structural_task)) {
                preparation.error =
                    "labeling task identity changed while acquiring its edit lease";
                return preparation;
            }
            overlay_task = pending_overlay;
        }
        preparation.task = HydrateCanonicalAsdfTask(
            *overlay_task,
            &preparation.asdf_snapshot,
            &preparation.error);
        if (!preparation.task) {
            return preparation;
        }
        preparation.refresh_status =
            TaskRefreshStatus::Ready;
        adopt_deferred_leases();
        return preparation;
    }

    // Temporary drafts carry their complete values in the structural cache;
    // avoid a second full-cache pass when no external result needs hydration.
    if (!structural_task->persistence.output_path) {
        preparation.task = *structural_task;
        if (deferred != deferred_task_leases_.end()) {
            // A checkpoint may lag; an already-held draft lease protects the
            // current process's working object, not the disk snapshot.
            if (!known_task.persistence.output_path && known_task.values.IsComplete())
                preparation.task = known_task;
        }
        preparation.refresh_status = TaskRefreshStatus::Ready;
        adopt_deferred_leases();
        return preparation;
    }

    SampleLabelingStateCacheLoadResult hydrated =
        state_cache_loader_(state_cache_path_);
    if (hydrated.issue_kind !=
        SampleLabelingStateCacheLoadIssueKind::None) {
        preparation.error = hydrated.warning;
        if (!hydrated.diagnostic_detail.empty()) {
            preparation.error +=
                preparation.error.empty() ? "" : ": ";
            preparation.error +=
                hydrated.diagnostic_detail;
        }
        return preparation;
    }
    const auto hydrated_source =
        hydrated.cache.sources.find(
            std::string(source_identity));
    if (hydrated_source == hydrated.cache.sources.end() ||
        hydrated_source->second.sample_count != sample_count) {
        preparation.error =
            "labeling source changed while acquiring its edit lease";
        return preparation;
    }
    const auto hydrated_task =
        std::find_if(
            hydrated_source->second.tasks.begin(),
            hydrated_source->second.tasks.end(),
            [&known_task](const SampleLabelingTask& task) {
                return task.task_id ==
                    known_task.task_id;
            });
    if (hydrated_task ==
            hydrated_source->second.tasks.end() ||
        OutputEditLeaseKeys(
            *hydrated_task) !=
            OutputEditLeaseKeys(
                *structural_task)) {
        preparation.error =
            "labeling task identity changed while acquiring its edit lease";
        return preparation;
    }
    preparation.task = *hydrated_task;
    if (deferred != deferred_task_leases_.end()) {
        if (const SampleLabelingTask* pending =
                PendingTaskUpsert(
                    source_identity,
                    known_task.task_id)) {
            preparation.task = *pending;
        }
    }
    preparation.refresh_status =
        TaskRefreshStatus::Ready;
    adopt_deferred_leases();
    return preparation;
}

SampleLabelingController::TaskCreationPreparation
SampleLabelingController::PrepareTaskCreation(
    std::string_view source_identity,
    std::string_view requested_task_id,
    std::size_t sample_count,
    const std::vector<SampleLabelingTask>& known_tasks,
    bool requires_temporary_slot) const
{
    TaskCreationPreparation preparation;
    if (source_identity.empty() ||
        !IsCanonicalUuidV4(requested_task_id) ||
        sample_count == 0) {
        preparation.error =
            "labeling task creation identity is empty";
        return preparation;
    }

    preparation.lease_status =
        ExclusiveFileLeaseAcquireStatus::Acquired;
    if (requires_temporary_slot) {
        SampleLabelingTask slot_candidate =
            CreateSampleLabelingTask(
                std::string(requested_task_id),
                std::string(requested_task_id),
                sample_count);
        ExclusiveFileLeaseAcquireResult slot_lease =
            TryAttachTemporarySlotLease(
                preparation.leases,
                source_identity,
                slot_candidate);
        preparation.lease_status = slot_lease.status;
        preparation.error = std::move(slot_lease.error);
        if (slot_lease.status !=
            ExclusiveFileLeaseAcquireStatus::Acquired) {
            return preparation;
        }
    }

    if (state_cache_path_.empty()) {
        const bool requested_id_exists = std::any_of(
            known_tasks.begin(),
            known_tasks.end(),
            [requested_task_id](const SampleLabelingTask& task) {
                return task.task_id == requested_task_id;
            });
        if (!requested_id_exists) {
            preparation.task_id = requested_task_id;
        } else {
            constexpr std::size_t kMaximumIdAttempts = 64;
            for (std::size_t attempt = 0;
                 attempt < kMaximumIdAttempts;
                 ++attempt) {
                const std::optional<std::string> generated =
                    task_id_generator_();
                if (generated &&
                    IsCanonicalUuidV4(*generated) &&
                    std::none_of(
                        known_tasks.begin(),
                        known_tasks.end(),
                        [&generated](const SampleLabelingTask& task) {
                            return task.task_id == *generated;
                        })) {
                    preparation.task_id = *generated;
                    break;
                }
            }
        }
        preparation.leases.task_identity_key =
            TaskIdentityEditLeaseKey(
                source_identity,
                preparation.task_id);
        preparation.ready =
            !preparation.leases.task_identity_key.empty();
        return preparation;
    }

    std::unordered_set<std::string> reserved_ids;
    constexpr std::size_t kMaximumLeaseAttempts = 64;
    for (std::size_t attempt = 0;
         attempt < kMaximumLeaseAttempts;
         ++attempt) {
        SampleLabelingStateCacheLoadResult latest =
            LoadSampleLabelingStateCache(
                runtime_paths_,
                state_cache_path_,
                {},
                SampleLabelingStateCacheLoadPolicy::
                    AllowPersistentOutputsWithoutResultHydration);
        if (latest.issue_kind !=
            SampleLabelingStateCacheLoadIssueKind::None) {
            preparation.error = latest.warning;
            if (!latest.diagnostic_detail.empty()) {
                preparation.error +=
                    preparation.error.empty()
                    ? ""
                    : ": ";
                preparation.error +=
                    latest.diagnostic_detail;
            }
            return preparation;
        }

        static const std::vector<SampleLabelingTask>
            kNoTasks;
        const auto latest_source =
            latest.cache.sources.find(
                std::string(source_identity));
        const std::vector<SampleLabelingTask>*
            latest_tasks = &kNoTasks;
        if (latest_source !=
            latest.cache.sources.end()) {
            if (latest_source->second.sample_count !=
                sample_count) {
                preparation.error =
                    "labeling source sample count changed";
                return preparation;
            }
            latest_tasks =
                &latest_source->second.tasks;
            if (requires_temporary_slot &&
                std::any_of(
                    latest_tasks->begin(),
                    latest_tasks->end(),
                    [](const SampleLabelingTask& task) {
                        return !task.persistence.output_path;
                    })) {
                preparation.error =
                    "labeling source already has a temporary task";
                return preparation;
            }
        }

        std::unordered_set<std::string> existing_ids =
            reserved_ids;
        existing_ids.reserve(
            existing_ids.size() + latest_tasks->size() +
            known_tasks.size());
        for (const SampleLabelingTask& task : *latest_tasks) {
            existing_ids.insert(task.task_id);
        }
        for (const SampleLabelingTask& task : known_tasks) {
            existing_ids.insert(task.task_id);
        }

        std::string candidate_id;
        if (!existing_ids.contains(
                std::string(requested_task_id))) {
            candidate_id = requested_task_id;
        } else {
            constexpr std::size_t kMaximumIdAttempts = 64;
            for (std::size_t id_attempt = 0;
                 id_attempt < kMaximumIdAttempts;
                 ++id_attempt) {
                const std::optional<std::string> generated =
                    task_id_generator_();
                if (generated &&
                    IsCanonicalUuidV4(*generated) &&
                    !existing_ids.contains(*generated)) {
                    candidate_id = *generated;
                    break;
                }
            }
        }
        if (candidate_id.empty()) {
            preparation.error =
                "could not generate a canonical labeling task identity";
            return preparation;
        }
        const std::string candidate_lease_key =
            TaskIdentityEditLeaseKey(
                source_identity,
                candidate_id);
        TaskEditLeaseAcquireResult task_lease =
            TryAcquireTaskEditLease(
                candidate_lease_key);
        if (task_lease.status ==
            ExclusiveFileLeaseAcquireStatus::Unavailable) {
            reserved_ids.insert(candidate_id);
            continue;
        }
        preparation.lease_status = task_lease.status;
        preparation.error =
            std::move(task_lease.error);
        if (task_lease.status !=
            ExclusiveFileLeaseAcquireStatus::Acquired) {
            return preparation;
        }
        preparation.leases.task_identity =
            std::move(task_lease.component);
        preparation.leases.task_identity_key =
            candidate_lease_key;

        SampleLabelingStateCacheLoadResult verified =
            LoadSampleLabelingStateCache(
                runtime_paths_,
                state_cache_path_,
                {},
                SampleLabelingStateCacheLoadPolicy::
                    AllowPersistentOutputsWithoutResultHydration);
        if (verified.issue_kind !=
            SampleLabelingStateCacheLoadIssueKind::None) {
            preparation.error = verified.warning;
            if (!verified.diagnostic_detail.empty()) {
                preparation.error +=
                    preparation.error.empty()
                    ? ""
                    : ": ";
                preparation.error +=
                    verified.diagnostic_detail;
            }
            return preparation;
        }
        const auto verified_source =
            verified.cache.sources.find(
                std::string(source_identity));
        if (verified_source !=
            verified.cache.sources.end()) {
            if (verified_source->second.sample_count !=
                sample_count) {
                preparation.error =
                    "labeling source sample count changed while creating a task";
                return preparation;
            }
            if (requires_temporary_slot &&
                std::any_of(
                    verified_source->second.tasks.begin(),
                    verified_source->second.tasks.end(),
                    [](const SampleLabelingTask& task) {
                        return !task.persistence.output_path;
                    })) {
                preparation.error =
                    "labeling source gained a temporary task while creating a task";
                return preparation;
            }
            if (std::any_of(
                    verified_source->second.tasks.begin(),
                    verified_source->second.tasks.end(),
                    [&candidate_id](
                        const SampleLabelingTask& task) {
                        return task.task_id ==
                            candidate_id;
                    })) {
                preparation.leases
                    .task_identity.Reset();
                preparation.leases
                    .task_identity_key.clear();
                reserved_ids.insert(candidate_id);
                continue;
            }
        }

        preparation.task_id = candidate_id;
        preparation.ready = true;
        preparation.lease_status =
            ExclusiveFileLeaseAcquireStatus::Acquired;
        preparation.error.clear();
        return preparation;
    }

    preparation.lease_status =
        ExclusiveFileLeaseAcquireStatus::Unavailable;
    preparation.error =
        "could not reserve a labeling task identity";
    return preparation;
}

SampleLabelingController::TaskEditLeaseAcquireResult
SampleLabelingController::TryAcquireTaskEditLease(
    std::string_view lease_key) const
{
    TaskEditLeaseAcquireResult result;
    const std::vector<std::filesystem::path> paths =
        TaskEditLeasePaths(
            state_cache_path_,
            lease_key);
    if (paths.empty()) {
        result.error =
            "could not resolve labeling edit lease identity";
        return result;
    }
    for (std::size_t index = 0; index < paths.size(); ++index) {
        ExclusiveFileLeaseAcquireResult acquired =
            TryAcquireExclusiveFileLease(paths[index]);
        if (acquired.status !=
            ExclusiveFileLeaseAcquireStatus::Acquired) {
            result.status = acquired.status;
            result.error = std::move(acquired.error);
            return result;
        }
        if (index == 0) {
            result.component.baseline =
                std::move(acquired.lease);
        } else {
            result.component.aliases.push_back(
                std::move(acquired.lease));
        }
    }
    result.status =
        ExclusiveFileLeaseAcquireStatus::Acquired;
    return result;
}

ExclusiveFileLeaseAcquireResult
SampleLabelingController::TryAttachTemporarySlotLease(
    TaskEditLeaseSet& leases,
    std::string_view source_identity,
    const SampleLabelingTask& task) const
{
    ExclusiveFileLeaseAcquireResult result;
    if (task.persistence.output_path) {
        leases.temporary_slot.Reset();
        leases.temporary_slot_key.clear();
        result.status =
            ExclusiveFileLeaseAcquireStatus::Acquired;
        return result;
    }

    const std::string slot_key =
        TemporarySlotEditLeaseKey(
            source_identity);
    if (slot_key.empty()) {
        result.error =
            "labeling temporary slot identity is empty";
        return result;
    }
    if (leases.temporary_slot_key == slot_key &&
        (state_cache_path_.empty() ||
         leases.temporary_slot.Held())) {
        result.status =
            ExclusiveFileLeaseAcquireStatus::Acquired;
        return result;
    }
    if (state_cache_path_.empty()) {
        leases.temporary_slot_key = slot_key;
        result.status =
            ExclusiveFileLeaseAcquireStatus::Acquired;
        return result;
    }
    TaskEditLeaseAcquireResult acquired =
        TryAcquireTaskEditLease(slot_key);
    result.status = acquired.status;
    result.error = std::move(acquired.error);
    if (acquired.status ==
        ExclusiveFileLeaseAcquireStatus::Acquired) {
        leases.temporary_slot =
            std::move(acquired.component);
        leases.temporary_slot_key =
            slot_key;
    }
    return result;
}

ExclusiveFileLeaseAcquireResult
SampleLabelingController::TryAttachOutputLease(
    TaskEditLeaseSet& leases,
    const SampleLabelingTask& task,
    bool resolve_physical_paths) const
{
    ExclusiveFileLeaseAcquireResult result;
    std::vector<std::string> output_keys =
        OutputEditLeaseKeys(
            task,
            resolve_physical_paths);
    if (!resolve_physical_paths) {
        // The normalized-path leases are the synchronous coordination
        // guarantee on the output-save hot path. Keep physical FILE_ID and
        // directory-entry leases acquired during activation until a later
        // non-hot refresh: an atomic replacement can assign a new physical
        // identity, so alias protection is best-effort and is not guaranteed
        // to cover that new identity immediately. Avoid another synchronous
        // physical probe here, including for network- or cloud-backed paths.
        for (const std::string& held_key :
             leases.output_artifact_keys) {
            if (!held_key.starts_with("artifact\n") ||
                std::find(
                    output_keys.begin(),
                    output_keys.end(),
                    held_key) != output_keys.end()) {
                continue;
            }
            output_keys.push_back(held_key);
        }
        std::sort(output_keys.begin(), output_keys.end());
    }
    if (!task.persistence.output_path) {
        leases.output_artifacts.clear();
        leases.output_artifact_keys.clear();
        result.status =
            ExclusiveFileLeaseAcquireStatus::Acquired;
        return result;
    }
    return TryAttachArtifactLeases(
        leases,
        std::move(output_keys));
}

ExclusiveFileLeaseAcquireResult
SampleLabelingController::TryAttachArtifactLeases(
    TaskEditLeaseSet& leases,
    std::vector<std::string> output_keys) const
{
    ExclusiveFileLeaseAcquireResult result;
    if (output_keys.empty()) {
        result.error =
            "labeling output identity is empty";
        return result;
    }
    const bool all_leases_held =
        leases.output_artifacts.size() ==
            output_keys.size() &&
        std::all_of(
            leases.output_artifacts.begin(),
            leases.output_artifacts.end(),
            [](const TaskEditLeaseSet::Component& lease) {
                return lease.Held();
            });
    if (leases.output_artifact_keys == output_keys &&
        (state_cache_path_.empty() || all_leases_held)) {
        result.status =
            ExclusiveFileLeaseAcquireStatus::Acquired;
        return result;
    }
    if (state_cache_path_.empty()) {
        leases.output_artifact_keys = output_keys;
        result.status =
            ExclusiveFileLeaseAcquireStatus::Acquired;
        return result;
    }
    std::vector<std::pair<
        std::string,
        TaskEditLeaseSet::Component>>
        acquired;
    acquired.reserve(output_keys.size());
    for (const std::string& output_key : output_keys) {
        const auto held = std::find(
            leases.output_artifact_keys.begin(),
            leases.output_artifact_keys.end(),
            output_key);
        if (held !=
                leases.output_artifact_keys.end() &&
            static_cast<std::size_t>(
                std::distance(
                    leases.output_artifact_keys.begin(),
                    held)) <
                leases.output_artifacts.size() &&
            leases.output_artifacts[
                static_cast<std::size_t>(
                    std::distance(
                        leases.output_artifact_keys.begin(),
                        held))]
                .Held()) {
            continue;
        }
        TaskEditLeaseAcquireResult output_lease =
            TryAcquireTaskEditLease(output_key);
        result.status = output_lease.status;
        result.error = std::move(output_lease.error);
        if (result.status !=
            ExclusiveFileLeaseAcquireStatus::Acquired) {
            return result;
        }
        acquired.emplace_back(
            output_key,
            std::move(output_lease.component));
    }
    std::vector<TaskEditLeaseSet::Component> complete;
    complete.reserve(output_keys.size());
    for (const std::string& output_key : output_keys) {
        const auto held = std::find(
            leases.output_artifact_keys.begin(),
            leases.output_artifact_keys.end(),
            output_key);
        if (held !=
            leases.output_artifact_keys.end()) {
            const std::size_t held_index =
                static_cast<std::size_t>(
                    std::distance(
                        leases.output_artifact_keys.begin(),
                        held));
            if (held_index <
                leases.output_artifacts.size() &&
                leases.output_artifacts[held_index].Held()) {
                complete.push_back(
                    std::move(
                        leases.output_artifacts[
                            held_index]));
                continue;
            }
        }
        const auto added = std::find_if(
            acquired.begin(),
            acquired.end(),
            [&output_key](const auto& entry) {
                return entry.first == output_key;
            });
        complete.push_back(
            std::move(added->second));
    }
    leases.output_artifacts = std::move(complete);
    leases.output_artifact_keys = output_keys;
    result.status =
        ExclusiveFileLeaseAcquireStatus::Acquired;
    return result;
}

bool SampleLabelingController::ActiveTaskLeaseMatches(
    std::string_view source_identity,
    const SampleLabelingTask& task)
{
    const std::vector<std::string> required_output_keys =
        OutputEditLeaseKeys(
            task,
            false);
    const bool required_output_leases_held =
        !task.persistence.output_path ||
        (!required_output_keys.empty() &&
         std::all_of(
             required_output_keys.begin(),
             required_output_keys.end(),
             [this](const std::string& key) {
                 const auto held = std::find(
                     active_task_leases_
                         .output_artifact_keys.begin(),
                     active_task_leases_
                         .output_artifact_keys.end(),
                     key);
                 if (held == active_task_leases_
                                 .output_artifact_keys.end()) {
                     return false;
                 }
                 const std::size_t index =
                     static_cast<std::size_t>(
                         std::distance(
                             active_task_leases_
                                 .output_artifact_keys.begin(),
                             held));
                return state_cache_path_.empty() ||
                    (index < active_task_leases_
                                  .output_artifacts.size() &&
                      active_task_leases_
                          .output_artifacts[index]
                          .Held());
              }));
    bool canonical_generation_matches =
        task.persistence.output_format !=
            SampleLabelingOutputArtifactFormat::CanonicalAsdf;
    if (!canonical_generation_matches && task.persistence.output_path &&
        active_asdf_snapshot_ && active_source_descriptor_ &&
        OutputPathMatches(
            active_asdf_snapshot_->path(),
            *task.persistence.output_path) &&
        active_asdf_snapshot_->document().labeling.id ==
            task.task_id) {
        canonical_generation_matches =
            active_asdf_snapshot_source_descriptor_generation_ &&
            *active_asdf_snapshot_source_descriptor_generation_ ==
                active_source_descriptor_generation_;
        if (!canonical_generation_matches &&
            !CheckSampleLabelingSourceCompatibility(
                 active_asdf_snapshot_->document(),
                 SampleLabelingCompatibilityView(
                     *active_source_descriptor_))
                 .has_value()) {
            active_asdf_snapshot_source_descriptor_generation_ =
                active_source_descriptor_generation_;
            canonical_generation_matches = true;
        }
    }
    return active_task_leases_.task_identity_key ==
               TaskIdentityEditLeaseKey(
                   source_identity,
                   task.task_id) &&
            required_output_leases_held &&
            canonical_generation_matches &&
            (state_cache_path_.empty() ||
             active_task_leases_.task_identity.Held()) &&
           (task.persistence.output_path ||
            (active_task_leases_.temporary_slot_key ==
                 TemporarySlotEditLeaseKey(
                     source_identity) &&
             (state_cache_path_.empty() ||
                active_task_leases_.temporary_slot.Held())));
}

bool SampleLabelingController::TaskIdentityLeaseHeld(
    const TaskEditLeaseSet& leases,
    std::string_view source_identity,
    std::string_view task_id) const
{
    return leases.task_identity_key ==
               TaskIdentityEditLeaseKey(
                   source_identity,
                   task_id) &&
         (state_cache_path_.empty() ||
          leases.task_identity.Held());
}

bool SampleLabelingController::LocalTaskProjectionProtected(
    std::string_view source_identity,
    std::string_view task_id) const
{
    if (PendingTaskUpsert(source_identity, task_id) != nullptr ||
        TaskIdentityLeaseHeld(
            active_task_leases_,
            source_identity,
            task_id)) {
        return true;
    }
    return std::any_of(
        deferred_task_leases_.begin(),
        deferred_task_leases_.end(),
        [this, source_identity, task_id](
            const TaskEditLeaseSet& leases) {
            return TaskIdentityLeaseHeld(
                leases,
                source_identity,
                task_id);
        });
}

void SampleLabelingController::AdoptActiveTaskLeases(
    TaskEditLeaseSet leases)
{
    // The prepared snapshot is handed back to active_asdf_snapshot_; do not
    // retain a second, potentially older durable base on the active lease.
    leases.pending_asdf_snapshot.reset();
    active_task_leases_ = std::move(leases);
}

void SampleLabelingController::ReplaceActiveSourceDescriptor(
    std::optional<SampleLabelingCanonicalSourceDescriptor>
        source_descriptor)
{
    active_source_descriptor_ = std::move(source_descriptor);
    ++active_source_descriptor_generation_;
}

void SampleLabelingController::ReplaceActiveAsdfSnapshot(
    std::optional<SampleLabelingAsdfOpenSnapshot>
        asdf_snapshot)
{
    active_asdf_snapshot_ = std::move(asdf_snapshot);
    if (active_asdf_snapshot_) {
        active_asdf_snapshot_source_descriptor_generation_ =
            active_source_descriptor_generation_;
    } else {
        active_asdf_snapshot_source_descriptor_generation_.reset();
    }
}

bool SampleLabelingController::
    DowngradeActiveCanonicalTaskProjection() noexcept
{
    if (!active_asdf_snapshot_) {
        return false;
    }
    const SampleLabelingDocument& document =
        active_asdf_snapshot_->document();
    const auto source = sources_.find(
        document.source.base_identity);
    if (source == sources_.end()) {
        return false;
    }
    const auto task = std::find_if(
        source->second.tasks.begin(),
        source->second.tasks.end(),
        [&document](const SampleLabelingTask& candidate) {
            return candidate.task_id ==
                document.labeling.id;
        });
    if (task == source->second.tasks.end() ||
        task->persistence.output_format !=
            SampleLabelingOutputArtifactFormat::CanonicalAsdf ||
        !task->persistence.output_path ||
        !OutputPathMatches(
            active_asdf_snapshot_->path(),
            *task->persistence.output_path) ||
        !task->values.IsComplete()) {
        return false;
    }

    if (!DowngradeCanonicalSampleLabelingTaskToStructural(
            *task)) {
        return false;
    }
    BumpActiveSourceTasksGeneration();
    return true;
}

void SampleLabelingController::TransferActiveTemporarySlotLease(
    TaskEditLeaseSet& leases)
{
    leases.temporary_slot =
        std::move(active_task_leases_.temporary_slot);
    leases.temporary_slot_key =
        std::move(active_task_leases_.temporary_slot_key);
}

void SampleLabelingController::TransitionActiveTaskLeases(
    TaskEditLeaseSet leases,
    std::optional<SampleLabelingAsdfOpenSnapshot>
        asdf_snapshot,
    bool pending_patch_saved)
{
    static_cast<void>(
        DowngradeActiveCanonicalTaskProjection());
    const std::string task_identity_key =
        leases.task_identity_key;
    if (!pending_patch_saved) {
        DeferActiveTaskLeases();
    }
    AdoptActiveTaskLeases(std::move(leases));
    ReplaceActiveAsdfSnapshot(
        std::move(asdf_snapshot));
    if (!task_identity_key.empty()) {
        lease_unavailable_task_targets_.erase(
            task_identity_key);
    }
    if (active_source_identity_) {
        lease_unavailable_source_identities_.erase(
            *active_source_identity_);
    }
}

void SampleLabelingController::ReleaseUnneededActiveLeaseComponents()
{
    const SampleLabelingTask* active_task =
        ActiveTask();
    if (active_task == nullptr) {
        return;
    }
    if (active_task->persistence.output_path) {
        active_task_leases_.temporary_slot.Reset();
        active_task_leases_.temporary_slot_key.clear();
        // A failed owner-switch cache commit deliberately retains both the
        // durable old path and the pending replacement path. Once the cache
        // commit succeeds, converge to the exact artifact identities of the
        // now-durable owner; TryAttachOutputLease keeps the existing union
        // intact if an unexpected replacement identity cannot be acquired.
        static_cast<void>(
            TryAttachOutputLease(
                active_task_leases_,
                *active_task));
    } else {
        active_task_leases_.output_artifacts.clear();
        active_task_leases_.output_artifact_keys.clear();
    }
}

void SampleLabelingController::DeferActiveTaskLeases()
{
    if (const auto* task = ActiveTask();
        task && HasPendingOutputSave(*task) && active_asdf_snapshot_) {
        active_task_leases_.pending_asdf_snapshot = active_asdf_snapshot_;
    }
    static_cast<void>(
        DowngradeActiveCanonicalTaskProjection());
    if (!active_task_leases_.task_identity_key.empty()) {
        deferred_task_leases_.push_back(
            std::move(active_task_leases_));
    }
    active_task_leases_ = {};
    ReplaceActiveAsdfSnapshot(std::nullopt);
}

bool SampleLabelingController::PendingRecoveryPatchProtectsTaskLease(
    std::string_view source_identity,
    std::string_view task_id) const
{
    if (const auto live = sources_.find(std::string(source_identity)); live != sources_.end()) {
        for (const auto& task : live->second.tasks) {
            if (task.task_id == task_id && HasPendingOutputSave(task)) return true;
        }
    }
    const auto source = pending_cache_patch_.sources.find(
        std::string(source_identity));
    if (source == pending_cache_patch_.sources.end()) {
        return false;
    }
    const SampleLabelingSourceStatePatch& patch =
        source->second;
    return std::any_of(
               patch.task_upserts.begin(),
               patch.task_upserts.end(),
               [task_id](const SampleLabelingTask& task) {
                   return task.task_id == task_id;
               }) ||
        std::find(
            patch.task_tombstones.begin(),
            patch.task_tombstones.end(),
            task_id) != patch.task_tombstones.end();
}

void SampleLabelingController::ReleaseDeferredTaskLeaseUnlessRecoveryPending(
    std::string_view source_identity,
    std::string_view task_id)
{
    if (PendingRecoveryPatchProtectsTaskLease(
            source_identity,
            task_id)) {
        return;
    }
    const std::string task_identity_key =
        TaskIdentityEditLeaseKey(
            source_identity,
            task_id);
    deferred_task_leases_.erase(
        std::remove_if(
            deferred_task_leases_.begin(),
            deferred_task_leases_.end(),
            [&task_identity_key](
                const TaskEditLeaseSet& leases) {
                return leases.task_identity_key ==
                    task_identity_key;
            }),
        deferred_task_leases_.end());
}

void SampleLabelingController::ReleaseActiveTaskLeaseForTransition()
{
    if (const auto* task = ActiveTask(); task && HasPendingOutputSave(*task)) {
        DeferActiveTaskLeases();
        return;
    }
    if (state_cache_save_scheduler_.dirty() &&
        !FlushStateCache()) {
        DeferActiveTaskLeases();
        return;
    }
    ReleaseActiveTaskLease();
}

bool SampleLabelingController::RestoreActiveTaskLease()
{
    SourceState* state = ActiveSource();
    if (state == nullptr || !state->active_task_id) {
        ReleaseActiveTaskLease();
        return false;
    }
    const auto active =
        std::find_if(
            state->tasks.begin(),
            state->tasks.end(),
            [state](const SampleLabelingTask& task) {
                return task.task_id ==
                    *state->active_task_id;
            });
    if (active == state->tasks.end()) {
        ClearRecoveryTaskTrust(
            *active_source_identity_,
            *state->active_task_id);
        lease_unavailable_task_targets_.erase(
            TaskIdentityEditLeaseKey(
                *active_source_identity_,
                *state->active_task_id));
        state->active_task_id.reset();
        ReleaseActiveTaskLease();
        if (active_source_identity_) {
            lease_unavailable_source_identities_.erase(
                *active_source_identity_);
        }
        return true;
    }
    const bool canonical_generation_requires_reopen =
        active->persistence.output_format ==
            SampleLabelingOutputArtifactFormat::CanonicalAsdf &&
        (!active_asdf_snapshot_ ||
         (active_source_descriptor_ &&
          (!active_asdf_snapshot_source_descriptor_generation_ ||
           *active_asdf_snapshot_source_descriptor_generation_ !=
               active_source_descriptor_generation_)));
    const std::string revalidating_source_identity =
        canonical_generation_requires_reopen
        ? *active_source_identity_
        : std::string{};
    const std::string revalidating_task_id =
        canonical_generation_requires_reopen
        ? active->task_id
        : std::string{};
    if (!canonical_generation_requires_reopen &&
        active_source_identity_ &&
        ActiveTaskLeaseMatches(
            *active_source_identity_,
            *active)) {
        lease_unavailable_task_targets_.erase(
            TaskIdentityEditLeaseKey(
                *active_source_identity_,
                active->task_id));
        lease_unavailable_source_identities_.erase(
            *active_source_identity_);
        return false;
    }
    if (canonical_generation_requires_reopen) {
        // Preserve the already-held owner leases while the changed source
        // generation or a post-rewrite missing snapshot is reopened.
        // PrepareTaskActivation can adopt this set; a failure releases it
        // unless an unpersisted task recovery patch still needs protection.
        DeferActiveTaskLeases();
    }
    TaskActivationPreparation preparation =
        PrepareTaskActivation(
            *active_source_identity_,
            *active,
            state->sample_count,
            true,
            true);
    if (preparation.lease_status !=
            ExclusiveFileLeaseAcquireStatus::Acquired ||
        preparation.refresh_status !=
            TaskRefreshStatus::Ready ||
        !preparation.task) {
        if (DowngradeCanonicalSampleLabelingTaskToStructural(
                *active)) {
            BumpActiveSourceTasksGeneration();
        }
        if (preparation.refresh_status ==
                TaskRefreshStatus::Missing &&
            active_source_identity_) {
            const std::string missing_task_id =
                active->task_id;
            ClearRecoveryTaskTrust(
                *active_source_identity_,
                missing_task_id);
            lease_unavailable_task_targets_.erase(
                TaskIdentityEditLeaseKey(
                    *active_source_identity_,
                    missing_task_id));
            const auto refreshed_source =
                preparation.latest_cache.sources.find(
                    *active_source_identity_);
            if (refreshed_source !=
                preparation.latest_cache.sources.end()) {
                const SourceState local = *state;
                SourceState merged =
                    MergeRefreshedSourceState(
                        *active_source_identity_,
                        local,
                        refreshed_source->second);
                *state = std::move(merged);
            } else {
                state->tasks.erase(
                    std::remove_if(
                        state->tasks.begin(),
                        state->tasks.end(),
                        [&missing_task_id](
                            const SampleLabelingTask& task) {
                            return task.task_id ==
                                missing_task_id;
                        }),
                    state->tasks.end());
            }
        }
        state->active_task_id.reset();
        ReleaseActiveTaskLease();
        if (canonical_generation_requires_reopen) {
            ReleaseDeferredTaskLeaseUnlessRecoveryPending(
                revalidating_source_identity,
                revalidating_task_id);
        }
        if (active_source_identity_ &&
            preparation.lease_status ==
                ExclusiveFileLeaseAcquireStatus::Unavailable) {
            lease_unavailable_source_identities_.insert(
                *active_source_identity_);
            const std::string task_identity_key =
                TaskIdentityEditLeaseKey(
                    *active_source_identity_,
                    active->task_id);
            if (!task_identity_key.empty()) {
                lease_unavailable_task_targets_.insert(
                    task_identity_key);
            }
        }
        return true;
    }
    const bool task_projection_changed =
        !SameTaskProjection(
            *active,
            *preparation.task);
    *active = std::move(*preparation.task);
    ClearRecoveryTaskTrust(
        *active_source_identity_,
        active->task_id);
    if (preparation.reuses_active_temporary_slot) {
        TransferActiveTemporarySlotLease(
            preparation.leases);
    }
    AdoptActiveTaskLeases(
        std::move(preparation.leases));
    ReplaceActiveAsdfSnapshot(
        std::move(preparation.asdf_snapshot));
    if (active_source_identity_) {
        lease_unavailable_task_targets_.erase(
            TaskIdentityEditLeaseKey(
                *active_source_identity_,
                active->task_id));
        lease_unavailable_source_identities_.erase(
            *active_source_identity_);
    }
    return task_projection_changed;
}

void SampleLabelingController::ReleaseActiveTaskLease() noexcept
{
    static_cast<void>(
        DowngradeActiveCanonicalTaskProjection());
    active_task_leases_ = {};
    ReplaceActiveAsdfSnapshot(std::nullopt);
}

const SampleLabelingTask*
SampleLabelingController::PendingTaskUpsert(
    std::string_view source_identity,
    std::string_view task_id) const
{
    const auto source = pending_cache_patch_.sources.find(
        std::string(source_identity));
    if (source == pending_cache_patch_.sources.end()) {
        return nullptr;
    }
    const auto task = std::find_if(
        source->second.task_upserts.begin(),
        source->second.task_upserts.end(),
        [task_id](const SampleLabelingTask& candidate) {
            return candidate.task_id == task_id;
        });
    return task == source->second.task_upserts.end()
        ? nullptr
        : &*task;
}

bool SampleLabelingController::PendingTaskExpectedAbsent(
    std::string_view source_identity,
    std::string_view task_id) const
{
    const auto source = pending_cache_patch_.sources.find(
        std::string(source_identity));
    return source != pending_cache_patch_.sources.end() &&
        source->second.task_ids_expected_absent.contains(
            std::string(task_id));
}

void SampleLabelingController::MarkSourceMetadataUpsert(
    std::string_view source_identity,
    const SourceState& state)
{
    if (source_identity.empty() || state.sample_count == 0) {
        return;
    }
    SampleLabelingSourceStatePatch& patch =
        pending_cache_patch_.sources[
            std::string(source_identity)];
    patch.metadata =
        SampleLabelingSourceMetadataPatch{
            .sample_count = state.sample_count,
            .source_name = state.source_name,
            .source_fingerprint = state.source_fingerprint,
            .context_fingerprint = state.context_fingerprint};
}

void SampleLabelingController::MarkTaskUpsert(
    std::string_view source_identity,
    const SourceState& state,
    const SampleLabelingTask& task,
    bool expected_absent)
{
    MarkSourceMetadataUpsert(source_identity, state);
    SampleLabelingSourceStatePatch& patch =
        pending_cache_patch_.sources[
            std::string(source_identity)];
    patch.task_tombstones.erase(
        std::remove(
            patch.task_tombstones.begin(),
            patch.task_tombstones.end(),
            task.task_id),
        patch.task_tombstones.end());
    if (expected_absent) {
        patch.task_ids_expected_absent.insert(
            task.task_id);
    }
    const auto existing =
        std::find_if(
            patch.task_upserts.begin(),
            patch.task_upserts.end(),
            [&task](const SampleLabelingTask& candidate) {
                return candidate.task_id == task.task_id;
            });
    if (existing == patch.task_upserts.end()) {
        patch.task_upserts.push_back(task);
    } else {
        *existing = task;
    }
}

void SampleLabelingController::MarkTaskTombstone(
    std::string_view source_identity,
    const SourceState& state,
    std::string task_id)
{
    if (task_id.empty()) {
        return;
    }
    MarkSourceMetadataUpsert(source_identity, state);
    SampleLabelingSourceStatePatch& patch =
        pending_cache_patch_.sources[
            std::string(source_identity)];
    patch.task_upserts.erase(
        std::remove_if(
            patch.task_upserts.begin(),
            patch.task_upserts.end(),
            [&task_id](const SampleLabelingTask& task) {
                return task.task_id == task_id;
            }),
        patch.task_upserts.end());
    const bool canceled_pending_create =
        patch.task_ids_expected_absent.erase(task_id) != 0;
    if (canceled_pending_create) {
        // This task was never durably present. Cancel the local create
        // precondition instead of tombstoning a same-id task that another
        // instance may have repaired into the shared cache.
        return;
    }
    if (std::find(
            patch.task_tombstones.begin(),
            patch.task_tombstones.end(),
            task_id) == patch.task_tombstones.end()) {
        patch.task_tombstones.push_back(
            std::move(task_id));
    }
}

void SampleLabelingController::MarkActiveTaskSelection(
    std::string_view source_identity,
    const SourceState& state)
{
    MarkSourceMetadataUpsert(source_identity, state);
    SampleLabelingSourceStatePatch& patch =
        pending_cache_patch_.sources[
            std::string(source_identity)];
    patch.active_task_selection_changed = true;
    patch.active_task_id = state.active_task_id;
}

std::optional<bool>
SampleLabelingController::LatestCacheHasOutputConflict(
    const SampleLabelingTask& candidate) const
{
    if (!candidate.persistence.output_path ||
        !active_source_identity_) {
        return false;
    }
    const SampleLabelingStateCacheLoadResult latest =
        LoadSampleLabelingStateCache(
            runtime_paths_,
            state_cache_path_,
            {},
            SampleLabelingStateCacheLoadPolicy::
                OrdinaryRegistrationsOnly);
    if (latest.issue_kind !=
        SampleLabelingStateCacheLoadIssueKind::None) {
        return std::nullopt;
    }
    return HasSampleLabelingOutputPathConflict(
        latest.cache,
        candidate,
        *active_source_identity_);
}

SampleLabelingMaintenanceResult
SampleLabelingController::MaybeRetryOutputSaves(
    LocalUserStateSaveScheduler::TimePoint now)
{
    if (!output_retry_scheduler_.ShouldAttemptSave(now)) {
        return {};
    }

    const TaskOutputRetryResult retry =
        TryRetryOutputSaves();
    if (retry.all_succeeded) {
        output_retry_scheduler_.MarkSaveSucceeded();
    } else {
        output_retry_scheduler_.MarkSaveFailed();
    }
    return SampleLabelingMaintenanceResult{
        .output_retry_attempted = true,
        .canonical_output_published =
            retry.canonical_output_published};
}

bool SampleLabelingController::TrySaveStateCache(
    bool wait_for_commit_lock)
{
    EnsureStateCacheLoaded();
    const auto normalize_saved_task = [](SampleLabelingTask& task) {
        if (!task.persistence.output_path) {
            task.persistence.pending_sample_indices.clear();
            task.persistence.metadata_save_pending = false;
            task.persistence.save_state.pending_count = 0;
            if (task.persistence.save_state.kind != SampleLabelSaveStateKind::Failed) {
                task.persistence.save_state.kind = SampleLabelSaveStateKind::InternalDraftOnly;
                if (task.persistence.save_state.message_kind ==
                    SampleLabelSaveMessageKind::None) {
                    task.persistence.save_state.message.clear();
                }
            }
            return;
        }
        task.persistence.save_state.pending_count = task.persistence.pending_sample_indices.size();
        if (!HasPendingOutputSave(task) &&
            task.persistence.save_state.kind != SampleLabelSaveStateKind::Failed) {
            MarkSampleLabelTaskPersisted(task, SampleLabelSaveStateKind::AutosavedToOutput);
        } else if (HasPendingOutputSave(task) &&
                   task.persistence.save_state.kind != SampleLabelSaveStateKind::Failed) {
            task.persistence.save_state.kind = SampleLabelSaveStateKind::Pending;
            task.persistence.save_state.message_kind =
                SampleLabelSaveMessageKind::None;
            task.persistence.save_state.message.clear();
        }
    };
    const auto normalize_saved_states = [&normalize_saved_task](
                                            std::unordered_map<std::string, SourceState>& sources) {
        for (auto& [identity, state] : sources) {
            (void)identity;
            for (SampleLabelingTask& task : state.tasks) {
                normalize_saved_task(task);
            }
        }
    };

    SampleLabelingStateCachePatch patch =
        pending_cache_patch_;
    for (auto& [identity, source_patch] : patch.sources) {
        (void)identity;
        for (SampleLabelingTask& task :
             source_patch.task_upserts) {
            normalize_saved_task(task);
        }
    }
    const auto result =
        CommitSampleLabelingStateCachePatch(
            runtime_paths_,
            state_cache_path_,
            patch,
            nullptr,
            wait_for_commit_lock
                ? kSynchronousCommitLockWait
                : std::chrono::milliseconds::zero());
    for (auto& [identity, source_patch] : pending_cache_patch_.sources) {
        (void)identity;
        for (const auto& task : source_patch.task_upserts) {
            if (result.TaskIdentityPersisted(task)) {
                source_patch.task_ids_expected_absent.erase(task.task_id);
            }
        }
    }
    if (result.Succeeded()) {
        normalize_saved_states(sources_);
        pending_cache_patch_ = {};
        ReleaseUnneededActiveLeaseComponents();
        std::erase_if(deferred_task_leases_, [this](const TaskEditLeaseSet& leases) {
            for (const auto& [identity, source] : sources_) {
                for (const auto& task : source.tasks) {
                    if (HasPendingOutputSave(task) &&
                        TaskIdentityEditLeaseKey(identity, task.task_id) == leases.task_identity_key)
                        return false;
                }
            }
            return true;
        });
        for (const auto& [identity, source_patch] : patch.sources) {
            for (const SampleLabelingTask& task :
                 source_patch.task_upserts) {
                ClearRecoveryTaskTrust(
                    identity,
                    task.task_id);
            }
            for (const std::string& task_id :
                 source_patch.task_tombstones) {
                ClearRecoveryTaskTrust(
                    identity,
                    task_id);
            }
        }
        state_cache_load_warning_.clear();
        state_cache_load_diagnostic_detail_.clear();
        state_cache_save_scheduler_.MarkSaveSucceeded(state_cache_save_status_);
    } else {
        state_cache_save_scheduler_.MarkDirty();
        for (auto& [identity, source_patch] :
             pending_cache_patch_.sources) {
            (void)identity;
            source_patch.active_task_selection_changed = false;
            source_patch.active_task_id.reset();
        }
        state_cache_save_status_.MarkFailed(
            result.diagnostic_detail.empty()
                ? "could not write local sample-labeling task record"
                : result.diagnostic_detail);
    }
    Touch();
    return result.Succeeded();
}

SampleLabelingMaintenanceResult
SampleLabelingController::RunMaintenance(
    LocalUserStateSaveScheduler::TimePoint now)
{
    // Canonical retry consumes runtime dirty state and has no checkpoint dependency.
    SampleLabelingMaintenanceResult result =
        MaybeRetryOutputSaves(now);
    if (state_cache_save_scheduler_.ShouldAttemptSave(now) &&
        !TrySaveStateCache()) {
        state_cache_save_scheduler_.MarkSaveFailed();
    }
    return result;
}

std::optional<LocalUserStateSaveScheduler::TimePoint> SampleLabelingController::NextMaintenanceDeadline() const
{
    std::optional<LocalUserStateSaveScheduler::TimePoint> deadline =
        state_cache_save_scheduler_.next_attempt_time();
    const auto output_retry_deadline = output_retry_scheduler_.next_attempt_time();
    if (output_retry_deadline && (!deadline || *output_retry_deadline < *deadline)) {
        deadline = output_retry_deadline;
    }
    return deadline;
}

bool SampleLabelingController::FlushStateCache()
{
    if (!state_cache_save_scheduler_.dirty()) {
        return true;
    }
    return TrySaveStateCache(true);
}

bool SampleLabelingController::state_save_pending() const
{
    return state_cache_save_scheduler_.dirty();
}

bool SampleLabelingController::state_save_failed() const
{
    return state_cache_save_status_.failed();
}

std::string_view SampleLabelingController::state_save_error() const
{
    return state_cache_save_status_.message_view();
}

std::string_view SampleLabelingController::state_load_warning() const
{
    return state_cache_load_warning_;
}

LocalUserStatePersistenceStatus
SampleLabelingController::PersistenceStatus() const
{
    return {
        .retrying = state_cache_save_status_.failed(),
        .recovered = state_cache_save_status_.recovered(),
        .load_warning = state_cache_load_warning_,
        .save_message = state_cache_save_status_.message(),
        .load_diagnostic_detail = state_cache_load_diagnostic_detail_,
        .save_diagnostic_detail = state_cache_save_status_.message(),
    };
}

}  // namespace spectiary
