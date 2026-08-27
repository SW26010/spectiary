#pragma once

#include "domain/sample_labeling_document.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace specforge {

// Synchronous view of the canonical base source identity and roster used to
// decide whether a labeling document belongs to the active source.
// context_fingerprint is intentionally absent because annotations contribute
// to it and therefore cannot identify the annotation-independent base source.
struct SampleLabelingSourceCompatibility {
    std::string_view base_identity;
    std::string_view source_name;
    std::string_view source_fingerprint;
    std::size_t sample_count = 0;
    std::span<const std::string> sample_names;
};

enum class SampleLabelingSourceCompatibilityErrorKind {
    None,
    SampleCountMismatch,
    SourceIdentityMismatch,
    RosterMismatch,
};

struct SampleLabelingSourceCompatibilityError {
    SampleLabelingSourceCompatibilityErrorKind kind =
        SampleLabelingSourceCompatibilityErrorKind::None;
    std::string message;
};

using SampleLabelingSourceCompatibilityCheckpoint =
    std::function<void()>;

[[nodiscard]] std::optional<SampleLabelingSourceCompatibilityError>
CheckSampleLabelingSourceCompatibility(
    const SampleLabelingDocument& document,
    const SampleLabelingSourceCompatibility& source,
    const SampleLabelingSourceCompatibilityCheckpoint& checkpoint = {});

}  // namespace specforge
