#include "domain/sample_labeling.h"
#include "domain/sample_labeling_document.h"
#include "domain/source_collection_manifest.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
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
    specforge::SampleLabelingTask task =
        specforge::CreateSampleLabelingTask("task-alpha", "天体分类", 3);
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
        document.labeling.id == "task-alpha" &&
            document.labeling.name == "天体分类" &&
            document.labeling.labels.size() == 2,
        "builder should copy task identity, name, and label definitions");
    Require(
        specforge::ValidateSampleLabelingDocument(document).valid(),
        "builder output should satisfy canonical semantic invariants");
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
        specforge::CreateSampleLabelingTask("index-task", "Index labels", 2);
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
    document.schema_version = "2.0.0";
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
        TestSourceIndexRosterIsExplicitWithoutMaterializedIndexes();
        TestValidatorEnforcesSampleAlignmentAndRosterShape();
        TestValidatorEnforcesLabelAndUnlabeledInvariants();
        TestValidatorRejectsUnsupportedDocumentSemantics();
        TestFailFastValidatorBoundsDiagnostics();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sample labeling document test failure: "
                  << error.what() << '\n';
        return 1;
    }
}
