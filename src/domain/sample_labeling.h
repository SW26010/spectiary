#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
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

struct SampleLabelResultMetadataSource {
    std::string source_name;
    std::string source_fingerprint;
    std::string context_fingerprint;
    std::size_t spectrum_count = 0;
};

struct SampleLabelResultMetadata {
    std::string result_file;
    std::string task_id;
    std::size_t value_count = 0;
    std::string expected_dtype;
    int unlabeled_sentinel = kUnlabeledSampleLabelCode;
    std::string task_name;
    SampleLabelSet label_set;
    std::optional<SampleLabelResultMetadataSource> source;
};

struct SampleLabelResultMetadataLoadResult {
    std::optional<SampleLabelResultMetadata> metadata;
    std::string warning;
};

enum class SampleLabelSaveStateKind {
    InternalDraftOnly,
    AutosavedToOutput,
    Pending,
    Failed,
};

enum class SampleLabelSaveMessageKind {
    None,
    OutputPathAlreadyUsed,
    OutputSaveFailed,
    SystemDetail,
};

enum class SampleLabelingOutputArtifactFormat {
    None,
    LegacyNpyWithSidecar,
    CanonicalAsdf,
};

struct SampleLabelSaveState {
    SampleLabelSaveStateKind kind = SampleLabelSaveStateKind::InternalDraftOnly;
    std::size_t pending_count = 0;
    SampleLabelSaveMessageKind message_kind = SampleLabelSaveMessageKind::None;
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
    // Formal output ownership is explicit state. A path is present exactly
    // when the format is not None; callers must not infer it from a suffix.
    std::optional<std::filesystem::path> output_path;
    SampleLabelingOutputArtifactFormat output_format =
        SampleLabelingOutputArtifactFormat::None;
    std::unordered_set<std::size_t> pending_sample_indices;
    bool metadata_save_pending = false;
    SampleLabelSaveState save_state;
    // Derived, non-persisted presentation statistics. Mutations maintain these
    // incrementally; cache ingestion rebuilds them off the UI thread.
    std::unordered_map<int, std::size_t> label_usage_counts;
    std::size_t labeled_count = 0;
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
    bool array_saved = false;
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
[[nodiscard]] bool UpdateSampleLabel(
    SampleLabelingTask& task,
    int original_code,
    SampleLabelDefinition label,
    bool allow_used_code_change);
[[nodiscard]] bool RemoveSampleLabel(SampleLabelingTask& task, int code);
[[nodiscard]] std::optional<int> SampleLabelCodeForShortcut(const SampleLabelSet& label_set, char shortcut);
[[nodiscard]] std::string FormatSampleLabelValue(
    const SampleLabelSet& label_set,
    int code,
    int unlabeled_sentinel = kUnlabeledSampleLabelCode);
[[nodiscard]] std::size_t CountLabeledSamples(const SampleLabelingTask& task);
[[nodiscard]] std::size_t CountUnlabeledSamples(const SampleLabelingTask& task);
void RebuildSampleLabelingTaskStatistics(
    SampleLabelingTask& task,
    const std::function<void()>& cancellation_checkpoint = {});

[[nodiscard]] SampleLabelWriteResult AssignSampleLabel(
    SampleLabelingTask& task,
    std::size_t sample_index,
    int code);
[[nodiscard]] SampleLabelWriteResult ClearSampleLabel(SampleLabelingTask& task, std::size_t sample_index);
void SelectSampleLabelTaskOutputPath(SampleLabelingTask& task, std::filesystem::path output_path);
void MarkSampleLabelTaskMetadataPending(SampleLabelingTask& task);
void MarkSampleLabelTaskPersisted(SampleLabelingTask& task, SampleLabelSaveStateKind clean_state);
void MarkSampleLabelTaskSaveFailed(
    SampleLabelingTask& task,
    std::string message,
    SampleLabelSaveMessageKind message_kind =
        SampleLabelSaveMessageKind::SystemDetail);
[[nodiscard]] SampleLabelTaskPersistResult PersistSampleLabelingTaskResult(
    SampleLabelingTask& task,
    const SampleLabelResultMetadataSource* source = nullptr);

}  // namespace specforge
