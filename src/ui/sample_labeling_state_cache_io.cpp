#include "ui/sample_labeling_state_cache_io.h"

#include "app/local_user_state.h"
#include "app/local_user_state_json.h"
#include "app/local_user_state_paths.h"
#include "domain/sample_annotation_io.h"
#include "domain/source_collection_identity_digest.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace specforge {
namespace {

constexpr const char* kStateFormatKind = "specforge.sample_labeling_tasks.cache";
constexpr int kStateSchemaVersion = 2;

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

void ApplyPendingValues(
    SampleLabelingTask& task,
    const JsonValue& task_object,
    const std::function<void()>& cancellation_checkpoint)
{
    const JsonValue* pending_values = ObjectMember(task_object, "pending_values");
    if (pending_values == nullptr || pending_values->kind != JsonValue::Kind::Array) {
        return;
    }

    for (std::size_t index = 0; index < pending_values->array.size(); ++index) {
        if ((index & 0xfffU) == 0U && cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        const JsonValue& pending_object = pending_values->array[index];
        if (pending_object.kind != JsonValue::Kind::Object) {
            continue;
        }
        const std::optional<std::size_t> sample_index = ReadSizeMember(pending_object, "index");
        const std::optional<int> value = ReadIntMember(pending_object, "value");
        if (!sample_index || !value || *sample_index >= task.values.size()) {
            continue;
        }
        task.values[*sample_index] = *value;
        task.pending_sample_indices.insert(*sample_index);
    }
}

SampleLabelSet ParseLabelSet(
    const JsonValue& task_object,
    const std::function<void()>& cancellation_checkpoint)
{
    SampleLabelSet label_set;
    const JsonValue* labels = ObjectMember(task_object, "labels");
    if (labels == nullptr || labels->kind != JsonValue::Kind::Array) {
        return label_set;
    }

    for (std::size_t index = 0; index < labels->array.size(); ++index) {
        if ((index & 0xfffU) == 0U && cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        const JsonValue& label_object = labels->array[index];
        if (label_object.kind != JsonValue::Kind::Object) {
            continue;
        }
        const std::optional<int> code = ReadIntMember(label_object, "code");
        const std::optional<std::string> name = ReadStringMember(label_object, "name");
        const std::optional<std::string> shortcut_text = ReadStringMember(label_object, "shortcut");
        if (!code || !name) {
            continue;
        }
        const char shortcut = shortcut_text && !shortcut_text->empty() ? (*shortcut_text)[0] : '\0';
        (void)UpsertSampleLabel(label_set, SampleLabelDefinition{*code, *name, shortcut});
    }
    return label_set;
}

std::optional<SampleLabelingTask> ParseTask(
    const JsonValue& task_object,
    std::size_t sample_count,
    const std::function<void()>& cancellation_checkpoint)
{
    if (task_object.kind != JsonValue::Kind::Object) {
        return std::nullopt;
    }

    const std::optional<std::string> task_id = ReadStringMember(task_object, "task_id");
    const std::optional<std::string> task_name = ReadStringMember(task_object, "task_name");
    if (!task_id || task_id->empty()) {
        return std::nullopt;
    }

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
    task.label_set = ParseLabelSet(task_object, cancellation_checkpoint);
    task.auto_advance = ReadBoolMember(task_object, "auto_advance", false);
    task.skip_labeled_on_advance = ReadBoolMember(task_object, "skip_labeled_on_advance", false);
    if (const std::optional<std::size_t> remembered = ReadSizeMember(task_object, "remembered_position")) {
        if (*remembered < sample_count) {
            task.remembered_position = *remembered;
        }
    }
    if (const JsonValue* output_path = ObjectMember(task_object, "output_path")) {
        if (std::optional<std::filesystem::path> path = ReadPersistedPathReference(*output_path);
            path && !path->empty()) {
            task.output_path = std::move(*path);
        }
    }
    bool output_load_failed = false;
    std::string output_load_error;
    bool metadata_load_failed = false;
    bool metadata_retry_pending = false;
    std::string metadata_load_error;
    if (task.output_path) {
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
        ApplyPendingValues(task, task_object, cancellation_checkpoint);
    } else if (const JsonValue* values = ObjectMember(task_object, "values")) {
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
                    break;
                }
                parsed_values.push_back(static_cast<int>(value.integer_value));
            }
            if (all_ints) {
                task.values = std::move(parsed_values);
            }
        }
    }
    task.save_state.kind = task.output_path ? SampleLabelSaveStateKind::AutosavedToOutput
                                            : SampleLabelSaveStateKind::InternalDraftOnly;
    if (const std::optional<std::string> save_state = ReadStringMember(task_object, "save_state")) {
        task.save_state.kind = ParseSaveStateKind(*save_state);
    }
    if (const std::optional<std::string> save_message = ReadStringMember(task_object, "save_message")) {
        task.save_state.message = *save_message;
    }
    if (const std::optional<std::string> save_message_kind =
            ReadStringMember(
                task_object,
                "save_message_kind")) {
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
    } else {
        task.save_state.message_kind =
            LegacySaveMessageKind(
                task.save_state.message);
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
    return task;
}

}  // namespace

std::filesystem::path DefaultSampleLabelingStateCachePath()
{
    return DefaultLocalUserStatePath(
        local_user_state_paths::kSampleLabelingState);
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
            {1, kStateSchemaVersion},
            "sample-labeling task record",
            cancellation_checkpoint);
    if (!cache.document) {
        result.warning = std::move(cache.warning);
        return result;
    }

    const JsonValue* sources = ObjectMember(cache.document->root, "sources");
    if (sources == nullptr || sources->kind != JsonValue::Kind::Array) {
        return result;
    }

    for (std::size_t source_index = 0; source_index < sources->array.size(); ++source_index) {
        if (cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        const JsonValue& source_object = sources->array[source_index];
        if (source_object.kind != JsonValue::Kind::Object) {
            continue;
        }
        const std::optional<std::string> identity = ReadStringMember(source_object, "identity");
        const std::optional<std::size_t> sample_count = ReadSizeMember(source_object, "sample_count");
        if (!identity || identity->empty() || !sample_count || *sample_count == 0) {
            continue;
        }

        const bool legacy_identity = IsLegacySourceCollectionIdentity(*identity);
        SampleLabelingSourceState state;
        state.sample_count = *sample_count;
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
        if (const JsonValue* tasks = ObjectMember(source_object, "tasks")) {
            if (tasks->kind != JsonValue::Kind::Array) {
                continue;
            }
            for (const JsonValue& task_object : tasks->array) {
                if (cancellation_checkpoint) {
                    cancellation_checkpoint();
                }
                if (policy ==
                        SampleLabelingStateCacheLoadPolicy::
                            InternalDraftsOnly) {
                    const JsonValue* output_path =
                        ObjectMember(
                            task_object,
                            "output_path");
                    if (output_path != nullptr &&
                        output_path->kind !=
                            JsonValue::Kind::Null) {
                        result.cache = {};
                        result.warning =
                            "Persistent labeling output paths are not permitted in an automation state seed.";
                        return result;
                    }
                }
                if (std::optional<SampleLabelingTask> task =
                        ParseTask(task_object, state.sample_count, cancellation_checkpoint)) {
                    state.tasks.push_back(std::move(*task));
                }
            }
        }
        if (state.active_task_id && std::none_of(state.tasks.begin(), state.tasks.end(), [&state](const auto& task) {
                return task.task_id == *state.active_task_id;
            })) {
            state.active_task_id.reset();
        }
        result.cache.sources.emplace(
            NormalizePersistedSourceCollectionIdentity(std::move(*identity)),
            std::move(state));
    }
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
    return result;
}

bool SaveSampleLabelingStateCache(
    const std::filesystem::path& path,
    const SampleLabelingStateCache& cache)
{
    if (path.empty()) {
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
                    stream << "          \"output_path\": ";
                    if (task.output_path) {
                        WritePersistedPathReference(stream, *task.output_path);
                    } else {
                        stream << "null";
                    }
                    stream << ",\n";
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
        });
}

}  // namespace specforge
