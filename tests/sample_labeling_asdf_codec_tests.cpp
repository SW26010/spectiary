#include "app/local_user_state_json.h"
#include "domain/sample_labeling_asdf_codec.h"
#include "platform/file_sha256.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::filesystem::path FixturePath(std::string_view name)
{
    return std::filesystem::path(SPECFORGE_ASDF_LABELING_FIXTURE_DIR) /
        std::string(name);
}

std::filesystem::path TempPath(std::string_view suffix)
{
    std::filesystem::path path = std::filesystem::temp_directory_path();
    path /= "specforge_sample_labeling_asdf_codec_";
    path += std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    path += std::string(suffix);
    return path;
}

std::vector<unsigned char> ReadAllBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    Require(input.good(), "test fixture should open");
    const std::streampos end = input.tellg();
    Require(end >= 0, "test fixture should report its size");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!bytes.empty()) {
        input.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    Require(input.good(), "test fixture should be read completely");
    return bytes;
}

specforge::JsonValue ReadJsonFile(const std::filesystem::path& path)
{
    const std::vector<unsigned char> bytes = ReadAllBytes(path);
    const std::string text(
        reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::string error;
    std::optional<specforge::JsonValue> parsed =
        specforge::ParseJson(text, error);
    Require(parsed.has_value(), "JSON fixture should parse");
    return std::move(*parsed);
}

bool JsonEquals(
    const specforge::JsonValue& left,
    const specforge::JsonValue& right)
{
    if (left.kind != right.kind) {
        return false;
    }
    switch (left.kind) {
    case specforge::JsonValue::Kind::Null:
        return true;
    case specforge::JsonValue::Kind::Object:
        if (left.object.size() != right.object.size()) {
            return false;
        }
        for (const auto& [key, value] : left.object) {
            const auto found = right.object.find(key);
            if (found == right.object.end() ||
                !JsonEquals(value, found->second)) {
                return false;
            }
        }
        return true;
    case specforge::JsonValue::Kind::Array:
        if (left.array.size() != right.array.size()) {
            return false;
        }
        for (std::size_t index = 0; index < left.array.size(); ++index) {
            if (!JsonEquals(left.array[index], right.array[index])) {
                return false;
            }
        }
        return true;
    case specforge::JsonValue::Kind::String:
        return left.string_value == right.string_value;
    case specforge::JsonValue::Kind::Bool:
        return left.bool_value == right.bool_value;
    case specforge::JsonValue::Kind::Integer:
        return left.integer_value == right.integer_value;
    }
    return false;
}

specforge::JsonValue SemanticSummary(
    const specforge::SampleLabelingDocument& document)
{
    specforge::JsonValue sample_names = specforge::JsonArrayValue();
    sample_names.array.reserve(document.source.roster.sample_names.size());
    for (const std::string& name : document.source.roster.sample_names) {
        sample_names.array.push_back(specforge::JsonStringValue(name));
    }

    specforge::JsonValue labels = specforge::JsonArrayValue();
    labels.array.reserve(document.labeling.labels.size());
    for (const specforge::SampleLabelingDocumentLabel& label :
        document.labeling.labels) {
        labels.array.push_back(specforge::JsonObjectValue({
            {"code", specforge::JsonIntegerValue(label.code)},
            {"name", specforge::JsonStringValue(label.name)},
            {"shortcut", specforge::JsonStringValue(label.shortcut)},
        }));
    }

    specforge::JsonValue values = specforge::JsonArrayValue();
    values.array.reserve(document.annotation.values.size());
    for (const std::int32_t value : document.annotation.values) {
        values.array.push_back(specforge::JsonIntegerValue(value));
    }

    return specforge::JsonObjectValue({
        {"format_kind", specforge::JsonStringValue(document.format_kind)},
        {"schema_version", specforge::JsonStringValue(document.schema_version)},
        {"source_kind", specforge::JsonStringValue(document.source.kind)},
        {"source_name", specforge::JsonStringValue(document.source.name)},
        {"source_identity", specforge::JsonStringValue(document.source.base_identity)},
        {"source_fingerprint", specforge::JsonStringValue(document.source.fingerprint)},
        {"sample_count", specforge::JsonIntegerValue(
             static_cast<std::int64_t>(document.source.sample_count))},
        {"roster_identity_kind", specforge::JsonStringValue(
             document.source.roster.identity_kind)},
        {"sample_names", std::move(sample_names)},
        {"annotation_kind", specforge::JsonStringValue(document.annotation.kind)},
        {"annotation_name", specforge::JsonStringValue(document.annotation.name)},
        {"missing_semantic", specforge::JsonStringValue(
             document.annotation.missing.semantic)},
        {"missing_value", specforge::JsonIntegerValue(
             document.annotation.missing.value)},
        {"task_id", specforge::JsonStringValue(document.labeling.id)},
        {"task_name", specforge::JsonStringValue(document.labeling.name)},
        {"labels", std::move(labels)},
        {"values", std::move(values)},
        {"values_dtype", specforge::JsonStringValue("int32")},
        {"values_shape", specforge::JsonArrayValue({
             specforge::JsonIntegerValue(static_cast<std::int64_t>(
                 document.annotation.values.size()))})},
    });
}

void WriteAllBytes(
    const std::filesystem::path& path,
    std::span<const unsigned char> bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    Require(output.good(), "test output should open");
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    Require(output.good(), "test output should be written completely");
}

template <typename UInt>
UInt ReadBigEndian(std::span<const unsigned char> bytes, std::size_t offset)
{
    Require(
        offset <= bytes.size() && sizeof(UInt) <= bytes.size() - offset,
        "raw ASDF test parser should stay in bounds");
    UInt value = 0;
    for (std::size_t index = 0; index < sizeof(UInt); ++index) {
        value = static_cast<UInt>((value << 8U) | bytes[offset + index]);
    }
    return value;
}

void AppendBigEndianBytes(
    std::vector<unsigned char>& bytes,
    std::uint64_t value,
    std::size_t width)
{
    for (std::size_t index = width; index > 0; --index) {
        bytes.push_back(static_cast<unsigned char>(
            (value >> ((index - 1U) * 8U)) & 0xffU));
    }
}

void AppendCorruptZlibBlock(std::vector<unsigned char>& bytes)
{
    bytes.insert(bytes.end(), {0xd3, 'B', 'L', 'K'});
    AppendBigEndianBytes(bytes, 48, 2);
    AppendBigEndianBytes(bytes, 0, 4);
    bytes.insert(bytes.end(), {'z', 'l', 'i', 'b'});
    AppendBigEndianBytes(bytes, 1, 8);
    AppendBigEndianBytes(bytes, 1, 8);
    AppendBigEndianBytes(bytes, 4, 8);
    bytes.insert(bytes.end(), 16, 0);
    bytes.push_back(0);
}

struct RawBlock {
    std::size_t offset = 0;
    std::size_t size = 0;
    std::size_t payload_offset = 0;
    std::array<unsigned char, 4> compression{};
    std::array<unsigned char, 16> checksum{};
};

struct RawAsdf {
    std::size_t tree_end = 0;
    std::vector<RawBlock> blocks;
};

RawAsdf ParseRawAsdf(const std::vector<unsigned char>& bytes)
{
    constexpr std::string_view terminator = "\n...\n";
    const auto marker = std::search(
        bytes.begin(),
        bytes.end(),
        terminator.begin(),
        terminator.end());
    Require(marker != bytes.end(), "raw ASDF should contain a YAML terminator");
    RawAsdf parsed;
    parsed.tree_end = static_cast<std::size_t>(marker - bytes.begin()) +
        terminator.size();
    std::size_t offset = parsed.tree_end;
    while (offset < bytes.size()) {
        while (offset < bytes.size() &&
               (bytes[offset] == 0 ||
                std::isspace(static_cast<unsigned char>(bytes[offset])) != 0)) {
            ++offset;
        }
        if (offset == bytes.size()) {
            break;
        }
        Require(
            offset + 6 <= bytes.size() && bytes[offset] == 0xd3 &&
                bytes[offset + 1] == 'B' && bytes[offset + 2] == 'L' &&
                bytes[offset + 3] == 'K',
            "raw ASDF should contain only internal blocks");
        const std::uint16_t header_size =
            ReadBigEndian<std::uint16_t>(bytes, offset + 4);
        Require(header_size >= 48, "raw ASDF block header should be standard-sized");
        const std::size_t fields = offset + 6;
        Require(
            fields + header_size <= bytes.size(),
            "raw ASDF block header should fit");
        const std::uint64_t allocated =
            ReadBigEndian<std::uint64_t>(bytes, fields + 8);
        Require(
            allocated <= std::numeric_limits<std::size_t>::max() &&
                allocated <= bytes.size() - fields - header_size,
            "raw ASDF block payload should fit");
        RawBlock block;
        block.offset = offset;
        block.size = 6 + header_size + static_cast<std::size_t>(allocated);
        block.payload_offset = fields + header_size;
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(fields + 4), 4, block.compression.begin());
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(fields + 32), 16, block.checksum.begin());
        parsed.blocks.push_back(block);
        offset += block.size;
    }
    return parsed;
}

void AppendText(std::vector<unsigned char>& bytes, std::string_view text)
{
    bytes.insert(bytes.end(), text.begin(), text.end());
}

void ReplaceTextOnce(std::vector<unsigned char>& bytes,
    std::string_view old_text,
    std::string_view new_text)
{
    const auto found = std::search(
        bytes.begin(), bytes.end(), old_text.begin(), old_text.end());
    Require(found != bytes.end(), "ASDF test patch target should exist");
    const std::size_t offset = static_cast<std::size_t>(found - bytes.begin());
    bytes.erase(found, found + static_cast<std::ptrdiff_t>(old_text.size()));
    bytes.insert(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        new_text.begin(),
        new_text.end());
}

void StoreBigEndian(std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::uint64_t value,
    std::size_t width)
{
    Require(
        offset <= bytes.size() && width <= bytes.size() - offset,
        "ASDF test integer patch should stay in bounds");
    for (std::size_t index = 0; index < width; ++index) {
        const std::size_t shift = 8U * (width - index - 1U);
        bytes[offset + index] =
            static_cast<unsigned char>((value >> shift) & 0xffU);
    }
}

void SetZlibFlevel(std::vector<unsigned char>& bytes,
    const RawBlock& block,
    unsigned int flevel)
{
    Require(flevel <= 3U, "zlib FLEVEL test value should fit two bits");
    Require(
        block.compression ==
                std::array<unsigned char, 4>{'z', 'l', 'i', 'b'} &&
            block.payload_offset + 2U <= bytes.size(),
        "zlib FLEVEL patch should target a zlib stream header");
    const unsigned int cmf = bytes[block.payload_offset];
    const unsigned int fdict = bytes[block.payload_offset + 1U] & 0x20U;
    const unsigned int prefix = (flevel << 6U) | fdict;
    for (unsigned int fcheck = 0; fcheck < 32U; ++fcheck) {
        const unsigned int flg = prefix | fcheck;
        if (((cmf << 8U) | flg) % 31U == 0U) {
            bytes[block.payload_offset + 1U] =
                static_cast<unsigned char>(flg);
            return;
        }
    }
    throw std::runtime_error("could not construct a valid zlib FLG byte");
}

unsigned int ZlibFlevel(
    const std::vector<unsigned char>& bytes,
    const RawBlock& block)
{
    Require(
        block.compression ==
                std::array<unsigned char, 4>{'z', 'l', 'i', 'b'} &&
            block.payload_offset + 2U <= bytes.size(),
        "zlib FLEVEL read should target a zlib stream header");
    return bytes[block.payload_offset + 1U] >> 6U;
}

std::string StandardBlockIndex(const RawAsdf& raw)
{
    std::ostringstream index;
    index << "#ASDF BLOCK INDEX\n%YAML 1.1\n---\n";
    for (const RawBlock& block : raw.blocks) {
        index << "- " << block.offset << "\n";
    }
    index << "...\n";
    return index.str();
}

void CorruptFirstBlockPayload(const std::filesystem::path& path)
{
    const RawAsdf raw = ParseRawAsdf(ReadAllBytes(path));
    Require(!raw.blocks.empty(), "source-mutation fixture should contain a block");
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    Require(file.good(), "source-mutation fixture should reopen for patching");
    file.seekg(static_cast<std::streamoff>(raw.blocks[0].payload_offset));
    char byte = 0;
    file.read(&byte, 1);
    Require(file.good(), "source-mutation fixture payload should be readable");
    byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0xffU);
    file.seekp(static_cast<std::streamoff>(raw.blocks[0].payload_offset));
    file.write(&byte, 1);
    Require(file.good(), "source-mutation fixture payload should be patchable");
}

specforge::SampleLabelingDocument ProductionDocument(bool explicit_roster = true)
{
    specforge::SampleLabelingDocument document;
    document.source.base_identity = "sha256-v1:production-source";
    document.source.kind = "folder";
    document.source.name = "巡天样本";
    document.source.fingerprint = "sha256-v1:production-fingerprint";
    document.source.sample_count = 3;
    if (explicit_roster) {
        document.source.roster.identity_kind =
            std::string{specforge::kSampleLabelingDocumentExplicitNamesRoster};
        document.source.roster.sample_names =
            {"alpha.fits", "星系-β.fits", "échelle-γ.fits"};
    }
    document.annotation.name = "天体分类";
    document.annotation.values = {-1, 0, 1};
    document.labeling.id = "task-alpha";
    document.labeling.name = "天体分类";
    document.labeling.labels = {
        {0, "Galaxy", "g"},
        {1, "Quasar", "q"}};
    return document;
}

void AppendUtf8ForTest(std::string& output, std::uint32_t codepoint)
{
    if (codepoint <= 0x7fU) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        output.push_back(
            static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
}

std::string YamlSpecialCharacterMatrix()
{
    std::string result =
        "yaml-indicators:-?:,[]{}#&*!|>'\"%@`/\\ space ";
    for (std::uint32_t codepoint = 0; codepoint <= 0x1fU; ++codepoint) {
        AppendUtf8ForTest(result, codepoint);
    }
    for (std::uint32_t codepoint = 0x7fU; codepoint <= 0x9fU;
        ++codepoint) {
        AppendUtf8ForTest(result, codepoint);
    }
    for (const std::uint32_t codepoint :
        {0xa0U, 0x2028U, 0x2029U, 0xfeffU, 0xfffeU, 0xffffU}) {
        AppendUtf8ForTest(result, codepoint);
    }
    return result;
}

specforge::SampleLabelingAsdfWriteResult WriteDocument(
    const std::filesystem::path& path,
    const specforge::SampleLabelingDocument& document)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    Require(output.good(), "test output should open");
    const specforge::SampleLabelingAsdfWriteResult result =
        specforge::WriteSampleLabelingAsdfDocument(output, document);
    output.close();
    return result;
}

void WriteYamlAliasAmplificationFixture(
    const std::filesystem::path& path,
    std::size_t label_count,
    std::size_t shared_name_bytes)
{
    const std::filesystem::path block_source =
        TempPath("_alias_block_source.asdf");
    Require(
        WriteDocument(block_source, ProductionDocument(false)).succeeded(),
        "YAML alias fixture values block should be written");
    const std::vector<unsigned char> source_bytes = ReadAllBytes(block_source);
    const RawAsdf source_raw = ParseRawAsdf(source_bytes);
    Require(
        source_raw.blocks.size() == 1,
        "YAML alias fixture should reuse one source-index values block");

    std::ostringstream metadata;
    metadata << "#ASDF 1.0.0\n"
             << "#ASDF_STANDARD 1.5.0\n"
             << "%YAML 1.1\n"
             << "%TAG ! tag:stsci.edu:asdf/\n"
             << "--- !core/asdf-1.1.0\n"
             << "format_kind: \"specforge.sample_labeling\"\n"
             << "schema_version: \"1.0.0\"\n"
             << "source_collection:\n"
             << "  identity: \"source:yaml-alias-budget\"\n"
             << "  source_kind: \"npy\"\n"
             << "  name: \"alias budget\"\n"
             << "  fingerprint: \"sha256:yaml-alias-budget\"\n"
             << "  sample_count: 3\n"
             << "sample_roster:\n"
             << "  identity_kind: \"source_index\"\n"
             << "annotation:\n"
             << "  kind: \"categorical_integer\"\n"
             << "  name: \"Alias budget\"\n"
             << "  values: !core/ndarray-1.0.0\n"
             << "    source: 0\n"
             << "    datatype: int32\n"
             << "    byteorder: little\n"
             << "    shape: [3]\n"
             << "  missing:\n"
             << "    semantic: \"unlabeled\"\n"
             << "    value: -1\n"
             << "labeling_task:\n"
             << "  id: \"yaml-alias-budget\"\n"
             << "  name: \"Alias budget\"\n"
             << "  labels:\n";
    for (std::size_t index = 0; index < label_count; ++index) {
        metadata << "  - code: " << index << "\n"
                 << "    name: ";
        if (index == 0) {
            metadata << "&shared_label_name \""
                     << std::string(shared_name_bytes, 'x') << "\"\n";
        } else {
            metadata << "*shared_label_name\n";
        }
    }
    metadata << "...\n";

    const std::string metadata_bytes = metadata.str();
    std::vector<unsigned char> output(
        metadata_bytes.begin(), metadata_bytes.end());
    output.insert(output.end(),
        source_bytes.begin() + static_cast<std::ptrdiff_t>(
            source_raw.blocks[0].offset),
        source_bytes.begin() + static_cast<std::ptrdiff_t>(
            source_raw.blocks[0].offset + source_raw.blocks[0].size));
    WriteAllBytes(path, output);

    std::error_code cleanup_error;
    std::filesystem::remove(block_source, cleanup_error);
}

bool g_metadata_build_observed = false;

void ObserveMetadataBuild()
{
    g_metadata_build_observed = true;
}

void TestYamlScalarMaterializationUsesResidentPreflight()
{
    constexpr std::uint64_t reader_budget = 600ULL * 1024ULL;
    constexpr std::uint64_t writer_budget = 1792ULL * 1024ULL;
    constexpr std::size_t label_count = 1024U;
    constexpr std::size_t label_name_bytes = 512U;

    const std::filesystem::path path = TempPath("_yaml_alias_budget.asdf");
    WriteYamlAliasAmplificationFixture(
        path, label_count, label_name_bytes);
    const specforge::SampleLabelingAsdfReadResult read =
        specforge::sample_labeling_asdf_test_seam::ReadWithResidentBudget(
            path, reader_budget);
    Require(
        !read.succeeded() &&
            read.error.kind ==
                specforge::SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
        "YAML alias-expanded canonical text must obey the shared resident budget");

    specforge::SampleLabelingDocument document = ProductionDocument(false);
    document.labeling.labels.clear();
    document.labeling.labels.reserve(label_count);
    for (std::size_t index = 0; index < label_count; ++index) {
        document.labeling.labels.push_back({
            static_cast<std::int32_t>(index),
            std::string(label_name_bytes, 'x'),
            {}});
    }
    document.annotation.values = {0, 1, 2};
    g_metadata_build_observed = false;
    std::ostringstream output(std::ios::binary);
    const specforge::SampleLabelingAsdfWriteResult write =
        specforge::sample_labeling_asdf_test_seam::WriteWithResidentBudget(
            output,
            document,
            writer_budget,
            &ObserveMetadataBuild);
    Require(
        !write.succeeded() &&
            write.error.kind ==
                specforge::SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
        "writer canonical text and escaped metadata must obey the shared resident budget");
    Require(
        !g_metadata_build_observed && output.str().empty(),
        "writer must reject metadata expansion before constructing or emitting it");

    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
}

void TestRejectsDuplicateYamlMappingKeys()
{
    struct DuplicateCase {
        std::string_view suffix;
        std::string_view old_text;
        std::string_view new_text;
    };
    constexpr std::array cases{
        DuplicateCase{"_duplicate_known_key.asdf",
            "  sample_count: 3\n",
            "  sample_count: 3\n  sample_count: 4\n"},
        DuplicateCase{"_duplicate_unknown_nested_key.asdf",
            "labeling_task:\n",
            "forward_unknown:\n  nested:\n    repeated: 1\n"
            "    repeated: 2\nlabeling_task:\n"},
    };

    for (const DuplicateCase& duplicate : cases) {
        const std::filesystem::path path = TempPath(duplicate.suffix);
        Require(
            WriteDocument(path, ProductionDocument(false)).succeeded(),
            "duplicate-key source fixture should be written");
        std::vector<unsigned char> bytes = ReadAllBytes(path);
        ReplaceTextOnce(bytes, duplicate.old_text, duplicate.new_text);
        WriteAllBytes(path, bytes);

        const specforge::SampleLabelingAsdfReadResult read =
            specforge::ReadSampleLabelingAsdfDocument(path);
        Require(
            !read.succeeded() && !read.durable_base.has_value() &&
                read.error.kind ==
                    specforge::SampleLabelingAsdfErrorKind::MalformedDocument,
            "duplicate YAML mapping keys must be rejected before a durable "
            "base is created");

        std::error_code cleanup_error;
        std::filesystem::remove(path, cleanup_error);
    }
}

class ThrowingReadBuffer final : public std::streambuf {
protected:
    std::streamsize xsgetn(char*, std::streamsize) override
    {
        throw std::ios_base::failure("simulated device read failure");
    }
};

void TestExactReadDistinguishesTruncationFromIoFailure()
{
    std::istringstream truncated("ab", std::ios::binary);
    const specforge::SampleLabelingAsdfError truncated_error =
        specforge::sample_labeling_asdf_test_seam::ProbeExactRead(
            truncated, 4);
    Require(
        truncated_error.kind ==
            specforge::SampleLabelingAsdfErrorKind::MalformedDocument,
        "clean EOF should remain a malformed truncated document");

    ThrowingReadBuffer failing_buffer;
    std::istream failing_input(&failing_buffer);
    const specforge::SampleLabelingAsdfError io_error =
        specforge::sample_labeling_asdf_test_seam::ProbeExactRead(
            failing_input, 4);
    Require(
        io_error.kind == specforge::SampleLabelingAsdfErrorKind::IoFailure,
        "badbit short reads should be reported as I/O failures");
}

void TestReadsApprovedPythonFixtures()
{
    const specforge::JsonValue manifest =
        ReadJsonFile(FixturePath("manifest.json"));
    const specforge::JsonValue* reference =
        specforge::JsonObjectMember(manifest, "reference");
    Require(
        reference != nullptr &&
            specforge::ReadJsonStringMember(*reference, "asdf") == "5.3.1" &&
            specforge::ReadJsonStringMember(
                *reference, "asdf_standard_version") == "1.5.0" &&
            specforge::ReadJsonStringMember(
                *reference, "schema_version") ==
                specforge::kSampleLabelingDocumentSchemaVersion,
        "fixture manifest should pin the approved ASDF oracle and standard");
    const specforge::JsonValue* fixtures =
        specforge::JsonObjectMember(manifest, "fixtures");
    Require(
        fixtures != nullptr &&
            fixtures->kind == specforge::JsonValue::Kind::Array,
        "fixture manifest should contain fixture records");

    std::size_t approved_count = 0;
    for (const specforge::JsonValue& fixture : fixtures->array) {
        const std::optional<std::string> path =
            specforge::ReadJsonStringMember(fixture, "path");
        const std::optional<std::string> expected_hash =
            specforge::ReadJsonStringMember(fixture, "sha256");
        Require(
            path.has_value() && expected_hash.has_value() &&
                expected_hash->size() == 64U,
            "every ASDF fixture should carry a manifest SHA-256");
        std::string hash_error;
        const std::optional<std::string> actual_hash =
            specforge::ComputeFileSha256(FixturePath(*path), &hash_error);
        Require(
            actual_hash.has_value() && *actual_hash == *expected_hash,
            std::string("ASDF fixture bytes must match the manifest SHA-256: ") +
                *path + (hash_error.empty() ? std::string{}
                                            : " (" + hash_error + ")"));

        if (!specforge::ReadJsonBoolMember(
                fixture, "structurally_valid", false) ||
            !specforge::ReadJsonBoolMember(
                fixture, "semantically_valid", false) ||
            !specforge::ReadJsonBoolMember(
                fixture, "native_profile_supported", false)) {
            continue;
        }
        const std::optional<std::string> semantic_path =
            specforge::ReadJsonStringMember(fixture, "semantic_path");
        Require(
            path.has_value() && semantic_path.has_value() &&
                !semantic_path->empty(),
            "approved fixture should name its ASDF and semantic oracle");

        const specforge::SampleLabelingAsdfReadResult result =
            specforge::ReadSampleLabelingAsdfDocument(FixturePath(*path));
        Require(result.succeeded(), "approved Python ASDF fixture should read");
        const specforge::JsonValue expected =
            ReadJsonFile(FixturePath(*semantic_path));
        Require(
            JsonEquals(SemanticSummary(*result.document), expected),
            std::string("approved fixture should equal its complete semantic oracle: ") +
                *path);
        ++approved_count;
    }
    Require(approved_count > 0, "manifest should select approved native fixtures");
}

void TestRejectsSemanticViolationsWithControlledErrors()
{
    constexpr std::array fixtures{
        "invalid_count.asdf",
        "invalid_duplicate_label.asdf",
        "invalid_sentinel_collision.asdf",
        "invalid_undefined_value.asdf"};
    for (const std::string_view fixture : fixtures) {
        const specforge::SampleLabelingAsdfReadResult result =
            specforge::ReadSampleLabelingAsdfDocument(FixturePath(fixture));
        Require(!result.succeeded(), "semantic-invalid fixture should be rejected");
        Require(
            result.error.kind ==
                specforge::SampleLabelingAsdfErrorKind::SemanticValidationFailed,
            "semantic-invalid fixture should return a typed semantic error");
        Require(!result.error.message.empty(), "semantic error should carry a diagnostic");
    }
}

void TestRejectsMalformedCorruptAndUnsupportedInputs()
{
    constexpr std::array malformed{
        "malformed_bad_magic.asdf",
        "malformed_yaml.asdf",
        "malformed_truncated_block.asdf",
        "malformed_zlib_payload.asdf"};
    for (const std::string_view fixture : malformed) {
        const specforge::SampleLabelingAsdfReadResult result =
            specforge::ReadSampleLabelingAsdfDocument(FixturePath(fixture));
        Require(!result.succeeded(), "malformed fixture should be rejected");
        Require(
            result.error.kind != specforge::SampleLabelingAsdfErrorKind::None,
            "malformed fixture should return a controlled typed error");
    }

    const specforge::SampleLabelingAsdfReadResult checksum =
        specforge::ReadSampleLabelingAsdfDocument(
            FixturePath("profile_checksum.asdf"));
    Require(!checksum.succeeded(), "checksummed profile should be rejected");
    Require(
        checksum.error.kind ==
            specforge::SampleLabelingAsdfErrorKind::UnsupportedProfile,
        "nonzero checksum should be an unsupported-profile error");

    const specforge::SampleLabelingAsdfReadResult missing =
        specforge::ReadSampleLabelingAsdfDocument(
            TempPath("_missing.asdf"));
    Require(!missing.succeeded(), "missing input should fail without throwing");
    Require(
        missing.error.kind == specforge::SampleLabelingAsdfErrorKind::OpenFailed,
        "missing input should return an open error");
}

void TestRejectsNdarrayMaskProfile()
{
    for (const bool roster_mask : {false, true}) {
        const std::filesystem::path path = TempPath(
            roster_mask ? "_roster_mask.asdf" : "_values_mask.asdf");
        Require(
            WriteDocument(path, ProductionDocument(roster_mask)).succeeded(),
            "ndarray-mask input should be written");
        std::vector<unsigned char> bytes = ReadAllBytes(path);
        if (roster_mask) {
            ReplaceTextOnce(bytes,
                "    byteorder: little\n",
                "    mask: 0\n    byteorder: little\n");
        } else {
            ReplaceTextOnce(bytes,
                "    datatype: int32\n",
                "    datatype: int32\n    mask: -1\n");
        }
        WriteAllBytes(path, bytes);

        const specforge::SampleLabelingAsdfReadResult read =
            specforge::ReadSampleLabelingAsdfDocument(path);
        Require(!read.succeeded(), "v1 must reject every ndarray mask");
        Require(
            read.error.kind ==
                specforge::SampleLabelingAsdfErrorKind::UnsupportedProfile,
            "ndarray mask should return a controlled profile error");
        Require(
            !read.durable_base,
            "unsupported ndarray mask must not produce a durable base");

        std::error_code cleanup_error;
        std::filesystem::remove(path, cleanup_error);
    }
}

void TestRejectsUnreferencedAndOutOfOrderBlocks()
{
    for (const bool explicit_roster : {false, true}) {
        const std::filesystem::path path = TempPath(
            explicit_roster ? "_extra_explicit_block.asdf"
                            : "_extra_index_block.asdf");
        Require(
            WriteDocument(path, ProductionDocument(explicit_roster)).succeeded(),
            "extra-block input should be written");
        std::vector<unsigned char> bytes = ReadAllBytes(path);
        AppendCorruptZlibBlock(bytes);
        WriteAllBytes(path, bytes);

        const specforge::SampleLabelingAsdfReadResult read =
            specforge::ReadSampleLabelingAsdfDocument(path);
        Require(
            !read.succeeded(),
            "fixed ASDF profile must reject every unreferenced extra block");
        Require(
            read.error.kind ==
                specforge::SampleLabelingAsdfErrorKind::UnsupportedProfile,
            "extra blocks should return a controlled unsupported-profile error");

        std::error_code cleanup_error;
        std::filesystem::remove(path, cleanup_error);
    }

    specforge::SampleLabelingDocument document = ProductionDocument();
    document.source.roster.sample_names = {"A", "B", "C"};
    document.annotation.values = {65, 66, 67};
    document.labeling.labels = {
        {65, "A", "a"},
        {66, "B", "b"},
        {67, "C", "c"}};
    const std::filesystem::path order_path =
        TempPath("_out_of_order_sources.asdf");
    Require(
        WriteDocument(order_path, document).succeeded(),
        "source-order input should be written");
    std::vector<unsigned char> bytes = ReadAllBytes(order_path);
    const std::string_view text(
        reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const std::size_t roster_source = text.find("    source: 0");
    const std::size_t values_source = text.find("    source: 1", roster_source);
    Require(
        roster_source != std::string_view::npos &&
            values_source != std::string_view::npos,
        "source-order metadata entries should exist");
    constexpr std::size_t source_value_offset =
        std::string_view("    source: ").size();
    bytes[roster_source + source_value_offset] = '1';
    bytes[values_source + source_value_offset] = '0';
    WriteAllBytes(order_path, bytes);

    const specforge::SampleLabelingAsdfReadResult out_of_order =
        specforge::ReadSampleLabelingAsdfDocument(order_path);
    Require(
        !out_of_order.succeeded(),
        "fixed ASDF profile must reject out-of-order block sources");
    Require(
        out_of_order.error.kind ==
            specforge::SampleLabelingAsdfErrorKind::UnsupportedProfile,
        "out-of-order sources should return a controlled profile error");

    std::error_code cleanup_error;
    std::filesystem::remove(order_path, cleanup_error);
}

void TestRejectsMalformedBlockIndexes()
{
    const std::filesystem::path valid_path = TempPath("_valid_index.asdf");
    Require(
        WriteDocument(valid_path, ProductionDocument()).succeeded(),
        "block-index input should be written");
    std::vector<unsigned char> valid_bytes = ReadAllBytes(valid_path);
    AppendText(valid_bytes, StandardBlockIndex(ParseRawAsdf(valid_bytes)));
    WriteAllBytes(valid_path, valid_bytes);
    Require(
        specforge::ReadSampleLabelingAsdfDocument(valid_path).succeeded(),
        "a standard block index should remain readable");

    const std::array malformed_indexes{
        std::string{"#ASDF BLOCK INDEX"},
        std::string{"#ASDF BLOCK INDEX garbage\n%YAML 1.1\n---\n- 0\n...\n"},
        std::string{"#ASDF BLOCK INDEX\n%YAML 1.1\n---\n- 0\n...\n"}};
    for (std::size_t index = 0; index < malformed_indexes.size(); ++index) {
        const std::filesystem::path path =
            TempPath("_malformed_index_" + std::to_string(index) + ".asdf");
        Require(
            WriteDocument(path, ProductionDocument()).succeeded(),
            "malformed block-index base should be written");
        std::vector<unsigned char> bytes = ReadAllBytes(path);
        AppendText(bytes, malformed_indexes[index]);
        WriteAllBytes(path, bytes);

        const specforge::SampleLabelingAsdfReadResult read =
            specforge::ReadSampleLabelingAsdfDocument(path);
        Require(!read.succeeded(), "malformed block index must be rejected");
        Require(
            read.error.kind ==
                specforge::SampleLabelingAsdfErrorKind::MalformedDocument,
            "malformed block index should return a controlled format error");
        std::error_code cleanup_error;
        std::filesystem::remove(path, cleanup_error);
    }

    std::error_code cleanup_error;
    std::filesystem::remove(valid_path, cleanup_error);
}

void TestDurableBaseUsesTheValidatedPrefixSnapshot()
{
    const std::filesystem::path path = TempPath("_source_mutation.asdf");
    Require(
        WriteDocument(path, ProductionDocument()).succeeded(),
        "source-mutation input should be written");
    const specforge::SampleLabelingAsdfReadResult read =
        specforge::sample_labeling_asdf_test_seam::ReadWithBeforePrefixCapture(
            path, CorruptFirstBlockPayload);
    Require(
        !read.succeeded(),
        "a roster corrupted before prefix capture must not produce a document");
    Require(
        !read.durable_base,
        "a roster corrupted before prefix capture must not produce a durable base");

    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
}

void TestReaderEnforcesCombinedResidentMemoryBudget()
{
    constexpr std::uint64_t declared_count = 70'000'000ULL;
    const std::filesystem::path path = TempPath("_resident_budget.asdf");
    Require(
        WriteDocument(path, ProductionDocument(false)).succeeded(),
        "resident-budget input should be written");
    std::vector<unsigned char> bytes = ReadAllBytes(path);
    ReplaceTextOnce(bytes,
        "  sample_count: 3\n",
        "  sample_count: 70000000\n");
    ReplaceTextOnce(
        bytes, "    shape: [3]\n", "    shape: [70000000]\n");
    const RawAsdf raw = ParseRawAsdf(bytes);
    Require(raw.blocks.size() == 1, "resident-budget input should use one block");
    StoreBigEndian(bytes,
        raw.blocks[0].offset + 6U + 24U,
        declared_count * sizeof(std::int32_t),
        8);
    WriteAllBytes(path, bytes);

    const specforge::SampleLabelingAsdfReadResult read =
        specforge::ReadSampleLabelingAsdfDocument(path);
    Require(!read.succeeded(), "stacked reader allocations must be bounded");
    Require(
        read.error.kind ==
            specforge::SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
        "combined allocation pressure should return a resource-limit error");

    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
}

void TestSharedProfilePreflightBoundaryMatrix()
{
    using specforge::SampleLabelingAsdfErrorKind;
    using specforge::sample_labeling_asdf_test_seam::ProfilePreflightProbe;

    constexpr std::size_t metadata_limit = 8ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t resident_limit = 512ULL * 1024ULL * 1024ULL;
    constexpr std::size_t resident_values_count =
        static_cast<std::size_t>(resident_limit /
            (2ULL * sizeof(std::int32_t)));
    constexpr std::uint64_t legacy_roster_bytes_per_sample =
        2ULL * 2ULL * sizeof(std::uint32_t) + sizeof(std::string) +
        2ULL * sizeof(std::int32_t);
    constexpr std::size_t roster_dedup_pressure_count =
        static_cast<std::size_t>(
            resident_limit / legacy_roster_bytes_per_sample);
    constexpr std::size_t label_scratch_count = 100'000U;
    constexpr std::size_t label_scratch_pressure_values_count =
        static_cast<std::size_t>((resident_limit -
            label_scratch_count * sizeof(std::size_t) / 2U) /
            (2ULL * sizeof(std::int32_t)));

    struct BoundaryCase {
        std::string_view name;
        ProfilePreflightProbe probe;
        SampleLabelingAsdfErrorKind expected;
    };
    const std::array cases{
        BoundaryCase{"baseline", {}, SampleLabelingAsdfErrorKind::None},
        BoundaryCase{"metadata-at-limit",
            {.metadata_bytes = metadata_limit},
            SampleLabelingAsdfErrorKind::None},
        BoundaryCase{"metadata-over-limit",
            {.metadata_bytes = metadata_limit + 1U},
            SampleLabelingAsdfErrorKind::ResourceLimitExceeded},
        BoundaryCase{"labels-at-limit",
            {.label_count = 100'000U},
            SampleLabelingAsdfErrorKind::None},
        BoundaryCase{"labels-over-limit",
            {.label_count = 100'001U},
            SampleLabelingAsdfErrorKind::ResourceLimitExceeded},
        BoundaryCase{"label-validation-scratch-over-resident-limit",
            {.source_sample_count = label_scratch_pressure_values_count,
                .values_count = label_scratch_pressure_values_count,
                .label_count = label_scratch_count},
            SampleLabelingAsdfErrorKind::ResourceLimitExceeded},
        BoundaryCase{"roster-width-at-limit",
            {.explicit_roster = true,
                .roster_string_width = 1'000'000U},
            SampleLabelingAsdfErrorKind::None},
        BoundaryCase{"roster-width-over-limit",
            {.explicit_roster = true,
                .roster_string_width = 1'000'001U},
            SampleLabelingAsdfErrorKind::ResourceLimitExceeded},
        BoundaryCase{"roster-width-zero",
            {.explicit_roster = true, .roster_string_width = 0},
            SampleLabelingAsdfErrorKind::ResourceLimitExceeded},
        BoundaryCase{"values-shape-mismatch",
            {.source_sample_count = 1},
            SampleLabelingAsdfErrorKind::SemanticValidationFailed},
        BoundaryCase{"roster-shape-mismatch",
            {.source_sample_count = 1,
                .explicit_roster = true,
                .values_count = 1},
            SampleLabelingAsdfErrorKind::SemanticValidationFailed},
        BoundaryCase{"sample-count-over-limit",
            {.source_sample_count = 100'000'001U,
                .values_count = 100'000'001U},
            SampleLabelingAsdfErrorKind::ResourceLimitExceeded},
        BoundaryCase{"decoded-roster-block-over-limit",
            {.source_sample_count = 135,
                .explicit_roster = true,
                .roster_sample_count = 135,
                .roster_string_width = 1'000'000U,
                .values_count = 135},
            SampleLabelingAsdfErrorKind::ResourceLimitExceeded},
        BoundaryCase{"roster-dedup-scratch-over-resident-limit",
            {.source_sample_count = roster_dedup_pressure_count,
                .explicit_roster = true,
                .roster_sample_count = roster_dedup_pressure_count,
                .roster_string_width = 2,
                .values_count = roster_dedup_pressure_count},
            SampleLabelingAsdfErrorKind::ResourceLimitExceeded},
        BoundaryCase{"resident-at-limit",
            {.source_sample_count = resident_values_count,
                .values_count = resident_values_count},
            SampleLabelingAsdfErrorKind::None},
        BoundaryCase{"resident-over-limit",
            {.source_sample_count = resident_values_count + 1U,
                .values_count = resident_values_count + 1U},
            SampleLabelingAsdfErrorKind::ResourceLimitExceeded},
        BoundaryCase{"prefix-at-resident-limit",
            {.reusable_prefix_bytes = resident_limit},
            SampleLabelingAsdfErrorKind::None},
        BoundaryCase{"prefix-over-resident-limit",
            {.reusable_prefix_bytes = resident_limit + 1U},
            SampleLabelingAsdfErrorKind::ResourceLimitExceeded},
        BoundaryCase{"canonical-text-at-resident-limit",
            {.canonical_text_bytes = resident_limit},
            SampleLabelingAsdfErrorKind::None},
        BoundaryCase{"canonical-text-over-resident-limit",
            {.canonical_text_bytes = resident_limit + 1U},
            SampleLabelingAsdfErrorKind::ResourceLimitExceeded},
        BoundaryCase{"label-storage-at-resident-limit",
            {.label_storage_bytes = resident_limit},
            SampleLabelingAsdfErrorKind::None},
        BoundaryCase{"label-storage-over-resident-limit",
            {.label_storage_bytes = resident_limit + 1U},
            SampleLabelingAsdfErrorKind::ResourceLimitExceeded},
        BoundaryCase{"codec-scratch-at-resident-limit",
            {.codec_scratch_bytes = resident_limit},
            SampleLabelingAsdfErrorKind::None},
        BoundaryCase{"codec-scratch-over-resident-limit",
            {.codec_scratch_bytes = resident_limit + 1U},
            SampleLabelingAsdfErrorKind::ResourceLimitExceeded},
        BoundaryCase{"file-at-limit",
            {.file_bytes = 1024ULL * 1024ULL * 1024ULL},
            SampleLabelingAsdfErrorKind::None},
        BoundaryCase{"file-over-limit",
            {.file_bytes = 1024ULL * 1024ULL * 1024ULL + 1U},
            SampleLabelingAsdfErrorKind::ResourceLimitExceeded},
    };

    for (const BoundaryCase& boundary : cases) {
        const specforge::SampleLabelingAsdfError error =
            specforge::sample_labeling_asdf_test_seam::ProbeProfilePreflight(
                boundary.probe);
        Require(
            error.kind == boundary.expected,
            std::string("shared profile preflight boundary failed: ") +
                std::string(boundary.name));
    }
}

void TestWriterRejectsRosterNulBeforeOutput()
{
    const std::array names{
        std::string("a\0b", 3),
        std::string("a\0", 2)};
    for (const std::string& name : names) {
        specforge::SampleLabelingDocument document = ProductionDocument();
        document.source.roster.sample_names[0] = name;
        std::ostringstream output(std::ios::binary);
        const specforge::SampleLabelingAsdfWriteResult write =
            specforge::WriteSampleLabelingAsdfDocument(output, document);
        Require(
            !write.succeeded(),
            "U+0000 roster names must be rejected instead of lossy encoding");
        Require(
            write.error.kind ==
                specforge::SampleLabelingAsdfErrorKind::SemanticValidationFailed,
            "unrepresentable roster NUL should return a semantic error");
        Require(
            output.str().empty(),
            "roster NUL validation must fail before writing any bytes");
    }
}

void TestWriterRejectsInvalidUtf8BeforeOutput()
{
    specforge::SampleLabelingDocument document = ProductionDocument(false);
    document.source.name = std::string(1, static_cast<char>(0xc3));
    std::ostringstream output(std::ios::binary);
    const specforge::SampleLabelingAsdfWriteResult write =
        specforge::WriteSampleLabelingAsdfDocument(output, document);
    Require(!write.succeeded(), "invalid canonical UTF-8 must be rejected");
    Require(
        write.error.kind ==
            specforge::SampleLabelingAsdfErrorKind::MalformedDocument,
        "invalid canonical UTF-8 should return a controlled text error");
    Require(
        output.str().empty(),
        "invalid UTF-8 validation must fail before writer output begins");
}

void TestReaderRejectsInvalidUtf8InUnknownYamlBeforeHydration()
{
    const std::filesystem::path path = TempPath("_invalid_yaml_utf8.asdf");
    Require(
        WriteDocument(path, ProductionDocument(false)).succeeded(),
        "invalid-YAML-UTF-8 base should be written");
    std::vector<unsigned char> bytes = ReadAllBytes(path);
    constexpr std::string_view terminator = "...\n";
    const auto marker = std::search(bytes.begin(),
        bytes.end(),
        terminator.begin(),
        terminator.end());
    Require(marker != bytes.end(), "invalid-UTF-8 insertion point should exist");
    const std::array<unsigned char, 12> invalid_comment{
        '#', ' ', 'u', 'n', 'k', 'n', 'o', 'w', 'n', ':', 0xc3U, '\n'};
    bytes.insert(marker, invalid_comment.begin(), invalid_comment.end());
    WriteAllBytes(path, bytes);

    const specforge::SampleLabelingAsdfReadResult read =
        specforge::ReadSampleLabelingAsdfDocument(path);
    Require(
        !read.succeeded() &&
            read.error.kind ==
                specforge::SampleLabelingAsdfErrorKind::MalformedDocument,
        "invalid UTF-8 anywhere in YAML metadata must be rejected");

    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
}

void TestWriterEscapesYamlSpecialCharacterMatrixLosslessly()
{
    const std::string special_characters = YamlSpecialCharacterMatrix();
    specforge::SampleLabelingDocument document = ProductionDocument(false);
    document.source.name = special_characters;
    std::ostringstream output(std::ios::binary);
    const specforge::SampleLabelingAsdfWriteResult write =
        specforge::WriteSampleLabelingAsdfDocument(output, document);
    Require(
        write.succeeded(),
        "every YAML special character should be writable losslessly");
    Require(
        output.str().find("\\u007f") != std::string::npos &&
            output.str().find("\\u0080") != std::string::npos &&
            output.str().find("\\u0085") != std::string::npos &&
            output.str().find("\\u00a0") != std::string::npos &&
            output.str().find("\\u2028") != std::string::npos &&
            output.str().find("\\u2029") != std::string::npos &&
            output.str().find("\\ufeff") != std::string::npos &&
            output.str().find("\\ufffe") != std::string::npos &&
            output.str().find("\\uffff") != std::string::npos,
        "YAML writer must escape every non-printable and line-break scalar");

    const std::filesystem::path path = TempPath("_yaml_specials.asdf");
    const std::string bytes = output.str();
    WriteAllBytes(path,
        std::span<const unsigned char>(
            reinterpret_cast<const unsigned char*>(bytes.data()),
            bytes.size()));
    const specforge::SampleLabelingAsdfReadResult read =
        specforge::ReadSampleLabelingAsdfDocument(path);
    if (!read.succeeded()) {
        throw std::runtime_error(
            "YAML special matrix read failed: " + read.error.message);
    }
    if (read.succeeded() &&
        read.document->source.name != special_characters) {
        const std::string& actual = read.document->source.name;
        const std::size_t shared = std::min(
            actual.size(), special_characters.size());
        std::size_t difference = 0;
        while (difference < shared &&
               actual[difference] == special_characters[difference]) {
            ++difference;
        }
        std::ostringstream diagnostic;
        diagnostic << "YAML special mismatch at byte " << difference
                   << "; expected-size=" << special_characters.size()
                   << "; actual-size=" << actual.size();
        if (difference < shared) {
            diagnostic << "; expected="
                       << static_cast<unsigned int>(
                              static_cast<unsigned char>(
                                  special_characters[difference]))
                       << "; actual="
                       << static_cast<unsigned int>(
                              static_cast<unsigned char>(actual[difference]));
        }
        throw std::runtime_error(diagnostic.str());
    }
    Require(
        read.succeeded() &&
            read.document->source.name == special_characters,
        "native writer/reader round-trip must preserve all YAML specials");

    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
}

void TestWriterRejectsDocumentsOutsideReaderResidentBudget()
{
    constexpr std::size_t sample_count = 68;
    constexpr std::size_t roster_width = 1'000'000;
    specforge::SampleLabelingDocument document = ProductionDocument();
    document.source.sample_count = sample_count;
    document.source.roster.sample_names.clear();
    document.source.roster.sample_names.reserve(sample_count);
    for (std::size_t index = 0; index < sample_count; ++index) {
        std::string name(roster_width, 'x');
        name[0] = static_cast<char>('A' + index / 26U);
        name[1] = static_cast<char>('A' + index % 26U);
        document.source.roster.sample_names.push_back(std::move(name));
    }
    document.annotation.values.assign(
        sample_count, specforge::kSampleLabelingDocumentUnlabeledValue);

    std::ostringstream output(std::ios::binary);
    const specforge::SampleLabelingAsdfWriteResult write =
        specforge::WriteSampleLabelingAsdfDocument(output, document);
    Require(
        !write.succeeded(),
        "writer must reject a document that the reader resident budget rejects");
    Require(
        write.error.kind ==
            specforge::SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
        "writer/reader capacity mismatch should be a resource-limit error");
    Require(
        output.str().empty(),
        "reader-capacity validation must fail before writer output begins");
}

void TestWriterEmitsFixedProductionProfileAndRoundTrips()
{
    const std::filesystem::path path = TempPath("_profile.asdf");
    const specforge::SampleLabelingDocument document = ProductionDocument();
    const specforge::SampleLabelingAsdfWriteResult write =
        WriteDocument(path, document);
    Require(write.succeeded(), "production ASDF writer should succeed");

    const std::vector<unsigned char> bytes = ReadAllBytes(path);
    const std::string text(
        reinterpret_cast<const char*>(bytes.data()),
        std::min<std::size_t>(bytes.size(), 4096));
    Require(
        text.starts_with("#ASDF 1.0.0\n#ASDF_STANDARD 1.5.0\n"),
        "writer should pin the ASDF 1.0 / Standard 1.5 header");
    Require(
        text.find("--- !core/asdf-1.1.0") != std::string::npos &&
            text.find("!core/ndarray-1.0.0") != std::string::npos &&
            text.find("datatype: [ucs4, ") != std::string::npos &&
            text.find("datatype: int32") != std::string::npos &&
            text.find("byteorder: little") != std::string::npos,
        "writer should pin core tags, UCS-4/int32 arrays, and little endian");
    Require(
        text.find(
            "asdf_library: !core/software-1.0.0 {name: SpecForge, version: "
            SPECFORGE_EXPECTED_VERSION "}") != std::string::npos,
        "writer provenance should use the configured SpecForge project version");

    const RawAsdf raw = ParseRawAsdf(bytes);
    Require(raw.blocks.size() == 2, "explicit roster output should use two blocks");
    for (const RawBlock& block : raw.blocks) {
        Require(
            block.compression == std::array<unsigned char, 4>{'z', 'l', 'i', 'b'},
            "every production block should use zlib");
        Require(
            ZlibFlevel(bytes, block) == 2U,
            "every production zlib stream should declare FLEVEL=2");
        Require(
            std::all_of(
                block.checksum.begin(),
                block.checksum.end(),
                [](unsigned char byte) { return byte == 0; }),
            "production blocks should use zero checksum bytes");
    }

    const specforge::SampleLabelingAsdfReadResult read =
        specforge::ReadSampleLabelingAsdfDocument(path);
    Require(read.succeeded(), "production writer output should be readable");
    Require(
        read.durable_base && read.durable_base->valid(),
        "fixed production profile should produce a durable rewrite base");
    Require(
        JsonEquals(
            SemanticSummary(*read.document), SemanticSummary(document)),
        "production writer should round-trip every canonical semantic field");

    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
}

void TestWriterEmitsSourceIndexProductionProfileAndRoundTrips()
{
    const std::filesystem::path path = TempPath("_source_index_profile.asdf");
    const specforge::SampleLabelingDocument document =
        ProductionDocument(false);
    Require(
        WriteDocument(path, document).succeeded(),
        "source-index production writer should succeed");

    const std::vector<unsigned char> bytes = ReadAllBytes(path);
    const RawAsdf raw = ParseRawAsdf(bytes);
    Require(
        raw.blocks.size() == 1,
        "source-index production profile should contain only values block 0");
    Require(
        raw.blocks[0].compression ==
                std::array<unsigned char, 4>{'z', 'l', 'i', 'b'} &&
            std::ranges::all_of(raw.blocks[0].checksum,
                [](unsigned char byte) { return byte == 0; }),
        "source-index production block should use zlib and zero checksum");
    const std::string_view metadata(
        reinterpret_cast<const char*>(bytes.data()), raw.tree_end);
    Require(
        metadata.find("identity_kind: \"source_index\"") !=
                std::string_view::npos &&
            metadata.find("    source: 0\n") != std::string_view::npos &&
            metadata.find("    source: 1\n") == std::string_view::npos,
        "source-index metadata should bind values to the only block");

    const specforge::SampleLabelingAsdfReadResult read =
        specforge::ReadSampleLabelingAsdfDocument(path);
    Require(
        read.succeeded() &&
            JsonEquals(
                SemanticSummary(*read.document), SemanticSummary(document)),
        "source-index writer should round-trip every canonical semantic field");

    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
}

void TestLabelRewriteReusesRosterBlockVerbatim()
{
    const std::filesystem::path input_path = TempPath("_reuse_input.asdf");
    const std::filesystem::path output_path = TempPath("_reuse_output.asdf");
    Require(
        WriteDocument(input_path, ProductionDocument()).succeeded(),
        "block-reuse input should be written");
    const std::vector<unsigned char> before = ReadAllBytes(input_path);
    const RawAsdf before_raw = ParseRawAsdf(before);
    Require(before_raw.blocks.size() == 2, "block-reuse fixture should have two blocks");

    const specforge::SampleLabelingAsdfReadResult validated =
        specforge::ReadSampleLabelingAsdfDocument(input_path);
    Require(validated.succeeded(), "block-reuse input should validate once");
    Require(
        validated.durable_base && validated.durable_base->valid(),
        "validated production input should expose a durable rewrite base");
    std::ostringstream rejected_output(std::ios::binary);
    const std::array<std::int32_t, 3> invalid_replacement{42, 0, 1};
    const specforge::SampleLabelingAsdfWriteResult rejected =
        specforge::RewriteSampleLabelingAsdfValuesPreservingRosterBlock(
            *validated.durable_base,
            rejected_output,
            invalid_replacement);
    Require(
        !rejected.succeeded() &&
            rejected.error.kind ==
                specforge::SampleLabelingAsdfErrorKind::SemanticValidationFailed &&
            rejected_output.str().empty(),
        "rewrite preflight must reject invalid values before output begins");
    std::error_code cleanup_error;
    Require(
        std::filesystem::remove(input_path, cleanup_error),
        "durable rewrite base should not retain an open source handle");

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    Require(output.good(), "block-reuse output should open");
    const std::array<std::int32_t, 3> replacement{0, 0, 1};
    const specforge::SampleLabelingAsdfWriteResult rewrite =
        specforge::RewriteSampleLabelingAsdfValuesPreservingRosterBlock(
            *validated.durable_base,
            output,
            replacement);
    output.close();
    Require(rewrite.succeeded(), "label-only block-reuse write should succeed");
    Require(
        rewrite.roster_block_reused,
        "explicit roster block reuse is a required production property");

    const std::vector<unsigned char> after = ReadAllBytes(output_path);
    const RawAsdf after_raw = ParseRawAsdf(after);
    Require(after_raw.blocks.size() == 2, "rewritten document should retain two blocks");
    Require(
        before_raw.blocks[0].size == after_raw.blocks[0].size &&
            std::equal(
                before.begin() + static_cast<std::ptrdiff_t>(before_raw.blocks[0].offset),
                before.begin() + static_cast<std::ptrdiff_t>(before_raw.blocks[0].offset + before_raw.blocks[0].size),
                after.begin() + static_cast<std::ptrdiff_t>(after_raw.blocks[0].offset)),
        "encoded roster block must be copied byte-for-byte");
    Require(
        before_raw.blocks[1].offset == after_raw.blocks[1].offset &&
            std::equal(
                before.begin(),
                before.begin() + static_cast<std::ptrdiff_t>(before_raw.blocks[1].offset),
                after.begin()),
        "metadata and roster prefix must remain byte-for-byte unchanged");

    const specforge::SampleLabelingAsdfReadResult read =
        specforge::ReadSampleLabelingAsdfDocument(output_path);
    Require(read.succeeded(), "rewritten production document should read");
    specforge::SampleLabelingDocument expected = ProductionDocument();
    expected.annotation.values.assign(
        replacement.begin(), replacement.end());
    Require(
        JsonEquals(
            SemanticSummary(*read.document), SemanticSummary(expected)),
        "rewritten document should preserve all semantics except replaced values");

    std::filesystem::remove(input_path, cleanup_error);
    std::filesystem::remove(output_path, cleanup_error);
}

void TestSourceIndexRewriteUsesSingleValuesBlock()
{
    const std::filesystem::path input_path = TempPath("_index_input.asdf");
    const std::filesystem::path output_path = TempPath("_index_output.asdf");
    Require(
        WriteDocument(input_path, ProductionDocument(false)).succeeded(),
        "source-index input should be written");
    const specforge::SampleLabelingAsdfReadResult validated =
        specforge::ReadSampleLabelingAsdfDocument(input_path);
    Require(
        validated.succeeded() && validated.durable_base,
        "source-index input should expose a durable rewrite base");

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    const std::array<std::int32_t, 3> replacement{-1, 1, 1};
    const specforge::SampleLabelingAsdfWriteResult rewrite =
        specforge::RewriteSampleLabelingAsdfValuesPreservingRosterBlock(
            *validated.durable_base,
            output,
            replacement);
    output.close();
    Require(rewrite.succeeded(), "source-index label rewrite should succeed");
    Require(
        !rewrite.roster_block_reused,
        "source-index documents have no roster block to reuse");
    Require(
        ParseRawAsdf(ReadAllBytes(output_path)).blocks.size() == 1,
        "source-index output should retain a single values block");
    const specforge::SampleLabelingAsdfReadResult read =
        specforge::ReadSampleLabelingAsdfDocument(output_path);
    specforge::SampleLabelingDocument expected = ProductionDocument(false);
    expected.annotation.values.assign(
        replacement.begin(), replacement.end());
    Require(
        read.succeeded() &&
            JsonEquals(
                SemanticSummary(*read.document), SemanticSummary(expected)),
        "source-index rewrite should preserve every canonical semantic field");

    std::error_code cleanup_error;
    std::filesystem::remove(input_path, cleanup_error);
    std::filesystem::remove(output_path, cleanup_error);
}

void TestMetadataRewritePreservesForwardUnknownFields()
{
    const std::filesystem::path input_path =
        TempPath("_forward_metadata_input.asdf");
    const std::filesystem::path output_path =
        TempPath("_forward_metadata_output.asdf");
    const specforge::SampleLabelingDocument original = ProductionDocument();
    Require(
        WriteDocument(input_path, original).succeeded(),
        "forward-metadata input should be written");

    std::vector<unsigned char> bytes = ReadAllBytes(input_path);
    ReplaceTextOnce(bytes,
        "\nschema_version: ",
        "\nfuture_root:\n"
        "  string_token: \"true\"\n"
        "  real_boolean: true\n"
        "  real_integer: 1\n"
        "schema_version: ");
    ReplaceTextOnce(bytes,
        "\n  sample_count: 3\nsample_roster:",
        "\n  future_source: \"source-survives\"\n  sample_count: 3\nsample_roster:");
    ReplaceTextOnce(bytes,
        "\n  names: !core/ndarray-1.0.0",
        "\n  future_roster: \"roster-survives\"\n  names: !core/ndarray-1.0.0");
    ReplaceTextOnce(bytes,
        "    shape: [3]\n  missing:",
        "    shape: [3]\n    future_values: \"values-survive\"\n  missing:");
    ReplaceTextOnce(bytes,
        "\n    value: -1\nlabeling_task:",
        "\n    value: -1\n    future_missing: \"missing-survives\"\nlabeling_task:");
    ReplaceTextOnce(bytes,
        "\n  labels:\n",
        "\n  future_task: \"task-survives\"\n  labels:\n");
    ReplaceTextOnce(bytes,
        "    shortcut: \"g\"\n",
        "    shortcut: \"g\"\n    future_label: \"label-survives\"\n");
    WriteAllBytes(input_path, bytes);

    const specforge::SampleLabelingAsdfReadResult opened =
        specforge::ReadSampleLabelingAsdfDocument(input_path);
    Require(
        opened.succeeded() && opened.durable_base,
        "forward-metadata input should expose a durable base");
    specforge::SampleLabelingDocument edited = *opened.document;
    edited.annotation.name = "Edited annotation";
    edited.annotation.values = {1, 1, 0};
    edited.labeling.name = "Edited task";
    edited.labeling.labels[0].name = "Edited Galaxy";
    edited.labeling.labels[0].shortcut = "1";

    specforge::SampleLabelingDocument unrelated = edited;
    unrelated.labeling.id = "another-task";
    std::ostringstream mismatch_output(std::ios::binary);
    const specforge::SampleLabelingAsdfWriteResult identity_mismatch =
        specforge::RewriteSampleLabelingAsdfDocumentPreservingUnknownMetadata(
            *opened.durable_base,
            mismatch_output,
            unrelated);
    Require(
        !identity_mismatch.succeeded() &&
            identity_mismatch.error.kind ==
                specforge::SampleLabelingAsdfErrorKind::
                    SemanticValidationFailed &&
            mismatch_output.str().empty(),
        "public metadata rewrite must reject a durable base from another document identity before output");

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    Require(output.good(), "forward-metadata output should open");
    const specforge::SampleLabelingAsdfWriteResult rewrite =
        specforge::RewriteSampleLabelingAsdfDocumentPreservingUnknownMetadata(
            *opened.durable_base,
            output,
            edited);
    output.close();
    Require(
        rewrite.succeeded() && !rewrite.roster_block_reused,
        rewrite.error.message.empty()
            ? "metadata rewrite should succeed and encode a new generation"
            : rewrite.error.message);

    const std::vector<unsigned char> rewritten_bytes =
        ReadAllBytes(output_path);
    const std::string rewritten(
        reinterpret_cast<const char*>(rewritten_bytes.data()),
        rewritten_bytes.size());
    for (const std::string_view token : {
             "source-survives",
             "roster-survives",
             "values-survive",
             "missing-survives",
             "task-survives",
             "label-survives",
         }) {
        Require(
            rewritten.find(token) != std::string::npos,
            "metadata rewrite should retain every unknown metadata token");
    }
    Require(
        rewritten.find("string_token: true") == std::string::npos &&
            rewritten.find("shortcut: 1") == std::string::npos,
        "metadata rewrite must not emit string scalars with YAML-ambiguous plain spelling");
    Require(
        rewritten.find("real_boolean: true") != std::string::npos &&
            rewritten.find("real_integer: 1") != std::string::npos,
        "metadata rewrite must not stringify genuine unknown booleans or integers");

    const specforge::SampleLabelingAsdfReadResult reopened =
        specforge::ReadSampleLabelingAsdfDocument(output_path);
    Require(
        reopened.succeeded() &&
            JsonEquals(
                SemanticSummary(*reopened.document),
                SemanticSummary(edited)),
        "metadata rewrite should replace every edited canonical field");

    std::error_code cleanup_error;
    std::filesystem::remove(input_path, cleanup_error);
    std::filesystem::remove(output_path, cleanup_error);
}

void TestRewriteRejectsUnverifiedRosterBlocks()
{
    const specforge::SampleLabelingAsdfReadResult corrupt =
        specforge::ReadSampleLabelingAsdfDocument(
            FixturePath("malformed_zlib_payload.asdf"));
    Require(!corrupt.succeeded(), "corrupt roster zlib must fail initial validation");
    Require(
        !corrupt.durable_base,
        "corrupt roster must never produce a durable rewrite base");

    const specforge::SampleLabelingAsdfReadResult uncompressed =
        specforge::ReadSampleLabelingAsdfDocument(
            FixturePath("folder_roster.asdf"));
    Require(uncompressed.succeeded(), "read path may accept uncompressed ASDF");
    Require(
        !uncompressed.durable_base,
        "uncompressed roster must not produce a production rewrite base");

    const std::filesystem::path big_endian_input =
        TempPath("_big_endian_roster_input.asdf");
    Require(
        WriteDocument(big_endian_input, ProductionDocument()).succeeded(),
        "big-endian roster rejection input should be written");
    {
        const std::vector<unsigned char> bytes = ReadAllBytes(big_endian_input);
        const std::string_view metadata(
            reinterpret_cast<const char*>(bytes.data()),
            bytes.size());
        const std::size_t byteorder = metadata.find("byteorder: little");
        Require(byteorder != std::string_view::npos, "roster byteorder should exist");
        std::fstream file(
            big_endian_input,
            std::ios::binary | std::ios::in | std::ios::out);
        Require(file.good(), "big-endian roster fixture should open");
        file.seekp(static_cast<std::streamoff>(
            byteorder + std::string_view("byteorder: ").size()));
        file.write("big   ", 6);
        Require(file.good(), "roster byteorder fixture should be patched");
    }
    const specforge::SampleLabelingAsdfReadResult big_endian =
        specforge::ReadSampleLabelingAsdfDocument(big_endian_input);
    Require(
        !big_endian.succeeded() || !big_endian.durable_base,
        "big-endian roster must never produce a production rewrite base");

    std::error_code cleanup_error;
    std::filesystem::remove(big_endian_input, cleanup_error);
}

void TestReaderCompatibilityProfileCannotBecomeDurableVerbatim()
{
    const std::filesystem::path path =
        TempPath("_big_endian_compatibility.asdf");
    std::vector<unsigned char> bytes =
        ReadAllBytes(FixturePath("folder_roster.asdf"));
    const RawAsdf raw = ParseRawAsdf(bytes);
    Require(
        raw.blocks.size() == 2,
        "big-endian compatibility base should have roster and values blocks");
    for (const RawBlock& block : raw.blocks) {
        Require(
            block.compression == std::array<unsigned char, 4>{0, 0, 0, 0},
            "big-endian compatibility base should be uncompressed");
        const std::size_t payload_size =
            block.offset + block.size - block.payload_offset;
        Require(
            payload_size % sizeof(std::uint32_t) == 0,
            "UCS-4/int32 compatibility payload should be word aligned");
        for (std::size_t offset = block.payload_offset;
            offset < block.payload_offset + payload_size;
            offset += sizeof(std::uint32_t)) {
            std::reverse(
                bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes.begin() + static_cast<std::ptrdiff_t>(
                    offset + sizeof(std::uint32_t)));
        }
    }
    ReplaceTextOnce(
        bytes, "    byteorder: little\n", "    byteorder: big   \n");
    ReplaceTextOnce(
        bytes, "    byteorder: little\n", "    byteorder: big   \n");
    WriteAllBytes(path, bytes);

    const specforge::SampleLabelingAsdfReadResult read =
        specforge::ReadSampleLabelingAsdfDocument(path);
    const specforge::JsonValue expected =
        ReadJsonFile(FixturePath("folder_roster.semantic.json"));
    Require(
        read.succeeded() &&
            JsonEquals(SemanticSummary(*read.document), expected),
        "reader compatibility profile should decode big-endian semantics");
    Require(
        !read.durable_base,
        "big-endian compatibility input must not enter verbatim rewrite profile");

    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
}

void TestDurableRosterRequiresDefaultZlibFlevel()
{
    for (const unsigned int flevel : {0U, 3U}) {
        const std::filesystem::path path =
            TempPath(flevel == 0U ? "_zlib_level1.asdf"
                                 : "_zlib_level9.asdf");
        Require(
            WriteDocument(path, ProductionDocument()).succeeded(),
            "FLEVEL compatibility input should be written");
        std::vector<unsigned char> bytes = ReadAllBytes(path);
        const RawAsdf raw = ParseRawAsdf(bytes);
        Require(
            raw.blocks.size() == 2,
            "FLEVEL compatibility input should contain two blocks");
        SetZlibFlevel(bytes, raw.blocks[0], flevel);
        WriteAllBytes(path, bytes);

        const specforge::SampleLabelingAsdfReadResult read =
            specforge::ReadSampleLabelingAsdfDocument(path);
        Require(
            read.succeeded() &&
                JsonEquals(SemanticSummary(*read.document),
                    SemanticSummary(ProductionDocument())),
            "non-default FLEVEL roster should remain reader-compatible");
        Require(
            !read.durable_base,
            "only FLEVEL=2 roster streams may enter durable verbatim reuse");

        std::error_code cleanup_error;
        std::filesystem::remove(path, cleanup_error);
    }
}

void TestWriterRejectsUnreadableRosterWidth()
{
    specforge::SampleLabelingDocument document = ProductionDocument();
    document.source.sample_count = 1;
    document.source.roster.sample_names = {
        std::string(1'000'001, 'x')};
    document.annotation.values = {0};
    std::ostringstream output(std::ios::binary);
    const specforge::SampleLabelingAsdfWriteResult write =
        specforge::WriteSampleLabelingAsdfDocument(output, document);
    Require(!write.succeeded(), "writer must reject an unreadable roster width");
    Require(
        write.error.kind ==
            specforge::SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
        "writer and reader should share the roster-width resource limit");
}

void TestCanonicalReaderRequiresExplicitSourceIdentity()
{
    const std::filesystem::path path = TempPath("_missing_identity.asdf");
    Require(
        WriteDocument(path, ProductionDocument()).succeeded(),
        "missing-identity input should be written");
    std::vector<unsigned char> bytes = ReadAllBytes(path);
    const std::string_view text(
        reinterpret_cast<const char*>(bytes.data()),
        bytes.size());
    const std::size_t identity = text.find("  identity: ");
    Require(identity != std::string_view::npos, "identity line should exist");
    const std::size_t line_end = text.find('\n', identity);
    Require(line_end != std::string_view::npos, "identity line should end");
    bytes.erase(
        bytes.begin() + static_cast<std::ptrdiff_t>(identity),
        bytes.begin() + static_cast<std::ptrdiff_t>(line_end + 1U));
    WriteAllBytes(path, bytes);

    const specforge::SampleLabelingAsdfReadResult read =
        specforge::ReadSampleLabelingAsdfDocument(path);
    Require(!read.succeeded(), "canonical v1 must require source identity");
    Require(
        read.error.kind ==
            specforge::SampleLabelingAsdfErrorKind::MalformedDocument,
        "missing canonical source identity should be a malformed document");

    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
}

void TestWriterRejectsUnreadableLabelCount()
{
    specforge::SampleLabelingDocument document = ProductionDocument(false);
    document.labeling.labels.clear();
    document.labeling.labels.reserve(100'001);
    for (std::int32_t code = 0; code < 100'001; ++code) {
        document.labeling.labels.push_back({code, "Label", {}});
    }
    document.annotation.values = {0, 0, 0};
    std::ostringstream output(std::ios::binary);
    const specforge::SampleLabelingAsdfWriteResult write =
        specforge::WriteSampleLabelingAsdfDocument(output, document);
    Require(!write.succeeded(), "writer must reject an unreadable label count");
    Require(
        write.error.kind ==
            specforge::SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
        "writer and reader should share the label-count resource limit");
}

void TestMetadataReadIsBounded()
{
    const std::filesystem::path path = TempPath("_oversized_metadata_lines.asdf");
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        Require(output.good(), "oversized metadata fixture should open");
        output << "#ASDF 1.0.0\n#ASDF_STANDARD 1.5.0\n";
        const std::string chunk(1024, 'x');
        for (std::size_t index = 0; index < 8200; ++index) {
            output << "#" << chunk << "\n";
        }
        output << "...\n";
    }
    const specforge::SampleLabelingAsdfReadResult result =
        specforge::ReadSampleLabelingAsdfDocument(path);
    Require(!result.succeeded(), "oversized metadata should be rejected");
    Require(
        result.error.kind ==
            specforge::SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
        "oversized metadata should hit the bounded streaming limit");
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);

    const std::filesystem::path long_line_path =
        TempPath("_oversized_metadata_line.asdf");
    {
        std::ofstream output(long_line_path, std::ios::binary | std::ios::trunc);
        Require(output.good(), "long-line metadata fixture should open");
        output << "#ASDF 1.0.0\n#ASDF_STANDARD 1.5.0\n#";
        const std::string chunk(64U * 1024U, 'x');
        for (std::size_t index = 0; index < 129; ++index) {
            output << chunk;
        }
    }
    const specforge::SampleLabelingAsdfReadResult long_line_result =
        specforge::ReadSampleLabelingAsdfDocument(long_line_path);
    Require(!long_line_result.succeeded(), "oversized YAML line should be rejected");
    Require(
        long_line_result.error.kind ==
            specforge::SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
        "a single YAML line should not bypass the streaming metadata limit");
    std::filesystem::remove(long_line_path, cleanup_error);
}

}  // namespace

int main(int argc, char* argv[])
{
    try {
        if (argc == 3 &&
            std::string_view(argv[1]) == "write-yaml-specials-oracle") {
            specforge::SampleLabelingDocument document =
                ProductionDocument(false);
            document.source.name = YamlSpecialCharacterMatrix();
            const specforge::SampleLabelingAsdfWriteResult write =
                WriteDocument(argv[2], document);
            if (!write.succeeded()) {
                std::cerr << "production YAML-special oracle write failed: "
                          << write.error.message << '\n';
                return 1;
            }
            return 0;
        }
        if (argc == 3 &&
            std::string_view(argv[1]) == "write-explicit-roster-oracle") {
            const specforge::SampleLabelingAsdfWriteResult write =
                WriteDocument(argv[2], ProductionDocument());
            if (!write.succeeded()) {
                std::cerr << "production explicit-roster oracle write failed: "
                          << write.error.message << '\n';
                return 1;
            }
            return 0;
        }
        if (argc == 3 &&
            std::string_view(argv[1]) == "write-source-index-oracle") {
            const specforge::SampleLabelingAsdfWriteResult write =
                WriteDocument(argv[2], ProductionDocument(false));
            if (!write.succeeded()) {
                std::cerr << "production source-index oracle write failed: "
                          << write.error.message << '\n';
                return 1;
            }
            return 0;
        }
        if (argc == 4 &&
            std::string_view(argv[1]) == "rewrite-explicit-roster-oracle") {
            const specforge::SampleLabelingAsdfReadResult read =
                specforge::ReadSampleLabelingAsdfDocument(argv[2]);
            if (!read.succeeded() || !read.durable_base) {
                std::cerr << "production explicit-roster oracle input did not "
                             "produce a durable base\n";
                return 1;
            }
            std::ofstream output(
                argv[3], std::ios::binary | std::ios::trunc);
            if (!output) {
                std::cerr << "production explicit-roster oracle output failed "
                             "to open\n";
                return 1;
            }
            const std::array<std::int32_t, 3> replacement{0, 0, 1};
            const specforge::SampleLabelingAsdfWriteResult rewrite =
                specforge::RewriteSampleLabelingAsdfValuesPreservingRosterBlock(
                    *read.durable_base,
                    output,
                    replacement);
            output.close();
            if (!rewrite.succeeded() || !rewrite.roster_block_reused) {
                std::cerr << "production explicit-roster oracle rewrite failed: "
                          << rewrite.error.message << '\n';
                return 1;
            }
            return 0;
        }
        if (argc == 4 &&
            std::string_view(argv[1]) == "rewrite-metadata-oracle") {
            const specforge::SampleLabelingAsdfReadResult read =
                specforge::ReadSampleLabelingAsdfDocument(argv[2]);
            if (!read.succeeded() || !read.durable_base) {
                std::cerr << "production metadata oracle input did not produce "
                             "a durable base\n";
                return 1;
            }
            specforge::SampleLabelingDocument edited = *read.document;
            edited.annotation.name = "Forward metadata edited";
            edited.labeling.name = "Forward metadata edited";
            edited.labeling.labels[0].name = "Edited Galaxy";
            edited.labeling.labels[0].shortcut = "1";
            std::ofstream output(
                argv[3], std::ios::binary | std::ios::trunc);
            if (!output) {
                std::cerr << "production metadata oracle output failed to open\n";
                return 1;
            }
            const specforge::SampleLabelingAsdfWriteResult rewrite =
                specforge::RewriteSampleLabelingAsdfDocumentPreservingUnknownMetadata(
                    *read.durable_base,
                    output,
                    edited);
            output.close();
            if (!rewrite.succeeded()) {
                std::cerr << "production metadata oracle rewrite failed: "
                          << rewrite.error.message << '\n';
                return 1;
            }
            return 0;
        }
        if (argc == 6 &&
            std::string_view(argv[1]) == "rewrite-production-oracle") {
            const std::size_t index =
                static_cast<std::size_t>(std::stoull(argv[4]));
            const std::int32_t replacement =
                static_cast<std::int32_t>(std::stol(argv[5]));
            const specforge::SampleLabelingAsdfReadResult read =
                specforge::ReadSampleLabelingAsdfDocument(argv[2]);
            if (!read.succeeded() || !read.durable_base ||
                index >= read.document->annotation.values.size()) {
                std::cerr << "production oracle rewrite input is not a valid "
                             "durable document or index\n";
                return 1;
            }
            std::vector<std::int32_t> values =
                read.document->annotation.values;
            values[index] = replacement;
            std::ofstream output(
                argv[3], std::ios::binary | std::ios::trunc);
            if (!output) {
                std::cerr << "production oracle rewrite output failed to open\n";
                return 1;
            }
            const specforge::SampleLabelingAsdfWriteResult rewrite =
                specforge::RewriteSampleLabelingAsdfValuesPreservingRosterBlock(
                    *read.durable_base,
                    output,
                    values);
            output.close();
            if (!rewrite.succeeded()) {
                std::cerr << "production oracle rewrite failed: "
                          << rewrite.error.message << '\n';
                return 1;
            }
            return 0;
        }
        Require(argc == 1, "unexpected sample labeling codec test arguments");
        TestReadsApprovedPythonFixtures();
        TestRejectsSemanticViolationsWithControlledErrors();
        TestRejectsMalformedCorruptAndUnsupportedInputs();
        TestRejectsNdarrayMaskProfile();
        TestWriterRejectsRosterNulBeforeOutput();
        TestWriterRejectsInvalidUtf8BeforeOutput();
        TestReaderRejectsInvalidUtf8InUnknownYamlBeforeHydration();
        TestYamlScalarMaterializationUsesResidentPreflight();
        TestExactReadDistinguishesTruncationFromIoFailure();
        TestRejectsDuplicateYamlMappingKeys();
        TestWriterRejectsDocumentsOutsideReaderResidentBudget();
        TestWriterEscapesYamlSpecialCharacterMatrixLosslessly();
        TestRejectsUnreferencedAndOutOfOrderBlocks();
        TestDurableBaseUsesTheValidatedPrefixSnapshot();
        TestRejectsMalformedBlockIndexes();
        TestReaderEnforcesCombinedResidentMemoryBudget();
        TestSharedProfilePreflightBoundaryMatrix();
        TestWriterEmitsFixedProductionProfileAndRoundTrips();
        TestWriterEmitsSourceIndexProductionProfileAndRoundTrips();
        TestLabelRewriteReusesRosterBlockVerbatim();
        TestSourceIndexRewriteUsesSingleValuesBlock();
        TestMetadataRewritePreservesForwardUnknownFields();
        TestRewriteRejectsUnverifiedRosterBlocks();
        TestReaderCompatibilityProfileCannotBecomeDurableVerbatim();
        TestDurableRosterRequiresDefaultZlibFlevel();
        TestWriterRejectsUnreadableRosterWidth();
        TestWriterRejectsUnreadableLabelCount();
        TestCanonicalReaderRequiresExplicitSourceIdentity();
        TestMetadataReadIsBounded();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sample labeling ASDF codec test failure: "
                  << error.what() << '\n';
        return 1;
    }
}
