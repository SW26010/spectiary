#include "domain/sample_labeling_asdf_store.h"

#include "platform/atomic_file.h"

#include <exception>
#include <ios>
#include <new>
#include <utility>

namespace specforge {
namespace {

using BeforeReplace =
    sample_labeling_asdf_store_test_seam::BeforeReplace;

SampleLabelingAsdfStoreError CodecStoreError(
    const SampleLabelingAsdfError& error)
{
    return SampleLabelingAsdfStoreError{
        .kind = SampleLabelingAsdfStoreErrorKind::CodecFailure,
        .codec_kind = error.kind,
        .message = error.message.empty()
            ? "ASDF labeling codec operation failed"
            : error.message};
}

SampleLabelingAsdfStoreError UnexpectedStoreError(
    std::string message)
{
    return SampleLabelingAsdfStoreError{
        .kind = SampleLabelingAsdfStoreErrorKind::AtomicWriteFailure,
        .message = std::move(message)};
}

std::optional<SampleLabelingAsdfStoreError> SourceCompatibilityError(
    const SampleLabelingDocument& document,
    const SampleLabelingSourceCompatibility& source,
    const SampleLabelingAsdfReadCheckpoint& checkpoint)
{
    if (const std::optional<SampleLabelingSourceCompatibilityError> mismatch =
            CheckSampleLabelingSourceCompatibility(
                document,
                source,
                checkpoint)) {
        return SampleLabelingAsdfStoreError{
            .kind = SampleLabelingAsdfStoreErrorKind::SourceMismatch,
            .message = mismatch->message};
    }
    return std::nullopt;
}

std::optional<SampleLabelingAsdfStoreError> PreservationIdentityError(
    const SampleLabelingDocument& original,
    const SampleLabelingDocument& replacement)
{
    if (original.format_kind == replacement.format_kind &&
        original.schema_version == replacement.schema_version &&
        original.source.base_identity == replacement.source.base_identity &&
        original.source.kind == replacement.source.kind &&
        original.source.name == replacement.source.name &&
        original.source.fingerprint == replacement.source.fingerprint &&
        original.source.sample_count == replacement.source.sample_count &&
        original.source.roster.identity_kind ==
            replacement.source.roster.identity_kind &&
        original.source.roster.sample_names ==
            replacement.source.roster.sample_names &&
        original.annotation.kind == replacement.annotation.kind &&
        original.labeling.id == replacement.labeling.id) {
        return std::nullopt;
    }
    return SampleLabelingAsdfStoreError{
        .kind = SampleLabelingAsdfStoreErrorKind::
            PreservationIdentityMismatch,
        .message =
            "metadata rewrite document identity does not match the opened ASDF generation"};
}

SampleLabelingAsdfStoreWriteResult WriteAtomically(
    const std::filesystem::path& path,
    const SampleLabelingDocument& document,
    const BeforeReplace& before_replace) noexcept
{
    try {
        std::optional<SampleLabelingAsdfDurableBase> existing_base;
        std::error_code exists_error;
        const bool target_exists = std::filesystem::exists(
            path, exists_error);
        if (exists_error) {
            return SampleLabelingAsdfStoreWriteResult{
                .error = UnexpectedStoreError(
                    "could not inspect the existing ASDF labeling document: " +
                    exists_error.message())};
        }
        if (target_exists) {
            SampleLabelingAsdfReadResult existing =
                ReadSampleLabelingAsdfDocument(path);
            if (!existing.succeeded()) {
                return SampleLabelingAsdfStoreWriteResult{
                    .error = CodecStoreError(existing.error)};
            }
            if (!existing.durable_base ||
                !existing.durable_base->valid()) {
                return SampleLabelingAsdfStoreWriteResult{
                    .error = {
                        .kind = SampleLabelingAsdfStoreErrorKind::
                            DurableBaseUnavailable,
                        .message =
                            "existing ASDF labeling document cannot safely preserve forward-compatible metadata"}};
            }
            if (const std::optional<SampleLabelingAsdfStoreError> mismatch =
                    PreservationIdentityError(
                        *existing.document, document)) {
                return SampleLabelingAsdfStoreWriteResult{
                    .error = std::move(*mismatch)};
            }
            existing_base = std::move(*existing.durable_base);
        }

        std::optional<SampleLabelingAsdfError> codec_error;
        AtomicFileWriteOptions options;
        options.open_mode = std::ios::binary | std::ios::trunc;
        options.target_description = "ASDF labeling document";
        options.before_replace = before_replace;
        std::string atomic_error;
        const bool written = WriteFileAtomically(
            path,
            options,
            [&document, &existing_base, &codec_error](
                std::ostream& stream,
                std::string& error) {
                const SampleLabelingAsdfWriteResult result = existing_base
                    ? RewriteSampleLabelingAsdfDocumentPreservingUnknownMetadata(
                          *existing_base,
                          stream,
                          document)
                    : WriteSampleLabelingAsdfDocument(stream, document);
                if (!result.succeeded()) {
                    codec_error = result.error;
                    error = result.error.message;
                    return false;
                }
                return true;
            },
            &atomic_error);
        if (!written) {
            return SampleLabelingAsdfStoreWriteResult{
                .error = codec_error
                    ? CodecStoreError(*codec_error)
                    : UnexpectedStoreError(
                          atomic_error.empty()
                              ? "atomic ASDF labeling document write failed"
                              : std::move(atomic_error))};
        }
        return SampleLabelingAsdfStoreWriteResult{.written = true};
    } catch (const std::bad_alloc&) {
        return SampleLabelingAsdfStoreWriteResult{
            .error = UnexpectedStoreError(
                "atomic ASDF labeling document allocation failed")};
    } catch (const std::exception& error) {
        return SampleLabelingAsdfStoreWriteResult{
            .error = UnexpectedStoreError(
                "atomic ASDF labeling document write failed: " +
                std::string(error.what()))};
    } catch (...) {
        return SampleLabelingAsdfStoreWriteResult{
            .error = UnexpectedStoreError(
                "atomic ASDF labeling document write failed")};
    }
}

SampleLabelingAsdfStoreWriteResult RewriteDocumentAtomically(
    const SampleLabelingAsdfOpenSnapshot& snapshot,
    const SampleLabelingDocument& document,
    const BeforeReplace& before_replace) noexcept
{
    if (!snapshot.durable_base().valid()) {
        return SampleLabelingAsdfStoreWriteResult{
            .error = {
                .kind = SampleLabelingAsdfStoreErrorKind::
                    DurableBaseUnavailable,
                .message =
                    "ASDF labeling document durable base is unavailable"}};
    }
    if (const std::optional<SampleLabelingAsdfStoreError> mismatch =
            PreservationIdentityError(
                snapshot.document(), document)) {
        return SampleLabelingAsdfStoreWriteResult{
            .error = std::move(*mismatch)};
    }
    try {
        std::optional<SampleLabelingAsdfError> codec_error;
        AtomicFileWriteOptions options;
        options.open_mode = std::ios::binary | std::ios::trunc;
        options.target_description = "ASDF labeling document";
        options.before_replace = before_replace;
        std::string atomic_error;
        const bool written = WriteFileAtomically(
            snapshot.path(),
            options,
            [&snapshot, &document, &codec_error](
                std::ostream& stream,
                std::string& error) {
                const SampleLabelingAsdfWriteResult result =
                    RewriteSampleLabelingAsdfDocumentPreservingUnknownMetadata(
                        snapshot.durable_base(),
                        stream,
                        document);
                if (!result.succeeded()) {
                    codec_error = result.error;
                    error = result.error.message;
                    return false;
                }
                return true;
            },
            &atomic_error);
        if (!written) {
            return SampleLabelingAsdfStoreWriteResult{
                .error = codec_error
                    ? CodecStoreError(*codec_error)
                    : UnexpectedStoreError(
                          atomic_error.empty()
                              ? "atomic ASDF labeling metadata rewrite failed"
                              : std::move(atomic_error))};
        }
        return SampleLabelingAsdfStoreWriteResult{.written = true};
    } catch (const std::bad_alloc&) {
        return SampleLabelingAsdfStoreWriteResult{
            .error = UnexpectedStoreError(
                "atomic ASDF labeling metadata rewrite allocation failed")};
    } catch (const std::exception& error) {
        return SampleLabelingAsdfStoreWriteResult{
            .error = UnexpectedStoreError(
                "atomic ASDF labeling metadata rewrite failed: " +
                std::string(error.what()))};
    } catch (...) {
        return SampleLabelingAsdfStoreWriteResult{
            .error = UnexpectedStoreError(
                "atomic ASDF labeling metadata rewrite failed")};
    }
}

SampleLabelingAsdfStoreWriteResult RewriteAtomically(
    const SampleLabelingAsdfOpenSnapshot& snapshot,
    std::span<const std::int32_t> values,
    const BeforeReplace& before_replace) noexcept
{
    if (!snapshot.durable_base().valid()) {
        return SampleLabelingAsdfStoreWriteResult{
            .error = {
                .kind = SampleLabelingAsdfStoreErrorKind::
                    DurableBaseUnavailable,
                .message =
                    "ASDF labeling document durable base is unavailable"}};
    }
    try {
        std::optional<SampleLabelingAsdfError> codec_error;
        bool roster_block_reused = false;
        AtomicFileWriteOptions options;
        options.open_mode = std::ios::binary | std::ios::trunc;
        options.target_description = "ASDF labeling document";
        options.before_replace = before_replace;
        std::string atomic_error;
        const bool written = WriteFileAtomically(
            snapshot.path(),
            options,
            [&snapshot, values, &codec_error, &roster_block_reused](
                std::ostream& stream,
                std::string& error) {
                const SampleLabelingAsdfWriteResult result =
                    RewriteSampleLabelingAsdfValuesPreservingRosterBlock(
                        snapshot.durable_base(),
                        stream,
                        values);
                if (!result.succeeded()) {
                    codec_error = result.error;
                    error = result.error.message;
                    return false;
                }
                roster_block_reused = result.roster_block_reused;
                return true;
            },
            &atomic_error);
        if (!written) {
            return SampleLabelingAsdfStoreWriteResult{
                .error = codec_error
                    ? CodecStoreError(*codec_error)
                    : UnexpectedStoreError(
                          atomic_error.empty()
                              ? "atomic ASDF labeling document rewrite failed"
                              : std::move(atomic_error))};
        }
        return SampleLabelingAsdfStoreWriteResult{
            .written = true,
            .roster_block_reused = roster_block_reused};
    } catch (const std::bad_alloc&) {
        return SampleLabelingAsdfStoreWriteResult{
            .error = UnexpectedStoreError(
                "atomic ASDF labeling document rewrite allocation failed")};
    } catch (const std::exception& error) {
        return SampleLabelingAsdfStoreWriteResult{
            .error = UnexpectedStoreError(
                "atomic ASDF labeling document rewrite failed: " +
                std::string(error.what()))};
    } catch (...) {
        return SampleLabelingAsdfStoreWriteResult{
            .error = UnexpectedStoreError(
                "atomic ASDF labeling document rewrite failed")};
    }
}

}  // namespace

SampleLabelingAsdfOpenSnapshot::SampleLabelingAsdfOpenSnapshot(
    std::filesystem::path path,
    SampleLabelingDocument document,
    SampleLabelingAsdfDurableBase durable_base)
    : path_(std::move(path)),
      document_(std::move(document)),
      durable_base_(std::move(durable_base))
{
}

SampleLabelingAsdfStoreOpenResult
OpenSampleLabelingAsdfDocumentStore(
    const std::filesystem::path& path,
    const SampleLabelingSourceCompatibility& source,
    const SampleLabelingAsdfReadCheckpoint& checkpoint) noexcept
{
    try {
        SampleLabelingAsdfReadResult read =
            ReadSampleLabelingAsdfDocument(path, checkpoint);
        if (!read.succeeded()) {
            return SampleLabelingAsdfStoreOpenResult{
                .error = CodecStoreError(read.error)};
        }
        if (const std::optional<SampleLabelingAsdfStoreError> mismatch =
                SourceCompatibilityError(
                    *read.document,
                    source,
                    checkpoint)) {
            return SampleLabelingAsdfStoreOpenResult{
                .error = std::move(*mismatch)};
        }
        if (!read.durable_base || !read.durable_base->valid()) {
            return SampleLabelingAsdfStoreOpenResult{
                .error = {
                    .kind = SampleLabelingAsdfStoreErrorKind::
                        DurableBaseUnavailable,
                    .message =
                        "ASDF labeling document profile cannot provide a durable rewrite base"}};
        }
        return SampleLabelingAsdfStoreOpenResult{
            .snapshot = SampleLabelingAsdfOpenSnapshot(
                path,
                std::move(*read.document),
                std::move(*read.durable_base))};
    } catch (const std::bad_alloc&) {
        return SampleLabelingAsdfStoreOpenResult{
            .error = {
                .kind = SampleLabelingAsdfStoreErrorKind::CodecFailure,
                .codec_kind =
                    SampleLabelingAsdfErrorKind::ResourceLimitExceeded,
                .message = "ASDF labeling document store allocation failed"}};
    } catch (const std::exception& error) {
        return SampleLabelingAsdfStoreOpenResult{
            .error = {
                .kind = SampleLabelingAsdfStoreErrorKind::CodecFailure,
                .codec_kind = SampleLabelingAsdfErrorKind::IoFailure,
                .message =
                    "ASDF labeling document store open failed: " +
                    std::string(error.what())}};
    } catch (...) {
        return SampleLabelingAsdfStoreOpenResult{
            .error = {
                .kind = SampleLabelingAsdfStoreErrorKind::CodecFailure,
                .codec_kind = SampleLabelingAsdfErrorKind::IoFailure,
                .message = "ASDF labeling document store open failed"}};
    }
}

SampleLabelingAsdfStoreWriteResult
WriteSampleLabelingAsdfDocumentAtomically(
    const std::filesystem::path& path,
    const SampleLabelingDocument& document) noexcept
{
    return WriteAtomically(path, document, {});
}

SampleLabelingAsdfStoreWriteResult
RewriteSampleLabelingAsdfValuesAtomically(
    const SampleLabelingAsdfOpenSnapshot& snapshot,
    std::span<const std::int32_t> values) noexcept
{
    return RewriteAtomically(snapshot, values, {});
}

SampleLabelingAsdfStoreWriteResult
RewriteSampleLabelingAsdfDocumentAtomically(
    const SampleLabelingAsdfOpenSnapshot& snapshot,
    const SampleLabelingDocument& document) noexcept
{
    return RewriteDocumentAtomically(snapshot, document, {});
}

namespace sample_labeling_asdf_store_test_seam {

SampleLabelingAsdfStoreWriteResult WriteWithBeforeReplace(
    const std::filesystem::path& path,
    const SampleLabelingDocument& document,
    const BeforeReplace& before_replace) noexcept
{
    return WriteAtomically(path, document, before_replace);
}

SampleLabelingAsdfStoreWriteResult RewriteWithBeforeReplace(
    const SampleLabelingAsdfOpenSnapshot& snapshot,
    std::span<const std::int32_t> values,
    const BeforeReplace& before_replace) noexcept
{
    return RewriteAtomically(snapshot, values, before_replace);
}

SampleLabelingAsdfStoreWriteResult
RewriteDocumentWithBeforeReplace(
    const SampleLabelingAsdfOpenSnapshot& snapshot,
    const SampleLabelingDocument& document,
    const BeforeReplace& before_replace) noexcept
{
    return RewriteDocumentAtomically(
        snapshot, document, before_replace);
}

}  // namespace sample_labeling_asdf_store_test_seam

}  // namespace specforge
