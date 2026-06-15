#include "domain/sample_labeling.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

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

std::string ShapeText(std::size_t value_count)
{
    return "(" + std::to_string(value_count) + ",)";
}

std::filesystem::path TemporaryOutputPath(const std::filesystem::path& path)
{
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path temporary = path;
    temporary += ".tmp.";
#ifdef _WIN32
    temporary += std::to_string(GetCurrentProcessId());
#else
    temporary += "pid";
#endif
    temporary += ".";
    temporary += std::to_string(timestamp);
    return temporary;
}

bool ReplaceFileAtomically(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& target_path,
    std::string* error_message)
{
#ifdef _WIN32
    if (MoveFileExW(
            temporary_path.c_str(),
            target_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
        return true;
    }
    if (error_message != nullptr) {
        *error_message = "could not replace output file: " + std::system_category().message(GetLastError());
    }
    return false;
#else
    std::error_code rename_error;
    std::filesystem::rename(temporary_path, target_path, rename_error);
    if (!rename_error) {
        return true;
    }
    if (error_message != nullptr) {
        *error_message = "could not replace output file: " + rename_error.message();
    }
    return false;
#endif
}

bool WriteInt32NpyFile(const std::filesystem::path& path, const std::vector<int>& values, std::string* error_message)
{
    std::error_code filesystem_error;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            if (error_message != nullptr) {
                *error_message = "could not create output directory";
            }
            return false;
        }
    }

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.good()) {
        if (error_message != nullptr) {
            *error_message = "could not open output file";
        }
        return false;
    }

    std::string header = "{'descr': '<i4', 'fortran_order': False, 'shape': ";
    header += ShapeText(values.size());
    header += ", }";

    constexpr std::size_t kPreambleSize = 10;
    const std::size_t header_with_newline = header.size() + 1;
    const std::size_t padding = (16 - ((kPreambleSize + header_with_newline) % 16)) % 16;
    header.append(padding, ' ');
    header.push_back('\n');
    if (header.size() > std::numeric_limits<std::uint16_t>::max()) {
        if (error_message != nullptr) {
            *error_message = "NPY header is too large";
        }
        return false;
    }

    constexpr std::array<unsigned char, 6> kMagic = {0x93, 'N', 'U', 'M', 'P', 'Y'};
    stream.write(reinterpret_cast<const char*>(kMagic.data()), static_cast<std::streamsize>(kMagic.size()));
    constexpr std::array<char, 2> kVersion = {1, 0};
    stream.write(kVersion.data(), static_cast<std::streamsize>(kVersion.size()));

    const auto header_length = static_cast<std::uint16_t>(header.size());
    const std::array<char, 2> length_bytes = {
        static_cast<char>(header_length & 0xffU),
        static_cast<char>((header_length >> 8U) & 0xffU),
    };
    stream.write(length_bytes.data(), static_cast<std::streamsize>(length_bytes.size()));
    stream.write(header.data(), static_cast<std::streamsize>(header.size()));

    for (const int value : values) {
        const auto int_value = static_cast<std::int32_t>(value);
        const auto raw_value = static_cast<std::uint32_t>(int_value);
        const std::array<unsigned char, 4> bytes = {
            static_cast<unsigned char>(raw_value & 0xffU),
            static_cast<unsigned char>((raw_value >> 8U) & 0xffU),
            static_cast<unsigned char>((raw_value >> 16U) & 0xffU),
            static_cast<unsigned char>((raw_value >> 24U) & 0xffU),
        };
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    if (!stream.good()) {
        if (error_message != nullptr) {
            *error_message = "could not write output file";
        }
        return false;
    }
    return true;
}

bool WriteInt32Npy(const std::filesystem::path& path, const std::vector<int>& values, std::string* error_message)
{
    if (error_message != nullptr) {
        error_message->clear();
    }

    const std::filesystem::path temporary_path = TemporaryOutputPath(path);
    if (!WriteInt32NpyFile(temporary_path, values, error_message)) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary_path, cleanup_error);
        return false;
    }
    if (!ReplaceFileAtomically(temporary_path, path, error_message)) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary_path, cleanup_error);
        return false;
    }
    return true;
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

    std::array<unsigned char, 6> magic = {};
    stream.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
    constexpr std::array<unsigned char, 6> kExpectedMagic = {0x93, 'N', 'U', 'M', 'P', 'Y'};
    if (!stream.good() || magic != kExpectedMagic) {
        if (error_message != nullptr) {
            *error_message = "label output is not an NPY file";
        }
        return std::nullopt;
    }

    std::array<unsigned char, 2> version = {};
    stream.read(reinterpret_cast<char*>(version.data()), static_cast<std::streamsize>(version.size()));
    if (!stream.good()) {
        if (error_message != nullptr) {
            *error_message = "NPY version header is truncated";
        }
        return std::nullopt;
    }

    std::uint32_t header_length = 0;
    if (version[0] == 1) {
        std::array<unsigned char, 2> length_bytes = {};
        stream.read(reinterpret_cast<char*>(length_bytes.data()), static_cast<std::streamsize>(length_bytes.size()));
        if (!stream.good()) {
            if (error_message != nullptr) {
                *error_message = "NPY v1 header length is truncated";
            }
            return std::nullopt;
        }
        header_length =
            static_cast<std::uint32_t>(static_cast<unsigned char>(length_bytes[0])) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(length_bytes[1])) << 8U);
    } else if (version[0] == 2 || version[0] == 3) {
        std::array<unsigned char, 4> length_bytes = {};
        stream.read(reinterpret_cast<char*>(length_bytes.data()), static_cast<std::streamsize>(length_bytes.size()));
        if (!stream.good()) {
            if (error_message != nullptr) {
                *error_message = "NPY v2/v3 header length is truncated";
            }
            return std::nullopt;
        }
        header_length =
            static_cast<std::uint32_t>(static_cast<unsigned char>(length_bytes[0])) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(length_bytes[1])) << 8U) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(length_bytes[2])) << 16U) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(length_bytes[3])) << 24U);
    } else {
        if (error_message != nullptr) {
            *error_message = "unsupported NPY version";
        }
        return std::nullopt;
    }

    std::string header(header_length, '\0');
    stream.read(header.data(), static_cast<std::streamsize>(header.size()));
    if (!stream.good()) {
        if (error_message != nullptr) {
            *error_message = "NPY header is truncated";
        }
        return std::nullopt;
    }

    if (header.find("'descr': '<i4'") == std::string::npos &&
        header.find("\"descr\": \"<i4\"") == std::string::npos) {
        if (error_message != nullptr) {
            *error_message = "label output is not int32";
        }
        return std::nullopt;
    }
    const std::string expected_shape = "'shape': (" + std::to_string(expected_count) + ",)";
    const std::string expected_shape_json = "\"shape\": [" + std::to_string(expected_count) + "]";
    if (header.find(expected_shape) == std::string::npos && header.find(expected_shape_json) == std::string::npos) {
        if (error_message != nullptr) {
            *error_message = "label output length does not match this source";
        }
        return std::nullopt;
    }

    std::vector<int> values;
    values.reserve(expected_count);
    for (std::size_t index = 0; index < expected_count; ++index) {
        std::array<unsigned char, 4> bytes = {};
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream.good()) {
            if (error_message != nullptr) {
                *error_message = "label output data is truncated";
            }
            return std::nullopt;
        }
        const std::uint32_t raw =
            static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1]) << 8U) |
            (static_cast<std::uint32_t>(bytes[2]) << 16U) |
            (static_cast<std::uint32_t>(bytes[3]) << 24U);
        values.push_back(static_cast<int>(static_cast<std::int32_t>(raw)));
    }
    return values;
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
