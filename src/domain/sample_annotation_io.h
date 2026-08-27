#pragma once

#include "domain/sample_labeling.h"
#include "domain/sample_labeling_document.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace specforge {

using SampleAnnotationCancellationCheckpoint = std::function<void()>;

enum class SampleLabelingOutputArtifactFormat {
    CanonicalAsdf,
    LegacyNpyWithSidecar,
};

struct SampleAnnotationArtifactIdentitySet {
    // Complete artifact set owned by one labeling output. Canonical ASDF owns
    // only its document path; legacy NPY owns both the result path and its
    // adjacent <stem>.sf-labels.json. Read-only annotation dependency capture
    // is separate from this output lease set.
    // Non-probing identities keep an offline historical artifact addressable.
    std::vector<std::string> stable_path_keys;
    // Physical identities collapse junction, drive-mapping, and UNC aliases
    // when the target can be probed; they are an optional enhancement over
    // stable_path_keys, not a prerequisite for editing.
    std::vector<std::string> physical_path_keys;
    bool all_paths_physically_resolved = false;
};

[[nodiscard]] SampleAnnotationArtifactIdentitySet
SampleAnnotationArtifactIdentities(
    const std::filesystem::path& result_path,
    SampleLabelingOutputArtifactFormat format,
    bool resolve_physical_paths = true);

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
    using SemanticValue = std::variant<std::int64_t, std::uint64_t, double, std::string>;

    SemanticValue semantic;
};

// Synchronous view of the canonical base source identity and roster used to
// decide whether an attached labeling document belongs to the active source.
// context_fingerprint is intentionally absent because annotations contribute
// to it and therefore cannot identify the annotation-independent base source.
struct SampleAnnotationSourceCompatibility {
    std::string_view base_identity;
    std::string_view source_name;
    std::string_view source_fingerprint;
    std::size_t sample_count = 0;
    std::span<const std::string> sample_names;
};

struct SampleAnnotationResult {
    std::string name;
    std::filesystem::path path;
    SampleAnnotationKind kind = SampleAnnotationKind::Text;
    std::string dtype;
    std::string dtype_name;
    SampleAnnotationWorkflowRelationship relationship = SampleAnnotationWorkflowRelationship::PlainAnnotation;
    // Legacy NPY companion metadata only. Canonical ASDF semantics live in
    // labeling_document and are not recast as sidecar-era fields.
    std::optional<SampleLabelResultMetadata> label_metadata;
    // Present for self-contained canonical labeling documents. The immutable
    // shared value keeps task, source, roster, and annotation semantics intact
    // while existing annotation views consume their projected values/metadata.
    std::shared_ptr<const SampleLabelingDocument> labeling_document;
    std::string metadata_warning;
    std::vector<SampleAnnotationValue> values;
};

struct LoadedSampleLabelResult {
    std::vector<int> values;
    bool metadata_sidecar_exists = false;
    std::optional<SampleLabelResultMetadata> metadata;
    std::string metadata_warning;
};

struct SampleLabelResultWriteOutcome {
    bool array_saved = false;
    bool metadata_saved = false;
    std::string message;
};

class SampleAnnotationIoAdapter {
public:
    [[nodiscard]] std::optional<SampleAnnotationResult> Load(
        const std::filesystem::path& path,
        std::size_t expected_count,
        std::string* error_message = nullptr) const;
    [[nodiscard]] std::optional<SampleAnnotationResult> LoadCancelable(
        const std::filesystem::path& path,
        std::size_t expected_count,
        const SampleAnnotationCancellationCheckpoint& cancellation_checkpoint,
        std::string* error_message = nullptr) const;
    [[nodiscard]] std::optional<SampleAnnotationResult> LoadForSource(
        const std::filesystem::path& path,
        const SampleAnnotationSourceCompatibility& source,
        std::string* error_message = nullptr) const;
    [[nodiscard]] std::optional<SampleAnnotationResult> LoadForSourceCancelable(
        const std::filesystem::path& path,
        const SampleAnnotationSourceCompatibility& source,
        const SampleAnnotationCancellationCheckpoint& cancellation_checkpoint,
        std::string* error_message = nullptr) const;
    [[nodiscard]] std::optional<LoadedSampleLabelResult> LoadLabelResult(
        const std::filesystem::path& path,
        std::size_t expected_count,
        const SampleAnnotationCancellationCheckpoint& cancellation_checkpoint = {},
        std::string* error_message = nullptr) const;
    [[nodiscard]] bool SaveLabelArray(
        const std::filesystem::path& path,
        const SampleLabelingTask& task,
        std::string* error_message = nullptr) const;
    [[nodiscard]] bool SaveLabelMetadata(
        const std::filesystem::path& result_path,
        const SampleLabelingTask& task,
        const SampleLabelResultMetadataSource* source = nullptr,
        std::string* error_message = nullptr) const;
    [[nodiscard]] SampleLabelResultWriteOutcome SaveLabelResult(
        const std::filesystem::path& path,
        const SampleLabelingTask& task,
        const SampleLabelResultMetadataSource* source = nullptr) const;
    [[nodiscard]] SampleLabelResultMetadataLoadResult LoadLabelMetadata(
        const std::filesystem::path& result_path,
        std::size_t expected_count,
        std::string_view expected_dtype,
        const SampleAnnotationCancellationCheckpoint& cancellation_checkpoint = {}) const;

    [[nodiscard]] static std::filesystem::path MetadataPathForResult(
        const std::filesystem::path& result_path);
};

[[nodiscard]] std::string FormatSampleAnnotationValue(
    const SampleAnnotationResult& annotation,
    const SampleAnnotationValue& value);
[[nodiscard]] std::string SampleAnnotationValueKey(const SampleAnnotationValue& value);
[[nodiscard]] std::optional<int> SampleAnnotationValueAsInt(const SampleAnnotationValue& value);
[[nodiscard]] std::string_view SampleAnnotationKindLabel(SampleAnnotationKind kind);

}  // namespace specforge
