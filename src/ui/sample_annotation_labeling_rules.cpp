#include "ui/sample_annotation_labeling_rules.h"

#include <algorithm>
#include <cctype>
#include <system_error>
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

std::string TaskIdFromName(std::string_view task_name)
{
    std::string task_id;
    bool previous_dash = false;
    for (const unsigned char character : task_name) {
        if (std::isalnum(character) != 0) {
            task_id.push_back(static_cast<char>(std::tolower(character)));
            previous_dash = false;
        } else if (!task_id.empty() && !previous_dash) {
            task_id.push_back('-');
            previous_dash = true;
        }
    }
    while (!task_id.empty() && task_id.back() == '-') {
        task_id.pop_back();
    }
    return task_id.empty() ? "labeling" : task_id;
}

bool TaskIdExists(const std::vector<SampleLabelingTask>& tasks, std::string_view task_id)
{
    return std::any_of(tasks.begin(), tasks.end(), [task_id](const SampleLabelingTask& task) {
        return task.task_id == task_id;
    });
}

bool TaskIdExists(const std::vector<SampleLabelingTask>* tasks, std::string_view task_id)
{
    return tasks != nullptr && TaskIdExists(*tasks, task_id);
}

std::string UniqueTaskIdFromName(
    std::string_view task_name,
    const std::vector<SampleLabelingTask>* active_source_tasks)
{
    const std::string base_task_id = TaskIdFromName(task_name);
    if (!TaskIdExists(active_source_tasks, base_task_id)) {
        return base_task_id;
    }

    for (std::size_t suffix = 2; suffix < 10000; ++suffix) {
        std::string candidate = base_task_id;
        candidate += '-';
        candidate += std::to_string(suffix);
        if (!TaskIdExists(*active_source_tasks, candidate)) {
            return candidate;
        }
    }
    return base_task_id + "-copy";
}

bool PathExists(const std::filesystem::path& path)
{
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

bool PathsReferToSameFile(const std::filesystem::path& left, const std::filesystem::path& right)
{
    if (left.empty() || right.empty()) {
        return false;
    }
    std::error_code equivalent_error;
    if (PathExists(left) && PathExists(right) &&
        std::filesystem::equivalent(left, right, equivalent_error) && !equivalent_error) {
        return true;
    }
    return left.lexically_normal() == right.lexically_normal();
}

std::optional<std::vector<int>> AnnotationIntegerValues(const SampleAnnotationResult& annotation)
{
    if (annotation.kind != SampleAnnotationKind::CategoricalInteger) {
        return std::nullopt;
    }

    std::vector<int> values;
    values.reserve(annotation.values.size());
    for (const SampleAnnotationValue& value : annotation.values) {
        if (!value.integer_value) {
            return std::nullopt;
        }
        values.push_back(*value.integer_value);
    }
    return values;
}

SampleLabelSet LabelSetFromUniqueIntegerValues(const std::vector<int>& values)
{
    std::vector<int> unique_values;
    unique_values.reserve(values.size());
    for (int value : values) {
        if (value == kUnlabeledSampleLabelCode) {
            continue;
        }
        unique_values.push_back(value);
    }
    std::sort(unique_values.begin(), unique_values.end());
    unique_values.erase(std::unique(unique_values.begin(), unique_values.end()), unique_values.end());

    SampleLabelSet label_set;
    label_set.labels.reserve(unique_values.size());
    for (int value : unique_values) {
        label_set.labels.push_back(SampleLabelDefinition{value, std::to_string(value), '\0'});
    }
    return label_set;
}

const SampleLabelingTask* FindTaskByMetadataOutput(
    const std::vector<SampleLabelingTask>* active_source_tasks,
    const SampleAnnotationResult& annotation,
    const SampleLabelResultMetadata& metadata)
{
    if (active_source_tasks == nullptr || annotation.path.empty()) {
        return nullptr;
    }

    const auto match = std::find_if(
        active_source_tasks->begin(),
        active_source_tasks->end(),
        [&annotation, &metadata](const SampleLabelingTask& task) {
            return task.output_path && task.task_id == metadata.task_id &&
                   task.values.size() == annotation.values.size() &&
                   PathsReferToSameFile(*task.output_path, annotation.path);
        });
    return match == active_source_tasks->end() ? nullptr : &*match;
}

}  // namespace

std::string DefaultedSampleLabelingTaskName(std::string task_name)
{
    task_name = TrimAscii(std::move(task_name));
    return task_name.empty() ? "Manual labeling" : task_name;
}

std::string TaskIdForCreatedSampleLabelingTask(
    std::string_view task_name,
    const std::vector<SampleLabelingTask>* active_source_tasks)
{
    const std::string base_task_id = TaskIdFromName(task_name);
    if (active_source_tasks == nullptr) {
        return base_task_id;
    }
    const auto base_match = std::find_if(
        active_source_tasks->begin(),
        active_source_tasks->end(),
        [&base_task_id](const SampleLabelingTask& task) {
            return task.task_id == base_task_id;
        });
    if (base_match == active_source_tasks->end() || base_match->task_name == task_name) {
        return base_task_id;
    }
    return UniqueTaskIdFromName(task_name, active_source_tasks);
}

const SampleLabelingTask* FindLocalTaskForLoadedAnnotation(
    const std::vector<SampleLabelingTask>* active_source_tasks,
    const SampleAnnotationResult& annotation)
{
    if (annotation.label_metadata) {
        return FindTaskByMetadataOutput(active_source_tasks, annotation, *annotation.label_metadata);
    }

    if (annotation.kind != SampleAnnotationKind::CategoricalInteger || annotation.path.empty() ||
        active_source_tasks == nullptr) {
        return nullptr;
    }

    const auto match = std::find_if(
        active_source_tasks->begin(),
        active_source_tasks->end(),
        [&annotation](const SampleLabelingTask& task) {
            return task.output_path && task.values.size() == annotation.values.size() &&
                   PathsReferToSameFile(*task.output_path, annotation.path);
        });
    return match == active_source_tasks->end() ? nullptr : &*match;
}

SampleAnnotationLabelingActivationPlan PlanSampleAnnotationLabelingActivation(
    const SampleAnnotationLabelingActivationRequest& request)
{
    SampleAnnotationLabelingActivationPlan plan;
    if (request.annotation == nullptr) {
        return plan;
    }

    const SampleAnnotationResult& annotation = *request.annotation;
    if (request.active_task != nullptr) {
        if (request.metadata == nullptr || request.active_task->task_id != request.metadata->task_id) {
            return plan;
        }
    }

    if (request.metadata != nullptr) {
        if (const SampleLabelingTask* existing_task =
                FindTaskByMetadataOutput(request.active_source_tasks, annotation, *request.metadata)) {
            plan.kind = SampleAnnotationLabelingActivationKind::ActivateExistingTask;
            plan.task_id = existing_task->task_id;
            return plan;
        }
    }
    if (request.active_task != nullptr) {
        return plan;
    }

    std::optional<std::vector<int>> values = AnnotationIntegerValues(annotation);
    if (!values) {
        return plan;
    }

    std::string task_name = request.metadata != nullptr && !request.metadata->task_name.empty()
        ? request.metadata->task_name
        : annotation.name;
    task_name = DefaultedSampleLabelingTaskName(std::move(task_name));
    const std::string preferred_task_id =
        request.metadata != nullptr && !request.metadata->task_id.empty()
        ? request.metadata->task_id
        : UniqueTaskIdFromName(task_name, request.active_source_tasks);
    const std::string task_id = TaskIdExists(request.active_source_tasks, preferred_task_id)
        ? UniqueTaskIdFromName(task_name, request.active_source_tasks)
        : preferred_task_id;

    plan.kind = SampleAnnotationLabelingActivationKind::CreateTaskFromAnnotation;
    plan.task_id = task_id;
    plan.task_name = std::move(task_name);
    plan.label_set = request.metadata != nullptr
        ? request.metadata->label_set
        : LabelSetFromUniqueIntegerValues(*values);
    plan.values = std::move(*values);
    plan.metadata_clean = request.metadata != nullptr;
    return plan;
}

}  // namespace specforge
