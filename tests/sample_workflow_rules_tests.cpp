#include "domain/sample_filter.h"
#include "domain/source_collection_manifest.h"
#include "ui/sample_annotation_labeling_rules.h"
#include "ui/sample_sorting_sources.h"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::filesystem::path TempPath(std::string_view suffix)
{
    std::filesystem::path path = std::filesystem::temp_directory_path();
    path /= "specforge_sample_workflow_rules";
    path += std::string(suffix);
    return path;
}

specforge::SampleLabelingTask MakeTask(
    std::string task_id,
    std::string task_name,
    std::size_t sample_count,
    std::filesystem::path output_path = {})
{
    specforge::SampleLabelingTask task =
        specforge::CreateSampleLabelingTask(std::move(task_id), std::move(task_name), sample_count);
    if (!output_path.empty()) {
        task.output_path = std::move(output_path);
    }
    return task;
}

specforge::SampleAnnotationResult MakeIntegerAnnotation(
    std::string name,
    std::filesystem::path path,
    std::vector<int> values)
{
    specforge::SampleAnnotationResult annotation;
    annotation.name = std::move(name);
    annotation.path = std::move(path);
    annotation.kind = specforge::SampleAnnotationKind::CategoricalInteger;
    annotation.values.reserve(values.size());
    for (int value : values) {
        annotation.values.push_back(specforge::SampleAnnotationValue{std::to_string(value), value});
    }
    return annotation;
}

specforge::SampleAnnotationResult MakeTextAnnotation(
    std::string name,
    std::filesystem::path path,
    std::vector<std::string> values)
{
    specforge::SampleAnnotationResult annotation;
    annotation.name = std::move(name);
    annotation.path = std::move(path);
    annotation.kind = specforge::SampleAnnotationKind::Text;
    annotation.values.reserve(values.size());
    for (std::string& value : values) {
        annotation.values.push_back(specforge::SampleAnnotationValue{std::move(value), std::nullopt});
    }
    return annotation;
}

specforge::SampleAnnotationResult MakeFloatAnnotation(
    std::string name,
    std::filesystem::path path,
    std::vector<std::string> values)
{
    specforge::SampleAnnotationResult annotation;
    annotation.name = std::move(name);
    annotation.path = std::move(path);
    annotation.kind = specforge::SampleAnnotationKind::ContinuousFloat;
    annotation.values.reserve(values.size());
    for (std::string& value : values) {
        annotation.values.push_back(specforge::SampleAnnotationValue{std::move(value), std::nullopt});
    }
    return annotation;
}

void TestTaskNamingRules()
{
    std::vector<specforge::SampleLabelingTask> tasks;
    tasks.push_back(MakeTask("quality-review", "Quality review", 3));

    Require(
        specforge::DefaultedSampleLabelingTaskName("  Quality review  ") == "Quality review",
        "task names should trim ASCII whitespace");
    Require(
        specforge::DefaultedSampleLabelingTaskName("   ") == "Manual labeling",
        "blank task names should use the default label");
    Require(
        specforge::TaskIdForCreatedSampleLabelingTask("Quality review", &tasks) == "quality-review",
        "same name should reuse the stable task id");

    tasks[0].task_name = "Different name";
    Require(
        specforge::TaskIdForCreatedSampleLabelingTask("Quality review", &tasks) == "quality-review-2",
        "same base id with a different name should allocate a suffix");
}

void TestPlainAnnotationActivationPlanCreatesEditableTask()
{
    const std::filesystem::path path = TempPath("_plain.npy");
    specforge::SampleAnnotationResult annotation =
        MakeIntegerAnnotation("  Quality rank  ", path, {7, specforge::kUnlabeledSampleLabelCode, 5});

    specforge::SampleAnnotationLabelingActivationPlan plan =
        specforge::PlanSampleAnnotationLabelingActivation(
            specforge::SampleAnnotationLabelingActivationRequest{.annotation = &annotation});

    Require(
        plan.kind == specforge::SampleAnnotationLabelingActivationKind::CreateTaskFromAnnotation,
        "plain integer annotations should create editable labeling tasks");
    Require(plan.task_id == "quality-rank", "plain activation should derive a stable task id");
    Require(plan.task_name == "Quality rank", "plain activation should trim the task name");
    Require(!plan.metadata_clean, "plain activation should require metadata to be written");
    Require(plan.values == std::vector<int>({7, specforge::kUnlabeledSampleLabelCode, 5}), "values should be copied");
    Require(plan.label_set.labels.size() == 2, "label set should contain unique labeled values only");
    Require(plan.label_set.labels[0].code == 5, "label set should sort unique values");
    Require(plan.label_set.labels[1].code == 7, "label set should include the second unique value");
}

void TestMetadataActivationPlanReusesExistingTask()
{
    const std::filesystem::path path = TempPath("_metadata.npy");
    std::vector<specforge::SampleLabelingTask> tasks;
    tasks.push_back(MakeTask("quality", "Quality", 3, path));
    specforge::SampleAnnotationResult annotation = MakeIntegerAnnotation("Quality", path, {5, -1, 9});
    specforge::SampleLabelResultMetadata metadata;
    metadata.task_id = "quality";
    metadata.task_name = "Quality";

    specforge::SampleAnnotationLabelingActivationPlan plan =
        specforge::PlanSampleAnnotationLabelingActivation(
            specforge::SampleAnnotationLabelingActivationRequest{
                .annotation = &annotation,
                .active_source_tasks = &tasks,
                .metadata = &metadata});

    Require(
        plan.kind == specforge::SampleAnnotationLabelingActivationKind::ActivateExistingTask,
        "metadata-backed annotations should activate the matching local task");
    Require(plan.task_id == "quality", "activation should target the matching local task id");

    const specforge::SampleLabelingTask active_other = MakeTask("other", "Other", 3);
    plan = specforge::PlanSampleAnnotationLabelingActivation(
        specforge::SampleAnnotationLabelingActivationRequest{
            .annotation = &annotation,
            .active_task = &active_other,
            .active_source_tasks = &tasks,
            .metadata = &metadata});
    Require(
        plan.kind == specforge::SampleAnnotationLabelingActivationKind::None,
        "a different active task should block metadata activation");
}

void TestMetadataCreatePlanAvoidsTaskIdCollision()
{
    const std::filesystem::path path = TempPath("_metadata_new.npy");
    std::vector<specforge::SampleLabelingTask> tasks;
    tasks.push_back(MakeTask("quality", "Quality", 3, TempPath("_other.npy")));
    specforge::SampleAnnotationResult annotation = MakeIntegerAnnotation("Quality", path, {5, -1, 9});
    specforge::SampleLabelResultMetadata metadata;
    metadata.task_id = "quality";
    metadata.task_name = "Quality";
    metadata.label_set.labels.push_back(specforge::SampleLabelDefinition{5, "bad", 'b'});

    specforge::SampleAnnotationLabelingActivationPlan plan =
        specforge::PlanSampleAnnotationLabelingActivation(
            specforge::SampleAnnotationLabelingActivationRequest{
                .annotation = &annotation,
                .active_source_tasks = &tasks,
                .metadata = &metadata});

    Require(
        plan.kind == specforge::SampleAnnotationLabelingActivationKind::CreateTaskFromAnnotation,
        "metadata without a matching output task should create a local task");
    Require(plan.task_id == "quality-2", "metadata task id collisions should be avoided");
    Require(plan.metadata_clean, "metadata-backed creation should start clean");
    Require(plan.label_set.labels.size() == 1 && plan.label_set.labels[0].name == "bad", "metadata labels should be reused");
}

void TestSampleNameSortingSource()
{
    specforge::SourceCollectionManifest manifest;
    manifest.sample_names = {"gamma", "alpha", "beta"};

    std::optional<specforge::SampleSortingSource> source =
        specforge::BuildSampleSortingSource(&manifest, nullptr, 3, "sample-name");

    Require(source.has_value(), "matching sample names should build a sorting source");
    Require(source->id == "sample-name", "sample-name source should use the fixed id");
    Require(std::get<std::string>(source->values[0]) == "gamma", "sample-name source should preserve row values");
    Require(
        specforge::BuildSampleSortingSource(&manifest, nullptr, 2, "sample-name") == std::nullopt,
        "mismatched sample-name count should reject the source");
}

void TestAnnotationSortingSources()
{
    const std::filesystem::path rank_path = TempPath("_rank.npy");
    const std::filesystem::path text_path = TempPath("_text.npy");
    specforge::SourceCollectionManifest manifest;
    manifest.sample_names = {"c", "a", "b"};
    manifest.annotations.push_back(MakeIntegerAnnotation("Rank", rank_path, {2, 1, 1}));
    manifest.annotations.push_back(MakeTextAnnotation("Group", text_path, {"z", "x", "y"}));

    const std::string rank_source_id = specforge::BuildAnnotationFilterSourceId(manifest.annotations[0]);
    const std::vector<specforge::SourceCollectionSampleSortSourceView> views =
        specforge::BuildSampleSortingSourceViews(&manifest, nullptr, 3);
    Require(views.size() == 3, "sample names and two plain annotations should be sort sources");
    Require(views[1].id == rank_source_id, "integer annotation source should use shared annotation ids");

    std::optional<specforge::SampleSortingSource> source =
        specforge::BuildSampleSortingSource(&manifest, nullptr, 3, rank_source_id);
    Require(source.has_value(), "plain integer annotations should build sortable values");
    Require(std::get<double>(source->values[0]) == 2.0, "integer annotation values should sort numerically");

    source = specforge::BuildSampleSortingSource(
        &manifest,
        nullptr,
        3,
        specforge::BuildAnnotationFilterSourceId(manifest.annotations[1]));
    Require(source.has_value(), "plain text annotations should build sortable values");
    Require(std::get<std::string>(source->values[1]) == "x", "text annotation values should sort lexically");
}

void TestAnnotationSortingExclusions()
{
    const std::filesystem::path rank_path = TempPath("_local_rank.npy");
    specforge::SourceCollectionManifest manifest;
    manifest.annotations.push_back(MakeIntegerAnnotation("Rank", rank_path, {2, 1, 1}));
    const std::string source_id = specforge::BuildAnnotationFilterSourceId(manifest.annotations[0]);

    std::vector<specforge::SampleLabelingTask> tasks;
    tasks.push_back(MakeTask("rank", "Rank", 3, rank_path));
    Require(
        !specforge::BuildSampleSortingSource(&manifest, &tasks, 3, source_id).has_value(),
        "annotations owned by local labeling tasks should not be sort sources");
    Require(
        specforge::BuildSampleSortingSourceViews(&manifest, &tasks, 3).empty(),
        "local-task annotation sources should be excluded from view sources when names are unavailable");

    manifest.annotations[0].label_metadata = specforge::SampleLabelResultMetadata{};
    Require(
        !specforge::BuildSampleSortingSource(&manifest, nullptr, 3, source_id).has_value(),
        "metadata-backed label result annotations should not be sort sources");

    specforge::SourceCollectionManifest float_manifest;
    float_manifest.annotations.push_back(MakeFloatAnnotation("Score", TempPath("_score.npy"), {"0.5", "bad"}));
    Require(
        specforge::BuildSampleSortingSourceViews(&float_manifest, nullptr, 2).empty(),
        "non-comparable float annotations should be excluded from view sources");
}

}  // namespace

int main()
{
    TestTaskNamingRules();
    TestPlainAnnotationActivationPlanCreatesEditableTask();
    TestMetadataActivationPlanReusesExistingTask();
    TestMetadataCreatePlanAvoidsTaskIdCollision();
    TestSampleNameSortingSource();
    TestAnnotationSortingSources();
    TestAnnotationSortingExclusions();
    return 0;
}
