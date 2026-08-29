#include "domain/sample_label_export.h"

#include "domain/sample_annotation_io.h"
#include "domain/sample_labeling_source_compatibility.h"
#include "domain/source_collection_manifest.h"

#include <utility>

namespace specforge {
namespace {

void SetError(
    std::string* error_message,
    std::string message)
{
    if (error_message != nullptr) {
        *error_message = std::move(message);
    }
}

bool IsAsciiCaseInsensitiveExtension(
    const std::filesystem::path& path,
    std::string_view expected)
{
    const std::filesystem::path::string_type extension =
        path.extension().native();
    if (extension.size() != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        auto character = extension[index];
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<std::filesystem::path::value_type>(
                character - 'A' + 'a');
        }
        if (character !=
            static_cast<std::filesystem::path::value_type>(
                expected[index])) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::optional<SampleLabelExportSnapshot>
BuildSampleLabelExportSnapshot(
    SampleLabelExportFormat format,
    const SampleLabelingTask& task,
    const SampleLabelingCanonicalSourceDescriptor& source,
    std::string* error_message)
{
    if (!task.values_are_authoritative) {
        SetError(
            error_message,
            "active labeling task values are not authoritative");
        return std::nullopt;
    }
    if (source.source_kind.empty()) {
        SetError(
            error_message,
            "canonical labeling source kind is empty");
        return std::nullopt;
    }
    if (source.sample_count != task.values.size()) {
        SetError(
            error_message,
            "active labeling task values do not match the canonical source roster");
        return std::nullopt;
    }
    if (!source.sample_names.empty() &&
        !SourceCollectionSampleNamesFormCanonicalRoster(
            source.sample_names,
            source.sample_count)) {
        SetError(
            error_message,
            "canonical labeling source sample names do not form a valid roster");
        return std::nullopt;
    }

    return SampleLabelExportSnapshot{
        .format = format,
        .source_kind = source.source_kind,
        .sample_names = source.sample_names,
        .uses_source_index = source.sample_names.empty(),
        .labels = task.label_set,
        .values = task.values,
    };
}

std::string SerializeSampleLabelValueForExport(
    const SampleLabelSet& labels,
    int value)
{
    if (value == kUnlabeledSampleLabelCode) {
        return std::string{kUnlabeledSampleLabelExportText};
    }
    if (const SampleLabelDefinition* label =
            FindSampleLabel(labels, value)) {
        return SerializeSampleLabelNameForExport(
            label->name);
    }
    return SerializeSampleLabelNameForExport(
        std::to_string(value));
}

std::string SerializeSampleLabelNameForExport(
    std::string_view label_name)
{
    if (label_name == kUnlabeledSampleLabelExportText ||
        (!label_name.empty() &&
         label_name.front() ==
             kSampleLabelExportEscapePrefix)) {
        return std::string(1, kSampleLabelExportEscapePrefix) +
            std::string(label_name);
    }
    return std::string(label_name);
}

DeserializedSampleLabelExportValue
DeserializeSampleLabelValueFromExport(
    std::string_view serialized)
{
    if (serialized == kUnlabeledSampleLabelExportText) {
        return DeserializedSampleLabelExportValue{
            .represents_unlabeled = true,
        };
    }
    if (!serialized.empty() &&
        serialized.front() ==
            kSampleLabelExportEscapePrefix) {
        serialized.remove_prefix(1);
    }
    return DeserializedSampleLabelExportValue{
        .represents_unlabeled = false,
        .label_text = std::string(serialized),
    };
}

bool IsSampleLabelExportPath(
    const std::filesystem::path& path,
    SampleLabelExportFormat format)
{
    if (path.empty()) {
        return false;
    }
    switch (format) {
    case SampleLabelExportFormat::Npy:
        return IsLabelValuesNpyExportPath(path);
    case SampleLabelExportFormat::Csv:
        return IsAsciiCaseInsensitiveExtension(path, ".csv");
    }
    return false;
}

bool ExportSampleLabelSnapshot(
    const std::filesystem::path& path,
    const SampleLabelExportSnapshot& snapshot,
    std::string* error_message)
{
    if (!IsSampleLabelExportPath(path, snapshot.format)) {
        SetError(
            error_message,
            "sample label export path does not match the selected format");
        return false;
    }
    if (snapshot.uses_source_index !=
            snapshot.sample_names.empty() ||
        (!snapshot.uses_source_index &&
         snapshot.sample_names.size() !=
             snapshot.values.size())) {
        SetError(
            error_message,
            "sample label export snapshot roster is invalid");
        return false;
    }

    switch (snapshot.format) {
    case SampleLabelExportFormat::Npy:
        return ExportLabelValuesToNpy(
            path,
            snapshot.values,
            error_message);
    case SampleLabelExportFormat::Csv:
        SetError(
            error_message,
            "CSV sample label export is not implemented");
        return false;
    }
    SetError(
        error_message,
        "sample label export format is unsupported");
    return false;
}

}  // namespace specforge
