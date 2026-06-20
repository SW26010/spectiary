#include "domain/fits_file_reader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <zlib.h>

namespace specforge::detail {
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

std::string UpperAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return value;
}

std::string NormalizedColumnName(std::string value)
{
    value = UpperAscii(TrimAscii(std::move(value)));
    std::string normalized;
    normalized.reserve(value.size());
    for (const char character : value) {
        if (std::isalnum(static_cast<unsigned char>(character)) != 0) {
            normalized.push_back(character);
        }
    }
    return normalized;
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

std::optional<double> ParseDouble(std::string text)
{
    text = TrimAscii(std::move(text));
    if (text.empty()) {
        return std::nullopt;
    }
    std::replace(text.begin(), text.end(), 'D', 'E');
    std::replace(text.begin(), text.end(), 'd', 'e');

    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || end == nullptr || *end != '\0') {
        return std::nullopt;
    }
    return value;
}

std::optional<std::int64_t> ParseInteger(std::string text)
{
    text = TrimAscii(std::move(text));
    if (text.empty()) {
        return std::nullopt;
    }

    std::size_t parsed_offset = 0;
    try {
        const long long value = std::stoll(text, &parsed_offset, 10);
        if (parsed_offset != text.size()) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(value);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string ParseFitsCardValue(std::string_view card)
{
    if (card.size() < 10 || card[8] != '=') {
        return {};
    }

    const std::string_view field = card.substr(10);
    std::string raw;
    bool in_quote = false;
    for (std::size_t index = 0; index < field.size(); ++index) {
        const char character = field[index];
        if (character == '\'') {
            raw.push_back(character);
            if (in_quote && index + 1 < field.size() && field[index + 1] == '\'') {
                raw.push_back(field[index + 1]);
                ++index;
            } else {
                in_quote = !in_quote;
            }
        } else if (character == '/' && !in_quote) {
            break;
        } else {
            raw.push_back(character);
        }
    }

    raw = TrimAscii(std::move(raw));
    if (raw.size() >= 2 && raw.front() == '\'' && raw.back() == '\'') {
        std::string text;
        for (std::size_t index = 1; index + 1 < raw.size(); ++index) {
            if (raw[index] == '\'' && index + 1 < raw.size() - 1 && raw[index + 1] == '\'') {
                text.push_back('\'');
                ++index;
            } else {
                text.push_back(raw[index]);
            }
        }
        return TrimAscii(std::move(text));
    }
    return raw;
}

std::size_t RoundUpFitsBlock(std::size_t value)
{
    constexpr std::size_t kFitsBlockSize = 2880;
    return ((value + kFitsBlockSize - 1U) / kFitsBlockSize) * kFitsBlockSize;
}

std::size_t FitsBitpixElementSize(std::int64_t bitpix)
{
    switch (bitpix) {
    case 8:
        return 1;
    case 16:
        return 2;
    case 32:
    case -32:
        return 4;
    case 64:
    case -64:
        return 8;
    default:
        return 0;
    }
}

std::size_t FitsTableRowDataSize(const FitsHeader& header)
{
    const std::int64_t row_width = FitsInteger(header, "NAXIS1");
    const std::int64_t row_count = FitsInteger(header, "NAXIS2");
    if (row_width < 0 || row_count < 0) {
        throw FitsFileError(FitsFileErrorCode::InvalidShape, "FITS binary table has invalid dimensions.");
    }
    const auto width = static_cast<std::size_t>(row_width);
    const auto count = static_cast<std::size_t>(row_count);
    if (width != 0 && count > std::numeric_limits<std::size_t>::max() / width) {
        throw FitsFileError(FitsFileErrorCode::InvalidShape, "FITS binary table dimensions overflow.");
    }
    return width * count;
}

std::size_t FitsDataSize(const FitsHeader& header)
{
    const std::string xtension = UpperAscii(FitsValue(header, "XTENSION").value_or({}));
    if (xtension == "BINTABLE") {
        const std::int64_t heap_bytes = FitsInteger(header, "PCOUNT");
        if (heap_bytes < 0) {
            throw FitsFileError(FitsFileErrorCode::InvalidShape, "FITS binary table has invalid dimensions.");
        }
        const auto heap = static_cast<std::size_t>(heap_bytes);
        const std::size_t table_bytes = FitsTableRowDataSize(header);
        if (heap > std::numeric_limits<std::size_t>::max() - table_bytes) {
            throw FitsFileError(FitsFileErrorCode::InvalidShape, "FITS binary table heap size overflows.");
        }
        return table_bytes + heap;
    }

    const std::int64_t axis_count = FitsInteger(header, "NAXIS");
    if (axis_count <= 0) {
        return 0;
    }
    const std::size_t element_size = FitsBitpixElementSize(FitsInteger(header, "BITPIX"));
    if (element_size == 0) {
        throw FitsFileError(FitsFileErrorCode::UnsupportedFormat, "FITS image BITPIX is not supported.");
    }

    std::size_t element_count = 1;
    for (std::int64_t axis = 1; axis <= axis_count; ++axis) {
        const std::int64_t axis_size = FitsInteger(header, "NAXIS" + std::to_string(axis));
        if (axis_size < 0) {
            throw FitsFileError(FitsFileErrorCode::InvalidShape, "FITS image has invalid axis dimensions.");
        }
        if (axis_size == 0) {
            return 0;
        }
        if (element_count > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(axis_size)) {
            throw FitsFileError(FitsFileErrorCode::InvalidShape, "FITS image dimensions overflow.");
        }
        element_count *= static_cast<std::size_t>(axis_size);
    }
    if (element_count > std::numeric_limits<std::size_t>::max() / element_size) {
        throw FitsFileError(FitsFileErrorCode::InvalidShape, "FITS image byte size overflows.");
    }
    return element_count * element_size;
}

std::optional<FitsColumn> ParseFitsColumn(const FitsHeader& header, std::size_t column_index, std::size_t byte_offset)
{
    const std::optional<std::string> name = FitsValue(header, "TTYPE" + std::to_string(column_index));
    const std::optional<std::string> form = FitsValue(header, "TFORM" + std::to_string(column_index));
    if (!name || !form) {
        return std::nullopt;
    }

    const std::string tform = TrimAscii(*form);
    std::size_t cursor = 0;
    while (cursor < tform.size() && std::isdigit(static_cast<unsigned char>(tform[cursor])) != 0) {
        ++cursor;
    }
    const std::size_t repeat =
        cursor == 0 ? 1U : ParsePositiveSize(std::string_view(tform).substr(0, cursor)).value_or(0);
    if (repeat == 0 || cursor >= tform.size()) {
        return std::nullopt;
    }

    const char code = static_cast<char>(std::toupper(static_cast<unsigned char>(tform[cursor])));
    std::size_t element_size = 0;
    switch (code) {
    case 'A':
    case 'B':
    case 'L':
        element_size = 1;
        break;
    case 'I':
        element_size = 2;
        break;
    case 'J':
    case 'E':
        element_size = 4;
        break;
    case 'K':
    case 'D':
        element_size = 8;
        break;
    default:
        return std::nullopt;
    }

    if (repeat > std::numeric_limits<std::size_t>::max() / element_size) {
        throw FitsFileError(FitsFileErrorCode::InvalidShape, "FITS table column width overflows.");
    }

    return FitsColumn{
        TrimAscii(*name),
        NormalizedColumnName(*name),
        repeat,
        code,
        element_size,
        byte_offset,
        repeat * element_size,
    };
}

void PopulateFitsColumns(FitsHdu& hdu)
{
    if (UpperAscii(FitsValue(hdu.header, "XTENSION").value_or({})) != "BINTABLE") {
        return;
    }

    const std::int64_t field_count = FitsInteger(hdu.header, "TFIELDS");
    if (field_count <= 0) {
        return;
    }
    const std::int64_t row_width_value = FitsInteger(hdu.header, "NAXIS1");
    if (row_width_value < 0) {
        throw FitsFileError(FitsFileErrorCode::InvalidShape, "FITS binary table has invalid dimensions.");
    }
    const auto row_width = static_cast<std::size_t>(row_width_value);

    std::size_t offset = 0;
    std::vector<FitsColumn> columns;
    columns.reserve(static_cast<std::size_t>(field_count));
    for (std::int64_t index = 1; index <= field_count; ++index) {
        const std::optional<FitsColumn> column = ParseFitsColumn(hdu.header, static_cast<std::size_t>(index), offset);
        if (!column) {
            hdu.columns.clear();
            return;
        }
        if (column->byte_width > row_width || offset > row_width - column->byte_width) {
            throw FitsFileError(FitsFileErrorCode::InvalidShape, "FITS table column layout exceeds NAXIS1 row width.");
        }
        offset += column->byte_width;
        columns.push_back(*column);
    }
    if (offset != row_width) {
        throw FitsFileError(FitsFileErrorCode::InvalidShape, "FITS table column layout does not match NAXIS1 row width.");
    }
    hdu.columns = std::move(columns);
}

std::uint16_t ReadBigEndianU16(const unsigned char* bytes)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) | bytes[1]);
}

std::uint32_t ReadBigEndianU32(const unsigned char* bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) | (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
}

std::uint64_t ReadBigEndianU64(const unsigned char* bytes)
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8U) | static_cast<std::uint64_t>(bytes[index]);
    }
    return value;
}

double ReadFitsNumericValue(const unsigned char* bytes, char code)
{
    switch (code) {
    case 'B':
        return static_cast<double>(bytes[0]);
    case 'I':
        return static_cast<double>(static_cast<std::int16_t>(ReadBigEndianU16(bytes)));
    case 'J':
        return static_cast<double>(static_cast<std::int32_t>(ReadBigEndianU32(bytes)));
    case 'K':
        return static_cast<double>(static_cast<std::int64_t>(ReadBigEndianU64(bytes)));
    case 'E': {
        const std::uint32_t bits = ReadBigEndianU32(bytes);
        float value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return static_cast<double>(value);
    }
    case 'D': {
        const std::uint64_t bits = ReadBigEndianU64(bytes);
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    default:
        throw FitsFileError(FitsFileErrorCode::UnsupportedFormat, "FITS numeric column type is not supported.");
    }
}

}  // namespace

FitsFileError::FitsFileError(FitsFileErrorCode code, std::string message)
    : std::runtime_error(std::move(message))
    , code_(code)
{
}

FitsFileErrorCode FitsFileError::code() const noexcept
{
    return code_;
}

std::vector<unsigned char> ReadFitsFileBytes(const std::filesystem::path& path, std::uintmax_t max_bytes)
{
    std::error_code size_error;
    const std::uintmax_t file_size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        throw FitsFileError(FitsFileErrorCode::OpenFailed, "Could not inspect the FITS file size.");
    }
    if (file_size > max_bytes) {
        throw FitsFileError(
            FitsFileErrorCode::UnsupportedFormat,
            "FITS file is too large for the synchronous single-spectrum loader.");
    }
    if (file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        throw FitsFileError(FitsFileErrorCode::UnsupportedFormat, "FITS file size is not addressable.");
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw FitsFileError(FitsFileErrorCode::OpenFailed, "Could not open the FITS file.");
    }

    std::vector<unsigned char> bytes(static_cast<std::size_t>(file_size));
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream && !bytes.empty()) {
        throw FitsFileError(FitsFileErrorCode::OpenFailed, "Could not read the complete FITS file.");
    }
    return bytes;
}

std::vector<unsigned char> DecompressGzipFitsBytes(
    const std::vector<unsigned char>& compressed,
    std::size_t max_inflated_bytes)
{
    if (compressed.empty() || compressed.size() > std::numeric_limits<uInt>::max()) {
        throw FitsFileError(FitsFileErrorCode::OpenFailed, "Compressed FITS file is empty or too large.");
    }

    z_stream stream = {};
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(compressed.data()));
    stream.avail_in = static_cast<uInt>(compressed.size());
    if (inflateInit2(&stream, MAX_WBITS + 16) != Z_OK) {
        throw FitsFileError(FitsFileErrorCode::OpenFailed, "Could not initialize gzip decompression.");
    }

    std::vector<unsigned char> output;
    std::array<unsigned char, 64 * 1024> buffer = {};
    int result = Z_OK;
    while (result != Z_STREAM_END) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());
        result = inflate(&stream, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END) {
            inflateEnd(&stream);
            throw FitsFileError(FitsFileErrorCode::OpenFailed, "Could not decompress the gzip FITS file.");
        }

        const std::size_t produced = buffer.size() - stream.avail_out;
        if (produced > max_inflated_bytes || output.size() > max_inflated_bytes - produced) {
            inflateEnd(&stream);
            throw FitsFileError(
                FitsFileErrorCode::UnsupportedFormat,
                "Gzipped FITS expands beyond the synchronous single-spectrum loader limit.");
        }
        output.insert(output.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(produced));
    }

    inflateEnd(&stream);
    return output;
}

std::optional<std::string> FitsValue(const FitsHeader& header, std::string_view key)
{
    const auto found = header.values.find(UpperAscii(std::string(key)));
    if (found == header.values.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::int64_t FitsInteger(const FitsHeader& header, std::string_view key, std::int64_t default_value)
{
    const std::optional<std::string> value = FitsValue(header, key);
    if (!value) {
        return default_value;
    }
    return ParseInteger(*value).value_or(default_value);
}

std::optional<double> FitsDouble(const FitsHeader& header, std::string_view key)
{
    const std::optional<std::string> value = FitsValue(header, key);
    if (!value) {
        return std::nullopt;
    }
    return ParseDouble(std::string(*value));
}

std::vector<FitsHdu> ParseFitsHdus(const std::vector<unsigned char>& bytes)
{
    constexpr std::size_t kFitsBlockSize = 2880;
    std::vector<FitsHdu> hdus;
    std::size_t offset = 0;

    while (offset + kFitsBlockSize <= bytes.size()) {
        FitsHdu hdu;
        hdu.index = hdus.size();
        std::size_t header_offset = offset;
        bool found_end = false;

        while (header_offset + kFitsBlockSize <= bytes.size()) {
            for (std::size_t card_offset = 0; card_offset < kFitsBlockSize; card_offset += 80U) {
                const std::size_t absolute = header_offset + card_offset;
                const std::string card(
                    reinterpret_cast<const char*>(bytes.data() + absolute),
                    reinterpret_cast<const char*>(bytes.data() + absolute + 80U));
                const std::string key = TrimAscii(card.substr(0, 8));
                if (key == "END") {
                    found_end = true;
                    break;
                }
                if (card.size() >= 10 && card[8] == '=') {
                    hdu.header.values[UpperAscii(key)] = ParseFitsCardValue(card);
                }
            }
            header_offset += kFitsBlockSize;
            if (found_end) {
                break;
            }
        }

        if (!found_end) {
            throw FitsFileError(FitsFileErrorCode::InvalidShape, "FITS header is missing END.");
        }

        hdu.data_offset = header_offset;
        hdu.data_size = FitsDataSize(hdu.header);
        if (hdu.data_offset > bytes.size() || hdu.data_size > bytes.size() - hdu.data_offset) {
            throw FitsFileError(FitsFileErrorCode::InvalidShape, "FITS HDU data is truncated.");
        }
        PopulateFitsColumns(hdu);
        hdus.push_back(std::move(hdu));

        offset = header_offset + RoundUpFitsBlock(hdus.back().data_size);
    }

    if (hdus.empty()) {
        throw FitsFileError(FitsFileErrorCode::UnsupportedFormat, "FITS file has no readable HDUs.");
    }
    return hdus;
}

std::vector<double> ReadFitsColumnVector(
    const std::vector<unsigned char>& bytes,
    const FitsHdu& hdu,
    const FitsColumn& column,
    std::size_t row_index,
    bool scalar_rows)
{
    const std::size_t row_width = static_cast<std::size_t>(FitsInteger(hdu.header, "NAXIS1"));
    const std::size_t row_count = static_cast<std::size_t>(FitsInteger(hdu.header, "NAXIS2"));
    const std::size_t table_data_size = FitsTableRowDataSize(hdu.header);
    if (table_data_size > hdu.data_size) {
        throw FitsFileError(FitsFileErrorCode::InvalidShape, "FITS table row data exceeds the HDU data range.");
    }
    std::vector<double> values;

    if (scalar_rows) {
        values.reserve(row_count);
        for (std::size_t row = 0; row < row_count; ++row) {
            const std::size_t relative_offset = row * row_width + column.byte_offset;
            if (relative_offset > table_data_size || column.element_size > table_data_size - relative_offset) {
                throw FitsFileError(FitsFileErrorCode::InvalidShape, "FITS table column data is truncated.");
            }
            const std::size_t offset = hdu.data_offset + relative_offset;
            values.push_back(ReadFitsNumericValue(bytes.data() + offset, column.code));
        }
        return values;
    }

    if (row_index >= row_count) {
        throw FitsFileError(FitsFileErrorCode::InvalidShape, "Requested FITS table row is out of range.");
    }
    values.reserve(column.repeat);
    const std::size_t row_offset = row_index * row_width + column.byte_offset;
    for (std::size_t item = 0; item < column.repeat; ++item) {
        const std::size_t relative_offset = row_offset + item * column.element_size;
        if (relative_offset > table_data_size || column.element_size > table_data_size - relative_offset) {
            throw FitsFileError(FitsFileErrorCode::InvalidShape, "FITS table vector data is truncated.");
        }
        const std::size_t offset = hdu.data_offset + relative_offset;
        values.push_back(ReadFitsNumericValue(bytes.data() + offset, column.code));
    }
    return values;
}

std::vector<double> ReadFitsImageRow(
    const std::vector<unsigned char>& bytes,
    const FitsHdu& hdu,
    std::size_t row_index,
    std::size_t column_count)
{
    const std::int64_t bitpix = FitsInteger(hdu.header, "BITPIX");
    const std::size_t element_size = FitsBitpixElementSize(bitpix);
    if ((bitpix != -32 && bitpix != -64) || element_size == 0) {
        throw FitsFileError(FitsFileErrorCode::UnsupportedFormat, "Only float32/float64 FITS image spectra are supported.");
    }

    std::vector<double> values;
    values.reserve(column_count);
    const std::size_t row_offset = hdu.data_offset + row_index * column_count * element_size;
    for (std::size_t column = 0; column < column_count; ++column) {
        const std::size_t offset = row_offset + column * element_size;
        if (offset > bytes.size() || element_size > bytes.size() - offset) {
            throw FitsFileError(FitsFileErrorCode::InvalidShape, "FITS image row data is truncated.");
        }
        const unsigned char* value_bytes = bytes.data() + offset;
        values.push_back(ReadFitsNumericValue(value_bytes, bitpix == -32 ? 'E' : 'D'));
    }
    return values;
}

}  // namespace specforge::detail
