#pragma once

#include "domain/sample_annotation_io.h"
#include "domain/sample_labeling.h"
#include "domain/source_collection_manifest.h"
#include "ui/sample_navigation_sequence.h"
#include "ui/source_collection_session_types.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace spectiary {

struct SampleSortingSource {
    std::string id;
    std::string name;
    std::vector<SampleNavigationSortValue> values;
};

[[nodiscard]] std::vector<SourceCollectionSampleSortSourceView> BuildSampleSortingSourceViews(
    const SourceCollectionManifest* context,
    const std::vector<SampleLabelingTask>* active_source_tasks,
    std::size_t sample_count);
[[nodiscard]] std::vector<SourceCollectionSampleSortSourceView> BuildSampleSortingSourceViews(
    const SourceCollectionManifest* context,
    const std::vector<SampleLabelingTask>* active_source_tasks,
    std::size_t sample_count,
    const std::function<void()>& cancellation_checkpoint);
[[nodiscard]] std::optional<SampleSortingSource> BuildSampleSortingSource(
    const SourceCollectionManifest* context,
    const std::vector<SampleLabelingTask>* active_source_tasks,
    std::size_t sample_count,
    std::string_view source_id);
[[nodiscard]] std::optional<SampleSortingSource> BuildSampleSortingSource(
    const SourceCollectionManifest* context,
    const std::vector<SampleLabelingTask>* active_source_tasks,
    std::size_t sample_count,
    std::string_view source_id,
    const std::function<void()>& cancellation_checkpoint);

}  // namespace spectiary
