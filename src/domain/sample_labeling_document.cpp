#include "domain/sample_labeling_document.h"

#include "domain/sample_labeling.h"
#include "domain/sample_labeling_source_compatibility.h"
#include "domain/source_collection_manifest.h"
#include "domain/utf8.h"
#include "domain/uuid_v4.h"

#include <algorithm>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace specforge {
namespace {

struct ValidationComplete final {
    SampleLabelingDocumentValidationIssue issue;
};

bool HasNonWhitespaceText(std::string_view value)
{
    return IsValidUtf8WithNonWhitespace(value);
}

bool IsLowercaseToken(std::string_view value)
{
    if (value.empty() || value.front() < 'a' || value.front() > 'z') {
        return false;
    }
    return std::ranges::all_of(value, [](char character) {
        return (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') || character == '_';
    });
}

bool IsAnnotationFingerprint(std::string_view value)
{
    constexpr std::string_view prefix = "sha256:";
    return value.size() == prefix.size() + 64U &&
        value.starts_with(prefix) &&
        std::ranges::all_of(value.substr(prefix.size()), [](char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f');
        });
}

void AddIssue(
    SampleLabelingDocumentValidationResult& result,
    bool fail_fast,
    SampleLabelingDocumentValidationIssueKind kind,
    std::size_t index = kSampleLabelingDocumentNoIssueIndex)
{
    if (fail_fast) {
        throw ValidationComplete{{kind, index}};
    }
    result.issues.push_back({kind, index});
}

SampleLabelingDocument BuildDocumentWithSource(
    SampleLabelingDocumentSource source,
    const SampleLabelingTask& task)
{
    SampleLabelingDocument document;
    document.source = std::move(source);
    document.annotation.values.reserve(task.values.size());
    for (const int value : task.values) {
        document.annotation.values.push_back(
            static_cast<std::int32_t>(value));
    }

    document.labeling.id = task.task_id;
    document.labeling.name = task.task_name;
    document.labeling.canonical_metadata = task.canonical_metadata;
    document.labeling.labels.reserve(
        task.label_set.labels.size());
    for (const SampleLabelDefinition& label :
         task.label_set.labels) {
        document.labeling.labels.push_back({
            static_cast<std::int32_t>(label.code),
            label.name,
            label.shortcut == '\0'
                ? std::string{}
                : std::string(1, label.shortcut)});
    }
    return document;
}

}  // namespace

SampleLabelingDocument BuildSampleLabelingDocument(
    std::string source_kind,
    const SourceCollectionContext& source_context,
    const SampleLabelingTask& task)
{
    SampleLabelingDocumentSource source;
    const SourceCollectionIdentity& source_identity = source_context.identity;
    source.base_identity = source_identity.id;
    source.kind = std::move(source_kind);
    source.name = source_identity.source_name;
    source.fingerprint = source_identity.source_fingerprint;
    source.sample_count = source_identity.spectrum_count;
    if (SourceCollectionSampleNamesFormCanonicalRoster(
            source_context.manifest.sample_names,
            source.sample_count)) {
        source.roster.identity_kind =
            std::string{kSampleLabelingDocumentExplicitNamesRoster};
        source.roster.sample_names =
            source_context.manifest.sample_names;
    }
    return BuildDocumentWithSource(
        std::move(source),
        task);
}

SampleLabelingDocument BuildSampleLabelingDocument(
    const SampleLabelingCanonicalSourceDescriptor& source,
    const SampleLabelingTask& task)
{
    SampleLabelingDocumentSource document_source;
    document_source.base_identity = source.base_identity;
    document_source.kind = source.source_kind;
    document_source.name = source.source_name;
    document_source.fingerprint = source.source_fingerprint;
    document_source.sample_count = source.sample_count;
    if (SourceCollectionSampleNamesFormCanonicalRoster(
            source.sample_names,
            document_source.sample_count)) {
        document_source.roster.identity_kind =
            std::string{kSampleLabelingDocumentExplicitNamesRoster};
        document_source.roster.sample_names =
            source.sample_names;
    }
    return BuildDocumentWithSource(
        std::move(document_source),
        task);
}

std::optional<SampleLabelingTask>
ProjectSampleLabelingDocumentTask(
    const SampleLabelingDocument& document,
    const SampleLabelingTask& local_state,
    const std::function<void()>& cancellation_checkpoint)
{
    if (!local_state.output_path ||
        local_state.output_format !=
            SampleLabelingOutputArtifactFormat::CanonicalAsdf ||
        document.labeling.id != local_state.task_id ||
        document.annotation.values.size() !=
            local_state.values.size()) {
        return std::nullopt;
    }

    const bool has_pending_local_edits =
        !local_state.pending_sample_indices.empty() ||
        local_state.metadata_save_pending;
    if (has_pending_local_edits &&
        (local_state.canonical_metadata.created_at !=
             document.labeling.canonical_metadata.created_at ||
         local_state.canonical_metadata.origin !=
             document.labeling.canonical_metadata.origin ||
         local_state.canonical_metadata.modified_at <
             document.labeling.canonical_metadata.modified_at)) {
        return std::nullopt;
    }

    SampleLabelingTask projected =
        CreateSampleLabelingTask(
            document.labeling.id,
            document.labeling.name,
            0,
            document.labeling.canonical_metadata);
    projected.values.reserve(
        document.annotation.values.size());
    for (std::size_t index = 0;
         index < document.annotation.values.size();
         ++index) {
        if ((index & 0xfffU) == 0U &&
            cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        projected.values.push_back(
            static_cast<int>(
                document.annotation.values[index]));
    }
    projected.label_set.labels.reserve(
        document.labeling.labels.size());
    for (std::size_t index = 0;
         index < document.labeling.labels.size();
         ++index) {
        if ((index & 0xfffU) == 0U &&
            cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        const SampleLabelingDocumentLabel& label =
            document.labeling.labels[index];
        projected.label_set.labels.push_back(
            SampleLabelDefinition{
                .code = static_cast<int>(label.code),
                .name = label.name,
                .shortcut = label.shortcut.empty()
                    ? '\0'
                    : label.shortcut.front(),
            });
    }

    projected.auto_advance = local_state.auto_advance;
    projected.skip_labeled_on_advance =
        local_state.skip_labeled_on_advance;
    projected.remembered_position =
        local_state.remembered_position;
    projected.output_path = local_state.output_path;
    projected.output_format = local_state.output_format;
    projected.values_are_authoritative = true;
    projected.pending_sample_indices =
        local_state.pending_sample_indices;
    if (!projected.pending_sample_indices.empty() &&
        local_state.canonical_metadata.modified_at >
            projected.canonical_metadata.modified_at) {
        projected.canonical_metadata.modified_at =
            local_state.canonical_metadata.modified_at;
    }
    for (const std::size_t sample_index :
         projected.pending_sample_indices) {
        if (sample_index >= projected.values.size()) {
            return std::nullopt;
        }
        projected.values[sample_index] =
            local_state.values[sample_index];
    }

    projected.metadata_save_pending =
        local_state.metadata_save_pending;
    if (projected.metadata_save_pending) {
        projected.task_name = local_state.task_name;
        projected.label_set = local_state.label_set;
        projected.canonical_metadata.description =
            local_state.canonical_metadata.description;
        projected.canonical_metadata.authors =
            local_state.canonical_metadata.authors;
        if (local_state.canonical_metadata.modified_at >
            projected.canonical_metadata.modified_at) {
            projected.canonical_metadata.modified_at =
                local_state.canonical_metadata.modified_at;
        }
    }

    std::vector<int> defined_label_codes;
    defined_label_codes.reserve(
        projected.label_set.labels.size());
    for (const SampleLabelDefinition& label :
         projected.label_set.labels) {
        defined_label_codes.push_back(label.code);
    }
    std::ranges::sort(defined_label_codes);
    for (std::size_t index = 0;
         index < projected.values.size();
         ++index) {
        if ((index & 0xfffU) == 0U &&
            cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        const int value = projected.values[index];
        if (value != kUnlabeledSampleLabelCode &&
            !std::ranges::binary_search(
                defined_label_codes,
                value)) {
            return std::nullopt;
        }
    }
    projected.save_state = local_state.save_state;
    projected.save_state.pending_count =
        projected.pending_sample_indices.size();
    const bool has_pending_output =
        !projected.pending_sample_indices.empty() ||
        projected.metadata_save_pending;
    if (has_pending_output &&
        projected.save_state.kind !=
            SampleLabelSaveStateKind::Failed) {
        projected.save_state.kind =
            SampleLabelSaveStateKind::Pending;
    } else if (!has_pending_output &&
               projected.save_state.kind ==
                   SampleLabelSaveStateKind::Pending) {
        projected.save_state.kind =
            SampleLabelSaveStateKind::AutosavedToOutput;
    }
    RebuildSampleLabelingTaskStatistics(
        projected,
        cancellation_checkpoint);
    return projected;
}

namespace {

SampleLabelingDocumentValidationResult ValidateSampleLabelingDocumentImpl(
    const SampleLabelingDocument& document,
    bool fail_fast)
{
    SampleLabelingDocumentValidationResult result;
    if (document.format_kind != kSampleLabelingDocumentFormatKind) {
        AddIssue(
            result,
            fail_fast,
            SampleLabelingDocumentValidationIssueKind::UnsupportedFormatKind);
    }
    if (document.schema_version != kSampleLabelingDocumentSchemaVersion) {
        AddIssue(
            result,
            fail_fast,
            SampleLabelingDocumentValidationIssueKind::UnsupportedSchemaVersion);
    }

    if (!HasNonWhitespaceText(document.source.base_identity)) {
        AddIssue(
            result,
            fail_fast,
            SampleLabelingDocumentValidationIssueKind::MissingSourceBaseIdentity);
    }
    if (!HasNonWhitespaceText(document.source.kind)) {
        AddIssue(
            result,
            fail_fast,
            SampleLabelingDocumentValidationIssueKind::MissingSourceKind);
    }
    if (!HasNonWhitespaceText(document.source.name)) {
        AddIssue(
            result,
            fail_fast,
            SampleLabelingDocumentValidationIssueKind::MissingSourceName);
    }
    if (!HasNonWhitespaceText(document.source.fingerprint)) {
        AddIssue(
            result,
            fail_fast,
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
            fail_fast,
            SampleLabelingDocumentValidationIssueKind::UnsupportedRosterIdentityKind);
    } else if (source_index && !document.source.roster.sample_names.empty()) {
        AddIssue(
            result,
            fail_fast,
            SampleLabelingDocumentValidationIssueKind::SourceIndexRosterHasNames);
    }

    if (explicit_names) {
        if (document.source.roster.sample_names.size() !=
            document.source.sample_count) {
            AddIssue(
                result,
                fail_fast,
                SampleLabelingDocumentValidationIssueKind::RosterSampleCountMismatch);
        }
        for (std::size_t index = 0;
             index < document.source.roster.sample_names.size();
             ++index) {
            const std::string& name =
                document.source.roster.sample_names[index];
            if (!HasNonWhitespaceText(name)) {
                AddIssue(
                    result,
                    fail_fast,
                    SampleLabelingDocumentValidationIssueKind::EmptySampleName,
                    index);
            }
        }

        // Keep duplicate detection bounded and allocation-predictable. The
        // order vector references canonical strings instead of copying every
        // roster name into hash-table nodes and buckets.
        std::vector<std::size_t> name_order(
            document.source.roster.sample_names.size());
        std::iota(name_order.begin(), name_order.end(), std::size_t{0});
        std::ranges::sort(name_order,
            [&document](std::size_t left, std::size_t right) {
                const std::string& left_name =
                    document.source.roster.sample_names[left];
                const std::string& right_name =
                    document.source.roster.sample_names[right];
                return left_name < right_name ||
                       (left_name == right_name && left < right);
            });
        for (std::size_t index = 1; index < name_order.size(); ++index) {
            const std::size_t previous = name_order[index - 1U];
            const std::size_t current = name_order[index];
            if (document.source.roster.sample_names[previous] ==
                document.source.roster.sample_names[current]) {
                AddIssue(
                    result,
                    fail_fast,
                    SampleLabelingDocumentValidationIssueKind::DuplicateSampleName,
                    current);
            }
        }
    }

    if (document.annotation.kind !=
        kSampleLabelingDocumentCategoricalIntegerKind) {
        AddIssue(
            result,
            fail_fast,
            SampleLabelingDocumentValidationIssueKind::UnsupportedAnnotationKind);
    }
    if (document.annotation.missing.semantic !=
        kSampleLabelingDocumentUnlabeledSemantic) {
        AddIssue(
            result,
            fail_fast,
            SampleLabelingDocumentValidationIssueKind::UnsupportedMissingSemantic);
    }
    if (document.annotation.missing.value !=
        kSampleLabelingDocumentUnlabeledValue) {
        AddIssue(
            result,
            fail_fast,
            SampleLabelingDocumentValidationIssueKind::InvalidUnlabeledValue);
    }
    if (document.annotation.values.size() != document.source.sample_count) {
        AddIssue(
            result,
            fail_fast,
            SampleLabelingDocumentValidationIssueKind::AnnotationSampleCountMismatch);
    }

    if (!HasNonWhitespaceText(document.labeling.id)) {
        AddIssue(
            result,
            fail_fast,
            SampleLabelingDocumentValidationIssueKind::MissingTaskId);
    } else if (!IsCanonicalUuidV4(document.labeling.id)) {
        AddIssue(
            result,
            fail_fast,
            SampleLabelingDocumentValidationIssueKind::InvalidTaskId);
    }
    if (!HasNonWhitespaceText(document.labeling.name)) {
        AddIssue(
            result,
            fail_fast,
            SampleLabelingDocumentValidationIssueKind::MissingTaskName);
    }

    const SampleLabelingTaskCanonicalMetadata& metadata =
        document.labeling.canonical_metadata;
    if (metadata.modified_at < metadata.created_at) {
        AddIssue(
            result,
            fail_fast,
            SampleLabelingDocumentValidationIssueKind::
                InvalidCanonicalTimestampOrder);
    }
    const bool known_manual = metadata.origin.kind == "manual";
    const bool known_promotion =
        metadata.origin.kind == "annotation_promotion";
    if (!IsLowercaseToken(metadata.origin.kind)) {
        AddIssue(
            result,
            fail_fast,
            SampleLabelingDocumentValidationIssueKind::InvalidOriginKind);
    }
    if ((known_manual && metadata.origin.annotation.has_value()) ||
        (known_promotion && !metadata.origin.annotation.has_value())) {
        AddIssue(
            result,
            fail_fast,
            SampleLabelingDocumentValidationIssueKind::
                InvalidOriginAnnotation);
    }
    if (metadata.origin.annotation) {
        const SampleLabelingAnnotationOrigin& annotation =
            *metadata.origin.annotation;
        const bool fingerprint_valid = !annotation.fingerprint ||
            IsAnnotationFingerprint(*annotation.fingerprint);
        if (!IsValidSampleLabelingAnnotationOriginName(
                annotation.name) ||
            (annotation.format != "csv" && annotation.format != "npy") ||
            !fingerprint_valid) {
            AddIssue(
                result,
                fail_fast,
                SampleLabelingDocumentValidationIssueKind::
                    InvalidOriginAnnotation);
        }
    }
    if (metadata.description &&
        !IsValidUtf8(*metadata.description)) {
        AddIssue(
            result,
            fail_fast,
            SampleLabelingDocumentValidationIssueKind::InvalidDescription);
    }
    for (std::size_t index = 0; index < metadata.authors.size(); ++index) {
        const SampleLabelingAuthor& author = metadata.authors[index];
        if (!HasNonWhitespaceText(author.name) ||
            (author.identifier &&
             !HasNonWhitespaceText(*author.identifier)) ||
            (author.email && !HasNonWhitespaceText(*author.email))) {
            AddIssue(
                result,
                fail_fast,
                SampleLabelingDocumentValidationIssueKind::InvalidAuthor,
                index);
        }
    }

    for (std::size_t index = 0; index < document.labeling.labels.size(); ++index) {
        const SampleLabelingDocumentLabel& label =
            document.labeling.labels[index];
        if (label.code == kSampleLabelingDocumentUnlabeledValue) {
            AddIssue(
                result,
                fail_fast,
                SampleLabelingDocumentValidationIssueKind::ReservedLabelCode,
                index);
        }
        if (!HasNonWhitespaceText(label.name)) {
            AddIssue(
                result,
                fail_fast,
                SampleLabelingDocumentValidationIssueKind::MissingLabelName,
                index);
        }
        if (!label.shortcut.empty()) {
            if (label.shortcut.size() != 1 ||
                !IsValidSampleLabelShortcut(label.shortcut.front())) {
                AddIssue(
                    result,
                    fail_fast,
                    SampleLabelingDocumentValidationIssueKind::InvalidLabelShortcut,
                    index);
            }
        }
    }

    // Reuse one bounded index vector for label-code and shortcut uniqueness
    // plus value membership. This avoids implementation-dependent hash nodes,
    // buckets, and rehash peaks at the production label-count boundary.
    std::vector<std::size_t> label_order(document.labeling.labels.size());
    std::iota(label_order.begin(), label_order.end(), std::size_t{0});
    const auto code_less = [&document](std::size_t left, std::size_t right) {
        const std::int32_t left_code = document.labeling.labels[left].code;
        const std::int32_t right_code = document.labeling.labels[right].code;
        return left_code < right_code ||
               (left_code == right_code && left < right);
    };
    std::ranges::sort(label_order, code_less);
    for (std::size_t index = 1; index < label_order.size(); ++index) {
        const std::size_t previous = label_order[index - 1U];
        const std::size_t current = label_order[index];
        const std::int32_t code = document.labeling.labels[current].code;
        if (code != kSampleLabelingDocumentUnlabeledValue &&
            code == document.labeling.labels[previous].code) {
            AddIssue(
                result,
                fail_fast,
                SampleLabelingDocumentValidationIssueKind::DuplicateLabelCode,
                current);
        }
    }

    const auto valid_shortcut = [&document](std::size_t index) {
        const std::string& shortcut =
            document.labeling.labels[index].shortcut;
        return shortcut.size() == 1U &&
               IsValidSampleLabelShortcut(shortcut.front());
    };
    std::ranges::sort(label_order,
        [&document, &valid_shortcut](std::size_t left, std::size_t right) {
            const bool left_valid = valid_shortcut(left);
            const bool right_valid = valid_shortcut(right);
            if (left_valid != right_valid) {
                return left_valid;
            }
            if (!left_valid) {
                return left < right;
            }
            const char left_shortcut = NormalizeSampleLabelShortcut(
                document.labeling.labels[left].shortcut.front());
            const char right_shortcut = NormalizeSampleLabelShortcut(
                document.labeling.labels[right].shortcut.front());
            return left_shortcut < right_shortcut ||
                   (left_shortcut == right_shortcut && left < right);
        });
    for (std::size_t index = 1; index < label_order.size(); ++index) {
        const std::size_t previous = label_order[index - 1U];
        const std::size_t current = label_order[index];
        if (!valid_shortcut(previous) || !valid_shortcut(current)) {
            break;
        }
        if (NormalizeSampleLabelShortcut(
                document.labeling.labels[previous].shortcut.front()) ==
            NormalizeSampleLabelShortcut(
                document.labeling.labels[current].shortcut.front())) {
            AddIssue(
                result,
                fail_fast,
                SampleLabelingDocumentValidationIssueKind::DuplicateLabelShortcut,
                current);
        }
    }

    std::ranges::sort(label_order, code_less);

    for (std::size_t index = 0; index < document.annotation.values.size(); ++index) {
        const std::int32_t value = document.annotation.values[index];
        if (value != kSampleLabelingDocumentUnlabeledValue &&
            !std::ranges::binary_search(label_order,
                value,
                {},
                [&document](std::size_t label_index) {
                    return document.labeling.labels[label_index].code;
                })) {
            AddIssue(
                result,
                fail_fast,
                SampleLabelingDocumentValidationIssueKind::UndefinedAnnotationValue,
                index);
        }
    }
    return result;
}

}  // namespace

SampleLabelingDocumentValidationResult ValidateSampleLabelingDocument(
    const SampleLabelingDocument& document)
{
    return ValidateSampleLabelingDocumentImpl(document, false);
}

SampleLabelingDocumentValidationResult
ValidateSampleLabelingDocumentFailFast(
    const SampleLabelingDocument& document)
{
    try {
        return ValidateSampleLabelingDocumentImpl(document, true);
    } catch (const ValidationComplete& complete) {
        SampleLabelingDocumentValidationResult result;
        result.issues.push_back(complete.issue);
        return result;
    }
}

}  // namespace specforge
