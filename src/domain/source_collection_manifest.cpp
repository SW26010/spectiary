#include "domain/source_collection_manifest.h"

#include "domain/npy_array_io.h"
#include "domain/source_path_identity.h"
#include "domain/stable_sha256.h"
#include "domain/spectrum_loader_support.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace spectiary {
namespace {

void Checkpoint(const SourceCollectionCancellationCheckpoint& cancellation_checkpoint)
{
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
}

class SourceCollectionManifestError : public std::runtime_error {
public:
    explicit SourceCollectionManifestError(std::string message)
        : std::runtime_error(std::move(message))
    {
    }
};

struct NpyStringData {
    NpyScalarType scalar_type;
    std::uint64_t data_offset = 0;
};

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::string FileNameToUtf8(const std::filesystem::path& path)
{
    const std::filesystem::path filename = path.filename();
    return filename.empty() ? PathToUtf8(path) : PathToUtf8(filename);
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string ExtensionLower(const std::filesystem::path& path)
{
    return LowerAscii(PathToUtf8(path.extension()));
}

std::optional<std::filesystem::path> CompanionNpyPath(
    const std::filesystem::path& path,
    std::string_view underscore_replacement,
    std::string_view dash_replacement,
    std::string_view bare_replacement)
{
    const std::string filename = FileNameToUtf8(path.filename());
    const std::string lower_filename = LowerAscii(filename);

    struct SuffixRule {
        std::string_view suffix;
        std::string_view replacement;
    };
    const std::array<SuffixRule, 4> rules = {
        SuffixRule{"_x.npy", underscore_replacement},
        SuffixRule{"-x.npy", dash_replacement},
        SuffixRule{"_flux.npy", underscore_replacement},
        SuffixRule{"-flux.npy", dash_replacement},
    };

    for (const SuffixRule& rule : rules) {
        if (lower_filename.ends_with(rule.suffix)) {
            std::string candidate = filename.substr(0, filename.size() - rule.suffix.size());
            candidate += rule.replacement;
            return path.parent_path() / candidate;
        }
    }

    if (lower_filename == "x.npy" || lower_filename == "flux.npy") {
        return path.parent_path() / std::filesystem::path(std::string(bare_replacement));
    }
    if (ExtensionLower(path) == ".npy") {
        std::string candidate = FileNameToUtf8(path.stem());
        candidate += underscore_replacement;
        return path.parent_path() / candidate;
    }
    return std::nullopt;
}

bool PathExists(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

std::string_view MetadataValue(const std::vector<SpectrumMetadataEntry>& metadata, std::string_view key)
{
    for (const SpectrumMetadataEntry& entry : metadata) {
        if (entry.key == key) {
            return entry.value;
        }
    }
    return {};
}

std::string FileTimeFingerprint(const std::filesystem::file_time_type time)
{
    return std::to_string(time.time_since_epoch().count());
}

std::string FileStatFingerprint(const std::filesystem::path& path)
{
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    const std::uintmax_t safe_size = error ? 0 : size;
    const std::filesystem::file_time_type time = std::filesystem::last_write_time(path, error);
    return "size=" + std::to_string(safe_size) + ";mtime=" + (error ? std::string{"unknown"} : FileTimeFingerprint(time));
}

bool PathsReferToSameFile(const std::filesystem::path& left, const std::filesystem::path& right)
{
    if (left.empty() || right.empty()) {
        return false;
    }
    std::error_code equivalent_error;
    if (PathExists(left) && PathExists(right) &&
        std::filesystem::equivalent(left, right, equivalent_error) && !equivalent_error) {
        return true;
    }
    return left.lexically_normal() == right.lexically_normal();
}

std::string DirectoryEntryStatFingerprint(const std::filesystem::directory_entry& entry)
{
    std::error_code size_error;
    const std::uintmax_t size = entry.file_size(size_error);
    const std::uintmax_t safe_size = size_error ? 0 : size;
    std::error_code time_error;
    const std::filesystem::file_time_type time = entry.last_write_time(time_error);
    return "size=" + std::to_string(safe_size) +
           ";mtime=" + (time_error ? std::string{"unknown"} : FileTimeFingerprint(time));
}

std::string OptionalFileFingerprint(const std::optional<std::filesystem::path>& path)
{
    if (!path) {
        return "none";
    }
    if (!PathExists(*path)) {
        return "missing";
    }
    return FileStatFingerprint(*path);
}

std::string NpySourceFingerprint(const SpectrumSnapshot& snapshot)
{
    std::string fingerprint = FileStatFingerprint(snapshot.source.path);
    const std::string_view dtype = MetadataValue(snapshot.source.metadata, "dtype");
    const std::string_view rows = MetadataValue(snapshot.source.metadata, "shape_rows");
    const std::string_view columns = MetadataValue(snapshot.source.metadata, "shape_columns");
    if (!dtype.empty()) {
        fingerprint += ";dtype=";
        fingerprint += dtype;
    }
    if (!rows.empty() || !columns.empty()) {
        fingerprint += ";shape=";
        fingerprint += rows;
        fingerprint += "x";
        fingerprint += columns;
    }
    return fingerprint;
}

struct FolderIdentityDigests {
    std::string source_fingerprint;
    std::string identity;
};

FolderIdentityDigests BuildFolderIdentityDigests(
    std::string_view source_name,
    std::size_t spectrum_count,
    const SourceCollectionFolderListing& listing,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint = {})
{
    const std::string_view initial_fingerprint = listing.readable ? std::string_view{"folder"}
                                                                  : std::string_view{"folder_unreadable"};
    StableSha256 source_digest;
    StableSha256 identity_digest;
    source_digest.Append(initial_fingerprint);
    identity_digest.Append("name=");
    identity_digest.Append(source_name);
    identity_digest.Append("|fingerprint=");
    identity_digest.Append(initial_fingerprint);

    if (listing.readable) {
        for (std::size_t index = 0; index < listing.spectra.size(); ++index) {
            if ((index & 0xfffU) == 0U) {
                Checkpoint(cancellation_checkpoint);
            }
            const SourceCollectionFolderSpectrumFile& sample = listing.spectra[index];
            const std::string filename = FileNameToUtf8(sample.path.filename());
            for (StableSha256* digest : {&source_digest, &identity_digest}) {
                digest->Append(";");
                digest->Append(filename);
                digest->Append(":");
                digest->Append(sample.stat_fingerprint);
            }
        }
    }

    identity_digest.Append("|count=");
    identity_digest.Append(std::to_string(spectrum_count));
    return FolderIdentityDigests{
        FinishVersionedSha256Digest(source_digest),
        FinishVersionedSha256Digest(identity_digest),
    };
}

std::string ObservedFileFingerprint(const std::filesystem::path& path)
{
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(path, exists_error);
    if (exists_error) {
        return "unavailable";
    }
    if (!exists) {
        return "missing";
    }
    return FileStatFingerprint(path);
}

std::vector<SourceCollectionFileDependencyState> CaptureAnnotationDependencies(
    std::vector<std::filesystem::path> paths,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint)
{
    std::vector<std::filesystem::path> expanded;
    expanded.reserve(paths.size() * 2U);
    for (const std::filesystem::path& path : paths) {
        Checkpoint(cancellation_checkpoint);
        if (path.empty()) {
            continue;
        }
        expanded.push_back(path);
        if (ExtensionLower(path) != ".asdf") {
            expanded.push_back(
                SampleAnnotationIoAdapter::MetadataPathForResult(path));
        }
    }

    std::vector<SourceCollectionFileDependencyState> dependencies;
    dependencies.reserve(expanded.size());
    for (const std::filesystem::path& path : expanded) {
        Checkpoint(cancellation_checkpoint);
        dependencies.push_back({SourcePathIdentityKey(path), ObservedFileFingerprint(path)});
    }
    std::sort(
        dependencies.begin(),
        dependencies.end(),
        [](const SourceCollectionFileDependencyState& left,
           const SourceCollectionFileDependencyState& right) {
            return left.path_key < right.path_key;
        });
    dependencies.erase(
        std::unique(
            dependencies.begin(),
            dependencies.end(),
            [](const SourceCollectionFileDependencyState& left,
               const SourceCollectionFileDependencyState& right) {
                return left.path_key == right.path_key;
            }),
        dependencies.end());
    Checkpoint(cancellation_checkpoint);
    return dependencies;
}

NpyStringData ReadStringNpyData(
    std::ifstream& stream,
    const std::filesystem::path& path,
    std::size_t expected_count)
{
    const NpyHeader header = ReadNpyHeader(stream);
    if (header.shape.size() != 1 || header.shape[0] != expected_count) {
        throw SourceCollectionManifestError("NPY array length does not match the source collection");
    }
    const std::optional<NpyScalarType> scalar_type = ParseNpyScalarType(header.descr);
    if (!scalar_type || (scalar_type->kind != NpyScalarKind::Bytes && scalar_type->kind != NpyScalarKind::Unicode)) {
        throw SourceCollectionManifestError("NPY array is not a string dtype");
    }
    ValidateNpyPayloadSize(path, header, expected_count, scalar_type->item_size);

    if (header.data_offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw SourceCollectionManifestError("NPY data offset is too large");
    }
    return NpyStringData{*scalar_type, header.data_offset};
}

std::vector<std::string> ReadStringNpyValues(
    const std::filesystem::path& path,
    std::size_t expected_count,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint = {})
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw SourceCollectionManifestError("could not open the NPY file");
    }

    const NpyStringData data = ReadStringNpyData(stream, path, expected_count);
    stream.seekg(static_cast<std::streamoff>(data.data_offset), std::ios::beg);
    if (!stream) {
        throw SourceCollectionManifestError("could not seek to the NPY data");
    }

    std::vector<std::string> values;
    values.reserve(expected_count);
    std::string bytes(data.scalar_type.item_size, '\0');
    for (std::size_t index = 0; index < expected_count; ++index) {
        if ((index & 0xfffU) == 0U) {
            Checkpoint(cancellation_checkpoint);
        }
        stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            throw SourceCollectionManifestError("NPY string data is truncated");
        }
        values.push_back(DecodeNpyString(bytes, data.scalar_type));
    }
    return values;
}

void LoadNpySampleNames(
    SourceCollectionManifest& manifest,
    const std::filesystem::path& source_path,
    std::size_t spectrum_count,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint = {})
{
    Checkpoint(cancellation_checkpoint);
    const std::optional<std::filesystem::path> name_path = SourceCollectionCompanionNamePath(source_path);
    if (!name_path || !PathExists(*name_path)) {
        return;
    }

    try {
        manifest.sample_names = ReadStringNpyValues(*name_path, spectrum_count, cancellation_checkpoint);
        if (!SourceCollectionSampleNamesFormCanonicalRoster(
                manifest.sample_names,
                spectrum_count)) {
            manifest.diagnostics.push_back({
                .kind =
                    SourceCollectionManifestDiagnosticKind::
                        SampleNamesIgnored,
                .path = *name_path,
                .detail =
                    "sample names remain available for display but must be non-blank and unique for canonical roster identity; using source-index roster identity",
            });
        }
    } catch (const std::exception& error) {
        Checkpoint(cancellation_checkpoint);
        manifest.diagnostics.push_back({
            .kind =
                SourceCollectionManifestDiagnosticKind::
                    SampleNamesIgnored,
            .path = *name_path,
            .detail = error.what(),
        });
    }
}

void LoadNpyAutoAnnotations(
    SourceCollectionManifest& manifest,
    const std::filesystem::path& source_path,
    std::size_t spectrum_count,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint = {})
{
    Checkpoint(cancellation_checkpoint);
    const std::optional<std::filesystem::path> annotation_path = SourceCollectionCompanionAnnotationPath(source_path);
    if (!annotation_path || !PathExists(*annotation_path)) {
        return;
    }

    std::string error_message;
    std::optional<SampleAnnotationResult> annotation =
        SampleAnnotationIoAdapter{}.LoadCancelable(
            *annotation_path,
            spectrum_count,
            cancellation_checkpoint,
            &error_message);
    if (annotation) {
        if (!annotation->metadata_warning.empty()) {
            manifest.diagnostics.push_back({
                .kind =
                    SourceCollectionManifestDiagnosticKind::
                        AnnotationMetadataIgnored,
                .path =
                    SampleAnnotationIoAdapter::
                        MetadataPathForResult(
                            annotation->path),
                .detail =
                    annotation->metadata_warning,
            });
        }
        manifest.annotations.push_back(std::move(*annotation));
    } else {
        manifest.diagnostics.push_back({
            .kind =
                SourceCollectionManifestDiagnosticKind::
                    AnnotationIgnored,
            .path = *annotation_path,
            .detail = std::move(error_message),
        });
    }
}

std::vector<std::string> LoadFolderSampleNames(
    const SourceCollectionFolderListing& listing,
    std::size_t expected_count,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint = {})
{
    if (!listing.readable || listing.spectra.size() != expected_count) {
        return {};
    }

    std::vector<std::string> names;
    names.reserve(listing.spectra.size());
    for (std::size_t index = 0; index < listing.spectra.size(); ++index) {
        if ((index & 0xfffU) == 0U) {
            Checkpoint(cancellation_checkpoint);
        }
        names.push_back(FileNameToUtf8(listing.spectra[index].path.filename()));
    }
    return names;
}

SourceCollectionIdentity BuildFolderSourceCollectionIdentity(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionFolderListing& listing,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint = {})
{
    SourceCollectionIdentity identity;
    identity.source_name = FileNameToUtf8(snapshot.source.path.filename());
    identity.spectrum_count = snapshot.collection.spectrum_count;
    FolderIdentityDigests digests =
        BuildFolderIdentityDigests(
            identity.source_name,
            identity.spectrum_count,
            listing,
            cancellation_checkpoint);
    identity.source_fingerprint = std::move(digests.source_fingerprint);
    identity.context_fingerprint = identity.source_fingerprint;
    identity.id = std::move(digests.identity);
    return identity;
}

SourceCollectionManifest BuildFolderSourceCollectionManifest(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionFolderListing& listing,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint = {})
{
    SourceCollectionManifest manifest;
    manifest.sample_names =
        LoadFolderSampleNames(listing, snapshot.collection.spectrum_count, cancellation_checkpoint);
    return manifest;
}

void PushExample(std::vector<std::string>& examples, const std::filesystem::path& path)
{
    constexpr std::size_t kMaxExamples = 3;
    if (examples.size() < kMaxExamples) {
        examples.push_back(FileNameToUtf8(path));
    }
}

}  // namespace

SourceCollectionIdentity BuildSourceCollectionIdentity(const SpectrumSnapshot& snapshot)
{
    SourceCollectionIdentity identity;
    identity.source_name = FileNameToUtf8(snapshot.source.path.filename());
    identity.spectrum_count = snapshot.collection.spectrum_count;

    std::error_code directory_error;
    const bool is_directory = std::filesystem::is_directory(snapshot.source.path, directory_error);
    if (!directory_error && is_directory) {
        return BuildFolderSourceCollectionIdentity(snapshot, ScanSourceCollectionFolder(snapshot.source.path));
    } else if (ExtensionLower(snapshot.source.path) == ".npy") {
        const std::string legacy_source_fingerprint = NpySourceFingerprint(snapshot);
        const std::string legacy_context_fingerprint =
            legacy_source_fingerprint +
            "|name=" + OptionalFileFingerprint(SourceCollectionCompanionNamePath(snapshot.source.path)) +
            "|annotation=" + OptionalFileFingerprint(SourceCollectionCompanionAnnotationPath(snapshot.source.path));
        identity.source_fingerprint = VersionedSha256Digest(legacy_source_fingerprint);
        identity.context_fingerprint = VersionedSha256Digest(legacy_context_fingerprint);
        identity.id = VersionedSha256Digest(
            "name=" + identity.source_name + "|fingerprint=" + legacy_source_fingerprint +
            "|count=" + std::to_string(identity.spectrum_count));
    } else {
        const std::string legacy_source_fingerprint = FileStatFingerprint(snapshot.source.path);
        identity.source_fingerprint = VersionedSha256Digest(legacy_source_fingerprint);
        identity.context_fingerprint = identity.source_fingerprint;
        identity.id = VersionedSha256Digest(
            "name=" + identity.source_name + "|fingerprint=" + legacy_source_fingerprint +
            "|count=" + std::to_string(identity.spectrum_count));
    }
    return identity;
}

SourceCollectionIdentity BuildSourceCollectionIdentity(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionSingleFileState& file_state)
{
    SourceCollectionIdentity identity;
    identity.source_name = FileNameToUtf8(snapshot.source.path.filename());
    identity.spectrum_count = snapshot.collection.spectrum_count;
    if (ExtensionLower(snapshot.source.path) == ".npy") {
        std::string legacy_source_fingerprint = file_state.source_stat_fingerprint;
        const std::string_view dtype = MetadataValue(snapshot.source.metadata, "dtype");
        const std::string_view rows = MetadataValue(snapshot.source.metadata, "shape_rows");
        const std::string_view columns = MetadataValue(snapshot.source.metadata, "shape_columns");
        if (!dtype.empty()) {
            legacy_source_fingerprint += ";dtype=";
            legacy_source_fingerprint += dtype;
        }
        if (!rows.empty() || !columns.empty()) {
            legacy_source_fingerprint += ";shape=";
            legacy_source_fingerprint += rows;
            legacy_source_fingerprint += "x";
            legacy_source_fingerprint += columns;
        }
        const std::string legacy_context_fingerprint =
            legacy_source_fingerprint + "|name=" + file_state.companion_name_fingerprint +
            "|annotation=" + file_state.companion_annotation_fingerprint;
        identity.source_fingerprint = VersionedSha256Digest(legacy_source_fingerprint);
        identity.context_fingerprint = VersionedSha256Digest(legacy_context_fingerprint);
        identity.id = VersionedSha256Digest(
            "name=" + identity.source_name + "|fingerprint=" + legacy_source_fingerprint +
            "|count=" + std::to_string(identity.spectrum_count));
    } else {
        identity.source_fingerprint = VersionedSha256Digest(file_state.source_stat_fingerprint);
        identity.context_fingerprint = identity.source_fingerprint;
        identity.id = VersionedSha256Digest(
            "name=" + identity.source_name + "|fingerprint=" + file_state.source_stat_fingerprint +
            "|count=" + std::to_string(identity.spectrum_count));
    }
    return identity;
}

SourceCollectionManifest LoadSourceCollectionManifest(const SpectrumSnapshot& snapshot)
{
    return LoadSourceCollectionManifestCancelable(snapshot, {});
}

SourceCollectionManifest LoadSourceCollectionManifestCancelable(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint,
    SourceCollectionAnnotationDiscovery annotation_discovery)
{
    Checkpoint(cancellation_checkpoint);
    SourceCollectionManifest manifest;
    if (snapshot.source.path.empty() || snapshot.collection.spectrum_count == 0) {
        return manifest;
    }

    std::error_code directory_error;
    const bool is_directory = std::filesystem::is_directory(snapshot.source.path, directory_error);
    if (!directory_error && is_directory) {
        const SourceCollectionFolderListing listing = ScanSourceCollectionFolder(
            snapshot.source.path,
            [&cancellation_checkpoint](std::size_t) { Checkpoint(cancellation_checkpoint); });
        return BuildFolderSourceCollectionManifest(snapshot, listing, cancellation_checkpoint);
    }

    if (ExtensionLower(snapshot.source.path) == ".npy") {
        LoadNpySampleNames(
            manifest,
            snapshot.source.path,
            snapshot.collection.spectrum_count,
            cancellation_checkpoint);
        if (annotation_discovery == SourceCollectionAnnotationDiscovery::Enabled) {
            LoadNpyAutoAnnotations(
                manifest,
                snapshot.source.path,
                snapshot.collection.spectrum_count,
                cancellation_checkpoint);
        }
    }
    Checkpoint(cancellation_checkpoint);
    return manifest;
}

namespace {

bool FinishReadOnlySampleAnnotationIngestion(
    SourceCollectionManifest& manifest,
    const std::filesystem::path& path,
    std::optional<SampleAnnotationResult> annotation,
    std::string load_error,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint,
    std::string* message)
{
    if (!annotation) {
        std::string ignored_message =
            "Ignored " + FileNameToUtf8(path.filename()) + ": " +
            load_error + ".";
        manifest.diagnostics.push_back({
            .kind =
                SourceCollectionManifestDiagnosticKind::
                    AnnotationIgnored,
            .path = path,
            .detail = std::move(load_error),
        });
        if (message != nullptr) {
            *message = std::move(ignored_message);
        }
        return false;
    }

    const std::string metadata_warning = annotation->metadata_warning;
    const auto existing = std::find_if(
        manifest.annotations.begin(),
        manifest.annotations.end(),
        [&path](const SampleAnnotationResult& candidate) {
            return PathsReferToSameFile(candidate.path, path);
        });
    if (existing != manifest.annotations.end()) {
        *existing = std::move(*annotation);
    } else {
        manifest.annotations.push_back(std::move(*annotation));
    }
    if (!metadata_warning.empty()) {
        manifest.diagnostics.push_back({
            .kind =
                SourceCollectionManifestDiagnosticKind::
                    AnnotationMetadataIgnored,
            .path =
                SampleAnnotationIoAdapter::
                    MetadataPathForResult(path),
            .detail = metadata_warning,
        });
    }
    if (message != nullptr) {
        message->clear();
    }
    Checkpoint(cancellation_checkpoint);
    return true;
}

}  // namespace

bool SourceCollectionSampleNamesFormCanonicalRoster(
    std::span<const std::string> sample_names,
    std::size_t expected_count)
{
    if (sample_names.empty() ||
        sample_names.size() != expected_count) {
        return false;
    }
    for (const std::string& name : sample_names) {
        if (std::none_of(
                name.begin(),
                name.end(),
                [](unsigned char character) {
                    return std::isspace(character) == 0;
                })) {
            return false;
        }
    }

    std::vector<std::size_t> order(sample_names.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::sort(
        order,
        [sample_names](std::size_t left, std::size_t right) {
            const std::string& left_name = sample_names[left];
            const std::string& right_name = sample_names[right];
            return left_name < right_name ||
                (left_name == right_name && left < right);
        });
    for (std::size_t index = 1; index < order.size(); ++index) {
        if (sample_names[order[index - 1U]] ==
            sample_names[order[index]]) {
            return false;
        }
    }
    return true;
}

bool IngestReadOnlySampleAnnotation(
    SourceCollectionManifest& manifest,
    const std::filesystem::path& path,
    std::size_t expected_count,
    std::string* message)
{
    return IngestReadOnlySampleAnnotationCancelable(manifest, path, expected_count, {}, message);
}

bool IngestReadOnlySampleAnnotation(
    SourceCollectionManifest& manifest,
    const std::filesystem::path& path,
    const SampleAnnotationSourceCompatibility& source,
    std::string* message)
{
    return IngestReadOnlySampleAnnotationCancelable(
        manifest,
        path,
        source,
        {},
        message);
}

bool IngestReadOnlySampleAnnotationCancelable(
    SourceCollectionManifest& manifest,
    const std::filesystem::path& path,
    std::size_t expected_count,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint,
    std::string* message)
{
    Checkpoint(cancellation_checkpoint);
    std::string load_error;
    std::optional<SampleAnnotationResult> annotation =
        SampleAnnotationIoAdapter{}.LoadCancelable(
            path,
            expected_count,
            cancellation_checkpoint,
            &load_error);
    return FinishReadOnlySampleAnnotationIngestion(
        manifest,
        path,
        std::move(annotation),
        std::move(load_error),
        cancellation_checkpoint,
        message);
}

bool IngestReadOnlySampleAnnotationCancelable(
    SourceCollectionManifest& manifest,
    const std::filesystem::path& path,
    const SampleAnnotationSourceCompatibility& source,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint,
    std::string* message)
{
    Checkpoint(cancellation_checkpoint);
    std::string load_error;
    std::optional<SampleAnnotationResult> annotation =
        SampleAnnotationIoAdapter{}.LoadForSourceCancelable(
            path,
            source,
            cancellation_checkpoint,
            &load_error);
    return FinishReadOnlySampleAnnotationIngestion(
        manifest,
        path,
        std::move(annotation),
        std::move(load_error),
        cancellation_checkpoint,
        message);
}

bool SourceCollectionManifestContainsAnnotation(
    const SourceCollectionManifest& manifest,
    const std::filesystem::path& path)
{
    return std::any_of(
        manifest.annotations.begin(),
        manifest.annotations.end(),
        [&path](const SampleAnnotationResult& annotation) {
            return PathsReferToSameFile(annotation.path, path);
        });
}

SourceCollectionContext LoadSourceCollectionContext(const SpectrumSnapshot& snapshot)
{
    return LoadSourceCollectionContextCancelable(snapshot, {});
}

SourceCollectionContext LoadSourceCollectionContextCancelable(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint)
{
    Checkpoint(cancellation_checkpoint);
    if (snapshot.source.path.empty() || snapshot.collection.spectrum_count == 0) {
        return {};
    }

    std::error_code directory_error;
    const bool is_directory = std::filesystem::is_directory(snapshot.source.path, directory_error);
    if (!directory_error && is_directory) {
        const SourceCollectionFolderListing listing = ScanSourceCollectionFolder(
            snapshot.source.path,
            [&cancellation_checkpoint](std::size_t) { Checkpoint(cancellation_checkpoint); });
        return BuildFolderSourceCollectionContextCancelable(snapshot, listing, cancellation_checkpoint);
    }

    SourceCollectionIdentity identity = BuildSourceCollectionIdentity(snapshot);
    Checkpoint(cancellation_checkpoint);
    SourceCollectionManifest manifest =
        LoadSourceCollectionManifestCancelable(snapshot, cancellation_checkpoint);
    return SourceCollectionContext{std::move(identity), std::move(manifest)};
}

SourceCollectionContext LoadSourceCollectionContextCancelable(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionSingleFileState& file_state,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint,
    SourceCollectionAnnotationDiscovery annotation_discovery)
{
    Checkpoint(cancellation_checkpoint);
    if (snapshot.source.path.empty() || snapshot.collection.spectrum_count == 0) {
        return {};
    }
    SourceCollectionIdentity identity = BuildSourceCollectionIdentity(snapshot, file_state);
    Checkpoint(cancellation_checkpoint);
    SourceCollectionManifest manifest =
        LoadSourceCollectionManifestCancelable(snapshot, cancellation_checkpoint, annotation_discovery);
    return SourceCollectionContext{std::move(identity), std::move(manifest)};
}

SourceCollectionSingleFileState CaptureSourceCollectionSingleFileState(
    const std::filesystem::path& source_path,
    const std::vector<std::filesystem::path>& annotation_paths,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint)
{
    Checkpoint(cancellation_checkpoint);
    SourceCollectionSingleFileState state;
    state.source_stat_fingerprint = ObservedFileFingerprint(source_path);
    const std::optional<std::filesystem::path> name_path = SourceCollectionCompanionNamePath(source_path);
    const std::optional<std::filesystem::path> annotation_path =
        SourceCollectionCompanionAnnotationPath(source_path);
    state.companion_name_fingerprint = OptionalFileFingerprint(name_path);
    state.companion_annotation_fingerprint = OptionalFileFingerprint(annotation_path);
    std::vector<std::filesystem::path> all_annotation_paths = annotation_paths;
    if (annotation_path) {
        all_annotation_paths.push_back(*annotation_path);
    }
    state.annotation_dependencies =
        CaptureAnnotationDependencies(std::move(all_annotation_paths), cancellation_checkpoint);
    return state;
}

bool SourceCollectionSingleFileStatesMatch(
    const SourceCollectionSingleFileState& left,
    const SourceCollectionSingleFileState& right)
{
    return left == right;
}

void FinalizeSourceCollectionAnnotationContextFingerprint(
    SourceCollectionContext& context,
    const std::vector<std::filesystem::path>& requested_annotation_paths,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint)
{
    std::vector<std::filesystem::path> paths = requested_annotation_paths;
    paths.reserve(paths.size() + context.manifest.annotations.size());
    for (const SampleAnnotationResult& annotation : context.manifest.annotations) {
        Checkpoint(cancellation_checkpoint);
        if (!annotation.path.empty()) {
            paths.push_back(annotation.path);
        }
    }
    const std::vector<SourceCollectionFileDependencyState> dependencies =
        CaptureAnnotationDependencies(std::move(paths), cancellation_checkpoint);
    if (dependencies.empty()) {
        return;
    }

    StableSha256 digest;
    digest.Append("base=");
    digest.Append(context.identity.context_fingerprint);
    for (const SourceCollectionFileDependencyState& dependency : dependencies) {
        Checkpoint(cancellation_checkpoint);
        digest.Append("|path=");
        digest.Append(dependency.path_key);
        digest.Append("|stat=");
        digest.Append(dependency.stat_fingerprint);
    }
    context.identity.context_fingerprint = FinishVersionedSha256Digest(digest);
    Checkpoint(cancellation_checkpoint);
}

SourceCollectionFolderListing ScanSourceCollectionFolder(const std::filesystem::path& path)
{
    return ScanSourceCollectionFolder(path, {}, {});
}

SourceCollectionFolderListing ScanSourceCollectionFolder(
    const std::filesystem::path& path,
    const SourceCollectionFolderScanProgress& progress)
{
    return ScanSourceCollectionFolder(path, progress, {});
}

SourceCollectionFolderListing ScanSourceCollectionFolder(
    const std::filesystem::path& path,
    const SourceCollectionFolderScanProgress& progress,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint)
{
    SourceCollectionFolderListing listing;
    std::error_code iterator_error;
    std::filesystem::directory_iterator iterator(
        path,
        std::filesystem::directory_options::none,
        iterator_error);
    if (iterator_error) {
        listing.readable = false;
        listing.error_message = "Could not enumerate the input folder.";
        return listing;
    }

    std::size_t processed_entry_count = 0;
    for (const std::filesystem::directory_entry& entry : iterator) {
        Checkpoint(cancellation_checkpoint);
        ++processed_entry_count;
        if (progress) {
            progress(processed_entry_count);
        }
        std::error_code type_error;
        if (entry.is_directory(type_error)) {
            ++listing.ignored_directory_count;
            PushExample(listing.ignored_directory_examples, entry.path());
            continue;
        }
        if (type_error) {
            ++listing.ignored_file_count;
            PushExample(listing.ignored_file_examples, entry.path());
            continue;
        }

        if (!entry.is_regular_file(type_error) || type_error) {
            ++listing.ignored_file_count;
            PushExample(listing.ignored_file_examples, entry.path());
            continue;
        }

        const std::string format = detail::SourceFormatLabel(entry.path());
        if (!detail::IsSupportedSingleFileSpectrumPath(entry.path())) {
            ++listing.ignored_file_count;
            PushExample(listing.ignored_file_examples, entry.path());
            continue;
        }

        if (format == "csv") {
            ++listing.csv_count;
        } else if (format == "fits" || format == "fits.gz") {
            ++listing.fits_count;
        }
        listing.spectra.push_back(SourceCollectionFolderSpectrumFile{
            entry.path(),
            format,
            DirectoryEntryStatFingerprint(entry),
        });
    }

    std::size_t comparison_count = 0;
    std::stable_sort(
        listing.spectra.begin(),
        listing.spectra.end(),
        [&comparison_count, &cancellation_checkpoint](
            const SourceCollectionFolderSpectrumFile& left,
            const SourceCollectionFolderSpectrumFile& right) {
            if ((comparison_count++ & 0x3ffU) == 0U) {
                Checkpoint(cancellation_checkpoint);
            }
            return LowerAscii(FileNameToUtf8(left.path)) < LowerAscii(FileNameToUtf8(right.path));
        });
    Checkpoint(cancellation_checkpoint);
    return listing;
}

bool SourceCollectionFolderListingsMatch(
    const SourceCollectionFolderListing& left,
    const SourceCollectionFolderListing& right,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint)
{
    if (left.readable != right.readable || left.spectra.size() != right.spectra.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.spectra.size(); ++index) {
        if ((index & 0xfffU) == 0U) {
            Checkpoint(cancellation_checkpoint);
        }
        const SourceCollectionFolderSpectrumFile& left_file = left.spectra[index];
        const SourceCollectionFolderSpectrumFile& right_file = right.spectra[index];
        if (left_file.path != right_file.path || left_file.format != right_file.format ||
            left_file.stat_fingerprint != right_file.stat_fingerprint) {
            return false;
        }
    }
    Checkpoint(cancellation_checkpoint);
    return true;
}

bool SourceCollectionFolderSpectrumFileMatchesCurrentState(
    const SourceCollectionFolderSpectrumFile& file)
{
    std::error_code type_error;
    return std::filesystem::is_regular_file(file.path, type_error) && !type_error &&
           FileStatFingerprint(file.path) == file.stat_fingerprint;
}

SourceCollectionContext BuildFolderSourceCollectionContext(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionFolderListing& listing)
{
    return BuildFolderSourceCollectionContextCancelable(snapshot, listing, {});
}

SourceCollectionContext BuildFolderSourceCollectionContextCancelable(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionFolderListing& listing,
    const SourceCollectionCancellationCheckpoint& cancellation_checkpoint)
{
    return SourceCollectionContext{
        BuildFolderSourceCollectionIdentity(snapshot, listing, cancellation_checkpoint),
        BuildFolderSourceCollectionManifest(snapshot, listing, cancellation_checkpoint),
    };
}

bool IsSourceCollectionAuxiliaryNpyArrayName(const std::filesystem::path& source_path)
{
    const std::string filename = LowerAscii(FileNameToUtf8(source_path));
    static constexpr std::array<std::string_view, 9> kAuxiliaryNames = {
        "y.npy",
        "label.npy",
        "index.npy",
        "ormask.npy",
        "inverse.npy",
        "known_mask.npy",
        "ivar.npy",
        "mask.npy",
        "name.npy",
    };
    static constexpr std::array<std::string_view, 9> kAuxiliarySuffixes = {
        "_y.npy",
        "_label.npy",
        "_index.npy",
        "_ormask.npy",
        "_inverse.npy",
        "_known_mask.npy",
        "_ivar.npy",
        "_mask.npy",
        "_name.npy",
    };

    return std::any_of(kAuxiliaryNames.begin(), kAuxiliaryNames.end(), [&filename](std::string_view name) {
               return filename == name;
           }) ||
           std::any_of(kAuxiliarySuffixes.begin(), kAuxiliarySuffixes.end(), [&filename](std::string_view suffix) {
               return filename.ends_with(suffix);
           });
}

std::optional<std::filesystem::path> SourceCollectionCompanionNamePath(const std::filesystem::path& source_path)
{
    return CompanionNpyPath(source_path, "_name.npy", "-name.npy", "name.npy");
}

std::optional<std::filesystem::path> SourceCollectionCompanionAnnotationPath(const std::filesystem::path& source_path)
{
    return CompanionNpyPath(source_path, "_y.npy", "-y.npy", "y.npy");
}

std::optional<std::string> LoadSourceCollectionNpySampleName(
    const std::filesystem::path& source_path,
    std::size_t expected_count,
    std::size_t sample_index)
{
    if (sample_index >= expected_count) {
        return std::nullopt;
    }

    const std::optional<std::filesystem::path> name_path = SourceCollectionCompanionNamePath(source_path);
    if (!name_path || !PathExists(*name_path)) {
        return std::nullopt;
    }

    std::ifstream stream(*name_path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }

    try {
        const NpyStringData data = ReadStringNpyData(stream, *name_path, expected_count);
        const std::uint64_t row_offset =
            data.data_offset + static_cast<std::uint64_t>(sample_index) * data.scalar_type.item_size;
        if (row_offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            return std::nullopt;
        }
        stream.seekg(static_cast<std::streamoff>(row_offset), std::ios::beg);
        if (!stream) {
            return std::nullopt;
        }
        std::string bytes(data.scalar_type.item_size, '\0');
        stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            return std::nullopt;
        }
        std::string decoded = DecodeNpyString(bytes, data.scalar_type);
        if (decoded.empty()) {
            return std::nullopt;
        }
        return decoded;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace spectiary
