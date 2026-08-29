#pragma once

#include "domain/sample_labeling_asdf_codec.h"
#include "domain/sample_labeling_source_compatibility.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
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

// One source-validated open generation. The document advances after each
// successful value-only rewrite, while the durable base remains reusable
// because those rewrites preserve its metadata/roster prefix verbatim. A full
// write or any external replacement starts a new file generation and requires
// reopening the store; using this old snapshot after that point could restore
// its old metadata/roster prefix.
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
    // publications replace only its values vector so metadata and an explicit
    // roster are never copied on the autosave path. Public handles remain
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
        std::span<const std::int32_t> values) noexcept;
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
// production-profile v1 document, this operation first obtains its durable base
// and preserves forward-compatible unknown metadata. An existing document that
// cannot provide such a base is rejected instead of being silently replaced.
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
// document. The snapshot is stale after success and must be discarded.
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
// and every intervening publication is a value-only rewrite through it.
[[nodiscard]] SampleLabelingAsdfStoreWriteResult
RewriteSampleLabelingAsdfValuesAtomically(
    SampleLabelingAsdfOpenSnapshot& snapshot,
    std::span<const std::int32_t> values) noexcept;

namespace sample_labeling_asdf_store_test_seam {

using BeforeReplace = std::function<void(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& target_path)>;

[[nodiscard]] SampleLabelingAsdfStoreWriteResult WriteWithBeforeReplace(
    const std::filesystem::path& path,
    const SampleLabelingDocument& document,
    const BeforeReplace& before_replace) noexcept;

[[nodiscard]] SampleLabelingAsdfStoreWriteResult RewriteWithBeforeReplace(
    const SampleLabelingAsdfOpenSnapshot& snapshot,
    std::span<const std::int32_t> values,
    const BeforeReplace& before_replace) noexcept;

[[nodiscard]] SampleLabelingAsdfStoreWriteResult
RewriteDocumentWithBeforeReplace(
    const SampleLabelingAsdfOpenSnapshot& snapshot,
    const SampleLabelingDocument& document,
    const BeforeReplace& before_replace) noexcept;

}  // namespace sample_labeling_asdf_store_test_seam

}  // namespace specforge
