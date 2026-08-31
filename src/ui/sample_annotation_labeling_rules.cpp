#include "ui/sample_annotation_labeling_rules.h"

#include "domain/utf8.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
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

struct AnnotationPromotionProjection {
    SampleLabelSet label_set;
    std::vector<int> values;
};

SampleLabelSet LabelSetFromUniqueIntegerValues(
    const std::vector<int>& values);

std::optional<AnnotationPromotionProjection>
BuildAnnotationPromotionProjection(
    const SampleAnnotationResult& annotation)
{
    AnnotationPromotionProjection projection;
    projection.values.reserve(annotation.values.size());
    if (annotation.kind ==
        SampleAnnotationKind::CategoricalInteger) {
        for (const SampleAnnotationValue& value :
             annotation.values) {
            const std::optional<int> integer_value =
                SampleAnnotationValueAsInt(value);
            if (!integer_value) {
                return std::nullopt;
            }
            projection.values.push_back(*integer_value);
        }
        projection.label_set =
            LabelSetFromUniqueIntegerValues(
                projection.values);
        return projection;
    }

    if (annotation.kind != SampleAnnotationKind::Text ||
        !annotation.artifact_provenance ||
        annotation.artifact_provenance->format != "csv") {
        return std::nullopt;
    }

    std::vector<std::string> label_names;
    label_names.reserve(annotation.values.size());
    for (const SampleAnnotationValue& value : annotation.values) {
        if (value.missing) {
            continue;
        }
        const std::string* text =
            std::get_if<std::string>(&value.semantic);
        if (text == nullptr ||
            !IsValidUtf8WithNonWhitespace(*text)) {
            return std::nullopt;
        }
        label_names.push_back(*text);
    }
    std::sort(label_names.begin(), label_names.end());
    label_names.erase(
        std::unique(label_names.begin(), label_names.end()),
        label_names.end());
    if (label_names.size() >
        static_cast<std::size_t>(
            std::numeric_limits<int>::max())) {
        return std::nullopt;
    }

    projection.label_set.labels.reserve(label_names.size());
    for (std::size_t index = 0;
         index < label_names.size();
         ++index) {
        projection.label_set.labels.push_back(
            SampleLabelDefinition{
                static_cast<int>(index),
                label_names[index],
                '\0'});
    }
    for (const SampleAnnotationValue& value : annotation.values) {
        if (value.missing) {
            projection.values.push_back(
                kUnlabeledSampleLabelCode);
            continue;
        }
        const std::string& text =
            std::get<std::string>(value.semantic);
        const auto match = std::lower_bound(
            label_names.begin(), label_names.end(), text);
        projection.values.push_back(static_cast<int>(
            std::distance(label_names.begin(), match)));
    }
    return projection;
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

const SampleLabelingTask* FindTaskByAnnotationOutput(
    const std::vector<SampleLabelingTask>* active_source_tasks,
    const SampleAnnotationResult& annotation)
{
    if (active_source_tasks == nullptr || annotation.path.empty()) {
        return nullptr;
    }

    const auto match = std::find_if(
        active_source_tasks->begin(),
        active_source_tasks->end(),
        [&annotation](const SampleLabelingTask& task) {
            return task.output_path && PathsReferToSameFile(*task.output_path, annotation.path);
        });
    return match == active_source_tasks->end() ? nullptr : &*match;
}

}  // namespace

std::string DefaultedSampleLabelingTaskName(std::string task_name)
{
    task_name = TrimAscii(std::move(task_name));
    return task_name.empty() ? "Manual labeling" : task_name;
}

bool CanPromoteSampleAnnotationToLabeling(
    const SampleAnnotationResult& annotation)
{
    return annotation.artifact_provenance.has_value() &&
        BuildAnnotationPromotionProjection(annotation)
            .has_value();
}

const SampleLabelingTask* FindLocalTaskForLoadedAnnotation(
    const std::vector<SampleLabelingTask>* active_source_tasks,
    const SampleAnnotationResult& annotation)
{
    if (annotation.labeling_document) {
        if (active_source_tasks == nullptr ||
            annotation.path.empty()) {
            return nullptr;
        }
        const auto match = std::find_if(
            active_source_tasks->begin(),
            active_source_tasks->end(),
            [&annotation](const SampleLabelingTask& task) {
                return task.output_path &&
                    task.output_format ==
                        SampleLabelingOutputArtifactFormat::
                            CanonicalAsdf &&
                    task.task_id ==
                        annotation.labeling_document
                            ->labeling.id &&
                    task.values.size() ==
                        annotation.values.size() &&
                    PathsReferToSameFile(
                        *task.output_path,
                        annotation.path);
            });
        return match == active_source_tasks->end()
            ? nullptr
            : &*match;
    }
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
    if (annotation.labeling_document) {
        const SampleLabelingTask* existing_task =
            FindLocalTaskForLoadedAnnotation(
                request.active_source_tasks,
                annotation);
        if (existing_task != nullptr) {
            if (request.active_task != nullptr &&
                request.active_task->task_id !=
                    existing_task->task_id) {
                return plan;
            }
            plan.kind =
                SampleAnnotationLabelingActivationKind::
                    ActivateExistingTask;
            plan.task_id = existing_task->task_id;
            return plan;
        }

        const std::string& document_task_id =
            annotation.labeling_document->labeling.id;
        if (annotation.path.empty() ||
            document_task_id.empty()) {
            return plan;
        }
        plan.kind =
            SampleAnnotationLabelingActivationKind::
                AdoptCanonicalAsdfTask;
        plan.task_id = document_task_id;
        return plan;
    }
    if (request.active_task != nullptr) {
        if (request.metadata == nullptr ||
            request.active_task->task_id != request.metadata->task_id) {
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
    } else if (const SampleLabelingTask* existing_task =
                   FindLocalTaskForLoadedAnnotation(request.active_source_tasks, annotation)) {
        plan.kind = SampleAnnotationLabelingActivationKind::ActivateExistingTask;
        plan.task_id = existing_task->task_id;
        return plan;
    }
    if (FindTaskByAnnotationOutput(request.active_source_tasks, annotation) != nullptr) {
        return plan;
    }
    if (request.active_task != nullptr) {
        return plan;
    }

    std::optional<AnnotationPromotionProjection> projection =
        BuildAnnotationPromotionProjection(annotation);
    if (!projection || !annotation.artifact_provenance) {
        return plan;
    }

    std::string task_name = request.metadata != nullptr && !request.metadata->task_name.empty()
        ? request.metadata->task_name
        : annotation.name;
    task_name = DefaultedSampleLabelingTaskName(std::move(task_name));
    plan.kind = SampleAnnotationLabelingActivationKind::CreateTaskFromAnnotation;
    plan.task_name = std::move(task_name);
    plan.label_set = request.metadata != nullptr
        ? request.metadata->label_set
        : std::move(projection->label_set);
    plan.values = std::move(projection->values);
    plan.origin.kind = "annotation_promotion";
    plan.origin.annotation = *annotation.artifact_provenance;
    // A promoted task always receives a fresh UUID, so even a verified legacy
    // sidecar cannot already contain the new canonical task identity.
    plan.metadata_clean = false;
    return plan;
}

}  // namespace specforge
