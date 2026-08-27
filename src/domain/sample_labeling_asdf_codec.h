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

// Immutable snapshot of the already-validated metadata and encoded roster
// prefix belonging to one opened canonical document. Keeping this handle lets
// repeated label-only saves reuse the roster without reopening or decoding it.
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
        std::span<const std::int32_t> values) noexcept;
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
    SampleLabelingAsdfError error;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return written;
    }
};

// Reads only a bounded YAML prefix and the encoded blocks referenced by the v1
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

// Writes the fixed production v1 profile to a caller-owned stream. Documents
// outside the production reader's codec-controlled bulk-allocation contract are
// rejected before any bytes are emitted. This is not a process-RSS or
// third-party allocator bound. The caller remains responsible for atomic
// replacement and durable lifecycle policy.
[[nodiscard]] SampleLabelingAsdfWriteResult WriteSampleLabelingAsdfDocument(
    std::ostream& output,
    const SampleLabelingDocument& document) noexcept;

// Label-only persistence seam. The durable base must come from a successful
// read of the same canonical document. Its already-validated metadata prefix
// and FLEVEL=2 encoded roster block are copied verbatim without reopening or
// decoding the source; only the int32 values block is encoded again through a
// bounded file-backed spool. Encoding and the shared profile preflight complete
// before output begins.
[[nodiscard]] SampleLabelingAsdfWriteResult
RewriteSampleLabelingAsdfValuesPreservingRosterBlock(
    const SampleLabelingAsdfDurableBase& durable_base,
    std::ostream& output,
    std::span<const std::int32_t> values) noexcept;

}  // namespace specforge
