#include "domain/sample_labeling.h"

#include "domain/sample_annotation_io.h"
#include "domain/utf8.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
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

bool HasPendingPersistence(const SampleLabelingTask& task)
{
    return !task.persistence.pending_sample_indices.empty() || task.persistence.metadata_save_pending;
}

bool HaveSameSampleLabels(
    const SampleLabelSet& left,
    const SampleLabelSet& right)
{
    if (left.labels.size() != right.labels.size()) {
        return false;
    }
    return std::all_of(
        left.labels.begin(),
        left.labels.end(),
        [&right](const SampleLabelDefinition& label) {
            const SampleLabelDefinition* match =
                FindSampleLabel(right, label.code);
            return match != nullptr &&
                match->name == label.name &&
                match->shortcut == label.shortcut;
        });
}

void RefreshPendingSaveState(SampleLabelingTask& task)
{
    task.persistence.save_state.pending_count = task.persistence.pending_sample_indices.size();
    if (HasPendingPersistence(task)) {
        task.persistence.save_state.kind = SampleLabelSaveStateKind::Pending;
        task.persistence.save_state.message_kind =
            SampleLabelSaveMessageKind::None;
        task.persistence.save_state.message.clear();
    } else if (task.persistence.output_path) {
        task.persistence.save_state.kind = SampleLabelSaveStateKind::AutosavedToOutput;
        task.persistence.save_state.message_kind =
            SampleLabelSaveMessageKind::None;
        task.persistence.save_state.message.clear();
    } else {
        task.persistence.save_state.kind = SampleLabelSaveStateKind::InternalDraftOnly;
        task.persistence.save_state.message_kind =
            SampleLabelSaveMessageKind::None;
        task.persistence.save_state.message.clear();
    }
}

}  // namespace

bool IsValidSampleLabelingAnnotationOriginName(
    std::string_view name) noexcept
{
    if (!IsValidUtf8WithNonWhitespace(name) ||
        name == "." || name == ".." ||
        name.find_first_of("/\\") != std::string_view::npos) {
        return false;
    }
    const bool has_ascii_drive_prefix = name.size() >= 2U &&
        ((name.front() >= 'A' && name.front() <= 'Z') ||
         (name.front() >= 'a' && name.front() <= 'z')) &&
        name[1] == ':';
    return !has_ascii_drive_prefix;
}

SampleLabelingTask CreateSampleLabelingTask(
    std::string task_id,
    std::string task_name,
    std::size_t sample_count)
{
    const CanonicalTimestamp created_at = CurrentCanonicalTimestamp();
    SampleLabelingTaskCanonicalMetadata canonical_metadata;
    canonical_metadata.created_at = created_at;
    canonical_metadata.modified_at = created_at;
    canonical_metadata.origin.kind = "manual";
    return CreateSampleLabelingTask(
        std::move(task_id),
        std::move(task_name),
        sample_count,
        std::move(canonical_metadata));
}

SampleLabelingTask CreateSampleLabelingTask(
    std::string task_id,
    std::string task_name,
    std::size_t sample_count,
    SampleLabelingTaskCanonicalMetadata canonical_metadata)
{
    SampleLabelingTask task;
    task.task_id = std::move(task_id);
    task.task_name = std::move(task_name);
    task.canonical_metadata = std::move(canonical_metadata);
    task.values.assign(sample_count, kUnlabeledSampleLabelCode);
    task.statistics.label_usage_counts.clear();
    task.statistics.labeled_count = 0;
    task.persistence.save_state.kind = SampleLabelSaveStateKind::InternalDraftOnly;
    return task;
}

bool DowngradeCanonicalSampleLabelingTaskToStructural(
    SampleLabelingTask& task) noexcept
{
    if (task.persistence.output_format !=
        SampleLabelingOutputArtifactFormat::CanonicalAsdf) {
        return false;
    }

    bool changed = task.values_are_authoritative ||
        task.statistics.labeled_count != 0 ||
        !task.statistics.label_usage_counts.empty() ||
        task.persistence.save_state.pending_count !=
            task.persistence.pending_sample_indices.size();
    for (std::size_t sample_index = 0;
         sample_index < task.values.size();
         ++sample_index) {
        if (task.persistence.pending_sample_indices.contains(
                sample_index)) {
            continue;
        }
        changed = changed ||
            task.values[sample_index] !=
                kUnlabeledSampleLabelCode;
        task.values[sample_index] =
            kUnlabeledSampleLabelCode;
    }
    task.values_are_authoritative = false;
    task.statistics.label_usage_counts.clear();
    task.statistics.labeled_count = 0;
    task.persistence.save_state.pending_count =
        task.persistence.pending_sample_indices.size();
    return changed;
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
    if (HaveSameSampleLabels(updated_label_set, task.label_set)) {
        return false;
    }
    task.label_set = std::move(updated_label_set);

    if (code_changed) {
        for (std::size_t index = 0; index < task.values.size(); ++index) {
            if (task.values[index] != original_code) {
                continue;
            }
            task.values[index] = updated_code;
            task.persistence.pending_sample_indices.insert(index);
        }
        RefreshPendingSaveState(task);
        RebuildSampleLabelingTaskStatistics(task);
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
            task.persistence.pending_sample_indices.insert(index);
        }
    }
    task.label_set.labels.erase(match);
    RefreshPendingSaveState(task);
    RebuildSampleLabelingTaskStatistics(task);
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

void RebuildSampleLabelingTaskStatistics(
    SampleLabelingTask& task,
    const std::function<void()>& cancellation_checkpoint)
{
    task.statistics.label_usage_counts.clear();
    task.statistics.labeled_count = 0;
    for (std::size_t index = 0; index < task.values.size(); ++index) {
        if ((index & 0xfffU) == 0U && cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        const int code = task.values[index];
        if (code != kUnlabeledSampleLabelCode) {
            ++task.statistics.label_usage_counts[code];
            ++task.statistics.labeled_count;
        }
    }
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
}

SampleLabelWriteResult AssignSampleLabel(SampleLabelingTask& task, std::size_t sample_index, int code)
{
    SampleLabelWriteResult result;
    result.sample_index = sample_index;
    result.current_code = code;
    if (sample_index >= task.values.size() || !ContainsSampleLabelCode(task.label_set, code)) {
        result.pending_count = task.persistence.pending_sample_indices.size();
        return result;
    }

    result.accepted = true;
    result.previous_code = task.values[sample_index];
    task.values[sample_index] = code;
    result.current_code = code;
    result.changed = result.previous_code != result.current_code;
    if (result.changed) {
        const auto previous = task.statistics.label_usage_counts.find(result.previous_code);
        const bool statistics_consistent = result.previous_code == kUnlabeledSampleLabelCode ||
                                           (previous != task.statistics.label_usage_counts.end() &&
                                            previous->second > 0 && task.statistics.labeled_count > 0);
        if (!statistics_consistent) {
            RebuildSampleLabelingTaskStatistics(task);
        } else {
            if (result.previous_code != kUnlabeledSampleLabelCode) {
                if (--previous->second == 0) {
                    task.statistics.label_usage_counts.erase(previous);
                }
                --task.statistics.labeled_count;
            }
            ++task.statistics.label_usage_counts[result.current_code];
            ++task.statistics.labeled_count;
        }
        result.advance_requested = task.session.auto_advance;
        task.session.remembered_position = sample_index;
        task.persistence.pending_sample_indices.insert(sample_index);
        RefreshPendingSaveState(task);
    }
    result.pending_count = task.persistence.pending_sample_indices.size();
    return result;
}

SampleLabelWriteResult ClearSampleLabel(SampleLabelingTask& task, std::size_t sample_index)
{
    SampleLabelWriteResult result;
    result.sample_index = sample_index;
    if (sample_index >= task.values.size()) {
        result.pending_count = task.persistence.pending_sample_indices.size();
        return result;
    }

    result.accepted = true;
    result.previous_code = task.values[sample_index];
    task.values[sample_index] = kUnlabeledSampleLabelCode;
    result.current_code = kUnlabeledSampleLabelCode;
    result.changed = result.previous_code != result.current_code;
    if (result.changed) {
        auto previous = task.statistics.label_usage_counts.find(result.previous_code);
        if (previous == task.statistics.label_usage_counts.end() || previous->second == 0 || task.statistics.labeled_count == 0) {
            RebuildSampleLabelingTaskStatistics(task);
        } else {
            if (--previous->second == 0) {
                task.statistics.label_usage_counts.erase(previous);
            }
            --task.statistics.labeled_count;
        }
        result.advance_requested = task.session.auto_advance;
        task.session.remembered_position = sample_index;
        task.persistence.pending_sample_indices.insert(sample_index);
        RefreshPendingSaveState(task);
    }
    result.pending_count = task.persistence.pending_sample_indices.size();
    return result;
}

void MarkSampleLabelTaskPersisted(SampleLabelingTask& task, SampleLabelSaveStateKind clean_state)
{
    task.persistence.initial_publication_pending = false;
    task.persistence.pending_sample_indices.clear();
    task.persistence.metadata_save_pending = false;
    task.persistence.save_state.pending_count = 0;
    task.persistence.save_state.kind = clean_state;
    task.persistence.save_state.message_kind =
        SampleLabelSaveMessageKind::None;
    task.persistence.save_state.message.clear();
}


void MarkSampleLabelTaskMetadataPending(SampleLabelingTask& task)
{
    if (!task.persistence.output_path) {
        return;
    }
    task.persistence.metadata_save_pending = true;
    RefreshPendingSaveState(task);
}

void MarkCanonicalValueMutation(
    SampleLabelingTask& task,
    CanonicalTimestamp mutation_time)
{
    if (mutation_time > task.canonical_metadata.modified_at) {
        task.canonical_metadata.modified_at = mutation_time;
    }
    RefreshPendingSaveState(task);
}

void MarkCanonicalSemanticMutation(
    SampleLabelingTask& task,
    CanonicalTimestamp mutation_time)
{
    if (mutation_time > task.canonical_metadata.modified_at) {
        task.canonical_metadata.modified_at = mutation_time;
    }
    if (task.persistence.output_path) {
        task.persistence.metadata_save_pending = true;
    }
    RefreshPendingSaveState(task);
}

void MarkSampleLabelTaskSaveFailed(
    SampleLabelingTask& task,
    std::string message,
    SampleLabelSaveMessageKind message_kind)
{
    task.persistence.save_state.kind = SampleLabelSaveStateKind::Failed;
    task.persistence.save_state.pending_count = task.persistence.pending_sample_indices.size();
    if (message.empty() &&
        message_kind == SampleLabelSaveMessageKind::SystemDetail) {
        message_kind = SampleLabelSaveMessageKind::OutputSaveFailed;
    }
    task.persistence.save_state.message_kind = message_kind;
    task.persistence.save_state.message = std::move(message);
}


}  // namespace specforge
