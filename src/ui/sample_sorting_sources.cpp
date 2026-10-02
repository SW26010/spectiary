#include "ui/sample_sorting_sources.h"

#include "domain/sample_filter.h"
#include "ui/sample_annotation_labeling_rules.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <utility>

namespace spectiary {
namespace {

constexpr std::string_view kSourceOrderSortSourceId = "source-order";

SourceCollectionSampleSortSourceView MakeSampleSortSourceView(
    std::string id,
    std::string name,
    std::filesystem::path annotation_path = {})
{
    SourceCollectionSampleSortSourceView view;
    view.id = std::move(id);
    view.name = std::move(name);
    view.annotation_path = std::move(annotation_path);
    return view;
}

std::optional<SourceCollectionSampleSortSourceView> BuildSampleNameSortingSourceView(
    const SourceCollectionManifest& context,
    std::size_t sample_count)
{
    if (context.sample_names.size() != sample_count) {
        return std::nullopt;
    }
    return MakeSampleSortSourceView("sample-name", "Sample name");
}

std::optional<SampleSortingSource> BuildSampleNameSortingSource(
    const SourceCollectionManifest& context,
    std::size_t sample_count,
    const std::function<void()>& cancellation_checkpoint)
{
    if (context.sample_names.size() != sample_count) {
        return std::nullopt;
    }

    SampleSortingSource source;
    source.id = "sample-name";
    source.name = "Sample name";
    source.values.reserve(context.sample_names.size());
    for (std::size_t index = 0; index < context.sample_names.size(); ++index) {
        if ((index & 0xfffU) == 0U && cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        source.values.push_back(MakeSampleNavigationSortValue(context.sample_names[index]));
    }
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
    return source;
}

std::optional<SampleSortingSource> BuildSourceOrderSortingSource(
    std::size_t sample_count,
    const std::function<void()>& cancellation_checkpoint)
{
    if (sample_count == 0) {
        return std::nullopt;
    }

    SampleSortingSource source;
    source.id = std::string{kSourceOrderSortSourceId};
    source.name = "Source order";
    source.values.reserve(sample_count);
    for (std::size_t row = 0; row < sample_count; ++row) {
        if ((row & 0xfffU) == 0U && cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        source.values.push_back(
            MakeSampleNavigationSortValue(static_cast<std::uint64_t>(row)));
    }
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
    return source;
}

bool IsAnnotationSortingSourceCandidate(
    const std::vector<SampleLabelingTask>* active_source_tasks,
    const SampleAnnotationResult& annotation,
    std::size_t sample_count)
{
    return annotation.values.size() == sample_count &&
           annotation.relationship == SampleAnnotationWorkflowRelationship::PlainAnnotation &&
           !annotation.label_metadata &&
           FindLocalTaskForLoadedAnnotation(active_source_tasks, annotation) == nullptr;
}

std::optional<SampleSortingSource> BuildAnnotationSortingSource(
    const std::vector<SampleLabelingTask>* active_source_tasks,
    const SampleAnnotationResult& annotation,
    std::size_t sample_count,
    const std::function<void()>& cancellation_checkpoint)
{
    if (!IsAnnotationSortingSourceCandidate(active_source_tasks, annotation, sample_count)) {
        return std::nullopt;
    }

    SampleSortingSource source;
    source.id = BuildAnnotationFilterSourceId(annotation);
    source.name = annotation.name;
    source.values.reserve(annotation.values.size());
    for (std::size_t index = 0; index < annotation.values.size(); ++index) {
        if ((index & 0xfffU) == 0U && cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        const SampleAnnotationValue& value = annotation.values[index];
        if (value.missing) {
            return std::nullopt;
        }
        switch (annotation.kind) {
        case SampleAnnotationKind::CategoricalInteger:
            if (const std::int64_t* signed_value =
                    std::get_if<std::int64_t>(&value.semantic)) {
                source.values.push_back(MakeSampleNavigationSortValue(*signed_value));
            } else if (const std::uint64_t* unsigned_value =
                           std::get_if<std::uint64_t>(&value.semantic)) {
                source.values.push_back(MakeSampleNavigationSortValue(*unsigned_value));
            } else {
                return std::nullopt;
            }
            break;
        case SampleAnnotationKind::ContinuousFloat:
            if (const double* floating_value = std::get_if<double>(&value.semantic);
                floating_value != nullptr && std::isfinite(*floating_value)) {
                source.values.push_back(MakeSampleNavigationSortValue(*floating_value));
            } else {
                return std::nullopt;
            }
            break;
        case SampleAnnotationKind::Text:
            if (const std::string* text = std::get_if<std::string>(&value.semantic)) {
                source.values.push_back(MakeSampleNavigationSortValue(*text));
            } else {
                return std::nullopt;
            }
            break;
        }
    }
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
    return source;
}

}  // namespace

std::vector<SourceCollectionSampleSortSourceView> BuildSampleSortingSourceViews(
    const SourceCollectionManifest* context,
    const std::vector<SampleLabelingTask>* active_source_tasks,
    std::size_t sample_count)
{
    return BuildSampleSortingSourceViews(context, active_source_tasks, sample_count, {});
}

std::vector<SourceCollectionSampleSortSourceView> BuildSampleSortingSourceViews(
    const SourceCollectionManifest* context,
    const std::vector<SampleLabelingTask>* active_source_tasks,
    std::size_t sample_count,
    const std::function<void()>& cancellation_checkpoint)
{
    std::vector<SourceCollectionSampleSortSourceView> sources;
    if (context == nullptr || sample_count == 0) {
        return sources;
    }

    sources.reserve(context->annotations.size() + 1);
    if (std::optional<SourceCollectionSampleSortSourceView> sample_names =
            BuildSampleNameSortingSourceView(*context, sample_count)) {
        sources.push_back(std::move(*sample_names));
    }
    for (std::size_t index = 0; index < context->annotations.size(); ++index) {
        if ((index & 0xfffU) == 0U && cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        const SampleAnnotationResult& annotation = context->annotations[index];
        if (std::optional<SampleSortingSource> annotation_source =
                BuildAnnotationSortingSource(
                    active_source_tasks,
                    annotation,
                    sample_count,
                    cancellation_checkpoint)) {
            sources.push_back(MakeSampleSortSourceView(
                std::move(annotation_source->id),
                std::move(annotation_source->name),
                annotation.path));
        }
    }
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
    return sources;
}

std::optional<SampleSortingSource> BuildSampleSortingSource(
    const SourceCollectionManifest* context,
    const std::vector<SampleLabelingTask>* active_source_tasks,
    std::size_t sample_count,
    std::string_view source_id)
{
    return BuildSampleSortingSource(context, active_source_tasks, sample_count, source_id, {});
}

std::optional<SampleSortingSource> BuildSampleSortingSource(
    const SourceCollectionManifest* context,
    const std::vector<SampleLabelingTask>* active_source_tasks,
    std::size_t sample_count,
    std::string_view source_id,
    const std::function<void()>& cancellation_checkpoint)
{
    if (source_id == kSourceOrderSortSourceId) {
        return BuildSourceOrderSortingSource(sample_count, cancellation_checkpoint);
    }
    if (context == nullptr || sample_count == 0) {
        return std::nullopt;
    }

    if (source_id == "sample-name") {
        return BuildSampleNameSortingSource(*context, sample_count, cancellation_checkpoint);
    }
    for (const SampleAnnotationResult& annotation : context->annotations) {
        if (BuildAnnotationFilterSourceId(annotation) == source_id) {
            return BuildAnnotationSortingSource(
                active_source_tasks,
                annotation,
                sample_count,
                cancellation_checkpoint);
        }
    }
    return std::nullopt;
}

}  // namespace spectiary
