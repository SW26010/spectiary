#include "ui/sample_sorting_sources.h"

#include "domain/sample_filter.h"
#include "ui/sample_annotation_labeling_rules.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string_view>
#include <utility>

namespace specforge {
namespace {

constexpr std::string_view kSourceOrderSortSourceId = "source-order";

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

std::optional<double> ParseFiniteDouble(std::string_view text)
{
    std::string trimmed = TrimAscii(std::string{text});
    if (trimmed.empty()) {
        return std::nullopt;
    }

    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(trimmed.c_str(), &end);
    if (end == trimmed.c_str() || *end != '\0' || errno == ERANGE || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

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
    std::size_t sample_count)
{
    if (context.sample_names.size() != sample_count) {
        return std::nullopt;
    }

    SampleSortingSource source;
    source.id = "sample-name";
    source.name = "Sample name";
    source.values.reserve(context.sample_names.size());
    for (const std::string& sample_name : context.sample_names) {
        source.values.push_back(MakeSampleNavigationSortValue(sample_name));
    }
    return source;
}

std::optional<SampleSortingSource> BuildSourceOrderSortingSource(std::size_t sample_count)
{
    if (sample_count == 0) {
        return std::nullopt;
    }

    SampleSortingSource source;
    source.id = std::string{kSourceOrderSortSourceId};
    source.name = "Source order";
    source.values.reserve(sample_count);
    for (std::size_t row = 0; row < sample_count; ++row) {
        source.values.push_back(MakeSampleNavigationSortValue(static_cast<double>(row)));
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
    std::size_t sample_count)
{
    if (!IsAnnotationSortingSourceCandidate(active_source_tasks, annotation, sample_count)) {
        return std::nullopt;
    }

    SampleSortingSource source;
    source.id = BuildAnnotationFilterSourceId(annotation);
    source.name = annotation.name;
    source.values.reserve(annotation.values.size());
    for (const SampleAnnotationValue& value : annotation.values) {
        switch (annotation.kind) {
        case SampleAnnotationKind::CategoricalInteger:
            if (!value.integer_value) {
                return std::nullopt;
            }
            source.values.push_back(
                MakeSampleNavigationSortValue(static_cast<double>(*value.integer_value)));
            break;
        case SampleAnnotationKind::ContinuousFloat:
            if (const std::optional<double> parsed = ParseFiniteDouble(value.display_text)) {
                source.values.push_back(MakeSampleNavigationSortValue(*parsed));
            } else {
                return std::nullopt;
            }
            break;
        case SampleAnnotationKind::Text:
            source.values.push_back(MakeSampleNavigationSortValue(value.display_text));
            break;
        }
    }
    return source;
}

}  // namespace

std::vector<SourceCollectionSampleSortSourceView> BuildSampleSortingSourceViews(
    const SourceCollectionManifest* context,
    const std::vector<SampleLabelingTask>* active_source_tasks,
    std::size_t sample_count)
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
    for (const SampleAnnotationResult& annotation : context->annotations) {
        if (std::optional<SampleSortingSource> annotation_source =
                BuildAnnotationSortingSource(active_source_tasks, annotation, sample_count)) {
            sources.push_back(MakeSampleSortSourceView(
                std::move(annotation_source->id),
                std::move(annotation_source->name),
                annotation.path));
        }
    }
    return sources;
}

std::optional<SampleSortingSource> BuildSampleSortingSource(
    const SourceCollectionManifest* context,
    const std::vector<SampleLabelingTask>* active_source_tasks,
    std::size_t sample_count,
    std::string_view source_id)
{
    if (source_id == kSourceOrderSortSourceId) {
        return BuildSourceOrderSortingSource(sample_count);
    }
    if (context == nullptr || sample_count == 0) {
        return std::nullopt;
    }

    if (source_id == "sample-name") {
        return BuildSampleNameSortingSource(*context, sample_count);
    }
    for (const SampleAnnotationResult& annotation : context->annotations) {
        if (BuildAnnotationFilterSourceId(annotation) == source_id) {
            return BuildAnnotationSortingSource(active_source_tasks, annotation, sample_count);
        }
    }
    return std::nullopt;
}

}  // namespace specforge
