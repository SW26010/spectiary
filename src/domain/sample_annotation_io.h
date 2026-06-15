#pragma once

#include "domain/spectrum_snapshot.h"

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

struct SampleCollectionContext {
    std::vector<std::string> sample_names;
    std::vector<SampleAnnotationResult> annotations;
    std::vector<std::string> messages;
};

struct SampleCollectionIdentity {
    std::string id;
    std::string source_name;
    std::string source_fingerprint;
    std::string context_fingerprint;
    std::size_t spectrum_count = 0;
};

[[nodiscard]] SampleCollectionContext LoadSampleCollectionContext(const SpectrumSnapshot& snapshot);
[[nodiscard]] SampleCollectionIdentity BuildSampleCollectionIdentity(const SpectrumSnapshot& snapshot);
[[nodiscard]] bool IsSampleCollectionAuxiliaryNpyArrayName(const std::filesystem::path& source_path);
[[nodiscard]] std::optional<std::filesystem::path> SampleCollectionCompanionNamePath(
    const std::filesystem::path& source_path);
[[nodiscard]] std::string_view SampleAnnotationKindLabel(SampleAnnotationKind kind);

}  // namespace specforge
