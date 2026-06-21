#pragma once

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

struct SampleAnnotationValue {
    std::string display_text;
};

struct SampleAnnotationResult {
    std::string name;
    std::filesystem::path path;
    SampleAnnotationKind kind = SampleAnnotationKind::Text;
    std::string dtype;
    std::vector<SampleAnnotationValue> values;
};

[[nodiscard]] std::optional<SampleAnnotationResult> LoadSampleAnnotationResultFromPath(
    const std::filesystem::path& path,
    std::size_t expected_count,
    std::string* error_message = nullptr);
[[nodiscard]] std::string_view SampleAnnotationKindLabel(SampleAnnotationKind kind);

}  // namespace specforge
