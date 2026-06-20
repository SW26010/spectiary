#include "domain/sample_labeling.h"

#include "domain/npy_array_io.h"
#include "platform/atomic_file.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace specforge {
namespace {

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

void RefreshPendingSaveState(SampleLabelingTask& task)
{
    task.save_state.pending_count = task.pending_sample_indices.size();
    if (!task.pending_sample_indices.empty()) {
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

    if (label.shortcut != '\0') {
        for (SampleLabelDefinition& existing : label_set.labels) {
            if (existing.code != label.code && existing.shortcut == label.shortcut) {
                existing.shortcut = '\0';
            }
        }
    }

    auto match = std::find_if(label_set.labels.begin(), label_set.labels.end(), [&label](const auto& existing) {
        return existing.code == label.code;
    });
    if (match == label_set.labels.end()) {
        label_set.labels.push_back(std::move(label));
    } else {
        *match = std::move(label);
    }

    std::stable_sort(label_set.labels.begin(), label_set.labels.end(), [](const auto& left, const auto& right) {
        return left.code < right.code;
    });
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

std::string FormatSampleLabelValue(const SampleLabelSet& label_set, int code)
{
    if (code == kUnlabeledSampleLabelCode) {
        return "Unlabeled (-1)";
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
    task.save_state.pending_count = 0;
    task.save_state.kind = clean_state;
    task.save_state.message.clear();
}

void SelectSampleLabelTaskOutputPath(SampleLabelingTask& task, std::filesystem::path output_path)
{
    task.output_path = std::move(output_path);
    task.pending_sample_indices.clear();

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

void MarkSampleLabelTaskSaveFailed(SampleLabelingTask& task, std::string message)
{
    task.save_state.kind = SampleLabelSaveStateKind::Failed;
    task.save_state.pending_count = task.pending_sample_indices.size();
    task.save_state.message = std::move(message);
}

SampleLabelTaskPersistResult PersistSampleLabelingTaskResult(SampleLabelingTask& task)
{
    SampleLabelTaskPersistResult result;
    result.output_path_selected = task.output_path.has_value();
    if (!task.output_path) {
        return result;
    }

    std::string error;
    result.output_saved = SaveSampleLabelResultNpy(*task.output_path, task, &error);
    if (result.output_saved) {
        MarkSampleLabelTaskPersisted(task, SampleLabelSaveStateKind::AutosavedToOutput);
    } else {
        result.message = error.empty() ? "could not save label output" : std::move(error);
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

}  // namespace specforge
