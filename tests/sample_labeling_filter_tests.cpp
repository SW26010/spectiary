#include "domain/sample_filter.h"
#include "domain/sample_annotation_io.h"
#include "domain/csv_record_codec.h"
#include "domain/sample_labeling.h"
#include "domain/sample_label_export.h"
#include "domain/sample_labeling_asdf_store.h"
#include "domain/source_collection_manifest.h"
#include "domain/source_path_identity.h"
#include "domain/stable_sha256.h"
#include "platform/exclusive_file_lease.h"
#include "ui/sample_annotation_labeling_rules.h"
#include "ui/sample_labeling_controller.h"
#include "ui/sample_labeling_state_cache_io.h"

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

specforge::SampleLabelingMaintenanceResult
RunMaintenanceUntilIdle(
    specforge::SampleLabelingController& controller)
{
    specforge::SampleLabelingMaintenanceResult aggregate;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const auto deadline = controller.NextMaintenanceDeadline();
        if (!deadline) {
            return aggregate;
        }
        const specforge::SampleLabelingMaintenanceResult
            maintenance =
                controller.RunMaintenance(*deadline);
        aggregate.output_retry_attempted =
            aggregate.output_retry_attempted ||
            maintenance.output_retry_attempted;
        aggregate.canonical_output_published =
            aggregate.canonical_output_published ||
            maintenance.canonical_output_published;
    }
    Require(
        !controller.NextMaintenanceDeadline(),
        "sample-labeling maintenance should converge after successful persistence");
    return aggregate;
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

void ActivateCanonicalTestSource(
    specforge::SampleLabelingController& controller,
    std::string source_identity,
    std::size_t sample_count)
{
    const std::string source_name = source_identity + ".npy";
    const std::string source_fingerprint =
        "fingerprint:" + source_identity;
    specforge::SourceCollectionIdentity identity{
        .id = source_identity,
        .source_name = source_name,
        .source_fingerprint = source_fingerprint,
        .context_fingerprint =
            "context:" + source_identity,
        .spectrum_count = sample_count,
    };
    controller.ActivateSource(
        identity,
        specforge::SampleLabelingCanonicalSourceDescriptor{
            .base_identity = std::move(source_identity),
            .source_kind = "npy",
            .source_name = std::move(source_name),
            .source_fingerprint =
                std::move(source_fingerprint),
            .sample_count = sample_count,
        });
}

const specforge::SampleLabelingTask* ActiveSourceTask(
    const specforge::SampleLabelingController& controller,
    std::string_view task_id)
{
    const std::vector<specforge::SampleLabelingTask>* tasks =
        ActiveSourceTasks(controller);
    if (tasks == nullptr) {
        return nullptr;
    }
    const auto task = std::find_if(
        tasks->begin(),
        tasks->end(),
        [task_id](const specforge::SampleLabelingTask& candidate) {
            return candidate.task_id == task_id;
        });
    return task == tasks->end() ? nullptr : &*task;
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
    Require(
        stream.good(),
        "could not open written NPY file: " +
            path.string());

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

std::vector<std::int32_t> ReadTestCanonicalAsdfValues(
    const std::filesystem::path& path)
{
    const specforge::SampleLabelingAsdfReadResult read =
        specforge::ReadSampleLabelingAsdfDocument(path);
    Require(
        read.succeeded(),
        read.error.message.empty()
            ? "could not read canonical ASDF output"
            : read.error.message);
    return read.document->annotation.values;
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

specforge::SampleLabelingTask OutputTask(
    std::string task_id,
    const std::filesystem::path& output_path,
    specforge::SampleLabelingOutputArtifactFormat output_format)
{
    std::string task_name = task_id;
    specforge::SampleLabelingTask task =
        specforge::CreateSampleLabelingTask(
            std::move(task_id),
            std::move(task_name),
            3);
    task.output_path = output_path;
    task.output_format = output_format;
    return task;
}

void WriteTextFile(const std::filesystem::path& path, std::string_view contents)
{
    std::ofstream stream(path, std::ios::trunc);
    Require(stream.good(), "could not open text file for writing");
    stream << contents;
    Require(stream.good(), "could not write text file");
}

std::vector<unsigned char> ReadBinaryFile(
    const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open binary file for reading");
    return std::vector<unsigned char>(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

void WriteBinaryFile(
    const std::filesystem::path& path,
    std::span<const unsigned char> bytes)
{
    std::ofstream stream(
        path,
        std::ios::binary | std::ios::trunc);
    Require(stream.good(), "could not open binary file for writing");
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    stream.close();
    Require(stream.good(), "could not write binary file");
}

void ReplaceBinaryTextOnce(
    std::vector<unsigned char>& bytes,
    std::string_view old_text,
    std::string_view new_text)
{
    const auto found = std::search(
        bytes.begin(),
        bytes.end(),
        old_text.begin(),
        old_text.end());
    Require(found != bytes.end(), "binary patch target should exist");
    const std::size_t offset =
        static_cast<std::size_t>(found - bytes.begin());
    bytes.erase(
        found,
        found + static_cast<std::ptrdiff_t>(old_text.size()));
    bytes.insert(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        new_text.begin(),
        new_text.end());
}

bool BinaryFileContains(
    const std::filesystem::path& path,
    std::string_view text)
{
    const std::vector<unsigned char> bytes = ReadBinaryFile(path);
    return std::search(
               bytes.begin(),
               bytes.end(),
               text.begin(),
               text.end()) != bytes.end();
}

std::filesystem::path FreshTestDirectory(std::string_view name);
const specforge::SampleLabelingTask* FindTask(
    const specforge::SampleLabelingStateCache& cache,
    std::string_view source_identity,
    std::string_view task_id);

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
    Require(
        task.output_format ==
            specforge::SampleLabelingOutputArtifactFormat::None,
        "a temporary task should have no formal output owner");

    specforge::SampleLabelingStateCache cache;
    specforge::SampleLabelingSourceState state;
    state.sample_count = 3;
    state.active_task_id = "quality";
    state.tasks.push_back(std::move(task));
    cache.sources.emplace("source-identity", std::move(state));
    Require(specforge::SaveSampleLabelingStateCache(cache_path, cache), "sample-labeling cache should save");

    const std::string cache_text = ReadTextFile(cache_path);
    Require(
        cache_text.find("\"schema_version\": 3") != std::string::npos &&
            cache_text.find("\"output\": {") != std::string::npos &&
            cache_text.find("\"path\": null") != std::string::npos &&
            cache_text.find("\"format\": \"none\"") != std::string::npos &&
            cache_text.find("\"output_path\"") == std::string::npos,
        "schema 3 should persist an explicit output owner for temporary tasks");

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

void TestSampleLabelingOutputFormatMigrationAndRoundTrip()
{
    const std::filesystem::path directory =
        FreshTestDirectory("specforge_labeling_output_format_cache");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path legacy_output =
        directory / "legacy.npy";
    const auto legacy_utf8 = legacy_output.generic_u8string();
    const std::string legacy_text(
        legacy_utf8.begin(),
        legacy_utf8.end());
    WriteTextFile(
        cache_path,
        "{\n"
        "  \"format_kind\": \"specforge.sample_labeling_tasks.cache\",\n"
        "  \"schema_version\": 2,\n"
        "  \"sources\": [{\n"
        "    \"identity\": \"legacy-source\",\n"
        "    \"sample_count\": 3,\n"
        "    \"tasks\": [{\n"
        "      \"task_id\": \"legacy-task\",\n"
        "      \"task_name\": \"Legacy task\",\n"
        "      \"output_path\": \"" + legacy_text + "\",\n"
        "      \"labels\": []\n"
        "    }]\n"
        "  }]\n"
        "}\n");

    specforge::SampleLabelingStateCacheLoadResult migrated =
        specforge::LoadSampleLabelingStateCache(
            cache_path,
            {},
            specforge::SampleLabelingStateCacheLoadPolicy::
                AllowPersistentOutputsWithoutResultHydration);
    Require(migrated.warning.empty(), migrated.warning);
    specforge::SampleLabelingTask& legacy_task =
        migrated.cache.sources.at("legacy-source").tasks.front();
    Require(
        legacy_task.output_path ==
                std::optional<std::filesystem::path>{legacy_output} &&
            legacy_task.output_format ==
                specforge::SampleLabelingOutputArtifactFormat::
                    LegacyNpyWithSidecar,
        "schema 1/2 output_path should migrate explicitly to legacy NPY ownership");

    specforge::SampleLabelingTask canonical_task =
        OutputTask(
            "canonical-task",
            directory / "canonical.asdf",
            specforge::SampleLabelingOutputArtifactFormat::CanonicalAsdf);
    migrated.cache.sources.at("legacy-source").tasks.push_back(
        canonical_task);
    Require(
        specforge::SaveSampleLabelingStateCache(
            cache_path,
            migrated.cache),
        "migrated output formats should save as schema 3");
    const std::string cache_text = ReadTextFile(cache_path);
    Require(
        cache_text.find("\"schema_version\": 3") != std::string::npos &&
            cache_text.find("\"output_path\"") == std::string::npos &&
            cache_text.find("\"format\": \"legacy_npy_with_sidecar\"") !=
                std::string::npos &&
            cache_text.find("\"format\": \"canonical_asdf\"") !=
                std::string::npos,
        "schema 3 should persist formal output path and format together");

    const specforge::SampleLabelingStateCacheLoadResult round_tripped =
        specforge::LoadSampleLabelingStateCache(
            cache_path,
            {},
            specforge::SampleLabelingStateCacheLoadPolicy::
                AllowPersistentOutputsWithoutResultHydration);
    Require(round_tripped.warning.empty(), round_tripped.warning);
    const auto& tasks =
        round_tripped.cache.sources.at("legacy-source").tasks;
    Require(
        tasks.size() == 2 &&
            tasks[0].output_format ==
                specforge::SampleLabelingOutputArtifactFormat::
                    LegacyNpyWithSidecar &&
            tasks[1].output_format ==
                specforge::SampleLabelingOutputArtifactFormat::CanonicalAsdf,
        "schema 3 should restore each formal task's persisted output owner");
}

void TestInterruptedInitialCanonicalPublicationRestoresTemporaryDraft()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_interrupted_initial_publication");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "interrupted.asdf";
    const auto output_utf8_view =
        output_path.generic_u8string();
    const std::string output_utf8(
        output_utf8_view.begin(),
        output_utf8_view.end());
    WriteTextFile(
        cache_path,
        "{\n"
        "  \"format_kind\": \"specforge.sample_labeling_tasks.cache\",\n"
        "  \"schema_version\": 3,\n"
        "  \"sources\": [{\n"
        "    \"identity\": \"interrupted-source\",\n"
        "    \"sample_count\": 3,\n"
        "    \"source_name\": \"interrupted-source.npy\",\n"
        "    \"source_fingerprint\": \"fingerprint:interrupted-source\",\n"
        "    \"context_fingerprint\": \"context:interrupted-source\",\n"
        "    \"active_task_id\": \"interrupted-task\",\n"
        "    \"tasks\": [{\n"
        "      \"task_id\": \"interrupted-task\",\n"
        "      \"task_name\": \"Interrupted formalization\",\n"
        "      \"output\": {\n"
        "        \"path\": { \"path_kind\": \"absolute\", \"path\": \"" +
            output_utf8 + "\" },\n"
        "        \"format\": \"canonical_asdf\"\n"
        "      },\n"
        "      \"initial_publication_pending\": true,\n"
        "      \"labels\": [{ \"code\": 5, \"name\": \"accepted\", \"shortcut\": \"a\" }],\n"
        "      \"save_state\": \"pending\",\n"
        "      \"save_message\": \"\",\n"
        "      \"save_message_kind\": \"none\",\n"
        "      \"metadata_pending\": true,\n"
        "      \"pending_values\": [{ \"index\": 1, \"value\": 5 }]\n"
        "    }]\n"
        "  }]\n"
        "}\n");

    specforge::SampleLabelingController recovered(
        cache_path);
    ActivateCanonicalTestSource(
        recovered,
        "interrupted-source",
        3);
    const specforge::SampleLabelingTask* task =
        ActiveTask(recovered);
    Require(
        task != nullptr &&
            !task->output_path &&
            task->output_format ==
                specforge::SampleLabelingOutputArtifactFormat::None &&
            task->task_name ==
                specforge::kTemporarySampleLabelingTaskName &&
            task->values_are_authoritative &&
            task->values == std::vector<int>({-1, 5, -1}) &&
            TemporaryTask(recovered) == task,
        "a restart between the initial checkpoint and ASDF creation must restore a real temporary draft");

    const specforge::SampleLabelingOperationResult retried =
        recovered.SaveActiveTemporaryTaskToOutput(
            output_path,
            "interrupted");
    Require(
        retried.output_saved &&
            ActiveTask(recovered) != nullptr &&
            ActiveTask(recovered)->output_format ==
                specforge::SampleLabelingOutputArtifactFormat::CanonicalAsdf &&
            ReadTestCanonicalAsdfValues(output_path) ==
                std::vector<std::int32_t>({-1, 5, -1}),
        "the recovered temporary draft should remain available for a safe Save As retry");
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

std::filesystem::path FreshTestDirectory(
    std::string_view name)
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        std::string(name);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    error.clear();
    std::filesystem::create_directories(directory, error);
    Require(!error, "could not create test directory");
    return directory;
}

specforge::SampleLabelingDocument
CanonicalOwnerDocument()
{
    specforge::SampleLabelingDocument document;
    document.source.base_identity = "canonical-source";
    document.source.kind = "npy";
    document.source.name = "spectra.npy";
    document.source.fingerprint =
        "canonical-source-fingerprint";
    document.source.sample_count = 3;
    document.source.roster.identity_kind =
        std::string{
            specforge::
                kSampleLabelingDocumentExplicitNamesRoster};
    document.source.roster.sample_names = {
        "sample-a",
        "sample-b",
        "sample-c",
    };
    document.annotation.name = "quality-code";
    document.annotation.values = {-1, 2, 7};
    document.labeling.id = "quality-task";
    document.labeling.name = "Canonical quality";
    document.labeling.labels = {
        {2, "accepted", "a"},
        {7, "rejected", "r"},
    };
    return document;
}

specforge::SourceCollectionIdentity
CanonicalOwnerSourceIdentity()
{
    return specforge::SourceCollectionIdentity{
        .id = "canonical-source",
        .source_name = "spectra.npy",
        .source_fingerprint =
            "canonical-source-fingerprint",
        .context_fingerprint =
            "annotation-sensitive-context",
        .spectrum_count = 3,
    };
}

specforge::SampleLabelingCanonicalSourceDescriptor
CanonicalOwnerSourceDescriptor()
{
    return specforge::
        SampleLabelingCanonicalSourceDescriptor{
            .base_identity = "canonical-source",
            .source_kind = "npy",
            .source_name = "spectra.npy",
            .source_fingerprint =
                "canonical-source-fingerprint",
            .sample_count = 3,
            .sample_names = {
                "sample-a",
                "sample-b",
                "sample-c",
            },
        };
}

void SaveCanonicalOwnerCache(
    const std::filesystem::path& cache_path,
    const std::filesystem::path& asdf_path)
{
    specforge::SampleLabelingTask task =
        specforge::CreateSampleLabelingTask(
            "quality-task",
            "structural canonical owner",
            3);
    task.output_path = asdf_path;
    task.output_format =
        specforge::SampleLabelingOutputArtifactFormat::
            CanonicalAsdf;

    specforge::SampleLabelingSourceState source;
    source.sample_count = 3;
    source.source_name = "spectra.npy";
    source.source_fingerprint =
        "canonical-source-fingerprint";
    source.active_task_id = "quality-task";
    source.tasks.push_back(std::move(task));

    specforge::SampleLabelingStateCache cache;
    cache.sources.emplace(
        "canonical-source",
        std::move(source));
    Require(
        specforge::SaveSampleLabelingStateCache(
            cache_path,
            cache),
        "canonical owner cache fixture should save");
}

void TestCanonicalAsdfTaskOwnerHydratesWithPendingOverlay()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_canonical_owner_hydration");
    const std::filesystem::path asdf_path =
        directory / "quality.asdf";
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const specforge::SampleLabelingDocument document =
        CanonicalOwnerDocument();
    Require(
        specforge::WriteSampleLabelingAsdfDocumentAtomically(
            asdf_path,
            document)
            .succeeded(),
        "canonical owner fixture should publish its ASDF document");

    specforge::SampleLabelingStateCache cache;
    specforge::SampleLabelingSourceState source;
    source.sample_count = 3;
    source.source_name = "spectra.npy";
    source.source_fingerprint =
        "canonical-source-fingerprint";
    source.active_task_id = "quality-task";
    specforge::SampleLabelingTask cached =
        specforge::CreateSampleLabelingTask(
            "quality-task",
            "stale cache name",
            3);
    cached.label_set.labels = {
        {99, "stale cache label", 's'},
    };
    cached.output_path = asdf_path;
    cached.output_format =
        specforge::SampleLabelingOutputArtifactFormat::
            CanonicalAsdf;
    cached.auto_advance = true;
    cached.skip_labeled_on_advance = true;
    cached.remembered_position = 2;
    cached.values[1] = 7;
    cached.pending_sample_indices.insert(1);
    cached.save_state.kind =
        specforge::SampleLabelSaveStateKind::Pending;
    source.tasks.push_back(cached);
    cache.sources.emplace(
        "canonical-source",
        std::move(source));
    Require(
        specforge::SaveSampleLabelingStateCache(
            cache_path,
            cache),
        "canonical owner cache fixture should save");

    const std::string cache_text =
        ReadTextFile(cache_path);
    Require(
        cache_text.find("\"pending_values\"") !=
                std::string::npos &&
            cache_text.find("          \"values\":") ==
                std::string::npos,
        "canonical owner cache should retain only its sparse pending values");
    const specforge::SampleLabelingStateCacheLoadResult
        structural =
            specforge::LoadSampleLabelingStateCache(
                cache_path);
    const specforge::SampleLabelingTask* structural_task =
        FindTask(
            structural.cache,
            "canonical-source",
            "quality-task");
    Require(
        structural.issue_kind ==
                specforge::
                    SampleLabelingStateCacheLoadIssueKind::
                        None &&
            structural_task != nullptr &&
            structural_task->save_state.kind !=
                specforge::SampleLabelSaveStateKind::Failed &&
            structural_task->values ==
                std::vector<int>({-1, 7, -1}),
        "default cache load must leave canonical owners structural instead of sending them to the NPY loader");

    {
        std::size_t legacy_publication_calls = 0;
        std::size_t canonical_publication_calls = 0;
        bool roster_block_reused = false;
        specforge::SampleLabelingController controller(
            cache_path,
            [](const std::filesystem::path& path) {
                return specforge::LoadSampleLabelingStateCache(
                    path);
            },
            [&legacy_publication_calls](
                specforge::SampleLabelingTask& task,
                const specforge::SampleLabelResultMetadataSource* source) {
                ++legacy_publication_calls;
                return specforge::PublishLegacySampleLabelingTaskOutput(
                    task,
                    source);
            },
            [&canonical_publication_calls,
             &roster_block_reused](
                specforge::SampleLabelingAsdfOpenSnapshot& snapshot,
                std::span<const std::int32_t> values) {
                ++canonical_publication_calls;
                specforge::SampleLabelingAsdfStoreWriteResult result =
                    specforge::RewriteSampleLabelingAsdfValuesAtomically(
                        snapshot,
                        values);
                roster_block_reused =
                    result.roster_block_reused;
                return result;
            });
        controller.ActivateSource(
            CanonicalOwnerSourceIdentity(),
            CanonicalOwnerSourceDescriptor());
        const specforge::SampleLabelingTask* task =
            ActiveTask(controller);
        Require(
            task != nullptr &&
                task->task_id == "quality-task" &&
                task->task_name ==
                    "Canonical quality" &&
                task->label_set.labels.size() == 2 &&
                task->label_set.labels[0].code == 2 &&
                task->label_set.labels[0].name ==
                    "accepted" &&
                task->values ==
                    std::vector<int>({-1, 7, 7}) &&
                task->auto_advance &&
                task->skip_labeled_on_advance &&
                task->remembered_position ==
                    std::optional<std::size_t>{2} &&
                task->output_format ==
                    specforge::
                        SampleLabelingOutputArtifactFormat::
                            CanonicalAsdf,
            "canonical hydration should use ASDF metadata and base values before applying local session and pending state");

        const specforge::SampleLabelingWriteOperationResult
            write = controller.AssignLabel(0, 2);
        const specforge::SampleLabelingWriteOperationResult
            cleared = controller.ClearLabel(1);
        const std::optional<specforge::SampleAnnotationResult>
            current_projection =
                controller.
                    ActiveCanonicalAsdfAnnotationProjection();
        Require(
            write.write.accepted &&
                write.operation.state_saved &&
                write.operation.output_save_attempted &&
                write.operation.output_saved &&
                !write.operation.output_retry_scheduled &&
                cleared.write.changed &&
                cleared.operation.state_saved &&
                cleared.operation.output_save_attempted &&
                cleared.operation.output_saved &&
                !cleared.operation.output_retry_scheduled &&
                legacy_publication_calls == 0 &&
                canonical_publication_calls == 2 &&
                roster_block_reused &&
                ActiveTask(controller) != nullptr &&
                ActiveTask(controller)
                    ->pending_sample_indices.empty() &&
                current_projection &&
                current_projection->labeling_document &&
                current_projection->labeling_document
                        ->annotation.values ==
                    std::vector<std::int32_t>({2, -1, 7}),
            "canonical assign and clear dispatch should checkpoint, reuse the roster block, publish, and clear each sparse overlay");
    }

    const specforge::SampleLabelingAsdfStoreOpenResult
        published =
            specforge::OpenSampleLabelingAsdfDocumentStore(
                asdf_path,
                specforge::SampleLabelingCompatibilityView(
                    CanonicalOwnerSourceDescriptor()));
    Require(
        published.succeeded() &&
            published.snapshot->document()
                    .annotation.values ==
                std::vector<std::int32_t>({2, -1, 7}),
        "editing an existing canonical owner should publish the authoritative value generation");

    const specforge::SampleLabelingStateCacheLoadResult
        clean_cache =
            specforge::LoadSampleLabelingStateCache(
                cache_path);
    const specforge::SampleLabelingTask* clean_cached_task =
        FindTask(
            clean_cache.cache,
            "canonical-source",
            "quality-task");
    Require(
        clean_cached_task != nullptr &&
            clean_cached_task
                ->pending_sample_indices.empty() &&
            clean_cached_task->values ==
                std::vector<int>({-1, -1, -1}),
        "successful canonical publication should clear the durable sparse overlay without caching full values");

    specforge::SampleLabelingController reopened(
        cache_path);
    reopened.ActivateSource(
        CanonicalOwnerSourceIdentity(),
        CanonicalOwnerSourceDescriptor());
    Require(
        ActiveTask(reopened) != nullptr &&
            ActiveTask(reopened)->values ==
                std::vector<int>({2, -1, 7}),
        "a reopened canonical owner should hydrate the published ASDF values without a cache overlay");

    specforge::SourceCollectionIdentity changed_context =
        CanonicalOwnerSourceIdentity();
    changed_context.context_fingerprint =
        "changed-annotation-context";
    reopened.ActivateSource(
        changed_context,
        CanonicalOwnerSourceDescriptor());
    Require(
        ActiveTask(reopened) != nullptr,
        "a changed context generation should revalidate and retain a compatible canonical owner");

    changed_context.context_fingerprint =
        "changed-kind-context";
    specforge::SampleLabelingCanonicalSourceDescriptor
        wrong_kind = CanonicalOwnerSourceDescriptor();
    wrong_kind.source_kind = "fits";
    const std::uint64_t generation_before_failed_revalidation =
        reopened.active_source_tasks_generation();
    reopened.ActivateSource(
        changed_context,
        std::move(wrong_kind));
    Require(
        ActiveTask(reopened) == nullptr,
        "a changed context generation must fail closed when the canonical source kind no longer matches");
    const specforge::SampleLabelingTask* failed_owner =
        ActiveSourceTask(reopened, "quality-task");
    Require(
        failed_owner != nullptr &&
            !failed_owner->values_are_authoritative &&
            failed_owner->values ==
                std::vector<int>({-1, -1, -1}) &&
            failed_owner->pending_sample_indices.empty() &&
            reopened.active_source_tasks_generation() >
                generation_before_failed_revalidation,
        "failed canonical revalidation must discard the stale base projection and invalidate projection caches");

    specforge::SampleLabelingController takeover(
        cache_path);
    takeover.ActivateSource(
        CanonicalOwnerSourceIdentity(),
        CanonicalOwnerSourceDescriptor());
    Require(
        ActiveTask(takeover) != nullptr,
        "failed canonical source revalidation without a pending recovery patch must immediately release the owner lease");
}

void TestCanonicalAsdfValueFailureRetainsOverlayAndRetries()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_canonical_value_retry");
    const std::filesystem::path asdf_path =
        directory / "quality.asdf";
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const specforge::SampleLabelingDocument original =
        CanonicalOwnerDocument();
    Require(
        specforge::WriteSampleLabelingAsdfDocumentAtomically(
            asdf_path,
            original)
            .succeeded(),
        "canonical retry fixture should publish its initial generation");
    SaveCanonicalOwnerCache(
        cache_path,
        asdf_path);

    bool fail_publication = true;
    bool retry_reused_roster = false;
    std::size_t canonical_publication_calls = 0;
    specforge::SampleLabelingController controller(
        cache_path,
        [](const std::filesystem::path& path) {
            return specforge::LoadSampleLabelingStateCache(
                path);
        },
        [](specforge::SampleLabelingTask& task,
           const specforge::SampleLabelResultMetadataSource* source) {
            return specforge::PublishLegacySampleLabelingTaskOutput(
                task,
                source);
        },
        [&fail_publication,
         &retry_reused_roster,
         &canonical_publication_calls](
            specforge::SampleLabelingAsdfOpenSnapshot& snapshot,
            std::span<const std::int32_t> values) {
            ++canonical_publication_calls;
            if (fail_publication) {
                return specforge::
                    sample_labeling_asdf_store_test_seam::
                        RewriteWithBeforeReplace(
                            snapshot,
                            values,
                            [](const auto&, const auto&) {
                                throw std::runtime_error(
                                    "injected canonical value publication failure");
                            });
            }
            specforge::SampleLabelingAsdfStoreWriteResult result =
                specforge::RewriteSampleLabelingAsdfValuesAtomically(
                    snapshot,
                    values);
            retry_reused_roster =
                result.roster_block_reused;
            return result;
        });
    controller.ActivateSource(
        CanonicalOwnerSourceIdentity(),
        CanonicalOwnerSourceDescriptor());

    const specforge::SampleLabelingWriteOperationResult failed =
        controller.AssignLabel(0, 2);
    const specforge::SampleLabelingTask* failed_task =
        ActiveTask(controller);
    Require(
        failed.write.changed &&
            failed.operation.state_saved &&
            failed.operation.output_save_attempted &&
            !failed.operation.output_saved &&
            failed.operation.output_retry_scheduled &&
            canonical_publication_calls == 1 &&
            failed_task != nullptr &&
            failed_task->values ==
                std::vector<int>({2, 2, 7}) &&
            failed_task->pending_sample_indices ==
                std::unordered_set<std::size_t>({0}) &&
            failed_task->save_state.kind ==
                specforge::SampleLabelSaveStateKind::Failed,
        "failed canonical publication should retain the newest authoritative value and schedule retry");

    const specforge::SampleLabelingAsdfStoreOpenResult
        old_generation =
            specforge::OpenSampleLabelingAsdfDocumentStore(
                asdf_path,
                specforge::SampleLabelingCompatibilityView(
                    CanonicalOwnerSourceDescriptor()));
    const specforge::SampleLabelingStateCacheLoadResult
        pending_cache =
            specforge::LoadSampleLabelingStateCache(
                cache_path);
    const specforge::SampleLabelingTask* pending_task =
        FindTask(
            pending_cache.cache,
            "canonical-source",
            "quality-task");
    Require(
        old_generation.succeeded() &&
            old_generation.snapshot->document()
                    .annotation.values ==
                original.annotation.values &&
            pending_task != nullptr &&
            pending_task->values ==
                std::vector<int>({2, -1, -1}) &&
            pending_task->pending_sample_indices ==
                std::unordered_set<std::size_t>({0}),
        "failed canonical publication should preserve the old trusted ASDF generation and durable sparse checkpoint");

    fail_publication = false;
    specforge::SourceCollectionIdentity other_source{
        .id = "other-canonical-source",
        .source_name = "other.npy",
        .source_fingerprint = "other-source-fingerprint",
        .context_fingerprint = "other-context-fingerprint",
        .spectrum_count = 3,
    };
    specforge::SampleLabelingCanonicalSourceDescriptor
        other_descriptor = CanonicalOwnerSourceDescriptor();
    other_descriptor.base_identity = other_source.id;
    other_descriptor.source_name =
        other_source.source_name;
    other_descriptor.source_fingerprint =
        other_source.source_fingerprint;
    controller.ActivateSource(
        other_source,
        std::move(other_descriptor));
    const specforge::SampleLabelingMaintenanceResult
        inactive_maintenance =
            RunMaintenanceUntilIdle(controller);
    const specforge::SampleLabelingStateCacheLoadResult
        inactive_cache =
            specforge::LoadSampleLabelingStateCache(
                cache_path);
    const specforge::SampleLabelingTask*
        inactive_pending_task = FindTask(
            inactive_cache.cache,
            "canonical-source",
            "quality-task");
    Require(
        canonical_publication_calls == 1 &&
            !inactive_maintenance
                 .canonical_output_published &&
            !controller.NextMaintenanceDeadline() &&
            inactive_pending_task != nullptr &&
            inactive_pending_task->pending_sample_indices ==
                std::unordered_set<std::size_t>({0}),
        "maintenance should park an inactive canonical owner's durable overlay instead of retrying it with another source descriptor");

    controller.ActivateSource(
        CanonicalOwnerSourceIdentity(),
        CanonicalOwnerSourceDescriptor());
    const specforge::SampleLabelingMaintenanceResult
        maintenance =
            RunMaintenanceUntilIdle(controller);
    const specforge::SampleLabelingTask* retried_task =
        ActiveTask(controller);
    const specforge::SampleLabelingAsdfStoreOpenResult
        retried_generation =
            specforge::OpenSampleLabelingAsdfDocumentStore(
                asdf_path,
                specforge::SampleLabelingCompatibilityView(
                    CanonicalOwnerSourceDescriptor()));
    const specforge::SampleLabelingStateCacheLoadResult
        clean_cache =
            specforge::LoadSampleLabelingStateCache(
                cache_path);
    const specforge::SampleLabelingTask* clean_task =
        FindTask(
            clean_cache.cache,
            "canonical-source",
            "quality-task");
    Require(
        canonical_publication_calls == 2 &&
            maintenance.output_retry_attempted &&
            maintenance.canonical_output_published &&
            retry_reused_roster &&
            retried_task != nullptr &&
            retried_task->pending_sample_indices.empty() &&
            retried_task->save_state.kind ==
                specforge::SampleLabelSaveStateKind::
                    AutosavedToOutput &&
            retried_generation.succeeded() &&
            retried_generation.snapshot->document()
                    .annotation.values ==
                std::vector<std::int32_t>({2, 2, 7}) &&
            clean_task != nullptr &&
            clean_task->pending_sample_indices.empty(),
        "canonical value retry should publish the checkpointed generation, reuse the roster, and clear the overlay");
}

void TestCanonicalAsdfMetadataMutationsPublishFullGenerations()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_canonical_metadata_boundary");
    const std::filesystem::path asdf_path =
        directory / "quality.asdf";
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const specforge::SampleLabelingDocument original =
        CanonicalOwnerDocument();
    Require(
        specforge::WriteSampleLabelingAsdfDocumentAtomically(
            asdf_path,
            original)
            .succeeded(),
        "canonical metadata boundary fixture should publish its initial generation");
    std::vector<unsigned char> forward_bytes =
        ReadBinaryFile(asdf_path);
    ReplaceBinaryTextOnce(
        forward_bytes,
        "\nschema_version: ",
        "\nfuture_root: \"metadata-root-survives\"\nschema_version: ");
    ReplaceBinaryTextOnce(
        forward_bytes,
        "    shortcut: \"a\"\n",
        "    shortcut: \"a\"\n    future_label: \"metadata-label-survives\"\n");
    WriteBinaryFile(asdf_path, forward_bytes);
    SaveCanonicalOwnerCache(
        cache_path,
        asdf_path);

    std::size_t value_publication_calls = 0;
    std::size_t document_publication_calls = 0;
    specforge::SampleLabelingController controller(
        cache_path,
        [](const std::filesystem::path& path) {
            return specforge::LoadSampleLabelingStateCache(
                path);
        },
        [](specforge::SampleLabelingTask& task,
           const specforge::SampleLabelResultMetadataSource* source) {
            return specforge::PublishLegacySampleLabelingTaskOutput(
                task,
                source);
        },
        [&value_publication_calls](
            specforge::SampleLabelingAsdfOpenSnapshot& snapshot,
            std::span<const std::int32_t> values) {
            ++value_publication_calls;
            return specforge::RewriteSampleLabelingAsdfValuesAtomically(
                snapshot,
                values);
        },
        [&document_publication_calls](
            const specforge::SampleLabelingAsdfOpenSnapshot& snapshot,
            const specforge::SampleLabelingDocument& document,
            const specforge::SampleLabelingCanonicalSourceDescriptor& source) {
            ++document_publication_calls;
            return specforge::
                RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
                    snapshot,
                    document,
                    specforge::SampleLabelingCompatibilityView(source));
        });
    controller.ActivateSource(
        CanonicalOwnerSourceIdentity(),
        CanonicalOwnerSourceDescriptor());
    const std::optional<specforge::SampleAnnotationResult>
        initial_projection =
            controller.ActiveCanonicalAsdfAnnotationProjection();
    Require(
        initial_projection && initial_projection->labeling_document,
        "canonical metadata fixture should expose its initial generation");
    const std::shared_ptr<const specforge::SampleLabelingDocument>
        initial_generation = initial_projection->labeling_document;

    const specforge::SampleLabelingOperationResult renamed =
        controller.RenameActiveTask(
            "Locally renamed canonical task");
    const specforge::SampleLabelingOperationResult added =
        controller.UpsertActiveLabel({9, "temporary", 't'});
    const specforge::SampleLabelingOperationResult edited =
        controller.UpdateActiveLabel(
            2,
            {2, "locally edited", 'e'},
            false);
    const specforge::SampleLabelingOperationResult removed =
        controller.RemoveActiveLabel(9);
    const specforge::SampleLabelingOperationResult code_changed =
        controller.UpdateActiveLabel(
            7,
            {8, "recoded", 'c'},
            true);
    Require(
        renamed.changed && added.changed && edited.changed &&
            removed.changed && code_changed.changed &&
            renamed.output_save_attempted && renamed.output_saved &&
            added.output_save_attempted && added.output_saved &&
            edited.output_save_attempted && edited.output_saved &&
            removed.output_save_attempted && removed.output_saved &&
            code_changed.output_save_attempted &&
            code_changed.output_saved &&
            !code_changed.output_retry_scheduled &&
            value_publication_calls == 0 &&
            document_publication_calls == 5 &&
            !controller.NextMaintenanceDeadline(),
        "canonical metadata mutations should publish and reopen one full document generation each without using the values-only path");

    const specforge::SampleLabelingAsdfStoreOpenResult published =
        specforge::OpenSampleLabelingAsdfDocumentStore(
            asdf_path,
            specforge::SampleLabelingCompatibilityView(
                CanonicalOwnerSourceDescriptor()));
    const specforge::SampleLabelingStateCacheLoadResult
        clean_cache =
            specforge::LoadSampleLabelingStateCache(
                cache_path);
    const specforge::SampleLabelingTask* clean_task =
        FindTask(
            clean_cache.cache,
            "canonical-source",
            "quality-task");
    const std::optional<specforge::SampleAnnotationResult>
        current_projection =
            controller.ActiveCanonicalAsdfAnnotationProjection();
    Require(
        published.succeeded() &&
            published.snapshot->document().annotation.name ==
                original.annotation.name &&
            published.snapshot->document().labeling.name ==
                "Locally renamed canonical task" &&
            published.snapshot->document().labeling.labels.size() == 2 &&
            published.snapshot->document().labeling.labels[0].code == 2 &&
            published.snapshot->document().labeling.labels[0].name ==
                "locally edited" &&
            published.snapshot->document().labeling.labels[0].shortcut ==
                "e" &&
            published.snapshot->document().labeling.labels[1].code == 8 &&
            published.snapshot->document().labeling.labels[1].name ==
                "recoded" &&
            published.snapshot->document().labeling.labels[1].shortcut ==
                "c" &&
            published.snapshot->document().annotation.values ==
                std::vector<std::int32_t>({-1, 2, 8}) &&
            BinaryFileContains(
                asdf_path,
                "metadata-root-survives") &&
            BinaryFileContains(
                asdf_path,
                "metadata-label-survives") &&
            clean_task != nullptr &&
            !clean_task->metadata_save_pending &&
            clean_task->pending_sample_indices.empty() &&
            current_projection &&
            current_projection->labeling_document &&
            current_projection->labeling_document !=
                initial_generation &&
            current_projection->labeling_document->annotation.values ==
                std::vector<std::int32_t>({-1, 2, 8}),
        "metadata and recoded values should publish as one reopened generation while preserving forward-compatible metadata");
}

void TestCanonicalAsdfMetadataReopenFailureRetriesFromCurrentGeneration()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_canonical_metadata_reopen_retry");
    const std::filesystem::path asdf_path =
        directory / "quality.asdf";
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const specforge::SampleLabelingDocument original =
        CanonicalOwnerDocument();
    Require(
        specforge::WriteSampleLabelingAsdfDocumentAtomically(
            asdf_path,
            original)
            .succeeded(),
        "canonical metadata reopen fixture should publish its initial generation");
    std::vector<unsigned char> initial_bytes =
        ReadBinaryFile(asdf_path);
    ReplaceBinaryTextOnce(
        initial_bytes,
        "\nschema_version: ",
        "\nfuture_root: \"old-generation\"\nschema_version: ");
    WriteBinaryFile(asdf_path, initial_bytes);
    SaveCanonicalOwnerCache(cache_path, asdf_path);

    std::size_t document_publication_calls = 0;
    specforge::SampleLabelingController controller(
        cache_path,
        [](const std::filesystem::path& path) {
            return specforge::LoadSampleLabelingStateCache(path);
        },
        [](specforge::SampleLabelingTask& task,
           const specforge::SampleLabelResultMetadataSource* source) {
            return specforge::PublishLegacySampleLabelingTaskOutput(
                task,
                source);
        },
        [](specforge::SampleLabelingAsdfOpenSnapshot& snapshot,
           std::span<const std::int32_t> values) {
            return specforge::RewriteSampleLabelingAsdfValuesAtomically(
                snapshot,
                values);
        },
        [&document_publication_calls](
            const specforge::SampleLabelingAsdfOpenSnapshot& snapshot,
            const specforge::SampleLabelingDocument& document,
            const specforge::SampleLabelingCanonicalSourceDescriptor& source) {
            ++document_publication_calls;
            if (document_publication_calls == 1) {
                const specforge::SampleLabelingAsdfStoreWriteResult write =
                    specforge::RewriteSampleLabelingAsdfDocumentAtomically(
                        snapshot,
                        document);
                if (!write.succeeded()) {
                    return specforge::
                        SampleLabelingAsdfStoreGenerationWriteResult{
                            .error = write.error};
                }
                return specforge::
                    SampleLabelingAsdfStoreGenerationWriteResult{
                        .document_replaced = true,
                        .error = {
                            .kind = specforge::
                                SampleLabelingAsdfStoreErrorKind::
                                    AtomicWriteFailure,
                            .message =
                                "injected post-rewrite reopen failure"}};
            }
            return specforge::
                RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
                    snapshot,
                    document,
                    specforge::SampleLabelingCompatibilityView(source));
        });
    controller.ActivateSource(
        CanonicalOwnerSourceIdentity(),
        CanonicalOwnerSourceDescriptor());
    const std::optional<specforge::SampleAnnotationResult>
        initial_projection =
            controller.ActiveCanonicalAsdfAnnotationProjection();
    Require(
        initial_projection && initial_projection->labeling_document,
        "canonical reopen fixture should expose its initial snapshot");
    const std::shared_ptr<const specforge::SampleLabelingDocument>
        initial_generation = initial_projection->labeling_document;

    const specforge::SampleLabelingOperationResult failed =
        controller.RenameActiveTask(
            "Renamed through retry");
    const specforge::SampleLabelingTask* failed_task =
        ActiveTask(controller);
    const specforge::SampleLabelingAsdfStoreOpenResult
        replaced_generation =
            specforge::OpenSampleLabelingAsdfDocumentStore(
                asdf_path,
                specforge::SampleLabelingCompatibilityView(
                    CanonicalOwnerSourceDescriptor()));
    Require(
        failed.changed && failed.state_saved &&
            failed.output_save_attempted &&
            !failed.output_saved &&
            failed.output_retry_scheduled &&
            document_publication_calls == 1 &&
            failed_task != nullptr &&
            failed_task->metadata_save_pending &&
            !controller.ActiveCanonicalAsdfAnnotationProjection() &&
            replaced_generation.succeeded() &&
            replaced_generation.snapshot->document().labeling.name ==
                "Renamed through retry",
        "a post-rewrite reopen failure must remain pending and discard the stale active snapshot instead of reporting success");

    std::vector<unsigned char> newer_bytes =
        ReadBinaryFile(asdf_path);
    ReplaceBinaryTextOnce(
        newer_bytes,
        "old-generation",
        "new-generation");
    WriteBinaryFile(asdf_path, newer_bytes);

    const specforge::SampleLabelingMaintenanceResult maintenance =
        RunMaintenanceUntilIdle(controller);
    const specforge::SampleLabelingTask* retried_task =
        ActiveTask(controller);
    const std::optional<specforge::SampleAnnotationResult>
        retried_projection =
            controller.ActiveCanonicalAsdfAnnotationProjection();
    const specforge::SampleLabelingStateCacheLoadResult clean_cache =
        specforge::LoadSampleLabelingStateCache(cache_path);
    const specforge::SampleLabelingTask* clean_task =
        FindTask(
            clean_cache.cache,
            "canonical-source",
            "quality-task");
    Require(
        document_publication_calls == 2 &&
            maintenance.output_retry_attempted &&
            maintenance.canonical_output_published &&
            retried_task != nullptr &&
            !retried_task->metadata_save_pending &&
            retried_task->pending_sample_indices.empty() &&
            retried_projection &&
            retried_projection->labeling_document &&
            retried_projection->labeling_document !=
                initial_generation &&
            retried_projection->labeling_document->labeling.name ==
                "Renamed through retry" &&
            BinaryFileContains(asdf_path, "new-generation") &&
            !BinaryFileContains(asdf_path, "old-generation") &&
            clean_task != nullptr &&
            !clean_task->metadata_save_pending,
        "retry must reopen the current durable generation before rewriting so stale unknown metadata cannot overwrite a newer file");
}

void TestCanonicalAsdfProjectionDowngradesWhenDeactivated()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_canonical_owner_deactivation");
    const std::filesystem::path asdf_path =
        directory / "quality.asdf";
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    Require(
        specforge::WriteSampleLabelingAsdfDocumentAtomically(
            asdf_path,
            CanonicalOwnerDocument())
            .succeeded(),
        "canonical deactivation fixture should publish its ASDF document");

    specforge::SampleLabelingTask cached =
        specforge::CreateSampleLabelingTask(
            "quality-task",
            "structural cache owner",
            3);
    cached.output_path = asdf_path;
    cached.output_format =
        specforge::SampleLabelingOutputArtifactFormat::
            CanonicalAsdf;
    specforge::SampleLabelingSourceState source;
    source.sample_count = 3;
    source.source_name = "spectra.npy";
    source.source_fingerprint =
        "canonical-source-fingerprint";
    source.active_task_id = "quality-task";
    source.tasks.push_back(std::move(cached));
    specforge::SampleLabelingStateCache cache;
    cache.sources.emplace(
        "canonical-source",
        std::move(source));
    Require(
        specforge::SaveSampleLabelingStateCache(
            cache_path,
            cache),
        "canonical deactivation cache fixture should save");

    specforge::SampleLabelingController controller(cache_path);
    controller.ActivateSource(
        CanonicalOwnerSourceIdentity(),
        CanonicalOwnerSourceDescriptor());
    Require(
        ActiveTask(controller) != nullptr &&
            ActiveTask(controller)->values_are_authoritative &&
            ActiveTask(controller)->values ==
                CanonicalOwnerDocument().annotation.values,
        "canonical deactivation fixture should begin with a hydrated authoritative projection");

    specforge::SampleLabelingSourceState authoritative_prepared_state;
    authoritative_prepared_state.sample_count = 3;
    authoritative_prepared_state.source_name = "spectra.npy";
    authoritative_prepared_state.source_fingerprint =
        "canonical-source-fingerprint";
    authoritative_prepared_state.context_fingerprint =
        "annotation-sensitive-context";
    authoritative_prepared_state.active_task_id =
        "quality-task";
    authoritative_prepared_state.tasks =
        *ActiveSourceTasks(controller);
    authoritative_prepared_state.tasks[0].values[1] = 7;
    authoritative_prepared_state.tasks[0]
        .pending_sample_indices.insert(1);
    authoritative_prepared_state.tasks[0].save_state.kind =
        specforge::SampleLabelSaveStateKind::Pending;

    const std::optional<specforge::SampleLabelingSourceState>
        exported_state = controller.SourceStateForIdentity(
            "canonical-source");
    Require(
        exported_state &&
            exported_state->tasks.size() == 1 &&
            !exported_state->tasks[0]
                 .values_are_authoritative &&
            exported_state->tasks[0].values ==
                std::vector<int>({-1, -1, -1}),
        "exporting a prepared source must not copy a snapshot-bound canonical base projection");

    const std::uint64_t generation_before_deactivation =
        controller.active_source_tasks_generation();
    const specforge::SampleLabelingOperationResult deactivated =
        controller.DeactivateActiveTask();
    Require(
        deactivated.accepted &&
            deactivated.changed &&
            deactivated.state_saved,
        "a clean canonical owner should deactivate after its selection is saved");
    const specforge::SampleLabelingTask* structural =
        ActiveSourceTask(controller, "quality-task");
    Require(
        structural != nullptr &&
            !structural->values_are_authoritative &&
            structural->values ==
                std::vector<int>({-1, -1, -1}) &&
            structural->pending_sample_indices.empty() &&
            structural->labeled_count == 0 &&
            structural->label_usage_counts.empty() &&
            controller.active_source_tasks_generation() >
                generation_before_deactivation,
        "losing the canonical snapshot on deactivation must leave only structural local state and invalidate projection caches");

    WriteTextFile(
        asdf_path,
        "not an ASDF document");
    specforge::SampleLabelingController prepared_receiver(
        cache_path);
    const specforge::SampleLabelingPreparedSourceActivationResult
        failed_prepared_activation =
            prepared_receiver.ActivatePreparedSource(
                CanonicalOwnerSourceIdentity(),
                std::move(authoritative_prepared_state),
                CanonicalOwnerSourceDescriptor());
    const specforge::SampleLabelingTask* failed_prepared_owner =
        ActiveSourceTask(
            prepared_receiver,
            "quality-task");
    Require(
        failed_prepared_activation
                .prepared_task_projection_changed &&
            ActiveTask(prepared_receiver) == nullptr &&
            failed_prepared_owner != nullptr &&
            !failed_prepared_owner
                 ->values_are_authoritative &&
            failed_prepared_owner->values ==
                std::vector<int>({-1, 7, -1}) &&
            failed_prepared_owner
                    ->pending_sample_indices ==
                std::unordered_set<std::size_t>({1}),
        "a prepared canonical projection must fail closed to pending-only structural state when hydration cannot establish a snapshot");

    std::error_code remove_error;
    std::filesystem::remove(
        asdf_path,
        remove_error);
    Require(
        !remove_error,
        "canonical deactivation fixture should remove its malformed generation");

    specforge::SampleLabelingDocument next_generation =
        CanonicalOwnerDocument();
    next_generation.labeling.name =
        "Canonical quality generation C";
    next_generation.annotation.values = {7, -1, 2};
    Require(
        specforge::WriteSampleLabelingAsdfDocumentAtomically(
            asdf_path,
            next_generation)
            .succeeded(),
        "deactivation should release the one-file owner lease for a later ASDF generation");

    const specforge::SampleLabelingOperationResult reactivated =
        controller.ActivateTask("quality-task");
    Require(
        reactivated.accepted &&
            ActiveTask(controller) != nullptr &&
            ActiveTask(controller)->values_are_authoritative &&
            ActiveTask(controller)->task_name ==
                "Canonical quality generation C" &&
            ActiveTask(controller)->values ==
                std::vector<int>({7, -1, 2}),
        "reactivating a downgraded owner must hydrate the current ASDF generation instead of reusing the retired projection");
}

void TestCanonicalRevalidationRetainsLeaseForPendingRecoveryPatch()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_canonical_revalidation_pending_patch");
    const std::filesystem::path asdf_path =
        directory / "quality.asdf";
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    Require(
        specforge::WriteSampleLabelingAsdfDocumentAtomically(
            asdf_path,
            CanonicalOwnerDocument())
            .succeeded(),
        "pending revalidation fixture should publish its canonical owner");

    specforge::SampleLabelingTask cached =
        specforge::CreateSampleLabelingTask(
            "quality-task",
            "structural cache owner",
            3);
    cached.output_path = asdf_path;
    cached.output_format =
        specforge::SampleLabelingOutputArtifactFormat::
            CanonicalAsdf;
    specforge::SampleLabelingSourceState source;
    source.sample_count = 3;
    source.source_name = "spectra.npy";
    source.source_fingerprint =
        "canonical-source-fingerprint";
    source.active_task_id = "quality-task";
    source.tasks.push_back(std::move(cached));
    specforge::SampleLabelingStateCache cache;
    cache.sources.emplace(
        "canonical-source",
        std::move(source));
    Require(
        specforge::SaveSampleLabelingStateCache(
            cache_path,
            cache),
        "pending revalidation cache fixture should save");

    specforge::SampleLabelingController owner(cache_path);
    owner.ActivateSource(
        CanonicalOwnerSourceIdentity(),
        CanonicalOwnerSourceDescriptor());
    Require(
        ActiveTask(owner) != nullptr,
        "pending revalidation fixture should hydrate its owner");

    specforge::ExclusiveFileLeaseAcquireResult blocked_commit =
        specforge::TryAcquireExclusiveFileLease(
            specforge::SampleLabelingStateCoordinationDirectory(
                cache_path) /
            "cache-commit.lock");
    Require(
        blocked_commit.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::Acquired,
        "pending revalidation fixture should block its cache commit");
    const specforge::SampleLabelingWriteOperationResult pending =
        owner.AssignLabel(0, 2);
    Require(
        pending.write.changed &&
            !pending.operation.state_saved &&
            owner.state_save_pending(),
        "pending revalidation fixture should retain an unpersisted recovery overlay");

    specforge::SourceCollectionIdentity changed_context =
        CanonicalOwnerSourceIdentity();
    changed_context.context_fingerprint =
        "incompatible-roster-context";
    specforge::SampleLabelingCanonicalSourceDescriptor
        incompatible = CanonicalOwnerSourceDescriptor();
    incompatible.sample_names[0] = "renamed-sample-a";
    owner.ActivateSource(
        changed_context,
        std::move(incompatible));
    Require(
        ActiveTask(owner) == nullptr,
        "incompatible canonical roster should fail revalidation closed");

    specforge::SampleLabelingController blocked(cache_path);
    blocked.ActivateSource(
        CanonicalOwnerSourceIdentity(),
        CanonicalOwnerSourceDescriptor());
    Require(
        ActiveTask(blocked) == nullptr,
        "unpersisted recovery overlay must keep the deferred canonical owner lease");

    blocked_commit.lease.Reset();
    Require(
        owner.FlushStateCache(),
        "pending recovery overlay should flush after commit contention ends");
    specforge::SampleLabelingController after_flush(
        cache_path);
    after_flush.ActivateSource(
        CanonicalOwnerSourceIdentity(),
        CanonicalOwnerSourceDescriptor());
    Require(
        ActiveTask(after_flush) != nullptr &&
            ActiveTask(after_flush)->values[0] == 2,
        "successful recovery flush should release the deferred canonical owner lease and preserve the overlay");
}

void TestCanonicalAsdfTaskOwnerFailsClosed()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_canonical_owner_fail_closed");
    const std::filesystem::path asdf_path =
        directory / "quality.asdf";
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    WriteTextFile(
        asdf_path,
        "not an ASDF document");

    specforge::SampleLabelingStateCache cache;
    specforge::SampleLabelingSourceState source;
    source.sample_count = 3;
    source.active_task_id = "quality-task";
    specforge::SampleLabelingTask cached =
        specforge::CreateSampleLabelingTask(
            "quality-task",
            "must not activate",
            3);
    cached.output_path = asdf_path;
    cached.output_format =
        specforge::SampleLabelingOutputArtifactFormat::
            CanonicalAsdf;
    source.tasks.push_back(std::move(cached));
    cache.sources.emplace(
        "canonical-source",
        std::move(source));
    Require(
        specforge::SaveSampleLabelingStateCache(
            cache_path,
            cache),
        "malformed canonical owner cache fixture should save");

    {
        specforge::SampleLabelingController controller(
            cache_path);
        controller.ActivateSource(
            CanonicalOwnerSourceIdentity(),
            CanonicalOwnerSourceDescriptor());
        Require(
            ActiveTask(controller) == nullptr,
            "malformed canonical owner must fail closed instead of activating cache placeholders or falling back to NPY");
    }

    std::error_code replace_error;
    std::filesystem::remove(
        asdf_path,
        replace_error);
    Require(
        !replace_error,
        "source-mismatch fixture should remove the malformed generation");
    Require(
        specforge::WriteSampleLabelingAsdfDocumentAtomically(
            asdf_path,
            CanonicalOwnerDocument())
            .succeeded(),
        "source-mismatch fixture should publish a valid ASDF document");
    specforge::SourceCollectionIdentity mismatched_identity =
        CanonicalOwnerSourceIdentity();
    mismatched_identity.source_fingerprint =
        "different-active-fingerprint";
    specforge::SampleLabelingCanonicalSourceDescriptor
        mismatched_descriptor =
            CanonicalOwnerSourceDescriptor();
    mismatched_descriptor.source_fingerprint =
        mismatched_identity.source_fingerprint;
    specforge::SampleLabelingController mismatched(
        cache_path);
    mismatched.ActivateSource(
        mismatched_identity,
        std::move(mismatched_descriptor));
    Require(
        ActiveTask(mismatched) == nullptr,
        "source-mismatched canonical owner must fail closed without falling back to legacy hydration");
}

void TestCanonicalAsdfTaskOwnerRejectsUndefinedPendingCode()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_canonical_owner_undefined_pending");
    const std::filesystem::path asdf_path =
        directory / "quality.asdf";
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    Require(
        specforge::WriteSampleLabelingAsdfDocumentAtomically(
            asdf_path,
            CanonicalOwnerDocument())
            .succeeded(),
        "undefined pending fixture should publish its canonical generation");

    specforge::SampleLabelingTask cached =
        specforge::CreateSampleLabelingTask(
            "quality-task",
            "stale local state",
            3);
    cached.output_path = asdf_path;
    cached.output_format =
        specforge::SampleLabelingOutputArtifactFormat::
            CanonicalAsdf;
    cached.values[1] = 99;
    cached.pending_sample_indices.insert(1);
    cached.save_state.kind =
        specforge::SampleLabelSaveStateKind::Pending;

    specforge::SampleLabelingSourceState source;
    source.sample_count = 3;
    source.source_name = "spectra.npy";
    source.source_fingerprint =
        "canonical-source-fingerprint";
    source.active_task_id = "quality-task";
    source.tasks.push_back(std::move(cached));
    specforge::SampleLabelingStateCache cache;
    cache.sources.emplace(
        "canonical-source",
        std::move(source));
    Require(
        specforge::SaveSampleLabelingStateCache(
            cache_path,
            cache),
        "undefined pending cache fixture should save");

    specforge::SampleLabelingController controller(cache_path);
    controller.ActivateSource(
        CanonicalOwnerSourceIdentity(),
        CanonicalOwnerSourceDescriptor());
    Require(
        ActiveTask(controller) == nullptr,
        "a pending code absent from the leased canonical generation must fail activation closed");
}

std::size_t CountLeaseFiles(
    const std::filesystem::path& directory)
{
    std::error_code error;
    if (!std::filesystem::exists(directory, error) || error) {
        return 0;
    }
    std::size_t count = 0;
    for (std::filesystem::directory_iterator entries(
             directory,
             error);
         !error && entries !=
             std::filesystem::directory_iterator{};
         entries.increment(error)) {
        if (entries->is_regular_file(error) && !error) {
            ++count;
        }
    }
    Require(!error, "could not enumerate labeling lease files");
    return count;
}

std::filesystem::path LabelingTargetLeasePath(
    const std::filesystem::path& cache_path,
    std::string_view lease_key)
{
    specforge::StableSha256 digest;
    digest.Append(
        "specforge.sample-labeling.edit-lease.v1\n");
    digest.Append(lease_key);
    return specforge::SampleLabelingStateCoordinationDirectory(
               cache_path) /
        "targets" /
        (digest.FinishHex() + ".lock");
}

specforge::ExclusiveFileLeaseAcquireResult
TryAcquireCurrentArtifactLease(
    const std::filesystem::path& cache_path,
    const std::filesystem::path& output_path)
{
    const std::vector<std::string> identities =
        specforge::OutputPathIdentityKeys(output_path);
    const auto file_object = std::find_if(
        identities.begin(),
        identities.end(),
        [](std::string_view identity) {
            return identity.starts_with(
                "file-object:");
        });
    Require(
        file_object != identities.end(),
        "artifact lease fixture should resolve the current file-object identity");
    return specforge::TryAcquireExclusiveFileLease(
        LabelingTargetLeasePath(
            cache_path,
            "artifact\n" +
                *file_object));
}

specforge::ExclusiveFileLeaseAcquireResult
TryAcquireCurrentStableArtifactLease(
    const std::filesystem::path& cache_path,
    const std::filesystem::path& output_path)
{
    const std::string stable_identity =
        specforge::SourcePathIdentityKey(output_path);
    Require(
        !stable_identity.empty(),
        "artifact lease fixture should resolve the normalized output identity");
    return specforge::TryAcquireExclusiveFileLease(
        LabelingTargetLeasePath(
            cache_path,
            "artifact-path\n" + stable_identity));
}

bool CreateDirectoryJunction(
    const std::filesystem::path& junction,
    const std::filesystem::path& target)
{
    std::wstring command =
        L"cmd.exe /d /c mklink /J \"" +
        junction.wstring() + L"\" \"" +
        target.wstring() + L"\" >nul";
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (CreateProcessW(
            nullptr,
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process) == FALSE) {
        return false;
    }
    CloseHandle(process.hThread);
    const DWORD wait =
        WaitForSingleObject(process.hProcess, 10'000);
    DWORD exit_code = 1;
    const bool succeeded =
        wait == WAIT_OBJECT_0 &&
        GetExitCodeProcess(
            process.hProcess,
            &exit_code) != FALSE &&
        exit_code == 0;
    CloseHandle(process.hProcess);
    return succeeded;
}

bool CreateFileHardLink(
    const std::filesystem::path& link,
    const std::filesystem::path& existing)
{
    return CreateHardLinkW(
               link.c_str(),
               existing.c_str(),
               nullptr) != FALSE;
}

const specforge::SampleLabelingTask* FindTask(
    const specforge::SampleLabelingStateCache& cache,
    std::string_view source_identity,
    std::string_view task_id)
{
    const auto source =
        cache.sources.find(std::string(source_identity));
    if (source == cache.sources.end()) {
        return nullptr;
    }
    const auto task =
        std::find_if(
            source->second.tasks.begin(),
            source->second.tasks.end(),
            [task_id](const specforge::SampleLabelingTask& candidate) {
                return candidate.task_id == task_id;
            });
    return task == source->second.tasks.end()
        ? nullptr
        : &*task;
}

void CreateFormalTask(
    specforge::SampleLabelingController& controller,
    std::string source_identity,
    std::string task_id,
    const std::filesystem::path& output_path,
    int label_code)
{
    controller.ActivateSource(
        std::move(source_identity),
        3);
    specforge::SampleLabelSet label_set;
    Require(
        specforge::UpsertSampleLabel(
            label_set,
            specforge::SampleLabelDefinition{
                label_code,
                task_id,
                '\0'}),
        "formal task fixture should define its legacy label metadata");
    Require(
        controller.CreateTaskFromAnnotation(
                      task_id,
                      task_id,
                      std::move(label_set),
                      {-1, -1, -1},
                      output_path,
                      false)
            .output_saved,
        "formal task fixture should save its existing legacy output owner");
    Require(
        controller.DeactivateActiveTask().changed,
        "formal task fixture should deactivate its task");
    Require(
        controller.FlushStateCache(),
        "formal task fixture should flush its cache");
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

void TestSampleLabelExportSnapshotUsesCanonicalRosterAndStableSerialization()
{
    specforge::SampleLabelingTask task =
        specforge::CreateSampleLabelingTask(
            "quality",
            "Quality",
            3);
    Require(
        specforge::UpsertSampleLabel(
            task.label_set,
            specforge::SampleLabelDefinition{
                5,
                "selected",
                's'}),
        "export snapshot fixture should add its label");
    Require(
        specforge::AssignSampleLabel(task, 0, 5).accepted &&
            specforge::AssignSampleLabel(task, 2, 5).accepted,
        "export snapshot fixture should assign canonical source rows");

    specforge::SampleLabelingCanonicalSourceDescriptor source{
        .base_identity = "folder-source",
        .source_kind = "folder",
        .source_name = "spectra",
        .source_fingerprint = "folder-fingerprint",
        .sample_count = 3,
        .sample_names = {
            "zeta.fits",
            "alpha.fits",
            "beta.fits"},
    };
    std::string error;
    const std::optional<specforge::SampleLabelExportSnapshot>
        named_snapshot =
            specforge::BuildSampleLabelExportSnapshot(
                specforge::SampleLabelExportFormat::Csv,
                task,
                source,
                &error);
    Require(
        named_snapshot.has_value(),
        error.empty()
            ? "canonical named export snapshot should build"
            : error);
    Require(
        named_snapshot->format ==
                specforge::SampleLabelExportFormat::Csv &&
            named_snapshot->source_kind == "folder" &&
            named_snapshot->sample_names ==
                std::vector<std::string>({
                    "zeta.fits",
                    "alpha.fits",
                    "beta.fits"}) &&
            !named_snapshot->uses_source_index &&
            named_snapshot->values ==
                std::vector<int>({5, -1, 5}) &&
            named_snapshot->labels.labels.size() == 1 &&
            named_snapshot->labels.labels[0].name ==
                "selected",
        "snapshot should copy active task data in unsorted canonical source roster order");

    task.values[0] = specforge::kUnlabeledSampleLabelCode;
    source.sample_names[0] = "changed.fits";
    Require(
        named_snapshot->values[0] == 5 &&
            named_snapshot->sample_names[0] ==
                "zeta.fits",
        "export snapshot should own an immutable copy of task and source data");

    Require(
        specforge::SerializeSampleLabelValueForExport(
            named_snapshot->labels,
            specforge::kUnlabeledSampleLabelCode) ==
                "unlabeled" &&
            specforge::SerializeSampleLabelValueForExport(
                named_snapshot->labels,
                5) == "selected" &&
            specforge::SerializeSampleLabelValueForExport(
                named_snapshot->labels,
                42) == "42" &&
            specforge::FormatSampleLabelValue(
                named_snapshot->labels,
                5) == "selected (5)",
        "export label serialization should be stable and separate from presentation text");

    task.values = {5, -1, 5};
    source.sample_names.clear();
    error.clear();
    const std::optional<specforge::SampleLabelExportSnapshot>
        source_index_snapshot =
            specforge::BuildSampleLabelExportSnapshot(
                specforge::SampleLabelExportFormat::Npy,
                task,
                source,
                &error);
    Require(
        source_index_snapshot.has_value() &&
            source_index_snapshot->uses_source_index &&
            source_index_snapshot->sample_names.empty(),
        error.empty()
            ? "unnamed export snapshot should retain source-index identity"
            : error);

    specforge::SampleLabelingCanonicalSourceDescriptor invalid_source =
        source;
    invalid_source.sample_names = {"duplicate", "duplicate", "third"};
    error.clear();
    Require(
        !specforge::BuildSampleLabelExportSnapshot(
             specforge::SampleLabelExportFormat::Csv,
             task,
             invalid_source,
             &error) &&
            !error.empty(),
        "snapshot builder should reject a non-canonical sample-name roster");

    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_label_export_snapshot_dispatch");
    const std::filesystem::path direct_path =
        directory / "direct.npy";
    const std::filesystem::path dispatched_path =
        directory / "dispatched.npy";
    error.clear();
    Require(
        specforge::ExportLabelValuesToNpy(
            direct_path,
            source_index_snapshot->values,
            &error),
        error.empty()
            ? "reference NPY export should succeed"
            : error);
    error.clear();
    Require(
        specforge::ExportSampleLabelSnapshot(
            dispatched_path,
            *source_index_snapshot,
            &error),
        error.empty()
            ? "snapshot-dispatched NPY export should succeed"
            : error);
    Require(
        ReadBinaryFile(direct_path) ==
            ReadBinaryFile(dispatched_path),
        "NPY snapshot dispatcher must preserve the existing byte representation");

    specforge::SampleLabelExportSnapshot csv_snapshot =
        *named_snapshot;
    const std::filesystem::path csv_path =
        directory / "labels.csv";
    error.clear();
    Require(
        specforge::ExportSampleLabelSnapshot(
            csv_path,
            csv_snapshot,
            &error),
        error.empty()
            ? "snapshot-dispatched CSV export should succeed"
            : error);
    specforge::BoundedCsvFileReader csv_reader(csv_path);
    const specforge::CsvRecordReadResult header =
        csv_reader.ReadRecord();
    const specforge::CsvRecordReadResult first =
        csv_reader.ReadRecord();
    const specforge::CsvRecordReadResult second =
        csv_reader.ReadRecord();
    const specforge::CsvRecordReadResult third =
        csv_reader.ReadRecord();
    Require(
        header.record ==
                specforge::CsvRecord({"filename", "label"}) &&
            first.record == specforge::CsvRecord(
                {"zeta.fits", "selected"}) &&
            second.record == specforge::CsvRecord(
                {"alpha.fits", "unlabeled"}) &&
            third.record == specforge::CsvRecord(
                {"beta.fits", "selected"}) &&
            csv_reader.ReadRecord().status ==
                specforge::CsvRecordReadStatus::End,
        "CSV dispatcher should preserve the canonical source roster order");
}

void TestLabelValueExportsAreStateless()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_label_values_npy_export");
    const std::filesystem::path direct_export =
        directory / "direct.npy";
    const std::filesystem::path direct_sidecar =
        specforge::SampleAnnotationIoAdapter::
            MetadataPathForResult(direct_export);

    const std::array<int, 3> direct_values = {7, -1, 4};
    std::string error;
    Require(
        specforge::ExportLabelValuesToNpy(
            direct_export,
            direct_values,
            &error),
        error.empty()
            ? "one-shot NPY export should succeed"
            : error);
    Require(
        ReadTestInt32NpyPayload(direct_export) ==
            std::vector<std::int32_t>({7, -1, 4}),
        "one-shot NPY export should preserve canonical roster order and values");
    const std::vector<unsigned char> direct_bytes =
        ReadBinaryFile(direct_export);
    WriteTextFile(direct_sidecar, "sidecar sentinel\n");
    error.clear();
    const std::array<int, 3> replacement_values = {1, 1, 1};
    Require(
        !specforge::ExportLabelValuesToNpy(
            direct_export,
            replacement_values,
            &error) &&
            error.find("metadata sidecar") !=
                std::string::npos,
        "one-shot NPY export should reject a target with adjacent legacy metadata");
    Require(
        ReadBinaryFile(direct_export) == direct_bytes &&
        ReadTextFile(direct_sidecar) ==
            "sidecar sentinel\n",
        "a rejected legacy pair must preserve both the NPY payload and sidecar");

    const std::filesystem::path cache_path =
        directory / "labeling-cache.json";
    specforge::SampleLabelingController controller(
        cache_path);
    ActivateCanonicalTestSource(
        controller,
        "export-source",
        3);
    Require(
        controller.StartOrResumeTemporaryTask().accepted,
        "export fixture should create a temporary task");
    Require(
        controller.UpsertActiveLabel(
            specforge::SampleLabelDefinition{
                5,
                "selected",
                's'}).accepted,
        "export fixture should add a label");
    Require(
        controller.AssignLabel(0, 5).write.accepted &&
            controller.AssignLabel(2, 5).write.accepted,
        "export fixture should retain current pending values");
    Require(
        controller.RememberActivePosition(1).accepted,
        "export fixture should remember its current canonical sample");

    const specforge::SampleLabelingTask before =
        *ActiveTask(controller);
    const std::uint64_t revision_before =
        controller.View().revision;
    const bool state_save_pending_before =
        controller.state_save_pending();
    const auto maintenance_deadline_before =
        controller.NextMaintenanceDeadline();
    const std::filesystem::path controller_export =
        directory / "controller-export.npy";
    const std::filesystem::path controller_sidecar =
        specforge::SampleAnnotationIoAdapter::
            MetadataPathForResult(controller_export);

    const specforge::SampleLabelingOperationResult invalid =
        controller.ExportActiveLabelValuesToNpy(
            directory / "wrong.csv");
    Require(
        !invalid.accepted && !invalid.export_attempted &&
            invalid.issue ==
                specforge::SampleLabelingOperationResult::
                    Issue::LabelValuesExportInvalidPath,
        "the controller should reject a non-NPY export path with a stable issue");

    const std::filesystem::path blocked_export =
        directory / "blocked.npy";
    std::filesystem::create_directories(blocked_export);
    const specforge::SampleLabelingOperationResult failed =
        controller.ExportActiveLabelValuesToNpy(
            blocked_export);
    Require(
        failed.accepted && failed.export_attempted &&
            !failed.exported &&
            failed.issue ==
                specforge::SampleLabelingOperationResult::
                    Issue::LabelValuesExportFailed &&
            !failed.diagnostic.empty(),
        "an NPY export I/O failure should retain diagnostic detail behind a stable UI issue");

    const specforge::SampleLabelingOperationResult exported =
        controller.ExportActiveLabelValuesToNpy(
            controller_export);
    Require(
        exported.accepted &&
            exported.export_attempted &&
            exported.exported &&
            !exported.changed &&
            !exported.output_save_attempted &&
            !exported.output_saved &&
            exported.issue ==
                specforge::SampleLabelingOperationResult::
                    Issue::None,
        "NPY export should be reported separately from canonical persistence");
    Require(
        ReadTestInt32NpyPayload(controller_export) ==
            std::vector<std::int32_t>({5, -1, 5}),
        "controller export should use the current task values in source roster order");

    const std::filesystem::path blocked_csv_export =
        directory / "blocked-controller-export.csv";
    specforge::ExclusiveFileLeaseAcquireResult csv_target_lease =
        TryAcquireCurrentStableArtifactLease(
            cache_path,
            blocked_csv_export);
    Require(
        csv_target_lease.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::
                Acquired,
        "CSV export fixture should acquire the target's single-file lease");
    const specforge::SampleLabelingOperationResult csv_blocked =
        controller.ExportActiveLabels(
            blocked_csv_export,
            specforge::SampleLabelExportFormat::Csv);
    Require(
        !csv_blocked.accepted &&
            !csv_blocked.export_attempted &&
            csv_blocked.issue ==
                specforge::SampleLabelingOperationResult::
                    Issue::LabelValuesExportTargetProtected,
        "CSV export should respect a lease on its one target file");
    csv_target_lease.lease.Reset();

    const std::filesystem::path csv_export =
        directory / "controller-export.csv";
    const std::filesystem::path csv_adjacent =
        specforge::SampleAnnotationIoAdapter::
            MetadataPathForResult(csv_export);
    specforge::ExclusiveFileLeaseAcquireResult adjacent_lease =
        TryAcquireCurrentStableArtifactLease(
            cache_path,
            csv_adjacent);
    Require(
        adjacent_lease.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::
                Acquired,
        "CSV export fixture should hold an unrelated adjacent-path lease");
    const specforge::SampleLabelingOperationResult csv_exported =
        controller.ExportActiveLabels(
            csv_export,
            specforge::SampleLabelExportFormat::Csv);
    Require(
        csv_exported.accepted &&
            csv_exported.export_attempted &&
            csv_exported.exported &&
            !csv_exported.changed &&
            !csv_exported.output_save_attempted &&
            !csv_exported.output_saved &&
            csv_exported.issue ==
                specforge::SampleLabelingOperationResult::
                    Issue::None,
        "CSV export should remain separate from canonical persistence");
    adjacent_lease.lease.Reset();
    specforge::BoundedCsvFileReader csv_reader(csv_export);
    const specforge::CsvRecordReadResult csv_header =
        csv_reader.ReadRecord();
    const specforge::CsvRecordReadResult csv_first =
        csv_reader.ReadRecord();
    const specforge::CsvRecordReadResult csv_second =
        csv_reader.ReadRecord();
    const specforge::CsvRecordReadResult csv_third =
        csv_reader.ReadRecord();
    Require(
        csv_header.record == specforge::CsvRecord(
                {"sample", "label"}) &&
            csv_first.record == specforge::CsvRecord(
                {"0", "selected"}) &&
            csv_second.record == specforge::CsvRecord(
                {"1", "unlabeled"}) &&
            csv_third.record == specforge::CsvRecord(
                {"2", "selected"}) &&
            csv_reader.ReadRecord().status ==
                specforge::CsvRecordReadStatus::End,
        "controller CSV export should place zero-based canonical source indices in the sample column");
    Require(
        !std::filesystem::exists(csv_adjacent),
        "CSV export must not create or claim an adjacent legacy sidecar");

    const specforge::SampleLabelingTask& after =
        *ActiveTask(controller);
    Require(
        after.task_id == before.task_id &&
            after.task_name == before.task_name &&
            after.output_path == before.output_path &&
            after.output_format == before.output_format &&
            after.values == before.values &&
            after.remembered_position ==
                before.remembered_position &&
            after.pending_sample_indices ==
                before.pending_sample_indices &&
            after.metadata_save_pending ==
                before.metadata_save_pending &&
            after.save_state.kind == before.save_state.kind &&
            after.save_state.pending_count ==
                before.save_state.pending_count,
        "one-shot exports must not change the task owner, autosave state, "
        "pending overlay, or remembered sample");
    Require(
        controller.View().revision == revision_before &&
            controller.state_save_pending() ==
                state_save_pending_before &&
            controller.NextMaintenanceDeadline() ==
                maintenance_deadline_before,
        "one-shot exports must not touch controller revision or persistence scheduling");
    Require(
        !std::filesystem::exists(controller_sidecar),
        "controller export must not create an adjacent legacy sidecar");

    specforge::ExclusiveFileLeaseAcquireResult export_lease =
        TryAcquireCurrentStableArtifactLease(
            cache_path,
            controller_export);
    Require(
        export_lease.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::
                Acquired,
        "one-shot export must not retain a task artifact lease");
    specforge::ExclusiveFileLeaseAcquireResult csv_export_lease =
        TryAcquireCurrentStableArtifactLease(
            cache_path,
            csv_export);
    Require(
        csv_export_lease.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::
                Acquired,
        "one-shot CSV export must not retain its transient target lease");
}

void TestLabelValuesNpyExportRejectsManagedAndLeasedOwners()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_label_values_export_owner_guard");
    const std::filesystem::path cache_path =
        directory / "labeling-cache.json";
    const std::filesystem::path managed_output =
        directory / "managed.npy";
    const std::filesystem::path managed_alias =
        directory / "managed-alias.npy";
    const std::array<int, 3> managed_values = {2, -1, 2};
    std::string error;
    Require(
        specforge::ExportLabelValuesToNpy(
            managed_output,
            managed_values,
            &error),
        error.empty()
            ? "managed-owner fixture should create its NPY"
            : error);
    Require(
        CreateFileHardLink(
            managed_alias,
            managed_output),
        "managed-owner fixture should create a physical path alias");

    specforge::SampleLabelingStateCache cache;
    specforge::SampleLabelingSourceState managed_source;
    managed_source.sample_count = 3;
    managed_source.tasks.push_back(
        OutputTask(
            "managed-task",
            managed_output,
            specforge::SampleLabelingOutputArtifactFormat::
                LegacyNpyWithSidecar));
    cache.sources.emplace(
        "managed-source",
        std::move(managed_source));
    Require(
        specforge::SaveSampleLabelingStateCache(
            cache_path,
            cache),
        "managed-owner fixture should persist its local owner");

    specforge::SampleLabelingController controller(
        cache_path);
    ActivateCanonicalTestSource(
        controller,
        "export-source",
        3);
    Require(
        controller.StartOrResumeTemporaryTask().accepted,
        "owner-guard fixture should create its exporting task");
    Require(
        controller.UpsertActiveLabel(
            specforge::SampleLabelDefinition{
                9,
                "new",
                'n'}).accepted &&
            controller.AssignLabel(0, 9).write.accepted,
        "owner-guard fixture should prepare different export values");

    const std::vector<unsigned char> managed_bytes =
        ReadBinaryFile(managed_output);
    const auto require_protected = [](
                                       const specforge::SampleLabelingOperationResult& result,
                                       std::string_view message) {
        Require(
            !result.accepted &&
                !result.export_attempted &&
                !result.exported &&
                result.issue ==
                    specforge::SampleLabelingOperationResult::
                        Issue::LabelValuesExportTargetProtected,
            message);
    };
    require_protected(
        controller.ExportActiveLabelValuesToNpy(
            managed_output),
        "export must reject an exact managed legacy owner path");
    require_protected(
        controller.ExportActiveLabelValuesToNpy(
            managed_alias),
        "export must reject a hard-link alias of a managed legacy owner");
    Require(
        ReadBinaryFile(managed_output) == managed_bytes,
        "managed owner rejection must preserve its NPY generation");

    const std::filesystem::path sidecar_target =
        directory / "sidecar-owner.npy";
    Require(
        specforge::ExportLabelValuesToNpy(
            sidecar_target,
            managed_values,
            &error),
        "sidecar-owner fixture should create its original NPY");
    WriteTextFile(
        specforge::SampleAnnotationIoAdapter::
            MetadataPathForResult(sidecar_target),
        "legacy metadata sentinel\n");
    const std::vector<unsigned char> sidecar_target_bytes =
        ReadBinaryFile(sidecar_target);
    require_protected(
        controller.ExportActiveLabelValuesToNpy(
            sidecar_target),
        "export must reject any target carrying adjacent legacy metadata");
    Require(
        ReadBinaryFile(sidecar_target) ==
            sidecar_target_bytes,
        "sidecar-owner rejection must preserve the existing NPY payload");

    const std::filesystem::path leased_output =
        directory / "leased.npy";
    const std::filesystem::path leased_alias =
        directory / "leased-alias.npy";
    Require(
        specforge::ExportLabelValuesToNpy(
            leased_output,
            managed_values,
            &error),
        "lease-owner fixture should create its original NPY");
    Require(
        CreateFileHardLink(
            leased_alias,
            leased_output),
        "lease-owner fixture should create a physical alias");
    const std::vector<unsigned char> leased_bytes =
        ReadBinaryFile(leased_output);
    specforge::ExclusiveFileLeaseAcquireResult external_lease =
        TryAcquireCurrentStableArtifactLease(
            cache_path,
            leased_output);
    Require(
        external_lease.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::
                Acquired,
        "lease-owner fixture should emulate another instance's artifact lease");
    require_protected(
        controller.ExportActiveLabelValuesToNpy(
            leased_output),
        "export must respect another instance's legacy artifact lease");
    Require(
        ReadBinaryFile(leased_output) == leased_bytes,
        "lease rejection must preserve the protected NPY generation");
    external_lease.lease.Reset();

    specforge::ExclusiveFileLeaseAcquireResult physical_lease =
        TryAcquireCurrentArtifactLease(
            cache_path,
            leased_output);
    Require(
        physical_lease.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::
                Acquired,
        "lease-owner fixture should emulate a physical-identity lease");
    require_protected(
        controller.ExportActiveLabelValuesToNpy(
            leased_alias),
        "export must respect a physical owner lease through a hard-link alias");
    Require(
        ReadBinaryFile(leased_output) == leased_bytes,
        "aliased lease rejection must preserve the protected NPY generation");
    physical_lease.lease.Reset();

    const specforge::SampleLabelingOperationResult after_release =
        controller.ExportActiveLabelValuesToNpy(
            leased_output);
    Require(
        after_release.accepted &&
            after_release.export_attempted &&
            after_release.exported,
        "an unmanaged export target should become writable after its transient external lease is released");
}

void TestOutputArtifactOwnershipIsFormatAware()
{
    const std::filesystem::path directory =
        FreshTestDirectory("specforge_npy_owner_asdf_boundary");
    const std::filesystem::path asdf_path = directory / "labels.asdf";
    const std::filesystem::path npy_path = directory / "labels.npy";
    const std::filesystem::path sidecar_path =
        specforge::SampleAnnotationIoAdapter::MetadataPathForResult(
            asdf_path);
    specforge::SampleLabelingTask task =
        specforge::CreateSampleLabelingTask(
            "quality",
            "Quality",
            3);
    const specforge::SampleAnnotationIoAdapter adapter;

    std::string error;
    Require(
        !adapter.SaveLabelArray(asdf_path, task, &error) &&
            error.find("ASDF label output") != std::string::npos,
        "the NPY array writer should reject an ASDF output path");
    error.clear();
    Require(
        !adapter.SaveLabelMetadata(
            asdf_path,
            task,
            nullptr,
            &error) &&
            error.find("ASDF label output") != std::string::npos,
        "the NPY sidecar writer should reject an ASDF output path");
    const specforge::SampleLabelResultWriteOutcome outcome =
        adapter.SaveLabelResult(asdf_path, task);
    Require(
        !outcome.array_saved && !outcome.metadata_saved &&
            outcome.message.find("ASDF label output") !=
                std::string::npos,
        "the paired NPY persistence boundary should reject ASDF before writing either artifact");
    Require(
        !std::filesystem::exists(asdf_path) &&
            !std::filesystem::exists(sidecar_path),
        "a rejected ASDF output must not create a disguised NPY file or shared sidecar");

    const specforge::SampleAnnotationArtifactIdentitySet
        canonical_asdf_identities =
        specforge::SampleAnnotationArtifactIdentities(
            asdf_path,
            specforge::SampleLabelingOutputArtifactFormat::CanonicalAsdf,
            false);
    const specforge::SampleAnnotationArtifactIdentitySet npy_identities =
        specforge::SampleAnnotationArtifactIdentities(
            npy_path,
            specforge::SampleLabelingOutputArtifactFormat::
                LegacyNpyWithSidecar,
            false);
    std::size_t shared_identity_count = 0;
    for (const std::string& asdf_identity :
         canonical_asdf_identities.stable_path_keys) {
        shared_identity_count += static_cast<std::size_t>(
            std::find(
                npy_identities.stable_path_keys.begin(),
                npy_identities.stable_path_keys.end(),
                asdf_identity) !=
            npy_identities.stable_path_keys.end());
    }
    Require(
        canonical_asdf_identities.stable_path_keys.size() == 1 &&
            npy_identities.stable_path_keys.size() == 2 &&
            shared_identity_count == 0,
        "canonical ASDF should own one file while legacy NPY owns its result and sidecar");

    const specforge::SampleAnnotationArtifactIdentitySet
        rejected_npy_writer_identities =
            specforge::SampleAnnotationArtifactIdentities(
                asdf_path,
                specforge::SampleLabelingOutputArtifactFormat::
                    LegacyNpyWithSidecar,
                false);
    Require(
        rejected_npy_writer_identities.stable_path_keys.size() == 2,
        "the current NPY writer ownership must remain two-file even when its rejected path has an ASDF extension");
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
    const specforge::SampleLabelOutputPublicationResult result =
        specforge::PublishLegacySampleLabelingTaskOutput(task, &source);
    Require(
        result.attempted && result.published &&
            result.artifacts_replaced && !result.retryable,
        result.message.empty()
            ? "metadata-backed output should publish through the generic result seam"
            : result.message);
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
    const specforge::SampleLabelOutputPublicationResult saved =
        specforge::PublishLegacySampleLabelingTaskOutput(task);
    Require(saved.published, saved.message.empty() ? "label result should save" : saved.message);

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

void TestCreateFromAnnotationDefersPhysicalIdentityRefresh()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_annotation_hot_path_lease");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "imported.npy";
    specforge::ExclusiveFileLeaseAcquireResult
        refreshed_identity_blocker;
    specforge::SampleLabelingController controller(
        cache_path,
        [](const std::filesystem::path& path) {
            return specforge::LoadSampleLabelingStateCache(
                path);
        },
        [&](specforge::SampleLabelingTask& task,
            const specforge::SampleLabelResultMetadataSource* source) {
            specforge::SampleLabelOutputPublicationResult result =
                specforge::PublishLegacySampleLabelingTaskOutput(
                    task,
                    source);
            if (result.artifacts_replaced) {
                // The fixture holds the post-replacement FILE_ID to prove
                // that the hot path relies on the normalized lease; physical
                // alias coverage remains best-effort until a later refresh.
                refreshed_identity_blocker =
                    TryAcquireCurrentArtifactLease(
                        cache_path,
                        output_path);
            }
            return result;
        });
    controller.ActivateSource("shared-source", 3);
    specforge::SampleLabelSet labels;
    labels.labels.push_back({5, "accepted", 'a'});

    const specforge::SampleLabelingOperationResult created =
        controller.CreateTaskFromAnnotation(
            "imported-task",
            "Imported task",
            std::move(labels),
            {5, -1, -1},
            output_path,
            false);
    Require(
        refreshed_identity_blocker.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::
                Acquired,
        "annotation refresh fixture should reserve the post-write physical result identity");
    Require(
        created.accepted &&
            created.output_saved &&
            ActiveTask(controller) != nullptr,
        "output publication should guarantee the normalized lease without probing the new FILE_ID on the UI hot path");

    refreshed_identity_blocker.lease.Reset();
    Require(
        controller.DeactivateActiveTask().changed,
        "the hot-path lease fixture should release its task before handoff");
    specforge::SampleLabelingController next(cache_path);
    next.ActivateSource("shared-source", 3);
    Require(
        next.ActivateTask("imported-task").accepted &&
            ActiveTask(next) != nullptr,
        "a later controller should acquire the task after the stable lease is released");
}

void TestCreateFromAnnotationWaitsForRecoveryCheckpoint()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_annotation_checkpoint_first");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "imported.npy";
    std::size_t persist_calls = 0;
    specforge::SampleLabelingController controller(
        cache_path,
        [](const std::filesystem::path& path) {
            return specforge::LoadSampleLabelingStateCache(
                path);
        },
        [&](specforge::SampleLabelingTask& task,
            const specforge::SampleLabelResultMetadataSource* source) {
            ++persist_calls;
            return specforge::PublishLegacySampleLabelingTaskOutput(
                task,
                source);
        });
    controller.ActivateSource("shared-source", 3);
    specforge::ExclusiveFileLeaseAcquireResult commit_lock =
        specforge::TryAcquireExclusiveFileLease(
            specforge::SampleLabelingStateCoordinationDirectory(
                cache_path) /
            "cache-commit.lock");
    Require(
        commit_lock.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::
                Acquired,
        "annotation checkpoint fixture should block the cache commit");
    specforge::SampleLabelSet labels;
    labels.labels.push_back({5, "accepted", 'a'});

    const specforge::SampleLabelingOperationResult blocked =
        controller.CreateTaskFromAnnotation(
            "imported-task",
            "Imported task",
            labels,
            {5, -1, -1},
            output_path,
            false);
    Require(
        !blocked.accepted &&
            blocked.state_save_attempted &&
            !blocked.state_saved &&
            !blocked.output_save_attempted &&
            persist_calls == 0 &&
            ActiveTask(controller) == nullptr,
        "annotation output must not be written or published before its recovery checkpoint commits");
    const std::optional<specforge::SampleLabelingController::SourceState>
        blocked_state = controller.SourceStateForIdentity(
            "shared-source");
    std::error_code exists_error;
    Require(
        blocked_state && blocked_state->tasks.empty() &&
            !std::filesystem::exists(
                output_path,
                exists_error) &&
            !exists_error,
        "failed annotation checkpoint should leave both controller state and output untouched");

    commit_lock.lease.Reset();
    const specforge::SampleLabelingOperationResult retried =
        controller.CreateTaskFromAnnotation(
            "imported-task",
            "Imported task",
            std::move(labels),
            {5, -1, -1},
            output_path,
            false);
    Require(
        retried.accepted && retried.output_saved &&
            ActiveTask(controller) != nullptr &&
            persist_calls == 1,
        "annotation creation should proceed after its recovery checkpoint becomes durable");
}

void TestCreateFromAnnotationRefreshesLeaseAfterPartialWrite()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_annotation_partial_write");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "partial.npy";
    const std::filesystem::path metadata_path =
        specforge::SampleAnnotationIoAdapter::
            MetadataPathForResult(output_path);
    std::error_code directory_error;
    std::filesystem::create_directory(
        metadata_path,
        directory_error);
    Require(
        !directory_error,
        "partial-write fixture should block the metadata sidecar with a directory");

    specforge::SampleLabelingController controller(
        cache_path);
    controller.ActivateSource("shared-source", 3);
    specforge::SampleLabelSet labels;
    labels.labels.push_back({5, "accepted", 'a'});
    const specforge::SampleLabelingOperationResult created =
        controller.CreateTaskFromAnnotation(
            "partial-task",
            "Partial task",
            std::move(labels),
            {5, -1, -1},
            output_path,
            false);
    Require(
        created.accepted &&
            created.output_save_attempted &&
            !created.output_saved &&
            ActiveTask(controller) != nullptr,
        "array success with metadata failure should retain an active retryable task when its refreshed lease is valid");
    specforge::ExclusiveFileLeaseAcquireResult duplicate =
        TryAcquireCurrentStableArtifactLease(
            cache_path,
            output_path);
    Require(
        duplicate.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::
                Unavailable,
        "partial output replacement must retain the normalized result-path lease while physical alias coverage remains best-effort");
}

void TestControllerKeepsOneTemporaryTaskPerSource()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_single_temporary_state.json";
    const std::filesystem::path output_path =
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_formalized_result.asdf";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    std::filesystem::remove(output_path, cleanup_error);
    std::filesystem::remove(
        specforge::SampleAnnotationIoAdapter::MetadataPathForResult(output_path),
        cleanup_error);

    specforge::SampleLabelingController controller(cache_path);
    ActivateCanonicalTestSource(
        controller,
        "source-identity",
        3);
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
    const specforge::SampleLabelingTask* formalized =
        ActiveTask(controller);
    Require(
        formalized != nullptr &&
            formalized->output_format ==
                specforge::SampleLabelingOutputArtifactFormat::
                    CanonicalAsdf &&
            formalized->output_path ==
                std::optional<std::filesystem::path>{
                    output_path} &&
            controller.ActiveCanonicalAsdfAnnotationProjection()
                .has_value(),
        "temporary formalization should adopt the opened canonical ASDF generation");
    const specforge::SampleLabelingAsdfReadResult document =
        specforge::ReadSampleLabelingAsdfDocument(
            output_path);
    Require(
        document.succeeded() &&
            document.document->labeling.id ==
                temporary_task_id &&
            document.document->annotation.values ==
                std::vector<std::int32_t>({-1, 5, -1}),
        "temporary formalization should publish task metadata and values as one canonical generation");
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
                    PublishLegacySampleLabelingTaskOutput(
                        task,
                        source);
            }
            specforge::MarkSampleLabelTaskSaveFailed(
                task,
                "disk full");
            return specforge::SampleLabelOutputPublicationResult{
                .attempted = true,
                .published = false,
                .artifacts_replaced = false,
                .retryable = true,
                .message = "disk full",
            };
        });
    controller.ActivateSource("source-identity", 3);
    Require(
        controller.CreateTaskFromAnnotation(
                      "formal",
                      "Formal",
                      {},
                      {-1, -1, -1},
                      output_path,
                      false)
            .output_saved,
        "controller should establish a clean existing legacy formal task");
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
        specforge::SampleLabelSet labels;
        Require(
            specforge::UpsertSampleLabel(
                labels,
                {5, "bad", 'b'}),
            "legacy fixture should define its label metadata");
        const specforge::SampleLabelingOperationResult save =
            controller.CreateTaskFromAnnotation(
                "quality",
                "Quality",
                std::move(labels),
                {-1, 5, -1},
                output_path,
                false);
        Require(save.output_saved && save.state_saved, "controller should persist output-backed task");
        const specforge::SampleLabelingTask* task = ActiveTask(controller);
        Require(
            task != nullptr &&
                task->output_format ==
                    specforge::SampleLabelingOutputArtifactFormat::
                        LegacyNpyWithSidecar,
            "the existing save flow should formalize the task under legacy NPY ownership");
    }

    const std::string cache_text = ReadTextFile(cache_path);
    Require(
        cache_text.find("\"output\": {") != std::string::npos &&
            cache_text.find("\"format\": \"legacy_npy_with_sidecar\"") !=
                std::string::npos,
        "task record should keep the formal output path and format");
    Require(cache_text.find("\"values\"") == std::string::npos, "output-backed task record must not duplicate label values");

    {
        specforge::SampleLabelingController restored(cache_path);
        restored.ActivateSource("source-identity", 3);
        const specforge::SampleLabelingTask* task = ActiveTask(restored);
        Require(task != nullptr, "output-backed task should restore");
        Require(
            task->output_path && *task->output_path == output_path &&
                task->output_format ==
                    specforge::SampleLabelingOutputArtifactFormat::
                        LegacyNpyWithSidecar,
            "legacy output path and format should restore");
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
    specforge::SampleLabelSet initial_labels;
    Require(
        specforge::UpsertSampleLabel(
            initial_labels,
            {5, "bad", 'b'}),
        "legacy metadata fixture should define its initial label");
    const specforge::SampleLabelingOperationResult initial_save =
        controller.CreateTaskFromAnnotation(
            "quality",
            "Quality",
            std::move(initial_labels),
            {-1, 5, -1},
            output_path,
            false);
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
        std::filesystem::temp_directory_path() / "specforge_sample_labeling_output_conflict.asdf";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    std::filesystem::remove(output_path, cleanup_error);
    std::filesystem::remove(
        specforge::SampleAnnotationIoAdapter::MetadataPathForResult(output_path),
        cleanup_error);

    {
        specforge::SampleLabelingController controller(cache_path);
        ActivateCanonicalTestSource(
            controller,
            "source-identity",
            3);
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
    ActivateCanonicalTestSource(
        restored,
        "source-identity",
        3);
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
        specforge::SampleLabelSet labels;
        Require(
            specforge::UpsertSampleLabel(
                labels,
                {5, "bad", 'b'}),
            "missing-output fixture should define its legacy metadata");
        Require(
            controller.CreateTaskFromAnnotation(
                          "quality",
                          "Quality",
                          std::move(labels),
                          {-1, 5, -1},
                          output_path,
                          false)
                .output_saved,
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
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_sample_labeling_draft_output_failure");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "blocked.asdf";
    std::filesystem::create_directories(output_path);
    std::error_code cleanup_error;

    {
        specforge::SampleLabelingController controller(cache_path);
        ActivateCanonicalTestSource(
            controller,
            "source-identity",
            3);
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
    Require(
        cache_text.find("\"path\": null") != std::string::npos &&
            cache_text.find("\"format\": \"none\"") != std::string::npos,
        "failed first save should not bind a formal output owner");
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

void TestPendingCreateCannotReplaceRepairedSameIdTask()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_pending_create_repair");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "repaired_y.npy";
    WriteTextFile(cache_path, "{ corrupt cache\n");

    specforge::SampleLabelingController stale(cache_path);
    stale.ActivateSource("shared-source", 3);
    const specforge::SampleLabelingOperationResult created =
        stale.CreateTask("shared-task", "Pending draft");
    Require(
        created.accepted && !created.state_saved,
        "an untrusted cache should leave the new draft as a pending create");

    specforge::SampleLabelingTask repaired =
        specforge::CreateSampleLabelingTask(
            "shared-task",
            "Repaired formal task",
            3);
    repaired.label_set.labels.push_back(
        specforge::SampleLabelDefinition{
            7,
            "repaired",
            'r'});
    repaired.values = {7, -1, 7};
    repaired.output_path = output_path;
    repaired.output_format =
        specforge::SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
    const specforge::SampleLabelOutputPublicationResult persisted =
        specforge::PublishLegacySampleLabelingTaskOutput(
            repaired,
            nullptr);
    Require(
        persisted.published,
        "repair fixture should persist its formal artifact set");
    specforge::SampleLabelingStateCache repaired_cache;
    specforge::SampleLabelingSourceState repaired_source;
    repaired_source.sample_count = 3;
    repaired_source.tasks.push_back(repaired);
    repaired_cache.sources.emplace(
        "shared-source",
        std::move(repaired_source));
    Require(
        specforge::SaveSampleLabelingStateCache(
            cache_path,
            repaired_cache),
        "repair fixture should replace the corrupt cache");
    const std::string repaired_cache_bytes =
        ReadTextFile(cache_path);

    Require(
        !stale.FlushStateCache(),
        "a pending create must fail when repair introduced the same task id");
    Require(
        ReadTextFile(cache_path) == repaired_cache_bytes,
        "a rejected pending create must preserve the repaired cache bytes");
    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(cache_path);
    const specforge::SampleLabelingTask* task =
        FindTask(
            loaded.cache,
            "shared-source",
            "shared-task");
    Require(
        task != nullptr &&
            task->task_name == "Repaired formal task" &&
            task->output_path ==
                std::optional<std::filesystem::path>{
                    output_path} &&
            task->values == std::vector<int>({7, -1, 7}),
        "retry must not replace the repaired formal task with the stale draft");

    const specforge::SampleLabelingOperationResult deleted =
        stale.DeleteActiveTask();
    Require(
        deleted.accepted && deleted.state_saved,
        "deleting a stale expected-absent draft should cancel only its local pending create");
    const specforge::SampleLabelingStateCacheLoadResult after_delete =
        specforge::LoadSampleLabelingStateCache(cache_path);
    task = FindTask(
        after_delete.cache,
        "shared-source",
        "shared-task");
    Require(
        task != nullptr &&
            task->task_name == "Repaired formal task" &&
            task->output_path ==
                std::optional<std::filesystem::path>{
                    output_path} &&
            task->values == std::vector<int>({7, -1, 7}),
        "deleting the stale draft must not tombstone the repaired task with the same id");
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
                    return specforge::PublishLegacySampleLabelingTaskOutput(task, source);
                }
                specforge::MarkSampleLabelTaskSaveFailed(task, "disk full");
                return specforge::SampleLabelOutputPublicationResult{
                    .attempted = true,
                    .published = false,
                    .artifacts_replaced = false,
                    .retryable = true,
                    .message = "disk full"};
            });
        controller.ActivateSource("source-identity", 3);
        specforge::SampleLabelSet labels;
        Require(
            specforge::UpsertSampleLabel(
                labels,
                {5, "bad", 'b'}),
            "failed-output fixture should define its legacy metadata");
        Require(
            controller.CreateTaskFromAnnotation(
                          "quality",
                          "Quality",
                          std::move(labels),
                          {-1, -1, -1},
                          output_path,
                          false)
                .output_saved,
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
    Require(
        cache_text.find("\"output\": {") != std::string::npos &&
            cache_text.find("\"format\": \"legacy_npy_with_sidecar\"") !=
                std::string::npos,
        "task record should keep the formal output path and format");
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
                return specforge::PublishLegacySampleLabelingTaskOutput(task, source);
            }
            specforge::MarkSampleLabelTaskSaveFailed(task, "disk full");
            return specforge::SampleLabelOutputPublicationResult{
                .attempted = true,
                .published = false,
                .artifacts_replaced = false,
                .retryable = true,
                .message = "disk full"};
        });
    controller.ActivateSource("source-identity", 3);
    specforge::SampleLabelSet labels;
    Require(
        specforge::UpsertSampleLabel(
            labels,
            {5, "bad", 'b'}),
        "retry fixture should define its legacy metadata");
    Require(
        controller.CreateTaskFromAnnotation(
                      "quality",
                      "Quality",
                      std::move(labels),
                      {-1, -1, -1},
                      output_path,
                      false)
            .output_saved,
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

void TestOutputWriteWaitsForPendingOverlayCommit()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_pending_before_output");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "labels.npy";
    specforge::SampleLabelingController controller(cache_path);
    controller.ActivateSource("shared-source", 3);
    specforge::SampleLabelSet labels;
    Require(
        specforge::UpsertSampleLabel(
            labels,
            {5, "accepted", 'a'}) &&
            controller.CreateTaskFromAnnotation(
                          "shared-task",
                          "Shared task",
                          std::move(labels),
                          {-1, -1, -1},
                          output_path,
                          false)
                .output_saved,
        "pending-before-output fixture should establish a clean formal task");

    specforge::ExclusiveFileLeaseAcquireResult commit_lock =
        specforge::TryAcquireExclusiveFileLease(
            specforge::SampleLabelingStateCoordinationDirectory(
                cache_path) /
            "cache-commit.lock");
    Require(
        commit_lock.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::
                Acquired,
        "pending-before-output fixture should block the cache commit");
    const specforge::SampleLabelingWriteOperationResult write =
        controller.AssignLabel(0, 5);
    Require(
        write.write.changed &&
            !write.operation.output_save_attempted &&
            !write.operation.state_saved,
        "a failed pending-overlay commit must prevent the external output write");
    Require(
        ReadTestInt32NpyPayload(output_path)[0] == -1,
        "external output must remain unchanged until its newest pending overlay is durable");

    commit_lock.lease.Reset();
    RunMaintenanceUntilIdle(controller);
    Require(
        ReadTestInt32NpyPayload(output_path)[0] == 5,
        "maintenance should write the label after the pending overlay commit succeeds");
}

void TestInteractiveOutputLabelWritesDoNotWaitForCacheCommitLock()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_interactive_commit_contention");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "labels.npy";
    specforge::SampleLabelingController controller(cache_path);
    CreateFormalTask(
        controller,
        "shared-source",
        "shared-task",
        output_path,
        5);
    Require(
        controller.ActivateTask("shared-task").accepted,
        "interactive contention fixture should activate its formal task");

    specforge::ExclusiveFileLeaseAcquireResult commit_lock =
        specforge::TryAcquireExclusiveFileLease(
            specforge::SampleLabelingStateCoordinationDirectory(
                cache_path) /
            "cache-commit.lock");
    Require(
        commit_lock.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::Acquired,
        "interactive contention fixture should hold the cache commit lock");

    const auto assign_start = std::chrono::steady_clock::now();
    const specforge::SampleLabelingWriteOperationResult assigned =
        controller.AssignLabel(0, 5);
    const auto assign_elapsed =
        std::chrono::steady_clock::now() - assign_start;
    Require(
        assigned.write.changed &&
            assigned.operation.accepted &&
            !assigned.operation.state_saved &&
            !assigned.operation.output_save_attempted &&
            assigned.operation.output_retry_scheduled &&
            assign_elapsed < std::chrono::milliseconds(250),
        "interactive assign should retain its pending label without waiting for the cache lock");

    const auto clear_start = std::chrono::steady_clock::now();
    const specforge::SampleLabelingWriteOperationResult cleared =
        controller.ClearLabel(0);
    const auto clear_elapsed =
        std::chrono::steady_clock::now() - clear_start;
    Require(
        cleared.write.changed &&
            cleared.operation.accepted &&
            !cleared.operation.state_saved &&
            !cleared.operation.output_save_attempted &&
            cleared.operation.output_retry_scheduled &&
            clear_elapsed < std::chrono::milliseconds(250),
        "interactive clear should retain its pending tombstone without waiting for the cache lock");

    Require(
        ActiveTask(controller) != nullptr &&
            ActiveTask(controller)->values[0] ==
                specforge::kUnlabeledSampleLabelCode,
        "interactive edits should keep the newest in-memory label while persistence is pending");
    commit_lock.lease.Reset();

    RunMaintenanceUntilIdle(controller);
    Require(
        ReadTestInt32NpyPayload(output_path)[0] ==
            specforge::kUnlabeledSampleLabelCode,
        "maintenance should publish the newest label after the cache checkpoint commits");
    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(cache_path);
    const specforge::SampleLabelingTask* task =
        FindTask(loaded.cache, "shared-source", "shared-task");
    Require(
        task != nullptr &&
            task->pending_sample_indices.empty() &&
            task->save_state.kind ==
                specforge::SampleLabelSaveStateKind::AutosavedToOutput,
        "maintenance should persist the clean output state after retrying the pending overlay");
}

void TestSuccessfulOutputCannotRetainOlderPendingOverlay()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_overlay_generation");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "labels.npy";
    bool fail_output = false;
    bool block_clean_commit = false;
    specforge::ExclusiveFileLeaseAcquireResult
        clean_commit_blocker;
    {
        specforge::SampleLabelingController controller(
            cache_path,
            [](const std::filesystem::path& path) {
                return specforge::LoadSampleLabelingStateCache(
                    path);
            },
            [&](specforge::SampleLabelingTask& task,
                const specforge::SampleLabelResultMetadataSource* source) {
                if (fail_output) {
                    specforge::MarkSampleLabelTaskSaveFailed(
                        task,
                        "simulated array failure");
                    return specforge::SampleLabelOutputPublicationResult{
                        .attempted = true,
                        .published = false,
                        .artifacts_replaced = false,
                        .retryable = true,
                        .message = "simulated array failure"};
                }
                specforge::SampleLabelOutputPublicationResult result =
                    specforge::PublishLegacySampleLabelingTaskOutput(
                        task,
                        source);
                if (block_clean_commit &&
                    result.artifacts_replaced) {
                    clean_commit_blocker =
                        specforge::TryAcquireExclusiveFileLease(
                            specforge::SampleLabelingStateCoordinationDirectory(
                                cache_path) /
                            "cache-commit.lock");
                }
                return result;
            });
        controller.ActivateSource("shared-source", 3);
        specforge::SampleLabelSet labels;
        Require(
            specforge::UpsertSampleLabel(
                labels,
                {5, "old", 'o'}) &&
                specforge::UpsertSampleLabel(
                    labels,
                    {7, "new", 'n'}) &&
                controller.CreateTaskFromAnnotation(
                              "shared-task",
                              "Shared task",
                              std::move(labels),
                              {-1, -1, -1},
                              output_path,
                              false)
                    .output_saved,
            "overlay-generation fixture should establish a clean formal task with both labels");

        fail_output = true;
        const specforge::SampleLabelingWriteOperationResult old_write =
            controller.AssignLabel(0, 5);
        Require(
            old_write.write.changed &&
                !old_write.operation.output_saved &&
                old_write.operation.state_saved,
            "fixture should persist the old failed value as a pending overlay");
        Require(
            ReadTextFile(cache_path).find(
                "\"value\": 5") !=
                std::string::npos,
            "fixture cache should contain the old pending value");

        fail_output = false;
        block_clean_commit = true;
        const specforge::SampleLabelingWriteOperationResult new_write =
            controller.AssignLabel(0, 7);
        Require(
            new_write.write.changed &&
                new_write.operation.output_saved &&
                !new_write.operation.state_saved &&
                clean_commit_blocker.status ==
                    specforge::ExclusiveFileLeaseAcquireStatus::
                        Acquired,
            "fixture should publish the new output while blocking only the post-write clean patch");
        Require(
            ReadTestInt32NpyPayload(output_path)[0] == 7,
            "the second write should publish the new label value");
    }

    clean_commit_blocker.lease.Reset();
    const std::string durable_pending =
        ReadTextFile(cache_path);
    Require(
        durable_pending.find("\"value\": 7") !=
                std::string::npos &&
            durable_pending.find("\"value\": 5") ==
                std::string::npos,
        "a successful output may retain only its own generation's pending overlay");

    specforge::SampleLabelingController restored(cache_path);
    restored.ActivateSource("shared-source", 3);
    Require(
        ActiveTask(restored) != nullptr &&
            ActiveTask(restored)->values[0] == 7,
        "restart must not replay the older failed value over the newer output");
    RunMaintenanceUntilIdle(restored);
    Require(
        ReadTestInt32NpyPayload(output_path)[0] == 7,
        "maintenance must preserve the newest label generation");
}

void TestDifferentFormalTargetsMergeAcrossInstances()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_multi_instance_formal");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path first_output =
        directory / "first_y.asdf";
    const std::filesystem::path second_output =
        directory / "second_y.asdf";

    specforge::SampleLabelingController first(cache_path);
    specforge::SampleLabelingController second(cache_path);
    ActivateCanonicalTestSource(
        first,
        "source-first",
        3);
    ActivateCanonicalTestSource(
        second,
        "source-second",
        3);

    Require(
        first.CreateTask("first-task", "First").accepted,
        "first instance should create its task");
    Require(
        first.UpsertActiveLabel(
                 specforge::SampleLabelDefinition{5, "first", 'f'})
            .changed,
        "first instance should add its label");
    Require(
        first.AssignLabel(0, 5).write.changed,
        "first instance should edit its draft");
    Require(
        first.SaveActiveTemporaryTaskToOutput(
                 first_output,
                 "First")
            .state_saved,
        "first formal target should commit its task record");

    Require(
        second.CreateTask("second-task", "Second").accepted,
        "second instance should create its task from a stale snapshot");
    Require(
        second.UpsertActiveLabel(
                  specforge::SampleLabelDefinition{7, "second", 's'})
            .changed,
        "second instance should add its label");
    Require(
        second.AssignLabel(1, 7).write.changed,
        "second instance should edit its draft");
    Require(
        second.SaveActiveTemporaryTaskToOutput(
                  second_output,
                  "Second")
            .state_saved,
        "second formal target should merge its task record");

    Require(
        ReadTestCanonicalAsdfValues(first_output)[0] == 5,
        "first instance output should remain intact");
    Require(
        ReadTestCanonicalAsdfValues(second_output)[1] == 7,
        "second instance output should remain intact");
    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(cache_path);
    Require(loaded.warning.empty(), loaded.warning);
    Require(
        FindTask(
            loaded.cache,
            "source-first",
            "first-task") != nullptr,
        "merged cache should retain the first formal task");
    Require(
        FindTask(
            loaded.cache,
            "source-second",
            "second-task") != nullptr,
        "merged cache should retain the second formal task");
}

void TestDifferentTemporaryTasksMergeAcrossInstances()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_multi_instance_temporary");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";

    specforge::SampleLabelingController first(cache_path);
    specforge::SampleLabelingController second(cache_path);
    first.ActivateSource("source-first", 3);
    second.ActivateSource("source-second", 3);
    Require(
        first.CreateTask("first-draft", "First draft").accepted,
        "first instance should create its temporary task");
    Require(
        second.CreateTask("second-draft", "Second draft").accepted,
        "second instance should create its temporary task");
    Require(
        first.UpsertActiveLabel(
                 specforge::SampleLabelDefinition{5, "first", 'f'})
            .changed &&
            first.AssignLabel(0, 5).write.changed,
        "first temporary task should be editable");
    Require(
        second.UpsertActiveLabel(
                  specforge::SampleLabelDefinition{7, "second", 's'})
            .changed &&
            second.AssignLabel(1, 7).write.changed,
        "second temporary task should be editable");
    Require(first.FlushStateCache(), "first draft should flush");
    Require(second.FlushStateCache(), "second draft should merge");

    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(cache_path);
    const specforge::SampleLabelingTask* first_task =
        FindTask(
            loaded.cache,
            "source-first",
            "first-draft");
    const specforge::SampleLabelingTask* second_task =
        FindTask(
            loaded.cache,
            "source-second",
            "second-draft");
    Require(
        first_task != nullptr && first_task->values[0] == 5,
        "merged cache should retain the first draft");
    Require(
        second_task != nullptr && second_task->values[1] == 7,
        "merged cache should retain the second draft");
}

void TestStaleNewTaskDoesNotActivateLatestFormalTask()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_stale_new_after_formalize");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "formalized.asdf";

    specforge::SampleLabelingController first(cache_path);
    ActivateCanonicalTestSource(
        first,
        "shared-source",
        3);

    Require(
        first.StartOrResumeTemporaryTask().accepted,
        "first instance should create the default draft");
    const std::string formal_task_id =
        ActiveTask(first)->task_id;
    specforge::SampleLabelingController stale(cache_path);
    ActivateCanonicalTestSource(
        stale,
        "shared-source",
        3);
    Require(
        TemporaryTask(stale) != nullptr &&
            TemporaryTask(stale)->task_id == formal_task_id,
        "stale instance should load the draft before another instance formalizes it");
    Require(
        first.SaveActiveTemporaryTaskToOutput(
                 output_path,
                 "Formalized")
            .output_saved,
        "first instance should formalize its default draft");
    Require(
        first.DeactivateActiveTask().state_saved,
        "first instance should release the formal task before handoff");

    const specforge::SampleLabelingOperationResult created =
        stale.StartOrResumeTemporaryTask();
    Require(
        created.accepted && created.state_saved,
        "stale New labeling task should create and persist a fresh draft");
    Require(
        ActiveTask(stale) != nullptr &&
            !ActiveTask(stale)->output_path &&
            ActiveTask(stale)->task_id ==
                formal_task_id + "-2",
        "stale New labeling task must not activate the latest formal task with the requested id");

    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(
            cache_path,
            {},
            specforge::SampleLabelingStateCacheLoadPolicy::
                AllowPersistentOutputsWithoutResultHydration);
    const auto source =
        loaded.cache.sources.find("shared-source");
    Require(
        loaded.issue_kind ==
                specforge::SampleLabelingStateCacheLoadIssueKind::
                    None &&
            source != loaded.cache.sources.end() &&
            source->second.tasks.size() == 2 &&
            std::count_if(
                source->second.tasks.begin(),
                source->second.tasks.end(),
                [](const specforge::SampleLabelingTask& task) {
                    return !task.output_path;
                }) == 1,
        "latest cache should retain the formal task and one newly-created draft");
}

void TestStaleExplicitCreateDoesNotActivateFormalizedDraft()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_stale_create_after_formalize");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";

    specforge::SampleLabelingController first(cache_path);
    ActivateCanonicalTestSource(
        first,
        "shared-source",
        3);
    Require(
        first.CreateTask("shared-draft", "Original draft")
            .accepted,
        "first instance should create the named draft");
    specforge::SampleLabelingController stale(cache_path);
    ActivateCanonicalTestSource(
        stale,
        "shared-source",
        3);
    Require(
        TemporaryTask(stale) != nullptr &&
            TemporaryTask(stale)->task_id ==
                "shared-draft",
        "stale explicit-create instance should load the original draft before formalization");

    Require(
        first.SaveActiveTemporaryTaskToOutput(
                 directory / "formalized.asdf",
                 "Formalized")
            .output_saved &&
            first.DeactivateActiveTask().state_saved,
        "first instance should formalize and release the named draft");

    const specforge::SampleLabelingOperationResult created =
        stale.CreateTask(
            "shared-draft",
            "Fresh draft");
    Require(
        created.accepted && created.state_saved &&
            ActiveTask(stale) != nullptr &&
            !ActiveTask(stale)->output_path &&
            ActiveTask(stale)->task_id ==
                "shared-draft-2" &&
            ActiveTask(stale)->task_name ==
                "Fresh draft",
        "explicit CreateTask should create a new uniquely-named draft instead of activating the refreshed formal task");

    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(
            cache_path,
            {},
            specforge::SampleLabelingStateCacheLoadPolicy::
                AllowPersistentOutputsWithoutResultHydration);
    const auto source =
        loaded.cache.sources.find("shared-source");
    Require(
        loaded.issue_kind ==
                specforge::SampleLabelingStateCacheLoadIssueKind::
                    None &&
            source != loaded.cache.sources.end() &&
            source->second.tasks.size() == 2 &&
            std::count_if(
                source->second.tasks.begin(),
                source->second.tasks.end(),
                [](const specforge::SampleLabelingTask& task) {
                    return !task.output_path;
                }) == 1,
        "explicit create should retain the formal task and persist exactly one fresh draft");
}

void TestDuplicateTaskIdsFailClosedBeforeOutputPersistence()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_duplicate_task_ids");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path first_output =
        directory / "first.npy";
    const std::filesystem::path second_output =
        directory / "second.npy";

    specforge::SampleLabelingTask first_task =
        specforge::CreateSampleLabelingTask(
            "duplicate-task",
            "First duplicate",
            3);
    first_task.label_set.labels = {
        {5, "first", 'f'},
        {7, "changed", 'c'}};
    first_task.values = {5, -1, -1};
    first_task.output_path = first_output;
    first_task.output_format =
        specforge::SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
    Require(
        specforge::PublishLegacySampleLabelingTaskOutput(
            first_task,
            nullptr)
            .published,
        "duplicate-id fixture should persist its first output");

    specforge::SampleLabelingTask second_task =
        specforge::CreateSampleLabelingTask(
            "duplicate-task",
            "Second duplicate",
            3);
    second_task.label_set.labels = {
        {5, "first", 'f'},
        {7, "changed", 'c'}};
    second_task.values = {-1, 7, -1};
    second_task.output_path = second_output;
    second_task.output_format =
        specforge::SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
    Require(
        specforge::PublishLegacySampleLabelingTaskOutput(
            second_task,
            nullptr)
            .published,
        "duplicate-id fixture should persist its second output");

    specforge::SampleLabelingStateCache cache;
    specforge::SampleLabelingSourceState source;
    source.sample_count = 3;
    source.tasks = {first_task, second_task};
    cache.sources.emplace(
        "damaged-source",
        std::move(source));
    Require(
        specforge::SaveSampleLabelingStateCache(
            cache_path,
            cache),
        "duplicate-id fixture should write the structurally damaged cache");
    const specforge::SampleLabelingStateCacheLoadResult salvaged =
        specforge::LoadSampleLabelingStateCache(
            cache_path,
            {},
            specforge::SampleLabelingStateCacheLoadPolicy::
                AllowPersistentOutputsWithoutResultHydration);
    Require(
        salvaged.issue_kind ==
                specforge::SampleLabelingStateCacheLoadIssueKind::
                    InvalidDocument &&
            salvaged.cache.sources.at("damaged-source")
                    .tasks.size() == 2,
        "duplicate task ids should be salvaged for read-only inspection but mark the cache untrusted");

    const std::string original_cache =
        ReadTextFile(cache_path);
    const std::vector<std::int32_t> original_first =
        ReadTestInt32NpyPayload(first_output);
    const std::vector<std::int32_t> original_second =
        ReadTestInt32NpyPayload(second_output);
    const std::string original_first_metadata =
        ReadTextFile(
            specforge::SampleAnnotationIoAdapter::
                MetadataPathForResult(first_output));
    const std::string original_second_metadata =
        ReadTextFile(
            specforge::SampleAnnotationIoAdapter::
                MetadataPathForResult(second_output));

    int persist_calls = 0;
    specforge::SampleLabelingController controller(
        cache_path,
        [](const std::filesystem::path& path) {
            return specforge::LoadSampleLabelingStateCache(
                path);
        },
        [&persist_calls](
            specforge::SampleLabelingTask& task,
            const specforge::SampleLabelResultMetadataSource* metadata) {
            ++persist_calls;
            return specforge::PublishLegacySampleLabelingTaskOutput(
                task,
                metadata);
        });
    controller.ActivateSource("damaged-source", 3);
    const specforge::SampleLabelingOperationResult activated =
        controller.ActivateTask("duplicate-task");
    const specforge::SampleLabelingWriteOperationResult write =
        controller.AssignLabel(1, 7);

    Require(
        !controller.state_load_warning().empty() &&
            !activated.accepted &&
            !write.write.accepted &&
            persist_calls == 0,
        "duplicate task ids must remain read-only and reject editing before the persister is called");
    Require(
        ReadTextFile(cache_path) == original_cache &&
            ReadTestInt32NpyPayload(first_output) ==
                original_first &&
            ReadTestInt32NpyPayload(second_output) ==
                original_second &&
            ReadTextFile(
                specforge::SampleAnnotationIoAdapter::
                    MetadataPathForResult(first_output)) ==
                original_first_metadata &&
            ReadTextFile(
                specforge::SampleAnnotationIoAdapter::
                    MetadataPathForResult(second_output)) ==
                original_second_metadata,
        "rejected duplicate-id editing must preserve cache, outputs, and metadata bytes");
}

void TestSameTargetLeaseRejectsSecondInstance()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_multi_instance_lease");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "shared_y.npy";
    {
        specforge::SampleLabelingController seed(cache_path);
        CreateFormalTask(
            seed,
            "shared-source",
            "shared-task",
            output_path,
            5);
    }

    specforge::SampleLabelingController first(cache_path);
    specforge::SampleLabelingController second(cache_path);
    first.ActivateSource("shared-source", 3);
    second.ActivateSource("shared-source", 3);
    Require(
        first.ActivateTask("shared-task").accepted,
        "first instance should acquire the target lease");
    const specforge::SampleLabelingOperationResult rejected =
        second.ActivateTask("shared-task");
    Require(
        !rejected.accepted &&
            rejected.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable,
        "second instance should immediately reject the leased target");
    Require(
        second.View().active_task == nullptr &&
            second.View().active_source_tasks != nullptr &&
            second.View().active_source_tasks->size() == 1,
        "second instance should retain a read-only view of the task");

    Require(
        first.DeactivateActiveTask().changed,
        "first instance should release the target on deactivation");
    Require(
        second.ActivateTask("shared-task").accepted,
        "second instance should acquire the released target");
}

void TestTemporaryDraftRecoveryRevalidatesSourceAndTask()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_temporary_recovery_activation");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    {
        specforge::SampleLabelingController seed(cache_path);
        seed.ActivateSource("shared-source", 3);
        Require(
            seed.CreateTask(
                        "paused-draft",
                        "Paused draft")
                .accepted &&
                seed.DeactivateActiveTask().state_saved,
            "temporary recovery fixture should persist a paused draft");
    }

    specforge::SampleLabelingController stale(cache_path);
    stale.ActivateSource("shared-source", 3);
    const specforge::SampleLabelingOperationResult wrong_source =
        stale.RecoverTemporaryTask(
            "other-source",
            "paused-draft");
    Require(
        !wrong_source.accepted &&
            wrong_source.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditTargetChanged &&
            ActiveTask(stale) == nullptr,
        "temporary recovery must reject a command from another source");

    {
        specforge::SampleLabelingController deleting(cache_path);
        deleting.ActivateSource("shared-source", 3);
        Require(
            deleting.ActivateTask("paused-draft").accepted &&
                deleting.DeleteActiveTask().state_saved,
            "replacement instance should remove the stale recovery target");
    }

    const specforge::SampleLabelingOperationResult missing =
        stale.RecoverTemporaryTask(
            "shared-source",
            "paused-draft");
    Require(
        !missing.accepted &&
            missing.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditTargetChanged &&
            ActiveTask(stale) == nullptr &&
            ActiveSourceTasks(stale) != nullptr &&
            ActiveSourceTasks(stale)->empty(),
        "temporary recovery must revalidate the latest source/task identity before activation");

    stale.ActivateSource("shared-source", 3);
    Require(
        stale.CreateTask(
                    "paused-draft",
                    "Paused draft")
                .accepted &&
            stale.DeactivateActiveTask().state_saved,
        "temporary recovery fixture should recreate a paused draft");
    const specforge::SampleLabelingOperationResult recovered =
        stale.RecoverTemporaryTask(
            "shared-source",
            "paused-draft");
    Require(
        recovered.accepted &&
            recovered.state_saved &&
            ActiveTask(stale) != nullptr &&
            ActiveTask(stale)->task_id == "paused-draft" &&
            !ActiveTask(stale)->output_path,
        "temporary recovery should acquire the normal edit lease and activate the latest draft");

    specforge::SampleLabelingController blocked(cache_path);
    blocked.ActivateSource("shared-source", 3);
    const specforge::SampleLabelingOperationResult lease_conflict =
        blocked.ActivateTask("paused-draft");
    Require(
        !lease_conflict.accepted &&
            lease_conflict.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable,
        "a recovered temporary draft should retain its normal edit lease");
}

void TestTemporaryDraftRecoveryDeletionKeepsLeaseUntilTombstoneCommits()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_temporary_recovery_delete");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    {
        specforge::SampleLabelingController seed(cache_path);
        seed.ActivateSource("shared-source", 3);
        Require(
            seed.CreateTask(
                        "paused-draft",
                        "Paused draft")
                .accepted &&
                seed.DeactivateActiveTask().state_saved,
            "temporary deletion fixture should persist a paused draft");
    }

    specforge::SampleLabelingController deleting(cache_path);
    specforge::SampleLabelingController stale_second(cache_path);
    deleting.ActivateSource("shared-source", 3);
    stale_second.ActivateSource("shared-source", 3);
    const specforge::SampleLabelingRecoveryView recovery =
        deleting.RecoveryView();
    Require(
        recovery.source_identity == "shared-source" &&
            recovery.temporary_drafts.size() == 1 &&
            recovery.temporary_drafts.front().task != nullptr,
        "temporary deletion fixture should expose a borrowed recovery task");
    specforge::ExclusiveFileLeaseAcquireResult blocked_commit =
        specforge::TryAcquireExclusiveFileLease(
            specforge::SampleLabelingStateCoordinationDirectory(
                cache_path) /
            "cache-commit.lock");
    Require(
        blocked_commit.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::Acquired,
        "temporary deletion fixture should hold the cache commit lock");

    const specforge::SampleLabelingOperationResult pending =
        deleting.DeleteTemporaryTask(
            recovery.source_identity,
            recovery.temporary_drafts.front().task->task_id);
    Require(
        pending.accepted &&
            !pending.state_saved &&
            ActiveTask(deleting) == nullptr &&
            ActiveSourceTasks(deleting) != nullptr &&
            ActiveSourceTasks(deleting)->empty(),
        "temporary deletion should remove the local draft while retaining a pending tombstone");

    const specforge::SampleLabelingOperationResult lease_conflict =
        stale_second.ActivateTask("paused-draft");
    Require(
        !lease_conflict.accepted &&
            lease_conflict.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable,
        "a pending temporary tombstone must retain its edit lease");

    blocked_commit.lease.Reset();
    Require(
        deleting.FlushStateCache(),
        "temporary deletion tombstone should commit after the lock is released");
    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(cache_path);
    Require(
        FindTask(
            loaded.cache,
            "shared-source",
            "paused-draft") == nullptr,
        "committed temporary deletion should persist a tombstone");

    stale_second.ActivateSource("shared-source", 3);
    Require(
        ActiveSourceTasks(stale_second) != nullptr &&
            ActiveSourceTasks(stale_second)->empty() &&
            stale_second.StartOrResumeTemporaryTask().accepted,
        "a released temporary deletion lease should allow the draft slot to be recreated");
}

void TestTemporaryDraftRecoveryCancelsPendingCreateBeforeFlush()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_temporary_recovery_pending_create_delete");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    specforge::SampleLabelingController controller(cache_path);
    controller.ActivateSource("shared-source", 3);

    specforge::ExclusiveFileLeaseAcquireResult blocked_commit =
        specforge::TryAcquireExclusiveFileLease(
            specforge::SampleLabelingStateCoordinationDirectory(
                cache_path) /
            "cache-commit.lock");
    Require(
        blocked_commit.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::Acquired,
        "pending-create deletion fixture should hold the cache commit lock");
    const specforge::SampleLabelingOperationResult created =
        controller.CreateTask(
            "pending-draft",
            "Pending draft");
    Require(
        created.accepted &&
            !created.state_saved &&
            ActiveTask(controller) != nullptr,
        "pending-create fixture should retain a locally created draft after a failed save");
    const specforge::SampleLabelingOperationResult deactivated =
        controller.DeactivateActiveTask();
    Require(
        deactivated.accepted &&
            !deactivated.state_saved &&
            ActiveTask(controller) == nullptr,
        "pending-create fixture should retain its deferred lease after deactivation fails");

    const specforge::SampleLabelingOperationResult deleted =
        controller.DeleteTemporaryTask(
            "shared-source",
            "pending-draft");
    Require(
        !deleted.accepted &&
            deleted.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditTargetChanged &&
            ActiveSourceTasks(controller) != nullptr &&
            ActiveSourceTasks(controller)->empty(),
        "deleting a not-yet-durable draft should remove only its local projection");

    blocked_commit.lease.Reset();
    Require(
        controller.FlushStateCache(),
        "pending-create deletion should flush after the cache lock is released");
    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(cache_path);
    Require(
        FindTask(
            loaded.cache,
            "shared-source",
            "pending-draft") == nullptr,
        "deleting a failed create must not resurrect its expected-absent upsert");
}

void TestTemporaryDraftRecoveryUsesPendingCreateBeforeFlush()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_temporary_recovery_pending_create");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    specforge::SampleLabelingController controller(cache_path);
    controller.ActivateSource("shared-source", 3);

    specforge::ExclusiveFileLeaseAcquireResult blocked_commit =
        specforge::TryAcquireExclusiveFileLease(
            specforge::SampleLabelingStateCoordinationDirectory(
                cache_path) /
            "cache-commit.lock");
    Require(
        blocked_commit.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::Acquired,
        "pending-create recovery fixture should hold the cache commit lock");

    const specforge::SampleLabelingOperationResult created =
        controller.CreateTask(
            "pending-draft",
            "Pending draft");
    Require(
        created.accepted &&
            created.state_save_attempted &&
            !created.state_saved &&
            ActiveTask(controller) != nullptr,
        "pending-create recovery fixture should retain a locally created draft after a failed save");
    Require(
        controller.state_save_failed() &&
            controller.state_save_pending(),
        "pending-create recovery fixture should report the failed initial save as pending");
    Require(
        controller.DeactivateActiveTask().accepted &&
            controller.state_save_failed() &&
            controller.state_save_pending() &&
            ActiveTask(controller) == nullptr,
        "pending-create recovery fixture should pause the draft locally");

    const specforge::ExclusiveFileLeaseAcquireResult task_lease_before_recover =
        specforge::TryAcquireExclusiveFileLease(
            LabelingTargetLeasePath(
                cache_path,
                "task\nshared-source\npending-draft"));
    Require(
        task_lease_before_recover.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::Unavailable,
        "pause should retain the pending task identity lease before recovery");
    const specforge::ExclusiveFileLeaseAcquireResult temporary_slot_lease_before_recover =
        specforge::TryAcquireExclusiveFileLease(
            LabelingTargetLeasePath(
                cache_path,
                "temporary-slot\nshared-source"));
    Require(
        temporary_slot_lease_before_recover.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::Unavailable,
        "pause should retain the pending temporary-slot lease before recovery");

    const specforge::SampleLabelingOperationResult recovered =
        controller.RecoverTemporaryTask(
            "shared-source",
            "pending-draft");
    Require(
        recovered.accepted &&
            !recovered.state_saved &&
            ActiveTask(controller) != nullptr &&
            ActiveTask(controller)->task_id == "pending-draft" &&
            ActiveTask(controller)->values.size() == 3,
        "Recover should restore a pending-create draft before its first cache flush");

    specforge::ExclusiveFileLeaseAcquireResult task_lease =
        specforge::TryAcquireExclusiveFileLease(
            LabelingTargetLeasePath(
                cache_path,
                "task\nshared-source\npending-draft"));
    Require(
        task_lease.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::Unavailable,
        "Recover should preserve the pending task identity lease");
    specforge::ExclusiveFileLeaseAcquireResult temporary_slot_lease =
        specforge::TryAcquireExclusiveFileLease(
            LabelingTargetLeasePath(
                cache_path,
                "temporary-slot\nshared-source"));
    Require(
        temporary_slot_lease.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::Unavailable,
        "Recover should preserve the pending temporary-slot lease");

    blocked_commit.lease.Reset();
    Require(
        controller.FlushStateCache(),
        "pending-create recovery should flush after the cache lock is released");
    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(cache_path);
    const specforge::SampleLabelingTask* task =
        FindTask(
            loaded.cache,
            "shared-source",
            "pending-draft");
    Require(
        task != nullptr &&
            task->task_name == "Pending draft" &&
            task->values == std::vector<int>({-1, -1, -1}) &&
            ActiveTask(controller) != nullptr &&
            ActiveTask(controller)->task_id == "pending-draft",
        "Recover should persist the pending-create draft without a restart");

    Require(
        controller.DeactivateActiveTask().state_saved,
        "a recovered pending-create draft should release leases after selection persistence");
    task_lease = specforge::TryAcquireExclusiveFileLease(
        LabelingTargetLeasePath(
            cache_path,
            "task\nshared-source\npending-draft"));
    Require(
        task_lease.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::Acquired,
        "the task identity lease should release after the recovered draft is paused");
    task_lease.lease.Reset();
    temporary_slot_lease = specforge::TryAcquireExclusiveFileLease(
        LabelingTargetLeasePath(
            cache_path,
            "temporary-slot\nshared-source"));
    Require(
        temporary_slot_lease.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::Acquired,
        "the temporary-slot lease should release after the recovered draft is paused");
}

void TestTemporaryDraftRecoveryMissingTargetDoesNotTombstoneRecreatedTask()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_temporary_recovery_missing_delete");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    {
        specforge::SampleLabelingController seed(cache_path);
        seed.ActivateSource("shared-source", 3);
        Require(
            seed.CreateTask(
                        "missing-draft",
                        "Missing draft")
                .accepted &&
                seed.DeactivateActiveTask().state_saved,
            "missing-delete fixture should persist the initial draft");
    }

    specforge::SampleLabelingController stale(cache_path);
    stale.ActivateSource("shared-source", 3);
    {
        specforge::SampleLabelingController deleting(cache_path);
        deleting.ActivateSource("shared-source", 3);
        Require(
            deleting.ActivateTask("missing-draft").accepted &&
                deleting.DeleteActiveTask().state_saved,
            "missing-delete fixture should remove the original draft");
    }
    const specforge::SampleLabelingOperationResult missing =
        stale.DeleteTemporaryTask(
            "shared-source",
            "missing-draft");
    Require(
        !missing.accepted &&
            missing.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditTargetChanged,
        "deleting a stale-missing target should only converge the local projection");

    {
        specforge::SampleLabelingController recreated(cache_path);
        recreated.ActivateSource("shared-source", 3);
        Require(
            recreated.CreateTask(
                            "missing-draft",
                            "Recreated draft")
                    .accepted &&
                recreated.DeactivateActiveTask().state_saved,
            "missing-delete fixture should recreate the same task ID");
    }

    const specforge::SourceCollectionIdentity identity{
        .id = "shared-source",
        .spectrum_count = 3};
    stale.ActivateSource(identity);
    Require(
        stale.FlushStateCache(),
        "ordinary source metadata refresh should flush any pending projection work");
    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(cache_path);
    const specforge::SampleLabelingTask* task =
        FindTask(
            loaded.cache,
            "shared-source",
            "missing-draft");
    Require(
        task != nullptr &&
            task->task_name == "Recreated draft",
        "a stale-missing delete must not tombstone a same-ID task recreated later");
}

void TestTemporaryDraftRecoveryRetainsDeferredPendingEdit()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_temporary_recovery_pending_edit");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    {
        specforge::SampleLabelingController seed(cache_path);
        seed.ActivateSource("shared-source", 3);
        Require(
            seed.CreateTask(
                        "paused-draft",
                        "Paused draft")
                .accepted &&
                seed.UpsertActiveLabel(
                           specforge::SampleLabelDefinition{
                               5,
                               "accepted",
                               'a'})
                    .changed &&
                seed.FlushStateCache() &&
                seed.DeactivateActiveTask().state_saved,
            "pending-edit fixture should persist a clean paused draft");
    }

    specforge::SampleLabelingController editor(cache_path);
    editor.ActivateSource("shared-source", 3);
    Require(
        editor.ActivateTask("paused-draft").accepted,
        "pending-edit fixture should acquire the draft lease");
    specforge::ExclusiveFileLeaseAcquireResult blocked_commit =
        specforge::TryAcquireExclusiveFileLease(
            specforge::SampleLabelingStateCoordinationDirectory(
                cache_path) /
            "cache-commit.lock");
    Require(
        blocked_commit.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::Acquired,
        "pending-edit fixture should hold the cache commit lock");
    Require(
        editor.AssignLabel(0, 5).write.changed &&
            ActiveTask(editor) != nullptr &&
            ActiveTask(editor)->values[0] == 5,
        "pending-edit fixture should create a newer local edit");
    Require(
        editor.DeactivateActiveTask().accepted &&
            editor.state_save_failed() &&
            editor.state_save_pending() &&
            ActiveTask(editor) == nullptr,
        "pending-edit fixture should retain the newer edit after deactivation save fails");

    const specforge::SampleLabelingOperationResult recovered =
        editor.RecoverTemporaryTask(
            "shared-source",
            "paused-draft");
    Require(
        recovered.accepted &&
            !recovered.state_saved &&
            ActiveTask(editor) != nullptr &&
            ActiveTask(editor)->values[0] == 5,
        "recovering a deferred temporary draft must retain its pending local edit");
    Require(
        editor.AssignLabel(1, 5).write.changed &&
            ActiveTask(editor) != nullptr &&
            ActiveTask(editor)->values[0] == 5 &&
            ActiveTask(editor)->values[1] == 5,
        "editing after deferred temporary recovery must build on the retained pending value");

    blocked_commit.lease.Reset();
    Require(
        editor.FlushStateCache(),
        "pending temporary edit should flush after the cache lock is released");
    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(cache_path);
    const specforge::SampleLabelingTask* task =
        FindTask(
            loaded.cache,
            "shared-source",
            "paused-draft");
    Require(
        task != nullptr &&
            task->values.size() == 3 &&
            task->values[0] == 5 &&
            task->values[1] == 5,
        "the recovered pending temporary edit must remain durable");
}

void TestTemporaryDraftRecoveryDeleteConvergesFormalizedProjection()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_temporary_recovery_formalized_delete");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "formalized.asdf";
    {
        specforge::SampleLabelingController seed(cache_path);
        seed.ActivateSource("shared-source", 3);
        Require(
            seed.CreateTask(
                        "formalized-draft",
                        "Draft")
                .accepted &&
                seed.DeactivateActiveTask().state_saved,
            "formalized-delete fixture should persist a temporary draft");
    }

    specforge::SampleLabelingController stale(cache_path);
    ActivateCanonicalTestSource(
        stale,
        "shared-source",
        3);
    specforge::SampleLabelingController formalizer(cache_path);
    ActivateCanonicalTestSource(
        formalizer,
        "shared-source",
        3);
    Require(
        formalizer.ActivateTask("formalized-draft").accepted &&
            formalizer
                .SaveActiveTemporaryTaskToOutput(
                    output_path,
                    "Formalized")
                .output_saved &&
            formalizer.DeactivateActiveTask().state_saved,
        "formalized-delete fixture should convert and release the latest task");

    const specforge::SampleLabelingRecoveryView before =
        stale.RecoveryView();
    Require(
        before.temporary_drafts.size() == 1 &&
            before.temporary_drafts.front().task != nullptr,
        "formalized-delete fixture should start with a stale temporary projection");
    const specforge::SampleLabelingOperationResult deleted =
        stale.DeleteTemporaryTask(
            before.source_identity,
            before.temporary_drafts.front().task->task_id);
    Require(
        !deleted.accepted &&
            deleted.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditTargetChanged &&
            ActiveSourceTasks(stale) != nullptr &&
            ActiveSourceTasks(stale)->size() == 1 &&
            ActiveSourceTasks(stale)->front().output_path &&
            stale.RecoveryView().temporary_drafts.empty(),
        "deleting a task formalized elsewhere should converge the local projection instead of retaining a temporary ghost");
}

void TestTemporaryRecoveryRejectsAlreadyActiveFormalTask()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_temporary_recovery_active_formal");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "formalized.asdf";
    specforge::SampleLabelingController controller(cache_path);
    ActivateCanonicalTestSource(
        controller,
        "shared-source",
        3);
    Require(
        controller.CreateTask(
                        "draft",
                        "Draft")
                .accepted,
        "active-formal recovery fixture should create a draft");
    const specforge::SampleLabelingRecoveryView before =
        controller.RecoveryView();
    Require(
        before.temporary_drafts.size() == 1 &&
            before.temporary_drafts.front().task != nullptr,
        "active-formal recovery fixture should expose the draft");
    const std::string stale_task_id =
        before.temporary_drafts.front().task->task_id;
    Require(
        controller.SaveActiveTemporaryTaskToOutput(
                         output_path,
                         "Formalized")
            .output_saved &&
            ActiveTask(controller) != nullptr &&
            ActiveTask(controller)->output_path,
        "active-formal recovery fixture should formalize the draft in place");

    const specforge::SampleLabelingOperationResult recovered =
        controller.RecoverTemporaryTask(
            before.source_identity,
            stale_task_id);
    Require(
        !recovered.accepted &&
            recovered.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditTargetChanged &&
            ActiveTask(controller) != nullptr &&
            ActiveTask(controller)->task_id == stale_task_id &&
            ActiveTask(controller)->output_path,
        "an old temporary recovery command must reject an already-active formal task");
}

void TestTemporaryDraftDeleteIgnoresUnrelatedFailedActiveFormalTask()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_temporary_delete_active_formal_failure");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "formal.npy";
    {
        specforge::SampleLabelingController seed(cache_path);
        CreateFormalTask(
            seed,
            "shared-source",
            "formal-task",
            output_path,
            5);
        seed.ActivateSource("shared-source", 3);
        Require(
            seed.CreateTask(
                        "paused-draft",
                        "Paused draft")
                .accepted &&
                seed.DeactivateActiveTask().state_saved,
            "active-formal deletion fixture should persist a paused draft");
    }

    specforge::SampleLabelingController controller(
        cache_path,
        [](const std::filesystem::path& path) {
            return specforge::LoadSampleLabelingStateCache(path);
        },
        [](specforge::SampleLabelingTask& task,
           const specforge::SampleLabelResultMetadataSource*) {
            specforge::MarkSampleLabelTaskSaveFailed(
                task,
                "simulated output failure");
            return specforge::SampleLabelOutputPublicationResult{
                .attempted = true,
                .published = false,
                .artifacts_replaced = false,
                .retryable = true,
                .message = "simulated output failure"};
        });
    controller.ActivateSource("shared-source", 3);
    Require(
        controller.ActivateTask("formal-task").accepted,
        "active-formal deletion fixture should activate the formal task");
    const specforge::SampleLabelingWriteOperationResult failed_write =
        controller.AssignLabel(0, 5);
    Require(
        failed_write.write.changed &&
            failed_write.operation.output_save_attempted &&
            !failed_write.operation.output_saved &&
            ActiveTask(controller) != nullptr &&
            ActiveTask(controller)->save_state.kind ==
                specforge::SampleLabelSaveStateKind::Failed,
        "active-formal deletion fixture should leave the unrelated active task failed");

    const specforge::SampleLabelingOperationResult deleted =
        controller.DeleteTemporaryTask(
            "shared-source",
            "paused-draft");
    Require(
        deleted.accepted &&
            deleted.state_saved &&
            ActiveTask(controller) != nullptr &&
            ActiveTask(controller)->task_id == "formal-task" &&
            controller.RecoveryView().temporary_drafts.empty(),
        "a failed active formal task must not block deletion of an independent paused draft");
}

void TestLeaseConflictTemporaryReopenPreservesPendingEdit()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_conflict_temporary_reopen");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "shared_y.npy";
    {
        specforge::SampleLabelingController seed(cache_path);
        CreateFormalTask(
            seed,
            "shared-source",
            "shared-task",
            output_path,
            5);
    }

    specforge::SampleLabelingController formal_owner(cache_path);
    specforge::SampleLabelingController fallback(cache_path);
    formal_owner.ActivateSource("shared-source", 3);
    Require(
        formal_owner.ActivateTask("shared-task").accepted,
        "formal owner should acquire the shared task lease");

    fallback.ActivateSource("shared-source", 3);
    const specforge::SampleLabelingOperationResult formal_conflict =
        fallback.ActivateTask("shared-task");
    Require(
        !formal_conflict.accepted &&
            formal_conflict.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable,
        "temporary recovery fixture should first observe the formal lease conflict");

    Require(
        fallback.StartOrResumeTemporaryTask().accepted &&
            ActiveTask(fallback) != nullptr &&
            !ActiveTask(fallback)->output_path,
        "formal lease conflict should allow a same-source temporary task takeover");
    const std::string temporary_task_id =
        ActiveTask(fallback)->task_id;
    Require(
        fallback.UpsertActiveLabel(
                    specforge::SampleLabelDefinition{
                        5,
                        "accepted",
                        'a'})
                .changed &&
            fallback.AssignLabel(0, 5).write.changed &&
            fallback.state_save_pending(),
        "temporary recovery edit should remain pending before reopen");

    fallback.ActivateSource("shared-source", 3);
    Require(
        ActiveTask(fallback) != nullptr &&
            ActiveTask(fallback)->task_id == temporary_task_id &&
            ActiveTask(fallback)->values[0] == 5,
        "reopening the conflicted source must preserve the pending temporary edit and selection");

    Require(
        fallback.UpsertActiveLabel(
                    specforge::SampleLabelDefinition{
                        7,
                        "reviewed",
                        'r'})
                .changed &&
            fallback.AssignLabel(1, 7).write.changed &&
            fallback.FlushStateCache(),
        "temporary recovery edit should remain editable and persist after reopen");

    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(cache_path);
    const auto source = loaded.cache.sources.find("shared-source");
    const specforge::SampleLabelingTask* recovered =
        FindTask(
            loaded.cache,
            "shared-source",
            temporary_task_id);
    Require(
        source != loaded.cache.sources.end() &&
            source->second.active_task_id &&
            *source->second.active_task_id == temporary_task_id &&
            recovered != nullptr &&
            recovered->values ==
                std::vector<int>{5, 7, specforge::kUnlabeledSampleLabelCode},
        "recovered temporary selection and edits should be committed without reverting to the formal snapshot");

    specforge::SampleLabelingController blocked(cache_path);
    blocked.ActivateSource("shared-source", 3);
    const specforge::SampleLabelingOperationResult lease_conflict =
        blocked.ActivateTask(temporary_task_id);
    Require(
        !lease_conflict.accepted &&
            lease_conflict.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable,
        "reopened temporary task should retain its lease against a third editor");
}

void TestFormalLeaseConflictDoesNotPoisonRecoverableDraft()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_formal_conflict_recovery_projection");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "formal.npy";
    {
        specforge::SampleLabelingController seed(cache_path);
        CreateFormalTask(
            seed,
            "shared-source",
            "formal-task",
            output_path,
            5);
        Require(
            seed.CreateTask(
                        "paused-draft",
                        "Paused draft")
                .accepted,
            "formal conflict fixture should create a paused draft");
        Require(
            seed.DeactivateActiveTask().state_saved,
            "formal conflict fixture should persist the paused draft");
    }

    specforge::SampleLabelingController fallback(cache_path);
    fallback.ActivateSource("shared-source", 3);
    specforge::SampleLabelingController formal_owner(cache_path);
    formal_owner.ActivateSource("shared-source", 3);
    Require(
        formal_owner.ActivateTask("formal-task").accepted,
        "formal conflict fixture should acquire the formal task lease");

    const specforge::SampleLabelingRecoveryView before =
        fallback.RecoveryView();
    Require(
        before.temporary_drafts.size() == 1 &&
            before.temporary_drafts[0].status ==
                specforge::SampleLabelingRecoveryDraftStatus::Recoverable,
        "a paused draft should initially be recoverable");
    const specforge::SampleLabelingOperationResult rejected =
        fallback.ActivateTask("formal-task");
    const specforge::SampleLabelingRecoveryView after =
        fallback.RecoveryView();
    Require(
        !rejected.accepted &&
            rejected.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable &&
            after.revision > before.revision &&
            after.temporary_drafts.size() == 1 &&
            after.temporary_drafts[0].status ==
                specforge::SampleLabelingRecoveryDraftStatus::Recoverable,
        "a formal task lease conflict must advance recovery revision without conflicting the independent draft");
}

void TestRecoveryViewMarksOwnDraftLeaseConflictAndAdvancesRevision()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_draft_conflict_recovery_projection");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    {
        specforge::SampleLabelingController seed(cache_path);
        seed.ActivateSource("shared-source", 3);
        Require(
            seed.CreateTask(
                        "paused-draft",
                        "Paused draft")
                .accepted,
            "draft conflict fixture should create a draft");
        Require(
            seed.DeactivateActiveTask().state_saved,
            "draft conflict fixture should persist the draft");
    }

    specforge::SampleLabelingController observer(cache_path);
    observer.ActivateSource("shared-source", 3);
    specforge::SampleLabelingController owner(cache_path);
    owner.ActivateSource("shared-source", 3);
    Require(
        owner.ActivateTask("paused-draft").accepted,
        "draft conflict fixture should acquire the draft lease");

    const specforge::SampleLabelingRecoveryView before =
        observer.RecoveryView();
    Require(
        before.temporary_drafts.size() == 1 &&
            before.temporary_drafts[0].status ==
                specforge::SampleLabelingRecoveryDraftStatus::Recoverable,
        "the observer should not infer a conflict before trying the draft");
    const specforge::SampleLabelingOperationResult rejected =
        observer.ActivateTask("paused-draft");
    const specforge::SampleLabelingRecoveryView after =
        observer.RecoveryView();
    Require(
        !rejected.accepted &&
            rejected.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable &&
            after.revision > before.revision &&
            after.temporary_drafts.size() == 1 &&
            after.temporary_drafts[0].status ==
                specforge::SampleLabelingRecoveryDraftStatus::Conflicting,
        "a draft's own lease conflict must mark only that draft and advance recovery revision");
}

void TestOutputPathAliasesShareConflictAndLeaseIdentity()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_output_alias_identity");
    const std::filesystem::path real_directory =
        directory / "real";
    const std::filesystem::path alias_directory =
        directory / "alias";
    std::filesystem::create_directories(real_directory);
    Require(
        CreateDirectoryJunction(
            alias_directory,
            real_directory),
        "output alias fixture should create a directory junction");

    const std::filesystem::path real_output =
        real_directory / "shared_y.npy";
    const std::filesystem::path alias_output =
        alias_directory / "shared_y.npy";
    WriteTextFile(real_output, "identity fixture\n");

    specforge::SampleLabelingStateCache conflict_cache;
    specforge::SampleLabelingSourceState conflict_source;
    conflict_source.sample_count = 3;
    specforge::SampleLabelingTask conflict_task =
        specforge::CreateSampleLabelingTask(
            "real-task",
            "Real task",
            3);
    conflict_task.output_path = real_output;
    conflict_task.output_format =
        specforge::SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
    conflict_source.tasks.push_back(conflict_task);
    conflict_cache.sources.emplace(
        "real-source",
        conflict_source);
    Require(
        specforge::HasSampleLabelingOutputPathConflict(
            conflict_cache,
            OutputTask(
                "alias-task",
                alias_output,
                specforge::SampleLabelingOutputArtifactFormat::
                    LegacyNpyWithSidecar),
            "alias-source"),
        "an existing output and its junction alias must share one cache conflict identity");

    const std::filesystem::path real_future =
        real_directory / "future_y.npy";
    const std::filesystem::path alias_future =
        alias_directory / "future_y.npy";
    conflict_cache.sources.at("real-source")
        .tasks.front()
        .output_path = real_future;
    Require(
        specforge::HasSampleLabelingOutputPathConflict(
            conflict_cache,
            OutputTask(
                "alias-task",
                alias_future,
                specforge::SampleLabelingOutputArtifactFormat::
                    LegacyNpyWithSidecar),
            "alias-source"),
        "a missing output must use its resolved parent identity plus leaf name");

    specforge::SampleLabelingStateCache lease_cache;
    for (const auto& [source_identity, task_id, output_path] :
         std::array{
             std::tuple{
                 std::string{"real-source"},
                 std::string{"real-task"},
                 real_output},
             std::tuple{
                 std::string{"alias-source"},
                 std::string{"alias-task"},
                 alias_output}}) {
        specforge::SampleLabelingSourceState source;
        source.sample_count = 3;
        specforge::SampleLabelingTask task =
            specforge::CreateSampleLabelingTask(
                task_id,
                task_id,
                3);
        task.output_path = output_path;
        task.output_format =
            specforge::SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
        source.tasks.push_back(std::move(task));
        lease_cache.sources.emplace(
            source_identity,
            std::move(source));
    }
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    Require(
        specforge::SaveSampleLabelingStateCache(
            cache_path,
            lease_cache),
        "output alias fixture should save both historical task records");
    const auto structural_loader = [](
                                       const std::filesystem::path& path) {
        return specforge::LoadSampleLabelingStateCache(
            path,
            {},
            specforge::SampleLabelingStateCacheLoadPolicy::
                AllowPersistentOutputsWithoutResultHydration);
    };
    specforge::SampleLabelingController first(
        cache_path,
        structural_loader);
    specforge::SampleLabelingController second(
        cache_path,
        structural_loader);
    first.ActivateSource("real-source", 3);
    second.ActivateSource("alias-source", 3);
    Require(
        first.ActivateTask("real-task").accepted,
        "first alias fixture instance should acquire the physical output");
    const specforge::SampleLabelingOperationResult rejected =
        second.ActivateTask("alias-task");
    Require(
        !rejected.accepted &&
            rejected.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable,
        "a junction alias must not acquire a second output lease for the same physical target");
}

void TestExistingHardLinksShareFileObjectIdentity()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_file_object_identity");
    const std::filesystem::path first_directory =
        directory / "first";
    const std::filesystem::path second_directory =
        directory / "second";
    std::filesystem::create_directories(first_directory);
    std::filesystem::create_directories(second_directory);
    const std::filesystem::path first_output =
        first_directory / "shared_y.npy";
    const std::filesystem::path alias_output =
        second_directory / "alias_y.npy";
    WriteTextFile(first_output, "file-object fixture\n");
    Require(
        CreateFileHardLink(
            alias_output,
            first_output),
        "file-object identity fixture should create a hard link");

    specforge::SampleLabelingStateCache conflict_cache;
    specforge::SampleLabelingSourceState first_source;
    first_source.sample_count = 3;
    specforge::SampleLabelingTask first_task =
        specforge::CreateSampleLabelingTask(
            "first-task",
            "First task",
            3);
    first_task.output_path = first_output;
    first_task.output_format =
        specforge::SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
    first_source.tasks.push_back(first_task);
    conflict_cache.sources.emplace(
        "first-source",
        first_source);
    Require(
        specforge::HasSampleLabelingOutputPathConflict(
            conflict_cache,
            OutputTask(
                "alias-task",
                alias_output,
                specforge::SampleLabelingOutputArtifactFormat::
                    LegacyNpyWithSidecar),
            "alias-source"),
        "hard links to one existing result must share a cache conflict identity");

    specforge::SampleLabelingSourceState alias_source;
    alias_source.sample_count = 3;
    specforge::SampleLabelingTask alias_task =
        specforge::CreateSampleLabelingTask(
            "alias-task",
            "Alias task",
            3);
    alias_task.output_path = alias_output;
    alias_task.output_format =
        specforge::SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
    alias_source.tasks.push_back(alias_task);
    conflict_cache.sources.emplace(
        "alias-source",
        std::move(alias_source));
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    Require(
        specforge::SaveSampleLabelingStateCache(
            cache_path,
            conflict_cache),
        "file-object identity fixture should save historical aliases");
    const auto structural_loader = [](
                                       const std::filesystem::path& path) {
        return specforge::LoadSampleLabelingStateCache(
            path,
            {},
            specforge::SampleLabelingStateCacheLoadPolicy::
                AllowPersistentOutputsWithoutResultHydration);
    };
    specforge::SampleLabelingController first(
        cache_path,
        structural_loader);
    specforge::SampleLabelingController second(
        cache_path,
        structural_loader);
    first.ActivateSource("first-source", 3);
    second.ActivateSource("alias-source", 3);
    Require(
        first.ActivateTask("first-task").accepted,
        "first hard-link editor should acquire the file object");
    const specforge::SampleLabelingOperationResult rejected =
        second.ActivateTask("alias-task");
    Require(
        !rejected.accepted &&
            rejected.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable,
        "a hard-link alias must not acquire a second lease for the same file object");
}

void TestOutputArtifactSetSharesConflictAndLeaseIdentity()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_output_artifact_set");
    const std::filesystem::path npy_output =
        directory / "shared.npy";
    const std::filesystem::path csv_output =
        directory / "shared.csv";
    const std::filesystem::path asdf_output =
        directory / "shared.asdf";
    Require(
        specforge::SampleAnnotationIoAdapter::
            MetadataPathForResult(npy_output) ==
            specforge::SampleAnnotationIoAdapter::
                MetadataPathForResult(csv_output),
        "artifact-set fixture should share one metadata sidecar");

    specforge::SampleLabelingStateCache cache;
    specforge::SampleLabelingSourceState first_source;
    first_source.sample_count = 3;
    specforge::SampleLabelingTask first_task =
        specforge::CreateSampleLabelingTask(
            "npy-task",
            "NPY task",
            3);
    first_task.output_path = npy_output;
    first_task.output_format =
        specforge::SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
    first_source.tasks.push_back(first_task);
    cache.sources.emplace(
        "npy-source",
        first_source);
    Require(
        specforge::HasSampleLabelingOutputPathConflict(
            cache,
            OutputTask(
                "csv-task",
                csv_output,
                specforge::SampleLabelingOutputArtifactFormat::
                    LegacyNpyWithSidecar),
            "csv-source"),
        "output conflict checks must include the derived metadata sidecar");
    Require(
        !specforge::HasSampleLabelingOutputPathConflict(
            cache,
            OutputTask(
                "asdf-task",
                asdf_output,
                specforge::SampleLabelingOutputArtifactFormat::CanonicalAsdf),
            "asdf-source"),
        "canonical ASDF must not conflict with a legacy NPY sidecar that shares only its stem");

    specforge::SampleLabelingSourceState second_source;
    second_source.sample_count = 3;
    specforge::SampleLabelingTask second_task =
        specforge::CreateSampleLabelingTask(
            "csv-task",
            "CSV task",
            3);
    second_task.output_path = csv_output;
    second_task.output_format =
        specforge::SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
    second_source.tasks.push_back(second_task);
    cache.sources.emplace(
        "csv-source",
        second_source);
    specforge::SampleLabelingSourceState asdf_source;
    asdf_source.sample_count = 3;
    asdf_source.source_name = "asdf-source.npy";
    asdf_source.source_fingerprint =
        "asdf-source-fingerprint";
    asdf_source.tasks.push_back(
        OutputTask(
            "asdf-task",
            asdf_output,
            specforge::SampleLabelingOutputArtifactFormat::CanonicalAsdf));
    cache.sources.emplace(
        "asdf-source",
        std::move(asdf_source));
    specforge::SampleLabelingDocument asdf_document;
    asdf_document.source.base_identity =
        "asdf-source";
    asdf_document.source.kind = "npy";
    asdf_document.source.name = "asdf-source.npy";
    asdf_document.source.fingerprint =
        "asdf-source-fingerprint";
    asdf_document.source.sample_count = 3;
    asdf_document.annotation.name = "ASDF task";
    asdf_document.annotation.values = {-1, -1, -1};
    asdf_document.labeling.id = "asdf-task";
    asdf_document.labeling.name = "ASDF task";
    Require(
        specforge::WriteSampleLabelingAsdfDocumentAtomically(
            asdf_output,
            asdf_document)
            .succeeded(),
        "artifact-set fixture should publish its canonical owner");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    Require(
        specforge::SaveSampleLabelingStateCache(
            cache_path,
            cache),
        "artifact-set fixture should save historical overlapping records");

    const auto structural_loader = [](
                                       const std::filesystem::path& path) {
        return specforge::LoadSampleLabelingStateCache(
            path,
            {},
            specforge::SampleLabelingStateCacheLoadPolicy::
                AllowPersistentOutputsWithoutResultHydration);
    };
    specforge::SampleLabelingController first(
        cache_path,
        structural_loader);
    specforge::SampleLabelingController second(
        cache_path,
        structural_loader);
    specforge::SampleLabelingController canonical(
        cache_path,
        structural_loader);
    first.ActivateSource("npy-source", 3);
    second.ActivateSource("csv-source", 3);
    canonical.ActivateSource(
        specforge::SourceCollectionIdentity{
            .id = "asdf-source",
            .source_name = "asdf-source.npy",
            .source_fingerprint =
                "asdf-source-fingerprint",
            .context_fingerprint =
                "asdf-context",
            .spectrum_count = 3,
        },
        specforge::
            SampleLabelingCanonicalSourceDescriptor{
                .base_identity = "asdf-source",
                .source_kind = "npy",
                .source_name = "asdf-source.npy",
                .source_fingerprint =
                    "asdf-source-fingerprint",
                .sample_count = 3,
            });
    Require(
        first.ActivateTask("npy-task").accepted,
        "first artifact-set editor should acquire its result and sidecar leases");
    Require(
        canonical.ActivateTask("asdf-task").accepted,
        "canonical ASDF should acquire only its document lease from persisted task ownership");
    const specforge::SampleLabelingOperationResult rejected =
        second.ActivateTask("csv-task");
    Require(
        !rejected.accepted &&
            rejected.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable,
        "a shared sidecar must reject the second result editor even when result extensions differ");
}

void TestOfflineHistoricalOutputDoesNotBlockUnrelatedPatch()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_offline_historical_output");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path offline_output =
        directory / "disconnected-volume" /
        "historical.npy";

    specforge::SampleLabelingStateCache cache;
    specforge::SampleLabelingSourceState offline_source;
    offline_source.sample_count = 3;
    specforge::SampleLabelingTask offline_task =
        specforge::CreateSampleLabelingTask(
            "offline-task",
            "Offline task",
            3);
    offline_task.output_path = offline_output;
    offline_task.output_format =
        specforge::SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
    offline_source.tasks.push_back(offline_task);
    cache.sources.emplace(
        "offline-source",
        offline_source);
    Require(
        specforge::SaveSampleLabelingStateCache(
            cache_path,
            cache),
        "offline-history fixture should persist its historical task");

    specforge::SampleLabelingStateCachePatch unrelated;
    unrelated.sources["local-source"].metadata =
        specforge::SampleLabelingSourceMetadataPatch{
            .sample_count = 3,
            .source_name = "Local source"};
    std::string error;
    Require(
        specforge::CommitSampleLabelingStateCachePatch(
            cache_path,
            unrelated,
            &error),
        error.empty()
            ? "an offline historical output must not block an unrelated cache patch"
            : error);
    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(
            cache_path,
            {},
            specforge::SampleLabelingStateCacheLoadPolicy::
                AllowPersistentOutputsWithoutResultHydration);
    Require(
        FindTask(
            loaded.cache,
            "offline-source",
            "offline-task") != nullptr &&
            loaded.cache.sources.contains(
                "local-source"),
        "unrelated patch should preserve the offline historical record");

    specforge::SampleLabelingTask new_offline_task =
        specforge::CreateSampleLabelingTask(
            "new-offline-task",
            "New offline task",
            3);
    new_offline_task.output_path =
        directory / "another-disconnected-volume" /
        "new.npy";
    new_offline_task.output_format =
        specforge::SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
    specforge::SampleLabelingStateCachePatch unsafe;
    unsafe.sources["new-offline-source"].metadata =
        specforge::SampleLabelingSourceMetadataPatch{
            .sample_count = 3};
    unsafe.sources["new-offline-source"]
        .task_upserts.push_back(
            std::move(new_offline_task));
    const std::string before_unsafe =
        ReadTextFile(cache_path);
    error.clear();
    Require(
        specforge::CommitSampleLabelingStateCachePatch(
            cache_path,
            unsafe,
            &error),
        error.empty()
            ? "a newly introduced offline output should use its normalized path identity"
            : error);
    Require(
        ReadTextFile(cache_path) != before_unsafe,
        "a newly introduced offline output should be persisted by its path identity");
    const specforge::SampleLabelingStateCacheLoadResult after_unsafe =
        specforge::LoadSampleLabelingStateCache(
            cache_path,
            {},
            specforge::SampleLabelingStateCacheLoadPolicy::
                AllowPersistentOutputsWithoutResultHydration);
    Require(
        FindTask(
            after_unsafe.cache,
            "new-offline-source",
            "new-offline-task") != nullptr,
        "a path-only output identity should remain editable in a single instance");
}

void TestOfflineOutputLeaseFallsBackToStablePathIdentity()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_offline_output_lease");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "disconnected-volume" /
        "historical.npy";

    const specforge::SampleAnnotationArtifactIdentitySet identities =
        specforge::SampleAnnotationArtifactIdentities(
            output_path,
            specforge::SampleLabelingOutputArtifactFormat::
                LegacyNpyWithSidecar);
    Require(
        !identities.stable_path_keys.empty(),
        "offline output identity should always retain its normalized path key");
    Require(
        !identities.all_paths_physically_resolved,
        "offline output fixture should exercise the optional physical identity path");

    specforge::SampleLabelingStateCache cache;
    specforge::SampleLabelingSourceState source;
    source.sample_count = 3;
    specforge::SampleLabelingTask task =
        specforge::CreateSampleLabelingTask(
            "offline-task",
            "Offline task",
            3);
    task.output_path = output_path;
    task.output_format =
        specforge::SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
    source.tasks.push_back(std::move(task));
    cache.sources.emplace("offline-source", std::move(source));
    Require(
        specforge::SaveSampleLabelingStateCache(
            cache_path,
            cache),
        "offline output lease fixture should persist its cache record");

    const auto structural_loader = [](
                                       const std::filesystem::path& path) {
        return specforge::LoadSampleLabelingStateCache(
            path,
            {},
            specforge::SampleLabelingStateCacheLoadPolicy::
                AllowPersistentOutputsWithoutResultHydration);
    };
    specforge::SampleLabelingController first(
        cache_path,
        structural_loader);
    specforge::SampleLabelingController second(
        cache_path,
        structural_loader);
    first.ActivateSource("offline-source", 3);
    second.ActivateSource("offline-source", 3);
    Require(
        first.ActivateTask("offline-task").accepted,
        "single-instance labeling should acquire a path-only output lease");
    const specforge::SampleLabelingOperationResult rejected =
        second.ActivateTask("offline-task");
    Require(
        !rejected.accepted &&
            rejected.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable,
        "the normalized path fallback should still exclude a second editor");
}

void TestSynchronousFlushWaitsForShortCommitLockContention()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_commit_lock_wait");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    specforge::SampleLabelingController controller(
        cache_path);
    controller.ActivateSource("shared-source", 3);
    Require(
        controller.CreateTask("shared-task", "Shared task")
            .state_saved,
        "commit-lock fixture should create its task");
    Require(
        controller.RememberActivePosition(1).changed &&
            controller.state_save_pending(),
        "commit-lock fixture should leave a patch pending");

    specforge::ExclusiveFileLeaseAcquireResult commit_lock =
        specforge::TryAcquireExclusiveFileLease(
            specforge::SampleLabelingStateCoordinationDirectory(
                cache_path) /
            "cache-commit.lock");
    Require(
        commit_lock.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::
                Acquired,
        "commit-lock fixture should acquire the short-lived lock");
    std::thread releaser(
        [lease = std::move(commit_lock.lease)]() mutable {
            Sleep(75);
            lease.Reset();
        });
    const bool flushed =
        controller.FlushStateCache();
    releaser.join();
    Require(
        flushed,
        "synchronous flush should wait through brief commit-lock contention");
    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(cache_path);
    const specforge::SampleLabelingTask* task =
        FindTask(
            loaded.cache,
            "shared-source",
            "shared-task");
    Require(
        task != nullptr &&
            task->remembered_position ==
                std::optional<std::size_t>{1},
        "bounded lock retry should persist the pending patch before shutdown can discard it");
}

void TestLeaseHandoffRefreshInvalidatesTaskProjectionGeneration()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_handoff_generation");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "shared.npy";
    {
        specforge::SampleLabelingController seed(cache_path);
        CreateFormalTask(
            seed,
            "shared-source",
            "shared-task",
            output_path,
            5);
    }

    specforge::SampleLabelingController stale(cache_path);
    stale.ActivateSource("shared-source", 3);
    Require(
        ActiveSourceTasks(stale) != nullptr &&
            ActiveSourceTasks(stale)->size() == 1,
        "handoff-generation fixture should preheat the stale task projection");
    const std::uint64_t stale_generation =
        stale.active_source_tasks_generation();

    {
        specforge::SampleLabelingController editing(cache_path);
        editing.ActivateSource("shared-source", 3);
        Require(
            editing.ActivateTask("shared-task").accepted,
            "editing instance should acquire the shared task");
        Require(
            editing.UpsertActiveLabel(
                       specforge::SampleLabelDefinition{
                           7,
                           "reviewed",
                           'r'})
                .output_saved &&
                editing.AssignLabel(1, 7)
                    .operation.output_saved,
            "editing instance should persist refreshed labels and values");
        Require(
            editing.DeactivateActiveTask().state_saved,
            "editing instance should release the refreshed task");
    }

    Require(
        stale.ActivateTask("shared-task").accepted,
        "stale instance should acquire the released task");
    Require(
        ActiveTask(stale) != nullptr &&
            ActiveTask(stale)->values[1] == 7 &&
            specforge::FindSampleLabel(
                ActiveTask(stale)->label_set,
                7) != nullptr,
        "lease handoff should refresh the complete task before editing");
    Require(
        stale.active_source_tasks_generation() >
            stale_generation,
        "a substantive handoff refresh must invalidate derived filter and sorting projections");
}

void TestSameSourceRefreshInvalidatesTaskProjectionGeneration()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_same_source_refresh_generation");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    {
        specforge::SampleLabelingController seed(cache_path);
        CreateFormalTask(
            seed,
            "shared-source",
            "shared-task",
            directory / "shared.npy",
            5);
        CreateFormalTask(
            seed,
            "shared-source",
            "deleted-task",
            directory / "deleted.npy",
            7);
    }

    specforge::SampleLabelingController owner(cache_path);
    owner.ActivateSource("shared-source", 3);
    Require(
        owner.ActivateTask("shared-task").state_saved,
        "same-source refresh fixture should persist the task lease owner selection");

    specforge::SampleLabelingController stale(cache_path);
    stale.ActivateSource("shared-source", 3);
    const auto contains_active_source_task =
        [](const std::vector<specforge::SampleLabelingTask>* tasks,
           std::string_view task_id) {
            return tasks != nullptr &&
                std::any_of(
                    tasks->begin(),
                    tasks->end(),
                    [task_id](const specforge::SampleLabelingTask& task) {
                        return task.task_id == task_id;
                    });
        };
    const specforge::SampleLabelingOperationResult conflict =
        stale.ActivateTask("shared-task");
    Require(
        !conflict.accepted &&
            conflict.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable &&
            stale.View().active_task == nullptr &&
            ActiveSourceTasks(stale) != nullptr &&
            ActiveSourceTasks(stale)->size() == 2 &&
            contains_active_source_task(
                ActiveSourceTasks(stale),
                "shared-task") &&
            contains_active_source_task(
                ActiveSourceTasks(stale),
                "deleted-task"),
        "same-source refresh fixture should leave the stale GUI with a read-only task projection after the lease conflict");

    {
        specforge::SampleLabelingController deleting(cache_path);
        deleting.ActivateSource("shared-source", 3);
        Require(
            deleting.ActivateTask("deleted-task").accepted,
            "same-source refresh fixture should acquire the other task for the tombstone");
        Require(
            deleting.DeleteActiveTask().state_saved,
            "same-source refresh fixture should commit the other task tombstone");
    }

    const std::uint64_t generation_before_refresh =
        stale.active_source_tasks_generation();
    stale.ActivateSource("shared-source", 3);
    Require(
        stale.active_source_tasks_generation() >
            generation_before_refresh,
        "same-source lease-conflict refresh must invalidate derived task projections when the task list changes");
    Require(
        ActiveSourceTasks(stale) != nullptr &&
            ActiveSourceTasks(stale)->size() == 1 &&
            contains_active_source_task(
                ActiveSourceTasks(stale),
                "shared-task") &&
            !contains_active_source_task(
                ActiveSourceTasks(stale),
                "deleted-task"),
        "same-source lease-conflict refresh must remove a tombstoned task from the stale GUI projection");
}

void TestSameSourceReopenRemovesMissingTaskProjection()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_same_source_reopen_missing");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    {
        specforge::SampleLabelingController seed(cache_path);
        seed.ActivateSource("shared-source", 3);
        Require(
            seed.CreateTask("ghost-task", "Ghost task").state_saved,
            "missing-task reopen fixture should persist its active task selection");
    }

    specforge::SampleLabelingController stale(cache_path);
    stale.ActivateSource("shared-source", 3);
    Require(
        ActiveTask(stale) != nullptr &&
            ActiveTask(stale)->task_id == "ghost-task",
        "missing-task reopen fixture should load the task before the external tombstone");

    specforge::SampleLabelingStateCachePatch tombstone;
    tombstone.sources["shared-source"].task_tombstones.push_back(
        "ghost-task");
    tombstone.sources["shared-source"].active_task_selection_changed =
        true;
    tombstone.sources["shared-source"].active_task_id.reset();
    std::string error;
    Require(
        specforge::CommitSampleLabelingStateCachePatch(
            cache_path,
            tombstone,
            &error),
        error.empty()
            ? "missing-task reopen fixture should commit the external tombstone"
            : error);

    stale.ClearActiveSource();
    const std::uint64_t generation_before_reopen =
        stale.active_source_tasks_generation();
    stale.ActivateSource("shared-source", 3);
    Require(
        stale.active_source_tasks_generation() >
            generation_before_reopen &&
            ActiveTask(stale) == nullptr &&
            ActiveSourceTasks(stale) != nullptr &&
            ActiveSourceTasks(stale)->empty(),
        "same-source reopen after a missing active task must remove the tombstoned task from the projection");
}

void TestDirectLeaseConflictRefreshRemovesTombstonedTaskProjection()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_direct_conflict_tombstone");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    {
        specforge::SampleLabelingController seed(cache_path);
        seed.ActivateSource("shared-source", 3);
        Require(
            seed.CreateTask("ghost-task", "Ghost task").state_saved,
            "direct-conflict fixture should create the task");
        Require(
            seed.DeactivateActiveTask().state_saved,
            "direct-conflict fixture should leave the task unselected");
    }

    specforge::SampleLabelingController stale(cache_path);
    stale.ActivateSource("shared-source", 3);
    Require(
        ActiveTask(stale) == nullptr &&
            ActiveSourceTasks(stale) != nullptr &&
            ActiveSourceTasks(stale)->size() == 1,
        "direct-conflict fixture should preheat a stale unselected task projection");

    specforge::SampleLabelingController owner(cache_path);
    owner.ActivateSource("shared-source", 3);
    Require(
        owner.ActivateTask("ghost-task").state_saved,
        "direct-conflict fixture should acquire the task lease in the owner");
    const specforge::SampleLabelingOperationResult conflict =
        stale.ActivateTask("ghost-task");
    Require(
        !conflict.accepted &&
            conflict.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable,
        "direct task activation should observe the owner lease conflict");

    Require(
        owner.DeleteActiveTask().state_saved,
        "direct-conflict fixture should commit the tombstone from the lease owner");

    const std::uint64_t generation_before_reopen =
        stale.active_source_tasks_generation();
    stale.ActivateSource("shared-source", 3);
    Require(
        stale.active_source_tasks_generation() >
            generation_before_reopen &&
            ActiveTask(stale) == nullptr &&
            ActiveSourceTasks(stale) != nullptr &&
            ActiveSourceTasks(stale)->empty(),
        "same-source reopen after a direct lease conflict must refresh away the tombstoned task");
}

void TestLeaseSetupFailureIsNotReportedAsAnotherEditor()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_lease_setup_failure");
    const std::filesystem::path blocker =
        directory / "not-a-directory";
    WriteTextFile(blocker, "blocker\n");
    specforge::SampleLabelingController controller(
        blocker / "sample-labeling-tasks.json");
    controller.ActivateSource("shared-source", 3);

    const specforge::SampleLabelingOperationResult rejected =
        controller.CreateTask(
            "shared-task",
            "Shared task");
    Require(
        !rejected.accepted &&
            rejected.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditLeaseFailed,
        "lease directory failures must not be reported as another active editor");
}

void TestLongCoordinationPathCanAcquireTargetLease()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_long_coordination_path");
    std::filesystem::path state_directory = directory;
    const auto representative_lease_path = [](
                                               const std::filesystem::path& directory) {
        const std::filesystem::path cache =
            directory / "sample-labeling-tasks.json";
        return specforge::SampleLabelingStateCoordinationDirectory(
                   cache) /
            "targets" /
            (std::string(64, 'a') + ".lock");
    };
    while (representative_lease_path(
               state_directory)
               .wstring()
               .size() <= 260) {
        state_directory /=
            "state-directory-segment-0123456789";
    }
    const std::filesystem::path cache_path =
        state_directory /
        "sample-labeling-tasks.json";
    const std::filesystem::path representative_target_lease =
        representative_lease_path(
            state_directory);
    Require(
        cache_path.wstring().size() < 230 &&
            representative_target_lease.wstring().size() > 260,
        "long-path fixture must isolate the added target-lease suffix");

    specforge::SampleLabelingController controller(cache_path);
    controller.ActivateSource("long-path-source", 3);
    const specforge::SampleLabelingOperationResult created =
        controller.CreateTask(
            "long-path-task",
            "Long path task");
    Require(
        created.accepted,
        "a target lease beyond MAX_PATH should remain editable");
    Require(
        created.state_saved,
        "an edit protected by a long target-lease path should persist its cache record");
}

void TestFirstCacheCreationKeepsStableTaskLease()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_first_cache_lease");
    const std::filesystem::path cache_path =
        directory / "created-later" / "sample-labeling-tasks.json";

    specforge::SampleLabelingController first(cache_path);
    first.ActivateSource("shared-source", 3);
    Require(
        first.CreateTask("shared-task", "Shared task").accepted,
        "the first writer should create the cache and draft");

    specforge::SampleLabelingController second(cache_path);
    second.ActivateSource("shared-source", 3);
    const specforge::SampleLabelingOperationResult rejected =
        second.ActivateTask("shared-task");
    Require(
        !rejected.accepted &&
            rejected.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable,
        "a second instance must not bypass the normalized task lease after first cache creation");
}

void TestTemporaryFormalizationRequiresRecoveryCheckpoint()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_formalization_checkpoint");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "formalized.asdf";
    std::size_t persist_calls = 0;
    std::string task_id;
    {
        specforge::SampleLabelingController controller(
            cache_path,
            [](const std::filesystem::path& path) {
                return specforge::LoadSampleLabelingStateCache(path);
            },
            [&](specforge::SampleLabelingTask& task,
                const specforge::SampleLabelResultMetadataSource* source) {
                ++persist_calls;
                return specforge::PublishLegacySampleLabelingTaskOutput(
                    task,
                    source);
            });
        ActivateCanonicalTestSource(
            controller,
            "shared-source",
            3);
        Require(
            controller.CreateTask("temporary", "Temporary").accepted,
            "checkpoint fixture should create a temporary task");
        Require(
            controller.UpsertActiveLabel({5, "accepted", 'a'}).changed,
            "checkpoint fixture should establish a label definition");
        const specforge::SampleLabelingTask* active =
            ActiveTask(controller);
        Require(
            active != nullptr,
            "checkpoint fixture should retain the active temporary task");
        task_id = active->task_id;

        specforge::ExclusiveFileLeaseAcquireResult commit_lock =
            specforge::TryAcquireExclusiveFileLease(
                specforge::SampleLabelingStateCoordinationDirectory(
                    cache_path) /
                "cache-commit.lock");
        Require(
            commit_lock.status ==
                specforge::ExclusiveFileLeaseAcquireStatus::Acquired,
            "checkpoint fixture should hold the cache commit lock");
        const specforge::SampleLabelingOperationResult blocked =
            controller.SaveActiveTemporaryTaskToOutput(
                output_path,
                "Formalized");
        Require(
            !blocked.accepted &&
                blocked.state_save_attempted &&
                !blocked.output_save_attempted &&
                persist_calls == 0,
            "a failed formalization checkpoint must prevent the output write");
        std::error_code exists_error;
        Require(
            !std::filesystem::exists(output_path, exists_error) &&
                !exists_error,
            "a failed formalization checkpoint must not leave an output artifact");
        commit_lock.lease.Reset();
    }

    specforge::SampleLabelingController recovered(cache_path);
    ActivateCanonicalTestSource(
        recovered,
        "shared-source",
        3);
    Require(
        recovered.ActivateTask(task_id).accepted,
        "a restarted controller should reacquire the still-temporary task after checkpoint failure");
    const specforge::SampleLabelingOperationResult retried =
        recovered.SaveActiveTemporaryTaskToOutput(
            output_path,
            "Formalized");
    Require(
        retried.accepted &&
            retried.output_saved &&
            retried.state_saved,
        "formalization should be retryable after the checkpoint lock is released");
}

void TestTemporaryFormalizationKeepsStableTaskLease()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_temporary_formalization_lease");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "formalized_y.asdf";
    {
        specforge::SampleLabelingController seed(cache_path);
        seed.ActivateSource("shared-source", 3);
        Require(
            seed.CreateTask("shared-task", "Shared draft")
                .accepted,
            "stable-lease fixture should create its draft");
        Require(
            seed.DeactivateActiveTask().state_saved,
            "stable-lease fixture should persist its inactive draft");
    }

    specforge::SampleLabelingController first(cache_path);
    specforge::SampleLabelingController stale_second(cache_path);
    ActivateCanonicalTestSource(
        first,
        "shared-source",
        3);
    ActivateCanonicalTestSource(
        stale_second,
        "shared-source",
        3);
    Require(
        first.ActivateTask("shared-task").accepted,
        "first instance should acquire the draft task lease");
    Require(
        first.SaveActiveTemporaryTaskToOutput(
                 output_path,
                 "Formalized")
            .output_saved,
        "first instance should formalize the draft");

    const specforge::SampleLabelingOperationResult rejected =
        stale_second.ActivateTask("shared-task");
    Require(
        !rejected.accepted &&
            rejected.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable,
        "formalization must retain the stable source/task lease against stale instances");
}

void TestSequentialLeaseHandoffRefreshesLatestTask()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_sequential_handoff_refresh");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "shared_y.npy";
    {
        specforge::SampleLabelingController seed(cache_path);
        CreateFormalTask(
            seed,
            "shared-source",
            "shared-task",
            output_path,
            5);
    }

    specforge::SampleLabelingController first(cache_path);
    specforge::SampleLabelingController stale_second(cache_path);
    first.ActivateSource("shared-source", 3);
    stale_second.ActivateSource("shared-source", 3);
    Require(
        first.ActivateTask("shared-task").accepted,
        "first instance should acquire the task");
    Require(
        first.UpsertActiveLabel(
                 specforge::SampleLabelDefinition{
                     7,
                     "reviewed",
                     'r'})
            .output_saved,
        "first instance should persist its new label definition");
    Require(
        first.AssignLabel(1, 7).operation.output_saved,
        "first instance should persist its sample edit");
    Require(
        first.DeactivateActiveTask().state_saved,
        "first instance should persist and release the task");

    Require(
        stale_second.ActivateTask("shared-task").accepted,
        "second instance should acquire the released task");
    const specforge::SampleLabelingTask* refreshed =
        ActiveTask(stale_second);
    Require(
        refreshed != nullptr &&
            refreshed->values[1] == 7 &&
            specforge::FindSampleLabel(
                refreshed->label_set,
                7) != nullptr,
        "lease handoff must refresh values and metadata before editing");
}

void TestPendingDeletionKeepsTaskLeaseUntilTombstoneCommits()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_pending_delete_lease");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    {
        specforge::SampleLabelingController seed(cache_path);
        CreateFormalTask(
            seed,
            "shared-source",
            "shared-task",
            directory / "shared_y.npy",
            5);
    }

    specforge::SampleLabelingController deleting(cache_path);
    specforge::SampleLabelingController stale_second(cache_path);
    deleting.ActivateSource("shared-source", 3);
    stale_second.ActivateSource("shared-source", 3);
    Require(
        deleting.ActivateTask("shared-task").accepted,
        "deleting instance should acquire the task lease");

    specforge::ExclusiveFileLeaseAcquireResult blocked_commit =
        specforge::TryAcquireExclusiveFileLease(
            specforge::SampleLabelingStateCoordinationDirectory(
                cache_path) /
            "cache-commit.lock");
    Require(
        blocked_commit.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::Acquired,
        "deletion fixture should hold the cache commit lock");
    Require(
        !deleting.DeleteActiveTask().state_saved,
        "deletion should remain pending while the commit lock is held");

    const specforge::SampleLabelingOperationResult rejected =
        stale_second.ActivateTask("shared-task");
    Require(
        !rejected.accepted &&
            rejected.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable,
        "a pending tombstone must retain its stable task lease");
}

void TestFailedTaskSwitchDefersPreviousTaskLease()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_failed_switch_lease");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    {
        specforge::SampleLabelingController seed(cache_path);
        CreateFormalTask(
            seed,
            "shared-source",
            "first-task",
            directory / "first_y.npy",
            5);
        CreateFormalTask(
            seed,
            "shared-source",
            "second-task",
            directory / "second_y.npy",
            7);
    }

    specforge::SampleLabelingController switching(cache_path);
    specforge::SampleLabelingController stale_second(cache_path);
    switching.ActivateSource("shared-source", 3);
    stale_second.ActivateSource("shared-source", 3);
    Require(
        switching.ActivateTask("first-task").accepted,
        "switching instance should acquire the first task");
    Require(
        switching.RememberActivePosition(1).changed,
        "switching fixture should leave a first-task upsert pending");

    specforge::ExclusiveFileLeaseAcquireResult blocked_commit =
        specforge::TryAcquireExclusiveFileLease(
            specforge::SampleLabelingStateCoordinationDirectory(
                cache_path) /
            "cache-commit.lock");
    Require(
        blocked_commit.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::Acquired,
        "switch fixture should hold the cache commit lock");
    const specforge::SampleLabelingOperationResult switched =
        switching.ActivateTask("second-task");
    Require(
        switched.accepted && !switched.state_saved,
        "task switch should remain locally active while its cache patch is pending");
    const specforge::SampleLabelingOperationResult blocked =
        stale_second.ActivateTask("first-task");
    Require(
        !blocked.accepted &&
            blocked.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable,
        "failed switch persistence must retain the previous task lease while its upsert is pending");

    blocked_commit.lease.Reset();
    Require(
        switching.FlushStateCache(),
        "pending switch patch should commit after the lock is released");
    Require(
        stale_second.ActivateTask("first-task").accepted,
        "successful patch retry should release the deferred previous-task lease");
    Require(
        ActiveTask(stale_second) != nullptr &&
            ActiveTask(stale_second)->remembered_position == 1,
        "the next editor should refresh the protected first-task upsert");
}

void TestDeferredTaskLeaseCanBeReusedByItsController()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_deferred_lease_reuse");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    {
        specforge::SampleLabelingController seed(cache_path);
        CreateFormalTask(
            seed,
            "shared-source",
            "first-task",
            directory / "first_y.npy",
            5);
        CreateFormalTask(
            seed,
            "shared-source",
            "second-task",
            directory / "second_y.npy",
            7);
    }

    specforge::SampleLabelingController switching(cache_path);
    specforge::SampleLabelingController other(cache_path);
    switching.ActivateSource("shared-source", 3);
    other.ActivateSource("shared-source", 3);
    Require(
        switching.ActivateTask("first-task").accepted,
        "deferred-reuse fixture should acquire the first task");
    Require(
        switching.RememberActivePosition(1).changed,
        "deferred-reuse fixture should leave a protected upsert pending");

    specforge::ExclusiveFileLeaseAcquireResult blocked_commit =
        specforge::TryAcquireExclusiveFileLease(
            specforge::SampleLabelingStateCoordinationDirectory(
                cache_path) /
            "cache-commit.lock");
    Require(
        blocked_commit.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::Acquired,
        "deferred-reuse fixture should hold the cache commit lock");
    Require(
        switching.ActivateTask("second-task").accepted,
        "deferred-reuse fixture should switch locally while persistence is blocked");
    Require(
        other.ActivateTask("first-task").issue ==
            specforge::SampleLabelingOperationResult::Issue::
                EditLeaseUnavailable,
        "another controller must remain blocked by the deferred first-task lease");

    const specforge::SampleLabelingOperationResult switched_back =
        switching.ActivateTask("first-task");
    Require(
        switched_back.accepted &&
            switched_back.issue ==
                specforge::SampleLabelingOperationResult::Issue::None,
        "the owning controller should atomically reuse its deferred task lease when switching back");
    Require(
        ActiveTask(switching) != nullptr &&
            ActiveTask(switching)->remembered_position == 1,
        "reusing a deferred lease must retain the locally accepted pending task projection");
    const specforge::SampleLabelingOperationResult second_edit =
        switching.SetActiveAutoAdvance(true);
    Require(
        second_edit.changed && !second_edit.state_saved,
        "a second edit after deferred-lease reuse should remain pending while the commit lock is held");
    Require(
        other.ActivateTask("first-task").issue ==
            specforge::SampleLabelingOperationResult::Issue::
                EditLeaseUnavailable,
        "reusing a deferred lease must preserve exclusion against other controllers");

    blocked_commit.lease.Reset();
    Require(
        switching.FlushStateCache(),
        "deferred-reuse patch should commit after lock contention ends");
    Require(
        switching.DeactivateActiveTask().state_saved,
        "deferred-reuse owner should release the first task after persistence");
    Require(
        other.ActivateTask("first-task").accepted,
        "another controller should acquire the first task after the owner releases it");
    Require(
        ActiveTask(other) != nullptr &&
            ActiveTask(other)->remembered_position == 1 &&
            ActiveTask(other)->auto_advance,
        "the next controller should refresh both edits protected by the reused lease");
}

void TestDeferredTaskLeaseRestoreRetainsPendingTaskProjection()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_deferred_lease_restore");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    {
        specforge::SampleLabelingController seed(cache_path);
        CreateFormalTask(
            seed,
            "source-a",
            "task-a",
            directory / "source_a_y.npy",
            5);
        CreateFormalTask(
            seed,
            "source-b",
            "task-b",
            directory / "source_b_y.npy",
            7);
    }

    specforge::SampleLabelingController switching(cache_path);
    specforge::SampleLabelingController other(cache_path);
    switching.ActivateSource("source-a", 3);
    other.ActivateSource("source-a", 3);
    Require(
        switching.ActivateTask("task-a").accepted &&
            switching.RememberActivePosition(1).changed,
        "deferred restore fixture should leave a protected source-A upsert pending");

    specforge::ExclusiveFileLeaseAcquireResult blocked_commit =
        specforge::TryAcquireExclusiveFileLease(
            specforge::SampleLabelingStateCoordinationDirectory(
                cache_path) /
            "cache-commit.lock");
    Require(
        blocked_commit.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::Acquired,
        "deferred restore fixture should hold the cache commit lock");
    switching.ActivateSource("source-b", 3);
    switching.ActivateSource("source-a", 3);
    Require(
        ActiveTask(switching) != nullptr &&
            ActiveTask(switching)->task_id == "task-a" &&
            ActiveTask(switching)->remembered_position == 1,
        "restoring source A with its deferred lease must retain the pending local task projection");
    const specforge::SampleLabelingOperationResult second_edit =
        switching.SetActiveAutoAdvance(true);
    Require(
        second_edit.changed && !second_edit.state_saved,
        "the restored task should accept a second edit while the original upsert remains pending");
    Require(
        other.ActivateTask("task-a").issue ==
            specforge::SampleLabelingOperationResult::Issue::
                EditLeaseUnavailable,
        "source restoration must keep excluding another controller until both edits commit");

    blocked_commit.lease.Reset();
    Require(
        switching.FlushStateCache() &&
            switching.DeactivateActiveTask().state_saved,
        "the restored pending task should commit and release its lease after contention ends");
    Require(
        other.ActivateTask("task-a").accepted &&
            ActiveTask(other) != nullptr &&
            ActiveTask(other)->remembered_position == 1 &&
            ActiveTask(other)->auto_advance,
        "the next editor should observe both edits retained across deferred source restoration");
}

void TestPreparedReopenAdoptsUnprotectedTasksAndDeletions()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_prepared_reopen_merge");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const specforge::SourceCollectionIdentity identity{
        "reopen-source",
        "reopen",
        "source-fingerprint",
        "context-fingerprint",
        3};
    {
        specforge::SampleLabelingController seed(cache_path);
        CreateFormalTask(
            seed,
            identity.id,
            "reopen-task",
            directory / "reopen_y.npy",
            5);
    }

    specforge::SampleLabelingController stale(cache_path);
    stale.ActivateSource(identity);
    const std::optional<specforge::SampleLabelingController::SourceState>
        old_state = stale.SourceStateForIdentity(identity.id);
    Require(
        old_state && old_state->tasks.size() == 1 &&
            old_state->tasks[0].values[0] ==
                specforge::kUnlabeledSampleLabelCode,
        "prepared reopen fixture should materialize the original task projection");

    {
        specforge::SampleLabelingController editor(cache_path);
        editor.ActivateSource(identity);
        Require(
            editor.ActivateTask("reopen-task").accepted &&
                editor.AssignLabel(0, 5).operation.output_saved &&
                editor.DeactivateActiveTask().state_saved,
            "another controller should publish a newer task projection");
    }
    specforge::SampleLabelingStateCacheLoadResult modified =
        specforge::LoadSampleLabelingStateCache(cache_path);
    const auto modified_source = modified.cache.sources.find(
        identity.id);
    Require(
        modified_source != modified.cache.sources.end(),
        "modified prepared state should contain the source");
    stale.RemoveSource(identity.id);
    specforge::SampleLabelingPreparedSourceActivationResult
        modified_activation =
            stale.ActivatePreparedSource(
                identity,
                modified_source->second);
    Require(
        modified_activation.prepared_task_projection_changed,
        "reopen should tell the coordinator to rebuild derived projections after adopting newer task values");
    const std::optional<specforge::SampleLabelingController::SourceState>
        reopened = stale.SourceStateForIdentity(identity.id);
    Require(
        reopened && reopened->tasks.size() == 1 &&
            reopened->tasks[0].values[0] == 5,
        "explicit reopen should replace an unprotected stale task with the latest prepared projection");

    {
        specforge::SampleLabelingController deleting(cache_path);
        deleting.ActivateSource(identity);
        Require(
            deleting.ActivateTask("reopen-task").accepted &&
                deleting.DeleteActiveTask().state_saved,
            "another controller should publish the task deletion");
    }
    specforge::SampleLabelingStateCacheLoadResult deleted =
        specforge::LoadSampleLabelingStateCache(cache_path);
    const auto deleted_source = deleted.cache.sources.find(
        identity.id);
    Require(
        deleted_source != deleted.cache.sources.end(),
        "deleted prepared state should retain its source record");
    stale.RemoveSource(identity.id);
    specforge::SampleLabelingPreparedSourceActivationResult
        deleted_activation =
            stale.ActivatePreparedSource(
                identity,
                deleted_source->second);
    Require(
        deleted_activation.prepared_task_projection_changed,
        "reopen should tell the coordinator to rebuild derived projections after adopting a deletion");
    const std::optional<specforge::SampleLabelingController::SourceState>
        after_delete = stale.SourceStateForIdentity(identity.id);
    Require(
        after_delete && after_delete->tasks.empty(),
        "explicit reopen should remove an unprotected task deleted by another controller");
}

void TestPreparedReopenMergesOnlyProtectedLocalTasks()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_prepared_reopen_protected");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const specforge::SourceCollectionIdentity identity{
        "protected-source",
        "protected",
        "source-fingerprint",
        "context-fingerprint",
        3};
    {
        specforge::SampleLabelingController seed(cache_path);
        CreateFormalTask(
            seed,
            identity.id,
            "protected-task",
            directory / "protected_y.npy",
            5);
        CreateFormalTask(
            seed,
            identity.id,
            "unprotected-task",
            directory / "unprotected_y.npy",
            7);
    }

    specforge::SampleLabelingController controller(cache_path);
    controller.ActivateSource(identity);
    Require(
        controller.ActivateTask("protected-task").accepted &&
            controller.RememberActivePosition(1).changed,
        "protected reopen fixture should hold a lease and pending upsert for one task");
    specforge::SampleLabelingStateCacheLoadResult prepared =
        specforge::LoadSampleLabelingStateCache(cache_path);
    auto prepared_source = prepared.cache.sources.find(identity.id);
    Require(
        prepared_source != prepared.cache.sources.end() &&
            prepared_source->second.tasks.size() == 2,
        "protected reopen fixture should load both prepared tasks");
    specforge::SampleLabelingTask* prepared_protected =
        nullptr;
    specforge::SampleLabelingTask* prepared_unprotected =
        nullptr;
    for (specforge::SampleLabelingTask& task :
         prepared_source->second.tasks) {
        if (task.task_id == "protected-task") {
            prepared_protected = &task;
        } else if (task.task_id == "unprotected-task") {
            prepared_unprotected = &task;
        }
    }
    Require(
        prepared_protected != nullptr &&
            prepared_unprotected != nullptr,
        "protected reopen fixture should resolve both task projections");
    prepared_protected->remembered_position.reset();
    prepared_unprotected->task_name =
        "Latest unprotected task";

    specforge::SampleLabelingPreparedSourceActivationResult
        activation =
            controller.ActivatePreparedSource(
                identity,
                prepared_source->second);
    Require(
        activation.prepared_task_projection_changed,
        "prepared reopen should invalidate derived projections when an unprotected task changed");
    const std::optional<specforge::SampleLabelingController::SourceState>
        merged = controller.SourceStateForIdentity(identity.id);
    const auto find_merged_task = [&merged](
                                      std::string_view task_id)
        -> const specforge::SampleLabelingTask* {
        if (!merged) {
            return nullptr;
        }
        const auto task = std::find_if(
            merged->tasks.begin(),
            merged->tasks.end(),
            [task_id](const specforge::SampleLabelingTask& candidate) {
                return candidate.task_id == task_id;
            });
        return task == merged->tasks.end() ? nullptr : &*task;
    };
    const specforge::SampleLabelingTask* protected_task =
        find_merged_task("protected-task");
    const specforge::SampleLabelingTask* unprotected_task =
        find_merged_task("unprotected-task");
    Require(
        protected_task != nullptr &&
            protected_task->remembered_position == 1,
        "pending task upsert should remain authoritative during prepared reopen");
    Require(
        unprotected_task != nullptr &&
            unprotected_task->task_name ==
                "Latest unprotected task",
        "prepared reopen should still adopt unrelated unprotected task changes");
}

void TestRepeatedOutputSavesKeepLeaseDirectoryBounded()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_bounded_lease_files");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path targets =
        specforge::SampleLabelingStateCoordinationDirectory(
            cache_path) /
        "targets";
    {
        specforge::SampleLabelingController seed(cache_path);
        CreateFormalTask(
            seed,
            "bounded-source",
            "bounded-task",
            directory / "bounded_y.npy",
            5);
    }
    Require(
        CountLeaseFiles(targets) == 0,
        "released seed leases should not leave permanent target lock files");

    specforge::SampleLabelingController controller(cache_path);
    controller.ActivateSource("bounded-source", 3);
    Require(
        controller.ActivateTask("bounded-task").accepted,
        "bounded lease fixture should acquire its formal task");
    const std::size_t active_lease_count =
        CountLeaseFiles(targets);
    Require(
        active_lease_count > 0,
        "an active formal task should expose its live target lease files");
    for (int iteration = 0; iteration < 12; ++iteration) {
        const specforge::SampleLabelingWriteOperationResult write =
            iteration % 2 == 0
            ? controller.AssignLabel(0, 5)
            : controller.ClearLabel(0);
        Require(
            write.operation.output_saved &&
                CountLeaseFiles(targets) ==
                    active_lease_count,
            "atomic output replacement should rotate lease identities without growing directory entries");
    }
    Require(
        controller.DeactivateActiveTask().state_saved &&
            CountLeaseFiles(targets) == 0,
        "releasing the final task lease should remove all target lock files");
}

void TestLeaseCleanupSurvivesSuccessorAcquisitionRace()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_lease_cleanup_race");
    const std::filesystem::path targets =
        directory / "targets";
    const std::filesystem::path lease_path =
        targets / "shared.lock";
    specforge::ExclusiveFileLeaseAcquireResult first =
        specforge::TryAcquireExclusiveFileLease(
            lease_path);
    Require(
        first.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::
                Acquired,
        "lease-cleanup race fixture should acquire its first owner");

    std::barrier first_attempt_complete(2);
    specforge::ExclusiveFileLease successor;
    bool saw_unavailable = false;
    bool acquire_failed = false;
    std::thread contender(
        [&]() {
            specforge::ExclusiveFileLeaseAcquireResult blocked =
                specforge::TryAcquireExclusiveFileLease(
                    lease_path);
            saw_unavailable =
                blocked.status ==
                specforge::ExclusiveFileLeaseAcquireStatus::
                    Unavailable;
            acquire_failed =
                blocked.status ==
                specforge::ExclusiveFileLeaseAcquireStatus::
                    Failed;
            first_attempt_complete.arrive_and_wait();
            for (int attempt = 0;
                 attempt < 10'000 && !successor &&
                 !acquire_failed;
                 ++attempt) {
                specforge::ExclusiveFileLeaseAcquireResult retry =
                    specforge::TryAcquireExclusiveFileLease(
                        lease_path);
                if (retry.status ==
                    specforge::ExclusiveFileLeaseAcquireStatus::
                        Acquired) {
                    successor = std::move(retry.lease);
                } else if (
                    retry.status ==
                    specforge::ExclusiveFileLeaseAcquireStatus::
                        Failed) {
                    acquire_failed = true;
                } else {
                    SwitchToThread();
                }
            }
        });
    first_attempt_complete.arrive_and_wait();
    first.lease.Reset();
    contender.join();

    Require(
        saw_unavailable && !acquire_failed && successor,
        "a contending successor should acquire the same lease after its owner releases it");
    Require(
        CountLeaseFiles(targets) == 1,
        "cleanup racing a successor must retain exactly the successor's live lock file");
    successor.Reset();
    Require(
        CountLeaseFiles(targets) == 0,
        "the successor should clean the shared lease name on final release");
}

void TestFailedDeletionRetryClearsPersistedSelection()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_delete_retry_selection");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    {
        specforge::SampleLabelingController seed(cache_path);
        CreateFormalTask(
            seed,
            "shared-source",
            "deleted-task",
            directory / "deleted_y.npy",
            5);
        CreateFormalTask(
            seed,
            "shared-source",
            "surviving-task",
            directory / "surviving_y.npy",
            7);
        Require(
            seed.ActivateTask("deleted-task").state_saved,
            "deletion retry fixture should persist an active-task selection");
    }

    specforge::SampleLabelingController deleting(cache_path);
    deleting.ActivateSource("shared-source", 3);
    Require(
        ActiveTask(deleting) != nullptr,
        "deleting instance should restore and lease the selected task");
    specforge::SampleLabelingController stale_second(cache_path);
    stale_second.ActivateSource("shared-source", 3);

    specforge::ExclusiveFileLeaseAcquireResult blocked_commit =
        specforge::TryAcquireExclusiveFileLease(
            specforge::SampleLabelingStateCoordinationDirectory(
                cache_path) /
            "cache-commit.lock");
    Require(
        blocked_commit.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::Acquired,
        "deletion retry fixture should hold the cache commit lock");
    Require(
        !deleting.DeleteActiveTask().state_saved,
        "first deletion commit should fail while the lock is held");
    Require(
        stale_second.ActivateTask("deleted-task").issue ==
            specforge::SampleLabelingOperationResult::Issue::
                EditLeaseUnavailable,
        "failed deletion should retain its task lease");

    blocked_commit.lease.Reset();
    const specforge::SampleLabelingOperationResult
        selected_surviving =
            stale_second.ActivateTask(
                "surviving-task");
    Require(
        selected_surviving.accepted &&
            selected_surviving.state_saved,
        "another instance should be able to select an unrelated task before the tombstone retry");
    Require(
        deleting.FlushStateCache(),
        "tombstone retry should also clear the persisted selection and converge");
    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(cache_path);
    const auto source = loaded.cache.sources.find("shared-source");
    Require(
        source != loaded.cache.sources.end() &&
            source->second.active_task_id ==
                std::optional<std::string>{
                    "surviving-task"} &&
            FindTask(
                loaded.cache,
                "shared-source",
                "deleted-task") == nullptr,
        "deletion retry should remove the task without overwriting a newer explicit selection");
    Require(
        stale_second.ActivateTask("deleted-task").issue ==
            specforge::SampleLabelingOperationResult::Issue::
                EditTargetChanged,
        "successful deletion retry should release the deferred lease and reject only the stale task object");
}

void TestOutputRetryRespectsTaskLease()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_retry_lease");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "shared_y.npy";
    {
        specforge::SampleLabelingController seed(cache_path);
        CreateFormalTask(
            seed,
            "shared-source",
            "shared-task",
            output_path,
            5);
    }

    bool fail_output_save = false;
    int persist_call_count = 0;
    specforge::SampleLabelingController retrying(
        cache_path,
        [](const std::filesystem::path& path) {
            return specforge::LoadSampleLabelingStateCache(path);
        },
        [&fail_output_save, &persist_call_count](
            specforge::SampleLabelingTask& task,
            const specforge::SampleLabelResultMetadataSource* source) {
            ++persist_call_count;
            if (fail_output_save) {
                specforge::MarkSampleLabelTaskSaveFailed(
                    task,
                    "retry fixture failure");
                return specforge::SampleLabelOutputPublicationResult{
                    .attempted = true,
                    .published = false,
                    .artifacts_replaced = false,
                    .retryable = true,
                    .message = "retry fixture failure"};
            }
            return specforge::PublishLegacySampleLabelingTaskOutput(
                task,
                source);
        });
    retrying.ActivateSource("shared-source", 3);
    Require(
        retrying.ActivateTask("shared-task").accepted,
        "retrying instance should initially acquire the task");
    fail_output_save = true;
    Require(
        retrying.AssignLabel(0, 5).operation.output_retry_scheduled,
        "failed output should schedule retry");
    const int calls_before_retry = persist_call_count;
    retrying.ClearActiveSource();

    specforge::SampleLabelingController owner(cache_path);
    owner.ActivateSource("shared-source", 3);
    Require(
        owner.View().active_task != nullptr ||
            owner.ActivateTask("shared-task").accepted,
        "other instance should acquire the released task");

    fail_output_save = false;
    const auto retry_deadline = retrying.NextMaintenanceDeadline();
    Require(
        retry_deadline.has_value(),
        "failed output should expose a retry deadline");
    retrying.RunMaintenance(*retry_deadline);
    Require(
        persist_call_count == calls_before_retry,
        "background retry must not call the persister while another instance holds the task lease");
}

void TestCachePatchFailsClosedOnUntrustedLatestFile()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_fail_closed_commit");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::array<std::string, 2> untrusted_documents = {
        "{ definitely not valid JSON\n",
        "{\n"
        "  \"format_kind\": \"specforge.sample_labeling_tasks.cache\",\n"
        "  \"schema_version\": 999,\n"
        "  \"sources\": []\n"
        "}\n"};

    for (const std::string& document : untrusted_documents) {
        WriteTextFile(cache_path, document);
        specforge::SampleLabelingStateCachePatch patch;
        patch.sources["new-source"].metadata =
            specforge::SampleLabelingSourceMetadataPatch{
                .sample_count = 3};
        std::string error;
        Require(
            !specforge::CommitSampleLabelingStateCachePatch(
                cache_path,
                patch,
                &error),
            "cache patch must fail closed on an untrusted latest file");
        Require(
            !error.empty(),
            "fail-closed cache commit should explain the failure");
        Require(
            ReadTextFile(cache_path) == document,
            "fail-closed cache commit must preserve the original bytes");
    }
}

void TestSchemaOneMultipleDraftsRemainPatchable()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_schema_one_drafts");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    WriteTextFile(
        cache_path,
        "{\n"
        "  \"format_kind\": \"specforge.sample_labeling_tasks.cache\",\n"
        "  \"schema_version\": 1,\n"
        "  \"sources\": [\n"
        "    {\n"
        "      \"identity\": \"legacy-source\",\n"
        "      \"sample_count\": 3,\n"
        "      \"active_task_id\": \"\",\n"
        "      \"tasks\": [\n"
        "        { \"task_id\": \"legacy-a\", \"task_name\": \"A\", \"output_path\": null, \"labels\": [], \"values\": [-1, -1, -1] },\n"
        "        { \"task_id\": \"legacy-b\", \"task_name\": \"B\", \"output_path\": null, \"labels\": [], \"values\": [-1, -1, -1] }\n"
        "      ]\n"
        "    }\n"
        "  ]\n"
        "}\n");

    specforge::SampleLabelingStateCacheLoadResult legacy =
        specforge::LoadSampleLabelingStateCache(cache_path);
    Require(
        legacy.warning.empty() &&
            legacy.cache.sources.at("legacy-source")
                    .tasks.size() == 2,
        "schema-1 fixture should load both historical drafts");
    specforge::SampleLabelingTask updated =
        legacy.cache.sources.at("legacy-source").tasks[0];
    updated.values[1] = 42;
    specforge::SampleLabelingStateCachePatch patch;
    patch.sources["legacy-source"].task_upserts.push_back(
        updated);
    std::string error;
    Require(
        specforge::CommitSampleLabelingStateCachePatch(
            cache_path,
            patch,
            &error),
        error.empty()
            ? "schema-1 drafts should remain patchable"
            : error);

    const specforge::SampleLabelingStateCacheLoadResult migrated =
        specforge::LoadSampleLabelingStateCache(cache_path);
    const auto& tasks =
        migrated.cache.sources.at("legacy-source").tasks;
    Require(
        tasks.size() == 2 &&
            FindTask(
                migrated.cache,
                "legacy-source",
                "legacy-a")
                    ->values[1] == 42 &&
            FindTask(
                migrated.cache,
                "legacy-source",
                "legacy-b") != nullptr,
        "patching one legacy draft must preserve the other draft");
}

void TestRecoveryViewClassifiesCurrentAndRecoverableDraft()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_recovery_projection");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";

    specforge::SampleLabelingStateCache cache;
    specforge::SampleLabelingSourceState source;
    source.sample_count = 3;
    source.tasks.push_back(
        specforge::CreateSampleLabelingTask(
            "draft",
            "Draft",
            3));
    source.active_task_id = "draft";
    cache.sources.emplace(
        "projection-source",
        std::move(source));
    Require(
        specforge::SaveSampleLabelingStateCache(
            cache_path,
            cache),
        "recovery projection fixture should be persisted");

    specforge::SampleLabelingController controller(cache_path);
    controller.ActivateSource("projection-source", 3);
    const std::uint64_t current_revision =
        controller.View().revision;
    const specforge::SampleLabelingRecoveryView current =
        controller.RecoveryView();
    Require(
        current.source_identity == "projection-source" &&
            current.revision == current_revision &&
            current.temporary_drafts.size() == 1 &&
            current.temporary_drafts[0].task != nullptr &&
            current.temporary_drafts[0].task->task_id == "draft" &&
            current.temporary_drafts[0].status ==
                specforge::SampleLabelingRecoveryDraftStatus::Current &&
            controller.View().revision == current_revision,
        "recovery projection should expose the selected draft as current without mutation");

    Require(
        controller.DeactivateActiveTask().changed,
        "projection fixture draft should be deactivatable");
    const specforge::SampleLabelingRecoveryView recoverable =
        controller.RecoveryView();
    Require(
        recoverable.temporary_drafts.size() == 1 &&
            recoverable.temporary_drafts[0].status ==
                specforge::SampleLabelingRecoveryDraftStatus::Recoverable,
        "an inactive trusted draft should be exposed as recoverable");
}

void TestRecoveryViewClassifiesConflictingDrafts()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_recovery_conflicts");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    WriteTextFile(
        cache_path,
        "{\n"
        "  \"format_kind\": \"specforge.sample_labeling_tasks.cache\",\n"
        "  \"schema_version\": 1,\n"
        "  \"sources\": [\n"
        "    {\n"
        "      \"identity\": \"conflicting-source\",\n"
        "      \"sample_count\": 3,\n"
        "      \"active_task_id\": \"\",\n"
        "      \"tasks\": [\n"
        "        { \"task_id\": \"draft-a\", \"task_name\": \"A\", \"output_path\": null, \"labels\": [], \"values\": [-1, -1, -1] },\n"
        "        { \"task_id\": \"draft-b\", \"task_name\": \"B\", \"output_path\": null, \"labels\": [], \"values\": [-1, -1, -1] }\n"
        "      ]\n"
        "    }\n"
        "  ]\n"
        "}\n");

    specforge::SampleLabelingController controller(cache_path);
    controller.ActivateSource("conflicting-source", 3);
    const specforge::SampleLabelingRecoveryView recovery =
        controller.RecoveryView();
    Require(
        recovery.temporary_drafts.size() == 2,
        "recovery projection should retain all legacy temporary drafts");
    for (const specforge::SampleLabelingRecoveryDraftView& draft :
         recovery.temporary_drafts) {
        Require(
            draft.task != nullptr &&
                draft.status ==
                    specforge::SampleLabelingRecoveryDraftStatus::Conflicting,
            "multiple temporary drafts should be exposed as conflicting");
    }
}

void TestRecoveryViewClassifiesUntrustedDraftAsStale()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_recovery_stale");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    WriteTextFile(
        cache_path,
        "{\n"
        "  \"format_kind\": \"specforge.sample_labeling_tasks.cache\",\n"
        "  \"schema_version\": 2,\n"
        "  \"sources\": [{\n"
        "    \"identity\": \"stale-source\",\n"
        "    \"sample_count\": 3,\n"
        "    \"active_task_id\": 42,\n"
        "    \"tasks\": [{ \"task_id\": \"draft\", \"task_name\": \"Draft\", \"output_path\": null, \"labels\": [], \"values\": [-1, -1, -1] }]\n"
        "  }]\n"
        "}\n");

    specforge::SampleLabelingController controller(cache_path);
    controller.ActivateSource("stale-source", 3);
    const specforge::SampleLabelingRecoveryView recovery =
        controller.RecoveryView();
    Require(
        !controller.state_load_warning().empty() &&
            recovery.temporary_drafts.size() == 1 &&
            recovery.temporary_drafts[0].task != nullptr &&
            recovery.temporary_drafts[0].status ==
                specforge::SampleLabelingRecoveryDraftStatus::Stale,
        "a salvaged draft from an untrusted cache should be exposed as stale");
}

void TestRecoveryViewRetainsUntrustedPreparedSnapshotTrust()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_prepared_recovery_stale");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    Require(
        specforge::SaveSampleLabelingStateCache(
            cache_path,
            specforge::SampleLabelingStateCache{}),
        "prepared recovery fixture should start from a healthy cache");

    specforge::SampleLabelingController controller(cache_path);
    controller.ActivateSource("prepared-source", 3);
    Require(
        controller.state_load_warning().empty(),
        "prepared recovery fixture should have a healthy initial load");

    auto stale_snapshot =
        std::make_shared<specforge::SampleLabelingStateCacheLoadResult>();
    stale_snapshot->issue_kind =
        specforge::SampleLabelingStateCacheLoadIssueKind::InvalidDocument;
    stale_snapshot->warning = "salvaged prepared snapshot";
    specforge::SampleLabelingSourceState stale_source;
    stale_source.sample_count = 3;
    stale_source.tasks.push_back(
        specforge::CreateSampleLabelingTask(
            "rescued-draft",
            "Rescued draft",
            3));
    stale_snapshot->cache.sources.emplace(
        "prepared-source",
        stale_source);

    const specforge::SourceCollectionIdentity identity{
        "prepared-source",
        "prepared",
        "prepared-source-fingerprint",
        "prepared-context-fingerprint",
        3};
    (void)controller.AdoptPreparedStateCache(stale_snapshot);
    (void)controller.ActivatePreparedSource(
        identity,
        stale_snapshot->cache.sources.at("prepared-source"));
    const specforge::SampleLabelingRecoveryView stale_recovery =
        controller.RecoveryView();
    Require(
        controller.state_load_warning().empty() &&
            stale_recovery.temporary_drafts.size() == 1 &&
            stale_recovery.temporary_drafts[0].status ==
                specforge::SampleLabelingRecoveryDraftStatus::Stale,
        "a non-first untrusted prepared snapshot must stay stale without resurrecting the global persistence warning");

    controller.ActivateSource(identity);
    Require(
        controller.FlushStateCache(),
        "metadata-only source synchronization should succeed");
    const specforge::SampleLabelingRecoveryView after_metadata_sync =
        controller.RecoveryView();
    Require(
        after_metadata_sync.temporary_drafts.size() == 1 &&
            after_metadata_sync.temporary_drafts[0].status ==
                specforge::SampleLabelingRecoveryDraftStatus::Stale,
        "a metadata-only source patch must not trust an unpersisted salvaged draft");

    auto trusted_snapshot =
        std::make_shared<specforge::SampleLabelingStateCacheLoadResult>(
            *stale_snapshot);
    trusted_snapshot->issue_kind =
        specforge::SampleLabelingStateCacheLoadIssueKind::None;
    trusted_snapshot->warning.clear();
    (void)controller.AdoptPreparedStateCache(trusted_snapshot);
    (void)controller.ActivatePreparedSource(
        identity,
        trusted_snapshot->cache.sources.at("prepared-source"));
    const specforge::SampleLabelingRecoveryView trusted_recovery =
        controller.RecoveryView();
    Require(
        controller.state_load_warning().empty() &&
            trusted_recovery.temporary_drafts.size() == 1 &&
            trusted_recovery.temporary_drafts[0].status ==
                specforge::SampleLabelingRecoveryDraftStatus::Recoverable,
        "a trusted prepared snapshot should clear only that source's recovery staleness");
}

void TestRecoveryViewTrustedPreparedSnapshotClearsStalenessDespiteWarning()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_prepared_recovery_warning");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    Require(
        specforge::SaveSampleLabelingStateCache(
            cache_path,
            specforge::SampleLabelingStateCache{}),
        "prepared warning fixture should start from a healthy cache");

    const specforge::SourceCollectionIdentity identity{
        "prepared-warning-source",
        "prepared",
        "prepared-warning-source-fingerprint",
        "prepared-warning-context-fingerprint",
        3};
    auto stale_snapshot =
        std::make_shared<specforge::SampleLabelingStateCacheLoadResult>();
    stale_snapshot->issue_kind =
        specforge::SampleLabelingStateCacheLoadIssueKind::InvalidDocument;
    stale_snapshot->warning = "first prepared snapshot was salvaged";
    specforge::SampleLabelingSourceState stale_source;
    stale_source.sample_count = 3;
    stale_source.tasks.push_back(
        specforge::CreateSampleLabelingTask(
            "rescued-draft",
            "Rescued draft",
            3));
    stale_snapshot->cache.sources.emplace(
        identity.id,
        stale_source);

    specforge::SampleLabelingController controller(cache_path);
    (void)controller.AdoptPreparedStateCache(stale_snapshot);
    (void)controller.ActivatePreparedSource(
        identity,
        stale_snapshot->cache.sources.at(identity.id));
    Require(
        !controller.state_load_warning().empty() &&
            controller.RecoveryView().temporary_drafts.size() == 1 &&
            controller.RecoveryView().temporary_drafts[0].status ==
                specforge::SampleLabelingRecoveryDraftStatus::Stale,
        "the first untrusted prepared snapshot should set both source staleness and the global warning");

    auto trusted_snapshot =
        std::make_shared<specforge::SampleLabelingStateCacheLoadResult>(
            *stale_snapshot);
    trusted_snapshot->issue_kind =
        specforge::SampleLabelingStateCacheLoadIssueKind::None;
    trusted_snapshot->warning.clear();
    (void)controller.AdoptPreparedStateCache(trusted_snapshot);
    (void)controller.ActivatePreparedSource(
        identity,
        trusted_snapshot->cache.sources.at(identity.id));
    const specforge::SampleLabelingRecoveryView recovery =
        controller.RecoveryView();
    Require(
        !controller.state_load_warning().empty() &&
            recovery.temporary_drafts.size() == 1 &&
            recovery.temporary_drafts[0].status ==
                specforge::SampleLabelingRecoveryDraftStatus::Recoverable,
        "a later trusted prepared snapshot should clear source-local staleness without clearing the global warning");
}

void TestRecoveryViewDoesNotStaleProtectedLocalTask()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_prepared_recovery_protected");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const specforge::SourceCollectionIdentity identity{
        "protected-recovery-source",
        "protected",
        "protected-source-fingerprint",
        "protected-context-fingerprint",
        3};
    {
        specforge::SampleLabelingController seed(cache_path);
        seed.ActivateSource(identity.id, identity.spectrum_count);
        Require(
            seed.CreateTask(
                        "protected-draft",
                        "Protected draft")
                .accepted &&
            seed.DeactivateActiveTask().state_saved,
            "protected recovery fixture should persist its local draft");
    }

    specforge::SampleLabelingController controller(cache_path);
    controller.ActivateSource(identity);
    Require(
        controller.ActivateTask("protected-draft").accepted,
        "protected recovery fixture should hold the local task lease");

    auto stale_snapshot =
        std::make_shared<specforge::SampleLabelingStateCacheLoadResult>();
    stale_snapshot->issue_kind =
        specforge::SampleLabelingStateCacheLoadIssueKind::InvalidDocument;
    stale_snapshot->warning = "prepared source was salvaged";
    specforge::SampleLabelingSourceState stale_source;
    stale_source.sample_count = identity.spectrum_count;
    stale_source.tasks.push_back(
        specforge::CreateSampleLabelingTask(
            "rescued-draft",
            "Rescued draft",
            identity.spectrum_count));
    stale_snapshot->cache.sources.emplace(
        identity.id,
        stale_source);

    (void)controller.AdoptPreparedStateCache(stale_snapshot);
    (void)controller.ActivatePreparedSource(
        identity,
        stale_snapshot->cache.sources.at(identity.id));
    const specforge::SampleLabelingRecoveryView recovery =
        controller.RecoveryView();
    const auto find_status =
        [&recovery](std::string_view task_id)
        -> std::optional<specforge::SampleLabelingRecoveryDraftStatus> {
        const auto draft = std::find_if(
            recovery.temporary_drafts.begin(),
            recovery.temporary_drafts.end(),
            [task_id](
                const specforge::SampleLabelingRecoveryDraftView& candidate) {
                return candidate.task != nullptr &&
                    candidate.task->task_id == task_id;
            });
        if (draft == recovery.temporary_drafts.end()) {
            return std::nullopt;
        }
        return draft->status;
    };
    Require(
        find_status("protected-draft") ==
                specforge::SampleLabelingRecoveryDraftStatus::Current &&
            find_status("rescued-draft") ==
                specforge::SampleLabelingRecoveryDraftStatus::Stale,
        "an untrusted prepared source must not stale a protected local task while marking its rescued task stale");
}

void TestRecoveryViewVerifiedRestoreBeatsStalePreparedProvenance()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_verified_restore_recovery");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    {
        specforge::SampleLabelingController seed(cache_path);
        seed.ActivateSource("verified-restore-source", 3);
        Require(
            seed.CreateTask(
                        "verified-draft",
                        "Verified draft")
                .accepted &&
            seed.DeactivateActiveTask().state_saved,
            "verified restore fixture should persist an inactive draft");
    }

    const specforge::SourceCollectionIdentity identity{
        "verified-restore-source",
        "verified",
        "verified-source-fingerprint",
        "verified-context-fingerprint",
        3};
    auto stale_snapshot =
        std::make_shared<specforge::SampleLabelingStateCacheLoadResult>();
    stale_snapshot->issue_kind =
        specforge::SampleLabelingStateCacheLoadIssueKind::InvalidDocument;
    stale_snapshot->warning =
        "prepared snapshot was salvaged before restore";
    specforge::SampleLabelingSourceState stale_source;
    stale_source.sample_count = identity.spectrum_count;
    stale_source.active_task_id = "verified-draft";
    stale_source.tasks.push_back(
        specforge::CreateSampleLabelingTask(
            "verified-draft",
            "Verified draft",
            identity.spectrum_count));
    stale_snapshot->cache.sources.emplace(
        identity.id,
        stale_source);

    specforge::SampleLabelingController controller(cache_path);
    (void)controller.AdoptPreparedStateCache(stale_snapshot);
    (void)controller.ActivatePreparedSource(
        identity,
        stale_snapshot->cache.sources.at(identity.id));
    const specforge::SampleLabelingRecoveryView recovery =
        controller.RecoveryView();
    Require(
        !controller.state_load_warning().empty() &&
            recovery.temporary_drafts.size() == 1 &&
            recovery.temporary_drafts[0].task != nullptr &&
            recovery.temporary_drafts[0].task->task_id ==
                "verified-draft" &&
            recovery.temporary_drafts[0].status ==
                specforge::SampleLabelingRecoveryDraftStatus::Current,
        "a trusted restore must clear stale prepared provenance after it validates and adopts the current draft");
}

void TestHistoricalDuplicateOutputsRemainPatchable()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_legacy_duplicate_outputs");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path shared_output =
        directory / "shared_y.npy";

    specforge::SampleLabelingStateCache legacy;
    for (const std::string source_identity :
         {"legacy-source-a", "legacy-source-b"}) {
        specforge::SampleLabelingSourceState source;
        source.sample_count = 3;
        specforge::SampleLabelingTask task =
            specforge::CreateSampleLabelingTask(
                source_identity + "-task",
                source_identity,
                3);
        task.output_path = shared_output;
        task.output_format =
            specforge::SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
        specforge::MarkSampleLabelTaskPersisted(
            task,
            specforge::SampleLabelSaveStateKind::
                AutosavedToOutput);
        source.tasks.push_back(std::move(task));
        legacy.sources.emplace(
            source_identity,
            std::move(source));
    }
    Require(
        specforge::SaveSampleLabelingStateCache(
            cache_path,
            legacy),
        "duplicate-output fixture should write a historical schema-2 cache");

    specforge::SampleLabelingStateCachePatch unrelated;
    unrelated.sources["new-source"].metadata =
        specforge::SampleLabelingSourceMetadataPatch{
            .sample_count = 3,
            .source_name = "New source"};
    std::string error;
    Require(
        specforge::CommitSampleLabelingStateCachePatch(
            cache_path,
            unrelated,
            &error),
        error.empty()
            ? "historical duplicate outputs should not block an unrelated patch"
            : error);
    const specforge::SampleLabelingStateCacheLoadResult preserved =
        specforge::LoadSampleLabelingStateCache(cache_path);
    Require(
        preserved.cache.sources.size() == 3 &&
            FindTask(
                preserved.cache,
                "legacy-source-a",
                "legacy-source-a-task") != nullptr &&
            FindTask(
                preserved.cache,
                "legacy-source-b",
                "legacy-source-b-task") != nullptr,
        "unrelated patch should preserve both historical duplicate-output owners");

    specforge::SampleLabelingTask introduced =
        specforge::CreateSampleLabelingTask(
            "new-conflict",
            "New conflict",
            3);
    introduced.output_path = shared_output;
    introduced.output_format =
        specforge::SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
    specforge::SampleLabelingStateCachePatch conflicting;
    conflicting.sources["new-source"].metadata =
        specforge::SampleLabelingSourceMetadataPatch{
            .sample_count = 3};
    conflicting.sources["new-source"].task_upserts.push_back(
        std::move(introduced));
    const std::string before_conflict =
        ReadTextFile(cache_path);
    error.clear();
    Require(
        !specforge::CommitSampleLabelingStateCachePatch(
            cache_path,
            conflicting,
            &error) &&
            !error.empty(),
        "a patch must still reject a newly introduced duplicate output owner");
    Require(
        ReadTextFile(cache_path) == before_conflict,
        "rejected output conflict must preserve the latest cache bytes");
}

void TestStructurallyDamagedCacheFailsClosedAfterSalvage()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_partial_structure_damage");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::array<std::string, 2> damaged_documents = {
        "{\n"
        "  \"format_kind\": \"specforge.sample_labeling_tasks.cache\",\n"
        "  \"schema_version\": 2,\n"
        "  \"sources\": [42, { \"identity\": \"salvaged\", \"sample_count\": 3, \"tasks\": [] }]\n"
        "}\n",
        "{\n"
        "  \"format_kind\": \"specforge.sample_labeling_tasks.cache\",\n"
        "  \"schema_version\": 2,\n"
        "  \"sources\": [{ \"identity\": \"damaged-tasks\", \"sample_count\": 3, \"tasks\": {} }]\n"
        "}\n"};

    for (const std::string& document : damaged_documents) {
        WriteTextFile(cache_path, document);
        const specforge::SampleLabelingStateCacheLoadResult salvaged =
            specforge::LoadSampleLabelingStateCache(cache_path);
        Require(
            salvaged.issue_kind ==
                specforge::SampleLabelingStateCacheLoadIssueKind::
                    InvalidDocument &&
                !salvaged.cache.sources.empty(),
            "read-only load should salvage valid records while marking omitted structure untrusted");

        specforge::SampleLabelingStateCachePatch patch;
        patch.sources["unrelated"].metadata =
            specforge::SampleLabelingSourceMetadataPatch{
                .sample_count = 3};
        std::string error;
        Require(
            !specforge::CommitSampleLabelingStateCachePatch(
                cache_path,
                patch,
                &error),
            "commit must fail closed after partial structural salvage");
        Require(
            ReadTextFile(cache_path) == document,
            "fail-closed structural salvage must preserve the original bytes");
    }
}

void TestMalformedTaskFieldsFailClosedAfterSalvage()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_malformed_task_fields");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::array<std::string, 3> damaged_documents = {
        "{\n"
        "  \"format_kind\": \"specforge.sample_labeling_tasks.cache\",\n"
        "  \"schema_version\": 2,\n"
        "  \"sources\": [{\n"
        "    \"identity\": \"damaged-source\",\n"
        "    \"sample_count\": 3,\n"
        "    \"tasks\": [{ \"task_id\": \"damaged\", \"task_name\": \"Damaged\", \"output_path\": null, \"labels\": [], \"values\": [5, 7] }]\n"
        "  }]\n"
        "}\n",
        "{\n"
        "  \"format_kind\": \"specforge.sample_labeling_tasks.cache\",\n"
        "  \"schema_version\": 2,\n"
        "  \"sources\": [{\n"
        "    \"identity\": \"damaged-source\",\n"
        "    \"sample_count\": 3,\n"
        "    \"tasks\": [{ \"task_id\": \"damaged\", \"task_name\": \"Damaged\", \"output_path\": null, \"labels\": [], \"values\": [5, \"bad\", -1] }]\n"
        "  }]\n"
        "}\n",
        "{\n"
        "  \"format_kind\": \"specforge.sample_labeling_tasks.cache\",\n"
        "  \"schema_version\": 2,\n"
        "  \"sources\": [{\n"
        "    \"identity\": \"damaged-source\",\n"
        "    \"sample_count\": 3,\n"
        "    \"tasks\": [{ \"task_id\": \"damaged\", \"task_name\": \"Damaged\", \"output_path\": null, \"labels\": [], \"values\": [-1, -1, -1], \"save_state\": 42, \"save_message\": [], \"save_message_kind\": \"unknown-kind\" }]\n"
        "  }]\n"
        "}\n"};

    for (const std::string& document : damaged_documents) {
        WriteTextFile(cache_path, document);
        const specforge::SampleLabelingStateCacheLoadResult salvaged =
            specforge::LoadSampleLabelingStateCache(cache_path);
        Require(
            salvaged.issue_kind ==
                specforge::SampleLabelingStateCacheLoadIssueKind::
                    InvalidDocument &&
                FindTask(
                    salvaged.cache,
                    "damaged-source",
                    "damaged") != nullptr,
            "read-only load should salvage a malformed task while marking it untrusted");

        specforge::SampleLabelingStateCachePatch patch;
        patch.sources["unrelated"].metadata =
            specforge::SampleLabelingSourceMetadataPatch{
                .sample_count = 3};
        std::string error;
        Require(
            !specforge::CommitSampleLabelingStateCachePatch(
                cache_path,
                patch,
                &error),
            "an unrelated patch must fail closed on malformed present task fields");
        Require(
            ReadTextFile(cache_path) == document,
            "fail-closed malformed-task salvage must preserve the original bytes");
    }
}

void TestMalformedSourceFieldsFailClosedAfterSalvage()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_malformed_source_fields");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::string document =
        "{\n"
        "  \"format_kind\": \"specforge.sample_labeling_tasks.cache\",\n"
        "  \"schema_version\": 2,\n"
        "  \"sources\": [{\n"
        "    \"identity\": \"damaged-source\",\n"
        "    \"sample_count\": 3,\n"
        "    \"source_name\": \"Source\",\n"
        "    \"source_fingerprint\": \"source-fingerprint\",\n"
        "    \"context_fingerprint\": \"context-fingerprint\",\n"
        "    \"active_task_id\": 42,\n"
        "    \"tasks\": [{ \"task_id\": \"draft\", \"task_name\": \"Draft\", \"output_path\": null, \"labels\": [], \"values\": [-1, -1, -1] }]\n"
        "  }]\n"
        "}\n";
    WriteTextFile(cache_path, document);
    const specforge::SampleLabelingStateCacheLoadResult salvaged =
        specforge::LoadSampleLabelingStateCache(cache_path);
    Require(
        salvaged.issue_kind ==
                specforge::SampleLabelingStateCacheLoadIssueKind::
                    InvalidDocument &&
            FindTask(
                salvaged.cache,
                "damaged-source",
                "draft") != nullptr,
        "read-only load should salvage valid tasks while marking malformed source fields untrusted");

    specforge::SampleLabelingStateCachePatch patch;
    patch.sources["unrelated"].metadata =
        specforge::SampleLabelingSourceMetadataPatch{
            .sample_count = 3};
    std::string error;
    Require(
        !specforge::CommitSampleLabelingStateCachePatch(
            cache_path,
            patch,
            &error),
        "commit must fail closed after malformed source-field salvage");
    Require(
        ReadTextFile(cache_path) == document,
        "fail-closed source-field salvage must preserve the original bytes");
}

void TestMissingRequiredTaskFieldsFailClosed()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_missing_required_fields");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::string document =
        "{\n"
        "  \"format_kind\": \"specforge.sample_labeling_tasks.cache\",\n"
        "  \"schema_version\": 2,\n"
        "  \"sources\": [{\n"
        "    \"identity\": \"missing-values\",\n"
        "    \"sample_count\": 3,\n"
        "    \"tasks\": [{ \"task_id\": \"draft\", \"task_name\": \"Draft\", \"output_path\": null, \"labels\": [] }]\n"
        "  }]\n"
        "}\n";
    WriteTextFile(cache_path, document);
    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(cache_path);
    Require(
        loaded.issue_kind ==
                specforge::SampleLabelingStateCacheLoadIssueKind::
                    InvalidDocument &&
            FindTask(loaded.cache, "missing-values", "draft") != nullptr,
        "temporary task missing values must be salvaged as untrusted");
    specforge::SampleLabelingStateCachePatch patch;
    patch.sources["unrelated"].metadata =
        specforge::SampleLabelingSourceMetadataPatch{.sample_count = 3};
    std::string error;
    Require(
        !specforge::CommitSampleLabelingStateCachePatch(
            cache_path,
            patch,
            &error),
        "missing temporary values must block unrelated cache commits");
    Require(
        ReadTextFile(cache_path) == document,
        "missing temporary values must preserve original cache bytes");
}

void TestMissingSourceTasksFieldFailsClosed()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_missing_tasks_field");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::string document =
        "{\n"
        "  \"format_kind\": \"specforge.sample_labeling_tasks.cache\",\n"
        "  \"schema_version\": 2,\n"
        "  \"sources\": [{\n"
        "    \"identity\": \"missing-tasks\",\n"
        "    \"sample_count\": 3\n"
        "  }]\n"
        "}\n";
    WriteTextFile(cache_path, document);
    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(cache_path);
    Require(
        loaded.issue_kind ==
            specforge::SampleLabelingStateCacheLoadIssueKind::
                InvalidDocument,
        "source missing tasks must be marked untrusted");
    specforge::SampleLabelingStateCachePatch patch;
    patch.sources["unrelated"].metadata =
        specforge::SampleLabelingSourceMetadataPatch{.sample_count = 3};
    std::string error;
    Require(
        !specforge::CommitSampleLabelingStateCachePatch(
            cache_path,
            patch,
            &error),
        "source missing tasks must block unrelated cache commits");
    Require(
        ReadTextFile(cache_path) == document,
        "source missing tasks must preserve original cache bytes");
}

void TestMissingTaskRefreshRemovesGhostAndRestartsDraft()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_missing_task_refresh");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    {
        specforge::SampleLabelingController seed(cache_path);
        seed.ActivateSource("shared-source", 3);
        Require(
            seed.CreateTask(
                        "temporary-labeling-task",
                        "Old draft")
                .accepted,
            "missing-task fixture should create its draft");
        Require(
            seed.DeactivateActiveTask().state_saved,
            "missing-task fixture should persist an inactive draft");
    }

    specforge::SampleLabelingController stale(cache_path);
    stale.ActivateSource("shared-source", 3);
    const std::uint64_t generation_before =
        stale.active_source_tasks_generation();
    const specforge::SampleLabelingRecoveryView before_conflict =
        stale.RecoveryView();
    Require(
        before_conflict.temporary_drafts.size() == 1 &&
            before_conflict.temporary_drafts[0].status ==
                specforge::SampleLabelingRecoveryDraftStatus::Recoverable,
        "missing-task fixture should start with a recoverable draft");
    {
        specforge::SampleLabelingController deleting(cache_path);
        deleting.ActivateSource("shared-source", 3);
        Require(
            deleting.ActivateTask("temporary-labeling-task").accepted,
            "deleting instance should acquire the old draft");
        const specforge::SampleLabelingOperationResult conflict =
            stale.ActivateTask("temporary-labeling-task");
        Require(
            !conflict.accepted &&
                conflict.issue ==
                    specforge::SampleLabelingOperationResult::Issue::
                        EditLeaseUnavailable &&
                stale.RecoveryView().temporary_drafts.size() == 1 &&
                stale.RecoveryView().temporary_drafts[0].status ==
                    specforge::SampleLabelingRecoveryDraftStatus::Conflicting,
            "missing-task fixture should record the old draft lease conflict");
        Require(
            deleting.DeleteActiveTask().state_saved,
            "deleting instance should commit the draft tombstone");
    }

    stale.ActivateSource("shared-source", 3);
    Require(
        ActiveSourceTasks(stale) != nullptr &&
            ActiveSourceTasks(stale)->empty() &&
            stale.RecoveryView().temporary_drafts.empty(),
        "a trusted refresh should remove the deleted draft before its id is reused");
    {
        specforge::SampleLabelingController recreated(cache_path);
        recreated.ActivateSource("shared-source", 3);
        Require(
            recreated.StartOrResumeTemporaryTask().accepted &&
                recreated.DeactivateActiveTask().state_saved,
            "another instance should be able to recreate the deleted draft id");
    }
    stale.ActivateSource("shared-source", 3);
    const specforge::SampleLabelingRecoveryView after_recreation =
        stale.RecoveryView();
    Require(
        after_recreation.temporary_drafts.size() == 1 &&
            after_recreation.temporary_drafts[0].task != nullptr &&
            after_recreation.temporary_drafts[0].task->task_id ==
                "temporary-labeling-task" &&
            after_recreation.temporary_drafts[0].status ==
                specforge::SampleLabelingRecoveryDraftStatus::Recoverable,
        "a recreated task id must not inherit the deleted task's lease conflict marker");

    const specforge::SampleLabelingOperationResult restarted =
        stale.StartOrResumeTemporaryTask();
    Require(
        restarted.accepted &&
            restarted.state_saved &&
            ActiveTask(stale) != nullptr &&
            ActiveTask(stale)->task_id ==
                "temporary-labeling-task" &&
            !ActiveTask(stale)->output_path &&
            stale.RecoveryView().temporary_drafts.size() == 1 &&
            stale.RecoveryView().temporary_drafts[0].status ==
                specforge::SampleLabelingRecoveryDraftStatus::Current,
        "the stale instance should remove the missing draft and create a fresh one in the same operation");
    Require(
        stale.active_source_tasks_generation() >
            generation_before &&
            ActiveSourceTasks(stale) != nullptr &&
            ActiveSourceTasks(stale)->size() == 1,
        "confirmed missing refresh should remove the ghost task and advance task generation");
}

void TestRecoveryViewExpiresLeaseConflictAfterTrustedRefresh()
{
    const auto seed_cache =
        [](const std::filesystem::path& cache_path,
           std::string_view source_identity) {
        specforge::SampleLabelingController seed(cache_path);
        seed.ActivateSource(std::string(source_identity), 3);
        Require(
            seed.CreateTask(
                        "temporary-labeling-task",
                        "Draft")
                .accepted &&
            seed.DeactivateActiveTask().state_saved,
            "lease conflict refresh fixture should persist an inactive draft");
    };
    const auto require_recoverable_after_refresh =
        [](specforge::SampleLabelingController& controller,
           std::string_view message) {
        controller.ActivateSource("shared-source", 3);
        const specforge::SampleLabelingRecoveryView recovery =
            controller.RecoveryView();
        Require(
            recovery.temporary_drafts.size() == 1 &&
                recovery.temporary_drafts[0].status ==
                    specforge::SampleLabelingRecoveryDraftStatus::Recoverable,
            message);
    };

    {
        const std::filesystem::path directory =
            FreshTestDirectory(
                "specforge_labeling_conflict_release_refresh");
        const std::filesystem::path cache_path =
            directory / "sample-labeling-tasks.json";
        seed_cache(cache_path, "shared-source");
        specforge::SampleLabelingController blocked(cache_path);
        blocked.ActivateSource("shared-source", 3);
        {
            specforge::SampleLabelingController holder(cache_path);
            holder.ActivateSource("shared-source", 3);
            Require(
                holder.ActivateTask(
                    "temporary-labeling-task")
                    .accepted,
                "release refresh fixture should acquire the draft lease");
            Require(
                blocked.ActivateTask(
                            "temporary-labeling-task")
                        .issue ==
                    specforge::SampleLabelingOperationResult::Issue::
                        EditLeaseUnavailable,
                "release refresh fixture should record the lease conflict");
            Require(
                holder.DeactivateActiveTask().state_saved,
                "release refresh fixture should release the draft lease");
        }
        require_recoverable_after_refresh(
            blocked,
            "a trusted refresh after lease release should expire the historical conflict");
    }

    {
        const std::filesystem::path directory =
            FreshTestDirectory(
                "specforge_labeling_conflict_still_held_refresh");
        const std::filesystem::path cache_path =
            directory / "sample-labeling-tasks.json";
        seed_cache(cache_path, "shared-source");
        specforge::SampleLabelingController blocked(cache_path);
        blocked.ActivateSource("shared-source", 3);
        specforge::ExclusiveFileLeaseAcquireResult holder =
            specforge::TryAcquireExclusiveFileLease(
                LabelingTargetLeasePath(
                    cache_path,
                    "task\nshared-source\ntemporary-labeling-task"));
        Require(
            holder.status ==
                specforge::ExclusiveFileLeaseAcquireStatus::Acquired,
            "still-held refresh fixture should hold the task lease directly");
        Require(
            blocked.ActivateTask(
                        "temporary-labeling-task")
                    .issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable,
            "still-held refresh fixture should observe the lease conflict");
        require_recoverable_after_refresh(
            blocked,
            "a trusted refresh should expire the observation while the holder still owns the lease");
        Require(
            blocked.ActivateTask(
                        "temporary-labeling-task")
                    .issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable,
            "the next activation should still report the live lease conflict");
    }

    {
        const std::filesystem::path directory =
            FreshTestDirectory(
                "specforge_labeling_conflict_aba_refresh");
        const std::filesystem::path cache_path =
            directory / "sample-labeling-tasks.json";
        seed_cache(cache_path, "shared-source");
        specforge::SampleLabelingController blocked(cache_path);
        blocked.ActivateSource("shared-source", 3);
        {
            specforge::SampleLabelingController replacing(cache_path);
            replacing.ActivateSource("shared-source", 3);
            Require(
                replacing.ActivateTask(
                    "temporary-labeling-task")
                    .accepted,
                "ABA refresh fixture should acquire the old draft lease");
            Require(
                blocked.ActivateTask(
                            "temporary-labeling-task")
                        .issue ==
                    specforge::SampleLabelingOperationResult::Issue::
                        EditLeaseUnavailable,
                "ABA refresh fixture should record the old lease conflict");
            Require(
                replacing.DeleteActiveTask().state_saved &&
                    replacing.StartOrResumeTemporaryTask().accepted &&
                    replacing.DeactivateActiveTask().state_saved,
                "ABA refresh fixture should delete and recreate the stable draft id before refresh");
        }
        require_recoverable_after_refresh(
            blocked,
            "a trusted refresh must expire a conflict when the same id was rebuilt without a missing window");
    }
}

void TestTemporarySlotLeaseSerializesDifferentTaskIds()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_temporary_slot_barrier");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    std::array<specforge::SampleLabelingOperationResult, 2>
        results;
    std::barrier synchronization(2);
    std::array<std::thread, 2> workers;
    for (std::size_t index = 0;
         index < workers.size();
         ++index) {
        workers[index] = std::thread(
            [&, index]() {
                specforge::SampleLabelingController controller(
                    cache_path);
                controller.ActivateSource(
                    "shared-source",
                    3);
                synchronization.arrive_and_wait();
                results[index] = controller.CreateTask(
                    index == 0
                        ? "temporary-labeling-task"
                        : "temporary-labeling-task-2",
                    "Draft");
                synchronization.arrive_and_wait();
            });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    const std::size_t accepted_count =
        static_cast<std::size_t>(results[0].accepted) +
        static_cast<std::size_t>(results[1].accepted);
    const std::size_t unavailable_count =
        static_cast<std::size_t>(
            results[0].issue ==
            specforge::SampleLabelingOperationResult::Issue::
                EditLeaseUnavailable) +
        static_cast<std::size_t>(
            results[1].issue ==
            specforge::SampleLabelingOperationResult::Issue::
                EditLeaseUnavailable);
    Require(
        accepted_count == 1 &&
            unavailable_count == 1,
        "same-source draft creation must be serialized before different task ids can diverge");
    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(cache_path);
    Require(
        loaded.issue_kind ==
                specforge::SampleLabelingStateCacheLoadIssueKind::
                    None &&
            loaded.cache.sources.at("shared-source")
                    .tasks.size() == 1,
        "temporary-slot serialization should leave exactly one persisted draft");
}

void TestActiveTemporaryDraftCanRecoverAnotherDraftWithSharedSlot()
{
    const auto write_fixture = [](
                                    const std::filesystem::path& cache_path) {
        specforge::SampleLabelingStateCache cache;
        specforge::SampleLabelingSourceState source;
        source.sample_count = 3;
        source.active_task_id = "draft-a";
        specforge::SampleLabelingTask active =
            specforge::CreateSampleLabelingTask(
                "draft-a",
                "Draft A",
                3);
        specforge::SampleLabelingTask paused =
            specforge::CreateSampleLabelingTask(
                "draft-b",
                "Draft B",
                3);
        specforge::MarkSampleLabelTaskPersisted(
            active,
            specforge::SampleLabelSaveStateKind::InternalDraftOnly);
        specforge::MarkSampleLabelTaskPersisted(
            paused,
            specforge::SampleLabelSaveStateKind::InternalDraftOnly);
        source.tasks = {
            std::move(active),
            std::move(paused)};
        cache.sources.emplace(
            "shared-source",
            std::move(source));
        Require(
            specforge::SaveSampleLabelingStateCache(
                cache_path,
                cache),
            "shared temporary-slot fixture should save two drafts");
    };

    {
        const std::filesystem::path directory =
            FreshTestDirectory(
                "specforge_labeling_active_temporary_recover_shared_slot");
        const std::filesystem::path cache_path =
            directory / "sample-labeling-tasks.json";
        write_fixture(cache_path);
        specforge::SampleLabelingController controller(cache_path);
        controller.ActivateSource("shared-source", 3);
        Require(
            ActiveTask(controller) != nullptr &&
                ActiveTask(controller)->task_id == "draft-a",
            "shared temporary-slot recovery fixture should activate draft A");
        const specforge::SampleLabelingOperationResult recovered =
            controller.RecoverTemporaryTask(
                "shared-source",
                "draft-b");
        Require(
            recovered.accepted &&
                ActiveTask(controller) != nullptr &&
                ActiveTask(controller)->task_id == "draft-b",
            "recovering paused draft B should reuse active draft A's temporary slot lease");
    }

    {
        const std::filesystem::path directory =
            FreshTestDirectory(
                "specforge_labeling_active_temporary_delete_shared_slot");
        const std::filesystem::path cache_path =
            directory / "sample-labeling-tasks.json";
        write_fixture(cache_path);
        specforge::SampleLabelingController controller(cache_path);
        controller.ActivateSource("shared-source", 3);
        Require(
            ActiveTask(controller) != nullptr &&
                ActiveTask(controller)->task_id == "draft-a",
            "shared temporary-slot deletion fixture should activate draft A");

        const specforge::SampleLabelingOperationResult deleted =
            controller.DeleteTemporaryTask(
                "shared-source",
                "draft-b");
        const std::vector<specforge::SampleLabelingTask>* tasks =
            ActiveSourceTasks(controller);
        Require(
            deleted.accepted &&
                deleted.state_saved &&
                ActiveTask(controller) != nullptr &&
                ActiveTask(controller)->task_id == "draft-a" &&
                tasks != nullptr &&
                tasks->size() == 1 &&
                tasks->front().task_id == "draft-a",
            "deleting paused draft B should borrow A's slot and leave A active");
    }

    {
        const std::filesystem::path directory =
            FreshTestDirectory(
                "specforge_labeling_active_temporary_recover_shared_slot_pending");
        const std::filesystem::path cache_path =
            directory / "sample-labeling-tasks.json";
        write_fixture(cache_path);
        specforge::SampleLabelingController recovering(cache_path);
        specforge::SampleLabelingController observer(cache_path);
        recovering.ActivateSource("shared-source", 3);
        observer.ActivateSource("shared-source", 3);
        Require(
            ActiveTask(recovering) != nullptr &&
                ActiveTask(recovering)->task_id == "draft-a",
            "pending shared-slot recovery fixture should activate draft A");

        specforge::ExclusiveFileLeaseAcquireResult blocked_commit =
            specforge::TryAcquireExclusiveFileLease(
                specforge::SampleLabelingStateCoordinationDirectory(
                    cache_path) /
                "cache-commit.lock");
        Require(
            blocked_commit.status ==
                specforge::ExclusiveFileLeaseAcquireStatus::Acquired,
            "pending shared-slot recovery fixture should hold the cache commit lock");

        const specforge::SampleLabelingOperationResult recovered =
            recovering.RecoverTemporaryTask(
                "shared-source",
                "draft-b");
        Require(
            recovered.accepted &&
                !recovered.state_saved &&
                ActiveTask(recovering) != nullptr &&
                ActiveTask(recovering)->task_id == "draft-b",
            "recovering draft B should remain locally active when selection persistence is blocked");
        Require(
            !observer.ActivateTask("draft-a").accepted &&
                observer.ActivateTask("draft-b").issue ==
                    specforge::SampleLabelingOperationResult::Issue::
                        EditLeaseUnavailable,
            "a failed shared-slot recovery must retain both the deferred old identity and new active lease");

        blocked_commit.lease.Reset();
        Require(
            recovering.FlushStateCache(),
            "shared-slot recovery leases should be releasable after the selection patch can commit");
        Require(
            recovering.DeactivateActiveTask().state_saved,
            "the recovered draft should release its transferred slot after persistence");
        observer.ActivateSource("shared-source", 3);
        Require(
            observer.ActivateTask("draft-a").accepted,
            "the old draft identity and shared slot should both be reusable after recovery completes");
    }

    {
        const std::filesystem::path directory =
            FreshTestDirectory(
                "specforge_labeling_active_temporary_delete_shared_slot_pending");
        const std::filesystem::path cache_path =
            directory / "sample-labeling-tasks.json";
        write_fixture(cache_path);
        specforge::SampleLabelingController deleting(cache_path);
        specforge::SampleLabelingController observer(cache_path);
        deleting.ActivateSource("shared-source", 3);
        observer.ActivateSource("shared-source", 3);
        Require(
            ActiveTask(deleting) != nullptr &&
                ActiveTask(deleting)->task_id == "draft-a",
            "pending shared-slot deletion fixture should activate draft A");

        specforge::ExclusiveFileLeaseAcquireResult blocked_commit =
            specforge::TryAcquireExclusiveFileLease(
                specforge::SampleLabelingStateCoordinationDirectory(
                    cache_path) /
                "cache-commit.lock");
        Require(
            blocked_commit.status ==
                specforge::ExclusiveFileLeaseAcquireStatus::Acquired,
            "pending shared-slot deletion fixture should hold the cache commit lock");

        const specforge::SampleLabelingOperationResult deleted =
            deleting.DeleteTemporaryTask(
                "shared-source",
                "draft-b");
        const std::vector<specforge::SampleLabelingTask>* tasks =
            ActiveSourceTasks(deleting);
        Require(
            deleted.accepted &&
                !deleted.state_saved &&
                ActiveTask(deleting) != nullptr &&
                ActiveTask(deleting)->task_id == "draft-a" &&
                tasks != nullptr &&
                tasks->size() == 1 &&
                tasks->front().task_id == "draft-a" &&
                observer.ActivateTask("draft-b").issue ==
                    specforge::SampleLabelingOperationResult::Issue::
                        EditLeaseUnavailable,
            "a failed deletion must retain the deleted draft identity while leaving A's slot active");

        blocked_commit.lease.Reset();
        Require(
            deleting.FlushStateCache(),
            "the pending draft tombstone should commit after the cache lock is released");
        const specforge::SampleLabelingStateCacheLoadResult loaded =
            specforge::LoadSampleLabelingStateCache(cache_path);
        Require(
            FindTask(
                loaded.cache,
                "shared-source",
                "draft-b") == nullptr,
            "the shared-slot deletion retry should persist B's tombstone");
        observer.ActivateSource("shared-source", 3);
        Require(
            !observer.ActivateTask("draft-b").accepted &&
                observer.ActivateTask("draft-a").issue ==
                    specforge::SampleLabelingOperationResult::Issue::
                        EditLeaseUnavailable,
            "after deletion commits, B must stay absent while A retains the shared slot");
        Require(
            deleting.DeactivateActiveTask().state_saved,
            "the active draft should release the shared slot after the deletion retry");
        observer.ActivateSource("shared-source", 3);
        Require(
            observer.ActivateTask("draft-a").accepted,
            "the remaining draft should become reusable after its owner releases the slot");
    }
}

void TestTaskDeletionMergesWithAnotherInstanceUpsert()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_multi_instance_delete");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    {
        specforge::SampleLabelingController seed(cache_path);
        CreateFormalTask(
            seed,
            "shared-source",
            "old-task",
            directory / "old_y.npy",
            5);
    }

    specforge::SampleLabelingController deleting(cache_path);
    specforge::SampleLabelingController adding(cache_path);
    deleting.ActivateSource("shared-source", 3);
    adding.ActivateSource("shared-source", 3);
    Require(
        deleting.ActivateTask("old-task").accepted,
        "deleting instance should activate the old task");
    Require(
        adding.CreateTask("new-draft", "New draft").accepted,
        "other instance should add a distinct temporary task");
    Require(
        adding.UpsertActiveLabel(
                  specforge::SampleLabelDefinition{7, "new", 'n'})
            .changed &&
            adding.AssignLabel(2, 7).write.changed,
        "other instance should edit its new task");
    Require(adding.FlushStateCache(), "new task should commit");
    Require(
        deleting.DeleteActiveTask().state_saved,
        "explicit deletion tombstone should commit");

    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(cache_path);
    Require(
        FindTask(
            loaded.cache,
            "shared-source",
            "old-task") == nullptr,
        "deleted task should be removed");
    const specforge::SampleLabelingTask* added =
        FindTask(
            loaded.cache,
            "shared-source",
            "new-draft");
    Require(
        added != nullptr && added->values[2] == 7,
        "deletion merge should retain the other instance task upsert");
}

void TestOrdinaryTaskSavePreservesLatestExplicitSelection()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_multi_instance_active_selection");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    {
        specforge::SampleLabelingController seed(cache_path);
        CreateFormalTask(
            seed,
            "shared-source",
            "first-task",
            directory / "first_y.npy",
            5);
        CreateFormalTask(
            seed,
            "shared-source",
            "second-task",
            directory / "second_y.npy",
            7);
    }

    specforge::SampleLabelingController first(cache_path);
    specforge::SampleLabelingController second(cache_path);
    first.ActivateSource("shared-source", 3);
    second.ActivateSource("shared-source", 3);
    Require(
        first.ActivateTask("first-task").state_saved,
        "first explicit activation should commit immediately");
    Require(
        second.ActivateTask("second-task").state_saved,
        "later explicit activation should commit immediately");
    Require(
        first.SetActiveAutoAdvance(true).state_saved,
        "ordinary task setting save should commit its task upsert");

    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(cache_path);
    const auto source = loaded.cache.sources.find("shared-source");
    Require(
        source != loaded.cache.sources.end() &&
            source->second.active_task_id &&
            *source->second.active_task_id == "second-task",
        "ordinary task autosave must preserve the latest explicit selection");
    const specforge::SampleLabelingTask* first_task =
        FindTask(
            loaded.cache,
            "shared-source",
            "first-task");
    Require(
        first_task != nullptr && first_task->auto_advance,
        "ordinary task upsert should still persist its own fields");
}

int HoldLabelingLeaseForCrashTest(
    const std::filesystem::path& cache_path,
    const std::filesystem::path& ready_path)
{
    specforge::SampleLabelingController controller(cache_path);
    controller.ActivateSource("crash-source", 3);
    if (!controller.ActivateTask("crash-task").accepted) {
        return 2;
    }
    WriteTextFile(ready_path, "ready\n");
    Sleep(INFINITE);
    return 0;
}

void TestTargetLeaseIsReleasedAfterProcessTermination()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_multi_instance_crash");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path ready_path =
        directory / "child-ready.txt";
    {
        specforge::SampleLabelingController seed(cache_path);
        CreateFormalTask(
            seed,
            "crash-source",
            "crash-task",
            directory / "crash_y.npy",
            5);
    }

    std::wstring executable(MAX_PATH, L'\0');
    const DWORD executable_size =
        GetModuleFileNameW(
            nullptr,
            executable.data(),
            static_cast<DWORD>(executable.size()));
    Require(
        executable_size > 0 &&
            executable_size < executable.size(),
        "could not resolve the test executable path");
    executable.resize(executable_size);
    std::wstring command_line =
        L"\"" + executable +
        L"\" --hold-labeling-lease \"" +
        cache_path.wstring() + L"\" \"" +
        ready_path.wstring() + L"\"";
    std::vector<wchar_t> mutable_command(
        command_line.begin(),
        command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    const BOOL created =
        CreateProcessW(
            nullptr,
            mutable_command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process);
    Require(created != FALSE, "could not start lease-holder child process");

    bool ready = false;
    for (int attempt = 0; attempt < 500; ++attempt) {
        std::error_code exists_error;
        if (std::filesystem::exists(ready_path, exists_error) &&
            !exists_error) {
            ready = true;
            break;
        }
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
            break;
        }
        Sleep(10);
    }
    (void)TerminateProcess(process.hProcess, 73);
    (void)WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Require(ready, "lease-holder child process did not become ready");
    Require(
        CountLeaseFiles(
            specforge::SampleLabelingStateCoordinationDirectory(
                cache_path) /
            "targets") == 0,
        "process teardown should delete every live target lock file before takeover");

    {
        specforge::SampleLabelingController recovered(cache_path);
        recovered.ActivateSource("crash-source", 3);
        Require(
            recovered.View().active_task != nullptr &&
                recovered.View().active_task->task_id == "crash-task",
            "operating-system lease should be reacquired after abnormal process exit");
        recovered.ClearActiveSource();
    }
    Require(
        CountLeaseFiles(
            specforge::SampleLabelingStateCoordinationDirectory(
                cache_path) /
            "targets") == 0,
        "the recovered owner should also clean its target lock files");
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

void TestCanonicalFormalizationFailsClosedAndRetriesAfterReopenFailure()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_canonical_formalization_reopen");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "formalized.asdf";
    int legacy_publication_calls = 0;
    int creation_publication_calls = 0;
    bool initial_checkpoint_observed = false;
    specforge::SampleLabelingController controller(
        cache_path,
        [](const std::filesystem::path& path) {
            return specforge::LoadSampleLabelingStateCache(path);
        },
        [&legacy_publication_calls](
            specforge::SampleLabelingTask& task,
            const specforge::SampleLabelResultMetadataSource* source) {
            ++legacy_publication_calls;
            return specforge::PublishLegacySampleLabelingTaskOutput(
                task,
                source);
        },
        [](specforge::SampleLabelingAsdfOpenSnapshot& snapshot,
           std::span<const std::int32_t> values) {
            return specforge::RewriteSampleLabelingAsdfValuesAtomically(
                snapshot,
                values);
        },
        [](const specforge::SampleLabelingAsdfOpenSnapshot& snapshot,
           const specforge::SampleLabelingDocument& document,
           const specforge::SampleLabelingCanonicalSourceDescriptor& source) {
            return specforge::
                RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
                    snapshot,
                    document,
                    specforge::SampleLabelingCompatibilityView(
                        source));
        },
        [&creation_publication_calls,
         &initial_checkpoint_observed,
         &cache_path](
            const std::filesystem::path& path,
            const specforge::SampleLabelingDocument& document,
            const specforge::SampleLabelingCanonicalSourceDescriptor& source) {
            ++creation_publication_calls;
            if (creation_publication_calls == 1) {
                const std::string checkpoint =
                    ReadTextFile(cache_path);
                initial_checkpoint_observed =
                    checkpoint.find(
                        "\"initial_publication_pending\": true") !=
                        std::string::npos &&
                    checkpoint.find(
                        "{ \"index\": 1, \"value\": 5 }") !=
                        std::string::npos;
                const specforge::SampleLabelingAsdfStoreWriteResult write =
                    specforge::WriteSampleLabelingAsdfDocumentAtomically(
                        path,
                        document);
                Require(
                    write.succeeded(),
                    "reopen-failure fixture should replace the canonical document");
                return specforge::
                    SampleLabelingAsdfStoreGenerationWriteResult{
                        .document_replaced = true,
                        .error = {
                            .kind = specforge::
                                SampleLabelingAsdfStoreErrorKind::
                                    AtomicWriteFailure,
                            .message =
                                "injected owner reopen failure"}};
            }
            return specforge::
                WriteSampleLabelingAsdfDocumentAndOpenAtomically(
                    path,
                    document,
                    specforge::SampleLabelingCompatibilityView(
                        source));
        });
    ActivateCanonicalTestSource(
        controller,
        "canonical-formalization-source",
        3);
    Require(
        controller.CreateTask("draft", "Draft").accepted &&
            controller.UpsertActiveLabel({5, "accepted", 'a'})
                .changed &&
            controller.AssignLabel(1, 5).write.changed,
        "reopen-failure fixture should prepare its temporary draft");

    const specforge::SampleLabelingOperationResult failed =
        controller.SaveActiveTemporaryTaskToOutput(
            output_path,
            "Formalized");
    Require(
        failed.output_save_attempted &&
            !failed.output_saved &&
            failed.state_saved &&
            ActiveTask(controller) != nullptr &&
            !ActiveTask(controller)->output_path &&
            ActiveTask(controller)->values ==
                std::vector<int>({-1, 5, -1}) &&
            controller.ActiveCanonicalAsdfAnnotationProjection() ==
                std::nullopt,
        "a replaced document that cannot be reopened must not be adopted as a formal owner");
    Require(
        specforge::ReadSampleLabelingAsdfDocument(output_path)
                .succeeded(),
        "reopen failure may leave a durable document but must retain the temporary recovery record");

    const specforge::SampleLabelingOperationResult retried =
        controller.SaveActiveTemporaryTaskToOutput(
            output_path,
            "Formalized");
    Require(
        retried.output_saved &&
            ActiveTask(controller) != nullptr &&
            ActiveTask(controller)->output_format ==
                specforge::SampleLabelingOutputArtifactFormat::
                    CanonicalAsdf &&
            controller.ActiveCanonicalAsdfAnnotationProjection()
                .has_value() &&
            initial_checkpoint_observed &&
            creation_publication_calls == 2 &&
            legacy_publication_calls == 0,
        "retry should safely reopen and adopt the canonical owner without entering the legacy writer");

    const std::filesystem::path descriptorless_cache =
        directory / "descriptorless-state.json";
    const std::filesystem::path descriptorless_output =
        directory / "descriptorless.asdf";
    specforge::SampleLabelingController descriptorless(
        descriptorless_cache,
        [](const std::filesystem::path& path) {
            return specforge::LoadSampleLabelingStateCache(path);
        },
        [&legacy_publication_calls](
            specforge::SampleLabelingTask& task,
            const specforge::SampleLabelResultMetadataSource* source) {
            ++legacy_publication_calls;
            return specforge::PublishLegacySampleLabelingTaskOutput(
                task,
                source);
        });
    descriptorless.ActivateSource(
        "descriptorless-source",
        3);
    Require(
        descriptorless.CreateTask("draft", "Draft").accepted,
        "descriptorless fixture should create its local draft");
    const specforge::SampleLabelingOperationResult rejected =
        descriptorless.SaveActiveTemporaryTaskToOutput(
            descriptorless_output,
            "Formalized");
    Require(
        rejected.output_save_attempted &&
            !rejected.output_saved &&
            ActiveTask(descriptorless) != nullptr &&
            !ActiveTask(descriptorless)->output_path &&
            !std::filesystem::exists(descriptorless_output) &&
            legacy_publication_calls == 0,
        "missing canonical source facts must fail closed without falling back to a legacy owner");
}

void TestCanonicalFormalizationRecoversAfterCompensationCheckpointFailure()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_canonical_formalization_compensation");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path output_path =
        directory / "formalized.asdf";
    specforge::ExclusiveFileLease compensation_checkpoint_lock;
    int creation_publication_calls = 0;
    specforge::SampleLabelingController controller(
        cache_path,
        [](const std::filesystem::path& path) {
            return specforge::LoadSampleLabelingStateCache(path);
        },
        [](specforge::SampleLabelingTask& task,
           const specforge::SampleLabelResultMetadataSource* source) {
            return specforge::PublishLegacySampleLabelingTaskOutput(
                task,
                source);
        },
        [](specforge::SampleLabelingAsdfOpenSnapshot& snapshot,
           std::span<const std::int32_t> values) {
            return specforge::RewriteSampleLabelingAsdfValuesAtomically(
                snapshot,
                values);
        },
        [](const specforge::SampleLabelingAsdfOpenSnapshot& snapshot,
           const specforge::SampleLabelingDocument& document,
           const specforge::SampleLabelingCanonicalSourceDescriptor& source) {
            return specforge::
                RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
                    snapshot,
                    document,
                    specforge::SampleLabelingCompatibilityView(
                        source));
        },
        [&compensation_checkpoint_lock,
         &creation_publication_calls,
         &cache_path](
            const std::filesystem::path& path,
            const specforge::SampleLabelingDocument& document,
            const specforge::SampleLabelingCanonicalSourceDescriptor& source) {
            ++creation_publication_calls;
            if (creation_publication_calls > 1) {
                return specforge::
                    WriteSampleLabelingAsdfDocumentAndOpenAtomically(
                        path,
                        document,
                        specforge::SampleLabelingCompatibilityView(
                            source));
            }
            specforge::ExclusiveFileLeaseAcquireResult lock =
                specforge::TryAcquireExclusiveFileLease(
                    specforge::SampleLabelingStateCoordinationDirectory(
                        cache_path) /
                    "cache-commit.lock");
            Require(
                lock.status ==
                    specforge::ExclusiveFileLeaseAcquireStatus::Acquired,
                "compensation fixture should block the compensating checkpoint after initial publication fails");
            compensation_checkpoint_lock =
                std::move(lock.lease);
            return specforge::
                SampleLabelingAsdfStoreGenerationWriteResult{
                    .document_replaced = false,
                    .error = {
                        .kind = specforge::
                            SampleLabelingAsdfStoreErrorKind::
                                AtomicWriteFailure,
                        .message =
                            "injected initial publication failure"}};
        });
    ActivateCanonicalTestSource(
        controller,
        "canonical-compensation-source",
        3);
    Require(
        controller.CreateTask("draft", "Draft").accepted &&
            controller.UpsertActiveLabel({5, "accepted", 'a'})
                .changed &&
            controller.AssignLabel(1, 5).write.changed,
        "compensation fixture should prepare a temporary draft");

    const specforge::SampleLabelingOperationResult failed =
        controller.SaveActiveTemporaryTaskToOutput(
            output_path,
            "Formalized");
    Require(
        failed.output_save_attempted &&
            !failed.output_saved &&
            !failed.state_saved &&
            failed.output_retry_scheduled &&
            ActiveTask(controller) != nullptr &&
            ActiveTask(controller)->initial_publication_pending &&
            ActiveTask(controller)->output_path == output_path,
        "a failed compensating checkpoint must retain the durable initial-publication owner and schedule recovery");

    compensation_checkpoint_lock.Reset();
    const auto deadline = controller.NextMaintenanceDeadline();
    Require(
        deadline.has_value(),
        "failed compensation must arm maintenance recovery");
    const specforge::SampleLabelingMaintenanceResult maintenance =
        controller.RunMaintenance(
            *deadline + std::chrono::seconds(5));
    Require(
        maintenance.output_retry_attempted &&
            ActiveTask(controller) != nullptr &&
            !ActiveTask(controller)->output_path &&
            ActiveTask(controller)->output_format ==
                specforge::SampleLabelingOutputArtifactFormat::None &&
            !ActiveTask(controller)->initial_publication_pending &&
            ActiveTask(controller)->values ==
                std::vector<int>({-1, 5, -1}) &&
            !controller.NextMaintenanceDeadline(),
        "maintenance must reconcile the durable initial-publication checkpoint back into an editable temporary draft");

    const specforge::SampleLabelingOperationResult retried =
        controller.SaveActiveTemporaryTaskToOutput(
            output_path,
            "Formalized");
    Require(
        retried.output_saved,
        "the recovered draft must remain eligible for Save As");
}

void TestLegacyOwnerMigrationPublishesCanonicalAsdfWithoutChangingLegacyArtifacts()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_legacy_owner_migration");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path legacy_path =
        directory / "legacy-labels.npy";
    const std::filesystem::path legacy_metadata_path =
        specforge::SampleAnnotationIoAdapter::
            MetadataPathForResult(legacy_path);
    const std::filesystem::path canonical_path =
        directory / "migrated-labels.asdf";
    const std::string source_identity =
        "legacy-migration-source";
    const specforge::SourceCollectionIdentity identity{
        .id = source_identity,
        .source_name = "migration-source.npy",
        .source_fingerprint = "migration-source-fingerprint",
        .context_fingerprint = "migration-source-context",
        .spectrum_count = 3,
    };
    const specforge::SampleLabelingCanonicalSourceDescriptor
        descriptor{
            .base_identity = source_identity,
            .source_kind = "npy",
            .source_name = identity.source_name,
            .source_fingerprint =
                identity.source_fingerprint,
            .sample_count = 3,
            .sample_names = {"alpha", "beta", "gamma"},
        };

    specforge::SampleLabelingTask legacy =
        specforge::CreateSampleLabelingTask(
            "legacy-task-id",
            "Legacy quality task",
            3);
    Require(
        specforge::UpsertSampleLabel(
            legacy.label_set,
            {5, "bad", 'b'}) &&
            specforge::UpsertSampleLabel(
                legacy.label_set,
                {7, "good", 'g'}),
        "legacy migration fixture should define its portable labels");
    legacy.values = {-1, 5, -1};
    specforge::RebuildSampleLabelingTaskStatistics(legacy);
    specforge::SelectSampleLabelTaskOutputPath(
        legacy,
        legacy_path);
    const specforge::SampleLabelResultMetadataSource
        legacy_source{
            .source_name = identity.source_name,
            .source_fingerprint =
                identity.source_fingerprint,
            .context_fingerprint =
                identity.context_fingerprint,
            .spectrum_count = identity.spectrum_count,
        };
    Require(
        specforge::PublishLegacySampleLabelingTaskOutput(
            legacy,
            &legacy_source)
            .published,
        "legacy migration fixture should publish its NPY and sidecar owner");
    const std::vector<unsigned char> legacy_bytes =
        ReadBinaryFile(legacy_path);
    const std::vector<unsigned char> legacy_metadata_bytes =
        ReadBinaryFile(legacy_metadata_path);

    bool write_ahead_legacy_owner_observed = false;
    int canonical_creation_calls = 0;
    specforge::SampleLabelingController controller(
        cache_path,
        [](const std::filesystem::path& path) {
            return specforge::LoadSampleLabelingStateCache(path);
        },
        [](specforge::SampleLabelingTask& task,
           const specforge::SampleLabelResultMetadataSource*) {
            specforge::MarkSampleLabelTaskSaveFailed(
                task,
                "injected legacy autosave failure");
            return specforge::SampleLabelOutputPublicationResult{
                .attempted = true,
                .published = false,
                .artifacts_replaced = false,
                .retryable = true,
                .message =
                    "injected legacy autosave failure"};
        },
        [](specforge::SampleLabelingAsdfOpenSnapshot& snapshot,
           std::span<const std::int32_t> values) {
            return specforge::RewriteSampleLabelingAsdfValuesAtomically(
                snapshot,
                values);
        },
        [](const specforge::SampleLabelingAsdfOpenSnapshot& snapshot,
           const specforge::SampleLabelingDocument& document,
           const specforge::SampleLabelingCanonicalSourceDescriptor& source) {
            return specforge::
                RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
                    snapshot,
                    document,
                    specforge::SampleLabelingCompatibilityView(
                        source));
        },
        [&cache_path,
         &canonical_creation_calls,
         &write_ahead_legacy_owner_observed,
         &source_identity,
         &legacy_path](
            const std::filesystem::path& path,
            const specforge::SampleLabelingDocument& document,
            const specforge::SampleLabelingCanonicalSourceDescriptor& source) {
            ++canonical_creation_calls;
            const specforge::SampleLabelingStateCacheLoadResult
                checkpoint =
                    specforge::LoadSampleLabelingStateCache(
                        cache_path,
                        {},
                        specforge::
                            SampleLabelingStateCacheLoadPolicy::
                                AllowPersistentOutputsWithoutResultHydration);
            const specforge::SampleLabelingTask* checkpoint_task =
                FindTask(
                    checkpoint.cache,
                    source_identity,
                    "legacy-task-id");
            write_ahead_legacy_owner_observed =
                checkpoint.issue_kind ==
                    specforge::
                        SampleLabelingStateCacheLoadIssueKind::
                            None &&
                checkpoint_task != nullptr &&
                checkpoint_task->output_path == legacy_path &&
                checkpoint_task->output_format ==
                    specforge::
                        SampleLabelingOutputArtifactFormat::
                            LegacyNpyWithSidecar &&
                checkpoint_task->values ==
                    std::vector<int>({7, -1, -1}) &&
                checkpoint_task->pending_sample_indices.contains(0);
            return specforge::
                WriteSampleLabelingAsdfDocumentAndOpenAtomically(
                    path,
                    document,
                    specforge::SampleLabelingCompatibilityView(
                        source));
        });
    controller.ActivateSource(identity, descriptor);
    Require(
        controller.CreateTaskFromAnnotation(
                      legacy.task_id,
                      legacy.task_name,
                      legacy.label_set,
                      legacy.values,
                      legacy_path,
                      true)
            .accepted,
        "legacy migration fixture should activate the existing owner");
    const specforge::SampleLabelingWriteOperationResult
        pending_edit = controller.AssignLabel(0, 7);
    Require(
        pending_edit.write.changed &&
            pending_edit.operation.output_save_attempted &&
            !pending_edit.operation.output_saved,
        "legacy migration fixture should retain a newest pending overlay over the old durable base");

    specforge::ExclusiveFileLeaseAcquireResult old_lease_before =
        TryAcquireCurrentStableArtifactLease(
            cache_path,
            legacy_path);
    Require(
        old_lease_before.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::
                Unavailable,
        "active legacy owner should hold its old artifact lease before migration");

    const specforge::SampleLabelingOperationResult migrated =
        controller.MigrateActiveLegacyTaskToCanonicalAsdf(
            canonical_path);
    const specforge::SampleLabelingTask* active =
        ActiveTask(controller);
    Require(
        migrated.output_save_attempted &&
            migrated.output_saved &&
            active != nullptr &&
            active->task_id == "legacy-task-id" &&
            active->task_name == "Legacy quality task" &&
            active->output_path == canonical_path &&
            active->output_format ==
                specforge::
                    SampleLabelingOutputArtifactFormat::
                        CanonicalAsdf &&
            active->values ==
                std::vector<int>({7, 5, -1}) &&
            specforge::FindSampleLabel(
                active->label_set,
                5) != nullptr &&
            specforge::FindSampleLabel(
                active->label_set,
                7) != nullptr,
        "successful migration should preserve the legacy task identity, definitions, and newest values while switching ownership");
    Require(
        write_ahead_legacy_owner_observed &&
            canonical_creation_calls == 1,
        "migration should checkpoint the legacy owner and pending overlay before creating ASDF");
    Require(
        ReadBinaryFile(legacy_path) == legacy_bytes &&
            ReadBinaryFile(legacy_metadata_path) ==
                legacy_metadata_bytes,
        "migration must not rewrite the original NPY or sidecar bytes");

    const specforge::SampleLabelingAsdfReadResult read =
        specforge::ReadSampleLabelingAsdfDocument(
            canonical_path);
    Require(
        read.succeeded() &&
            read.document->labeling.id ==
                "legacy-task-id" &&
            read.document->labeling.name ==
                "Legacy quality task" &&
            read.document->annotation.values ==
                std::vector<std::int32_t>({7, 5, -1}) &&
            read.document->source.roster.identity_kind ==
                specforge::
                    kSampleLabelingDocumentExplicitNamesRoster &&
            read.document->source.roster.sample_names ==
                descriptor.sample_names,
        "migrated ASDF should use the current canonical source roster and preserve legacy task semantics");

    const specforge::SampleLabelingStateCacheLoadResult cache =
        specforge::LoadSampleLabelingStateCache(
            cache_path,
            {},
            specforge::SampleLabelingStateCacheLoadPolicy::
                AllowPersistentOutputsWithoutResultHydration);
    const specforge::SampleLabelingTask* cached =
        FindTask(
            cache.cache,
            source_identity,
            "legacy-task-id");
    Require(
        cache.issue_kind ==
                specforge::
                    SampleLabelingStateCacheLoadIssueKind::None &&
            cached != nullptr &&
            cached->output_path == canonical_path &&
            cached->output_format ==
                specforge::
                    SampleLabelingOutputArtifactFormat::
                        CanonicalAsdf,
        "successful migration should durably switch the task owner record to canonical ASDF");

    specforge::ExclusiveFileLeaseAcquireResult old_lease_after =
        TryAcquireCurrentStableArtifactLease(
            cache_path,
            legacy_path);
    Require(
        old_lease_after.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::
                Acquired,
        "successful owner switch should release the old legacy artifact lease");
    specforge::ExclusiveFileLeaseAcquireResult new_lease_after =
        TryAcquireCurrentStableArtifactLease(
            cache_path,
            canonical_path);
    Require(
        new_lease_after.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::
                Unavailable,
        "successful owner switch should retain the new canonical one-file lease");
}

void TestLegacyOwnerMigrationFailureKeepsLegacyOwnerAndArtifacts()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_legacy_owner_migration_failure");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path legacy_path =
        directory / "legacy-labels.npy";
    const std::filesystem::path metadata_path =
        specforge::SampleAnnotationIoAdapter::
            MetadataPathForResult(legacy_path);
    const std::filesystem::path canonical_path =
        directory / "failed-migration.asdf";
    const std::filesystem::path checkpoint_failure_path =
        directory / "checkpoint-failed-migration.asdf";
    specforge::SampleLabelingTask legacy =
        specforge::CreateSampleLabelingTask(
            "legacy-failure-task",
            "Legacy failure task",
            3);
    Require(
        specforge::UpsertSampleLabel(
            legacy.label_set,
            {5, "bad", 'b'}),
        "failed migration fixture should define its legacy label");
    legacy.values = {-1, 5, -1};
    specforge::RebuildSampleLabelingTaskStatistics(legacy);
    specforge::SelectSampleLabelTaskOutputPath(
        legacy,
        legacy_path);
    Require(
        specforge::PublishLegacySampleLabelingTaskOutput(
            legacy)
            .published,
        "failed migration fixture should publish its legacy owner");
    const std::vector<unsigned char> legacy_bytes =
        ReadBinaryFile(legacy_path);
    const std::vector<unsigned char> metadata_bytes =
        ReadBinaryFile(metadata_path);

    specforge::SampleLabelingController controller(
        cache_path,
        [](const std::filesystem::path& path) {
            return specforge::LoadSampleLabelingStateCache(path);
        },
        [](specforge::SampleLabelingTask& task,
           const specforge::SampleLabelResultMetadataSource*) {
            specforge::MarkSampleLabelTaskSaveFailed(
                task,
                "injected legacy autosave failure");
            return specforge::SampleLabelOutputPublicationResult{
                .attempted = true,
                .published = false,
                .artifacts_replaced = false,
                .retryable = true,
                .message =
                    "injected legacy autosave failure"};
        },
        [](specforge::SampleLabelingAsdfOpenSnapshot& snapshot,
           std::span<const std::int32_t> values) {
            return specforge::RewriteSampleLabelingAsdfValuesAtomically(
                snapshot,
                values);
        },
        [](const specforge::SampleLabelingAsdfOpenSnapshot& snapshot,
           const specforge::SampleLabelingDocument& document,
           const specforge::SampleLabelingCanonicalSourceDescriptor& source) {
            return specforge::
                RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
                    snapshot,
                    document,
                    specforge::SampleLabelingCompatibilityView(
                        source));
        },
        [](const std::filesystem::path&,
           const specforge::SampleLabelingDocument&,
           const specforge::SampleLabelingCanonicalSourceDescriptor&) {
            return specforge::
                SampleLabelingAsdfStoreGenerationWriteResult{
                    .document_replaced = false,
                    .error = {
                        .kind = specforge::
                            SampleLabelingAsdfStoreErrorKind::
                                AtomicWriteFailure,
                        .message =
                            "injected migration publication failure"}};
        });
    ActivateCanonicalTestSource(
        controller,
        "legacy-migration-failure-source",
        3);
    Require(
        controller.CreateTaskFromAnnotation(
                      legacy.task_id,
                      legacy.task_name,
                      legacy.label_set,
                      legacy.values,
                      legacy_path,
                      true)
            .accepted,
        "failed migration fixture should activate its legacy owner");
    const specforge::SampleLabelingWriteOperationResult pending_edit =
        controller.AssignLabel(0, 5);
    Require(
        pending_edit.write.changed &&
            !pending_edit.operation.output_saved,
        "failed migration fixture should retain a pending value over its legacy base");

    specforge::ExclusiveFileLeaseAcquireResult checkpoint_lock =
        specforge::TryAcquireExclusiveFileLease(
            specforge::SampleLabelingStateCoordinationDirectory(
                cache_path) /
            "cache-commit.lock");
    Require(
        checkpoint_lock.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::Acquired,
        "failed migration fixture should block its write-ahead checkpoint");
    const specforge::SampleLabelingOperationResult checkpoint_failed =
        controller.MigrateActiveLegacyTaskToCanonicalAsdf(
            checkpoint_failure_path);
    Require(
        !checkpoint_failed.output_save_attempted &&
            !checkpoint_failed.output_saved &&
            checkpoint_failed.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    OutputMigrationCheckpointFailed &&
            !checkpoint_failed.diagnostic.empty() &&
            !std::filesystem::exists(
                checkpoint_failure_path) &&
            ActiveTask(controller) != nullptr &&
            ActiveTask(controller)->output_path == legacy_path,
        "write-ahead checkpoint failure should expose a stable issue before touching ASDF or the legacy owner");
    checkpoint_lock.lease.Reset();

    const specforge::SampleLabelingOperationResult failed =
        controller.MigrateActiveLegacyTaskToCanonicalAsdf(
            canonical_path);
    const specforge::SampleLabelingTask* active =
        ActiveTask(controller);
    Require(
        failed.output_save_attempted &&
            !failed.output_saved &&
            failed.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    OutputMigrationPublicationFailed &&
            !failed.diagnostic.empty() &&
            active != nullptr &&
            active->output_path == legacy_path &&
            active->output_format ==
                specforge::
                    SampleLabelingOutputArtifactFormat::
                        LegacyNpyWithSidecar &&
            active->values ==
                std::vector<int>({5, 5, -1}) &&
            active->pending_sample_indices.contains(0) &&
            !controller.ActiveCanonicalAsdfAnnotationProjection(),
        "failed migration should keep the old legacy owner and its recovery overlay active");
    Require(
        ReadBinaryFile(legacy_path) == legacy_bytes &&
            ReadBinaryFile(metadata_path) == metadata_bytes &&
            !std::filesystem::exists(canonical_path),
        "failed migration must leave both old legacy artifacts and the target path unchanged");

    const specforge::SampleLabelingStateCacheLoadResult cache =
        specforge::LoadSampleLabelingStateCache(
            cache_path,
            {},
            specforge::SampleLabelingStateCacheLoadPolicy::
                AllowPersistentOutputsWithoutResultHydration);
    const specforge::SampleLabelingTask* cached =
        FindTask(
            cache.cache,
            "legacy-migration-failure-source",
            "legacy-failure-task");
    Require(
        cached != nullptr &&
            cached->output_path == legacy_path &&
            cached->output_format ==
                specforge::
                    SampleLabelingOutputArtifactFormat::
                        LegacyNpyWithSidecar &&
            cached->values ==
                std::vector<int>({5, -1, -1}) &&
            cached->pending_sample_indices.contains(0),
        "failed migration checkpoint should keep the legacy durable base plus newest sparse overlay");
    specforge::ExclusiveFileLeaseAcquireResult old_lease_after =
        TryAcquireCurrentStableArtifactLease(
            cache_path,
            legacy_path);
    Require(
        old_lease_after.status ==
            specforge::ExclusiveFileLeaseAcquireStatus::
                Unavailable,
        "failed migration should retain the active legacy artifact lease");
}

void TestLegacyOwnerMigrationRejectsUsedOutputPathWithStructuredIssue()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_legacy_owner_migration_conflict");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path legacy_path =
        directory / "legacy-labels.npy";
    const std::filesystem::path canonical_path =
        directory / "reserved.asdf";

    specforge::SampleLabelingTask legacy =
        specforge::CreateSampleLabelingTask(
            "legacy-conflict-task",
            "Legacy conflict task",
            3);
    Require(
        specforge::UpsertSampleLabel(
            legacy.label_set,
            {5, "bad", 'b'}),
        "migration conflict fixture should define its legacy label");
    legacy.values = {5, -1, -1};
    specforge::RebuildSampleLabelingTaskStatistics(legacy);
    specforge::SelectSampleLabelTaskOutputPath(
        legacy,
        legacy_path);
    Require(
        specforge::PublishLegacySampleLabelingTaskOutput(
            legacy)
            .published,
        "migration conflict fixture should publish its legacy owner");

    specforge::SampleLabelingController controller(cache_path);
    ActivateCanonicalTestSource(
        controller,
        "legacy-migration-conflict-source",
        3);
    Require(
        controller.CreateTaskFromAnnotation(
                      legacy.task_id,
                      legacy.task_name,
                      legacy.label_set,
                      legacy.values,
                      legacy_path,
                      true)
                .accepted &&
            controller.DeactivateActiveTask().accepted &&
            controller.CreateTask(
                          "reserved-canonical-task",
                          "Reserved canonical task")
                .accepted &&
            controller.UpsertActiveLabel({7, "good", 'g'})
                .changed &&
            controller.AssignLabel(1, 7).write.changed &&
            controller.SaveActiveTemporaryTaskToOutput(
                          canonical_path,
                          "Reserved canonical task")
                .output_saved &&
            controller.DeactivateActiveTask().accepted &&
            controller.ActivateTask(legacy.task_id).accepted,
        "migration conflict fixture should retain both formal task owners");

    const specforge::SampleLabelingOperationResult conflict =
        controller.MigrateActiveLegacyTaskToCanonicalAsdf(
            canonical_path);
    const specforge::SampleLabelingTask* active =
        ActiveTask(controller);
    Require(
        !conflict.accepted &&
            conflict.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    OutputPathAlreadyUsed &&
            conflict.diagnostic.empty() &&
            active != nullptr &&
            active->task_id == legacy.task_id &&
            active->output_path == legacy_path &&
            active->output_format ==
                specforge::SampleLabelingOutputArtifactFormat::
                    LegacyNpyWithSidecar,
        "migration should report a structured output-path conflict without changing the active legacy owner");
}

void TestLegacyOwnerMigrationOwnerSwitchCheckpointFailureKeepsLegacyOwnerAndCanRetry()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge_labeling_legacy_owner_migration_owner_switch_failure");
    const std::filesystem::path cache_path =
        directory / "sample-labeling-tasks.json";
    const std::filesystem::path legacy_path =
        directory / "legacy-labels.npy";
    const std::filesystem::path metadata_path =
        specforge::SampleAnnotationIoAdapter::
            MetadataPathForResult(legacy_path);
    const std::filesystem::path canonical_path =
        directory / "published-but-unowned.asdf";
    specforge::SampleLabelingTask legacy =
        specforge::CreateSampleLabelingTask(
            "legacy-owner-switch-task",
            "Legacy owner switch task",
            3);
    Require(
        specforge::UpsertSampleLabel(
            legacy.label_set,
            {5, "bad", 'b'}),
        "owner-switch failure fixture should define its legacy label");
    legacy.values = {-1, 5, -1};
    specforge::RebuildSampleLabelingTaskStatistics(legacy);
    specforge::SelectSampleLabelTaskOutputPath(
        legacy,
        legacy_path);
    Require(
        specforge::PublishLegacySampleLabelingTaskOutput(
            legacy)
            .published,
        "owner-switch failure fixture should publish its legacy owner");
    const std::vector<unsigned char> legacy_bytes =
        ReadBinaryFile(legacy_path);
    const std::vector<unsigned char> metadata_bytes =
        ReadBinaryFile(metadata_path);

    specforge::ExclusiveFileLease owner_switch_checkpoint_lock;
    int canonical_creation_calls = 0;
    specforge::SampleLabelingController controller(
        cache_path,
        [](const std::filesystem::path& path) {
            return specforge::LoadSampleLabelingStateCache(path);
        },
        [](specforge::SampleLabelingTask& task,
           const specforge::SampleLabelResultMetadataSource* source) {
            return specforge::PublishLegacySampleLabelingTaskOutput(
                task,
                source);
        },
        [](specforge::SampleLabelingAsdfOpenSnapshot& snapshot,
           std::span<const std::int32_t> values) {
            return specforge::RewriteSampleLabelingAsdfValuesAtomically(
                snapshot,
                values);
        },
        [](const specforge::SampleLabelingAsdfOpenSnapshot& snapshot,
           const specforge::SampleLabelingDocument& document,
           const specforge::SampleLabelingCanonicalSourceDescriptor& source) {
            return specforge::
                RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
                    snapshot,
                    document,
                    specforge::SampleLabelingCompatibilityView(
                        source));
        },
        [&owner_switch_checkpoint_lock,
         &canonical_creation_calls,
         &cache_path](
            const std::filesystem::path& path,
            const specforge::SampleLabelingDocument& document,
            const specforge::SampleLabelingCanonicalSourceDescriptor& source) {
            ++canonical_creation_calls;
            specforge::SampleLabelingAsdfStoreGenerationWriteResult write =
                specforge::WriteSampleLabelingAsdfDocumentAndOpenAtomically(
                    path,
                    document,
                    specforge::SampleLabelingCompatibilityView(
                        source));
            if (canonical_creation_calls == 1 && write.succeeded()) {
                specforge::ExclusiveFileLeaseAcquireResult lock =
                    specforge::TryAcquireExclusiveFileLease(
                        specforge::SampleLabelingStateCoordinationDirectory(
                            cache_path) /
                        "cache-commit.lock");
                Require(
                    lock.status ==
                        specforge::ExclusiveFileLeaseAcquireStatus::
                            Acquired,
                    "owner-switch failure fixture should block the owner-switch checkpoint after ASDF publication");
                owner_switch_checkpoint_lock =
                    std::move(lock.lease);
            }
            return write;
        });
    ActivateCanonicalTestSource(
        controller,
        "legacy-owner-switch-source",
        3);
    Require(
        controller.CreateTaskFromAnnotation(
                      legacy.task_id,
                      legacy.task_name,
                      legacy.label_set,
                      legacy.values,
                      legacy_path,
                      true)
            .accepted,
        "owner-switch failure fixture should activate its legacy owner");

    const specforge::SampleLabelingOperationResult failed =
        controller.MigrateActiveLegacyTaskToCanonicalAsdf(
            canonical_path);
    const specforge::SampleLabelingTask* active =
        ActiveTask(controller);
    Require(
        failed.output_save_attempted &&
            !failed.output_saved &&
            failed.issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    OutputMigrationOwnerSwitchFailed &&
            !failed.diagnostic.empty() &&
            active != nullptr &&
            active->output_path == legacy_path &&
            active->output_format ==
                specforge::SampleLabelingOutputArtifactFormat::
                    LegacyNpyWithSidecar &&
            !controller.ActiveCanonicalAsdfAnnotationProjection(),
        "owner-switch checkpoint failure should keep the active legacy owner and expose a stable failure kind");
    Require(
        ReadBinaryFile(legacy_path) == legacy_bytes &&
            ReadBinaryFile(metadata_path) == metadata_bytes &&
            specforge::ReadSampleLabelingAsdfDocument(
                canonical_path)
                .succeeded(),
        "owner-switch checkpoint failure should leave old artifacts unchanged and the published ASDF complete but unowned");
    const specforge::SampleLabelingStateCacheLoadResult cache =
        specforge::LoadSampleLabelingStateCache(
            cache_path,
            {},
            specforge::SampleLabelingStateCacheLoadPolicy::
                AllowPersistentOutputsWithoutResultHydration);
    const specforge::SampleLabelingTask* cached =
        FindTask(
            cache.cache,
            "legacy-owner-switch-source",
            legacy.task_id);
    Require(
        cached != nullptr &&
            cached->output_path == legacy_path &&
            cached->output_format ==
                specforge::SampleLabelingOutputArtifactFormat::
                    LegacyNpyWithSidecar,
        "owner-switch checkpoint failure should leave the durable task owner on its legacy base");
    specforge::ExclusiveFileLeaseAcquireResult old_lease_after_failure =
        TryAcquireCurrentStableArtifactLease(
            cache_path,
            legacy_path);
    specforge::ExclusiveFileLeaseAcquireResult unowned_asdf_lease =
        TryAcquireCurrentStableArtifactLease(
            cache_path,
            canonical_path);
    Require(
        old_lease_after_failure.status ==
                specforge::ExclusiveFileLeaseAcquireStatus::
                    Unavailable &&
            unowned_asdf_lease.status ==
                specforge::ExclusiveFileLeaseAcquireStatus::
                    Acquired,
        "owner-switch checkpoint failure should retain old leases while releasing the unowned ASDF lease");

    unowned_asdf_lease.lease.Reset();
    owner_switch_checkpoint_lock.Reset();
    const specforge::SampleLabelingOperationResult retried =
        controller.MigrateActiveLegacyTaskToCanonicalAsdf(
            canonical_path);
    Require(
        retried.output_saved &&
            canonical_creation_calls == 2 &&
            ActiveTask(controller) != nullptr &&
            ActiveTask(controller)->output_path == canonical_path &&
            ActiveTask(controller)->output_format ==
                specforge::SampleLabelingOutputArtifactFormat::
                    CanonicalAsdf,
        "retry after owner-switch checkpoint recovery should adopt the canonical ASDF owner");
    specforge::ExclusiveFileLeaseAcquireResult old_lease_after_retry =
        TryAcquireCurrentStableArtifactLease(
            cache_path,
            legacy_path);
    specforge::ExclusiveFileLeaseAcquireResult new_lease_after_retry =
        TryAcquireCurrentStableArtifactLease(
            cache_path,
            canonical_path);
    Require(
        old_lease_after_retry.status ==
                specforge::ExclusiveFileLeaseAcquireStatus::
                    Acquired &&
            new_lease_after_retry.status ==
                specforge::ExclusiveFileLeaseAcquireStatus::
                    Unavailable,
        "successful retry should release legacy leases and retain the canonical ASDF lease");
}

void TestCanonicalAnnotationFilterIndexesLargeLabelSet()
{
    constexpr int kLabelCount = 4096;
    constexpr std::size_t kSampleCount = 16'384;

    auto document =
        std::make_shared<specforge::SampleLabelingDocument>();
    document->labeling.labels.reserve(kLabelCount);
    for (int code = 0; code < kLabelCount; ++code) {
        document->labeling.labels.push_back(
            specforge::SampleLabelingDocumentLabel{
                code,
                "label-" + std::to_string(code),
                {}});
    }

    specforge::SampleAnnotationResult annotation;
    annotation.name = "canonical-labels";
    annotation.kind =
        specforge::SampleAnnotationKind::CategoricalInteger;
    annotation.labeling_document = std::move(document);
    annotation.values.reserve(kSampleCount);
    for (std::size_t index = 0; index < kSampleCount; ++index) {
        annotation.values.push_back(specforge::SampleAnnotationValue{
            std::int64_t{index % 2U == 0U
                    ? kLabelCount - 1
                    : specforge::kUnlabeledSampleLabelCode}});
    }

    const specforge::SampleFilterSource source =
        specforge::BuildAnnotationFilterSource(annotation);
    Require(
        source.options.size() == 2,
        "canonical annotation filtering should collapse repeated codes");
    const auto labeled = std::find_if(
        source.options.begin(),
        source.options.end(),
        [](const specforge::SampleFilterValueOption& option) {
            return option.key == "4095";
        });
    Require(
        labeled != source.options.end() &&
            labeled->display_text == "label-4095 (4095)" &&
            labeled->sample_count == kSampleCount / 2U,
        "canonical filter options should use the pre-indexed label display");
    const auto unlabeled = std::find_if(
        source.options.begin(),
        source.options.end(),
        [](const specforge::SampleFilterValueOption& option) {
            return option.represents_unlabeled_value;
        });
    Require(
        unlabeled != source.options.end() &&
            unlabeled->display_text == "Unlabeled (-1)" &&
            unlabeled->sample_count == kSampleCount / 2U,
        "canonical filter options should preserve missing-value semantics");
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

int main(int argc, char* argv[])
{
    if (argc == 4 &&
        std::string_view(argv[1]) ==
            "--hold-labeling-lease") {
        return HoldLabelingLeaseForCrashTest(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]));
    }
    try {
        TestSampleLabelingStateCacheRoundTrip();
        TestSampleLabelingOutputFormatMigrationAndRoundTrip();
        TestInterruptedInitialCanonicalPublicationRestoresTemporaryDraft();
        TestCanonicalAsdfTaskOwnerHydratesWithPendingOverlay();
        TestCanonicalAsdfValueFailureRetainsOverlayAndRetries();
        TestCanonicalAsdfMetadataMutationsPublishFullGenerations();
        TestCanonicalAsdfMetadataReopenFailureRetriesFromCurrentGeneration();
        TestCanonicalAsdfProjectionDowngradesWhenDeactivated();
        TestCanonicalRevalidationRetainsLeaseForPendingRecoveryPatch();
        TestCanonicalAsdfTaskOwnerFailsClosed();
        TestCanonicalAsdfTaskOwnerRejectsUndefinedPendingCode();
        TestSampleLabelingStateCacheReportsCorruptJson();
        TestSampleLabelingStateCacheReportsUnsupportedSchema();
        TestSampleLabelTaskWritesStableCodes();
        TestRemovingSampleLabelClearsAssignedValues();
        TestChangingUnusedSampleLabelCode();
        TestChangingUsedSampleLabelCodeRequiresConfirmation();
        TestSampleLabelResultWritesCompactNpy();
        TestSampleLabelExportSnapshotUsesCanonicalRosterAndStableSerialization();
        TestLabelValueExportsAreStateless();
        TestLabelValuesNpyExportRejectsManagedAndLeasedOwners();
        TestOutputArtifactOwnershipIsFormatAware();
        TestSampleLabelResultWritesMetadataSidecar();
        TestAnnotationAdapterRoundTripsLabelArtifacts();
        TestAnnotationAdapterRejectsEmptyTaskIdBeforeWriting();
        TestSampleAnnotationUsesMatchingLabelMetadata();
        TestMismatchedLabelMetadataFallsBackToRawAnnotationValues();
        TestFailedNpySaveDoesNotDamageExistingOutput();
        TestSampleLabelingControllerAutosavesDraftRecord();
        TestTaskRecordFlushKeepsActiveTaskAddressStable();
        TestControllerRevisionTracksOwnedTaskChanges();
        TestCreateFromAnnotationWaitsForRecoveryCheckpoint();
        TestCreateFromAnnotationDefersPhysicalIdentityRefresh();
        TestCreateFromAnnotationRefreshesLeaseAfterPartialWrite();
        TestControllerKeepsOneTemporaryTaskPerSource();
        TestControllerAtomicallyStartsOrResumesTemporaryTask();
        TestCanonicalFormalizationFailsClosedAndRetriesAfterReopenFailure();
        TestCanonicalFormalizationRecoversAfterCompensationCheckpointFailure();
        TestLegacyOwnerMigrationPublishesCanonicalAsdfWithoutChangingLegacyArtifacts();
        TestLegacyOwnerMigrationFailureKeepsLegacyOwnerAndArtifacts();
        TestLegacyOwnerMigrationRejectsUsedOutputPathWithStructuredIssue();
        TestLegacyOwnerMigrationOwnerSwitchCheckpointFailureKeepsLegacyOwnerAndCanRetry();
        TestExternalOutputIsResultSourceOfTruth();
        TestMetadataOnlyChangesRewriteSidecarOnRetry();
        TestOutputPathConflictIsRejectedWithinSource();
        TestSampleLabelSaveMessageKindsSeparateBusinessAndSystemErrors();
        TestMissingExternalOutputRestoresFailedState();
        TestFailedFirstOutputSaveKeepsTemporaryDraftRecoveryValues();
        TestCorruptLocalTaskRecordIsIgnored();
        TestPendingCreateCannotReplaceRepairedSameIdTask();
        TestFailedExternalOutputPersistsPendingOverlay();
        TestFailedExternalOutputRetriesAfterBackoff();
        TestOutputWriteWaitsForPendingOverlayCommit();
        TestInteractiveOutputLabelWritesDoNotWaitForCacheCommitLock();
        TestSuccessfulOutputCannotRetainOlderPendingOverlay();
        TestDifferentFormalTargetsMergeAcrossInstances();
        TestDifferentTemporaryTasksMergeAcrossInstances();
        TestStaleNewTaskDoesNotActivateLatestFormalTask();
        TestStaleExplicitCreateDoesNotActivateFormalizedDraft();
        TestDuplicateTaskIdsFailClosedBeforeOutputPersistence();
        TestSameTargetLeaseRejectsSecondInstance();
        TestTemporaryDraftRecoveryRevalidatesSourceAndTask();
        TestTemporaryDraftRecoveryDeletionKeepsLeaseUntilTombstoneCommits();
        TestTemporaryDraftRecoveryCancelsPendingCreateBeforeFlush();
        TestTemporaryDraftRecoveryUsesPendingCreateBeforeFlush();
        TestTemporaryDraftRecoveryMissingTargetDoesNotTombstoneRecreatedTask();
        TestTemporaryDraftRecoveryRetainsDeferredPendingEdit();
        TestTemporaryDraftRecoveryDeleteConvergesFormalizedProjection();
        TestTemporaryRecoveryRejectsAlreadyActiveFormalTask();
        TestTemporaryDraftDeleteIgnoresUnrelatedFailedActiveFormalTask();
        TestLeaseConflictTemporaryReopenPreservesPendingEdit();
        TestFormalLeaseConflictDoesNotPoisonRecoverableDraft();
        TestRecoveryViewMarksOwnDraftLeaseConflictAndAdvancesRevision();
        TestOutputPathAliasesShareConflictAndLeaseIdentity();
        TestExistingHardLinksShareFileObjectIdentity();
        TestOutputArtifactSetSharesConflictAndLeaseIdentity();
        TestOfflineHistoricalOutputDoesNotBlockUnrelatedPatch();
        TestOfflineOutputLeaseFallsBackToStablePathIdentity();
        TestSynchronousFlushWaitsForShortCommitLockContention();
        TestLeaseSetupFailureIsNotReportedAsAnotherEditor();
        TestLongCoordinationPathCanAcquireTargetLease();
        TestFirstCacheCreationKeepsStableTaskLease();
        TestTemporaryFormalizationRequiresRecoveryCheckpoint();
        TestTemporaryFormalizationKeepsStableTaskLease();
        TestSequentialLeaseHandoffRefreshesLatestTask();
        TestLeaseHandoffRefreshInvalidatesTaskProjectionGeneration();
        TestSameSourceRefreshInvalidatesTaskProjectionGeneration();
        TestSameSourceReopenRemovesMissingTaskProjection();
        TestDirectLeaseConflictRefreshRemovesTombstonedTaskProjection();
        TestPendingDeletionKeepsTaskLeaseUntilTombstoneCommits();
        TestFailedTaskSwitchDefersPreviousTaskLease();
        TestDeferredTaskLeaseCanBeReusedByItsController();
        TestDeferredTaskLeaseRestoreRetainsPendingTaskProjection();
        TestPreparedReopenAdoptsUnprotectedTasksAndDeletions();
        TestPreparedReopenMergesOnlyProtectedLocalTasks();
        TestRepeatedOutputSavesKeepLeaseDirectoryBounded();
        TestLeaseCleanupSurvivesSuccessorAcquisitionRace();
        TestFailedDeletionRetryClearsPersistedSelection();
        TestOutputRetryRespectsTaskLease();
        TestCachePatchFailsClosedOnUntrustedLatestFile();
        TestSchemaOneMultipleDraftsRemainPatchable();
        TestRecoveryViewClassifiesCurrentAndRecoverableDraft();
        TestRecoveryViewClassifiesConflictingDrafts();
        TestRecoveryViewClassifiesUntrustedDraftAsStale();
        TestRecoveryViewRetainsUntrustedPreparedSnapshotTrust();
        TestRecoveryViewTrustedPreparedSnapshotClearsStalenessDespiteWarning();
        TestRecoveryViewDoesNotStaleProtectedLocalTask();
        TestRecoveryViewVerifiedRestoreBeatsStalePreparedProvenance();
        TestHistoricalDuplicateOutputsRemainPatchable();
        TestStructurallyDamagedCacheFailsClosedAfterSalvage();
        TestMalformedTaskFieldsFailClosedAfterSalvage();
        TestMalformedSourceFieldsFailClosedAfterSalvage();
        TestMissingRequiredTaskFieldsFailClosed();
        TestMissingSourceTasksFieldFailsClosed();
        TestMissingTaskRefreshRemovesGhostAndRestartsDraft();
        TestRecoveryViewExpiresLeaseConflictAfterTrustedRefresh();
        TestTemporarySlotLeaseSerializesDifferentTaskIds();
        TestActiveTemporaryDraftCanRecoverAnotherDraftWithSharedSlot();
        TestTaskDeletionMergesWithAnotherInstanceUpsert();
        TestOrdinaryTaskSavePreservesLatestExplicitSelection();
        TestTargetLeaseIsReleasedAfterProcessTermination();
        TestSampleFiltersStackCategoricalConditions();
        TestCanonicalAnnotationFilterIndexesLargeLabelSet();
        TestFloatingAnnotationsAreNotFilterable();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
