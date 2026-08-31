#pragma once

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace specforge::asdf_mutation_test {

enum class MutationCategory {
    YamlStructure,
    YamlScalar,
    BlockHeader,
    BlockIndex,
    Payload,
    UnknownMetadata,
};

enum class ExpectedInvariant {
    Controlled,
    MustReject,
    MustRejectResourceLimit,
    MustAcceptAndPreserveUnknown,
};

struct MutationRecipe {
    std::string_view name;
    MutationCategory category;
    ExpectedInvariant expected;
    std::size_t variant = 0;
};

inline constexpr std::array<MutationRecipe, 73> kMutationRecipes = {{
    {"yaml-map-to-scalar", MutationCategory::YamlStructure, ExpectedInvariant::MustReject, 0},
    {"yaml-map-to-sequence", MutationCategory::YamlStructure, ExpectedInvariant::MustReject, 1},
    {"yaml-map-to-null", MutationCategory::YamlStructure, ExpectedInvariant::MustReject, 2},
    {"yaml-duplicate-required-key", MutationCategory::YamlStructure, ExpectedInvariant::MustReject, 3},
    {"yaml-delete-required-key", MutationCategory::YamlStructure, ExpectedInvariant::MustReject, 4},
    {"yaml-unsupported-root-tag", MutationCategory::YamlStructure, ExpectedInvariant::MustReject, 5},
    {"yaml-wrong-ndarray-tag-version", MutationCategory::YamlStructure, ExpectedInvariant::MustReject, 6},
    {"yaml-bounded-deep-mapping", MutationCategory::YamlStructure, ExpectedInvariant::Controlled, 7},
    {"yaml-bounded-anchor-alias", MutationCategory::YamlStructure, ExpectedInvariant::Controlled, 8},
    {"yaml-remove-document-start", MutationCategory::YamlStructure, ExpectedInvariant::MustReject, 9},
    {"yaml-remove-document-end", MutationCategory::YamlStructure, ExpectedInvariant::MustReject, 10},
    {"yaml-prefix-bom", MutationCategory::YamlStructure, ExpectedInvariant::MustReject, 11},
    {"yaml-mixed-crlf-lf", MutationCategory::YamlStructure, ExpectedInvariant::Controlled, 12},
    {"yaml-embedded-nul", MutationCategory::YamlStructure, ExpectedInvariant::MustReject, 13},
    {"yaml-invalid-utf8", MutationCategory::YamlStructure, ExpectedInvariant::MustReject, 14},
    {"yaml-root-sequence-entry", MutationCategory::YamlStructure, ExpectedInvariant::MustReject, 15},

    {"scalar-sample-count-string", MutationCategory::YamlScalar, ExpectedInvariant::Controlled, 0},
    {"scalar-sample-count-bool", MutationCategory::YamlScalar, ExpectedInvariant::MustReject, 1},
    {"scalar-sample-count-null", MutationCategory::YamlScalar, ExpectedInvariant::MustReject, 2},
    {"scalar-sample-count-negative", MutationCategory::YamlScalar, ExpectedInvariant::MustReject, 3},
    {"scalar-format-kind-int", MutationCategory::YamlScalar, ExpectedInvariant::MustReject, 4},
    {"scalar-schema-version-bool", MutationCategory::YamlScalar, ExpectedInvariant::MustReject, 5},
    {"scalar-shape-string", MutationCategory::YamlScalar, ExpectedInvariant::MustReject, 6},
    {"scalar-block-source-bool", MutationCategory::YamlScalar, ExpectedInvariant::MustReject, 7},
    {"scalar-missing-value-string", MutationCategory::YamlScalar, ExpectedInvariant::Controlled, 8},
    {"scalar-label-code-null", MutationCategory::YamlScalar, ExpectedInvariant::MustReject, 9},

    {"header-magic", MutationCategory::BlockHeader, ExpectedInvariant::MustReject, 0},
    {"header-size-too-small", MutationCategory::BlockHeader, ExpectedInvariant::MustReject, 1},
    {"header-size-past-file", MutationCategory::BlockHeader, ExpectedInvariant::MustReject, 2},
    {"header-flags", MutationCategory::BlockHeader, ExpectedInvariant::MustReject, 3},
    {"header-compression-token", MutationCategory::BlockHeader, ExpectedInvariant::MustReject, 4},
    {"header-decoded-size-resource-limit", MutationCategory::BlockHeader, ExpectedInvariant::MustRejectResourceLimit, 5},
    {"header-allocated-less-than-used", MutationCategory::BlockHeader, ExpectedInvariant::MustReject, 6},
    {"header-used-allocated-resource-limit", MutationCategory::BlockHeader, ExpectedInvariant::MustRejectResourceLimit, 7},
    {"header-data-size-mismatch", MutationCategory::BlockHeader, ExpectedInvariant::MustReject, 8},
    {"header-checksum", MutationCategory::BlockHeader, ExpectedInvariant::MustReject, 9},
    {"header-extension-bytes", MutationCategory::BlockHeader, ExpectedInvariant::Controlled, 10},
    {"header-block-order", MutationCategory::BlockHeader, ExpectedInvariant::MustReject, 11},
    {"header-unreferenced-block", MutationCategory::BlockHeader, ExpectedInvariant::Controlled, 12},
    {"header-duplicate-block-source", MutationCategory::BlockHeader, ExpectedInvariant::MustReject, 13},

    {"index-missing", MutationCategory::BlockIndex, ExpectedInvariant::Controlled, 0},
    {"index-truncated", MutationCategory::BlockIndex, ExpectedInvariant::MustReject, 1},
    {"index-nonnumeric", MutationCategory::BlockIndex, ExpectedInvariant::MustReject, 2},
    {"index-negative", MutationCategory::BlockIndex, ExpectedInvariant::MustReject, 3},
    {"index-decreasing", MutationCategory::BlockIndex, ExpectedInvariant::MustReject, 4},
    {"index-duplicate", MutationCategory::BlockIndex, ExpectedInvariant::MustReject, 5},
    {"index-points-to-nonmagic", MutationCategory::BlockIndex, ExpectedInvariant::MustReject, 6},
    {"index-first-offset-mismatch", MutationCategory::BlockIndex, ExpectedInvariant::MustReject, 7},
    {"index-last-block-discontinuity", MutationCategory::BlockIndex, ExpectedInvariant::MustReject, 8},
    {"index-trailing-garbage", MutationCategory::BlockIndex, ExpectedInvariant::MustReject, 9},
    {"index-offset-overflow", MutationCategory::BlockIndex, ExpectedInvariant::MustReject, 10},

    {"payload-truncate-q1", MutationCategory::Payload, ExpectedInvariant::MustReject, 0},
    {"payload-truncate-q2", MutationCategory::Payload, ExpectedInvariant::MustReject, 1},
    {"payload-truncate-q3", MutationCategory::Payload, ExpectedInvariant::MustReject, 2},
    {"payload-truncate-q4", MutationCategory::Payload, ExpectedInvariant::MustReject, 3},
    {"payload-truncate-q5", MutationCategory::Payload, ExpectedInvariant::MustReject, 4},
    {"payload-truncate-q6", MutationCategory::Payload, ExpectedInvariant::MustReject, 5},
    {"payload-truncate-q7", MutationCategory::Payload, ExpectedInvariant::MustReject, 6},
    {"payload-truncate-q8", MutationCategory::Payload, ExpectedInvariant::MustReject, 7},
    {"payload-corrupt-zlib", MutationCategory::Payload, ExpectedInvariant::MustReject, 8},
    {"payload-decoded-size-mismatch", MutationCategory::Payload, ExpectedInvariant::MustReject, 9},
    {"payload-ucs4-surrogate", MutationCategory::Payload, ExpectedInvariant::MustReject, 10},
    {"payload-ucs4-out-of-range", MutationCategory::Payload, ExpectedInvariant::MustReject, 11},
    {"payload-roster-nul-nonterminal", MutationCategory::Payload, ExpectedInvariant::MustReject, 12},
    {"payload-int32-boundaries", MutationCategory::Payload, ExpectedInvariant::MustReject, 13},
    {"payload-undefined-label", MutationCategory::Payload, ExpectedInvariant::MustReject, 14},
    {"payload-cross-shape-mismatch", MutationCategory::Payload, ExpectedInvariant::MustReject, 15},

    {"unknown-root-source", MutationCategory::UnknownMetadata, ExpectedInvariant::MustAcceptAndPreserveUnknown, 0},
    {"unknown-annotation-task", MutationCategory::UnknownMetadata, ExpectedInvariant::MustAcceptAndPreserveUnknown, 1},
    {"unknown-origin-annotation", MutationCategory::UnknownMetadata, ExpectedInvariant::MustAcceptAndPreserveUnknown, 2},
    {"unknown-label-by-code", MutationCategory::UnknownMetadata, ExpectedInvariant::MustAcceptAndPreserveUnknown, 3},
    {"unknown-all-supported-maps", MutationCategory::UnknownMetadata, ExpectedInvariant::MustAcceptAndPreserveUnknown, 4},
    {"unknown-nested-supported-maps", MutationCategory::UnknownMetadata, ExpectedInvariant::MustAcceptAndPreserveUnknown, 5},
}};

inline constexpr std::size_t kDefaultMutationCaseCount =
    kMutationRecipes.size();
inline constexpr std::size_t kMaximumMutationCaseCount = 512;
inline constexpr std::size_t kDefaultMaximumInputBytes = 1024U * 1024U;
inline constexpr std::size_t kMaximumInputBytes = 8U * 1024U * 1024U;
inline constexpr std::uint64_t kProductionDecodedBlockLimitBytes =
    512ULL * 1024ULL * 1024ULL;

struct RawBlock {
    std::size_t offset = 0;
    std::size_t size = 0;
    std::size_t header_size = 0;
    std::size_t payload_offset = 0;
    std::size_t allocated_size = 0;
    std::size_t used_size = 0;
    std::size_t decoded_size = 0;
    std::array<unsigned char, 4> compression{};
};

struct RawAsdf {
    std::size_t tree_end = 0;
    std::vector<RawBlock> blocks;
    std::optional<std::size_t> block_index_offset;
};

struct UnknownMappingExpectation {
    std::string node;
    std::string key;
    std::string token;
    bool mapping_value = false;

    [[nodiscard]] bool operator==(
        const UnknownMappingExpectation&) const = default;
};

struct GeneratedMutationCase {
    std::vector<unsigned char> bytes;
    MutationRecipe recipe;
    std::vector<std::string> mutation_chain;
    std::vector<std::string> unknown_tokens;
    std::vector<UnknownMappingExpectation> unknown_mappings;
};

class DeterministicRandom final {
public:
    explicit DeterministicRandom(std::uint64_t state) noexcept
        : state_(state)
    {
    }

    [[nodiscard]] std::uint64_t Next() noexcept
    {
        state_ += 0x9e3779b97f4a7c15ULL;
        std::uint64_t value = state_;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }

    [[nodiscard]] std::size_t Bounded(std::size_t upper_exclusive) noexcept
    {
        return upper_exclusive == 0U
            ? 0U
            : static_cast<std::size_t>(Next() % upper_exclusive);
    }

private:
    std::uint64_t state_;
};

[[nodiscard]] inline std::string_view CategoryName(
    MutationCategory category) noexcept
{
    switch (category) {
    case MutationCategory::YamlStructure:
        return "yaml_structure";
    case MutationCategory::YamlScalar:
        return "yaml_scalar";
    case MutationCategory::BlockHeader:
        return "block_header";
    case MutationCategory::BlockIndex:
        return "block_index";
    case MutationCategory::Payload:
        return "payload";
    case MutationCategory::UnknownMetadata:
        return "unknown_metadata";
    }
    return "unknown";
}

[[nodiscard]] inline std::string_view ExpectedInvariantName(
    ExpectedInvariant expected) noexcept
{
    switch (expected) {
    case ExpectedInvariant::Controlled:
        return "controlled_accept_or_typed_reject";
    case ExpectedInvariant::MustReject:
        return "typed_reject";
    case ExpectedInvariant::MustRejectResourceLimit:
        return "resource_limit_reject";
    case ExpectedInvariant::MustAcceptAndPreserveUnknown:
        return "accept_validate_and_preserve_unknown_mappings";
    }
    return "unknown";
}

template <typename UInt>
[[nodiscard]] inline UInt ReadBigEndian(
    std::span<const unsigned char> bytes,
    std::size_t offset)
{
    if (offset > bytes.size() || sizeof(UInt) > bytes.size() - offset) {
        throw std::runtime_error("mutation raw read exceeds input bounds");
    }
    UInt value = 0;
    for (std::size_t index = 0; index < sizeof(UInt); ++index) {
        value = static_cast<UInt>((value << 8U) | bytes[offset + index]);
    }
    return value;
}

inline void StoreBigEndian(std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::uint64_t value,
    std::size_t width)
{
    if (offset > bytes.size() || width > bytes.size() - offset) {
        throw std::runtime_error("mutation raw write exceeds input bounds");
    }
    for (std::size_t index = 0; index < width; ++index) {
        const std::size_t shift = 8U * (width - index - 1U);
        bytes[offset + index] =
            static_cast<unsigned char>((value >> shift) & 0xffU);
    }
}

[[nodiscard]] inline RawAsdf ParseRawAsdf(
    const std::vector<unsigned char>& bytes)
{
    constexpr std::string_view terminator = "\n...\n";
    const auto marker = std::search(
        bytes.begin(), bytes.end(), terminator.begin(), terminator.end());
    if (marker == bytes.end()) {
        throw std::runtime_error("mutation seed has no YAML terminator");
    }

    RawAsdf parsed;
    parsed.tree_end = static_cast<std::size_t>(marker - bytes.begin()) +
        terminator.size();
    std::size_t offset = parsed.tree_end;
    while (offset < bytes.size()) {
        while (offset < bytes.size() &&
               (bytes[offset] == 0U ||
                   std::isspace(
                       static_cast<unsigned char>(bytes[offset])) != 0)) {
            ++offset;
        }
        if (offset == bytes.size()) {
            break;
        }
        constexpr std::string_view index_header = "#ASDF BLOCK INDEX";
        const std::string_view suffix(
            reinterpret_cast<const char*>(bytes.data() + offset),
            bytes.size() - offset);
        if (suffix.starts_with(index_header)) {
            parsed.block_index_offset = offset;
            break;
        }
        if (offset + 6U > bytes.size() || bytes[offset] != 0xd3U ||
            bytes[offset + 1U] != 'B' || bytes[offset + 2U] != 'L' ||
            bytes[offset + 3U] != 'K') {
            throw std::runtime_error(
                "mutation seed has unexpected bytes between blocks");
        }
        const std::size_t header_size =
            ReadBigEndian<std::uint16_t>(bytes, offset + 4U);
        const std::size_t fields = offset + 6U;
        if (header_size < 48U || fields > bytes.size() ||
            header_size > bytes.size() - fields) {
            throw std::runtime_error("mutation seed block header is invalid");
        }
        const std::uint64_t allocated =
            ReadBigEndian<std::uint64_t>(bytes, fields + 8U);
        const std::uint64_t used =
            ReadBigEndian<std::uint64_t>(bytes, fields + 16U);
        const std::uint64_t decoded =
            ReadBigEndian<std::uint64_t>(bytes, fields + 24U);
        if (allocated > std::numeric_limits<std::size_t>::max() ||
            used > std::numeric_limits<std::size_t>::max() ||
            decoded > std::numeric_limits<std::size_t>::max() ||
            allocated > bytes.size() - fields - header_size) {
            throw std::runtime_error("mutation seed block payload is invalid");
        }
        RawBlock block;
        block.offset = offset;
        block.header_size = header_size;
        block.payload_offset = fields + header_size;
        block.allocated_size = static_cast<std::size_t>(allocated);
        block.used_size = static_cast<std::size_t>(used);
        block.decoded_size = static_cast<std::size_t>(decoded);
        block.size = 6U + header_size + block.allocated_size;
        std::copy_n(
            bytes.begin() + static_cast<std::ptrdiff_t>(fields + 4U),
            4U,
            block.compression.begin());
        parsed.blocks.push_back(block);
        offset += block.size;
    }
    if (parsed.blocks.empty()) {
        throw std::runtime_error("mutation seed contains no blocks");
    }
    return parsed;
}

[[nodiscard]] inline std::string BuildBlockIndex(const RawAsdf& raw)
{
    std::ostringstream index;
    index << "#ASDF BLOCK INDEX\n%YAML 1.1\n---\n";
    for (const RawBlock& block : raw.blocks) {
        index << "- " << block.offset << "\n";
    }
    index << "...\n";
    return index.str();
}

inline void RebuildBlockIndex(std::vector<unsigned char>& bytes)
{
    const RawAsdf raw = ParseRawAsdf(bytes);
    if (raw.block_index_offset) {
        bytes.erase(
            bytes.begin() +
                static_cast<std::ptrdiff_t>(*raw.block_index_offset),
            bytes.end());
    }
    const RawAsdf without_index = ParseRawAsdf(bytes);
    const std::string index = BuildBlockIndex(without_index);
    bytes.insert(bytes.end(), index.begin(), index.end());
}

inline void ReplaceTextOnce(std::vector<unsigned char>& bytes,
    std::string_view old_text,
    std::string_view new_text)
{
    const auto found = std::search(
        bytes.begin(), bytes.end(), old_text.begin(), old_text.end());
    if (found == bytes.end()) {
        throw std::runtime_error(
            "mutation text target is absent: " + std::string(old_text));
    }
    const std::size_t offset =
        static_cast<std::size_t>(found - bytes.begin());
    bytes.erase(
        found,
        found + static_cast<std::ptrdiff_t>(old_text.size()));
    bytes.insert(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        new_text.begin(),
        new_text.end());
}

inline void InsertBefore(std::vector<unsigned char>& bytes,
    std::string_view marker,
    std::string_view insertion)
{
    const auto found = std::search(
        bytes.begin(), bytes.end(), marker.begin(), marker.end());
    if (found == bytes.end()) {
        throw std::runtime_error(
            "mutation insertion target is absent: " + std::string(marker));
    }
    bytes.insert(found, insertion.begin(), insertion.end());
}

inline void ReplaceLineValue(std::vector<unsigned char>& bytes,
    std::string_view prefix,
    std::string_view replacement_value,
    bool last_match = false)
{
    auto found = std::search(
        bytes.begin(), bytes.end(), prefix.begin(), prefix.end());
    if (last_match) {
        for (;;) {
            const auto next = std::search(
                found == bytes.end() ? bytes.end() : found + 1,
                bytes.end(),
                prefix.begin(),
                prefix.end());
            if (next == bytes.end()) {
                break;
            }
            found = next;
        }
    }
    if (found == bytes.end()) {
        throw std::runtime_error(
            "mutation scalar target is absent: " + std::string(prefix));
    }
    const auto value_begin = found +
        static_cast<std::ptrdiff_t>(prefix.size());
    const auto line_end = std::find(value_begin, bytes.end(), '\n');
    if (line_end == bytes.end()) {
        throw std::runtime_error("mutation scalar line is incomplete");
    }
    const std::size_t offset =
        static_cast<std::size_t>(value_begin - bytes.begin());
    bytes.erase(value_begin, line_end);
    bytes.insert(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        replacement_value.begin(),
        replacement_value.end());
}

[[nodiscard]] inline std::string LineValue(
    const std::vector<unsigned char>& bytes,
    std::string_view prefix)
{
    const auto found = std::search(
        bytes.begin(), bytes.end(), prefix.begin(), prefix.end());
    if (found == bytes.end()) {
        throw std::runtime_error(
            "mutation scalar source is absent: " + std::string(prefix));
    }
    const auto value_begin = found +
        static_cast<std::ptrdiff_t>(prefix.size());
    const auto line_end = std::find(value_begin, bytes.end(), '\n');
    return std::string(value_begin, line_end);
}

[[nodiscard]] inline std::vector<unsigned char> DecodeBlock(
    const std::vector<unsigned char>& bytes,
    const RawBlock& block)
{
    const std::span<const unsigned char> encoded(
        bytes.data() + block.payload_offset, block.used_size);
    if (block.compression ==
        std::array<unsigned char, 4>{0U, 0U, 0U, 0U}) {
        return std::vector<unsigned char>(encoded.begin(), encoded.end());
    }
    if (block.compression !=
        std::array<unsigned char, 4>{'z', 'l', 'i', 'b'}) {
        throw std::runtime_error("mutation seed compression is unsupported");
    }
    std::vector<unsigned char> decoded(block.decoded_size);
    uLongf decoded_size = static_cast<uLongf>(decoded.size());
    const int status = uncompress(
        decoded.data(),
        &decoded_size,
        encoded.data(),
        static_cast<uLong>(encoded.size()));
    if (status != Z_OK || decoded_size != decoded.size()) {
        throw std::runtime_error("mutation seed zlib block did not decode");
    }
    return decoded;
}

inline void AppendBigEndian(std::vector<unsigned char>& output,
    std::uint64_t value,
    std::size_t width)
{
    for (std::size_t remaining = width; remaining > 0U; --remaining) {
        output.push_back(static_cast<unsigned char>(
            (value >> ((remaining - 1U) * 8U)) & 0xffU));
    }
}

inline void ReplaceBlockWithDecoded(std::vector<unsigned char>& bytes,
    const RawBlock& block,
    std::span<const unsigned char> decoded)
{
    uLongf compressed_size = compressBound(static_cast<uLong>(decoded.size()));
    std::vector<unsigned char> compressed(compressed_size);
    const int status = compress2(
        compressed.data(),
        &compressed_size,
        decoded.data(),
        static_cast<uLong>(decoded.size()),
        6);
    if (status != Z_OK) {
        throw std::runtime_error("mutation zlib block could not be encoded");
    }
    compressed.resize(static_cast<std::size_t>(compressed_size));

    std::vector<unsigned char> replacement;
    replacement.insert(replacement.end(), {0xd3U, 'B', 'L', 'K'});
    AppendBigEndian(replacement, 48U, 2U);
    AppendBigEndian(replacement, 0U, 4U);
    replacement.insert(replacement.end(), {'z', 'l', 'i', 'b'});
    AppendBigEndian(replacement, compressed.size(), 8U);
    AppendBigEndian(replacement, compressed.size(), 8U);
    AppendBigEndian(replacement, decoded.size(), 8U);
    replacement.insert(replacement.end(), 16U, 0U);
    replacement.insert(
        replacement.end(), compressed.begin(), compressed.end());

    bytes.erase(
        bytes.begin() + static_cast<std::ptrdiff_t>(block.offset),
        bytes.begin() +
            static_cast<std::ptrdiff_t>(block.offset + block.size));
    bytes.insert(
        bytes.begin() + static_cast<std::ptrdiff_t>(block.offset),
        replacement.begin(),
        replacement.end());
    RebuildBlockIndex(bytes);
}

inline void StoreLittleEndian32(std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::uint32_t value)
{
    if (offset > bytes.size() || 4U > bytes.size() - offset) {
        throw std::runtime_error("mutation int32 write exceeds block bounds");
    }
    for (std::size_t index = 0; index < 4U; ++index) {
        bytes[offset + index] =
            static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
    }
}

[[nodiscard]] inline std::string Token(
    std::uint64_t seed,
    std::size_t case_index,
    std::string_view node)
{
    std::ostringstream token;
    token << "mutation-" << node << '-' << std::hex << std::setw(16)
          << std::setfill('0') << seed << '-' << std::dec << case_index;
    return token.str();
}

inline void AddContextUnknown(std::vector<unsigned char>& bytes,
    std::uint64_t seed,
    std::size_t case_index,
    std::vector<std::string>& chain)
{
    const std::string token = Token(seed, case_index, "context");
    const std::string insertion =
        "mutation_context: {token: \"" + token + "\"}\n";
    InsertBefore(bytes, "schema_version: ", insertion);
    RebuildBlockIndex(bytes);
    chain.push_back("inject-supported-root-context:" + token);
}

inline void ApplyYamlStructure(std::vector<unsigned char>& bytes,
    std::size_t variant,
    std::vector<std::string>& chain)
{
    switch (variant) {
    case 0:
        ReplaceTextOnce(bytes,
            "source_collection:\n",
            "source_collection: scalar\n");
        break;
    case 1:
        ReplaceTextOnce(bytes, "annotation:\n", "annotation: []\n");
        break;
    case 2:
        ReplaceTextOnce(bytes, "sample_roster:\n", "sample_roster: null\n");
        break;
    case 3:
        InsertBefore(bytes,
            "schema_version: ",
            "schema_version: \"2.0.0\"\n");
        break;
    case 4: {
        const std::string line =
            "schema_version: " +
            LineValue(bytes, "schema_version: ") + "\n";
        ReplaceTextOnce(bytes, line, "");
        break;
    }
    case 5:
        ReplaceTextOnce(bytes,
            "--- !core/asdf-1.1.0",
            "--- !future/asdf-9.9.9");
        break;
    case 6:
        ReplaceTextOnce(bytes,
            "!core/ndarray-1.0.0",
            "!core/ndarray-9.9.9");
        break;
    case 7: {
        std::string insertion = "mutation_deep:\n";
        for (std::size_t depth = 0; depth < 48U; ++depth) {
            insertion.append(depth * 2U + 2U, ' ');
            insertion += "level_" + std::to_string(depth) + ":\n";
        }
        insertion.append(98U, ' ');
        insertion += "leaf: true\n";
        InsertBefore(bytes, "schema_version: ", insertion);
        break;
    }
    case 8:
        InsertBefore(bytes,
            "schema_version: ",
            "mutation_anchor: &bounded_anchor\n"
            "  nested: [1, 2, 3]\n"
            "mutation_alias: *bounded_anchor\n");
        break;
    case 9:
        ReplaceTextOnce(bytes,
            "--- !core/asdf-1.1.0\n",
            "# mutation removed document start\n");
        break;
    case 10:
        ReplaceTextOnce(bytes, "\n...\n", "\n");
        chain.push_back("remove-yaml-document-end");
        return;
    case 11:
        bytes.insert(bytes.begin(), {0xefU, 0xbbU, 0xbfU});
        break;
    case 12: {
        std::size_t converted = 0;
        for (std::size_t index = 0;
             index < bytes.size() && converted < 8U;
             ++index) {
            if (bytes[index] == '\n') {
                bytes.insert(
                    bytes.begin() + static_cast<std::ptrdiff_t>(index),
                    '\r');
                ++index;
                ++converted;
            }
        }
        break;
    }
    case 13: {
        constexpr std::string_view marker = "schema_version: ";
        const auto found = std::search(bytes.begin(),
            bytes.end(),
            marker.begin(),
            marker.end());
        if (found == bytes.end()) {
            throw std::runtime_error("embedded-NUL target is absent");
        }
        bytes.insert(
            found + static_cast<std::ptrdiff_t>(marker.size()),
            0U);
        break;
    }
    case 14: {
        const std::string_view marker = "  name: \"";
        const auto found = std::search(
            bytes.begin(), bytes.end(), marker.begin(), marker.end());
        if (found == bytes.end()) {
            throw std::runtime_error("invalid-UTF-8 target is absent");
        }
        const std::size_t offset =
            static_cast<std::size_t>(found - bytes.begin()) + marker.size();
        bytes[offset] = 0xffU;
        break;
    }
    case 15:
        InsertBefore(bytes, "format_kind: ", "- mutation-sequence-entry\n");
        break;
    default:
        throw std::runtime_error("unknown YAML structure mutation");
    }
    RebuildBlockIndex(bytes);
    chain.push_back("yaml-structure-variant:" + std::to_string(variant));
}

inline void ApplyYamlScalar(std::vector<unsigned char>& bytes,
    std::size_t variant,
    std::vector<std::string>& chain)
{
    switch (variant) {
    case 0: {
        const std::string original = LineValue(bytes, "  sample_count: ");
        ReplaceLineValue(
            bytes, "  sample_count: ", "\"" + original + "\"");
        break;
    }
    case 1:
        ReplaceLineValue(bytes, "  sample_count: ", "true");
        break;
    case 2:
        ReplaceLineValue(bytes, "  sample_count: ", "null");
        break;
    case 3:
        ReplaceLineValue(bytes, "  sample_count: ", "-1");
        break;
    case 4:
        ReplaceLineValue(bytes, "format_kind: ", "7");
        break;
    case 5:
        ReplaceLineValue(bytes, "schema_version: ", "false");
        break;
    case 6:
        ReplaceLineValue(bytes, "    shape: ", "\"3\"");
        break;
    case 7:
        ReplaceLineValue(bytes, "    source: ", "true", true);
        break;
    case 8:
        ReplaceLineValue(bytes, "    value: ", "\"-1\"");
        break;
    case 9:
        ReplaceLineValue(bytes, "  - code: ", "null");
        break;
    default:
        throw std::runtime_error("unknown YAML scalar mutation");
    }
    RebuildBlockIndex(bytes);
    chain.push_back("yaml-scalar-variant:" + std::to_string(variant));
}

inline void ApplyBlockHeader(std::vector<unsigned char>& bytes,
    std::size_t variant,
    std::vector<std::string>& chain)
{
    const RawAsdf raw = ParseRawAsdf(bytes);
    const RawBlock& block = raw.blocks.front();
    const std::size_t fields = block.offset + 6U;
    switch (variant) {
    case 0:
        bytes[block.offset] ^= 0xffU;
        break;
    case 1:
        StoreBigEndian(bytes, block.offset + 4U, 47U, 2U);
        break;
    case 2:
        StoreBigEndian(bytes, block.offset + 4U, 0xffffU, 2U);
        break;
    case 3:
        StoreBigEndian(bytes, fields, 1U, 4U);
        break;
    case 4:
        std::copy_n("lz4x", 4U, bytes.begin() +
            static_cast<std::ptrdiff_t>(fields + 4U));
        break;
    case 5:
        StoreBigEndian(bytes,
            fields + 24U,
            kProductionDecodedBlockLimitBytes + 1U,
            8U);
        break;
    case 6:
        StoreBigEndian(bytes, fields + 8U, 0U, 8U);
        break;
    case 7:
        StoreBigEndian(bytes,
            fields + 8U,
            kProductionDecodedBlockLimitBytes + 1U,
            8U);
        StoreBigEndian(bytes,
            fields + 16U,
            kProductionDecodedBlockLimitBytes + 1U,
            8U);
        break;
    case 8:
        StoreBigEndian(bytes,
            fields + 24U,
            static_cast<std::uint64_t>(block.decoded_size) + 4U,
            8U);
        break;
    case 9:
        bytes[fields + 32U] = 1U;
        break;
    case 10:
        bytes.insert(
            bytes.begin() +
                static_cast<std::ptrdiff_t>(block.payload_offset),
            8U,
            0xa5U);
        StoreBigEndian(bytes,
            block.offset + 4U,
            static_cast<std::uint64_t>(block.header_size) + 8U,
            2U);
        RebuildBlockIndex(bytes);
        break;
    case 11: {
        if (raw.blocks.size() < 2U || !raw.block_index_offset) {
            throw std::runtime_error("block-order mutation needs two blocks");
        }
        const RawBlock& second = raw.blocks[1];
        std::vector<unsigned char> reordered;
        reordered.insert(reordered.end(),
            bytes.begin(),
            bytes.begin() + static_cast<std::ptrdiff_t>(block.offset));
        reordered.insert(reordered.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(second.offset),
            bytes.begin() +
                static_cast<std::ptrdiff_t>(second.offset + second.size));
        reordered.insert(reordered.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(block.offset),
            bytes.begin() +
                static_cast<std::ptrdiff_t>(block.offset + block.size));
        reordered.insert(reordered.end(),
            bytes.begin() +
                static_cast<std::ptrdiff_t>(*raw.block_index_offset),
            bytes.end());
        bytes = std::move(reordered);
        RebuildBlockIndex(bytes);
        break;
    }
    case 12: {
        if (!raw.block_index_offset) {
            throw std::runtime_error(
                "unreferenced-block mutation needs an index");
        }
        const std::vector<unsigned char> copy(
            bytes.begin() + static_cast<std::ptrdiff_t>(block.offset),
            bytes.begin() +
                static_cast<std::ptrdiff_t>(block.offset + block.size));
        bytes.insert(
            bytes.begin() +
                static_cast<std::ptrdiff_t>(*raw.block_index_offset),
            copy.begin(),
            copy.end());
        break;
    }
    case 13:
        ReplaceLineValue(bytes, "    source: ", "0", true);
        RebuildBlockIndex(bytes);
        break;
    default:
        throw std::runtime_error("unknown block-header mutation");
    }
    chain.push_back("block-header-variant:" + std::to_string(variant));
}

[[nodiscard]] inline std::string CustomIndex(
    const std::vector<std::string>& entries)
{
    std::string index = "#ASDF BLOCK INDEX\n%YAML 1.1\n---\n";
    for (const std::string& entry : entries) {
        index += "- " + entry + "\n";
    }
    index += "...\n";
    return index;
}

inline void ReplaceIndex(std::vector<unsigned char>& bytes,
    const RawAsdf& raw,
    std::string_view index)
{
    if (!raw.block_index_offset) {
        throw std::runtime_error("block-index mutation needs an index");
    }
    bytes.erase(
        bytes.begin() +
            static_cast<std::ptrdiff_t>(*raw.block_index_offset),
        bytes.end());
    bytes.insert(bytes.end(), index.begin(), index.end());
}

inline void ApplyBlockIndex(std::vector<unsigned char>& bytes,
    std::size_t variant,
    std::vector<std::string>& chain)
{
    const RawAsdf raw = ParseRawAsdf(bytes);
    if (!raw.block_index_offset) {
        throw std::runtime_error("block-index mutation seed has no index");
    }
    std::vector<std::string> actual;
    for (const RawBlock& block : raw.blocks) {
        actual.push_back(std::to_string(block.offset));
    }
    switch (variant) {
    case 0:
        bytes.erase(bytes.begin() +
                static_cast<std::ptrdiff_t>(*raw.block_index_offset),
            bytes.end());
        break;
    case 1:
        bytes.resize(*raw.block_index_offset + 12U);
        break;
    case 2:
        actual.front() = "not-a-number";
        ReplaceIndex(bytes, raw, CustomIndex(actual));
        break;
    case 3:
        actual.front() = "-1";
        ReplaceIndex(bytes, raw, CustomIndex(actual));
        break;
    case 4:
        if (actual.size() < 2U) {
            throw std::runtime_error("decreasing index needs two blocks");
        }
        std::swap(actual[0], actual[1]);
        ReplaceIndex(bytes, raw, CustomIndex(actual));
        break;
    case 5:
        if (actual.size() < 2U) {
            throw std::runtime_error("duplicate index needs two blocks");
        }
        actual[1] = actual[0];
        ReplaceIndex(bytes, raw, CustomIndex(actual));
        break;
    case 6:
        actual.back() = std::to_string(raw.blocks.back().offset + 1U);
        ReplaceIndex(bytes, raw, CustomIndex(actual));
        break;
    case 7:
        if (raw.tree_end == 0U) {
            throw std::runtime_error(
                "first-offset mismatch needs a nonzero tree boundary");
        }
        actual.front() = std::to_string(raw.tree_end - 1U);
        ReplaceIndex(bytes, raw, CustomIndex(actual));
        break;
    case 8:
        bytes.insert(
            bytes.begin() +
                static_cast<std::ptrdiff_t>(*raw.block_index_offset),
            'X');
        break;
    case 9:
        bytes.insert(bytes.end(), {'g', 'a', 'r', 'b', 'a', 'g', 'e'});
        break;
    case 10:
        actual.front() = "184467440737095516160";
        ReplaceIndex(bytes, raw, CustomIndex(actual));
        break;
    default:
        throw std::runtime_error("unknown block-index mutation");
    }
    chain.push_back("block-index-variant:" + std::to_string(variant));
}

inline void ApplyPayload(std::vector<unsigned char>& bytes,
    std::size_t variant,
    DeterministicRandom& random,
    std::vector<std::string>& chain)
{
    if (variant < 8U) {
        const std::size_t jitter = random.Bounded(3U);
        const std::size_t numerator =
            std::min<std::size_t>(9U, variant + 1U + jitter);
        const std::size_t position = std::max<std::size_t>(
            1U, ((bytes.size() - 1U) * numerator) / 10U);
        bytes.resize(position);
        chain.push_back("truncate-at-byte:" + std::to_string(position));
        return;
    }

    const RawAsdf raw = ParseRawAsdf(bytes);
    const RawBlock& roster = raw.blocks.front();
    const RawBlock& values = raw.blocks.back();
    switch (variant) {
    case 8: {
        const std::size_t offset = roster.payload_offset +
            std::max<std::size_t>(1U, roster.used_size / 2U);
        if (offset >= roster.payload_offset + roster.used_size) {
            throw std::runtime_error("zlib corruption target is absent");
        }
        bytes[offset] ^= 0x5aU;
        break;
    }
    case 9:
        StoreBigEndian(bytes,
            values.offset + 6U + 24U,
            static_cast<std::uint64_t>(values.decoded_size) + 4U,
            8U);
        break;
    case 10: {
        std::vector<unsigned char> decoded = DecodeBlock(bytes, roster);
        StoreLittleEndian32(decoded, 0U, 0xd800U);
        ReplaceBlockWithDecoded(bytes, roster, decoded);
        break;
    }
    case 11: {
        std::vector<unsigned char> decoded = DecodeBlock(bytes, roster);
        StoreLittleEndian32(decoded, 0U, 0x110000U);
        ReplaceBlockWithDecoded(bytes, roster, decoded);
        break;
    }
    case 12: {
        std::vector<unsigned char> decoded = DecodeBlock(bytes, roster);
        if (decoded.size() < 8U) {
            throw std::runtime_error("roster NUL mutation needs two scalars");
        }
        StoreLittleEndian32(decoded, 0U, 0U);
        ReplaceBlockWithDecoded(bytes, roster, decoded);
        break;
    }
    case 13: {
        std::vector<unsigned char> decoded = DecodeBlock(bytes, values);
        if (decoded.size() < 8U) {
            throw std::runtime_error("int32 boundary mutation needs two values");
        }
        StoreLittleEndian32(decoded, 0U, 0x80000000U);
        StoreLittleEndian32(decoded, 4U, 0x7fffffffU);
        ReplaceBlockWithDecoded(bytes, values, decoded);
        break;
    }
    case 14: {
        std::vector<unsigned char> decoded = DecodeBlock(bytes, values);
        StoreLittleEndian32(decoded, 0U, 99U);
        ReplaceBlockWithDecoded(bytes, values, decoded);
        break;
    }
    case 15: {
        const std::string sample_count_text =
            LineValue(bytes, "  sample_count: ");
        std::size_t consumed = 0;
        const unsigned long long sample_count =
            std::stoull(sample_count_text, &consumed, 10);
        if (consumed != sample_count_text.size() ||
            sample_count ==
                std::numeric_limits<unsigned long long>::max()) {
            throw std::runtime_error(
                "shape mutation seed has an invalid sample count");
        }
        ReplaceLineValue(bytes,
            "    shape: ",
            "[" + std::to_string(sample_count + 1U) + "]",
            true);
        RebuildBlockIndex(bytes);
        break;
    }
    default:
        throw std::runtime_error("unknown payload mutation");
    }
    chain.push_back("payload-variant:" + std::to_string(variant));
}

inline void InsertUnknownAt(std::vector<unsigned char>& bytes,
    std::string_view marker,
    std::string_view indentation,
    std::string_view key,
    const std::string& token,
    bool nested)
{
    std::string insertion(indentation);
    insertion += key;
    insertion += nested ? ": {token: \"" : ": \"";
    insertion += token;
    insertion += nested ? "\"}\n" : "\"\n";
    InsertBefore(bytes, marker, insertion);
}

inline void InsertUnknownLabel(std::vector<unsigned char>& bytes,
    const std::string& token,
    bool nested)
{
    constexpr std::string_view code_marker = "  - code: ";
    const auto code = std::search(
        bytes.begin(), bytes.end(), code_marker.begin(), code_marker.end());
    if (code == bytes.end()) {
        throw std::runtime_error("unknown label mutation needs a label entry");
    }
    constexpr std::string_view name_marker = "    name: ";
    const auto name = std::search(
        code, bytes.end(), name_marker.begin(), name_marker.end());
    if (name == bytes.end()) {
        throw std::runtime_error("unknown label mutation needs a label name");
    }
    const auto line_end = std::find(name, bytes.end(), '\n');
    const std::string insertion = nested
        ? "    mutation_unknown_label: {token: \"" + token + "\"}\n"
        : "    mutation_unknown_label: \"" + token + "\"\n";
    bytes.insert(line_end + 1, insertion.begin(), insertion.end());
}

inline void ApplyUnknownMetadata(std::vector<unsigned char>& bytes,
    std::uint64_t seed,
    std::size_t case_index,
    std::size_t variant,
    std::vector<std::string>& chain,
    std::vector<std::string>& tokens,
    std::vector<UnknownMappingExpectation>& generated_expectations)
{
    const bool nested = variant == 5U;
    const auto add = [&](std::string_view node,
                         std::string_view marker,
                         std::string_view indentation,
                         std::string_view key) {
        const std::string token = Token(seed, case_index, node);
        InsertUnknownAt(
            bytes, marker, indentation, key, token, nested);
        tokens.push_back(token);
        generated_expectations.push_back(UnknownMappingExpectation{
            .node = std::string(node),
            .key = std::string(key),
            .token = token,
            .mapping_value = nested,
        });
        chain.push_back("inject-unknown-" + std::string(node));
    };

    if (variant == 0U || variant >= 4U) {
        add("root", "schema_version: ", "", "mutation_unknown_root");
        add("source",
            "  fingerprint: ",
            "  ",
            "mutation_unknown_source");
    }
    if (variant == 1U || variant >= 4U) {
        add("annotation",
            "  values: ",
            "  ",
            "mutation_unknown_annotation");
        add("task", "  labels:\n", "  ", "mutation_unknown_task");
    }
    if (variant == 2U || variant >= 4U) {
        add("origin",
            "    annotation:\n",
            "    ",
            "mutation_unknown_origin");
        add("origin_annotation",
            "      format: ",
            "      ",
            "mutation_unknown_origin_annotation");
    }
    if (variant == 3U || variant >= 4U) {
        const std::string token = Token(seed, case_index, "label");
        InsertUnknownLabel(bytes, token, nested);
        tokens.push_back(token);
        generated_expectations.push_back(UnknownMappingExpectation{
            .node = "label",
            .key = "mutation_unknown_label",
            .token = token,
            .mapping_value = nested,
        });
        chain.push_back("inject-unknown-label-by-stable-code");
    }
    RebuildBlockIndex(bytes);
}

[[nodiscard]] inline GeneratedMutationCase GenerateMutationCase(
    const std::vector<unsigned char>& seed_bytes,
    std::uint64_t seed,
    std::size_t case_index,
    std::size_t maximum_input_bytes)
{
    if (seed_bytes.empty() || seed_bytes.size() > maximum_input_bytes) {
        throw std::runtime_error("mutation seed violates the input-size bound");
    }
    const MutationRecipe recipe = kMutationRecipes[(case_index +
        static_cast<std::size_t>(seed % kMutationRecipes.size())) %
        kMutationRecipes.size()];
    DeterministicRandom random(
        seed ^ (0xd1b54a32d192ed03ULL * (case_index + 1U)));
    GeneratedMutationCase generated{
        .bytes = seed_bytes,
        .recipe = recipe,
    };

    if (recipe.category != MutationCategory::UnknownMetadata) {
        AddContextUnknown(
            generated.bytes,
            seed,
            case_index,
            generated.mutation_chain);
    }

    switch (recipe.category) {
    case MutationCategory::YamlStructure:
        ApplyYamlStructure(
            generated.bytes, recipe.variant, generated.mutation_chain);
        break;
    case MutationCategory::YamlScalar:
        ApplyYamlScalar(
            generated.bytes, recipe.variant, generated.mutation_chain);
        break;
    case MutationCategory::BlockHeader:
        ApplyBlockHeader(
            generated.bytes, recipe.variant, generated.mutation_chain);
        break;
    case MutationCategory::BlockIndex:
        ApplyBlockIndex(
            generated.bytes, recipe.variant, generated.mutation_chain);
        break;
    case MutationCategory::Payload:
        ApplyPayload(generated.bytes,
            recipe.variant,
            random,
            generated.mutation_chain);
        break;
    case MutationCategory::UnknownMetadata:
        ApplyUnknownMetadata(generated.bytes,
            seed,
            case_index,
            recipe.variant,
            generated.mutation_chain,
            generated.unknown_tokens,
            generated.unknown_mappings);
        break;
    }
    generated.mutation_chain.insert(
        generated.mutation_chain.begin(),
        "recipe:" + std::string(recipe.name));
    if (generated.bytes.empty() ||
        generated.bytes.size() > maximum_input_bytes) {
        throw std::runtime_error(
            "generated mutation violates the input-size bound");
    }
    return generated;
}

[[nodiscard]] inline std::string InputFingerprint(
    std::span<const unsigned char> bytes)
{
    std::uint64_t value = 14695981039346656037ULL;
    for (const unsigned char byte : bytes) {
        value ^= byte;
        value *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << value;
    return output.str();
}

}  // namespace specforge::asdf_mutation_test
