#include "domain/sample_labeling.h"

#include "app/local_user_state_json.h"
#include "domain/npy_array_io.h"
#include "platform/atomic_file.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace specforge {
namespace {

constexpr const char* kMetadataFormatKind = "specforge.sample_label_result.metadata";
constexpr int kMetadataSchemaVersion = 1;
constexpr std::string_view kInt32DtypeText = "int32";

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::filesystem::path Utf8ToPath(const std::string& value)
{
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

std::string TrimAscii(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char character) {
        return std::isspace(character) != 0;
    }).base();

    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

bool HasPendingPersistence(const SampleLabelingTask& task)
{
    return !task.pending_sample_indices.empty() || task.metadata_save_pending;
}

void RefreshPendingSaveState(SampleLabelingTask& task)
{
    task.save_state.pending_count = task.pending_sample_indices.size();
    if (HasPendingPersistence(task)) {
        task.save_state.kind = SampleLabelSaveStateKind::Pending;
        task.save_state.message.clear();
    } else if (task.output_path) {
        task.save_state.kind = SampleLabelSaveStateKind::AutosavedToOutput;
        task.save_state.message.clear();
    } else {
        task.save_state.kind = SampleLabelSaveStateKind::InternalDraftOnly;
        task.save_state.message.clear();
    }
}

std::string RelativeResultFileReference(const std::filesystem::path& result_path)
{
    const std::filesystem::path filename = result_path.filename();
    return filename.empty() ? PathToUtf8(result_path) : PathToUtf8(filename);
}

bool PathExists(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

bool PathsReferToSameFile(const std::filesystem::path& left, const std::filesystem::path& right)
{
    std::error_code equivalent_error;
    if (PathExists(left) && PathExists(right) &&
        std::filesystem::equivalent(left, right, equivalent_error) && !equivalent_error) {
        return true;
    }
    return left.lexically_normal() == right.lexically_normal();
}

bool MetadataReferenceMatchesResult(
    const std::filesystem::path& metadata_path,
    const std::filesystem::path& result_path,
    std::string_view result_file)
{
    if (result_file.empty()) {
        return false;
    }
    const std::filesystem::path reference_path = Utf8ToPath(std::string(result_file));
    if (reference_path.is_absolute()) {
        return false;
    }
    return PathsReferToSameFile(metadata_path.parent_path() / reference_path, result_path);
}

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

SampleLabelSet ParseMetadataLabelSet(const JsonValue& root)
{
    SampleLabelSet label_set;
    const JsonValue* labels = ObjectMember(root, "labels");
    if (labels == nullptr || labels->kind != JsonValue::Kind::Array) {
        return label_set;
    }

    for (const JsonValue& label_object : labels->array) {
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

std::optional<SampleLabelResultMetadataSource> ParseMetadataSource(const JsonValue& root)
{
    const JsonValue* source = ObjectMember(root, "source_collection");
    if (source == nullptr || source->kind != JsonValue::Kind::Object) {
        return std::nullopt;
    }

    SampleLabelResultMetadataSource parsed;
    parsed.source_name = ReadStringMember(*source, "source_name").value_or("");
    parsed.source_fingerprint = ReadStringMember(*source, "source_fingerprint").value_or("");
    parsed.context_fingerprint = ReadStringMember(*source, "context_fingerprint").value_or("");
    parsed.spectrum_count = ReadSizeMember(*source, "spectrum_count").value_or(0);
    if (parsed.source_name.empty() && parsed.source_fingerprint.empty() &&
        parsed.context_fingerprint.empty() && parsed.spectrum_count == 0) {
        return std::nullopt;
    }
    return parsed;
}

bool WriteInt32Npy(const std::filesystem::path& path, const std::vector<int>& values, std::string* error_message)
{
    AtomicFileWriteOptions options;
    options.open_mode = std::ios::binary | std::ios::trunc;
    options.target_description = "sample label result";
    return WriteFileAtomically(path, options, [&values](std::ostream& stream, std::string& error_message) {
        try {
            WriteNpyInt32Vector(stream, values);
        } catch (const NpyArrayError& error) {
            error_message = error.what();
            return false;
        }
        return true;
    }, error_message);
}

std::optional<std::vector<int>> ReadInt32Npy(
    const std::filesystem::path& path,
    std::size_t expected_count,
    std::string* error_message)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream.good()) {
        if (error_message != nullptr) {
            *error_message = "could not open label output";
        }
        return std::nullopt;
    }

    try {
        const NpyHeader header = ReadNpyHeader(stream);
        const std::optional<NpyScalarType> scalar_type = ParseNpyScalarType(header.descr);
        if (!scalar_type || scalar_type->kind != NpyScalarKind::SignedInteger || scalar_type->item_size != sizeof(std::int32_t)) {
            if (error_message != nullptr) {
                *error_message = "label output is not int32";
            }
            return std::nullopt;
        }
        if (header.shape.size() != 1 || header.shape[0] != expected_count) {
            if (error_message != nullptr) {
                *error_message = "label output length does not match this source";
            }
            return std::nullopt;
        }

        ValidateNpyPayloadSize(path, header, expected_count, scalar_type->item_size);
        SeekNpyData(stream, header);

        const std::vector<std::int32_t> typed_values =
            ReadNpyTypedValues<std::int32_t>(stream, expected_count, "label output data is truncated");
        std::vector<int> values;
        values.reserve(typed_values.size());
        for (const std::int32_t value : typed_values) {
            values.push_back(static_cast<int>(value));
        }
        return values;
    } catch (const NpyArrayError& error) {
        if (error_message != nullptr) {
            *error_message = error.what();
        }
        return std::nullopt;
    }
}

}  // namespace

SampleLabelingTask CreateSampleLabelingTask(
    std::string task_id,
    std::string task_name,
    std::size_t sample_count)
{
    SampleLabelingTask task;
    task.task_id = std::move(task_id);
    task.task_name = std::move(task_name);
    task.values.assign(sample_count, kUnlabeledSampleLabelCode);
    task.save_state.kind = SampleLabelSaveStateKind::InternalDraftOnly;
    return task;
}

bool IsValidSampleLabelShortcut(char shortcut)
{
    const unsigned char value = static_cast<unsigned char>(shortcut);
    return std::isalnum(value) != 0;
}

char NormalizeSampleLabelShortcut(char shortcut)
{
    if (!IsValidSampleLabelShortcut(shortcut)) {
        return '\0';
    }
    return static_cast<char>(std::tolower(static_cast<unsigned char>(shortcut)));
}

bool ContainsSampleLabelCode(const SampleLabelSet& label_set, int code)
{
    return FindSampleLabel(label_set, code) != nullptr;
}

const SampleLabelDefinition* FindSampleLabel(const SampleLabelSet& label_set, int code)
{
    const auto match = std::find_if(label_set.labels.begin(), label_set.labels.end(), [code](const auto& label) {
        return label.code == code;
    });
    return match == label_set.labels.end() ? nullptr : &*match;
}

int NextAvailableSampleLabelCode(const SampleLabelSet& label_set)
{
    int candidate = 0;
    while (ContainsSampleLabelCode(label_set, candidate) || candidate == kUnlabeledSampleLabelCode) {
        ++candidate;
    }
    return candidate;
}

bool UpsertSampleLabel(SampleLabelSet& label_set, SampleLabelDefinition label)
{
    label.name = TrimAscii(std::move(label.name));
    label.shortcut = NormalizeSampleLabelShortcut(label.shortcut);
    if (label.code == kUnlabeledSampleLabelCode || label.name.empty()) {
        return false;
    }

    bool changed = false;
    if (label.shortcut != '\0') {
        for (SampleLabelDefinition& existing : label_set.labels) {
            if (existing.code != label.code && existing.shortcut == label.shortcut) {
                existing.shortcut = '\0';
                changed = true;
            }
        }
    }

    auto match = std::find_if(label_set.labels.begin(), label_set.labels.end(), [&label](const auto& existing) {
        return existing.code == label.code;
    });
    if (match == label_set.labels.end()) {
        label_set.labels.push_back(std::move(label));
        changed = true;
    } else if (match->name != label.name || match->shortcut != label.shortcut) {
        *match = std::move(label);
        changed = true;
    }

    if (!changed) {
        return false;
    }

    std::stable_sort(label_set.labels.begin(), label_set.labels.end(), [](const auto& left, const auto& right) {
        return left.code < right.code;
    });
    return true;
}

bool UpdateSampleLabel(
    SampleLabelingTask& task,
    int original_code,
    SampleLabelDefinition label,
    bool allow_used_code_change)
{
    const auto original = std::find_if(
        task.label_set.labels.begin(),
        task.label_set.labels.end(),
        [original_code](const SampleLabelDefinition& candidate) {
            return candidate.code == original_code;
        });
    if (original == task.label_set.labels.end()) {
        return false;
    }

    const int updated_code = label.code;
    const bool code_changed = updated_code != original_code;
    if (code_changed) {
        const bool target_code_has_label = ContainsSampleLabelCode(task.label_set, updated_code);
        const bool target_code_has_values =
            std::find(task.values.begin(), task.values.end(), updated_code) != task.values.end();
        const bool original_code_has_values =
            std::find(task.values.begin(), task.values.end(), original_code) != task.values.end();
        if (updated_code == kUnlabeledSampleLabelCode || target_code_has_label || target_code_has_values ||
            (original_code_has_values && !allow_used_code_change)) {
            return false;
        }
    }

    SampleLabelSet updated_label_set = task.label_set;
    updated_label_set.labels.erase(
        std::remove_if(
            updated_label_set.labels.begin(),
            updated_label_set.labels.end(),
            [original_code](const SampleLabelDefinition& candidate) {
                return candidate.code == original_code;
            }),
        updated_label_set.labels.end());
    if (!UpsertSampleLabel(updated_label_set, std::move(label))) {
        return false;
    }
    task.label_set = std::move(updated_label_set);

    if (code_changed) {
        for (std::size_t index = 0; index < task.values.size(); ++index) {
            if (task.values[index] != original_code) {
                continue;
            }
            task.values[index] = updated_code;
            task.pending_sample_indices.insert(index);
        }
        RefreshPendingSaveState(task);
    }
    return true;
}

bool RemoveSampleLabel(SampleLabelingTask& task, int code)
{
    if (code == kUnlabeledSampleLabelCode) {
        return false;
    }

    const auto match = std::find_if(task.label_set.labels.begin(), task.label_set.labels.end(), [code](const auto& label) {
        return label.code == code;
    });
    if (match == task.label_set.labels.end()) {
        return false;
    }

    for (std::size_t index = 0; index < task.values.size(); ++index) {
        if (task.values[index] == code) {
            task.values[index] = kUnlabeledSampleLabelCode;
            task.pending_sample_indices.insert(index);
        }
    }
    task.label_set.labels.erase(match);
    RefreshPendingSaveState(task);
    return true;
}

std::optional<int> SampleLabelCodeForShortcut(const SampleLabelSet& label_set, char shortcut)
{
    const char normalized = NormalizeSampleLabelShortcut(shortcut);
    if (normalized == '\0') {
        return std::nullopt;
    }

    const auto match = std::find_if(label_set.labels.begin(), label_set.labels.end(), [normalized](const auto& label) {
        return label.shortcut == normalized;
    });
    if (match == label_set.labels.end()) {
        return std::nullopt;
    }
    return match->code;
}

std::string FormatSampleLabelValue(const SampleLabelSet& label_set, int code, int unlabeled_sentinel)
{
    if (code == unlabeled_sentinel) {
        return "Unlabeled (" + std::to_string(unlabeled_sentinel) + ")";
    }
    if (const SampleLabelDefinition* label = FindSampleLabel(label_set, code)) {
        return label->name + " (" + std::to_string(code) + ")";
    }
    return std::to_string(code);
}

std::size_t CountLabeledSamples(const SampleLabelingTask& task)
{
    return static_cast<std::size_t>(std::count_if(task.values.begin(), task.values.end(), [](int value) {
        return value != kUnlabeledSampleLabelCode;
    }));
}

std::size_t CountUnlabeledSamples(const SampleLabelingTask& task)
{
    return task.values.size() - CountLabeledSamples(task);
}

SampleLabelWriteResult AssignSampleLabel(SampleLabelingTask& task, std::size_t sample_index, int code)
{
    SampleLabelWriteResult result;
    result.sample_index = sample_index;
    result.current_code = code;
    if (sample_index >= task.values.size() || !ContainsSampleLabelCode(task.label_set, code)) {
        result.pending_count = task.pending_sample_indices.size();
        return result;
    }

    result.accepted = true;
    result.previous_code = task.values[sample_index];
    task.values[sample_index] = code;
    result.current_code = code;
    result.changed = result.previous_code != result.current_code;
    if (result.changed) {
        result.advance_requested = task.auto_advance;
        task.remembered_position = sample_index;
        task.pending_sample_indices.insert(sample_index);
        RefreshPendingSaveState(task);
    }
    result.pending_count = task.pending_sample_indices.size();
    return result;
}

SampleLabelWriteResult ClearSampleLabel(SampleLabelingTask& task, std::size_t sample_index)
{
    SampleLabelWriteResult result;
    result.sample_index = sample_index;
    if (sample_index >= task.values.size()) {
        result.pending_count = task.pending_sample_indices.size();
        return result;
    }

    result.accepted = true;
    result.previous_code = task.values[sample_index];
    task.values[sample_index] = kUnlabeledSampleLabelCode;
    result.current_code = kUnlabeledSampleLabelCode;
    result.changed = result.previous_code != result.current_code;
    if (result.changed) {
        result.advance_requested = task.auto_advance;
        task.remembered_position = sample_index;
        task.pending_sample_indices.insert(sample_index);
        RefreshPendingSaveState(task);
    }
    result.pending_count = task.pending_sample_indices.size();
    return result;
}

void MarkSampleLabelTaskPersisted(SampleLabelingTask& task, SampleLabelSaveStateKind clean_state)
{
    task.pending_sample_indices.clear();
    task.metadata_save_pending = false;
    task.save_state.pending_count = 0;
    task.save_state.kind = clean_state;
    task.save_state.message.clear();
}

void SelectSampleLabelTaskOutputPath(SampleLabelingTask& task, std::filesystem::path output_path)
{
    task.output_path = std::move(output_path);
    task.pending_sample_indices.clear();
    task.metadata_save_pending = true;

    std::string ignored_error;
    const std::optional<std::vector<int>> output_values =
        LoadSampleLabelResultNpy(*task.output_path, task.values.size(), &ignored_error);
    for (std::size_t index = 0; index < task.values.size(); ++index) {
        const int base_value = output_values ? (*output_values)[index] : kUnlabeledSampleLabelCode;
        if (task.values[index] != base_value) {
            task.pending_sample_indices.insert(index);
        }
    }
    RefreshPendingSaveState(task);
}

void MarkSampleLabelTaskMetadataPending(SampleLabelingTask& task)
{
    if (!task.output_path) {
        return;
    }
    task.metadata_save_pending = true;
    RefreshPendingSaveState(task);
}

void MarkSampleLabelTaskSaveFailed(SampleLabelingTask& task, std::string message)
{
    task.save_state.kind = SampleLabelSaveStateKind::Failed;
    task.save_state.pending_count = task.pending_sample_indices.size();
    task.save_state.message = std::move(message);
}

SampleLabelTaskPersistResult PersistSampleLabelingTaskResult(
    SampleLabelingTask& task,
    const SampleLabelResultMetadataSource* source)
{
    SampleLabelTaskPersistResult result;
    result.output_path_selected = task.output_path.has_value();
    if (!task.output_path) {
        return result;
    }

    std::string error;
    const bool label_result_saved = SaveSampleLabelResultNpy(*task.output_path, task, &error);
    if (!label_result_saved) {
        result.message = error.empty() ? "could not save label output" : std::move(error);
        MarkSampleLabelTaskSaveFailed(task, result.message);
        return result;
    }

    task.pending_sample_indices.clear();
    task.metadata_save_pending = true;

    std::string metadata_error;
    const bool metadata_saved =
        SaveSampleLabelResultMetadataSidecar(*task.output_path, task, source, &metadata_error);
    result.output_saved = metadata_saved;
    if (metadata_saved) {
        MarkSampleLabelTaskPersisted(task, SampleLabelSaveStateKind::AutosavedToOutput);
    } else {
        result.message =
            metadata_error.empty() ? "could not save label output metadata" : std::move(metadata_error);
        MarkSampleLabelTaskSaveFailed(task, result.message);
    }
    return result;
}

bool SaveSampleLabelResultNpy(
    const std::filesystem::path& path,
    const SampleLabelingTask& task,
    std::string* error_message)
{
    return WriteInt32Npy(path, task.values, error_message);
}

std::optional<std::vector<int>> LoadSampleLabelResultNpy(
    const std::filesystem::path& path,
    std::size_t expected_count,
    std::string* error_message)
{
    return ReadInt32Npy(path, expected_count, error_message);
}

std::filesystem::path SampleLabelResultMetadataPathForResult(const std::filesystem::path& result_path)
{
    std::filesystem::path filename = result_path.stem();
    filename += ".sf-labels.json";
    return result_path.parent_path() / filename;
}

bool SaveSampleLabelResultMetadataSidecar(
    const std::filesystem::path& result_path,
    const SampleLabelingTask& task,
    const SampleLabelResultMetadataSource* source,
    std::string* error_message)
{
    if (result_path.empty()) {
        if (error_message != nullptr) {
            *error_message = "label output path is empty";
        }
        return false;
    }

    const std::filesystem::path metadata_path = SampleLabelResultMetadataPathForResult(result_path);
    const std::string result_file = RelativeResultFileReference(result_path);
    return WriteVersionedJsonCacheFile(
        metadata_path,
        kMetadataFormatKind,
        kMetadataSchemaVersion,
        "sample label result metadata",
        [&](std::ostream& stream, std::string&) {
            stream << ",\n";
            stream << "  \"result_file\": ";
            WriteJsonString(stream, result_file);
            stream << ",\n";
            stream << "  \"task_id\": ";
            WriteJsonString(stream, task.task_id);
            stream << ",\n";
            stream << "  \"value_count\": " << task.values.size() << ",\n";
            stream << "  \"expected_dtype\": ";
            WriteJsonString(stream, kInt32DtypeText);
            stream << ",\n";
            stream << "  \"unlabeled_sentinel\": " << kUnlabeledSampleLabelCode << ",\n";
            stream << "  \"task_name\": ";
            WriteJsonString(stream, task.task_name);
            stream << ",\n";
            stream << "  \"labels\": [";
            if (!task.label_set.labels.empty()) {
                stream << "\n";
            }
            for (std::size_t label_index = 0; label_index < task.label_set.labels.size(); ++label_index) {
                const SampleLabelDefinition& label = task.label_set.labels[label_index];
                stream << "    { \"code\": " << label.code << ", \"name\": ";
                WriteJsonString(stream, label.name);
                stream << ", \"shortcut\": ";
                const std::string shortcut =
                    label.shortcut == '\0' ? std::string{} : std::string(1, label.shortcut);
                WriteJsonString(stream, shortcut);
                stream << " }" << (label_index + 1 == task.label_set.labels.size() ? "\n" : ",\n");
            }
            if (!task.label_set.labels.empty()) {
                stream << "  ";
            }
            stream << "]";
            if (source != nullptr) {
                stream << ",\n";
                stream << "  \"source_collection\": {\n";
                stream << "    \"source_name\": ";
                WriteJsonString(stream, source->source_name);
                stream << ",\n";
                stream << "    \"source_fingerprint\": ";
                WriteJsonString(stream, source->source_fingerprint);
                stream << ",\n";
                stream << "    \"context_fingerprint\": ";
                WriteJsonString(stream, source->context_fingerprint);
                stream << ",\n";
                stream << "    \"spectrum_count\": " << source->spectrum_count << "\n";
                stream << "  }";
            }
            stream << "\n";
            return true;
        },
        error_message);
}

SampleLabelResultMetadataLoadResult LoadSampleLabelResultMetadataForResult(
    const std::filesystem::path& result_path,
    std::size_t expected_count,
    std::string_view expected_dtype)
{
    SampleLabelResultMetadataLoadResult result;
    const std::filesystem::path metadata_path = SampleLabelResultMetadataPathForResult(result_path);
    std::error_code exists_error;
    if (!std::filesystem::exists(metadata_path, exists_error) || exists_error) {
        return result;
    }

    std::ifstream stream(metadata_path);
    if (!stream.good()) {
        result.warning = "could not read sample label result metadata";
        return result;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();

    std::string parse_error;
    std::optional<JsonValue> root = ParseJson(buffer.str(), parse_error);
    if (!root || root->kind != JsonValue::Kind::Object) {
        result.warning = parse_error.empty() ? "invalid sample label result metadata" : parse_error;
        return result;
    }

    const std::optional<std::string> format_kind = ReadStringMember(*root, "format_kind");
    const std::optional<int> schema_version = ReadIntMember(*root, "schema_version");
    if (!format_kind || *format_kind != kMetadataFormatKind ||
        !schema_version || *schema_version != kMetadataSchemaVersion) {
        result.warning = "unsupported sample label result metadata";
        return result;
    }

    SampleLabelResultMetadata metadata;
    metadata.result_file = ReadStringMember(*root, "result_file").value_or("");
    metadata.task_id = ReadStringMember(*root, "task_id").value_or("");
    metadata.value_count = ReadSizeMember(*root, "value_count").value_or(0);
    metadata.expected_dtype = ReadStringMember(*root, "expected_dtype").value_or("");
    metadata.unlabeled_sentinel = ReadIntMember(*root, "unlabeled_sentinel").value_or(kUnlabeledSampleLabelCode);
    metadata.task_name = ReadStringMember(*root, "task_name").value_or(metadata.task_id);
    metadata.label_set = ParseMetadataLabelSet(*root);
    metadata.source = ParseMetadataSource(*root);

    if (!MetadataReferenceMatchesResult(metadata_path, result_path, metadata.result_file)) {
        result.warning = "metadata references a different label result";
        return result;
    }
    if (metadata.value_count != expected_count) {
        result.warning = "metadata value count does not match the label result";
        return result;
    }
    if (metadata.expected_dtype != expected_dtype) {
        result.warning = "metadata dtype does not match the label result";
        return result;
    }
    if (metadata.task_id.empty()) {
        result.warning = "metadata is missing a task id";
        return result;
    }

    result.metadata = std::move(metadata);
    return result;
}

}  // namespace specforge
