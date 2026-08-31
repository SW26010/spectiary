#include "domain/sample_labeling_asdf_store.h"
#include "platform/atomic_file.h"
#include "platform/file_sha256.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using specforge::SampleLabelingAsdfOpenSnapshot;
using specforge::SampleLabelingAsdfStoreErrorKind;
using specforge::SampleLabelingAsdfStoreGenerationWriteResult;
using specforge::SampleLabelingAsdfStoreWriteResult;
using specforge::SampleLabelingDocument;

constexpr std::array<std::string_view, 9> kForwardUnknownTokens = {
    "future_vendor",
    "new_flag: true",
    "new_text: preserve or ignore",
    "future_task",
    "token: task-survives",
    "future_origin",
    "token: origin-survives",
    "future_label",
    "token: label-survives",
};

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::filesystem::path FreshTestDirectory(std::string_view name)
{
    static std::uint64_t sequence = 0;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("specforge-asdf-store-property-" + std::string(name) + "-" +
            std::to_string(++sequence));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    error.clear();
    std::filesystem::create_directories(directory, error);
    Require(!error, "property test directory should be created");
    return directory;
}

std::filesystem::path FixturePath(std::string_view name)
{
    return std::filesystem::path(SPECFORGE_ASDF_LABELING_FIXTURE_DIR) /
        std::string(name);
}

std::vector<unsigned char> ReadAllBytes(
    const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    Require(stream.good(), "property test ASDF file should open");
    return std::vector<unsigned char>(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

void WriteAllBytes(
    const std::filesystem::path& path,
    std::span<const unsigned char> bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    Require(stream.good(), "property test ASDF output should open");
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    stream.close();
    Require(stream.good(), "property test ASDF output should flush");
}

bool ContainsText(
    std::span<const unsigned char> bytes,
    std::string_view text)
{
    return std::search(
               bytes.begin(), bytes.end(), text.begin(), text.end()) !=
        bytes.end();
}

void ReplaceTextOnce(
    std::vector<unsigned char>& bytes,
    std::string_view old_text,
    std::string_view new_text)
{
    const auto found = std::search(
        bytes.begin(), bytes.end(), old_text.begin(), old_text.end());
    Require(found != bytes.end(), "property test patch target should exist");
    const std::size_t offset =
        static_cast<std::size_t>(found - bytes.begin());
    bytes.erase(found, found + static_cast<std::ptrdiff_t>(old_text.size()));
    bytes.insert(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        new_text.begin(),
        new_text.end());
}

std::uint64_t ReadBigEndian(
    std::span<const unsigned char> bytes,
    std::size_t offset,
    std::size_t width)
{
    Require(
        offset <= bytes.size() && width <= bytes.size() - offset,
        "property test block header should be complete");
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index) {
        value = (value << 8U) | bytes[offset + index];
    }
    return value;
}

std::size_t FirstBlockOffset(const std::vector<unsigned char>& bytes)
{
    constexpr std::array<unsigned char, 4> magic = {
        0xd3,
        'B',
        'L',
        'K',
    };
    const auto block = std::search(
        bytes.begin(), bytes.end(), magic.begin(), magic.end());
    Require(block != bytes.end(), "property test ASDF should contain a block");
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
        "property test raw block should be complete");
    return std::vector<unsigned char>(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        bytes.begin() + static_cast<std::ptrdiff_t>(offset + raw_size));
}

bool HasTemporarySibling(const std::filesystem::path& target)
{
    const std::string prefix = target.filename().string() + ".tmp.";
    std::error_code error;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(target.parent_path(), error)) {
        if (error) {
            return true;
        }
        if (entry.path().filename().string().starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

std::string FileSha256(const std::filesystem::path& path)
{
    std::string error;
    const std::optional<std::string> digest =
        specforge::ComputeFileSha256(path, &error);
    Require(digest.has_value(), error.empty()
        ? "property test SHA-256 should succeed"
        : error);
    return *digest;
}

SampleLabelingDocument MakeDocument()
{
    SampleLabelingDocument document;
    document.source.base_identity = "source-base-v1";
    document.source.kind = "npy";
    document.source.name = "spectra.npy";
    document.source.fingerprint = "source-fingerprint-v1";
    document.source.sample_count = 3;
    document.source.roster.identity_kind = std::string{
        specforge::kSampleLabelingDocumentExplicitNamesRoster};
    document.source.roster.sample_names = {
        "sample-a",
        "sample-b",
        "sample-c",
    };
    document.annotation.values = {-1, 2, 7};
    document.labeling.id = "00000000-0000-4000-8000-000000000005";
    document.labeling.name = "Quality review";
    const auto timestamp = specforge::ParseCanonicalTimestamp(
        "2026-08-30T08:00:00.000Z");
    Require(timestamp.has_value(), "property timestamp should parse");
    document.labeling.canonical_metadata.created_at = *timestamp;
    document.labeling.canonical_metadata.modified_at = *timestamp;
    document.labeling.canonical_metadata.origin.kind = "manual";
    document.labeling.labels = {
        {2, "accepted", "a"},
        {7, "rejected", "r"},
    };
    return document;
}

specforge::SampleLabelingSourceCompatibility CompatibleSource(
    const SampleLabelingDocument& document)
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

specforge::CanonicalTimestamp AdvancedTimestamp(
    specforge::CanonicalTimestamp timestamp,
    std::chrono::milliseconds delta = std::chrono::milliseconds{1})
{
    const auto advanced = specforge::CanonicalTimestamp::FromTimePoint(
        timestamp.time_point() + delta);
    Require(advanced.has_value(), "advanced property timestamp should fit");
    return *advanced;
}

void ChangeValueToDifferentLegalCode(
    SampleLabelingDocument& document,
    std::size_t index)
{
    Require(index < document.annotation.values.size(),
        "values mutation index should be in range");
    const std::int32_t previous = document.annotation.values[index];
    for (const auto& label : document.labeling.labels) {
        if (label.code != previous) {
            document.annotation.values[index] = label.code;
            break;
        }
    }
    if (document.annotation.values[index] == previous &&
        document.annotation.missing.value != previous) {
        document.annotation.values[index] =
            document.annotation.missing.value;
    }
    Require(document.annotation.values[index] != previous,
        "values mutation should select a different legal value");
}

bool DocumentsEqual(
    const SampleLabelingDocument& left,
    const SampleLabelingDocument& right)
{
    if (left.format_kind != right.format_kind ||
        left.schema_version != right.schema_version ||
        left.source.base_identity != right.source.base_identity ||
        left.source.kind != right.source.kind ||
        left.source.name != right.source.name ||
        left.source.fingerprint != right.source.fingerprint ||
        left.source.sample_count != right.source.sample_count ||
        left.source.roster.identity_kind !=
            right.source.roster.identity_kind ||
        left.source.roster.sample_names !=
            right.source.roster.sample_names ||
        left.annotation.kind != right.annotation.kind ||
        left.annotation.missing.semantic !=
            right.annotation.missing.semantic ||
        left.annotation.missing.value != right.annotation.missing.value ||
        left.annotation.values != right.annotation.values ||
        left.labeling.id != right.labeling.id ||
        left.labeling.name != right.labeling.name ||
        left.labeling.canonical_metadata !=
            right.labeling.canonical_metadata ||
        left.labeling.labels.size() != right.labeling.labels.size()) {
        return false;
    }
    for (std::size_t index = 0;
         index < left.labeling.labels.size();
         ++index) {
        const auto& expected = left.labeling.labels[index];
        const auto& actual = right.labeling.labels[index];
        if (expected.code != actual.code ||
            expected.name != actual.name ||
            expected.shortcut != actual.shortcut) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> PresentUnknownTokens(
    std::span<const unsigned char> bytes)
{
    std::vector<std::string> tokens;
    for (const std::string_view token : kForwardUnknownTokens) {
        if (ContainsText(bytes, token)) {
            tokens.emplace_back(token);
        }
    }
    return tokens;
}

void RequireUnknownTokens(
    std::span<const unsigned char> bytes,
    const std::vector<std::string>& tokens,
    std::string_view context)
{
    for (const std::string& token : tokens) {
        Require(ContainsText(bytes, token), context);
    }
}

void RequireTargetMatches(
    const std::filesystem::path& path,
    const SampleLabelingDocument& expected,
    std::string_view context)
{
    const specforge::SampleLabelingAsdfReadResult reread =
        specforge::ReadSampleLabelingAsdfDocument(path);
    Require(reread.succeeded() &&
            DocumentsEqual(*reread.document, expected),
        context);
}

struct TrustedGeneration {
    std::vector<unsigned char> bytes;
    std::string sha256;
    SampleLabelingDocument document;
    std::vector<std::int32_t> values;
    specforge::CanonicalTimestamp modified_at;
    std::shared_ptr<const SampleLabelingDocument> document_handle;
    std::vector<std::string> unknown_tokens;
};

TrustedGeneration CaptureTrustedGeneration(
    const std::filesystem::path& path,
    const SampleLabelingAsdfOpenSnapshot& snapshot)
{
    const std::vector<unsigned char> bytes = ReadAllBytes(path);
    Require(snapshot.durable_base().valid(),
        "trusted property generation should have a durable base");
    return TrustedGeneration{
        .bytes = bytes,
        .sha256 = FileSha256(path),
        .document = snapshot.document(),
        .values = snapshot.document().annotation.values,
        .modified_at = snapshot.document()
            .labeling.canonical_metadata.modified_at,
        .document_handle = snapshot.document_handle(),
        .unknown_tokens = PresentUnknownTokens(bytes),
    };
}

void RequireControlledError(
    const SampleLabelingAsdfStoreWriteResult& result,
    SampleLabelingAsdfStoreErrorKind expected,
    std::string_view context)
{
    Require(!result.succeeded(), context);
    Require(result.error.kind != SampleLabelingAsdfStoreErrorKind::None,
        "failed property write should have a controlled error kind");
    Require(result.error.kind == expected, context);
}

void RequireControlledError(
    const SampleLabelingAsdfStoreGenerationWriteResult& result,
    SampleLabelingAsdfStoreErrorKind expected,
    std::string_view context)
{
    Require(!result.succeeded(), context);
    Require(result.error.kind != SampleLabelingAsdfStoreErrorKind::None,
        "failed property publication should have a controlled error kind");
    Require(result.error.kind == expected, context);
}

void VerifyDurableBaseRepresentsTrustedGeneration(
    const SampleLabelingAsdfOpenSnapshot& snapshot,
    const TrustedGeneration& trusted)
{
    SampleLabelingDocument probe = trusted.document;
    Require(!probe.annotation.values.empty(),
        "durable-base probe should have values");
    ChangeValueToDifferentLegalCode(probe, 0U);
    // Equal is valid for the trusted base T, but would be rejected if a failed
    // publication had incorrectly advanced the in-memory durable base to T+1.
    probe.labeling.canonical_metadata.modified_at = trusted.modified_at;
    std::ostringstream output(std::ios::out | std::ios::binary);
    const specforge::SampleLabelingAsdfWriteResult rewritten =
        specforge::RewriteSampleLabelingAsdfValuesPreservingRosterBlock(
            snapshot.durable_base(), output, probe);
    Require(rewritten.succeeded() && rewritten.durable_base.has_value(),
        "old durable base should still encode from the trusted generation");
    const std::string encoded = output.str();
    for (const std::string& token : trusted.unknown_tokens) {
        Require(encoded.find(token) != std::string::npos,
            "old durable base should retain trusted unknown mappings");
    }
}

void RequireTrustedGenerationUnchanged(
    const std::filesystem::path& path,
    const SampleLabelingAsdfOpenSnapshot& snapshot,
    const TrustedGeneration& trusted,
    std::string_view context)
{
    const std::vector<unsigned char> bytes = ReadAllBytes(path);
    Require(bytes == trusted.bytes, context);
    Require(FileSha256(path) == trusted.sha256, context);
    Require(snapshot.document_handle() == trusted.document_handle, context);
    Require(DocumentsEqual(snapshot.document(), trusted.document), context);
    Require(snapshot.document().annotation.values == trusted.values, context);
    Require(snapshot.document().labeling.canonical_metadata.modified_at ==
            trusted.modified_at,
        context);
    Require(snapshot.durable_base().valid(), context);
    RequireUnknownTokens(bytes, trusted.unknown_tokens, context);
    Require(!HasTemporarySibling(path), context);
    const specforge::SampleLabelingAsdfReadResult reread =
        specforge::ReadSampleLabelingAsdfDocument(path);
    Require(reread.succeeded() &&
            DocumentsEqual(*reread.document, trusted.document),
        context);
    VerifyDurableBaseRepresentsTrustedGeneration(snapshot, trusted);
}

specforge::SampleLabelingAsdfStoreOpenResult OpenForwardUnknownFixture(
    const std::filesystem::path& path)
{
    const std::vector<unsigned char> bytes =
        ReadAllBytes(FixturePath("forward_unknown.asdf"));
    Require(PresentUnknownTokens(bytes).size() == kForwardUnknownTokens.size(),
        "forward fixture should contain every supported unknown mapping token");
    WriteAllBytes(path, bytes);
    const specforge::SampleLabelingAsdfReadResult read =
        specforge::ReadSampleLabelingAsdfDocument(path);
    Require(read.succeeded(), "forward fixture should read");
    specforge::SampleLabelingAsdfStoreOpenResult opened =
        specforge::OpenSampleLabelingAsdfDocumentStore(
            path, CompatibleSource(*read.document));
    Require(opened.succeeded(), "forward fixture should open as a store");
    return opened;
}

specforge::sample_labeling_asdf_store_test_seam::BeforeReplace
InterruptBeforeReplace(bool& checkpoint_reached)
{
    return [&checkpoint_reached](
               const std::filesystem::path&,
               const std::filesystem::path&) {
        checkpoint_reached = true;
        throw std::runtime_error("injected publication interruption");
    };
}

void ThrowPreservedMetadataBuildFailure()
{
    throw std::runtime_error("injected preserved metadata construction failure");
}

void TestFirstFullPublicationOperation()
{
    const std::filesystem::path path =
        FreshTestDirectory("first-publication") / "labels.asdf";
    const SampleLabelingDocument intended = MakeDocument();
    bool checkpoint_reached = false;
    const SampleLabelingAsdfStoreGenerationWriteResult interrupted =
        specforge::sample_labeling_asdf_store_test_seam::
            WriteAndOpenWithCheckpoints(
                path,
                intended,
                CompatibleSource(intended),
                InterruptBeforeReplace(checkpoint_reached),
                {});
    Require(checkpoint_reached,
        "first publication should reach the before-replace checkpoint");
    RequireControlledError(interrupted,
        SampleLabelingAsdfStoreErrorKind::AtomicWriteFailure,
        "first publication interruption should be controlled");
    Require(!interrupted.document_replaced &&
            !std::filesystem::exists(path) &&
            !HasTemporarySibling(path),
        "first publication interruption should leave no partial output");

    const SampleLabelingAsdfStoreGenerationWriteResult retry =
        specforge::WriteSampleLabelingAsdfDocumentAndOpenAtomically(
            path, intended, CompatibleSource(intended));
    Require(retry.succeeded() && retry.document_replaced &&
            DocumentsEqual(retry.snapshot->document(), intended),
        retry.error.message.empty()
            ? "first publication retry should publish the intended generation"
            : retry.error.message);
}

void TestOverwriteExistingOperation()
{
    const std::filesystem::path path =
        FreshTestDirectory("overwrite-existing") / "labels.asdf";
    const SampleLabelingDocument original = MakeDocument();
    Require(specforge::WriteSampleLabelingAsdfDocumentAtomically(path, original)
            .succeeded(),
        "overwrite fixture should write");
    specforge::SampleLabelingAsdfStoreOpenResult opened =
        specforge::OpenSampleLabelingAsdfDocumentStore(
            path, CompatibleSource(original));
    Require(opened.succeeded(), "overwrite fixture should open");
    const TrustedGeneration trusted =
        CaptureTrustedGeneration(path, *opened.snapshot);
    SampleLabelingDocument intended = original;
    intended.labeling.name = "Overwritten generation";
    intended.labeling.canonical_metadata.modified_at =
        AdvancedTimestamp(trusted.modified_at);

    bool checkpoint_reached = false;
    const SampleLabelingAsdfStoreWriteResult interrupted =
        specforge::sample_labeling_asdf_store_test_seam::
            WriteWithBeforeReplace(
                path,
                intended,
                InterruptBeforeReplace(checkpoint_reached));
    Require(checkpoint_reached,
        "full overwrite should reach the before-replace checkpoint");
    RequireControlledError(interrupted,
        SampleLabelingAsdfStoreErrorKind::AtomicWriteFailure,
        "full overwrite interruption should be controlled");
    RequireTrustedGenerationUnchanged(
        path, *opened.snapshot, trusted,
        "full overwrite interruption should preserve the trusted generation");

    const SampleLabelingAsdfStoreWriteResult retry =
        specforge::WriteSampleLabelingAsdfDocumentAtomically(path, intended);
    Require(retry.succeeded(), retry.error.message.empty()
        ? "full overwrite retry should succeed"
        : retry.error.message);
    const specforge::SampleLabelingAsdfReadResult reread =
        specforge::ReadSampleLabelingAsdfDocument(path);
    Require(reread.succeeded() && DocumentsEqual(*reread.document, intended),
        "full overwrite retry should publish the intended generation");
}

void TestValuesRewriteOperation()
{
    const std::filesystem::path path =
        FreshTestDirectory("values-rewrite") / "labels.asdf";
    specforge::SampleLabelingAsdfStoreOpenResult opened =
        OpenForwardUnknownFixture(path);
    const TrustedGeneration trusted =
        CaptureTrustedGeneration(path, *opened.snapshot);
    SampleLabelingDocument intended = opened.snapshot->document();
    ChangeValueToDifferentLegalCode(intended, 0U);
    Require(intended.annotation.values != trusted.values,
        "values rewrite should intend a different values generation");
    intended.labeling.canonical_metadata.modified_at =
        AdvancedTimestamp(trusted.modified_at);

    bool checkpoint_reached = false;
    const SampleLabelingAsdfStoreWriteResult interrupted =
        specforge::sample_labeling_asdf_store_test_seam::
            RewriteWithBeforeReplace(
                *opened.snapshot,
                intended,
                InterruptBeforeReplace(checkpoint_reached));
    Require(checkpoint_reached,
        "values rewrite should reach the before-replace checkpoint");
    RequireControlledError(interrupted,
        SampleLabelingAsdfStoreErrorKind::AtomicWriteFailure,
        "values rewrite interruption should be controlled");
    RequireTrustedGenerationUnchanged(
        path, *opened.snapshot, trusted,
        "values rewrite interruption should preserve the trusted generation");

    const SampleLabelingAsdfStoreWriteResult retry =
        specforge::RewriteSampleLabelingAsdfValuesAtomically(
            *opened.snapshot, intended);
    Require(retry.succeeded() && retry.roster_block_reused &&
            DocumentsEqual(opened.snapshot->document(), intended),
        retry.error.message.empty()
            ? "values rewrite retry should advance the snapshot"
            : retry.error.message);
    RequireTargetMatches(path, intended,
        "values rewrite retry should publish matching values and timestamp to disk");
}

void TestMetadataRewriteOperation()
{
    const std::filesystem::path path =
        FreshTestDirectory("metadata-rewrite") / "labels.asdf";
    specforge::SampleLabelingAsdfStoreOpenResult opened =
        OpenForwardUnknownFixture(path);
    const TrustedGeneration trusted =
        CaptureTrustedGeneration(path, *opened.snapshot);
    SampleLabelingDocument intended = opened.snapshot->document();
    intended.labeling.name = "Metadata rewrite";
    intended.labeling.canonical_metadata.modified_at =
        AdvancedTimestamp(trusted.modified_at);

    bool checkpoint_reached = false;
    const SampleLabelingAsdfStoreWriteResult interrupted =
        specforge::sample_labeling_asdf_store_test_seam::
            RewriteDocumentWithBeforeReplace(
                *opened.snapshot,
                intended,
                InterruptBeforeReplace(checkpoint_reached));
    Require(checkpoint_reached,
        "metadata rewrite should reach the before-replace checkpoint");
    RequireControlledError(interrupted,
        SampleLabelingAsdfStoreErrorKind::AtomicWriteFailure,
        "metadata rewrite interruption should be controlled");
    RequireTrustedGenerationUnchanged(
        path, *opened.snapshot, trusted,
        "metadata rewrite interruption should preserve the trusted generation");

    const SampleLabelingAsdfStoreGenerationWriteResult retry =
        specforge::RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
            *opened.snapshot, intended, CompatibleSource(intended));
    Require(retry.succeeded() && retry.document_replaced &&
            DocumentsEqual(retry.snapshot->document(), intended),
        retry.error.message.empty()
            ? "metadata rewrite retry should reopen the intended generation"
            : retry.error.message);
    RequireTargetMatches(path, intended,
        "metadata rewrite retry should publish the intended generation to disk");
    RequireUnknownTokens(ReadAllBytes(path), trusted.unknown_tokens,
        "metadata rewrite retry should preserve supported unknown mappings");
}

void TestRewriteAndReopenOperation()
{
    const std::filesystem::path path =
        FreshTestDirectory("rewrite-and-reopen") / "labels.asdf";
    specforge::SampleLabelingAsdfStoreOpenResult opened =
        OpenForwardUnknownFixture(path);
    const TrustedGeneration trusted =
        CaptureTrustedGeneration(path, *opened.snapshot);
    SampleLabelingDocument intended = opened.snapshot->document();
    intended.labeling.canonical_metadata.description =
        "rewrite-and-reopen property";
    intended.labeling.canonical_metadata.modified_at =
        AdvancedTimestamp(trusted.modified_at);

    bool checkpoint_reached = false;
    const SampleLabelingAsdfStoreGenerationWriteResult interrupted =
        specforge::sample_labeling_asdf_store_test_seam::
            RewriteDocumentAndReopenWithCheckpoints(
                *opened.snapshot,
                intended,
                CompatibleSource(intended),
                InterruptBeforeReplace(checkpoint_reached),
                {});
    Require(checkpoint_reached,
        "rewrite-and-reopen should reach the before-replace checkpoint");
    RequireControlledError(interrupted,
        SampleLabelingAsdfStoreErrorKind::AtomicWriteFailure,
        "rewrite-and-reopen interruption should be controlled");
    Require(!interrupted.document_replaced,
        "pre-replacement rewrite failure should report no replacement");
    RequireTrustedGenerationUnchanged(
        path, *opened.snapshot, trusted,
        "rewrite-and-reopen interruption should preserve the trusted generation");

    const SampleLabelingAsdfStoreGenerationWriteResult retry =
        specforge::RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
            *opened.snapshot, intended, CompatibleSource(intended));
    Require(retry.succeeded() && retry.document_replaced &&
            DocumentsEqual(retry.snapshot->document(), intended),
        retry.error.message.empty()
            ? "rewrite-and-reopen retry should succeed"
            : retry.error.message);
    RequireTargetMatches(path, intended,
        "rewrite-and-reopen retry should publish the intended generation to disk");
}

void TestRepeatedValuesThroughOneSnapshotOperation()
{
    const std::filesystem::path path =
        FreshTestDirectory("repeated-values") / "labels.asdf";
    specforge::SampleLabelingAsdfStoreOpenResult opened =
        OpenForwardUnknownFixture(path);
    SampleLabelingDocument first = opened.snapshot->document();
    const std::vector<std::int32_t> initial_values =
        opened.snapshot->document().annotation.values;
    ChangeValueToDifferentLegalCode(first, 0U);
    Require(first.annotation.values != initial_values,
        "first repeated rewrite should intend a different values generation");
    first.labeling.canonical_metadata.modified_at = AdvancedTimestamp(
        first.labeling.canonical_metadata.modified_at);
    Require(specforge::RewriteSampleLabelingAsdfValuesAtomically(
                *opened.snapshot, first)
            .succeeded(),
        "first repeated values rewrite should succeed");

    const TrustedGeneration trusted =
        CaptureTrustedGeneration(path, *opened.snapshot);
    SampleLabelingDocument second = opened.snapshot->document();
    ChangeValueToDifferentLegalCode(
        second, second.annotation.values.size() - 1U);
    Require(second.annotation.values != trusted.values,
        "second repeated rewrite should differ from the first published generation");
    second.labeling.canonical_metadata.modified_at =
        AdvancedTimestamp(trusted.modified_at);
    bool checkpoint_reached = false;
    const SampleLabelingAsdfStoreWriteResult interrupted =
        specforge::sample_labeling_asdf_store_test_seam::
            RewriteWithBeforeReplace(
                *opened.snapshot,
                second,
                InterruptBeforeReplace(checkpoint_reached));
    Require(checkpoint_reached,
        "repeated values rewrite should reach the before-replace checkpoint");
    RequireControlledError(interrupted,
        SampleLabelingAsdfStoreErrorKind::AtomicWriteFailure,
        "second repeated values rewrite interruption should be controlled");
    RequireTrustedGenerationUnchanged(
        path, *opened.snapshot, trusted,
        "second repeated rewrite should retain the first published generation");
    const SampleLabelingAsdfStoreWriteResult retry =
        specforge::RewriteSampleLabelingAsdfValuesAtomically(
            *opened.snapshot, second);
    Require(retry.succeeded() && retry.roster_block_reused &&
            DocumentsEqual(opened.snapshot->document(), second),
        retry.error.message.empty()
            ? "second repeated values rewrite retry should succeed"
            : retry.error.message);
    RequireTargetMatches(path, second,
        "repeated values retry should publish matching values and timestamp to disk");
}

void TestOperationMatrix()
{
    using Operation = std::pair<std::string_view, void (*)()>;
    constexpr std::array<Operation, 6> operations = {{
        {"first full publication", TestFirstFullPublicationOperation},
        {"overwrite existing canonical", TestOverwriteExistingOperation},
        {"values rewrite", TestValuesRewriteOperation},
        {"metadata rewrite", TestMetadataRewriteOperation},
        {"rewrite and reopen", TestRewriteAndReopenOperation},
        {"repeated values through snapshot",
            TestRepeatedValuesThroughOneSnapshotOperation},
    }};
    for (const auto& [name, operation] : operations) {
        try {
            operation();
        } catch (const std::exception& error) {
            throw std::runtime_error(
                std::string(name) + ": " + error.what());
        }
    }
}

void TestSemanticPreflightFailure()
{
    const std::filesystem::path path =
        FreshTestDirectory("semantic-preflight") / "labels.asdf";
    const SampleLabelingDocument original = MakeDocument();
    Require(specforge::WriteSampleLabelingAsdfDocumentAtomically(path, original)
            .succeeded(),
        "semantic preflight fixture should write");
    specforge::SampleLabelingAsdfStoreOpenResult opened =
        specforge::OpenSampleLabelingAsdfDocumentStore(
            path, CompatibleSource(original));
    Require(opened.succeeded(), "semantic preflight fixture should open");
    const TrustedGeneration trusted =
        CaptureTrustedGeneration(path, *opened.snapshot);
    SampleLabelingDocument invalid = opened.snapshot->document();
    invalid.annotation.values.pop_back();

    for (int attempt = 0; attempt < 2; ++attempt) {
        const SampleLabelingAsdfStoreWriteResult rejected =
            specforge::RewriteSampleLabelingAsdfValuesAtomically(
                *opened.snapshot, invalid);
        RequireControlledError(rejected,
            SampleLabelingAsdfStoreErrorKind::CodecFailure,
            "semantic preflight should return a controlled codec error");
        Require(rejected.error.codec_kind ==
                specforge::SampleLabelingAsdfErrorKind::
                    SemanticValidationFailed,
            "semantic preflight should identify semantic validation");
        RequireTrustedGenerationUnchanged(
            path, *opened.snapshot, trusted,
            "semantic preflight rejection should preserve the trusted generation");
    }
}

void TestResourceLimitPreflightFailure()
{
    const std::filesystem::path path =
        FreshTestDirectory("resource-preflight") / "labels.asdf";
    const SampleLabelingDocument original = MakeDocument();
    Require(specforge::WriteSampleLabelingAsdfDocumentAtomically(path, original)
            .succeeded(),
        "resource preflight fixture should write");
    specforge::SampleLabelingAsdfStoreOpenResult opened =
        specforge::OpenSampleLabelingAsdfDocumentStore(
            path, CompatibleSource(original));
    Require(opened.succeeded(), "resource preflight fixture should open");
    const TrustedGeneration trusted =
        CaptureTrustedGeneration(path, *opened.snapshot);
    SampleLabelingDocument oversized = opened.snapshot->document();
    oversized.labeling.canonical_metadata.description =
        std::string(8U * 1024U * 1024U + 256U, 'x');
    oversized.labeling.canonical_metadata.modified_at =
        AdvancedTimestamp(trusted.modified_at);

    for (int attempt = 0; attempt < 2; ++attempt) {
        const SampleLabelingAsdfStoreWriteResult rejected =
            specforge::WriteSampleLabelingAsdfDocumentAtomically(
                path, oversized);
        RequireControlledError(rejected,
            SampleLabelingAsdfStoreErrorKind::CodecFailure,
            "resource preflight should return a controlled codec error");
        Require(rejected.error.codec_kind ==
                specforge::SampleLabelingAsdfErrorKind::
                    ResourceLimitExceeded,
            "resource preflight should identify the codec resource limit");
        RequireTrustedGenerationUnchanged(
            path, *opened.snapshot, trusted,
            "resource preflight rejection should preserve the trusted generation");
    }
}

void TestPreservationIdentityMismatch()
{
    const std::filesystem::path path =
        FreshTestDirectory("identity-mismatch") / "labels.asdf";
    specforge::SampleLabelingAsdfStoreOpenResult opened =
        OpenForwardUnknownFixture(path);
    const TrustedGeneration trusted =
        CaptureTrustedGeneration(path, *opened.snapshot);
    SampleLabelingDocument unrelated = opened.snapshot->document();
    unrelated.labeling.id = "00000000-0000-4000-8000-000000000099";

    for (int attempt = 0; attempt < 2; ++attempt) {
        const SampleLabelingAsdfStoreWriteResult rejected =
            specforge::RewriteSampleLabelingAsdfDocumentAtomically(
                *opened.snapshot, unrelated);
        RequireControlledError(rejected,
            SampleLabelingAsdfStoreErrorKind::PreservationIdentityMismatch,
            "identity mismatch should be controlled");
        RequireTrustedGenerationUnchanged(
            path, *opened.snapshot, trusted,
            "identity mismatch should preserve the trusted generation");
    }
}

void TestMissingDurableBase()
{
    const std::filesystem::path path =
        FreshTestDirectory("missing-durable-base") / "labels.asdf";
    const SampleLabelingDocument original = MakeDocument();
    Require(specforge::WriteSampleLabelingAsdfDocumentAtomically(path, original)
            .succeeded(),
        "missing durable-base fixture should write");
    std::vector<unsigned char> compatibility_bytes = ReadAllBytes(path);
    const std::size_t block_offset = FirstBlockOffset(compatibility_bytes);
    const std::size_t header_size = static_cast<std::size_t>(
        ReadBigEndian(compatibility_bytes, block_offset + 4U, 2U));
    const std::size_t zlib_header = block_offset + 6U + header_size;
    Require(zlib_header + 1U < compatibility_bytes.size() &&
            compatibility_bytes[zlib_header] == 0x78U,
        "compatibility fixture should contain a zlib header");
    compatibility_bytes[zlib_header + 1U] = 0x01U;
    WriteAllBytes(path, compatibility_bytes);
    const std::string trusted_sha = FileSha256(path);
    const specforge::SampleLabelingAsdfReadResult compatible =
        specforge::ReadSampleLabelingAsdfDocument(path);
    Require(compatible.succeeded() && !compatible.durable_base.has_value(),
        "compatibility fixture should read without a durable base");
    const SampleLabelingDocument trusted_document = *compatible.document;
    SampleLabelingDocument intended = trusted_document;
    intended.labeling.name = "must not replace nondurable input";
    intended.labeling.canonical_metadata.modified_at = AdvancedTimestamp(
        trusted_document.labeling.canonical_metadata.modified_at);

    for (int attempt = 0; attempt < 2; ++attempt) {
        const SampleLabelingAsdfStoreWriteResult rejected =
            specforge::WriteSampleLabelingAsdfDocumentAtomically(
                path, intended);
        RequireControlledError(rejected,
            SampleLabelingAsdfStoreErrorKind::DurableBaseUnavailable,
            "missing durable base should be controlled");
        Require(ReadAllBytes(path) == compatibility_bytes &&
                FileSha256(path) == trusted_sha &&
                !HasTemporarySibling(path),
            "missing durable base should preserve target bytes and SHA-256");
        const specforge::SampleLabelingAsdfReadResult reread =
            specforge::ReadSampleLabelingAsdfDocument(path);
        Require(reread.succeeded() &&
                DocumentsEqual(*reread.document, trusted_document),
            "missing durable base should preserve old values and metadata");
    }
}

void TestTemporaryOutputStreamFailure()
{
    const std::filesystem::path path =
        FreshTestDirectory("temporary-stream") / "labels.asdf";
    specforge::SampleLabelingAsdfStoreOpenResult opened =
        OpenForwardUnknownFixture(path);
    const TrustedGeneration trusted =
        CaptureTrustedGeneration(path, *opened.snapshot);
    SampleLabelingDocument intended = opened.snapshot->document();
    intended.labeling.name = "temporary stream retry";
    intended.labeling.canonical_metadata.modified_at =
        AdvancedTimestamp(trusted.modified_at);

    const SampleLabelingAsdfStoreWriteResult failed =
        specforge::sample_labeling_asdf_store_test_seam::
            WriteWithBeforeCodecWrite(
                path,
                intended,
                [](std::ostream& output) {
                    output.setstate(std::ios::badbit);
                });
    RequireControlledError(failed,
        SampleLabelingAsdfStoreErrorKind::CodecFailure,
        "temporary stream failure should be controlled");
    Require(failed.error.codec_kind ==
            specforge::SampleLabelingAsdfErrorKind::IoFailure,
        "temporary stream failure should be an I/O codec error");
    RequireTrustedGenerationUnchanged(
        path, *opened.snapshot, trusted,
        "temporary stream failure should preserve the trusted generation");
    const SampleLabelingAsdfStoreWriteResult retry =
        specforge::WriteSampleLabelingAsdfDocumentAtomically(path, intended);
    Require(retry.succeeded(), retry.error.message.empty()
        ? "temporary stream failure should be retryable"
        : retry.error.message);
    RequireTargetMatches(path, intended,
        "temporary stream retry should publish the intended generation to disk");
}

void TestReplacementFailureThenCoreRetry()
{
    const std::filesystem::path path =
        FreshTestDirectory("replacement-failure") / "labels.asdf";
    specforge::SampleLabelingAsdfStoreOpenResult opened =
        OpenForwardUnknownFixture(path);
    const TrustedGeneration trusted =
        CaptureTrustedGeneration(path, *opened.snapshot);
    const std::vector<unsigned char> trusted_roster =
        FirstRawBlock(trusted.bytes);
    SampleLabelingDocument intended = opened.snapshot->document();
    ChangeValueToDifferentLegalCode(intended, 0U);
    Require(intended.annotation.values != trusted.values,
        "replacement retry should intend a different values generation");
    intended.labeling.canonical_metadata.modified_at =
        AdvancedTimestamp(trusted.modified_at);
    bool removed_temporary = false;
    const SampleLabelingAsdfStoreWriteResult failed =
        specforge::sample_labeling_asdf_store_test_seam::
            RewriteWithBeforeReplace(
                *opened.snapshot,
                intended,
                [&removed_temporary](
                    const std::filesystem::path& temporary,
                    const std::filesystem::path&) {
                    std::error_code error;
                    removed_temporary =
                        std::filesystem::remove(temporary, error);
                    Require(!error && removed_temporary,
                        "replacement-failure seam should remove the temporary file");
                });
    Require(removed_temporary, "replacement failure seam should run");
    RequireControlledError(failed,
        SampleLabelingAsdfStoreErrorKind::AtomicWriteFailure,
        "replacement failure should be controlled");
    RequireTrustedGenerationUnchanged(
        path, *opened.snapshot, trusted,
        "replacement failure should preserve bytes, SHA, values, metadata, and snapshot");
    Require(FirstRawBlock(ReadAllBytes(path)) == trusted_roster,
        "replacement failure should preserve the old encoded roster");

    const SampleLabelingAsdfStoreWriteResult retry =
        specforge::RewriteSampleLabelingAsdfValuesAtomically(
            *opened.snapshot, intended);
    const std::vector<unsigned char> published = ReadAllBytes(path);
    Require(retry.succeeded() && retry.roster_block_reused &&
            opened.snapshot->document_handle() == trusted.document_handle &&
            DocumentsEqual(opened.snapshot->document(), intended) &&
            FirstRawBlock(published) == trusted_roster,
        retry.error.message.empty()
            ? "the same intended values replacement should succeed on retry"
            : retry.error.message);
    RequireTargetMatches(path, intended,
        "replacement retry should publish matching values and timestamp to disk");
    RequireUnknownTokens(published, trusted.unknown_tokens,
        "successful retry should retain supported unknown mappings");
}

void TestExternalReplacementDuringPublication()
{
    const std::filesystem::path directory =
        FreshTestDirectory("external-replacement");
    const std::filesystem::path path = directory / "labels.asdf";
    specforge::SampleLabelingAsdfStoreOpenResult opened =
        OpenForwardUnknownFixture(path);
    const TrustedGeneration old_trusted =
        CaptureTrustedGeneration(path, *opened.snapshot);
    std::vector<unsigned char> external_bytes = old_trusted.bytes;
    ReplaceTextOnce(external_bytes,
        "\nschema_version: ",
        "\nexternal_generation: mid-publication\nschema_version: ");
    const std::filesystem::path external_path =
        directory / "external-generation.asdf";
    WriteAllBytes(external_path, external_bytes);
    const specforge::SampleLabelingAsdfReadResult external_read =
        specforge::ReadSampleLabelingAsdfDocument(external_path);
    Require(external_read.succeeded() && external_read.durable_base.has_value(),
        "external generation should be a valid durable document");
    const std::string external_sha = FileSha256(external_path);
    SampleLabelingDocument intended = opened.snapshot->document();
    ChangeValueToDifferentLegalCode(intended, 0U);
    Require(intended.annotation.values != old_trusted.values,
        "external replacement case should intend a different values generation");
    intended.labeling.canonical_metadata.modified_at =
        AdvancedTimestamp(old_trusted.modified_at);

    bool external_replaced = false;
    const SampleLabelingAsdfStoreWriteResult failed =
        specforge::sample_labeling_asdf_store_test_seam::
            RewriteWithBeforeReplace(
                *opened.snapshot,
                intended,
                [&external_replaced, &external_path](
                    const std::filesystem::path&,
                    const std::filesystem::path& target) {
                    std::string error;
                    external_replaced = specforge::ReplaceFileAtomically(
                        external_path, target, &error,
                        "external property generation");
                    Require(external_replaced, error.empty()
                        ? "external property replacement should succeed"
                        : error);
                    throw std::runtime_error(
                        "external writer replaced the target generation");
                });
    Require(external_replaced, "external replacement seam should run");
    RequireControlledError(failed,
        SampleLabelingAsdfStoreErrorKind::AtomicWriteFailure,
        "external replacement interruption should be controlled");
    Require(ReadAllBytes(path) == external_bytes &&
            FileSha256(path) == external_sha &&
            !HasTemporarySibling(path),
        "external replacement should leave the externally published generation intact");
    Require(opened.snapshot->document_handle() == old_trusted.document_handle &&
            DocumentsEqual(opened.snapshot->document(), old_trusted.document) &&
            opened.snapshot->durable_base().valid(),
        "external replacement should not mutate the old in-memory snapshot");

    specforge::SampleLabelingAsdfStoreOpenResult recovered =
        specforge::OpenSampleLabelingAsdfDocumentStore(
            path, CompatibleSource(*external_read.document));
    Require(recovered.succeeded(),
        "owner should conservatively reopen after external replacement");
    const SampleLabelingAsdfStoreWriteResult retry =
        specforge::RewriteSampleLabelingAsdfValuesAtomically(
            *recovered.snapshot, intended);
    Require(retry.succeeded() && retry.roster_block_reused &&
            DocumentsEqual(recovered.snapshot->document(), intended) &&
            ContainsText(ReadAllBytes(path), "external_generation"),
        retry.error.message.empty()
            ? "same intended replacement should succeed through the recovered snapshot"
            : retry.error.message);
    RequireTargetMatches(path, intended,
        "external replacement recovery should publish the intended generation to disk");
}

void TestReopenGenerationMismatchRequiresRecovery()
{
    const std::filesystem::path directory =
        FreshTestDirectory("generation-mismatch");
    const std::filesystem::path path = directory / "labels.asdf";
    specforge::SampleLabelingAsdfStoreOpenResult opened =
        OpenForwardUnknownFixture(path);
    const TrustedGeneration old_trusted =
        CaptureTrustedGeneration(path, *opened.snapshot);

    const std::filesystem::path external_path =
        directory / "external-generation.asdf";
    WriteAllBytes(external_path, old_trusted.bytes);
    specforge::SampleLabelingAsdfStoreOpenResult external_opened =
        specforge::OpenSampleLabelingAsdfDocumentStore(
            external_path, CompatibleSource(old_trusted.document));
    Require(external_opened.succeeded(),
        "mismatch external generation should open");
    SampleLabelingDocument external_document =
        external_opened.snapshot->document();
    ChangeValueToDifferentLegalCode(
        external_document,
        external_document.annotation.values.size() - 1U);
    Require(external_document.annotation.values != old_trusted.values,
        "generation mismatch replacement should publish different values");
    external_document.labeling.canonical_metadata.modified_at =
        AdvancedTimestamp(old_trusted.modified_at);
    Require(specforge::RewriteSampleLabelingAsdfValuesAtomically(
                *external_opened.snapshot, external_document)
            .succeeded(),
        "mismatch external generation should publish");
    const std::vector<unsigned char> external_bytes =
        ReadAllBytes(external_path);
    const std::string external_sha = FileSha256(external_path);

    SampleLabelingDocument intended = opened.snapshot->document();
    intended.labeling.name = "intended metadata generation";
    intended.labeling.canonical_metadata.modified_at = AdvancedTimestamp(
        old_trusted.modified_at, std::chrono::milliseconds{2});
    bool checkpoint_replaced = false;
    const SampleLabelingAsdfStoreGenerationWriteResult mismatch =
        specforge::sample_labeling_asdf_store_test_seam::
            RewriteDocumentAndReopenWithCheckpoints(
                *opened.snapshot,
                intended,
                CompatibleSource(intended),
                {},
                [&checkpoint_replaced, &external_path](
                    const std::filesystem::path& target) {
                    std::string error;
                    checkpoint_replaced = specforge::ReplaceFileAtomically(
                        external_path, target, &error,
                        "mismatched property generation");
                    Require(checkpoint_replaced, error.empty()
                        ? "mismatched generation replacement should succeed"
                        : error);
                });
    Require(checkpoint_replaced,
        "after-replace generation checkpoint should run");
    RequireControlledError(mismatch,
        SampleLabelingAsdfStoreErrorKind::PublishedGenerationMismatch,
        "reopened generation mismatch should be controlled");
    Require(mismatch.document_replaced && !mismatch.snapshot.has_value(),
        "generation mismatch must report replacement without a writable snapshot");
    Require(ReadAllBytes(path) == external_bytes &&
            FileSha256(path) == external_sha &&
            !HasTemporarySibling(path),
        "generation mismatch should leave the actually published generation intact");
    Require(opened.snapshot->document_handle() == old_trusted.document_handle &&
            DocumentsEqual(opened.snapshot->document(), old_trusted.document) &&
            opened.snapshot->durable_base().valid(),
        "generation mismatch must not advance the old owner snapshot");

    specforge::SampleLabelingAsdfStoreOpenResult recovered =
        specforge::OpenSampleLabelingAsdfDocumentStore(
            path, CompatibleSource(external_document));
    Require(recovered.succeeded(),
        "generation mismatch should require and permit conservative reopen");
    const SampleLabelingAsdfStoreGenerationWriteResult retry =
        specforge::RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
            *recovered.snapshot, intended, CompatibleSource(intended));
    Require(retry.succeeded() && retry.document_replaced &&
            DocumentsEqual(retry.snapshot->document(), intended),
        retry.error.message.empty()
            ? "same intended generation should publish after conservative recovery"
            : retry.error.message);
    RequireTargetMatches(path, intended,
        "generation mismatch recovery should publish the intended generation to disk");
}

void TestUnsupportedChecksumProfile()
{
    const std::filesystem::path path =
        FreshTestDirectory("unsupported-profile") / "labels.asdf";
    const std::vector<unsigned char> trusted =
        ReadAllBytes(FixturePath("profile_checksum.asdf"));
    WriteAllBytes(path, trusted);
    const std::string trusted_sha = FileSha256(path);
    const SampleLabelingDocument intended = MakeDocument();

    for (int attempt = 0; attempt < 2; ++attempt) {
        const SampleLabelingAsdfStoreWriteResult rejected =
            specforge::WriteSampleLabelingAsdfDocumentAtomically(
                path, intended);
        RequireControlledError(rejected,
            SampleLabelingAsdfStoreErrorKind::CodecFailure,
            "unsupported checksum/profile should be controlled");
        Require(rejected.error.codec_kind ==
                specforge::SampleLabelingAsdfErrorKind::UnsupportedProfile,
            "unsupported checksum/profile should retain its codec classification");
        Require(ReadAllBytes(path) == trusted &&
                FileSha256(path) == trusted_sha &&
                !HasTemporarySibling(path),
            "unsupported checksum/profile should remain byte-for-byte unchanged");
    }
}

void TestUnknownMetadataConstructionFailure()
{
    const std::filesystem::path path =
        FreshTestDirectory("unknown-construction") / "labels.asdf";
    specforge::SampleLabelingAsdfStoreOpenResult opened =
        OpenForwardUnknownFixture(path);
    const TrustedGeneration trusted =
        CaptureTrustedGeneration(path, *opened.snapshot);
    SampleLabelingDocument intended = opened.snapshot->document();
    intended.labeling.name = "unknown metadata retry";
    intended.labeling.canonical_metadata.modified_at =
        AdvancedTimestamp(trusted.modified_at);

    const SampleLabelingAsdfStoreWriteResult failed =
        specforge::sample_labeling_asdf_store_test_seam::
            RewriteDocumentWithBeforePreservedMetadataBuild(
                *opened.snapshot,
                intended,
                ThrowPreservedMetadataBuildFailure);
    RequireControlledError(failed,
        SampleLabelingAsdfStoreErrorKind::CodecFailure,
        "unknown metadata construction failure should be controlled");
    Require(failed.error.codec_kind ==
            specforge::SampleLabelingAsdfErrorKind::IoFailure,
        "unknown metadata construction exception should be classified as codec I/O failure");
    RequireTrustedGenerationUnchanged(
        path, *opened.snapshot, trusted,
        "unknown metadata construction failure should preserve the trusted generation");

    const SampleLabelingAsdfStoreGenerationWriteResult retry =
        specforge::RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
            *opened.snapshot, intended, CompatibleSource(intended));
    const std::vector<unsigned char> retry_bytes = ReadAllBytes(path);
    Require(retry.succeeded() && retry.document_replaced &&
            DocumentsEqual(retry.snapshot->document(), intended),
        retry.error.message.empty()
            ? "unknown metadata construction failure should be retryable"
            : retry.error.message);
    RequireUnknownTokens(retry_bytes, trusted.unknown_tokens,
        "unknown metadata retry should preserve supported mappings");
}

void TestFailureMatrix()
{
    using Failure = std::pair<std::string_view, void (*)()>;
    constexpr std::array<Failure, 10> failures = {{
        {"semantic preflight", TestSemanticPreflightFailure},
        {"resource-limit preflight", TestResourceLimitPreflightFailure},
        {"preservation identity", TestPreservationIdentityMismatch},
        {"missing durable base", TestMissingDurableBase},
        {"temporary output stream", TestTemporaryOutputStreamFailure},
        {"replacement and retry", TestReplacementFailureThenCoreRetry},
        {"external replacement", TestExternalReplacementDuringPublication},
        {"reopen generation mismatch",
            TestReopenGenerationMismatchRequiresRecovery},
        {"checksum/profile", TestUnsupportedChecksumProfile},
        {"unknown preservation construction",
            TestUnknownMetadataConstructionFailure},
    }};
    for (const auto& [name, failure] : failures) {
        try {
            failure();
        } catch (const std::exception& error) {
            throw std::runtime_error(
                std::string(name) + ": " + error.what());
        }
    }
}

}  // namespace

int main()
{
    try {
        // The operation matrix contains the systematic before-replace
        // exception cases; the failure matrix adds preflight, stream,
        // replacement, concurrent-writer, reopen, and preservation failures.
        TestOperationMatrix();
        TestFailureMatrix();
        std::cout <<
            "sample labeling ASDF store publication properties passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
