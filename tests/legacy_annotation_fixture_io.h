#pragma once

#include "domain/sample_annotation_io.h"
#include "domain/npy_array_io.h"
#include "app/local_user_state_json.h"
#include "platform/atomic_file.h"

namespace spectiary::test_support {

struct SampleLabelResultWriteOutcome {
    bool array_saved = false;
    bool metadata_saved = false;
    std::string message;
};

// Writes historical import fixtures only. Application annotation I/O is read-only.
class LegacyFixtureIo : public SampleAnnotationIoAdapter {
public:
    [[nodiscard]] bool SaveLabelArray(
        const std::filesystem::path& path,
        const SampleLabelingTask& task,
        std::string* error_message = nullptr) const;
    [[nodiscard]] bool SaveLabelMetadata(
        const std::filesystem::path& result_path,
        const SampleLabelingTask& task,
        const SampleLabelResultMetadataSource* source = nullptr,
        std::string* error_message = nullptr) const;
    [[nodiscard]] SampleLabelResultWriteOutcome SaveLabelResult(
        const std::filesystem::path& path,
        const SampleLabelingTask& task,
        const SampleLabelResultMetadataSource* source = nullptr) const;
};

namespace legacy_fixture_detail {
inline constexpr const char* kMetadataFormatKind = "specforge.sample_label_result.metadata";
inline constexpr int kMetadataSchemaVersion = 1;
inline constexpr std::string_view kInt32DtypeText = "int32";
inline std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}
inline std::string RelativeResultFileReference(const std::filesystem::path& result_path)
{
    const std::filesystem::path filename = result_path.filename();
    return filename.empty() ? PathToUtf8(result_path) : PathToUtf8(filename);
}

inline bool ValidateNpyLabelOutputPath(
    const std::filesystem::path& result_path,
    std::string* error_message)
{
    if (result_path.empty()) {
        if (error_message != nullptr) {
            *error_message = "label output path is empty";
        }
        return false;
    }
    if (result_path.extension() == ".asdf") {
        if (error_message != nullptr) {
            *error_message =
                "ASDF label output is unavailable until the ASDF persistence owner is enabled";
        }
        return false;
    }
    return true;
}

inline bool WriteLabelValuesToNpyAtomically(
    const std::filesystem::path& path,
    std::span<const int> values,
    std::string* error_message,
    std::string_view target_description)
{
    AtomicFileWriteOptions options;
    options.open_mode = std::ios::binary | std::ios::trunc;
    options.target_description = std::string(target_description);
    return WriteFileAtomically(
        path,
        options,
        [values](std::ostream& stream, std::string& error) {
            try {
                WriteNpyInt32Values(stream, values);
            } catch (const NpyArrayError& write_error) {
                error = write_error.what();
                return false;
            }
            return true;
        },
        error_message);
}

inline bool ValidateLabelMetadataContract(
    const std::filesystem::path& result_path,
    const SampleLabelingTask& task,
    std::string* error_message)
{
    if (!ValidateNpyLabelOutputPath(result_path, error_message)) {
        return false;
    }
    if (task.task_id.empty()) {
        if (error_message != nullptr) {
            *error_message = "sample label result metadata task id is empty";
        }
        return false;
    }
    return true;
}

}  // namespace legacy_fixture_detail

inline bool LegacyFixtureIo::SaveLabelArray(
    const std::filesystem::path& path,
    const SampleLabelingTask& task,
    std::string* error_message) const
{
    if (!legacy_fixture_detail::ValidateNpyLabelOutputPath(path, error_message)) {
        return false;
    }
    return legacy_fixture_detail::WriteLabelValuesToNpyAtomically(
        path,
        task.values.Complete(),
        error_message,
        "sample label result");
}

inline bool LegacyFixtureIo::SaveLabelMetadata(
    const std::filesystem::path& result_path,
    const SampleLabelingTask& task,
    const SampleLabelResultMetadataSource* source,
    std::string* error_message) const
{
    if (!legacy_fixture_detail::ValidateLabelMetadataContract(result_path, task, error_message)) {
        return false;
    }

    const std::filesystem::path metadata_path = MetadataPathForResult(result_path);
    const std::string result_file = legacy_fixture_detail::RelativeResultFileReference(result_path);
    return WriteVersionedJsonCacheFile(
        metadata_path,
        legacy_fixture_detail::kMetadataFormatKind,
        legacy_fixture_detail::kMetadataSchemaVersion,
        "sample label result metadata",
        [&](std::ostream& stream, std::string&) {
            stream << ",\n";
            stream << "  \"result_file\": ";
            WriteJsonString(stream, result_file);
            stream << ",\n";
            stream << "  \"task_id\": ";
            WriteJsonString(stream, task.task_id);
            stream << ",\n";
            stream << "  \"value_count\": " << task.values.SampleCount() << ",\n";
            stream << "  \"expected_dtype\": ";
            WriteJsonString(stream, legacy_fixture_detail::kInt32DtypeText);
            stream << ",\n";
            stream << "  \"unlabeled_sentinel\": " << kUnlabeledSampleLabelCode << ",\n";
            stream << "  \"task_name\": ";
            WriteJsonString(stream, task.task_name);
            stream << ",\n";
            stream << "  \"labels\": [";
            if (!task.label_set.labels.empty()) {
                stream << "\n";
            }
            for (std::size_t label_index = 0; label_index < task.label_set.labels.size(); ++label_index) {
                const SampleLabelDefinition& label = task.label_set.labels[label_index];
                stream << "    { \"code\": " << label.code << ", \"name\": ";
                WriteJsonString(stream, label.name);
                stream << ", \"shortcut\": ";
                const std::string shortcut =
                    label.shortcut == '\0' ? std::string{} : std::string(1, label.shortcut);
                WriteJsonString(stream, shortcut);
                stream << " }" << (label_index + 1 == task.label_set.labels.size() ? "\n" : ",\n");
            }
            if (!task.label_set.labels.empty()) {
                stream << "  ";
            }
            stream << "]";
            if (source != nullptr) {
                stream << ",\n";
                stream << "  \"source_collection\": {\n";
                stream << "    \"source_name\": ";
                WriteJsonString(stream, source->source_name);
                stream << ",\n";
                stream << "    \"source_fingerprint\": ";
                WriteJsonString(stream, source->source_fingerprint);
                stream << ",\n";
                stream << "    \"context_fingerprint\": ";
                WriteJsonString(stream, source->context_fingerprint);
                stream << ",\n";
                stream << "    \"spectrum_count\": " << source->spectrum_count << "\n";
                stream << "  }";
            }
            stream << "\n";
            return true;
        },
        error_message);
}

inline SampleLabelResultWriteOutcome LegacyFixtureIo::SaveLabelResult(
    const std::filesystem::path& path,
    const SampleLabelingTask& task,
    const SampleLabelResultMetadataSource* source) const
{
    SampleLabelResultWriteOutcome outcome;
    if (!legacy_fixture_detail::ValidateLabelMetadataContract(path, task, &outcome.message)) {
        return outcome;
    }
    if (!SaveLabelArray(path, task, &outcome.message)) {
        return outcome;
    }
    outcome.array_saved = true;

    if (!SaveLabelMetadata(path, task, source, &outcome.message)) {
        return outcome;
    }
    outcome.metadata_saved = true;
    return outcome;
}


}  // namespace spectiary::test_support
