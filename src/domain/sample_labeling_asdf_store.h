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
};

struct SampleLabelingAsdfStoreError {
    SampleLabelingAsdfStoreErrorKind kind =
        SampleLabelingAsdfStoreErrorKind::None;
    SampleLabelingAsdfErrorKind codec_kind =
        SampleLabelingAsdfErrorKind::None;
    std::string message;
};

struct SampleLabelingAsdfStoreOpenResult;

// One immutable, source-validated open generation. The document is the
// generation read from disk; the durable base remains reusable for successive
// value-only rewrites because those rewrites preserve its metadata/roster
// prefix verbatim. A full write or any external replacement starts a new file
// generation and requires reopening the store; using this old snapshot after
// that point could restore its old metadata/roster prefix.
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
    std::shared_ptr<const SampleLabelingDocument> document_;
    SampleLabelingAsdfDurableBase durable_base_;

    friend struct SampleLabelingAsdfStoreOpenResult;
    friend SampleLabelingAsdfStoreOpenResult
    OpenSampleLabelingAsdfDocumentStore(
        const std::filesystem::path& path,
        const SampleLabelingSourceCompatibility& source,
        const SampleLabelingAsdfReadCheckpoint& checkpoint) noexcept;
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

// Metadata-changing rewrite of one opened generation. This preserves unknown
// metadata from the snapshot while replacing the known canonical fields from
// document. The snapshot is stale after success and must be discarded.
[[nodiscard]] SampleLabelingAsdfStoreWriteResult
RewriteSampleLabelingAsdfDocumentAtomically(
    const SampleLabelingAsdfOpenSnapshot& snapshot,
    const SampleLabelingDocument& document) noexcept;

// Safe to repeat with one snapshot only while this store is the sole writer
// and every intervening publication is a value-only rewrite through it.
[[nodiscard]] SampleLabelingAsdfStoreWriteResult
RewriteSampleLabelingAsdfValuesAtomically(
    const SampleLabelingAsdfOpenSnapshot& snapshot,
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
