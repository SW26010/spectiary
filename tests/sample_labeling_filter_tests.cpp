#include "domain/sample_filter.h"
#include "domain/sample_annotation_io.h"
#include "domain/sample_labeling.h"
#include "ui/sample_labeling_controller.h"
#include "ui/sample_labeling_state_cache_io.h"

#include <array>
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

void TestSampleLabelResultWritesCompactNpy()
{
    specforge::SampleLabelingTask task = specforge::CreateSampleLabelingTask("quality", "Quality", 3);
    Require(specforge::UpsertSampleLabel(task.label_set, specforge::SampleLabelDefinition{5, "bad", 'b'}), "label should be accepted");
    Require(specforge::AssignSampleLabel(task, 0, 5).accepted, "first sample should be labelable");
    Require(specforge::AssignSampleLabel(task, 2, 5).accepted, "third sample should be labelable");

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_sample_label_result.npy";
    std::string error;
    Require(specforge::SaveSampleLabelResultNpy(path, task, &error), error.empty() ? "NPY save failed" : error);
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
    const std::filesystem::path metadata_path = specforge::SampleLabelResultMetadataPathForResult(path);
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

void TestSampleAnnotationUsesMatchingLabelMetadata()
{
    specforge::SampleLabelingTask task = specforge::CreateSampleLabelingTask("quality", "Quality", 3);
    Require(specforge::UpsertSampleLabel(task.label_set, specforge::SampleLabelDefinition{5, "bad", 'b'}), "label should be accepted");
    Require(specforge::AssignSampleLabel(task, 0, 5).accepted, "first sample should be labelable");

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_sample_annotation_metadata.npy";
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
    std::filesystem::remove(specforge::SampleLabelResultMetadataPathForResult(path), cleanup_error);

    specforge::SelectSampleLabelTaskOutputPath(task, path);
    const specforge::SampleLabelTaskPersistResult saved = specforge::PersistSampleLabelingTaskResult(task);
    Require(saved.output_saved, saved.message.empty() ? "label result should save" : saved.message);

    std::string error;
    std::optional<specforge::SampleAnnotationResult> annotation =
        specforge::LoadSampleAnnotationResultFromPath(path, 3, &error);
    Require(annotation.has_value(), error.empty() ? "metadata-backed annotation should load" : error);
    Require(
        annotation->relationship == specforge::SampleAnnotationWorkflowRelationship::ExternalLabelResult,
        "matching metadata should mark annotation as an external label result");
    Require(annotation->name == "Quality", "metadata task name should become the row name");
    Require(annotation->values[0].display_text == "bad (5)", "metadata should map numeric codes to labels");
    Require(annotation->values[1].display_text == "Unlabeled (-1)", "metadata should map the unlabeled sentinel");
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
    const std::filesystem::path metadata_path = specforge::SampleLabelResultMetadataPathForResult(path);
    std::filesystem::remove(metadata_path, cleanup_error);
    std::string error;
    Require(specforge::SaveSampleLabelResultNpy(path, task, &error), error.empty() ? "NPY save failed" : error);
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
        specforge::LoadSampleAnnotationResultFromPath(path, 3, &error);
    Require(annotation.has_value(), error.empty() ? "annotation should still load" : error);
    Require(
        annotation->relationship == specforge::SampleAnnotationWorkflowRelationship::PlainAnnotation,
        "mismatched metadata should not be applied");
    Require(annotation->values[0].display_text == "5", "mismatched metadata should fall back to raw numeric value");
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
    Require(specforge::SaveSampleLabelResultNpy(path, original, &error), error.empty() ? "initial NPY save failed" : error);

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
    Require(!specforge::SaveSampleLabelResultNpy(path, replacement, &error), "read-only target should reject replacement");

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
        specforge::SampleLabelingTask* task = controller.CreateTask("quality", "Quality");
        Require(task != nullptr, "controller should create an active task");
        Require(
            controller.UpsertActiveLabel(specforge::SampleLabelDefinition{5, "bad", 'b'}),
            "controller should persist label-set edits");
        task = controller.active_task();
        Require(task != nullptr, "task should remain active after label edit");
        task->auto_advance = true;
        Require(controller.PersistActiveTaskRecord(), "workflow setting should persist to local task record");
        Require(controller.AssignLabel(1, 5).accepted, "controller should label a sample");
        Require(controller.state_save_pending(), "sample label writes should queue a debounced local task save");
        controller.MaybeSaveStateCache(10);
        Require(controller.state_save_pending(), "debounced local task save should not flush immediately");
        controller.MaybeSaveStateCache(40);
        Require(!controller.state_save_pending(), "debounced local task save should flush after the delay");
    }

    Require(std::filesystem::exists(cache_path), "autosave should create a local task record");

    {
        specforge::SampleLabelingController restored(cache_path);
        restored.ActivateSource("source-identity", 3);
        const specforge::SampleLabelingTask* task = restored.active_task();
        Require(task != nullptr, "controller should restore active task from local record");
        Require(task->task_id == "quality", "restored task id should match");
        Require(task->auto_advance, "restored workflow setting should match");
        Require(specforge::FindSampleLabel(task->label_set, 5) != nullptr, "restored label set should include saved code");
        Require(task->values.size() == 3 && task->values[1] == 5, "restored draft should include saved sample value");
        Require(task->pending_sample_indices.empty(), "restored autosave should be clean");
        Require(
            task->save_state.kind == specforge::SampleLabelSaveStateKind::InternalDraftOnly,
            "restored draft should report internal autosave state");
        Require(restored.RememberActivePosition(2), "remembered position should be persisted through controller");
        Require(restored.FlushStateCache(), "remembered position should flush to local task record");
    }

    {
        specforge::SampleLabelingController restored(cache_path);
        restored.ActivateSource("source-identity", 3);
        const specforge::SampleLabelingTask* task = restored.active_task();
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
    specforge::SampleLabelingTask* task = controller.CreateTask("quality", "Quality");
    Require(task != nullptr, "controller should create task");
    Require(
        controller.UpsertActiveLabel(specforge::SampleLabelDefinition{5, "bad", 'b'}),
        "controller should add label");
    task = controller.active_task();
    Require(task != nullptr, "task should remain active after label edit");
    Require(controller.AssignLabel(1, 5).accepted, "controller should label a draft sample");

    const specforge::SampleLabelingTask* before_flush = controller.active_task();
    Require(before_flush != nullptr, "active task should exist before flush");
    Require(controller.PersistActiveTaskRecord(), "local task record should flush");
    const specforge::SampleLabelingTask* after_flush = controller.active_task();
    Require(after_flush == before_flush, "flushing local task record must not invalidate active task pointer");
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
        Require(controller.CreateTask("quality", "Quality") != nullptr, "controller should create task");
        Require(
            controller.UpsertActiveLabel(specforge::SampleLabelDefinition{5, "bad", 'b'}),
            "controller should persist label definition");
        Require(controller.AssignLabel(1, 5).accepted, "controller should label draft value");
        Require(controller.SetActiveTaskOutputPath(output_path), "controller should store external output path");
        const specforge::SampleLabelingTask* task = controller.active_task();
        Require(task != nullptr, "task should remain active after output path is selected");
        Require(controller.PersistActiveTask(), "controller should persist output-backed task through its service seam");
    }

    const std::string cache_text = ReadTextFile(cache_path);
    Require(cache_text.find("\"output_path\"") != std::string::npos, "task record should keep the output path");
    Require(cache_text.find("\"values\"") == std::string::npos, "output-backed task record must not duplicate label values");

    {
        specforge::SampleLabelingController restored(cache_path);
        restored.ActivateSource("source-identity", 3);
        const specforge::SampleLabelingTask* task = restored.active_task();
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
    const std::filesystem::path metadata_path = specforge::SampleLabelResultMetadataPathForResult(output_path);
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    std::filesystem::remove(output_path, cleanup_error);
    std::filesystem::remove(metadata_path, cleanup_error);

    specforge::SampleLabelingController controller(cache_path);
    controller.ActivateSource("source-identity", 3);
    Require(controller.CreateTask("quality", "Quality") != nullptr, "controller should create task");
    Require(
        controller.UpsertActiveLabel(specforge::SampleLabelDefinition{5, "bad", 'b'}),
        "controller should add initial label");
    Require(controller.AssignLabel(1, 5).accepted, "controller should label a sample");
    Require(controller.SetActiveTaskOutputPath(output_path), "controller should select output path");
    Require(controller.PersistActiveTask(), "controller should persist initial output and metadata");
    Require(ReadTextFile(metadata_path).find("\"name\": \"bad\"") != std::string::npos, "initial metadata should contain the first label name");

    Require(
        controller.UpsertActiveLabel(specforge::SampleLabelDefinition{5, "excellent", 'e'}),
        "renaming an output-backed label should be accepted");
    const specforge::SampleLabelingTask* task = controller.active_task();
    Require(task != nullptr && task->metadata_save_pending, "metadata-only edit should mark metadata pending");
    Require(
        task->save_state.kind == specforge::SampleLabelSaveStateKind::Pending,
        "metadata-only edit should enter pending save state");
    Require(task->save_state.pending_count == 0, "metadata-only pending state should not invent pending samples");

    controller.MaybeSaveStateCache(10);
    controller.MaybeSaveStateCache(140);
    task = controller.active_task();
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
    std::filesystem::remove(specforge::SampleLabelResultMetadataPathForResult(output_path), cleanup_error);

    specforge::SampleLabelingController controller(cache_path);
    controller.ActivateSource("source-identity", 3);
    Require(controller.CreateTask("first", "First") != nullptr, "first task should be created");
    Require(controller.SetActiveTaskOutputPath(output_path), "first task should claim the output path");
    Require(controller.CreateTask("second", "Second") != nullptr, "second task should be created");
    Require(!controller.SetActiveTaskOutputPath(output_path), "second task must not claim an already-owned output path");
    const specforge::SampleLabelingTask* second = controller.active_task();
    Require(second != nullptr && !second->output_path, "conflicting output path should not be stored on the second task");
    Require(
        second != nullptr && second->save_state.message.find("already used") != std::string::npos,
        "conflicting output path should leave a user-visible message");
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
        Require(controller.CreateTask("quality", "Quality") != nullptr, "controller should create task");
        Require(
            controller.UpsertActiveLabel(specforge::SampleLabelDefinition{5, "bad", 'b'}),
            "controller should persist label definition");
        Require(controller.AssignLabel(1, 5).accepted, "controller should label draft value");
        Require(controller.SetActiveTaskOutputPath(output_path), "controller should store external output path");
        Require(controller.PersistActiveTask(), "controller should persist output-backed task");
    }

    std::filesystem::remove(output_path, cleanup_error);

    specforge::SampleLabelingController restored(cache_path);
    restored.ActivateSource("source-identity", 3);
    const specforge::SampleLabelingTask* task = restored.active_task();
    Require(task != nullptr, "output-backed task should restore even when output is missing");
    Require(
        task->save_state.kind == specforge::SampleLabelSaveStateKind::Failed,
        "missing external output must not restore as clean");
    Require(!task->save_state.message.empty(), "missing external output should explain the failed restore");
}

void TestDraftOutputFailureKeepsDraftRecoveryValues()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_draft_output_failure_state.json";
    const std::filesystem::path output_path = std::filesystem::temp_directory_path();
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);

    {
        specforge::SampleLabelingController controller(cache_path);
        controller.ActivateSource("source-identity", 3);
        Require(controller.CreateTask("quality", "Quality") != nullptr, "controller should create task");
        Require(
            controller.UpsertActiveLabel(specforge::SampleLabelDefinition{5, "bad", 'b'}),
            "controller should add label definition");
        Require(controller.AssignLabel(1, 5).accepted, "controller should label draft value");
        Require(controller.PersistActiveTaskRecord(), "draft-only autosave should succeed before output selection");

        Require(controller.SetActiveTaskOutputPath(output_path), "controller should accept selected output path");
        Require(!controller.PersistActiveTask(), "saving to a directory path should fail as an output file");
    }

    const std::string cache_text = ReadTextFile(cache_path);
    Require(cache_text.find("\"output_path\"") != std::string::npos, "failed output task should keep output path");
    Require(cache_text.find("\"values\"") == std::string::npos, "output-backed failed task should not store full values");
    Require(cache_text.find("\"pending_values\"") != std::string::npos, "failed first output save should keep recovery overlay");
    Require(cache_text.find("\"index\": 1") != std::string::npos, "recovery overlay should include the draft sample");
    Require(cache_text.find("\"value\": 5") != std::string::npos, "recovery overlay should include the draft value");

    {
        specforge::SampleLabelingController restored(cache_path);
        restored.ActivateSource("source-identity", 3);
        const specforge::SampleLabelingTask* task = restored.active_task();
        Require(task != nullptr, "failed output task should restore");
        Require(task->values.size() == 3 && task->values[1] == 5, "failed output restore should preserve draft label");
        Require(task->pending_sample_indices.find(1) != task->pending_sample_indices.end(), "draft label should be pending output retry");
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
    Require(controller.active_task() == nullptr, "corrupt local task record should be ignored");
    Require(!controller.state_load_warning().empty(), "corrupt local task record should report a load warning");
    Require(controller.CreateTask("quality", "Quality") != nullptr, "controller should remain usable after corrupt cache");
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
        specforge::SampleLabelingController controller(cache_path);
        controller.ActivateSource("source-identity", 3);
        Require(controller.CreateTask("quality", "Quality") != nullptr, "controller should create task");
        Require(
            controller.UpsertActiveLabel(specforge::SampleLabelDefinition{5, "bad", 'b'}),
            "controller should persist label definition");
        Require(controller.SetActiveTaskOutputPath(output_path), "controller should store external output path");

        const specforge::SampleLabelingTask* task = controller.active_task();
        Require(task != nullptr, "task should remain active after output path is selected");
        std::string error;
        Require(specforge::SaveSampleLabelResultNpy(output_path, *task, &error), error.empty() ? "NPY save failed" : error);
        Require(controller.MarkActiveOutputPersisted(), "controller should start from a clean external output");
        Require(controller.PersistActiveTaskRecord(), "clean output metadata should persist");

        Require(controller.AssignLabel(1, 5).accepted, "controller should label a pending output value");
        Require(
            controller.MarkActiveOutputSaveFailed("disk full"),
            "controller should record the failed output save state");
        Require(controller.FlushStateCache(), "failed output state should flush to local task record");
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
        const specforge::SampleLabelingTask* task = restored.active_task();
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
    std::filesystem::remove_all(output_path, cleanup_error);
    std::filesystem::create_directory(output_path, cleanup_error);
    Require(!cleanup_error, "test should create a directory at the output path");

    specforge::SampleLabelingController controller(cache_path);
    controller.ActivateSource("source-identity", 3);
    Require(controller.CreateTask("quality", "Quality") != nullptr, "controller should create task");
    Require(
        controller.UpsertActiveLabel(specforge::SampleLabelDefinition{5, "bad", 'b'}),
        "controller should persist label definition");
    Require(controller.AssignLabel(1, 5).accepted, "controller should label a pending output value");
    Require(controller.SetActiveTaskOutputPath(output_path), "controller should store external output path");
    Require(!controller.PersistActiveTask(), "directory output path should fail the first output save");

    const specforge::SampleLabelingTask* task = controller.active_task();
    Require(task != nullptr, "task should remain active after failed save");
    Require(
        task->save_state.kind == specforge::SampleLabelSaveStateKind::Failed,
        "failed output save should mark task failed");
    Require(task->save_state.pending_count == 1, "failed output save should keep the pending count");

    std::filesystem::remove_all(output_path, cleanup_error);
    Require(!cleanup_error, "test should remove the directory blocking the retry");
    controller.MaybeSaveStateCache(10);
    controller.MaybeSaveStateCache(140);

    task = controller.active_task();
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
}

void TestFloatingAnnotationsAreNotFilterable()
{
    specforge::SampleAnnotationResult annotation;
    annotation.name = "score_y.npy";
    annotation.kind = specforge::SampleAnnotationKind::ContinuousFloat;
    annotation.values = {{"0.1"}, {"0.2"}};

    const specforge::SampleFilterSource source = specforge::BuildAnnotationFilterSource(annotation);
    Require(!source.filterable, "floating annotations should not be filterable");

    specforge::SampleFilterController filters;
    filters.SetCondition(source.id, std::unordered_set<std::string>{"0.1"});
    const specforge::SampleFilterEvaluation evaluation = filters.Evaluate({source}, 2);
    Require(!evaluation.active, "floating annotation condition should be ignored");
    Require(evaluation.included_count == 2, "ignored floating condition should include all samples");
    Require(!evaluation.messages.empty(), "ignored floating condition should explain why it was ignored");
}

}  // namespace

int main()
{
    try {
        TestSampleLabelingStateCacheRoundTrip();
        TestSampleLabelingStateCacheReportsCorruptJson();
        TestSampleLabelingStateCacheReportsUnsupportedSchema();
        TestSampleLabelTaskWritesStableCodes();
        TestSampleLabelResultWritesCompactNpy();
        TestSampleLabelResultWritesMetadataSidecar();
        TestSampleAnnotationUsesMatchingLabelMetadata();
        TestMismatchedLabelMetadataFallsBackToRawAnnotationValues();
        TestFailedNpySaveDoesNotDamageExistingOutput();
        TestSampleLabelingControllerAutosavesDraftRecord();
        TestTaskRecordFlushKeepsActiveTaskAddressStable();
        TestExternalOutputIsResultSourceOfTruth();
        TestMetadataOnlyChangesRewriteSidecarOnRetry();
        TestOutputPathConflictIsRejectedWithinSource();
        TestMissingExternalOutputRestoresFailedState();
        TestDraftOutputFailureKeepsDraftRecoveryValues();
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
