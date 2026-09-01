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
    value.auto_advance;
};

template <typename T>
concept HasSkipLabeledOnAdvance = requires(T value) {
    value.skip_labeled_on_advance;
};

template <typename T>
concept HasRememberedPosition = requires(T value) {
    value.remembered_position;
};

template <typename T>
concept HasPendingSampleIndices = requires(T value) {
    value.pending_sample_indices;
};

template <typename T>
concept HasMetadataSavePending = requires(T value) {
    value.metadata_save_pending;
};

template <typename T>
concept HasSaveState = requires(T value) {
    value.save_state;
};

template <typename T>
concept HasOutputPath = requires(T value) {
    value.output_path;
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
    value.label_usage_counts;
};

template <typename T>
concept HasLabeledCount = requires(T value) {
    value.labeled_count;
};

template <typename T>
concept HasAnnotationName = requires(T value) {
    value.name;
};

static_assert(!HasAutoAdvance<specforge::SampleLabelingDocument>);
static_assert(!HasSkipLabeledOnAdvance<specforge::SampleLabelingDocument>);
static_assert(!HasRememberedPosition<specforge::SampleLabelingDocument>);
static_assert(!HasPendingSampleIndices<specforge::SampleLabelingDocument>);
static_assert(!HasMetadataSavePending<specforge::SampleLabelingDocument>);
static_assert(!HasSaveState<specforge::SampleLabelingDocument>);
static_assert(!HasOutputPath<specforge::SampleLabelingDocument>);
static_assert(!HasResultFile<specforge::SampleLabelingDocument>);
static_assert(!HasExpectedDtype<specforge::SampleLabelingDocument>);
static_assert(!HasValueCount<specforge::SampleLabelingDocument>);
static_assert(!HasLabelUsageCounts<specforge::SampleLabelingDocument>);
static_assert(!HasLabeledCount<specforge::SampleLabelingDocument>);
static_assert(!HasContextFingerprint<specforge::SampleLabelingDocumentSource>);
static_assert(!HasAnnotationName<specforge::SampleLabelingDocumentAnnotation>);

bool HasIssue(
    const specforge::SampleLabelingDocumentValidationResult& result,
    specforge::SampleLabelingDocumentValidationIssueKind kind,
    std::size_t index = specforge::kSampleLabelingDocumentNoIssueIndex)
{
    return std::any_of(
        result.issues.begin(),
        result.issues.end(),
        [kind, index](const auto& issue) {
            return issue.kind == kind &&
                   (index == specforge::kSampleLabelingDocumentNoIssueIndex ||
                    issue.index == index);
        });
}

specforge::SampleLabelingDocument ValidDocument()
{
    specforge::SourceCollectionContext source_context;
    source_context.identity = {
        .id = "sha256-v1:base-source-identity",
        .source_name = "巡天样本.npy",
        .source_fingerprint = "sha256-v1:base-source-fingerprint",
        .context_fingerprint = "sha256-v1:annotation-sensitive-context",
        .spectrum_count = 3};
    source_context.manifest.sample_names =
        {"星系一", "类星体β", "échelle-γ"};
    specforge::SampleLabelingTaskCanonicalMetadata canonical_metadata;
    canonical_metadata.created_at =
        *specforge::ParseCanonicalTimestamp(
            "2026-08-30T16:23:45.123Z");
    canonical_metadata.modified_at =
        *specforge::ParseCanonicalTimestamp(
            "2026-08-30T17:01:12.456Z");
    canonical_metadata.origin.kind = "manual";
    specforge::SampleLabelingTask task =
        specforge::CreateSampleLabelingTask(
            "00000000-0000-4000-8000-000000000001",
            "天体分类",
            3,
            std::move(canonical_metadata));
    Require(
        specforge::UpsertSampleLabel(task.label_set, {0, "Galaxy", 'g'}),
        "first label fixture should be valid");
    Require(
        specforge::UpsertSampleLabel(task.label_set, {1, "Quasar", 'q'}),
        "second label fixture should be valid");
    task.values = {0, specforge::kUnlabeledSampleLabelCode, 1};
    task.auto_advance = true;
    task.skip_labeled_on_advance = true;
    task.remembered_position = 2;
    task.output_path = "ignored-session-output.npy";
    task.output_format =
        specforge::SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
    task.pending_sample_indices = {1};
    task.metadata_save_pending = true;
    task.save_state.kind = specforge::SampleLabelSaveStateKind::Pending;

    return specforge::BuildSampleLabelingDocument(
        "npy",
        source_context,
        task);
}

void TestBuildSeparatesCanonicalDocumentFromTaskSessionState()
{
    const specforge::SampleLabelingDocument document = ValidDocument();

    Require(
        document.format_kind == specforge::kSampleLabelingDocumentFormatKind &&
            document.schema_version == specforge::kSampleLabelingDocumentSchemaVersion,
        "builder should establish the canonical document identity");
    Require(
        document.source.base_identity == "sha256-v1:base-source-identity" &&
            document.source.base_identity != "sha256-v1:annotation-sensitive-context",
        "canonical source identity must use the base identity, not context_fingerprint");
    Require(
        document.source.roster.identity_kind ==
                specforge::kSampleLabelingDocumentExplicitNamesRoster &&
            document.source.roster.sample_names[1] == "类星体β",
        "builder should preserve the canonical Unicode source roster");
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
        specforge::ValidateSampleLabelingDocument(document).valid(),
        "builder output should satisfy canonical semantic invariants");
}

void TestCanonicalSourceDescriptorOwnsPreparedSourceFacts()
{
    specforge::SpectrumSnapshot snapshot;
    snapshot.source.metadata = {
        {"source_type", "npy_matrix", "domain"},
        {"format", "npy", "domain"},
    };
    specforge::SourceCollectionContext context;
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

    specforge::SampleLabelingCanonicalSourceDescriptor
        descriptor =
            specforge::
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
    descriptor = specforge::
        BuildSampleLabelingCanonicalSourceDescriptor(
            snapshot,
            specforge::SourceCollectionIdentity{
                .id = "folder-source",
                .source_name = "folder",
                .source_fingerprint = "folder-fingerprint",
                .spectrum_count = 1,
            },
            specforge::SourceCollectionManifest{});
    Require(
        descriptor.source_kind == "folder",
        "folder collection identity should take precedence over the current member file format");
}

void TestSourceIndexRosterIsExplicitWithoutMaterializedIndexes()
{
    specforge::SourceCollectionContext source_context;
    source_context.identity = {
        .id = "sha256-v1:index-source",
        .source_name = "matrix.npy",
        .source_fingerprint = "sha256-v1:index-fingerprint",
        .context_fingerprint = "sha256-v1:index-context",
        .spectrum_count = 2};
    specforge::SampleLabelingTask task =
        specforge::CreateSampleLabelingTask(
            "00000000-0000-4000-8000-000000000002",
            "Index labels",
            2);
    Require(
        specforge::UpsertSampleLabel(task.label_set, {0, "Target", 't'}),
        "source-index label fixture should be valid");
    task.values = {-1, 0};

    const specforge::SampleLabelingDocument document =
        specforge::BuildSampleLabelingDocument("npy", source_context, task);
    Require(
        document.source.roster.identity_kind ==
                specforge::kSampleLabelingDocumentSourceIndexRoster &&
            document.source.roster.sample_names.empty(),
        "unnamed sources should declare source-index identity without a redundant index array");
    Require(
        specforge::ValidateSampleLabelingDocument(document).valid(),
        "source-index roster should be semantically valid");
}

void TestInvalidSourceNamesFallBackToSourceIndexRoster()
{
    specforge::SourceCollectionContext source_context;
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
    specforge::SampleLabelingTask task =
        specforge::CreateSampleLabelingTask(
            "00000000-0000-4000-8000-000000000003",
            "Index fallback",
            3);

    const specforge::SampleLabelingDocument from_context =
        specforge::BuildSampleLabelingDocument(
            "npy",
            source_context,
            task);
    Require(
        from_context.source.roster.identity_kind ==
                specforge::kSampleLabelingDocumentSourceIndexRoster &&
            from_context.source.roster.sample_names.empty() &&
            specforge::ValidateSampleLabelingDocument(from_context)
                .valid(),
        "blank or duplicate manifest names must fall back to source-index identity instead of making canonical Save As invalid");

    specforge::SpectrumSnapshot snapshot;
    snapshot.source.metadata = {
        {"format", "npy", "domain"},
    };
    const specforge::SampleLabelingCanonicalSourceDescriptor descriptor =
        specforge::BuildSampleLabelingCanonicalSourceDescriptor(
            snapshot,
            source_context);
    const specforge::SampleLabelingDocument from_descriptor =
        specforge::BuildSampleLabelingDocument(
            descriptor,
            task);
    Require(
        descriptor.sample_names.empty() &&
            from_descriptor.source.roster.identity_kind ==
                specforge::kSampleLabelingDocumentSourceIndexRoster &&
            specforge::ValidateSampleLabelingDocument(from_descriptor)
                .valid(),
        "the durable source descriptor must preserve the same source-index fallback used by the direct builder");
}

void TestValidatorEnforcesSampleAlignmentAndRosterShape()
{
    specforge::SampleLabelingDocument document = ValidDocument();
    document.source.sample_count = 4;
    const auto count_result = specforge::ValidateSampleLabelingDocument(document);
    Require(
        HasIssue(
            count_result,
            specforge::SampleLabelingDocumentValidationIssueKind::RosterSampleCountMismatch) &&
            HasIssue(
                count_result,
                specforge::SampleLabelingDocumentValidationIssueKind::AnnotationSampleCountMismatch),
        "validator should align sample_count with both roster and values shape");

    document = ValidDocument();
    document.source.roster.sample_names[2] = document.source.roster.sample_names[0];
    const auto duplicate_result = specforge::ValidateSampleLabelingDocument(document);
    Require(
        HasIssue(
            duplicate_result,
            specforge::SampleLabelingDocumentValidationIssueKind::DuplicateSampleName,
            2),
        "explicit roster identities should be unique");

    document = ValidDocument();
    document.source.roster.identity_kind =
        std::string{specforge::kSampleLabelingDocumentSourceIndexRoster};
    const auto index_result = specforge::ValidateSampleLabelingDocument(document);
    Require(
        HasIssue(
            index_result,
            specforge::SampleLabelingDocumentValidationIssueKind::SourceIndexRosterHasNames),
        "source-index rosters must not carry materialized names");
}

void TestValidatorEnforcesLabelAndUnlabeledInvariants()
{
    specforge::SampleLabelingDocument document = ValidDocument();
    document.labeling.labels[0].code = -1;
    Require(
        HasIssue(
            specforge::ValidateSampleLabelingDocument(document),
            specforge::SampleLabelingDocumentValidationIssueKind::ReservedLabelCode,
            0),
        "-1 must remain reserved for unlabeled values");

    document = ValidDocument();
    document.labeling.labels[1].code = document.labeling.labels[0].code;
    Require(
        HasIssue(
            specforge::ValidateSampleLabelingDocument(document),
            specforge::SampleLabelingDocumentValidationIssueKind::DuplicateLabelCode,
            1),
        "label codes should be unique");

    document = ValidDocument();
    document.labeling.labels[1].shortcut = "G";
    Require(
        HasIssue(
            specforge::ValidateSampleLabelingDocument(document),
            specforge::SampleLabelingDocumentValidationIssueKind::DuplicateLabelShortcut,
            1),
        "shortcut uniqueness should follow the task domain's normalized semantics");

    document = ValidDocument();
    document.annotation.values[2] = 42;
    Require(
        HasIssue(
            specforge::ValidateSampleLabelingDocument(document),
            specforge::SampleLabelingDocumentValidationIssueKind::UndefinedAnnotationValue,
            2),
        "every labeled value should resolve to a label definition");

    document = ValidDocument();
    document.annotation.missing.value = 0;
    Require(
        HasIssue(
            specforge::ValidateSampleLabelingDocument(document),
            specforge::SampleLabelingDocumentValidationIssueKind::InvalidUnlabeledValue),
        "canonical missing semantics should remain fixed to -1");
}

void TestValidatorRejectsUnsupportedDocumentSemantics()
{
    specforge::SampleLabelingDocument document = ValidDocument();
    document.format_kind = "specforge.session";
    document.schema_version = "1.0.0";
    document.annotation.kind = "continuous_float";
    document.annotation.missing.semantic = "nan";
    document.source.base_identity.clear();
    document.labeling.id.clear();

    const auto result = specforge::ValidateSampleLabelingDocument(document);
    const std::unordered_set<specforge::SampleLabelingDocumentValidationIssueKind> expected = {
        specforge::SampleLabelingDocumentValidationIssueKind::UnsupportedFormatKind,
        specforge::SampleLabelingDocumentValidationIssueKind::UnsupportedSchemaVersion,
        specforge::SampleLabelingDocumentValidationIssueKind::MissingSourceBaseIdentity,
        specforge::SampleLabelingDocumentValidationIssueKind::UnsupportedAnnotationKind,
        specforge::SampleLabelingDocumentValidationIssueKind::UnsupportedMissingSemantic,
        specforge::SampleLabelingDocumentValidationIssueKind::MissingTaskId};
    for (const auto issue : expected) {
        Require(
            HasIssue(result, issue),
            "validator should reject unsupported or missing canonical semantics");
    }
}

void TestValidatorEnforcesSchemaTwoIdentityAndMetadata()
{
    specforge::SampleLabelingDocument document = ValidDocument();
    document.labeling.id = "00000000-0000-4000-8000-00000000000A";
    Require(
        HasIssue(
            specforge::ValidateSampleLabelingDocument(document),
            specforge::SampleLabelingDocumentValidationIssueKind::
                InvalidTaskId),
        "schema 2 task ids must use the lowercase canonical UUID v4 form");

    document = ValidDocument();
    document.labeling.canonical_metadata.modified_at =
        *specforge::ParseCanonicalTimestamp(
            "2026-08-30T16:00:00.000Z");
    Require(
        HasIssue(
            specforge::ValidateSampleLabelingDocument(document),
            specforge::SampleLabelingDocumentValidationIssueKind::
                InvalidCanonicalTimestampOrder),
        "created_at must not be later than modified_at");

    document = ValidDocument();
    document.labeling.canonical_metadata.origin.annotation =
        specforge::SampleLabelingAnnotationOrigin{
            .name = "labels.csv",
            .format = "csv"};
    Require(
        HasIssue(
            specforge::ValidateSampleLabelingDocument(document),
            specforge::SampleLabelingDocumentValidationIssueKind::
                InvalidOriginAnnotation),
        "manual origin must not carry annotation promotion metadata");

    document = ValidDocument();
    document.labeling.canonical_metadata.origin.kind =
        "annotation_promotion";
    Require(
        HasIssue(
            specforge::ValidateSampleLabelingDocument(document),
            specforge::SampleLabelingDocumentValidationIssueKind::
                InvalidOriginAnnotation),
        "annotation promotion must carry portable annotation provenance");

    document = ValidDocument();
    document.labeling.canonical_metadata.origin.kind = "Bad-Origin";
    Require(
        HasIssue(
            specforge::ValidateSampleLabelingDocument(document),
            specforge::SampleLabelingDocumentValidationIssueKind::
                InvalidOriginKind),
        "origin kinds must remain lowercase extensible tokens");

    document = ValidDocument();
    document.labeling.canonical_metadata.origin.kind =
        "annotation_promotion";
    document.labeling.canonical_metadata.origin.annotation =
        specforge::SampleLabelingAnnotationOrigin{
            .name = "\xE3\x80\x80",
            .format = "json",
            .fingerprint =
                "sha256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"};
    Require(
        HasIssue(
            specforge::ValidateSampleLabelingDocument(document),
            specforge::SampleLabelingDocumentValidationIssueKind::
                InvalidOriginAnnotation),
        "promotion provenance must use a non-whitespace UTF-8 name, csv/npy format, and lowercase SHA-256");

    document = ValidDocument();
    document.labeling.canonical_metadata.description =
        std::string{"\xC3\x28", 2};
    Require(
        HasIssue(
            specforge::ValidateSampleLabelingDocument(document),
            specforge::SampleLabelingDocumentValidationIssueKind::
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
        specforge::ValidateSampleLabelingDocument(document);
    Require(
        HasIssue(
            invalid_authors,
            specforge::SampleLabelingDocumentValidationIssueKind::
                InvalidAuthor,
            0) &&
            HasIssue(
                invalid_authors,
                specforge::SampleLabelingDocumentValidationIssueKind::
                    InvalidAuthor,
                1) &&
            HasIssue(
                invalid_authors,
                specforge::SampleLabelingDocumentValidationIssueKind::
                    InvalidAuthor,
                2) &&
            HasIssue(
                invalid_authors,
                specforge::SampleLabelingDocumentValidationIssueKind::
                    InvalidAuthor,
                3),
        "author name, optional identifier, and optional email must contain non-whitespace UTF-8 text");

    document = ValidDocument();
    document.labeling.canonical_metadata.origin.kind =
        "annotation_promotion";
    document.labeling.canonical_metadata.origin.annotation =
        specforge::SampleLabelingAnnotationOrigin{
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
        specforge::ValidateSampleLabelingDocument(document).valid(),
        "schema 2 canonical metadata should accept valid Unicode declarations without normalization");
}

void TestValidatorRequiresPortableAnnotationOriginBasename()
{
    specforge::SampleLabelingDocument promoted = ValidDocument();
    promoted.labeling.canonical_metadata.origin.kind =
        "annotation_promotion";
    promoted.labeling.canonical_metadata.origin.annotation =
        specforge::SampleLabelingAnnotationOrigin{
            .name = "初始标签.csv",
            .format = "csv",
            .fingerprint = "sha256:" + std::string(64, 'a')};
    Require(
        specforge::ValidateSampleLabelingDocument(promoted).valid(),
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
                specforge::ValidateSampleLabelingDocument(promoted),
                specforge::SampleLabelingDocumentValidationIssueKind::
                    InvalidOriginAnnotation),
            "shared document validation must reject rooted, directory-bearing, and drive-relative annotation provenance names");
    }
}

void TestCanonicalProjectionRejectsUndefinedPendingCodes()
{
    const specforge::SampleLabelingDocument document =
        ValidDocument();
    specforge::SampleLabelingTask local_state =
        specforge::CreateSampleLabelingTask(
            document.labeling.id,
            "local state",
            document.annotation.values.size());
    local_state.output_path = "canonical-owner.asdf";
    local_state.output_format =
        specforge::SampleLabelingOutputArtifactFormat::
            CanonicalAsdf;
    local_state.canonical_metadata =
        document.labeling.canonical_metadata;
    local_state.values[1] = 42;
    local_state.pending_sample_indices.insert(1);

    Require(
        !specforge::ProjectSampleLabelingDocumentTask(
            document,
            local_state),
        "a pending code removed from canonical labels must fail projection instead of creating an undefined task value");

    local_state.metadata_save_pending = true;
    local_state.task_name = "Locally edited metadata";
    local_state.label_set.labels = {
        {0, "Galaxy", 'g'},
        {1, "Quasar", 'q'},
        {42, "Pending class", 'p'},
    };
    const std::optional<specforge::SampleLabelingTask>
        metadata_projection =
            specforge::ProjectSampleLabelingDocumentTask(
                document,
                local_state);
    Require(
        metadata_projection &&
            metadata_projection->values ==
                std::vector<int>({0, 42, 1}),
        "pending values should be validated after a compatible local metadata overlay is applied");
}

void TestCanonicalProjectionUsesOneMetadataAuthority()
{
    const specforge::SampleLabelingDocument document =
        ValidDocument();
    specforge::SampleLabelingTask local_state =
        specforge::CreateSampleLabelingTask(
            document.labeling.id,
            "stale cache name",
            document.annotation.values.size(),
            document.labeling.canonical_metadata);
    local_state.output_path = "canonical-owner.asdf";
    local_state.output_format =
        specforge::SampleLabelingOutputArtifactFormat::CanonicalAsdf;

    local_state.canonical_metadata.description =
        "stale cache description";
    const auto clean_projection =
        specforge::ProjectSampleLabelingDocumentTask(
            document,
            local_state);
    Require(
        clean_projection &&
            clean_projection->task_name ==
                document.labeling.name &&
            clean_projection->canonical_metadata ==
                document.labeling.canonical_metadata,
        "without a pending overlay, the durable canonical document must be the sole metadata authority");

    local_state.pending_sample_indices.insert(1);
    local_state.values[1] = 0;
    local_state.canonical_metadata.created_at =
        *specforge::ParseCanonicalTimestamp(
            "2026-08-30T16:23:45.124Z");
    Require(
        !specforge::ProjectSampleLabelingDocumentTask(
            document,
            local_state),
        "pending cache edits must not merge with a different immutable creation timestamp");

    local_state.canonical_metadata =
        document.labeling.canonical_metadata;
    local_state.canonical_metadata.origin.kind = "future_origin";
    Require(
        !specforge::ProjectSampleLabelingDocumentTask(
            document,
            local_state),
        "pending cache edits must not merge with a different immutable origin");

    local_state.canonical_metadata =
        document.labeling.canonical_metadata;
    local_state.canonical_metadata.modified_at =
        *specforge::ParseCanonicalTimestamp(
            "2026-08-30T16:30:00.000Z");
    Require(
        !specforge::ProjectSampleLabelingDocumentTask(
            document,
            local_state),
        "an older pending cache generation must be treated as stale rather than overlaid on newer disk metadata");
}

void TestFailFastValidatorBoundsDiagnostics()
{
    specforge::SampleLabelingDocument document = ValidDocument();
    document.source.sample_count = 25'000;
    document.source.roster.identity_kind =
        std::string{specforge::kSampleLabelingDocumentSourceIndexRoster};
    document.source.roster.sample_names.clear();
    document.annotation.values.assign(document.source.sample_count, 42);

    const auto result =
        specforge::ValidateSampleLabelingDocumentFailFast(document);
    Require(
        result.issues.size() == 1 &&
            result.issues.front().kind ==
                specforge::SampleLabelingDocumentValidationIssueKind::
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
        TestValidatorEnforcesSchemaTwoIdentityAndMetadata();
        TestValidatorRequiresPortableAnnotationOriginBasename();
        TestCanonicalProjectionRejectsUndefinedPendingCodes();
        TestCanonicalProjectionUsesOneMetadataAuthority();
        TestFailFastValidatorBoundsDiagnostics();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sample labeling document test failure: "
                  << error.what() << '\n';
        return 1;
    }
}
