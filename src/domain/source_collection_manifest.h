#pragma once

#include "domain/sample_annotation_io.h"
#include "domain/spectrum_snapshot.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace specforge {

struct SourceCollectionIdentity {
    std::string id;
    std::string source_name;
    std::string source_fingerprint;
    std::string context_fingerprint;
    std::size_t spectrum_count = 0;
};

struct SourceCollectionFolderSpectrumFile {
    std::filesystem::path path;
    std::string format;
    std::string stat_fingerprint;
};

struct SourceCollectionFolderListing {
    bool readable = true;
    std::string error_message;
    std::vector<SourceCollectionFolderSpectrumFile> spectra;
    std::size_t csv_count = 0;
    std::size_t fits_count = 0;
    std::size_t ignored_file_count = 0;
    std::size_t ignored_directory_count = 0;
    std::vector<std::string> ignored_file_examples;
    std::vector<std::string> ignored_directory_examples;
};

struct SourceCollectionFileDependencyState {
    std::string path_key;
    std::string stat_fingerprint;

    [[nodiscard]] bool operator==(const SourceCollectionFileDependencyState&) const = default;
};

// A point-in-time filesystem observation used to keep a decoded single-file
// snapshot and every identity/annotation dependency in the same generation.
struct SourceCollectionSingleFileState {
    std::string source_stat_fingerprint;
    std::string companion_name_fingerprint;
    std::string companion_annotation_fingerprint;
    std::vector<SourceCollectionFileDependencyState> annotation_dependencies;

    [[nodiscard]] bool operator==(const SourceCollectionSingleFileState&) const = default;
};

// Proof captured only after a decoded snapshot and all source/context
// dependencies passed the post-decode consistency check. A later load may use
// it to prove that the already-owned context remains valid without rebuilding
// its manifest.
struct SourceCollectionContextReuseProof {
    SourceCollectionIdentity identity;
    SourceCollectionSingleFileState dependency_state;
};

struct SourceCollectionManifest {
    std::vector<std::string> sample_names;
    std::vector<SampleAnnotationResult> annotations;
    std::vector<std::string> messages;
};

struct SourceCollectionContext {
    SourceCollectionIdentity identity;
    SourceCollectionManifest manifest;
};

using SourceCollectionFolderScanProgress = std::function<void(std::size_t processed_entry_count)>;
using SourceCollectionCancellationCheckpoint = std::function<void()>;

[[nodiscard]] SourceCollectionIdentity BuildSourceCollectionIdentity(const SpectrumSnapshot& snapshot);
[[nodiscard]] SourceCollectionIdentity BuildSourceCollectionIdentity(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionSingleFileState& file_state);
[[nodiscard]] SourceCollectionManifest LoadSourceCollectionManifest(const SpectrumSnapshot& snapshot);
[[nodiscard]] SourceCollectionManifest LoadSourceCollectionManifestCancelable(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint);
[[nodiscard]] bool IngestReadOnlySampleAnnotation(
    SourceCollectionManifest& manifest,
    const std::filesystem::path& path,
    std::size_t expected_count,
    std::string* message = nullptr);
[[nodiscard]] bool IngestReadOnlySampleAnnotationCancelable(
    SourceCollectionManifest& manifest,
    const std::filesystem::path& path,
    std::size_t expected_count,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint,
    std::string* message = nullptr);
[[nodiscard]] bool SourceCollectionManifestContainsAnnotation(
    const SourceCollectionManifest& manifest,
    const std::filesystem::path& path);
[[nodiscard]] SourceCollectionContext LoadSourceCollectionContext(const SpectrumSnapshot& snapshot);
[[nodiscard]] SourceCollectionContext LoadSourceCollectionContextCancelable(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint);
[[nodiscard]] SourceCollectionContext LoadSourceCollectionContextCancelable(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionSingleFileState& file_state,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint);
[[nodiscard]] SourceCollectionSingleFileState CaptureSourceCollectionSingleFileState(
    const std::filesystem::path& source_path,
    const std::vector<std::filesystem::path>& annotation_paths = {},
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint = {});
[[nodiscard]] bool SourceCollectionSingleFileStatesMatch(
    const SourceCollectionSingleFileState& left,
    const SourceCollectionSingleFileState& right);
void FinalizeSourceCollectionAnnotationContextFingerprint(
    SourceCollectionContext& context,
    const std::vector<std::filesystem::path>& requested_annotation_paths,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint = {});
[[nodiscard]] SourceCollectionFolderListing ScanSourceCollectionFolder(const std::filesystem::path& path);
[[nodiscard]] SourceCollectionFolderListing ScanSourceCollectionFolder(
    const std::filesystem::path& path,
    const SourceCollectionFolderScanProgress& progress);
[[nodiscard]] SourceCollectionFolderListing ScanSourceCollectionFolder(
    const std::filesystem::path& path,
    const SourceCollectionFolderScanProgress& progress,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint);
[[nodiscard]] bool SourceCollectionFolderListingsMatch(
    const SourceCollectionFolderListing& left,
    const SourceCollectionFolderListing& right,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint = {});
[[nodiscard]] bool SourceCollectionFolderSpectrumFileMatchesCurrentState(
    const SourceCollectionFolderSpectrumFile& file);
[[nodiscard]] SourceCollectionContext BuildFolderSourceCollectionContext(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionFolderListing& listing);
[[nodiscard]] SourceCollectionContext BuildFolderSourceCollectionContextCancelable(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionFolderListing& listing,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint);
[[nodiscard]] bool IsSourceCollectionAuxiliaryNpyArrayName(const std::filesystem::path& source_path);
[[nodiscard]] std::optional<std::filesystem::path> SourceCollectionCompanionNamePath(
    const std::filesystem::path& source_path);
[[nodiscard]] std::optional<std::filesystem::path> SourceCollectionCompanionAnnotationPath(
    const std::filesystem::path& source_path);
[[nodiscard]] std::optional<std::string> LoadSourceCollectionNpySampleName(
    const std::filesystem::path& source_path,
    std::size_t expected_count,
    std::size_t sample_index);

}  // namespace specforge
