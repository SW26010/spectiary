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

specforge::SampleLabelingDocument MakeDocument()
{
    specforge::SampleLabelingDocument document;
    document.source.base_identity = "source-base-v1";
    document.source.kind = "npy";
    document.source.name = "spectra.npy";
    document.source.fingerprint = "source-fingerprint-v1";
    document.source.sample_count = 3;
    document.source.roster.identity_kind =
        std::string{
            specforge::kSampleLabelingDocumentExplicitNamesRoster};
    document.source.roster.sample_names = {
        "sample-a",
        "sample-b",
        "sample-c",
    };
    document.annotation.name = "quality-code";
    document.annotation.values = {-1, 2, 7};
    document.labeling.id = "quality-task";
    document.labeling.name = "Quality review";
    document.labeling.labels = {
        {2, "accepted", "a"},
        {7, "rejected", "r"},
    };
    return document;
}

specforge::SampleLabelingSourceCompatibility CompatibleSource(
    const specforge::SampleLabelingDocument& document)
{
    return specforge::SampleLabelingSourceCompatibility{
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
        FreshTestDirectory("specforge-asdf-store-kind-mismatch");
    const std::filesystem::path path =
        directory / "labels.asdf";
    const specforge::SampleLabelingDocument document = MakeDocument();
    Require(
        specforge::WriteSampleLabelingAsdfDocumentAtomically(
            path,
            document)
            .succeeded(),
        "source-kind mismatch fixture should write");

    specforge::SampleLabelingSourceCompatibility wrong_kind =
        CompatibleSource(document);
    wrong_kind.source_kind = "fits";
    const specforge::SampleLabelingAsdfStoreOpenResult opened =
        specforge::OpenSampleLabelingAsdfDocumentStore(
            path,
            wrong_kind);
    Require(
        !opened.succeeded() &&
            opened.error.kind ==
                specforge::SampleLabelingAsdfStoreErrorKind::
                    SourceMismatch,
        "store open should reject a different canonical source kind");

    specforge::SampleLabelingSourceCompatibility missing_kind =
        CompatibleSource(document);
    missing_kind.source_kind = {};
    const specforge::SampleLabelingAsdfStoreOpenResult
        opened_without_kind =
            specforge::OpenSampleLabelingAsdfDocumentStore(
                path,
                missing_kind);
    Require(
        !opened_without_kind.succeeded() &&
            opened_without_kind.error.kind ==
                specforge::SampleLabelingAsdfStoreErrorKind::
                    SourceMismatch,
        "writable store open should not allow callers to bypass source-kind validation");
}

void TestAtomicFullWriteAndSourceAwareOpen()
{
    const std::filesystem::path directory =
        FreshTestDirectory("specforge-asdf-store-open");
    const std::filesystem::path path =
        directory / "nested" / "labels.asdf";
    const specforge::SampleLabelingDocument document = MakeDocument();

    const specforge::SampleLabelingAsdfStoreWriteResult write =
        specforge::WriteSampleLabelingAsdfDocumentAtomically(
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

    const specforge::SampleLabelingAsdfStoreOpenResult opened =
        specforge::OpenSampleLabelingAsdfDocumentStore(
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

    specforge::SampleLabelingSourceCompatibility wrong_source =
        CompatibleSource(document);
    wrong_source.base_identity = "another-source";
    const specforge::SampleLabelingAsdfStoreOpenResult rejected_identity =
        specforge::OpenSampleLabelingAsdfDocumentStore(
            path,
            wrong_source);
    Require(
        !rejected_identity.succeeded() &&
            rejected_identity.error.kind ==
                specforge::SampleLabelingAsdfStoreErrorKind::
                    SourceMismatch,
        "store open should reject a different base source identity");

    const std::vector<std::string> reordered = {
        "sample-b",
        "sample-a",
        "sample-c",
    };
    specforge::SampleLabelingSourceCompatibility wrong_roster =
        CompatibleSource(document);
    wrong_roster.sample_names = reordered;
    const specforge::SampleLabelingAsdfStoreOpenResult rejected_roster =
        specforge::OpenSampleLabelingAsdfDocumentStore(
            path,
            wrong_roster);
    Require(
        !rejected_roster.succeeded() &&
            rejected_roster.error.kind ==
                specforge::SampleLabelingAsdfStoreErrorKind::
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
    const specforge::SampleLabelingAsdfReadResult compatible_read =
        specforge::ReadSampleLabelingAsdfDocument(compatibility_path);
    Require(
        compatible_read.succeeded() &&
            !compatible_read.durable_base.has_value(),
        "codec compatibility read should accept a valid non-FLEVEL=2 roster without granting durable reuse");
    const specforge::SampleLabelingAsdfStoreOpenResult nondurable =
        specforge::OpenSampleLabelingAsdfDocumentStore(
            compatibility_path,
            CompatibleSource(document));
    Require(
        !nondurable.succeeded() &&
            nondurable.error.kind ==
                specforge::SampleLabelingAsdfStoreErrorKind::
                    DurableBaseUnavailable,
        "writable store open should reject a compatibility profile without a durable base");
    specforge::SampleLabelingDocument replacement = document;
    replacement.labeling.name = "unsafe replacement must fail";
    const specforge::SampleLabelingAsdfStoreWriteResult refused_replacement =
        specforge::WriteSampleLabelingAsdfDocumentAtomically(
            compatibility_path,
            replacement);
    Require(
        !refused_replacement.succeeded() &&
            refused_replacement.error.kind ==
                specforge::SampleLabelingAsdfStoreErrorKind::
                    DurableBaseUnavailable &&
            ReadAllBytes(compatibility_path) == compatibility_bytes &&
            !HasTemporarySibling(compatibility_path),
        "full write should refuse an existing document whose unknown metadata cannot be preserved safely");
}

void TestInitialCanonicalPublicationReopensExactGeneration()
{
    const std::filesystem::path directory =
        FreshTestDirectory("specforge-asdf-store-initial-publication");
    const std::filesystem::path path = directory / "labels.asdf";
    const specforge::SampleLabelingDocument document = MakeDocument();

    const specforge::SampleLabelingAsdfStoreGenerationWriteResult published =
        specforge::WriteSampleLabelingAsdfDocumentAndOpenAtomically(
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
    specforge::SampleLabelingSourceCompatibility wrong_source =
        CompatibleSource(document);
    wrong_source.source_kind = "fits";
    const specforge::SampleLabelingAsdfStoreGenerationWriteResult rejected =
        specforge::WriteSampleLabelingAsdfDocumentAndOpenAtomically(
            rejected_path,
            document,
            wrong_source);
    Require(
        !rejected.document_replaced &&
            !rejected.succeeded() &&
            rejected.error.kind ==
                specforge::SampleLabelingAsdfStoreErrorKind::
                    SourceMismatch &&
            !std::filesystem::exists(rejected_path),
        "initial canonical publication should reject source mismatch before replacement");
}

void TestValueOnlyRewriteReusesRosterBlockWithoutReopen()
{
    const std::filesystem::path directory =
        FreshTestDirectory("specforge-asdf-store-rewrite");
    const std::filesystem::path path = directory / "labels.asdf";
    const specforge::SampleLabelingDocument document = MakeDocument();
    Require(
        specforge::WriteSampleLabelingAsdfDocumentAtomically(
            path,
            document)
            .succeeded(),
        "rewrite fixture should write");
    specforge::SampleLabelingAsdfStoreOpenResult opened =
        specforge::OpenSampleLabelingAsdfDocumentStore(
            path,
            CompatibleSource(document));
    Require(opened.succeeded(), "rewrite fixture should open");
    const std::shared_ptr<const specforge::SampleLabelingDocument>
        generation_handle =
            opened.snapshot->document_handle();
    const std::string* const roster_storage =
        generation_handle->source.roster.sample_names.data();
    const std::vector<unsigned char> roster_before =
        FirstRawBlock(ReadAllBytes(path));

    const std::array<std::int32_t, 3> first_values = {7, 2, -1};
    const specforge::SampleLabelingAsdfStoreWriteResult first_rewrite =
        specforge::RewriteSampleLabelingAsdfValuesAtomically(
            *opened.snapshot,
            first_values);
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
    const specforge::SampleLabelingAsdfStoreWriteResult second_rewrite =
        specforge::RewriteSampleLabelingAsdfValuesAtomically(
            *opened.snapshot,
            second_values);
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

    const specforge::SampleLabelingAsdfStoreOpenResult reopened =
        specforge::OpenSampleLabelingAsdfDocumentStore(
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
        FreshTestDirectory("specforge-asdf-store-forward-metadata");
    const std::filesystem::path path = directory / "labels.asdf";
    const specforge::SampleLabelingDocument original = MakeDocument();
    Require(
        specforge::WriteSampleLabelingAsdfDocumentAtomically(path, original)
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

    specforge::SampleLabelingAsdfStoreOpenResult opened =
        specforge::OpenSampleLabelingAsdfDocumentStore(
            path,
            CompatibleSource(original));
    Require(opened.succeeded(), "forward-metadata store fixture should open");
    specforge::SampleLabelingDocument edited = opened.snapshot->document();
    edited.labeling.name = "Edited through snapshot";
    edited.labeling.labels[0].name = "snapshot edit";
    const std::shared_ptr<const specforge::SampleLabelingDocument>
        old_generation = opened.snapshot->document_handle();
    specforge::SampleLabelingSourceCompatibility
        incompatible_source = CompatibleSource(original);
    incompatible_source.source_kind = "fits";
    const std::vector<unsigned char> before_incompatible_source =
        ReadAllBytes(path);
    const specforge::SampleLabelingAsdfStoreGenerationWriteResult
        incompatible_rewrite =
            specforge::
                RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
                    *opened.snapshot,
                    edited,
                    incompatible_source);
    Require(
        !incompatible_rewrite.succeeded() &&
            !incompatible_rewrite.document_replaced &&
            incompatible_rewrite.error.kind ==
                specforge::SampleLabelingAsdfStoreErrorKind::
                    SourceMismatch &&
            ReadAllBytes(path) == before_incompatible_source &&
            !HasTemporarySibling(path),
        "metadata publication should reject an incompatible reopen descriptor before replacing the durable document");

    specforge::SampleLabelingAsdfStoreGenerationWriteResult
        snapshot_rewrite =
        specforge::RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
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
    specforge::SampleLabelingDocument unrelated =
        snapshot_rewrite.snapshot->document();
    unrelated.labeling.id = "another-task";
    const specforge::SampleLabelingAsdfStoreWriteResult identity_mismatch =
        specforge::RewriteSampleLabelingAsdfDocumentAtomically(
            *snapshot_rewrite.snapshot,
            unrelated);
    Require(
        !identity_mismatch.succeeded() &&
            identity_mismatch.error.kind ==
                specforge::SampleLabelingAsdfStoreErrorKind::
                    PreservationIdentityMismatch &&
            ReadAllBytes(path) == before_identity_mismatch &&
            !HasTemporarySibling(path),
        "metadata preservation must not carry unknown fields into another document identity");
    specforge::SampleLabelingDocument edited_again =
        snapshot_rewrite.snapshot->document();
    edited_again.annotation.name = "Edited through path write";
    const specforge::SampleLabelingAsdfStoreWriteResult path_rewrite =
        specforge::WriteSampleLabelingAsdfDocumentAtomically(
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

void TestWriteFailuresPreserveThePreviousDocument()
{
    const std::filesystem::path directory =
        FreshTestDirectory("specforge-asdf-store-failure");
    const std::filesystem::path path = directory / "labels.asdf";
    const specforge::SampleLabelingDocument document = MakeDocument();
    Require(
        specforge::WriteSampleLabelingAsdfDocumentAtomically(
            path,
            document)
            .succeeded(),
        "failure fixture should write");
    const std::vector<unsigned char> original = ReadAllBytes(path);

    specforge::SampleLabelingDocument invalid = document;
    invalid.annotation.values[0] = 99;
    const specforge::SampleLabelingAsdfStoreWriteResult codec_failure =
        specforge::WriteSampleLabelingAsdfDocumentAtomically(
            path,
            invalid);
    Require(
        !codec_failure.succeeded() &&
            codec_failure.error.kind ==
                specforge::SampleLabelingAsdfStoreErrorKind::CodecFailure &&
            ReadAllBytes(path) == original &&
            !HasTemporarySibling(path),
        "a full-write codec failure should preserve the previous document and clean its temporary file");

    // This store seam aborts publication immediately before replacement. The
    // shared atomic-file suite separately injects failures in the platform
    // replacement operation itself.
    bool full_publish_hook_reached = false;
    const specforge::SampleLabelingAsdfStoreWriteResult full_publish_abort =
        specforge::sample_labeling_asdf_store_test_seam::
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
                specforge::SampleLabelingAsdfStoreErrorKind::
                    AtomicWriteFailure &&
            ReadAllBytes(path) == original &&
            !HasTemporarySibling(path),
        "a full-write pre-replacement abort should preserve the previous document");

    specforge::SampleLabelingAsdfStoreOpenResult opened =
        specforge::OpenSampleLabelingAsdfDocumentStore(
            path,
            CompatibleSource(document));
    Require(opened.succeeded(), "failure fixture should open");
    const std::array<std::int32_t, 2> wrong_count = {2, 7};
    const specforge::SampleLabelingAsdfStoreWriteResult rewrite_codec_failure =
        specforge::RewriteSampleLabelingAsdfValuesAtomically(
            *opened.snapshot,
            wrong_count);
    Require(
        !rewrite_codec_failure.succeeded() &&
            rewrite_codec_failure.error.kind ==
                specforge::SampleLabelingAsdfStoreErrorKind::CodecFailure &&
            opened.snapshot->document().annotation.values ==
                document.annotation.values &&
            ReadAllBytes(path) == original &&
            !HasTemporarySibling(path),
        "a rewrite codec failure should preserve the previous document and open snapshot");

    bool metadata_publish_hook_reached = false;
    specforge::SampleLabelingDocument metadata_edit =
        opened.snapshot->document();
    metadata_edit.labeling.name = "metadata publication must abort";
    const specforge::SampleLabelingAsdfStoreWriteResult
        metadata_publish_abort =
            specforge::sample_labeling_asdf_store_test_seam::
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
                specforge::SampleLabelingAsdfStoreErrorKind::
                    AtomicWriteFailure &&
            ReadAllBytes(path) == original &&
            !HasTemporarySibling(path),
        "a metadata rewrite pre-replacement abort should preserve the previous document and clean its temporary file");

    bool rewrite_publish_hook_reached = false;
    const std::array<std::int32_t, 3> replacement = {7, 7, 2};
    const specforge::SampleLabelingAsdfStoreWriteResult
        rewrite_publish_abort =
            specforge::sample_labeling_asdf_store_test_seam::
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
                specforge::SampleLabelingAsdfStoreErrorKind::
                    AtomicWriteFailure &&
            ReadAllBytes(path) == original &&
            !HasTemporarySibling(path),
        "a rewrite pre-replacement abort should preserve the previous document");
}

}  // namespace

int main()
{
    try {
        TestSourceKindMismatchIsRejected();
        TestAtomicFullWriteAndSourceAwareOpen();
        TestInitialCanonicalPublicationReopensExactGeneration();
        TestValueOnlyRewriteReusesRosterBlockWithoutReopen();
        TestMetadataChangingRewritesPreserveForwardUnknownFields();
        TestWriteFailuresPreserveThePreviousDocument();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
