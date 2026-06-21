#include "domain/source_collection_manifest.h"

#include "domain/npy_array_io.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace specforge {
namespace {

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

bool IsFitsExtension(std::string_view extension)
{
    return extension == ".fits" || extension == ".fit" || extension == ".fts";
}

bool IsFitsSourceFile(const std::filesystem::path& path)
{
    const std::string extension = ExtensionLower(path);
    return IsFitsExtension(extension) || (extension == ".gz" && IsFitsExtension(ExtensionLower(path.stem())));
}

std::string SourceCollectionFileFormat(const std::filesystem::path& path)
{
    const std::string extension = ExtensionLower(path);
    if (extension == ".csv") {
        return "csv";
    }
    if (IsFitsExtension(extension)) {
        return "fits";
    }
    if (extension == ".gz" && IsFitsExtension(ExtensionLower(path.stem()))) {
        return "fits.gz";
    }
    return "file";
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

std::string FolderSourceFingerprint(const std::filesystem::path& path)
{
    const SourceCollectionFolderListing listing = ScanSourceCollectionFolder(path);
    if (!listing.readable) {
        return "folder_unreadable";
    }

    std::string fingerprint = "folder";
    for (const SourceCollectionFolderSpectrumFile& sample : listing.spectra) {
        fingerprint += ";";
        fingerprint += FileNameToUtf8(sample.path.filename());
        fingerprint += ":";
        fingerprint += FileStatFingerprint(sample.path);
    }
    return fingerprint;
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

std::vector<std::string> ReadStringNpyValues(const std::filesystem::path& path, std::size_t expected_count)
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
        stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            throw SourceCollectionManifestError("NPY string data is truncated");
        }
        values.push_back(DecodeNpyString(bytes, data.scalar_type));
    }
    return values;
}

void LoadNpySampleNames(SourceCollectionManifest& manifest, const std::filesystem::path& source_path, std::size_t spectrum_count)
{
    const std::optional<std::filesystem::path> name_path = SourceCollectionCompanionNamePath(source_path);
    if (!name_path || !PathExists(*name_path)) {
        return;
    }

    try {
        manifest.sample_names = ReadStringNpyValues(*name_path, spectrum_count);
    } catch (const std::exception& error) {
        manifest.messages.push_back("Ignored " + FileNameToUtf8(name_path->filename()) + ": " + error.what() + ".");
    }
}

void LoadNpyAutoAnnotations(
    SourceCollectionManifest& manifest,
    const std::filesystem::path& source_path,
    std::size_t spectrum_count)
{
    const std::optional<std::filesystem::path> annotation_path = SourceCollectionCompanionAnnotationPath(source_path);
    if (!annotation_path || !PathExists(*annotation_path)) {
        return;
    }

    std::string error_message;
    std::optional<SampleAnnotationResult> annotation =
        LoadSampleAnnotationResultFromPath(*annotation_path, spectrum_count, &error_message);
    if (annotation) {
        manifest.annotations.push_back(std::move(*annotation));
    } else {
        manifest.messages.push_back("Ignored " + FileNameToUtf8(annotation_path->filename()) + ": " + error_message + ".");
    }
}

std::vector<std::string> LoadFolderSampleNames(const std::filesystem::path& path, std::size_t expected_count)
{
    const SourceCollectionFolderListing listing = ScanSourceCollectionFolder(path);
    if (!listing.readable || listing.spectra.size() != expected_count) {
        return {};
    }

    std::vector<std::string> names;
    names.reserve(listing.spectra.size());
    for (const SourceCollectionFolderSpectrumFile& sample : listing.spectra) {
        names.push_back(FileNameToUtf8(sample.path.filename()));
    }
    return names;
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
        identity.source_fingerprint = FolderSourceFingerprint(snapshot.source.path);
        identity.context_fingerprint = identity.source_fingerprint;
    } else if (ExtensionLower(snapshot.source.path) == ".npy") {
        identity.source_fingerprint = NpySourceFingerprint(snapshot);
        identity.context_fingerprint = identity.source_fingerprint +
                                       "|name=" + OptionalFileFingerprint(SourceCollectionCompanionNamePath(snapshot.source.path)) +
                                       "|annotation=" +
                                           OptionalFileFingerprint(SourceCollectionCompanionAnnotationPath(snapshot.source.path));
    } else {
        identity.source_fingerprint = FileStatFingerprint(snapshot.source.path);
        identity.context_fingerprint = identity.source_fingerprint;
    }

    identity.id = "name=" + identity.source_name + "|fingerprint=" + identity.source_fingerprint +
                  "|count=" + std::to_string(identity.spectrum_count);
    return identity;
}

SourceCollectionManifest LoadSourceCollectionManifest(const SpectrumSnapshot& snapshot)
{
    SourceCollectionManifest manifest;
    if (snapshot.source.path.empty() || snapshot.collection.spectrum_count == 0) {
        return manifest;
    }

    std::error_code directory_error;
    const bool is_directory = std::filesystem::is_directory(snapshot.source.path, directory_error);
    if (!directory_error && is_directory) {
        manifest.sample_names = LoadFolderSampleNames(snapshot.source.path, snapshot.collection.spectrum_count);
        return manifest;
    }

    if (ExtensionLower(snapshot.source.path) == ".npy") {
        LoadNpySampleNames(manifest, snapshot.source.path, snapshot.collection.spectrum_count);
        LoadNpyAutoAnnotations(manifest, snapshot.source.path, snapshot.collection.spectrum_count);
    }
    return manifest;
}

SourceCollectionFolderListing ScanSourceCollectionFolder(const std::filesystem::path& path)
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

    for (const std::filesystem::directory_entry& entry : iterator) {
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

        if (ExtensionLower(entry.path()) == ".csv") {
            ++listing.csv_count;
            listing.spectra.push_back(SourceCollectionFolderSpectrumFile{entry.path(), "csv"});
        } else if (IsFitsSourceFile(entry.path())) {
            ++listing.fits_count;
            listing.spectra.push_back(SourceCollectionFolderSpectrumFile{entry.path(), SourceCollectionFileFormat(entry.path())});
        } else {
            ++listing.ignored_file_count;
            PushExample(listing.ignored_file_examples, entry.path());
        }
    }

    std::stable_sort(
        listing.spectra.begin(),
        listing.spectra.end(),
        [](const SourceCollectionFolderSpectrumFile& left, const SourceCollectionFolderSpectrumFile& right) {
            return LowerAscii(FileNameToUtf8(left.path)) < LowerAscii(FileNameToUtf8(right.path));
        });
    return listing;
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

}  // namespace specforge
