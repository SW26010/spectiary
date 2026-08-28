#pragma once

#include "domain/sample_labeling_document.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace specforge {

struct SourceCollectionContext;
struct SourceCollectionIdentity;
struct SourceCollectionManifest;
struct SpectrumSnapshot;

// Controller-owned canonical source facts. Unlike the borrowed compatibility
// view below, this descriptor may outlive source preparation and is also the
// source contract for creating future canonical labeling documents.
struct SampleLabelingCanonicalSourceDescriptor {
    std::string base_identity;
    std::string source_kind;
    std::string source_name;
    std::string source_fingerprint;
    std::size_t sample_count = 0;
    std::vector<std::string> sample_names;

    [[nodiscard]] bool operator==(
        const SampleLabelingCanonicalSourceDescriptor&) const =
        default;
};

// Synchronous view of the canonical base source identity and roster used to
// decide whether a labeling document belongs to the active source.
// context_fingerprint is intentionally absent because annotations contribute
// to it and therefore cannot identify the annotation-independent base source.
struct SampleLabelingSourceCompatibility {
    std::string_view base_identity;
    // Writable owner hydration always supplies this. Legacy read-only
    // annotation attachment may leave it empty until its source session owns
    // the canonical descriptor.
    std::string_view source_kind;
    std::string_view source_name;
    std::string_view source_fingerprint;
    std::size_t sample_count = 0;
    std::span<const std::string> sample_names;
};

[[nodiscard]] SampleLabelingCanonicalSourceDescriptor
BuildSampleLabelingCanonicalSourceDescriptor(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionContext& source_context);

[[nodiscard]] SampleLabelingCanonicalSourceDescriptor
BuildSampleLabelingCanonicalSourceDescriptor(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionIdentity& identity,
    const SourceCollectionManifest& manifest);

[[nodiscard]] SampleLabelingSourceCompatibility
SampleLabelingCompatibilityView(
    const SampleLabelingCanonicalSourceDescriptor& source) noexcept;

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
