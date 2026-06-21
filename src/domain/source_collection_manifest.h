#pragma once

#include "domain/sample_annotation_io.h"
#include "domain/spectrum_snapshot.h"

#include <cstddef>
#include <filesystem>
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

struct SourceCollectionManifest {
    std::vector<std::string> sample_names;
    std::vector<SampleAnnotationResult> annotations;
    std::vector<std::string> messages;
};

[[nodiscard]] SourceCollectionIdentity BuildSourceCollectionIdentity(const SpectrumSnapshot& snapshot);
[[nodiscard]] SourceCollectionManifest LoadSourceCollectionManifest(const SpectrumSnapshot& snapshot);
[[nodiscard]] SourceCollectionFolderListing ScanSourceCollectionFolder(const std::filesystem::path& path);
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
