#include "domain/sample_annotation_io.h"

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
#include <regex>
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

enum class NpyScalarKind {
    SignedInteger,
    UnsignedInteger,
    Float,
    Bytes,
    Unicode,
};

struct NpyHeader {
    std::string descr;
    std::vector<std::size_t> shape;
    std::uint64_t data_offset = 0;
};

struct NpyScalarType {
    NpyScalarKind kind = NpyScalarKind::Float;
    std::size_t item_size = 0;
    std::size_t code_units = 0;
};

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

std::uint16_t ReadLittleEndianU16(const std::array<unsigned char, 2>& bytes)
{
    return static_cast<std::uint16_t>(bytes[0]) | (static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint32_t ReadLittleEndianU32(const std::array<unsigned char, 4>& bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::uint32_t ReadLittleEndianU32(const unsigned char* bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::optional<std::string> RegexCapture(const std::string& text, const std::regex& expression)
{
    std::smatch match;
    if (!std::regex_search(text, match, expression) || match.size() < 2) {
        return std::nullopt;
    }
    return match[1].str();
}

std::optional<std::size_t> ParsePositiveSize(std::string_view text)
{
    if (text.empty()) {
        return std::nullopt;
    }

    std::size_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        const std::size_t digit = static_cast<std::size_t>(character - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
            return std::nullopt;
        }
        value = value * 10U + digit;
    }
    return value == 0 ? std::nullopt : std::optional<std::size_t>{value};
}

std::optional<std::vector<std::size_t>> ParseShape(const std::string& shape_text)
{
    std::vector<std::size_t> shape;
    std::stringstream stream(shape_text);
    std::string token;

    while (std::getline(stream, token, ',')) {
        token = TrimAscii(token);
        if (token.empty()) {
            continue;
        }
        std::size_t parsed_offset = 0;
        unsigned long long value = 0;
        try {
            value = std::stoull(token, &parsed_offset, 10);
        } catch (const std::exception&) {
            return std::nullopt;
        }
        if (parsed_offset != token.size() || value > std::numeric_limits<std::size_t>::max()) {
            return std::nullopt;
        }
        shape.push_back(static_cast<std::size_t>(value));
    }

    if (shape.empty()) {
        return std::nullopt;
    }
    return shape;
}

NpyHeader ReadNpyHeader(std::ifstream& stream)
{
    std::array<unsigned char, 6> magic = {};
    stream.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
    constexpr std::array<unsigned char, 6> kExpectedMagic = {0x93, 'N', 'U', 'M', 'P', 'Y'};
    if (!stream || magic != kExpectedMagic) {
        throw NpyAnnotationError("file does not start with the NPY magic header");
    }

    std::array<unsigned char, 2> version = {};
    stream.read(reinterpret_cast<char*>(version.data()), static_cast<std::streamsize>(version.size()));
    if (!stream) {
        throw NpyAnnotationError("NPY version header is truncated");
    }

    std::uint32_t header_length = 0;
    std::uint64_t data_offset = magic.size() + version.size();
    if (version[0] == 1) {
        std::array<unsigned char, 2> length_bytes = {};
        stream.read(reinterpret_cast<char*>(length_bytes.data()), static_cast<std::streamsize>(length_bytes.size()));
        if (!stream) {
            throw NpyAnnotationError("NPY v1 header length is truncated");
        }
        header_length = ReadLittleEndianU16(length_bytes);
        data_offset += length_bytes.size();
    } else if (version[0] == 2 || version[0] == 3) {
        std::array<unsigned char, 4> length_bytes = {};
        stream.read(reinterpret_cast<char*>(length_bytes.data()), static_cast<std::streamsize>(length_bytes.size()));
        if (!stream) {
            throw NpyAnnotationError("NPY v2/v3 header length is truncated");
        }
        header_length = ReadLittleEndianU32(length_bytes);
        data_offset += length_bytes.size();
    } else {
        throw NpyAnnotationError("unsupported NPY major version");
    }

    std::string header_text(header_length, '\0');
    stream.read(header_text.data(), static_cast<std::streamsize>(header_text.size()));
    if (!stream) {
        throw NpyAnnotationError("NPY header is truncated");
    }
    data_offset += header_length;

    static const std::regex kDescrExpression("'descr'\\s*:\\s*'([^']+)'");
    static const std::regex kFortranExpression("'fortran_order'\\s*:\\s*(True|False)");
    static const std::regex kShapeExpression("'shape'\\s*:\\s*\\(([^\\)]*)\\)");

    const std::optional<std::string> descr = RegexCapture(header_text, kDescrExpression);
    const std::optional<std::string> fortran_order = RegexCapture(header_text, kFortranExpression);
    const std::optional<std::string> shape_text = RegexCapture(header_text, kShapeExpression);
    if (!descr || !fortran_order || !shape_text) {
        throw NpyAnnotationError("NPY header is missing descr, fortran_order, or shape");
    }
    if (*fortran_order != "False") {
        throw NpyAnnotationError("Fortran-order NPY arrays are not supported");
    }

    const std::optional<std::vector<std::size_t>> shape = ParseShape(*shape_text);
    if (!shape) {
        throw NpyAnnotationError("NPY shape is invalid");
    }

    return NpyHeader{*descr, *shape, data_offset};
}

std::optional<NpyScalarType> ParseScalarType(std::string_view descr)
{
    if (descr.size() < 3) {
        return std::nullopt;
    }

    const char endian = descr[0];
    const char kind = descr[1];
    if (endian == '>') {
        return std::nullopt;
    }
    if (endian != '<' && endian != '=' && endian != '|') {
        return std::nullopt;
    }

    const std::optional<std::size_t> size = ParsePositiveSize(descr.substr(2));
    if (!size) {
        return std::nullopt;
    }

    if (kind == 'i' && (*size == 1 || *size == 2 || *size == 4 || *size == 8)) {
        return NpyScalarType{NpyScalarKind::SignedInteger, *size, 0};
    }
    if (kind == 'u' && (*size == 1 || *size == 2 || *size == 4 || *size == 8)) {
        return NpyScalarType{NpyScalarKind::UnsignedInteger, *size, 0};
    }
    if (kind == 'f' && (*size == 4 || *size == 8)) {
        return NpyScalarType{NpyScalarKind::Float, *size, 0};
    }
    if (kind == 'S' || kind == 'a') {
        return NpyScalarType{NpyScalarKind::Bytes, *size, *size};
    }
    if (kind == 'U') {
        if (*size > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t)) {
            return std::nullopt;
        }
        return NpyScalarType{NpyScalarKind::Unicode, *size * sizeof(std::uint32_t), *size};
    }
    return std::nullopt;
}

void ValidateNpyPayloadSize(
    const std::filesystem::path& path,
    const NpyHeader& header,
    std::size_t value_count,
    std::size_t item_size)
{
    if (value_count > std::numeric_limits<std::uint64_t>::max() / item_size) {
        throw NpyAnnotationError("NPY byte size overflows");
    }
    const std::uint64_t data_bytes = static_cast<std::uint64_t>(value_count) * item_size;
    if (header.data_offset > std::numeric_limits<std::uint64_t>::max() - data_bytes) {
        throw NpyAnnotationError("NPY byte size overflows");
    }

    std::error_code error;
    const std::uintmax_t file_size = std::filesystem::file_size(path, error);
    if (error) {
        throw NpyAnnotationError("could not inspect NPY file size");
    }
    if (file_size < header.data_offset + data_bytes) {
        throw NpyAnnotationError("NPY file is smaller than the declared array data");
    }
}

template <typename T>
std::vector<T> ReadTypedValues(std::ifstream& stream, std::size_t value_count)
{
    if (value_count > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()) / sizeof(T)) {
        throw NpyAnnotationError("NPY array is too large to read");
    }

    std::vector<T> values(value_count);
    stream.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(T)));
    if (!stream) {
        throw NpyAnnotationError("NPY array data is truncated");
    }
    return values;
}

void AppendUtf8(std::string& output, std::uint32_t code_point)
{
    if (code_point == 0) {
        return;
    }
    if (code_point <= 0x7fU) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (code_point >> 6U)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
    } else if (code_point <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (code_point >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
    } else if (code_point <= 0x10ffffU) {
        output.push_back(static_cast<char>(0xf0U | (code_point >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
    }
}

std::string DecodeNpyString(std::string_view bytes, const NpyScalarType& scalar_type)
{
    std::string decoded;
    if (scalar_type.kind == NpyScalarKind::Bytes) {
        decoded.assign(bytes.begin(), bytes.end());
    } else {
        decoded.reserve(scalar_type.code_units);
        for (std::size_t index = 0; index < scalar_type.code_units; ++index) {
            const std::size_t offset = index * sizeof(std::uint32_t);
            if (offset + sizeof(std::uint32_t) > bytes.size()) {
                break;
            }
            AppendUtf8(decoded, ReadLittleEndianU32(reinterpret_cast<const unsigned char*>(bytes.data() + offset)));
        }
    }

    while (!decoded.empty() && (decoded.back() == '\0' || std::isspace(static_cast<unsigned char>(decoded.back())) != 0)) {
        decoded.pop_back();
    }
    return decoded;
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
    const std::optional<NpyScalarType> scalar_type = ParseScalarType(header.descr);
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
    const std::vector<T> typed_values = ReadTypedValues<T>(stream, value_count);
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
    const std::vector<T> typed_values = ReadTypedValues<T>(stream, value_count);
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
    const std::optional<NpyScalarType> scalar_type = ParseScalarType(header.descr);
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
    identity.source_name = FileNameToUtf8(snapshot.source.path);
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
