#include "domain/sample_filter.h"
#include "domain/source_collection_manifest.h"
#include "ui/sample_annotation_labeling_rules.h"
#include "ui/sample_sorting_sources.h"
#include "ui/sample_workflow_source_policy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
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
        annotation.values.push_back(
            specforge::SampleAnnotationValue{static_cast<std::int64_t>(value)});
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
        annotation.values.push_back(specforge::SampleAnnotationValue{std::move(value)});
    }
    return annotation;
}

specforge::SampleAnnotationResult MakeFloatAnnotation(
    std::string name,
    std::filesystem::path path,
    std::vector<double> values)
{
    specforge::SampleAnnotationResult annotation;
    annotation.name = std::move(name);
    annotation.path = std::move(path);
    annotation.kind = specforge::SampleAnnotationKind::ContinuousFloat;
    annotation.values.reserve(values.size());
    for (const double value : values) {
        annotation.values.push_back(specforge::SampleAnnotationValue{value});
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
        specforge::TaskIdForNewSampleLabelingTask("Quality review", &tasks) == "quality-review-2",
        "new task ids should never reuse an existing task record");
    Require(
        specforge::SampleLabelingTaskNameForOutputPath("saved-review.npy") == "saved-review",
        "formal task names should derive from the selected output filename");
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

void TestMetadataActivationPlanRejectsSamePathIdentityMismatch()
{
    const std::filesystem::path path = TempPath("_metadata_mismatch.npy");
    std::vector<specforge::SampleLabelingTask> tasks;
    tasks.push_back(MakeTask("local-task", "Local task", 3, path));
    specforge::SampleAnnotationResult annotation = MakeIntegerAnnotation("External task", path, {5, -1, 9});
    specforge::SampleLabelResultMetadata metadata;
    metadata.task_id = "external-task";
    metadata.task_name = "External task";

    const specforge::SampleAnnotationLabelingActivationPlan plan =
        specforge::PlanSampleAnnotationLabelingActivation(
            specforge::SampleAnnotationLabelingActivationRequest{
                .annotation = &annotation,
                .active_source_tasks = &tasks,
                .metadata = &metadata});

    Require(
        plan.kind == specforge::SampleAnnotationLabelingActivationKind::None,
        "a same-path local task must not activate when sidecar task identity differs");

    tasks[0].task_id = metadata.task_id;
    tasks[0].values.resize(2);
    const specforge::SampleAnnotationLabelingActivationPlan count_mismatch_plan =
        specforge::PlanSampleAnnotationLabelingActivation(
            specforge::SampleAnnotationLabelingActivationRequest{
                .annotation = &annotation,
                .active_source_tasks = &tasks,
                .metadata = &metadata});
    Require(
        count_mismatch_plan.kind == specforge::SampleAnnotationLabelingActivationKind::None,
        "a same-path local task must not activate when its sample count differs");
}

void TestCanonicalAsdfAnnotationRemainsReadOnly()
{
    const std::filesystem::path path = TempPath("_canonical.asdf");
    specforge::SampleAnnotationResult annotation =
        MakeIntegerAnnotation("wire annotation", path, {-1, 5, 9});
    annotation.relationship =
        specforge::SampleAnnotationWorkflowRelationship::
            ExternalLabelResult;
    auto document =
        std::make_shared<specforge::SampleLabelingDocument>();
    document->source.base_identity = "source-base";
    document->source.kind = "npy";
    document->source.name = "source.npy";
    document->source.fingerprint = "source-fingerprint";
    document->source.sample_count = 3;
    document->annotation.name = "quality-code";
    document->annotation.values = {-1, 5, 9};
    document->labeling.id = "canonical-quality";
    document->labeling.name = "Canonical quality";
    document->labeling.labels = {
        {5, "bad", "b"},
        {9, "good", "g"},
    };
    annotation.labeling_document = std::move(document);

    specforge::SampleAnnotationLabelingActivationPlan plan =
        specforge::PlanSampleAnnotationLabelingActivation(
            specforge::SampleAnnotationLabelingActivationRequest{
                .annotation = &annotation});
    Require(
        plan.kind ==
                specforge::SampleAnnotationLabelingActivationKind::
                    None,
        "canonical ASDF annotations must not enter the NPY task persistence owner");

    std::vector<specforge::SampleLabelingTask> tasks;
    tasks.push_back(
        MakeTask(
            "canonical-quality",
            "Canonical quality",
            3,
            path));
    plan = specforge::PlanSampleAnnotationLabelingActivation(
        specforge::SampleAnnotationLabelingActivationRequest{
            .annotation = &annotation,
            .active_source_tasks = &tasks});
    Require(
        plan.kind ==
                specforge::SampleAnnotationLabelingActivationKind::
                    None &&
            specforge::FindLocalTaskForLoadedAnnotation(
                &tasks,
                annotation) == nullptr,
        "canonical ASDF annotations must remain read-only even when a local task shares their path");

    const specforge::SampleFilterSource filter =
        specforge::BuildAnnotationFilterSource(annotation);
    const auto unlabeled = std::find_if(
        filter.options.begin(),
        filter.options.end(),
        [](const specforge::SampleFilterValueOption& option) {
            return option.key == "-1";
        });
    Require(
        unlabeled != filter.options.end() &&
            unlabeled->represents_unlabeled_value &&
            unlabeled->display_text == "Unlabeled (-1)",
        "canonical missing semantics should project into annotation filtering");
}

void TestSampleNameSortingSource()
{
    specforge::SourceCollectionManifest manifest;
    manifest.sample_names = {"gamma", "alpha", "beta"};

    std::optional<specforge::SampleSortingSource> source =
        specforge::BuildSampleSortingSource(nullptr, nullptr, 3, "source-order");
    Require(source.has_value(), "source-order should build without annotation context");
    Require(source->id == "source-order", "source-order source should use the fixed id");
    Require(
        std::get<std::uint64_t>(source->values[2]) == 2,
        "source-order values should preserve row indexes");

    source = specforge::BuildSampleSortingSource(&manifest, nullptr, 3, "sample-name");

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
    Require(
        std::get<std::int64_t>(source->values[0]) == 2,
        "integer annotation values should sort numerically");

    source = specforge::BuildSampleSortingSource(
        &manifest,
        nullptr,
        3,
        specforge::BuildAnnotationFilterSourceId(manifest.annotations[1]));
    Require(source.has_value(), "plain text annotations should build sortable values");
    Require(std::get<std::string>(source->values[1]) == "x", "text annotation values should sort lexically");
}

void TestTypedAnnotationSortingPreservesNumericPrecision()
{
    specforge::SourceCollectionManifest manifest;

    specforge::SampleAnnotationResult signed_annotation;
    signed_annotation.name = "Signed";
    signed_annotation.path = TempPath("_signed_i8.npy");
    signed_annotation.kind = specforge::SampleAnnotationKind::CategoricalInteger;
    signed_annotation.values = {
        specforge::SampleAnnotationValue{std::numeric_limits<std::int64_t>::max()},
        specforge::SampleAnnotationValue{std::numeric_limits<std::int64_t>::max() - 1},
    };
    manifest.annotations.push_back(signed_annotation);

    const std::string signed_source_id =
        specforge::BuildAnnotationFilterSourceId(manifest.annotations.back());
    std::optional<specforge::SampleSortingSource> source =
        specforge::BuildSampleSortingSource(&manifest, nullptr, 2, signed_source_id);
    Require(source.has_value(), "int64 annotation should remain sortable");
    Require(
        std::get<std::int64_t>(source->values[0]) ==
            std::numeric_limits<std::int64_t>::max(),
        "int64 annotation should not pass through double");

    specforge::SampleNavigationSortChoice choice;
    choice.active = true;
    choice.values = source->values;
    specforge::SampleNavigationSequenceInput input;
    input.source_row_count = 2;
    input.sort_choice = &choice;
    Require(
        specforge::BuildSampleNavigationSequence(input).ordered_rows ==
            std::vector<std::size_t>({1, 0}),
        "adjacent large int64 values should sort distinctly");

    specforge::SampleAnnotationResult unsigned_annotation;
    unsigned_annotation.name = "Unsigned";
    unsigned_annotation.path = TempPath("_unsigned_u8.npy");
    unsigned_annotation.kind = specforge::SampleAnnotationKind::CategoricalInteger;
    unsigned_annotation.values = {
        specforge::SampleAnnotationValue{std::numeric_limits<std::uint64_t>::max()},
        specforge::SampleAnnotationValue{std::numeric_limits<std::uint64_t>::max() - 1},
    };
    manifest.annotations.push_back(unsigned_annotation);
    source = specforge::BuildSampleSortingSource(
        &manifest,
        nullptr,
        2,
        specforge::BuildAnnotationFilterSourceId(manifest.annotations.back()));
    Require(source.has_value(), "uint64 annotation should remain sortable");
    Require(
        std::get<std::uint64_t>(source->values[0]) ==
            std::numeric_limits<std::uint64_t>::max(),
        "uint64 annotation should not pass through double");

    const double adjacent = std::nextafter(1.0, 2.0);
    specforge::SampleAnnotationResult float_annotation =
        MakeFloatAnnotation("Adjacent", TempPath("_adjacent_f8.npy"), {adjacent, 1.0});
    manifest.annotations.push_back(float_annotation);
    source = specforge::BuildSampleSortingSource(
        &manifest,
        nullptr,
        2,
        specforge::BuildAnnotationFilterSourceId(manifest.annotations.back()));
    Require(source.has_value(), "finite double annotation should remain sortable");
    choice.values = source->values;
    Require(
        specforge::BuildSampleNavigationSequence(input).ordered_rows ==
            std::vector<std::size_t>({1, 0}),
        "adjacent doubles should sort by their typed values");
    Require(
        specforge::FormatSampleAnnotationValue(float_annotation, float_annotation.values[0]) !=
            specforge::FormatSampleAnnotationValue(float_annotation, float_annotation.values[1]),
        "adjacent doubles should have distinct round-trip display projections");
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
    float_manifest.annotations.push_back(
        MakeFloatAnnotation(
            "Score",
            TempPath("_score.npy"),
            {0.5, std::numeric_limits<double>::quiet_NaN()}));
    Require(
        specforge::BuildSampleSortingSourceViews(&float_manifest, nullptr, 2).empty(),
        "non-comparable float annotations should be excluded from view sources");
}

void TestWorkflowSourcePolicyOwnsDisplayNamesFilteringAndSorting()
{
    const std::filesystem::path rank_path = TempPath("_policy_rank.npy");
    specforge::SourceCollectionManifest manifest;
    manifest.sample_names = {"gamma", "alpha", "beta"};
    manifest.annotations.push_back(MakeIntegerAnnotation("Rank", rank_path, {2, 1, 1}));
    const std::string source_id = specforge::BuildAnnotationFilterSourceId(manifest.annotations[0]);

    specforge::SampleWorkflowSourcePolicy policy;
    const specforge::SampleWorkflowSourceContext context{
        .collection = &manifest,
        .labeling_tasks = nullptr,
        .sample_count = 3};

    specforge::SourceCollectionFilterView filter_view = policy.BuildFilterView(context);
    Require(filter_view.sources.empty(), "policy should start with no selected sample filter sources");
    Require(filter_view.available_sources.size() == 1, "policy should expose the annotation as addable filtering");
    Require(filter_view.available_sources[0].name == "Rank", "policy should use the default annotation display name");

    specforge::SourceCollectionSampleSortingView sorting_view = policy.BuildSortingView(context);
    Require(
        sorting_view.available_sources.size() == 1 && sorting_view.available_sources[0].name == "Rank",
        "policy should expose the annotation as addable sorting");

    Require(
        policy.RenameAnnotationDisplayName(context, rank_path, "  Quality rank  "),
        "policy should accept an annotation display-name override");
    filter_view = policy.BuildFilterView(context);
    Require(
        filter_view.available_sources[0].name == "Quality rank",
        "policy should apply display names to filter sources");
    sorting_view = policy.BuildSortingView(context);
    Require(
        sorting_view.available_sources[0].name == "Quality rank",
        "policy should apply display names to sorting sources");

    Require(policy.AddFilterSource(context, source_id), "policy should explicitly select a sample filter source");
    Require(
        policy.SetFilterValueSelected(context, source_id, "1", true),
        "policy should mutate selected sample filter values");
    const specforge::SampleFilterEvaluation evaluation = policy.EvaluateFilters(context);
    Require(evaluation.active, "selected values should activate sample filtering");
    Require(evaluation.included_count == 2, "sample filtering should include matching annotation values");

    Require(policy.SetSampleSortSource(context, source_id), "policy should select an annotation sort source");
    policy.SetSampleSortDirection(specforge::SampleNavigationSortDirection::Descending);
    specforge::SampleWorkflowSortChoiceResult sort_choice = policy.BuildSortChoice(context, true);
    Require(sort_choice.choice.has_value(), "policy should build a navigation sort choice");
    Require(
        sort_choice.choice->direction == specforge::SampleNavigationSortDirection::Descending,
        "policy should keep the selected source direction");
    Require(
        std::get<std::int64_t>(sort_choice.choice->values[0]) == 2,
        "policy sort choice should use annotation values");
}

void TestWorkflowSourcePolicyTracksOwnerGenerations()
{
    const std::filesystem::path rank_path =
        TempPath("_generation_rank.npy");
    const std::filesystem::path other_path =
        TempPath("_generation_other.npy");
    specforge::SourceCollectionManifest manifest;
    std::vector<specforge::SampleLabelingTask> tasks;
    specforge::SampleWorkflowSourcePolicy policy;
    specforge::SampleWorkflowSourceContext context{
        .collection = &manifest,
        .labeling_tasks = &tasks,
        .sample_count = 3,
        .context_generation = 1,
        .labeling_generation = 1,
    };

    Require(
        policy.BuildFilterView(context)
                .available_sources.empty() &&
            policy.BuildSortingView(context)
                .available_sources.empty(),
        "empty owner projections should prime empty caches");

    manifest.annotations.push_back(
        MakeIntegerAnnotation(
            "Rank",
            rank_path,
            {2, 1, 1}));
    ++context.context_generation;
    specforge::SourceCollectionFilterView filter_view =
        policy.BuildFilterView(context);
    specforge::SourceCollectionSampleSortingView
        sorting_view = policy.BuildSortingView(context);
    Require(
        filter_view.available_sources.size() == 1 &&
            sorting_view.available_sources.size() == 1,
        "an in-place annotation load should refresh both caches when the owner generation advances");

    manifest.annotations[0].name = "Updated rank";
    ++context.context_generation;
    filter_view = policy.BuildFilterView(context);
    sorting_view = policy.BuildSortingView(context);
    Require(
        filter_view.available_sources[0].name ==
                "Updated rank" &&
            sorting_view.available_sources[0].name ==
                "Updated rank",
        "an in-place annotation display change should refresh cached projections");

    tasks.push_back(
        MakeTask(
            "rank",
            "Local rank",
            3,
            rank_path));
    ++context.labeling_generation;
    filter_view = policy.BuildFilterView(context);
    sorting_view = policy.BuildSortingView(context);
    Require(
        filter_view.available_sources.size() == 1 &&
            filter_view.available_sources[0].name ==
                "Local rank",
        "an in-place labeling task addition should refresh the filter projection");
    Require(
        sorting_view.available_sources.empty(),
        "a local labeling task should invalidate the matching annotation sort source");

    tasks[0].task_name = "Reviewed rank";
    ++context.labeling_generation;
    filter_view = policy.BuildFilterView(context);
    Require(
        filter_view.available_sources[0].name ==
            "Reviewed rank",
        "an in-place labeling task mutation should refresh its cached display");

    tasks.clear();
    ++context.labeling_generation;
    sorting_view = policy.BuildSortingView(context);
    Require(
        sorting_view.available_sources.size() == 1,
        "removing an in-place labeling task should restore the annotation sort source");

    manifest.annotations.clear();
    ++context.context_generation;
    Require(
        policy.BuildFilterView(context)
                .available_sources.empty() &&
            policy.BuildSortingView(context)
                .available_sources.empty(),
        "an in-place annotation removal should refresh both caches");

    manifest = specforge::SourceCollectionManifest{};
    manifest.annotations.push_back(
        MakeIntegerAnnotation(
            "Other source",
            other_path,
            {3, 2, 1}));
    ++context.context_generation;
    filter_view = policy.BuildFilterView(context);
    sorting_view = policy.BuildSortingView(context);
    Require(
        filter_view.available_sources.size() == 1 &&
            filter_view.available_sources[0].name ==
                "Other source" &&
            sorting_view.available_sources.size() == 1 &&
            sorting_view.available_sources[0].name ==
                "Other source",
        "reusing the same manifest address for a new source generation must not reuse stale projections");
}

void TestLegacyV2MappedAnnotationFilterKeysMigrateToCanonicalKeys()
{
    const std::filesystem::path annotation_path = TempPath("_legacy_mapped_filter.npy");
    const std::filesystem::path cache_path = TempPath("_legacy_mapped_filter_cache.json");
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);

    specforge::SourceCollectionManifest manifest;
    manifest.annotations.push_back(
        MakeIntegerAnnotation("Quality", annotation_path, {5, 9, 5}));
    specforge::SampleLabelResultMetadata metadata;
    metadata.task_id = "quality";
    metadata.task_name = "Quality";
    metadata.label_set.labels.push_back(
        specforge::SampleLabelDefinition{5, "bad", 'b'});
    metadata.label_set.labels.push_back(
        specforge::SampleLabelDefinition{9, "good", 'g'});
    manifest.annotations[0].label_metadata = std::move(metadata);
    manifest.annotations[0].relationship =
        specforge::SampleAnnotationWorkflowRelationship::ExternalLabelResult;

    const std::string source_id =
        specforge::BuildAnnotationFilterSourceId(manifest.annotations[0]);
    specforge::SampleWorkflowSourceState legacy_state;
    legacy_state.selected_filter_source_ids.push_back(source_id);
    specforge::SampleFilterCondition legacy_condition;
    legacy_condition.source_id = source_id;
    legacy_condition.allowed_value_keys.insert("bad (5)");
    legacy_state.filter_conditions.push_back(std::move(legacy_condition));

    specforge::SampleWorkflowStateCache legacy_v2_cache;
    legacy_v2_cache.sources_by_identity.emplace(
        "legacy-mapped-source",
        std::move(legacy_state));
    Require(
        specforge::SaveSampleWorkflowStateCache(cache_path, legacy_v2_cache),
        "legacy schema-v2 workflow cache fixture should save");
    {
        std::ifstream cache_stream(cache_path);
        std::string cache_text;
        std::getline(cache_stream, cache_text, '\0');
        Require(
            cache_text.find("\"schema_version\": 2") != std::string::npos,
            "legacy mapped-key regression fixture should remain pinned to schema v2");
    }

    const specforge::SampleWorkflowStateCacheLoadResult load =
        specforge::LoadSampleWorkflowStateCache(cache_path);
    Require(load.warning.empty(), "valid workflow cache should load without warning");
    const specforge::SampleWorkflowStateCache& loaded = load.cache;
    std::filesystem::remove(cache_path, cleanup_error);
    const auto restored = loaded.sources_by_identity.find("legacy-mapped-source");
    Require(
        restored != loaded.sources_by_identity.end(),
        "legacy schema-v2 workflow cache fixture should load");

    specforge::SampleWorkflowSourcePolicy policy;
    policy.RestoreState(restored->second);
    const specforge::SampleWorkflowSourceContext context{
        .collection = &manifest,
        .labeling_tasks = nullptr,
        .sample_count = 3};
    const specforge::SampleFilterEvaluation evaluation =
        policy.EvaluateFilters(context);

    Require(evaluation.active, "mapped legacy filter should remain active");
    Require(
        evaluation.included_count == 2,
        "mapped legacy display key should match its canonical annotation code");
    const specforge::SampleWorkflowSourceState migrated = policy.StoreState();
    Require(
        migrated.filter_conditions.size() == 1 &&
            migrated.filter_conditions[0].allowed_value_keys ==
                std::unordered_set<std::string>{"5"},
        "restored mapped filter state should be rewritten to the canonical key");

    specforge::SampleWorkflowSourceState unmappable = restored->second;
    unmappable.filter_conditions[0].allowed_value_keys = {"renamed label (5)"};
    policy.RestoreState(unmappable);
    const specforge::SampleFilterEvaluation safe_evaluation =
        policy.EvaluateFilters(context);
    Require(
        !safe_evaluation.active && safe_evaluation.included_count == 3,
        "an unmappable legacy display key should safely disable its filter condition");
    Require(
        policy.StoreState().filter_conditions.empty(),
        "an unmappable legacy display key should be removed from restored state");
}

}  // namespace

int main()
{
    TestTaskNamingRules();
    TestPlainAnnotationActivationPlanCreatesEditableTask();
    TestMetadataActivationPlanReusesExistingTask();
    TestMetadataCreatePlanAvoidsTaskIdCollision();
    TestMetadataActivationPlanRejectsSamePathIdentityMismatch();
    TestCanonicalAsdfAnnotationRemainsReadOnly();
    TestSampleNameSortingSource();
    TestAnnotationSortingSources();
    TestTypedAnnotationSortingPreservesNumericPrecision();
    TestAnnotationSortingExclusions();
    TestWorkflowSourcePolicyOwnsDisplayNamesFilteringAndSorting();
    TestWorkflowSourcePolicyTracksOwnerGenerations();
    TestLegacyV2MappedAnnotationFilterKeysMigrateToCanonicalKeys();
    return 0;
}
