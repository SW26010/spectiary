#include "domain/sample_annotation_io.h"

#include "domain/npy_array_io.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace specforge {
namespace {

class NpyAnnotationError : public std::runtime_error {
public:
    explicit NpyAnnotationError(std::string message)
        : std::runtime_error(std::move(message))
    {
    }
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

std::string TrimAscii(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char character) {
        return std::isspace(character) != 0;
    }).base();

    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::string FormatFloatingValue(double value)
{
    if (std::isnan(value)) {
        return "nan";
    }
    if (std::isinf(value)) {
        return value < 0.0 ? "-inf" : "inf";
    }

    std::ostringstream stream;
    stream << std::setprecision(15) << value;
    return stream.str();
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

std::optional<std::filesystem::path> CompanionNamePath(const std::filesystem::path& path)
{
    return CompanionNpyPath(path, "_name.npy", "-name.npy", "name.npy");
}

std::optional<std::filesystem::path> CompanionAnnotationPath(const std::filesystem::path& path)
{
    return CompanionNpyPath(path, "_y.npy", "-y.npy", "y.npy");
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
    std::vector<std::filesystem::path> sample_paths;
    std::error_code iterator_error;
    std::filesystem::directory_iterator iterator(path, std::filesystem::directory_options::none, iterator_error);
    if (iterator_error) {
        return "folder_unreadable";
    }

    for (const std::filesystem::directory_entry& entry : iterator) {
        std::error_code type_error;
        if (!entry.is_regular_file(type_error) || type_error) {
            continue;
        }
        if (ExtensionLower(entry.path()) == ".csv" || IsFitsSourceFile(entry.path())) {
            sample_paths.push_back(entry.path());
        }
    }

    std::stable_sort(sample_paths.begin(), sample_paths.end(), [](const std::filesystem::path& left, const std::filesystem::path& right) {
        return LowerAscii(FileNameToUtf8(left)) < LowerAscii(FileNameToUtf8(right));
    });

    std::string fingerprint = "folder";
    for (const std::filesystem::path& sample_path : sample_paths) {
        fingerprint += ";";
        fingerprint += FileNameToUtf8(sample_path.filename());
        fingerprint += ":";
        fingerprint += FileStatFingerprint(sample_path);
    }
    return fingerprint;
}

std::vector<std::string> ReadStringNpyValues(const std::filesystem::path& path, std::size_t expected_count)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw NpyAnnotationError("could not open the NPY file");
    }

    const NpyHeader header = ReadNpyHeader(stream);
    if (header.shape.size() != 1 || header.shape[0] != expected_count) {
        throw NpyAnnotationError("NPY array length does not match the source collection");
    }
    const std::optional<NpyScalarType> scalar_type = ParseNpyScalarType(header.descr);
    if (!scalar_type || (scalar_type->kind != NpyScalarKind::Bytes && scalar_type->kind != NpyScalarKind::Unicode)) {
        throw NpyAnnotationError("NPY array is not a string dtype");
    }
    ValidateNpyPayloadSize(path, header, expected_count, scalar_type->item_size);

    if (header.data_offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw NpyAnnotationError("NPY data offset is too large");
    }
    stream.seekg(static_cast<std::streamoff>(header.data_offset), std::ios::beg);
    if (!stream) {
        throw NpyAnnotationError("could not seek to the NPY data");
    }

    std::vector<std::string> values;
    values.reserve(expected_count);
    std::string bytes(scalar_type->item_size, '\0');
    for (std::size_t index = 0; index < expected_count; ++index) {
        stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            throw NpyAnnotationError("NPY string data is truncated");
        }
        values.push_back(DecodeNpyString(bytes, *scalar_type));
    }
    return values;
}

template <typename T>
void AssignIntegralValues(SampleAnnotationResult& result, std::ifstream& stream, std::size_t value_count)
{
    const std::vector<T> typed_values = ReadNpyTypedValues<T>(stream, value_count);
    result.values.reserve(value_count);
    for (const T value : typed_values) {
        if constexpr (std::is_signed_v<T>) {
            result.values.push_back({std::to_string(static_cast<long long>(value))});
        } else {
            result.values.push_back({std::to_string(static_cast<unsigned long long>(value))});
        }
    }
}

template <typename T>
void AssignFloatingValues(SampleAnnotationResult& result, std::ifstream& stream, std::size_t value_count)
{
    const std::vector<T> typed_values = ReadNpyTypedValues<T>(stream, value_count);
    result.values.reserve(value_count);
    for (const T value : typed_values) {
        result.values.push_back({FormatFloatingValue(static_cast<double>(value))});
    }
}

void AssignStringValues(SampleAnnotationResult& result, std::ifstream& stream, const NpyScalarType& scalar_type, std::size_t value_count)
{
    result.values.reserve(value_count);
    std::string bytes(scalar_type.item_size, '\0');
    for (std::size_t index = 0; index < value_count; ++index) {
        stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            throw NpyAnnotationError("NPY string data is truncated");
        }
        result.values.push_back({DecodeNpyString(bytes, scalar_type)});
    }
}

SampleAnnotationResult ReadAnnotationNpyValues(const std::filesystem::path& path, std::size_t expected_count)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw NpyAnnotationError("could not open the NPY file");
    }

    const NpyHeader header = ReadNpyHeader(stream);
    if (header.shape.size() != 1 || header.shape[0] != expected_count) {
        throw NpyAnnotationError("NPY array length does not match the source collection");
    }
    const std::optional<NpyScalarType> scalar_type = ParseNpyScalarType(header.descr);
    if (!scalar_type) {
        throw NpyAnnotationError("NPY dtype is not supported for read-only sample annotations");
    }
    ValidateNpyPayloadSize(path, header, expected_count, scalar_type->item_size);

    if (header.data_offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw NpyAnnotationError("NPY data offset is too large");
    }
    stream.seekg(static_cast<std::streamoff>(header.data_offset), std::ios::beg);
    if (!stream) {
        throw NpyAnnotationError("could not seek to the NPY data");
    }

    SampleAnnotationResult result;
    result.name = FileNameToUtf8(path.filename());
    result.path = path;
    result.dtype = header.descr;

    switch (scalar_type->kind) {
    case NpyScalarKind::SignedInteger:
        result.kind = SampleAnnotationKind::CategoricalInteger;
        switch (scalar_type->item_size) {
        case 1:
            AssignIntegralValues<std::int8_t>(result, stream, expected_count);
            break;
        case 2:
            AssignIntegralValues<std::int16_t>(result, stream, expected_count);
            break;
        case 4:
            AssignIntegralValues<std::int32_t>(result, stream, expected_count);
            break;
        case 8:
            AssignIntegralValues<std::int64_t>(result, stream, expected_count);
            break;
        default:
            throw NpyAnnotationError("signed integer NPY dtype is not supported");
        }
        break;
    case NpyScalarKind::UnsignedInteger:
        result.kind = SampleAnnotationKind::CategoricalInteger;
        switch (scalar_type->item_size) {
        case 1:
            AssignIntegralValues<std::uint8_t>(result, stream, expected_count);
            break;
        case 2:
            AssignIntegralValues<std::uint16_t>(result, stream, expected_count);
            break;
        case 4:
            AssignIntegralValues<std::uint32_t>(result, stream, expected_count);
            break;
        case 8:
            AssignIntegralValues<std::uint64_t>(result, stream, expected_count);
            break;
        default:
            throw NpyAnnotationError("unsigned integer NPY dtype is not supported");
        }
        break;
    case NpyScalarKind::Float:
        result.kind = SampleAnnotationKind::ContinuousFloat;
        if (scalar_type->item_size == 4) {
            AssignFloatingValues<float>(result, stream, expected_count);
        } else if (scalar_type->item_size == 8) {
            AssignFloatingValues<double>(result, stream, expected_count);
        } else {
            throw NpyAnnotationError("floating-point NPY dtype is not supported");
        }
        break;
    case NpyScalarKind::Bytes:
    case NpyScalarKind::Unicode:
        result.kind = SampleAnnotationKind::Text;
        AssignStringValues(result, stream, *scalar_type, expected_count);
        break;
    default:
        throw NpyAnnotationError("NPY dtype is not supported for read-only sample annotations");
    }

    return result;
}

void LoadNpySampleNames(SampleCollectionContext& context, const std::filesystem::path& source_path, std::size_t spectrum_count)
{
    const std::optional<std::filesystem::path> name_path = CompanionNamePath(source_path);
    if (!name_path || !PathExists(*name_path)) {
        return;
    }

    try {
        context.sample_names = ReadStringNpyValues(*name_path, spectrum_count);
    } catch (const std::exception& error) {
        context.messages.push_back("Ignored " + FileNameToUtf8(name_path->filename()) + ": " + error.what() + ".");
    }
}

void LoadNpyAutoAnnotations(SampleCollectionContext& context, const std::filesystem::path& source_path, std::size_t spectrum_count)
{
    const std::optional<std::filesystem::path> annotation_path = CompanionAnnotationPath(source_path);
    if (!annotation_path || !PathExists(*annotation_path)) {
        return;
    }

    try {
        context.annotations.push_back(ReadAnnotationNpyValues(*annotation_path, spectrum_count));
    } catch (const std::exception& error) {
        context.messages.push_back("Ignored " + FileNameToUtf8(annotation_path->filename()) + ": " + error.what() + ".");
    }
}

std::vector<std::string> LoadFolderSampleNames(const std::filesystem::path& path, std::size_t expected_count)
{
    std::vector<std::filesystem::path> sample_paths;
    std::error_code iterator_error;
    std::filesystem::directory_iterator iterator(
        path,
        std::filesystem::directory_options::none,
        iterator_error);
    if (iterator_error) {
        return {};
    }

    for (const std::filesystem::directory_entry& entry : iterator) {
        std::error_code type_error;
        if (!entry.is_regular_file(type_error) || type_error) {
            continue;
        }
        if (ExtensionLower(entry.path()) == ".csv" || IsFitsSourceFile(entry.path())) {
            sample_paths.push_back(entry.path());
        }
    }

    std::stable_sort(sample_paths.begin(), sample_paths.end(), [](const std::filesystem::path& left, const std::filesystem::path& right) {
        return LowerAscii(FileNameToUtf8(left)) < LowerAscii(FileNameToUtf8(right));
    });

    if (sample_paths.size() != expected_count) {
        return {};
    }

    std::vector<std::string> names;
    names.reserve(sample_paths.size());
    for (const std::filesystem::path& sample_path : sample_paths) {
        names.push_back(FileNameToUtf8(sample_path.filename()));
    }
    return names;
}

}  // namespace

std::optional<SampleAnnotationResult> LoadSampleAnnotationResultFromPath(
    const std::filesystem::path& path,
    std::size_t expected_count,
    std::string* error_message)
{
    try {
        return ReadAnnotationNpyValues(path, expected_count);
    } catch (const std::exception& error) {
        if (error_message != nullptr) {
            *error_message = error.what();
        }
        return std::nullopt;
    }
}

SampleCollectionContext LoadSampleCollectionContext(const SpectrumSnapshot& snapshot)
{
    SampleCollectionContext context;
    if (snapshot.source.path.empty() || snapshot.collection.spectrum_count == 0) {
        return context;
    }

    std::error_code directory_error;
    const bool is_directory = std::filesystem::is_directory(snapshot.source.path, directory_error);
    if (!directory_error && is_directory) {
        context.sample_names = LoadFolderSampleNames(snapshot.source.path, snapshot.collection.spectrum_count);
        return context;
    }

    if (ExtensionLower(snapshot.source.path) == ".npy") {
        LoadNpySampleNames(context, snapshot.source.path, snapshot.collection.spectrum_count);
        LoadNpyAutoAnnotations(context, snapshot.source.path, snapshot.collection.spectrum_count);
    }
    return context;
}

SampleCollectionIdentity BuildSampleCollectionIdentity(const SpectrumSnapshot& snapshot)
{
    SampleCollectionIdentity identity;
    identity.source_name = FileNameToUtf8(snapshot.source.path.filename());
    identity.spectrum_count = snapshot.collection.spectrum_count;

    std::error_code directory_error;
    const bool is_directory = std::filesystem::is_directory(snapshot.source.path, directory_error);
    if (!directory_error && is_directory) {
        identity.source_name = FileNameToUtf8(snapshot.source.path.filename());
        identity.source_fingerprint = FolderSourceFingerprint(snapshot.source.path);
        identity.context_fingerprint = identity.source_fingerprint;
    } else if (ExtensionLower(snapshot.source.path) == ".npy") {
        identity.source_fingerprint = NpySourceFingerprint(snapshot);
        identity.context_fingerprint = identity.source_fingerprint +
                                       "|name=" + OptionalFileFingerprint(CompanionNamePath(snapshot.source.path)) +
                                       "|annotation=" + OptionalFileFingerprint(CompanionAnnotationPath(snapshot.source.path));
    } else {
        identity.source_fingerprint = FileStatFingerprint(snapshot.source.path);
        identity.context_fingerprint = identity.source_fingerprint;
    }

    identity.id = "name=" + identity.source_name + "|fingerprint=" + identity.source_fingerprint +
                  "|count=" + std::to_string(identity.spectrum_count);
    return identity;
}

bool IsSampleCollectionAuxiliaryNpyArrayName(const std::filesystem::path& source_path)
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

std::optional<std::filesystem::path> SampleCollectionCompanionNamePath(const std::filesystem::path& source_path)
{
    return CompanionNamePath(source_path);
}

std::string_view SampleAnnotationKindLabel(SampleAnnotationKind kind)
{
    switch (kind) {
    case SampleAnnotationKind::CategoricalInteger:
        return "categorical";
    case SampleAnnotationKind::Text:
        return "text";
    case SampleAnnotationKind::ContinuousFloat:
        return "continuous";
    default:
        return "unknown";
    }
}

}  // namespace specforge
