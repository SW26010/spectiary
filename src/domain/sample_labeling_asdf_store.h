#pragma once

#include "domain/sample_labeling_asdf_codec.h"
#include "domain/sample_labeling_source_compatibility.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace specforge {

enum class SampleLabelingAsdfStoreErrorKind {
    None,
    CodecFailure,
    SourceMismatch,
    PreservationIdentityMismatch,
    DurableBaseUnavailable,
    AtomicWriteFailure,
    PublishedGenerationMismatch,
};

struct SampleLabelingAsdfStoreError {
    SampleLabelingAsdfStoreErrorKind kind =
        SampleLabelingAsdfStoreErrorKind::None;
    SampleLabelingAsdfErrorKind codec_kind =
        SampleLabelingAsdfErrorKind::None;
    std::string message;
};

struct SampleLabelingAsdfStoreOpenResult;
struct SampleLabelingAsdfStoreWriteResult;

// One source-validated open generation. Each successful value-only rewrite
// re-emits modified_at, preserves forward-compatible unknown mappings and the
// encoded roster block, then refreshes the durable base for the new generation.
// A full write or any external replacement starts a new file generation and
// requires reopening the store; using this old snapshot after that point could
// restore its old metadata/roster generation.
class SampleLabelingAsdfOpenSnapshot {
public:
    SampleLabelingAsdfOpenSnapshot(
        const SampleLabelingAsdfOpenSnapshot&) = default;
    SampleLabelingAsdfOpenSnapshot& operator=(
        const SampleLabelingAsdfOpenSnapshot&) = default;
    SampleLabelingAsdfOpenSnapshot(
        SampleLabelingAsdfOpenSnapshot&&) noexcept = default;
    SampleLabelingAsdfOpenSnapshot& operator=(
        SampleLabelingAsdfOpenSnapshot&&) noexcept = default;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

    [[nodiscard]] const SampleLabelingDocument& document() const noexcept
    {
        return *document_;
    }

    [[nodiscard]] std::shared_ptr<const SampleLabelingDocument>
        document_handle() const noexcept
    {
        return document_;
    }

    [[nodiscard]] const SampleLabelingAsdfDurableBase& durable_base()
        const noexcept
    {
        return durable_base_;
    }

private:
    SampleLabelingAsdfOpenSnapshot(
        std::filesystem::path path,
        SampleLabelingDocument document,
        SampleLabelingAsdfDurableBase durable_base);

    std::filesystem::path path_;
    // The open generation owns one stable document object. Value-only
    // publications replace its values vector and modified_at while an explicit
    // roster is never copied on the autosave path. Public handles remain
    // read-only and observe the advanced generation.
    std::shared_ptr<SampleLabelingDocument> document_;
    SampleLabelingAsdfDurableBase durable_base_;

    friend struct SampleLabelingAsdfStoreOpenResult;
    friend SampleLabelingAsdfStoreOpenResult
    OpenSampleLabelingAsdfDocumentStore(
        const std::filesystem::path& path,
        const SampleLabelingSourceCompatibility& source,
        const SampleLabelingAsdfReadCheckpoint& checkpoint) noexcept;
    friend SampleLabelingAsdfStoreWriteResult
    RewriteSampleLabelingAsdfValuesAtomically(
        SampleLabelingAsdfOpenSnapshot& snapshot,
        const SampleLabelingDocument& replacement) noexcept;
};

struct SampleLabelingAsdfStoreOpenResult {
    std::optional<SampleLabelingAsdfOpenSnapshot> snapshot;
    SampleLabelingAsdfStoreError error;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return snapshot.has_value();
    }
};

struct SampleLabelingAsdfStoreWriteResult {
    bool written = false;
    bool roster_block_reused = false;
    SampleLabelingAsdfStoreError error;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return written;
    }
};

// Result of a metadata-changing publication. document_replaced distinguishes
// an atomic rewrite that reached disk from a failure before replacement. A
// replaced document is not considered published to the owner session until it
// has been reopened and returned as the new writable generation.
struct SampleLabelingAsdfStoreGenerationWriteResult {
    std::optional<SampleLabelingAsdfOpenSnapshot> snapshot;
    bool document_replaced = false;
    SampleLabelingAsdfStoreError error;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return snapshot.has_value();
    }
};

[[nodiscard]] SampleLabelingAsdfStoreOpenResult
OpenSampleLabelingAsdfDocumentStore(
    const std::filesystem::path& path,
    const SampleLabelingSourceCompatibility& source,
    const SampleLabelingAsdfReadCheckpoint& checkpoint = {}) noexcept;

// Publishes a new full-document generation. If path already contains a readable
// SpecForge sample-labeling schema 2.0.0 document, this operation first obtains
// its durable base and preserves forward-compatible unknown metadata. An
// existing document that cannot provide such a base is rejected instead of
// being silently replaced.
// Every snapshot previously opened for this path must be discarded and
// reopened before another rewrite.
[[nodiscard]] SampleLabelingAsdfStoreWriteResult
WriteSampleLabelingAsdfDocumentAtomically(
    const std::filesystem::path& path,
    const SampleLabelingDocument& document) noexcept;

// Publishes a new canonical owner and reopens the exact known generation. The
// path is not considered adopted by a controller unless this returns a
// snapshot. document_replaced still reports a durable replacement when reopen
// or verification fails so callers can retain conservative recovery state.
[[nodiscard]] SampleLabelingAsdfStoreGenerationWriteResult
WriteSampleLabelingAsdfDocumentAndOpenAtomically(
    const std::filesystem::path& path,
    const SampleLabelingDocument& document,
    const SampleLabelingSourceCompatibility& source) noexcept;

// Metadata-changing rewrite of one opened generation. This preserves unknown
// metadata from the snapshot while replacing the known canonical fields from
// document. created_at and origin remain immutable, and modified_at may only
// advance. The snapshot is stale after success and must be discarded.
[[nodiscard]] SampleLabelingAsdfStoreWriteResult
RewriteSampleLabelingAsdfDocumentAtomically(
    const SampleLabelingAsdfOpenSnapshot& snapshot,
    const SampleLabelingDocument& document) noexcept;

// Validates source against the intended document before replacement, publishes
// known metadata and values as one document generation, then opens and verifies
// that exact known generation before returning a writable snapshot. Once
// document_replaced is true, the caller must discard the old snapshot even if
// reopen or verification fails.
[[nodiscard]] SampleLabelingAsdfStoreGenerationWriteResult
RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
    const SampleLabelingAsdfOpenSnapshot& snapshot,
    const SampleLabelingDocument& document,
    const SampleLabelingSourceCompatibility& source) noexcept;

// Safe to repeat with one snapshot only while this store is the sole writer
// and every intervening publication is a timestamped values rewrite through
// it. replacement must differ only in values and monotonically advanced
// modified_at; the snapshot advances in place only after atomic replacement.
[[nodiscard]] SampleLabelingAsdfStoreWriteResult
RewriteSampleLabelingAsdfValuesAtomically(
    SampleLabelingAsdfOpenSnapshot& snapshot,
    const SampleLabelingDocument& replacement) noexcept;

namespace sample_labeling_asdf_store_test_seam {

using BeforeReplace = std::function<void(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& target_path)>;
using BeforeCodecWrite = std::function<void(std::ostream& output)>;
using AfterReplaceBeforeReopen = std::function<void(
    const std::filesystem::path& target_path)>;

[[nodiscard]] SampleLabelingAsdfStoreWriteResult WriteWithBeforeReplace(
    const std::filesystem::path& path,
    const SampleLabelingDocument& document,
    const BeforeReplace& before_replace) noexcept;

[[nodiscard]] SampleLabelingAsdfStoreWriteResult WriteWithBeforeCodecWrite(
    const std::filesystem::path& path,
    const SampleLabelingDocument& document,
    const BeforeCodecWrite& before_codec_write) noexcept;

// Runs the complete production values wrapper, including the conditional
// result-to-snapshot handoff, with a deterministic publication checkpoint.
[[nodiscard]] SampleLabelingAsdfStoreWriteResult RewriteWithBeforeReplace(
    SampleLabelingAsdfOpenSnapshot& snapshot,
    const SampleLabelingDocument& replacement,
    const BeforeReplace& before_replace) noexcept;

[[nodiscard]] SampleLabelingAsdfStoreWriteResult
RewriteDocumentWithBeforeReplace(
    const SampleLabelingAsdfOpenSnapshot& snapshot,
    const SampleLabelingDocument& document,
    const BeforeReplace& before_replace) noexcept;

[[nodiscard]] SampleLabelingAsdfStoreWriteResult
RewriteDocumentWithBeforePreservedMetadataBuild(
    const SampleLabelingAsdfOpenSnapshot& snapshot,
    const SampleLabelingDocument& document,
    sample_labeling_asdf_test_seam::BeforeMetadataBuild
        before_metadata_build) noexcept;

[[nodiscard]] SampleLabelingAsdfStoreGenerationWriteResult
WriteAndOpenWithCheckpoints(
    const std::filesystem::path& path,
    const SampleLabelingDocument& document,
    const SampleLabelingSourceCompatibility& source,
    const BeforeReplace& before_replace,
    const AfterReplaceBeforeReopen& after_replace_before_reopen) noexcept;

[[nodiscard]] SampleLabelingAsdfStoreGenerationWriteResult
RewriteDocumentAndReopenWithCheckpoints(
    const SampleLabelingAsdfOpenSnapshot& snapshot,
    const SampleLabelingDocument& document,
    const SampleLabelingSourceCompatibility& source,
    const BeforeReplace& before_replace,
    const AfterReplaceBeforeReopen& after_replace_before_reopen) noexcept;

}  // namespace sample_labeling_asdf_store_test_seam

}  // namespace specforge
