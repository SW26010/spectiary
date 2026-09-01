#include "native_codec.h"

#include <yaml-cpp/yaml.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace specforge::asdf_labeling_spike {
namespace {

constexpr std::string_view kFormatKind = "specforge.sample_labeling";
constexpr std::string_view kSchemaVersion = "2.0.0";
constexpr std::string_view kFixtureBuildSourceRevision =
    "0123456789abcdef0123456789abcdef01234567";
constexpr std::uint64_t kMaximumSampleCount = 100'000'000;
constexpr std::size_t kMaximumAuthorCount = 10'000;
constexpr std::array<unsigned char, 4> kBlockMagic{0xd3, 'B', 'L', 'K'};
constexpr std::array<unsigned char, 4> kZlibCompression{'z', 'l', 'i', 'b'};
constexpr int kZlibCompressionLevel = 6;

class CodecError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] std::vector<unsigned char> ReadAll(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw CodecError("could not open ASDF file");
    }
    const std::streampos end = stream.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw CodecError("ASDF file is too large");
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(end));
    stream.seekg(0);
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream) {
        throw CodecError("could not read ASDF file");
    }
    return bytes;
}

[[nodiscard]] bool HasPrefix(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::string_view value)
{
    return offset <= bytes.size() && value.size() <= bytes.size() - offset &&
        std::equal(value.begin(), value.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

[[nodiscard]] std::size_t FindTreeEnd(const std::vector<unsigned char>& bytes)
{
    if (!HasPrefix(bytes, 0, "#ASDF ")) {
        throw CodecError("ASDF magic header is missing");
    }
    const std::string_view view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::size_t marker = view.find("\n...\n");
    if (marker != std::string_view::npos) {
        return marker + 5;
    }
    marker = view.find("\r\n...\r\n");
    if (marker != std::string_view::npos) {
        return marker + 8;
    }
    throw CodecError("ASDF YAML document terminator is missing");
}

template <typename UInt>
[[nodiscard]] UInt ReadBigEndian(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    static_assert(std::is_unsigned_v<UInt>);
    if (offset > bytes.size() || sizeof(UInt) > bytes.size() - offset) {
        throw CodecError("ASDF block header is truncated");
    }
    UInt value = 0;
    for (std::size_t index = 0; index < sizeof(UInt); ++index) {
        value = static_cast<UInt>((value << 8U) | bytes[offset + index]);
    }
    return value;
}

void AppendBigEndian(std::vector<unsigned char>& bytes, std::uint64_t value, std::size_t width)
{
    for (std::size_t index = 0; index < width; ++index) {
        const std::size_t shift = 8 * (width - index - 1);
        bytes.push_back(static_cast<unsigned char>((value >> shift) & 0xffU));
    }
}

void AppendLittleEndian32(std::vector<unsigned char>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<unsigned char>(value & 0xffU));
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<unsigned char>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<unsigned char>((value >> 24U) & 0xffU));
}

void AppendInt32(
    std::vector<unsigned char>& bytes,
    std::int32_t signed_value,
    bool little_endian)
{
    const std::uint32_t value = static_cast<std::uint32_t>(signed_value);
    if (little_endian) {
        AppendLittleEndian32(bytes, value);
        return;
    }
    bytes.push_back(static_cast<unsigned char>((value >> 24U) & 0xffU));
    bytes.push_back(static_cast<unsigned char>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<unsigned char>(value & 0xffU));
}

[[nodiscard]] std::vector<unsigned char> CompressZlib(
    const std::vector<unsigned char>& payload)
{
    if (payload.size() > std::numeric_limits<uLong>::max()) {
        throw CodecError("ASDF block is too large for zlib");
    }
    const uLong source_size = static_cast<uLong>(payload.size());
    uLongf compressed_size = compressBound(source_size);
    std::vector<unsigned char> compressed(static_cast<std::size_t>(compressed_size));
    const int result = compress2(
        reinterpret_cast<Bytef*>(compressed.data()),
        &compressed_size,
        reinterpret_cast<const Bytef*>(payload.data()),
        source_size,
        kZlibCompressionLevel);
    if (result != Z_OK) {
        throw CodecError("zlib compression failed");
    }
    compressed.resize(static_cast<std::size_t>(compressed_size));
    return compressed;
}

void AppendCompressedBlock(
    std::vector<unsigned char>& output,
    const std::vector<unsigned char>& payload)
{
    const std::vector<unsigned char> compressed = CompressZlib(payload);
    output.insert(output.end(), kBlockMagic.begin(), kBlockMagic.end());
    AppendBigEndian(output, 48, 2);
    AppendBigEndian(output, 0, 4);  // flags
    output.insert(output.end(), kZlibCompression.begin(), kZlibCompression.end());
    AppendBigEndian(output, compressed.size(), 8);
    AppendBigEndian(output, compressed.size(), 8);
    AppendBigEndian(output, payload.size(), 8);
    output.insert(output.end(), 16, 0);  // optional checksum intentionally absent
    output.insert(output.end(), compressed.begin(), compressed.end());
}

enum class BlockCompression {
    None,
    Zlib,
};

struct Block {
    std::size_t raw_offset = 0;
    std::size_t raw_size = 0;
    std::size_t payload_offset = 0;
    std::size_t encoded_size = 0;
    std::size_t data_size = 0;
    BlockCompression compression = BlockCompression::None;
};

[[nodiscard]] std::vector<Block> ReadBlocks(
    const std::vector<unsigned char>& bytes,
    std::size_t offset)
{
    std::vector<Block> blocks;
    while (offset < bytes.size()) {
        while (offset < bytes.size() &&
               (bytes[offset] == 0 || std::isspace(static_cast<unsigned char>(bytes[offset])) != 0)) {
            ++offset;
        }
        if (offset == bytes.size() || HasPrefix(bytes, offset, "#ASDF BLOCK INDEX")) {
            break;
        }
        if (offset + kBlockMagic.size() > bytes.size() ||
            !std::equal(kBlockMagic.begin(), kBlockMagic.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset))) {
            throw CodecError("unexpected data between ASDF blocks");
        }
        if (offset > bytes.size() - 6) {
            throw CodecError("ASDF block prefix is truncated");
        }
        const std::uint16_t header_size = ReadBigEndian<std::uint16_t>(bytes, offset + 4);
        if (header_size < 48) {
            throw CodecError("ASDF block header is smaller than the standard header");
        }
        if (offset > bytes.size() - 6 || header_size > bytes.size() - offset - 6) {
            throw CodecError("ASDF block header exceeds the file");
        }
        const std::size_t fields = offset + 6;
        const std::uint32_t flags = ReadBigEndian<std::uint32_t>(bytes, fields);
        if ((flags & 1U) != 0U) {
            throw CodecError("streamed ASDF blocks are outside the native wire profile");
        }
        const auto compression_begin = bytes.begin() + static_cast<std::ptrdiff_t>(fields + 4);
        const auto compression_end = bytes.begin() + static_cast<std::ptrdiff_t>(fields + 8);
        const bool uncompressed = std::all_of(
            compression_begin,
            compression_end,
            [](unsigned char byte) { return byte == 0; });
        const bool zlib_compressed =
            std::equal(kZlibCompression.begin(), kZlibCompression.end(), compression_begin);
        if (!uncompressed && !zlib_compressed) {
            throw CodecError("unsupported ASDF block compression");
        }
        const std::uint64_t allocated = ReadBigEndian<std::uint64_t>(bytes, fields + 8);
        const std::uint64_t used = ReadBigEndian<std::uint64_t>(bytes, fields + 16);
        const std::uint64_t data = ReadBigEndian<std::uint64_t>(bytes, fields + 24);
        const bool has_checksum = std::any_of(
            bytes.begin() + static_cast<std::ptrdiff_t>(fields + 32),
            bytes.begin() + static_cast<std::ptrdiff_t>(fields + 48),
            [](unsigned char byte) { return byte != 0; });
        if (has_checksum) {
            throw CodecError("checksummed ASDF blocks are outside the native spike subset");
        }
        if (used > allocated || (uncompressed && data != used)) {
            throw CodecError("ASDF block sizes are inconsistent");
        }
        if (allocated > std::numeric_limits<std::size_t>::max() ||
            used > std::numeric_limits<std::size_t>::max() ||
            data > std::numeric_limits<std::size_t>::max()) {
            throw CodecError("ASDF block is too large for this process");
        }
        const std::size_t payload = offset + 6 + header_size;
        const std::size_t allocated_size = static_cast<std::size_t>(allocated);
        if (payload > bytes.size() || allocated_size > bytes.size() - payload) {
            throw CodecError("ASDF block payload is truncated");
        }
        const std::size_t raw_size = 6 + static_cast<std::size_t>(header_size) + allocated_size;
        blocks.push_back(Block{
            offset,
            raw_size,
            payload,
            static_cast<std::size_t>(used),
            static_cast<std::size_t>(data),
            zlib_compressed ? BlockCompression::Zlib : BlockCompression::None,
        });
        offset = payload + allocated_size;
    }
    return blocks;
}

[[nodiscard]] std::vector<unsigned char> DecodeBlockPayload(
    const std::vector<unsigned char>& bytes,
    const Block& block,
    std::size_t expected_size)
{
    if (block.data_size != expected_size) {
        throw CodecError("ASDF block decoded size does not match ndarray shape/dtype");
    }
    if (block.payload_offset > bytes.size() ||
        block.encoded_size > bytes.size() - block.payload_offset) {
        throw CodecError("ASDF block payload is truncated");
    }
    if (block.compression == BlockCompression::None) {
        return std::vector<unsigned char>(
            bytes.begin() + static_cast<std::ptrdiff_t>(block.payload_offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(block.payload_offset + block.encoded_size));
    }
    if (block.encoded_size > std::numeric_limits<uInt>::max() ||
        expected_size > std::numeric_limits<uInt>::max()) {
        throw CodecError("ASDF zlib block exceeds the bounded decoder size");
    }

    std::vector<unsigned char> decoded(expected_size);
    unsigned char empty_output = 0;
    z_stream state{};
    const int initialization = inflateInit(&state);
    if (initialization != Z_OK) {
        throw CodecError("zlib decoder initialization failed");
    }
    state.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(bytes.data() + block.payload_offset));
    state.avail_in = static_cast<uInt>(block.encoded_size);
    state.next_out = expected_size == 0 ? &empty_output : reinterpret_cast<Bytef*>(decoded.data());
    state.avail_out = static_cast<uInt>(expected_size == 0 ? 1 : expected_size);
    const int result = inflate(&state, Z_FINISH);
    const uLong total_in = state.total_in;
    const uLong total_out = state.total_out;
    inflateEnd(&state);
    if (result != Z_STREAM_END || total_in != block.encoded_size || total_out != expected_size) {
        throw CodecError("invalid or truncated ASDF zlib block");
    }
    return decoded;
}

[[nodiscard]] YAML::Node RequiredNode(const YAML::Node& parent, std::string_view key)
{
    if (!parent || !parent.IsMap()) {
        throw CodecError("required ASDF node is not a map");
    }
    const YAML::Node node = parent[std::string(key)];
    if (!node) {
        throw CodecError("required ASDF field is missing: " + std::string(key));
    }
    return node;
}

template <typename Value>
[[nodiscard]] Value RequiredScalar(const YAML::Node& parent, std::string_view key)
{
    const YAML::Node node = RequiredNode(parent, key);
    if (!node.IsScalar()) {
        throw CodecError("required ASDF field is not scalar: " + std::string(key));
    }
    try {
        return node.as<Value>();
    }
    catch (const YAML::Exception&) {
        throw CodecError("required ASDF field has the wrong scalar type: " + std::string(key));
    }
}

[[nodiscard]] bool IsValidUtf8(std::string_view text)
{
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        std::size_t continuation = 0;
        std::uint32_t codepoint = 0;
        if (lead <= 0x7f) {
            ++index;
            continue;
        }
        if ((lead & 0xe0U) == 0xc0U) {
            continuation = 1;
            codepoint = lead & 0x1fU;
        }
        else if ((lead & 0xf0U) == 0xe0U) {
            continuation = 2;
            codepoint = lead & 0x0fU;
        }
        else if ((lead & 0xf8U) == 0xf0U) {
            continuation = 3;
            codepoint = lead & 0x07U;
        }
        else {
            return false;
        }
        if (index + continuation >= text.size()) {
            return false;
        }
        for (std::size_t part = 0; part < continuation; ++part) {
            const unsigned char byte = static_cast<unsigned char>(text[index + part + 1]);
            if ((byte & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (byte & 0x3fU);
        }
        const std::uint32_t minimum = continuation == 1 ? 0x80U : continuation == 2 ? 0x800U : 0x10000U;
        if (codepoint < minimum || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
        index += continuation + 1;
    }
    return true;
}

[[nodiscard]] std::vector<std::uint32_t> DecodeUtf8Codepoints(std::string_view text)
{
    if (!IsValidUtf8(text)) {
        throw CodecError("invalid UTF-8 string");
    }
    std::vector<std::uint32_t> result;
    for (std::size_t index = 0; index < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        if (lead <= 0x7f) {
            result.push_back(lead);
            ++index;
            continue;
        }
        std::size_t continuation = 0;
        std::uint32_t codepoint = 0;
        if ((lead & 0xe0U) == 0xc0U) {
            continuation = 1;
            codepoint = lead & 0x1fU;
        }
        else if ((lead & 0xf0U) == 0xe0U) {
            continuation = 2;
            codepoint = lead & 0x0fU;
        }
        else {
            continuation = 3;
            codepoint = lead & 0x07U;
        }
        for (std::size_t part = 0; part < continuation; ++part) {
            codepoint = (codepoint << 6U) |
                (static_cast<unsigned char>(text[index + part + 1]) & 0x3fU);
        }
        result.push_back(codepoint);
        index += continuation + 1;
    }
    return result;
}

void AppendUtf8(std::string& output, std::uint32_t codepoint)
{
    if (codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
        throw CodecError("UCS-4 sample roster contains an invalid codepoint");
    }
    if (codepoint <= 0x7fU) {
        output.push_back(static_cast<char>(codepoint));
    }
    else if (codepoint <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
    else if (codepoint <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
    else {
        output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
}

void RequireUtf8(std::string_view value, std::string_view field)
{
    if (!IsValidUtf8(value)) {
        throw CodecError("ASDF field is not valid UTF-8: " + std::string(field));
    }
}

[[nodiscard]] bool IsUnicodeWhitespace(std::uint32_t codepoint)
{
    return (codepoint >= 0x0009U && codepoint <= 0x000dU) ||
        codepoint == 0x0020U || codepoint == 0x0085U || codepoint == 0x00a0U ||
        codepoint == 0x1680U || (codepoint >= 0x2000U && codepoint <= 0x200aU) ||
        codepoint == 0x2028U || codepoint == 0x2029U || codepoint == 0x202fU ||
        codepoint == 0x205fU || codepoint == 0x3000U;
}

void RequireNonWhitespaceUtf8(std::string_view value, std::string_view field)
{
    RequireUtf8(value, field);
    const std::vector<std::uint32_t> codepoints = DecodeUtf8Codepoints(value);
    if (std::ranges::all_of(codepoints, IsUnicodeWhitespace)) {
        throw CodecError("ASDF field contains only Unicode whitespace: " + std::string(field));
    }
}

[[nodiscard]] bool IsLowercaseToken(std::string_view value)
{
    return !value.empty() && value.front() >= 'a' && value.front() <= 'z' &&
        std::ranges::all_of(value, [](char character) {
            return (character >= 'a' && character <= 'z') ||
                (character >= '0' && character <= '9') || character == '_';
        });
}

[[nodiscard]] bool IsPortableAnnotationOriginName(std::string_view name)
{
    if (name == "." || name == ".." ||
        name.find_first_of("/\\") != std::string_view::npos) {
        return false;
    }
    const bool has_ascii_drive_prefix = name.size() >= 2U &&
        ((name.front() >= 'A' && name.front() <= 'Z') ||
         (name.front() >= 'a' && name.front() <= 'z')) &&
        name[1] == ':';
    return !has_ascii_drive_prefix;
}

[[nodiscard]] bool IsCanonicalUuidV4(std::string_view value)
{
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-' || value[14] != '4' ||
        (value[19] != '8' && value[19] != '9' && value[19] != 'a' && value[19] != 'b')) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            continue;
        }
        const char character = value[index];
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] unsigned ParseDecimal(std::string_view value, std::size_t offset, std::size_t count)
{
    unsigned result = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const char character = value[offset + index];
        if (character < '0' || character > '9') {
            return std::numeric_limits<unsigned>::max();
        }
        result = result * 10U + static_cast<unsigned>(character - '0');
    }
    return result;
}

[[nodiscard]] bool IsLeapYear(unsigned year)
{
    return year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U);
}

[[nodiscard]] bool IsCanonicalTimestamp(std::string_view value)
{
    if (value.size() != 24 || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
        value[19] != '.' || value[23] != 'Z') {
        return false;
    }
    const unsigned year = ParseDecimal(value, 0, 4);
    const unsigned month = ParseDecimal(value, 5, 2);
    const unsigned day = ParseDecimal(value, 8, 2);
    const unsigned hour = ParseDecimal(value, 11, 2);
    const unsigned minute = ParseDecimal(value, 14, 2);
    const unsigned second = ParseDecimal(value, 17, 2);
    const unsigned millisecond = ParseDecimal(value, 20, 3);
    if (year == std::numeric_limits<unsigned>::max() || month < 1U || month > 12U ||
        hour > 23U || minute > 59U || second > 59U || millisecond > 999U) {
        return false;
    }
    constexpr std::array<unsigned, 12> month_days{
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const unsigned maximum_day = month == 2U && IsLeapYear(year)
        ? 29U
        : month_days[month - 1U];
    return day >= 1U && day <= maximum_day;
}

[[nodiscard]] bool IsSha256Fingerprint(std::string_view value)
{
    constexpr std::string_view prefix = "sha256:";
    return value.size() == prefix.size() + 64U && value.starts_with(prefix) &&
        std::ranges::all_of(value.substr(prefix.size()), [](char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f');
        });
}

[[nodiscard]] std::int32_t DecodeInt32(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    bool little_endian)
{
    if (offset > bytes.size() || 4 > bytes.size() - offset) {
        throw CodecError("ASDF int32 payload is truncated");
    }
    std::uint32_t value = 0;
    if (little_endian) {
        value = static_cast<std::uint32_t>(bytes[offset]) |
            (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
            (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
            (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
    }
    else {
        value = (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
            (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
            (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
            static_cast<std::uint32_t>(bytes[offset + 3]);
    }
    return static_cast<std::int32_t>(value);
}

void ValidateDocument(const LabelingDocument& document)
{
    if (document.format_kind != kFormatKind || document.schema_version != kSchemaVersion) {
        throw CodecError("unsupported SpecForge labeling identity/version");
    }
    const bool valid_head_revision = document.build_source_revision &&
        document.build_source_revision->size() == 40U &&
        std::ranges::all_of(*document.build_source_revision, [](char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f');
        });
    if ((document.build_source_mode == "head" && !valid_head_revision) ||
        (document.build_source_mode == "working_tree" && document.build_source_revision) ||
        (document.build_source_mode != "head" &&
            document.build_source_mode != "working_tree")) {
        throw CodecError("invalid SpecForge build source identity");
    }
    if (document.sample_count > kMaximumSampleCount ||
        document.sample_count != document.values.size()) {
        throw CodecError("sample_count/value count invariant failed");
    }
    if (document.roster_identity_kind == "explicit_names") {
        if (document.sample_names.size() != document.values.size()) {
            throw CodecError("roster/value count invariant failed");
        }
        std::set<std::string> names;
        for (const std::string& name : document.sample_names) {
            RequireUtf8(name, "sample_roster.names");
            if (!names.insert(name).second) {
                throw CodecError("sample names must be unique");
            }
        }
    }
    else if (document.roster_identity_kind != "source_index" || !document.sample_names.empty()) {
        throw CodecError("unsupported roster identity kind");
    }
    if (document.annotation_kind != "categorical_integer" ||
        document.alignment_mode != "by_index" ||
        document.alignment_target != "sample_roster" ||
        document.missing_semantic != "unlabeled" || document.missing_value != kUnlabeled) {
        throw CodecError("unsupported annotation alignment or missing semantics");
    }
    std::set<std::int32_t> codes;
    std::set<std::string> shortcuts;
    for (const LabelDefinition& label : document.labels) {
        RequireNonWhitespaceUtf8(label.name, "label.name");
        if (label.code == kUnlabeled) {
            throw CodecError("label code collides with unlabeled");
        }
        if (!codes.insert(label.code).second) {
            throw CodecError("label codes must be unique");
        }
        if (!label.shortcut.empty() && !shortcuts.insert(label.shortcut).second) {
            throw CodecError("label shortcuts must be unique");
        }
    }
    for (const std::int32_t value : document.values) {
        if (value != kUnlabeled && !codes.contains(value)) {
            throw CodecError("categorical value has no label definition");
        }
    }
    RequireNonWhitespaceUtf8(document.source_kind, "source_kind");
    RequireNonWhitespaceUtf8(document.source_name, "source_name");
    RequireNonWhitespaceUtf8(document.source_identity, "source_identity");
    RequireNonWhitespaceUtf8(document.source_fingerprint, "source_fingerprint");
    if (!IsCanonicalUuidV4(document.task_id)) {
        throw CodecError("labeling_task.id is not a canonical UUID v4");
    }
    RequireNonWhitespaceUtf8(document.task_name, "task_name");
    if (!IsCanonicalTimestamp(document.created_at) ||
        !IsCanonicalTimestamp(document.modified_at) ||
        document.modified_at < document.created_at) {
        throw CodecError("labeling task timestamps are invalid");
    }
    if (!IsLowercaseToken(document.origin.kind)) {
        throw CodecError("labeling task origin kind is invalid");
    }
    if ((document.origin.kind == "manual" && document.origin.annotation) ||
        (document.origin.kind == "annotation_promotion" && !document.origin.annotation)) {
        throw CodecError("labeling task origin annotation is inconsistent");
    }
    if (document.origin.annotation) {
        const AnnotationOrigin& annotation = *document.origin.annotation;
        RequireNonWhitespaceUtf8(annotation.name, "labeling_task.origin.annotation.name");
        if (!IsPortableAnnotationOriginName(annotation.name)) {
            throw CodecError(
                "labeling task annotation origin name is not a portable basename");
        }
        if (annotation.format != "csv" && annotation.format != "npy") {
            throw CodecError("labeling task annotation origin format is invalid");
        }
        if (annotation.fingerprint && !IsSha256Fingerprint(*annotation.fingerprint)) {
            throw CodecError("labeling task annotation origin fingerprint is invalid");
        }
    }
    if (document.description) {
        RequireUtf8(*document.description, "labeling_task.description");
    }
    if (document.authors.size() > kMaximumAuthorCount) {
        throw CodecError("labeling task author count exceeds the native profile");
    }
    for (const Author& author : document.authors) {
        RequireNonWhitespaceUtf8(author.name, "labeling_task.authors.name");
        if (author.identifier) {
            RequireNonWhitespaceUtf8(*author.identifier, "labeling_task.authors.identifier");
        }
    }
}

[[nodiscard]] std::string QuoteJson(std::string_view text)
{
    std::ostringstream out;
    out << '"';
    for (const unsigned char byte : text) {
        switch (byte) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (byte < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(byte)
                    << std::dec << std::setfill(' ');
            }
            else {
                out << static_cast<char>(byte);
            }
        }
    }
    out << '"';
    return out.str();
}

[[nodiscard]] std::string QuoteYaml(std::string_view text)
{
    return QuoteJson(text);
}

void WriteStringArray(std::ostringstream& out, const std::vector<std::string>& values)
{
    out << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out << ',';
        }
        out << QuoteJson(values[index]);
    }
    out << ']';
}

}  // namespace

LabelingDocument ReadLabelingDocument(const std::filesystem::path& path)
{
    const std::vector<unsigned char> bytes = ReadAll(path);
    const std::size_t tree_end = FindTreeEnd(bytes);
    YAML::Node root;
    try {
        root = YAML::Load(std::string(reinterpret_cast<const char*>(bytes.data()), tree_end));
    }
    catch (const YAML::Exception& error) {
        throw CodecError(std::string("ASDF YAML parse failed: ") + error.what());
    }
    if (!root || !root.IsMap()) {
        throw CodecError("ASDF root is not a map");
    }
    const std::vector<Block> blocks = ReadBlocks(bytes, tree_end);

    LabelingDocument document;
    document.format_kind = RequiredScalar<std::string>(root, "format_kind");
    document.schema_version = RequiredScalar<std::string>(root, "schema_version");
    const YAML::Node build_source = RequiredNode(root, "specforge_build");
    if (!build_source.IsMap()) {
        throw CodecError("specforge_build must be a map");
    }
    document.build_source_mode =
        RequiredScalar<std::string>(build_source, "source_mode");
    if (const YAML::Node revision = build_source["source_revision"]) {
        if (!revision.IsScalar()) {
            throw CodecError("specforge_build.source_revision must be scalar");
        }
        document.build_source_revision = revision.as<std::string>();
    }
    const YAML::Node source = RequiredNode(root, "source_collection");
    document.source_kind = RequiredScalar<std::string>(source, "source_kind");
    document.source_name = RequiredScalar<std::string>(source, "name");
    document.source_identity = RequiredScalar<std::string>(source, "identity");
    document.source_fingerprint = RequiredScalar<std::string>(source, "fingerprint");
    document.sample_count = RequiredScalar<std::uint64_t>(source, "sample_count");

    const YAML::Node roster = RequiredNode(root, "sample_roster");
    document.roster_identity_kind = RequiredScalar<std::string>(roster, "identity_kind");
    if (document.roster_identity_kind == "explicit_names") {
        const YAML::Node names = RequiredNode(roster, "names");
        if (names.IsSequence()) {
            // Kept only to compare the alternative representation in #77.
            if (names.size() > kMaximumSampleCount) {
                throw CodecError("sample roster is too large");
            }
            document.sample_names.reserve(names.size());
            for (const YAML::Node& name : names) {
                if (!name.IsScalar()) {
                    throw CodecError("sample roster name is not scalar");
                }
                document.sample_names.push_back(name.as<std::string>());
            }
        }
        else if (names.IsMap() && names.Tag().find("ndarray-") != std::string::npos) {
            const YAML::Node datatype = RequiredNode(names, "datatype");
            if (!datatype.IsSequence() || datatype.size() != 2 ||
                datatype[0].as<std::string>() != "ucs4") {
                throw CodecError("sample roster string ndarray must use UCS-4");
            }
            const std::uint64_t width = datatype[1].as<std::uint64_t>();
            const YAML::Node shape = RequiredNode(names, "shape");
            if (!shape.IsSequence() || shape.size() != 1) {
                throw CodecError("sample roster string ndarray must be rank one");
            }
            const std::uint64_t count = shape[0].as<std::uint64_t>();
            if (count > kMaximumSampleCount || width == 0 || width > 1'000'000 ||
                count > std::numeric_limits<std::size_t>::max() / 4 / width) {
                throw CodecError("sample roster string ndarray shape is too large");
            }
            const std::string byteorder = RequiredScalar<std::string>(names, "byteorder");
            if (byteorder != "little" && byteorder != "big") {
                throw CodecError("sample roster string ndarray byteorder is invalid");
            }
            const std::uint64_t source_index = RequiredScalar<std::uint64_t>(names, "source");
            if (source_index >= blocks.size()) {
                throw CodecError("sample roster references a missing ASDF block");
            }
            const Block& block = blocks[static_cast<std::size_t>(source_index)];
            const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
            const std::vector<unsigned char> payload = DecodeBlockPayload(
                bytes,
                block,
                static_cast<std::size_t>(count) * row_bytes);
            document.sample_names.reserve(static_cast<std::size_t>(count));
            for (std::size_t row = 0; row < static_cast<std::size_t>(count); ++row) {
                std::string decoded;
                bool padding = false;
                for (std::size_t column = 0; column < static_cast<std::size_t>(width); ++column) {
                    const std::uint32_t codepoint = static_cast<std::uint32_t>(DecodeInt32(
                        payload,
                        row * row_bytes + column * 4,
                        byteorder == "little"));
                    if (codepoint == 0) {
                        padding = true;
                        continue;
                    }
                    if (padding) {
                        throw CodecError("sample roster UCS-4 padding is non-terminal");
                    }
                    AppendUtf8(decoded, codepoint);
                }
                document.sample_names.push_back(std::move(decoded));
            }
        }
        else {
            throw CodecError("sample roster names must be a string ndarray");
        }
    }

    const YAML::Node annotation = RequiredNode(root, "annotation");
    document.annotation_kind = RequiredScalar<std::string>(annotation, "kind");
    const YAML::Node alignment = RequiredNode(annotation, "alignment");
    if (!alignment.IsMap()) {
        throw CodecError("annotation alignment must be a map");
    }
    document.alignment_mode = RequiredScalar<std::string>(alignment, "mode");
    document.alignment_target = RequiredScalar<std::string>(alignment, "target");
    if (annotation["name"]) {
        throw CodecError("annotation.name is not part of schema 2.0");
    }
    const YAML::Node missing = RequiredNode(annotation, "missing");
    document.missing_semantic = RequiredScalar<std::string>(missing, "semantic");
    document.missing_value = RequiredScalar<std::int32_t>(missing, "value");
    const YAML::Node values = RequiredNode(annotation, "values");
    if (!values.IsMap() || values.Tag().find("ndarray-") == std::string::npos) {
        throw CodecError("annotation values are not an ASDF core ndarray");
    }
    if (RequiredScalar<std::string>(values, "datatype") != "int32") {
        throw CodecError("annotation values must use int32");
    }
    const std::string byteorder = RequiredScalar<std::string>(values, "byteorder");
    if (byteorder != "little" && byteorder != "big") {
        throw CodecError("annotation values byteorder is invalid");
    }
    const YAML::Node shape = RequiredNode(values, "shape");
    if (!shape.IsSequence() || shape.size() != 1) {
        throw CodecError("annotation values must be rank one");
    }
    const std::uint64_t value_count = shape[0].as<std::uint64_t>();
    if (value_count > kMaximumSampleCount || value_count > std::numeric_limits<std::size_t>::max() / 4) {
        throw CodecError("annotation values shape is too large");
    }
    const std::uint64_t source_index = RequiredScalar<std::uint64_t>(values, "source");
    if (source_index >= blocks.size()) {
        throw CodecError("annotation values reference a missing ASDF block");
    }
    const Block& block = blocks[static_cast<std::size_t>(source_index)];
    const std::size_t expected_bytes = static_cast<std::size_t>(value_count) * 4;
    const std::vector<unsigned char> payload = DecodeBlockPayload(bytes, block, expected_bytes);
    document.values.reserve(static_cast<std::size_t>(value_count));
    for (std::size_t index = 0; index < static_cast<std::size_t>(value_count); ++index) {
        document.values.push_back(DecodeInt32(payload, index * 4, byteorder == "little"));
    }

    const YAML::Node task = RequiredNode(root, "labeling_task");
    document.task_id = RequiredScalar<std::string>(task, "id");
    document.task_name = RequiredScalar<std::string>(task, "name");
    document.created_at = RequiredScalar<std::string>(task, "created_at");
    document.modified_at = RequiredScalar<std::string>(task, "modified_at");
    const YAML::Node origin = RequiredNode(task, "origin");
    document.origin.kind = RequiredScalar<std::string>(origin, "kind");
    if (origin["annotation"]) {
        const YAML::Node annotation_origin = RequiredNode(origin, "annotation");
        AnnotationOrigin parsed;
        parsed.name = RequiredScalar<std::string>(annotation_origin, "name");
        parsed.format = RequiredScalar<std::string>(annotation_origin, "format");
        if (annotation_origin["fingerprint"]) {
            parsed.fingerprint =
                RequiredScalar<std::string>(annotation_origin, "fingerprint");
        }
        document.origin.annotation = std::move(parsed);
    }
    if (task["description"]) {
        document.description = RequiredScalar<std::string>(task, "description");
    }
    if (const YAML::Node authors = task["authors"]) {
        if (!authors.IsSequence()) {
            throw CodecError("labeling task authors must be a sequence");
        }
        if (authors.size() > kMaximumAuthorCount) {
            throw CodecError("labeling task author count exceeds the native profile");
        }
        for (const YAML::Node& node : authors) {
            Author author;
            author.name = RequiredScalar<std::string>(node, "name");
            if (node["identifier"]) {
                author.identifier = RequiredScalar<std::string>(node, "identifier");
            }
            document.authors.push_back(std::move(author));
        }
    }
    const YAML::Node labels = RequiredNode(task, "labels");
    if (!labels.IsSequence()) {
        throw CodecError("label definitions must be a sequence");
    }
    for (const YAML::Node& node : labels) {
        LabelDefinition label;
        label.code = RequiredScalar<std::int32_t>(node, "code");
        label.name = RequiredScalar<std::string>(node, "name");
        if (node["shortcut"]) {
            label.shortcut = node["shortcut"].as<std::string>();
        }
        document.labels.push_back(std::move(label));
    }
    ValidateDocument(document);
    return document;
}

void RewriteLabelValuePreservingRosterBlock(
    const std::filesystem::path& input_path,
    const std::filesystem::path& output_path,
    std::size_t value_index,
    std::int32_t value)
{
    // Validate the complete schema-2 document before the experimental raw-block
    // rewrite. The tree prefix is copied byte-for-byte, so forward fields at all
    // preserved mapping levels retain their original representation.
    static_cast<void>(ReadLabelingDocument(input_path));
    const std::vector<unsigned char> bytes = ReadAll(input_path);
    const std::size_t tree_end = FindTreeEnd(bytes);
    YAML::Node root;
    try {
        root = YAML::Load(std::string(reinterpret_cast<const char*>(bytes.data()), tree_end));
    }
    catch (const YAML::Exception& error) {
        throw CodecError(std::string("ASDF YAML parse failed: ") + error.what());
    }
    if (!root || !root.IsMap() ||
        RequiredScalar<std::string>(root, "format_kind") != kFormatKind ||
        RequiredScalar<std::string>(root, "schema_version") != kSchemaVersion) {
        throw CodecError("unsupported SpecForge labeling identity/version");
    }

    const YAML::Node source = RequiredNode(root, "source_collection");
    const std::uint64_t sample_count = RequiredScalar<std::uint64_t>(source, "sample_count");
    if (sample_count > kMaximumSampleCount) {
        throw CodecError("sample count exceeds the native profile");
    }

    const YAML::Node roster = RequiredNode(root, "sample_roster");
    if (RequiredScalar<std::string>(roster, "identity_kind") != "explicit_names") {
        throw CodecError("roster-block reuse requires explicit sample names");
    }
    const YAML::Node names = RequiredNode(roster, "names");
    if (!names.IsMap() || names.Tag().find("ndarray-") == std::string::npos) {
        throw CodecError("sample roster names must be an ASDF ndarray");
    }
    const YAML::Node names_datatype = RequiredNode(names, "datatype");
    if (!names_datatype.IsSequence() || names_datatype.size() != 2 ||
        names_datatype[0].as<std::string>() != "ucs4") {
        throw CodecError("sample roster string ndarray must use UCS-4");
    }
    const std::uint64_t string_width = names_datatype[1].as<std::uint64_t>();
    const YAML::Node names_shape = RequiredNode(names, "shape");
    if (!names_shape.IsSequence() || names_shape.size() != 1) {
        throw CodecError("sample roster string ndarray must be rank one");
    }
    const std::uint64_t roster_count = names_shape[0].as<std::uint64_t>();
    if (roster_count != sample_count || string_width == 0 || string_width > 1'000'000 ||
        roster_count > std::numeric_limits<std::size_t>::max() / 4 / string_width) {
        throw CodecError("sample roster shape is invalid for block reuse");
    }
    const std::uint64_t roster_source = RequiredScalar<std::uint64_t>(names, "source");

    const YAML::Node annotation = RequiredNode(root, "annotation");
    if (RequiredScalar<std::string>(annotation, "kind") != "categorical_integer") {
        throw CodecError("unsupported annotation kind");
    }
    const YAML::Node missing = RequiredNode(annotation, "missing");
    if (RequiredScalar<std::string>(missing, "semantic") != "unlabeled" ||
        RequiredScalar<std::int32_t>(missing, "value") != kUnlabeled) {
        throw CodecError("unsupported missing semantics");
    }
    const YAML::Node values_node = RequiredNode(annotation, "values");
    if (!values_node.IsMap() || values_node.Tag().find("ndarray-") == std::string::npos ||
        RequiredScalar<std::string>(values_node, "datatype") != "int32") {
        throw CodecError("annotation values must be an int32 ASDF ndarray");
    }
    const std::string byteorder = RequiredScalar<std::string>(values_node, "byteorder");
    if (byteorder != "little" && byteorder != "big") {
        throw CodecError("annotation values byteorder is invalid");
    }
    const YAML::Node values_shape = RequiredNode(values_node, "shape");
    if (!values_shape.IsSequence() || values_shape.size() != 1) {
        throw CodecError("annotation values must be rank one");
    }
    const std::uint64_t value_count = values_shape[0].as<std::uint64_t>();
    if (value_count != sample_count || value_count > std::numeric_limits<std::size_t>::max() / 4) {
        throw CodecError("annotation values shape is invalid for block reuse");
    }
    if (value_index >= static_cast<std::size_t>(value_count)) {
        throw CodecError("label value index is out of range");
    }
    const std::uint64_t values_source = RequiredScalar<std::uint64_t>(values_node, "source");

    const std::vector<Block> blocks = ReadBlocks(bytes, tree_end);
    if (blocks.size() != 2 || roster_source != 0 || values_source != 1) {
        throw CodecError("block reuse requires the canonical roster/value block order");
    }
    const Block& roster_block = blocks[0];
    const Block& values_block = blocks[1];
    const std::size_t expected_roster_bytes =
        static_cast<std::size_t>(roster_count) * static_cast<std::size_t>(string_width) * 4;
    if (roster_block.data_size != expected_roster_bytes) {
        throw CodecError("sample roster block size does not match shape/dtype");
    }
    const std::size_t expected_values_bytes = static_cast<std::size_t>(value_count) * 4;
    const std::vector<unsigned char> values_payload =
        DecodeBlockPayload(bytes, values_block, expected_values_bytes);

    const YAML::Node task = RequiredNode(root, "labeling_task");
    const YAML::Node labels = RequiredNode(task, "labels");
    if (!labels.IsSequence()) {
        throw CodecError("label definitions must be a sequence");
    }
    std::set<std::int32_t> label_codes;
    for (const YAML::Node& label : labels) {
        const std::int32_t code = RequiredScalar<std::int32_t>(label, "code");
        if (code == kUnlabeled || !label_codes.insert(code).second) {
            throw CodecError("label definitions are invalid");
        }
    }

    std::vector<std::int32_t> decoded_values;
    decoded_values.reserve(static_cast<std::size_t>(value_count));
    for (std::size_t index = 0; index < static_cast<std::size_t>(value_count); ++index) {
        const std::int32_t decoded = DecodeInt32(values_payload, index * 4, byteorder == "little");
        if (decoded != kUnlabeled && !label_codes.contains(decoded)) {
            throw CodecError("categorical value has no label definition");
        }
        decoded_values.push_back(decoded);
    }
    if (value != kUnlabeled && !label_codes.contains(value)) {
        throw CodecError("replacement value has no label definition");
    }
    decoded_values[value_index] = value;

    std::vector<unsigned char> replacement_payload;
    replacement_payload.reserve(expected_values_bytes);
    for (const std::int32_t decoded : decoded_values) {
        AppendInt32(replacement_payload, decoded, byteorder == "little");
    }
    std::vector<unsigned char> replacement_block;
    AppendCompressedBlock(replacement_block, replacement_payload);

    const std::size_t roster_end = roster_block.raw_offset + roster_block.raw_size;
    if (roster_end > values_block.raw_offset || values_block.raw_offset > bytes.size()) {
        throw CodecError("ASDF block ranges are inconsistent");
    }
    std::ofstream stream(output_path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw CodecError("could not create rewritten ASDF file");
    }
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(roster_block.raw_offset));
    stream.write(
        reinterpret_cast<const char*>(bytes.data() + roster_block.raw_offset),
        static_cast<std::streamsize>(roster_block.raw_size));
    stream.write(
        reinterpret_cast<const char*>(bytes.data() + roster_end),
        static_cast<std::streamsize>(values_block.raw_offset - roster_end));
    stream.write(
        reinterpret_cast<const char*>(replacement_block.data()),
        static_cast<std::streamsize>(replacement_block.size()));
    stream.flush();
    if (!stream) {
        throw CodecError("could not complete rewritten ASDF file");
    }
}

void WriteLabelingDocument(const std::filesystem::path& path, const LabelingDocument& document)
{
    ValidateDocument(document);
    std::size_t string_width = 0;
    std::vector<std::vector<std::uint32_t>> encoded_names;
    if (document.roster_identity_kind == "explicit_names") {
        encoded_names.reserve(document.sample_names.size());
        for (const std::string& name : document.sample_names) {
            encoded_names.push_back(DecodeUtf8Codepoints(name));
            string_width = std::max(string_width, encoded_names.back().size());
        }
        string_width = std::max<std::size_t>(string_width, 1);
    }
    const std::size_t values_source = document.roster_identity_kind == "explicit_names" ? 1 : 0;
    std::ostringstream metadata;
    metadata << "#ASDF 1.0.0\n"
             << "#ASDF_STANDARD 1.5.0\n"
             << "%YAML 1.1\n"
             << "%TAG ! tag:stsci.edu:asdf/\n"
             << "--- !core/asdf-1.1.0\n"
             << "asdf_library: !core/software-1.0.0 {name: SpecForge, version: 0.8.0}\n"
             << "specforge_build:\n"
             << "  source_mode: " << QuoteYaml(document.build_source_mode) << "\n";
    if (document.build_source_revision) {
        metadata << "  source_revision: " << QuoteYaml(*document.build_source_revision) << "\n";
    }
    metadata
             << "format_kind: " << QuoteYaml(document.format_kind) << "\n"
             << "schema_version: " << QuoteYaml(document.schema_version) << "\n"
             << "source_collection:\n"
             << "  identity: " << QuoteYaml(document.source_identity) << "\n"
             << "  source_kind: " << QuoteYaml(document.source_kind) << "\n"
             << "  name: " << QuoteYaml(document.source_name) << "\n"
             << "  fingerprint: " << QuoteYaml(document.source_fingerprint) << "\n"
             << "  sample_count: " << document.sample_count << "\n"
             << "sample_roster:\n"
             << "  identity_kind: " << QuoteYaml(document.roster_identity_kind) << "\n";
    if (document.roster_identity_kind == "explicit_names") {
        metadata << "  names: !core/ndarray-1.0.0\n"
                 << "    source: 0\n"
                 << "    datatype: [ucs4, " << string_width << "]\n"
                 << "    byteorder: little\n"
                 << "    shape: [" << document.sample_names.size() << "]\n";
    }
    metadata << "annotation:\n"
             << "  kind: " << QuoteYaml(document.annotation_kind) << "\n"
             << "  alignment:\n"
             << "    mode: " << QuoteYaml(document.alignment_mode) << "\n"
             << "    target: " << QuoteYaml(document.alignment_target) << "\n"
             << "  values: !core/ndarray-1.0.0\n"
             << "    source: " << values_source << "\n"
             << "    datatype: int32\n"
             << "    byteorder: little\n"
             << "    shape: [" << document.values.size() << "]\n"
             << "  missing:\n"
             << "    semantic: " << QuoteYaml(document.missing_semantic) << "\n"
             << "    value: " << document.missing_value << "\n"
             << "labeling_task:\n"
             << "  id: " << QuoteYaml(document.task_id) << "\n"
             << "  name: " << QuoteYaml(document.task_name) << "\n"
             << "  created_at: " << QuoteYaml(document.created_at) << "\n"
             << "  modified_at: " << QuoteYaml(document.modified_at) << "\n"
             << "  origin:\n"
             << "    kind: " << QuoteYaml(document.origin.kind) << "\n";
    if (document.origin.annotation) {
        const AnnotationOrigin& annotation_origin = *document.origin.annotation;
        metadata << "    annotation:\n"
                 << "      name: " << QuoteYaml(annotation_origin.name) << "\n"
                 << "      format: " << QuoteYaml(annotation_origin.format) << "\n";
        if (annotation_origin.fingerprint) {
            metadata << "      fingerprint: "
                     << QuoteYaml(*annotation_origin.fingerprint) << "\n";
        }
    }
    if (document.description) {
        metadata << "  description: " << QuoteYaml(*document.description) << "\n";
    }
    if (!document.authors.empty()) {
        metadata << "  authors:\n";
        for (const Author& author : document.authors) {
            metadata << "  - name: " << QuoteYaml(author.name) << "\n";
            if (author.identifier) {
                metadata << "    identifier: " << QuoteYaml(*author.identifier) << "\n";
            }
        }
    }
    metadata << "  labels:\n";
    for (const LabelDefinition& label : document.labels) {
        metadata << "  - code: " << label.code << "\n"
                 << "    name: " << QuoteYaml(label.name) << "\n";
        if (!label.shortcut.empty()) {
            metadata << "    shortcut: " << QuoteYaml(label.shortcut) << "\n";
        }
    }
    metadata << "...\n";

    std::vector<unsigned char> blocks;
    if (document.roster_identity_kind == "explicit_names") {
        std::vector<unsigned char> names_payload;
        names_payload.reserve(document.sample_names.size() * string_width * 4);
        for (const std::vector<std::uint32_t>& name : encoded_names) {
            for (const std::uint32_t codepoint : name) {
                AppendLittleEndian32(names_payload, codepoint);
            }
            for (std::size_t padding = name.size(); padding < string_width; ++padding) {
                AppendLittleEndian32(names_payload, 0);
            }
        }
        AppendCompressedBlock(blocks, names_payload);
    }
    std::vector<unsigned char> values_payload;
    values_payload.reserve(document.values.size() * 4);
    for (const std::int32_t signed_value : document.values) {
        AppendLittleEndian32(values_payload, static_cast<std::uint32_t>(signed_value));
    }
    AppendCompressedBlock(blocks, values_payload);

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw CodecError("could not create ASDF file");
    }
    const std::string tree = metadata.str();
    stream.write(tree.data(), static_cast<std::streamsize>(tree.size()));
    stream.write(reinterpret_cast<const char*>(blocks.data()), static_cast<std::streamsize>(blocks.size()));
    stream.flush();
    if (!stream) {
        throw CodecError("could not complete ASDF file write");
    }
}

std::string SemanticJson(const LabelingDocument& document)
{
    std::ostringstream out;
    out << '{'
        << "\"format_kind\":" << QuoteJson(document.format_kind) << ','
        << "\"schema_version\":" << QuoteJson(document.schema_version) << ','
        << "\"build_source_mode\":" << QuoteJson(document.build_source_mode) << ','
        << "\"build_source_revision\":";
    if (document.build_source_revision) {
        out << QuoteJson(*document.build_source_revision);
    }
    else {
        out << "null";
    }
    out << ','
        << "\"source_kind\":" << QuoteJson(document.source_kind) << ','
        << "\"source_name\":" << QuoteJson(document.source_name) << ','
        << "\"source_identity\":" << QuoteJson(document.source_identity) << ','
        << "\"source_fingerprint\":" << QuoteJson(document.source_fingerprint) << ','
        << "\"sample_count\":" << document.sample_count << ','
        << "\"roster_identity_kind\":" << QuoteJson(document.roster_identity_kind) << ','
        << "\"sample_names\":";
    WriteStringArray(out, document.sample_names);
    out << ",\"annotation_kind\":" << QuoteJson(document.annotation_kind)
        << ",\"alignment_mode\":" << QuoteJson(document.alignment_mode)
        << ",\"alignment_target\":" << QuoteJson(document.alignment_target)
        << ",\"missing_semantic\":" << QuoteJson(document.missing_semantic)
        << ",\"missing_value\":" << document.missing_value
        << ",\"task_id\":" << QuoteJson(document.task_id)
        << ",\"task_name\":" << QuoteJson(document.task_name)
        << ",\"created_at\":" << QuoteJson(document.created_at)
        << ",\"modified_at\":" << QuoteJson(document.modified_at)
        << ",\"origin_kind\":" << QuoteJson(document.origin.kind)
        << ",\"origin_annotation\":";
    if (!document.origin.annotation) {
        out << "null";
    }
    else {
        const AnnotationOrigin& annotation_origin = *document.origin.annotation;
        out << "{\"name\":" << QuoteJson(annotation_origin.name)
            << ",\"format\":" << QuoteJson(annotation_origin.format)
            << ",\"fingerprint\":";
        if (annotation_origin.fingerprint) {
            out << QuoteJson(*annotation_origin.fingerprint);
        }
        else {
            out << "null";
        }
        out << '}';
    }
    out << ",\"description\":";
    if (document.description) {
        out << QuoteJson(*document.description);
    }
    else {
        out << "null";
    }
    out << ",\"authors\":[";
    for (std::size_t index = 0; index < document.authors.size(); ++index) {
        if (index != 0) {
            out << ',';
        }
        const Author& author = document.authors[index];
        out << "{\"name\":" << QuoteJson(author.name) << ",\"identifier\":";
        if (author.identifier) {
            out << QuoteJson(*author.identifier);
        }
        else {
            out << "null";
        }
        out << '}';
    }
    out << ']'
        << ",\"labels\":[";
    for (std::size_t index = 0; index < document.labels.size(); ++index) {
        if (index != 0) {
            out << ',';
        }
        const LabelDefinition& label = document.labels[index];
        out << "{\"code\":" << label.code << ",\"name\":" << QuoteJson(label.name)
            << ",\"shortcut\":" << QuoteJson(label.shortcut) << '}';
    }
    out << "],\"values\":[";
    for (std::size_t index = 0; index < document.values.size(); ++index) {
        if (index != 0) {
            out << ',';
        }
        out << document.values[index];
    }
    out << "],\"values_dtype\":\"int32\",\"values_shape\":[" << document.values.size() << "]}";
    return out.str();
}

LabelingDocument NativeFixture()
{
    LabelingDocument document;
    document.format_kind = std::string(kFormatKind);
    document.schema_version = std::string(kSchemaVersion);
    document.build_source_mode = "head";
    document.build_source_revision = std::string(kFixtureBuildSourceRevision);
    document.source_kind = "folder";
    document.source_name = "Native fixture";
    document.source_identity = "source:native-fixture";
    document.source_fingerprint = "sha256:native-fixture";
    document.sample_count = 3;
    document.roster_identity_kind = "explicit_names";
    document.sample_names = {"alpha.fits", "星系-β.fits", "gamma.fits"};
    document.annotation_kind = "categorical_integer";
    document.alignment_mode = "by_index";
    document.alignment_target = "sample_roster";
    document.missing_semantic = "unlabeled";
    document.missing_value = kUnlabeled;
    document.task_id = "00000000-0000-4000-8000-000000000002";
    document.task_name = "Native writer task";
    document.created_at = "2026-08-30T08:00:00.000Z";
    document.modified_at = "2026-08-30T08:00:00.000Z";
    document.origin.kind = "annotation_promotion";
    document.origin.annotation = AnnotationOrigin{
        "native-labels.csv",
        "csv",
        "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};
    document.description = "Native schema 2.0 interoperability fixture 🚀";
    document.authors = {
        {"SpecForge spike", "https://example.invalid/specforge-spike"},
        {"验证者", std::nullopt}};
    document.labels = {{0, "Galaxy", "g"}, {1, "Quasar", "q"}};
    document.values = {kUnlabeled, 0, 1};
    return document;
}

}  // namespace specforge::asdf_labeling_spike
