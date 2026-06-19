#include "domain/npy_array_io.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <utility>

namespace specforge {
namespace {

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

std::optional<std::vector<std::size_t>> ParseShape(std::string shape_text)
{
    std::vector<std::size_t> shape;
    std::stringstream stream(std::move(shape_text));
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

std::string ShapeText(std::size_t value_count)
{
    return "(" + std::to_string(value_count) + ",)";
}

std::string MakeNpyHeader(std::string_view descr, std::size_t value_count)
{
    std::string header = "{'descr': '";
    header += descr;
    header += "', 'fortran_order': False, 'shape': ";
    header += ShapeText(value_count);
    header += ", }";

    constexpr std::size_t kPreambleSize = 10;
    const std::size_t header_with_newline = header.size() + 1;
    const std::size_t padding = (16 - ((kPreambleSize + header_with_newline) % 16)) % 16;
    header.append(padding, ' ');
    header.push_back('\n');
    if (header.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw NpyArrayError(NpyArrayErrorKind::InvalidShape, "NPY header is too large");
    }
    return header;
}

}  // namespace

NpyArrayError::NpyArrayError(NpyArrayErrorKind kind, std::string message)
    : std::runtime_error(std::move(message)), kind_(kind)
{
}

NpyArrayErrorKind NpyArrayError::kind() const noexcept
{
    return kind_;
}

NpyHeader ReadNpyHeader(std::istream& stream)
{
    std::array<unsigned char, 6> magic = {};
    stream.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
    constexpr std::array<unsigned char, 6> kExpectedMagic = {0x93, 'N', 'U', 'M', 'P', 'Y'};
    if (!stream || magic != kExpectedMagic) {
        throw NpyArrayError(NpyArrayErrorKind::UnsupportedFormat, "file does not start with the NPY magic header");
    }

    std::array<unsigned char, 2> version = {};
    stream.read(reinterpret_cast<char*>(version.data()), static_cast<std::streamsize>(version.size()));
    if (!stream) {
        throw NpyArrayError(NpyArrayErrorKind::InvalidShape, "NPY version header is truncated");
    }

    std::uint32_t header_length = 0;
    std::uint64_t data_offset = magic.size() + version.size();
    if (version[0] == 1) {
        std::array<unsigned char, 2> length_bytes = {};
        stream.read(reinterpret_cast<char*>(length_bytes.data()), static_cast<std::streamsize>(length_bytes.size()));
        if (!stream) {
            throw NpyArrayError(NpyArrayErrorKind::InvalidShape, "NPY v1 header length is truncated");
        }
        header_length = ReadLittleEndianU16(length_bytes);
        data_offset += length_bytes.size();
    } else if (version[0] == 2 || version[0] == 3) {
        std::array<unsigned char, 4> length_bytes = {};
        stream.read(reinterpret_cast<char*>(length_bytes.data()), static_cast<std::streamsize>(length_bytes.size()));
        if (!stream) {
            throw NpyArrayError(NpyArrayErrorKind::InvalidShape, "NPY v2/v3 header length is truncated");
        }
        header_length = ReadLittleEndianU32(length_bytes);
        data_offset += length_bytes.size();
    } else {
        throw NpyArrayError(NpyArrayErrorKind::UnsupportedFormat, "unsupported NPY major version");
    }

    std::string header_text(header_length, '\0');
    stream.read(header_text.data(), static_cast<std::streamsize>(header_text.size()));
    if (!stream) {
        throw NpyArrayError(NpyArrayErrorKind::InvalidShape, "NPY header is truncated");
    }
    data_offset += header_length;

    static const std::regex kDescrExpression("'descr'\\s*:\\s*'([^']+)'|\"descr\"\\s*:\\s*\"([^\"]+)\"");
    static const std::regex kFortranExpression("'fortran_order'\\s*:\\s*(True|False)|\"fortran_order\"\\s*:\\s*(true|false)");
    static const std::regex kShapeExpression("'shape'\\s*:\\s*\\(([^\\)]*)\\)|\"shape\"\\s*:\\s*\\[([^\\]]*)\\]");

    std::smatch descr_match;
    std::smatch fortran_match;
    std::smatch shape_match;
    if (!std::regex_search(header_text, descr_match, kDescrExpression) ||
        !std::regex_search(header_text, fortran_match, kFortranExpression) ||
        !std::regex_search(header_text, shape_match, kShapeExpression)) {
        throw NpyArrayError(NpyArrayErrorKind::InvalidShape, "NPY header is missing descr, fortran_order, or shape");
    }
    const std::string descr = descr_match[1].matched ? descr_match[1].str() : descr_match[2].str();
    const std::string fortran_order = fortran_match[1].matched ? fortran_match[1].str() : fortran_match[2].str();
    const std::string shape_text = shape_match[1].matched ? shape_match[1].str() : shape_match[2].str();

    if (fortran_order != "False" && fortran_order != "false") {
        throw NpyArrayError(NpyArrayErrorKind::UnsupportedFormat, "Fortran-order NPY arrays are not supported");
    }

    const std::optional<std::vector<std::size_t>> shape = ParseShape(shape_text);
    if (!shape) {
        throw NpyArrayError(NpyArrayErrorKind::InvalidShape, "NPY shape is invalid");
    }

    return NpyHeader{descr, *shape, data_offset};
}

std::optional<NpyScalarType> ParseNpyScalarType(std::string_view descr)
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
    if (item_size == 0 || value_count > std::numeric_limits<std::uint64_t>::max() / item_size) {
        throw NpyArrayError(NpyArrayErrorKind::InvalidShape, "NPY byte size overflows");
    }
    const std::uint64_t data_bytes = static_cast<std::uint64_t>(value_count) * item_size;
    if (header.data_offset > std::numeric_limits<std::uint64_t>::max() - data_bytes) {
        throw NpyArrayError(NpyArrayErrorKind::InvalidShape, "NPY byte size overflows");
    }

    std::error_code error;
    const std::uintmax_t file_size = std::filesystem::file_size(path, error);
    if (error) {
        throw NpyArrayError(NpyArrayErrorKind::OpenFailed, "could not inspect NPY file size");
    }
    if (file_size < header.data_offset + data_bytes) {
        throw NpyArrayError(NpyArrayErrorKind::InvalidShape, "NPY file is smaller than the declared array data");
    }
}

void SeekNpyData(std::istream& stream, const NpyHeader& header)
{
    if (header.data_offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw NpyArrayError(NpyArrayErrorKind::InvalidShape, "NPY data offset is too large");
    }
    stream.seekg(static_cast<std::streamoff>(header.data_offset), std::ios::beg);
    if (!stream) {
        throw NpyArrayError(NpyArrayErrorKind::OpenFailed, "could not seek to the NPY data");
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

void WriteNpyInt32Vector(std::ostream& stream, const std::vector<int>& values)
{
    const std::string header = MakeNpyHeader("<i4", values.size());
    constexpr std::array<unsigned char, 6> kMagic = {0x93, 'N', 'U', 'M', 'P', 'Y'};
    stream.write(reinterpret_cast<const char*>(kMagic.data()), static_cast<std::streamsize>(kMagic.size()));
    constexpr std::array<char, 2> kVersion = {1, 0};
    stream.write(kVersion.data(), static_cast<std::streamsize>(kVersion.size()));

    const auto header_length = static_cast<std::uint16_t>(header.size());
    const std::array<char, 2> length_bytes = {
        static_cast<char>(header_length & 0xffU),
        static_cast<char>((header_length >> 8U) & 0xffU),
    };
    stream.write(length_bytes.data(), static_cast<std::streamsize>(length_bytes.size()));
    stream.write(header.data(), static_cast<std::streamsize>(header.size()));

    for (const int value : values) {
        const auto int_value = static_cast<std::int32_t>(value);
        const auto raw_value = static_cast<std::uint32_t>(int_value);
        const std::array<unsigned char, 4> bytes = {
            static_cast<unsigned char>(raw_value & 0xffU),
            static_cast<unsigned char>((raw_value >> 8U) & 0xffU),
            static_cast<unsigned char>((raw_value >> 16U) & 0xffU),
            static_cast<unsigned char>((raw_value >> 24U) & 0xffU),
        };
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
}

}  // namespace specforge
