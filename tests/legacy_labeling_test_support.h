#pragma once

#include "legacy_annotation_fixture_io.h"

#include "domain/sample_annotation_io.h"
#include "domain/sample_labeling.h"

namespace spectiary::test_support {

inline void SelectLegacyFixtureOutputPath(SampleLabelingTask& task, std::filesystem::path output_path)
{
    task.persistence.output_path = std::move(output_path);
    task.persistence.output_format =
        SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
    task.persistence.pending_sample_indices.clear();
    task.persistence.metadata_save_pending = true;

    std::string ignored_error;
    const std::optional<LoadedSampleLabelResult> output =
        SampleAnnotationIoAdapter{}.LoadLabelResult(
            *task.persistence.output_path,
            task.values.SampleCount(),
            {},
            &ignored_error);
    for (std::size_t index = 0; index < task.values.SampleCount(); ++index) {
        const int base_value =
            output ? output->values[index] : kUnlabeledSampleLabelCode;
        if (task.values.Complete()[index] != base_value) {
            task.persistence.pending_sample_indices.insert(index);
        }
    }
    task.persistence.save_state.pending_count = task.persistence.pending_sample_indices.size();
    task.persistence.save_state.kind = SampleLabelSaveStateKind::Pending;
    task.persistence.save_state.message_kind = SampleLabelSaveMessageKind::None;
    task.persistence.save_state.message.clear();
}


// Historical fixture writer; never linked into the application.
inline SampleLabelOutputPublicationResult
PublishLegacyFixture(
    SampleLabelingTask& task,
    const SampleLabelResultMetadataSource* source = nullptr)
{
    SampleLabelOutputPublicationResult result;
    if (!task.persistence.output_path) {
        return result;
    }
    if (task.persistence.output_format !=
        SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar) {
        result.message =
            "labeling output format is not owned by the legacy NPY writer";
        MarkSampleLabelTaskSaveFailed(task, result.message);
        return result;
    }

    result.attempted = true;
    const SampleLabelResultWriteOutcome write =
        LegacyFixtureIo{}.SaveLabelResult(*task.persistence.output_path, task, source);
    result.artifacts_replaced = write.array_saved;
    if (!write.array_saved) {
        result.retryable = true;
        if (write.message.empty()) {
            result.message = "could not save label output";
            MarkSampleLabelTaskSaveFailed(
                task,
                {},
                SampleLabelSaveMessageKind::OutputSaveFailed);
        } else {
            result.message = write.message;
            MarkSampleLabelTaskSaveFailed(task, result.message);
        }
        return result;
    }

    task.persistence.pending_sample_indices.clear();
    task.persistence.metadata_save_pending = true;

    result.published = write.metadata_saved;
    if (write.metadata_saved) {
        MarkSampleLabelTaskPersisted(task, SampleLabelSaveStateKind::AutosavedToOutput);
    } else {
        result.retryable = true;
        if (write.message.empty()) {
            result.message =
                "could not save label output metadata";
            MarkSampleLabelTaskSaveFailed(
                task,
                {},
                SampleLabelSaveMessageKind::OutputSaveFailed);
        } else {
            result.message = write.message;
            MarkSampleLabelTaskSaveFailed(task, result.message);
        }
    }
    return result;
}

}  // namespace spectiary::test_support
