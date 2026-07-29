#include "domain/sample_filter.h"
#include "domain/sample_annotation_io.h"
#include "domain/sample_labeling.h"
#include "ui/sample_labeling_controller.h"
#include "ui/sample_labeling_state_cache_io.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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

void RunMaintenanceUntilIdle(specforge::SampleLabelingController& controller)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        const auto deadline = controller.NextMaintenanceDeadline();
        if (!deadline) {
            return;
        }
        controller.RunMaintenance(*deadline);
    }
    Require(
        !controller.NextMaintenanceDeadline(),
        "sample-labeling maintenance should converge after successful persistence");
}

const specforge::SampleLabelingTask* ActiveTask(
    const specforge::SampleLabelingController& controller)
{
    return controller.View().active_task;
}

const specforge::SampleLabelingTask* TemporaryTask(
    const specforge::SampleLabelingController& controller)
{
    return controller.View().temporary_task;
}

const std::vector<specforge::SampleLabelingTask>* ActiveSourceTasks(
    const specforge::SampleLabelingController& controller)
{
    return controller.View().active_source_tasks;
}

std::int32_t ReadLittleEndianI32(const std::array<unsigned char, 4>& bytes)
{
    const std::uint32_t value = static_cast<std::uint32_t>(bytes[0]) |
                                (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                                (static_cast<std::uint32_t>(bytes[2]) << 16U) |
                                (static_cast<std::uint32_t>(bytes[3]) << 24U);
    return static_cast<std::int32_t>(value);
}

std::vector<std::int32_t> ReadTestInt32NpyPayload(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open written NPY file");

    std::array<unsigned char, 6> magic = {};
    stream.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
    constexpr std::array<unsigned char, 6> kExpectedMagic = {0x93, 'N', 'U', 'M', 'P', 'Y'};
    Require(magic == kExpectedMagic, "written NPY should have the expected magic");

    std::array<unsigned char, 2> version = {};
    stream.read(reinterpret_cast<char*>(version.data()), static_cast<std::streamsize>(version.size()));
    Require(version[0] == 1 && version[1] == 0, "written NPY should use v1.0");

    std::array<unsigned char, 2> length_bytes = {};
    stream.read(reinterpret_cast<char*>(length_bytes.data()), static_cast<std::streamsize>(length_bytes.size()));
    const std::uint16_t header_length =
        static_cast<std::uint16_t>(length_bytes[0]) | (static_cast<std::uint16_t>(length_bytes[1]) << 8U);
    std::string header(header_length, '\0');
    stream.read(header.data(), static_cast<std::streamsize>(header.size()));
    Require(header.find("'descr': '<i4'") != std::string::npos, "written NPY should be int32");
    Require(header.find("'shape': (3,)") != std::string::npos, "written NPY should preserve sample count");

    std::vector<std::int32_t> values;
    for (;;) {
        std::array<unsigned char, 4> bytes = {};
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (stream.gcount() == 0) {
            break;
        }
        Require(stream.gcount() == static_cast<std::streamsize>(bytes.size()), "written NPY payload is truncated");
        values.push_back(ReadLittleEndianI32(bytes));
    }
    return values;
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    Require(stream.good(), "could not open text file");
    std::string contents;
    std::string line;
    while (std::getline(stream, line)) {
        contents += line;
        contents += '\n';
    }
    return contents;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

void WriteTextFile(const std::filesystem::path& path, std::string_view contents)
{
    std::ofstream stream(path, std::ios::trunc);
    Require(stream.good(), "could not open text file for writing");
    stream << contents;
    Require(stream.good(), "could not write text file");
}

void TestSampleLabelingStateCacheRoundTrip()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_adapter_roundtrip.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);

    specforge::SampleLabelingTask task = specforge::CreateSampleLabelingTask("quality", "Quality", 3);
    Require(
        specforge::UpsertSampleLabel(task.label_set, specforge::SampleLabelDefinition{5, "bad", 'b'}),
        "adapter fixture should accept label");
    task.auto_advance = true;
    task.skip_labeled_on_advance = true;
    Require(specforge::AssignSampleLabel(task, 1, 5).accepted, "adapter fixture should accept sample value");
    specforge::MarkSampleLabelTaskPersisted(task, specforge::SampleLabelSaveStateKind::InternalDraftOnly);
    task.remembered_position = 2;

    specforge::SampleLabelingStateCache cache;
    specforge::SampleLabelingSourceState state;
    state.sample_count = 3;
    state.active_task_id = "quality";
    state.tasks.push_back(std::move(task));
    cache.sources.emplace("source-identity", std::move(state));
    Require(specforge::SaveSampleLabelingStateCache(cache_path, cache), "sample-labeling cache should save");

    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(cache_path);
    Require(loaded.warning.empty(), loaded.warning);
    const auto source = loaded.cache.sources.find("source-identity");
    Require(source != loaded.cache.sources.end(), "sample-labeling cache should restore source state");
    Require(source->second.active_task_id && *source->second.active_task_id == "quality", "active task should restore");
    Require(source->second.tasks.size() == 1, "task should restore");
    const specforge::SampleLabelingTask& restored_task = source->second.tasks.front();
    Require(restored_task.auto_advance, "auto-advance should round-trip");
    Require(restored_task.skip_labeled_on_advance, "skip-labeled setting should round-trip");
    Require(
        restored_task.remembered_position && *restored_task.remembered_position == 2,
        "remembered position should round-trip");
    Require(
        specforge::FindSampleLabel(restored_task.label_set, 5) != nullptr,
        "label set should round-trip");
    Require(restored_task.values.size() == 3 && restored_task.values[1] == 5, "draft values should round-trip");
}

void TestSampleLabelingStateCacheReportsCorruptJson()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_adapter_corrupt.json";
    WriteTextFile(cache_path, "{ invalid json");

    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(cache_path);
    Require(!loaded.warning.empty(), "corrupt sample-labeling cache should report a warning");
    Require(loaded.cache.sources.empty(), "corrupt sample-labeling cache should be ignored");
}

void TestSampleLabelingStateCacheReportsUnsupportedSchema()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_adapter_schema.json";
    WriteTextFile(
        cache_path,
        "{\n"
        "  \"format_kind\": \"specforge.sample_labeling_tasks.cache\",\n"
        "  \"schema_version\": 999,\n"
        "  \"sources\": []\n"
        "}\n");

    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(cache_path);
    Require(!loaded.warning.empty(), "unsupported sample-labeling cache schema should report a warning");
    Require(loaded.cache.sources.empty(), "unsupported sample-labeling cache schema should be ignored");
}

void TestSampleLabelTaskWritesStableCodes()
{
    specforge::SampleLabelingTask task = specforge::CreateSampleLabelingTask("quality", "Quality", 4);

    Require(
        specforge::UpsertSampleLabel(task.label_set, specforge::SampleLabelDefinition{1, "bad", 'b'}),
        "first label should be accepted");
    Require(
        specforge::UpsertSampleLabel(task.label_set, specforge::SampleLabelDefinition{2, "good", 'g'}),
        "second label should be accepted");
    Require(
        specforge::UpsertSampleLabel(task.label_set, specforge::SampleLabelDefinition{3, "borderline", 'B'}),
        "duplicate shortcut should rebind to the edited label");
    Require(
        specforge::SampleLabelCodeForShortcut(task.label_set, 'b') == 3,
        "shortcut should resolve to the latest bound label");
    const specforge::SampleLabelDefinition* bad_label = specforge::FindSampleLabel(task.label_set, 1);
    Require(bad_label != nullptr && bad_label->shortcut == '\0', "previous duplicate shortcut should be unbound");
    Require(specforge::NextAvailableSampleLabelCode(task.label_set) == 0, "next suggested code should be unused");

    task.auto_advance = true;
    specforge::SampleLabelWriteResult result = specforge::AssignSampleLabel(task, 1, 2);
    Require(result.accepted, "known label code should be accepted");
    Require(result.changed, "first write should change the sample");
    Require(result.advance_requested, "auto-advance should request navigation after a label write");
    Require(task.values[1] == 2, "label write should store the stable code");
    Require(task.remembered_position && *task.remembered_position == 1, "label write should remember position");
    Require(result.pending_count == 1, "first write should mark one pending sample");

    result = specforge::AssignSampleLabel(task, 1, 2);
    Require(result.accepted, "same label write should still be accepted");
    Require(!result.changed, "same label write should not report a value change");
    Require(result.pending_count == 1, "rewriting one sample should not double-count pending samples");

    result = specforge::ClearSampleLabel(task, 1);
    Require(result.accepted, "clear should be accepted for an in-range sample");
    Require(task.values[1] == specforge::kUnlabeledSampleLabelCode, "clear should write the unlabeled sentinel");
    Require(result.pending_count == 1, "clear should keep the same pending sample count");

    specforge::MarkSampleLabelTaskPersisted(task, specforge::SampleLabelSaveStateKind::InternalDraftOnly);
    Require(specforge::AssignSampleLabel(task, 2, 3).accepted, "test sample should be labelable before no-op check");
    specforge::MarkSampleLabelTaskPersisted(task, specforge::SampleLabelSaveStateKind::InternalDraftOnly);
    result = specforge::AssignSampleLabel(task, 2, 3);
    Require(result.accepted, "same label write should still be accepted");
    Require(!result.changed, "same clean label write should report no value change");
    Require(!result.advance_requested, "same clean label write should not request navigation");
    Require(task.pending_sample_indices.empty(), "same clean label write should not mark pending samples");
    Require(result.pending_count == 0, "same clean label write should not report pending samples");

    result = specforge::ClearSampleLabel(task, 3);
    Require(result.accepted, "clearing an already unlabeled sample should be accepted");
    Require(!result.changed, "clearing an already unlabeled sample should report no change");
    Require(task.pending_sample_indices.empty(), "clearing an already unlabeled sample should not mark pending samples");

    result = specforge::AssignSampleLabel(task, 2, 42);
    Require(!result.accepted, "unknown label code should not be accepted");
    Require(task.values[2] == 3, "unknown label code must not mutate values");

    specforge::MarkSampleLabelTaskPersisted(task, specforge::SampleLabelSaveStateKind::InternalDraftOnly);
    Require(task.pending_sample_indices.empty(), "persisting should clear pending samples");
    Require(
        task.save_state.kind == specforge::SampleLabelSaveStateKind::InternalDraftOnly,
        "draft persistence should keep the internal-draft state");
}

void TestRemovingSampleLabelClearsAssignedValues()
{
    specforge::SampleLabelingTask task = specforge::CreateSampleLabelingTask("quality", "Quality", 3);
    Require(
        specforge::UpsertSampleLabel(task.label_set, specforge::SampleLabelDefinition{1, "used", 'u'}),
        "used label should be accepted");
    Require(
        specforge::UpsertSampleLabel(task.label_set, specforge::SampleLabelDefinition{2, "unused", 'n'}),
        "unused label should be accepted");
    Require(specforge::AssignSampleLabel(task, 0, 1).accepted, "fixture should assign the used label");

    Require(specforge::RemoveSampleLabel(task, 1), "a used label should be removable");
    Require(specforge::FindSampleLabel(task.label_set, 1) == nullptr, "used label definition should be removed");
    Require(
        task.values[0] == specforge::kUnlabeledSampleLabelCode,
        "samples using a removed label should become unlabeled");
    Require(task.pending_sample_indices.contains(0), "cleared sample values should be pending persistence");

    Require(specforge::RemoveSampleLabel(task, 2), "an unused label should be removable");
    Require(specforge::FindSampleLabel(task.label_set, 2) == nullptr, "removed label should leave the label set");
    Require(!specforge::RemoveSampleLabel(task, 2), "removing an unknown label should be a no-op");
}

void TestChangingUnusedSampleLabelCode()
{
    specforge::SampleLabelingTask task = specforge::CreateSampleLabelingTask("quality", "Quality", 3);
    Require(
        specforge::UpsertSampleLabel(task.label_set, specforge::SampleLabelDefinition{1, "review", 'r'}),
        "fixture label should be accepted");

    Require(
        specforge::UpdateSampleLabel(
            task,
            1,
            specforge::SampleLabelDefinition{7, "accepted", 'a'},
            false),
        "an unused label code should change without confirmation");
    Require(specforge::FindSampleLabel(task.label_set, 1) == nullptr, "the old unused code should disappear");
    const specforge::SampleLabelDefinition* updated = specforge::FindSampleLabel(task.label_set, 7);
    Require(updated != nullptr, "the new unused code should be present");
    Require(updated->name == "accepted" && updated->shortcut == 'a', "the label edit should be atomic");
    Require(task.pending_sample_indices.empty(), "changing an unused code should not dirty sample values");
}

void TestChangingUsedSampleLabelCodeRequiresConfirmation()
{
    specforge::SampleLabelingTask task = specforge::CreateSampleLabelingTask("quality", "Quality", 3);
    Require(
        specforge::UpsertSampleLabel(task.label_set, specforge::SampleLabelDefinition{1, "review", 'r'}),
        "used fixture label should be accepted");
    Require(
        specforge::UpsertSampleLabel(task.label_set, specforge::SampleLabelDefinition{2, "other", 'o'}),
        "occupied target fixture should be accepted");
    Require(specforge::AssignSampleLabel(task, 0, 1).accepted, "first fixture sample should be labeled");
    Require(specforge::AssignSampleLabel(task, 2, 1).accepted, "third fixture sample should be labeled");
    specforge::MarkSampleLabelTaskPersisted(task, specforge::SampleLabelSaveStateKind::InternalDraftOnly);

    Require(
        !specforge::UpdateSampleLabel(
            task,
            1,
            specforge::SampleLabelDefinition{7, "accepted", 'a'},
            false),
        "a used label code should require confirmation");
    Require(specforge::FindSampleLabel(task.label_set, 1) != nullptr, "rejected recode should keep the old label");
    Require(task.values == std::vector<int>({1, -1, 1}), "rejected recode should keep sample values");

    Require(
        specforge::UpdateSampleLabel(
            task,
            1,
            specforge::SampleLabelDefinition{7, "accepted", 'a'},
            true),
        "confirmation should allow a used label code to change");
    Require(specforge::FindSampleLabel(task.label_set, 1) == nullptr, "confirmed recode should remove the old code");
    Require(specforge::FindSampleLabel(task.label_set, 7) != nullptr, "confirmed recode should add the new code");
    Require(task.values == std::vector<int>({7, -1, 7}), "confirmed recode should migrate assigned values");
    Require(
        task.pending_sample_indices == std::unordered_set<std::size_t>({0, 2}),
        "confirmed recode should dirty every migrated sample value");

    Require(
        !specforge::UpdateSampleLabel(
            task,
            7,
            specforge::SampleLabelDefinition{2, "collision", 'c'},
            true),
        "confirmation should not overwrite another label code");

    task.values[1] = 9;
    Require(
        !specforge::UpdateSampleLabel(
            task,
            7,
            specforge::SampleLabelDefinition{9, "missing metadata collision", 'm'},
            true),
        "confirmation should not overwrite a code already present in sample values");
    Require(
        specforge::FindSampleLabel(task.label_set, 7) != nullptr &&
            specforge::FindSampleLabel(task.label_set, 9) == nullptr,
        "a sample-value collision should leave label definitions unchanged");
}

void TestSampleLabelResultWritesCompactNpy()
{
    specforge::SampleLabelingTask task = specforge::CreateSampleLabelingTask("quality", "Quality", 3);
    Require(specforge::UpsertSampleLabel(task.label_set, specforge::SampleLabelDefinition{5, "bad", 'b'}), "label should be accepted");
    Require(specforge::AssignSampleLabel(task, 0, 5).accepted, "first sample should be labelable");
    Require(specforge::AssignSampleLabel(task, 2, 5).accepted, "third sample should be labelable");

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_sample_label_result.npy";
    std::string error;
    Require(
        specforge::SampleAnnotationIoAdapter{}.SaveLabelArray(path, task, &error),
        error.empty() ? "NPY save failed" : error);
    const std::vector<std::int32_t> values = ReadTestInt32NpyPayload(path);
    Require(values.size() == 3, "written NPY should have one value per sample");
    Require(values[0] == 5 && values[1] == -1 && values[2] == 5, "written NPY should preserve label codes and sentinel");
}

void TestSampleLabelResultWritesMetadataSidecar()
{
    specforge::SampleLabelingTask task = specforge::CreateSampleLabelingTask("quality", "Quality", 3);
    Require(specforge::UpsertSampleLabel(task.label_set, specforge::SampleLabelDefinition{5, "bad", 'b'}), "label should be accepted");
    Require(specforge::AssignSampleLabel(task, 0, 5).accepted, "first sample should be labelable");
    Require(specforge::AssignSampleLabel(task, 2, 5).accepted, "third sample should be labelable");

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_sample_label_result_metadata.npy";
    const std::filesystem::path metadata_path =
        specforge::SampleAnnotationIoAdapter::MetadataPathForResult(path);
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
    std::filesystem::remove(metadata_path, cleanup_error);

    specforge::SelectSampleLabelTaskOutputPath(task, path);
    specforge::SampleLabelResultMetadataSource source;
    source.source_name = "source.npy";
    source.source_fingerprint = "size=12;mtime=1;dtype=<f8;shape=3x2";
    source.context_fingerprint = "context";
    source.spectrum_count = 3;
    const specforge::SampleLabelTaskPersistResult result =
        specforge::PersistSampleLabelingTaskResult(task, &source);
    Require(result.output_saved, result.message.empty() ? "metadata-backed output should save" : result.message);
    Require(task.pending_sample_indices.empty(), "successful metadata-backed output should clear pending samples");
    Require(!task.metadata_save_pending, "successful metadata-backed output should clear metadata pending");
    Require(
        task.save_state.kind == specforge::SampleLabelSaveStateKind::AutosavedToOutput,
        "successful metadata-backed output should be clean");

    const std::string metadata = ReadTextFile(metadata_path);
    Require(
        metadata.find("\"format_kind\": \"specforge.sample_label_result.metadata\"") != std::string::npos,
        "metadata should identify the sidecar format");
    Require(metadata.find("\"result_file\": \"specforge_sample_label_result_metadata.npy\"") != std::string::npos, "metadata should reference the result relatively");
    Require(metadata.find("\"task_id\": \"quality\"") != std::string::npos, "metadata should carry the stable task id");
    Require(metadata.find("\"expected_dtype\": \"int32\"") != std::string::npos, "metadata should carry the expected dtype");
    Require(metadata.find("\"source_name\": \"source.npy\"") != std::string::npos, "metadata should carry source summary");
    Require(metadata.find(PathToUtf8(std::filesystem::temp_directory_path())) == std::string::npos, "metadata must not store a local absolute source path");
}

void TestAnnotationAdapterRoundTripsLabelArtifacts()
{
    specforge::SampleLabelingTask task =
        specforge::CreateSampleLabelingTask("round-trip", "Round trip", 3);
    Require(
        specforge::UpsertSampleLabel(
            task.label_set,
            specforge::SampleLabelDefinition{9, "accepted", 'a'}),
        "round-trip label should be accepted");
    Require(
        specforge::AssignSampleLabel(task, 1, 9).accepted,
        "round-trip sample should be labelable");

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "specforge_annotation_adapter_round_trip.npy";
    const std::filesystem::path metadata_path =
        specforge::SampleAnnotationIoAdapter::MetadataPathForResult(path);
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
    std::filesystem::remove(metadata_path, cleanup_error);

    specforge::SampleLabelResultMetadataSource source;
    source.source_name = "source.npy";
    source.source_fingerprint = "source-fingerprint";
    source.context_fingerprint = "context-fingerprint";
    source.spectrum_count = 3;

    const specforge::SampleAnnotationIoAdapter adapter;
    const specforge::SampleLabelResultWriteOutcome saved =
        adapter.SaveLabelResult(path, task, &source);
    Require(saved.array_saved, saved.message.empty() ? "adapter array should save" : saved.message);
    Require(saved.metadata_saved, saved.message.empty() ? "adapter metadata should save" : saved.message);

    std::string error;
    const std::optional<specforge::LoadedSampleLabelResult> loaded =
        adapter.LoadLabelResult(path, 3, {}, &error);
    Require(loaded.has_value(), error.empty() ? "adapter label result should load" : error);
    Require(
        loaded->values == task.values,
        "adapter round-trip should preserve every int32 label value");
    Require(
        loaded->metadata_sidecar_exists && loaded->metadata &&
            loaded->metadata->task_id == task.task_id,
        "adapter round-trip should pair the matching sidecar");
    Require(
        loaded->metadata->source &&
            loaded->metadata->source->context_fingerprint == source.context_fingerprint,
        "adapter round-trip should preserve source metadata");

    std::filesystem::remove(path, cleanup_error);
    std::filesystem::remove(metadata_path, cleanup_error);
}

void TestAnnotationAdapterRejectsEmptyTaskIdBeforeWriting()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "specforge_annotation_adapter_empty_task_id.npy";
    const std::filesystem::path metadata_path =
        specforge::SampleAnnotationIoAdapter::MetadataPathForResult(path);
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
    std::filesystem::remove(metadata_path, cleanup_error);

    const specforge::SampleLabelingTask task =
        specforge::CreateSampleLabelingTask("", "Missing identity", 3);
    const specforge::SampleAnnotationIoAdapter adapter;
    const specforge::SampleLabelResultWriteOutcome outcome =
        adapter.SaveLabelResult(path, task);

    Require(
        !outcome.array_saved && !outcome.metadata_saved,
        "empty task identity should reject the label-result pair before either file is written");
    Require(
        outcome.message.find("task id") != std::string::npos,
        "empty task identity should report the violated metadata contract");
    Require(
        !std::filesystem::exists(path) && !std::filesystem::exists(metadata_path),
        "empty task identity should not leave a partial label-result pair");

    std::string error;
    Require(
        !adapter.SaveLabelMetadata(path, task, nullptr, &error),
        "metadata-only save should reject an empty task identity");
    Require(
        error.find("task id") != std::string::npos,
        "metadata-only save should report the same task identity contract");
    Require(
        !std::filesystem::exists(metadata_path),
        "rejected metadata-only save should not create a sidecar");
}

void TestSampleAnnotationUsesMatchingLabelMetadata()
{
    specforge::SampleLabelingTask task = specforge::CreateSampleLabelingTask("quality", "Quality", 3);
    Require(specforge::UpsertSampleLabel(task.label_set, specforge::SampleLabelDefinition{5, "bad", 'b'}), "label should be accepted");
    Require(specforge::AssignSampleLabel(task, 0, 5).accepted, "first sample should be labelable");

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_sample_annotation_metadata.npy";
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
    std::filesystem::remove(
        specforge::SampleAnnotationIoAdapter::MetadataPathForResult(path),
        cleanup_error);

    specforge::SelectSampleLabelTaskOutputPath(task, path);
    const specforge::SampleLabelTaskPersistResult saved = specforge::PersistSampleLabelingTaskResult(task);
    Require(saved.output_saved, saved.message.empty() ? "label result should save" : saved.message);

    std::string error;
    std::optional<specforge::SampleAnnotationResult> annotation =
        specforge::SampleAnnotationIoAdapter{}.Load(path, 3, &error);
    Require(annotation.has_value(), error.empty() ? "metadata-backed annotation should load" : error);
    Require(
        annotation->relationship == specforge::SampleAnnotationWorkflowRelationship::ExternalLabelResult,
        "matching metadata should mark annotation as an external label result");
    Require(annotation->name == "Quality", "metadata task name should become the row name");
    Require(
        specforge::FormatSampleAnnotationValue(*annotation, annotation->values[0]) == "bad (5)",
        "metadata should map numeric codes to labels");
    Require(
        specforge::FormatSampleAnnotationValue(*annotation, annotation->values[1]) ==
            "Unlabeled (-1)",
        "metadata should map the unlabeled sentinel");
    Require(annotation->label_metadata && annotation->label_metadata->task_id == "quality", "metadata should be attached to the annotation");
}

void TestMismatchedLabelMetadataFallsBackToRawAnnotationValues()
{
    specforge::SampleLabelingTask task = specforge::CreateSampleLabelingTask("quality", "Quality", 3);
    Require(specforge::UpsertSampleLabel(task.label_set, specforge::SampleLabelDefinition{5, "bad", 'b'}), "label should be accepted");
    Require(specforge::AssignSampleLabel(task, 0, 5).accepted, "first sample should be labelable");

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_sample_annotation_bad_metadata.npy";
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
    const std::filesystem::path metadata_path =
        specforge::SampleAnnotationIoAdapter::MetadataPathForResult(path);
    std::filesystem::remove(metadata_path, cleanup_error);
    std::string error;
    Require(
        specforge::SampleAnnotationIoAdapter{}.SaveLabelArray(path, task, &error),
        error.empty() ? "NPY save failed" : error);
    WriteTextFile(
        metadata_path,
        "{\n"
        "  \"format_kind\": \"specforge.sample_label_result.metadata\",\n"
        "  \"schema_version\": 1,\n"
        "  \"result_file\": \"another.npy\",\n"
        "  \"task_id\": \"quality\",\n"
        "  \"value_count\": 3,\n"
        "  \"expected_dtype\": \"int32\",\n"
        "  \"unlabeled_sentinel\": -1,\n"
        "  \"task_name\": \"Quality\",\n"
        "  \"labels\": [{ \"code\": 5, \"name\": \"bad\", \"shortcut\": \"b\" }]\n"
        "}\n");

    std::optional<specforge::SampleAnnotationResult> annotation =
        specforge::SampleAnnotationIoAdapter{}.Load(path, 3, &error);
    Require(annotation.has_value(), error.empty() ? "annotation should still load" : error);
    Require(
        annotation->relationship == specforge::SampleAnnotationWorkflowRelationship::PlainAnnotation,
        "mismatched metadata should not be applied");
    Require(
        specforge::FormatSampleAnnotationValue(*annotation, annotation->values[0]) == "5",
        "mismatched metadata should fall back to raw numeric value");
    Require(!annotation->metadata_warning.empty(), "mismatched metadata should produce a warning");
}

void TestFailedNpySaveDoesNotDamageExistingOutput()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_sample_label_result_atomic_failure.npy";
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);

    specforge::SampleLabelingTask original = specforge::CreateSampleLabelingTask("quality", "Quality", 3);
    Require(specforge::UpsertSampleLabel(original.label_set, specforge::SampleLabelDefinition{5, "bad", 'b'}), "label should be accepted");
    Require(specforge::AssignSampleLabel(original, 0, 5).accepted, "original output should be labelable");
    std::string error;
    const specforge::SampleAnnotationIoAdapter adapter;
    Require(
        adapter.SaveLabelArray(path, original, &error),
        error.empty() ? "initial NPY save failed" : error);

    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::group_read |
            std::filesystem::perms::others_read,
        std::filesystem::perm_options::replace,
        cleanup_error);

    specforge::SampleLabelingTask replacement = specforge::CreateSampleLabelingTask("quality", "Quality", 3);
    Require(specforge::UpsertSampleLabel(replacement.label_set, specforge::SampleLabelDefinition{7, "good", 'g'}), "replacement label should be accepted");
    Require(specforge::AssignSampleLabel(replacement, 1, 7).accepted, "replacement output should be labelable");
    error.clear();
    Require(
        !adapter.SaveLabelArray(path, replacement, &error),
        "read-only target should reject replacement");

    const std::vector<std::int32_t> values = ReadTestInt32NpyPayload(path);
    Require(values.size() == 3, "existing output should still be readable after failed save");
    Require(values[0] == 5 && values[1] == -1 && values[2] == -1, "failed save must not damage existing output");

    std::filesystem::permissions(path, std::filesystem::perms::owner_all, std::filesystem::perm_options::add, cleanup_error);
}

void TestSampleLabelingControllerAutosavesDraftRecord()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_task_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);

    {
        specforge::SampleLabelingController controller(cache_path);
        controller.ActivateSource("source-identity", 3);
        Require(controller.CreateTask("quality", "Quality").accepted, "controller should create an active task");
        Require(
            controller.UpsertActiveLabel(specforge::SampleLabelDefinition{5, "bad", 'b'}).changed,
            "controller should persist label-set edits");
        const specforge::SampleLabelingTask* task = ActiveTask(controller);
        Require(task != nullptr, "task should remain active after label edit");
        const specforge::SampleLabelingOperationResult setting =
            controller.SetActiveAutoAdvance(true);
        Require(setting.changed && setting.state_saved, "workflow setting should persist to local task record");
        Require(controller.AssignLabel(1, 5).write.accepted, "controller should label a sample");
        Require(controller.state_save_pending(), "sample label writes should queue a debounced local task save");
        const auto deadline = controller.NextMaintenanceDeadline();
        Require(deadline.has_value(), "queued sample-labeling save should expose its deadline");
        controller.RunMaintenance(*deadline - std::chrono::milliseconds(1));
        Require(controller.state_save_pending(), "debounced local task save should not flush immediately");
        controller.RunMaintenance(*deadline);
        Require(!controller.state_save_pending(), "debounced local task save should flush after the delay");
    }

    Require(std::filesystem::exists(cache_path), "autosave should create a local task record");

    {
        specforge::SampleLabelingController restored(cache_path);
        restored.ActivateSource("source-identity", 3);
        const specforge::SampleLabelingTask* task = ActiveTask(restored);
        Require(task != nullptr, "controller should restore active task from local record");
        Require(task->task_id == "quality", "restored task id should match");
        Require(task->auto_advance, "restored workflow setting should match");
        Require(specforge::FindSampleLabel(task->label_set, 5) != nullptr, "restored label set should include saved code");
        Require(task->values.size() == 3 && task->values[1] == 5, "restored draft should include saved sample value");
        Require(task->pending_sample_indices.empty(), "restored autosave should be clean");
        Require(
            task->save_state.kind == specforge::SampleLabelSaveStateKind::InternalDraftOnly,
            "restored draft should report internal autosave state");
        Require(
            restored.RememberActivePosition(2).changed,
            "remembered position should be persisted through controller");
        Require(restored.FlushStateCache(), "remembered position should flush to local task record");
    }

    {
        specforge::SampleLabelingController restored(cache_path);
        restored.ActivateSource("source-identity", 3);
        const specforge::SampleLabelingTask* task = ActiveTask(restored);
        Require(task != nullptr, "task should restore after remembered-position update");
        Require(task->remembered_position && *task->remembered_position == 2, "remembered position should restore");
    }
}

void TestTaskRecordFlushKeepsActiveTaskAddressStable()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_pointer_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);

    specforge::SampleLabelingController controller(cache_path);
    controller.ActivateSource("source-identity", 3);
    Require(controller.CreateTask("quality", "Quality").accepted, "controller should create task");
    Require(
        controller.UpsertActiveLabel(specforge::SampleLabelDefinition{5, "bad", 'b'}).changed,
        "controller should add label");
    const specforge::SampleLabelingTask* task = ActiveTask(controller);
    Require(task != nullptr, "task should remain active after label edit");
    Require(controller.AssignLabel(1, 5).write.accepted, "controller should label a draft sample");

    const specforge::SampleLabelingTask* before_flush = ActiveTask(controller);
    Require(before_flush != nullptr, "active task should exist before flush");
    Require(controller.FlushStateCache(), "local task record should flush");
    const specforge::SampleLabelingTask* after_flush = ActiveTask(controller);
    Require(after_flush == before_flush, "flushing local task record must not invalidate active task pointer");
}

void TestControllerRevisionTracksOwnedTaskChanges()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_revision_state.json";
    const std::filesystem::path annotation_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_revision_annotation.npy";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    std::filesystem::remove(annotation_path, cleanup_error);

    specforge::SampleLabelingController controller(cache_path);
    const std::uint64_t initial_revision = controller.View().revision;
    const std::uint64_t initial_generation =
        controller.active_source_tasks_generation();
    controller.ActivateSource("source-identity", 3);
    const std::uint64_t source_revision = controller.View().revision;
    const std::uint64_t source_generation =
        controller.active_source_tasks_generation();
    Require(source_revision > initial_revision, "source activation should advance labeling revision");
    Require(
        source_generation > initial_generation,
        "source activation should advance the active task projection generation");

    controller.ActivateSource("source-identity", 3);
    Require(
        controller.active_source_tasks_generation() ==
            source_generation,
        "repeating an unchanged source activation should preserve the task projection cache generation");

    const specforge::SampleLabelingOperationResult created =
        controller.CreateTask("quality", "Quality");
    Require(created.accepted && created.changed, "task creation should report an owned mutation");
    Require(created.revision > source_revision, "task creation should advance labeling revision");
    Require(controller.View().revision == created.revision, "operation revision should match immutable view revision");
    const std::uint64_t created_generation =
        controller.active_source_tasks_generation();
    Require(
        created_generation == source_generation,
        "an internal draft should not invalidate filter and sorting projections");

    const specforge::SampleLabelingOperationResult no_change =
        controller.SetActiveAutoAdvance(false);
    Require(no_change.accepted && !no_change.changed, "setting an existing value should report a no-op");
    Require(no_change.revision == created.revision, "no-op should not invalidate labeling projections");
    Require(
        controller.active_source_tasks_generation() ==
            created_generation,
        "a task no-op should preserve the task projection generation");

    const specforge::SampleLabelingOperationResult changed =
        controller.SetActiveAutoAdvance(true);
    Require(changed.changed && changed.state_saved, "workflow setting mutation should own its record save");
    Require(changed.revision > no_change.revision, "workflow setting mutation should advance revision");
    Require(controller.View().revision == changed.revision, "saved mutation revision should include persistence state");
    const specforge::SampleLabelingTask* task = ActiveTask(controller);
    Require(task != nullptr && task->auto_advance, "borrowed read-only view should expose the committed setting");

    Require(
        controller.active_source_tasks_generation() ==
            created_generation,
        "auto-advance should not invalidate filter and sorting projections");
    Require(
        controller.SetActiveSkipLabeledOnAdvance(true).changed,
        "skip-labeled setting should report a mutation");
    Require(
        controller.RememberActivePosition(2).changed,
        "remembered position should report a mutation");
    Require(controller.FlushStateCache(), "remembered position should flush");
    Require(
        controller.active_source_tasks_generation() ==
            created_generation,
        "workflow settings, navigation position, and persistence should preserve the task projection generation");

    specforge::SampleLabelSet label_set;
    label_set.labels.push_back(
        specforge::SampleLabelDefinition{5, "review", 'r'});
    const specforge::SampleLabelingOperationResult
        formal_task =
            controller.CreateTaskFromAnnotation(
                "formal",
                "Formal",
                std::move(label_set),
                {-1, -1, -1},
                annotation_path,
                true);
    Require(
        formal_task.accepted && formal_task.changed,
        "output-backed task creation should report a mutation");
    const std::uint64_t projection_generation =
        controller.active_source_tasks_generation();
    Require(
        projection_generation > created_generation,
        "an output-backed task should invalidate filter and sorting projections");

    Require(
        controller.DeactivateActiveTask().changed,
        "clean output-backed task should deactivate");
    Require(
        controller.active_source_tasks_generation() ==
            projection_generation,
        "active task selection should preserve the task projection generation");
    Require(
        controller.ActivateTask("formal").changed,
        "formal task should reactivate");
    Require(
        controller.active_source_tasks_generation() ==
            projection_generation,
        "task activation should preserve the task projection generation");

    controller.ClearActiveSource();
    Require(
        controller.active_source_tasks_generation() >
            projection_generation,
        "clearing the active source should advance the task projection generation");
    const std::uint64_t inactive_revision = controller.View().revision;
    const specforge::SampleLabelingWriteOperationResult rejected_assign =
        controller.AssignLabel(0, 1);
    Require(!rejected_assign.write.accepted, "assign without an active task should be rejected");
    Require(
        rejected_assign.operation.revision == inactive_revision,
        "rejected assign should report the controller's current revision");
    const specforge::SampleLabelingWriteOperationResult rejected_clear =
        controller.ClearLabel(0);
    Require(!rejected_clear.write.accepted, "clear without an active task should be rejected");
    Require(
        rejected_clear.operation.revision == inactive_revision,
        "rejected clear should report the controller's current revision");
}

void TestControllerKeepsOneTemporaryTaskPerSource()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_single_temporary_state.json";
    const std::filesystem::path output_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_formalized_result.npy";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    std::filesystem::remove(output_path, cleanup_error);
    std::filesystem::remove(
        specforge::SampleAnnotationIoAdapter::MetadataPathForResult(output_path),
        cleanup_error);

    specforge::SampleLabelingController controller(cache_path);
    controller.ActivateSource("source-identity", 3);
    Require(
        controller.CreateTask("temporary", "Temporary labeling task").accepted,
        "controller should create the first temporary task");
    const specforge::SampleLabelingTask* first = ActiveTask(controller);
    Require(first != nullptr, "created temporary task should be active");
    const std::string temporary_task_id = first->task_id;
    Require(
        controller.UpsertActiveLabel(specforge::SampleLabelDefinition{5, "review", 'r'}).changed,
        "temporary task should accept a label definition");
    Require(controller.AssignLabel(1, 5).write.accepted, "temporary task should accept writes");
    Require(controller.DeactivateActiveTask().changed, "temporary task should be pausable");

    Require(
        controller.CreateTask("another", "Another temporary task").accepted,
        "create should resume the existing temporary task");
    const specforge::SampleLabelingTask* resumed = ActiveTask(controller);
    Require(resumed != nullptr && resumed->task_id == temporary_task_id, "create should resume the existing temporary task");
    Require(resumed->values[1] == 5, "resumed temporary task should keep its draft values");
    const std::vector<specforge::SampleLabelingTask>* tasks = ActiveSourceTasks(controller);
    Require(tasks != nullptr && tasks->size() == 1, "source should keep only one temporary task");

    const specforge::SampleLabelingOperationResult save =
        controller.SaveActiveTemporaryTaskToOutput(output_path, "Temporary labeling task");
    Require(save.output_saved && save.state_saved, "formalized task should persist before it is closed");
    Require(controller.DeactivateActiveTask().changed, "formalized task should be closable");
    Require(
        controller.CreateTask("temporary-2", "Temporary labeling task").accepted,
        "formal save should allow a fresh temporary task");
    const specforge::SampleLabelingTask* fresh = ActiveTask(controller);
    Require(fresh != nullptr && !fresh->output_path, "formal save should allow a fresh temporary task");
    tasks = ActiveSourceTasks(controller);
    Require(tasks != nullptr && tasks->size() == 2, "formal annotation and one temporary task should coexist");
}

void TestControllerAtomicallyStartsOrResumesTemporaryTask()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() /
        "specforge_sample_labeling_atomic_temporary_state.json";
    const std::filesystem::path output_path =
        std::filesystem::temp_directory_path() /
        "specforge_sample_labeling_atomic_temporary_result.npy";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    std::filesystem::remove(output_path, cleanup_error);
    std::filesystem::remove(
        specforge::SampleAnnotationIoAdapter::
            MetadataPathForResult(output_path),
        cleanup_error);

    bool fail_output_save = false;
    specforge::SampleLabelingController controller(
        cache_path,
        [](const std::filesystem::path& path) {
            return specforge::
                LoadSampleLabelingStateCache(path);
        },
        [&fail_output_save](
            specforge::SampleLabelingTask& task,
            const specforge::
                SampleLabelResultMetadataSource* source) {
            if (!fail_output_save) {
                return specforge::
                    PersistSampleLabelingTaskResult(
                        task,
                        source);
            }
            specforge::MarkSampleLabelTaskSaveFailed(
                task,
                "disk full");
            return specforge::SampleLabelTaskPersistResult{
                .output_path_selected =
                    task.output_path.has_value(),
                .output_saved = false,
                .message = "disk full",
            };
        });
    controller.ActivateSource("source-identity", 3);
    Require(
        controller.CreateTask("formal", "Formal").accepted,
        "controller should create the formal task");
    Require(
        controller
            .SaveActiveTemporaryTaskToOutput(
                output_path,
                "Formal")
            .output_saved,
        "controller should establish a clean formal task");
    const specforge::SampleLabelingTask* formal_task =
        ActiveTask(controller);
    Require(
        formal_task != nullptr,
        "the clean formal task should remain active");
    const std::string formal_task_id =
        formal_task->task_id;

    const specforge::SampleLabelingOperationResult
        created =
            controller.StartOrResumeTemporaryTask();
    const specforge::SampleLabelingTask* temporary_task =
        ActiveTask(controller);
    Require(
        created.accepted && created.changed &&
            temporary_task != nullptr &&
            !temporary_task->output_path,
        "the controller should atomically pause the formal task and create a temporary task");
    const std::string temporary_task_id =
        temporary_task->task_id;

    Require(
        controller.DeactivateActiveTask().changed,
        "the temporary task should be pausable");
    Require(
        controller.ActivateTask(formal_task_id).changed,
        "the formal task should reactivate");
    const specforge::SampleLabelingOperationResult
        resumed =
            controller.StartOrResumeTemporaryTask();
    temporary_task = ActiveTask(controller);
    Require(
        resumed.accepted && resumed.changed &&
            temporary_task != nullptr &&
            temporary_task->task_id ==
                temporary_task_id,
        "the controller should resume the existing temporary task by stable identity");

    Require(
        controller.DeactivateActiveTask().changed,
        "the resumed temporary task should be pausable");
    Require(
        controller.ActivateTask(formal_task_id).changed,
        "the formal task should reactivate before the failed save");
    Require(
        controller
            .UpsertActiveLabel(
                specforge::SampleLabelDefinition{
                    5,
                    "review",
                    'r'})
            .changed,
        "the formal task should accept a label");
    fail_output_save = true;
    const specforge::SampleLabelingWriteOperationResult
        failed_write = controller.AssignLabel(1, 5);
    Require(
        failed_write.write.accepted &&
            failed_write.operation.output_save_attempted &&
            !failed_write.operation.output_saved,
        "the formal task should enter a failed persistence state");

    const std::uint64_t revision_before_rejection =
        controller.View().revision;
    const specforge::SampleLabelingOperationResult
        rejected =
            controller.StartOrResumeTemporaryTask();
    const specforge::SampleLabelingTask* active_task =
        ActiveTask(controller);
    Require(
        !rejected.accepted && !rejected.changed &&
            rejected.revision ==
                revision_before_rejection,
        "an undeactivatable formal task should reject the atomic switch without mutation");
    Require(
        active_task != nullptr &&
            active_task->task_id == formal_task_id,
        "a rejected switch should retain the formal active task");
    Require(
        TemporaryTask(controller) != nullptr &&
            TemporaryTask(controller)->task_id ==
                temporary_task_id,
        "a rejected switch should retain the resumable temporary task");

    std::filesystem::remove(cache_path, cleanup_error);
    std::filesystem::remove(output_path, cleanup_error);
    std::filesystem::remove(
        specforge::SampleAnnotationIoAdapter::
            MetadataPathForResult(output_path),
        cleanup_error);
}

void TestExternalOutputIsResultSourceOfTruth()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_external_state.json";
    const std::filesystem::path output_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_external_result.npy";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    std::filesystem::remove(output_path, cleanup_error);

    {
        specforge::SampleLabelingController controller(cache_path);
        controller.ActivateSource("source-identity", 3);
        Require(controller.CreateTask("quality", "Quality").accepted, "controller should create task");
        Require(
            controller.UpsertActiveLabel(specforge::SampleLabelDefinition{5, "bad", 'b'}).changed,
            "controller should persist label definition");
        Require(controller.AssignLabel(1, 5).write.accepted, "controller should label draft value");
        const specforge::SampleLabelingOperationResult save =
            controller.SaveActiveTemporaryTaskToOutput(output_path, "Quality");
        Require(save.output_saved && save.state_saved, "controller should persist output-backed task");
        const specforge::SampleLabelingTask* task = ActiveTask(controller);
        Require(task != nullptr, "task should remain active after output path is selected");
    }

    const std::string cache_text = ReadTextFile(cache_path);
    Require(cache_text.find("\"output_path\"") != std::string::npos, "task record should keep the output path");
    Require(cache_text.find("\"values\"") == std::string::npos, "output-backed task record must not duplicate label values");

    {
        specforge::SampleLabelingController restored(cache_path);
        restored.ActivateSource("source-identity", 3);
        const specforge::SampleLabelingTask* task = ActiveTask(restored);
        Require(task != nullptr, "output-backed task should restore");
        Require(task->output_path && *task->output_path == output_path, "output path should restore");
        Require(task->values.size() == 3 && task->values[1] == 5, "output-backed task should load values from NPY");
    }
}

void TestMetadataOnlyChangesRewriteSidecarOnRetry()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_metadata_retry_state.json";
    const std::filesystem::path output_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_metadata_retry_result.npy";
    const std::filesystem::path metadata_path =
        specforge::SampleAnnotationIoAdapter::MetadataPathForResult(output_path);
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    std::filesystem::remove(output_path, cleanup_error);
    std::filesystem::remove_all(metadata_path, cleanup_error);

    specforge::SampleLabelingController controller(cache_path);
    controller.ActivateSource("source-identity", 3);
    Require(controller.CreateTask("quality", "Quality").accepted, "controller should create task");
    Require(
        controller.UpsertActiveLabel(specforge::SampleLabelDefinition{5, "bad", 'b'}).changed,
        "controller should add initial label");
    Require(controller.AssignLabel(1, 5).write.accepted, "controller should label a sample");
    const specforge::SampleLabelingOperationResult initial_save =
        controller.SaveActiveTemporaryTaskToOutput(output_path, "Quality");
    Require(initial_save.output_saved && initial_save.state_saved, "controller should persist initial output and metadata");
    Require(ReadTextFile(metadata_path).find("\"name\": \"bad\"") != std::string::npos, "initial metadata should contain the first label name");

    std::filesystem::remove_all(metadata_path, cleanup_error);
    std::filesystem::create_directory(metadata_path, cleanup_error);
    Require(!cleanup_error, "test should block metadata replacement with a directory");
    WriteTextFile(metadata_path / "blocker.txt", "blocked");
    Require(
        controller.UpsertActiveLabel(specforge::SampleLabelDefinition{5, "excellent", 'e'}).changed,
        "renaming an output-backed label should be accepted");
    const specforge::SampleLabelingTask* task = ActiveTask(controller);
    Require(task != nullptr && task->metadata_save_pending, "metadata-only edit should mark metadata pending");
    Require(
        task->save_state.kind == specforge::SampleLabelSaveStateKind::Failed,
        "failed atomic metadata save should enter retryable failed state");
    Require(task->save_state.pending_count == 0, "metadata-only pending state should not invent pending samples");

    std::filesystem::remove_all(metadata_path, cleanup_error);
    Require(!cleanup_error, "test should remove the directory blocking metadata retry");
    RunMaintenanceUntilIdle(controller);
    task = ActiveTask(controller);
    Require(task != nullptr && !task->metadata_save_pending, "metadata retry should clear metadata pending");
    Require(
        task->save_state.kind == specforge::SampleLabelSaveStateKind::AutosavedToOutput,
        "metadata retry should restore clean output save state");
    const std::string metadata = ReadTextFile(metadata_path);
    Require(metadata.find("\"name\": \"excellent\"") != std::string::npos, "metadata retry should write the renamed label");
    Require(metadata.find("\"name\": \"bad\"") == std::string::npos, "metadata retry should replace the old label name");
}

void TestOutputPathConflictIsRejectedWithinSource()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_output_conflict_state.json";
    const std::filesystem::path output_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_output_conflict.npy";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    std::filesystem::remove(output_path, cleanup_error);
    std::filesystem::remove(
        specforge::SampleAnnotationIoAdapter::MetadataPathForResult(output_path),
        cleanup_error);

    {
        specforge::SampleLabelingController controller(cache_path);
        controller.ActivateSource("source-identity", 3);
        Require(controller.CreateTask("first", "First").accepted, "first task should be created");
        Require(
            controller.SaveActiveTemporaryTaskToOutput(output_path, "First").output_saved,
            "first task should claim the output path");
        Require(controller.CreateTask("second", "Second").accepted, "second task should be created");
        Require(
            !controller.SaveActiveTemporaryTaskToOutput(output_path, "Second").accepted,
            "second task must not claim an already-owned output path");
        const specforge::SampleLabelingTask* second = ActiveTask(controller);
        Require(second != nullptr && !second->output_path, "conflicting output path should not be stored on the second task");
        Require(
            second != nullptr &&
                second->save_state.message_kind ==
                    specforge::SampleLabelSaveMessageKind::OutputPathAlreadyUsed &&
                second->save_state.message.empty(),
            "output conflicts should use a structured user-facing error");
        Require(
            controller.FlushStateCache(),
            "structured output conflict should flush to the task record");
    }

    specforge::SampleLabelingController restored(cache_path);
    restored.ActivateSource("source-identity", 3);
    const specforge::SampleLabelingTask* second =
        ActiveTask(restored);
    Require(
        second != nullptr &&
            second->save_state.message_kind ==
                specforge::SampleLabelSaveMessageKind::OutputPathAlreadyUsed,
        "structured output conflict should survive task-record restore");
}

void TestSampleLabelSaveMessageKindsSeparateBusinessAndSystemErrors()
{
    specforge::SampleLabelingTask task =
        specforge::CreateSampleLabelingTask(
            "quality",
            "Quality",
            2);
    specforge::MarkSampleLabelTaskSaveFailed(
        task,
        {});
    Require(
        task.save_state.message_kind ==
                specforge::SampleLabelSaveMessageKind::OutputSaveFailed &&
            task.save_state.message.empty(),
        "missing low-level output detail should use the structured generic error");

    specforge::MarkSampleLabelTaskSaveFailed(
        task,
        "disk full");
    Require(
        task.save_state.message_kind ==
                specforge::SampleLabelSaveMessageKind::SystemDetail &&
            task.save_state.message == "disk full",
        "system error details should remain raw diagnostics");
}

void TestMissingExternalOutputRestoresFailedState()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_missing_external_state.json";
    const std::filesystem::path output_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_missing_external_result.npy";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    std::filesystem::remove(output_path, cleanup_error);

    {
        specforge::SampleLabelingController controller(cache_path);
        controller.ActivateSource("source-identity", 3);
        Require(controller.CreateTask("quality", "Quality").accepted, "controller should create task");
        Require(
            controller.UpsertActiveLabel(specforge::SampleLabelDefinition{5, "bad", 'b'}).changed,
            "controller should persist label definition");
        Require(controller.AssignLabel(1, 5).write.accepted, "controller should label draft value");
        Require(
            controller.SaveActiveTemporaryTaskToOutput(output_path, "Quality").output_saved,
            "controller should persist output-backed task");
    }

    std::filesystem::remove(output_path, cleanup_error);

    specforge::SampleLabelingController restored(cache_path);
    restored.ActivateSource("source-identity", 3);
    const specforge::SampleLabelingTask* task = ActiveTask(restored);
    Require(task != nullptr, "output-backed task should restore even when output is missing");
    Require(
        task->save_state.kind == specforge::SampleLabelSaveStateKind::Failed,
        "missing external output must not restore as clean");
    Require(!task->save_state.message.empty(), "missing external output should explain the failed restore");
}

void TestFailedFirstOutputSaveKeepsTemporaryDraftRecoveryValues()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_draft_output_failure_state.json";
    const std::filesystem::path output_path = std::filesystem::temp_directory_path();
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);

    {
        specforge::SampleLabelingController controller(cache_path);
        controller.ActivateSource("source-identity", 3);
        Require(controller.CreateTask("quality", "Quality").accepted, "controller should create task");
        Require(
            controller.UpsertActiveLabel(specforge::SampleLabelDefinition{5, "bad", 'b'}).changed,
            "controller should add label definition");
        Require(controller.AssignLabel(1, 5).write.accepted, "controller should label draft value");
        Require(controller.FlushStateCache(), "draft-only autosave should succeed before output selection");

        const specforge::SampleLabelingOperationResult save =
            controller.SaveActiveTemporaryTaskToOutput(output_path, "Quality");
        Require(
            save.output_save_attempted && !save.output_saved,
            "saving to a directory path should fail as an output file");
        const specforge::SampleLabelingTask* task = ActiveTask(controller);
        Require(task != nullptr && !task->output_path, "failed first save should keep an output-free draft");
        Require(TemporaryTask(controller) != nullptr, "failed first save should keep the task resumable");
        Require(controller.CanDeactivateActiveTask(), "failed first save should keep the draft pausable");
    }

    const std::string cache_text = ReadTextFile(cache_path);
    Require(cache_text.find("\"output_path\": null") != std::string::npos, "failed first save should not bind output");
    Require(cache_text.find("\"values\"") != std::string::npos, "failed first save should retain full draft values");
    Require(cache_text.find("-1, 5, -1") != std::string::npos, "draft cache should retain the labeled sample");

    {
        specforge::SampleLabelingController restored(cache_path);
        restored.ActivateSource("source-identity", 3);
        const specforge::SampleLabelingTask* task = ActiveTask(restored);
        Require(task != nullptr && !task->output_path, "failed temporary draft should restore without an output");
        Require(task->values.size() == 3 && task->values[1] == 5, "failed draft restore should preserve its label");
        Require(TemporaryTask(restored) != nullptr, "restored failed draft should remain resumable");
        Require(restored.CanDeactivateActiveTask(), "restored failed draft should remain pausable");
        Require(
            task->save_state.kind == specforge::SampleLabelSaveStateKind::Failed,
            "failed first output save should restore failed state");
    }
}

void TestCorruptLocalTaskRecordIsIgnored()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_corrupt_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);

    WriteTextFile(
        cache_path,
        "{\n"
        "  \"format_kind\": \"specforge.sample_labeling_tasks.cache\",\n"
        "  \"schema_version\": 1,\n"
        "  \"sources\": [\n"
        "    { \"identity\": \"source-identity\", \"sample_count\": 999999999999999999999999999 }\n"
        "  ]\n"
        "}\n");

    specforge::SampleLabelingController controller(cache_path);
    controller.ActivateSource("source-identity", 3);
    Require(ActiveTask(controller) == nullptr, "corrupt local task record should be ignored");
    Require(!controller.state_load_warning().empty(), "corrupt local task record should report a load warning");
    Require(controller.CreateTask("quality", "Quality").accepted, "controller should remain usable after corrupt cache");
}

void TestFailedExternalOutputPersistsPendingOverlay()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_failed_external_state.json";
    const std::filesystem::path output_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_failed_external_result.npy";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    std::filesystem::remove(output_path, cleanup_error);

    {
        bool fail_output_save = false;
        specforge::SampleLabelingController controller(
            cache_path,
            [](const std::filesystem::path& path) {
                return specforge::LoadSampleLabelingStateCache(path);
            },
            [&fail_output_save](
                specforge::SampleLabelingTask& task,
                const specforge::SampleLabelResultMetadataSource* source) {
                if (!fail_output_save) {
                    return specforge::PersistSampleLabelingTaskResult(task, source);
                }
                specforge::MarkSampleLabelTaskSaveFailed(task, "disk full");
                return specforge::SampleLabelTaskPersistResult{
                    .output_path_selected = task.output_path.has_value(),
                    .output_saved = false,
                    .message = "disk full"};
            });
        controller.ActivateSource("source-identity", 3);
        Require(controller.CreateTask("quality", "Quality").accepted, "controller should create task");
        Require(
            controller.UpsertActiveLabel(specforge::SampleLabelDefinition{5, "bad", 'b'}).changed,
            "controller should persist label definition");
        Require(
            controller.SaveActiveTemporaryTaskToOutput(output_path, "Quality").output_saved,
            "controller should start from a clean external output");

        fail_output_save = true;
        const specforge::SampleLabelingWriteOperationResult write =
            controller.AssignLabel(1, 5);
        Require(write.write.accepted, "controller should label a pending output value");
        Require(
            write.operation.output_save_attempted && !write.operation.output_saved,
            "controller should own and report the failed output save");
        Require(write.operation.output_retry_scheduled, "failed owned output save should schedule retry");
        Require(write.operation.state_saved, "failed output state should flush to local task record");
    }

    const std::vector<std::int32_t> output_values = ReadTestInt32NpyPayload(output_path);
    Require(output_values[1] == -1, "failed external output should remain the source-of-truth base");

    const std::string cache_text = ReadTextFile(cache_path);
    Require(cache_text.find("\"output_path\"") != std::string::npos, "task record should keep the output path");
    Require(cache_text.find("\"values\"") == std::string::npos, "output-backed task record must not duplicate all values");
    Require(cache_text.find("\"pending_values\"") != std::string::npos, "failed output state should keep pending overlay");
    Require(cache_text.find("\"save_state\": \"failed\"") != std::string::npos, "failed output state should persist");
    Require(cache_text.find("\"index\": 1") != std::string::npos, "pending overlay should include the sample index");
    Require(cache_text.find("\"value\": 5") != std::string::npos, "pending overlay should include the pending label");

    {
        specforge::SampleLabelingController restored(cache_path);
        restored.ActivateSource("source-identity", 3);
        const specforge::SampleLabelingTask* task = ActiveTask(restored);
        Require(task != nullptr, "failed output-backed task should restore");
        Require(task->output_path && *task->output_path == output_path, "output path should restore");
        Require(task->values.size() == 3 && task->values[1] == 5, "pending overlay should restore over the output base");
        Require(task->pending_sample_indices.find(1) != task->pending_sample_indices.end(), "pending sample should restore");
        Require(
            task->save_state.kind == specforge::SampleLabelSaveStateKind::Failed,
            "failed save state should restore");
        Require(task->save_state.pending_count == 1, "failed save state should restore pending count");
    }
}

void TestFailedExternalOutputRetriesAfterBackoff()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_retry_external_state.json";
    const std::filesystem::path output_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_retry_external_result.npy";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    std::filesystem::remove(output_path, cleanup_error);

    bool fail_output_save = false;
    specforge::SampleLabelingController controller(
        cache_path,
        [](const std::filesystem::path& path) {
            return specforge::LoadSampleLabelingStateCache(path);
        },
        [&fail_output_save](
            specforge::SampleLabelingTask& task,
            const specforge::SampleLabelResultMetadataSource* source) {
            if (!fail_output_save) {
                return specforge::PersistSampleLabelingTaskResult(task, source);
            }
            specforge::MarkSampleLabelTaskSaveFailed(task, "disk full");
            return specforge::SampleLabelTaskPersistResult{
                .output_path_selected = task.output_path.has_value(),
                .output_saved = false,
                .message = "disk full"};
        });
    controller.ActivateSource("source-identity", 3);
    Require(controller.CreateTask("quality", "Quality").accepted, "controller should create task");
    Require(
        controller.UpsertActiveLabel(specforge::SampleLabelDefinition{5, "bad", 'b'}).changed,
        "controller should persist label definition");
    Require(
        controller.SaveActiveTemporaryTaskToOutput(output_path, "Quality").output_saved,
        "controller should establish a clean output before the simulated failure");
    fail_output_save = true;
    const specforge::SampleLabelingWriteOperationResult failed_write =
        controller.AssignLabel(1, 5);
    Require(failed_write.write.accepted, "controller should label a pending output value");
    Require(
        failed_write.operation.output_save_attempted && !failed_write.operation.output_saved,
        "simulated output failure should be reported by the domain operation");

    const specforge::SampleLabelingTask* task = ActiveTask(controller);
    Require(task != nullptr, "task should remain active after failed save");
    Require(
        task->save_state.kind == specforge::SampleLabelSaveStateKind::Failed,
        "failed output save should mark task failed");
    Require(task->save_state.pending_count == 1, "failed output save should keep the pending count");

    const std::uint64_t failed_revision = controller.View().revision;
    fail_output_save = false;
    RunMaintenanceUntilIdle(controller);

    task = ActiveTask(controller);
    Require(
        controller.View().revision > failed_revision,
        "successful owned retry should advance labeling revision");
    Require(task != nullptr, "task should remain active after retry");
    Require(
        task->save_state.kind == specforge::SampleLabelSaveStateKind::AutosavedToOutput,
        "retry should mark the external output clean");
    Require(task->pending_sample_indices.empty(), "retry should clear pending samples after output save");
    const std::vector<std::int32_t> output_values = ReadTestInt32NpyPayload(output_path);
    Require(output_values.size() == 3 && output_values[1] == 5, "retry should write pending label to output");

    const std::string cache_text = ReadTextFile(cache_path);
    Require(cache_text.find("\"save_state\": \"autosaved_to_output\"") != std::string::npos, "retry should persist clean state");
    Require(cache_text.find("\"pending_values\"") == std::string::npos, "retry should remove pending overlay from task record");
}

void TestSampleFiltersStackCategoricalConditions()
{
    specforge::SampleAnnotationResult annotation;
    annotation.name = "quality_y.npy";
    annotation.kind = specforge::SampleAnnotationKind::CategoricalInteger;
    annotation.values = {{"0"}, {"1"}, {"1"}, {"-1"}};

    specforge::SampleLabelingTask task = specforge::CreateSampleLabelingTask("review", "Review", 4);
    Require(specforge::UpsertSampleLabel(task.label_set, specforge::SampleLabelDefinition{7, "accepted", 'a'}), "label should be accepted");
    Require(specforge::AssignSampleLabel(task, 1, 7).accepted, "sample 1 should be labeled");
    Require(specforge::AssignSampleLabel(task, 2, 7).accepted, "sample 2 should be labeled");

    const specforge::SampleFilterSource annotation_source = specforge::BuildAnnotationFilterSource(annotation);
    const specforge::SampleFilterSource label_source = specforge::BuildLabelingFilterSource(task);
    Require(
        annotation_source.id == specforge::BuildAnnotationFilterSourceId(annotation),
        "annotation filter source id should come from the shared builder");
    Require(
        label_source.id == specforge::BuildLabelingFilterSourceId(task),
        "labeling filter source id should come from the shared builder");

    specforge::SampleFilterController filters;
    filters.SetCondition(annotation_source.id, std::unordered_set<std::string>{"1"});
    specforge::SampleFilterEvaluation evaluation = filters.Evaluate({annotation_source, label_source}, 4);
    Require(evaluation.active, "categorical annotation condition should make filtering active");
    Require(evaluation.included_count == 2, "one categorical condition should include two samples");
    Require(!evaluation.included_samples[0] && evaluation.included_samples[1] && evaluation.included_samples[2] && !evaluation.included_samples[3], "annotation filter should match selected values");

    filters.SetCondition(label_source.id, std::unordered_set<std::string>{"7"});
    evaluation = filters.Evaluate({annotation_source, label_source}, 4);
    Require(evaluation.active, "stacked conditions should stay active");
    Require(evaluation.included_count == 2, "stacked conditions should use AND semantics");
    Require(evaluation.included_samples[1] && evaluation.included_samples[2], "label condition should match the selected label code");
    Require(label_source.options.size() == 2, "label filter should expose label code and unlabeled sentinel options");
    const auto unlabeled_option = std::find_if(
        label_source.options.begin(),
        label_source.options.end(),
        [](const specforge::SampleFilterValueOption& option) {
            return option.represents_unlabeled_value;
        });
    Require(
        unlabeled_option != label_source.options.end() &&
            unlabeled_option->key == "-1",
        "label filter should identify its unlabeled option semantically");
}

void TestFloatingAnnotationsAreNotFilterable()
{
    specforge::SampleAnnotationResult annotation;
    annotation.name = "score_y.npy";
    annotation.kind = specforge::SampleAnnotationKind::ContinuousFloat;
    annotation.values = {
        specforge::SampleAnnotationValue{0.1},
        specforge::SampleAnnotationValue{0.2},
    };

    const specforge::SampleFilterSource source = specforge::BuildAnnotationFilterSource(annotation);
    Require(!source.filterable, "floating annotations should not be filterable");

    specforge::SampleFilterController filters;
    filters.SetCondition(source.id, std::unordered_set<std::string>{"0.1"});
    const specforge::SampleFilterEvaluation evaluation = filters.Evaluate({source}, 2);
    Require(!evaluation.active, "floating annotation condition should be ignored");
    Require(evaluation.included_count == 2, "ignored floating condition should include all samples");
    Require(
        evaluation.diagnostics.size() == 1 &&
            evaluation.diagnostics[0].kind ==
                specforge::SampleFilterDiagnosticKind::
                    SourceNotFilterable &&
            evaluation.diagnostics[0].source_name ==
                source.name,
        "ignored floating condition should explain why it was ignored");
}

}  // namespace

int main()
{
    try {
        TestSampleLabelingStateCacheRoundTrip();
        TestSampleLabelingStateCacheReportsCorruptJson();
        TestSampleLabelingStateCacheReportsUnsupportedSchema();
        TestSampleLabelTaskWritesStableCodes();
        TestRemovingSampleLabelClearsAssignedValues();
        TestChangingUnusedSampleLabelCode();
        TestChangingUsedSampleLabelCodeRequiresConfirmation();
        TestSampleLabelResultWritesCompactNpy();
        TestSampleLabelResultWritesMetadataSidecar();
        TestAnnotationAdapterRoundTripsLabelArtifacts();
        TestAnnotationAdapterRejectsEmptyTaskIdBeforeWriting();
        TestSampleAnnotationUsesMatchingLabelMetadata();
        TestMismatchedLabelMetadataFallsBackToRawAnnotationValues();
        TestFailedNpySaveDoesNotDamageExistingOutput();
        TestSampleLabelingControllerAutosavesDraftRecord();
        TestTaskRecordFlushKeepsActiveTaskAddressStable();
        TestControllerRevisionTracksOwnedTaskChanges();
        TestControllerKeepsOneTemporaryTaskPerSource();
        TestControllerAtomicallyStartsOrResumesTemporaryTask();
        TestExternalOutputIsResultSourceOfTruth();
        TestMetadataOnlyChangesRewriteSidecarOnRetry();
        TestOutputPathConflictIsRejectedWithinSource();
        TestSampleLabelSaveMessageKindsSeparateBusinessAndSystemErrors();
        TestMissingExternalOutputRestoresFailedState();
        TestFailedFirstOutputSaveKeepsTemporaryDraftRecoveryValues();
        TestCorruptLocalTaskRecordIsIgnored();
        TestFailedExternalOutputPersistsPendingOverlay();
        TestFailedExternalOutputRetriesAfterBackoff();
        TestSampleFiltersStackCategoricalConditions();
        TestFloatingAnnotationsAreNotFilterable();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
