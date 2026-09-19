#pragma once

#include "domain/sample_labeling_asdf_store.h"

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace spectiary::asdf_labeling_benchmark {

enum class DatasetCase {
    SourceIndex,
    ExplicitUnicode,
};

inline constexpr std::size_t kProductionSampleCount = 1'000'000;
inline constexpr std::size_t kExplicitUnicodeRosterWidth = 20;

struct BenchmarkDocument {
    SampleLabelingDocument document;
    std::size_t roster_width = 0;
};

[[nodiscard]] DatasetCase ParseDatasetCase(std::string_view value);
[[nodiscard]] std::string_view DatasetCaseName(DatasetCase dataset_case);

[[nodiscard]] BenchmarkDocument MakeBenchmarkDocument(
    DatasetCase dataset_case,
    std::size_t sample_count);

[[nodiscard]] SampleLabelingSourceCompatibility MakeCompatibilityView(
    const SampleLabelingDocument& document) noexcept;

void SeedForwardUnknownMetadata(const std::filesystem::path& path);

[[nodiscard]] bool ContainsSeededForwardUnknownMetadata(
    const std::filesystem::path& path);

}  // namespace spectiary::asdf_labeling_benchmark
