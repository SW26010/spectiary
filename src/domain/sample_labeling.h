#pragma once

#include "domain/canonical_timestamp.h"

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

struct SampleLabelingAnnotationOrigin {
    std::string name;
    std::string format;
    std::optional<std::string> fingerprint;

    [[nodiscard]] bool operator==(
        const SampleLabelingAnnotationOrigin&) const = default;
};

struct SampleLabelingOrigin {
    std::string kind = "manual";
    std::optional<SampleLabelingAnnotationOrigin> annotation;

    [[nodiscard]] bool operator==(
        const SampleLabelingOrigin&) const = default;
};

// Canonical provenance stores only a portable artifact basename: valid
// non-blank UTF-8 with no root, directory component, or ASCII drive prefix.
[[nodiscard]] bool IsValidSampleLabelingAnnotationOriginName(
    std::string_view name) noexcept;

struct SampleLabelingAuthor {
    std::string name;
    std::optional<std::string> identifier;

    [[nodiscard]] bool operator==(
        const SampleLabelingAuthor&) const = default;
};

struct SampleLabelingTaskCanonicalMetadata {
    CanonicalTimestamp created_at;
    CanonicalTimestamp modified_at;
    SampleLabelingOrigin origin;
    std::optional<std::string> description;
    std::vector<SampleLabelingAuthor> authors;

    [[nodiscard]] bool operator==(
        const SampleLabelingTaskCanonicalMetadata&) const = default;
};

struct SampleLabelingTask {
    std::string task_id;
    std::string task_name;
    SampleLabelingTaskCanonicalMetadata canonical_metadata;
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
    // Durable write-ahead phase used only while a temporary task is becoming
    // its first canonical owner. A cache reader rolls this phase back to a
    // real temporary draft if the process exits before publication completes.
    bool initial_publication_pending = false;
    // Runtime projection provenance; never serialized. Canonical ASDF cache
    // records carry only sparse local state, so their placeholder values are
    // not data-bearing until projected over the canonical document.
    bool values_are_authoritative = true;
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

struct SampleLabelOutputPublicationResult {
    bool attempted = false;
    bool published = false;
    bool artifacts_replaced = false;
    bool retryable = false;
    std::string message;
};

[[nodiscard]] SampleLabelingTask CreateSampleLabelingTask(
    std::string task_id,
    std::string task_name,
    std::size_t sample_count);
[[nodiscard]] SampleLabelingTask CreateSampleLabelingTask(
    std::string task_id,
    std::string task_name,
    std::size_t sample_count,
    SampleLabelingTaskCanonicalMetadata canonical_metadata);
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
// Removes the snapshot-bound canonical base projection while retaining sparse
// local pending values and session metadata. Prepared/cache-shaped state must
// cross owner boundaries only in this structural form.
[[nodiscard]] bool DowngradeCanonicalSampleLabelingTaskToStructural(
    SampleLabelingTask& task) noexcept;

[[nodiscard]] SampleLabelWriteResult AssignSampleLabel(
    SampleLabelingTask& task,
    std::size_t sample_index,
    int code);
[[nodiscard]] SampleLabelWriteResult ClearSampleLabel(SampleLabelingTask& task, std::size_t sample_index);
void SelectSampleLabelTaskOutputPath(SampleLabelingTask& task, std::filesystem::path output_path);
void MarkSampleLabelTaskMetadataPending(SampleLabelingTask& task);
// Records a canonical semantic value mutation. modified_at is persisted with
// the complete replacement document generation.
void MarkCanonicalValueMutation(
    SampleLabelingTask& task,
    CanonicalTimestamp mutation_time);
void MarkCanonicalSemanticMutation(
    SampleLabelingTask& task,
    CanonicalTimestamp mutation_time);
void MarkSampleLabelTaskPersisted(SampleLabelingTask& task, SampleLabelSaveStateKind clean_state);
void MarkSampleLabelTaskSaveFailed(
    SampleLabelingTask& task,
    std::string message,
    SampleLabelSaveMessageKind message_kind =
        SampleLabelSaveMessageKind::SystemDetail);
[[nodiscard]] SampleLabelOutputPublicationResult
PublishLegacySampleLabelingTaskOutput(
    SampleLabelingTask& task,
    const SampleLabelResultMetadataSource* source = nullptr);

}  // namespace specforge
