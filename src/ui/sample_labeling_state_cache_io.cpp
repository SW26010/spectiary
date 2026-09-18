#include "ui/sample_labeling_state_cache_io.h"
#include "ui/sample_labeling_persistence_owners.h"

#include "app/local_user_state.h"
#include "app/local_user_state_json.h"
#include "app/local_user_state_paths.h"
#include "domain/sample_annotation_io.h"
#include "domain/source_path_identity.h"
#include "domain/source_collection_identity_digest.h"
#include "domain/stable_sha256.h"
#include "domain/utf8.h"
#include "domain/uuid_v4.h"
#include "platform/exclusive_file_lease.h"
#include "ui/sample_annotation_labeling_rules.h"

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
constexpr int kStateSchemaVersion = 4;

const nlohmann::json* ObjectMember(const nlohmann::json& value, std::string_view key)
{
    return JsonObjectMember(value, key);
}

std::optional<std::string> ReadStringMember(const nlohmann::json& value, std::string_view key)
{
    return ReadJsonStringMember(value, key);
}

std::optional<std::size_t> ReadSizeMember(const nlohmann::json& value, std::string_view key)
{
    return ReadJsonSizeMember(value, key);
}

std::optional<int> ReadIntMember(const nlohmann::json& value, std::string_view key)
{
    return ReadJsonIntMember(value, key);
}

bool ReadBoolMember(const nlohmann::json& value, std::string_view key, bool fallback)
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
    const nlohmann::json& task_object,
    SampleLabelingTask& task,
    const RuntimePaths& runtime_paths)
{
    const nlohmann::json* output = ObjectMember(task_object, "output");
    if (output == nullptr || output->type() != nlohmann::json::value_t::object) {
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
    const nlohmann::json* output_path = ObjectMember(*output, "path");
    if (output_path == nullptr) {
        return false;
    }
    if (*format == SampleLabelingOutputArtifactFormat::None) {
        if (output_path->type() != nlohmann::json::value_t::null) {
            return false;
        }
        task.persistence.output_format = *format;
        return true;
    }
    std::optional<std::filesystem::path> path =
        ReadPersistedPathReference(*output_path, runtime_paths);
    if (!path || path->empty()) {
        return false;
    }
    task.persistence.output_path = std::move(*path);
    task.persistence.output_format = *format;
    return true;
}

bool ApplyPendingValues(
    SampleLabelingTask& task,
    const nlohmann::json& task_object,
    const std::function<void()>& cancellation_checkpoint)
{
    const nlohmann::json* pending_values = ObjectMember(task_object, "pending_values");
    if (pending_values == nullptr) {
        return true;
    }
    if (pending_values->type() != nlohmann::json::value_t::array) {
        return false;
    }

    bool valid = true;
    for (std::size_t index = 0; index < (*pending_values).size(); ++index) {
        if ((index & 0xfffU) == 0U && cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        const nlohmann::json& pending_object = (*pending_values)[index];
        if (pending_object.type() != nlohmann::json::value_t::object) {
            valid = false;
            continue;
        }
        const std::optional<std::size_t> sample_index = ReadSizeMember(pending_object, "index");
        const std::optional<int> value = ReadIntMember(pending_object, "value");
        if (!sample_index || !value || *sample_index >= task.values.SampleCount()) {
            valid = false;
            continue;
        }
        task.values.Complete()[*sample_index] = *value;
        task.persistence.pending_sample_indices.insert(*sample_index);
    }
    return valid;
}

SampleLabelSet ParseLabelSet(
    const nlohmann::json& task_object,
    const std::function<void()>& cancellation_checkpoint,
    bool* valid_out)
{
    bool valid = true;
    SampleLabelSet label_set;
    const nlohmann::json* labels = ObjectMember(task_object, "labels");
    if (labels == nullptr) {
        if (valid_out != nullptr) {
            *valid_out = true;
        }
        return label_set;
    }
    if (labels->type() != nlohmann::json::value_t::array) {
        if (valid_out != nullptr) {
            *valid_out = false;
        }
        return label_set;
    }

    for (std::size_t index = 0; index < (*labels).size(); ++index) {
        if ((index & 0xfffU) == 0U && cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        const nlohmann::json& label_object = (*labels)[index];
        if (label_object.type() != nlohmann::json::value_t::object) {
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
        const nlohmann::json* shortcut_value =
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

bool IsCanonicalOriginKind(std::string_view value)
{
    if (value.empty() || value.front() < 'a' || value.front() > 'z') {
        return false;
    }
    return std::all_of(
        value.begin(),
        value.end(),
        [](char character) {
            return (character >= 'a' && character <= 'z') ||
                (character >= '0' && character <= '9') ||
                character == '_';
        });
}

bool IsCanonicalAnnotationFingerprint(std::string_view value)
{
    constexpr std::string_view prefix = "sha256:";
    return value.size() == prefix.size() + 64U &&
        value.starts_with(prefix) &&
        std::all_of(
            value.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
            value.end(),
            [](char character) {
                return (character >= '0' && character <= '9') ||
                    (character >= 'a' && character <= 'f');
            });
}

std::optional<SampleLabelingTaskCanonicalMetadata>
ParseCanonicalMetadata(const nlohmann::json& task_object)
{
    const nlohmann::json* value =
        ObjectMember(task_object, "canonical_metadata");
    if (value == nullptr || value->type() != nlohmann::json::value_t::object) {
        return std::nullopt;
    }
    const std::optional<std::string> created_text =
        ReadStringMember(*value, "created_at");
    const std::optional<std::string> modified_text =
        ReadStringMember(*value, "modified_at");
    if (!created_text || !modified_text) {
        return std::nullopt;
    }
    const std::optional<CanonicalTimestamp> created_at =
        ParseCanonicalTimestamp(*created_text);
    const std::optional<CanonicalTimestamp> modified_at =
        ParseCanonicalTimestamp(*modified_text);
    if (!created_at || !modified_at || *modified_at < *created_at) {
        return std::nullopt;
    }

    const nlohmann::json* origin_value = ObjectMember(*value, "origin");
    if (origin_value == nullptr ||
        origin_value->type() != nlohmann::json::value_t::object) {
        return std::nullopt;
    }
    const std::optional<std::string> origin_kind =
        ReadStringMember(*origin_value, "kind");
    if (!origin_kind || !IsCanonicalOriginKind(*origin_kind)) {
        return std::nullopt;
    }

    SampleLabelingTaskCanonicalMetadata metadata;
    metadata.created_at = *created_at;
    metadata.modified_at = *modified_at;
    metadata.origin.kind = *origin_kind;
    if (const nlohmann::json* annotation =
            ObjectMember(*origin_value, "annotation");
        annotation != nullptr && annotation->type() != nlohmann::json::value_t::null) {
        if (annotation->type() != nlohmann::json::value_t::object) {
            return std::nullopt;
        }
        const std::optional<std::string> name =
            ReadStringMember(*annotation, "name");
        const std::optional<std::string> format =
            ReadStringMember(*annotation, "format");
        if (!name || !format) {
            return std::nullopt;
        }
        SampleLabelingAnnotationOrigin parsed_annotation{
            .name = *name,
            .format = *format};
        if (const nlohmann::json* fingerprint =
                ObjectMember(*annotation, "fingerprint");
            fingerprint != nullptr &&
            fingerprint->type() != nlohmann::json::value_t::null) {
            parsed_annotation.fingerprint =
                ReadStringMember(*annotation, "fingerprint");
            if (!parsed_annotation.fingerprint) {
                return std::nullopt;
            }
        }
        if (!IsValidSampleLabelingAnnotationOriginName(
                parsed_annotation.name) ||
            (parsed_annotation.format != "csv" &&
             parsed_annotation.format != "npy") ||
            (parsed_annotation.fingerprint &&
             !IsCanonicalAnnotationFingerprint(
                 *parsed_annotation.fingerprint))) {
            return std::nullopt;
        }
        metadata.origin.annotation = std::move(parsed_annotation);
    }
    if ((metadata.origin.kind == "manual" &&
         metadata.origin.annotation) ||
        (metadata.origin.kind == "annotation_promotion" &&
         !metadata.origin.annotation)) {
        return std::nullopt;
    }

    if (const nlohmann::json* description =
            ObjectMember(*value, "description");
        description != nullptr && description->type() != nlohmann::json::value_t::null) {
        metadata.description = ReadStringMember(*value, "description");
        if (!metadata.description ||
            !IsValidUtf8(*metadata.description)) {
            return std::nullopt;
        }
    }
    if (const nlohmann::json* authors = ObjectMember(*value, "authors");
        authors != nullptr && authors->type() != nlohmann::json::value_t::null) {
        if (authors->type() != nlohmann::json::value_t::array) {
            return std::nullopt;
        }
        metadata.authors.reserve((*authors).size());
        for (const nlohmann::json& author_value : (*authors)) {
            if (author_value.type() != nlohmann::json::value_t::object) {
                return std::nullopt;
            }
            const std::optional<std::string> name =
                ReadStringMember(author_value, "name");
            if (!name || !IsValidUtf8WithNonWhitespace(*name)) {
                return std::nullopt;
            }
            SampleLabelingAuthor author{.name = *name};
            if (const nlohmann::json* identifier =
                    ObjectMember(author_value, "identifier");
                identifier != nullptr &&
                identifier->type() != nlohmann::json::value_t::null) {
                author.identifier =
                    ReadStringMember(author_value, "identifier");
                if (!author.identifier) {
                    return std::nullopt;
                }
                if (!IsValidUtf8WithNonWhitespace(
                        *author.identifier)) {
                    return std::nullopt;
                }
            }
            if (const nlohmann::json* email =
                    ObjectMember(author_value, "email");
                email != nullptr) {
                if (email->type() != nlohmann::json::value_t::string) {
                    return std::nullopt;
                }
                author.email =
                    ReadStringMember(author_value, "email");
                if (!author.email ||
                    !IsValidUtf8WithNonWhitespace(*author.email)) {
                    return std::nullopt;
                }
            }
            metadata.authors.push_back(std::move(author));
        }
    }
    return metadata;
}

ParsedTask ParseTask(
    const nlohmann::json& task_object,
    std::size_t sample_count,
    const std::function<void()>& cancellation_checkpoint,
    bool hydrate_persistent_output,
    const RuntimePaths& runtime_paths)
{
    if (task_object.type() != nlohmann::json::value_t::object) {
        return {};
    }

    const std::optional<std::string> task_id = ReadStringMember(task_object, "task_id");
    const std::optional<std::string> task_name = ReadStringMember(task_object, "task_name");
    if (!task_id ||
        !IsCanonicalUuidV4(*task_id) ||
        !task_name ||
        !IsValidUtf8WithNonWhitespace(*task_name)) {
        return {};
    }

    bool malformed = false;

    std::optional<SampleLabelingTaskCanonicalMetadata> canonical_metadata =
        ParseCanonicalMetadata(task_object);
    if (!canonical_metadata) {
        return {};
    }
    SampleLabelingTask task = CreateSampleLabelingTask(
        *task_id,
        task_name.value_or(*task_id),
        0,
        std::move(*canonical_metadata));
    task.values.Complete().reserve(sample_count);
    constexpr std::size_t kInitializationChunk = 4096U;
    while (task.values.SampleCount() < sample_count) {
        if (cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        task.values.Complete().resize(
            std::min(sample_count, task.values.SampleCount() + kInitializationChunk),
            kUnlabeledSampleLabelCode);
    }
    bool labels_valid = true;
    task.label_set = ParseLabelSet(
        task_object,
        cancellation_checkpoint,
        &labels_valid);
    malformed = malformed || !labels_valid;
    if (const nlohmann::json* auto_advance =
            ObjectMember(task_object, "auto_advance");
        auto_advance != nullptr &&
        auto_advance->type() != nlohmann::json::value_t::boolean) {
        malformed = true;
    }
    task.session.auto_advance = ReadBoolMember(task_object, "auto_advance", false);
    if (const nlohmann::json* skip_labeled =
            ObjectMember(
                task_object,
                "skip_labeled_on_advance");
        skip_labeled != nullptr &&
        skip_labeled->type() != nlohmann::json::value_t::boolean) {
        malformed = true;
    }
    task.session.skip_labeled_on_advance = ReadBoolMember(task_object, "skip_labeled_on_advance", false);
    if (const nlohmann::json* remembered_value =
            ObjectMember(task_object, "remembered_position");
        remembered_value != nullptr &&
        remembered_value->type() != nlohmann::json::value_t::null) {
        const std::optional<std::size_t> remembered =
            ReadSizeMember(
                task_object,
                "remembered_position");
        if (remembered && *remembered < sample_count) {
            task.session.remembered_position = *remembered;
        } else {
            malformed = true;
        }
    }
    malformed = !ParseTaskOutput(task_object, task, runtime_paths) || malformed;
    if (const nlohmann::json* initial_publication_pending =
            ObjectMember(
                task_object,
                "initial_publication_pending");
        initial_publication_pending != nullptr &&
        initial_publication_pending->type() !=
            nlohmann::json::value_t::boolean) {
        malformed = true;
    }
    const bool legacy_initial_publication_pending =
        ReadBoolMember(
            task_object,
            "initial_publication_pending",
            false);
    if (legacy_initial_publication_pending &&
        (!task.persistence.output_path ||
         task.persistence.output_format !=
             SampleLabelingOutputArtifactFormat::CanonicalAsdf)) {
        malformed = true;
    }
    bool needs_projection = false;
    if (task.persistence.output_format == SampleLabelingOutputArtifactFormat::CanonicalAsdf ||
        (task.persistence.output_path && !hydrate_persistent_output)) {
        needs_projection = true;
    }
    bool output_load_failed = false;
    std::string output_load_error;
    bool metadata_load_failed = false;
    bool metadata_retry_pending = false;
    std::string metadata_load_error;
    if (task.persistence.output_path && hydrate_persistent_output &&
        task.persistence.output_format ==
            SampleLabelingOutputArtifactFormat::
                LegacyNpyWithSidecar) {
        std::optional<LoadedSampleLabelResult> loaded =
            SampleAnnotationIoAdapter{}.LoadLabelResult(
                *task.persistence.output_path,
                sample_count,
                cancellation_checkpoint,
                &output_load_error);
        if (loaded) {
            task.values.Complete() = std::move(loaded->values);
        } else {
            output_load_failed = true;
            // Sparse pending values cannot reconstruct the untouched rows of
            // a missing legacy base. Keep the recovery record, but do not
            // permit publishing this placeholder projection as canonical data.
            needs_projection = true;
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
    } else if (task.persistence.output_path) {
        malformed =
            !ApplyPendingValues(
                task,
                task_object,
                cancellation_checkpoint) ||
            malformed;
    } else {
        const nlohmann::json* values = ObjectMember(task_object, "values");
        if (values == nullptr) {
            malformed = true;
        } else {
            bool values_valid =
                values->type() == nlohmann::json::value_t::array &&
                (*values).size() == sample_count;
            if (values->type() == nlohmann::json::value_t::array && (*values).size() == sample_count) {
                std::vector<int> parsed_values;
                parsed_values.reserve((*values).size());
                bool all_ints = true;
                for (std::size_t index = 0; index < (*values).size(); ++index) {
                    if ((index & 0xfffU) == 0U && cancellation_checkpoint) {
                        cancellation_checkpoint();
                    }
                    const nlohmann::json& value = (*values)[index];
                    if (!specforge::JsonIsInt64(value) ||
                        value.get<std::int64_t>() < std::numeric_limits<int>::min() ||
                        value.get<std::int64_t>() > std::numeric_limits<int>::max()) {
                        all_ints = false;
                        values_valid = false;
                        break;
                    }
                    parsed_values.push_back(static_cast<int>(value.get<std::int64_t>()));
                }
                if (all_ints) {
                    task.values.Complete() = std::move(parsed_values);
                }
            }
            malformed = malformed || !values_valid;
        }
    }
    task.persistence.save_state.kind = task.persistence.output_path ? SampleLabelSaveStateKind::AutosavedToOutput
                                            : SampleLabelSaveStateKind::InternalDraftOnly;
    if (const nlohmann::json* save_state_value =
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
            task.persistence.save_state.kind =
                ParseSaveStateKind(*save_state);
        }
    }
    if (const nlohmann::json* save_message_value =
            ObjectMember(task_object, "save_message");
        save_message_value != nullptr) {
        const std::optional<std::string> save_message =
            ReadStringMember(
                task_object,
                "save_message");
        if (!save_message) {
            malformed = true;
        } else {
            task.persistence.save_state.message =
                *save_message;
        }
    }
    if (const nlohmann::json* save_message_kind_value =
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
            task.persistence.save_state.message_kind =
                ParseSaveMessageKind(
                    *save_message_kind);
            if (task.persistence.save_state.message_kind ==
                    SampleLabelSaveMessageKind::None &&
                !task.persistence.save_state.message.empty()) {
                task.persistence.save_state.message_kind =
                    LegacySaveMessageKind(
                        task.persistence.save_state.message);
            }
        }
    } else {
        task.persistence.save_state.message_kind =
            LegacySaveMessageKind(
                task.persistence.save_state.message);
    }
    if (const nlohmann::json* metadata_pending =
            ObjectMember(task_object, "metadata_pending");
        metadata_pending != nullptr &&
        metadata_pending->type() != nlohmann::json::value_t::boolean) {
        malformed = true;
    }
    task.persistence.metadata_save_pending =
        ReadBoolMember(task_object, "metadata_pending", false) || metadata_retry_pending;
    if (output_load_failed) {
        task.persistence.save_state.kind = SampleLabelSaveStateKind::Failed;
        if (task.persistence.save_state.message.empty()) {
            task.persistence.save_state.message =
                output_load_error.empty() ? "could not read label output" : "could not read label output: " + output_load_error;
        }
        task.persistence.save_state.message_kind =
            SampleLabelSaveMessageKind::SystemDetail;
    } else if (metadata_load_failed) {
        task.persistence.save_state.kind = SampleLabelSaveStateKind::Failed;
        if (task.persistence.save_state.message.empty()) {
            task.persistence.save_state.message = metadata_load_error.empty()
                ? "could not read label output metadata"
                : "could not read label output metadata: " + metadata_load_error;
        }
        task.persistence.save_state.message_kind =
            SampleLabelSaveMessageKind::SystemDetail;
    }
    task.persistence.save_state.pending_count = task.persistence.pending_sample_indices.size();
    if ((!task.persistence.pending_sample_indices.empty() || task.persistence.metadata_save_pending) &&
        task.persistence.save_state.kind != SampleLabelSaveStateKind::Failed) {
        task.persistence.save_state.kind = SampleLabelSaveStateKind::Pending;
    } else if (!task.persistence.output_path && task.persistence.save_state.kind == SampleLabelSaveStateKind::Pending) {
        task.persistence.save_state.kind = SampleLabelSaveStateKind::InternalDraftOnly;
    }
    if (legacy_initial_publication_pending) {
        // Historical schema-4 import only: an unadopted first publication
        // encoded all non-default draft values here. Convert it once into an
        // output-free draft; never resume or serialize the old WAL protocol.
        task.persistence.output_path.reset();
        task.persistence.output_format =
            SampleLabelingOutputArtifactFormat::None;
        needs_projection = false;
        task.persistence.pending_sample_indices.clear();
        task.persistence.metadata_save_pending = false;
        task.persistence.save_state = SampleLabelSaveState{
            .kind = SampleLabelSaveStateKind::InternalDraftOnly};
    }
    if (needs_projection) task.values.MakeSparse(task.persistence.pending_sample_indices);
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

bool ValidateCacheTaskIds(
    const SampleLabelingStateCache& cache,
    std::string* error_message)
{
    for (const auto& [source_identity, state] : cache.sources) {
        (void)source_identity;
        for (const SampleLabelingTask& task : state.tasks) {
            if (!IsCanonicalUuidV4(task.task_id)) {
                SetError(
                    error_message,
                    "sample-labeling cache task ids must be canonical UUID v4 values");
                return false;
            }
        }
    }
    return true;
}

bool ValidateOutputOwnership(
    const SampleLabelingStateCache& cache,
    std::string* error_message)
{
    for (const auto& [source_identity, state] : cache.sources) {
        (void)source_identity;
        for (const SampleLabelingTask& task : state.tasks) {
            if (!task.persistence.output_path) {
                if (task.persistence.output_format ==
                    SampleLabelingOutputArtifactFormat::None) {
                    continue;
                }
                SetError(
                    error_message,
                    "sample-labeling cache contains an output format without a path");
                return false;
            }
            if ((task.persistence.output_format !=
                     SampleLabelingOutputArtifactFormat::CanonicalAsdf &&
                 task.persistence.output_format !=
                     SampleLabelingOutputArtifactFormat::
                         LegacyNpyWithSidecar) ||
                task.persistence.output_path->empty() ||
                SampleAnnotationArtifactIdentities(
                    *task.persistence.output_path,
                    task.persistence.output_format,
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
    if (!ValidateCacheTaskIds(cache, error_message)) {
        return false;
    }
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
            if (task.values.SampleCount() != state.sample_count) {
                SetError(
                    error_message,
                    "sample-labeling cache task sample count does not match its source");
                return false;
            }
            if (const auto* sparse = task.values.Sparse()) {
                if (!task.persistence.output_path) {
                    SetError(error_message, "a sparse labeling cache task requires a persistent base");
                    return false;
                }
                for (const auto index : task.persistence.pending_sample_indices) {
                    if (index >= state.sample_count ||
                        !sparse->pending_values.contains(index)) {
                        SetError(error_message, "a pending labeling cache row has no overlay value");
                        return false;
                    }
                }
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
            if (!task.persistence.output_path) {
                continue;
            }
            const std::string owner = OutputOwnerKey(
                source_identity,
                task.task_id);
            for (const std::string& artifact_key :
                 OutputArtifactOwnerKeys(
                     *task.persistence.output_path,
                     task.persistence.output_format,
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
                        return task.persistence.output_path.has_value();
                    }
                    const auto existing = std::find_if(
                        source->second.tasks.begin(),
                        source->second.tasks.end(),
                        [&task](const SampleLabelingTask& candidate) {
                            return candidate.task_id ==
                                task.task_id;
                        });
                    if (existing == source->second.tasks.end()) {
                        if (task.persistence.output_path) {
                            return true;
                        }
                        continue;
                    }
                    if (existing->persistence.output_path != task.persistence.output_path ||
                        existing->persistence.output_format != task.persistence.output_format) {
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
            if (!task.persistence.output_path) {
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
            if (task.persistence.output_path) {
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

std::filesystem::path DefaultSampleLabelingStateCachePath(const RuntimePaths& runtime_paths)
{
    return runtime_paths.sample_labeling_state_path;
}

std::filesystem::path SampleLabelingDraftCheckpointPath(
    const RuntimePaths& runtime_paths, const std::filesystem::path& state_path)
{
    if (state_path.empty()) return {};
    if (state_path == runtime_paths.sample_labeling_state_path)
        return runtime_paths.sample_labeling_drafts_path;
    // Explicit controller/test roots remain isolated from the process default.
    return state_path.parent_path() / "unsaved" /
        (state_path.stem().wstring() + L".drafts.json");
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

static SampleLabelingStateCacheLoadResult LoadLegacySampleLabelingStateCache(
    const RuntimePaths& runtime_paths,
    const std::filesystem::path& path,
    const std::function<void()>& cancellation_checkpoint,
    SampleLabelingStateCacheLoadPolicy policy)
{
    SampleLabelingStateCacheLoadResult result;
    VersionedJsonCacheLoadResult cache =
        LoadVersionedJsonCacheFile(
            path,
            kStateFormatKind,
            {kStateSchemaVersion},
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

    const nlohmann::json* sources = ObjectMember(cache.document->root, "sources");
    if (sources == nullptr || sources->type() != nlohmann::json::value_t::array) {
        result.warning =
            "Ignored invalid sample-labeling task record.";
        result.issue_kind =
            SampleLabelingStateCacheLoadIssueKind::
                InvalidDocument;
        result.diagnostic_detail =
            "sources must be an array";
        return result;
    }

    for (std::size_t source_index = 0; source_index < (*sources).size(); ++source_index) {
        if (cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        const nlohmann::json& source_object = (*sources)[source_index];
        if (source_object.type() != nlohmann::json::value_t::object) {
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
            const nlohmann::json* member =
                ObjectMember(source_object, field);
            if (member != nullptr &&
                member->type() != nlohmann::json::value_t::string) {
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
        const nlohmann::json* tasks = ObjectMember(source_object, "tasks");
        if (tasks == nullptr) {
            mark_malformed_source_field("tasks");
        } else if (tasks->type() != nlohmann::json::value_t::array) {
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
            for (const nlohmann::json& task_object : (*tasks)) {
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
                                    InternalDraftsOnly, runtime_paths);
                    if (policy ==
                            SampleLabelingStateCacheLoadPolicy::
                                InternalDraftsOnly &&
                        parsed.task && parsed.task->persistence.output_path) {
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

SampleLabelingStateCacheLoadResult LoadLegacySampleLabelingDraftSeed(
    const std::filesystem::path& path)
{
    return LoadLegacySampleLabelingStateCache(RuntimePaths{}, path, {},
        SampleLabelingStateCacheLoadPolicy::InternalDraftsOnly);
}

SampleLabelingStateCacheLoadResult LoadSampleLabelingStateCache(
    const RuntimePaths& runtime_paths,
    const std::filesystem::path& path,
    const std::function<void()>& cancellation_checkpoint,
    SampleLabelingStateCacheLoadPolicy policy)
{
    const auto draft_path = SampleLabelingDraftCheckpointPath(runtime_paths, path);
    std::error_code state_error, draft_error;
    const bool state_exists = std::filesystem::exists(path, state_error);
    const bool draft_exists = std::filesystem::exists(draft_path, draft_error);
    // One bounded pre-release cutover. Once either new owner exists, the old
    // monolith is never consulted again and is never a write destination.
    if (!state_exists && !draft_exists && !state_error && !draft_error &&
        path == runtime_paths.sample_labeling_state_path) {
        auto legacy = LoadLegacySampleLabelingStateCache(runtime_paths,
            runtime_paths.legacy_sample_labeling_state_path, cancellation_checkpoint, policy);
        for (auto& [identity, source] : legacy.cache.sources) {
            for (auto& task : source.tasks) {
                if (!task.persistence.output_path) continue;
                task.persistence.pending_sample_indices.clear();
                task.persistence.metadata_save_pending = false;
                task.persistence.save_state = {.kind = SampleLabelSaveStateKind::AutosavedToOutput};
                task.values.ResetSparse(source.sample_count);
                task.label_set = {};
                task.canonical_metadata = {};
                if (task.persistence.output_format == SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar &&
                    policy == SampleLabelingStateCacheLoadPolicy::AllowPersistentOutputs) {
                    auto loaded = SampleAnnotationIoAdapter{}.LoadLabelResult(
                        *task.persistence.output_path, source.sample_count, cancellation_checkpoint);
                    if (loaded && loaded->metadata && loaded->metadata->task_id == task.task_id) {
                        task.values = SampleLabelingValues{};
                        task.values.Complete() = std::move(loaded->values);
                        task.task_name = loaded->metadata->task_name;
                        task.label_set = loaded->metadata->label_set;
                    }
                }
                RebuildSampleLabelingTaskStatistics(task, cancellation_checkpoint);
            }
        }
        return legacy;
    }

    auto ordinary = LoadSampleLabelingOrdinaryState(runtime_paths, path, cancellation_checkpoint);
    auto drafts = LoadSampleLabelingDraftCheckpoints(draft_path, cancellation_checkpoint);
    SampleLabelingStateCacheLoadResult result;
    const auto report = [&](VersionedJsonCacheLoadIssueKind issue, const std::string& diagnostic) {
        if (issue == VersionedJsonCacheLoadIssueKind::None) return;
        switch (issue) {
        case VersionedJsonCacheLoadIssueKind::ReadFailed:
            result.issue_kind = SampleLabelingStateCacheLoadIssueKind::ReadFailed; break;
        case VersionedJsonCacheLoadIssueKind::UnsupportedFormatOrSchema:
            result.issue_kind = SampleLabelingStateCacheLoadIssueKind::UnsupportedFormatOrSchema; break;
        default: result.issue_kind = SampleLabelingStateCacheLoadIssueKind::InvalidDocument; break;
        }
        result.warning = "Could not read labeling state or unsaved checkpoint.";
        result.diagnostic_detail = diagnostic;
    };
    report(ordinary.issue_kind, ordinary.diagnostic);
    report(drafts.issue_kind, drafts.diagnostic);
    for (const auto& [identity, registration] : ordinary.owner.sources) {
        auto& source = result.cache.sources[identity];
        source.sample_count = registration.sample_count;
        source.source_name = registration.source_name;
        source.source_fingerprint = registration.source_fingerprint;
        source.context_fingerprint = registration.context_fingerprint;
        source.active_task_id = registration.active_task_id;
        for (const auto& record : registration.tasks) {
            if (!record.output_path) continue; // Joined with its checkpoint below.
            if (policy == SampleLabelingStateCacheLoadPolicy::InternalDraftsOnly) {
                result.cache = {};
                report(VersionedJsonCacheLoadIssueKind::InvalidDocument,
                    "Persistent labeling output paths are not permitted in an automation state seed.");
                result.warning = result.diagnostic_detail;
                return result;
            }
            auto task = CreateSampleLabelingTask(record.task_id, record.display_name_hint.value_or(record.task_id), 0);
            task.values.ResetSparse(source.sample_count);
            task.session = record.session;
            task.persistence.output_path = record.output_path;
            task.persistence.output_format = record.output_format;
            task.persistence.save_state.kind = SampleLabelSaveStateKind::AutosavedToOutput;
            if (record.output_format == SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar &&
                policy == SampleLabelingStateCacheLoadPolicy::AllowPersistentOutputs) {
                std::string error;
                auto loaded = SampleAnnotationIoAdapter{}.LoadLabelResult(*record.output_path,
                    source.sample_count, cancellation_checkpoint, &error);
                if (loaded && loaded->metadata && loaded->metadata->task_id == record.task_id) {
                    task.values = SampleLabelingValues{};
                    task.values.Complete() = std::move(loaded->values);
                    task.task_name = loaded->metadata->task_name;
                    task.label_set = loaded->metadata->label_set;
                    RebuildSampleLabelingTaskStatistics(task, cancellation_checkpoint);
                }
            }
            source.tasks.push_back(std::move(task));
        }
    }
    for (const auto& [identity, checkpoint] : drafts.owner.sources) {
        if (cancellation_checkpoint) cancellation_checkpoint();
        auto& source = result.cache.sources[identity];
        const auto formal = std::find_if(source.tasks.begin(), source.tasks.end(), [&](const auto& task) {
            return task.task_id == checkpoint.draft.task_id && task.persistence.output_path;
        });
        if (formal != source.tasks.end()) continue; // Canonical registration supersedes stale checkpoint.
        if (source.sample_count != 0 &&
            (source.sample_count != checkpoint.sample_count ||
             source.source_fingerprint != checkpoint.source_fingerprint ||
             source.context_fingerprint != checkpoint.context_fingerprint)) {
            report(VersionedJsonCacheLoadIssueKind::InvalidDocument,
                "Draft checkpoint conflicts with registered source compatibility information.");
            continue;
        }
        source.sample_count = checkpoint.sample_count;
        source.source_name = checkpoint.source_name;
        source.source_fingerprint = checkpoint.source_fingerprint;
        source.context_fingerprint = checkpoint.context_fingerprint;
        const auto& draft = checkpoint.draft;
        auto task = CreateSampleLabelingTask(draft.task_id, draft.task_name, 0, draft.canonical_metadata);
        task.label_set = draft.label_set;
        task.values.Complete() = draft.values;
        if (const auto registration = ordinary.owner.sources.find(identity);
            registration != ordinary.owner.sources.end()) {
            for (const auto& record : registration->second.tasks) {
                if (record.task_id == draft.task_id && !record.output_path) task.session = record.session;
            }
        }
        RebuildSampleLabelingTaskStatistics(task, cancellation_checkpoint);
        source.tasks.push_back(std::move(task));
    }
    for (auto& [identity, source] : result.cache.sources) {
        if (source.active_task_id && std::none_of(source.tasks.begin(), source.tasks.end(),
                [&](const auto& task) { return task.task_id == *source.active_task_id; }))
            source.active_task_id.reset(); // Missing best-effort checkpoint is not document corruption.
    }
    return result;
}

bool SaveSampleLabelingStateCache(const RuntimePaths& runtime_paths,
    const std::filesystem::path& path, const SampleLabelingStateCache& cache,
    std::string* error_message, bool* ordinary_state_saved)
{
    if (ordinary_state_saved) *ordinary_state_saved = false;
    if (path.empty() || !ValidateCacheStructure(cache, error_message)) return false;
    SampleLabelingOrdinaryState ordinary;
    SampleLabelingDraftCheckpoints drafts;
    for (const auto& [identity, source] : cache.sources) {
        auto& registration = ordinary.sources[identity];
        registration.sample_count = source.sample_count;
        registration.source_name = source.source_name;
        registration.source_fingerprint = source.source_fingerprint;
        registration.context_fingerprint = source.context_fingerprint;
        registration.active_task_id = source.active_task_id;
        for (const auto& task : source.tasks) {
            registration.tasks.push_back({task.task_id, task.persistence.output_path,
                task.persistence.output_format, task.session,
                task.persistence.output_path ? std::optional{task.task_name} : std::nullopt});
            if (task.persistence.output_path) continue;
            if (!task.values.IsComplete() || drafts.sources.contains(identity)) {
                SetError(error_message, "A source must have at most one complete unsaved draft checkpoint.");
                return false;
            }
            drafts.sources.emplace(identity, SampleLabelingSourceDraftCheckpoint{
                source.sample_count, source.source_name, source.source_fingerprint, source.context_fingerprint,
                {task.task_id, task.task_name, task.canonical_metadata, task.label_set, task.values.Complete()}});
        }
    }
    std::string state_error, draft_error;
    const bool state_saved = SaveSampleLabelingOrdinaryState(runtime_paths, path, ordinary, &state_error);
    if (ordinary_state_saved) *ordinary_state_saved = state_saved;
    const auto checkpoint_path = SampleLabelingDraftCheckpointPath(runtime_paths, path);
    if (!state_saved) {
        // Draft edits may still checkpoint when ordinary state is unavailable.
        // Do not remove an old slot until its replacement registration landed.
        auto previous = LoadSampleLabelingDraftCheckpoints(checkpoint_path);
        if (previous.issue_kind != VersionedJsonCacheLoadIssueKind::None) {
            SetError(error_message, std::move(state_error));
            return false;
        }
        for (auto& [identity, checkpoint] : previous.owner.sources)
            drafts.sources.try_emplace(identity, std::move(checkpoint));
    }
    // Cleanup follows canonical registration. Failure cannot roll back an ASDF
    // publication; the controller reports output and local-state status separately.
    const bool drafts_saved = SaveSampleLabelingDraftCheckpoints(
        checkpoint_path, drafts, &draft_error);
    if (!state_saved || !drafts_saved)
        SetError(error_message, !state_saved ? std::move(state_error) : std::move(draft_error));
    return state_saved && drafts_saved;
}

bool CommitSampleLabelingStateCachePatch(
    const RuntimePaths& runtime_paths,
    const std::filesystem::path& path,
    const SampleLabelingStateCachePatch& patch,
    std::string* error_message,
    std::chrono::milliseconds commit_lock_wait, bool* ordinary_state_saved)
{
    if (ordinary_state_saved) *ordinary_state_saved = false;
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
            runtime_paths,
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
        runtime_paths,
        path,
        latest.cache,
        error_message, ordinary_state_saved);
}

bool HasSampleLabelingOutputPathConflict(
    const SampleLabelingStateCache& cache,
    const SampleLabelingTask& candidate,
    std::string_view source_identity)
{
    if (!candidate.persistence.output_path) {
        return false;
    }
    std::unordered_set<std::string> requested_keys;
    for (std::string key :
         OutputArtifactOwnerKeys(
             *candidate.persistence.output_path,
             candidate.persistence.output_format,
             true)) {
        requested_keys.insert(std::move(key));
    }
    for (const auto& [candidate_source, state] :
         cache.sources) {
        for (const SampleLabelingTask& task : state.tasks) {
            if (!task.persistence.output_path) {
                continue;
            }
            if (candidate_source == source_identity &&
                task.task_id == candidate.task_id) {
                continue;
            }
            for (const std::string& candidate_key :
                 OutputArtifactOwnerKeys(
                     *task.persistence.output_path,
                     task.persistence.output_format,
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
