#pragma once

#include "domain/sample_labeling_document.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace specforge {

inline constexpr std::string_view kSampleLabelingAsdfFileFormatVersion = "1.0.0";
inline constexpr std::string_view kSampleLabelingAsdfStandardVersion = "1.5.0";
inline constexpr std::string_view kSampleLabelingAsdfRootTag = "core/asdf-1.1.0";
inline constexpr std::string_view kSampleLabelingAsdfNdarrayTag = "core/ndarray-1.0.0";
inline constexpr int kSampleLabelingAsdfZlibCompressionLevel = 6;

enum class SampleLabelingAsdfErrorKind {
    None,
    OpenFailed,
    IoFailure,
    MalformedDocument,
    UnsupportedProfile,
    ResourceLimitExceeded,
    SemanticValidationFailed,
};

struct SampleLabelingAsdfError {
    SampleLabelingAsdfErrorKind kind = SampleLabelingAsdfErrorKind::None;
    std::string message;
};

struct SampleLabelingAsdfReadResult;
struct SampleLabelingAsdfWriteResult;
using SampleLabelingAsdfReadCheckpoint = std::function<void()>;

// Immutable snapshot of the already-validated metadata tree and exact encoded
// roster block belonging to one opened canonical document. Inter-block padding
// and the old values/index generation are not retained. Keeping this handle
// lets repeated label-only saves reuse the roster without reopening or decoding
// it.
class SampleLabelingAsdfDurableBase {
public:
    SampleLabelingAsdfDurableBase() noexcept = default;

    [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }

private:
    struct State;

    explicit SampleLabelingAsdfDurableBase(
        std::shared_ptr<const State> state) noexcept;

    std::shared_ptr<const State> state_;

    friend SampleLabelingAsdfReadResult ReadSampleLabelingAsdfDocument(
        const std::filesystem::path& path,
        const SampleLabelingAsdfReadCheckpoint& checkpoint) noexcept;
    friend SampleLabelingAsdfWriteResult
    RewriteSampleLabelingAsdfValuesPreservingRosterBlock(
        const SampleLabelingAsdfDurableBase& durable_base,
        std::ostream& output,
        const SampleLabelingDocument& replacement) noexcept;
    friend SampleLabelingAsdfWriteResult
    RewriteSampleLabelingAsdfDocumentPreservingUnknownMetadata(
        const SampleLabelingAsdfDurableBase& durable_base,
        std::ostream& output,
        const SampleLabelingDocument& document) noexcept;
};

struct SampleLabelingAsdfReadResult {
    std::optional<SampleLabelingDocument> document;
    std::optional<SampleLabelingAsdfDurableBase> durable_base;
    SampleLabelingAsdfError error;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return document.has_value();
    }
};

struct SampleLabelingAsdfWriteResult {
    bool written = false;
    bool roster_block_reused = false;
    std::optional<SampleLabelingAsdfDurableBase> durable_base;
    SampleLabelingAsdfError error;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return written;
    }
};

// Reads only a bounded YAML prefix and the encoded blocks referenced by the v2
// document. The complete file is never materialized as one buffer. Compatible
// zero-checksum uncompressed/big-endian inputs may be hydrated, but only the
// fixed little-endian/zlib roster profile with FLEVEL=2 receives a durable
// reuse base.
[[nodiscard]] SampleLabelingAsdfReadResult ReadSampleLabelingAsdfDocument(
    const std::filesystem::path& path,
    const SampleLabelingAsdfReadCheckpoint& checkpoint = {}) noexcept;

namespace sample_labeling_asdf_test_seam {

using BeforeReusablePrefixCapture = void (*)(
    const std::filesystem::path& path);
using BeforeMetadataBuild = void (*)();

struct ProfilePreflightProbe {
    std::size_t source_sample_count = 0;
    bool explicit_roster = false;
    std::size_t roster_sample_count = 0;
    std::size_t roster_string_width = 1;
    std::size_t values_count = 0;
    std::size_t label_count = 0;
    std::size_t metadata_bytes = 0;
    std::uint64_t canonical_text_bytes = 0;
    std::uint64_t label_storage_bytes = 0;
    std::uint64_t reusable_prefix_bytes = 0;
    std::uint64_t file_bytes = 0;
    std::uint64_t codec_scratch_bytes = 0;
};

// Deterministic regression seam for simulating a same-size source mutation
// after the initial block scan but before the reusable prefix is frozen.
[[nodiscard]] SampleLabelingAsdfReadResult ReadWithBeforePrefixCapture(
    const std::filesystem::path& path,
    BeforeReusablePrefixCapture before_prefix_capture) noexcept;

// Test-only scaled-budget seams exercise the real reader/writer ordering
// without allocating production-limit-sized fixtures.
[[nodiscard]] SampleLabelingAsdfReadResult ReadWithResidentBudget(
    const std::filesystem::path& path,
    std::uint64_t resident_budget_bytes) noexcept;
[[nodiscard]] SampleLabelingAsdfWriteResult WriteWithResidentBudget(
    std::ostream& output,
    const SampleLabelingDocument& document,
    std::uint64_t resident_budget_bytes,
    BeforeMetadataBuild before_metadata_build) noexcept;

// Injects a deterministic failure immediately before the preserved YAML tree
// is rebuilt. The production metadata-rewrite entry point leaves this empty.
[[nodiscard]] SampleLabelingAsdfWriteResult
RewriteDocumentWithBeforePreservedMetadataBuild(
    const SampleLabelingAsdfDurableBase& durable_base,
    std::ostream& output,
    const SampleLabelingDocument& document,
    BeforeMetadataBuild before_metadata_build) noexcept;

// Arithmetic-only seam for exercising the shared reader/writer profile
// contract without allocating boundary-sized ndarrays.
[[nodiscard]] SampleLabelingAsdfError ProbeProfilePreflight(
    const ProfilePreflightProbe& probe) noexcept;

// Exercises the production exact-read classification with caller-owned stream
// behavior: clean EOF is malformed input, while a stream badbit is I/O failure.
[[nodiscard]] SampleLabelingAsdfError ProbeExactRead(
    std::istream& input,
    std::size_t byte_count) noexcept;

}  // namespace sample_labeling_asdf_test_seam

// Writes the fixed production v2 profile to a caller-owned stream. Documents
// outside the production reader's codec-controlled bulk-allocation contract are
// rejected before any bytes are emitted. This is not a process-RSS or
// third-party allocator bound. The caller remains responsible for atomic
// replacement and durable lifecycle policy.
[[nodiscard]] SampleLabelingAsdfWriteResult WriteSampleLabelingAsdfDocument(
    std::ostream& output,
    const SampleLabelingDocument& document) noexcept;

// Timestamped values persistence seam. The complete replacement document must
// differ from the durable generation only in values and monotonically advanced
// modified_at. Canonical metadata is rebuilt with supported unknown mappings
// retained, while only the already-validated FLEVEL=2 roster block is copied
// without reopening or decoding it. Inter-block padding is discarded and a
// block index is emitted from the new block offsets. A refreshed durable base
// is returned for repeated saves.
[[nodiscard]] SampleLabelingAsdfWriteResult
RewriteSampleLabelingAsdfValuesPreservingRosterBlock(
    const SampleLabelingAsdfDurableBase& durable_base,
    std::ostream& output,
    const SampleLabelingDocument& replacement) noexcept;

// Metadata-changing rewrite seam for an opened v2 document. Known canonical
// fields are replaced from document while unrecognized YAML mapping entries at
// the root and inside canonical maps are carried forward. Label-entry metadata
// is matched by stable label code. Roster and values blocks are encoded again,
// so this starts a new durable generation and the old base must be discarded.
// A detached SampleLabelingDocument intentionally cannot request this rewrite:
// the validated durable base binds the source/roster, annotation kind, and task
// identity of the metadata being retained, and a mismatched replacement is
// rejected before output.
[[nodiscard]] SampleLabelingAsdfWriteResult
RewriteSampleLabelingAsdfDocumentPreservingUnknownMetadata(
    const SampleLabelingAsdfDurableBase& durable_base,
    std::ostream& output,
    const SampleLabelingDocument& document) noexcept;

}  // namespace specforge
