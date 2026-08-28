#include "ui/sample_labeling_state_cache_io.h"

#include "app/local_user_state.h"
#include "app/local_user_state_json.h"
#include "app/local_user_state_paths.h"
#include "domain/sample_annotation_io.h"
#include "domain/source_path_identity.h"
#include "domain/source_collection_identity_digest.h"
#include "domain/stable_sha256.h"
#include "platform/exclusive_file_lease.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace specforge {
namespace {

constexpr const char* kStateFormatKind = "specforge.sample_labeling_tasks.cache";
constexpr int kStateSchemaVersion = 3;

const JsonValue* ObjectMember(const JsonValue& value, std::string_view key)
{
    return JsonObjectMember(value, key);
}

std::optional<std::string> ReadStringMember(const JsonValue& value, std::string_view key)
{
    return ReadJsonStringMember(value, key);
}

std::optional<std::size_t> ReadSizeMember(const JsonValue& value, std::string_view key)
{
    return ReadJsonSizeMember(value, key);
}

std::optional<int> ReadIntMember(const JsonValue& value, std::string_view key)
{
    return ReadJsonIntMember(value, key);
}

bool ReadBoolMember(const JsonValue& value, std::string_view key, bool fallback)
{
    return ReadJsonBoolMember(value, key, fallback);
}

SampleLabelSaveStateKind ParseSaveStateKind(std::string_view text)
{
    if (text == "pending") {
        return SampleLabelSaveStateKind::Pending;
    }
    if (text == "failed") {
        return SampleLabelSaveStateKind::Failed;
    }
    if (text == "autosaved_to_output") {
        return SampleLabelSaveStateKind::AutosavedToOutput;
    }
    return SampleLabelSaveStateKind::InternalDraftOnly;
}

std::string_view SaveStateKindText(SampleLabelSaveStateKind kind)
{
    switch (kind) {
    case SampleLabelSaveStateKind::AutosavedToOutput:
        return "autosaved_to_output";
    case SampleLabelSaveStateKind::Pending:
        return "pending";
    case SampleLabelSaveStateKind::Failed:
        return "failed";
    case SampleLabelSaveStateKind::InternalDraftOnly:
    default:
        return "internal_draft_only";
    }
}

SampleLabelSaveMessageKind ParseSaveMessageKind(
    std::string_view text)
{
    if (text == "output_path_already_used") {
        return SampleLabelSaveMessageKind::OutputPathAlreadyUsed;
    }
    if (text == "output_save_failed") {
        return SampleLabelSaveMessageKind::OutputSaveFailed;
    }
    if (text == "system_detail") {
        return SampleLabelSaveMessageKind::SystemDetail;
    }
    return SampleLabelSaveMessageKind::None;
}

SampleLabelSaveMessageKind LegacySaveMessageKind(
    std::string_view message)
{
    if (message ==
        "Output path is already used by another local labeling task.") {
        return SampleLabelSaveMessageKind::OutputPathAlreadyUsed;
    }
    if (message == "Could not save labeling output." ||
        message == "could not save label output" ||
        message ==
            "could not save label output metadata") {
        return SampleLabelSaveMessageKind::OutputSaveFailed;
    }
    return message.empty()
        ? SampleLabelSaveMessageKind::None
        : SampleLabelSaveMessageKind::SystemDetail;
}

std::string_view SaveMessageKindText(
    SampleLabelSaveMessageKind kind)
{
    switch (kind) {
    case SampleLabelSaveMessageKind::OutputPathAlreadyUsed:
        return "output_path_already_used";
    case SampleLabelSaveMessageKind::OutputSaveFailed:
        return "output_save_failed";
    case SampleLabelSaveMessageKind::SystemDetail:
        return "system_detail";
    case SampleLabelSaveMessageKind::None:
    default:
        return "none";
    }
}

std::optional<SampleLabelingOutputArtifactFormat> ParseOutputFormat(
    std::string_view text)
{
    if (text == "none") {
        return SampleLabelingOutputArtifactFormat::None;
    }
    if (text == "legacy_npy_with_sidecar") {
        return SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
    }
    if (text == "canonical_asdf") {
        return SampleLabelingOutputArtifactFormat::CanonicalAsdf;
    }
    return std::nullopt;
}

std::string_view OutputFormatText(
    SampleLabelingOutputArtifactFormat format)
{
    switch (format) {
    case SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar:
        return "legacy_npy_with_sidecar";
    case SampleLabelingOutputArtifactFormat::CanonicalAsdf:
        return "canonical_asdf";
    case SampleLabelingOutputArtifactFormat::None:
    default:
        return "none";
    }
}

bool ParseTaskOutput(
    const JsonValue& task_object,
    int schema_version,
    SampleLabelingTask& task)
{
    if (schema_version < kStateSchemaVersion) {
        const JsonValue* output_path =
            ObjectMember(task_object, "output_path");
        if (output_path == nullptr ||
            output_path->kind == JsonValue::Kind::Null) {
            return true;
        }
        std::optional<std::filesystem::path> path =
            ReadPersistedPathReference(*output_path);
        if (!path || path->empty()) {
            return false;
        }
        task.output_path = std::move(*path);
        task.output_format =
            SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
        return true;
    }

    const JsonValue* output = ObjectMember(task_object, "output");
    if (output == nullptr || output->kind != JsonValue::Kind::Object) {
        return false;
    }
    const std::optional<std::string> format_text =
        ReadStringMember(*output, "format");
    if (!format_text) {
        return false;
    }
    const std::optional<SampleLabelingOutputArtifactFormat> format =
        ParseOutputFormat(*format_text);
    if (!format) {
        return false;
    }
    const JsonValue* output_path = ObjectMember(*output, "path");
    if (output_path == nullptr) {
        return false;
    }
    if (*format == SampleLabelingOutputArtifactFormat::None) {
        if (output_path->kind != JsonValue::Kind::Null) {
            return false;
        }
        task.output_format = *format;
        return true;
    }
    std::optional<std::filesystem::path> path =
        ReadPersistedPathReference(*output_path);
    if (!path || path->empty()) {
        return false;
    }
    task.output_path = std::move(*path);
    task.output_format = *format;
    return true;
}

bool ApplyPendingValues(
    SampleLabelingTask& task,
    const JsonValue& task_object,
    const std::function<void()>& cancellation_checkpoint)
{
    const JsonValue* pending_values = ObjectMember(task_object, "pending_values");
    if (pending_values == nullptr) {
        return true;
    }
    if (pending_values->kind != JsonValue::Kind::Array) {
        return false;
    }

    bool valid = true;
    for (std::size_t index = 0; index < pending_values->array.size(); ++index) {
        if ((index & 0xfffU) == 0U && cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        const JsonValue& pending_object = pending_values->array[index];
        if (pending_object.kind != JsonValue::Kind::Object) {
            valid = false;
            continue;
        }
        const std::optional<std::size_t> sample_index = ReadSizeMember(pending_object, "index");
        const std::optional<int> value = ReadIntMember(pending_object, "value");
        if (!sample_index || !value || *sample_index >= task.values.size()) {
            valid = false;
            continue;
        }
        task.values[*sample_index] = *value;
        task.pending_sample_indices.insert(*sample_index);
    }
    return valid;
}

SampleLabelSet ParseLabelSet(
    const JsonValue& task_object,
    const std::function<void()>& cancellation_checkpoint,
    bool* valid_out)
{
    bool valid = true;
    SampleLabelSet label_set;
    const JsonValue* labels = ObjectMember(task_object, "labels");
    if (labels == nullptr) {
        if (valid_out != nullptr) {
            *valid_out = true;
        }
        return label_set;
    }
    if (labels->kind != JsonValue::Kind::Array) {
        if (valid_out != nullptr) {
            *valid_out = false;
        }
        return label_set;
    }

    for (std::size_t index = 0; index < labels->array.size(); ++index) {
        if ((index & 0xfffU) == 0U && cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        const JsonValue& label_object = labels->array[index];
        if (label_object.kind != JsonValue::Kind::Object) {
            valid = false;
            continue;
        }
        const std::optional<int> code = ReadIntMember(label_object, "code");
        const std::optional<std::string> name = ReadStringMember(label_object, "name");
        const std::optional<std::string> shortcut_text = ReadStringMember(label_object, "shortcut");
        if (!code || !name) {
            valid = false;
            continue;
        }
        const JsonValue* shortcut_value =
            ObjectMember(label_object, "shortcut");
        if (shortcut_value != nullptr &&
            (!shortcut_text || shortcut_text->size() > 1)) {
            valid = false;
            continue;
        }
        const char shortcut =
            shortcut_text && !shortcut_text->empty()
            ? (*shortcut_text)[0]
            : '\0';
        if (!UpsertSampleLabel(
                label_set,
                SampleLabelDefinition{
                    *code,
                    *name,
                    shortcut})) {
            valid = false;
        }
    }
    if (valid_out != nullptr) {
        *valid_out = valid;
    }
    return label_set;
}

struct ParsedTask {
    std::optional<SampleLabelingTask> task;
    bool malformed = false;
};

ParsedTask ParseTask(
    const JsonValue& task_object,
    std::size_t sample_count,
    const std::function<void()>& cancellation_checkpoint,
    bool hydrate_persistent_output,
    int schema_version)
{
    if (task_object.kind != JsonValue::Kind::Object) {
        return {};
    }

    const std::optional<std::string> task_id = ReadStringMember(task_object, "task_id");
    const std::optional<std::string> task_name = ReadStringMember(task_object, "task_name");
    if (!task_id || task_id->empty()) {
        return {};
    }

    bool malformed =
        ObjectMember(task_object, "task_name") != nullptr &&
        !task_name;

    SampleLabelingTask task = CreateSampleLabelingTask(*task_id, task_name.value_or(*task_id), 0);
    task.values.reserve(sample_count);
    constexpr std::size_t kInitializationChunk = 4096U;
    while (task.values.size() < sample_count) {
        if (cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        task.values.resize(
            std::min(sample_count, task.values.size() + kInitializationChunk),
            kUnlabeledSampleLabelCode);
    }
    bool labels_valid = true;
    task.label_set = ParseLabelSet(
        task_object,
        cancellation_checkpoint,
        &labels_valid);
    malformed = malformed || !labels_valid;
    if (const JsonValue* auto_advance =
            ObjectMember(task_object, "auto_advance");
        auto_advance != nullptr &&
        auto_advance->kind != JsonValue::Kind::Bool) {
        malformed = true;
    }
    task.auto_advance = ReadBoolMember(task_object, "auto_advance", false);
    if (const JsonValue* skip_labeled =
            ObjectMember(
                task_object,
                "skip_labeled_on_advance");
        skip_labeled != nullptr &&
        skip_labeled->kind != JsonValue::Kind::Bool) {
        malformed = true;
    }
    task.skip_labeled_on_advance = ReadBoolMember(task_object, "skip_labeled_on_advance", false);
    if (const JsonValue* remembered_value =
            ObjectMember(task_object, "remembered_position");
        remembered_value != nullptr &&
        remembered_value->kind != JsonValue::Kind::Null) {
        const std::optional<std::size_t> remembered =
            ReadSizeMember(
                task_object,
                "remembered_position");
        if (remembered && *remembered < sample_count) {
            task.remembered_position = *remembered;
        } else {
            malformed = true;
        }
    }
    malformed =
        !ParseTaskOutput(task_object, schema_version, task) || malformed;
    bool output_load_failed = false;
    std::string output_load_error;
    bool metadata_load_failed = false;
    bool metadata_retry_pending = false;
    std::string metadata_load_error;
    if (task.output_path && hydrate_persistent_output) {
        std::optional<LoadedSampleLabelResult> loaded =
            SampleAnnotationIoAdapter{}.LoadLabelResult(
                *task.output_path,
                sample_count,
                cancellation_checkpoint,
                &output_load_error);
        if (loaded) {
            task.values = std::move(loaded->values);
        } else {
            output_load_failed = true;
        }
        if (!output_load_failed) {
            if (loaded->metadata) {
                if (loaded->metadata->task_id != task.task_id) {
                    metadata_load_failed = true;
                    metadata_load_error = "metadata task id does not match the local task record";
                }
            } else if (loaded->metadata_sidecar_exists) {
                metadata_load_failed = true;
                metadata_load_error = loaded->metadata_warning.empty()
                    ? "metadata does not match the label output"
                    : loaded->metadata_warning;
            } else {
                metadata_load_failed = true;
                metadata_retry_pending = true;
                metadata_load_error = "metadata sidecar is missing";
            }
        }
        malformed =
            !ApplyPendingValues(
                task,
                task_object,
                cancellation_checkpoint) ||
            malformed;
    } else if (task.output_path) {
        malformed =
            !ApplyPendingValues(
                task,
                task_object,
                cancellation_checkpoint) ||
            malformed;
    } else {
        const JsonValue* values = ObjectMember(task_object, "values");
        if (values == nullptr) {
            malformed = true;
        } else {
            bool values_valid =
                values->kind == JsonValue::Kind::Array &&
                values->array.size() == sample_count;
            if (values->kind == JsonValue::Kind::Array && values->array.size() == sample_count) {
                std::vector<int> parsed_values;
                parsed_values.reserve(values->array.size());
                bool all_ints = true;
                for (std::size_t index = 0; index < values->array.size(); ++index) {
                    if ((index & 0xfffU) == 0U && cancellation_checkpoint) {
                        cancellation_checkpoint();
                    }
                    const JsonValue& value = values->array[index];
                    if (value.kind != JsonValue::Kind::Integer ||
                        value.integer_value < std::numeric_limits<int>::min() ||
                        value.integer_value > std::numeric_limits<int>::max()) {
                        all_ints = false;
                        values_valid = false;
                        break;
                    }
                    parsed_values.push_back(static_cast<int>(value.integer_value));
                }
                if (all_ints) {
                    task.values = std::move(parsed_values);
                }
            }
            malformed = malformed || !values_valid;
        }
    }
    task.save_state.kind = task.output_path ? SampleLabelSaveStateKind::AutosavedToOutput
                                            : SampleLabelSaveStateKind::InternalDraftOnly;
    if (const JsonValue* save_state_value =
            ObjectMember(task_object, "save_state");
        save_state_value != nullptr) {
        const std::optional<std::string> save_state =
            ReadStringMember(
                task_object,
                "save_state");
        if (!save_state ||
            SaveStateKindText(
                ParseSaveStateKind(*save_state)) !=
                *save_state) {
            malformed = true;
        } else {
            task.save_state.kind =
                ParseSaveStateKind(*save_state);
        }
    }
    if (const JsonValue* save_message_value =
            ObjectMember(task_object, "save_message");
        save_message_value != nullptr) {
        const std::optional<std::string> save_message =
            ReadStringMember(
                task_object,
                "save_message");
        if (!save_message) {
            malformed = true;
        } else {
            task.save_state.message =
                *save_message;
        }
    }
    if (const JsonValue* save_message_kind_value =
            ObjectMember(
                task_object,
                "save_message_kind");
        save_message_kind_value != nullptr) {
        const std::optional<std::string> save_message_kind =
            ReadStringMember(
                task_object,
                "save_message_kind");
        if (!save_message_kind ||
            SaveMessageKindText(
                ParseSaveMessageKind(
                    *save_message_kind)) !=
                *save_message_kind) {
            malformed = true;
        } else {
            task.save_state.message_kind =
                ParseSaveMessageKind(
                    *save_message_kind);
            if (task.save_state.message_kind ==
                    SampleLabelSaveMessageKind::None &&
                !task.save_state.message.empty()) {
                task.save_state.message_kind =
                    LegacySaveMessageKind(
                        task.save_state.message);
            }
        }
    } else {
        task.save_state.message_kind =
            LegacySaveMessageKind(
                task.save_state.message);
    }
    if (const JsonValue* metadata_pending =
            ObjectMember(task_object, "metadata_pending");
        metadata_pending != nullptr &&
        metadata_pending->kind != JsonValue::Kind::Bool) {
        malformed = true;
    }
    task.metadata_save_pending =
        ReadBoolMember(task_object, "metadata_pending", false) || metadata_retry_pending;
    if (output_load_failed) {
        task.save_state.kind = SampleLabelSaveStateKind::Failed;
        if (task.save_state.message.empty()) {
            task.save_state.message =
                output_load_error.empty() ? "could not read label output" : "could not read label output: " + output_load_error;
        }
        task.save_state.message_kind =
            SampleLabelSaveMessageKind::SystemDetail;
    } else if (metadata_load_failed) {
        task.save_state.kind = SampleLabelSaveStateKind::Failed;
        if (task.save_state.message.empty()) {
            task.save_state.message = metadata_load_error.empty()
                ? "could not read label output metadata"
                : "could not read label output metadata: " + metadata_load_error;
        }
        task.save_state.message_kind =
            SampleLabelSaveMessageKind::SystemDetail;
    }
    task.save_state.pending_count = task.pending_sample_indices.size();
    if ((!task.pending_sample_indices.empty() || task.metadata_save_pending) &&
        task.save_state.kind != SampleLabelSaveStateKind::Failed) {
        task.save_state.kind = SampleLabelSaveStateKind::Pending;
    } else if (!task.output_path && task.save_state.kind == SampleLabelSaveStateKind::Pending) {
        task.save_state.kind = SampleLabelSaveStateKind::InternalDraftOnly;
    }
    RebuildSampleLabelingTaskStatistics(task, cancellation_checkpoint);
    return ParsedTask{
        .task = std::move(task),
        .malformed = malformed};
}

void SetError(
    std::string* error_message,
    std::string message)
{
    if (error_message != nullptr) {
        *error_message = std::move(message);
    }
}

bool ValidateOutputOwnership(
    const SampleLabelingStateCache& cache,
    std::string* error_message)
{
    for (const auto& [source_identity, state] : cache.sources) {
        (void)source_identity;
        for (const SampleLabelingTask& task : state.tasks) {
            if (!task.output_path) {
                if (task.output_format ==
                    SampleLabelingOutputArtifactFormat::None) {
                    continue;
                }
                SetError(
                    error_message,
                    "sample-labeling cache contains an output format without a path");
                return false;
            }
            if ((task.output_format !=
                     SampleLabelingOutputArtifactFormat::CanonicalAsdf &&
                 task.output_format !=
                     SampleLabelingOutputArtifactFormat::
                         LegacyNpyWithSidecar) ||
                task.output_path->empty() ||
                SampleAnnotationArtifactIdentities(
                    *task.output_path,
                    task.output_format,
                    false)
                    .stable_path_keys.empty()) {
                SetError(
                    error_message,
                    "sample-labeling cache contains an invalid output path or format");
                return false;
            }
        }
    }
    return true;
}

bool ValidateCacheStructure(
    const SampleLabelingStateCache& cache,
    std::string* error_message)
{
    for (const auto& [source_identity, state] : cache.sources) {
        if (source_identity.empty() || state.sample_count == 0) {
            SetError(
                error_message,
                "sample-labeling cache contains an invalid source identity or sample count");
            return false;
        }

        std::unordered_set<std::string> task_ids;
        for (const SampleLabelingTask& task : state.tasks) {
            if (task.task_id.empty() ||
                !task_ids.insert(task.task_id).second) {
                SetError(
                    error_message,
                    "sample-labeling cache contains duplicate or empty task ids");
                return false;
            }
            if (task.values.size() != state.sample_count) {
                SetError(
                    error_message,
                    "sample-labeling cache task sample count does not match its source");
                return false;
            }
        }
        if (state.active_task_id &&
            !task_ids.contains(*state.active_task_id)) {
            SetError(
                error_message,
                "sample-labeling cache active task does not exist");
            return false;
        }
    }
    return ValidateOutputOwnership(cache, error_message);
}

using OutputOwnerSet =
    std::unordered_set<std::string>;
using OutputOwnersByPath =
    std::unordered_map<std::string, OutputOwnerSet>;

std::vector<std::string> OutputArtifactOwnerKeys(
    const std::filesystem::path& output_path,
    SampleLabelingOutputArtifactFormat format,
    bool resolve_physical_paths)
{
    const SampleAnnotationArtifactIdentitySet identities =
        SampleAnnotationArtifactIdentities(
            output_path,
            format,
            resolve_physical_paths);
    std::vector<std::string> keys;
    keys.reserve(
        identities.stable_path_keys.size() +
        identities.physical_path_keys.size());
    for (const std::string& key :
         identities.stable_path_keys) {
        keys.push_back("stable\n" + key);
    }
    for (const std::string& key :
         identities.physical_path_keys) {
        keys.push_back("physical\n" + key);
    }
    return keys;
}

std::string OutputOwnerKey(
    std::string_view source_identity,
    std::string_view task_id)
{
    return std::to_string(source_identity.size()) +
        ":" + std::string(source_identity) +
        std::string(task_id);
}

OutputOwnersByPath CollectOutputOwners(
    const SampleLabelingStateCache& cache,
    bool resolve_physical_paths)
{
    OutputOwnersByPath owners;
    for (const auto& [source_identity, state] :
         cache.sources) {
        for (const SampleLabelingTask& task :
             state.tasks) {
            if (!task.output_path) {
                continue;
            }
            const std::string owner = OutputOwnerKey(
                source_identity,
                task.task_id);
            for (const std::string& artifact_key :
                 OutputArtifactOwnerKeys(
                     *task.output_path,
                     task.output_format,
                     resolve_physical_paths)) {
                owners[artifact_key].insert(owner);
            }
        }
    }
    return owners;
}

bool ValidateNoNewOutputConflicts(
    const OutputOwnersByPath& existing_owners,
    const SampleLabelingStateCache& merged,
    bool resolve_physical_paths,
    std::string* error_message)
{
    const OutputOwnersByPath merged_owners =
        CollectOutputOwners(
            merged,
            resolve_physical_paths);
    for (const auto& [output_key, owners] :
         merged_owners) {
        if (owners.size() <= 1) {
            continue;
        }
        const auto existing =
            existing_owners.find(output_key);
        for (const std::string& owner : owners) {
            if (existing == existing_owners.end() ||
                !existing->second.contains(owner)) {
                SetError(
                    error_message,
                    "sample-labeling cache patch would introduce a duplicate output path");
                return false;
            }
        }
    }
    return true;
}

bool ApplyPatch(
    SampleLabelingStateCache& cache,
    const SampleLabelingStateCachePatch& patch,
    std::string* error_message)
{
    if (!ValidateCacheStructure(cache, error_message)) {
        return false;
    }
    const bool output_target_changes =
        std::any_of(
            patch.sources.begin(),
            patch.sources.end(),
            [&cache](const auto& entry) {
                const auto source =
                    cache.sources.find(entry.first);
                for (const SampleLabelingTask& task :
                     entry.second.task_upserts) {
                    if (source == cache.sources.end()) {
                        return task.output_path.has_value();
                    }
                    const auto existing = std::find_if(
                        source->second.tasks.begin(),
                        source->second.tasks.end(),
                        [&task](const SampleLabelingTask& candidate) {
                            return candidate.task_id ==
                                task.task_id;
                        });
                    if (existing == source->second.tasks.end()) {
                        if (task.output_path) {
                            return true;
                        }
                        continue;
                    }
                    if (existing->output_path != task.output_path ||
                        existing->output_format != task.output_format) {
                        return true;
                    }
                }
                return false;
            });
    const OutputOwnersByPath existing_output_owners =
        CollectOutputOwners(
            cache,
            output_target_changes);
    for (const auto& [source_identity, source_patch] :
         patch.sources) {
        if (source_identity.empty()) {
            SetError(
                error_message,
                "sample-labeling cache patch contains an empty source identity");
            return false;
        }
        SampleLabelingSourceState& state =
            cache.sources[source_identity];
        std::unordered_set<std::string>
            existing_temporary_task_ids;
        for (const SampleLabelingTask& task : state.tasks) {
            if (!task.output_path) {
                existing_temporary_task_ids.insert(
                    task.task_id);
            }
        }
        if (source_patch.metadata) {
            state.sample_count =
                source_patch.metadata->sample_count;
            state.source_name =
                source_patch.metadata->source_name;
            state.source_fingerprint =
                source_patch.metadata->source_fingerprint;
            state.context_fingerprint =
                source_patch.metadata->context_fingerprint;
        }

        std::unordered_set<std::string> tombstones;
        for (const std::string& task_id :
             source_patch.task_tombstones) {
            if (task_id.empty() ||
                !tombstones.insert(task_id).second) {
                SetError(
                    error_message,
                    "sample-labeling cache patch contains duplicate or empty tombstones");
                return false;
            }
        }
        for (const std::string& task_id :
             source_patch.task_ids_expected_absent) {
            if (task_id.empty() ||
                tombstones.contains(task_id) ||
                std::count_if(
                    source_patch.task_upserts.begin(),
                    source_patch.task_upserts.end(),
                    [&task_id](const SampleLabelingTask& task) {
                        return task.task_id == task_id;
                    }) != 1) {
                SetError(
                    error_message,
                    "sample-labeling cache patch contains an invalid expected-absent task operation");
                return false;
            }
            if (std::any_of(
                    state.tasks.begin(),
                    state.tasks.end(),
                    [&task_id](const SampleLabelingTask& task) {
                        return task.task_id == task_id;
                    })) {
                SetError(
                    error_message,
                    "sample-labeling cache create expected the task id to be absent");
                return false;
            }
        }
        if (state.active_task_id &&
            tombstones.contains(*state.active_task_id)) {
            state.active_task_id.reset();
        }
        state.tasks.erase(
            std::remove_if(
                state.tasks.begin(),
                state.tasks.end(),
                [&tombstones](const SampleLabelingTask& task) {
                    return tombstones.contains(task.task_id);
                }),
            state.tasks.end());

        std::unordered_set<std::string> upsert_ids;
        for (const SampleLabelingTask& task :
             source_patch.task_upserts) {
            if (task.task_id.empty() ||
                tombstones.contains(task.task_id) ||
                !upsert_ids.insert(task.task_id).second) {
                SetError(
                    error_message,
                    "sample-labeling cache patch contains conflicting task operations");
                return false;
            }
            const auto existing =
                std::find_if(
                    state.tasks.begin(),
                    state.tasks.end(),
                    [&task](const SampleLabelingTask& candidate) {
                        return candidate.task_id ==
                            task.task_id;
                    });
            if (existing == state.tasks.end()) {
                state.tasks.push_back(task);
            } else {
                *existing = task;
            }
        }
        if (source_patch.active_task_selection_changed) {
            state.active_task_id =
                source_patch.active_task_id;
        }

        std::size_t temporary_task_count = 0;
        bool introduced_temporary_task = false;
        for (const SampleLabelingTask& task : state.tasks) {
            if (task.output_path) {
                continue;
            }
            ++temporary_task_count;
            introduced_temporary_task =
                introduced_temporary_task ||
                !existing_temporary_task_ids.contains(
                    task.task_id);
        }
        if (temporary_task_count > 1 &&
            introduced_temporary_task) {
            SetError(
                error_message,
                "sample-labeling cache patch would introduce another temporary task for a source");
            return false;
        }
    }
    return ValidateCacheStructure(cache, error_message) &&
        ValidateNoNewOutputConflicts(
            existing_output_owners,
            cache,
            output_target_changes,
            error_message);
}

}  // namespace

std::filesystem::path DefaultSampleLabelingStateCachePath()
{
    return DefaultLocalUserStatePath(
        local_user_state_paths::kSampleLabelingState);
}

std::filesystem::path
SampleLabelingStateCoordinationDirectory(
    const std::filesystem::path& state_cache_path)
{
    const std::vector<std::filesystem::path> directories =
        SampleLabelingStateCoordinationDirectories(
            state_cache_path);
    return directories.empty()
        ? std::filesystem::path{}
        : directories.front();
}

std::vector<std::filesystem::path>
SampleLabelingStateCoordinationDirectories(
    const std::filesystem::path& state_cache_path)
{
    std::vector<std::filesystem::path> directories;
    if (state_cache_path.empty()) {
        return directories;
    }
    std::error_code parent_error;
    const std::filesystem::path parent =
        state_cache_path.parent_path();
    if (!parent.empty() &&
        std::filesystem::exists(parent, parent_error) &&
        !std::filesystem::is_directory(parent, parent_error)) {
        return directories;
    }
    if (parent_error) {
        return directories;
    }
    const std::string stable_identity =
        SourcePathIdentityKey(state_cache_path);
    if (stable_identity.empty()) {
        return directories;
    }

    // The adjacent path is the mandatory normalized-path lock. It is
    // deliberately computed without physical probing, so the first writer
    // uses the same namespace before and after the cache is created and the
    // existing long-path behavior remains available.
    std::error_code absolute_error;
    std::filesystem::path normalized_cache_path =
        state_cache_path.is_absolute()
        ? state_cache_path
        : std::filesystem::absolute(
              state_cache_path,
              absolute_error);
    if (absolute_error) {
        normalized_cache_path = state_cache_path;
    }
    normalized_cache_path =
        normalized_cache_path.lexically_normal();
    const std::filesystem::path baseline =
        normalized_cache_path.parent_path() /
        (normalized_cache_path.filename().string() + ".locks");
    if (baseline.empty()) {
        return directories;
    }
    directories.push_back(baseline);

    // Physical identities are optional alias locks. A probe failure must not
    // remove the normalized baseline or make single-instance labeling
    // unusable; only resolved identities add extra namespaces.
    const std::vector<std::string> physical_identities =
        OutputPathIdentityKeys(state_cache_path);
    std::error_code temp_error;
    const std::filesystem::path coordination_root =
        std::filesystem::temp_directory_path(temp_error) /
        "SpecForge" /
        "sample-labeling-cache-locks";
    if (!temp_error) {
        std::vector<std::filesystem::path> aliases;
        aliases.reserve(physical_identities.size());
        for (const std::string& physical_identity :
             physical_identities) {
            StableSha256 digest;
            digest.Append(
                "sample-labeling-cache-alias-lock-v1\n");
            digest.Append(physical_identity);
            aliases.push_back(
                coordination_root /
                digest.FinishHex());
        }
        std::sort(
            aliases.begin(),
            aliases.end(),
            [](const std::filesystem::path& left,
               const std::filesystem::path& right) {
                return left.generic_wstring() <
                    right.generic_wstring();
            });
        aliases.erase(
            std::unique(
                aliases.begin(),
                aliases.end()),
            aliases.end());
        directories.insert(
            directories.end(),
            aliases.begin(),
            aliases.end());
    }
    return directories;
}

SampleLabelingStateCacheLoadResult LoadSampleLabelingStateCache(
    const std::filesystem::path& path,
    const std::function<void()>& cancellation_checkpoint,
    SampleLabelingStateCacheLoadPolicy policy)
{
    SampleLabelingStateCacheLoadResult result;
    VersionedJsonCacheLoadResult cache =
        LoadVersionedJsonCacheFile(
            path,
            kStateFormatKind,
            {1, 2, kStateSchemaVersion},
            "sample-labeling task record",
            cancellation_checkpoint);
    switch (cache.issue_kind) {
    case VersionedJsonCacheLoadIssueKind::None:
        break;
    case VersionedJsonCacheLoadIssueKind::ReadFailed:
        result.issue_kind =
            SampleLabelingStateCacheLoadIssueKind::
                ReadFailed;
        break;
    case VersionedJsonCacheLoadIssueKind::InvalidDocument:
        result.issue_kind =
            SampleLabelingStateCacheLoadIssueKind::
                InvalidDocument;
        break;
    case VersionedJsonCacheLoadIssueKind::UnsupportedFormatOrSchema:
        result.issue_kind =
            SampleLabelingStateCacheLoadIssueKind::
                UnsupportedFormatOrSchema;
        break;
    }
    result.diagnostic_detail =
        std::move(cache.diagnostic_detail);
    if (!cache.document) {
        result.warning = std::move(cache.warning);
        return result;
    }

    const JsonValue* sources = ObjectMember(cache.document->root, "sources");
    if (sources == nullptr || sources->kind != JsonValue::Kind::Array) {
        result.warning =
            "Ignored invalid sample-labeling task record.";
        result.issue_kind =
            SampleLabelingStateCacheLoadIssueKind::
                InvalidDocument;
        result.diagnostic_detail =
            "sources must be an array";
        return result;
    }

    for (std::size_t source_index = 0; source_index < sources->array.size(); ++source_index) {
        if (cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        const JsonValue& source_object = sources->array[source_index];
        if (source_object.kind != JsonValue::Kind::Object) {
            result.warning =
                "Ignored invalid sample-labeling task record entries.";
            result.issue_kind =
                SampleLabelingStateCacheLoadIssueKind::
                    InvalidDocument;
            if (result.diagnostic_detail.empty()) {
                result.diagnostic_detail =
                    "source entry is not an object";
            }
            continue;
        }
        const std::optional<std::string> identity = ReadStringMember(source_object, "identity");
        const std::optional<std::size_t> sample_count = ReadSizeMember(source_object, "sample_count");
        if (!identity || identity->empty() || !sample_count || *sample_count == 0) {
            result.warning =
                "Ignored invalid sample-labeling task record entries.";
            result.issue_kind =
                SampleLabelingStateCacheLoadIssueKind::
                    InvalidDocument;
            result.diagnostic_detail =
                "source entry has an invalid identity or sample count";
            continue;
        }

        const bool legacy_identity = IsLegacySourceCollectionIdentity(*identity);
        SampleLabelingSourceState state;
        state.sample_count = *sample_count;
        const auto mark_malformed_source_field =
            [&result](std::string_view field) {
                result.warning =
                    "Ignored invalid sample-labeling task record entries.";
                result.issue_kind =
                    SampleLabelingStateCacheLoadIssueKind::
                        InvalidDocument;
                if (result.diagnostic_detail.empty()) {
                    result.diagnostic_detail =
                        "source field '" +
                        std::string(field) +
                        "' has an invalid type";
                }
            };
        for (const std::string_view field : {
                 std::string_view{"source_name"},
                 std::string_view{"source_fingerprint"},
                 std::string_view{"context_fingerprint"},
                 std::string_view{"active_task_id"}}) {
            const JsonValue* member =
                ObjectMember(source_object, field);
            if (member != nullptr &&
                member->kind != JsonValue::Kind::String) {
                mark_malformed_source_field(field);
            }
        }
        state.source_name = ReadStringMember(source_object, "source_name").value_or("");
        state.source_fingerprint = NormalizeLegacySourceCollectionFingerprint(
            ReadStringMember(source_object, "source_fingerprint").value_or(""),
            legacy_identity);
        state.context_fingerprint = NormalizeLegacySourceCollectionFingerprint(
            ReadStringMember(source_object, "context_fingerprint").value_or(""),
            legacy_identity);
        if (std::optional<std::string> active_task_id = ReadStringMember(source_object, "active_task_id");
            active_task_id && !active_task_id->empty()) {
            state.active_task_id = std::move(*active_task_id);
        }
        std::unordered_set<std::string> parsed_task_ids;
        const JsonValue* tasks = ObjectMember(source_object, "tasks");
        if (tasks == nullptr) {
            mark_malformed_source_field("tasks");
        } else if (tasks->kind != JsonValue::Kind::Array) {
                result.warning =
                    "Ignored invalid sample-labeling task record entries.";
                result.issue_kind =
                    SampleLabelingStateCacheLoadIssueKind::
                        InvalidDocument;
                if (result.diagnostic_detail.empty()) {
                    result.diagnostic_detail =
                        "tasks must be an array";
                }
        } else {
            for (const JsonValue& task_object : tasks->array) {
                    if (cancellation_checkpoint) {
                        cancellation_checkpoint();
                    }
                    ParsedTask parsed = ParseTask(
                        task_object,
                        state.sample_count,
                        cancellation_checkpoint,
                        policy !=
                            SampleLabelingStateCacheLoadPolicy::
                                AllowPersistentOutputsWithoutResultHydration &&
                            policy !=
                                SampleLabelingStateCacheLoadPolicy::
                                    InternalDraftsOnly,
                        cache.document->schema_version);
                    if (policy ==
                            SampleLabelingStateCacheLoadPolicy::
                                InternalDraftsOnly &&
                        parsed.task && parsed.task->output_path) {
                        result.cache = {};
                        result.warning =
                            "Persistent labeling output paths are not permitted in an automation state seed.";
                        return result;
                    }
                    if (parsed.task) {
                        const bool duplicate_task_id =
                            !parsed_task_ids
                                 .insert(parsed.task->task_id)
                                 .second;
                        state.tasks.push_back(
                            std::move(*parsed.task));
                        if (duplicate_task_id) {
                            result.warning =
                                "Ignored invalid sample-labeling task record entries.";
                            result.issue_kind =
                                SampleLabelingStateCacheLoadIssueKind::
                                    InvalidDocument;
                            if (result.diagnostic_detail.empty()) {
                                result.diagnostic_detail =
                                    "source contains duplicate task ids";
                            }
                        }
                    }
                    if (!parsed.task || parsed.malformed) {
                        result.warning =
                            "Ignored invalid sample-labeling task record entries.";
                        result.issue_kind =
                            SampleLabelingStateCacheLoadIssueKind::
                                InvalidDocument;
                        if (result.diagnostic_detail.empty()) {
                            result.diagnostic_detail =
                                parsed.task
                                ? "task entry contains malformed fields"
                                : "task entry is invalid";
                        }
                    }
            }
        }
        if (state.active_task_id && std::none_of(state.tasks.begin(), state.tasks.end(), [&state](const auto& task) {
                return task.task_id == *state.active_task_id;
            })) {
            mark_malformed_source_field(
                "active_task_id");
            state.active_task_id.reset();
        }
        if (!result.cache.sources
                 .emplace(
                     NormalizePersistedSourceCollectionIdentity(
                         std::move(*identity)),
                     std::move(state))
                 .second) {
            result.warning =
                "Ignored duplicate sample-labeling source records.";
            result.issue_kind =
                SampleLabelingStateCacheLoadIssueKind::
                    InvalidDocument;
            result.diagnostic_detail =
                "duplicate normalized source identity";
        }
    }
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
    return result;
}

bool SaveSampleLabelingStateCache(
    const std::filesystem::path& path,
    const SampleLabelingStateCache& cache,
    std::string* error_message)
{
    if (path.empty()) {
        return false;
    }
    if (!ValidateOutputOwnership(cache, error_message)) {
        return false;
    }

    const std::vector<std::string> keys = SortedCacheKeys(cache.sources);

    return WriteVersionedJsonCacheFile(
        path,
        kStateFormatKind,
        kStateSchemaVersion,
        "sample-labeling task record",
        [&](std::ostream& stream, std::string&) {
            stream << ",\n";
            stream << "  \"sources\": [";
            if (!keys.empty()) {
                stream << "\n";
            }

            for (std::size_t source_index = 0; source_index < keys.size(); ++source_index) {
                const std::string& key = keys[source_index];
                const SampleLabelingSourceState& state = cache.sources.at(key);
                stream << "    {\n";
                stream << "      \"identity\": ";
                WriteJsonString(stream, key);
                stream << ",\n";
                stream << "      \"sample_count\": " << state.sample_count << ",\n";
                stream << "      \"source_name\": ";
                WriteJsonString(stream, state.source_name);
                stream << ",\n";
                stream << "      \"source_fingerprint\": ";
                WriteJsonString(stream, state.source_fingerprint);
                stream << ",\n";
                stream << "      \"context_fingerprint\": ";
                WriteJsonString(stream, state.context_fingerprint);
                stream << ",\n";
                stream << "      \"active_task_id\": ";
                WriteJsonString(stream, state.active_task_id.value_or(""));
                stream << ",\n";
                stream << "      \"tasks\": [";
                if (!state.tasks.empty()) {
                    stream << "\n";
                }
                for (std::size_t task_index = 0; task_index < state.tasks.size(); ++task_index) {
                    const SampleLabelingTask& task = state.tasks[task_index];
                    stream << "        {\n";
                    stream << "          \"task_id\": ";
                    WriteJsonString(stream, task.task_id);
                    stream << ",\n";
                    stream << "          \"task_name\": ";
                    WriteJsonString(stream, task.task_name);
                    stream << ",\n";
                    stream << "          \"auto_advance\": " << (task.auto_advance ? "true" : "false") << ",\n";
                    stream << "          \"skip_labeled_on_advance\": "
                           << (task.skip_labeled_on_advance ? "true" : "false") << ",\n";
                    stream << "          \"remembered_position\": ";
                    if (task.remembered_position) {
                        stream << *task.remembered_position;
                    } else {
                        stream << "null";
                    }
                    stream << ",\n";
                    stream << "          \"output\": {\n";
                    stream << "            \"path\": ";
                    if (task.output_path) {
                        WritePersistedPathReference(stream, *task.output_path);
                    } else {
                        stream << "null";
                    }
                    stream << ",\n";
                    stream << "            \"format\": ";
                    WriteJsonString(
                        stream,
                        OutputFormatText(task.output_format));
                    stream << "\n";
                    stream << "          },\n";
                    stream << "          \"labels\": [";
                    if (!task.label_set.labels.empty()) {
                        stream << "\n";
                    }
                    for (std::size_t label_index = 0; label_index < task.label_set.labels.size(); ++label_index) {
                        const SampleLabelDefinition& label = task.label_set.labels[label_index];
                        stream << "            { \"code\": " << label.code << ", \"name\": ";
                        WriteJsonString(stream, label.name);
                        stream << ", \"shortcut\": ";
                        const std::string shortcut =
                            label.shortcut == '\0' ? std::string{} : std::string(1, label.shortcut);
                        WriteJsonString(stream, shortcut);
                        stream << " }" << (label_index + 1 == task.label_set.labels.size() ? "\n" : ",\n");
                    }
                    if (!task.label_set.labels.empty()) {
                        stream << "          ";
                    }
                    stream << "],\n";
                    stream << "          \"save_state\": ";
                    WriteJsonString(stream, SaveStateKindText(task.save_state.kind));
                    stream << ",\n";
                    stream << "          \"save_message\": ";
                    WriteJsonString(stream, task.save_state.message);
                    stream << ",\n";
                    stream << "          \"save_message_kind\": ";
                    WriteJsonString(
                        stream,
                        SaveMessageKindText(
                            task.save_state.message_kind));
                    if (task.output_path && task.metadata_save_pending) {
                        stream << ",\n";
                        stream << "          \"metadata_pending\": true";
                    }
                    if (!task.output_path) {
                        stream << ",\n";
                        stream << "          \"values\": [";
                        for (std::size_t value_index = 0; value_index < task.values.size(); ++value_index) {
                            stream << task.values[value_index];
                            if (value_index + 1 != task.values.size()) {
                                stream << ", ";
                            }
                        }
                        stream << "]";
                    } else if (!task.pending_sample_indices.empty()) {
                        std::vector<std::size_t> pending_indices(
                            task.pending_sample_indices.begin(),
                            task.pending_sample_indices.end());
                        std::sort(pending_indices.begin(), pending_indices.end());

                        stream << ",\n";
                        stream << "          \"pending_values\": [";
                        bool wrote_pending_value = false;
                        for (const std::size_t sample_index : pending_indices) {
                            if (sample_index >= task.values.size()) {
                                continue;
                            }
                            if (wrote_pending_value) {
                                stream << ", ";
                            }
                            stream << "{ \"index\": " << sample_index << ", \"value\": " << task.values[sample_index]
                                   << " }";
                            wrote_pending_value = true;
                        }
                        stream << "]";
                    }
                    stream << "\n";
                    stream << "        }" << (task_index + 1 == state.tasks.size() ? "\n" : ",\n");
                }
                if (!state.tasks.empty()) {
                    stream << "      ";
                }
                stream << "]\n";
                stream << "    }" << (source_index + 1 == keys.size() ? "\n" : ",\n");
            }
            if (!keys.empty()) {
                stream << "  ";
            }
            stream << "]\n";
            return true;
        },
        error_message);
}

bool CommitSampleLabelingStateCachePatch(
    const std::filesystem::path& path,
    const SampleLabelingStateCachePatch& patch,
    std::string* error_message,
    std::chrono::milliseconds commit_lock_wait)
{
    if (error_message != nullptr) {
        error_message->clear();
    }
    if (path.empty()) {
        SetError(
            error_message,
            "sample-labeling cache path is empty");
        return false;
    }
    const std::vector<std::filesystem::path> coordination_directories =
        SampleLabelingStateCoordinationDirectories(path);
    if (coordination_directories.empty()) {
        SetError(
            error_message,
            "could not resolve sample-labeling cache coordination identity");
        return false;
    }
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::max(
            commit_lock_wait,
            std::chrono::milliseconds::zero());
    ExclusiveFileLeaseAcquireStatus commit_status =
        ExclusiveFileLeaseAcquireStatus::Failed;
    std::string commit_error;
    std::vector<ExclusiveFileLease> commit_locks;
    do {
        commit_locks.clear();
        commit_status =
            ExclusiveFileLeaseAcquireStatus::Acquired;
        commit_error.clear();
        for (const std::filesystem::path& coordination_directory :
             coordination_directories) {
            ExclusiveFileLeaseAcquireResult commit_lock =
                TryAcquireExclusiveFileLease(
                    coordination_directory /
                    "cache-commit.lock");
            if (commit_lock.status !=
                ExclusiveFileLeaseAcquireStatus::Acquired) {
                commit_status = commit_lock.status;
                commit_error = std::move(commit_lock.error);
                break;
            }
            commit_locks.push_back(
                std::move(commit_lock.lease));
        }
        if (commit_status !=
                ExclusiveFileLeaseAcquireStatus::Unavailable ||
            std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        // Never hold an earlier lock while waiting for a later alias lock;
        // every contender acquires this same ordered set from scratch.
        commit_locks.clear();
        std::this_thread::sleep_for(
            std::chrono::milliseconds{10});
    } while (true);
    if (commit_status !=
        ExclusiveFileLeaseAcquireStatus::Acquired) {
        SetError(
            error_message,
            commit_status ==
                    ExclusiveFileLeaseAcquireStatus::Unavailable
                ? "sample-labeling cache is being committed by another process"
                : "could not acquire sample-labeling cache commit lock: " +
                    commit_error);
        return false;
    }

    SampleLabelingStateCacheLoadResult latest =
        LoadSampleLabelingStateCache(
            path,
            {},
            SampleLabelingStateCacheLoadPolicy::
                AllowPersistentOutputsWithoutResultHydration);
    if (latest.issue_kind !=
        SampleLabelingStateCacheLoadIssueKind::None) {
        std::string message =
            latest.warning.empty()
            ? "could not trust the latest sample-labeling cache"
            : latest.warning;
        if (!latest.diagnostic_detail.empty()) {
            message += ": " + latest.diagnostic_detail;
        }
        SetError(error_message, std::move(message));
        return false;
    }
    if (!ApplyPatch(latest.cache, patch, error_message)) {
        return false;
    }
    return SaveSampleLabelingStateCache(
        path,
        latest.cache,
        error_message);
}

bool HasSampleLabelingOutputPathConflict(
    const SampleLabelingStateCache& cache,
    const SampleLabelingTask& candidate,
    std::string_view source_identity)
{
    if (!candidate.output_path) {
        return false;
    }
    std::unordered_set<std::string> requested_keys;
    for (std::string key :
         OutputArtifactOwnerKeys(
             *candidate.output_path,
             candidate.output_format,
             true)) {
        requested_keys.insert(std::move(key));
    }
    for (const auto& [candidate_source, state] :
         cache.sources) {
        for (const SampleLabelingTask& task : state.tasks) {
            if (!task.output_path) {
                continue;
            }
            if (candidate_source == source_identity &&
                task.task_id == candidate.task_id) {
                continue;
            }
            for (const std::string& candidate_key :
                 OutputArtifactOwnerKeys(
                     *task.output_path,
                     task.output_format,
                     true)) {
                if (requested_keys.contains(
                        candidate_key)) {
                    return true;
                }
            }
        }
    }
    return false;
}

}  // namespace specforge
