#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace specforge {

struct SampleLabelingTask;
struct SourceCollectionContext;

inline constexpr std::string_view kSampleLabelingDocumentFormatKind =
    "specforge.sample_labeling";
inline constexpr std::string_view kSampleLabelingDocumentSchemaVersion = "1.0.0";
inline constexpr std::string_view kSampleLabelingDocumentCategoricalIntegerKind =
    "categorical_integer";
inline constexpr std::string_view kSampleLabelingDocumentUnlabeledSemantic = "unlabeled";
inline constexpr std::string_view kSampleLabelingDocumentExplicitNamesRoster =
    "explicit_names";
inline constexpr std::string_view kSampleLabelingDocumentSourceIndexRoster =
    "source_index";
inline constexpr std::int32_t kSampleLabelingDocumentUnlabeledValue = -1;

struct SampleLabelingDocumentRoster {
    std::string identity_kind =
        std::string{kSampleLabelingDocumentSourceIndexRoster};
    std::vector<std::string> sample_names;
};

// This is deliberately the identity of the base source collection.  The
// annotation-sensitive SourceCollectionIdentity::context_fingerprint is not a
// canonical document field.
struct SampleLabelingDocumentSource {
    std::string base_identity;
    std::string kind;
    std::string name;
    std::string fingerprint;
    std::size_t sample_count = 0;
    SampleLabelingDocumentRoster roster;
};

struct SampleLabelingDocumentMissingValue {
    std::string semantic =
        std::string{kSampleLabelingDocumentUnlabeledSemantic};
    std::int32_t value = kSampleLabelingDocumentUnlabeledValue;
};

struct SampleLabelingDocumentAnnotation {
    std::string kind =
        std::string{kSampleLabelingDocumentCategoricalIntegerKind};
    std::string name;
    SampleLabelingDocumentMissingValue missing;
    // The element type and vector shape are the canonical int32/[sample_count]
    // value contract.  A codec may represent this as a typed ndarray without
    // adding sidecar-era expected_dtype or value_count fields to the model.
    std::vector<std::int32_t> values;
};

struct SampleLabelingDocumentLabel {
    std::int32_t code = 0;
    std::string name;
    std::string shortcut;
};

struct SampleLabelingDocumentTask {
    std::string id;
    std::string name;
    std::vector<SampleLabelingDocumentLabel> labels;
};

// Canonical, user-owned labeling data only.  Session, navigation, output-path,
// pending-write, save-state, and recovery fields remain owned by
// SampleLabelingTask and its controller/cache lifecycle.
struct SampleLabelingDocument {
    std::string format_kind =
        std::string{kSampleLabelingDocumentFormatKind};
    std::string schema_version =
        std::string{kSampleLabelingDocumentSchemaVersion};
    SampleLabelingDocumentSource source;
    SampleLabelingDocumentAnnotation annotation;
    SampleLabelingDocumentTask labeling;
};

enum class SampleLabelingDocumentValidationIssueKind {
    UnsupportedFormatKind,
    UnsupportedSchemaVersion,
    MissingSourceBaseIdentity,
    MissingSourceKind,
    MissingSourceName,
    MissingSourceFingerprint,
    UnsupportedRosterIdentityKind,
    SourceIndexRosterHasNames,
    RosterSampleCountMismatch,
    EmptySampleName,
    DuplicateSampleName,
    UnsupportedAnnotationKind,
    MissingAnnotationName,
    UnsupportedMissingSemantic,
    InvalidUnlabeledValue,
    AnnotationSampleCountMismatch,
    MissingTaskId,
    MissingTaskName,
    ReservedLabelCode,
    DuplicateLabelCode,
    MissingLabelName,
    InvalidLabelShortcut,
    DuplicateLabelShortcut,
    UndefinedAnnotationValue,
};

inline constexpr std::size_t kSampleLabelingDocumentNoIssueIndex =
    static_cast<std::size_t>(-1);

struct SampleLabelingDocumentValidationIssue {
    SampleLabelingDocumentValidationIssueKind kind =
        SampleLabelingDocumentValidationIssueKind::UnsupportedFormatKind;
    std::size_t index = kSampleLabelingDocumentNoIssueIndex;

    [[nodiscard]] bool operator==(
        const SampleLabelingDocumentValidationIssue&) const = default;
};

struct SampleLabelingDocumentValidationResult {
    std::vector<SampleLabelingDocumentValidationIssue> issues;

    [[nodiscard]] bool valid() const noexcept
    {
        return issues.empty();
    }
};

[[nodiscard]] SampleLabelingDocument BuildSampleLabelingDocument(
    std::string source_kind,
    const SourceCollectionContext& source_context,
    const SampleLabelingTask& task);

[[nodiscard]] SampleLabelingDocumentValidationResult
ValidateSampleLabelingDocument(const SampleLabelingDocument& document);

}  // namespace specforge
