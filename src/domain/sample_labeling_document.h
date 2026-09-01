#pragma once

#include "domain/sample_labeling.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace specforge {

struct SampleLabelingCanonicalSourceDescriptor;

struct SourceCollectionContext;

inline constexpr std::string_view kSampleLabelingDocumentFormatKind =
    "specforge.sample_labeling";
inline constexpr std::string_view kSampleLabelingDocumentSchemaVersion = "2.0.0";
inline constexpr std::string_view kSampleLabelingDocumentCategoricalIntegerKind =
    "categorical_integer";
inline constexpr std::string_view kSampleLabelingDocumentByIndexAlignmentMode =
    "by_index";
inline constexpr std::string_view kSampleLabelingDocumentSampleRosterAlignmentTarget =
    "sample_roster";
inline constexpr std::string_view kSampleLabelingDocumentUnlabeledSemantic = "unlabeled";
inline constexpr std::string_view kSampleLabelingDocumentExplicitNamesRoster =
    "explicit_names";
inline constexpr std::string_view kSampleLabelingDocumentSourceIndexRoster =
    "source_index";
inline constexpr std::string_view kSampleLabelingDocumentWorkingTreeBuildSource =
    "working_tree";
inline constexpr std::string_view kSampleLabelingDocumentHeadBuildSource =
    "head";
inline constexpr std::int32_t kSampleLabelingDocumentUnlabeledValue = -1;

struct SampleLabelingDocumentBuildSource {
    std::string source_mode;
    std::optional<std::string> source_revision;
};

[[nodiscard]] SampleLabelingDocumentBuildSource
CurrentSampleLabelingDocumentBuildSource();

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

// Schema 2.0 has exactly one alignment contract. This self-description is not
// an extensible runtime alignment strategy.
struct SampleLabelingDocumentAlignment {
    std::string mode =
        std::string{kSampleLabelingDocumentByIndexAlignmentMode};
    std::string target =
        std::string{kSampleLabelingDocumentSampleRosterAlignmentTarget};
};

struct SampleLabelingDocumentAnnotation {
    std::string kind =
        std::string{kSampleLabelingDocumentCategoricalIntegerKind};
    SampleLabelingDocumentAlignment alignment;
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
    SampleLabelingTaskCanonicalMetadata canonical_metadata;
    std::vector<SampleLabelingDocumentLabel> labels;
};

// Canonical labeling data plus producer-generation provenance. Session,
// navigation, output-path, pending-write, save-state, and recovery fields
// remain owned by SampleLabelingTask and its controller/cache lifecycle.
struct SampleLabelingDocument {
    std::string format_kind =
        std::string{kSampleLabelingDocumentFormatKind};
    std::string schema_version =
        std::string{kSampleLabelingDocumentSchemaVersion};
    SampleLabelingDocumentBuildSource build_source =
        CurrentSampleLabelingDocumentBuildSource();
    SampleLabelingDocumentSource source;
    SampleLabelingDocumentAnnotation annotation;
    SampleLabelingDocumentTask labeling;
};

enum class SampleLabelingDocumentValidationIssueKind {
    UnsupportedFormatKind,
    UnsupportedSchemaVersion,
    UnsupportedBuildSourceMode,
    MissingBuildSourceRevision,
    UnexpectedBuildSourceRevision,
    InvalidBuildSourceRevision,
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
    UnsupportedAnnotationAlignmentMode,
    UnsupportedAnnotationAlignmentTarget,
    UnsupportedMissingSemantic,
    InvalidUnlabeledValue,
    AnnotationSampleCountMismatch,
    MissingTaskId,
    InvalidTaskId,
    MissingTaskName,
    InvalidCanonicalTimestampOrder,
    InvalidOriginKind,
    InvalidOriginAnnotation,
    InvalidDescription,
    InvalidAuthor,
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

// Builds a canonical owner directly from the controller's durable source
// descriptor. This is the creation path used after source preparation has
// released its SourceCollectionContext.
[[nodiscard]] SampleLabelingDocument BuildSampleLabelingDocument(
    const SampleLabelingCanonicalSourceDescriptor& source,
    const SampleLabelingTask& task);

// Builds the editable/runtime projection of one canonical document generation.
// Canonical task metadata and values form the base; only local session state
// and explicitly pending cache overlays are applied above it.
[[nodiscard]] std::optional<SampleLabelingTask>
ProjectSampleLabelingDocumentTask(
    const SampleLabelingDocument& document,
    const SampleLabelingTask& local_state,
    const std::function<void()>& cancellation_checkpoint = {});

[[nodiscard]] SampleLabelingDocumentValidationResult
ValidateSampleLabelingDocument(const SampleLabelingDocument& document);

// Production codec paths use the same semantic rules without retaining an
// attacker-controlled number of diagnostics.
[[nodiscard]] SampleLabelingDocumentValidationResult
ValidateSampleLabelingDocumentFailFast(
    const SampleLabelingDocument& document);

}  // namespace specforge
