#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace specforge {

inline constexpr int kUnlabeledSampleLabelCode = -1;

struct SampleLabelDefinition {
    int code = 0;
    std::string name;
    char shortcut = '\0';
};

struct SampleLabelSet {
    std::vector<SampleLabelDefinition> labels;
};

enum class SampleLabelSaveStateKind {
    InternalDraftOnly,
    AutosavedToOutput,
    Pending,
    Failed,
};

struct SampleLabelSaveState {
    SampleLabelSaveStateKind kind = SampleLabelSaveStateKind::InternalDraftOnly;
    std::size_t pending_count = 0;
    std::string message;
};

struct SampleLabelingTask {
    std::string task_id;
    std::string task_name;
    SampleLabelSet label_set;
    std::vector<int> values;
    bool auto_advance = false;
    bool skip_labeled_on_advance = false;
    std::optional<std::size_t> remembered_position;
    std::optional<std::filesystem::path> output_path;
    std::unordered_set<std::size_t> pending_sample_indices;
    SampleLabelSaveState save_state;
};

struct SampleLabelWriteResult {
    bool accepted = false;
    bool changed = false;
    bool advance_requested = false;
    std::size_t sample_index = 0;
    int previous_code = kUnlabeledSampleLabelCode;
    int current_code = kUnlabeledSampleLabelCode;
    std::size_t pending_count = 0;
};

struct SampleLabelTaskPersistResult {
    bool output_path_selected = false;
    bool output_saved = false;
    std::string message;
};

[[nodiscard]] SampleLabelingTask CreateSampleLabelingTask(
    std::string task_id,
    std::string task_name,
    std::size_t sample_count);
[[nodiscard]] bool IsValidSampleLabelShortcut(char shortcut);
[[nodiscard]] char NormalizeSampleLabelShortcut(char shortcut);
[[nodiscard]] bool ContainsSampleLabelCode(const SampleLabelSet& label_set, int code);
[[nodiscard]] const SampleLabelDefinition* FindSampleLabel(const SampleLabelSet& label_set, int code);
[[nodiscard]] int NextAvailableSampleLabelCode(const SampleLabelSet& label_set);
[[nodiscard]] bool UpsertSampleLabel(SampleLabelSet& label_set, SampleLabelDefinition label);
[[nodiscard]] std::optional<int> SampleLabelCodeForShortcut(const SampleLabelSet& label_set, char shortcut);
[[nodiscard]] std::string FormatSampleLabelValue(const SampleLabelSet& label_set, int code);
[[nodiscard]] std::size_t CountLabeledSamples(const SampleLabelingTask& task);
[[nodiscard]] std::size_t CountUnlabeledSamples(const SampleLabelingTask& task);

[[nodiscard]] SampleLabelWriteResult AssignSampleLabel(
    SampleLabelingTask& task,
    std::size_t sample_index,
    int code);
[[nodiscard]] SampleLabelWriteResult ClearSampleLabel(SampleLabelingTask& task, std::size_t sample_index);
void SelectSampleLabelTaskOutputPath(SampleLabelingTask& task, std::filesystem::path output_path);
void MarkSampleLabelTaskPersisted(SampleLabelingTask& task, SampleLabelSaveStateKind clean_state);
void MarkSampleLabelTaskSaveFailed(SampleLabelingTask& task, std::string message);
[[nodiscard]] SampleLabelTaskPersistResult PersistSampleLabelingTaskResult(SampleLabelingTask& task);
[[nodiscard]] bool SaveSampleLabelResultNpy(
    const std::filesystem::path& path,
    const SampleLabelingTask& task,
    std::string* error_message = nullptr);
[[nodiscard]] std::optional<std::vector<int>> LoadSampleLabelResultNpy(
    const std::filesystem::path& path,
    std::size_t expected_count,
    std::string* error_message = nullptr);

}  // namespace specforge
