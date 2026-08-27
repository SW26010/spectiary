#include "domain/sample_labeling_document.h"

#include "domain/sample_labeling.h"
#include "domain/source_collection_manifest.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>
#include <utility>

namespace specforge {
namespace {

bool HasNonWhitespaceText(std::string_view value)
{
    return std::any_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character) == 0;
    });
}

void AddIssue(
    SampleLabelingDocumentValidationResult& result,
    SampleLabelingDocumentValidationIssueKind kind,
    std::size_t index = kSampleLabelingDocumentNoIssueIndex)
{
    result.issues.push_back({kind, index});
}

}  // namespace

SampleLabelingDocument BuildSampleLabelingDocument(
    std::string source_kind,
    const SourceCollectionContext& source_context,
    const SampleLabelingTask& task)
{
    SampleLabelingDocument document;
    const SourceCollectionIdentity& source_identity = source_context.identity;
    document.source.base_identity = source_identity.id;
    document.source.kind = std::move(source_kind);
    document.source.name = source_identity.source_name;
    document.source.fingerprint = source_identity.source_fingerprint;
    document.source.sample_count = source_identity.spectrum_count;
    if (!source_context.manifest.sample_names.empty()) {
        document.source.roster.identity_kind =
            std::string{kSampleLabelingDocumentExplicitNamesRoster};
        document.source.roster.sample_names =
            source_context.manifest.sample_names;
    }

    document.annotation.name = task.task_name;
    document.annotation.values.reserve(task.values.size());
    for (const int value : task.values) {
        document.annotation.values.push_back(
            static_cast<std::int32_t>(value));
    }

    document.labeling.id = task.task_id;
    document.labeling.name = task.task_name;
    document.labeling.labels.reserve(task.label_set.labels.size());
    for (const SampleLabelDefinition& label : task.label_set.labels) {
        document.labeling.labels.push_back({
            static_cast<std::int32_t>(label.code),
            label.name,
            label.shortcut == '\0' ? std::string{} : std::string(1, label.shortcut)});
    }
    return document;
}

SampleLabelingDocumentValidationResult ValidateSampleLabelingDocument(
    const SampleLabelingDocument& document)
{
    SampleLabelingDocumentValidationResult result;
    if (document.format_kind != kSampleLabelingDocumentFormatKind) {
        AddIssue(
            result,
            SampleLabelingDocumentValidationIssueKind::UnsupportedFormatKind);
    }
    if (document.schema_version != kSampleLabelingDocumentSchemaVersion) {
        AddIssue(
            result,
            SampleLabelingDocumentValidationIssueKind::UnsupportedSchemaVersion);
    }

    if (!HasNonWhitespaceText(document.source.base_identity)) {
        AddIssue(
            result,
            SampleLabelingDocumentValidationIssueKind::MissingSourceBaseIdentity);
    }
    if (!HasNonWhitespaceText(document.source.kind)) {
        AddIssue(result, SampleLabelingDocumentValidationIssueKind::MissingSourceKind);
    }
    if (!HasNonWhitespaceText(document.source.name)) {
        AddIssue(result, SampleLabelingDocumentValidationIssueKind::MissingSourceName);
    }
    if (!HasNonWhitespaceText(document.source.fingerprint)) {
        AddIssue(
            result,
            SampleLabelingDocumentValidationIssueKind::MissingSourceFingerprint);
    }

    const bool explicit_names =
        document.source.roster.identity_kind ==
        kSampleLabelingDocumentExplicitNamesRoster;
    const bool source_index =
        document.source.roster.identity_kind ==
        kSampleLabelingDocumentSourceIndexRoster;
    if (!explicit_names && !source_index) {
        AddIssue(
            result,
            SampleLabelingDocumentValidationIssueKind::UnsupportedRosterIdentityKind);
    } else if (source_index && !document.source.roster.sample_names.empty()) {
        AddIssue(
            result,
            SampleLabelingDocumentValidationIssueKind::SourceIndexRosterHasNames);
    }

    if (explicit_names) {
        if (document.source.roster.sample_names.size() !=
            document.source.sample_count) {
            AddIssue(
                result,
                SampleLabelingDocumentValidationIssueKind::RosterSampleCountMismatch);
        }
        std::unordered_set<std::string> names;
        for (std::size_t index = 0;
             index < document.source.roster.sample_names.size();
             ++index) {
            const std::string& name =
                document.source.roster.sample_names[index];
            if (!HasNonWhitespaceText(name)) {
                AddIssue(
                    result,
                    SampleLabelingDocumentValidationIssueKind::EmptySampleName,
                    index);
            }
            if (!names.insert(name).second) {
                AddIssue(
                    result,
                    SampleLabelingDocumentValidationIssueKind::DuplicateSampleName,
                    index);
            }
        }
    }

    if (document.annotation.kind !=
        kSampleLabelingDocumentCategoricalIntegerKind) {
        AddIssue(
            result,
            SampleLabelingDocumentValidationIssueKind::UnsupportedAnnotationKind);
    }
    if (!HasNonWhitespaceText(document.annotation.name)) {
        AddIssue(
            result,
            SampleLabelingDocumentValidationIssueKind::MissingAnnotationName);
    }
    if (document.annotation.missing.semantic !=
        kSampleLabelingDocumentUnlabeledSemantic) {
        AddIssue(
            result,
            SampleLabelingDocumentValidationIssueKind::UnsupportedMissingSemantic);
    }
    if (document.annotation.missing.value !=
        kSampleLabelingDocumentUnlabeledValue) {
        AddIssue(
            result,
            SampleLabelingDocumentValidationIssueKind::InvalidUnlabeledValue);
    }
    if (document.annotation.values.size() != document.source.sample_count) {
        AddIssue(
            result,
            SampleLabelingDocumentValidationIssueKind::AnnotationSampleCountMismatch);
    }

    if (!HasNonWhitespaceText(document.labeling.id)) {
        AddIssue(result, SampleLabelingDocumentValidationIssueKind::MissingTaskId);
    }
    if (!HasNonWhitespaceText(document.labeling.name)) {
        AddIssue(result, SampleLabelingDocumentValidationIssueKind::MissingTaskName);
    }

    std::unordered_set<std::int32_t> label_codes;
    std::unordered_set<char> label_shortcuts;
    for (std::size_t index = 0; index < document.labeling.labels.size(); ++index) {
        const SampleLabelingDocumentLabel& label =
            document.labeling.labels[index];
        if (label.code == kSampleLabelingDocumentUnlabeledValue) {
            AddIssue(
                result,
                SampleLabelingDocumentValidationIssueKind::ReservedLabelCode,
                index);
        } else if (!label_codes.insert(label.code).second) {
            AddIssue(
                result,
                SampleLabelingDocumentValidationIssueKind::DuplicateLabelCode,
                index);
        }
        if (!HasNonWhitespaceText(label.name)) {
            AddIssue(
                result,
                SampleLabelingDocumentValidationIssueKind::MissingLabelName,
                index);
        }
        if (!label.shortcut.empty()) {
            if (label.shortcut.size() != 1 ||
                !IsValidSampleLabelShortcut(label.shortcut.front())) {
                AddIssue(
                    result,
                    SampleLabelingDocumentValidationIssueKind::InvalidLabelShortcut,
                    index);
            } else {
                const char normalized =
                    NormalizeSampleLabelShortcut(label.shortcut.front());
                if (!label_shortcuts.insert(normalized).second) {
                    AddIssue(
                        result,
                        SampleLabelingDocumentValidationIssueKind::DuplicateLabelShortcut,
                        index);
                }
            }
        }
    }

    for (std::size_t index = 0; index < document.annotation.values.size(); ++index) {
        const std::int32_t value = document.annotation.values[index];
        if (value != kSampleLabelingDocumentUnlabeledValue &&
            !label_codes.contains(value)) {
            AddIssue(
                result,
                SampleLabelingDocumentValidationIssueKind::UndefinedAnnotationValue,
                index);
        }
    }
    return result;
}

}  // namespace specforge
