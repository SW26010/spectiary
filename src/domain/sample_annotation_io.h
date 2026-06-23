#pragma once

#include "domain/sample_labeling.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace specforge {

enum class SampleAnnotationKind {
    CategoricalInteger,
    Text,
    ContinuousFloat,
};

enum class SampleAnnotationWorkflowRelationship {
    PlainAnnotation,
    ExternalLabelResult,
    LocalLabelingTask,
};

struct SampleAnnotationValue {
    std::string display_text;
    std::optional<int> integer_value;
};

struct SampleAnnotationResult {
    std::string name;
    std::filesystem::path path;
    SampleAnnotationKind kind = SampleAnnotationKind::Text;
    std::string dtype;
    std::string dtype_name;
    SampleAnnotationWorkflowRelationship relationship = SampleAnnotationWorkflowRelationship::PlainAnnotation;
    std::optional<SampleLabelResultMetadata> label_metadata;
    std::string metadata_warning;
    std::vector<SampleAnnotationValue> values;
};

[[nodiscard]] std::optional<SampleAnnotationResult> LoadSampleAnnotationResultFromPath(
    const std::filesystem::path& path,
    std::size_t expected_count,
    std::string* error_message = nullptr);
[[nodiscard]] std::string_view SampleAnnotationKindLabel(SampleAnnotationKind kind);
[[nodiscard]] std::string_view SampleAnnotationWorkflowRelationshipLabel(
    SampleAnnotationWorkflowRelationship relationship);

}  // namespace specforge
