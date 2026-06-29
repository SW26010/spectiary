#pragma once

#include "domain/sample_annotation_io.h"
#include "domain/sample_labeling.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace specforge {

enum class SampleAnnotationLabelingActivationKind {
    None,
    ActivateExistingTask,
    CreateTaskFromAnnotation,
};

struct SampleAnnotationLabelingActivationRequest {
    const SampleAnnotationResult* annotation = nullptr;
    const SampleLabelingTask* active_task = nullptr;
    const std::vector<SampleLabelingTask>* active_source_tasks = nullptr;
    const SampleLabelResultMetadata* metadata = nullptr;
};

struct SampleAnnotationLabelingActivationPlan {
    SampleAnnotationLabelingActivationKind kind = SampleAnnotationLabelingActivationKind::None;
    std::string task_id;
    std::string task_name;
    SampleLabelSet label_set;
    std::vector<int> values;
    bool metadata_clean = false;
};

[[nodiscard]] std::string DefaultedSampleLabelingTaskName(std::string task_name);
[[nodiscard]] std::string TaskIdForCreatedSampleLabelingTask(
    std::string_view task_name,
    const std::vector<SampleLabelingTask>* active_source_tasks);
[[nodiscard]] const SampleLabelingTask* FindLocalTaskForLoadedAnnotation(
    const std::vector<SampleLabelingTask>* active_source_tasks,
    const SampleAnnotationResult& annotation);
[[nodiscard]] SampleAnnotationLabelingActivationPlan PlanSampleAnnotationLabelingActivation(
    const SampleAnnotationLabelingActivationRequest& request);

}  // namespace specforge
