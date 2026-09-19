#include "sample_labeling_document_test_support.h"
#include "domain/sample_labeling_document_diagnostics.h"
#include "domain/sample_labeling.h"
#include "domain/sample_labeling_document.h"
#include "domain/sample_labeling_source_compatibility.h"
#include "domain/source_collection_manifest.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename T>
concept HasContextFingerprint = requires(T value) {
    value.context_fingerprint;
};

template <typename T>
concept HasAutoAdvance = requires(T value) {
    value.session.auto_advance;
};

template <typename T>
concept HasSkipLabeledOnAdvance = requires(T value) {
    value.session.skip_labeled_on_advance;
};

template <typename T>
concept HasRememberedPosition = requires(T value) {
    value.session.remembered_position;
};

template <typename T>
concept HasPendingSampleIndices = requires(T value) {
    value.persistence.pending_sample_indices;
};

template <typename T>
concept HasMetadataSavePending = requires(T value) {
    value.persistence.metadata_save_pending;
};

template <typename T>
concept HasSaveState = requires(T value) {
    value.persistence.save_state;
};

template <typename T>
concept HasOutputPath = requires(T value) {
    value.persistence.output_path;
};

template <typename T>
concept HasResultFile = requires(T value) {
    value.result_file;
};

template <typename T>
concept HasExpectedDtype = requires(T value) {
    value.expected_dtype;
};

template <typename T>
concept HasValueCount = requires(T value) {
    value.value_count;
};

template <typename T>
concept HasLabelUsageCounts = requires(T value) {
    value.statistics.label_usage_counts;
};

template <typename T>
concept HasLabeledCount = requires(T value) {
    value.statistics.labeled_count;
};

template <typename T>
concept HasAnnotationName = requires(T value) {
    value.name;
};

static_assert(!HasAutoAdvance<spectiary::SampleLabelingDocument>);
static_assert(!HasSkipLabeledOnAdvance<spectiary::SampleLabelingDocument>);
static_assert(!HasRememberedPosition<spectiary::SampleLabelingDocument>);
static_assert(!HasPendingSampleIndices<spectiary::SampleLabelingDocument>);
static_assert(!HasMetadataSavePending<spectiary::SampleLabelingDocument>);
static_assert(!HasSaveState<spectiary::SampleLabelingDocument>);
static_assert(!HasOutputPath<spectiary::SampleLabelingDocument>);
static_assert(!HasResultFile<spectiary::SampleLabelingDocument>);
static_assert(!HasExpectedDtype<spectiary::SampleLabelingDocument>);
static_assert(!HasValueCount<spectiary::SampleLabelingDocument>);
static_assert(!HasLabelUsageCounts<spectiary::SampleLabelingDocument>);
static_assert(!HasLabeledCount<spectiary::SampleLabelingDocument>);
static_assert(!HasContextFingerprint<spectiary::SampleLabelingDocumentSource>);
static_assert(!HasAnnotationName<spectiary::SampleLabelingDocumentAnnotation>);

bool HasIssue(
    const spectiary::SampleLabelingDocumentValidationResult& result,
    spectiary::SampleLabelingDocumentValidationIssueKind kind,
    std::size_t index = spectiary::kSampleLabelingDocumentNoIssueIndex)
{
    return std::any_of(
        result.issues.begin(),
        result.issues.end(),
        [kind, index](const auto& issue) {
            return issue.kind == kind &&
                   (index == spectiary::kSampleLabelingDocumentNoIssueIndex ||
                    issue.index == index);
        });
}

spectiary::SampleLabelingDocument ValidDocument()
{
    spectiary::SourceCollectionContext source_context;
    source_context.identity = {
        .id = "sha256-v1:base-source-identity",
        .source_name = "巡天样本.npy",
        .source_fingerprint = "sha256-v1:base-source-fingerprint",
        .context_fingerprint = "sha256-v1:annotation-sensitive-context",
        .spectrum_count = 3};
    source_context.manifest.sample_names =
        {"星系一", "类星体β", "échelle-γ"};
    spectiary::SampleLabelingTaskCanonicalMetadata canonical_metadata;
    canonical_metadata.created_at =
        *spectiary::ParseCanonicalTimestamp(
            "2026-08-30T16:23:45.123Z");
    canonical_metadata.modified_at =
        *spectiary::ParseCanonicalTimestamp(
            "2026-08-30T17:01:12.456Z");
    canonical_metadata.origin.kind = "manual";
    spectiary::SampleLabelingTask task =
        spectiary::CreateSampleLabelingTask(
            "00000000-0000-4000-8000-000000000001",
            "天体分类",
            3,
            std::move(canonical_metadata));
    Require(
        spectiary::UpsertSampleLabel(task.label_set, {0, "Galaxy", 'g'}),
        "first label fixture should be valid");
    Require(
        spectiary::UpsertSampleLabel(task.label_set, {1, "Quasar", 'q'}),
        "second label fixture should be valid");
    task.values.Complete() = {0, spectiary::kUnlabeledSampleLabelCode, 1};
    task.session.auto_advance = true;
    task.session.skip_labeled_on_advance = true;
    task.session.remembered_position = 2;
    task.persistence.output_path = "ignored-session-output.npy";
    task.persistence.output_format =
        spectiary::SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
    task.persistence.pending_sample_indices = {1};
    task.persistence.metadata_save_pending = true;
    task.persistence.save_state.kind = spectiary::SampleLabelSaveStateKind::Pending;

    return spectiary::test_support::BuildSampleLabelingDocument(
        "npy",
        source_context,
        task);
}

void TestBuildSeparatesCanonicalDocumentFromTaskSessionState()
{
    const spectiary::SampleLabelingDocument document = ValidDocument();

    Require(
        document.format_kind == spectiary::kSampleLabelingDocumentFormatKind &&
            document.schema_version == spectiary::kSampleLabelingDocumentSchemaVersion,
        "builder should establish the canonical document identity");
    Require(
        document.source.base_identity == "sha256-v1:base-source-identity" &&
            document.source.base_identity != "sha256-v1:annotation-sensitive-context",
        "canonical source identity must use the base identity, not context_fingerprint");
    Require(
        document.source.roster.identity_kind ==
                spectiary::kSampleLabelingDocumentExplicitNamesRoster &&
            document.source.roster.sample_names[0] == "星系一" &&
            document.source.roster.sample_names[1] == "类星体β" &&
            document.source.roster.sample_names[2] == "échelle-γ" &&
            document.annotation.alignment.mode ==
                spectiary::kSampleLabelingDocumentByIndexAlignmentMode &&
            document.annotation.alignment.target ==
                spectiary::
                    kSampleLabelingDocumentSampleRosterAlignmentTarget &&
            document.annotation.values ==
                std::vector<std::int32_t>({0, -1, 1}),
        "explicit-names values[i] should align with sample_roster.names[i]");
    Require(
        document.annotation.values == std::vector<std::int32_t>({0, -1, 1}),
        "builder should copy only canonical annotation values");
    Require(
        document.labeling.id ==
                "00000000-0000-4000-8000-000000000001" &&
            document.labeling.name == "天体分类" &&
            document.labeling.labels.size() == 2,
        "builder should copy task identity, name, and label definitions");
    Require(
        spectiary::diagnostics::ValidateSampleLabelingDocument(document).valid(),
        "builder output should satisfy canonical semantic invariants");
}

void TestCanonicalSourceDescriptorOwnsPreparedSourceFacts()
{
    spectiary::SpectrumSnapshot snapshot;
    snapshot.source.metadata = {
        {"source_type", "npy_matrix", "domain"},
        {"format", "npy", "domain"},
    };
    spectiary::SourceCollectionContext context;
    context.identity = {
        .id = "sha256-v1:descriptor-source",
        .source_name = "descriptor.npy",
        .source_fingerprint =
            "sha256-v1:descriptor-fingerprint",
        .context_fingerprint =
            "sha256-v1:ignored-context",
        .spectrum_count = 2,
    };
    context.manifest.sample_names = {
        "sample-a",
        "sample-b",
    };

    spectiary::SampleLabelingCanonicalSourceDescriptor
        descriptor =
            spectiary::
                BuildSampleLabelingCanonicalSourceDescriptor(
                    snapshot,
                    context);
    context.identity.id = "changed-after-build";
    context.manifest.sample_names[0] =
        "changed-after-build";
    Require(
        descriptor.base_identity ==
                "sha256-v1:descriptor-source" &&
            descriptor.source_kind == "npy" &&
            descriptor.source_name == "descriptor.npy" &&
            descriptor.source_fingerprint ==
                "sha256-v1:descriptor-fingerprint" &&
            descriptor.sample_count == 2 &&
            descriptor.sample_names ==
                std::vector<std::string>({
                    "sample-a",
                    "sample-b",
                }),
        "canonical descriptor should own source preparation facts without retaining annotation-sensitive context");

    snapshot.source.metadata = {
        {"source_type", "folder_collection", "domain"},
        {"format", "fits", "domain"},
    };
    descriptor = spectiary::
        BuildSampleLabelingCanonicalSourceDescriptor(
            snapshot,
            spectiary::SourceCollectionIdentity{
                .id = "folder-source",
                .source_name = "folder",
                .source_fingerprint = "folder-fingerprint",
                .spectrum_count = 1,
            },
            spectiary::SourceCollectionManifest{});
    Require(
        descriptor.source_kind == "folder",
        "folder collection identity should take precedence over the current member file format");
}

void TestSourceIndexRosterIsExplicitWithoutMaterializedIndexes()
{
    spectiary::SourceCollectionContext source_context;
    source_context.identity = {
        .id = "sha256-v1:index-source",
        .source_name = "matrix.npy",
        .source_fingerprint = "sha256-v1:index-fingerprint",
        .context_fingerprint = "sha256-v1:index-context",
        .spectrum_count = 2};
    spectiary::SampleLabelingTask task =
        spectiary::CreateSampleLabelingTask(
            "00000000-0000-4000-8000-000000000002",
            "Index labels",
            2);
    Require(
        spectiary::UpsertSampleLabel(task.label_set, {0, "Target", 't'}),
        "source-index label fixture should be valid");
    task.values.Complete() = {-1, 0};

    const spectiary::SampleLabelingDocument document =
        spectiary::test_support::BuildSampleLabelingDocument("npy", source_context, task);
    Require(
        document.source.roster.identity_kind ==
                spectiary::kSampleLabelingDocumentSourceIndexRoster &&
            document.source.roster.sample_names.empty() &&
            document.annotation.alignment.mode ==
                spectiary::kSampleLabelingDocumentByIndexAlignmentMode &&
            document.annotation.alignment.target ==
                spectiary::
                    kSampleLabelingDocumentSampleRosterAlignmentTarget &&
            document.annotation.values[0] == -1 &&
            document.annotation.values[1] == 0,
        "source-index values[i] should align with source index i without a redundant index array");
    Require(
        spectiary::diagnostics::ValidateSampleLabelingDocument(document).valid(),
        "source-index roster should be semantically valid");
}

void TestInvalidSourceNamesFallBackToSourceIndexRoster()
{
    spectiary::SourceCollectionContext source_context;
    source_context.identity = {
        .id = "sha256-v1:invalid-roster-source",
        .source_name = "matrix.npy",
        .source_fingerprint = "sha256-v1:invalid-roster-fingerprint",
        .spectrum_count = 3};
    source_context.manifest.sample_names = {
        "sample-a",
        "   ",
        "sample-a",
    };
    spectiary::SampleLabelingTask task =
        spectiary::CreateSampleLabelingTask(
            "00000000-0000-4000-8000-000000000003",
            "Index fallback",
            3);

    const spectiary::SampleLabelingDocument from_context =
        spectiary::test_support::BuildSampleLabelingDocument(
            "npy",
            source_context,
            task);
    Require(
        from_context.source.roster.identity_kind ==
                spectiary::kSampleLabelingDocumentSourceIndexRoster &&
            from_context.source.roster.sample_names.empty() &&
            spectiary::diagnostics::ValidateSampleLabelingDocument(from_context)
                .valid(),
        "blank or duplicate manifest names must fall back to source-index identity instead of making canonical Save As invalid");

    spectiary::SpectrumSnapshot snapshot;
    snapshot.source.metadata = {
        {"format", "npy", "domain"},
    };
    const spectiary::SampleLabelingCanonicalSourceDescriptor descriptor =
        spectiary::BuildSampleLabelingCanonicalSourceDescriptor(
            snapshot,
            source_context);
    const spectiary::SampleLabelingDocument from_descriptor =
        spectiary::test_support::BuildSampleLabelingDocument(
            descriptor,
            task.Content().value());
    Require(
        descriptor.sample_names.empty() &&
            from_descriptor.source.roster.identity_kind ==
                spectiary::kSampleLabelingDocumentSourceIndexRoster &&
            spectiary::diagnostics::ValidateSampleLabelingDocument(from_descriptor)
                .valid(),
        "the durable source descriptor must preserve the same source-index fallback used by the direct builder");
}

void TestValidatorEnforcesSampleAlignmentAndRosterShape()
{
    spectiary::SampleLabelingDocument document = ValidDocument();
    document.source.sample_count = 4;
    const auto count_result = spectiary::diagnostics::ValidateSampleLabelingDocument(document);
    Require(
        HasIssue(
            count_result,
            spectiary::SampleLabelingDocumentValidationIssueKind::RosterSampleCountMismatch) &&
            HasIssue(
                count_result,
                spectiary::SampleLabelingDocumentValidationIssueKind::AnnotationSampleCountMismatch),
        "validator should align sample_count with both roster and values shape");

    document = ValidDocument();
    document.source.roster.sample_names[2] = document.source.roster.sample_names[0];
    const auto duplicate_result = spectiary::diagnostics::ValidateSampleLabelingDocument(document);
    Require(
        HasIssue(
            duplicate_result,
            spectiary::SampleLabelingDocumentValidationIssueKind::DuplicateSampleName,
            2),
        "explicit roster identities should be unique");

    document = ValidDocument();
    document.source.roster.identity_kind =
        std::string{spectiary::kSampleLabelingDocumentSourceIndexRoster};
    const auto index_result = spectiary::diagnostics::ValidateSampleLabelingDocument(document);
    Require(
        HasIssue(
            index_result,
            spectiary::SampleLabelingDocumentValidationIssueKind::SourceIndexRosterHasNames),
        "source-index rosters must not carry materialized names");

    document = ValidDocument();
    document.annotation.alignment.mode = "by_key";
    Require(
        HasIssue(
            spectiary::diagnostics::ValidateSampleLabelingDocument(document),
            spectiary::SampleLabelingDocumentValidationIssueKind::
                UnsupportedAnnotationAlignmentMode),
        "schema 2.0 must reject alignment modes other than by_index");

    document = ValidDocument();
    document.annotation.alignment.target = "source_collection";
    Require(
        HasIssue(
            spectiary::diagnostics::ValidateSampleLabelingDocument(document),
            spectiary::SampleLabelingDocumentValidationIssueKind::
                UnsupportedAnnotationAlignmentTarget),
        "schema 2.0 must reject alignment targets other than sample_roster");
}

void TestValidatorEnforcesLabelAndUnlabeledInvariants()
{
    spectiary::SampleLabelingDocument document = ValidDocument();
    document.labeling.labels[0].code = -1;
    Require(
        HasIssue(
            spectiary::diagnostics::ValidateSampleLabelingDocument(document),
            spectiary::SampleLabelingDocumentValidationIssueKind::ReservedLabelCode,
            0),
        "-1 must remain reserved for unlabeled values");

    document = ValidDocument();
    document.labeling.labels[1].code = document.labeling.labels[0].code;
    Require(
        HasIssue(
            spectiary::diagnostics::ValidateSampleLabelingDocument(document),
            spectiary::SampleLabelingDocumentValidationIssueKind::DuplicateLabelCode,
            1),
        "label codes should be unique");

    document = ValidDocument();
    document.labeling.labels[1].shortcut = "G";
    Require(
        HasIssue(
            spectiary::diagnostics::ValidateSampleLabelingDocument(document),
            spectiary::SampleLabelingDocumentValidationIssueKind::DuplicateLabelShortcut,
            1),
        "shortcut uniqueness should follow the task domain's normalized semantics");

    document = ValidDocument();
    document.annotation.values[2] = 42;
    Require(
        HasIssue(
            spectiary::diagnostics::ValidateSampleLabelingDocument(document),
            spectiary::SampleLabelingDocumentValidationIssueKind::UndefinedAnnotationValue,
            2),
        "every labeled value should resolve to a label definition");

    document = ValidDocument();
    document.annotation.missing.value = 0;
    Require(
        HasIssue(
            spectiary::diagnostics::ValidateSampleLabelingDocument(document),
            spectiary::SampleLabelingDocumentValidationIssueKind::InvalidUnlabeledValue),
        "canonical missing semantics should remain fixed to -1");
}

void TestValidatorRejectsUnsupportedDocumentSemantics()
{
    spectiary::SampleLabelingDocument document = ValidDocument();
    document.format_kind = "spectiary.session";
    document.schema_version = "1.0.0";
    document.annotation.kind = "continuous_float";
    document.annotation.missing.semantic = "nan";
    document.source.base_identity.clear();
    document.labeling.id.clear();

    const auto result = spectiary::diagnostics::ValidateSampleLabelingDocument(document);
    const std::unordered_set<spectiary::SampleLabelingDocumentValidationIssueKind> expected = {
        spectiary::SampleLabelingDocumentValidationIssueKind::UnsupportedFormatKind,
        spectiary::SampleLabelingDocumentValidationIssueKind::UnsupportedSchemaVersion,
        spectiary::SampleLabelingDocumentValidationIssueKind::MissingSourceBaseIdentity,
        spectiary::SampleLabelingDocumentValidationIssueKind::UnsupportedAnnotationKind,
        spectiary::SampleLabelingDocumentValidationIssueKind::UnsupportedMissingSemantic,
        spectiary::SampleLabelingDocumentValidationIssueKind::MissingTaskId};
    for (const auto issue : expected) {
        Require(
            HasIssue(result, issue),
            "validator should reject unsupported or missing canonical semantics");
    }
}

void TestValidatorEnforcesBuildSourceIdentityInvariant()
{
    constexpr std::string_view revision =
        "0123456789abcdef0123456789abcdef01234567";
    spectiary::SampleLabelingDocument document = ValidDocument();
    document.build_source = {
        .source_mode = "head",
        .source_revision = std::string{revision}};
    Require(
        spectiary::diagnostics::ValidateSampleLabelingDocument(document).valid(),
        "head build source should require and accept one full lowercase revision");

    document.build_source = {
        .source_mode = "working_tree",
        .source_revision = std::nullopt};
    Require(
        spectiary::diagnostics::ValidateSampleLabelingDocument(document).valid(),
        "working-tree build source should be valid without a revision");

    document.build_source.source_mode = "archive";
    Require(
        HasIssue(
            spectiary::diagnostics::ValidateSampleLabelingDocument(document),
            spectiary::SampleLabelingDocumentValidationIssueKind::
                UnsupportedBuildSourceMode),
        "unknown build source modes must be rejected");

    document.build_source = {
        .source_mode = "head",
        .source_revision = std::nullopt};
    Require(
        HasIssue(
            spectiary::diagnostics::ValidateSampleLabelingDocument(document),
            spectiary::SampleLabelingDocumentValidationIssueKind::
                MissingBuildSourceRevision),
        "head build source must carry a revision");

    document.build_source.source_revision = "0123456789abcdef";
    Require(
        HasIssue(
            spectiary::diagnostics::ValidateSampleLabelingDocument(document),
            spectiary::SampleLabelingDocumentValidationIssueKind::
                InvalidBuildSourceRevision),
        "head build source revision must be the full lowercase object id");

    document.build_source = {
        .source_mode = "working_tree",
        .source_revision = std::string{revision}};
    Require(
        HasIssue(
            spectiary::diagnostics::ValidateSampleLabelingDocument(document),
            spectiary::SampleLabelingDocumentValidationIssueKind::
                UnexpectedBuildSourceRevision),
        "working-tree build source must not carry a revision");
}

void TestValidatorEnforcesSchemaTwoIdentityAndMetadata()
{
    spectiary::SampleLabelingDocument document = ValidDocument();
    document.labeling.id = "00000000-0000-4000-8000-00000000000A";
    Require(
        HasIssue(
            spectiary::diagnostics::ValidateSampleLabelingDocument(document),
            spectiary::SampleLabelingDocumentValidationIssueKind::
                InvalidTaskId),
        "schema 2 task ids must use the lowercase canonical UUID v4 form");

    document = ValidDocument();
    document.labeling.canonical_metadata.modified_at =
        *spectiary::ParseCanonicalTimestamp(
            "2026-08-30T16:00:00.000Z");
    Require(
        HasIssue(
            spectiary::diagnostics::ValidateSampleLabelingDocument(document),
            spectiary::SampleLabelingDocumentValidationIssueKind::
                InvalidCanonicalTimestampOrder),
        "created_at must not be later than modified_at");

    document = ValidDocument();
    document.labeling.canonical_metadata.origin.annotation =
        spectiary::SampleLabelingAnnotationOrigin{
            .name = "labels.csv",
            .format = "csv"};
    Require(
        HasIssue(
            spectiary::diagnostics::ValidateSampleLabelingDocument(document),
            spectiary::SampleLabelingDocumentValidationIssueKind::
                InvalidOriginAnnotation),
        "manual origin must not carry annotation promotion metadata");

    document = ValidDocument();
    document.labeling.canonical_metadata.origin.kind =
        "annotation_promotion";
    Require(
        HasIssue(
            spectiary::diagnostics::ValidateSampleLabelingDocument(document),
            spectiary::SampleLabelingDocumentValidationIssueKind::
                InvalidOriginAnnotation),
        "annotation promotion must carry portable annotation provenance");

    document = ValidDocument();
    document.labeling.canonical_metadata.origin.kind = "Bad-Origin";
    Require(
        HasIssue(
            spectiary::diagnostics::ValidateSampleLabelingDocument(document),
            spectiary::SampleLabelingDocumentValidationIssueKind::
                InvalidOriginKind),
        "origin kinds must remain lowercase extensible tokens");

    document = ValidDocument();
    document.labeling.canonical_metadata.origin.kind =
        "annotation_promotion";
    document.labeling.canonical_metadata.origin.annotation =
        spectiary::SampleLabelingAnnotationOrigin{
            .name = "\xE3\x80\x80",
            .format = "json",
            .fingerprint =
                "sha256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"};
    Require(
        HasIssue(
            spectiary::diagnostics::ValidateSampleLabelingDocument(document),
            spectiary::SampleLabelingDocumentValidationIssueKind::
                InvalidOriginAnnotation),
        "promotion provenance must use a non-whitespace UTF-8 name, csv/npy format, and lowercase SHA-256");

    document = ValidDocument();
    document.labeling.canonical_metadata.description =
        std::string{"\xC3\x28", 2};
    Require(
        HasIssue(
            spectiary::diagnostics::ValidateSampleLabelingDocument(document),
            spectiary::SampleLabelingDocumentValidationIssueKind::
                InvalidDescription),
        "description must contain valid UTF-8");

    document = ValidDocument();
    document.labeling.canonical_metadata.authors = {
        {.name = "Alice", .identifier = "   "},
        {.name = "Bob", .email = " \t "},
        {.name = "Carol",
            .email = std::string(1, static_cast<char>(0xc3))},
        {.name = "\xE3\x80\x80"},
    };
    const auto invalid_authors =
        spectiary::diagnostics::ValidateSampleLabelingDocument(document);
    Require(
        HasIssue(
            invalid_authors,
            spectiary::SampleLabelingDocumentValidationIssueKind::
                InvalidAuthor,
            0) &&
            HasIssue(
                invalid_authors,
                spectiary::SampleLabelingDocumentValidationIssueKind::
                    InvalidAuthor,
                1) &&
            HasIssue(
                invalid_authors,
                spectiary::SampleLabelingDocumentValidationIssueKind::
                    InvalidAuthor,
                2) &&
            HasIssue(
                invalid_authors,
                spectiary::SampleLabelingDocumentValidationIssueKind::
                    InvalidAuthor,
                3),
        "author name, optional identifier, and optional email must contain non-whitespace UTF-8 text");

    document = ValidDocument();
    document.labeling.canonical_metadata.origin.kind =
        "annotation_promotion";
    document.labeling.canonical_metadata.origin.annotation =
        spectiary::SampleLabelingAnnotationOrigin{
            .name = "初始标签.csv",
            .format = "csv",
            .fingerprint = "sha256:" + std::string(64, 'a')};
    document.labeling.canonical_metadata.description =
        "中日韩描述 🧪";
    document.labeling.canonical_metadata.authors = {
        {.name = "Alice",
            .identifier = "https://example.test/alice",
            .email = "Mixed.Case@Example.TEST"},
        {.name = "山田太郎", .email = "連絡先"},
    };
    Require(
        spectiary::diagnostics::ValidateSampleLabelingDocument(document).valid(),
        "schema 2 canonical metadata should accept valid Unicode declarations without normalization");
}

void TestValidatorRequiresPortableAnnotationOriginBasename()
{
    spectiary::SampleLabelingDocument promoted = ValidDocument();
    promoted.labeling.canonical_metadata.origin.kind =
        "annotation_promotion";
    promoted.labeling.canonical_metadata.origin.annotation =
        spectiary::SampleLabelingAnnotationOrigin{
            .name = "初始标签.csv",
            .format = "csv",
            .fingerprint = "sha256:" + std::string(64, 'a')};
    Require(
        spectiary::diagnostics::ValidateSampleLabelingDocument(promoted).valid(),
        "shared document validation should accept a Unicode annotation provenance basename");

    for (const std::string_view nonportable_name : {
             "/home/user/labels.csv",
             "C:\\labels.npy",
             "C:/labels.csv",
             "\\\\server\\share\\labels.csv",
             "folder/labels.csv",
             "folder\\labels.npy",
             "C:labels.csv",
             ".",
             "..",
         }) {
        promoted.labeling.canonical_metadata.origin.annotation->name =
            nonportable_name;
        Require(
            HasIssue(
                spectiary::diagnostics::ValidateSampleLabelingDocument(promoted),
                spectiary::SampleLabelingDocumentValidationIssueKind::
                    InvalidOriginAnnotation),
            "shared document validation must reject rooted, directory-bearing, and drive-relative annotation provenance names");
    }
}

void TestCanonicalProjectionRejectsUndefinedPendingCodes()
{
    const spectiary::SampleLabelingDocument document =
        ValidDocument();
    spectiary::SampleLabelingTask local_state =
        spectiary::CreateSampleLabelingTask(
            document.labeling.id,
            "local state",
            document.annotation.values.size());
    local_state.persistence.output_path = "canonical-owner.asdf";
    local_state.persistence.output_format =
        spectiary::SampleLabelingOutputArtifactFormat::
            CanonicalAsdf;
    local_state.canonical_metadata =
        document.labeling.canonical_metadata;
    local_state.values.Complete()[1] = 42;
    local_state.persistence.pending_sample_indices.insert(1);

    Require(
        !spectiary::ProjectSampleLabelingDocumentTask(
            document,
            local_state),
        "a pending code removed from canonical labels must fail projection instead of creating an undefined task value");

    local_state.persistence.metadata_save_pending = true;
    local_state.task_name = "Locally edited metadata";
    local_state.label_set.labels = {
        {0, "Galaxy", 'g'},
        {1, "Quasar", 'q'},
        {42, "Pending class", 'p'},
    };
    const std::optional<spectiary::SampleLabelingTask>
        metadata_projection =
            spectiary::ProjectSampleLabelingDocumentTask(
                document,
                local_state);
    Require(
        metadata_projection &&
            metadata_projection->values.Complete() ==
                std::vector<int>({0, 42, 1}),
        "pending values should be validated after a compatible local metadata overlay is applied");
}

void TestCanonicalProjectionUsesOneMetadataAuthority()
{
    const spectiary::SampleLabelingDocument document =
        ValidDocument();
    spectiary::SampleLabelingTask local_state =
        spectiary::CreateSampleLabelingTask(
            document.labeling.id,
            "stale cache name",
            document.annotation.values.size(),
            document.labeling.canonical_metadata);
    local_state.persistence.output_path = "canonical-owner.asdf";
    local_state.persistence.output_format =
        spectiary::SampleLabelingOutputArtifactFormat::CanonicalAsdf;

    local_state.canonical_metadata.description =
        "stale cache description";
    const auto clean_projection =
        spectiary::ProjectSampleLabelingDocumentTask(
            document,
            local_state);
    Require(
        clean_projection &&
            clean_projection->task_name ==
                document.labeling.name &&
            clean_projection->canonical_metadata ==
                document.labeling.canonical_metadata,
        "without a pending overlay, the durable canonical document must be the sole metadata authority");

    local_state.persistence.pending_sample_indices.insert(1);
    local_state.values.Complete()[1] = 0;
    local_state.canonical_metadata.created_at =
        *spectiary::ParseCanonicalTimestamp(
            "2026-08-30T16:23:45.124Z");
    Require(
        !spectiary::ProjectSampleLabelingDocumentTask(
            document,
            local_state),
        "pending cache edits must not merge with a different immutable creation timestamp");

    local_state.canonical_metadata =
        document.labeling.canonical_metadata;
    local_state.canonical_metadata.origin.kind = "future_origin";
    Require(
        !spectiary::ProjectSampleLabelingDocumentTask(
            document,
            local_state),
        "pending cache edits must not merge with a different immutable origin");

    local_state.canonical_metadata =
        document.labeling.canonical_metadata;
    local_state.canonical_metadata.modified_at =
        *spectiary::ParseCanonicalTimestamp(
            "2026-08-30T16:30:00.000Z");
    Require(
        !spectiary::ProjectSampleLabelingDocumentTask(
            document,
            local_state),
        "an older pending cache generation must be treated as stale rather than overlaid on newer disk metadata");
}

void TestSparseValuesCannotExposeCanonicalContent()
{
    spectiary::SampleLabelingTask task;
    task.values.Complete() = {7, -1, 7};
    Require(task.Content().has_value(), "complete values must expose content");
    task.values.MakeSparse({1});
    Require(!task.Content() && !task.values.CompleteIfAvailable(),
        "sparse overlays must not expose placeholder canonical content");
    Require(task.values.SampleCount() == 3 &&
            task.values.Sparse()->pending_values.size() == 1 &&
            task.values.PendingValue(1) == -1,
        "sparse overlays must retain explicit clearing edits and source size");
    bool rejected_unknown_row = false;
    try {
        (void)task.values.PendingValue(0);
    } catch (const std::out_of_range&) {
        rejected_unknown_row = true;
    }
    Require(rejected_unknown_row,
        "an absent overlay row must not be interpreted as an unlabeled sample");
}

void TestFailFastValidatorBoundsDiagnostics()
{
    spectiary::SampleLabelingDocument document = ValidDocument();
    document.source.sample_count = 25'000;
    document.source.roster.identity_kind =
        std::string{spectiary::kSampleLabelingDocumentSourceIndexRoster};
    document.source.roster.sample_names.clear();
    document.annotation.values.assign(document.source.sample_count, 42);

    const auto result =
        spectiary::ValidateSampleLabelingDocumentFailFast(document);
    Require(
        result.issues.size() == 1 &&
            result.issues.front().kind ==
                spectiary::SampleLabelingDocumentValidationIssueKind::
                    UndefinedAnnotationValue,
        "fail-fast validation must retain only the first semantic diagnostic");
}

}  // namespace

int main()
{
    try {
        TestBuildSeparatesCanonicalDocumentFromTaskSessionState();
        TestCanonicalSourceDescriptorOwnsPreparedSourceFacts();
        TestSourceIndexRosterIsExplicitWithoutMaterializedIndexes();
        TestInvalidSourceNamesFallBackToSourceIndexRoster();
        TestValidatorEnforcesSampleAlignmentAndRosterShape();
        TestValidatorEnforcesLabelAndUnlabeledInvariants();
        TestValidatorRejectsUnsupportedDocumentSemantics();
        TestValidatorEnforcesBuildSourceIdentityInvariant();
        TestValidatorEnforcesSchemaTwoIdentityAndMetadata();
        TestValidatorRequiresPortableAnnotationOriginBasename();
        TestCanonicalProjectionRejectsUndefinedPendingCodes();
        TestCanonicalProjectionUsesOneMetadataAuthority();
        TestFailFastValidatorBoundsDiagnostics();
        TestSparseValuesCannotExposeCanonicalContent();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sample labeling document test failure: "
                  << error.what() << '\n';
        return 1;
    }
}
