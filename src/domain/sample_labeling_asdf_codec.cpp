#include "domain/sample_labeling_asdf_codec.h"

#include "domain/stable_sha256.h"
#include "domain/utf8.h"

#include "specforge/specforge_build_identity.h"

#include <yaml-cpp/yaml.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <new>
#include <ostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace specforge {

struct SampleLabelingAsdfDurableBase::State {
    std::vector<unsigned char> encoded_metadata;
    std::vector<unsigned char> encoded_roster_block;
    std::vector<std::int32_t> label_codes;
    std::string preservation_identity_digest;
    std::string values_rewrite_identity_digest;
    CanonicalTimestamp created_at;
    CanonicalTimestamp modified_at;
    SampleLabelingOrigin origin;
    std::size_t sample_count = 0;
    std::size_t metadata_bytes = 0;
    std::size_t roster_string_width = 0;
    bool roster_block_reused = false;
};

SampleLabelingAsdfDurableBase::SampleLabelingAsdfDurableBase(
    std::shared_ptr<const State> state) noexcept
    : state_(std::move(state))
{
}

namespace {

thread_local sample_labeling_asdf_test_seam::BeforeReusablePrefixCapture
    g_before_reusable_prefix_capture = nullptr;
thread_local sample_labeling_asdf_test_seam::BeforeMetadataBuild
    g_before_metadata_build = nullptr;

constexpr std::uint64_t kMaximumAsdfFileBytes = 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumMetadataBytes = 8ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumDecodedBlockBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumSampleCount = 100'000'000ULL;
constexpr std::uint64_t kMaximumRosterStringWidth = 1'000'000ULL;
constexpr std::size_t kMaximumLabelCount = 100'000ULL;
constexpr std::size_t kMaximumAuthorCount = 10'000ULL;
constexpr std::size_t kMaximumBlockCount = 64ULL;
constexpr std::size_t kMaximumBlockIndexBytes = 64ULL * 1024ULL;
constexpr std::size_t kMaximumInterBlockPaddingBytes = 1024ULL * 1024ULL;
constexpr std::size_t kIoChunkBytes = 64ULL * 1024ULL;
constexpr std::uint64_t kInflateResidentScratchBytes = 256ULL * 1024ULL;
constexpr std::uint64_t kDeflateResidentScratchBytes = 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumResidentCodecBytes =
    512ULL * 1024ULL * 1024ULL;
thread_local std::uint64_t g_maximum_resident_codec_bytes =
    kMaximumResidentCodecBytes;
constexpr std::uint64_t kMaximumReusablePrefixBytes =
    kMaximumMetadataBytes + kMaximumDecodedBlockBytes +
    2ULL * kMaximumInterBlockPaddingBytes + 6ULL +
    std::numeric_limits<std::uint16_t>::max();
constexpr std::array<unsigned char, 4> kBlockMagic{0xd3, 'B', 'L', 'K'};
constexpr std::array<unsigned char, 4> kZlibCompression{'z', 'l', 'i', 'b'};
constexpr std::string_view kYamlStringTag = "tag:yaml.org,2002:str";

void AppendIdentityDigestField(StableSha256& digest,
    std::string_view value)
{
    digest.Append(std::to_string(value.size()));
    digest.Append(":");
    digest.Append(value);
    digest.Append(";");
}

[[nodiscard]] std::string PreservationIdentityDigest(
    const SampleLabelingDocument& document)
{
    StableSha256 digest;
    AppendIdentityDigestField(
        digest, "specforge.sample_labeling.asdf-preservation-identity-v2");
    AppendIdentityDigestField(digest, document.format_kind);
    AppendIdentityDigestField(digest, document.schema_version);
    AppendIdentityDigestField(digest, document.source.base_identity);
    AppendIdentityDigestField(digest, document.source.kind);
    AppendIdentityDigestField(digest, document.source.name);
    AppendIdentityDigestField(digest, document.source.fingerprint);
    AppendIdentityDigestField(
        digest, std::to_string(document.source.sample_count));
    AppendIdentityDigestField(
        digest, document.source.roster.identity_kind);
    AppendIdentityDigestField(digest,
        std::to_string(document.source.roster.sample_names.size()));
    for (const std::string& sample_name :
        document.source.roster.sample_names) {
        AppendIdentityDigestField(digest, sample_name);
    }
    AppendIdentityDigestField(digest, document.annotation.kind);
    AppendIdentityDigestField(digest, document.labeling.id);
    const SampleLabelingTaskCanonicalMetadata& metadata =
        document.labeling.canonical_metadata;
    AppendIdentityDigestField(
        digest, FormatCanonicalTimestamp(metadata.created_at));
    AppendIdentityDigestField(digest, metadata.origin.kind);
    AppendIdentityDigestField(
        digest, metadata.origin.annotation ? "annotation" : "no-annotation");
    if (metadata.origin.annotation) {
        const SampleLabelingAnnotationOrigin& annotation =
            *metadata.origin.annotation;
        AppendIdentityDigestField(digest, annotation.name);
        AppendIdentityDigestField(digest, annotation.format);
        AppendIdentityDigestField(
            digest, annotation.fingerprint.value_or(std::string{}));
    }
    return digest.FinishHex();
}

[[nodiscard]] std::string ValuesRewriteIdentityDigest(
    const SampleLabelingDocument& document)
{
    StableSha256 digest;
    AppendIdentityDigestField(
        digest, "specforge.sample_labeling.asdf-values-rewrite-identity-v1");
    AppendIdentityDigestField(
        digest, PreservationIdentityDigest(document));
    AppendIdentityDigestField(
        digest, document.annotation.missing.semantic);
    AppendIdentityDigestField(
        digest, std::to_string(document.annotation.missing.value));
    AppendIdentityDigestField(digest, document.labeling.name);

    const SampleLabelingTaskCanonicalMetadata& metadata =
        document.labeling.canonical_metadata;
    AppendIdentityDigestField(
        digest, metadata.description ? "description" : "no-description");
    if (metadata.description) {
        AppendIdentityDigestField(digest, *metadata.description);
    }
    AppendIdentityDigestField(
        digest, std::to_string(metadata.authors.size()));
    for (const SampleLabelingAuthor& author : metadata.authors) {
        AppendIdentityDigestField(digest, author.name);
        AppendIdentityDigestField(
            digest, author.identifier ? "identifier" : "no-identifier");
        if (author.identifier) {
            AppendIdentityDigestField(digest, *author.identifier);
        }
        AppendIdentityDigestField(
            digest, author.email ? "email" : "no-email");
        if (author.email) {
            AppendIdentityDigestField(digest, *author.email);
        }
    }

    AppendIdentityDigestField(
        digest, std::to_string(document.labeling.labels.size()));
    for (const SampleLabelingDocumentLabel& label :
        document.labeling.labels) {
        AppendIdentityDigestField(digest, std::to_string(label.code));
        AppendIdentityDigestField(digest, label.name);
        AppendIdentityDigestField(digest, label.shortcut);
    }
    return digest.FinishHex();
}

class CodecFailure : public std::runtime_error {
public:
    CodecFailure(SampleLabelingAsdfErrorKind kind, std::string message)
        : std::runtime_error(std::move(message)), kind_(kind)
    {
    }

    [[nodiscard]] SampleLabelingAsdfErrorKind kind() const noexcept
    {
        return kind_;
    }

private:
    SampleLabelingAsdfErrorKind kind_;
};

[[noreturn]] void Fail(SampleLabelingAsdfErrorKind kind, std::string message)
{
    throw CodecFailure(kind, std::move(message));
}

void PollReadCheckpoint(
    const SampleLabelingAsdfReadCheckpoint& checkpoint)
{
    if (checkpoint) {
        checkpoint();
    }
}

[[nodiscard]] std::size_t CheckedSizeProduct(std::uint64_t left,
    std::uint64_t right,
    std::string_view description)
{
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            std::string(description) + " size overflows");
    }
    const std::uint64_t product = left * right;
    if (product > kMaximumDecodedBlockBytes ||
        product > std::numeric_limits<std::size_t>::max()) {
        Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            std::string(description) + " exceeds the production decode limit");
    }
    return static_cast<std::size_t>(product);
}

void SeekInput(std::istream& input, std::uint64_t offset)
{
    if (offset > static_cast<std::uint64_t>(
                     std::numeric_limits<std::streamoff>::max())) {
        Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            "ASDF stream offset exceeds the platform stream limit");
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input) {
        Fail(SampleLabelingAsdfErrorKind::IoFailure,
            "could not seek within ASDF input");
    }
}

void ReadExact(std::istream& input,
    std::span<unsigned char> destination,
    const SampleLabelingAsdfReadCheckpoint& checkpoint = {})
{
    std::size_t offset = 0;
    while (offset < destination.size()) {
        PollReadCheckpoint(checkpoint);
        const std::size_t chunk =
            std::min(destination.size() - offset, kIoChunkBytes);
        input.read(reinterpret_cast<char*>(destination.data() + offset),
            static_cast<std::streamsize>(chunk));
        if (input.gcount() != static_cast<std::streamsize>(chunk)) {
            if (input.bad()) {
                Fail(SampleLabelingAsdfErrorKind::IoFailure,
                    "could not read from ASDF input");
            }
            Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                "ASDF input is truncated");
        }
        offset += chunk;
    }
}

void ReadExactAt(std::istream& input,
    std::uint64_t offset,
    std::span<unsigned char> destination,
    const SampleLabelingAsdfReadCheckpoint& checkpoint = {})
{
    SeekInput(input, offset);
    ReadExact(input, destination, checkpoint);
}

class MemoryInputBuffer final : public std::streambuf {
public:
    explicit MemoryInputBuffer(std::span<unsigned char> bytes)
        : begin_(reinterpret_cast<char*>(bytes.data())),
          end_(begin_ + bytes.size())
    {
        setg(begin_, begin_, end_);
    }

protected:
    pos_type seekoff(off_type offset,
        std::ios_base::seekdir direction,
        std::ios_base::openmode mode) override
    {
        if ((mode & std::ios_base::in) == 0) {
            return pos_type(off_type(-1));
        }
        char* origin = nullptr;
        if (direction == std::ios_base::beg) {
            origin = begin_;
        } else if (direction == std::ios_base::cur) {
            origin = gptr();
        } else if (direction == std::ios_base::end) {
            origin = end_;
        }
        if (origin == nullptr || offset < begin_ - origin ||
            offset > end_ - origin) {
            return pos_type(off_type(-1));
        }
        char* position = origin + offset;
        setg(begin_, position, end_);
        return pos_type(position - begin_);
    }

    pos_type seekpos(pos_type position,
        std::ios_base::openmode mode) override
    {
        return seekoff(
            static_cast<off_type>(position), std::ios_base::beg, mode);
    }

private:
    char* begin_;
    char* end_;
};

void WriteBytes(std::ostream& output, std::span<const unsigned char> bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t chunk =
            std::min(bytes.size() - offset, kIoChunkBytes);
        output.write(reinterpret_cast<const char*>(bytes.data() + offset),
            static_cast<std::streamsize>(chunk));
        if (!output) {
            Fail(SampleLabelingAsdfErrorKind::IoFailure,
                "could not write ASDF output");
        }
        offset += chunk;
    }
}

void WriteText(std::ostream& output, std::string_view text)
{
    WriteBytes(output,
        std::span<const unsigned char>(
            reinterpret_cast<const unsigned char*>(text.data()), text.size()));
}

[[nodiscard]] std::string BuildBlockIndex(
    std::span<const std::uint64_t> block_offsets)
{
    std::string index = "#ASDF BLOCK INDEX\n%YAML 1.1\n---\n";
    for (const std::uint64_t offset : block_offsets) {
        index.append("- ").append(std::to_string(offset)).push_back('\n');
    }
    index.append("...\n");
    if (index.size() > kMaximumBlockIndexBytes) {
        Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            "ASDF block index exceeds the production limit");
    }
    return index;
}

[[nodiscard]] std::uint64_t InputFileSize(const std::filesystem::path& path)
{
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        Fail(SampleLabelingAsdfErrorKind::IoFailure,
            "could not determine ASDF input size: " + error.message());
    }
    if (size > kMaximumAsdfFileBytes) {
        Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            "ASDF input exceeds the production file-size limit");
    }
    return static_cast<std::uint64_t>(size);
}

struct MetadataTree {
    std::string bytes;
    std::uint64_t end_offset = 0;
};

[[nodiscard]] std::string_view WithoutCarriageReturn(std::string_view line)
{
    return !line.empty() && line.back() == '\r'
               ? line.substr(0, line.size() - 1)
               : line;
}

[[nodiscard]] MetadataTree ReadMetadataTree(std::istream& input,
    const SampleLabelingAsdfReadCheckpoint& checkpoint = {})
{
    MetadataTree tree;
    std::size_t line_number = 0;
    std::size_t line_start = 0;
    bool terminated = false;

    const auto finish_line = [&](std::size_t line_end) {
        ++line_number;
        const std::string_view normalized =
            WithoutCarriageReturn(std::string_view(tree.bytes)
                    .substr(line_start, line_end - line_start));
        if (line_number == 1 &&
            normalized !=
                std::string("#ASDF ") +
                    std::string(kSampleLabelingAsdfFileFormatVersion)) {
            Fail(SampleLabelingAsdfErrorKind::UnsupportedProfile,
                "unsupported or missing ASDF file-format version");
        }
        if (line_number == 2 &&
            normalized != std::string("#ASDF_STANDARD ") +
                              std::string(kSampleLabelingAsdfStandardVersion)) {
            Fail(SampleLabelingAsdfErrorKind::UnsupportedProfile,
                "unsupported or missing ASDF Standard version");
        }
        if (normalized == "...") {
            terminated = true;
        }
        line_start = tree.bytes.size();
    };

    while (!terminated) {
        if (tree.bytes.size() % kIoChunkBytes == 0) {
            PollReadCheckpoint(checkpoint);
        }
        const int next = input.get();
        if (next == std::char_traits<char>::eof()) {
            if (input.bad()) {
                Fail(SampleLabelingAsdfErrorKind::IoFailure,
                    "could not read ASDF YAML metadata");
            }
            if (line_start != tree.bytes.size()) {
                finish_line(tree.bytes.size());
            }
            break;
        }
        if (tree.bytes.size() == kMaximumMetadataBytes) {
            Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
                "ASDF YAML metadata exceeds the production limit");
        }
        tree.bytes.push_back(static_cast<char>(next));
        if (next == '\n') {
            finish_line(tree.bytes.size() - 1U);
        }
    }
    if (!terminated) {
        Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
            "ASDF YAML document terminator is missing");
    }
    if (line_number < 2) {
        Fail(SampleLabelingAsdfErrorKind::UnsupportedProfile,
            "ASDF profile headers are incomplete");
    }
    tree.end_offset = tree.bytes.size();
    return tree;
}

template <typename UInt>
[[nodiscard]] UInt ReadBigEndian(std::span<const unsigned char> bytes,
    std::size_t offset)
{
    static_assert(std::is_unsigned_v<UInt>);
    if (offset > bytes.size() || sizeof(UInt) > bytes.size() - offset) {
        Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
            "ASDF block header is truncated");
    }
    UInt value = 0;
    for (std::size_t index = 0; index < sizeof(UInt); ++index) {
        value = static_cast<UInt>((value << 8U) | bytes[offset + index]);
    }
    return value;
}

void AppendBigEndian(std::vector<unsigned char>& bytes,
    std::uint64_t value,
    std::size_t width)
{
    for (std::size_t index = 0; index < width; ++index) {
        const std::size_t shift = 8U * (width - index - 1U);
        bytes.push_back(static_cast<unsigned char>((value >> shift) & 0xffU));
    }
}

enum class BlockCompression {
    None,
    Zlib,
};

struct BlockDescriptor {
    std::uint64_t raw_offset = 0;
    std::uint64_t raw_size = 0;
    std::uint64_t payload_offset = 0;
    std::size_t encoded_size = 0;
    std::size_t decoded_size = 0;
    std::size_t allocated_size = 0;
    BlockCompression compression = BlockCompression::None;
};

[[nodiscard]] bool PrefixAt(std::istream& input,
    std::uint64_t offset,
    std::uint64_t file_size,
    std::string_view prefix,
    const SampleLabelingAsdfReadCheckpoint& checkpoint = {})
{
    if (offset > file_size || prefix.size() > file_size - offset) {
        return false;
    }
    std::vector<unsigned char> bytes(prefix.size());
    ReadExactAt(input, offset, bytes, checkpoint);
    return std::equal(prefix.begin(), prefix.end(), bytes.begin());
}

void ValidateBlockIndex(std::istream& input,
    std::uint64_t offset,
    std::uint64_t file_size,
    const std::vector<BlockDescriptor>& blocks,
    const SampleLabelingAsdfReadCheckpoint& checkpoint = {})
{
    constexpr std::string_view header = "#ASDF BLOCK INDEX\n";
    constexpr std::string_view yaml_start = "%YAML 1.1\n---\n";
    constexpr std::string_view yaml_end = "...\n";
    const std::uint64_t size = file_size - offset;
    if (size > kMaximumBlockIndexBytes ||
        size > std::numeric_limits<std::size_t>::max()) {
        Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            "ASDF block index exceeds the production limit");
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    ReadExactAt(input, offset, bytes, checkpoint);
    const std::string_view text(
        reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!text.starts_with(header) ||
        !text.substr(header.size()).starts_with(yaml_start) ||
        !text.ends_with(yaml_end)) {
        Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
            "ASDF block index is truncated or non-standard");
    }

    try {
        const YAML::Node index = YAML::Load(
            std::string(text.substr(header.size())));
        if (!index.IsSequence() || index.size() != blocks.size()) {
            Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                "ASDF block index does not describe every internal block");
        }
        for (std::size_t block_index = 0; block_index < blocks.size();
            ++block_index) {
            const YAML::Node entry = index[block_index];
            if (!entry.IsScalar()) {
                Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                    "ASDF block index entries must be byte offsets");
            }
            const std::string scalar = entry.Scalar();
            if (scalar.empty() ||
                !std::ranges::all_of(scalar,
                    [](unsigned char byte) { return std::isdigit(byte) != 0; })) {
                Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                    "ASDF block index entries must be unsigned byte offsets");
            }
            std::uint64_t value = 0;
            for (const unsigned char digit : scalar) {
                const std::uint64_t next = digit - '0';
                if (value >
                    (std::numeric_limits<std::uint64_t>::max() - next) / 10U) {
                    Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                        "ASDF block index offset overflows");
                }
                value = value * 10U + next;
            }
            if (value != blocks[block_index].raw_offset) {
                Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                    "ASDF block index offset does not match the block stream");
            }
        }
    } catch (const CodecFailure&) {
        throw;
    } catch (const YAML::Exception&) {
        Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
            "ASDF block index YAML is malformed");
    }
}

[[nodiscard]] std::vector<BlockDescriptor> ScanBlocks(std::istream& input,
    std::uint64_t file_size,
    std::uint64_t offset,
    const SampleLabelingAsdfReadCheckpoint& checkpoint = {})
{
    std::vector<BlockDescriptor> blocks;
    while (offset < file_size) {
        std::size_t padding = 0;
        std::array<unsigned char, kIoChunkBytes> padding_chunk{};
        bool found_block = false;
        for (;;) {
            if (offset >= file_size) {
                return blocks;
            }
            const std::size_t chunk_size =
                static_cast<std::size_t>(std::min<std::uint64_t>(
                    file_size - offset, padding_chunk.size()));
            ReadExactAt(input,
                offset,
                std::span<unsigned char>(padding_chunk.data(), chunk_size),
                checkpoint);
            std::size_t consumed = 0;
            for (; consumed < chunk_size; ++consumed) {
                const unsigned char byte = padding_chunk[consumed];
                if (byte != 0 && std::isspace(byte) == 0) {
                    found_block = true;
                    break;
                }
                if (++padding > kMaximumInterBlockPaddingBytes) {
                    Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
                        "ASDF inter-block padding exceeds the production "
                        "limit");
                }
            }
            offset += consumed;
            if (found_block) {
                break;
            }
        }

        if (PrefixAt(
                input, offset, file_size, "#ASDF BLOCK INDEX", checkpoint)) {
            ValidateBlockIndex(input, offset, file_size, blocks, checkpoint);
            return blocks;
        }
        if (blocks.size() >= kMaximumBlockCount) {
            Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
                "ASDF input contains too many internal blocks");
        }
        std::array<unsigned char, 6> prefix{};
        ReadExactAt(input, offset, prefix, checkpoint);
        if (!std::equal(
                kBlockMagic.begin(), kBlockMagic.end(), prefix.begin())) {
            Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                "unexpected data between ASDF blocks");
        }
        const std::uint16_t header_size =
            ReadBigEndian<std::uint16_t>(prefix, 4);
        if (header_size < 48) {
            Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                "ASDF block header is smaller than the standard header");
        }
        if (file_size - offset < 6 || header_size > file_size - offset - 6) {
            Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                "ASDF block header exceeds the input file");
        }
        std::vector<unsigned char> header(header_size);
        ReadExactAt(input, offset + 6, header, checkpoint);

        const std::uint32_t flags = ReadBigEndian<std::uint32_t>(header, 0);
        if (flags != 0) {
            Fail(SampleLabelingAsdfErrorKind::UnsupportedProfile,
                "streamed or flagged ASDF blocks are outside the v1 profile");
        }
        const bool uncompressed = std::all_of(header.begin() + 4,
            header.begin() + 8,
            [](unsigned char byte) { return byte == 0; });
        const bool zlib_compressed = std::equal(kZlibCompression.begin(),
            kZlibCompression.end(),
            header.begin() + 4);
        if (!uncompressed && !zlib_compressed) {
            Fail(SampleLabelingAsdfErrorKind::UnsupportedProfile,
                "unsupported ASDF block compression");
        }

        const std::uint64_t allocated = ReadBigEndian<std::uint64_t>(header, 8);
        const std::uint64_t used = ReadBigEndian<std::uint64_t>(header, 16);
        const std::uint64_t decoded = ReadBigEndian<std::uint64_t>(header, 24);
        if (std::any_of(header.begin() + 32,
                header.begin() + 48,
                [](unsigned char byte) { return byte != 0; })) {
            Fail(SampleLabelingAsdfErrorKind::UnsupportedProfile,
                "checksummed ASDF blocks are outside the supported "
                "production profile");
        }
        if (used > allocated || (uncompressed && decoded != used)) {
            Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                "ASDF block sizes are inconsistent");
        }
        if (used > kMaximumDecodedBlockBytes ||
            decoded > kMaximumDecodedBlockBytes ||
            used > std::numeric_limits<std::size_t>::max() ||
            decoded > std::numeric_limits<std::size_t>::max()) {
            Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
                "ASDF block exceeds the production block-size limit");
        }
        const std::uint64_t payload_offset = offset + 6U + header_size;
        if (payload_offset > file_size ||
            allocated > file_size - payload_offset) {
            Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                "ASDF block payload is truncated");
        }
        const std::uint64_t raw_size = 6U + header_size + allocated;
        blocks.push_back(BlockDescriptor{.raw_offset = offset,
            .raw_size = raw_size,
            .payload_offset = payload_offset,
            .encoded_size = static_cast<std::size_t>(used),
            .decoded_size = static_cast<std::size_t>(decoded),
            .allocated_size = static_cast<std::size_t>(allocated),
            .compression = zlib_compressed ? BlockCompression::Zlib
                                           : BlockCompression::None});
        offset = payload_offset + allocated;
    }
    return blocks;
}

class InflateGuard {
public:
    explicit InflateGuard(z_stream& stream) : stream_(&stream) {}
    ~InflateGuard() { inflateEnd(stream_); }

    InflateGuard(const InflateGuard&) = delete;
    InflateGuard& operator=(const InflateGuard&) = delete;

private:
    z_stream* stream_;
};

[[nodiscard]] std::vector<unsigned char> DecodeBlockPayload(std::istream& input,
    const BlockDescriptor& block,
    std::size_t expected_size,
    const SampleLabelingAsdfReadCheckpoint& checkpoint = {})
{
    if (expected_size > kMaximumDecodedBlockBytes) {
        Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            "ASDF ndarray exceeds the production decode limit");
    }
    if (block.decoded_size != expected_size) {
        Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
            "ASDF block decoded size does not match ndarray shape and "
            "datatype");
    }
    if (block.compression == BlockCompression::None) {
        if (block.encoded_size != expected_size) {
            Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                "uncompressed ASDF block size is inconsistent");
        }
        std::vector<unsigned char> decoded(expected_size);
        ReadExactAt(input, block.payload_offset, decoded, checkpoint);
        return decoded;
    }

    SeekInput(input, block.payload_offset);
    std::vector<unsigned char> decoded(expected_size);
    std::array<unsigned char, kIoChunkBytes> encoded_chunk{};
    unsigned char empty_output = 0;
    z_stream state{};
    if (inflateInit(&state) != Z_OK) {
        Fail(SampleLabelingAsdfErrorKind::IoFailure,
            "zlib decoder initialization failed");
    }
    InflateGuard guard(state);
    std::size_t encoded_remaining = block.encoded_size;
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        PollReadCheckpoint(checkpoint);
        if (state.avail_in == 0 && encoded_remaining != 0) {
            const std::size_t chunk =
                std::min(encoded_remaining, encoded_chunk.size());
            ReadExact(
                input,
                std::span<unsigned char>(encoded_chunk.data(), chunk),
                checkpoint);
            state.next_in = reinterpret_cast<Bytef*>(encoded_chunk.data());
            state.avail_in = static_cast<uInt>(chunk);
            encoded_remaining -= chunk;
        }

        const std::size_t total_out = static_cast<std::size_t>(state.total_out);
        if (total_out > expected_size) {
            Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                "ASDF zlib block expands beyond its declared size");
        }
        const std::size_t output_remaining = expected_size - total_out;
        state.next_out =
            output_remaining == 0
                ? &empty_output
                : reinterpret_cast<Bytef*>(decoded.data() + total_out);
        state.avail_out = static_cast<uInt>(
            output_remaining == 0 ? 1U
                                  : std::min<std::size_t>(output_remaining,
                                        std::numeric_limits<uInt>::max()));

        const uLong before_in = state.total_in;
        const uLong before_out = state.total_out;
        status = inflate(&state, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                "invalid or corrupt ASDF zlib block");
        }
        if (state.total_out > expected_size) {
            Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                "ASDF zlib block expands beyond its declared size");
        }
        if (status != Z_STREAM_END && state.total_in == before_in &&
            state.total_out == before_out) {
            Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                "ASDF zlib decoder made no progress");
        }
    }
    if (encoded_remaining != 0 || state.avail_in != 0 ||
        state.total_in != block.encoded_size ||
        state.total_out != expected_size) {
        Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
            "ASDF zlib block size does not match its declaration");
    }
    return decoded;
}

void RequireUtf8(std::string_view value, std::string_view field)
{
    if (!IsValidUtf8(value)) {
        Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
            "ASDF text is not valid UTF-8: " + std::string(field));
    }
}

[[nodiscard]] std::size_t CountUtf8Codepoints(std::string_view text)
{
    std::size_t count = 0;
    for (std::size_t index = 0; index < text.size();) {
        if (count == kMaximumRosterStringWidth) {
            Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
                "sample roster string width exceeds the production limit");
        }
        index += DecodeValidUtf8Scalar(text, index).width;
        ++count;
    }
    return count;
}

void AppendUtf8(std::string& output, std::uint32_t codepoint)
{
    if (codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
        Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
            "UCS-4 sample roster contains an invalid codepoint");
    }
    if (codepoint <= 0x7fU) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        output.push_back(
            static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
        output.push_back(
            static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
        output.push_back(
            static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
}

[[nodiscard]] std::uint32_t DecodeUint32(std::span<const unsigned char> bytes,
    std::size_t offset,
    bool little_endian)
{
    if (offset > bytes.size() || 4U > bytes.size() - offset) {
        Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
            "ASDF int32 payload is truncated");
    }
    if (little_endian) {
        return static_cast<std::uint32_t>(bytes[offset]) |
               (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
               (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
               (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
    }
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

[[nodiscard]] YAML::Node RequiredNode(const YAML::Node& parent,
    std::string_view key)
{
    if (!parent || !parent.IsMap()) {
        Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
            "required ASDF node is not a map");
    }
    const YAML::Node node = parent[std::string(key)];
    if (!node) {
        Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
            "required ASDF field is missing: " + std::string(key));
    }
    return node;
}

void ValidateYamlMappingsUnambiguous(const YAML::Node& node,
    std::unordered_set<int>& visited_positions)
{
    if (!node || node.IsNull() || node.IsScalar()) {
        return;
    }
    const YAML::Mark mark = node.Mark();
    if (!mark.is_null() &&
        !visited_positions.insert(mark.pos).second) {
        return;
    }

    if (node.IsSequence()) {
        for (const YAML::Node& child : node) {
            ValidateYamlMappingsUnambiguous(child, visited_positions);
        }
        return;
    }
    if (!node.IsMap()) {
        Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
            "ASDF YAML contains an unsupported node kind");
    }

    std::vector<std::string_view> keys;
    keys.reserve(node.size());
    for (const auto& entry : node) {
        if (!entry.first.IsScalar()) {
            Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                "ASDF profile mapping keys must be scalar text");
        }
        keys.emplace_back(entry.first.Scalar());
    }
    std::ranges::sort(keys);
    if (std::ranges::adjacent_find(keys) != keys.end()) {
        Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
            "ASDF YAML contains a duplicate mapping key");
    }
    for (const auto& entry : node) {
        ValidateYamlMappingsUnambiguous(
            entry.second, visited_positions);
    }
}

void ValidateYamlMappingsUnambiguous(const YAML::Node& root)
{
    std::unordered_set<int> visited_positions;
    ValidateYamlMappingsUnambiguous(root, visited_positions);
}

void AddResidentEstimate(std::uint64_t& total, std::uint64_t bytes);

[[nodiscard]] std::uint64_t CanonicalStringResidentBytes(
    std::size_t scalar_bytes)
{
    constexpr std::uint64_t allocation_overhead = 2ULL * sizeof(void*);
    if (scalar_bytes > std::numeric_limits<std::uint64_t>::max() -
            1ULL - allocation_overhead) {
        Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            "canonical YAML scalar size overflows");
    }
    return scalar_bytes + 1ULL + allocation_overhead;
}

class CanonicalMaterializationBudget {
public:
    CanonicalMaterializationBudget(std::size_t metadata_bytes,
        std::uint64_t reusable_prefix_bytes)
        : metadata_bytes_(metadata_bytes),
          reusable_prefix_bytes_(reusable_prefix_bytes)
    {
        Validate(canonical_text_bytes_, label_storage_bytes_, 0);
    }

    void AccountString(std::size_t scalar_bytes)
    {
        const std::uint64_t resident =
            CanonicalStringResidentBytes(scalar_bytes);
        if (canonical_text_bytes_ >
            std::numeric_limits<std::uint64_t>::max() - resident) {
            Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
                "canonical YAML text size overflows");
        }
        const std::uint64_t candidate = canonical_text_bytes_ + resident;
        Validate(candidate, label_storage_bytes_, label_count_);
        canonical_text_bytes_ = candidate;
    }

    void AccountLabels(std::size_t label_count)
    {
        if (label_count > std::numeric_limits<std::uint64_t>::max() /
                sizeof(SampleLabelingDocumentLabel)) {
            Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
                "canonical label storage size overflows");
        }
        const std::uint64_t candidate =
            static_cast<std::uint64_t>(label_count) *
            sizeof(SampleLabelingDocumentLabel);
        Validate(canonical_text_bytes_, candidate, label_count);
        label_storage_bytes_ = candidate;
        label_count_ = label_count;
    }

    [[nodiscard]] std::uint64_t canonical_text_bytes() const noexcept
    {
        return canonical_text_bytes_;
    }

    [[nodiscard]] std::uint64_t label_storage_bytes() const noexcept
    {
        return label_storage_bytes_;
    }

private:
    void Validate(std::uint64_t canonical_text_bytes,
        std::uint64_t label_storage_bytes,
        std::size_t label_count) const
    {
        std::uint64_t estimate = 0;
        AddResidentEstimate(estimate, metadata_bytes_);
        AddResidentEstimate(estimate, metadata_bytes_);
        AddResidentEstimate(estimate, reusable_prefix_bytes_);
        AddResidentEstimate(estimate, kInflateResidentScratchBytes);
        AddResidentEstimate(estimate, canonical_text_bytes);
        AddResidentEstimate(estimate, label_storage_bytes);
        AddResidentEstimate(estimate,
            static_cast<std::uint64_t>(label_count) *
                sizeof(std::size_t));
    }

    std::size_t metadata_bytes_ = 0;
    std::uint64_t reusable_prefix_bytes_ = 0;
    std::uint64_t canonical_text_bytes_ = 0;
    std::uint64_t label_storage_bytes_ = 0;
    std::size_t label_count_ = 0;
};

template <typename Value>
[[nodiscard]] Value RequiredScalar(const YAML::Node& parent,
    std::string_view key,
    CanonicalMaterializationBudget* materialization_budget = nullptr)
{
    const YAML::Node node = RequiredNode(parent, key);
    if (!node.IsScalar()) {
        Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
            "required ASDF field is not scalar: " + std::string(key));
    }
    try {
        if constexpr (std::is_same_v<Value, std::string>) {
            if (materialization_budget != nullptr) {
                materialization_budget->AccountString(node.Scalar().size());
            }
        }
        return node.as<Value>();
    } catch (const YAML::Exception&) {
        Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
            "required ASDF field has the wrong scalar type: " +
                std::string(key));
    }
}

[[nodiscard]] bool IsYaml11ImplicitNonStringScalar(std::string_view value)
{
    using namespace std::regex_constants;
    static const std::regex null_bool_special{
        R"(^(~|null|Null|NULL|y|Y|yes|Yes|YES|n|N|no|No|NO|true|True|TRUE|false|False|FALSE|on|On|ON|off|Off|OFF|<<|=|!|&|\*)$)",
        ECMAScript | optimize};
    static const std::regex integer{
        R"(^[-+]?(0b[0-1_]+|0[0-7_]+|0|[1-9][0-9_]*|0x[0-9a-fA-F_]+|[1-9][0-9_]*(:[0-5]?[0-9])+)$)",
        ECMAScript | optimize};
    static const std::regex floating_point{
        R"(^([-+]?(([0-9][0-9_]*)\.[0-9_]*([eE][-+][0-9]+)?|([0-9][0-9_]*)([eE][-+][0-9]+)|\.[0-9_]+([eE][-+][0-9]+)?|([0-9][0-9_]*)(:[0-5]?[0-9])+\.[0-9_]*|\.(inf|Inf|INF))|\.(nan|NaN|NAN))$)",
        ECMAScript | optimize};
    static const std::regex timestamp{
        R"(^([0-9]{4}-[0-9]{2}-[0-9]{2}|[0-9]{4}-[0-9]{1,2}-[0-9]{1,2}([Tt]|[ \t]+)[0-9]{1,2}:[0-9]{2}:[0-9]{2}(\.[0-9]*)?([ \t]*(Z|[-+][0-9]{1,2}(:[0-9]{2})?))?)$)",
        ECMAScript | optimize};
    return std::regex_match(value.begin(), value.end(), null_bool_special) ||
        std::regex_match(value.begin(), value.end(), integer) ||
        std::regex_match(value.begin(), value.end(), floating_point) ||
        std::regex_match(value.begin(), value.end(), timestamp);
}

[[nodiscard]] bool IsYamlStringScalar(const YAML::Node& node)
{
    if (!node.IsScalar()) {
        return false;
    }
    const std::string tag = node.Tag();
    if (tag == "!" || tag == kYamlStringTag) {
        return true;
    }
    if (tag != "?") {
        return false;
    }
    return !IsYaml11ImplicitNonStringScalar(node.Scalar());
}

[[nodiscard]] std::string RequiredStringScalar(
    const YAML::Node& parent,
    std::string_view key,
    CanonicalMaterializationBudget* materialization_budget = nullptr)
{
    const YAML::Node node = RequiredNode(parent, key);
    if (!IsYamlStringScalar(node)) {
        Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
            "required ASDF field is not a string: " + std::string(key));
    }
    if (materialization_budget != nullptr) {
        materialization_budget->AccountString(node.Scalar().size());
    }
    return node.Scalar();
}

[[nodiscard]] bool HasProfileTag(const YAML::Node& node,
    std::string_view expected)
{
    const std::string tag = node.Tag();
    return tag == "!" + std::string(expected) ||
           tag == "tag:stsci.edu:asdf/" + std::string(expected);
}

struct ArrayDescriptor {
    std::uint64_t source_index = 0;
    std::size_t count = 0;
    std::size_t item_width = 1;
    bool little_endian = true;
};

[[nodiscard]] ArrayDescriptor ParseArrayDescriptor(const YAML::Node& node,
    bool roster,
    CanonicalMaterializationBudget& materialization_budget)
{
    if (!node.IsMap() || !HasProfileTag(node, kSampleLabelingAsdfNdarrayTag)) {
        Fail(SampleLabelingAsdfErrorKind::UnsupportedProfile,
            roster
                ? "sample roster must use the ASDF core ndarray-1.0.0 tag"
                : "annotation values must use the ASDF core ndarray-1.0.0 tag");
    }
    if (node["mask"]) {
        Fail(SampleLabelingAsdfErrorKind::UnsupportedProfile,
            "masked ASDF ndarrays are outside the lossless v1 profile");
    }

    std::uint64_t item_width = 1;
    const YAML::Node datatype = RequiredNode(node, "datatype");
    if (roster) {
        if (!datatype.IsSequence() || datatype.size() != 2 ||
            !datatype[0].IsScalar()) {
            Fail(SampleLabelingAsdfErrorKind::UnsupportedProfile,
                "sample roster ndarray must use UCS-4");
        }
        materialization_budget.AccountString(datatype[0].Scalar().size());
        if (datatype[0].as<std::string>() != "ucs4") {
            Fail(SampleLabelingAsdfErrorKind::UnsupportedProfile,
                "sample roster ndarray must use UCS-4");
        }
        item_width = datatype[1].as<std::uint64_t>();
        if (item_width == 0 || item_width > kMaximumRosterStringWidth) {
            Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
                "sample roster string width exceeds the production limit");
        }
    } else {
        if (!datatype.IsScalar()) {
            Fail(SampleLabelingAsdfErrorKind::UnsupportedProfile,
                "annotation values ndarray must use int32");
        }
        materialization_budget.AccountString(datatype.Scalar().size());
        if (datatype.as<std::string>() != "int32") {
            Fail(SampleLabelingAsdfErrorKind::UnsupportedProfile,
                "annotation values ndarray must use int32");
        }
    }

    const std::string byteorder =
        RequiredScalar<std::string>(
            node, "byteorder", &materialization_budget);
    if (byteorder != "little" && byteorder != "big") {
        Fail(SampleLabelingAsdfErrorKind::UnsupportedProfile,
            "ASDF ndarray byteorder is unsupported");
    }
    const YAML::Node shape = RequiredNode(node, "shape");
    if (!shape.IsSequence() || shape.size() != 1) {
        Fail(SampleLabelingAsdfErrorKind::UnsupportedProfile,
            "ASDF labeling ndarrays must be rank one");
    }
    const std::uint64_t count = shape[0].as<std::uint64_t>();
    if (count > kMaximumSampleCount ||
        count > std::numeric_limits<std::size_t>::max()) {
        Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            "ASDF ndarray sample count exceeds the production limit");
    }
    const std::uint64_t row_bytes = CheckedSizeProduct(item_width,
        sizeof(std::uint32_t),
        roster ? "sample roster row" : "annotation value");
    (void)CheckedSizeProduct(count,
        row_bytes,
        roster ? "sample roster block" : "annotation values block");
    return ArrayDescriptor{
        .source_index = RequiredScalar<std::uint64_t>(node, "source"),
        .count = static_cast<std::size_t>(count),
        .item_width = static_cast<std::size_t>(item_width),
        .little_endian = byteorder == "little"};
}

struct ParsedTree {
    SampleLabelingDocument document;
    std::optional<ArrayDescriptor> roster_array;
    ArrayDescriptor values_array;
    std::uint64_t canonical_text_bytes = 0;
    std::uint64_t label_storage_bytes = 0;
};

struct ProfilePreflight {
    std::size_t source_sample_count = 0;
    std::optional<ArrayDescriptor> roster_array;
    ArrayDescriptor values_array;
    std::size_t label_count = 0;
    std::size_t author_count = 0;
    std::size_t metadata_bytes = 0;
    std::uint64_t canonical_text_bytes = 0;
    std::uint64_t label_storage_bytes = 0;
    std::uint64_t reusable_prefix_bytes = 0;
    std::uint64_t file_bytes = 0;
    std::uint64_t codec_scratch_bytes = 0;
};

[[nodiscard]] ParsedTree ParseTree(const MetadataTree& tree,
    std::uint64_t reusable_prefix_bytes)
{
    try {
        RequireUtf8(tree.bytes, "YAML metadata");
        CanonicalMaterializationBudget materialization_budget(
            tree.bytes.size(), reusable_prefix_bytes);
        const YAML::Node root = YAML::Load(tree.bytes);
        if (!root || !root.IsMap()) {
            Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                "ASDF root is not a map");
        }
        ValidateYamlMappingsUnambiguous(root);
        if (!HasProfileTag(root, kSampleLabelingAsdfRootTag)) {
            Fail(SampleLabelingAsdfErrorKind::UnsupportedProfile,
                "ASDF root must use the core/asdf-1.1.0 tag");
        }

        ParsedTree parsed;
        parsed.document.format_kind =
            RequiredScalar<std::string>(
                root, "format_kind", &materialization_budget);
        parsed.document.schema_version =
            RequiredScalar<std::string>(
                root, "schema_version", &materialization_budget);
        if (parsed.document.format_kind != kSampleLabelingDocumentFormatKind ||
            parsed.document.schema_version !=
                kSampleLabelingDocumentSchemaVersion) {
            Fail(SampleLabelingAsdfErrorKind::UnsupportedProfile,
                "unsupported SpecForge sample-labeling schema identity or "
                "version");
        }

        const YAML::Node source = RequiredNode(root, "source_collection");
        parsed.document.source.kind =
            RequiredScalar<std::string>(
                source, "source_kind", &materialization_budget);
        parsed.document.source.name =
            RequiredScalar<std::string>(
                source, "name", &materialization_budget);
        parsed.document.source.fingerprint =
            RequiredScalar<std::string>(
                source, "fingerprint", &materialization_budget);
        parsed.document.source.base_identity =
            RequiredScalar<std::string>(
                source, "identity", &materialization_budget);
        const std::uint64_t sample_count =
            RequiredScalar<std::uint64_t>(source, "sample_count");
        if (sample_count > kMaximumSampleCount ||
            sample_count > std::numeric_limits<std::size_t>::max()) {
            Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
                "source sample count exceeds the production limit");
        }
        parsed.document.source.sample_count =
            static_cast<std::size_t>(sample_count);

        const YAML::Node roster = RequiredNode(root, "sample_roster");
        parsed.document.source.roster.identity_kind =
            RequiredScalar<std::string>(
                roster, "identity_kind", &materialization_budget);
        if (parsed.document.source.roster.identity_kind ==
            kSampleLabelingDocumentExplicitNamesRoster) {
            parsed.roster_array =
                ParseArrayDescriptor(RequiredNode(roster, "names"),
                    true,
                    materialization_budget);
        } else if (parsed.document.source.roster.identity_kind ==
                   kSampleLabelingDocumentSourceIndexRoster) {
            if (roster["names"]) {
                Fail(SampleLabelingAsdfErrorKind::SemanticValidationFailed,
                    "source-index roster must not contain a names array");
            }
        }

        const YAML::Node annotation = RequiredNode(root, "annotation");
        parsed.document.annotation.kind =
            RequiredScalar<std::string>(
                annotation, "kind", &materialization_budget);
        if (annotation["name"]) {
            Fail(SampleLabelingAsdfErrorKind::SemanticValidationFailed,
                "annotation.name is not a schema 2.0 field");
        }
        parsed.values_array =
            ParseArrayDescriptor(RequiredNode(annotation, "values"),
                false,
                materialization_budget);
        const YAML::Node missing = RequiredNode(annotation, "missing");
        parsed.document.annotation.missing.semantic =
            RequiredScalar<std::string>(
                missing, "semantic", &materialization_budget);
        parsed.document.annotation.missing.value =
            RequiredScalar<std::int32_t>(missing, "value");

        const YAML::Node task = RequiredNode(root, "labeling_task");
        parsed.document.labeling.id = RequiredScalar<std::string>(
            task, "id", &materialization_budget);
        parsed.document.labeling.name =
            RequiredScalar<std::string>(
                task, "name", &materialization_budget);
        const std::string created_at = RequiredScalar<std::string>(
            task, "created_at", &materialization_budget);
        const std::string modified_at = RequiredScalar<std::string>(
            task, "modified_at", &materialization_budget);
        const auto parsed_created_at = ParseCanonicalTimestamp(created_at);
        const auto parsed_modified_at = ParseCanonicalTimestamp(modified_at);
        if (!parsed_created_at || !parsed_modified_at) {
            Fail(SampleLabelingAsdfErrorKind::SemanticValidationFailed,
                "labeling timestamps are not canonical UTC milliseconds");
        }
        parsed.document.labeling.canonical_metadata.created_at =
            *parsed_created_at;
        parsed.document.labeling.canonical_metadata.modified_at =
            *parsed_modified_at;

        const YAML::Node origin = RequiredNode(task, "origin");
        if (!origin.IsMap()) {
            Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                "labeling origin must be a map");
        }
        SampleLabelingOrigin& parsed_origin =
            parsed.document.labeling.canonical_metadata.origin;
        parsed_origin.kind = RequiredScalar<std::string>(
            origin, "kind", &materialization_budget);
        if (origin["annotation"]) {
            const YAML::Node annotation_origin = origin["annotation"];
            if (!annotation_origin.IsMap()) {
                Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                    "labeling annotation origin must be a map");
            }
            SampleLabelingAnnotationOrigin parsed_annotation;
            parsed_annotation.name = RequiredScalar<std::string>(
                annotation_origin, "name", &materialization_budget);
            parsed_annotation.format = RequiredScalar<std::string>(
                annotation_origin, "format", &materialization_budget);
            if (annotation_origin["fingerprint"]) {
                parsed_annotation.fingerprint = RequiredScalar<std::string>(
                    annotation_origin,
                    "fingerprint",
                    &materialization_budget);
            }
            parsed_origin.annotation = std::move(parsed_annotation);
        }
        if (task["description"]) {
            parsed.document.labeling.canonical_metadata.description =
                RequiredScalar<std::string>(
                    task, "description", &materialization_budget);
        }
        if (const YAML::Node authors = task["authors"]) {
            if (!authors.IsSequence()) {
                Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                    "labeling authors must be a sequence");
            }
            if (authors.size() > kMaximumAuthorCount) {
                Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
                    "labeling author count exceeds the production limit");
            }
            parsed.document.labeling.canonical_metadata.authors.reserve(
                authors.size());
            for (const YAML::Node& node : authors) {
                if (!node.IsMap()) {
                    Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                        "labeling author entry must be a map");
                }
                SampleLabelingAuthor author;
                author.name = RequiredScalar<std::string>(
                    node, "name", &materialization_budget);
                if (node["identifier"]) {
                    author.identifier = RequiredScalar<std::string>(
                        node, "identifier", &materialization_budget);
                }
                if (const YAML::Node email = node["email"];
                    email.IsDefined()) {
                    author.email = RequiredStringScalar(
                        node, "email", &materialization_budget);
                }
                parsed.document.labeling.canonical_metadata.authors.push_back(
                    std::move(author));
            }
        }
        const YAML::Node labels = RequiredNode(task, "labels");
        if (!labels.IsSequence()) {
            Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                "label definitions must be a sequence");
        }
        if (labels.size() > kMaximumLabelCount) {
            Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
                "label definition count exceeds the production limit");
        }
        materialization_budget.AccountLabels(labels.size());
        parsed.document.labeling.labels.reserve(labels.size());
        for (const YAML::Node& node : labels) {
            SampleLabelingDocumentLabel label;
            label.code = RequiredScalar<std::int32_t>(node, "code");
            label.name = RequiredScalar<std::string>(
                node, "name", &materialization_budget);
            if (node["shortcut"]) {
                label.shortcut = RequiredScalar<std::string>(
                    node, "shortcut", &materialization_budget);
            }
            parsed.document.labeling.labels.push_back(std::move(label));
        }
        parsed.canonical_text_bytes =
            materialization_budget.canonical_text_bytes();
        parsed.label_storage_bytes =
            materialization_budget.label_storage_bytes();
        return parsed;
    } catch (const CodecFailure&) {
        throw;
    } catch (const YAML::Exception& error) {
        Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
            "ASDF YAML parse failed: " + std::string(error.what()));
    }
}

void ValidateBlockProfile(
    const ParsedTree& parsed,
    const std::vector<BlockDescriptor>& blocks)
{
    if (parsed.roster_array) {
        if (blocks.size() != 2 ||
            parsed.roster_array->source_index != 0 ||
            parsed.values_array.source_index != 1) {
            Fail(SampleLabelingAsdfErrorKind::UnsupportedProfile,
                "explicit-roster ASDF must contain exactly roster block 0 "
                "and values block 1");
        }
        return;
    }
    if (blocks.size() != 1 || parsed.values_array.source_index != 0) {
        Fail(SampleLabelingAsdfErrorKind::UnsupportedProfile,
            "source-index ASDF must contain exactly values block 0");
    }
}

[[nodiscard]] bool HasDefaultZlibFlevel(std::istream& input,
    const BlockDescriptor& block,
    const SampleLabelingAsdfReadCheckpoint& checkpoint = {})
{
    if (block.compression != BlockCompression::Zlib ||
        block.encoded_size < 2U) {
        return false;
    }
    std::array<unsigned char, 2> zlib_header{};
    ReadExactAt(input, block.payload_offset, zlib_header, checkpoint);
    return (zlib_header[1] >> 6U) == 2U;
}

[[nodiscard]] bool HasReusablePrefixProfile(std::istream& input,
    const ParsedTree& parsed,
    const std::vector<BlockDescriptor>& blocks,
    const SampleLabelingAsdfReadCheckpoint& checkpoint = {})
{
    bool reusable = parsed.values_array.little_endian;
    if (parsed.roster_array) {
        const ArrayDescriptor& roster = *parsed.roster_array;
        return reusable && blocks.size() == 2 && roster.source_index == 0 &&
               parsed.values_array.source_index == 1 &&
               roster.little_endian &&
               blocks[0].compression == BlockCompression::Zlib &&
               blocks[0].allocated_size == blocks[0].encoded_size &&
               HasDefaultZlibFlevel(input, blocks[0], checkpoint);
    }
    return reusable && blocks.size() == 1 &&
           parsed.values_array.source_index == 0;
}

void AddResidentEstimate(std::uint64_t& total, std::uint64_t bytes)
{
    if (bytes > g_maximum_resident_codec_bytes ||
        total > g_maximum_resident_codec_bytes - bytes) {
        Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            "ASDF document exceeds the codec-controlled resident-memory budget");
    }
    total += bytes;
}

void ValidateProfilePreflight(const ProfilePreflight& profile)
{
    if (profile.source_sample_count > kMaximumSampleCount ||
        profile.values_array.count > kMaximumSampleCount) {
        Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            "sample count exceeds the production profile limit");
    }
    if (profile.label_count > kMaximumLabelCount) {
        Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            "label definition count exceeds the production profile limit");
    }
    if (profile.author_count > kMaximumAuthorCount) {
        Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            "labeling author count exceeds the production profile limit");
    }
    if (profile.metadata_bytes > kMaximumMetadataBytes) {
        Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            "ASDF YAML metadata exceeds the production profile limit");
    }
    if (profile.reusable_prefix_bytes > kMaximumReusablePrefixBytes ||
        profile.reusable_prefix_bytes >
            std::numeric_limits<std::size_t>::max()) {
        Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            "reusable ASDF prefix exceeds the production profile limit");
    }
    if (profile.file_bytes > kMaximumAsdfFileBytes) {
        Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            "ASDF document exceeds the production file-size limit");
    }
    if (profile.values_array.item_width != 1U) {
        Fail(SampleLabelingAsdfErrorKind::UnsupportedProfile,
            "annotation values must use the int32 profile width");
    }
    (void)CheckedSizeProduct(profile.values_array.count,
        sizeof(std::int32_t),
        "annotation values block");
    if (profile.values_array.count != profile.source_sample_count) {
        Fail(SampleLabelingAsdfErrorKind::SemanticValidationFailed,
            "annotation values shape does not match source sample count");
    }

    if (profile.roster_array) {
        const ArrayDescriptor& roster = *profile.roster_array;
        if (roster.count > kMaximumSampleCount || roster.item_width == 0U ||
            roster.item_width > kMaximumRosterStringWidth) {
            Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
                "sample roster shape exceeds the production profile limit");
        }
        const std::size_t row_bytes = CheckedSizeProduct(
            roster.item_width, sizeof(std::uint32_t), "sample roster row");
        (void)CheckedSizeProduct(
            roster.count, row_bytes, "sample roster block");
        if (roster.count != profile.source_sample_count) {
            Fail(SampleLabelingAsdfErrorKind::SemanticValidationFailed,
                "sample roster shape does not match source sample count");
        }
    }

    std::uint64_t estimate = 0;
    AddResidentEstimate(estimate, profile.metadata_bytes);
    AddResidentEstimate(estimate, profile.metadata_bytes);
    AddResidentEstimate(estimate, profile.canonical_text_bytes);
    AddResidentEstimate(estimate, profile.label_storage_bytes);
    AddResidentEstimate(estimate, profile.reusable_prefix_bytes);
    AddResidentEstimate(estimate, profile.codec_scratch_bytes);

    if (profile.roster_array) {
        const ArrayDescriptor& roster = *profile.roster_array;
        const std::size_t row_bytes = CheckedSizeProduct(
            roster.item_width, sizeof(std::uint32_t), "sample roster row");
        const std::size_t decoded_bytes = CheckedSizeProduct(
            roster.count, row_bytes, "sample roster block");
        // During hydration the decoded UCS-4 buffer coexists with the final
        // UTF-8 strings. A UTF-8 code point never occupies more bytes than its
        // UCS-4 input slot, so decoded_bytes is a conservative string-data
        // bound in addition to the string-object array itself.
        AddResidentEstimate(estimate, decoded_bytes);
        AddResidentEstimate(estimate, decoded_bytes);
        AddResidentEstimate(estimate,
            static_cast<std::uint64_t>(roster.count) * sizeof(std::string));
        // Canonical semantic validation sorts a roster-sized index vector to
        // detect duplicate names without copying strings into hash nodes.
        AddResidentEstimate(estimate,
            static_cast<std::uint64_t>(roster.count) * sizeof(std::size_t));
    }

    AddResidentEstimate(estimate,
        static_cast<std::uint64_t>(profile.label_count) *
            sizeof(std::size_t));

    const std::size_t values_bytes = CheckedSizeProduct(
        profile.values_array.count,
        sizeof(std::int32_t),
        "annotation values block");
    // DecodeValues currently owns the decoded byte buffer while constructing
    // the canonical int32 vector, so both allocations count toward the bound.
    AddResidentEstimate(estimate, values_bytes);
    AddResidentEstimate(estimate, values_bytes);
}

[[nodiscard]] ProfilePreflight BuildReaderProfilePreflight(
    const ParsedTree& parsed,
    std::size_t metadata_bytes,
    std::uint64_t reusable_prefix_bytes,
    std::uint64_t file_bytes)
{
    return ProfilePreflight{
        .source_sample_count = parsed.document.source.sample_count,
        .roster_array = parsed.roster_array,
        .values_array = parsed.values_array,
        .label_count = parsed.document.labeling.labels.size(),
        .author_count =
            parsed.document.labeling.canonical_metadata.authors.size(),
        .metadata_bytes = metadata_bytes,
        .canonical_text_bytes = parsed.canonical_text_bytes,
        .label_storage_bytes = parsed.label_storage_bytes,
        .reusable_prefix_bytes = reusable_prefix_bytes,
        .file_bytes = file_bytes,
        .codec_scratch_bytes = kInflateResidentScratchBytes};
}

[[nodiscard]] const BlockDescriptor& ReferencedBlock(
    const std::vector<BlockDescriptor>& blocks,
    std::uint64_t source_index,
    std::string_view description)
{
    if (source_index >= blocks.size()) {
        Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
            std::string(description) + " references a missing ASDF block");
    }
    return blocks[static_cast<std::size_t>(source_index)];
}

void DecodeRoster(std::istream& input,
    ParsedTree& parsed,
    const std::vector<BlockDescriptor>& blocks,
    const SampleLabelingAsdfReadCheckpoint& checkpoint = {})
{
    if (!parsed.roster_array) {
        return;
    }
    const ArrayDescriptor& array = *parsed.roster_array;
    const std::size_t row_bytes = CheckedSizeProduct(
        array.item_width, sizeof(std::uint32_t), "sample roster row");
    const std::size_t expected_size =
        CheckedSizeProduct(array.count, row_bytes, "sample roster block");
    const std::vector<unsigned char> payload = DecodeBlockPayload(input,
        ReferencedBlock(blocks, array.source_index, "sample roster"),
        expected_size,
        checkpoint);

    parsed.document.source.roster.sample_names.reserve(array.count);
    for (std::size_t row = 0; row < array.count; ++row) {
        if ((row & 0xfffU) == 0U) {
            PollReadCheckpoint(checkpoint);
        }
        std::string decoded;
        bool padding = false;
        for (std::size_t column = 0; column < array.item_width; ++column) {
            if (column != 0U && (column & 0x3fffU) == 0U) {
                PollReadCheckpoint(checkpoint);
            }
            const std::uint32_t codepoint = DecodeUint32(payload,
                row * row_bytes + column * sizeof(std::uint32_t),
                array.little_endian);
            if (codepoint == 0) {
                padding = true;
                continue;
            }
            if (padding) {
                Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                    "sample roster UCS-4 padding is non-terminal");
            }
            AppendUtf8(decoded, codepoint);
        }
        parsed.document.source.roster.sample_names.push_back(
            std::move(decoded));
    }
}

[[nodiscard]] std::vector<std::int32_t> DecodeValues(std::istream& input,
    const ArrayDescriptor& array,
    const std::vector<BlockDescriptor>& blocks,
    const SampleLabelingAsdfReadCheckpoint& checkpoint = {})
{
    const std::size_t expected_size = CheckedSizeProduct(
        array.count, sizeof(std::int32_t), "annotation values block");
    const std::vector<unsigned char> payload = DecodeBlockPayload(input,
        ReferencedBlock(blocks, array.source_index, "annotation values"),
        expected_size,
        checkpoint);
    std::vector<std::int32_t> values;
    values.reserve(array.count);
    for (std::size_t index = 0; index < array.count; ++index) {
        if ((index & 0xfffU) == 0U) {
            PollReadCheckpoint(checkpoint);
        }
        values.push_back(static_cast<std::int32_t>(DecodeUint32(
            payload, index * sizeof(std::int32_t), array.little_endian)));
    }
    return values;
}

void ValidateDocumentText(const SampleLabelingDocument& document)
{
    RequireUtf8(document.format_kind, "format_kind");
    RequireUtf8(document.schema_version, "schema_version");
    RequireUtf8(document.source.base_identity, "source_collection.identity");
    RequireUtf8(document.source.kind, "source_collection.source_kind");
    RequireUtf8(document.source.name, "source_collection.name");
    RequireUtf8(document.source.fingerprint, "source_collection.fingerprint");
    RequireUtf8(
        document.source.roster.identity_kind, "sample_roster.identity_kind");
    for (const std::string& name : document.source.roster.sample_names) {
        RequireUtf8(name, "sample_roster.names");
        if (name.find('\0') != std::string::npos) {
            Fail(SampleLabelingAsdfErrorKind::SemanticValidationFailed,
                "sample roster names cannot contain U+0000 because zero is "
                "reserved for UCS-4 padding");
        }
    }
    RequireUtf8(document.annotation.kind, "annotation.kind");
    RequireUtf8(
        document.annotation.missing.semantic, "annotation.missing.semantic");
    RequireUtf8(document.labeling.id, "labeling_task.id");
    RequireUtf8(document.labeling.name, "labeling_task.name");
    const SampleLabelingTaskCanonicalMetadata& metadata =
        document.labeling.canonical_metadata;
    RequireUtf8(metadata.origin.kind, "labeling_task.origin.kind");
    if (metadata.origin.annotation) {
        RequireUtf8(metadata.origin.annotation->name,
            "labeling_task.origin.annotation.name");
        RequireUtf8(metadata.origin.annotation->format,
            "labeling_task.origin.annotation.format");
        if (metadata.origin.annotation->fingerprint) {
            RequireUtf8(*metadata.origin.annotation->fingerprint,
                "labeling_task.origin.annotation.fingerprint");
        }
    }
    if (metadata.description) {
        RequireUtf8(*metadata.description, "labeling_task.description");
    }
    for (const SampleLabelingAuthor& author : metadata.authors) {
        RequireUtf8(author.name, "labeling_task.authors.name");
        if (author.identifier) {
            RequireUtf8(
                *author.identifier, "labeling_task.authors.identifier");
        }
        if (author.email) {
            RequireUtf8(*author.email, "labeling_task.authors.email");
        }
    }
    for (const SampleLabelingDocumentLabel& label : document.labeling.labels) {
        RequireUtf8(label.name, "labeling_task.labels.name");
        RequireUtf8(label.shortcut, "labeling_task.labels.shortcut");
    }
}

void ValidateDocumentBusinessSemantics(
    const SampleLabelingDocument& document)
{
    const SampleLabelingDocumentValidationResult validation =
        ValidateSampleLabelingDocumentFailFast(document);
    if (!validation.valid()) {
        Fail(SampleLabelingAsdfErrorKind::SemanticValidationFailed,
            "SpecForge sample-labeling semantic validation failed");
    }
}

void ValidateCurrentWriterOrigin(
    const SampleLabelingTaskCanonicalMetadata& metadata)
{
    if (metadata.origin.kind != "manual" &&
        metadata.origin.kind != "annotation_promotion") {
        Fail(SampleLabelingAsdfErrorKind::SemanticValidationFailed,
            "the current writer only emits manual or annotation_promotion origins");
    }
}

void ValidateDocumentSemantics(const SampleLabelingDocument& document)
{
    ValidateDocumentText(document);
    ValidateDocumentBusinessSemantics(document);
}

void WriteQuotedYaml(std::ostream& output, std::string_view text)
{
    constexpr std::string_view hex = "0123456789abcdef";
    output.put('"');
    for (std::size_t index = 0; index < text.size();) {
        const Utf8Scalar scalar = DecodeValidUtf8Scalar(text, index);
        switch (scalar.codepoint) {
        case 0x00U:
            output << "\\0";
            break;
        case 0x07U:
            output << "\\a";
            break;
        case 0x08U:
            output << "\\b";
            break;
        case 0x09U:
            output << "\\t";
            break;
        case 0x0aU:
            output << "\\n";
            break;
        case 0x0bU:
            output << "\\v";
            break;
        case 0x0cU:
            output << "\\f";
            break;
        case 0x0dU:
            output << "\\r";
            break;
        case 0x1bU:
            output << "\\e";
            break;
        case 0x22U:
            output << "\\\"";
            break;
        case 0x5cU:
            output << "\\\\";
            break;
        case 0x85U:
            output << "\\u0085";
            break;
        case 0xa0U:
            output << "\\u00a0";
            break;
        case 0x2028U:
            output << "\\u2028";
            break;
        case 0x2029U:
            output << "\\u2029";
            break;
        case 0xfeffU:
            output << "\\ufeff";
            break;
        case 0xfffeU:
            output << "\\ufffe";
            break;
        case 0xffffU:
            output << "\\uffff";
            break;
        default:
            if (scalar.codepoint <= 0x1fU ||
                (scalar.codepoint >= 0x7fU &&
                    scalar.codepoint <= 0x9fU)) {
                output << "\\u"
                       << hex[(scalar.codepoint >> 12U) & 0xfU]
                       << hex[(scalar.codepoint >> 8U) & 0xfU]
                       << hex[(scalar.codepoint >> 4U) & 0xfU]
                       << hex[scalar.codepoint & 0xfU];
            } else {
                output.write(text.data() + index,
                    static_cast<std::streamsize>(scalar.width));
            }
        }
        index += scalar.width;
    }
    output.put('"');
}

class DeflateSpool {
public:
    DeflateSpool()
    {
#ifdef _WIN32
        if (::tmpfile_s(&file_) != 0) {
            file_ = nullptr;
        }
#else
        file_ = std::tmpfile();
#endif
        if (file_ == nullptr) {
            Fail(SampleLabelingAsdfErrorKind::IoFailure,
                "could not create a temporary ASDF compression spool");
        }
        if (deflateInit(
                &stream_, kSampleLabelingAsdfZlibCompressionLevel) != Z_OK) {
            std::fclose(file_);
            file_ = nullptr;
            Fail(SampleLabelingAsdfErrorKind::IoFailure,
                "zlib encoder initialization failed");
        }
        initialized_ = true;
    }

    ~DeflateSpool()
    {
        if (initialized_) {
            deflateEnd(&stream_);
        }
        if (file_ != nullptr) {
            std::fclose(file_);
        }
    }

    DeflateSpool(const DeflateSpool&) = delete;
    DeflateSpool& operator=(const DeflateSpool&) = delete;

    void Feed(std::span<const unsigned char> bytes)
    {
        if (finished_ ||
            bytes.size() > kMaximumDecodedBlockBytes - decoded_size_) {
            Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
                "ASDF block exceeds the production zlib limit");
        }
        decoded_size_ += bytes.size();
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const std::size_t chunk = std::min<std::size_t>(
                bytes.size() - offset,
                std::numeric_limits<uInt>::max());
            stream_.next_in = const_cast<Bytef*>(
                reinterpret_cast<const Bytef*>(bytes.data() + offset));
            stream_.avail_in = static_cast<uInt>(chunk);
            while (stream_.avail_in != 0U) {
                DeflateStep(Z_NO_FLUSH);
            }
            offset += chunk;
        }
    }

    void Finish()
    {
        if (finished_) {
            return;
        }
        int status = Z_OK;
        do {
            status = DeflateStep(Z_FINISH);
        } while (status != Z_STREAM_END);
        if (deflateEnd(&stream_) != Z_OK) {
            initialized_ = false;
            Fail(SampleLabelingAsdfErrorKind::IoFailure,
                "zlib encoder finalization failed");
        }
        initialized_ = false;
        if (std::fflush(file_) != 0 || std::fseek(file_, 0, SEEK_SET) != 0) {
            Fail(SampleLabelingAsdfErrorKind::IoFailure,
                "could not finalize the ASDF compression spool");
        }
        finished_ = true;
    }

    [[nodiscard]] std::uint64_t block_size() const
    {
        return 54ULL + encoded_size_;
    }

    [[nodiscard]] std::uint64_t decoded_size() const noexcept
    {
        return decoded_size_;
    }

    void WriteBlock(std::ostream& output)
    {
        if (!finished_ || std::fseek(file_, 0, SEEK_SET) != 0) {
            Fail(SampleLabelingAsdfErrorKind::IoFailure,
                "ASDF compression spool is not ready for output");
        }
        std::vector<unsigned char> header;
        header.reserve(54U);
        header.insert(header.end(), kBlockMagic.begin(), kBlockMagic.end());
        AppendBigEndian(header, 48, 2);
        AppendBigEndian(header, 0, 4);
        header.insert(
            header.end(), kZlibCompression.begin(), kZlibCompression.end());
        AppendBigEndian(header, encoded_size_, 8);
        AppendBigEndian(header, encoded_size_, 8);
        AppendBigEndian(header, decoded_size_, 8);
        header.insert(header.end(), 16, 0);
        WriteBytes(output, header);

        std::uint64_t remaining = encoded_size_;
        while (remaining != 0U) {
            const std::size_t chunk = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, output_buffer_.size()));
            const std::size_t read =
                std::fread(output_buffer_.data(), 1, chunk, file_);
            if (read != chunk) {
                Fail(SampleLabelingAsdfErrorKind::IoFailure,
                    "could not read the ASDF compression spool");
            }
            WriteBytes(output,
                std::span<const unsigned char>(output_buffer_.data(), read));
            remaining -= read;
        }
    }

private:
    int DeflateStep(int flush)
    {
        stream_.next_out = output_buffer_.data();
        stream_.avail_out = static_cast<uInt>(output_buffer_.size());
        const int status = deflate(&stream_, flush);
        if (status != Z_OK && status != Z_STREAM_END) {
            Fail(SampleLabelingAsdfErrorKind::IoFailure,
                "zlib compression failed");
        }
        const std::size_t produced = output_buffer_.size() - stream_.avail_out;
        if (produced != 0U) {
            if (produced > kMaximumDecodedBlockBytes - encoded_size_) {
                Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
                    "compressed ASDF block exceeds the production limit");
            }
            if (std::fwrite(output_buffer_.data(), 1, produced, file_) !=
                produced) {
                Fail(SampleLabelingAsdfErrorKind::IoFailure,
                    "could not write the ASDF compression spool");
            }
            encoded_size_ += produced;
        }
        return status;
    }

    std::FILE* file_ = nullptr;
    z_stream stream_{};
    std::array<unsigned char, kIoChunkBytes> output_buffer_{};
    std::uint64_t decoded_size_ = 0;
    std::uint64_t encoded_size_ = 0;
    bool initialized_ = false;
    bool finished_ = false;
};

class LittleEndianWordDeflater {
public:
    explicit LittleEndianWordDeflater(DeflateSpool& spool) : spool_(spool) {}

    void Append(std::uint32_t value)
    {
        if (used_ + sizeof(value) > buffer_.size()) {
            Flush();
        }
        buffer_[used_++] = static_cast<unsigned char>(value & 0xffU);
        buffer_[used_++] = static_cast<unsigned char>((value >> 8U) & 0xffU);
        buffer_[used_++] = static_cast<unsigned char>((value >> 16U) & 0xffU);
        buffer_[used_++] = static_cast<unsigned char>((value >> 24U) & 0xffU);
    }

    void Finish() { Flush(); }

private:
    void Flush()
    {
        spool_.Feed(
            std::span<const unsigned char>(buffer_.data(), used_));
        used_ = 0;
    }

    DeflateSpool& spool_;
    std::array<unsigned char, kIoChunkBytes> buffer_{};
    std::size_t used_ = 0;
};

[[nodiscard]] std::size_t MeasureRosterStringWidth(
    const std::vector<std::string>& sample_names)
{
    std::size_t string_width = 0;
    for (const std::string& name : sample_names) {
        string_width = std::max(string_width, CountUtf8Codepoints(name));
    }
    string_width = std::max<std::size_t>(string_width, 1);
    if (string_width > kMaximumRosterStringWidth) {
        Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            "sample roster string width exceeds the production limit");
    }
    return string_width;
}

[[nodiscard]] std::unique_ptr<DeflateSpool> EncodeRosterToSpool(
    const std::vector<std::string>& sample_names,
    std::size_t string_width)
{
    const std::size_t row_bytes = CheckedSizeProduct(
        string_width, sizeof(std::uint32_t), "sample roster row");
    const std::size_t payload_size = CheckedSizeProduct(
        sample_names.size(), row_bytes, "sample roster block");
    auto spool = std::make_unique<DeflateSpool>();
    LittleEndianWordDeflater deflater(*spool);
    for (const std::string& name : sample_names) {
        std::size_t codepoint_count = 0;
        for (std::size_t offset = 0; offset < name.size();) {
            const Utf8Scalar scalar = DecodeValidUtf8Scalar(name, offset);
            deflater.Append(scalar.codepoint);
            offset += scalar.width;
            ++codepoint_count;
        }
        for (std::size_t index = codepoint_count;
            index < string_width;
            ++index) {
            deflater.Append(0);
        }
    }
    deflater.Finish();
    spool->Finish();
    if (spool->decoded_size() != payload_size) {
        Fail(SampleLabelingAsdfErrorKind::IoFailure,
            "sample roster encoder produced an inconsistent block size");
    }
    return spool;
}

[[nodiscard]] std::unique_ptr<DeflateSpool> EncodeValuesToSpool(
    std::span<const std::int32_t> values)
{
    const std::size_t payload_size = CheckedSizeProduct(
        values.size(), sizeof(std::int32_t), "annotation values block");
    auto spool = std::make_unique<DeflateSpool>();
    LittleEndianWordDeflater deflater(*spool);
    for (const std::int32_t value : values) {
        deflater.Append(static_cast<std::uint32_t>(value));
    }
    deflater.Finish();
    spool->Finish();
    if (spool->decoded_size() != payload_size) {
        Fail(SampleLabelingAsdfErrorKind::IoFailure,
            "annotation values encoder produced an inconsistent block size");
    }
    return spool;
}

void EmitMetadata(std::ostream& metadata,
    const SampleLabelingDocument& document,
    std::size_t roster_width)
{
    const bool explicit_roster = document.source.roster.identity_kind ==
                                 kSampleLabelingDocumentExplicitNamesRoster;
    const std::size_t values_source = explicit_roster ? 1U : 0U;
    metadata << "#ASDF " << kSampleLabelingAsdfFileFormatVersion << "\n"
             << "#ASDF_STANDARD " << kSampleLabelingAsdfStandardVersion << "\n"
             << "%YAML 1.1\n"
             << "%TAG ! tag:stsci.edu:asdf/\n"
             << "--- !core/asdf-1.1.0\n"
             << "asdf_library: !core/software-1.0.0 {name: SpecForge, version: "
             << build_info::kSpecForgeVersion << "}\n"
             << "format_kind: ";
    WriteQuotedYaml(metadata, document.format_kind);
    metadata << "\nschema_version: ";
    WriteQuotedYaml(metadata, document.schema_version);
    metadata << "\nsource_collection:\n  identity: ";
    WriteQuotedYaml(metadata, document.source.base_identity);
    metadata << "\n  source_kind: ";
    WriteQuotedYaml(metadata, document.source.kind);
    metadata << "\n  name: ";
    WriteQuotedYaml(metadata, document.source.name);
    metadata << "\n  fingerprint: ";
    WriteQuotedYaml(metadata, document.source.fingerprint);
    metadata << "\n  sample_count: " << document.source.sample_count
             << "\nsample_roster:\n  identity_kind: ";
    WriteQuotedYaml(metadata, document.source.roster.identity_kind);
    metadata << "\n";
    if (explicit_roster) {
        metadata << "  names: !core/ndarray-1.0.0\n"
                 << "    source: 0\n"
                 << "    datatype: [ucs4, " << roster_width << "]\n"
                 << "    byteorder: little\n"
                 << "    shape: ["
                 << document.source.roster.sample_names.size() << "]\n";
    }
    metadata << "annotation:\n  kind: ";
    WriteQuotedYaml(metadata, document.annotation.kind);
    metadata << "\n  values: !core/ndarray-1.0.0\n"
             << "    source: " << values_source << "\n"
             << "    datatype: int32\n"
             << "    byteorder: little\n"
             << "    shape: [" << document.annotation.values.size() << "]\n"
             << "  missing:\n    semantic: ";
    WriteQuotedYaml(metadata, document.annotation.missing.semantic);
    metadata << "\n    value: " << document.annotation.missing.value
             << "\nlabeling_task:\n  id: ";
    WriteQuotedYaml(metadata, document.labeling.id);
    metadata << "\n  name: ";
    WriteQuotedYaml(metadata, document.labeling.name);
    const SampleLabelingTaskCanonicalMetadata& canonical_metadata =
        document.labeling.canonical_metadata;
    metadata << "\n  created_at: ";
    WriteQuotedYaml(metadata,
        FormatCanonicalTimestamp(canonical_metadata.created_at));
    metadata << "\n  modified_at: ";
    WriteQuotedYaml(metadata,
        FormatCanonicalTimestamp(canonical_metadata.modified_at));
    metadata << "\n  origin:\n    kind: ";
    WriteQuotedYaml(metadata, canonical_metadata.origin.kind);
    if (canonical_metadata.origin.annotation) {
        const SampleLabelingAnnotationOrigin& origin_annotation =
            *canonical_metadata.origin.annotation;
        metadata << "\n    annotation:\n      name: ";
        WriteQuotedYaml(metadata, origin_annotation.name);
        metadata << "\n      format: ";
        WriteQuotedYaml(metadata, origin_annotation.format);
        if (origin_annotation.fingerprint) {
            metadata << "\n      fingerprint: ";
            WriteQuotedYaml(metadata, *origin_annotation.fingerprint);
        }
    }
    if (canonical_metadata.description) {
        metadata << "\n  description: ";
        WriteQuotedYaml(metadata, *canonical_metadata.description);
    }
    if (!canonical_metadata.authors.empty()) {
        metadata << "\n  authors:\n";
        for (const SampleLabelingAuthor& author :
            canonical_metadata.authors) {
            metadata << "  - name: ";
            WriteQuotedYaml(metadata, author.name);
            metadata << "\n";
            if (author.identifier) {
                metadata << "    identifier: ";
                WriteQuotedYaml(metadata, *author.identifier);
                metadata << "\n";
            }
            if (author.email) {
                metadata << "    email: ";
                WriteQuotedYaml(metadata, *author.email);
                metadata << "\n";
            }
        }
    } else {
        metadata << "\n";
    }
    if (document.labeling.labels.empty()) {
        metadata << "  labels: []\n";
    } else {
        metadata << "  labels:\n";
        for (const SampleLabelingDocumentLabel& label :
            document.labeling.labels) {
            metadata << "  - code: " << label.code << "\n    name: ";
            WriteQuotedYaml(metadata, label.name);
            metadata << "\n";
            if (!label.shortcut.empty()) {
                metadata << "    shortcut: ";
                WriteQuotedYaml(metadata, label.shortcut);
                metadata << "\n";
            }
        }
    }
    metadata << "...\n";
}

class CountingStreamBuffer final : public std::streambuf {
public:
    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }

protected:
    std::streamsize xsputn(const char*, std::streamsize count) override
    {
        if (count < 0 || static_cast<std::uint64_t>(count) >
                std::numeric_limits<std::uint64_t>::max() - size_) {
            return 0;
        }
        size_ += static_cast<std::uint64_t>(count);
        return count;
    }

    int_type overflow(int_type character) override
    {
        if (traits_type::eq_int_type(character, traits_type::eof())) {
            return traits_type::not_eof(character);
        }
        if (size_ == std::numeric_limits<std::uint64_t>::max()) {
            return traits_type::eof();
        }
        ++size_;
        return character;
    }

private:
    std::uint64_t size_ = 0;
};

class StringAppendStreamBuffer final : public std::streambuf {
public:
    explicit StringAppendStreamBuffer(std::string& output,
        std::size_t maximum_size =
            std::numeric_limits<std::size_t>::max())
        : output_(output), maximum_size_(maximum_size)
    {
    }

protected:
    std::streamsize xsputn(const char* text, std::streamsize count) override
    {
        if (count < 0) {
            return 0;
        }
        const std::size_t size = static_cast<std::size_t>(count);
        if (output_.size() > maximum_size_ ||
            size > maximum_size_ - output_.size()) {
            return 0;
        }
        output_.append(text, size);
        return count;
    }

    int_type overflow(int_type character) override
    {
        if (traits_type::eq_int_type(character, traits_type::eof())) {
            return traits_type::not_eof(character);
        }
        if (output_.size() >= maximum_size_) {
            return traits_type::eof();
        }
        output_.push_back(traits_type::to_char_type(character));
        return character;
    }

private:
    std::string& output_;
    std::size_t maximum_size_;
};

[[nodiscard]] std::size_t MeasureMetadataSize(
    const SampleLabelingDocument& document,
    std::size_t roster_width)
{
    CountingStreamBuffer buffer;
    std::ostream metadata(&buffer);
    EmitMetadata(metadata, document, roster_width);
    if (!metadata || buffer.size() > kMaximumMetadataBytes ||
        buffer.size() > std::numeric_limits<std::size_t>::max()) {
        Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            "ASDF YAML metadata exceeds the production limit");
    }
    return static_cast<std::size_t>(buffer.size());
}

[[nodiscard]] std::string BuildMetadata(
    const SampleLabelingDocument& document,
    std::size_t roster_width,
    std::size_t measured_size)
{
    std::string result;
    result.reserve(measured_size);
    StringAppendStreamBuffer buffer(result);
    std::ostream metadata(&buffer);
    EmitMetadata(metadata, document, roster_width);
    if (!metadata || result.size() != measured_size) {
        Fail(SampleLabelingAsdfErrorKind::IoFailure,
            "ASDF YAML metadata size changed during emission");
    }
    return result;
}

[[nodiscard]] YAML::Node YamlStringNode(std::string_view value)
{
    YAML::Node node{std::string(value)};
    node.SetTag(std::string{kYamlStringTag});
    return node;
}

void SetYamlString(YAML::Node parent,
    std::string_view key,
    std::string_view value)
{
    parent[std::string(key)] = YamlStringNode(value);
}

void PreserveParsedStringScalarTypes(YAML::Node node,
    std::unordered_set<int>& visited_positions)
{
    if (!node || node.IsNull()) {
        return;
    }
    const YAML::Mark mark = node.Mark();
    if (!mark.is_null() &&
        !visited_positions.insert(mark.pos).second) {
        return;
    }
    if (node.IsScalar()) {
        if (node.Tag() == "!") {
            node.SetTag(std::string{kYamlStringTag});
        }
        return;
    }
    if (node.IsSequence()) {
        for (YAML::Node child : node) {
            PreserveParsedStringScalarTypes(
                child, visited_positions);
        }
        return;
    }
    if (node.IsMap()) {
        for (const auto& entry : node) {
            PreserveParsedStringScalarTypes(
                entry.first, visited_positions);
            PreserveParsedStringScalarTypes(
                entry.second, visited_positions);
        }
    }
}

void PreserveParsedStringScalarTypes(YAML::Node root)
{
    std::unordered_set<int> visited_positions;
    PreserveParsedStringScalarTypes(
        std::move(root), visited_positions);
}

void SetCanonicalArrayDescriptor(YAML::Node node,
    std::uint64_t source_index,
    std::size_t count,
    std::optional<std::size_t> roster_width)
{
    node.SetTag("tag:stsci.edu:asdf/" +
                std::string{kSampleLabelingAsdfNdarrayTag});
    node["source"] = source_index;
    if (roster_width) {
        YAML::Node datatype(YAML::NodeType::Sequence);
        datatype.push_back(YamlStringNode("ucs4"));
        datatype.push_back(*roster_width);
        node["datatype"] = std::move(datatype);
    } else {
        node["datatype"] = YamlStringNode("int32");
    }
    node["byteorder"] = YamlStringNode("little");
    YAML::Node shape(YAML::NodeType::Sequence);
    shape.push_back(count);
    node["shape"] = std::move(shape);
}

[[nodiscard]] YAML::Node MapNodeOrNew(YAML::Node parent,
    std::string_view key)
{
    YAML::Node node = parent[std::string(key)];
    if (!node || !node.IsMap()) {
        node = YAML::Node(YAML::NodeType::Map);
        parent[std::string(key)] = node;
    }
    return node;
}

[[nodiscard]] YAML::Node LabelNodeForCode(
    const YAML::Node& labels,
    std::int32_t code)
{
    if (labels && labels.IsSequence()) {
        for (const YAML::Node& label : labels) {
            if (label.IsMap() && label["code"] &&
                label["code"].as<std::int32_t>() == code) {
                return label;
            }
        }
    }
    return YAML::Node(YAML::NodeType::Map);
}

[[nodiscard]] std::string BuildMetadataPreservingUnknownFields(
    std::span<const unsigned char> encoded_metadata,
    std::size_t metadata_bytes,
    const SampleLabelingDocument& document,
    std::size_t roster_width)
{
    if (metadata_bytes > encoded_metadata.size()) {
        Fail(SampleLabelingAsdfErrorKind::IoFailure,
            "durable ASDF metadata prefix is inconsistent");
    }
    const std::string original_metadata(
        reinterpret_cast<const char*>(encoded_metadata.data()),
        metadata_bytes);
    YAML::Node root = YAML::Load(original_metadata);
    if (!root || !root.IsMap()) {
        Fail(SampleLabelingAsdfErrorKind::IoFailure,
            "durable ASDF metadata tree is unavailable");
    }
    PreserveParsedStringScalarTypes(root);

    root.SetTag("tag:stsci.edu:asdf/" +
                std::string{kSampleLabelingAsdfRootTag});
    YAML::Node library = MapNodeOrNew(root, "asdf_library");
    library.SetTag("tag:stsci.edu:asdf/core/software-1.0.0");
    SetYamlString(library, "name", "SpecForge");
    SetYamlString(
        library, "version", build_info::kSpecForgeVersion);
    SetYamlString(root, "format_kind", document.format_kind);
    SetYamlString(root, "schema_version", document.schema_version);

    YAML::Node source = MapNodeOrNew(root, "source_collection");
    SetYamlString(source, "identity", document.source.base_identity);
    SetYamlString(source, "source_kind", document.source.kind);
    SetYamlString(source, "name", document.source.name);
    SetYamlString(source, "fingerprint", document.source.fingerprint);
    source["sample_count"] = document.source.sample_count;

    const bool explicit_roster = document.source.roster.identity_kind ==
                                 kSampleLabelingDocumentExplicitNamesRoster;
    YAML::Node roster = MapNodeOrNew(root, "sample_roster");
    SetYamlString(
        roster, "identity_kind", document.source.roster.identity_kind);
    if (explicit_roster) {
        YAML::Node names = MapNodeOrNew(roster, "names");
        SetCanonicalArrayDescriptor(names,
            0,
            document.source.roster.sample_names.size(),
            roster_width);
    } else {
        roster.remove("names");
    }

    YAML::Node annotation = MapNodeOrNew(root, "annotation");
    SetYamlString(annotation, "kind", document.annotation.kind);
    annotation.remove("name");
    YAML::Node values = MapNodeOrNew(annotation, "values");
    SetCanonicalArrayDescriptor(values,
        explicit_roster ? 1U : 0U,
        document.annotation.values.size(),
        std::nullopt);
    YAML::Node missing = MapNodeOrNew(annotation, "missing");
    SetYamlString(
        missing, "semantic", document.annotation.missing.semantic);
    missing["value"] = document.annotation.missing.value;

    YAML::Node task = MapNodeOrNew(root, "labeling_task");
    SetYamlString(task, "id", document.labeling.id);
    SetYamlString(task, "name", document.labeling.name);
    const SampleLabelingTaskCanonicalMetadata& canonical_metadata =
        document.labeling.canonical_metadata;
    SetYamlString(task,
        "created_at",
        FormatCanonicalTimestamp(canonical_metadata.created_at));
    SetYamlString(task,
        "modified_at",
        FormatCanonicalTimestamp(canonical_metadata.modified_at));
    YAML::Node origin = MapNodeOrNew(task, "origin");
    SetYamlString(origin, "kind", canonical_metadata.origin.kind);
    if (canonical_metadata.origin.annotation) {
        YAML::Node origin_annotation =
            MapNodeOrNew(origin, "annotation");
        SetYamlString(origin_annotation,
            "name",
            canonical_metadata.origin.annotation->name);
        SetYamlString(origin_annotation,
            "format",
            canonical_metadata.origin.annotation->format);
        if (canonical_metadata.origin.annotation->fingerprint) {
            SetYamlString(origin_annotation,
                "fingerprint",
                *canonical_metadata.origin.annotation->fingerprint);
        } else {
            origin_annotation.remove("fingerprint");
        }
    } else {
        origin.remove("annotation");
    }
    if (canonical_metadata.description) {
        SetYamlString(task, "description", *canonical_metadata.description);
    } else {
        task.remove("description");
    }
    if (canonical_metadata.authors.empty()) {
        task.remove("authors");
    } else {
        YAML::Node authors(YAML::NodeType::Sequence);
        for (const SampleLabelingAuthor& author : canonical_metadata.authors) {
            YAML::Node node(YAML::NodeType::Map);
            SetYamlString(node, "name", author.name);
            if (author.identifier) {
                SetYamlString(node, "identifier", *author.identifier);
            }
            if (author.email) {
                SetYamlString(node, "email", *author.email);
            }
            authors.push_back(std::move(node));
        }
        task["authors"] = std::move(authors);
    }
    const YAML::Node previous_labels = task["labels"];
    YAML::Node labels(YAML::NodeType::Sequence);
    for (const SampleLabelingDocumentLabel& label :
        document.labeling.labels) {
        YAML::Node node = LabelNodeForCode(previous_labels, label.code);
        node["code"] = label.code;
        SetYamlString(node, "name", label.name);
        if (label.shortcut.empty()) {
            node.remove("shortcut");
        } else {
            SetYamlString(node, "shortcut", label.shortcut);
        }
        labels.push_back(node);
    }
    task["labels"] = std::move(labels);

    std::string metadata;
    metadata.reserve(std::min<std::size_t>(
        metadata_bytes + 128U, kMaximumMetadataBytes));
    metadata.append("#ASDF ")
        .append(kSampleLabelingAsdfFileFormatVersion)
        .append("\n#ASDF_STANDARD ")
        .append(kSampleLabelingAsdfStandardVersion)
        .append("\n%YAML 1.1\n%TAG ! tag:stsci.edu:asdf/\n--- ");
    StringAppendStreamBuffer buffer(metadata, kMaximumMetadataBytes);
    std::ostream metadata_stream(&buffer);
    YAML::Emitter body(metadata_stream);
    body.SetIndent(2);
    body << root;
    if (!body.good() || !metadata_stream ||
        metadata.size() > kMaximumMetadataBytes - 5U) {
        Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            "preserved ASDF YAML metadata exceeds the production limit");
    }
    metadata.append("\n...\n");
    return metadata;
}

[[nodiscard]] std::uint64_t CanonicalYamlTextResidentBytes(
    const SampleLabelingDocument& document)
{
    std::uint64_t total = 0;
    const auto account = [&total](const std::string& text) {
        AddResidentEstimate(total, CanonicalStringResidentBytes(text.size()));
    };
    account(document.format_kind);
    account(document.schema_version);
    account(document.source.base_identity);
    account(document.source.kind);
    account(document.source.name);
    account(document.source.fingerprint);
    account(document.source.roster.identity_kind);
    account(document.annotation.kind);
    account(document.annotation.missing.semantic);
    account(document.labeling.id);
    account(document.labeling.name);
    account(FormatCanonicalTimestamp(
        document.labeling.canonical_metadata.created_at));
    account(FormatCanonicalTimestamp(
        document.labeling.canonical_metadata.modified_at));
    account(document.labeling.canonical_metadata.origin.kind);
    if (document.labeling.canonical_metadata.origin.annotation) {
        const SampleLabelingAnnotationOrigin& origin_annotation =
            *document.labeling.canonical_metadata.origin.annotation;
        account(origin_annotation.name);
        account(origin_annotation.format);
        if (origin_annotation.fingerprint) {
            account(*origin_annotation.fingerprint);
        }
    }
    if (document.labeling.canonical_metadata.description) {
        account(*document.labeling.canonical_metadata.description);
    }
    for (const SampleLabelingAuthor& author :
        document.labeling.canonical_metadata.authors) {
        account(author.name);
        if (author.identifier) {
            account(*author.identifier);
        }
        if (author.email) {
            account(*author.email);
        }
    }
    for (const SampleLabelingDocumentLabel& label : document.labeling.labels) {
        account(label.name);
        if (!label.shortcut.empty()) {
            account(label.shortcut);
        }
    }
    return total;
}

[[nodiscard]] std::uint64_t CanonicalLabelStorageBytes(
    const SampleLabelingDocument& document)
{
    if (document.labeling.labels.size() >
        std::numeric_limits<std::uint64_t>::max() /
            sizeof(SampleLabelingDocumentLabel)) {
        Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            "canonical label storage size overflows");
    }
    return static_cast<std::uint64_t>(document.labeling.labels.size()) *
           sizeof(SampleLabelingDocumentLabel);
}

[[nodiscard]] ProfilePreflight BuildWriterProfilePreflight(
    const SampleLabelingDocument& document,
    std::size_t roster_width,
    std::size_t metadata_bytes,
    std::uint64_t reusable_prefix_bytes,
    std::uint64_t file_bytes)
{
    ProfilePreflight profile{
        .source_sample_count = document.source.sample_count,
        .values_array = ArrayDescriptor{.source_index = 0,
            .count = document.annotation.values.size(),
            .item_width = 1,
            .little_endian = true},
        .label_count = document.labeling.labels.size(),
        .author_count = document.labeling.canonical_metadata.authors.size(),
        .metadata_bytes = metadata_bytes,
        .canonical_text_bytes =
            CanonicalYamlTextResidentBytes(document),
        .label_storage_bytes = CanonicalLabelStorageBytes(document),
        .reusable_prefix_bytes = reusable_prefix_bytes,
        .file_bytes = file_bytes,
        .codec_scratch_bytes = kDeflateResidentScratchBytes};
    if (document.source.roster.identity_kind ==
        kSampleLabelingDocumentExplicitNamesRoster) {
        profile.roster_array = ArrayDescriptor{
            .source_index = 0,
            .count = document.source.roster.sample_names.size(),
            .item_width = roster_width,
            .little_endian = true};
    }
    return profile;
}

[[nodiscard]] std::vector<unsigned char> ReadReusablePrefix(std::istream& input,
    std::uint64_t size,
    const SampleLabelingAsdfReadCheckpoint& checkpoint = {})
{
    if (size > std::numeric_limits<std::size_t>::max()) {
        Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            "reusable ASDF prefix exceeds the platform allocation limit");
    }
    std::vector<unsigned char> prefix(static_cast<std::size_t>(size));
    ReadExactAt(input, 0, prefix, checkpoint);
    return prefix;
}

[[nodiscard]] SampleLabelingAsdfError ErrorFromFailure(
    const CodecFailure& failure)
{
    return SampleLabelingAsdfError{failure.kind(), failure.what()};
}

[[nodiscard]] SampleLabelingAsdfError UnexpectedError(
    const std::exception& error)
{
    return SampleLabelingAsdfError{SampleLabelingAsdfErrorKind::IoFailure,
        "ASDF codec operation failed: " + std::string(error.what())};
}

}  // namespace

SampleLabelingAsdfReadResult ReadSampleLabelingAsdfDocument(
    const std::filesystem::path& path,
    const SampleLabelingAsdfReadCheckpoint& checkpoint) noexcept
{
    try {
        PollReadCheckpoint(checkpoint);
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            Fail(SampleLabelingAsdfErrorKind::OpenFailed,
                "could not open ASDF labeling document");
        }
        const std::uint64_t file_size = InputFileSize(path);
        const MetadataTree tree = ReadMetadataTree(input, checkpoint);
        if (tree.end_offset > file_size) {
            Fail(SampleLabelingAsdfErrorKind::MalformedDocument,
                "ASDF metadata exceeds the input file");
        }
        std::vector<BlockDescriptor> blocks =
            ScanBlocks(input, file_size, tree.end_offset, checkpoint);
        const std::uint64_t materialization_prefix_bytes =
            blocks.size() >= 2U ? blocks[1].raw_offset : tree.end_offset;
        ParsedTree parsed =
            ParseTree(tree, materialization_prefix_bytes);
        ValidateBlockProfile(parsed, blocks);

        const bool initially_reusable =
            HasReusablePrefixProfile(input, parsed, blocks, checkpoint);
        const bool initially_explicit_roster = parsed.roster_array.has_value();
        std::size_t validated_metadata_bytes = tree.bytes.size();
        std::vector<unsigned char> reusable_prefix;
        if (initially_reusable) {
            const BlockDescriptor& values_block = ReferencedBlock(
                blocks, parsed.values_array.source_index, "annotation values");
            ValidateProfilePreflight(BuildReaderProfilePreflight(
                parsed,
                tree.bytes.size(),
                values_block.raw_offset,
                file_size));
            if (g_before_reusable_prefix_capture != nullptr) {
                g_before_reusable_prefix_capture(path);
            }
            reusable_prefix =
                ReadReusablePrefix(
                    input, values_block.raw_offset, checkpoint);

            // The durable bytes, metadata semantics, and hydrated roster must
            // all come from this one frozen prefix. The source file is never
            // consulted again for metadata or roster content.
            MemoryInputBuffer frozen_buffer(reusable_prefix);
            std::istream frozen_input(&frozen_buffer);
            const MetadataTree frozen_tree =
                ReadMetadataTree(frozen_input, checkpoint);
            std::vector<BlockDescriptor> frozen_blocks = ScanBlocks(
                frozen_input,
                reusable_prefix.size(),
                frozen_tree.end_offset,
                checkpoint);
            if (initially_explicit_roster) {
                if (frozen_blocks.size() != 1) {
                    Fail(SampleLabelingAsdfErrorKind::IoFailure,
                        "ASDF source changed while its roster prefix was "
                        "being captured");
                }
                blocks[0] = frozen_blocks[0];
            } else if (!frozen_blocks.empty()) {
                Fail(SampleLabelingAsdfErrorKind::IoFailure,
                    "ASDF source changed while its metadata prefix was "
                    "being captured");
            }

            parsed = ParsedTree{};
            parsed = ParseTree(frozen_tree, reusable_prefix.size());
            validated_metadata_bytes = frozen_tree.bytes.size();
            ValidateBlockProfile(parsed, blocks);
            if (!HasReusablePrefixProfile(
                    frozen_input, parsed, blocks, checkpoint)) {
                Fail(SampleLabelingAsdfErrorKind::IoFailure,
                    "ASDF source changed while its reusable prefix was "
                    "being captured");
            }
            ValidateProfilePreflight(BuildReaderProfilePreflight(parsed,
                frozen_tree.bytes.size(),
                reusable_prefix.size(),
                file_size));
            DecodeRoster(frozen_input, parsed, blocks, checkpoint);
        } else {
            ValidateProfilePreflight(
                BuildReaderProfilePreflight(
                    parsed, tree.bytes.size(), 0, file_size));
            DecodeRoster(input, parsed, blocks, checkpoint);
        }

        parsed.document.annotation.values =
            DecodeValues(input, parsed.values_array, blocks, checkpoint);
        ValidateDocumentSemantics(parsed.document);

        std::optional<SampleLabelingAsdfDurableBase> durable_base;
        if (initially_reusable) {
            auto state =
                std::make_shared<SampleLabelingAsdfDurableBase::State>();
            if (validated_metadata_bytes > reusable_prefix.size()) {
                Fail(SampleLabelingAsdfErrorKind::IoFailure,
                    "durable ASDF metadata bytes are inconsistent");
            }
            state->encoded_metadata.assign(
                reusable_prefix.begin(),
                reusable_prefix.begin() +
                    static_cast<std::ptrdiff_t>(validated_metadata_bytes));
            if (parsed.roster_array) {
                const BlockDescriptor& roster_block = ReferencedBlock(
                    blocks,
                    parsed.roster_array->source_index,
                    "sample roster");
                if (roster_block.raw_offset > reusable_prefix.size() ||
                    roster_block.raw_size >
                        reusable_prefix.size() - roster_block.raw_offset) {
                    Fail(SampleLabelingAsdfErrorKind::IoFailure,
                        "durable ASDF roster block bytes are inconsistent");
                }
                const std::size_t roster_begin =
                    static_cast<std::size_t>(roster_block.raw_offset);
                const std::size_t roster_end = roster_begin +
                    static_cast<std::size_t>(roster_block.raw_size);
                state->encoded_roster_block.assign(
                    reusable_prefix.begin() +
                        static_cast<std::ptrdiff_t>(roster_begin),
                    reusable_prefix.begin() +
                        static_cast<std::ptrdiff_t>(roster_end));
            }
            state->preservation_identity_digest =
                PreservationIdentityDigest(parsed.document);
            state->values_rewrite_identity_digest =
                ValuesRewriteIdentityDigest(parsed.document);
            state->created_at =
                parsed.document.labeling.canonical_metadata.created_at;
            state->modified_at =
                parsed.document.labeling.canonical_metadata.modified_at;
            state->origin =
                parsed.document.labeling.canonical_metadata.origin;
            state->sample_count = parsed.document.source.sample_count;
            state->metadata_bytes = validated_metadata_bytes;
            state->roster_block_reused = parsed.roster_array.has_value();
            if (parsed.roster_array) {
                state->roster_string_width =
                    parsed.roster_array->item_width;
            }
            state->label_codes.reserve(parsed.document.labeling.labels.size());
            for (const SampleLabelingDocumentLabel& label :
                parsed.document.labeling.labels) {
                state->label_codes.push_back(label.code);
            }
            std::ranges::sort(state->label_codes);
            durable_base = SampleLabelingAsdfDurableBase(std::move(state));
        }
        return SampleLabelingAsdfReadResult{
            .document = std::move(parsed.document),
            .durable_base = std::move(durable_base),
            .error = {}};
    } catch (const CodecFailure& failure) {
        return SampleLabelingAsdfReadResult{
            .document = std::nullopt, .error = ErrorFromFailure(failure)};
    } catch (const std::bad_alloc&) {
        return SampleLabelingAsdfReadResult{.document = std::nullopt,
            .error = {SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
                "ASDF codec allocation failed"}};
    } catch (const std::exception& error) {
        return SampleLabelingAsdfReadResult{
            .document = std::nullopt, .error = UnexpectedError(error)};
    } catch (...) {
        return SampleLabelingAsdfReadResult{.document = std::nullopt,
            .error = {SampleLabelingAsdfErrorKind::IoFailure,
                "ASDF codec operation failed"}};
    }
}

namespace sample_labeling_asdf_test_seam {

SampleLabelingAsdfReadResult ReadWithBeforePrefixCapture(
    const std::filesystem::path& path,
    BeforeReusablePrefixCapture before_prefix_capture) noexcept
{
    const BeforeReusablePrefixCapture previous =
        g_before_reusable_prefix_capture;
    g_before_reusable_prefix_capture = before_prefix_capture;
    SampleLabelingAsdfReadResult result =
        ReadSampleLabelingAsdfDocument(path);
    g_before_reusable_prefix_capture = previous;
    return result;
}

SampleLabelingAsdfReadResult ReadWithResidentBudget(
    const std::filesystem::path& path,
    std::uint64_t resident_budget_bytes) noexcept
{
    const std::uint64_t previous = g_maximum_resident_codec_bytes;
    g_maximum_resident_codec_bytes = resident_budget_bytes;
    SampleLabelingAsdfReadResult result =
        ReadSampleLabelingAsdfDocument(path);
    g_maximum_resident_codec_bytes = previous;
    return result;
}

SampleLabelingAsdfWriteResult WriteWithResidentBudget(
    std::ostream& output,
    const SampleLabelingDocument& document,
    std::uint64_t resident_budget_bytes,
    BeforeMetadataBuild before_metadata_build) noexcept
{
    const std::uint64_t previous_budget =
        g_maximum_resident_codec_bytes;
    const BeforeMetadataBuild previous_callback = g_before_metadata_build;
    g_maximum_resident_codec_bytes = resident_budget_bytes;
    g_before_metadata_build = before_metadata_build;
    SampleLabelingAsdfWriteResult result =
        WriteSampleLabelingAsdfDocument(output, document);
    g_before_metadata_build = previous_callback;
    g_maximum_resident_codec_bytes = previous_budget;
    return result;
}

SampleLabelingAsdfWriteResult
RewriteDocumentWithBeforePreservedMetadataBuild(
    const SampleLabelingAsdfDurableBase& durable_base,
    std::ostream& output,
    const SampleLabelingDocument& document,
    BeforeMetadataBuild before_metadata_build) noexcept
{
    const BeforeMetadataBuild previous_callback = g_before_metadata_build;
    g_before_metadata_build = before_metadata_build;
    SampleLabelingAsdfWriteResult result =
        RewriteSampleLabelingAsdfDocumentPreservingUnknownMetadata(
            durable_base,
            output,
            document);
    g_before_metadata_build = previous_callback;
    return result;
}

SampleLabelingAsdfError ProbeProfilePreflight(
    const ProfilePreflightProbe& probe) noexcept
{
    try {
        ProfilePreflight profile{
            .source_sample_count = probe.source_sample_count,
            .values_array = ArrayDescriptor{.source_index = 0,
                .count = probe.values_count,
                .item_width = 1,
                .little_endian = true},
            .label_count = probe.label_count,
            .metadata_bytes = probe.metadata_bytes,
            .canonical_text_bytes = probe.canonical_text_bytes,
            .label_storage_bytes = probe.label_storage_bytes,
            .reusable_prefix_bytes = probe.reusable_prefix_bytes,
            .file_bytes = probe.file_bytes,
            .codec_scratch_bytes = probe.codec_scratch_bytes};
        if (probe.explicit_roster) {
            profile.roster_array = ArrayDescriptor{.source_index = 0,
                .count = probe.roster_sample_count,
                .item_width = probe.roster_string_width,
                .little_endian = true};
        }
        ValidateProfilePreflight(profile);
        return {};
    } catch (const CodecFailure& failure) {
        return ErrorFromFailure(failure);
    } catch (const std::bad_alloc&) {
        return SampleLabelingAsdfError{
            SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            "ASDF codec allocation failed"};
    } catch (const std::exception& error) {
        return UnexpectedError(error);
    } catch (...) {
        return SampleLabelingAsdfError{
            SampleLabelingAsdfErrorKind::IoFailure,
            "ASDF codec operation failed"};
    }
}

SampleLabelingAsdfError ProbeExactRead(
    std::istream& input,
    std::size_t byte_count) noexcept
{
    try {
        std::vector<unsigned char> bytes(byte_count);
        ReadExact(input, bytes);
        return {};
    } catch (const CodecFailure& failure) {
        return ErrorFromFailure(failure);
    } catch (const std::bad_alloc&) {
        return {SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
            "ASDF codec allocation failed"};
    } catch (const std::exception& error) {
        return UnexpectedError(error);
    } catch (...) {
        return {SampleLabelingAsdfErrorKind::IoFailure,
            "ASDF codec operation failed"};
    }
}

}  // namespace sample_labeling_asdf_test_seam

SampleLabelingAsdfWriteResult WriteSampleLabelingAsdfDocument(
    std::ostream& output,
    const SampleLabelingDocument& document) noexcept
{
    try {
        ValidateDocumentText(document);

        const bool explicit_roster =
            document.source.roster.identity_kind ==
            kSampleLabelingDocumentExplicitNamesRoster;
        const std::size_t roster_width = explicit_roster
            ? MeasureRosterStringWidth(document.source.roster.sample_names)
            : 0U;
        const std::size_t metadata_size =
            MeasureMetadataSize(document, roster_width);
        ValidateProfilePreflight(BuildWriterProfilePreflight(document,
            roster_width,
            metadata_size,
            metadata_size + (explicit_roster ? 54U : 0U),
            0));
        ValidateDocumentBusinessSemantics(document);
        ValidateCurrentWriterOrigin(
            document.labeling.canonical_metadata);
        if (g_before_metadata_build != nullptr) {
            g_before_metadata_build();
        }
        const std::string metadata =
            BuildMetadata(document, roster_width, metadata_size);

        // Reject documents that the production reader cannot reopen before
        // allocating the large decoded roster block or writing any bytes. The
        // exact encoded prefix is checked again after compression below.
        std::unique_ptr<DeflateSpool> roster_spool;
        if (explicit_roster) {
            roster_spool = EncodeRosterToSpool(
                document.source.roster.sample_names, roster_width);
        }
        std::unique_ptr<DeflateSpool> values_spool =
            EncodeValuesToSpool(document.annotation.values);

        const std::uint64_t reusable_prefix_bytes = metadata.size() +
            (roster_spool ? roster_spool->block_size() : 0U);
        ValidateProfilePreflight(BuildWriterProfilePreflight(document,
            roster_width,
            metadata.size(),
            reusable_prefix_bytes,
            reusable_prefix_bytes + values_spool->block_size()));

        WriteText(output, metadata);
        if (roster_spool) {
            roster_spool->WriteBlock(output);
        }
        values_spool->WriteBlock(output);
        if (!output) {
            Fail(SampleLabelingAsdfErrorKind::IoFailure,
                "could not complete ASDF labeling document write");
        }
        return SampleLabelingAsdfWriteResult{
            .written = true, .roster_block_reused = false, .error = {}};
    } catch (const CodecFailure& failure) {
        return SampleLabelingAsdfWriteResult{
            .error = ErrorFromFailure(failure)};
    } catch (const std::bad_alloc&) {
        return SampleLabelingAsdfWriteResult{
            .error = {SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
                "ASDF codec allocation failed"}};
    } catch (const std::exception& error) {
        return SampleLabelingAsdfWriteResult{.error = UnexpectedError(error)};
    } catch (...) {
        return SampleLabelingAsdfWriteResult{
            .error = {SampleLabelingAsdfErrorKind::IoFailure,
                "ASDF codec operation failed"}};
    }
}

SampleLabelingAsdfWriteResult
RewriteSampleLabelingAsdfDocumentPreservingUnknownMetadata(
    const SampleLabelingAsdfDurableBase& durable_base,
    std::ostream& output,
    const SampleLabelingDocument& document) noexcept
{
    try {
        if (!durable_base.state_) {
            Fail(SampleLabelingAsdfErrorKind::SemanticValidationFailed,
                "metadata rewrite requires a validated durable base");
        }
        const SampleLabelingAsdfDurableBase::State& state =
            *durable_base.state_;
        ValidateDocumentText(document);
        const SampleLabelingTaskCanonicalMetadata& canonical_metadata =
            document.labeling.canonical_metadata;
        if (canonical_metadata.created_at != state.created_at) {
            Fail(SampleLabelingAsdfErrorKind::SemanticValidationFailed,
                "metadata rewrite created_at does not match the durable base");
        }
        if (canonical_metadata.origin != state.origin) {
            Fail(SampleLabelingAsdfErrorKind::SemanticValidationFailed,
                "metadata rewrite origin does not match the durable base");
        }
        if (canonical_metadata.modified_at < state.modified_at) {
            Fail(SampleLabelingAsdfErrorKind::SemanticValidationFailed,
                "metadata rewrite modified_at must be monotonic");
        }
        if (state.preservation_identity_digest !=
            PreservationIdentityDigest(document)) {
            Fail(SampleLabelingAsdfErrorKind::SemanticValidationFailed,
                "metadata rewrite document identity does not match the durable base");
        }

        const bool explicit_roster =
            document.source.roster.identity_kind ==
            kSampleLabelingDocumentExplicitNamesRoster;
        const std::size_t roster_width = explicit_roster
            ? MeasureRosterStringWidth(document.source.roster.sample_names)
            : 0U;
        ValidateProfilePreflight(BuildWriterProfilePreflight(document,
            roster_width,
            state.metadata_bytes,
            state.encoded_metadata.size() +
                state.encoded_roster_block.size(),
            0));
        ValidateDocumentBusinessSemantics(document);
        if (g_before_metadata_build != nullptr) {
            g_before_metadata_build();
        }
        const std::string metadata =
            BuildMetadataPreservingUnknownFields(
                state.encoded_metadata,
                state.metadata_bytes,
                document,
                roster_width);

        std::unique_ptr<DeflateSpool> roster_spool;
        if (explicit_roster) {
            roster_spool = EncodeRosterToSpool(
                document.source.roster.sample_names, roster_width);
        }
        std::unique_ptr<DeflateSpool> values_spool =
            EncodeValuesToSpool(document.annotation.values);

        const std::uint64_t reusable_prefix_bytes = metadata.size() +
            (roster_spool ? roster_spool->block_size() : 0U);
        ValidateProfilePreflight(BuildWriterProfilePreflight(document,
            roster_width,
            metadata.size(),
            reusable_prefix_bytes,
            reusable_prefix_bytes + values_spool->block_size()));

        WriteText(output, metadata);
        if (roster_spool) {
            roster_spool->WriteBlock(output);
        }
        values_spool->WriteBlock(output);
        if (!output) {
            Fail(SampleLabelingAsdfErrorKind::IoFailure,
                "could not complete forward-compatible ASDF metadata rewrite");
        }
        return SampleLabelingAsdfWriteResult{
            .written = true, .roster_block_reused = false, .error = {}};
    } catch (const CodecFailure& failure) {
        return SampleLabelingAsdfWriteResult{
            .error = ErrorFromFailure(failure)};
    } catch (const std::bad_alloc&) {
        return SampleLabelingAsdfWriteResult{
            .error = {SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
                "ASDF codec allocation failed"}};
    } catch (const std::exception& error) {
        return SampleLabelingAsdfWriteResult{.error = UnexpectedError(error)};
    } catch (...) {
        return SampleLabelingAsdfWriteResult{
            .error = {SampleLabelingAsdfErrorKind::IoFailure,
                "ASDF codec operation failed"}};
    }
}

SampleLabelingAsdfWriteResult
RewriteSampleLabelingAsdfValuesPreservingRosterBlock(
    const SampleLabelingAsdfDurableBase& durable_base,
    std::ostream& output,
    const SampleLabelingDocument& replacement) noexcept
{
    try {
        if (!durable_base.state_) {
            Fail(SampleLabelingAsdfErrorKind::SemanticValidationFailed,
                "values rewrite requires a validated durable base");
        }
        const SampleLabelingAsdfDurableBase::State& state =
            *durable_base.state_;
        ValidateDocumentText(replacement);
        const CanonicalTimestamp modified_at =
            replacement.labeling.canonical_metadata.modified_at;
        if (modified_at < state.modified_at) {
            Fail(SampleLabelingAsdfErrorKind::SemanticValidationFailed,
                "values rewrite modified_at must be monotonic");
        }
        if (state.values_rewrite_identity_digest !=
            ValuesRewriteIdentityDigest(replacement)) {
            Fail(SampleLabelingAsdfErrorKind::SemanticValidationFailed,
                "values rewrite may change only values and modified_at");
        }
        ValidateDocumentBusinessSemantics(replacement);

        const std::span<const std::int32_t> values =
            replacement.annotation.values;
        const std::size_t roster_width = state.roster_string_width;
        ValidateProfilePreflight(BuildWriterProfilePreflight(replacement,
            roster_width,
            state.metadata_bytes,
            state.encoded_metadata.size() +
                state.encoded_roster_block.size(),
            0));

        const std::string metadata =
            BuildMetadataPreservingUnknownFields(
                state.encoded_metadata,
                state.metadata_bytes,
                replacement,
                roster_width);
        const std::span<const unsigned char> roster_block =
            state.encoded_roster_block;

        const std::uint64_t reusable_prefix_bytes =
            metadata.size() + roster_block.size();
        ValidateProfilePreflight(BuildWriterProfilePreflight(replacement,
            roster_width,
            metadata.size(),
            reusable_prefix_bytes,
            0));

        std::unique_ptr<DeflateSpool> values_spool =
            EncodeValuesToSpool(values);
        std::array<std::uint64_t, 2> block_offsets{};
        std::size_t block_count = 0;
        if (!roster_block.empty()) {
            block_offsets[block_count++] = metadata.size();
        }
        block_offsets[block_count++] = reusable_prefix_bytes;
        const std::string block_index = BuildBlockIndex(
            std::span<const std::uint64_t>(
                block_offsets.data(), block_count));
        if (values_spool->block_size() >
                kMaximumAsdfFileBytes - reusable_prefix_bytes ||
            block_index.size() >
                kMaximumAsdfFileBytes - reusable_prefix_bytes -
                    values_spool->block_size()) {
            Fail(SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
                "ASDF block-reuse output exceeds the production file-size limit");
        }
        const std::uint64_t file_bytes = reusable_prefix_bytes +
            values_spool->block_size() + block_index.size();
        ValidateProfilePreflight(BuildWriterProfilePreflight(replacement,
            roster_width,
            metadata.size(),
            reusable_prefix_bytes,
            file_bytes));

        WriteText(output, metadata);
        WriteBytes(output, roster_block);
        values_spool->WriteBlock(output);
        WriteText(output, block_index);
        if (!output) {
            Fail(SampleLabelingAsdfErrorKind::IoFailure,
                "could not complete ASDF block-reuse write");
        }

        auto refreshed_state =
            std::make_shared<SampleLabelingAsdfDurableBase::State>();
        refreshed_state->encoded_metadata.assign(
            metadata.begin(), metadata.end());
        refreshed_state->encoded_roster_block =
            state.encoded_roster_block;
        refreshed_state->label_codes = state.label_codes;
        refreshed_state->preservation_identity_digest =
            state.preservation_identity_digest;
        refreshed_state->values_rewrite_identity_digest =
            state.values_rewrite_identity_digest;
        refreshed_state->created_at = state.created_at;
        refreshed_state->modified_at = modified_at;
        refreshed_state->origin = state.origin;
        refreshed_state->sample_count = state.sample_count;
        refreshed_state->metadata_bytes = metadata.size();
        refreshed_state->roster_string_width = state.roster_string_width;
        refreshed_state->roster_block_reused = state.roster_block_reused;
        return SampleLabelingAsdfWriteResult{.written = true,
            .roster_block_reused = state.roster_block_reused,
            .durable_base = SampleLabelingAsdfDurableBase(
                std::move(refreshed_state)),
            .error = {}};
    } catch (const CodecFailure& failure) {
        return SampleLabelingAsdfWriteResult{
            .error = ErrorFromFailure(failure)};
    } catch (const std::bad_alloc&) {
        return SampleLabelingAsdfWriteResult{
            .error = {SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
                "ASDF codec allocation failed"}};
    } catch (const std::exception& error) {
        return SampleLabelingAsdfWriteResult{.error = UnexpectedError(error)};
    } catch (...) {
        return SampleLabelingAsdfWriteResult{
            .error = {SampleLabelingAsdfErrorKind::IoFailure,
                "ASDF codec operation failed"}};
    }
}

}  // namespace specforge
