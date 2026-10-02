#include "domain/sample_labeling_asdf_store.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
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

std::filesystem::path FreshTestDirectory(std::string_view name)
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / name;
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);
    return directory;
}

std::filesystem::path FixturePath(std::string_view name)
{
    return std::filesystem::path(SPECTIARY_ASDF_LABELING_FIXTURE_DIR) /
        std::string(name);
}

std::vector<unsigned char> ReadAllBytes(
    const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    Require(stream.good(), "test ASDF file should open");
    return std::vector<unsigned char>(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

std::uint64_t ReadBigEndian(
    std::span<const unsigned char> bytes,
    std::size_t offset,
    std::size_t width)
{
    Require(
        offset <= bytes.size() && width <= bytes.size() - offset,
        "test ASDF block header should be complete");
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index) {
        value = (value << 8U) | bytes[offset + index];
    }
    return value;
}

std::size_t FirstBlockOffset(
    const std::vector<unsigned char>& bytes)
{
    constexpr std::array<unsigned char, 4> magic = {
        0xd3,
        'B',
        'L',
        'K',
    };
    const auto block = std::search(
        bytes.begin(),
        bytes.end(),
        magic.begin(),
        magic.end());
    Require(block != bytes.end(), "test ASDF should contain a block");
    return static_cast<std::size_t>(block - bytes.begin());
}

std::vector<unsigned char> FirstRawBlock(
    const std::vector<unsigned char>& bytes)
{
    const std::size_t offset = FirstBlockOffset(bytes);
    const std::span<const unsigned char> view(bytes);
    const std::size_t header_size =
        static_cast<std::size_t>(ReadBigEndian(view, offset + 4U, 2U));
    const std::size_t allocated_size =
        static_cast<std::size_t>(ReadBigEndian(view, offset + 14U, 8U));
    const std::size_t raw_size = 6U + header_size + allocated_size;
    Require(
        offset <= bytes.size() && raw_size <= bytes.size() - offset,
        "test ASDF raw block should be complete");
    return std::vector<unsigned char>(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        bytes.begin() +
            static_cast<std::ptrdiff_t>(offset + raw_size));
}

void WriteAllBytes(
    const std::filesystem::path& path,
    std::span<const unsigned char> bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    Require(stream.good(), "test ASDF output should open");
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    stream.close();
    Require(stream.good(), "test ASDF output should flush");
}

void ReplaceTextOnce(std::vector<unsigned char>& bytes,
    std::string_view old_text,
    std::string_view new_text)
{
    const auto found = std::search(
        bytes.begin(), bytes.end(), old_text.begin(), old_text.end());
    Require(found != bytes.end(), "ASDF store test patch target should exist");
    const std::size_t offset = static_cast<std::size_t>(found - bytes.begin());
    bytes.erase(found, found + static_cast<std::ptrdiff_t>(old_text.size()));
    bytes.insert(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        new_text.begin(),
        new_text.end());
}

bool ContainsText(
    std::span<const unsigned char> bytes,
    std::string_view text)
{
    return std::search(
               bytes.begin(), bytes.end(), text.begin(), text.end()) !=
           bytes.end();
}

bool ContainsForwardUnknownMappingsWithConcreteValues(
    std::span<const unsigned char> bytes)
{
    return ContainsText(bytes, "future_vendor") &&
        ContainsText(bytes, "new_flag: true") &&
        ContainsText(bytes, "new_text: preserve or ignore") &&
        ContainsText(bytes, "future_task") &&
        ContainsText(bytes, "token: task-survives") &&
        ContainsText(bytes, "future_origin") &&
        ContainsText(bytes, "token: origin-survives") &&
        ContainsText(bytes, "future_label") &&
        ContainsText(bytes, "token: label-survives");
}

bool HasTemporarySibling(const std::filesystem::path& target)
{
    const std::string prefix = target.filename().string() + ".tmp.";
    std::error_code error;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(
             target.parent_path(),
             error)) {
        if (error) {
            return true;
        }
        if (entry.path().filename().string().starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

spectiary::SampleLabelingDocument MakeDocument()
{
    spectiary::SampleLabelingDocument document;
    document.source.base_identity = "source-base-v1";
    document.source.kind = "npy";
    document.source.name = "spectra.npy";
    document.source.fingerprint = "source-fingerprint-v1";
    document.source.sample_count = 3;
    document.source.roster.identity_kind =
        std::string{
            spectiary::kSampleLabelingDocumentExplicitNamesRoster};
    document.source.roster.sample_names = {
        "sample-a",
        "sample-b",
        "sample-c",
    };
    document.annotation.values = {-1, 2, 7};
    document.labeling.id =
        "00000000-0000-4000-8000-000000000005";
    document.labeling.name = "Quality review";
    const auto timestamp = spectiary::ParseCanonicalTimestamp(
        "2026-08-30T08:00:00.000Z");
    Require(timestamp.has_value(), "store timestamp fixture should parse");
    document.labeling.canonical_metadata.created_at = *timestamp;
    document.labeling.canonical_metadata.modified_at = *timestamp;
    document.labeling.canonical_metadata.origin.kind = "manual";
    document.labeling.labels = {
        {2, "accepted", "a"},
        {7, "rejected", "r"},
    };
    return document;
}

spectiary::SampleLabelingSourceCompatibility CompatibleSource(
    const spectiary::SampleLabelingDocument& document)
{
    return spectiary::SampleLabelingSourceCompatibility{
        .base_identity = document.source.base_identity,
        .source_kind = document.source.kind,
        .source_name = document.source.name,
        .source_fingerprint = document.source.fingerprint,
        .sample_count = document.source.sample_count,
        .sample_names = document.source.roster.sample_names,
    };
}

void TestSourceKindMismatchIsRejected()
{
    const std::filesystem::path directory =
        FreshTestDirectory("spectiary-asdf-store-kind-mismatch");
    const std::filesystem::path path =
        directory / "labels.asdf";
    const spectiary::SampleLabelingDocument document = MakeDocument();
    Require(
        spectiary::WriteSampleLabelingAsdfDocumentAtomically(
            path,
            document)
            .succeeded(),
        "source-kind mismatch fixture should write");

    spectiary::SampleLabelingSourceCompatibility wrong_kind =
        CompatibleSource(document);
    wrong_kind.source_kind = "fits";
    const spectiary::SampleLabelingAsdfStoreOpenResult opened =
        spectiary::OpenSampleLabelingAsdfDocumentStore(
            path,
            wrong_kind);
    Require(
        !opened.succeeded() &&
            opened.error.kind ==
                spectiary::SampleLabelingAsdfStoreErrorKind::
                    SourceMismatch,
        "store open should reject a different canonical source kind");

    spectiary::SampleLabelingSourceCompatibility missing_kind =
        CompatibleSource(document);
    missing_kind.source_kind = {};
    const spectiary::SampleLabelingAsdfStoreOpenResult
        opened_without_kind =
            spectiary::OpenSampleLabelingAsdfDocumentStore(
                path,
                missing_kind);
    Require(
        !opened_without_kind.succeeded() &&
            opened_without_kind.error.kind ==
                spectiary::SampleLabelingAsdfStoreErrorKind::
                    SourceMismatch,
        "writable store open should not allow callers to bypass source-kind validation");
}

void TestAtomicFullWriteAndSourceAwareOpen()
{
    const std::filesystem::path directory =
        FreshTestDirectory("spectiary-asdf-store-open");
    const std::filesystem::path path =
        directory / "nested" / "labels.asdf";
    const spectiary::SampleLabelingDocument document = MakeDocument();

    const spectiary::SampleLabelingAsdfStoreWriteResult write =
        spectiary::WriteSampleLabelingAsdfDocumentAtomically(
            path,
            document);
    Require(
        write.succeeded(),
        write.error.message.empty()
            ? "atomic ASDF full write should succeed"
            : write.error.message);
    Require(
        std::filesystem::exists(path) && !HasTemporarySibling(path),
        "atomic ASDF full write should publish only the target");

    const spectiary::SampleLabelingAsdfStoreOpenResult opened =
        spectiary::OpenSampleLabelingAsdfDocumentStore(
            path,
            CompatibleSource(document));
    Require(
        opened.succeeded() &&
            opened.snapshot->path() == path &&
            opened.snapshot->durable_base().valid() &&
            opened.snapshot->document().annotation.values ==
                document.annotation.values &&
            opened.snapshot->document().labeling.labels.size() == 2,
        opened.error.message.empty()
            ? "source-aware ASDF store open should restore the document and durable base"
            : opened.error.message);

    spectiary::SampleLabelingSourceCompatibility wrong_source =
        CompatibleSource(document);
    wrong_source.base_identity = "another-source";
    const spectiary::SampleLabelingAsdfStoreOpenResult rejected_identity =
        spectiary::OpenSampleLabelingAsdfDocumentStore(
            path,
            wrong_source);
    Require(
        !rejected_identity.succeeded() &&
            rejected_identity.error.kind ==
                spectiary::SampleLabelingAsdfStoreErrorKind::
                    SourceMismatch,
        "store open should reject a different base source identity");

    const std::vector<std::string> reordered = {
        "sample-b",
        "sample-a",
        "sample-c",
    };
    spectiary::SampleLabelingSourceCompatibility wrong_roster =
        CompatibleSource(document);
    wrong_roster.sample_names = reordered;
    const spectiary::SampleLabelingAsdfStoreOpenResult rejected_roster =
        spectiary::OpenSampleLabelingAsdfDocumentStore(
            path,
            wrong_roster);
    Require(
        !rejected_roster.succeeded() &&
            rejected_roster.error.kind ==
                spectiary::SampleLabelingAsdfStoreErrorKind::
                    SourceMismatch,
        "store open should reject a reordered explicit roster");

    const std::filesystem::path compatibility_path =
        directory / "compatible-nondurable.asdf";
    std::vector<unsigned char> compatibility_bytes = ReadAllBytes(path);
    const std::size_t block_offset =
        FirstBlockOffset(compatibility_bytes);
    const std::size_t block_header_size = static_cast<std::size_t>(
        ReadBigEndian(compatibility_bytes, block_offset + 4U, 2U));
    const std::size_t zlib_header_offset =
        block_offset + 6U + block_header_size;
    Require(
        zlib_header_offset + 1U < compatibility_bytes.size(),
        "test ASDF roster zlib header should be complete");
    Require(
        compatibility_bytes[zlib_header_offset] == 0x78U,
        "test ASDF roster should begin with the expected zlib CMF byte");
    compatibility_bytes[zlib_header_offset + 1U] = 0x01U;
    WriteAllBytes(compatibility_path, compatibility_bytes);
    const spectiary::SampleLabelingAsdfReadResult compatible_read =
        spectiary::ReadSampleLabelingAsdfDocument(compatibility_path);
    Require(
        compatible_read.succeeded() &&
            !compatible_read.durable_base.has_value(),
        "codec compatibility read should accept a valid non-FLEVEL=2 roster without granting durable reuse");
    const spectiary::SampleLabelingAsdfStoreOpenResult nondurable =
        spectiary::OpenSampleLabelingAsdfDocumentStore(
            compatibility_path,
            CompatibleSource(document));
    Require(
        !nondurable.succeeded() &&
            nondurable.error.kind ==
                spectiary::SampleLabelingAsdfStoreErrorKind::
                    DurableBaseUnavailable,
        "writable store open should reject a compatibility profile without a durable base");
    spectiary::SampleLabelingDocument replacement = document;
    replacement.labeling.name = "unsafe replacement must fail";
    const spectiary::SampleLabelingAsdfStoreWriteResult refused_replacement =
        spectiary::WriteSampleLabelingAsdfDocumentAtomically(
            compatibility_path,
            replacement);
    Require(
        !refused_replacement.succeeded() &&
            refused_replacement.error.kind ==
                spectiary::SampleLabelingAsdfStoreErrorKind::
                    DurableBaseUnavailable &&
            ReadAllBytes(compatibility_path) == compatibility_bytes &&
            !HasTemporarySibling(compatibility_path),
        "full write should refuse an existing document whose unknown metadata cannot be preserved safely");
}

void TestInitialCanonicalPublicationReopensExactGeneration()
{
    const std::filesystem::path directory =
        FreshTestDirectory("spectiary-asdf-store-initial-publication");
    const std::filesystem::path path = directory / "labels.asdf";
    const spectiary::SampleLabelingDocument document = MakeDocument();

    const spectiary::SampleLabelingAsdfStoreGenerationWriteResult published =
        spectiary::WriteSampleLabelingAsdfDocumentAndOpenAtomically(
            path,
            document,
            CompatibleSource(document));
    Require(
        published.document_replaced &&
            published.succeeded() &&
            published.snapshot->path() == path &&
            published.snapshot->durable_base().valid() &&
            published.snapshot->document().annotation.values ==
                document.annotation.values &&
            published.snapshot->document().labeling.id ==
                document.labeling.id &&
            !HasTemporarySibling(path),
        published.error.message.empty()
            ? "initial canonical publication should reopen the exact durable generation"
            : published.error.message);

    const std::filesystem::path rejected_path =
        directory / "source-mismatch.asdf";
    spectiary::SampleLabelingSourceCompatibility wrong_source =
        CompatibleSource(document);
    wrong_source.source_kind = "fits";
    const spectiary::SampleLabelingAsdfStoreGenerationWriteResult rejected =
        spectiary::WriteSampleLabelingAsdfDocumentAndOpenAtomically(
            rejected_path,
            document,
            wrong_source);
    Require(
        !rejected.document_replaced &&
            !rejected.succeeded() &&
            rejected.error.kind ==
                spectiary::SampleLabelingAsdfStoreErrorKind::
                    SourceMismatch &&
            !std::filesystem::exists(rejected_path),
        "initial canonical publication should reject source mismatch before replacement");
}

void TestValueOnlyRewriteReusesRosterBlockWithoutReopen()
{
    const std::filesystem::path directory =
        FreshTestDirectory("spectiary-asdf-store-rewrite");
    const std::filesystem::path path = directory / "labels.asdf";
    const spectiary::SampleLabelingDocument document = MakeDocument();
    Require(
        spectiary::WriteSampleLabelingAsdfDocumentAtomically(
            path,
            document)
            .succeeded(),
        "rewrite fixture should write");
    spectiary::SampleLabelingAsdfStoreOpenResult opened =
        spectiary::OpenSampleLabelingAsdfDocumentStore(
            path,
            CompatibleSource(document));
    Require(opened.succeeded(), "rewrite fixture should open");
    const std::shared_ptr<const spectiary::SampleLabelingDocument>
        generation_handle =
            opened.snapshot->document_handle();
    const std::string* const roster_storage =
        generation_handle->source.roster.sample_names.data();
    const std::vector<unsigned char> roster_before =
        FirstRawBlock(ReadAllBytes(path));

    const std::vector<unsigned char> before_identity_mismatch =
        ReadAllBytes(path);
    spectiary::SampleLabelingDocument identity_mismatch =
        opened.snapshot->document();
    identity_mismatch.labeling.name = "metadata change is not values-only";
    const spectiary::SampleLabelingAsdfStoreWriteResult rejected_identity =
        spectiary::RewriteSampleLabelingAsdfValuesAtomically(
            *opened.snapshot,
            identity_mismatch);
    Require(
        !rejected_identity.succeeded() &&
            rejected_identity.error.kind ==
                spectiary::SampleLabelingAsdfStoreErrorKind::
                    PreservationIdentityMismatch &&
            ReadAllBytes(path) == before_identity_mismatch &&
            opened.snapshot->document_handle() == generation_handle &&
            !HasTemporarySibling(path),
        "values fast path must reject metadata changes before creating a replacement generation");

    const std::array<std::int32_t, 3> first_values = {7, 2, -1};
    const auto first_modified_at = spectiary::ParseCanonicalTimestamp(
        "2026-08-30T08:00:01.000Z");
    spectiary::SampleLabelingDocument first_replacement =
        opened.snapshot->document();
    first_replacement.annotation.values.assign(
        first_values.begin(), first_values.end());
    first_replacement.labeling.canonical_metadata.modified_at =
        *first_modified_at;
    const spectiary::SampleLabelingAsdfStoreWriteResult first_rewrite =
        spectiary::RewriteSampleLabelingAsdfValuesAtomically(
            *opened.snapshot,
            first_replacement);
    Require(
        first_rewrite.succeeded() &&
            first_rewrite.roster_block_reused &&
            opened.snapshot->document_handle().get() ==
                generation_handle.get() &&
            opened.snapshot->document().source.roster
                    .sample_names.data() ==
                roster_storage &&
            opened.snapshot->document().annotation.values ==
                std::vector<std::int32_t>(
                    first_values.begin(),
                    first_values.end()),
        first_rewrite.error.message.empty()
            ? "value-only rewrite should reuse the roster block and advance the open snapshot"
            : first_rewrite.error.message);
    Require(
        FirstRawBlock(ReadAllBytes(path)) == roster_before,
        "value-only rewrite should preserve the raw roster block verbatim");

    const std::array<std::int32_t, 3> second_values = {2, 2, 7};
    const auto second_modified_at = spectiary::ParseCanonicalTimestamp(
        "2026-08-30T08:00:02.000Z");
    spectiary::SampleLabelingDocument second_replacement =
        opened.snapshot->document();
    second_replacement.annotation.values.assign(
        second_values.begin(), second_values.end());
    second_replacement.labeling.canonical_metadata.modified_at =
        *second_modified_at;
    const spectiary::SampleLabelingAsdfStoreWriteResult second_rewrite =
        spectiary::RewriteSampleLabelingAsdfValuesAtomically(
            *opened.snapshot,
            second_replacement);
    Require(
            second_rewrite.succeeded() &&
            second_rewrite.roster_block_reused &&
            opened.snapshot->document_handle().get() ==
                generation_handle.get() &&
            opened.snapshot->document().source.roster
                    .sample_names.data() ==
                roster_storage &&
            opened.snapshot->document().annotation.values ==
                std::vector<std::int32_t>(
                    second_values.begin(),
                    second_values.end()),
        "one opened durable base should support repeated rewrites and track the current values generation");
    Require(
        FirstRawBlock(ReadAllBytes(path)) == roster_before,
        "repeated rewrites should keep the same encoded roster bytes");

    const spectiary::SampleLabelingAsdfStoreOpenResult reopened =
        spectiary::OpenSampleLabelingAsdfDocumentStore(
            path,
            CompatibleSource(document));
    Require(
        reopened.succeeded() &&
            reopened.snapshot->document().annotation.values ==
                std::vector<std::int32_t>(
                    second_values.begin(),
                    second_values.end()),
        "rewritten ASDF values should be visible after reopen");
}

void TestMetadataChangingRewritesPreserveForwardUnknownFields()
{
    const std::filesystem::path directory =
        FreshTestDirectory("spectiary-asdf-store-forward-metadata");
    const std::filesystem::path path = directory / "labels.asdf";
    const spectiary::SampleLabelingDocument original = MakeDocument();
    Require(
        spectiary::WriteSampleLabelingAsdfDocumentAtomically(path, original)
            .succeeded(),
        "forward-metadata store fixture should write");

    std::vector<unsigned char> bytes = ReadAllBytes(path);
    ReplaceTextOnce(bytes,
        "\nschema_version: ",
        "\nfuture_root: \"store-root-survives\"\nschema_version: ");
    ReplaceTextOnce(bytes,
        "    shortcut: \"a\"\n",
        "    shortcut: \"a\"\n    future_label: \"store-label-survives\"\n");
    WriteAllBytes(path, bytes);

    spectiary::SampleLabelingAsdfStoreOpenResult opened =
        spectiary::OpenSampleLabelingAsdfDocumentStore(
            path,
            CompatibleSource(original));
    Require(opened.succeeded(), "forward-metadata store fixture should open");
    spectiary::SampleLabelingDocument edited = opened.snapshot->document();
    edited.labeling.name = "Edited through snapshot";
    edited.labeling.labels[0].name = "snapshot edit";
    const std::shared_ptr<const spectiary::SampleLabelingDocument>
        old_generation = opened.snapshot->document_handle();
    spectiary::SampleLabelingSourceCompatibility
        incompatible_source = CompatibleSource(original);
    incompatible_source.source_kind = "fits";
    const std::vector<unsigned char> before_incompatible_source =
        ReadAllBytes(path);
    const spectiary::SampleLabelingAsdfStoreGenerationWriteResult
        incompatible_rewrite =
            spectiary::
                RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
                    *opened.snapshot,
                    edited,
                    incompatible_source);
    Require(
        !incompatible_rewrite.succeeded() &&
            !incompatible_rewrite.document_replaced &&
            incompatible_rewrite.error.kind ==
                spectiary::SampleLabelingAsdfStoreErrorKind::
                    SourceMismatch &&
            ReadAllBytes(path) == before_incompatible_source &&
            !HasTemporarySibling(path),
        "metadata publication should reject an incompatible reopen descriptor before replacing the durable document");

    spectiary::SampleLabelingAsdfStoreGenerationWriteResult
        snapshot_rewrite =
        spectiary::RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
            *opened.snapshot,
            edited,
            CompatibleSource(original));
    Require(
        snapshot_rewrite.succeeded() &&
            snapshot_rewrite.document_replaced &&
            snapshot_rewrite.snapshot->document_handle() !=
                old_generation &&
            snapshot_rewrite.snapshot->document().labeling.name ==
                edited.labeling.name,
        snapshot_rewrite.error.message.empty()
            ? "snapshot metadata rewrite should reopen a new generation"
            : snapshot_rewrite.error.message);
    Require(
        ContainsText(ReadAllBytes(path), "store-root-survives") &&
            ContainsText(ReadAllBytes(path), "store-label-survives"),
        "snapshot metadata rewrite should preserve unknown metadata");

    const std::vector<unsigned char> before_identity_mismatch =
        ReadAllBytes(path);
    spectiary::SampleLabelingDocument unrelated =
        snapshot_rewrite.snapshot->document();
    unrelated.labeling.id = "another-task";
    const spectiary::SampleLabelingAsdfStoreWriteResult identity_mismatch =
        spectiary::RewriteSampleLabelingAsdfDocumentAtomically(
            *snapshot_rewrite.snapshot,
            unrelated);
    Require(
        !identity_mismatch.succeeded() &&
            identity_mismatch.error.kind ==
                spectiary::SampleLabelingAsdfStoreErrorKind::
                    PreservationIdentityMismatch &&
            ReadAllBytes(path) == before_identity_mismatch &&
            !HasTemporarySibling(path),
        "metadata preservation must not carry unknown fields into another document identity");
    spectiary::SampleLabelingDocument edited_again =
        snapshot_rewrite.snapshot->document();
    edited_again.labeling.name = "Edited through path write";
    edited_again.labeling.canonical_metadata.modified_at =
        *spectiary::ParseCanonicalTimestamp(
            "2026-08-30T08:00:03.000Z");
    const spectiary::SampleLabelingAsdfStoreWriteResult path_rewrite =
        spectiary::WriteSampleLabelingAsdfDocumentAtomically(
            path,
            edited_again);
    Require(
        path_rewrite.succeeded() &&
            ContainsText(ReadAllBytes(path), "store-root-survives") &&
            ContainsText(ReadAllBytes(path), "store-label-survives"),
        path_rewrite.error.message.empty()
            ? "path-based full rewrite should not bypass unknown metadata preservation"
            : path_rewrite.error.message);
}

void TestFullDocumentRewritePreservesCanonicalLifecycleIdentity()
{
    const std::filesystem::path directory =
        FreshTestDirectory("spectiary-asdf-store-canonical-identity");
    const std::filesystem::path path = directory / "labels.asdf";
    spectiary::SampleLabelingDocument original = MakeDocument();
    original.labeling.canonical_metadata.modified_at =
        *spectiary::ParseCanonicalTimestamp(
            "2026-08-30T08:00:05.000Z");
    Require(
        spectiary::WriteSampleLabelingAsdfDocumentAtomically(path, original)
            .succeeded(),
        "canonical-identity store fixture should write");

    spectiary::SampleLabelingAsdfStoreOpenResult opened =
        spectiary::OpenSampleLabelingAsdfDocumentStore(
            path,
            CompatibleSource(original));
    Require(
        opened.succeeded(),
        "canonical-identity store fixture should open");
    const std::vector<unsigned char> durable_bytes = ReadAllBytes(path);

    const auto require_rejected_without_replacement =
        [&](const spectiary::SampleLabelingDocument& replacement,
            std::string_view message) {
            const spectiary::SampleLabelingAsdfStoreWriteResult result =
                spectiary::RewriteSampleLabelingAsdfDocumentAtomically(
                    *opened.snapshot,
                    replacement);
            Require(
                !result.succeeded() &&
                    result.error.kind ==
                        spectiary::SampleLabelingAsdfStoreErrorKind::
                            PreservationIdentityMismatch &&
                    ReadAllBytes(path) == durable_bytes &&
                    !HasTemporarySibling(path),
                message);
        };

    spectiary::SampleLabelingDocument changed_created_at = original;
    changed_created_at.labeling.canonical_metadata.created_at =
        *spectiary::ParseCanonicalTimestamp(
            "2026-08-30T07:59:59.000Z");
    require_rejected_without_replacement(
        changed_created_at,
        "full-document rewrite must preserve the durable created_at");

    spectiary::SampleLabelingDocument changed_origin = original;
    changed_origin.labeling.canonical_metadata.origin = {
        .kind = "annotation_promotion",
        .annotation = spectiary::SampleLabelingAnnotationOrigin{
            .name = "labels.npy",
            .format = "npy",
            .fingerprint = std::string(64U, 'a'),
        },
    };
    require_rejected_without_replacement(
        changed_origin,
        "full-document rewrite must preserve the durable origin");

    spectiary::SampleLabelingDocument regressed_modified_at = original;
    regressed_modified_at.labeling.canonical_metadata.modified_at =
        *spectiary::ParseCanonicalTimestamp(
            "2026-08-30T08:00:03.000Z");
    require_rejected_without_replacement(
        regressed_modified_at,
        "full-document rewrite must not regress the durable modified_at");

    const spectiary::SampleLabelingAsdfStoreWriteResult path_rewrite =
        spectiary::WriteSampleLabelingAsdfDocumentAtomically(
            path,
            changed_created_at);
    Require(
        !path_rewrite.succeeded() &&
            path_rewrite.error.kind ==
                spectiary::SampleLabelingAsdfStoreErrorKind::
                    PreservationIdentityMismatch &&
            ReadAllBytes(path) == durable_bytes &&
            !HasTemporarySibling(path),
        "path-based full rewrite must enforce canonical lifecycle identity before replacement");
}

void TestWriteFailuresPreserveThePreviousDocument()
{
    const std::filesystem::path directory =
        FreshTestDirectory("spectiary-asdf-store-failure");
    const std::filesystem::path path = directory / "labels.asdf";
    const spectiary::SampleLabelingDocument document = MakeDocument();
    Require(
        spectiary::WriteSampleLabelingAsdfDocumentAtomically(
            path,
            document)
            .succeeded(),
        "failure fixture should write");
    const std::vector<unsigned char> original = ReadAllBytes(path);

    spectiary::SampleLabelingDocument invalid = document;
    invalid.annotation.values[0] = 99;
    const spectiary::SampleLabelingAsdfStoreWriteResult codec_failure =
        spectiary::WriteSampleLabelingAsdfDocumentAtomically(
            path,
            invalid);
    Require(
        !codec_failure.succeeded() &&
            codec_failure.error.kind ==
                spectiary::SampleLabelingAsdfStoreErrorKind::CodecFailure &&
            ReadAllBytes(path) == original &&
            !HasTemporarySibling(path),
        "a full-write codec failure should preserve the previous document and clean its temporary file");

    // This store seam aborts publication immediately before replacement. The
    // shared atomic-file suite separately injects failures in the platform
    // replacement operation itself.
    bool full_publish_hook_reached = false;
    const spectiary::SampleLabelingAsdfStoreWriteResult full_publish_abort =
        spectiary::sample_labeling_asdf_store_test_seam::
            WriteWithBeforeReplace(
                path,
                document,
                [&full_publish_hook_reached](const auto&, const auto&) {
                    full_publish_hook_reached = true;
                    throw std::runtime_error(
                        "injected pre-replacement publication abort");
                });
    Require(
        full_publish_hook_reached && !full_publish_abort.succeeded() &&
            full_publish_abort.error.kind ==
                spectiary::SampleLabelingAsdfStoreErrorKind::
                    AtomicWriteFailure &&
            ReadAllBytes(path) == original &&
            !HasTemporarySibling(path),
        "a full-write pre-replacement abort should preserve the previous document");

    spectiary::SampleLabelingAsdfStoreOpenResult opened =
        spectiary::OpenSampleLabelingAsdfDocumentStore(
            path,
            CompatibleSource(document));
    Require(opened.succeeded(), "failure fixture should open");
    spectiary::SampleLabelingDocument wrong_count =
        opened.snapshot->document();
    wrong_count.annotation.values = {2, 7};
    const spectiary::SampleLabelingAsdfStoreWriteResult rewrite_codec_failure =
        spectiary::RewriteSampleLabelingAsdfValuesAtomically(
            *opened.snapshot,
            wrong_count);
    Require(
        !rewrite_codec_failure.succeeded() &&
            rewrite_codec_failure.error.kind ==
                spectiary::SampleLabelingAsdfStoreErrorKind::CodecFailure &&
            opened.snapshot->document().annotation.values ==
                document.annotation.values &&
            ReadAllBytes(path) == original &&
            !HasTemporarySibling(path),
        "a rewrite codec failure should preserve the previous document and open snapshot");

    bool metadata_publish_hook_reached = false;
    spectiary::SampleLabelingDocument metadata_edit =
        opened.snapshot->document();
    metadata_edit.labeling.name = "metadata publication must abort";
    const spectiary::SampleLabelingAsdfStoreWriteResult
        metadata_publish_abort =
            spectiary::sample_labeling_asdf_store_test_seam::
                RewriteDocumentWithBeforeReplace(
                    *opened.snapshot,
                    metadata_edit,
                    [&metadata_publish_hook_reached](
                        const auto&, const auto&) {
                        metadata_publish_hook_reached = true;
                        throw std::runtime_error(
                            "injected metadata pre-replacement publication abort");
                    });
    Require(
        metadata_publish_hook_reached &&
            !metadata_publish_abort.succeeded() &&
            metadata_publish_abort.error.kind ==
                spectiary::SampleLabelingAsdfStoreErrorKind::
                    AtomicWriteFailure &&
            ReadAllBytes(path) == original &&
            !HasTemporarySibling(path),
        "a metadata rewrite pre-replacement abort should preserve the previous document and clean its temporary file");

    bool rewrite_publish_hook_reached = false;
    spectiary::SampleLabelingDocument replacement =
        opened.snapshot->document();
    replacement.annotation.values = {7, 7, 2};
    const spectiary::SampleLabelingAsdfStoreWriteResult
        rewrite_publish_abort =
            spectiary::sample_labeling_asdf_store_test_seam::
                RewriteWithBeforeReplace(
                    *opened.snapshot,
                    replacement,
                    [&rewrite_publish_hook_reached](const auto&, const auto&) {
                        rewrite_publish_hook_reached = true;
                        throw std::runtime_error(
                            "injected rewrite pre-replacement publication abort");
                    });
    Require(
        rewrite_publish_hook_reached &&
            !rewrite_publish_abort.succeeded() &&
            rewrite_publish_abort.error.kind ==
                spectiary::SampleLabelingAsdfStoreErrorKind::
                    AtomicWriteFailure &&
            ReadAllBytes(path) == original &&
            !HasTemporarySibling(path),
        "a rewrite pre-replacement abort should preserve the previous document");
}

void TestValueRewriteReplacementFailurePreservesWholeGeneration()
{
    const std::filesystem::path directory =
        FreshTestDirectory("spectiary-asdf-store-value-replace-failure");
    const std::filesystem::path path = directory / "labels.asdf";
    const std::vector<unsigned char> fixture_bytes =
        ReadAllBytes(FixturePath("forward_unknown.asdf"));
    Require(
        ContainsForwardUnknownMappingsWithConcreteValues(fixture_bytes),
        "forward-unknown fixture should contain concrete root, task, origin, and label mappings");
    WriteAllBytes(path, fixture_bytes);

    const spectiary::SampleLabelingAsdfReadResult fixture =
        spectiary::ReadSampleLabelingAsdfDocument(path);
    Require(
        fixture.succeeded(),
        "forward-unknown atomic rewrite fixture should be a valid canonical document");

    spectiary::SampleLabelingAsdfStoreOpenResult opened =
        spectiary::OpenSampleLabelingAsdfDocumentStore(
            path,
            CompatibleSource(*fixture.document));
    Require(
        opened.succeeded(),
        "value replacement failure fixture should open");
    const std::vector<unsigned char> durable_bytes = ReadAllBytes(path);
    const std::vector<unsigned char> durable_roster_block =
        FirstRawBlock(durable_bytes);
    const std::vector<std::int32_t> durable_values =
        opened.snapshot->document().annotation.values;
    const spectiary::CanonicalTimestamp durable_modified_at =
        opened.snapshot->document()
            .labeling.canonical_metadata.modified_at;
    const std::shared_ptr<const spectiary::SampleLabelingDocument>
        durable_generation = opened.snapshot->document_handle();
    const auto replacement_modified_at =
        spectiary::ParseCanonicalTimestamp(
            "2026-08-30T08:00:04.000Z");
    Require(
        replacement_modified_at.has_value(),
        "replacement timestamp fixture should parse");

    bool replacement_hook_reached = false;
    bool temporary_generation_matches = false;
    spectiary::SampleLabelingDocument replacement =
        opened.snapshot->document();
    replacement.annotation.values = {1, 0};
    replacement.labeling.canonical_metadata.modified_at =
        *replacement_modified_at;
    const spectiary::SampleLabelingAsdfStoreWriteResult aborted =
        spectiary::sample_labeling_asdf_store_test_seam::
            RewriteWithBeforeReplace(
                *opened.snapshot,
                replacement,
                [&replacement_hook_reached,
                    &temporary_generation_matches,
                    &replacement,
                    &durable_roster_block](
                    const std::filesystem::path& temporary_path,
                    const auto&) {
                    replacement_hook_reached = true;
                    const std::vector<unsigned char> temporary_bytes =
                        ReadAllBytes(temporary_path);
                    const spectiary::SampleLabelingAsdfReadResult
                        temporary_document =
                            spectiary::ReadSampleLabelingAsdfDocument(
                                temporary_path);
                    temporary_generation_matches =
                        temporary_document.succeeded() &&
                        temporary_document.document->annotation.values ==
                            replacement.annotation.values &&
                        temporary_document.document->labeling
                                .canonical_metadata.modified_at ==
                            replacement.labeling.canonical_metadata
                                .modified_at &&
                        ContainsForwardUnknownMappingsWithConcreteValues(
                            temporary_bytes) &&
                        FirstRawBlock(temporary_bytes) ==
                            durable_roster_block;
                    throw std::runtime_error(
                        "injected value replacement abort");
                });

    const std::vector<unsigned char> after_abort = ReadAllBytes(path);
    const spectiary::SampleLabelingAsdfStoreOpenResult reopened =
        spectiary::OpenSampleLabelingAsdfDocumentStore(
            path,
            CompatibleSource(*fixture.document));
    Require(
        replacement_hook_reached &&
            temporary_generation_matches &&
            !aborted.succeeded() &&
            aborted.error.kind ==
                spectiary::SampleLabelingAsdfStoreErrorKind::
                    AtomicWriteFailure &&
            after_abort == durable_bytes &&
            ContainsForwardUnknownMappingsWithConcreteValues(after_abort) &&
            FirstRawBlock(after_abort) == durable_roster_block &&
            opened.snapshot->document().annotation.values ==
                durable_values &&
            opened.snapshot->document()
                    .labeling.canonical_metadata.modified_at ==
                durable_modified_at &&
            opened.snapshot->document_handle() == durable_generation &&
            reopened.succeeded() &&
            reopened.snapshot->document().annotation.values ==
                durable_values &&
            reopened.snapshot->document()
                    .labeling.canonical_metadata.modified_at ==
                durable_modified_at &&
            !HasTemporarySibling(path),
        "an aborted value replacement must preserve old values, timestamp, unknown mappings, roster bytes, and snapshot generation");

    const spectiary::SampleLabelingAsdfStoreWriteResult retry =
        spectiary::RewriteSampleLabelingAsdfValuesAtomically(
            *opened.snapshot,
            replacement);
    const spectiary::SampleLabelingAsdfStoreOpenResult after_retry =
        spectiary::OpenSampleLabelingAsdfDocumentStore(
            path,
            CompatibleSource(*fixture.document));
    const std::vector<unsigned char> retry_bytes = ReadAllBytes(path);
    Require(
        retry.succeeded() && retry.roster_block_reused &&
            opened.snapshot->document_handle() == durable_generation &&
            opened.snapshot->document().annotation.values ==
                replacement.annotation.values &&
            opened.snapshot->document()
                    .labeling.canonical_metadata.modified_at ==
                replacement.labeling.canonical_metadata.modified_at &&
            after_retry.succeeded() &&
            after_retry.snapshot->document().annotation.values ==
                replacement.annotation.values &&
            after_retry.snapshot->document()
                    .labeling.canonical_metadata.modified_at ==
                replacement.labeling.canonical_metadata.modified_at &&
            ContainsForwardUnknownMappingsWithConcreteValues(
                retry_bytes) &&
            FirstRawBlock(retry_bytes) == durable_roster_block,
        retry.error.message.empty()
            ? "retry should publish the original replacement document and timestamp"
            : retry.error.message);
}

}  // namespace

void TestExternalGenerationConflicts()
{
    using namespace spectiary;
    const auto directory = FreshTestDirectory("spectiary-asdf-external-change");
    const auto path = directory / "labels.asdf";
    const auto original = MakeDocument();
    for (int mode = 0; mode < 4; ++mode) {
        std::filesystem::remove(path);
        Require(WriteSampleLabelingAsdfDocumentAtomically(path, original).succeeded(),
            "conflict fixture should publish");
        auto opened = OpenSampleLabelingAsdfDocumentStore(path, CompatibleSource(original));
        Require(opened.succeeded(), "conflict fixture should open");
        auto metadata_snapshot = *opened.snapshot;
        const auto original_bytes = ReadAllBytes(path);
        const auto timestamp = std::filesystem::last_write_time(path);
        auto external_bytes = original_bytes;
        ReplaceTextOnce(external_bytes, "Quality review", "Outside review");
        if (mode == 0) {
            WriteAllBytes(path, external_bytes);
            std::filesystem::last_write_time(path, timestamp);
        } else if (mode == 1) {
            std::filesystem::rename(path, directory / "moved.asdf");
            WriteAllBytes(path, external_bytes);
        } else if (mode == 2) {
            std::filesystem::remove(path);
        } else {
            std::filesystem::rename(path, directory / "missing.asdf");
        }
        auto replacement = original;
        replacement.annotation.values[0] = 2;
        const auto values = RewriteSampleLabelingAsdfValuesAtomically(*opened.snapshot, replacement);
        Require(!values.succeeded() && values.error.kind ==
                SampleLabelingAsdfStoreErrorKind::ExternalChangeConflict,
            "external modification/replacement/removal must reject values save");
        replacement.labeling.name = "Local edit";
        const auto metadata = RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
            metadata_snapshot, replacement, CompatibleSource(original));
        Require(!metadata.succeeded() && !metadata.document_replaced && metadata.error.kind ==
                SampleLabelingAsdfStoreErrorKind::ExternalChangeConflict,
            "metadata save must share the sticky conflict");
        Require(mode < 2 ? ReadAllBytes(path) == external_bytes : !std::filesystem::exists(path),
            "rejected save must preserve external bytes or absence");
        Require(!HasTemporarySibling(path), "conflict must clean staged sibling");
        WriteAllBytes(path, original_bytes);
        Require(!RewriteSampleLabelingAsdfDocumentAtomically(*opened.snapshot, replacement).succeeded(),
            "restoring original bytes must not clear the conflict on ordinary retry");
    }

    auto bytes = ReadAllBytes(path);
    bool changed = false;
    const auto raced_open = OpenSampleLabelingAsdfDocumentStore(
        path, CompatibleSource(original), [&] {
            if (!changed) {
                changed = true;
                ReplaceTextOnce(bytes, "Quality review", "Outside review");
                WriteAllBytes(path, bytes);
            }
        });
    Require(changed && !raced_open.succeeded(),
        "open must not bind an independently changed generation to its snapshot");

    WriteAllBytes(path, bytes);
    auto opened = OpenSampleLabelingAsdfDocumentStore(path, CompatibleSource(original));
    Require(opened.succeeded(), "external generation can be explicitly reopened");
    auto replacement = opened.snapshot->document();
    replacement.annotation.values[0] = 2;
    const auto late = sample_labeling_asdf_store_test_seam::RewriteWithBeforeReplace(
        *opened.snapshot, replacement, [&](const auto&, const auto& target) {
            std::filesystem::remove(target);
        });
    Require(!late.succeeded() && late.error.kind == SampleLabelingAsdfStoreErrorKind::ExternalChangeConflict &&
            !std::filesystem::exists(path),
        "last pre-publication check must detect changes during staging");
}

int main()
{
    try {
        TestExternalGenerationConflicts();
        TestSourceKindMismatchIsRejected();
        TestAtomicFullWriteAndSourceAwareOpen();
        TestInitialCanonicalPublicationReopensExactGeneration();
        TestValueOnlyRewriteReusesRosterBlockWithoutReopen();
        TestMetadataChangingRewritesPreserveForwardUnknownFields();
        TestFullDocumentRewritePreservesCanonicalLifecycleIdentity();
        TestWriteFailuresPreserveThePreviousDocument();
        TestValueRewriteReplacementFailurePreservesWholeGeneration();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
