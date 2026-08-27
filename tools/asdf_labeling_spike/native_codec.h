#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace specforge::asdf_labeling_spike {

inline constexpr std::int32_t kUnlabeled = -1;

struct LabelDefinition {
    std::int32_t code = 0;
    std::string name;
    std::string shortcut;
};

struct LabelingDocument {
    std::string format_kind;
    std::string schema_version;
    std::string source_kind;
    std::string source_name;
    std::string source_identity;
    std::string source_fingerprint;
    std::uint64_t sample_count = 0;
    std::string roster_identity_kind;
    std::vector<std::string> sample_names;
    std::string annotation_kind;
    std::string annotation_name;
    std::string missing_semantic;
    std::int32_t missing_value = kUnlabeled;
    std::string task_id;
    std::string task_name;
    std::vector<LabelDefinition> labels;
    std::vector<std::int32_t> values;
};

[[nodiscard]] LabelingDocument ReadLabelingDocument(const std::filesystem::path& path);
void WriteLabelingDocument(const std::filesystem::path& path, const LabelingDocument& document);
void RewriteLabelValuePreservingRosterBlock(
    const std::filesystem::path& input_path,
    const std::filesystem::path& output_path,
    std::size_t value_index,
    std::int32_t value);
[[nodiscard]] std::string SemanticJson(const LabelingDocument& document);
[[nodiscard]] LabelingDocument NativeFixture();

}  // namespace specforge::asdf_labeling_spike
