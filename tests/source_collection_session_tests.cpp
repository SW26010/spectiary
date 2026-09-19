#include "helpers/source_load_test_support.h"
#include "helpers/temporary_directory.h"
#include "legacy_annotation_fixture_io.h"
#include "domain/csv_record_codec.h"
#include "domain/sample_annotation_io.h"
#include "domain/sample_labeling.h"
#include "domain/sample_labeling_asdf_codec.h"
#include "domain/sample_labeling_document.h"
#include "domain/source_collection_manifest.h"
#include "domain/spectrum_snapshot.h"
#include "domain/uuid_v4.h"
#include "ui/sample_annotation_labeling_rules.h"
#include "ui/sample_labeling_state_cache_io.h"
#include "ui/sample_workflow_state_cache_io.h"
#include "ui/sample_workflow_coordinator.h"
#include "ui/sample_workflow_preparation.h"
#include "ui/source_collection_load_queue.h"
#include "ui/source_collection_load_queue_internal.h"
#include "ui/source_collection_roster.h"
#include "ui/source_collection_session.h"
#include "ui/source_collection_session_state_cache_io.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// main owns this directory until all sessions and their workers have stopped.
std::filesystem::path test_root;

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void Require(const spectiary::SampleLabelingStateCacheSaveResult& result,
             std::string_view message)
{
    Require(result.Succeeded(), message);
}

spectiary::SampleLabelingTaskCanonicalMetadata TestCanonicalMetadata()
{
    const auto timestamp =
        spectiary::ParseCanonicalTimestamp("2026-01-02T03:04:05.006Z");
    Require(timestamp.has_value(), "test canonical timestamp should parse");
    spectiary::SampleLabelingTaskCanonicalMetadata metadata;
    metadata.created_at = *timestamp;
    metadata.modified_at = *timestamp;
    metadata.origin.kind = "manual";
    return metadata;
}

void RequireResolvedSequencePosition(
    const spectiary::SourceCollectionNavigationView& view,
    std::optional<std::size_t> expected_zero_based_position,
    std::size_t expected_sequence_length,
    std::string_view message)
{
    Require(
        view.resolved_sequence_position.zero_based_position ==
                expected_zero_based_position &&
            view.resolved_sequence_position.sequence_length ==
                expected_sequence_length,
        message);
}

std::string Utf8(std::u8string_view value)
{
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

struct LoadedSourceSnapshot {
    std::filesystem::path path;
    std::size_t index = 0;
};

struct SourceFixture {
    std::filesystem::path path;
    std::size_t sample_count = 0;
};

std::filesystem::path UniqueTempPath(std::string_view suffix)
{
    static std::atomic_uint64_t next_id = 0;
    // The run directory already identifies the suite's fixtures. Keep filenames
    // short enough for derived persistence paths when TEMP is nested deeply.
    std::filesystem::path path = test_root / "fixture_";
    path += std::to_string(next_id.fetch_add(1));
    path += std::string(suffix);
    return path;
}

void TouchFile(const std::filesystem::path& path)
{
    std::ofstream stream(path);
    Require(stream.good(), "could not create source fixture file");
}

void WriteTextFile(const std::filesystem::path& path, std::string_view contents)
{
    std::ofstream stream(path, std::ios::trunc);
    Require(stream.good(), "could not open text file for writing");
    stream << contents;
    Require(stream.good(), "could not write text file");
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    Require(stream.good(), "could not open text file for reading");
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void WriteUnicodeNameNpy(
    const std::filesystem::path& path,
    std::initializer_list<std::string_view> values,
    std::size_t code_units)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open name NPY fixture for writing");

    std::string header = "{'descr': '<U";
    header += std::to_string(code_units);
    header += "', 'fortran_order': False, 'shape': (";
    header += std::to_string(values.size());
    header += ",), }";

    constexpr std::size_t kPreambleSize = 10;
    const std::size_t header_with_newline = header.size() + 1;
    const std::size_t padding = (16 - ((kPreambleSize + header_with_newline) % 16)) % 16;
    header.append(padding, ' ');
    header.push_back('\n');
    Require(header.size() <= std::numeric_limits<std::uint16_t>::max(), "test NPY header is too large");

    constexpr unsigned char kMagic[] = {0x93, 'N', 'U', 'M', 'P', 'Y'};
    stream.write(reinterpret_cast<const char*>(kMagic), static_cast<std::streamsize>(sizeof(kMagic)));
    constexpr char kVersion[] = {1, 0};
    stream.write(kVersion, static_cast<std::streamsize>(sizeof(kVersion)));

    const auto header_length = static_cast<std::uint16_t>(header.size());
    const char length_bytes[] = {
        static_cast<char>(header_length & 0xffU),
        static_cast<char>((header_length >> 8U) & 0xffU),
    };
    stream.write(length_bytes, static_cast<std::streamsize>(sizeof(length_bytes)));
    stream.write(header.data(), static_cast<std::streamsize>(header.size()));
    for (std::string_view value : values) {
        for (std::size_t index = 0; index < code_units; ++index) {
            const std::uint32_t code_point =
                index < value.size() ? static_cast<unsigned char>(value[index]) : 0U;
            const char bytes[] = {
                static_cast<char>(code_point & 0xffU),
                static_cast<char>((code_point >> 8U) & 0xffU),
                static_cast<char>((code_point >> 16U) & 0xffU),
                static_cast<char>((code_point >> 24U) & 0xffU),
            };
            stream.write(bytes, static_cast<std::streamsize>(sizeof(bytes)));
        }
    }
    Require(stream.good(), "could not write name NPY fixture");
}

std::filesystem::path CompanionNamePath(const std::filesystem::path& source_path)
{
    return source_path.parent_path() / (source_path.stem().string() + "_name.npy");
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::string AnnotationSourceId(const std::filesystem::path& path)
{
    return "annotation:" + PathToUtf8(path);
}

bool HasSortSource(
    const spectiary::SourceCollectionSampleSortingView& view,
    std::string_view source_id)
{
    return std::any_of(view.sources.begin(), view.sources.end(), [source_id](const auto& source) {
        return source.id == source_id;
    });
}

bool HasAvailableSortSource(
    const spectiary::SourceCollectionSampleSortingView& view,
    std::string_view source_id)
{
    return std::any_of(view.available_sources.begin(), view.available_sources.end(), [source_id](const auto& source) {
        return source.id == source_id;
    });
}

bool HasPersistenceMessage(
    const spectiary::LocalUserStateHealthView& health,
    spectiary::LocalUserStateArea area,
    std::optional<
        spectiary::LocalUserStateHealthMessageKind>
        kind = std::nullopt)
{
    return std::any_of(
        health.messages.begin(),
        health.messages.end(),
        [area, kind](
            const spectiary::
                LocalUserStateHealthMessage& message) {
            return message.area == area &&
                   (!kind || message.kind == *kind);
        });
}

const spectiary::SourceCollectionSampleSortSourceView* FindSortSource(
    const spectiary::SourceCollectionSampleSortingView& view,
    std::string_view source_id)
{
    const auto match = std::find_if(view.sources.begin(), view.sources.end(), [source_id](const auto& source) {
        return source.id == source_id;
    });
    return match == view.sources.end() ? nullptr : &*match;
}

void SaveLabelResultFixture(
    const std::filesystem::path& path,
    std::string task_id,
    std::string task_name,
    std::vector<int> values,
    spectiary::SampleLabelSet label_set,
    bool write_metadata)
{
    spectiary::SampleLabelingTask task =
        spectiary::CreateSampleLabelingTask(std::move(task_id), std::move(task_name), values.size());
    task.values.Complete() = std::move(values);
    task.label_set = std::move(label_set);
    std::string error;
    const spectiary::test_support::LegacyFixtureIo adapter;
    Require(
        adapter.SaveLabelArray(path, task, &error),
        error.empty() ? "label result fixture NPY should save" : error);
    if (write_metadata) {
        error.clear();
        Require(
            adapter.SaveLabelMetadata(path, task, nullptr, &error),
            error.empty() ? "label result fixture metadata should save" : error);
    }
}

spectiary::SpectrumSnapshotHandle MakeSnapshot(
    const std::filesystem::path& path,
    std::size_t spectrum_count,
    std::size_t current_index,
    std::string format = "test")
{
    auto snapshot = std::make_shared<spectiary::SpectrumSnapshot>();
    snapshot->source.id = "source";
    snapshot->source.display_name = "source";
    snapshot->source.path = path;
    snapshot->source.metadata.push_back(
        spectiary::SpectrumMetadataEntry{
            "format",
            format,
            format});
    snapshot->collection.spectrum_count = spectrum_count;
    snapshot->collection.current_index = current_index;
    snapshot->collection.can_move_previous = current_index > 0;
    snapshot->collection.can_move_next = current_index + 1 < spectrum_count;
    snapshot->current_spectrum.name = "sample-" + std::to_string(current_index + 1);
    snapshot->capabilities.can_plot_current_spectrum = true;
    snapshot->capabilities.can_switch_spectrum = spectrum_count > 1;
    return snapshot;
}

void ActivateCanonicalFixtureSource(
    spectiary::SampleLabelingController& controller,
    const std::filesystem::path& source_path,
    const spectiary::SourceCollectionContext& context)
{
    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(
            source_path,
            context.identity.spectrum_count,
            0);
    controller.ActivateSource(
        context.identity,
        spectiary::BuildSampleLabelingCanonicalSourceDescriptor(
            *snapshot,
            context));
}

void TestCanonicalSaveRejectsReservedStorage()
{
    using namespace spectiary;
    const auto base = UniqueTempPath("_canonical_storage_admission");
    for (const auto profile : {StorageProfile::Portable, StorageProfile::LocalAppData}) {
        const auto root = base / (profile == StorageProfile::Portable ? "portable" : "local");
        const auto paths = RuntimePathsForDeployment({.storage_profile = profile}, {
            .executable_path = root / "app.exe",
            .local_app_data_user_state_root = root,
        });
        std::filesystem::create_directories(root);
        const auto source = root / "source.npy";
        TouchFile(source);
        SourceCollectionContext context;
        context.identity = {Utf8(root.u8string()), "source", "source-fingerprint", "context-fingerprint", 3};
        SampleLabelingController controller(paths.sample_labeling_state_path, paths);
        ActivateCanonicalFixtureSource(controller, source, context);
        Require(controller.CreateTask("Storage admission").accepted, "create draft for storage admission");
        Require(controller.UpsertActiveLabel({1, "one", 'o'}).changed, "create draft label");
        Require(controller.AssignLabel(0, 1).write.changed, "retain user work before Save As");
        const auto task_id = controller.View().active_task->task_id;
        for (const auto* role : {"config", "state", "logs", "unsaved"}) {
            const auto target = root / role / "task.asdf";
            const auto rejected = controller.SaveActiveTemporaryTaskToOutput(target);
            Require(!rejected.accepted && rejected.issue == SampleLabelingOperationResult::Issue::UserFilePathRejected &&
                    !std::filesystem::exists(target),
                "Save As must reject reserved storage without publishing");
            Require(!controller.AdoptCanonicalAsdfTask(task_id, target).accepted,
                "canonical adoption must reject reserved storage");
            Require(controller.View().active_task && !controller.View().active_task->persistence.output_path,
                "rejection must retain the in-memory draft");
        }
        const auto canonical = root / "my-work/task.asdf";
        Require(controller.SaveActiveTemporaryTaskToOutput(canonical).output_saved,
            "canonical Save As remains allowed in a user subdirectory of application root");
        std::ifstream before_stream(canonical, std::ios::binary);
        const std::string before(std::istreambuf_iterator<char>(before_stream), {});
        before_stream.close();
        MigrateLegacyApplicationStorage(paths);
        std::ifstream after_stream(canonical, std::ios::binary);
        Require(std::string(std::istreambuf_iterator<char>(after_stream), {}) == before,
            "storage migration cannot modify an actual canonical ASDF");
    }
    std::filesystem::remove_all(base);
}

spectiary::PreparedSampleWorkflowState PrepareWorkflow(
    const spectiary::SpectrumSnapshotHandle& snapshot,
    const spectiary::SourceCollectionContext& context,
    std::size_t prepared_index,
    std::filesystem::path labeling_cache = {},
    std::filesystem::path workflow_cache = {})
{
    return spectiary::PrepareSampleWorkflowState(
        *snapshot,
        context,
        prepared_index,
        {std::move(labeling_cache), std::move(workflow_cache)});
}

using SnapshotLoader = std::function<spectiary::SpectrumSnapshotHandle(
    const std::filesystem::path&,
    std::size_t)>;

spectiary::SourceCollectionLoadDependencies LoadingDependencies(
    SnapshotLoader loader,
    const std::filesystem::path& navigation_cache,
    const std::filesystem::path& labeling_cache,
    const std::filesystem::path& workflow_cache)
{
    spectiary::SourceCollectionLoadDependencies adapters;
    adapters.workflow_cache_paths = spectiary::test_support::EmptyWorkflowCachePaths();
    SnapshotLoader folder_loader = loader;
    adapters.snapshot_loader = [loader = std::move(loader)](
                                   const std::filesystem::path& path,
                                   std::size_t spectrum_index,
                                   const auto& canceled) {
        if (canceled()) {
            throw spectiary::SourceCollectionPreparationCanceled();
        }
        return loader(path, spectrum_index);
    };
    adapters.folder_snapshot_loader = [loader = std::move(folder_loader)](
                                          const std::filesystem::path& path,
                                          std::size_t spectrum_index,
                                          const spectiary::SourceCollectionFolderListing&,
                                          const auto& canceled) {
        if (canceled()) {
            throw spectiary::SourceCollectionPreparationCanceled();
        }
        return loader(path, spectrum_index);
    };
    adapters.workflow_cache_paths = {
        labeling_cache,
        workflow_cache,
        navigation_cache,
    };
    return adapters;
}

class PreparedSession final : public spectiary::SourceCollectionSession {
public:
    PreparedSession(
        SnapshotLoader loader,
        std::filesystem::path source_session_cache,
        std::filesystem::path navigation_cache,
        std::filesystem::path labeling_cache,
        std::filesystem::path workflow_cache,
        spectiary::SampleLabelingController::
            CanonicalDocumentPublisher
                canonical_document_publisher = {},
        spectiary::SampleLabelingController::
            CanonicalValuesPublisher
                canonical_values_publisher = {},
        const spectiary::RuntimePaths& runtime_paths = {})
        : spectiary::SourceCollectionSession(
              source_session_cache,
              navigation_cache,
              labeling_cache,
              workflow_cache,
              spectiary::SampleLabelingStateCacheLoadPolicy::
                  AllowPersistentOutputs,
              std::move(canonical_document_publisher),
              std::move(canonical_values_publisher),
              runtime_paths),
          queue_(spectiary::MakeSourceCollectionLoadQueueForTesting(
              LoadingDependencies(
                  std::move(loader),
                  navigation_cache,
                  labeling_cache,
                  workflow_cache)))
    {
        RestorePreparedSources();
    }

    [[nodiscard]] spectiary::SourceCollectionSessionResult Open(
        const std::filesystem::path& path,
        std::size_t spectrum_index = 0,
        std::vector<std::filesystem::path> annotation_paths = {})
    {
        return ServiceFollowUps(CommitPrepared(
            path,
            spectrum_index,
            std::move(annotation_paths)));
    }

    [[nodiscard]] spectiary::SourceCollectionSessionResult SubmitAndService(
        spectiary::SourceCollectionSessionIntent intent)
    {
        return ServiceFollowUps(
            spectiary::SourceCollectionSession::Submit(std::move(intent)));
    }

private:
    [[nodiscard]] spectiary::SourceCollectionSessionResult CommitPrepared(
        const std::filesystem::path& path,
        std::size_t spectrum_index,
        std::vector<std::filesystem::path> annotation_paths)
    {
        spectiary::SourceCollectionLoadRequest request{
            .path = path,
            .spectrum_index = spectrum_index,
            .annotation_paths = std::move(annotation_paths),
        };
        if (std::optional<spectiary::SourceCollectionLoadHint> hint =
                LoadHintForSource(path, spectrum_index)) {
            request.reuse = std::move(hint->reuse);
        }
        auto completion = spectiary::test_support::WaitForSourceCompletion(
            queue_, queue_.Enqueue(std::move(request)));
        if (!completion.prepared) {
            throw std::runtime_error(completion.error_message);
        }
        auto prepared = std::move(*completion.prepared);
        return OpenPreparedSource(
            std::move(prepared.path),
            prepared.spectrum_index,
            std::move(prepared.snapshot),
            std::move(prepared.payload),
            std::move(prepared.folder_listing_generation),
            std::move(prepared.context_reuse_proof));
    }

    [[nodiscard]] spectiary::SourceCollectionSessionResult ServiceFollowUps(
        spectiary::SourceCollectionSessionResult result)
    {
        for (std::size_t attempt = 0; result.follow_up_spectrum_index; ++attempt) {
            Require(attempt < 8, "prepared session follow-up should converge");
            const spectiary::SpectrumSnapshotHandle snapshot =
                CurrentSourceSnapshot();
            Require(
                snapshot && !snapshot->source.path.empty(),
                "prepared follow-up should retain a source path");
            const std::filesystem::path path = snapshot->source.path;
            const std::size_t spectrum_index =
                *result.follow_up_spectrum_index;
            result.follow_up_spectrum_index.reset();
            spectiary::SourceCollectionSessionResult follow_up =
                CommitPrepared(
                    path,
                    spectrum_index,
                    AnnotationPathsForSource(path));
            spectiary::MergeSourceCollectionSessionAction(
                result.action,
                follow_up.action);
            result.loaded = result.loaded || follow_up.loaded;
            result.view_invalidated =
                result.view_invalidated ||
                follow_up.view_invalidated;
            if (!follow_up.message.empty()) {
                result.message = std::move(follow_up.message);
            }
            if (follow_up.canceled_source_follow_up_path) {
                result.canceled_source_follow_up_path =
                    std::move(follow_up.canceled_source_follow_up_path);
            }
            result.background_retirement.insert(
                result.background_retirement.end(),
                std::make_move_iterator(follow_up.background_retirement.begin()),
                std::make_move_iterator(follow_up.background_retirement.end()));
            result.follow_up_spectrum_index =
                follow_up.follow_up_spectrum_index;
        }
        return result;
    }

    void RestorePreparedSources()
    {
        std::optional<spectiary::SourceCollectionDeferredRestorePlan> restore =
            TakeDeferredRestorePlan();
        if (!restore) {
            return;
        }
        std::optional<std::filesystem::path> active_source_path;
        if (restore->active_source_index &&
            *restore->active_source_index < restore->sources.size()) {
            active_source_path =
                restore->sources[*restore->active_source_index].path;
        }
        for (const spectiary::SourceCollectionSavedSource& source :
             restore->sources) {
            try {
                (void)Open(
                    source.path,
                    source.last_spectrum_index,
                    source.annotation_paths);
            } catch (const std::exception&) {
                // The production activation transaction records a
                // failed preparation and continues the restore batch.
            }
        }
        if (active_source_path) {
            const spectiary::SourceCollectionSessionView view = View();
            const auto active = std::find_if(
                view.sources.begin(),
                view.sources.end(),
                [&active_source_path](const auto& source) {
                    return source.path == *active_source_path;
                });
            if (active != view.sources.end()) {
                const std::size_t active_index = static_cast<std::size_t>(
                    std::distance(view.sources.begin(), active));
                (void)spectiary::SourceCollectionSession::Submit(
                    spectiary::SourceCollectionSessionIntent::
                        EditSourceCollection(
                            spectiary::SourceCollectionIntent::SwitchActive(
                                active_index)));
            }
        }
        FinishDeferredRestore();
    }

    spectiary::SourceCollectionLoadQueue queue_;
};

PreparedSession MakeSession(
    std::vector<std::size_t>& loaded_indices,
    const std::filesystem::path& source_path,
    std::size_t sample_count)
{
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    return PreparedSession(
        [&loaded_indices, source_path, sample_count](
            const std::filesystem::path& path,
            std::size_t spectrum_index) {
            Require(path == source_path, "session should reload the active source path");
            loaded_indices.push_back(spectrum_index);
            return MakeSnapshot(source_path, sample_count, spectrum_index);
        },
        {},
        navigation_cache,
        labeling_cache,
        UniqueTempPath("_workflow.json"));
}

PreparedSession MakeMultiSourceSession(
    std::vector<LoadedSourceSnapshot>& loaded_snapshots,
    const std::filesystem::path& first_source_path,
    std::size_t first_sample_count,
    const std::filesystem::path& second_source_path,
    std::size_t second_sample_count)
{
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    return PreparedSession(
        [&loaded_snapshots, first_source_path, first_sample_count, second_source_path, second_sample_count](
            const std::filesystem::path& path,
            std::size_t spectrum_index) {
            Require(
                path == first_source_path || path == second_source_path,
                "multi-source session should reload a known source path");
            loaded_snapshots.push_back(LoadedSourceSnapshot{path, spectrum_index});
            if (path == first_source_path) {
                return MakeSnapshot(first_source_path, first_sample_count, spectrum_index);
            }
            return MakeSnapshot(second_source_path, second_sample_count, spectrum_index);
        },
        {},
        navigation_cache,
        labeling_cache,
        UniqueTempPath("_workflow.json"));
}

PreparedSession MakePersistentMultiSourceSession(
    std::vector<LoadedSourceSnapshot>& loaded_snapshots,
    const std::filesystem::path& source_session_cache,
    const std::filesystem::path& navigation_cache,
    const std::filesystem::path& labeling_cache,
    const std::filesystem::path& first_source_path,
    std::size_t first_sample_count,
    const std::filesystem::path& second_source_path,
    std::size_t second_sample_count)
{
    return PreparedSession(
        [&loaded_snapshots, first_source_path, first_sample_count, second_source_path, second_sample_count](
            const std::filesystem::path& path,
            std::size_t spectrum_index) {
            Require(
                path == first_source_path || path == second_source_path,
                "persistent session should reload a known source path");
            loaded_snapshots.push_back(LoadedSourceSnapshot{path, spectrum_index});
            if (path == first_source_path) {
                return MakeSnapshot(first_source_path, first_sample_count, spectrum_index);
            }
            return MakeSnapshot(second_source_path, second_sample_count, spectrum_index);
        },
        source_session_cache,
        navigation_cache,
        labeling_cache,
        UniqueTempPath("_workflow.json"));
}

PreparedSession MakePersistentSession(
    std::vector<LoadedSourceSnapshot>& loaded_snapshots,
    const std::filesystem::path& source_session_cache,
    const std::filesystem::path& navigation_cache,
    const std::filesystem::path& labeling_cache,
    std::vector<SourceFixture> fixtures,
    const spectiary::RuntimePaths& runtime_paths = {})
{
    return PreparedSession(
        [&loaded_snapshots, fixtures = std::move(fixtures)](
            const std::filesystem::path& path,
            std::size_t spectrum_index) {
            const auto match = std::find_if(fixtures.begin(), fixtures.end(), [&path](const SourceFixture& fixture) {
                return fixture.path == path;
            });
            Require(
                match != fixtures.end(),
                std::string("persistent session should reload a known source path: ") +
                    path.string());
            loaded_snapshots.push_back(LoadedSourceSnapshot{path, spectrum_index});
            return MakeSnapshot(match->path, match->sample_count, spectrum_index);
        },
        source_session_cache,
        navigation_cache,
        labeling_cache,
        UniqueTempPath("_workflow.json"),
        {}, {}, runtime_paths);
}

PreparedSession MakeWorkflowPersistentSession(
    std::vector<LoadedSourceSnapshot>& loaded_snapshots,
    const std::filesystem::path& navigation_cache,
    const std::filesystem::path& labeling_cache,
    const std::filesystem::path& workflow_cache,
    const std::filesystem::path& source_path,
    std::size_t sample_count)
{
    return PreparedSession(
        [&loaded_snapshots, source_path, sample_count](
            const std::filesystem::path& path,
            std::size_t spectrum_index) {
            Require(path == source_path, "workflow-persistent session should reload the active source path");
            loaded_snapshots.push_back(LoadedSourceSnapshot{path, spectrum_index});
            return MakeSnapshot(source_path, sample_count, spectrum_index);
        },
        std::filesystem::path{},
        navigation_cache,
        labeling_cache,
        workflow_cache);
}

spectiary::SourceCollectionSessionResult Submit(
    spectiary::SourceCollectionSession& session,
    spectiary::SourceCollectionSessionIntent intent)
{
    return session.Submit(std::move(intent));
}

spectiary::SourceCollectionSessionResult Submit(
    PreparedSession& session,
    spectiary::SourceCollectionSessionIntent intent)
{
    return session.SubmitAndService(
        std::move(intent));
}

struct PreparedSourceOpenRequest {
    std::filesystem::path path;
    std::size_t spectrum_index = 0;
};

PreparedSourceOpenRequest OpenSourceCollection(
    std::filesystem::path path,
    std::size_t spectrum_index = 0)
{
    return {
        std::move(path),
        spectrum_index,
    };
}

spectiary::SourceCollectionSessionResult Submit(
    PreparedSession& session,
    PreparedSourceOpenRequest request)
{
    return session.Open(
        request.path,
        request.spectrum_index);
}

spectiary::SourceCollectionSessionIntent SwitchSourceCollection(std::size_t source_index)
{
    return spectiary::SourceCollectionSessionIntent::EditSourceCollection(
        spectiary::SourceCollectionIntent::SwitchActive(source_index));
}

spectiary::SourceCollectionSessionIntent RemoveSourceCollection(std::size_t source_index)
{
    return spectiary::SourceCollectionSessionIntent::EditSourceCollection(
        spectiary::SourceCollectionIntent::Remove(source_index));
}

spectiary::SourceCollectionSessionIntent AddReadOnlyAnnotation(std::filesystem::path path)
{
    return spectiary::SourceCollectionSessionIntent::EditSourceCollection(
        spectiary::SourceCollectionIntent::AddReadOnlyAnnotationResult(std::move(path)));
}

spectiary::SourceCollectionSessionIntent RemoveReadOnlyAnnotation(std::filesystem::path path)
{
    return spectiary::SourceCollectionSessionIntent::EditSourceCollection(
        spectiary::SourceCollectionIntent::RemoveReadOnlyAnnotationResult(std::move(path)));
}

spectiary::SourceCollectionSessionIntent RenameAnnotationDisplayName(
    std::filesystem::path path,
    std::string display_name)
{
    return spectiary::SourceCollectionSessionIntent::EditSourceCollection(
        spectiary::SourceCollectionIntent::RenameAnnotationResultDisplayName(
            std::move(path),
            std::move(display_name)));
}

spectiary::SourceCollectionSessionIntent AddSampleFilterSource(std::string source_id)
{
    return spectiary::SourceCollectionSessionIntent::ApplySampleFiltering(
        spectiary::SampleFilteringIntent::AddSource(std::move(source_id)));
}

spectiary::SourceCollectionSessionIntent RemoveSampleFilterSource(std::string source_id)
{
    return spectiary::SourceCollectionSessionIntent::ApplySampleFiltering(
        spectiary::SampleFilteringIntent::RemoveSource(std::move(source_id)));
}

std::filesystem::path AddPlainIntegerFilterAnnotation(
    spectiary::SourceCollectionSession& session,
    std::vector<int> values,
    std::string_view suffix = "_filter.npy")
{
    const std::filesystem::path annotation_path = UniqueTempPath(suffix);
    SaveLabelResultFixture(
        annotation_path,
        "filter",
        "Filter",
        std::move(values),
        spectiary::SampleLabelSet{},
        false);
    const spectiary::SourceCollectionSessionResult result =
        Submit(session, AddReadOnlyAnnotation(annotation_path));
    Require(result.loaded, "plain integer filter annotation should load");
    return annotation_path;
}

std::string AddPlainIntegerSampleFilterSource(
    spectiary::SourceCollectionSession& session,
    std::vector<int> values,
    std::string_view suffix = "_filter.npy")
{
    const std::filesystem::path annotation_path =
        AddPlainIntegerFilterAnnotation(session, std::move(values), suffix);
    const std::string source_id = AnnotationSourceId(annotation_path);
    const spectiary::SourceCollectionSessionResult result =
        Submit(session, AddSampleFilterSource(source_id));
    Require(session.View().filter.sources.size() == 1, "sample filter source should be explicitly added");
    Require(session.View().filter.sources[0].id == source_id, "added filter source should use the annotation id");
    return source_id;
}

spectiary::SourceCollectionSessionIntent MoveSampleNavigation(spectiary::SampleNavigationRequest request)
{
    return spectiary::SourceCollectionSessionIntent::UpdateSampleNavigation(
        spectiary::SampleNavigationIntent::Move(std::move(request)));
}

spectiary::SourceCollectionSessionIntent SetSampleNameQuery(std::string query)
{
    return spectiary::SourceCollectionSessionIntent::UpdateSampleNavigation(
        spectiary::SampleNavigationIntent::SetSampleNameQuery(std::move(query)));
}

spectiary::SourceCollectionSessionIntent StartOrResumeTemporaryLabelingTask()
{
    return spectiary::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        spectiary::ActiveSampleWorkflowIntent::StartOrResumeTemporaryLabelingTask());
}

spectiary::SourceCollectionSessionIntent RenameActiveLabelingTask(
    std::string expected_task_id,
    std::string requested_name)
{
    return spectiary::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        spectiary::ActiveSampleWorkflowIntent::RenameActiveLabelingTask(
            std::move(expected_task_id),
            std::move(requested_name)));
}

spectiary::SourceCollectionSessionIntent RecoverTemporaryLabelingTask(
    std::string source_identity,
    std::string task_id)
{
    return spectiary::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        spectiary::ActiveSampleWorkflowIntent::RecoverTemporaryLabelingTask(
            std::move(source_identity),
            std::move(task_id)));
}

spectiary::SourceCollectionSessionIntent DeleteTemporaryLabelingTask(
    std::string source_identity,
    std::string task_id)
{
    return spectiary::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        spectiary::ActiveSampleWorkflowIntent::DeleteTemporaryLabelingTask(
            std::move(source_identity),
            std::move(task_id)));
}

spectiary::SourceCollectionSessionIntent ActivateLabelingTaskFromAnnotation(std::filesystem::path annotation_path)
{
    return spectiary::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        spectiary::ActiveSampleWorkflowIntent::ActivateLabelingTaskFromAnnotation(std::move(annotation_path)));
}

spectiary::SourceCollectionSessionIntent DeleteActiveLabelingTask()
{
    return spectiary::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        spectiary::ActiveSampleWorkflowIntent::DeleteActiveLabelingTask());
}

spectiary::SourceCollectionSessionIntent SetActiveLabelingOutputPath(std::filesystem::path output_path)
{
    return spectiary::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        spectiary::ActiveSampleWorkflowIntent::SetActiveLabelingOutputPath(std::move(output_path)));
}

spectiary::SourceCollectionSessionIntent ExportActiveLabels(
    std::filesystem::path output_path,
    spectiary::SampleLabelExportFormat format =
        spectiary::SampleLabelExportFormat::Npy)
{
    return spectiary::SourceCollectionSessionIntent::
        ChangeActiveSampleWorkflow(
            spectiary::ActiveSampleWorkflowIntent::
                ExportActiveLabels(
                    std::move(output_path),
                    format));
}

spectiary::SourceCollectionSessionIntent UpsertActiveLabel(spectiary::SampleLabelDefinition label)
{
    return spectiary::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        spectiary::ActiveSampleWorkflowIntent::UpsertActiveLabel(std::move(label)));
}

spectiary::SourceCollectionSessionIntent UpdateActiveLabel(
    int original_code,
    spectiary::SampleLabelDefinition label,
    bool allow_used_code_change)
{
    return spectiary::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        spectiary::ActiveSampleWorkflowIntent::UpdateActiveLabel(
            original_code,
            std::move(label),
            allow_used_code_change));
}

spectiary::SourceCollectionSessionIntent RemoveActiveLabel(int code)
{
    return spectiary::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        spectiary::ActiveSampleWorkflowIntent::RemoveActiveLabel(code));
}

spectiary::SourceCollectionSessionIntent SetActiveLabelingAutoAdvance(bool enabled)
{
    return spectiary::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        spectiary::ActiveSampleWorkflowIntent::SetActiveLabelingAutoAdvance(enabled));
}

spectiary::SourceCollectionSessionIntent DeactivateActiveLabelingTask()
{
    return spectiary::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        spectiary::ActiveSampleWorkflowIntent::DeactivateActiveLabelingTask());
}

spectiary::SourceCollectionSessionIntent AssignActiveLabelToCurrentSample(int code)
{
    return spectiary::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        spectiary::ActiveSampleWorkflowIntent::AssignActiveLabelToCurrentSample(code));
}

spectiary::SourceCollectionSessionIntent ClearActiveLabelForCurrentSample()
{
    return spectiary::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        spectiary::ActiveSampleWorkflowIntent::ClearActiveLabelForCurrentSample());
}

spectiary::SourceCollectionSessionIntent UndoLastLabelWrite()
{
    return spectiary::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        spectiary::ActiveSampleWorkflowIntent::UndoLastLabelWrite());
}

spectiary::SourceCollectionSessionIntent SetFilterValueSelected(
    std::string source_id,
    std::string value_key,
    bool selected)
{
    return spectiary::SourceCollectionSessionIntent::ApplySampleFiltering(
        spectiary::SampleFilteringIntent::SetFilterValueSelected(
            std::move(source_id),
            std::move(value_key),
            selected));
}

spectiary::SourceCollectionSessionIntent ClearSampleSorting()
{
    return spectiary::SourceCollectionSessionIntent::ApplySampleSorting(
        spectiary::SampleSortingIntent::Clear());
}

spectiary::SourceCollectionSessionIntent AddSampleSortSource(std::string source_id)
{
    return spectiary::SourceCollectionSessionIntent::ApplySampleSorting(
        spectiary::SampleSortingIntent::AddSource(std::move(source_id)));
}

spectiary::SourceCollectionSessionIntent RemoveSampleSortSource(std::string source_id)
{
    return spectiary::SourceCollectionSessionIntent::ApplySampleSorting(
        spectiary::SampleSortingIntent::RemoveSource(std::move(source_id)));
}

spectiary::SourceCollectionSessionIntent SetSampleSortSource(std::string source_id)
{
    return spectiary::SourceCollectionSessionIntent::ApplySampleSorting(
        spectiary::SampleSortingIntent::SetSortSource(std::move(source_id)));
}

spectiary::SourceCollectionSessionIntent SetSampleSortDirection(
    spectiary::SampleNavigationSortDirection direction)
{
    return spectiary::SourceCollectionSessionIntent::ApplySampleSorting(
        spectiary::SampleSortingIntent::SetSortDirection(direction));
}

void TestNavigationReloadsSnapshotAndRemembersLabelingPosition()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);

    const spectiary::SourceCollectionSessionResult open_result =
        Submit(session, OpenSourceCollection(source_path, 0));
    const spectiary::SourceCollectionSessionAction& open_action = open_result.action;
    Require(open_action.snapshot_changed, "opening a source should change the displayed snapshot");
    Require(open_action.workflow_changed, "opening a source should activate a workflow identity");
    Require(open_action.navigation_inputs_changed, "opening a source should refresh navigation inputs");
    Require(open_result.view_invalidated, "opening a source should report session-view invalidation");
    Require(session.View().sources.size() == 1, "opening a source should add one source entry");
    const spectiary::SourceCollectionSourceView& source =
        session.View().sources.front();
    Require(
        source.type && *source.type == "test",
        "session projection should preserve the source type semantic value");
    Require(
        source.state ==
            spectiary::SourceCollectionSourceState::Loaded,
        "session projection should expose a typed source state");
    Require(session.View().snapshot->collection.current_index == 0, "opened snapshot should start at requested index");
    const auto& open_transition = session.View().sample_transition;
    Require(
        open_transition &&
            open_transition->reason ==
                spectiary::SourceCollectionSampleTransitionReason::SourceActivation &&
            !open_transition->from_sample_index &&
            open_transition->current_sample_index == 0 &&
            !open_transition->accepted_label_value,
        "opening a source should expose a source-activation presentation transition");

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(session.View().labeling.has_active_task, "active source should accept a labeling task");

    const spectiary::SourceCollectionSessionResult next_result =
        Submit(session, MoveSampleNavigation(
                            spectiary::SampleNavigationRequest::Next()));
    Require(next_result.navigation.target_found, "next navigation should find a target");
    Require(next_result.navigation.current_index == 1, "next navigation should move to row 1");
    Require(next_result.action.snapshot_changed, "moving to another sample should reload the snapshot");
    Require(next_result.action.navigation_inputs_changed, "moving should refresh navigation inputs");
    Require(next_result.view_invalidated, "navigation should report its complete view-invalidating outcome");
    Require(session.View().snapshot->collection.current_index == 1, "session should expose the reloaded snapshot");
    const auto& next_transition = session.View().sample_transition;
    Require(
        next_transition &&
            next_transition->reason ==
                spectiary::SourceCollectionSampleTransitionReason::Next &&
            next_transition->from_sample_index == 0 &&
            next_transition->current_sample_index == 1 &&
            !next_transition->accepted_label_value,
        "manual next should replace the presentation transition without label feedback");

    const spectiary::SourceCollectionLabelingView active_labeling = session.View().labeling;
    Require(active_labeling.has_active_task, "labeling task should remain active after navigation");
    Require(
        active_labeling.remembered_position && *active_labeling.remembered_position == 1,
        "session navigation should sync the labeling remembered position");
    Require(
        loaded_indices == std::vector<std::size_t>({0, 1}),
        "session should load only the opened and navigated sample snapshots");
}

void TestAssigningLabelAutoAdvancesInsideSession()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    const spectiary::SourceCollectionSessionResult workflow_result =
        Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        workflow_result.view_invalidated,
        "starting a labeling task should report its complete view-invalidating outcome");
    Require(
        Submit(
            session,
            UpsertActiveLabel(
                spectiary::SampleLabelDefinition{1, "bad", 'b'}))
            .changed,
        "bad label should be accepted");
    (void)Submit(session, SetActiveLabelingAutoAdvance(true));

    const spectiary::SourceCollectionSessionResult assign_result =
        Submit(session, AssignActiveLabelToCurrentSample(1));
    const spectiary::SourceCollectionSessionAction& assign_action = assign_result.action;
    Require(
        assign_result.label_write &&
            assign_result.label_write->write.accepted &&
            assign_result.label_write->write.changed &&
            assign_result.label_write->write.sample_index == 0 &&
            assign_result.label_write->write.previous_code ==
                spectiary::kUnlabeledSampleLabelCode &&
            assign_result.label_write->write.current_code == 1 &&
            assign_result.label_write->write.advance_requested &&
            assign_result.label_write->operation.state_save_scheduled,
        "session should preserve the real label write and persistence outcome across auto-advance");
    Require(assign_action.snapshot_changed, "auto-advance should load the next sample snapshot");
    Require(assign_action.navigation_inputs_changed, "auto-advance should refresh navigation inputs");
    Require(session.View().snapshot->collection.current_index == 1, "auto-advance should move to row 1");
    const auto& auto_advance_transition =
        session.View().sample_transition;
    Require(
        auto_advance_transition &&
            auto_advance_transition->reason ==
                spectiary::SourceCollectionSampleTransitionReason::
                    LabelingAutoAdvance &&
            auto_advance_transition->from_sample_index == 0 &&
            auto_advance_transition->current_sample_index == 1 &&
            auto_advance_transition->accepted_label_value == 1,
        "auto-advance should expose the written label and its real source and target rows");

    Require(session.View().labeling.has_active_task, "task should remain active after auto-advance");
    const spectiary::SourceCollectionSessionResult locate_result =
        Submit(session, MoveSampleNavigation(
                            spectiary::SampleNavigationRequest::LocateRow(0)));
    const auto& locate_transition = session.View().sample_transition;
    Require(
        locate_transition &&
            locate_transition->reason ==
                spectiary::SourceCollectionSampleTransitionReason::LocateRow &&
            locate_transition->from_sample_index == 1 &&
            locate_transition->current_sample_index == 0 &&
            !locate_transition->accepted_label_value,
        "manual row location should clear prior auto-advance label feedback");
    Require(session.View().labeling.current_code == 1, "current sample label should be written before advance");
    Require(
        loaded_indices == std::vector<std::size_t>({0, 1, 0}),
        "session should load only the opened, auto-advanced, and verified sample snapshots");
}

void TestTaskRenameIntentPreservesExactNameAndRejectsStaleTarget()
{
    const std::filesystem::path source_path =
        UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session =
        MakeSession(loaded_indices, source_path, 1);
    (void)Submit(
        session,
        OpenSourceCollection(source_path, 0));
    (void)Submit(
        session,
        StartOrResumeTemporaryLabelingTask());

    const std::string task_id =
        session.View().labeling.task_id;
    const std::string original_name =
        session.View().labeling.task_name;
    const spectiary::SourceCollectionSessionResult stale =
        Submit(
            session,
            RenameActiveLabelingTask(
                "00000000-0000-4000-8000-000000000000",
                "Wrong target"));
    Require(
        !stale.changed &&
            stale.labeling_issue ==
                spectiary::SampleLabelingOperationResult::Issue::
                    EditTargetChanged &&
            session.View().labeling.task_id == task_id &&
            session.View().labeling.task_name == original_name,
        "a stale expected task ID must not rename the active task");

    const std::string requested_name =
        "  \xE5\xA4\x8D\xE6\xA0\xB8 \xF0\x9F\x99\x82  ";
    const spectiary::SourceCollectionSessionResult renamed =
        Submit(
            session,
            RenameActiveLabelingTask(
                task_id,
                requested_name));
    Require(
        renamed.changed && renamed.view_invalidated &&
            session.View().labeling.task_id == task_id &&
            session.View().labeling.task_name == requested_name &&
            !session.View().labeling.output_path,
        "the rename intent must reach the guarded controller and preserve the requested UTF-8 bytes without changing task identity or ownership");
}

std::string ReadBinaryFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open binary file for reading");
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

std::vector<spectiary::CsvRecord> ReadCsvRecords(
    const std::filesystem::path& path)
{
    spectiary::BoundedCsvFileReader reader(path);
    std::vector<spectiary::CsvRecord> records;
    while (true) {
        spectiary::CsvRecordReadResult result =
            reader.ReadRecord();
        if (result.status ==
            spectiary::CsvRecordReadStatus::End) {
            return records;
        }
        Require(
            result.has_record(),
            result.error.message.empty()
                ? "could not read CSV fixture"
                : result.error.message);
        records.push_back(std::move(result.record));
    }
}

void WriteCsvRecords(
    const std::filesystem::path& path,
    const std::vector<spectiary::CsvRecord>& records)
{
    const spectiary::CsvRecordWriteResult result =
        spectiary::WriteCsvRecordsAtomically(
            path,
            records);
    Require(
        result.succeeded(),
        result.error.message.empty()
            ? "could not write CSV fixture"
            : result.error.message);
}

void TestLabelAutoAdvanceExposesNonAdjacentFilteredTransition()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_filtered_auto_advance.npy");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session =
        MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    const std::string filter_source_id =
        AddPlainIntegerSampleFilterSource(
            session,
            {1, 0, 1},
            "_filtered_auto_advance_values.npy");
    (void)Submit(
        session,
        SetFilterValueSelected(
            filter_source_id,
            "1",
            true));
    Require(
        session.View().navigation.current_index == 0 &&
            session.View().navigation.sequence_count == 2,
        "sample filter should retain row 0 and make row 2 its next eligible target");

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(
            session,
            UpsertActiveLabel(
                spectiary::SampleLabelDefinition{1, "accepted", 'a'}))
            .changed,
        "filtered auto-advance fixture should add its label");
    (void)Submit(session, SetActiveLabelingAutoAdvance(true));
    const spectiary::SourceCollectionSessionResult labeled =
        Submit(session, AssignActiveLabelToCurrentSample(1));
    const auto& transition = session.View().sample_transition;
    Require(
        labeled.label_write &&
            labeled.label_write->write.sample_index == 0 &&
            session.View().navigation.current_index == 2 &&
            transition &&
            transition->reason ==
                spectiary::SourceCollectionSampleTransitionReason::
                    LabelingAutoAdvance &&
            transition->from_sample_index == 0 &&
            transition->current_sample_index == 2 &&
            transition->accepted_label_value == 1,
        "filtered auto-advance must expose its actual non-adjacent source row, target row, and accepted label");
}

void TestLabelUndoRestoresValueAndAutoAdvancePosition()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{1, "bad", 'b'})).changed,
        "first undo fixture label should be accepted");
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{2, "good", 'g'})).changed,
        "second undo fixture label should be accepted");
    (void)Submit(session, SetActiveLabelingAutoAdvance(true));

    (void)Submit(session, AssignActiveLabelToCurrentSample(1));
    Require(session.View().snapshot->collection.current_index == 1, "first label should auto-advance to row 1");
    (void)Submit(session, AssignActiveLabelToCurrentSample(2));
    Require(session.View().snapshot->collection.current_index == 2, "second label should auto-advance to row 2");

    spectiary::SourceCollectionSessionResult undo_result = Submit(session, UndoLastLabelWrite());
    Require(undo_result.action.snapshot_changed, "undo should reload the sample affected by auto-advance");
    Require(session.View().snapshot->collection.current_index == 1, "undo should return to the second labeled row");
    Require(
        session.View().labeling.current_code == spectiary::kUnlabeledSampleLabelCode,
        "undo should restore the second row's previous unlabeled value");

    undo_result = Submit(session, UndoLastLabelWrite());
    Require(undo_result.action.snapshot_changed, "repeated undo should reload the previous affected sample");
    Require(session.View().snapshot->collection.current_index == 0, "repeated undo should return to the first row");
    Require(
        session.View().labeling.current_code == spectiary::kUnlabeledSampleLabelCode,
        "repeated undo should restore the first row's previous unlabeled value");
    Require(
        session.View().labeling.remembered_position && *session.View().labeling.remembered_position == 0,
        "undo should restore the task's remembered labeling position");

    undo_result = Submit(session, UndoLastLabelWrite());
    Require(!undo_result.action.snapshot_changed, "undo with an empty history should do nothing");
    Require(session.View().snapshot->collection.current_index == 0, "empty undo should keep the current sample");
}

void TestLabelUndoRestoresExistingValueAfterOverwriteAndClear()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 1);
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{1, "bad", 'b'})).changed,
        "overwrite undo fixture should accept the first label");
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{2, "good", 'g'})).changed,
        "overwrite undo fixture should accept the second label");

    (void)Submit(session, AssignActiveLabelToCurrentSample(1));
    (void)Submit(session, AssignActiveLabelToCurrentSample(2));
    Require(session.View().labeling.current_code == 2, "overwrite fixture should start with the new label");
    (void)Submit(session, UndoLastLabelWrite());
    Require(
        session.View().labeling.current_code == 1,
        "undo after overwrite should restore the previous existing label");

    (void)Submit(session, ClearActiveLabelForCurrentSample());
    Require(
        session.View().labeling.current_code == spectiary::kUnlabeledSampleLabelCode,
        "clear fixture should remove the existing label");
    (void)Submit(session, UndoLastLabelWrite());
    Require(
        session.View().labeling.current_code == 1,
        "undo after clear should restore the previous existing label");
}

void TestLabelUndoRestoresSampleOutsideActiveSampleNavigationSequence()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{1, "bad", 'b'})).changed,
        "sample-navigation-sequence undo fixture should accept a sample label");
    (void)Submit(session, AssignActiveLabelToCurrentSample(1));

    const std::filesystem::path annotation_path =
        AddPlainIntegerFilterAnnotation(session, {1, 2, 2}, "_undo_sample_filter.npy");
    const std::string source_id = AnnotationSourceId(annotation_path);
    (void)Submit(session, AddSampleFilterSource(source_id));
    (void)Submit(session, SetFilterValueSelected(source_id, "2", true));
    Require(
        session.View().snapshot->collection.current_index == 1,
        "sample filtering should move away from the labeled row excluded by the active sample navigation sequence");

    const spectiary::SourceCollectionSessionResult undo_result = Submit(session, UndoLastLabelWrite());
    Require(
        undo_result.action.snapshot_changed,
        "undo should reload its affected row outside the active sample navigation sequence");
    Require(
        session.View().snapshot->collection.current_index == 0,
        "session undo should restore the affected row even when it is outside the active sample navigation sequence");
    Require(
        session.View().labeling.current_code == spectiary::kUnlabeledSampleLabelCode,
        "session undo outside the sample navigation sequence should restore the previous value");
    Require(
        session.View().navigation.filter_active && !session.View().navigation.current_sample_in_filter,
        "undo should preserve active sample filtering while displaying the restored out-of-sequence row");
}

void TestLabelUndoHistoryInvalidatesWithTaskAndLabelDefinitions()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 1);
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{1, "bad", 'b'})).changed,
        "history invalidation fixture should accept the first label");
    (void)Submit(session, AssignActiveLabelToCurrentSample(1));

    (void)Submit(session, DeactivateActiveLabelingTask());
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    (void)Submit(session, UndoLastLabelWrite());
    Require(
        session.View().labeling.current_code == 1,
        "reactivating a task should not resurrect undo history from before deactivation");

    (void)Submit(session, ClearActiveLabelForCurrentSample());
    (void)Submit(session, AssignActiveLabelToCurrentSample(1));
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{2, "good", 'g'})).changed,
        "changing label definitions should succeed");
    (void)Submit(session, UndoLastLabelWrite());
    Require(
        session.View().labeling.current_code == 1,
        "changing label definitions should invalidate earlier label-write history");
}

void TestLabelUndoHistoryIsBoundedToTwoHundredFiftySixWrites()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 1);
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{1, "one", 'o'})).changed,
        "capacity fixture should accept label one");
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{2, "two", 't'})).changed,
        "capacity fixture should accept label two");

    for (int write_index = 0; write_index < 257; ++write_index) {
        const int code = write_index % 2 == 0 ? 1 : 2;
        (void)Submit(session, AssignActiveLabelToCurrentSample(code));
    }
    Require(session.View().labeling.current_code == 1, "257 alternating writes should end at label one");

    for (int undo_index = 0; undo_index < 256; ++undo_index) {
        (void)Submit(session, UndoLastLabelWrite());
    }
    Require(
        session.View().labeling.current_code == 1,
        "undoing the retained 256 entries should stop at the state after the discarded oldest write");
    const spectiary::SourceCollectionSessionResult exhausted = Submit(session, UndoLastLabelWrite());
    Require(!exhausted.action.snapshot_changed, "a 257th undo should find no retained history entry");
    Require(session.View().labeling.current_code == 1, "exhausted bounded history should preserve the current value");
}

void TestSavingCanonicalOwnerKeepsLabelUndoHistory()
{
    const std::filesystem::path source_path = UniqueTempPath("_samples.npy");
    TouchFile(source_path);
    const std::filesystem::path output_path =
        UniqueTempPath("_labels.asdf");

    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 1);
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{1, "bad", 'b'})).changed,
        "companion-save undo fixture should accept its sample label");
    (void)Submit(session, AssignActiveLabelToCurrentSample(1));

    const spectiary::SourceCollectionSessionResult save_result =
        Submit(session, SetActiveLabelingOutputPath(output_path));
    Require(save_result.action.navigation_inputs_changed, "canonical save should resync sample workflow inputs");
    Require(std::filesystem::exists(output_path), "canonical ASDF owner should be written");

    (void)Submit(session, UndoLastLabelWrite());
    Require(
        session.View().labeling.current_code == spectiary::kUnlabeledSampleLabelCode,
        "formalizing the active task as canonical ASDF must preserve label undo history");
}

void TestNoOpLabelUpsertKeepsLabelUndoHistory()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 1);
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    const spectiary::SampleLabelDefinition label{1, "bad", 'b'};
    Require(Submit(session, UpsertActiveLabel(label)).changed, "no-op fixture should accept its initial sample label");
    (void)Submit(session, AssignActiveLabelToCurrentSample(1));

    const spectiary::SourceCollectionSessionResult no_op = Submit(session, UpsertActiveLabel(label));
    Require(!no_op.changed, "saving an identical sample label definition should report no change");
    (void)Submit(session, UndoLastLabelWrite());
    Require(
        session.View().labeling.current_code == spectiary::kUnlabeledSampleLabelCode,
        "an identical sample label save must preserve label undo history");
}

void TestAnnotationFilterSelectionAppliesToNavigation()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    const std::filesystem::path annotation_path =
        AddPlainIntegerFilterAnnotation(session, {1, 2, 2}, "_quality.npy");
    const std::string source_id = AnnotationSourceId(annotation_path);

    spectiary::SourceCollectionFilterView filter_view = session.View().filter;
    Require(filter_view.sources.empty(), "annotation filter sources should not be selected by default");
    Require(filter_view.available_sources.size() == 1, "filterable annotation should be available to add");
    Require(filter_view.available_sources[0].id == source_id, "available source should use the annotation id");

    spectiary::SourceCollectionSessionResult result = Submit(session, AddSampleFilterSource(source_id));
    Require(
        result.view_invalidated,
        "adding a filter source should report its complete view-invalidating outcome");
    filter_view = session.View().filter;
    Require(filter_view.sources.size() == 1, "added annotation should become a sample filter source");
    Require(filter_view.available_sources.empty(), "added annotation should leave the add-source list");
    Require(filter_view.sources[0].id == source_id, "selected filter source should use the annotation id");
    Require(filter_view.sources[0].options.size() == 2, "annotation filter view should expose observed values");
    Require(
        filter_view.sources[0].options[0].key == "1" && filter_view.sources[0].options[0].sample_count == 1,
        "annotation filter view should count the first value");
    Require(
        filter_view.sources[0].options[1].key == "2" && filter_view.sources[0].options[1].sample_count == 2,
        "annotation filter view should count the second value");

    const std::uint64_t topology_revision_before_filter =
        session.View().navigation.sequence_topology_revision;
    result = Submit(
        session,
        SetFilterValueSelected(source_id, "2", true));
    Require(
        result.view_invalidated,
        "filter reconciliation should report its complete view-invalidating outcome");
    filter_view = session.View().filter;
    spectiary::SourceCollectionNavigationView navigation_view = session.View().navigation;
    const std::uint64_t filtered_topology_revision =
        navigation_view.sequence_topology_revision;
    Require(
        filtered_topology_revision >
            topology_revision_before_filter,
        "filter reconciliation should publish a new sequence topology revision");
    Require(navigation_view.filter_active, "annotation condition should activate navigation filtering");
    Require(navigation_view.filtered_sample_count == 2, "filter should include the two matching samples");
    Require(navigation_view.sequence_active, "filter should expose an active navigation sequence");
    Require(navigation_view.sequence_count == 2, "sequence should include the two matching samples");
    Require(
        navigation_view.current_index && *navigation_view.current_index == 1,
        "filter should move navigation to the first included sample");
    Require(
        navigation_view.current_sequence_position && *navigation_view.current_sequence_position == 0,
        "first included sample should be sequence position 0");
    Require(navigation_view.current_sample_in_filter, "reconciled current sample should be inside the filter");
    Require(!navigation_view.row_location_available, "row-index location should be disabled for filtered sequence");
    Require(result.action.snapshot_changed, "filter should load the first included sample snapshot");
    Require(session.View().snapshot->collection.current_index == 1, "filter should display the first included sample");
    const auto& filter_transition =
        session.View().sample_transition;
    Require(
        filter_transition &&
            filter_transition->reason ==
                spectiary::SourceCollectionSampleTransitionReason::
                    NavigationInputReconciliation &&
            filter_transition->from_sample_index == 0 &&
            filter_transition->current_sample_index == 1 &&
            !filter_transition->accepted_label_value,
        "sample-filter reconciliation should replace any previous label feedback");

    const spectiary::SourceCollectionSessionResult locate_action =
        Submit(session, MoveSampleNavigation(
                            spectiary::SampleNavigationRequest::LocateRow(0)));
    Require(locate_action.navigation.blocked_by_filter, "row locate should be blocked while filtering changes order");
    Require(!locate_action.navigation.target_found, "blocked row locate should not resolve");
    Require(locate_action.navigation.current_index == 1, "blocked row locate should keep the sequence current row");
    Require(
        session.View().navigation.sequence_topology_revision ==
            filtered_topology_revision,
        "a blocked cursor request must not change the sequence topology revision");

    const spectiary::SourceCollectionSessionResult next_action =
        Submit(session, MoveSampleNavigation(
                            spectiary::SampleNavigationRequest::Next()));
    Require(next_action.navigation.target_found, "filtered next should find a visible target");
    Require(next_action.navigation.current_index == 2, "filtered next should move to the next matching sample");
    Require(
        session.View().navigation.sequence_topology_revision ==
            filtered_topology_revision,
        "committing a deferred cursor move must retain the sequence topology revision");

    result = Submit(session, RemoveSampleFilterSource(source_id));
    Require(!session.View().navigation.filter_active, "removing the sample filter source should clear navigation filtering");
    Require(
        session.View().navigation.sequence_topology_revision >
            filtered_topology_revision,
        "clearing the active filter should publish a new sequence topology revision");
    Require(session.View().filter.sources.empty(), "removed source should leave no selected sample filters");
    Require(session.View().filter.available_sources.size() == 1, "removed source should return to the add-source list");
}

void TestResolvedSequencePositionTracksFinalNavigationSequence()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_resolved_sequence_position.npy");
    TouchFile(source_path);
    WriteUnicodeNameNpy(
        CompanionNamePath(source_path),
        {"delta", "alpha", "charlie", "bravo"},
        7);
    std::vector<std::size_t> loaded_indices;
    PreparedSession session =
        MakeSession(loaded_indices, source_path, 4);
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    RequireResolvedSequencePosition(
        session.View().navigation,
        0,
        4,
        "source-order navigation should expose row 0 as position 0 of 4");

    const std::filesystem::path annotation_path =
        AddPlainIntegerFilterAnnotation(
            session,
            {1, 0, 1, 1},
            "_resolved_sequence_position_filter.npy");
    const std::string filter_source_id =
        AnnotationSourceId(annotation_path);
    (void)Submit(
        session,
        AddSampleFilterSource(filter_source_id));
    (void)Submit(
        session,
        SetFilterValueSelected(
            filter_source_id,
            "1",
            true));
    RequireResolvedSequencePosition(
        session.View().navigation,
        0,
        3,
        "sample filtering should expose row 0 as position 0 of the three-row filtered sequence");

    (void)Submit(
        session,
        SetSampleSortSource("sample-name"));
    RequireResolvedSequencePosition(
        session.View().navigation,
        2,
        3,
        "filtering plus sorting should expose row 0 in the final resolved order [3, 2, 0]");

    (void)Submit(
        session,
        SetFilterValueSelected(
            filter_source_id,
            "1",
            false));
    RequireResolvedSequencePosition(
        session.View().navigation,
        3,
        4,
        "sample sorting alone should expose row 0 in the final order [1, 3, 2, 0]");

    (void)Submit(session, ClearSampleSorting());
    RequireResolvedSequencePosition(
        session.View().navigation,
        0,
        4,
        "clearing derived navigation inputs should restore source-order position");

    spectiary::SourceCollectionSession& deferred_session =
        session;
    const spectiary::SourceCollectionSessionResult reconciled =
        deferred_session.Submit(
            SetFilterValueSelected(
                filter_source_id,
                "0",
                true));
    Require(
        reconciled.follow_up_spectrum_index == 1 &&
            session.View().navigation.current_index == 0 &&
            !session.View().navigation.current_sample_in_filter,
        "filter reconciliation should keep presenting excluded row 0 while row 1 is pending");
    RequireResolvedSequencePosition(
        session.View().navigation,
        std::nullopt,
        1,
        "a presented current sample outside the resolved sequence must be unavailable without a roster-index fallback");

    Require(
        session.Open(source_path, 1).loaded,
        "the reconciled row 1 snapshot should commit");
    RequireResolvedSequencePosition(
        session.View().navigation,
        0,
        1,
        "committing reconciliation should expose row 1 as position 0 of the one-row sequence");
}

void TestLocalLabelingAnnotationCanBeSampleFilterSource()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path output_path = UniqueTempPath("_quality.asdf");
    TouchFile(source_path);
    WriteUnicodeNameNpy(
        CompanionNamePath(source_path),
        {"gamma", "alpha", "beta"},
        6);
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{1, "bad", 'b'})).changed,
        "bad label should be accepted");
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{2, "good", 'g'})).changed,
        "good label should be accepted");
    (void)Submit(session, AssignActiveLabelToCurrentSample(1));
    (void)Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::LocateRow(1)));
    (void)Submit(session, AssignActiveLabelToCurrentSample(2));
    (void)Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::LocateRow(2)));
    (void)Submit(session, AssignActiveLabelToCurrentSample(2));
    (void)Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::LocateRow(0)));

    spectiary::SourceCollectionSessionResult result =
        Submit(session, SetActiveLabelingOutputPath(output_path));
    const std::string canonical_bytes_after_formalization =
        ReadBinaryFile(output_path);
    const std::string source_id =
        "labeling:" + session.View().labeling.task_id;
    Require(
        session.View().navigation.current_annotations.size() == 1 &&
            session.View().navigation.current_annotations[0].relationship ==
                spectiary::SampleAnnotationWorkflowRelationship::LocalLabelingTask,
        "local labeling task should appear as an annotation row");
    Require(
        session.View().navigation.current_annotations[0].can_filter_samples,
        "local labeling annotation row should be draggable to sample filters");
    Require(session.View().filter.sources.empty(), "local labeling source should not be selected by default");
    Require(session.View().filter.available_sources.size() == 1, "local labeling source should be available to add");
    Require(session.View().filter.available_sources[0].id == source_id, "local labeling filter source should use task id");

    result = Submit(session, AddSampleFilterSource(source_id));
    Require(session.View().filter.sources.size() == 1, "local labeling source should be explicitly addable");
    Require(session.View().filter.sources[0].options.size() == 3, "labeling source should expose labels and unlabeled");
    Require(
        ReadBinaryFile(output_path) ==
            canonical_bytes_after_formalization,
        "adding a canonical filter source must not rewrite or reorder the ASDF roster and values");

    result = Submit(session, SetFilterValueSelected(source_id, "2", true));
    Require(session.View().navigation.filter_active, "local labeling sample filter should affect navigation");
    Require(session.View().navigation.sequence_count == 2, "local labeling filter should include the good samples");
    Require(
        session.View().navigation.current_index && *session.View().navigation.current_index == 1,
        "local labeling filter should move to the first matching sample");
    Require(
        ReadBinaryFile(output_path) ==
            canonical_bytes_after_formalization,
        "enabling a canonical filter value must not rewrite or reorder the ASDF roster and values");

    result = Submit(
        session,
        SetSampleSortSource("sample-name"));
    Require(
        result.action.navigation_inputs_changed &&
            session.View().sorting.active,
        "canonical roster fixture should activate sample-name sorting");
    result = Submit(
        session,
        SetSampleSortDirection(
            spectiary::SampleNavigationSortDirection::Descending));
    Require(
        result.action.navigation_inputs_changed &&
            session.View().navigation.current_sequence_position &&
            *session.View().navigation.current_sequence_position == 1,
        "descending sorting should reorder the filtered navigation sequence while retaining the current source row");
    Require(
        ReadBinaryFile(output_path) ==
            canonical_bytes_after_formalization,
        "sorting must not rewrite or reorder the canonical ASDF roster and values");
}

void TestRemovingLabelSelectedBySampleFilterReloadsReconciledSnapshot()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path output_path = UniqueTempPath("_quality.asdf");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{3, "review", 'r'})).changed,
        "review label should be accepted");
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{4, "keep", 'k'})).changed,
        "keep label should be accepted");
    (void)Submit(session, AssignActiveLabelToCurrentSample(3));
    (void)Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::LocateRow(1)));
    (void)Submit(session, AssignActiveLabelToCurrentSample(4));
    (void)Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::LocateRow(0)));
    (void)Submit(session, SetActiveLabelingOutputPath(output_path));

    const std::string source_id =
        "labeling:" + session.View().labeling.task_id;
    (void)Submit(session, AddSampleFilterSource(source_id));
    (void)Submit(session, SetFilterValueSelected(source_id, "3", true));
    (void)Submit(session, SetFilterValueSelected(source_id, "4", true));
    Require(
        session.View().navigation.current_index && *session.View().navigation.current_index == 0,
        "test should start on the sample using the label to remove");
    Require(
        session.View().snapshot && session.View().snapshot->collection.current_index == 0,
        "test snapshot should start on the sample using the label to remove");

    const spectiary::SourceCollectionSessionResult result = Submit(session, RemoveActiveLabel(3));
    Require(result.changed, "removing the label selected by a sample filter should change the task");
    Require(
        session.View().navigation.current_index && *session.View().navigation.current_index == 1,
        "removing the label currently selected by sample filtering should reconcile navigation to the "
        "remaining match");
    Require(
        session.View().snapshot && session.View().snapshot->collection.current_index == 1,
        "removing the label currently selected by sample filtering should reload the spectrum snapshot");
    Require(
        session.View().current_sample_snapshot &&
            session.View().current_sample_snapshot->collection.current_index == 1,
        "the current sample snapshot should match reconciled navigation");
    Require(result.action.snapshot_changed, "reloading the reconciled sample should report a snapshot change");
}

void TestRemovingLabelPrunesItsSampleFilterValue()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path output_path = UniqueTempPath("_quality.asdf");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{3, "review", 'r'})).changed,
        "review label should be accepted");
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{4, "keep", 'k'})).changed,
        "keep label should be accepted");
    (void)Submit(session, AssignActiveLabelToCurrentSample(3));
    (void)Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::LocateRow(1)));
    (void)Submit(session, AssignActiveLabelToCurrentSample(4));
    (void)Submit(session, SetActiveLabelingOutputPath(output_path));

    const std::string source_id =
        "labeling:" + session.View().labeling.task_id;
    (void)Submit(session, AddSampleFilterSource(source_id));
    (void)Submit(session, SetFilterValueSelected(source_id, "3", true));
    (void)Submit(session, SetFilterValueSelected(source_id, "4", true));

    const spectiary::SourceCollectionSessionResult result = Submit(session, RemoveActiveLabel(3));
    Require(result.changed, "removing the selected label should change the task");
    Require(session.View().filter.sources.size() == 1, "the labeling sample-filter source should stay selected");
    const std::unordered_set<std::string> selected_keys =
        session.View().filter.sources[0].selected_value_keys;
    Require(
        selected_keys.find("3") == selected_keys.end(),
        "removing a label should remove its code from the active sample-filter condition");
    Require(
        selected_keys.find("4") != selected_keys.end(),
        "removing a label should preserve other selected values in the same sample-filter condition");
    Require(session.View().navigation.filter_active, "the remaining selected value should keep sample filtering active");
    Require(session.View().navigation.sequence_count == 1, "the remaining selected label should keep one sample");
}

void TestChangingUsedLabelCodeMigratesValuesAndSampleFilter()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path output_path = UniqueTempPath("_quality.asdf");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{3, "review", 'r'})).changed,
        "review label should be accepted");
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{4, "keep", 'k'})).changed,
        "keep label should be accepted");
    (void)Submit(session, AssignActiveLabelToCurrentSample(3));
    (void)Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::LocateRow(1)));
    (void)Submit(session, AssignActiveLabelToCurrentSample(4));
    (void)Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::LocateRow(0)));
    (void)Submit(session, SetActiveLabelingOutputPath(output_path));

    const std::string source_id =
        "labeling:" + session.View().labeling.task_id;
    (void)Submit(session, AddSampleFilterSource(source_id));
    (void)Submit(session, SetFilterValueSelected(source_id, "3", true));

    spectiary::SourceCollectionSessionResult result = Submit(
        session,
        UpdateActiveLabel(3, spectiary::SampleLabelDefinition{7, "accepted", 'a'}, false));
    Require(!result.changed, "changing a used label code should be rejected without confirmation");
    Require(session.View().labeling.current_code == 3, "rejected recode should keep the current sample value");

    result = Submit(
        session,
        UpdateActiveLabel(3, spectiary::SampleLabelDefinition{7, "accepted", 'a'}, true));
    Require(result.changed, "confirmed used label recode should flow through the session intent");
    Require(session.View().labeling.current_code == 7, "confirmed recode should migrate the current sample value");
    Require(
        spectiary::FindSampleLabel(session.View().labeling.label_set, 3) == nullptr &&
            spectiary::FindSampleLabel(session.View().labeling.label_set, 7) != nullptr,
        "confirmed recode should atomically replace the label definition");
    Require(session.View().filter.sources.size() == 1, "confirmed recode should keep the sample filter source");
    const std::unordered_set<std::string> selected_keys =
        session.View().filter.sources[0].selected_value_keys;
    Require(selected_keys.find("3") == selected_keys.end(), "sample filtering should drop the old label code");
    Require(selected_keys.find("7") != selected_keys.end(), "sample filtering should follow the new label code");
    Require(session.View().navigation.sequence_count == 1, "migrated sample filtering should keep one match");
    Require(
        session.View().navigation.current_index && *session.View().navigation.current_index == 0,
        "migrated sample filtering should keep the same matching sample current");
    Require(
        session.View().snapshot && session.View().snapshot->collection.current_index == 0,
        "migrated sample filtering should keep the spectrum snapshot aligned");

    const spectiary::SampleLabelingAsdfReadResult persisted =
        spectiary::ReadSampleLabelingAsdfDocument(
            output_path);
    Require(
        persisted.succeeded(),
        persisted.error.message.empty()
            ? "recode ASDF output should load"
            : persisted.error.message);
    Require(
        persisted.document->annotation.values ==
            std::vector<std::int32_t>({7, 4, -1}),
        "confirmed recode should persist migrated values");
}

void TestLabelingViewCodeConflictIncludesUndefinedSampleValues()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path annotation_path = UniqueTempPath("_incomplete_metadata.npy");
    spectiary::SampleLabelSet label_set;
    label_set.labels.push_back(spectiary::SampleLabelDefinition{5, "defined", 'd'});
    SaveLabelResultFixture(
        annotation_path,
        "incomplete-metadata",
        "Incomplete metadata",
        {5, 9, -1},
        label_set,
        true);

    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    Require(Submit(session, AddReadOnlyAnnotation(annotation_path)).loaded, "fixture annotation should load");
    (void)Submit(session, ActivateLabelingTaskFromAnnotation(annotation_path));

    const spectiary::SourceCollectionLabelingView view = session.View().labeling;
    Require(
        spectiary::FindSampleLabel(view.label_set, 9) == nullptr,
        "fixture metadata should intentionally omit sample value code 9");
    Require(view.label_usage_counts.at(9) == 1, "the labeling view should count undefined sample value code 9");

    Require(
        view.HasConflictingLabelCode(5, 9),
        "a code present in sample values should conflict even when its metadata definition is missing");
    Require(
        !Submit(
             session,
             UpdateActiveLabel(5, spectiary::SampleLabelDefinition{9, "collision", 'c'}, true))
             .changed,
        "the session should reject recoding a label to an undefined code already present in sample values");
}

void TestResumeLocateRespectsActiveFilterSequence()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    const std::string source_id = AddPlainIntegerSampleFilterSource(session, {1, 2, 2});

    spectiary::SourceCollectionSessionResult result =
        Submit(session, SetFilterValueSelected(source_id, "2", true));
    Require(session.View().navigation.sequence_active, "test should activate the filtered sequence");
    Require(session.View().navigation.sequence_count == 2, "test should include only the two matching samples");
    Require(
        session.View().navigation.current_index && *session.View().navigation.current_index == 1,
        "filter should reconcile to the first included row");

    result = Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::LocateRow(2)));
    Require(!result.navigation.target_found, "ordinary row locate remains blocked when sequence order differs");
    Require(result.navigation.blocked_by_filter, "ordinary row locate should report the active filter block");
    Require(result.navigation.current_index == 1, "blocked ordinary locate should keep the current row");

    result = Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::LocateSourceRowInSequence(2)));
    Require(result.navigation.target_found, "resume locate should allow a remembered row inside the sequence");
    Require(result.navigation.current_index == 2, "resume locate should jump to the remembered in-sequence row");
    Require(session.View().snapshot->collection.current_index == 2, "resume locate should load the remembered row snapshot");

    result = Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::LocateSourceRowInSequence(0)));
    Require(!result.navigation.target_found, "resume locate should not bypass the active sequence");
    Require(result.navigation.blocked_by_filter, "out-of-sequence resume locate should report the filter block");
    Require(result.navigation.current_index == 2, "blocked resume locate should keep the current sequence row");
}

void TestSourceOrderNavigationViewDoesNotMaterializeSequenceRows()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    TouchFile(source_path);
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 4);

    const spectiary::SourceCollectionSessionResult result =
        Submit(session, OpenSourceCollection(source_path, 0));
    const spectiary::SourceCollectionNavigationView navigation = session.View().navigation;

    Require(!navigation.sequence_active, "source order should not expose an active sequence");
    Require(navigation.sequence_count == 4, "source-order sequence count should still match sample count");
    Require(navigation.filtered_sample_count == 4, "source-order filtered count should still match sample count");
    Require(
        navigation.current_sequence_position && *navigation.current_sequence_position == 0,
        "source-order current position should remain available");
    Require(navigation.row_location_available, "source-order row location should remain available");
}

void TestRememberedPositionResumableTracksActiveSequence()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    const std::string source_id = AddPlainIntegerSampleFilterSource(session, {1, 2, 2});

    spectiary::SourceCollectionSessionResult result =
        Submit(session, SetFilterValueSelected(source_id, "2", true));
    Require(session.View().navigation.sequence_count == 2, "annotation filter should include rows 1 and 2");

    result = Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::Next()));
    Require(result.navigation.target_found && result.navigation.current_index == 2, "next should remember row 2");
    Require(
        session.View().labeling.remembered_position && *session.View().labeling.remembered_position == 2,
        "next navigation should remember row 2");
    Require(session.View().labeling.remembered_position_resumable, "remembered in-sequence row should be resumable");

    (void)Submit(session, SetFilterValueSelected(source_id, "2", false));
    result = Submit(session, SetFilterValueSelected(source_id, "1", true));
    Require(session.View().navigation.sequence_count == 1, "second annotation filter should include only row 0");
    Require(
        session.View().labeling.remembered_position && *session.View().labeling.remembered_position == 2,
        "remembered row should survive filter changes");
    Require(
        !session.View().labeling.remembered_position_resumable,
        "remembered out-of-sequence row should not be resumable");
}

void TestSampleSortingIntentAppliesNavigationSequence()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    TouchFile(source_path);
    WriteUnicodeNameNpy(CompanionNamePath(source_path), {"gamma", "alpha", "beta"}, 6);
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);

    spectiary::SourceCollectionSessionResult result =
        Submit(session, OpenSourceCollection(source_path, 0));
    Require(session.View().sorting.has_active_source, "sorting view should attach to the active source");
    Require(HasSortSource(session.View().sorting, "sample-name"), "sample names should be available as a sort source");

    result = Submit(session, SetSampleSortSource("sample-name"));
    Require(
        result.view_invalidated,
        "sorting should report its complete view-invalidating outcome");
    Require(session.View().sorting.active, "selecting sample-name sorting should activate sorting view state");
    Require(session.View().navigation.sequence_count == 3, "sample-name sorting should keep all rows in the sequence");
    Require(!session.View().navigation.row_location_available, "sorted sequence should disable ordinary row locate");
    Require(
        session.View().navigation.current_sequence_position &&
            *session.View().navigation.current_sequence_position == 2,
        "current row should keep selection and update its sorted sequence position");
    const auto& sorting_transition =
        session.View().sample_transition;
    Require(
        sorting_transition &&
            sorting_transition->reason ==
                spectiary::SourceCollectionSampleTransitionReason::
                    NavigationInputReconciliation &&
            sorting_transition->from_sample_index == 0 &&
            sorting_transition->current_sample_index == 0 &&
            !sorting_transition->accepted_label_value,
        "sorting reconciliation should clear label feedback even when the current source row stays selected");

    result = Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::Previous()));
    Require(result.navigation.target_found && result.navigation.current_index == 2, "previous should follow sorted order");

    result = Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::LocateRow(1)));
    Require(!result.navigation.target_found, "ordinary row locate should be blocked for sorted sequence");
    Require(result.navigation.current_index == 2, "blocked row locate should keep the sorted current row");

    result = Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::LocateSourceRowInSequence(1)));
    Require(result.navigation.target_found && result.navigation.current_index == 1, "sequence locate should allow sorted in-sequence rows");

    result = Submit(session, SetSampleSortDirection(spectiary::SampleNavigationSortDirection::Descending));
    Require(
        session.View().navigation.current_sequence_position &&
            *session.View().navigation.current_sequence_position == 2,
        "descending sample-name sorting should place alpha after gamma and beta");
    result = Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::Previous()));
    Require(result.navigation.target_found && result.navigation.current_index == 2, "previous should follow descending sorted order");

    result = Submit(session, ClearSampleSorting());
    Require(!session.View().sorting.active, "clearing sorting should return sorting view to source order");
    Require(
        session.View().sorting.direction == spectiary::SampleNavigationSortDirection::Ascending,
        "clearing sorting should restore ascending source order");
    Require(!session.View().navigation.sequence_active, "clearing sorting without filters should deactivate sequence state");
    Require(session.View().navigation.row_location_available, "source-order navigation should allow ordinary row locate again");

    result = Submit(session, SetSampleSortDirection(spectiary::SampleNavigationSortDirection::Descending));
    result = Submit(session, SetSampleSortSource("source-order"));
    Require(session.View().sorting.active, "descending source-order sorting should activate sorting view state");
    Require(
        session.View().sorting.active_source_id == "source-order",
        "source-order sorting should use the source-order source id");
    Require(session.View().navigation.sequence_active, "descending source-order sorting should activate navigation sequence");
    Require(!session.View().navigation.row_location_available, "descending source order should disable ordinary row locate");
    Require(
        session.View().navigation.current_sequence_position &&
            *session.View().navigation.current_sequence_position == 0,
        "descending source order should place row 2 at the first sequence position");

    result = Submit(session, SetSampleSortSource("sample-name"));
    result = Submit(session, SetSampleSortDirection(spectiary::SampleNavigationSortDirection::Ascending));
    Require(
        session.View().sorting.source_order_direction == spectiary::SampleNavigationSortDirection::Descending,
        "inactive source-order sorting should retain its own descending direction");

    result = Submit(session, SetSampleSortSource("source-order"));
    Require(
        session.View().sorting.direction == spectiary::SampleNavigationSortDirection::Descending,
        "reactivating source-order sorting should use its cached direction");

    result = Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::Next()));
    Require(
        result.navigation.target_found && result.navigation.current_index == 1,
        "next should follow descending source order");
}

void TestSampleSortingSourcesRequireExplicitAddition()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path rank_path = UniqueTempPath("_rank.npy");
    const std::string rank_source_id = AnnotationSourceId(rank_path);
    TouchFile(source_path);
    WriteUnicodeNameNpy(CompanionNamePath(source_path), {"gamma", "alpha", "beta"}, 6);
    SaveLabelResultFixture(
        rank_path,
        "rank",
        "Rank",
        {2, 1, 1},
        spectiary::SampleLabelSet{},
        false);

    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    spectiary::SourceCollectionSessionResult result = Submit(session, AddReadOnlyAnnotation(rank_path));
    Require(result.loaded, "plain integer annotation should load");
    spectiary::SourceCollectionSessionView view = session.View();
    Require(HasSortSource(view.sorting, "sample-name"), "sample-name should be a default sort entry");
    Require(!HasSortSource(view.sorting, rank_source_id), "annotation sorting should not be selected by default");
    Require(
        HasAvailableSortSource(view.sorting, rank_source_id),
        "plain annotation sorting should be available to add");

    result = Submit(session, AddSampleSortSource(rank_source_id));
    view = session.View();
    Require(!view.sorting.active, "adding a sort entry should not activate sorting by itself");
    Require(HasSortSource(view.sorting, rank_source_id), "added annotation sorting should enter the visible list");
    Require(
        !HasAvailableSortSource(view.sorting, rank_source_id),
        "added annotation sorting should leave the addable list");
    Require(view.sorting.sources.back().removable, "added annotation sorting should be removable");

    result = Submit(session, SetSampleSortSource("sample-name"));
    result = Submit(session, SetSampleSortDirection(spectiary::SampleNavigationSortDirection::Descending));
    view = session.View();
    const spectiary::SourceCollectionSampleSortSourceView* rank_before_activation =
        FindSortSource(view.sorting, rank_source_id);
    Require(rank_before_activation != nullptr, "added annotation sorting should remain visible");
    Require(
        rank_before_activation->direction == spectiary::SampleNavigationSortDirection::Ascending,
        "inactive annotation sorting should keep its own ascending direction");

    result = Submit(session, SetSampleSortSource(rank_source_id));
    view = session.View();
    Require(view.sorting.active, "activating an added sort entry should turn sorting on");
    Require(
        view.sorting.active_source_id == rank_source_id,
        "the added annotation should become the active sort source");
    Require(
        view.sorting.direction == spectiary::SampleNavigationSortDirection::Ascending,
        "activating annotation sorting should use its cached direction");
    const spectiary::SourceCollectionSampleSortSourceView* sample_name_after_activation =
        FindSortSource(view.sorting, "sample-name");
    Require(sample_name_after_activation != nullptr, "sample-name sorting should stay visible");
    Require(
        sample_name_after_activation->direction == spectiary::SampleNavigationSortDirection::Descending,
        "inactive sample-name sorting should retain its own descending direction");
    Require(view.navigation.sequence_active, "active annotation sorting should reorder navigation");

    result = Submit(session, RemoveSampleSortSource(rank_source_id));
    view = session.View();
    Require(!HasSortSource(view.sorting, rank_source_id), "removed annotation sorting should leave the visible list");
    Require(
        HasAvailableSortSource(view.sorting, rank_source_id),
        "removed annotation sorting should become available to add again");
    Require(!view.sorting.active, "removing the active sort entry should clear active sorting");
    Require(!view.navigation.sequence_active, "removing active sorting should restore source-order navigation");
}

void TestSampleWorkflowStateRestoresFiltersAndSorting()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    const std::filesystem::path workflow_cache = UniqueTempPath("_workflow.json");
    const std::filesystem::path annotation_path = UniqueTempPath("_quality.npy");
    const std::string annotation_source_id = AnnotationSourceId(annotation_path);
    TouchFile(source_path);
    WriteUnicodeNameNpy(CompanionNamePath(source_path), {"gamma", "alpha", "beta"}, 6);
    SaveLabelResultFixture(
        annotation_path,
        "quality",
        "Quality",
        {1, 2, 2},
        spectiary::SampleLabelSet{},
        false);

    {
        std::vector<LoadedSourceSnapshot> loaded_snapshots;
        PreparedSession session = MakeWorkflowPersistentSession(
            loaded_snapshots,
            navigation_cache,
            labeling_cache,
            workflow_cache,
            source_path,
            3);

        (void)Submit(session, OpenSourceCollection(source_path, 0));
        spectiary::SourceCollectionSessionResult result = Submit(session, AddReadOnlyAnnotation(annotation_path));
        Require(result.loaded, "test annotation should load before selecting sample filters");
        result = Submit(session, AddSampleFilterSource(annotation_source_id));
        Require(session.View().filter.sources.size() == 1, "test should add the annotation sample filter source");
        result = Submit(session, SetFilterValueSelected(annotation_source_id, "2", true));
        Require(session.View().navigation.filter_active, "test should activate an annotation-value sample filter");
        Require(session.View().navigation.sequence_count == 2, "test should keep only the two matching samples");

        result = Submit(session, SetSampleSortSource("sample-name"));
        Require(session.View().sorting.active, "test should activate sample-name sorting");
        result = Submit(session, SetSampleSortDirection(spectiary::SampleNavigationSortDirection::Descending));
        Require(
            session.View().sorting.direction == spectiary::SampleNavigationSortDirection::Descending,
            "test should switch sorting to descending");
        Require(
            session.View().navigation.current_sequence_position &&
                *session.View().navigation.current_sequence_position == 1,
            "descending filtered sequence should place alpha after beta");

        Require(session.FlushStateCaches(), "session flush should save workflow state");
        Require(std::filesystem::exists(workflow_cache), "workflow state flush should create the workflow cache");
    }

    std::vector<LoadedSourceSnapshot> restored_loads;
    PreparedSession restored = MakeWorkflowPersistentSession(
        restored_loads,
        navigation_cache,
        labeling_cache,
        workflow_cache,
        source_path,
        3);

    (void)restored.Open(
        source_path,
        1,
        {annotation_path});
    const spectiary::SourceCollectionSessionView& view = restored.View();
    Require(view.filter.sources.size() == 1, "restored workflow should expose the annotation sample filter source");
    Require(
        view.filter.sources[0].id == annotation_source_id,
        "restored workflow should select the annotation sample filter source");
    Require(
        view.filter.sources[0].selected_value_keys.find("2") != view.filter.sources[0].selected_value_keys.end(),
        "restored workflow should preserve the selected annotation value");
    Require(view.navigation.filter_active, "restored workflow should reactivate navigation filtering");
    Require(view.navigation.sequence_count == 2, "restored filter should still include two samples");
    Require(view.sorting.active, "restored workflow should reactivate sample sorting");
    Require(
        view.sorting.direction == spectiary::SampleNavigationSortDirection::Descending,
        "restored workflow should preserve descending sorting direction");
    Require(
        std::any_of(view.sorting.sources.begin(), view.sorting.sources.end(), [](const auto& source) {
            return source.id == "sample-name" && source.selected;
        }),
        "restored workflow should preserve the sample-name sort source");
    Require(
        view.navigation.current_index && *view.navigation.current_index == 1,
        std::string("restored workflow should reconcile to the first filtered sample; actual=") +
            (view.navigation.current_index
                 ? std::to_string(*view.navigation.current_index)
                 : "none"));
    Require(
        view.navigation.current_sequence_position && *view.navigation.current_sequence_position == 1,
        "restored descending sequence should keep alpha at position 1");
    Require(
        view.snapshot->collection.current_index == 1,
        "restored source should load the reconciled filtered sample snapshot");

    const spectiary::SourceCollectionSessionResult previous_result =
        Submit(restored, MoveSampleNavigation(spectiary::SampleNavigationRequest::Previous()));
    Require(
        previous_result.navigation.target_found && previous_result.navigation.current_index == 2,
        "restored previous navigation should follow the descending filtered sequence");
}

void TestAnnotationDisplayNameCustomizesWorkflowSurfacesAndPersists()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    const std::filesystem::path workflow_cache = UniqueTempPath("_workflow.json");
    const std::filesystem::path annotation_path = UniqueTempPath("_rank.npy");
    const std::string annotation_source_id = AnnotationSourceId(annotation_path);
    const std::string utf8_display_name = Utf8(u8"\u8d28\u91cf\u8bc4\u5206");
    TouchFile(source_path);
    SaveLabelResultFixture(
        annotation_path,
        "rank",
        "Rank",
        {2, 1, 3},
        spectiary::SampleLabelSet{},
        false);

    {
        std::vector<LoadedSourceSnapshot> loaded_snapshots;
        PreparedSession session = MakeWorkflowPersistentSession(
            loaded_snapshots,
            navigation_cache,
            labeling_cache,
            workflow_cache,
            source_path,
            3);

        (void)Submit(session, OpenSourceCollection(source_path, 0));
        spectiary::SourceCollectionSessionResult result = Submit(session, AddReadOnlyAnnotation(annotation_path));
        Require(result.loaded, "test annotation should load before renaming");
        Require(session.View().navigation.current_annotations.size() == 1, "loaded annotation should be visible");
        Require(session.View().navigation.current_annotations[0].name != "Quality score", "test should start from the default name");
        const std::string default_annotation_name = session.View().navigation.current_annotations[0].name;

        result = Submit(session, RenameAnnotationDisplayName(annotation_path, "  Quality score  "));
        Require(result.action.workflow_changed, "renaming an annotation display name should report workflow change");
        spectiary::SourceCollectionSessionView view = session.View();
        Require(
            view.navigation.current_annotations[0].name == "Quality score",
            "annotation row should use the custom display name");
        Require(
            view.filter.available_sources.size() == 1 &&
                view.filter.available_sources[0].name == "Quality score",
            "sample filters should show the custom annotation display name");
        Require(
            HasAvailableSortSource(view.sorting, annotation_source_id),
            "plain integer annotation should be available for sorting");
        Require(
            std::any_of(view.sorting.available_sources.begin(), view.sorting.available_sources.end(), [](const auto& source) {
                return source.name == "Quality score";
            }),
            "sample sorting add-source list should show the custom display name");

        result = Submit(session, AddSampleFilterSource(annotation_source_id));
        view = session.View();
        Require(view.filter.sources.size() == 1, "renamed annotation should still be addable as a filter source");
        Require(view.filter.sources[0].name == "Quality score", "selected sample filter source should keep the custom name");

        result = Submit(session, SetSampleSortSource(annotation_source_id));
        view = session.View();
        const spectiary::SourceCollectionSampleSortSourceView* sort_source =
            FindSortSource(view.sorting, annotation_source_id);
        Require(sort_source != nullptr, "renamed annotation should still be selectable as a sort source");
        Require(sort_source->name == "Quality score", "selected sample sort source should keep the custom name");

        result = Submit(session, RenameAnnotationDisplayName(annotation_path, "   "));
        Require(result.action.workflow_changed, "clearing an annotation display name should report workflow change");
        view = session.View();
        Require(
            view.navigation.current_annotations[0].name == default_annotation_name,
            "cleared annotation display name should restore the default annotation name");
        Require(
            view.filter.sources.size() == 1 &&
                view.filter.sources[0].name == default_annotation_name,
            "selected sample filter source should restore the default annotation name");
        sort_source = FindSortSource(view.sorting, annotation_source_id);
        Require(sort_source != nullptr, "cleared annotation should remain the selected sort source");
        Require(
            sort_source->name == default_annotation_name,
            "selected sample sort source should restore the default annotation name");

        result = Submit(session, RenameAnnotationDisplayName(annotation_path, utf8_display_name));
        view = session.View();
        Require(
            view.navigation.current_annotations[0].name == utf8_display_name,
            "annotation row should support UTF-8 display names after clearing the override");
        Require(
            view.filter.sources.size() == 1 &&
                view.filter.sources[0].name == utf8_display_name,
            "selected sample filter source should keep the UTF-8 annotation display name");
        sort_source = FindSortSource(view.sorting, annotation_source_id);
        Require(sort_source != nullptr, "UTF-8 renamed annotation should remain the selected sort source");
        Require(
            sort_source->name == utf8_display_name,
            "selected sample sort source should keep the UTF-8 annotation display name");

        Require(session.FlushStateCaches(), "session flush should save the custom annotation display name");
    }

    std::vector<LoadedSourceSnapshot> restored_loads;
    PreparedSession restored = MakeWorkflowPersistentSession(
        restored_loads,
        navigation_cache,
        labeling_cache,
        workflow_cache,
        source_path,
        3);

    (void)restored.Open(
        source_path,
        0,
        {annotation_path});
    const spectiary::SourceCollectionSessionView restored_view = restored.View();
    Require(
        restored_view.navigation.current_annotations[0].name == utf8_display_name,
        "restored workflow should keep the UTF-8 annotation display name");
    Require(
        restored_view.filter.sources.size() == 1 &&
            restored_view.filter.sources[0].name == utf8_display_name,
        "restored selected sample filter source should keep the UTF-8 annotation display name");
    const spectiary::SourceCollectionSampleSortSourceView*
        restored_sort_source =
            FindSortSource(
                restored_view.sorting,
                annotation_source_id);
    Require(
        restored_sort_source != nullptr &&
            restored_sort_source->name == utf8_display_name,
        "restored selected sample sort source should keep the UTF-8 annotation display name");
}

void TestAnnotationSortingSourcesRequireComparablePlainValues()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path rank_path = UniqueTempPath("_rank.npy");
    const std::filesystem::path label_result_path = UniqueTempPath("_quality.npy");
    TouchFile(source_path);
    SaveLabelResultFixture(
        rank_path,
        "rank",
        "Rank",
        {2, 1, 1},
        spectiary::SampleLabelSet{},
        false);

    spectiary::SampleLabelSet label_set;
    label_set.labels.push_back(spectiary::SampleLabelDefinition{1, "bad", 'b'});
    label_set.labels.push_back(spectiary::SampleLabelDefinition{2, "good", 'g'});
    SaveLabelResultFixture(
        label_result_path,
        "quality",
        "Quality",
        {2, 1, 1},
        label_set,
        true);

    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    spectiary::SourceCollectionSessionResult result = Submit(session, AddReadOnlyAnnotation(rank_path));
    Require(result.loaded, "plain integer annotation should load");
    Require(
        !HasSortSource(session.View().sorting, AnnotationSourceId(rank_path)),
        "plain integer annotation should not enter the visible sort list by default");
    Require(
        HasAvailableSortSource(session.View().sorting, AnnotationSourceId(rank_path)),
        "plain integer annotation should be available to add as a sort source");

    result = Submit(session, AddReadOnlyAnnotation(label_result_path));
    Require(result.loaded, "metadata-backed label result annotation should load");
    Require(
        !HasSortSource(session.View().sorting, AnnotationSourceId(label_result_path)),
        "label-result integer annotation should not enter the visible sort list");
    Require(
        !HasAvailableSortSource(session.View().sorting, AnnotationSourceId(label_result_path)),
        "label-result integer annotation should not be available to add as a sort source");

    result = Submit(session, SetSampleSortSource(AnnotationSourceId(rank_path)));
    Require(session.View().sorting.active, "plain annotation sort source should be selectable");
    Require(
        HasSortSource(session.View().sorting, AnnotationSourceId(rank_path)),
        "activating plain annotation sorting should add it to the visible list");
    Require(
        session.View().navigation.current_sequence_position &&
            *session.View().navigation.current_sequence_position == 2,
        "plain annotation sorting should use numeric values with source-order tie break");

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    result = Submit(session, SetActiveLabelingOutputPath(rank_path));
    Require(
        session.View().labeling.active_task_is_temporary &&
            !session.View().labeling.output_path,
        "canonical formalization must reject an existing legacy NPY annotation instead of adopting it as a new owner");
    Require(
        HasSortSource(
            session.View().sorting,
            AnnotationSourceId(rank_path)) &&
            session.View().sorting.active &&
            session.View().navigation.sequence_active,
        "a rejected canonical formalization must preserve the existing annotation sorting state");
}

void TestSourceSessionRestoresAnnotationSortingState()
{
    const std::filesystem::path source_session_cache = UniqueTempPath("_sources.json");
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    const std::filesystem::path workflow_cache = UniqueTempPath("_workflow.json");
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path rank_path = UniqueTempPath("_rank.npy");
    TouchFile(source_path);
    SaveLabelResultFixture(
        rank_path,
        "rank",
        "Rank",
        {3, 1, 2},
        spectiary::SampleLabelSet{},
        false);

    {
        std::vector<LoadedSourceSnapshot> loaded_snapshots;
        PreparedSession session(
            [&loaded_snapshots, source_path](const std::filesystem::path& path, std::size_t spectrum_index) {
                Require(path == source_path, "session should reload the source fixture");
                loaded_snapshots.push_back(LoadedSourceSnapshot{path, spectrum_index});
                return MakeSnapshot(source_path, 3, spectrum_index);
            },
            source_session_cache,
            navigation_cache,
            labeling_cache,
            workflow_cache);

        (void)Submit(session, OpenSourceCollection(source_path, 0));
        spectiary::SourceCollectionSessionResult result = Submit(session, AddReadOnlyAnnotation(rank_path));
        Require(result.loaded, "plain integer annotation should load before selecting ordering");
        result = Submit(session, SetSampleSortSource(AnnotationSourceId(rank_path)));
        Require(session.View().sorting.active, "annotation ordering should be active before saving");
        Require(
            session.View().navigation.current_sequence_position &&
                *session.View().navigation.current_sequence_position == 2,
            "annotation ordering should put row 0 last before saving");
        Require(session.FlushStateCaches(), "session flush should save source and workflow state");
    }

    std::vector<LoadedSourceSnapshot> restored_loads;
    PreparedSession restored(
        [&restored_loads, source_path](const std::filesystem::path& path, std::size_t spectrum_index) {
            Require(path == source_path, "restored session should reload the source fixture");
            restored_loads.push_back(LoadedSourceSnapshot{path, spectrum_index});
            return MakeSnapshot(source_path, 3, spectrum_index);
        },
        source_session_cache,
        navigation_cache,
        labeling_cache,
        workflow_cache);

    const spectiary::SourceCollectionSessionView view = restored.View();
    Require(view.sources.size() == 1, "restored session should restore the source entry");
    Require(
        view.navigation.current_annotations.size() == 1 &&
            view.navigation.current_annotations[0].path == rank_path,
        "restored session should restore the annotation used for ordering");
    Require(
        HasSortSource(view.sorting, AnnotationSourceId(rank_path)),
        "restored sorting view should expose the annotation ordering source");
    Require(view.sorting.active, "restored workflow should keep annotation ordering active");
    Require(
        view.sorting.active_source_id == AnnotationSourceId(rank_path),
        "restored workflow should keep the annotation ordering source selected");
    Require(view.navigation.sequence_active, "restored annotation ordering should activate the navigation sequence");
    Require(
        view.navigation.current_sequence_position && *view.navigation.current_sequence_position == 2,
        "restored annotation ordering should put row 0 last");

    const spectiary::SourceCollectionSessionResult previous_result =
        Submit(restored, MoveSampleNavigation(spectiary::SampleNavigationRequest::Previous()));
    Require(
        previous_result.navigation.target_found && previous_result.navigation.current_index == 2,
        "restored previous navigation should follow the annotation ordering");
}

void TestEmptyFilterSequenceDoesNotLoadFallbackSnapshot()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    const std::string source_id = AddPlainIntegerSampleFilterSource(session, {2, 2, 2});

    const spectiary::SourceCollectionSessionResult result =
        Submit(session, SetFilterValueSelected(source_id, "1", true));
    const spectiary::SourceCollectionNavigationView navigation = session.View().navigation;
    Require(navigation.filter_active, "zero-match condition should still activate navigation filtering");
    Require(navigation.sequence_active && navigation.sequence_empty, "zero-match condition should expose empty sequence");
    Require(navigation.sequence_count == 0, "empty sequence should expose zero sequence rows");
    Require(!navigation.current_index, "empty sequence should not expose a current index");
    Require(!navigation.current_source_row, "empty sequence should not expose a source row");
    Require(!navigation.current_sequence_position, "empty sequence should not expose a sequence position");
    Require(navigation.current_sample_display_name.empty(), "empty sequence should not display the stale snapshot sample");
    Require(navigation.current_annotations.empty(), "empty sequence should not display stale current annotations");
    Require(
        session.View().snapshot != nullptr,
        "empty sequence should keep the source snapshot available to source-management views");
    Require(
        session.View().current_sample_snapshot == nullptr,
        "empty sequence should suppress the stale snapshot for sample displays");
    Require(!session.View().labeling.current_index, "labeling should not receive a fallback current row");
    Require(!result.action.snapshot_changed, "empty sequence should not load a fallback sample snapshot");
    Require(loaded_indices == std::vector<std::size_t>({0}), "empty sequence should not call LoadActiveSourceAt");
}

void TestDeactivatingLabelingTaskKeepsAnnotationFilter()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    const std::string source_id = AddPlainIntegerSampleFilterSource(session, {1, 2, 2});
    spectiary::SourceCollectionSessionResult result =
        Submit(session, SetFilterValueSelected(source_id, "1", true));
    Require(session.View().navigation.filter_active, "annotation sample filter should affect navigation");
    Require(session.View().navigation.filtered_sample_count == 1, "filter should include the one matching sample");

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{1, "bad", 'b'})).changed,
        "label should be accepted");
    result = Submit(session, AssignActiveLabelToCurrentSample(1));
    Require(session.View().labeling.can_deactivate_task, "draft-only active task should be closable");

    result = Submit(session, DeactivateActiveLabelingTask());
    Require(result.action.workflow_changed, "deactivating active task should report workflow change");
    Require(!session.View().labeling.has_active_task, "deactivation should leave no active task");
    Require(session.View().filter.sources.size() == 1, "deactivation should keep the annotation sample filter source");
    Require(session.View().filter.sources[0].id == source_id, "deactivation should keep the annotation source id");
    Require(session.View().navigation.filter_active, "deactivation should keep annotation navigation filtering active");

    result = Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(session.View().labeling.has_active_task, "task record should remain available after deactivation");
    Require(session.View().labeling.current_code == 1, "reactivated task should keep its label result");
}

void TestTemporaryLabelingTaskUsesDefaultNameAndResumes()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    spectiary::SourceCollectionSessionResult result = Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(session.View().labeling.has_active_task, "temporary labeling task should become active");
    Require(session.View().labeling.has_temporary_task, "active draft should be exposed as the temporary task");
    Require(session.View().labeling.active_task_is_temporary, "new labeling task should remain temporary");
    Require(
        session.View().labeling.task_name == spectiary::kTemporarySampleLabelingTaskName,
        "temporary task should use the fixed default name");
    const std::string temporary_task_id = session.View().labeling.task_id;
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{3, "review", 'r'})).changed,
        "temporary task should accept labels");
    (void)Submit(session, AssignActiveLabelToCurrentSample(3));

    (void)Submit(session, DeactivateActiveLabelingTask());
    Require(!session.View().labeling.has_active_task, "paused temporary task should not remain active");
    Require(session.View().labeling.has_temporary_task, "paused temporary task should remain resumable");
    result = Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(session.View().labeling.has_active_task, "temporary task should resume");
    Require(session.View().labeling.task_id == temporary_task_id, "resumed task should keep its stable id");
    Require(session.View().labeling.current_code == 3, "resumed task should keep its draft values");
}

void TestLabelExportsIgnoreNavigationSequenceAndPreserveCanonicalState()
{
    const std::filesystem::path directory =
        UniqueTempPath("_label_export_matrix");
    std::filesystem::create_directories(directory);
    const std::filesystem::path source_path =
        directory / "spectra.npy";
    const std::filesystem::path labeling_cache =
        directory / "labeling.json";
    const std::filesystem::path owner_path =
        directory / "quality.asdf";
    TouchFile(source_path);
    WriteUnicodeNameNpy(
        CompanionNamePath(source_path),
        {
            "zeta,\r\nsample",
            "alpha\"sample",
            "beta\nsample",
            "delta",
        },
        16);

    bool fail_canonical_values_publication = false;
    std::vector<std::size_t> loaded_indices;
    PreparedSession session(
        [&loaded_indices, source_path](
            const std::filesystem::path& path,
            std::size_t spectrum_index) {
            Require(
                path == source_path,
                "label export matrix should reload its NPY source");
            loaded_indices.push_back(spectrum_index);
            return MakeSnapshot(
                source_path,
                4,
                spectrum_index,
                "npy");
        },
        {},
        directory / "navigation.json",
        labeling_cache,
        directory / "workflow.json",
        {},
        [&fail_canonical_values_publication](
            spectiary::SampleLabelingAsdfOpenSnapshot&
                owner_snapshot,
            const spectiary::SampleLabelingDocument& document) {
            if (fail_canonical_values_publication) {
                return spectiary::
                    SampleLabelingAsdfStoreWriteResult{
                        .error = {
                            .kind = spectiary::
                                SampleLabelingAsdfStoreErrorKind::
                                    AtomicWriteFailure,
                            .message =
                                "injected label export matrix publication failure"}};
            }
            return spectiary::
                RewriteSampleLabelingAsdfValuesAtomically(
                    owner_snapshot,
                    document);
        });
    Require(
        session.Open(source_path).loaded,
        "label export matrix source should open");
    Require(
        session.View().labeling.source_kind == "npy",
        "label export matrix should use the canonical NPY source kind");

    (void)Submit(
        session,
        StartOrResumeTemporaryLabelingTask());
    const std::string special_label =
        Utf8(u8"星系,\"A\"\r\n可信");
    Require(
        Submit(
            session,
            UpsertActiveLabel(
                spectiary::SampleLabelDefinition{
                    5,
                    special_label,
                    's'}))
                .changed &&
            Submit(
                session,
                UpsertActiveLabel(
                    spectiary::SampleLabelDefinition{
                        6,
                        "unlabeled",
                        'u'}))
                .changed,
        "label export matrix should accept stable edge-case labels");
    const spectiary::SourceCollectionSessionResult
        first_assignment = Submit(
            session,
            AssignActiveLabelToCurrentSample(5));
    Require(
        first_assignment.label_write &&
            first_assignment.label_write->write.changed,
        "label export matrix should label canonical row zero");
    (void)Submit(
        session,
        MoveSampleNavigation(
            spectiary::SampleNavigationRequest::
                LocateRow(2)));
    const spectiary::SourceCollectionSessionResult
        second_assignment = Submit(
            session,
            AssignActiveLabelToCurrentSample(6));
    Require(
        second_assignment.label_write &&
            second_assignment.label_write->write.changed,
        "label export matrix should label canonical row two");
    (void)Submit(
        session,
        MoveSampleNavigation(
            spectiary::SampleNavigationRequest::
                LocateRow(0)));
    const spectiary::SourceCollectionSessionResult
        formalized = Submit(
            session,
            SetActiveLabelingOutputPath(owner_path));
    Require(
        formalized.action.navigation_inputs_changed &&
            std::filesystem::exists(owner_path) &&
            !session.View().labeling.active_task_is_temporary &&
            session.View().labeling.output_path == owner_path &&
            session.View().labeling.output_format ==
                spectiary::
                    SampleLabelingOutputArtifactFormat::
                        CanonicalAsdf,
        "label export matrix should establish a canonical ASDF owner");

    fail_canonical_values_publication = true;
    (void)Submit(
        session,
        MoveSampleNavigation(
            spectiary::SampleNavigationRequest::
                LocateRow(3)));
    const spectiary::SourceCollectionSessionResult
        pending_write = Submit(
            session,
            AssignActiveLabelToCurrentSample(5));
    Require(
        pending_write.label_write &&
            pending_write.label_write->write.changed,
        "label export matrix pending write should change canonical row three");
    Require(
        pending_write.label_write->operation
                .output_save_attempted &&
            !pending_write.label_write->operation.output_saved,
        "label export matrix should exercise the injected canonical publication failure");
    Require(
            session.View().labeling.save_state.kind ==
                spectiary::SampleLabelSaveStateKind::Failed &&
            session.View().labeling.save_state.pending_count == 1,
        "label export matrix should retain one pending canonical overlay");
    (void)Submit(
        session,
        MoveSampleNavigation(
            spectiary::SampleNavigationRequest::
                LocateRow(0)));

    const std::vector<int> expected_values{
        5,
        spectiary::kUnlabeledSampleLabelCode,
        6,
        5,
    };
    const std::vector<spectiary::CsvRecord>
        expected_csv{
            {"sample", "label"},
            {"zeta,\r\nsample", special_label},
            {"alpha\"sample", "unlabeled"},
            {"beta\nsample", "\\unlabeled"},
            {"delta", special_label},
        };
    const std::filesystem::path csv_before =
        directory / "before-sequence.csv";
    const std::filesystem::path npy_before =
        directory / "before-sequence.npy";
    const std::filesystem::path npy_reference =
        directory / "reference.npy";

    const auto require_export_state_unchanged =
        [&](const spectiary::SourceCollectionSessionView& before,
            const std::string& cache_bytes,
            const std::string& owner_bytes,
            const std::vector<std::filesystem::path>&
                attachment_paths,
            std::string_view message) {
            const spectiary::SourceCollectionSessionView& after =
                session.View();
            const bool labels_unchanged =
                after.labeling.label_set.labels.size() ==
                    before.labeling.label_set.labels.size() &&
                std::equal(
                    after.labeling.label_set.labels.begin(),
                    after.labeling.label_set.labels.end(),
                    before.labeling.label_set.labels.begin(),
                    [](const auto& left, const auto& right) {
                        return left.code == right.code &&
                               left.name == right.name &&
                               left.shortcut == right.shortcut;
                    });
            Require(
                after.labeling.task_id ==
                        before.labeling.task_id &&
                    labels_unchanged &&
                    after.labeling.labeled_count ==
                        before.labeling.labeled_count &&
                    after.labeling.current_code ==
                        before.labeling.current_code &&
                    after.labeling.remembered_position ==
                        before.labeling.remembered_position &&
                    after.labeling.output_path ==
                        before.labeling.output_path &&
                    after.labeling.output_format ==
                        before.labeling.output_format &&
                    after.labeling.save_state.kind ==
                        before.labeling.save_state.kind &&
                    after.labeling.save_state.pending_count ==
                        before.labeling.save_state.pending_count &&
                    after.labeling.save_state.message_kind ==
                        before.labeling.save_state.message_kind &&
                    after.labeling.save_state.message ==
                        before.labeling.save_state.message &&
                    after.navigation.current_index ==
                        before.navigation.current_index &&
                    ReadBinaryFile(labeling_cache) ==
                        cache_bytes &&
                    ReadBinaryFile(owner_path) ==
                        owner_bytes &&
                    session.AnnotationPathsForSource(
                        source_path) == attachment_paths,
                message);
        };

    const spectiary::SourceCollectionSessionView
        state_before_first_export = session.View();
    const std::string cache_before_first_export =
        ReadBinaryFile(labeling_cache);
    const std::string owner_before_first_export =
        ReadBinaryFile(owner_path);
    const std::vector<std::filesystem::path>
        attachments_before_first_export =
            session.AnnotationPathsForSource(source_path);
    const spectiary::SourceCollectionSessionResult
        first_csv_result = Submit(
            session,
            ExportActiveLabels(
                csv_before,
                spectiary::SampleLabelExportFormat::Csv));
    const spectiary::SourceCollectionSessionResult
        first_npy_result = Submit(
            session,
            ExportActiveLabels(
                npy_before,
                spectiary::SampleLabelExportFormat::Npy));
    Require(
        first_csv_result.labeling_issue ==
                spectiary::SampleLabelingOperationResult::
                    Issue::None &&
            first_npy_result.labeling_issue ==
                spectiary::SampleLabelingOperationResult::
                    Issue::None &&
            !first_csv_result.changed &&
            !first_npy_result.changed &&
            !first_csv_result.action.workflow_changed &&
            !first_npy_result.action.workflow_changed &&
            !first_csv_result.action.annotation_roster_changed &&
            !first_npy_result.action.annotation_roster_changed,
        "NPY and CSV session exports should report stateless outcomes");
    Require(
        ReadCsvRecords(csv_before) == expected_csv,
        "CSV export should preserve comma, quote, Unicode, CR/LF, unlabeled, and escaped label fields in canonical order");
    std::string export_error;
    Require(
        spectiary::ExportLabelValuesToNpy(
            npy_reference,
            expected_values,
            &export_error) &&
            ReadBinaryFile(npy_before) ==
                ReadBinaryFile(npy_reference),
        export_error.empty()
            ? "session NPY bytes should match the existing stateless exporter"
            : export_error);
    require_export_state_unchanged(
        state_before_first_export,
        cache_before_first_export,
        owner_before_first_export,
        attachments_before_first_export,
        "export must preserve task values, pending overlay, canonical owner, output path, save state, current sample, and attachment roster");

    const std::string labeling_filter_source =
        session.View().filter.available_sources.front().id;
    Require(
        Submit(
            session,
            AddSampleFilterSource(
                labeling_filter_source))
                .action.workflow_changed &&
            Submit(
                session,
                SetFilterValueSelected(
                    labeling_filter_source,
                    "5",
                    true))
                .action.navigation_inputs_changed &&
            Submit(
                session,
                SetSampleSortSource(
                    "sample-name"))
                .action.navigation_inputs_changed &&
            Submit(
                session,
                SetSampleSortDirection(
                    spectiary::
                        SampleNavigationSortDirection::
                            Descending))
                .action.navigation_inputs_changed &&
            session.View().navigation.filter_active &&
            session.View().sorting.active,
        "label export matrix should activate a filtered and sorted navigation sequence");

    const std::filesystem::path csv_after =
        directory / "after-sequence.csv";
    const std::filesystem::path npy_after =
        directory / "after-sequence.npy";
    const spectiary::SourceCollectionSessionView
        state_before_sequence_export = session.View();
    const std::string cache_before_sequence_export =
        ReadBinaryFile(labeling_cache);
    const std::string owner_before_sequence_export =
        ReadBinaryFile(owner_path);
    const std::vector<std::filesystem::path>
        attachments_before_sequence_export =
            session.AnnotationPathsForSource(source_path);
    (void)Submit(
        session,
        ExportActiveLabels(
            csv_after,
            spectiary::SampleLabelExportFormat::Csv));
    (void)Submit(
        session,
        ExportActiveLabels(
            npy_after,
            spectiary::SampleLabelExportFormat::Npy));
    Require(
        ReadBinaryFile(csv_after) ==
                ReadBinaryFile(csv_before) &&
            ReadBinaryFile(npy_after) ==
                ReadBinaryFile(npy_before),
        "filtering and sorting must not change canonical CSV or NPY export mapping");
    require_export_state_unchanged(
        state_before_sequence_export,
        cache_before_sequence_export,
        owner_before_sequence_export,
        attachments_before_sequence_export,
        "sequence-active export must remain stateless across every labeling and source-session owner");

    const spectiary::SourceCollectionSessionResult attached =
        Submit(
            session,
            AddReadOnlyAnnotation(csv_before));
    Require(
        attached.loaded &&
            session.AnnotationPathsForSource(
                source_path).size() ==
                attachments_before_sequence_export.size() + 1,
        "an exported CSV should reattach as one plain annotation");

    const std::filesystem::path empty_path =
        directory / "empty-label.csv";
    WriteCsvRecords(
        empty_path,
        {
            {"sample", "label"},
            {"zeta,\r\nsample", ""},
            {"alpha\"sample", "A"},
            {"beta\nsample", "B"},
            {"delta", "C"},
        });
    Require(
        Submit(
            session,
            AddReadOnlyAnnotation(empty_path))
            .loaded,
        "CSV ingestion should retain an empty text annotation field");
    const auto empty_annotation = std::find_if(
        session.View().navigation.current_annotations.begin(),
        session.View().navigation.current_annotations.end(),
        [&empty_path](const auto& annotation) {
            return annotation.path == empty_path;
        });
    Require(
        empty_annotation !=
                session.View().navigation.current_annotations.end() &&
            empty_annotation->display_text.empty(),
        "an attached empty CSV label should remain an empty text value");

    const std::vector<std::filesystem::path>
        valid_attachment_paths =
            session.AnnotationPathsForSource(source_path);
    const std::filesystem::path duplicate_path =
        directory / "duplicate-identity.csv";
    WriteCsvRecords(
        duplicate_path,
        {
            {"sample", "label"},
            {"zeta,\r\nsample", "A"},
            {"zeta,\r\nsample", "duplicate"},
            {"alpha\"sample", "B"},
            {"beta\nsample", "C"},
            {"delta", "D"},
        });
    const spectiary::SourceCollectionSessionResult duplicate =
        Submit(
            session,
            AddReadOnlyAnnotation(duplicate_path));
    Require(
        !duplicate.loaded &&
            duplicate.message.find(
                "duplicate sample identity") !=
                std::string::npos &&
            session.AnnotationPathsForSource(
                source_path) == valid_attachment_paths,
        "duplicate CSV identity must be rejected without changing the attachment roster");

    const std::filesystem::path missing_path =
        directory / "missing-identity.csv";
    WriteCsvRecords(
        missing_path,
        {
            {"sample", "label"},
            {"zeta,\r\nsample", "A"},
            {"alpha\"sample", "B"},
            {"beta\nsample", "C"},
        });
    const spectiary::SourceCollectionSessionResult missing =
        Submit(
            session,
            AddReadOnlyAnnotation(missing_path));
    Require(
        !missing.loaded &&
            missing.message.find(
                "missing canonical sample identity") !=
                std::string::npos &&
            session.AnnotationPathsForSource(
                source_path) == valid_attachment_paths,
        "missing CSV identity must be rejected without changing the attachment roster");
}

void TestFolderSessionExportsDefaultCsvAndNpyOverrideArtifacts()
{
    const std::filesystem::path directory =
        UniqueTempPath("_folder_label_export_matrix");
    const std::filesystem::path source_path =
        directory / "spectra";
    const std::filesystem::path labeling_cache =
        directory / "labeling.json";
    std::filesystem::create_directories(source_path);
    TouchFile(source_path / "row-0.fits");
    TouchFile(source_path / "row-1.fits");

    std::vector<std::size_t> loaded_indices;
    PreparedSession session(
        [&loaded_indices, source_path](
            const std::filesystem::path& path,
            std::size_t spectrum_index) {
            Require(
                path == source_path,
                "folder export matrix should reload its source directory");
            loaded_indices.push_back(spectrum_index);
            return MakeSnapshot(
                source_path,
                2,
                spectrum_index,
                "folder");
        },
        {},
        directory / "navigation.json",
        labeling_cache,
        directory / "workflow.json");
    const spectiary::SourceCollectionSessionResult opened =
        session.Open(source_path);
    Require(
        opened.loaded,
        "folder export matrix source should open");
    Require(
        session.View().labeling.source_kind == "folder",
        "folder export matrix should activate a canonical folder source");
    (void)Submit(
        session,
        StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(
            session,
            UpsertActiveLabel(
                spectiary::SampleLabelDefinition{
                    5,
                    "selected",
                    's'}))
            .changed,
        "folder export matrix should add its label");
    const spectiary::SourceCollectionSessionResult assigned =
        Submit(
            session,
            AssignActiveLabelToCurrentSample(5));
    Require(
        assigned.label_write &&
            assigned.label_write->write.changed,
        "folder export matrix should label its first canonical filename");

    const spectiary::SourceCollectionSessionView before =
        session.View();
    const std::string cache_before =
        ReadBinaryFile(labeling_cache);
    const std::vector<std::filesystem::path>
        attachments_before =
            session.AnnotationPathsForSource(source_path);
    const std::filesystem::path csv_path =
        directory / "folder-default.csv";
    const std::filesystem::path npy_path =
        directory / "folder-override.npy";
    const spectiary::SourceCollectionSessionResult csv_result =
        Submit(
            session,
            ExportActiveLabels(
                csv_path,
                spectiary::SampleLabelExportFormat::Csv));
    const spectiary::SourceCollectionSessionResult npy_result =
        Submit(
            session,
            ExportActiveLabels(
                npy_path,
                spectiary::SampleLabelExportFormat::Npy));
    Require(
        csv_result.labeling_issue ==
                spectiary::SampleLabelingOperationResult::
                    Issue::None &&
            npy_result.labeling_issue ==
                spectiary::SampleLabelingOperationResult::
                    Issue::None &&
            ReadCsvRecords(csv_path) ==
                std::vector<spectiary::CsvRecord>({
                    {"filename", "label"},
                    {"row-0.fits", "selected"},
                    {"row-1.fits", "unlabeled"},
                }),
        "folder CSV default should use canonical filenames while the NPY override remains available");

    const std::filesystem::path npy_reference =
        directory / "folder-reference.npy";
    std::string export_error;
    const std::vector<int> expected_values{
        5,
        spectiary::kUnlabeledSampleLabelCode,
    };
    Require(
        spectiary::ExportLabelValuesToNpy(
            npy_reference,
            expected_values,
            &export_error) &&
            ReadBinaryFile(npy_path) ==
                ReadBinaryFile(npy_reference),
        export_error.empty()
            ? "folder NPY override should retain existing exporter bytes"
            : export_error);

    const spectiary::SourceCollectionSessionView& after =
        session.View();
    Require(
        after.labeling.task_id == before.labeling.task_id &&
            after.labeling.labeled_count ==
                before.labeling.labeled_count &&
            after.labeling.current_code ==
                before.labeling.current_code &&
            after.labeling.output_path ==
                before.labeling.output_path &&
            after.labeling.output_format ==
                before.labeling.output_format &&
            after.labeling.save_state.kind ==
                before.labeling.save_state.kind &&
            after.labeling.save_state.pending_count ==
                before.labeling.save_state.pending_count &&
            after.navigation.current_index ==
                before.navigation.current_index &&
            ReadBinaryFile(labeling_cache) ==
                cache_before &&
            session.AnnotationPathsForSource(
                source_path) == attachments_before,
        "folder CSV/NPY format choices must not mutate task or source-session state");
}

void TestAttachedCsvPreservesUnlabeledSemantics()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_csv_unlabeled_semantics.npy");
    const std::filesystem::path csv_path =
        UniqueTempPath("_csv_unlabeled_semantics.csv");
    WriteCsvRecords(
        csv_path,
        {
            {"sample", "label"},
            {"0", "unlabeled"},
            {"1", "\\unlabeled"},
            {"2", "\\\\unlabeled"},
        });
    const std::string original_csv_bytes =
        ReadBinaryFile(csv_path);
    const std::filesystem::path asdf_path =
        UniqueTempPath("_csv_promotion.asdf");

    std::vector<std::size_t> loaded_indices;
    PreparedSession session =
        MakeSession(loaded_indices, source_path, 3);
    (void)Submit(
        session,
        OpenSourceCollection(source_path, 0));
    const spectiary::SourceCollectionSessionResult attached =
        Submit(
            session,
            AddReadOnlyAnnotation(csv_path));
    Require(
        attached.loaded,
        "CSV unlabeled semantics fixture should attach");

    const auto current_annotation = [&]()
        -> const spectiary::SourceCollectionAnnotationValueView& {
        const auto& annotations =
            session.View().navigation.current_annotations;
        const auto match = std::find_if(
            annotations.begin(),
            annotations.end(),
            [&csv_path](const auto& annotation) {
                return annotation.path == csv_path;
            });
        Require(
            match != annotations.end(),
            "attached CSV should project its current annotation value");
        return *match;
    };

    Require(
        current_annotation().missing &&
            current_annotation().can_activate_labeling,
        "CSV unlabeled sentinel should project as missing while exposing promotion");
    (void)Submit(
        session,
        MoveSampleNavigation(
            spectiary::SampleNavigationRequest::
                LocateRow(1)));
    Require(
        !current_annotation().missing &&
            current_annotation().display_text == "unlabeled",
        "escaped CSV unlabeled text should remain a labeled value");
    (void)Submit(
        session,
        MoveSampleNavigation(
            spectiary::SampleNavigationRequest::
                LocateRow(2)));
    Require(
        !current_annotation().missing &&
            current_annotation().display_text == "\\unlabeled",
        "double-escaped CSV text should retain one leading backslash");

    const auto& available_sources =
        session.View().filter.available_sources;
    const auto source = std::find_if(
        available_sources.begin(),
        available_sources.end(),
        [&csv_path](const auto& candidate) {
            return candidate.annotation_path == csv_path;
        });
    Require(
        source != available_sources.end() &&
            source->options.size() == 3,
        "CSV sentinel and escaped labels should produce three distinct sample filter options");
    const auto unlabeled_option = std::find_if(
        source->options.begin(),
        source->options.end(),
        [](const auto& option) {
            return option.represents_unlabeled_value;
        });
    const auto labeled_unlabeled_option = std::find_if(
        source->options.begin(),
        source->options.end(),
        [](const auto& option) {
            return !option.represents_unlabeled_value &&
                   option.display_text == "unlabeled";
        });
    Require(
        unlabeled_option != source->options.end() &&
            labeled_unlabeled_option != source->options.end() &&
            unlabeled_option->key !=
                labeled_unlabeled_option->key,
        "CSV sentinel and labeled text unlabeled must retain different sample filter keys");

    const spectiary::SourceCollectionSessionResult activated =
        Submit(
            session,
            ActivateLabelingTaskFromAnnotation(csv_path));
    const spectiary::SourceCollectionSessionView& active_view =
        session.View();
    Require(
        activated.action.workflow_changed &&
            active_view.labeling.has_active_task &&
            spectiary::IsCanonicalUuidV4(
                active_view.labeling.task_id) &&
            active_view.labeling.active_task_is_temporary &&
            !active_view.labeling.output_path &&
            active_view.labeling.output_format ==
                spectiary::SampleLabelingOutputArtifactFormat::None,
        "CSV activation should create a fresh outputless canonical draft");
    Require(
        active_view.labeling.label_set.labels.size() == 2 &&
            active_view.labeling.label_set.labels[0].code == 0 &&
            active_view.labeling.label_set.labels[0].name ==
                "\\unlabeled" &&
            active_view.labeling.label_set.labels[1].code == 1 &&
            active_view.labeling.label_set.labels[1].name ==
                "unlabeled" &&
            active_view.labeling.current_code == 0,
        "CSV promotion should distinguish missing from escaped text with stable lexical codes");
    Require(
        ReadBinaryFile(csv_path) == original_csv_bytes,
        "CSV activation must not overwrite or adopt the source artifact");

    const spectiary::SourceCollectionSessionResult formalized =
        Submit(
            session,
            SetActiveLabelingOutputPath(asdf_path));
    Require(
        formalized.action.navigation_inputs_changed &&
            formalized.labeling_issue ==
                spectiary::SampleLabelingOperationResult::Issue::None &&
            session.View().labeling.output_path == asdf_path &&
            session.View().labeling.output_format ==
                spectiary::SampleLabelingOutputArtifactFormat::CanonicalAsdf,
        "the promoted CSV draft should formalize only at an explicit ASDF path");
    const spectiary::SampleLabelingAsdfReadResult saved =
        spectiary::ReadSampleLabelingAsdfDocument(asdf_path);
    Require(
        saved.succeeded() &&
            saved.document->annotation.values ==
                std::vector<std::int32_t>({-1, 1, 0}) &&
            saved.document->labeling.labels.size() == 2 &&
            saved.document->labeling.canonical_metadata.origin.kind ==
                "annotation_promotion" &&
            saved.document->labeling.canonical_metadata.origin.annotation &&
            saved.document->labeling.canonical_metadata.origin.annotation->name ==
                csv_path.filename().string() &&
            saved.document->labeling.canonical_metadata.origin.annotation->format ==
                "csv" &&
            saved.document->labeling.canonical_metadata.origin.annotation->fingerprint &&
            saved.document->labeling.canonical_metadata.origin.annotation->fingerprint->size() ==
                71 &&
            saved.document->labeling.canonical_metadata.origin.annotation->fingerprint->starts_with(
                "sha256:"),
        "formalized CSV promotion should persist values, definitions, and portable SHA-256 provenance");
    Require(
        ReadBinaryFile(csv_path) == original_csv_bytes,
        "formalizing a CSV promotion must leave the source CSV byte-for-byte unchanged");
}

void TestExportingLabelValuesDoesNotFormalizeOrAttachTask()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_export_source.npy");
    const std::filesystem::path export_path =
        UniqueTempPath("_export_labels.npy");
    const std::filesystem::path csv_export_path =
        UniqueTempPath("_export_labels.csv");
    const std::filesystem::path sidecar_path =
        spectiary::test_support::LegacyFixtureIo::
            MetadataPathForResult(export_path);
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(
        loaded_indices,
        source_path,
        3);
    (void)Submit(
        session,
        OpenSourceCollection(source_path, 0));
    (void)Submit(
        session,
        StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(
            session,
            UpsertActiveLabel(
                spectiary::SampleLabelDefinition{
                    8,
                    "exported",
                    'e'}))
            .changed,
        "export fixture should add a label");
    (void)Submit(
        session,
        AssignActiveLabelToCurrentSample(8));

    const std::string task_id_before =
        session.View().labeling.task_id;
    const spectiary::SourceCollectionSessionResult result =
        Submit(
            session,
            ExportActiveLabels(export_path));
    Require(
        result.labeling_issue ==
                spectiary::SampleLabelingOperationResult::
                    Issue::None &&
            !result.changed &&
            !result.action.workflow_changed &&
            !result.action.navigation_inputs_changed &&
            !result.action.annotation_roster_changed,
        "one-shot export should not mutate workflow or navigation state");

    std::string load_error;
    const std::optional<spectiary::LoadedSampleLabelResult>
        exported =
            spectiary::test_support::LegacyFixtureIo{}
                .LoadLabelResult(
                    export_path,
                    3,
                    {},
                    &load_error);
    Require(
        exported.has_value() &&
            exported->values ==
                std::vector<int>({8, -1, -1}),
        load_error.empty()
            ? "session export should write current values in source order"
            : load_error);
    Require(
        !std::filesystem::exists(sidecar_path),
        "session export must not write a canonical metadata sidecar");

    const spectiary::SourceCollectionSessionResult csv_result =
        Submit(
            session,
            ExportActiveLabels(
                csv_export_path,
                spectiary::SampleLabelExportFormat::Csv));
    Require(
        csv_result.labeling_issue ==
                spectiary::SampleLabelingOperationResult::
                    Issue::None &&
            !csv_result.changed &&
            !csv_result.action.workflow_changed,
        "CSV interchange export should use the same stateless session path");
    spectiary::BoundedCsvFileReader csv_reader(
        csv_export_path);
    const spectiary::CsvRecordReadResult csv_header =
        csv_reader.ReadRecord();
    const spectiary::CsvRecordReadResult csv_first =
        csv_reader.ReadRecord();
    const spectiary::CsvRecordReadResult csv_second =
        csv_reader.ReadRecord();
    const spectiary::CsvRecordReadResult csv_third =
        csv_reader.ReadRecord();
    Require(
        csv_header.record ==
                spectiary::CsvRecord({"sample", "label"}) &&
            csv_first.record ==
                spectiary::CsvRecord({"0", "exported"}) &&
            csv_second.record ==
                spectiary::CsvRecord({"1", "unlabeled"}) &&
            csv_third.record ==
                spectiary::CsvRecord({"2", "unlabeled"}),
        "session CSV intent should preserve canonical source-index order and stable label text");

    const spectiary::SourceCollectionLabelingView& labeling =
        session.View().labeling;
    Require(
        labeling.task_id == task_id_before &&
            labeling.has_active_task &&
            labeling.can_export_label_values &&
            labeling.has_temporary_task &&
            labeling.active_task_is_temporary &&
            !labeling.output_path &&
            labeling.output_format ==
                spectiary::
                    SampleLabelingOutputArtifactFormat::None,
        "session export must not formalize the task or change its autosave owner");
    Require(
        session.View().navigation.current_annotations.empty(),
        "interchange exports must not be attached as labeling owners or annotations");
}

void TestTemporaryDraftRecoveryViewRestoresAfterRestart()
{
    const std::filesystem::path source_path = UniqueTempPath("_recovery_restart.npy");
    TouchFile(source_path);
    const std::filesystem::path source_session_cache =
        UniqueTempPath("_recovery_restart_sources.json");
    const std::filesystem::path navigation_cache =
        UniqueTempPath("_recovery_restart_navigation.json");
    const std::filesystem::path labeling_cache =
        UniqueTempPath("_recovery_restart_labeling.json");
    std::vector<LoadedSourceSnapshot> loaded_snapshots;
    const auto make_session = [&]() {
        return MakePersistentSession(
            loaded_snapshots,
            source_session_cache,
            navigation_cache,
            labeling_cache,
            {{source_path, 3}});
    };

    std::string source_identity;
    std::string task_id;
    {
        PreparedSession seed = make_session();
        Require(
            seed.Open(source_path, 0).loaded,
            "restart recovery fixture should open the source");
        (void)Submit(seed, StartOrResumeTemporaryLabelingTask());
        Require(
            seed.View().labeling.has_active_task,
            "restart recovery fixture should create a temporary draft");
        source_identity = seed.View().labeling.source_identity;
        task_id = seed.View().labeling.task_id;
        (void)Submit(
            seed,
            UpsertActiveLabel(
                spectiary::SampleLabelDefinition{
                    3,
                    "review",
                    'r'}));
        (void)Submit(seed, AssignActiveLabelToCurrentSample(3));
        Require(
            seed.View().labeling.current_code == 3,
            "restart recovery fixture should save a draft value");
        Require(
            Submit(seed, DeactivateActiveLabelingTask()).action.workflow_changed,
            "restart recovery fixture should pause the draft");
        Require(
            seed.FlushStateCaches(),
            "restart recovery fixture should flush its source and draft state");
    }

    {
        PreparedSession restarted = make_session();
        const spectiary::SourceCollectionLabelingView& recovery_view =
            restarted.View().labeling;
        Require(
            recovery_view.source_identity == source_identity &&
                recovery_view.recovery_drafts.size() == 1 &&
                recovery_view.recovery_drafts[0].task_id == task_id &&
                recovery_view.recovery_drafts[0].status ==
                    spectiary::SampleLabelingRecoveryDraftStatus::Recoverable &&
                recovery_view.recovery_drafts[0].labeled_count == 1,
            "restart should expose the paused draft with its source/task identity and recoverable status");

        const spectiary::SourceCollectionSessionResult recovered = Submit(
            restarted,
            RecoverTemporaryLabelingTask(source_identity, task_id));
        Require(
            recovered.action.workflow_changed &&
                restarted.View().labeling.has_active_task &&
                restarted.View().labeling.task_id == task_id &&
                restarted.View().labeling.current_code == 3,
            "restart should recover the original draft values before deletion");
        Require(
            Submit(restarted, DeactivateActiveLabelingTask()).action.workflow_changed &&
                restarted.FlushStateCaches(),
            "the original recovered draft fixture should be paused before the replacement fixture");
    }

    {
        PreparedSession restarted = make_session();
        const spectiary::SourceCollectionSessionResult deleted = Submit(
            restarted,
            DeleteTemporaryLabelingTask(source_identity, task_id));
        Require(
            !deleted.action.workflow_changed &&
                deleted.view_invalidated &&
                restarted.View().labeling.recovery_drafts.empty(),
            "the recovery delete intent should refresh only the paused draft projection");

        (void)Submit(restarted, StartOrResumeTemporaryLabelingTask());
        const std::string replacement_task_id =
            restarted.View().labeling.task_id;
        Require(
            restarted.View().labeling.has_active_task &&
                spectiary::IsCanonicalUuidV4(
                    replacement_task_id) &&
                replacement_task_id != task_id &&
                Submit(restarted, DeactivateActiveLabelingTask()).action.workflow_changed,
            "restart recovery fixture should create and pause a replacement draft with a fresh UUID");
        const spectiary::SourceCollectionSessionResult recovered = Submit(
            restarted,
            RecoverTemporaryLabelingTask(
                source_identity,
                replacement_task_id));
        Require(
            recovered.action.workflow_changed &&
                restarted.View().labeling.has_active_task &&
                restarted.View().labeling.task_id ==
                    replacement_task_id &&
                restarted.View().labeling.current_code ==
                    spectiary::kUnlabeledSampleLabelCode,
            "the replacement recovery fixture should activate its empty fresh-ID draft");
    }
}

void TestTemporaryDraftRecoveryViewReportsLeaseConflict()
{
    const std::filesystem::path source_path = UniqueTempPath("_recovery_conflict.npy");
    TouchFile(source_path);
    const std::filesystem::path labeling_cache =
        UniqueTempPath("_recovery_conflict_labeling.json");
    std::vector<LoadedSourceSnapshot> loaded_snapshots;
    {
        PreparedSession seed = MakePersistentSession(
            loaded_snapshots,
            {},
            UniqueTempPath("_recovery_conflict_seed_navigation.json"),
            labeling_cache,
            {{source_path, 3}});
        Require(
            seed.Open(source_path, 0).loaded,
            "conflict recovery fixture should open the source");
        (void)Submit(seed, StartOrResumeTemporaryLabelingTask());
        Require(
            seed.View().labeling.has_active_task &&
                Submit(seed, DeactivateActiveLabelingTask()).action.workflow_changed &&
                seed.FlushStateCaches(),
            "conflict recovery fixture should persist a paused draft");
    }

    PreparedSession first = MakePersistentSession(
        loaded_snapshots,
        {},
        UniqueTempPath("_recovery_conflict_first_navigation.json"),
        labeling_cache,
        {{source_path, 3}});
    PreparedSession second = MakePersistentSession(
        loaded_snapshots,
        {},
        UniqueTempPath("_recovery_conflict_second_navigation.json"),
        labeling_cache,
        {{source_path, 3}});
    Require(
        first.Open(source_path, 0).loaded &&
            second.Open(source_path, 0).loaded,
        "conflict recovery instances should open the same source");
    const std::string source_identity =
        second.View().labeling.source_identity;
    const std::string task_id =
        second.View().labeling.recovery_drafts.front().task_id;
    Require(
        Submit(first, RecoverTemporaryLabelingTask(source_identity, task_id)).action.workflow_changed,
        "the first recovery instance should acquire the draft lease");

    const spectiary::SourceCollectionSessionResult conflict = Submit(
        second,
        RecoverTemporaryLabelingTask(source_identity, task_id));
    Require(
        conflict.labeling_issue ==
                spectiary::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable &&
            !second.View().labeling.has_active_task &&
            second.View().labeling.recovery_drafts.size() == 1 &&
            second.View().labeling.recovery_drafts[0].status ==
                spectiary::SampleLabelingRecoveryDraftStatus::Conflicting,
        "a recovery attempt held by another instance should expose a conflicting draft and an error issue");
}

void TestTemporaryDraftRecoveryViewReportsUntrustedStaleDrafts()
{
    const std::filesystem::path source_path = UniqueTempPath("_recovery_stale.npy");
    TouchFile(source_path);
    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(source_path, 3, 0);
    const spectiary::SourceCollectionIdentity identity =
        spectiary::BuildSourceCollectionIdentity(*snapshot);
    const std::filesystem::path labeling_cache =
        UniqueTempPath("_recovery_stale_labeling.json");

    spectiary::SampleLabelingSourceState source_state;
    source_state.sample_count = 3;
    source_state.tasks = {
        spectiary::CreateSampleLabelingTask(
            "44444444-4444-4444-8444-444444444444",
            "Recovered draft A",
            3),
        spectiary::CreateSampleLabelingTask(
            "44444444-4444-4444-8444-444444444444",
            "Recovered draft B",
            3)};
    spectiary::SampleLabelingStateCache cache;
    cache.sources.emplace(identity.id, std::move(source_state));
    Require(
        !spectiary::SaveSampleLabelingStateCache(spectiary::RuntimePaths{}, labeling_cache, cache),
        "ambiguous task slots must be rejected before either owner is written");
    Require(!std::filesystem::exists(labeling_cache),
        "rejected ambiguous state must not become a recovery source");
}

void TestTemporaryDraftRecoveryViewReportsFormalTaskIdentityConflict()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_recovery_formal_task_identity_conflict.npy");
    TouchFile(source_path);
    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(source_path, 3, 0);
    const spectiary::SourceCollectionIdentity identity =
        spectiary::BuildSourceCollectionIdentity(*snapshot);
    const std::filesystem::path labeling_cache =
        UniqueTempPath("_recovery_formal_task_identity_conflict_labeling.json");

    spectiary::SampleLabelingTask formal_task =
        spectiary::CreateSampleLabelingTask(
            "55555555-5555-4555-8555-555555555555",
            "Formal task",
            3);
    formal_task.persistence.output_path =
        UniqueTempPath("_recovery_formal_task_identity_conflict.npy");
    formal_task.persistence.output_format =
        spectiary::SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
    spectiary::SampleLabelingSourceState source_state;
    source_state.sample_count = 3;
    source_state.tasks = {
        std::move(formal_task),
        spectiary::CreateSampleLabelingTask(
            "55555555-5555-4555-8555-555555555555",
            "Temporary draft",
            3)};
    spectiary::SampleLabelingStateCache cache;
    cache.sources.emplace(identity.id, std::move(source_state));
    Require(
        !spectiary::SaveSampleLabelingStateCache(spectiary::RuntimePaths{}, labeling_cache, cache),
        "ambiguous task slots must be rejected before either owner is written");
    Require(!std::filesystem::exists(labeling_cache),
        "rejected ambiguous state must not become a recovery source");
}

void TestLabelingViewAndIntentClearValuesWhenRemovingUsedLabel()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{3, "used", 'u'})).changed,
        "used label should be accepted");
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{4, "unused", 'n'})).changed,
        "unused label should be accepted");
    (void)Submit(session, AssignActiveLabelToCurrentSample(3));

    const spectiary::SourceCollectionLabelingView labeling = session.View().labeling;
    Require(labeling.label_usage_counts.at(3) == 1, "view should expose the used label count");
    Require(
        labeling.label_usage_counts.find(4) == labeling.label_usage_counts.end(),
        "view should omit zero-count labels from its usage map");

    spectiary::SourceCollectionSessionResult result = Submit(session, RemoveActiveLabel(4));
    Require(result.changed, "unused label removal should flow through the session intent");
    Require(
        spectiary::FindSampleLabel(session.View().labeling.label_set, 4) == nullptr,
        "unused label should disappear from the view");

    result = Submit(session, RemoveActiveLabel(3));
    Require(result.changed, "used label removal should flow through the session intent");
    Require(
        spectiary::FindSampleLabel(session.View().labeling.label_set, 3) == nullptr,
        "used label definition should disappear from the view");
    Require(
        session.View().labeling.current_code == spectiary::kUnlabeledSampleLabelCode,
        "removing a used label should clear the current sample value");
    Require(session.View().labeling.labeled_count == 0, "cleared values should update labeling progress");
}

void TestDiscardingTemporaryLabelingTaskAllowsFreshStart()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{3, "review", 'r'})).changed,
        "temporary task should accept a label before discard");
    (void)Submit(session, AssignActiveLabelToCurrentSample(3));

    const std::string discarded_task_id = session.View().labeling.task_id;
    spectiary::SourceCollectionSessionResult result = Submit(session, DeleteActiveLabelingTask());
    Require(result.action.workflow_changed, "delete should report workflow change");
    Require(!session.View().labeling.has_active_task, "delete should clear the active task");
    Require(!session.View().labeling.has_temporary_task, "delete should discard the temporary task record");

    result = Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(session.View().labeling.has_active_task, "starting after discard should create a fresh task");
    Require(
        spectiary::IsCanonicalUuidV4(session.View().labeling.task_id) &&
            session.View().labeling.task_id != discarded_task_id,
        "starting after discard should generate a fresh canonical task id");
    Require(
        session.View().labeling.current_code == spectiary::kUnlabeledSampleLabelCode,
        "deleted draft values should not come back");
}

void TestSavingTemporaryTaskCreatesNamedAnnotationAndAllowsFreshTemporaryTask()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path output_path = UniqueTempPath("_quality.asdf");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(session.View().labeling.active_task_is_temporary, "task should start as a local temporary draft");
    spectiary::SourceCollectionSessionResult result =
        Submit(session, SetActiveLabelingOutputPath(output_path));
    Require(!session.View().labeling.has_temporary_task, "choosing output should formalize the temporary task");
    Require(!session.View().labeling.active_task_is_temporary, "saved task should become a formal annotation");
    Require(
        session.View().labeling.task_name == spectiary::kTemporarySampleLabelingTaskName,
        "choosing an output path should not rename canonical task metadata");
    Require(session.View().navigation.current_annotations.size() == 1, "local task output should appear in annotations");
    Require(
        session.View().navigation.current_annotations[0].name ==
            spectiary::kTemporarySampleLabelingTaskName,
        "local labeling annotation should default to the canonical task name");
    Require(
        session.View().filter.available_sources.size() == 1 &&
            session.View().filter.available_sources[0].name ==
                spectiary::kTemporarySampleLabelingTaskName,
        "local labeling filter source should default to the canonical task name");

    result = Submit(session, RenameAnnotationDisplayName(output_path, "Hard cases"));
    Require(
        session.View().labeling.task_name == spectiary::kTemporarySampleLabelingTaskName,
        "display-name customization should not rename metadata");
    Require(
        session.View().navigation.current_annotations[0].name == "Hard cases",
        "annotation row should use the custom local-task display name");
    Require(
        session.View().filter.available_sources[0].name == "Hard cases",
        "local labeling filter source should use the custom annotation display name");

    result = Submit(session, RenameAnnotationDisplayName(output_path, "   "));
    Require(result.action.workflow_changed, "clearing local-task annotation display name should report workflow change");
    Require(
        session.View().navigation.current_annotations[0].name ==
            spectiary::kTemporarySampleLabelingTaskName,
        "cleared local-task annotation display name should restore the canonical task name");

    result = Submit(session, DeactivateActiveLabelingTask());
    Require(!session.View().labeling.has_temporary_task, "closing a formal annotation should not create a draft");
    result = Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(session.View().labeling.active_task_is_temporary, "a fresh temporary task should start after formal save");
    Require(
        session.View().navigation.current_annotations.size() == 1 &&
            session.View().navigation.current_annotations[0].name ==
                spectiary::kTemporarySampleLabelingTaskName,
        "starting a new temporary task should keep the formal annotation available");

    const std::string fresh_temporary_task_id = session.View().labeling.task_id;
    result = Submit(session, ActivateLabelingTaskFromAnnotation(output_path));
    Require(
        !session.View().labeling.active_task_is_temporary && session.View().labeling.output_path == output_path,
        "selecting a labeling annotation should safely switch away from the active draft");
    Require(session.View().labeling.has_temporary_task, "switching annotations should retain the paused draft");

    (void)Submit(session, ClearActiveLabelForCurrentSample());
    Require(
        Submit(
            session,
            UpsertActiveLabel(
                spectiary::SampleLabelDefinition{11, "formal-review", 'f'}))
            .changed,
        "formal task should accept a label before deleting an unrelated paused draft");
    const spectiary::SourceCollectionSessionResult formal_assignment =
        Submit(session, AssignActiveLabelToCurrentSample(11));
    Require(
        formal_assignment.label_write &&
            formal_assignment.label_write->write.changed,
        "formal task should record a label before deleting an unrelated paused draft");
    const spectiary::SourceCollectionSessionResult deleted_paused_draft =
        Submit(
            session,
            DeleteTemporaryLabelingTask(
                session.View().labeling.source_identity,
                fresh_temporary_task_id));
    Require(
        !deleted_paused_draft.action.workflow_changed &&
            deleted_paused_draft.view_invalidated &&
            session.View().labeling.has_active_task &&
            !session.View().labeling.active_task_is_temporary,
        "deleting a paused draft should refresh recovery projection without resetting the unrelated formal task");
    (void)Submit(session, UndoLastLabelWrite());
    Require(
        session.View().labeling.current_code ==
            spectiary::kUnlabeledSampleLabelCode,
        "deleting an unrelated paused draft must preserve the formal task undo history");

    result = Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        session.View().labeling.active_task_is_temporary &&
            spectiary::IsCanonicalUuidV4(session.View().labeling.task_id) &&
            session.View().labeling.task_id != fresh_temporary_task_id,
        "starting after deleting the paused draft should create a fresh canonical task");
}

void TestFormalizedCanonicalAttachmentPersistsAcrossRestart()
{
    const std::filesystem::path source_session_cache =
        UniqueTempPath("_formal_attachment_sources.json");
    const std::filesystem::path navigation_cache =
        UniqueTempPath("_formal_attachment_navigation.json");
    const std::filesystem::path labeling_cache =
        UniqueTempPath("_formal_attachment_labeling.json");
    const std::filesystem::path source_path =
        UniqueTempPath("_formal_attachment_source.npy");
    const std::filesystem::path output_path =
        UniqueTempPath("_formal_attachment.asdf");
    const std::filesystem::path invalid_output_path =
        UniqueTempPath("_formal_attachment.npy");
    TouchFile(source_path);

    {
        std::vector<LoadedSourceSnapshot> loaded;
        PreparedSession session = MakePersistentSession(
            loaded,
            source_session_cache,
            navigation_cache,
            labeling_cache,
            {{source_path, 3}});
        (void)Submit(
            session,
            OpenSourceCollection(source_path, 0));
        Require(
            session.FlushStateCaches(),
            "formal attachment fixture should begin from a clean persisted source session");
        (void)Submit(
            session,
            StartOrResumeTemporaryLabelingTask());
        Require(
            Submit(
                session,
                UpsertActiveLabel(
                    spectiary::SampleLabelDefinition{
                        5,
                        "accepted",
                        'a'}))
                .changed,
            "formal attachment fixture should define its label");
        (void)Submit(
            session,
            AssignActiveLabelToCurrentSample(5));
        const spectiary::SourceCollectionSessionResult rejected =
            Submit(
                session,
                SetActiveLabelingOutputPath(
                    invalid_output_path));
        Require(
            !rejected.action.navigation_inputs_changed &&
                session.View().labeling.active_task_is_temporary &&
                !session.View().labeling.output_path &&
                session.View().navigation.current_annotations.empty() &&
                !std::filesystem::exists(invalid_output_path),
            "canonical formalization must reject a non-ASDF path before publishing or attaching it");
        const spectiary::SourceCollectionSessionResult saved =
            Submit(
                session,
                SetActiveLabelingOutputPath(output_path));
        Require(
            saved.action.navigation_inputs_changed &&
                std::filesystem::exists(output_path) &&
                session.View().navigation.current_annotations.size() == 1,
            "formal attachment fixture should add its canonical owner to the live manifest");
        Require(
            Submit(
                session,
                MoveSampleNavigation(
                    spectiary::SampleNavigationRequest::LocateRow(1)))
                .action.navigation_inputs_changed,
            "canonical lifecycle fixture should navigate before its first formal value edit");
        const spectiary::SourceCollectionSessionResult value_edit =
            Submit(
                session,
                AssignActiveLabelToCurrentSample(5));
        Require(
            value_edit.label_write &&
                value_edit.label_write->write.changed &&
                value_edit.label_write->operation.output_saved,
            "a formal canonical owner should autosave a value edit before restart");
        const spectiary::SampleLabelingAsdfReadResult edited =
            spectiary::ReadSampleLabelingAsdfDocument(output_path);
        Require(
            edited.succeeded() &&
                edited.document->annotation.values ==
                    std::vector<std::int32_t>({5, 5, -1}),
            edited.error.message.empty()
                ? "the first canonical generation should contain the formal value edit"
                : edited.error.message);
        (void)Submit(
            session,
            DeactivateActiveLabelingTask());
        Require(
            session.FlushStateCaches(),
            "formal attachment fixture should flush all owners before restart");
    }

    const spectiary::SourceCollectionSessionStateCache persisted =
        spectiary::LoadSourceCollectionSessionStateCache(spectiary::RuntimePaths{},
            source_session_cache)
            .cache;
    Require(
        persisted.sources.size() == 1 &&
            persisted.sources[0].annotation_paths ==
                std::vector<std::filesystem::path>{output_path},
        "formalization must mark the source-session attachment roster dirty and persist the ASDF path");

    const std::string canonical_cache_after_value_edit =
        ReadTextFile(labeling_cache);
    Require(
        canonical_cache_after_value_edit.find(
            "          \"values\":") ==
                std::string::npos &&
            canonical_cache_after_value_edit.find(
                "\"pending_values\"") ==
                std::string::npos,
        "a clean formal ASDF owner cache must not retain a second canonical values copy or stale overlay");

    {
        std::vector<LoadedSourceSnapshot> restored_loads;
        PreparedSession restored = MakePersistentSession(
            restored_loads,
            source_session_cache,
            navigation_cache,
            labeling_cache,
            {{source_path, 3}});
        Require(
            restored.View().navigation.current_annotations.size() == 1 &&
                restored.View().navigation.current_annotations[0].path ==
                    output_path,
            "restart should restore the newly formalized canonical attachment without manual reattachment");
        const spectiary::SourceCollectionSessionResult activated =
            Submit(
                restored,
                ActivateLabelingTaskFromAnnotation(output_path));
        Require(
            activated.action.workflow_changed &&
                restored.View().labeling.has_active_task &&
                restored.View().labeling.output_path == output_path,
            "the restored canonical attachment should remain selectable as its formal task owner");
        const spectiary::SampleLabelingAsdfReadResult hydrated =
            spectiary::ReadSampleLabelingAsdfDocument(output_path);
        Require(
            hydrated.succeeded() &&
                hydrated.document->annotation.values ==
                    std::vector<std::int32_t>({5, 5, -1}),
            "restart hydration should retain the value generation published before restart");

        Require(
            Submit(
                restored,
                UpsertActiveLabel(
                    spectiary::SampleLabelDefinition{
                        9,
                        Utf8(u8"复核 ✓"),
                        'r'}))
                .changed,
            "the hydrated canonical owner should autosave an added Unicode label definition");
        Require(
            Submit(
                restored,
                UpdateActiveLabel(
                    5,
                    spectiary::SampleLabelDefinition{
                        5,
                        Utf8(u8"已接受 ✓"),
                        'v'},
                    false))
                .changed,
            "the hydrated canonical owner should autosave label metadata edits");
        (void)Submit(
            restored,
            DeactivateActiveLabelingTask());
        Require(
            restored.FlushStateCaches(),
            "metadata generation should flush before the second restart");
    }

    std::vector<LoadedSourceSnapshot> metadata_restart_loads;
    PreparedSession metadata_restart = MakePersistentSession(
        metadata_restart_loads,
        source_session_cache,
        navigation_cache,
        labeling_cache,
        {{source_path, 3}});
    Require(
        Submit(
            metadata_restart,
            ActivateLabelingTaskFromAnnotation(output_path))
            .action.workflow_changed,
        "the metadata generation should remain activatable after a second restart");
    const spectiary::SourceCollectionLabelingView& final_view =
        metadata_restart.View().labeling;
    const spectiary::SampleLabelDefinition* accepted =
        spectiary::FindSampleLabel(final_view.label_set, 5);
    const spectiary::SampleLabelDefinition* reviewed =
        spectiary::FindSampleLabel(final_view.label_set, 9);
    const spectiary::SampleLabelingAsdfReadResult final_document =
        spectiary::ReadSampleLabelingAsdfDocument(output_path);
    Require(
        final_view.has_active_task &&
            accepted != nullptr &&
            accepted->name == Utf8(u8"已接受 ✓") &&
            accepted->shortcut == 'v' &&
            reviewed != nullptr &&
            reviewed->name == Utf8(u8"复核 ✓") &&
            final_document.succeeded() &&
            final_document.document->annotation.values ==
                std::vector<std::int32_t>({5, 5, -1}),
        "metadata restart hydration should preserve Unicode definitions and the same canonical value generation");
}

void TestCanonicalOwnerRepairsMissingPreparedAttachmentAfterCrash(
    const spectiary::RuntimePaths& runtime_paths = {},
    std::string_view output_child = {})
{
    const std::filesystem::path source_session_cache =
        UniqueTempPath("_crash_repair_sources.json");
    const std::filesystem::path navigation_cache =
        UniqueTempPath("_crash_repair_navigation.json");
    const std::filesystem::path labeling_cache =
        UniqueTempPath("_crash_repair_labeling.json");
    const std::filesystem::path source_path =
        UniqueTempPath("_crash_repair_source.npy");
    const std::filesystem::path output_path =
        output_child.empty()
        ? UniqueTempPath("_crash_repair_owner.asdf")
        : (runtime_paths.application_data_root / output_child / "task.asdf").lexically_normal();
    TouchFile(source_path);

    {
        std::vector<LoadedSourceSnapshot> loaded;
        PreparedSession session = MakePersistentSession(
            loaded,
            source_session_cache,
            navigation_cache,
            labeling_cache,
            {{source_path, 3}});
        (void)Submit(
            session,
            OpenSourceCollection(source_path, 0));
        Require(
            session.FlushStateCaches(),
            "crash-repair fixture should persist a source roster without annotations");
        (void)Submit(
            session,
            StartOrResumeTemporaryLabelingTask());
        Require(
            Submit(
                session,
                UpsertActiveLabel(
                    spectiary::SampleLabelDefinition{
                        5,
                        "accepted",
                        'a'}))
                .changed,
            "crash-repair fixture should define its label");
        (void)Submit(
            session,
            AssignActiveLabelToCurrentSample(5));
        Require(
            Submit(
                session,
                SetActiveLabelingOutputPath(output_path))
                .action.navigation_inputs_changed,
            "crash-repair fixture should synchronously publish its canonical owner");
        Require(
            Submit(
                session,
                DeactivateActiveLabelingTask())
                .action.workflow_changed,
            "crash-repair fixture should persist an inactive canonical owner");
        // Deliberately do not flush the source-session cache. This models a
        // process loss after the synchronous labeling checkpoint but before
        // the attachment-roster debounce fires.
    }

    const spectiary::SourceCollectionSessionStateCache before_repair =
        spectiary::LoadSourceCollectionSessionStateCache(spectiary::RuntimePaths{},
            source_session_cache)
            .cache;
    Require(
        before_repair.sources.size() == 1 &&
            before_repair.sources[0].annotation_paths.empty(),
        "the crash fixture must retain the old source-session roster while the labeling cache owns the ASDF path");

    const std::string canonical_before = ReadTextFile(output_path);

    std::vector<LoadedSourceSnapshot> restored_loads;
    PreparedSession restored = MakePersistentSession(
        restored_loads,
        source_session_cache,
        navigation_cache,
        labeling_cache,
        {{source_path, 3}},
        runtime_paths);
    if (!output_child.empty() && output_child != "my-work" && output_child != ".") {
        const auto view = restored.View();
        Require(restored_loads.size() == 1 && view.snapshot,
            "reserved owner rejection must still restore its source");
        Require(view.filter.sources.empty() && view.filter.available_sources.empty() &&
                std::all_of(view.navigation.current_annotations.begin(),
                    view.navigation.current_annotations.end(), [](const auto& annotation) {
                        return annotation.display_text.empty() && !annotation.can_filter_samples;
                    }),
            "reserved canonical registration must not restore an attachment or filter data");
        Require(std::any_of(view.navigation.annotation_diagnostics.begin(),
                    view.navigation.annotation_diagnostics.end(), [&](const auto& diagnostic) {
                        return diagnostic.path == output_path &&
                            diagnostic.kind == spectiary::SourceCollectionManifestDiagnosticKind::AnnotationIgnored;
                    }),
            "reserved canonical registration must retain a rejection diagnostic");
        Require(restored.FlushStateCaches(), "rejected attachment roster should flush");
        const auto after = spectiary::LoadSourceCollectionSessionStateCache(runtime_paths, source_session_cache).cache;
        Require(after.sources.size() == 1 && after.sources[0].annotation_paths.empty(),
            "rejected canonical registration must not enter the persisted attachment roster");
        Require(ReadTextFile(output_path) == canonical_before,
            "rejecting reserved canonical attachment must not modify the user's ASDF");
        return;
    }
    Require(
        !restored.View().labeling.has_active_task &&
            restored.View().navigation.current_annotations.size() == 1 &&
            restored.View().navigation.current_annotations[0].path ==
                output_path,
        "prepared restore must derive a missing attachment from the inactive durable canonical owner");
    Require(
        restored.FlushStateCaches(),
        "prepared attachment repair should be durable after the restore batch completes");
    const spectiary::SourceCollectionSessionStateCache after_repair =
        spectiary::LoadSourceCollectionSessionStateCache(runtime_paths,
            source_session_cache)
            .cache;
    Require(
        after_repair.sources.size() == 1 &&
            after_repair.sources[0].annotation_paths ==
                std::vector<std::filesystem::path>{output_path},
        "prepared restore must persist the repaired canonical attachment roster");
}

void TestCanonicalRegistrationRestoreChecksReservedStorage()
{
    for (const auto profile : {spectiary::StorageProfile::Portable, spectiary::StorageProfile::LocalAppData}) {
        const auto root = UniqueTempPath("_registration_storage_admission");
        const auto paths = spectiary::RuntimePathsForDeployment({.storage_profile = profile}, {
            .executable_path = root / "app.exe",
            .local_app_data_user_state_root = root,
        });
        // Seed through the context-free fixture to model an older version that
        // permitted these canonical locations, then restore with real role roots.
        for (const auto* child : {"config", "state", "logs", "unsaved", "my-work", "."}) {
            TestCanonicalOwnerRepairsMissingPreparedAttachmentAfterCrash(paths, child);
        }
    }
}

void AssertUnavailableCanonicalOwnerRemainsVisibleAfterRestart(
    bool corrupt_owner)
{
    const std::string suffix = corrupt_owner
        ? "_corrupt_canonical_owner"
        : "_missing_canonical_owner";
    const std::filesystem::path source_session_cache =
        UniqueTempPath(suffix + "_sources.json");
    const std::filesystem::path navigation_cache =
        UniqueTempPath(suffix + "_navigation.json");
    const std::filesystem::path labeling_cache =
        UniqueTempPath(suffix + "_labeling.json");
    const std::filesystem::path source_path =
        UniqueTempPath(suffix + "_source.npy");
    const std::filesystem::path output_path =
        UniqueTempPath(suffix + ".asdf");
    TouchFile(source_path);

    {
        std::vector<LoadedSourceSnapshot> loaded;
        PreparedSession session = MakePersistentSession(
            loaded,
            source_session_cache,
            navigation_cache,
            labeling_cache,
            {{source_path, 3}});
        (void)Submit(
            session,
            OpenSourceCollection(source_path, 0));
        (void)Submit(
            session,
            StartOrResumeTemporaryLabelingTask());
        Require(
            Submit(
                session,
                UpsertActiveLabel(
                    spectiary::SampleLabelDefinition{
                        5,
                        "accepted",
                        'a'}))
                .changed,
            "unavailable-owner fixture should define its label");
        (void)Submit(
            session,
            AssignActiveLabelToCurrentSample(5));
        Require(
            Submit(
                session,
                SetActiveLabelingOutputPath(output_path))
                .action.navigation_inputs_changed,
            "unavailable-owner fixture should formalize its canonical task");
        (void)Submit(
            session,
            DeactivateActiveLabelingTask());
        Require(
            session.FlushStateCaches(),
            "unavailable-owner fixture should persist task and attachment ownership");
    }

    if (corrupt_owner) {
        WriteTextFile(
            output_path,
            "not an ASDF labeling document\n");
    } else {
        std::error_code remove_error;
        Require(
            std::filesystem::remove(
                output_path,
                remove_error) &&
                !remove_error,
            "missing-owner fixture should remove its canonical document");
    }

    std::vector<LoadedSourceSnapshot> restored_loads;
    PreparedSession restored = MakePersistentSession(
        restored_loads,
        source_session_cache,
        navigation_cache,
        labeling_cache,
        {{source_path, 3}});
    const spectiary::SourceCollectionSessionView view =
        restored.View();
    Require(
        !view.labeling.has_active_task &&
            view.navigation.current_annotations.size() == 1,
        "an unavailable canonical owner must remain visible as one inactive annotation task row after restart");
    const spectiary::SourceCollectionAnnotationValueView& owner =
        view.navigation.current_annotations.front();
    Require(
        owner.path == output_path &&
            owner.relationship ==
                spectiary::SampleAnnotationWorkflowRelationship::
                    LocalLabelingTask &&
            owner.output_missing &&
            !owner.missing &&
            owner.display_text.empty() &&
            !owner.can_activate_labeling &&
            owner.can_remove_annotation == !corrupt_owner &&
            !owner.can_filter_samples,
        "the unavailable canonical row must expose owner identity and missing-output state without projecting structural values");
    Require(
        view.filter.sources.empty() &&
            view.filter.available_sources.empty(),
        "an unavailable canonical owner must not become a data-bearing filter source");
    Require(
        std::any_of(
            view.navigation.annotation_diagnostics.begin(),
            view.navigation.annotation_diagnostics.end(),
            [&output_path](
                const spectiary::SourceCollectionManifestDiagnostic&
                    diagnostic) {
                return diagnostic.path == output_path &&
                    diagnostic.kind ==
                        spectiary::
                            SourceCollectionManifestDiagnosticKind::
                                AnnotationIgnored;
            }),
        "the unavailable canonical row should retain its controlled attachment failure diagnostic");
    if (!corrupt_owner) {
        const spectiary::SourceCollectionSessionResult abandoned =
            Submit(
                restored,
                RemoveReadOnlyAnnotation(output_path));
        Require(
            abandoned.action.workflow_changed &&
                restored.View().labeling.task_ids.empty() &&
                restored.View().navigation.current_annotations.empty() &&
                std::none_of(
                    restored.View().navigation
                        .annotation_diagnostics.begin(),
                    restored.View().navigation
                        .annotation_diagnostics.end(),
                    [&output_path](
                        const spectiary::
                            SourceCollectionManifestDiagnostic&
                                diagnostic) {
                        return diagnostic.path == output_path &&
                            diagnostic.kind ==
                                spectiary::
                                    SourceCollectionManifestDiagnosticKind::
                                        AnnotationIgnored;
                    }),
            "removing a missing local owner row should abandon its task record and clear both the ghost annotation and its warning without requiring relink");
        const spectiary::SampleLabelingStateCacheLoadResult
            abandoned_cache =
                spectiary::LoadSampleLabelingStateCache(spectiary::RuntimePaths{},
                    labeling_cache,
                    {},
                    spectiary::SampleLabelingStateCacheLoadPolicy::
                        AllowPersistentOutputsWithoutResultHydration);
        const auto abandoned_source =
            abandoned_cache.cache.sources.find(
                restored.View().labeling.source_identity);
        Require(
            abandoned_cache.issue_kind ==
                    spectiary::SampleLabelingStateCacheLoadIssueKind::None &&
                abandoned_source !=
                    abandoned_cache.cache.sources.end() &&
                abandoned_source->second.tasks.empty(),
            "abandoning a missing owner should durably tombstone the local task record");
    }
}

void TestUnavailableCanonicalOwnersRemainVisibleAfterRestart()
{
    AssertUnavailableCanonicalOwnerRemainsVisibleAfterRestart(false);
    AssertUnavailableCanonicalOwnerRemainsVisibleAfterRestart(true);
}

void TestFailedFirstOutputSaveKeepsRecoverableTemporaryTask()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path blocked_output_path =
        UniqueTempPath("_blocked_output.asdf");
    const std::filesystem::path replacement_output_path = UniqueTempPath("_replacement.asdf");
    std::filesystem::create_directories(blocked_output_path);

    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    const std::string temporary_task_id = session.View().labeling.task_id;

    spectiary::SourceCollectionSessionResult result =
        Submit(session, SetActiveLabelingOutputPath(blocked_output_path));
    Require(session.View().labeling.has_active_task, "failed first save should keep the draft active");
    Require(session.View().labeling.has_temporary_task, "failed first save should keep a resumable draft");
    Require(session.View().labeling.active_task_is_temporary, "failed first save must not formalize the task");
    Require(!session.View().labeling.output_path, "failed first save must not retain the rejected output target");
    Require(
        session.View().labeling.task_name == spectiary::kTemporarySampleLabelingTaskName,
        "failed first save should retain the temporary task name");
    Require(
        session.View().labeling.save_state.kind == spectiary::SampleLabelSaveStateKind::Failed,
        "failed first save should surface the write failure");
    Require(session.View().labeling.can_deactivate_task, "failed first save should still allow pausing the draft");
    Require(session.View().labeling.can_delete_task, "failed first save should still allow deleting the draft");

    result = Submit(session, DeactivateActiveLabelingTask());
    Require(!session.View().labeling.has_active_task, "failed first-save draft should be pausable");
    Require(session.View().labeling.has_temporary_task, "paused failed draft should remain resumable");
    result = Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        session.View().labeling.task_id == temporary_task_id,
        "resuming after failed first save should preserve the draft identity");

    result = Submit(session, SetActiveLabelingOutputPath(replacement_output_path));
    Require(!session.View().labeling.active_task_is_temporary, "replacement output should formalize the draft");
    Require(
        session.View().labeling.output_path == replacement_output_path,
        "replacement output should become the formal task target");
    Require(
        session.View().labeling.save_state.kind == spectiary::SampleLabelSaveStateKind::AutosavedToOutput,
        "replacement output should save successfully");
}

void TestMalformedExistingAsdfKeepsRecoverableTemporaryTask()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path output_path = UniqueTempPath("_malformed.asdf");
    const std::filesystem::path replacement_output_path = UniqueTempPath("_replacement.asdf");
    WriteTextFile(output_path, "not an ASDF labeling document\n");

    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());

    spectiary::SourceCollectionSessionResult result = Submit(session, SetActiveLabelingOutputPath(output_path));
    Require(session.View().labeling.has_temporary_task, "malformed target failure should retain the draft");
    Require(
        session.View().labeling.active_task_is_temporary,
        "malformed ASDF target must not formalize the task");
    Require(!session.View().labeling.output_path, "malformed ASDF target must not bind an owner");
    Require(
        session.View().labeling.save_state.kind == spectiary::SampleLabelSaveStateKind::Failed,
        "malformed ASDF target should surface the controlled publication failure");
    Require(
        session.View().labeling.can_deactivate_task && session.View().labeling.can_delete_task,
        "malformed ASDF target should leave the draft recoverable");

    result = Submit(session, SetActiveLabelingOutputPath(replacement_output_path));
    Require(!session.View().labeling.active_task_is_temporary, "replacement target should formalize the draft");
    Require(
        session.View().labeling.output_path == replacement_output_path,
        "replacement target should replace the rejected partial output");
}

void TestActivatingExternalAnnotationResultCreatesLocalLabelingTask()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path annotation_path = UniqueTempPath("_quality.npy");
    const std::filesystem::path output_path = UniqueTempPath("_quality.asdf");
    const std::filesystem::path navigation_cache =
        UniqueTempPath("_quality_navigation.json");
    const std::filesystem::path labeling_cache =
        UniqueTempPath("_quality_labeling.json");
    const std::filesystem::path workflow_cache =
        UniqueTempPath("_quality_workflow.json");
    spectiary::SampleLabelSet label_set;
    label_set.labels.push_back(spectiary::SampleLabelDefinition{5, "bad", 'b'});
    label_set.labels.push_back(spectiary::SampleLabelDefinition{9, "good", 'g'});
    SaveLabelResultFixture(
        annotation_path,
        "quality-review",
        "Quality review",
        {5, -1, 9},
        label_set,
        true);
    std::vector<std::size_t> loaded_indices;
    PreparedSession session(
        [&loaded_indices, source_path](
            const std::filesystem::path& path,
            std::size_t spectrum_index) {
            Require(
                path == source_path,
                "local annotation task fixture should reload its source");
            loaded_indices.push_back(spectrum_index);
            return MakeSnapshot(source_path, 3, spectrum_index);
        },
        {},
        navigation_cache,
        labeling_cache,
        workflow_cache);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    spectiary::SourceCollectionSessionResult result = Submit(session, AddReadOnlyAnnotation(annotation_path));
    Require(result.loaded, "external label result annotation should load");
    Require(session.View().navigation.current_annotations.size() == 1, "loaded annotation should appear in navigation");
    Require(
        session.View().navigation.current_annotations[0].relationship ==
            spectiary::SampleAnnotationWorkflowRelationship::ExternalLabelResult,
        "metadata-backed annotation should start as external");
    Require(
        session.View().navigation.current_annotations[0]
                .labeling_owner_format ==
            spectiary::SampleLabelingOutputArtifactFormat::
                LegacyNpyWithSidecar,
        "legacy external annotation confirmation should retain NPY plus sidecar ownership");
    Require(
        session.View().navigation.current_annotations[0].can_activate_labeling,
        "categorical annotation should be draggable into labeling");

    result = Submit(session, ActivateLabelingTaskFromAnnotation(annotation_path));
    Require(session.View().labeling.has_active_task, "annotation activation should create an active task");
    Require(session.View().labeling.task_name == "Quality review", "annotation metadata task name should be reused");
    Require(session.View().labeling.current_code == 5, "labeling task should reuse annotation values");
    Require(!session.View().labeling.output_path && session.View().labeling.active_task_is_temporary,
        "legacy annotation import should create an output-free draft");
    (void)Submit(session, RemoveReadOnlyAnnotation(annotation_path));
    (void)Submit(session, SetActiveLabelingOutputPath(output_path));
    Require(session.View().labeling.output_path == output_path,
        "explicit Save As should establish a canonical owner");
    Require(
        session.View().navigation.current_annotations[0].relationship ==
            spectiary::SampleAnnotationWorkflowRelationship::LocalLabelingTask,
        "activated annotation should be shown as a local labeling task");
    const std::string labeling_filter_source_id =
        "labeling:" + session.View().labeling.task_id;

    result = Submit(session, RemoveReadOnlyAnnotation(output_path));
    Require(!result.action.workflow_changed, "removing an annotation should not delete the local task");
    Require(session.View().navigation.current_annotations.size() == 1, "local task should remain visible after annotation removal");
    Require(
        session.View().navigation.current_annotations[0].relationship ==
            spectiary::SampleAnnotationWorkflowRelationship::LocalLabelingTask,
        "remaining row should come from the local task record");
    Require(session.View().labeling.has_active_task, "removing an annotation should not remove the active local task");

    Require(
        Submit(session, DeactivateActiveLabelingTask())
            .action.workflow_changed,
        "the synthetic local task should pause before its output is removed externally");
    (void)Submit(session, AddReadOnlyAnnotation(output_path));
    Require(
        Submit(
            session,
            AddSampleFilterSource(
                labeling_filter_source_id))
            .action.workflow_changed,
        "the synthetic local task should be selectable as a filter source");
    (void)Submit(
        session,
        SetFilterValueSelected(
            labeling_filter_source_id,
            "9",
            true));
    Require(
        session.View().navigation.filter_active &&
            session.View().navigation.filtered_sample_count == 1,
        "the labeling filter should narrow the sequence before the task becomes missing");

    std::error_code remove_error;
    Require(
        std::filesystem::remove(
            output_path,
            remove_error) &&
            !remove_error,
        "the local task fixture should simulate external output removal");

    result = Submit(
        session,
        RemoveReadOnlyAnnotation(output_path));
    const spectiary::SourceCollectionSessionView abandoned_view =
        session.View();
    Require(
        result.action.workflow_changed &&
            abandoned_view.labeling.task_ids.empty() &&
            abandoned_view.navigation.current_annotations.empty() &&
            abandoned_view.filter.sources.empty() &&
            !abandoned_view.navigation.filter_active &&
            abandoned_view.navigation.filtered_sample_count == 3,
        "abandoning a synthetic missing task should clear its labeling filter and immediately restore the unfiltered sequence");

    Require(
        session.FlushStateCaches(),
        "abandoning the synthetic missing task should persist its workflow cleanup");
    const spectiary::SampleWorkflowStateCacheLoadResult workflow =
        spectiary::LoadSampleWorkflowStateCache(spectiary::RuntimePaths{},
            workflow_cache);
    const auto workflow_source =
        workflow.cache.sources_by_identity.find(
            abandoned_view.labeling.source_identity);
    Require(
        workflow.warning.empty() &&
            (workflow_source ==
                 workflow.cache.sources_by_identity.end() ||
             (std::find(
                  workflow_source->second
                      .selected_filter_source_ids.begin(),
                  workflow_source->second
                      .selected_filter_source_ids.end(),
                  labeling_filter_source_id) ==
                  workflow_source->second
                      .selected_filter_source_ids.end() &&
              std::none_of(
                  workflow_source->second
                      .filter_conditions.begin(),
                  workflow_source->second
                      .filter_conditions.end(),
                  [&labeling_filter_source_id](
                      const spectiary::SampleFilterCondition&
                          condition) {
                      return condition.source_id ==
                          labeling_filter_source_id;
                  }))),
        "the persisted workflow should not retain a ghost labeling filter after task abandonment");
}

void TestAnnotationActivationRequiresCurrentTaskToBeClosed()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path annotation_path = UniqueTempPath("_blocked_activation.npy");
    const std::filesystem::path formal_output_path = UniqueTempPath("_formal_output.asdf");

    spectiary::SampleLabelSet label_set;
    label_set.labels.push_back(spectiary::SampleLabelDefinition{5, "bad", 'b'});
    SaveLabelResultFixture(
        annotation_path,
        "external-task",
        "External task",
        {5, -1, 5},
        label_set,
        true);

    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    spectiary::SourceCollectionSessionResult result = Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(session.View().labeling.has_active_task, "current task should be active before activation attempt");
    result = Submit(session, SetActiveLabelingOutputPath(formal_output_path));
    Require(
        session.View().labeling.save_state.kind == spectiary::SampleLabelSaveStateKind::AutosavedToOutput,
        "current task should be formal before its later autosave failure");
    const std::string current_task_id = session.View().labeling.task_id;
    std::filesystem::remove(formal_output_path);
    std::filesystem::create_directories(formal_output_path);
    result = Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{7, "review", 'r'}));
    Require(
        session.View().labeling.save_state.kind == spectiary::SampleLabelSaveStateKind::Failed,
        "formal task should have a failed autosave guard");

    result = Submit(session, AddReadOnlyAnnotation(annotation_path));
    Require(result.loaded, "external annotation should load before blocked activation");
    result = Submit(session, ActivateLabelingTaskFromAnnotation(annotation_path));
    Require(session.View().labeling.has_active_task, "blocked activation should keep the active task");
    Require(session.View().labeling.task_id == current_task_id, "annotation activation must not switch active tasks");
    Require(
        session.View().labeling.save_state.kind == spectiary::SampleLabelSaveStateKind::Failed,
        "blocked activation should preserve the failed save state");
    const auto external = std::find_if(
        session.View().navigation.current_annotations.begin(),
        session.View().navigation.current_annotations.end(),
        [&annotation_path](
            const spectiary::SourceCollectionAnnotationValueView& annotation) {
            return annotation.path == annotation_path;
        });
    Require(
        external !=
                session.View().navigation.current_annotations.end() &&
            external->relationship ==
                spectiary::SampleAnnotationWorkflowRelationship::
                    ExternalLabelResult,
        "blocked activation should leave the requested annotation external");
}

void TestActivatingPlainIntegerAnnotationCreatesMetadataSidecar()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path annotation_path = UniqueTempPath("_plain.npy");
    SaveLabelResultFixture(
        annotation_path,
        "plain-fixture",
        "Plain fixture",
        {7, -1, 5},
        {},
        false);
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    spectiary::SourceCollectionSessionResult result = Submit(session, AddReadOnlyAnnotation(annotation_path));
    Require(result.loaded, "plain integer annotation should load");
    Require(
        session.View().navigation.current_annotations[0].relationship ==
            spectiary::SampleAnnotationWorkflowRelationship::PlainAnnotation,
        "annotation without metadata should start as plain");
    Require(
        session.View().navigation.current_annotations[0]
                .labeling_owner_format ==
            spectiary::SampleLabelingOutputArtifactFormat::None,
        "plain annotation should not claim a labeling output owner before activation");

    result = Submit(session, ActivateLabelingTaskFromAnnotation(annotation_path));
    Require(session.View().labeling.has_active_task, "plain annotation activation should create an active task");
    Require(session.View().labeling.current_code == 7, "plain annotation values should become editable label values");
    Require(session.View().labeling.label_set.labels.size() == 2, "plain annotation unique values should become labels");
    Require(session.View().labeling.label_set.labels[0].code == 5, "plain annotation labels should include value 5");
    Require(session.View().labeling.label_set.labels[1].code == 7, "plain annotation labels should include value 7");
    Require(
        session.View().labeling.active_task_is_temporary && !session.View().labeling.output_path,
        "plain annotation activation should create an output-free draft");
    Require(
        !std::filesystem::exists(
            spectiary::test_support::LegacyFixtureIo::MetadataPathForResult(annotation_path)),
        "plain annotation activation must not create a metadata sidecar");
    Require(
        session.View().navigation.current_annotations[0].relationship ==
            spectiary::SampleAnnotationWorkflowRelationship::PlainAnnotation,
        "draft import must not claim ownership of its plain annotation source");
}

void TestLegacyImportCanSaveAsCanonicalAsdf()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_migration_source.npy");
    const std::filesystem::path legacy_path =
        UniqueTempPath("_migration_labels.npy");
    const std::filesystem::path metadata_path =
        spectiary::test_support::LegacyFixtureIo::
            MetadataPathForResult(legacy_path);
    const std::filesystem::path canonical_path =
        UniqueTempPath("_migration_labels.asdf");
    spectiary::SampleLabelSet labels;
    labels.labels = {
        {5, "bad", 'b'},
        {7, "good", 'g'},
    };
    SaveLabelResultFixture(
        legacy_path,
        "session-legacy-task",
        "Session legacy task",
        {-1, 5, 7},
        labels,
        true);
    const std::string legacy_bytes =
        ReadBinaryFile(legacy_path);
    const std::string metadata_bytes =
        ReadBinaryFile(metadata_path);

    std::vector<std::size_t> loaded_indices;
    PreparedSession session =
        MakeSession(
            loaded_indices,
            source_path,
            3);
    (void)Submit(
        session,
        OpenSourceCollection(source_path, 0));
    Require(
        Submit(
            session,
            AddReadOnlyAnnotation(legacy_path))
            .loaded,
        "legacy migration session should attach its NPY result");
    (void)Submit(
        session,
        ActivateLabelingTaskFromAnnotation(legacy_path));
    const std::string promoted_task_id =
        session.View().labeling.task_id;
    Require(
        !session.View().labeling.output_path &&
            spectiary::IsCanonicalUuidV4(promoted_task_id) &&
            promoted_task_id != "session-legacy-task" &&
            session.View().labeling.task_name ==
                "Session legacy task",
        "legacy promotion should assign a fresh canonical UUID without inheriting the sidecar id");
    const std::string promoted_metadata_bytes =
        ReadBinaryFile(metadata_path);
    Require(
        promoted_metadata_bytes == metadata_bytes,
        "legacy import must preserve original sidecar identity metadata");

    const spectiary::SourceCollectionSessionResult migrated =
        Submit(
            session,
            SetActiveLabelingOutputPath(canonical_path));
    Require(
        migrated.action.navigation_inputs_changed &&
            session.View().labeling.output_path ==
                canonical_path &&
            std::filesystem::exists(canonical_path),
        "explicit output selection on an active legacy owner should migrate it to ASDF");
    Require(
        ReadBinaryFile(legacy_path) == legacy_bytes &&
            ReadBinaryFile(metadata_path) ==
                promoted_metadata_bytes,
        "session migration must leave the legacy NPY and promoted sidecar bytes unchanged");
    const spectiary::SampleLabelingAsdfReadResult read =
        spectiary::ReadSampleLabelingAsdfDocument(
            canonical_path);
    Require(
        read.succeeded() &&
            read.document->labeling.id ==
                promoted_task_id &&
            read.document->labeling.name ==
                "Session legacy task" &&
            read.document->annotation.values ==
                std::vector<std::int32_t>({-1, 5, 7}) &&
            read.document->labeling.labels.size() == 2,
        "session migration should preserve portable sidecar semantics in the canonical document");
    Require(
        std::any_of(
            session.View()
                .navigation.current_annotations.begin(),
            session.View()
                .navigation.current_annotations.end(),
            [&canonical_path](
                const spectiary::
                    SourceCollectionAnnotationValueView& annotation) {
                return annotation.path == canonical_path &&
                    annotation.relationship ==
                        spectiary::
                            SampleAnnotationWorkflowRelationship::
                                LocalLabelingTask;
            }),
        "successful migration should attach the new canonical owner to the current source session");
}

void TestImportedDraftDoesNotRequireMetadataSidecar()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path annotation_path = UniqueTempPath("_missing_metadata.npy");
    SaveLabelResultFixture(
        annotation_path,
        "plain-fixture",
        "Plain fixture",
        {7, -1, 5},
        {},
        false);
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    (void)Submit(session, AddReadOnlyAnnotation(annotation_path));

    spectiary::SourceCollectionSessionResult result =
        Submit(session, ActivateLabelingTaskFromAnnotation(annotation_path));
    Require(session.View().labeling.has_active_task, "plain annotation should become a local task");
    Require(!session.View().labeling.output_path,
        "imported draft must remain independent of the original annotation");
    Require(!std::filesystem::exists(
        spectiary::test_support::LegacyFixtureIo::MetadataPathForResult(annotation_path)),
        "import must not create a sidecar");
    result = Submit(session, AddReadOnlyAnnotation(annotation_path));
    Require(
        session.View().navigation.current_annotations.size() == 1,
        "same loaded output path should render as one annotation row");
    Require(
        session.View().navigation.current_annotations[0].relationship ==
            spectiary::SampleAnnotationWorkflowRelationship::PlainAnnotation,
        "reloading the original annotation should leave it read-only and independent");
    Require(
        !session.View().navigation.current_annotations[0].metadata_missing,
        "plain annotation must not report missing owner metadata");
    Require(
        session.View().navigation.current_annotations[0].display_text == "7",
        "original annotation values should drive the independent row");
}

void TestAnnotationLocalMatchRequiresSidecarTaskId()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path annotation_path = UniqueTempPath("_task_id_mismatch.npy");
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    TouchFile(source_path);

    spectiary::SampleLabelSet label_set;
    label_set.labels.push_back(spectiary::SampleLabelDefinition{5, "bad", 'b'});
    SaveLabelResultFixture(
        annotation_path,
        "external-task",
        "External task",
        {5, -1},
        label_set,
        true);

    const spectiary::SourceCollectionIdentity identity =
        spectiary::BuildSourceCollectionIdentity(*MakeSnapshot(source_path, 2, 0));
    spectiary::SampleLabelingTask local_task =
        spectiary::CreateSampleLabelingTask(
            "66666666-6666-4666-8666-666666666666",
            "Local task",
            2);
    local_task.persistence.output_path = annotation_path;
    local_task.persistence.output_format =
        spectiary::SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
    local_task.values.Complete() = {5, -1};

    spectiary::SampleLabelingSourceState source_state;
    source_state.sample_count = identity.spectrum_count;
    source_state.source_name = identity.source_name;
    source_state.source_fingerprint = identity.source_fingerprint;
    source_state.context_fingerprint = identity.context_fingerprint;
    source_state.tasks.push_back(std::move(local_task));
    spectiary::SampleLabelingStateCache cache;
    cache.sources.emplace(identity.id, std::move(source_state));
    Require(spectiary::SaveSampleLabelingStateCache(spectiary::RuntimePaths{}, labeling_cache, cache), "labeling cache fixture should save");

    std::vector<LoadedSourceSnapshot> loaded_snapshots;
    PreparedSession session(
        [&loaded_snapshots, source_path](const std::filesystem::path& path, std::size_t spectrum_index) {
            Require(path == source_path, "mismatch fixture should load the source path");
            loaded_snapshots.push_back(LoadedSourceSnapshot{path, spectrum_index});
            return MakeSnapshot(source_path, 2, spectrum_index);
        },
        {},
        navigation_cache,
        labeling_cache,
        UniqueTempPath("_workflow.json"));
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    spectiary::SourceCollectionSessionResult result = Submit(session, AddReadOnlyAnnotation(annotation_path));
    Require(result.loaded, "metadata-backed annotation should load");
    Require(!session.View().labeling.has_active_task, "mismatch fixture should start without an active task");
    Require(
        session.View().navigation.current_annotations[0].relationship ==
            spectiary::SampleAnnotationWorkflowRelationship::ExternalLabelResult,
        "same path and sample count should not make a local match without matching sidecar task id");

    result = Submit(session, ActivateLabelingTaskFromAnnotation(annotation_path));
    Require(
        !session.View().labeling.has_active_task,
        "activation must not bind an inactive same-path task when the sidecar task id differs");
    Require(
        session.View().navigation.current_annotations[0].relationship ==
            spectiary::SampleAnnotationWorkflowRelationship::ExternalLabelResult,
        "mismatched sidecar task id should keep the annotation external");
}

void TestSwitchingSourceCollectionRestoresWorkflowAndClearsFilters()
{
    const std::filesystem::path first_source_path = UniqueTempPath("_first.npy");
    const std::filesystem::path second_source_path = UniqueTempPath("_second.npy");
    std::vector<LoadedSourceSnapshot> loaded_snapshots;
    PreparedSession session = MakeMultiSourceSession(
        loaded_snapshots,
        first_source_path,
        3,
        second_source_path,
        2);

    (void)Submit(session, OpenSourceCollection(first_source_path, 0));
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(
            session,
            UpsertActiveLabel(
                spectiary::SampleLabelDefinition{1, "bad", 'b'}))
            .changed,
        "first source collection should accept a label definition");
    spectiary::SourceCollectionSessionResult result =
        Submit(session, AssignActiveLabelToCurrentSample(1));
    Require(session.View().labeling.current_code == 1, "first sample should be labeled before filtering");
    const std::filesystem::path annotation_path =
        AddPlainIntegerFilterAnnotation(session, {1, 2, 2}, "_first_filter.npy");
    const std::string source_id = AnnotationSourceId(annotation_path);
    result = Submit(session, AddSampleFilterSource(source_id));
    Require(session.View().filter.sources.size() == 1, "annotation sample filter should be explicitly selected");
    result = Submit(
        session,
        SetFilterValueSelected(source_id, "1", true));
    Require(session.View().navigation.filter_active, "annotation sample filter should affect sample navigation");
    Require(session.View().navigation.filtered_sample_count == 1, "filter should include the one matching sample");

    result = Submit(session, OpenSourceCollection(second_source_path, 0));
    Require(result.action.workflow_changed, "opening another source collection should change the active workflow");
    Require(!session.View().labeling.has_active_task, "second source collection should not inherit the first workflow");
    Require(session.View().filter.sources.empty(), "source switch should clear selected filter sources");
    Require(!session.View().navigation.filter_active, "source switch should clear navigation filtering");

    result = Submit(session, SwitchSourceCollection(0));
    Require(result.action.workflow_changed, "switching back should reactivate the first workflow");
    Require(session.View().labeling.has_active_task, "first source collection workflow should be restored");
    Require(session.View().labeling.current_code == 1, "first source collection label result should be restored");
    Require(
        session.View().filter.sources.size() == 1,
        "switching back should preserve the source-owned in-memory filter intent");
    Require(
        session.View().navigation.filter_active,
        "source reactivation should restore its live navigation filtering even without a disk cache path");
    const auto& source_transition =
        session.View().sample_transition;
    Require(
        source_transition &&
            source_transition->reason ==
                spectiary::SourceCollectionSampleTransitionReason::
                    SourceActivation &&
            source_transition->current_sample_index == 0 &&
            !source_transition->accepted_label_value,
        "source activation should replace prior sample-label feedback");

    result = Submit(session, UndoLastLabelWrite());
    Require(
        session.View().labeling.current_code == 1,
        "switching source identity should discard the previous source's label undo history");
}

void TestStandaloneCanonicalAsdfAnnotationAdoptsExactTask()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path annotation_path =
        UniqueTempPath("_standalone.asdf");
    const std::filesystem::path navigation_cache =
        UniqueTempPath("_standalone_navigation.json");
    const std::filesystem::path labeling_cache =
        UniqueTempPath("_standalone_labeling.json");
    const std::filesystem::path workflow_cache =
        UniqueTempPath("_standalone_workflow.json");
    TouchFile(source_path);
    std::vector<std::size_t> loaded_indices;
    PreparedSession session(
        [&loaded_indices, source_path](
            const std::filesystem::path& path,
            std::size_t spectrum_index) {
            Require(
                path == source_path,
                "standalone ASDF session should reload its source");
            loaded_indices.push_back(spectrum_index);
            return MakeSnapshot(source_path, 3, spectrum_index);
        },
        {},
        navigation_cache,
        labeling_cache,
        workflow_cache);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    const spectiary::SpectrumSnapshotHandle snapshot =
        session.CurrentSourceSnapshot();
    Require(snapshot != nullptr, "ASDF read-only fixture needs an active source");
    const spectiary::SourceCollectionIdentity identity =
        spectiary::BuildSourceCollectionIdentity(*snapshot);

    spectiary::SampleLabelingDocument document;
    document.source.base_identity = identity.id;
    document.source.kind = "test";
    document.source.name = identity.source_name;
    document.source.fingerprint = identity.source_fingerprint;
    document.source.sample_count = identity.spectrum_count;
    document.source.roster.identity_kind =
        std::string{
            spectiary::kSampleLabelingDocumentSourceIndexRoster};
    document.annotation.values = {5, -1, 9};
    document.labeling.id = "33333333-3333-4333-8333-333333333333";
    document.labeling.name = "Canonical quality";
    document.labeling.canonical_metadata = TestCanonicalMetadata();
    document.labeling.labels = {
        {5, "bad", "b"},
        {9, "good", "g"},
    };
    {
        std::ofstream stream(
            annotation_path,
            std::ios::binary | std::ios::trunc);
        const spectiary::SampleLabelingAsdfWriteResult write =
            spectiary::WriteSampleLabelingAsdfDocument(
                stream,
                document);
        Require(
            write.succeeded(),
            write.error.message.empty()
                ? "ASDF read-only fixture should write"
                : write.error.message);
    }
    const std::string original_bytes = ReadBinaryFile(annotation_path);

    spectiary::SourceCollectionSessionResult result =
        Submit(session, AddReadOnlyAnnotation(annotation_path));
    Require(result.loaded, "compatible ASDF annotation should attach");
    Require(
        session.View().navigation.current_annotations.size() == 1 &&
            session.View().navigation.current_annotations[0]
                 .can_activate_labeling &&
            session.View().navigation.current_annotations[0]
                    .labeling_owner_format ==
                spectiary::SampleLabelingOutputArtifactFormat::
                    CanonicalAsdf &&
            session.View().navigation.current_annotations[0]
                    .display_text == "bad (5)",
        "standalone ASDF should remain read-only until its explicit activation is offered");

    result = Submit(
        session,
        ActivateLabelingTaskFromAnnotation(annotation_path));
    Require(
        session.View().labeling.has_active_task &&
            session.View().labeling.task_id ==
                "33333333-3333-4333-8333-333333333333" &&
            session.View().labeling.task_name == "Canonical quality" &&
            session.View().labeling.current_code == 5 &&
            session.View().labeling.output_path == annotation_path &&
            session.View().labeling.label_set.labels.size() == 2 &&
            session.View().labeling.label_set.labels[1].code == 9 &&
            session.View().labeling.label_set.labels[1].name == "good",
        "explicit activation should adopt the ASDF task identity, metadata, values, and owner path exactly");
    Require(
        ReadBinaryFile(annotation_path) == original_bytes,
        "adoption must not rewrite the standalone ASDF document");

    const spectiary::SampleLabelingStateCacheLoadResult cache =
        spectiary::LoadSampleLabelingStateCache(spectiary::RuntimePaths{},
            labeling_cache,
            {},
            spectiary::SampleLabelingStateCacheLoadPolicy::
                AllowPersistentOutputsWithoutResultHydration);
    const auto source = cache.cache.sources.find(identity.id);
    Require(
        cache.issue_kind ==
                spectiary::SampleLabelingStateCacheLoadIssueKind::None &&
            source != cache.cache.sources.end() &&
            source->second.tasks.size() == 1 &&
            source->second.tasks[0].task_id ==
                "33333333-3333-4333-8333-333333333333" &&
            source->second.tasks[0].persistence.output_path == annotation_path &&
            source->second.tasks[0].persistence.output_format ==
                spectiary::SampleLabelingOutputArtifactFormat::
                    CanonicalAsdf &&
            !source->second.tasks[0]
                 .values.IsComplete(),
        "adoption should persist one canonical local owner record without creating a legacy owner");

    result = Submit(
        session,
        AssignActiveLabelToCurrentSample(9));
    const spectiary::SampleLabelingAsdfReadResult edited =
        spectiary::ReadSampleLabelingAsdfDocument(
            annotation_path);
    Require(
        result.labeling_issue ==
            spectiary::SampleLabelingOperationResult::Issue::None,
        "the first edit after adoption should retain its task and output leases");
    Require(
        result.label_write &&
            result.label_write->write.changed,
        "the first edit after adoption should be accepted as a labeling mutation");
    Require(
        session.View().labeling.current_code == 9,
        "the first edit after adoption should update the active runtime projection");
    Require(
        edited.succeeded(),
        edited.error.message.empty()
            ? "the first edit after adoption should leave a readable canonical ASDF"
            : edited.error.message);
    Require(
        edited.document->annotation.values ==
            std::vector<std::int32_t>({9, -1, 9}),
        "the first edit after adoption should publish through the existing canonical ASDF autosave pipeline");

    const std::filesystem::path conflicting_path =
        UniqueTempPath("_same_task_different_owner.asdf");
    {
        std::ofstream stream(
            conflicting_path,
            std::ios::binary | std::ios::trunc);
        Require(
            spectiary::WriteSampleLabelingAsdfDocument(
                stream,
                document)
                .succeeded(),
            "same-id conflict ASDF should write");
    }
    Require(
        Submit(
            session,
            AddReadOnlyAnnotation(conflicting_path))
            .loaded,
        "same-id conflict ASDF should attach read-only");
    const spectiary::SourceCollectionSessionResult conflict =
        Submit(
            session,
            ActivateLabelingTaskFromAnnotation(
                conflicting_path));
    Require(
        conflict.labeling_issue ==
                spectiary::SampleLabelingOperationResult::Issue::
                    EditTargetChanged &&
            session.View().labeling.task_id ==
                "33333333-3333-4333-8333-333333333333" &&
            session.View().labeling.output_path ==
                annotation_path &&
            session.View().labeling.task_ids.size() == 1,
        "a same-id different-owner adoption should report a conflict instead of inventing another task identity");

    spectiary::SampleLabelingController competing(
        labeling_cache);
    competing.ActivateSource(
        identity,
        spectiary::SampleLabelingCanonicalSourceDescriptor{
            .base_identity = identity.id,
            .source_kind = "test",
            .source_name = identity.source_name,
            .source_fingerprint =
                identity.source_fingerprint,
            .sample_count = identity.spectrum_count,
        });
    const spectiary::SampleLabelingOperationResult blocked =
        competing.ActivateTask("33333333-3333-4333-8333-333333333333");
    Require(
        !blocked.accepted &&
            blocked.issue ==
                spectiary::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable,
        "an adopted task should retain its canonical edit ownership lease");

    Require(
        Submit(
            session,
            DeactivateActiveLabelingTask())
            .action.workflow_changed,
        "the canonical owner should pause before its output disappears");
    std::error_code remove_error;
    const bool removed_original_owner =
        std::filesystem::remove(
            annotation_path,
            remove_error);
    Require(
        removed_original_owner && !remove_error,
        "the original canonical owner fixture should be deleted externally");

    const spectiary::SourceCollectionSessionResult recovered_conflict =
        Submit(
            session,
            ActivateLabelingTaskFromAnnotation(
                conflicting_path));
    const spectiary::SourceCollectionSessionView recovered_view =
        session.View();
    const auto recovered_owner = std::find_if(
        recovered_view.navigation.current_annotations.begin(),
        recovered_view.navigation.current_annotations.end(),
        [&conflicting_path](
            const spectiary::SourceCollectionAnnotationValueView&
                annotation) {
            return annotation.path == conflicting_path;
        });
    Require(
        recovered_conflict.labeling_issue ==
                spectiary::SampleLabelingOperationResult::Issue::None &&
            recovered_view.labeling.has_active_task &&
            recovered_view.labeling.output_path ==
                conflicting_path &&
            recovered_view.labeling.task_ids.size() == 1 &&
            recovered_owner !=
                recovered_view.navigation.current_annotations.end() &&
            recovered_owner->relationship ==
                spectiary::SampleAnnotationWorkflowRelationship::
                    LocalLabelingTask &&
            std::none_of(
                recovered_view.navigation.current_annotations.begin(),
                recovered_view.navigation.current_annotations.end(),
                [&annotation_path](
                    const spectiary::
                        SourceCollectionAnnotationValueView& annotation) {
                    return annotation.path == annotation_path;
                }) &&
            std::none_of(
                recovered_view.navigation.annotation_diagnostics.begin(),
                recovered_view.navigation.annotation_diagnostics.end(),
                [&annotation_path, &conflicting_path](
                    const spectiary::
                        SourceCollectionManifestDiagnostic& diagnostic) {
                    return diagnostic.kind ==
                            spectiary::
                                SourceCollectionManifestDiagnosticKind::
                                    AnnotationIgnored &&
                        (diagnostic.path == annotation_path ||
                         diagnostic.path == conflicting_path);
                }),
        "after owner A disappears, activating attached same-id B should adopt B as the unique local owner and safely retire A");

    Require(
        Submit(
            session,
            DeactivateActiveLabelingTask())
            .action.workflow_changed,
        "the recovered owner should pause before its output is moved");
    Require(
        session.FlushStateCaches(),
        "the recovered owner attachment should flush before the moved-path restart");
    const std::filesystem::path moved_path =
        UniqueTempPath("_standalone_moved.asdf");
    std::error_code move_error;
    std::filesystem::rename(
        conflicting_path,
        moved_path,
        move_error);
    Require(
        !move_error,
        "canonical owner fixture should move to its replacement path");

    std::vector<std::size_t> relinking_loads;
    PreparedSession relinking(
        [&relinking_loads, source_path](
            const std::filesystem::path& path,
            std::size_t spectrum_index) {
            Require(
                path == source_path,
                "moved owner relink should reload its source");
            relinking_loads.push_back(spectrum_index);
            return MakeSnapshot(
                source_path,
                3,
                spectrum_index);
        },
        {},
        navigation_cache,
        labeling_cache,
        workflow_cache);
    (void)relinking.Open(
        source_path,
        0,
        {conflicting_path});
    Require(
        std::any_of(
            relinking.View().navigation
                .annotation_diagnostics.begin(),
            relinking.View().navigation
                .annotation_diagnostics.end(),
            [&conflicting_path](
                const spectiary::
                    SourceCollectionManifestDiagnostic& diagnostic) {
                return diagnostic.path == conflicting_path &&
                    diagnostic.kind ==
                        spectiary::
                            SourceCollectionManifestDiagnosticKind::
                                AnnotationIgnored;
            }),
        "the moved-path restart should expose the old attachment warning before relink");
    const spectiary::SourceCollectionSessionResult imported =
        Submit(
            relinking,
            AddReadOnlyAnnotation(moved_path));
    Require(
        imported.loaded,
        "the moved canonical owner should attach as an imported annotation");
    const spectiary::SourceCollectionSessionView imported_view =
        relinking.View();
    const auto moved_annotation = std::find_if(
        imported_view.navigation.current_annotations.begin(),
        imported_view.navigation.current_annotations.end(),
        [&moved_path](
            const spectiary::SourceCollectionAnnotationValueView&
                annotation) {
            return annotation.path == moved_path;
        });
    Require(
        !imported_view.labeling.has_active_task &&
            imported_view.labeling.task_ids.size() == 1 &&
            moved_annotation !=
                imported_view.navigation.current_annotations.end() &&
            moved_annotation->relationship ==
                spectiary::SampleAnnotationWorkflowRelationship::
                    LocalLabelingTask &&
            moved_annotation->can_activate_labeling &&
            std::none_of(
                imported_view.navigation.current_annotations.begin(),
                imported_view.navigation.current_annotations.end(),
                [&annotation_path, &conflicting_path](
                    const spectiary::SourceCollectionAnnotationValueView&
                        annotation) {
                    return annotation.path == annotation_path ||
                        annotation.path == conflicting_path;
                }) &&
            std::none_of(
                imported_view.navigation
                    .annotation_diagnostics.begin(),
                imported_view.navigation
                    .annotation_diagnostics.end(),
                [&annotation_path, &conflicting_path, &moved_path](
                    const spectiary::
                        SourceCollectionManifestDiagnostic&
                            diagnostic) {
                    return diagnostic.kind ==
                            spectiary::
                                SourceCollectionManifestDiagnosticKind::
                                    AnnotationIgnored &&
                        (diagnostic.path == annotation_path ||
                         diagnostic.path == conflicting_path ||
                         diagnostic.path == moved_path);
                }),
        "importing a moved same-id ASDF should immediately merge it into the missing local owner and clear the stale attachment warning");

    Require(
        relinking.FlushStateCaches(),
        "the automatically relinked owner should flush before restart verification");
    {
        std::vector<std::size_t> restarted_loads;
        PreparedSession restarted(
            [&restarted_loads, source_path](
                const std::filesystem::path& path,
                std::size_t spectrum_index) {
                Require(
                    path == source_path,
                    "relinked owner restart should reload its source");
                restarted_loads.push_back(spectrum_index);
                return MakeSnapshot(
                    source_path,
                    3,
                    spectrum_index);
            },
            {},
            navigation_cache,
            labeling_cache,
            workflow_cache);
        (void)restarted.Open(
            source_path,
            0,
            {moved_path});
        const spectiary::SourceCollectionSessionView restarted_view =
            restarted.View();
        const std::size_t restarted_local_count =
            static_cast<std::size_t>(std::count_if(
                restarted_view.navigation.current_annotations.begin(),
                restarted_view.navigation.current_annotations.end(),
                [](const spectiary::
                       SourceCollectionAnnotationValueView& annotation) {
                    return annotation.relationship ==
                        spectiary::
                            SampleAnnotationWorkflowRelationship::
                                LocalLabelingTask;
                }));
        const auto restarted_owner = std::find_if(
            restarted_view.navigation.current_annotations.begin(),
            restarted_view.navigation.current_annotations.end(),
            [&moved_path](
                const spectiary::SourceCollectionAnnotationValueView&
                    annotation) {
                return annotation.path == moved_path;
            });
        Require(
            restarted_local_count == 1 &&
                restarted_owner !=
                    restarted_view.navigation.current_annotations.end() &&
                restarted_owner->relationship ==
                    spectiary::
                        SampleAnnotationWorkflowRelationship::
                            LocalLabelingTask &&
                restarted_owner->can_activate_labeling &&
                std::none_of(
                    restarted_view.navigation.current_annotations.begin(),
                    restarted_view.navigation.current_annotations.end(),
                    [&annotation_path, &conflicting_path](
                        const spectiary::
                            SourceCollectionAnnotationValueView&
                                annotation) {
                        return annotation.path == annotation_path ||
                            annotation.path == conflicting_path;
                    }),
            "after restart the replacement path should remain the unique activatable local owner and the old path should stay retired");

        const spectiary::SourceCollectionSessionResult
            restarted_activation =
                Submit(
                    restarted,
                    ActivateLabelingTaskFromAnnotation(
                        moved_path));
        Require(
            restarted_activation.labeling_issue ==
                    spectiary::SampleLabelingOperationResult::
                        Issue::None &&
                restarted.View().labeling.has_active_task &&
                restarted.View().labeling.output_path ==
                    moved_path,
            "the restarted replacement owner should activate directly without an external-annotation warning");
        (void)Submit(
            restarted,
            DeactivateActiveLabelingTask());
    }

    const spectiary::SourceCollectionSessionResult relinked =
        Submit(
            relinking,
            ActivateLabelingTaskFromAnnotation(moved_path));
    const spectiary::SourceCollectionSessionView relinked_view =
        relinking.View();
    Require(
        relinked.action.workflow_changed &&
            relinked.labeling_issue ==
                spectiary::SampleLabelingOperationResult::Issue::None &&
            relinked_view.labeling.has_active_task &&
            relinked_view.labeling.task_id ==
                "33333333-3333-4333-8333-333333333333" &&
            relinked_view.labeling.output_path == moved_path &&
            relinked_view.labeling.task_ids.size() == 1,
        "the automatically relinked local owner should activate directly without external-annotation conversion");
    Require(
        std::none_of(
            relinked_view.navigation.current_annotations.begin(),
            relinked_view.navigation.current_annotations.end(),
            [&annotation_path, &conflicting_path](
                const spectiary::SourceCollectionAnnotationValueView&
                    annotation) {
                return annotation.path == annotation_path ||
                    annotation.path == conflicting_path;
            }),
        "successful relink should retire the missing old-path attachment row");

    const spectiary::SampleLabelingStateCacheLoadResult relinked_cache =
        spectiary::LoadSampleLabelingStateCache(spectiary::RuntimePaths{},
            labeling_cache,
            {},
            spectiary::SampleLabelingStateCacheLoadPolicy::
                AllowPersistentOutputsWithoutResultHydration);
    const auto relinked_source =
        relinked_cache.cache.sources.find(identity.id);
    Require(
        relinked_cache.issue_kind ==
                spectiary::SampleLabelingStateCacheLoadIssueKind::None &&
            relinked_source != relinked_cache.cache.sources.end() &&
            relinked_source->second.tasks.size() == 1 &&
            relinked_source->second.tasks.front().persistence.output_path ==
                moved_path,
        "automatic relink should durably replace the missing owner path");

    const spectiary::SourceCollectionSessionResult deleted =
        Submit(
            relinking,
            DeleteActiveLabelingTask());
    Require(
        deleted.action.workflow_changed &&
            !relinking.View().labeling.has_active_task &&
            relinking.View().labeling.task_ids.empty() &&
            std::filesystem::exists(moved_path),
        "the relinked task should be deletable without deleting its ASDF result");
}

void TestStandaloneCanonicalAsdfAdoptionReopensCurrentGeneration()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_adoption_reopen.npy");
    const std::filesystem::path annotation_path =
        UniqueTempPath("_adoption_reopen.asdf");
    TouchFile(source_path);
    std::vector<std::size_t> loaded_indices;
    PreparedSession session =
        MakeSession(loaded_indices, source_path, 3);
    (void)Submit(
        session,
        OpenSourceCollection(source_path, 0));

    const spectiary::SpectrumSnapshotHandle snapshot =
        session.CurrentSourceSnapshot();
    Require(
        snapshot != nullptr,
        "adoption reopen fixture needs an active source");
    const spectiary::SourceCollectionIdentity identity =
        spectiary::BuildSourceCollectionIdentity(*snapshot);

    spectiary::SampleLabelingDocument document;
    document.source.base_identity = identity.id;
    document.source.kind = "test";
    document.source.name = identity.source_name;
    document.source.fingerprint =
        identity.source_fingerprint;
    document.source.sample_count =
        identity.spectrum_count;
    document.source.roster.identity_kind =
        std::string{
            spectiary::
                kSampleLabelingDocumentSourceIndexRoster};
    document.annotation.values = {5, -1, 9};
    document.labeling.id = "44444444-4444-4444-8444-444444444444";
    document.labeling.name = "Attached generation";
    document.labeling.canonical_metadata = TestCanonicalMetadata();
    document.labeling.labels = {
        {5, "bad", "b"},
        {9, "good", "g"},
    };
    {
        std::ofstream stream(
            annotation_path,
            std::ios::binary | std::ios::trunc);
        Require(
            spectiary::WriteSampleLabelingAsdfDocument(
                stream,
                document)
                .succeeded(),
            "attached ASDF generation should write");
    }
    Require(
        Submit(
            session,
            AddReadOnlyAnnotation(annotation_path))
            .loaded,
        "attached ASDF generation should load read-only");

    document.labeling.id = "55555555-5555-4555-8555-555555555555";
    document.labeling.name = "Replacement generation";
    {
        std::ofstream stream(
            annotation_path,
            std::ios::binary | std::ios::trunc);
        Require(
            spectiary::WriteSampleLabelingAsdfDocument(
                stream,
                document)
                .succeeded(),
            "replacement ASDF generation should write");
    }
    const std::string replacement_bytes =
        ReadBinaryFile(annotation_path);

    const spectiary::SourceCollectionSessionResult result =
        Submit(
            session,
            ActivateLabelingTaskFromAnnotation(
                annotation_path));
    Require(
        !session.View().labeling.has_active_task &&
            result.labeling_issue ==
                spectiary::SampleLabelingOperationResult::Issue::
                    EditTargetChanged,
        "adoption must fail closed when the leased durable generation no longer has the attached task identity");
    Require(
        ReadBinaryFile(annotation_path) ==
            replacement_bytes,
        "failed adoption must not rewrite the current durable generation");
}

void TestCanonicalAsdfAnnotationActivatesPersistedOwner()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_canonical_owner.npy");
    const std::filesystem::path annotation_path =
        UniqueTempPath("_canonical_owner.asdf");
    const std::filesystem::path labeling_cache =
        UniqueTempPath("_canonical_owner_labeling.json");
    TouchFile(source_path);

    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(source_path, 3, 0);
    const spectiary::SourceCollectionIdentity identity =
        spectiary::BuildSourceCollectionIdentity(
            *snapshot);

    spectiary::SampleLabelingDocument document;
    document.source.base_identity = identity.id;
    document.source.kind = "test";
    document.source.name = identity.source_name;
    document.source.fingerprint =
        identity.source_fingerprint;
    document.source.sample_count = identity.spectrum_count;
    document.source.roster.identity_kind =
        std::string{
            spectiary::
                kSampleLabelingDocumentSourceIndexRoster};
    document.annotation.values = {5, -1, 9};
    document.labeling.id = "33333333-3333-4333-8333-333333333333";
    document.labeling.name = "Canonical quality";
    document.labeling.canonical_metadata = TestCanonicalMetadata();
    document.labeling.labels = {
        {5, "bad", "b"},
        {9, "good", "g"},
    };
    {
        std::ofstream stream(
            annotation_path,
            std::ios::binary | std::ios::trunc);
        const spectiary::SampleLabelingAsdfWriteResult write =
            spectiary::WriteSampleLabelingAsdfDocument(
                stream,
                document);
        Require(
            write.succeeded(),
            write.error.message.empty()
                ? "canonical owner fixture should write"
                : write.error.message);
    }
    const std::string original_bytes =
        ReadBinaryFile(annotation_path);

    spectiary::SampleLabelingTask cached =
        spectiary::CreateSampleLabelingTask(
            "33333333-3333-4333-8333-333333333333",
            "stale cache name",
            3);
    cached.canonical_metadata =
        document.labeling.canonical_metadata;
    cached.label_set.labels = {
        {99, "stale cache label", 's'},
    };
    cached.values.Complete()[1] = 9;
    cached.persistence.pending_sample_indices.insert(1);
    cached.persistence.output_path = annotation_path;
    cached.persistence.output_format =
        spectiary::SampleLabelingOutputArtifactFormat::
            CanonicalAsdf;
    cached.persistence.save_state.kind =
        spectiary::SampleLabelSaveStateKind::Pending;

    spectiary::SampleLabelingSourceState source_state;
    source_state.sample_count = identity.spectrum_count;
    source_state.source_name = identity.source_name;
    source_state.source_fingerprint =
        identity.source_fingerprint;
    source_state.context_fingerprint =
        identity.context_fingerprint;
    source_state.tasks.push_back(std::move(cached));
    spectiary::SampleLabelingStateCache cache;
    cache.sources.emplace(
        identity.id,
        std::move(source_state));
    Require(
        spectiary::SaveSampleLabelingStateCache(spectiary::RuntimePaths{},
            labeling_cache,
            cache),
        "canonical owner cache fixture should save");

    std::vector<std::size_t> loaded_indices;
    std::size_t canonical_document_publication_attempts = 0;
    std::size_t canonical_values_publication_attempts = 0;
    PreparedSession session(
        [&loaded_indices, source_path](
            const std::filesystem::path& path,
            std::size_t spectrum_index) {
            Require(
                path == source_path,
                "canonical owner session should reload its source");
            loaded_indices.push_back(spectrum_index);
            return MakeSnapshot(
                source_path,
                3,
                spectrum_index);
        },
        {},
        UniqueTempPath(
            "_canonical_owner_navigation.json"),
        labeling_cache,
        UniqueTempPath(
            "_canonical_owner_workflow.json"),
        [&canonical_document_publication_attempts](
            const spectiary::SampleLabelingAsdfOpenSnapshot&
                owner_snapshot,
            const spectiary::SampleLabelingDocument& document,
            const spectiary::
                SampleLabelingCanonicalSourceDescriptor& source) {
            ++canonical_document_publication_attempts;
            if (canonical_document_publication_attempts == 1) {
                const spectiary::SampleLabelingAsdfStoreWriteResult
                    write = spectiary::
                        RewriteSampleLabelingAsdfDocumentAtomically(
                            owner_snapshot,
                            document);
                if (!write.succeeded()) {
                    return spectiary::
                        SampleLabelingAsdfStoreGenerationWriteResult{
                            .error = write.error};
                }
                return spectiary::
                    SampleLabelingAsdfStoreGenerationWriteResult{
                        .document_replaced = true,
                        .error = {
                            .kind = spectiary::
                                SampleLabelingAsdfStoreErrorKind::
                                    AtomicWriteFailure,
                            .message =
                                "injected session metadata reopen failure"}};
            }
            return spectiary::
                RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
                    owner_snapshot,
                    document,
                    spectiary::SampleLabelingCompatibilityView(
                        source));
        },
        [&canonical_values_publication_attempts](
            spectiary::SampleLabelingAsdfOpenSnapshot&
                owner_snapshot,
            const spectiary::SampleLabelingDocument& document) {
            ++canonical_values_publication_attempts;
            if (canonical_values_publication_attempts == 1) {
                return spectiary::
                    sample_labeling_asdf_store_test_seam::
                        RewriteWithBeforeReplace(
                            owner_snapshot,
                            document,
                            [](const std::filesystem::path&,
                               const std::filesystem::path&) {
                                throw std::runtime_error(
                                    "injected canonical session publication failure");
                            });
            }
            return spectiary::
                RewriteSampleLabelingAsdfValuesAtomically(
                    owner_snapshot,
                    document);
        });
    const spectiary::SourceCollectionSessionResult opened =
        session.Open(
            source_path,
            0,
            {annotation_path});
    Require(
        opened.loaded &&
            !session.View().labeling.has_active_task,
        "an inactive canonical owner should attach without becoming editable before activation");

    const spectiary::SourceCollectionSessionView before =
        session.View();
    Require(
        before.navigation.current_annotations.size() == 1 &&
            before.navigation.current_annotations[0].name ==
                "Canonical quality" &&
            before.navigation.current_annotations[0]
                .display_text == "bad (5)" &&
            before.navigation.current_annotations[0]
                .can_activate_labeling &&
            !before.navigation.current_annotations[0]
                 .metadata_missing,
        "an attached canonical owner should project canonical metadata and values without requiring a legacy sidecar");
    Require(
        before.filter.available_sources.size() == 1,
        "an attached canonical owner should expose one local labeling filter source");
    const auto good_filter = std::find_if(
        before.filter.available_sources[0].options.begin(),
        before.filter.available_sources[0].options.end(),
        [](const spectiary::SampleFilterValueOption& option) {
            return option.key == "9";
        });
    Require(
        good_filter !=
                before.filter.available_sources[0]
                    .options.end() &&
            good_filter->display_text == "good (9)" &&
            good_filter->sample_count == 1,
        "canonical filter projection must use ASDF values without persisted pending edits");

    spectiary::SampleLabelingDocument newer_document =
        document;
    newer_document.annotation.values = {9, -1, 9};
    newer_document.labeling.name =
        "Canonical quality generation B";
    newer_document.labeling.labels = {
        {5, "bad generation B", "b"},
        {9, "good generation B", "g"},
    };
    {
        std::ofstream stream(
            annotation_path,
            std::ios::binary | std::ios::trunc);
        const spectiary::SampleLabelingAsdfWriteResult write =
            spectiary::WriteSampleLabelingAsdfDocument(
                stream,
                newer_document);
        Require(
            write.succeeded(),
            write.error.message.empty()
                ? "newer canonical owner generation should publish"
                : write.error.message);
    }
    const std::string newer_generation_bytes =
        ReadBinaryFile(annotation_path);
    Require(
        newer_generation_bytes != original_bytes,
        "generation fixture should replace the ASDF bytes after attachment and before activation");

    const spectiary::SourceCollectionSessionResult activated =
        Submit(
            session,
            ActivateLabelingTaskFromAnnotation(
                annotation_path));
    const spectiary::SourceCollectionLabelingView& labeling =
        session.View().labeling;
    Require(
        activated.action.workflow_changed &&
            labeling.has_active_task &&
            labeling.task_id ==
                "33333333-3333-4333-8333-333333333333" &&
            labeling.task_name ==
                "Canonical quality generation B" &&
            labeling.current_code == 9 &&
            labeling.label_set.labels.size() == 2 &&
            labeling.output_path ==
                std::optional<std::filesystem::path>{
                    annotation_path},
        "activating the attached owner should hydrate its editable task from the leased ASDF generation");

    const spectiary::SourceCollectionSessionView after_activation =
        session.View();
    Require(
        after_activation.navigation.current_annotations.size() == 1 &&
            after_activation.navigation.current_annotations[0].name ==
                "Canonical quality generation B" &&
            after_activation.navigation.current_annotations[0]
                    .display_text ==
                "good generation B (9)",
        "annotation display should follow the hydrated controller generation instead of the stale attached document");
    Require(
        after_activation.filter.available_sources.size() == 1,
        "hydrated canonical owner should remain available as one filter source");
    const auto generation_b_filter = std::find_if(
        after_activation.filter.available_sources[0].options.begin(),
        after_activation.filter.available_sources[0].options.end(),
        [](const spectiary::SampleFilterValueOption& option) {
            return option.key == "9";
        });
    Require(
        generation_b_filter !=
                after_activation.filter.available_sources[0]
                    .options.end() &&
            generation_b_filter->display_text ==
                "good generation B (9)" &&
            generation_b_filter->sample_count == 2,
        "filter projection should follow the hydrated controller generation instead of the stale attached document");

    const std::string canonical_source_id =
        after_activation.filter.available_sources[0].id;
    Require(
        Submit(
            session,
            AddSampleFilterSource(
                canonical_source_id))
            .action.workflow_changed,
        "canonical retry fixture should add the owner as a filter source");
    Require(
        Submit(
            session,
            SetSampleSortSource(
                "source-order"))
            .action.navigation_inputs_changed,
        "canonical retry fixture should activate an existing sorting source");

    const spectiary::SourceCollectionSessionResult edited =
        Submit(
            session,
            AssignActiveLabelToCurrentSample(5));
    const spectiary::SourceCollectionSessionView after_edit =
        session.View();
    Require(
        edited.label_write &&
            edited.label_write->write.changed &&
            edited.label_write->operation.state_saved &&
            edited.label_write->operation.output_save_attempted &&
            !edited.label_write->operation.output_saved &&
            edited.label_write->operation.output_retry_scheduled &&
            canonical_values_publication_attempts == 1 &&
            canonical_document_publication_attempts == 0,
        "canonical session edit should retain its runtime dirty edits after the injected publication failure");
    Require(
        ReadBinaryFile(annotation_path) ==
            newer_generation_bytes,
        "failed canonical publication should retain the old trusted ASDF generation");
    Require(
        after_edit.navigation.current_annotations.size() == 1 &&
            after_edit.navigation.current_annotations[0]
                    .display_text ==
                "bad generation B (5)",
        "failed canonical publication should still present the newest runtime overlay while the task is active");

    spectiary::SourceCollectionSessionResult maintenance;
    bool retry_published = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::optional<
            spectiary::LocalUserStateSaveScheduler::
                TimePoint>
            deadline = session.NextMaintenanceDeadline();
        Require(
            deadline.has_value(),
            "failed canonical publication should expose a retry deadline");
        maintenance = session.RunMaintenance(*deadline);
        if (canonical_values_publication_attempts >= 2) {
            retry_published = true;
            break;
        }
    }

    const spectiary::SampleLabelingAsdfReadResult
        persisted_generation =
            spectiary::ReadSampleLabelingAsdfDocument(
                annotation_path);
    const spectiary::SourceCollectionSessionView after_retry =
        session.View();
    const auto refreshed_filter_source = std::find_if(
        after_retry.filter.sources.begin(),
        after_retry.filter.sources.end(),
        [&canonical_source_id](const auto& source) {
            return source.id == canonical_source_id;
        });
    const auto find_refreshed_option =
        [&refreshed_filter_source, &after_retry](
            std::string_view key) {
            if (refreshed_filter_source ==
                after_retry.filter.sources.end()) {
                return static_cast<const spectiary::
                    SampleFilterValueOption*>(nullptr);
            }
            const auto option = std::find_if(
                refreshed_filter_source->options.begin(),
                refreshed_filter_source->options.end(),
                [key](const auto& candidate) {
                    return candidate.key == key;
                });
            return option ==
                    refreshed_filter_source->options.end()
                ? nullptr
                : &*option;
        };
    const spectiary::SampleFilterValueOption*
        refreshed_bad = find_refreshed_option("5");
    const spectiary::SampleFilterValueOption*
        refreshed_good = find_refreshed_option("9");
    Require(
        retry_published &&
            canonical_values_publication_attempts == 2 &&
            canonical_document_publication_attempts == 0 &&
            maintenance.action.navigation_inputs_changed &&
            persisted_generation.succeeded() &&
            persisted_generation.document->annotation.values ==
                std::vector<std::int32_t>({5, -1, 9}) &&
            persisted_generation.document->labeling.name ==
                newer_document.labeling.name,
        "maintenance should expose canonical retry publication and persist only the newest values generation");
    Require(
        refreshed_filter_source !=
                after_retry.filter.sources.end() &&
            refreshed_bad != nullptr &&
            refreshed_bad->sample_count == 1 &&
            refreshed_good != nullptr &&
            refreshed_good->sample_count == 1 &&
            after_retry.sorting.active &&
            after_retry.sorting.active_source_id ==
                "source-order",
        "canonical retry publication should rebuild filter and sorting projections from the advanced generation");

    const spectiary::SourceCollectionSessionResult
        metadata_edited =
            Submit(
                session,
                UpdateActiveLabel(
                    9,
                    spectiary::SampleLabelDefinition{
                        11,
                        "excellent generation C",
                        'e'},
                    true));
    const spectiary::SourceCollectionSessionView
        after_failed_metadata_edit = session.View();
    Require(
        metadata_edited.changed &&
            canonical_values_publication_attempts == 2 &&
            canonical_document_publication_attempts == 1 &&
            after_failed_metadata_edit.labeling.save_state.kind ==
                spectiary::SampleLabelSaveStateKind::Failed,
        "a session metadata rewrite whose replacement cannot reopen should remain visibly failed and retryable");

    spectiary::SourceCollectionSessionResult
        metadata_maintenance;
    bool metadata_retry_published = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::optional<
            spectiary::LocalUserStateSaveScheduler::TimePoint>
            deadline = session.NextMaintenanceDeadline();
        Require(
            deadline.has_value(),
            "failed session metadata reopen should expose a retry deadline");
        metadata_maintenance =
            session.RunMaintenance(*deadline);
        if (canonical_document_publication_attempts >= 2) {
            metadata_retry_published = true;
            break;
        }
    }
    const spectiary::SampleLabelingAsdfReadResult
        metadata_generation =
            spectiary::ReadSampleLabelingAsdfDocument(
                annotation_path);
    const spectiary::SourceCollectionSessionView
        after_metadata_edit = session.View();
    const auto metadata_filter_source = std::find_if(
        after_metadata_edit.filter.sources.begin(),
        after_metadata_edit.filter.sources.end(),
        [&canonical_source_id](const auto& source) {
            return source.id == canonical_source_id;
        });
    const auto generation_c_option =
        metadata_filter_source ==
                after_metadata_edit.filter.sources.end()
        ? static_cast<const spectiary::SampleFilterValueOption*>(
              nullptr)
        : [&metadata_filter_source]() {
              const auto option = std::find_if(
                  metadata_filter_source->options.begin(),
                  metadata_filter_source->options.end(),
                  [](const auto& candidate) {
                      return candidate.key == "11";
                  });
              return option ==
                      metadata_filter_source->options.end()
                  ? static_cast<const spectiary::
                        SampleFilterValueOption*>(nullptr)
                  : &*option;
          }();
    Require(
        metadata_retry_published &&
            metadata_maintenance.action
                .navigation_inputs_changed &&
            canonical_values_publication_attempts == 2 &&
            canonical_document_publication_attempts == 2 &&
            after_metadata_edit.labeling.save_state.kind ==
                spectiary::SampleLabelSaveStateKind::
                    AutosavedToOutput &&
            metadata_generation.succeeded() &&
            metadata_generation.document->labeling.labels.size() == 2 &&
            metadata_generation.document->labeling.labels[1].code == 11 &&
            metadata_generation.document->labeling.labels[1].name ==
                "excellent generation C" &&
            metadata_generation.document->labeling.labels[1].shortcut ==
                "e" &&
            metadata_generation.document->annotation.values ==
                std::vector<std::int32_t>({5, -1, 11}) &&
            generation_c_option != nullptr &&
            generation_c_option->display_text ==
                "excellent generation C (11)" &&
            generation_c_option->sample_count == 1,
        "session retry should reopen a replaced metadata generation, publish recoded values and definitions together, and refresh the attached generation");

    const spectiary::SourceCollectionSessionResult deactivated =
        Submit(
            session,
            DeactivateActiveLabelingTask());
    const spectiary::SourceCollectionSessionView after_deactivation =
        session.View();
    Require(
        deactivated.action.workflow_changed &&
            !after_deactivation.labeling.has_active_task &&
            after_deactivation.navigation.current_annotations.size() == 1 &&
            after_deactivation.navigation.current_annotations[0]
                    .display_text ==
                "bad generation B (5)",
        "deactivation after canonical retry should retain the newly published attached generation");
}

void TestCanonicalAsdfDeactivationRetainsHydratedAttachmentGeneration()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_canonical_deactivation_owner.npy");
    const std::filesystem::path annotation_path =
        UniqueTempPath("_canonical_deactivation_owner.asdf");
    const std::filesystem::path labeling_cache =
        UniqueTempPath("_canonical_deactivation_owner_labeling.json");
    TouchFile(source_path);

    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(source_path, 3, 0);
    const spectiary::SourceCollectionIdentity identity =
        spectiary::BuildSourceCollectionIdentity(*snapshot);

    spectiary::SampleLabelingDocument generation_a;
    generation_a.source.base_identity = identity.id;
    generation_a.source.kind = "test";
    generation_a.source.name = identity.source_name;
    generation_a.source.fingerprint =
        identity.source_fingerprint;
    generation_a.source.sample_count =
        identity.spectrum_count;
    generation_a.source.roster.identity_kind =
        std::string{
            spectiary::
                kSampleLabelingDocumentSourceIndexRoster};
    generation_a.annotation.values = {5, -1, 9};
    generation_a.labeling.id =
        "33333333-3333-4333-8333-333333333333";
    generation_a.labeling.name =
        "Canonical quality generation A";
    generation_a.labeling.canonical_metadata = TestCanonicalMetadata();
    generation_a.labeling.labels = {
        {5, "bad generation A", "b"},
        {9, "good generation A", "g"},
    };
    {
        std::ofstream stream(
            annotation_path,
            std::ios::binary | std::ios::trunc);
        const spectiary::SampleLabelingAsdfWriteResult write =
            spectiary::WriteSampleLabelingAsdfDocument(
                stream,
                generation_a);
        Require(
            write.succeeded(),
            write.error.message.empty()
                ? "canonical deactivation generation A should write"
                : write.error.message);
    }

    spectiary::SampleLabelingTask cached =
        spectiary::CreateSampleLabelingTask(
            "33333333-3333-4333-8333-333333333333",
            "structural cache owner",
            3);
    cached.persistence.output_path = annotation_path;
    cached.persistence.output_format =
        spectiary::SampleLabelingOutputArtifactFormat::
            CanonicalAsdf;
    spectiary::SampleLabelingSourceState source_state;
    source_state.sample_count = identity.spectrum_count;
    source_state.source_name = identity.source_name;
    source_state.source_fingerprint =
        identity.source_fingerprint;
    source_state.context_fingerprint =
        identity.context_fingerprint;
    source_state.tasks.push_back(std::move(cached));
    spectiary::SampleLabelingStateCache cache;
    cache.sources.emplace(
        identity.id,
        std::move(source_state));
    Require(
        spectiary::SaveSampleLabelingStateCache(spectiary::RuntimePaths{},
            labeling_cache,
            cache),
        "canonical deactivation owner cache should save");

    std::vector<std::size_t> loaded_indices;
    PreparedSession session(
        [&loaded_indices, source_path](
            const std::filesystem::path& path,
            std::size_t spectrum_index) {
            Require(
                path == source_path,
                "canonical deactivation session should reload its source");
            loaded_indices.push_back(spectrum_index);
            return MakeSnapshot(
                source_path,
                3,
                spectrum_index);
        },
        {},
        UniqueTempPath(
            "_canonical_deactivation_navigation.json"),
        labeling_cache,
        UniqueTempPath(
            "_canonical_deactivation_workflow.json"));
    Require(
        session.Open(
                source_path,
                0,
                {annotation_path})
            .loaded,
        "canonical deactivation generation A should attach");

    spectiary::SampleLabelingDocument generation_b =
        generation_a;
    generation_b.annotation.values = {9, 9, -1};
    generation_b.labeling.name =
        "Canonical quality generation B";
    generation_b.labeling.labels = {
        {5, "bad generation B", "b"},
        {9, "good generation B", "g"},
    };
    {
        std::ofstream stream(
            annotation_path,
            std::ios::binary | std::ios::trunc);
        const spectiary::SampleLabelingAsdfWriteResult write =
            spectiary::WriteSampleLabelingAsdfDocument(
                stream,
                generation_b);
        Require(
            write.succeeded(),
            write.error.message.empty()
                ? "canonical deactivation generation B should publish"
                : write.error.message);
    }

    Require(
        Submit(
            session,
            ActivateLabelingTaskFromAnnotation(
                annotation_path))
            .action.workflow_changed,
        "canonical deactivation owner should hydrate generation B");
    Require(
        Submit(
            session,
            DeactivateActiveLabelingTask())
            .action.workflow_changed,
        "clean canonical generation B should deactivate");

    const spectiary::SourceCollectionSessionView view =
        session.View();
    Require(
        !view.labeling.has_active_task &&
            view.navigation.current_annotations.size() == 1 &&
            view.navigation.current_annotations[0].name ==
                "Canonical quality generation B" &&
            view.navigation.current_annotations[0]
                    .display_text ==
                "good generation B (9)",
        "deactivation must retain the hydrated attachment generation instead of falling back to generation A");
    Require(
        view.filter.available_sources.size() == 1,
        "the inactive generation B owner should remain available as one filter source");
    const auto generation_b_filter = std::find_if(
        view.filter.available_sources[0].options.begin(),
        view.filter.available_sources[0].options.end(),
        [](const spectiary::SampleFilterValueOption& option) {
            return option.key == "9";
        });
    Require(
        generation_b_filter !=
                view.filter.available_sources[0]
                    .options.end() &&
            generation_b_filter->display_text ==
                "good generation B (9)" &&
            generation_b_filter->sample_count == 2,
        "inactive filtering must continue to evaluate the hydrated generation B attachment");
}

void TestInactiveCanonicalOwnerRepairsAttachmentProjection()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_inactive_canonical_owner.npy");
    const std::filesystem::path annotation_path =
        UniqueTempPath("_inactive_canonical_owner.asdf");
    const std::filesystem::path labeling_cache =
        UniqueTempPath("_inactive_canonical_owner_labeling.json");
    TouchFile(source_path);

    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(source_path, 3, 0);
    const spectiary::SourceCollectionIdentity identity =
        spectiary::BuildSourceCollectionIdentity(*snapshot);

    spectiary::SampleLabelingDocument document;
    document.source.base_identity = identity.id;
    document.source.kind = "test";
    document.source.name = identity.source_name;
    document.source.fingerprint =
        identity.source_fingerprint;
    document.source.sample_count = identity.spectrum_count;
    document.source.roster.identity_kind =
        std::string{
            spectiary::
                kSampleLabelingDocumentSourceIndexRoster};
    document.annotation.values = {5, -1, 9};
    document.labeling.id = "66666666-6666-4666-8666-666666666666";
    document.labeling.name = "Inactive canonical quality";
    document.labeling.canonical_metadata = TestCanonicalMetadata();
    document.labeling.labels = {
        {5, "bad", "b"},
        {9, "good", "g"},
    };
    {
        std::ofstream stream(
            annotation_path,
            std::ios::binary | std::ios::trunc);
        const spectiary::SampleLabelingAsdfWriteResult write =
            spectiary::WriteSampleLabelingAsdfDocument(
                stream,
                document);
        Require(
            write.succeeded(),
            write.error.message.empty()
                ? "inactive canonical owner fixture should write"
                : write.error.message);
    }

    spectiary::SampleLabelingTask cached =
        spectiary::CreateSampleLabelingTask(
            "66666666-6666-4666-8666-666666666666",
            "structural cache placeholder",
            3);
    cached.canonical_metadata =
        document.labeling.canonical_metadata;
    cached.values.Complete()[1] = 9;
    cached.persistence.pending_sample_indices.insert(1);
    cached.persistence.output_path = annotation_path;
    cached.persistence.output_format =
        spectiary::SampleLabelingOutputArtifactFormat::
            CanonicalAsdf;
    cached.persistence.save_state.kind =
        spectiary::SampleLabelSaveStateKind::Pending;

    spectiary::SampleLabelingSourceState source_state;
    source_state.sample_count = identity.spectrum_count;
    source_state.source_name = identity.source_name;
    source_state.source_fingerprint =
        identity.source_fingerprint;
    source_state.context_fingerprint =
        identity.context_fingerprint;
    source_state.tasks.push_back(std::move(cached));
    spectiary::SampleLabelingStateCache cache;
    cache.sources.emplace(identity.id, std::move(source_state));
    Require(
        spectiary::SaveSampleLabelingStateCache(spectiary::RuntimePaths{},
            labeling_cache,
            cache),
        "inactive canonical owner cache fixture should save");

    std::vector<std::size_t> loaded_indices;
    PreparedSession session(
        [&loaded_indices, source_path](
            const std::filesystem::path& path,
            std::size_t spectrum_index) {
            Require(
                path == source_path,
                "inactive canonical owner session should reload its source");
            loaded_indices.push_back(spectrum_index);
            return MakeSnapshot(
                source_path,
                3,
                spectrum_index);
        },
        {},
        UniqueTempPath(
            "_inactive_canonical_owner_navigation.json"),
        labeling_cache,
        UniqueTempPath(
            "_inactive_canonical_owner_workflow.json"));
    const spectiary::SourceCollectionSessionResult opened =
        session.Open(source_path, 0, {});
    Require(
        opened.loaded &&
            !session.View().labeling.has_active_task,
        "inactive canonical owner should restore only as structural task state");

    const spectiary::SourceCollectionSessionView view =
        session.View();
    Require(
        view.navigation.current_annotations.size() == 1 &&
            view.navigation.current_annotations[0].path ==
                annotation_path &&
            view.navigation.current_annotations[0].display_text ==
                "bad (5)",
        "an inactive canonical owner must repair its attachment and project ASDF base values instead of structural placeholders");
    Require(
        view.filter.available_sources.size() == 1,
        "the repaired inactive canonical attachment must become an available labeling filter source");
    const auto good = std::find_if(
        view.filter.available_sources[0].options.begin(),
        view.filter.available_sources[0].options.end(),
        [](const spectiary::SampleFilterValueOption& option) {
            return option.key == "9";
        });
    Require(
        good != view.filter.available_sources[0].options.end() &&
            good->sample_count == 1,
        "the repaired canonical filter projection must ignore persisted pending values");
}

void TestSameIdentitySourceActivationReplacesAutoAdvanceFeedback()
{
    const std::filesystem::path source_a =
        UniqueTempPath("_same_identity_activation_a.npy");
    const std::filesystem::path source_b =
        UniqueTempPath("_same_identity_activation_b.npy");
    spectiary::SourceCollectionSession session({}, {}, {}, {});
    const spectiary::SourceCollectionIdentity shared_identity = {
        "same-identity-activation",
        "shared-source",
        "shared-source-fingerprint",
        "shared-context-fingerprint",
        2,
    };

    const auto open_new_source =
        [&](const std::filesystem::path& path,
            std::size_t spectrum_index) {
            const spectiary::SpectrumSnapshotHandle snapshot =
                MakeSnapshot(path, 2, spectrum_index);
            spectiary::SourceCollectionContext context;
            context.identity = shared_identity;
            context.manifest.sample_names = {"alpha", "beta"};
            spectiary::PreparedSampleWorkflowState prepared =
                PrepareWorkflow(
                    snapshot,
                    context,
                    spectrum_index,
                    {},
                    {});
            return session.OpenPreparedSource(
                path,
                spectrum_index,
                snapshot,
                std::move(context),
                std::move(prepared));
        };

    Require(
        open_new_source(source_a, 0).loaded,
        "same-identity source A should load at row 0");
    Require(
        Submit(
            session,
            MoveSampleNavigation(
                spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 1,
        "source A should request row 1 before it is cached");
    const spectiary::SpectrumSnapshotHandle source_a_row_one =
        MakeSnapshot(source_a, 2, 1);
    Require(
        session.OpenPreparedSource(
                   source_a,
                   1,
                   source_a_row_one,
                   spectiary::PreparedSourceCollectionReuse{
                       shared_identity})
            .loaded,
        "source A row 1 should become its cached presentation");

    Require(
        open_new_source(source_b, 0).loaded,
        "same-identity source B should load independently at row 0");
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(
            session,
            UpsertActiveLabel(
                spectiary::SampleLabelDefinition{1, "accepted", 'a'}))
            .changed,
        "source B should add its accepted label");
    (void)Submit(session, SetActiveLabelingAutoAdvance(true));
    Require(
        Submit(session, AssignActiveLabelToCurrentSample(1))
                .follow_up_spectrum_index == 1,
        "source B should auto-advance from row 0 to row 1");
    const spectiary::SpectrumSnapshotHandle source_b_row_one =
        MakeSnapshot(source_b, 2, 1);
    Require(
        session.OpenPreparedSource(
                   source_b,
                   1,
                   source_b_row_one,
                   spectiary::PreparedSourceCollectionReuse{
                       shared_identity})
            .loaded,
        "source B auto-advance target should commit");
    Require(
        session.View().sample_transition &&
            session.View().sample_transition->reason ==
                spectiary::SourceCollectionSampleTransitionReason::
                    LabelingAutoAdvance &&
            session.View().sample_transition->accepted_label_value == 1,
        "source B should expose its committed auto-advance feedback before activation");

    const spectiary::SourceCollectionSessionResult activated =
        Submit(session, SwitchSourceCollection(0));
    const auto& transition = session.View().sample_transition;
    Require(
        activated.action.snapshot_changed &&
            session.CurrentSampleSnapshot() == source_a_row_one &&
            transition &&
            transition->reason ==
                spectiary::SourceCollectionSampleTransitionReason::
                    SourceActivation &&
            !transition->from_sample_index &&
            transition->current_sample_index == 1 &&
            !transition->accepted_label_value,
        "activating a different path with the same identity and row must replace source B auto-advance feedback");
}

void TestNavigationViewSeparatesSampleNameFromDisplayName()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 3);

    spectiary::SourceCollectionSessionResult result =
        Submit(session, OpenSourceCollection(source_path, 0));
    spectiary::SourceCollectionNavigationView navigation = session.View().navigation;
    Require(navigation.current_sample_display_name == "sample-1", "view should expose the current display name");
    Require(navigation.current_sample_name.empty(), "unnamed samples should not expose a searchable sample name");

    result = Submit(
        session,
        SetSampleNameQuery(navigation.current_sample_display_name));
    navigation = session.View().navigation;
    Require(!navigation.exact_sample_name_match, "fallback display names should not be exact sample-name matches");
    Require(navigation.sample_name_matches.empty(), "fallback display names should not be sample-name search matches");
}

void TestNavigationViewExposesSourceProvidedSampleName()
{
    const std::filesystem::path source_path = UniqueTempPath("_folder");
    std::filesystem::create_directories(source_path);
    TouchFile(source_path / "alpha.csv");
    TouchFile(source_path / "beta.csv");
    std::vector<std::size_t> loaded_indices;
    PreparedSession session = MakeSession(loaded_indices, source_path, 2);

    spectiary::SourceCollectionSessionResult result =
        Submit(session, OpenSourceCollection(source_path, 0));
    spectiary::SourceCollectionNavigationView navigation = session.View().navigation;
    Require(navigation.has_sample_names, "folder source should expose source-provided sample names");
    Require(navigation.current_sample_name == "alpha.csv", "view should expose the current source-provided sample name");
    Require(navigation.current_sample_display_name == "sample-1", "display text should remain separate from sample name");

    result = Submit(
        session,
        SetSampleNameQuery(navigation.current_sample_name));
    navigation = session.View().navigation;
    Require(
        navigation.exact_sample_name_match && *navigation.exact_sample_name_match == 0,
        "source-provided sample names should remain searchable");
}

void TestRemovingActiveSourceActivatesNextSourceWorkflow()
{
    const std::filesystem::path first_source_path = UniqueTempPath("_first.npy");
    const std::filesystem::path second_source_path = UniqueTempPath("_second.npy");
    std::vector<LoadedSourceSnapshot> loaded_snapshots;
    PreparedSession session = MakeMultiSourceSession(
        loaded_snapshots,
        first_source_path,
        3,
        second_source_path,
        2);

    spectiary::SourceCollectionSessionResult result =
        Submit(session, OpenSourceCollection(first_source_path, 1));
    Require(session.View().current_source_index && *session.View().current_source_index == 0, "first source should be active");
    Require(session.View().snapshot->source.path == first_source_path, "first source snapshot should be visible");
    Require(session.View().snapshot->collection.current_index == 1, "first source should open at requested row");
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(
            session,
            UpsertActiveLabel(
                spectiary::SampleLabelDefinition{10, "first-label", 'f'}))
            .changed,
        "first source should accept its own active label");

    result = Submit(session, OpenSourceCollection(second_source_path, 0));
    Require(session.View().current_source_index && *session.View().current_source_index == 1, "second source should be active");
    Require(session.View().snapshot->source.path == second_source_path, "second source snapshot should be visible");
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(
            session,
            UpsertActiveLabel(
                spectiary::SampleLabelDefinition{20, "second-label", 's'}))
            .changed,
        "second source should accept its own active label");

    result = Submit(session, SwitchSourceCollection(0));
    Require(result.action.snapshot_changed, "activating first source should swap to its cached snapshot");
    Require(
        session.View().current_source_index && *session.View().current_source_index == 0,
        "first source should be active again");
    Require(session.View().snapshot->source.path == first_source_path, "reactivated snapshot should be the first source");
    Require(session.View().snapshot->collection.current_index == 1, "reactivated first source should keep its cached row");
    Require(session.View().labeling.has_active_task, "first source labeling task should be restored");
    Require(session.View().labeling.label_set.labels.size() == 1, "first source should expose its own label set");
    Require(session.View().labeling.label_set.labels[0].code == 10, "first source label set should not come from second source");

    const std::size_t loaded_count_before_remove = loaded_snapshots.size();
    result = Submit(session, RemoveSourceCollection(0));
    Require(result.action.snapshot_changed, "removing active first source should activate the next source snapshot");
    Require(result.action.workflow_changed, "removing active first source should resync the workflow");
    Require(result.action.navigation_inputs_changed, "removing active first source should refresh navigation inputs");
    Require(session.View().sources.size() == 1, "removing first source should leave one source");
    Require(session.View().sources[0].path == second_source_path, "remaining source should be the second source");
    Require(
        session.View().current_source_index && *session.View().current_source_index == 0,
        "second source should become index 0");
    Require(session.View().snapshot->source.path == second_source_path, "second source snapshot should be visible after removal");
    Require(session.View().snapshot->collection.current_index == 0, "second source cached row should be preserved");
    Require(session.View().labeling.has_active_task, "second source labeling task should be restored after removal");
    Require(session.View().labeling.label_set.labels.size() == 1, "second source should expose its own label set");
    Require(session.View().labeling.label_set.labels[0].code == 20, "second source label set should survive first removal");
    Require(
        loaded_snapshots.size() == loaded_count_before_remove,
        "removing active source should activate the next cached source without reloading");
}

void TestSourceSessionRestoresSourcesAndActiveIndex()
{
    const std::filesystem::path source_session_cache = UniqueTempPath("_sources.json");
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    const std::filesystem::path first_source_path = UniqueTempPath("_first.npy");
    const std::filesystem::path second_source_path = UniqueTempPath("_second.npy");
    const std::filesystem::path first_annotation_path = UniqueTempPath("_first_annotation.npy");
    const std::filesystem::path second_annotation_path = UniqueTempPath("_second_annotation.npy");
    TouchFile(first_source_path);
    TouchFile(second_source_path);
    SaveLabelResultFixture(first_annotation_path, "first-annotation", "First annotation", {1, 2, 3}, {}, false);
    SaveLabelResultFixture(second_annotation_path, "second-annotation", "Second annotation", {4, 5}, {}, false);

    {
        std::vector<LoadedSourceSnapshot> loaded_snapshots;
        PreparedSession session = MakePersistentMultiSourceSession(
            loaded_snapshots,
            source_session_cache,
            navigation_cache,
            labeling_cache,
            first_source_path,
            3,
            second_source_path,
            2);

        (void)Submit(session, OpenSourceCollection(first_source_path, 1));
        (void)Submit(session, AddReadOnlyAnnotation(first_annotation_path));
        (void)Submit(session, OpenSourceCollection(second_source_path, 0));
        (void)Submit(session, AddReadOnlyAnnotation(second_annotation_path));
        (void)Submit(session, SwitchSourceCollection(0));
        (void)Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::Next()));
        Require(session.View().snapshot->source.path == first_source_path, "first source should be active");
        Require(session.View().snapshot->collection.current_index == 2, "first source should reach row 2");
        Require(session.FlushStateCaches(), "session state caches should flush");
    }

    std::vector<LoadedSourceSnapshot> restored_loads;
    PreparedSession restored = MakePersistentMultiSourceSession(
        restored_loads,
        source_session_cache,
        navigation_cache,
        labeling_cache,
        first_source_path,
        3,
        second_source_path,
        2);

    const spectiary::SourceCollectionSessionView view = restored.View();
    Require(view.sources.size() == 2, "restored session should reload both source entries");
    Require(view.sources[0].path == first_source_path, "first restored source should keep its path");
    Require(view.sources[1].path == second_source_path, "second restored source should keep its path");
    Require(view.current_source_index && *view.current_source_index == 0, "restored active source should be first");
    Require(view.snapshot->source.path == first_source_path, "restored snapshot should be the active first source");
    Require(view.snapshot->collection.current_index == 2, "restored first source should keep its last sample index");
    Require(
        view.sample_transition &&
            view.sample_transition->reason ==
                spectiary::SourceCollectionSampleTransitionReason::Restore &&
            view.sample_transition->current_sample_index == 2 &&
            !view.sample_transition->accepted_label_value,
        "source-session restore should expose a restore transition without previous-label feedback");
    Require(view.navigation.current_annotations.size() == 1, "restored first source should restore annotations");
    Require(
        view.navigation.current_annotations[0].path == first_annotation_path,
        "restored first source annotation path should come from source session state");
    Require(restored_loads.size() == 2, "restoring two cached sources should load each source once");
    Require(restored_loads[0].path == first_source_path && restored_loads[0].index == 2, "first source should restore row 2");
    Require(restored_loads[1].path == second_source_path && restored_loads[1].index == 0, "second source should restore row 0");

    std::filesystem::remove(source_session_cache);
    Require(restored.FlushStateCaches(), "flush after restore should succeed without a dirty source session");
    Require(
        !std::filesystem::exists(source_session_cache),
        "restore should not mark the source session cache dirty immediately");

    (void)Submit(restored, SwitchSourceCollection(1));
    Require(
        restored.View().navigation.current_annotations.size() == 1,
        "restored second source should restore its own annotations");
    Require(
        restored.View().navigation.current_annotations[0].path == second_annotation_path,
        "restored second source annotation path should come from source session state");
}

void TestSourceSessionStateCacheRoundTrip()
{
    const std::filesystem::path source_session_cache = UniqueTempPath("_adapter_sources.json");
    const std::filesystem::path first_source_path = UniqueTempPath("_adapter_first.npy");
    const std::filesystem::path second_source_path = UniqueTempPath("_adapter_second.npy");
    const std::filesystem::path annotation_path = UniqueTempPath("_adapter_annotation.npy");

    spectiary::SourceCollectionSessionStateCache cache;
    spectiary::SourceCollectionSavedSource first_source{first_source_path, 2};
    first_source.annotation_paths.push_back(annotation_path);
    cache.sources = {first_source, spectiary::SourceCollectionSavedSource{second_source_path, 0}};
    cache.active_source_index = 1;
    Require(
        spectiary::SaveSourceCollectionSessionStateCache(spectiary::RuntimePaths{}, source_session_cache, cache),
        "source session cache should save");

    const spectiary::SourceCollectionSessionStateCache loaded =
        spectiary::LoadSourceCollectionSessionStateCache(spectiary::RuntimePaths{}, source_session_cache)
            .cache;
    Require(loaded.sources.size() == 2, "source session cache should restore all sources");
    Require(loaded.sources[0].path == first_source_path, "first source path should round-trip");
    Require(loaded.sources[0].last_spectrum_index == 2, "first source index should round-trip");
    Require(loaded.sources[0].annotation_paths.size() == 1, "first source annotation paths should round-trip");
    Require(
        loaded.sources[0].annotation_paths[0] == annotation_path,
        "first source annotation path should round-trip");
    Require(loaded.sources[1].path == second_source_path, "second source path should round-trip");
    Require(
        loaded.active_source_index && *loaded.active_source_index == 1,
        "active source index should round-trip");
}

void TestSourceSessionStateCacheIgnoresCorruptJson()
{
    const std::filesystem::path source_session_cache = UniqueTempPath("_adapter_corrupt_sources.json");
    WriteTextFile(source_session_cache, "{ invalid json");

    const spectiary::SourceCollectionSessionStateCacheLoadResult loaded =
        spectiary::LoadSourceCollectionSessionStateCache(spectiary::RuntimePaths{},
            source_session_cache);
    Require(loaded.cache.sources.empty(), "corrupt source session cache should be ignored");
    Require(
        !loaded.cache.active_source_index,
        "corrupt source session cache should not restore an active source");
    Require(
        !loaded.warning.empty(),
        "corrupt source session cache should retain its non-blocking warning");
}

void TestMissingPersistenceCachesAreHealthyDefaults()
{
    const std::filesystem::path source_session_cache =
        UniqueTempPath("_missing_source_session.json");
    const std::filesystem::path navigation_cache =
        UniqueTempPath("_missing_navigation.json");
    const std::filesystem::path labeling_cache =
        UniqueTempPath("_missing_labeling.json");
    const std::filesystem::path workflow_cache =
        UniqueTempPath("_missing_workflow.json");

    Require(
        spectiary::LoadSourceCollectionSessionStateCache(spectiary::RuntimePaths{},
            source_session_cache)
            .warning.empty(),
        "missing source-session cache should be a healthy default");
    Require(
        spectiary::LoadSampleNavigationStateCache(
            navigation_cache)
            .warning.empty(),
        "missing navigation cache should be a healthy default");
    Require(
        spectiary::LoadSampleLabelingStateCache(spectiary::RuntimePaths{}, labeling_cache)
            .warning.empty(),
        "missing labeling cache should be a healthy default");
    Require(
        spectiary::LoadSampleWorkflowStateCache(spectiary::RuntimePaths{},
            workflow_cache)
            .warning.empty(),
        "missing workflow cache should be a healthy default");
}

void TestSourceSessionStateCacheIgnoresUnsupportedSchema()
{
    const std::filesystem::path source_session_cache = UniqueTempPath("_adapter_schema_sources.json");
    WriteTextFile(
        source_session_cache,
        "{\n"
        "  \"format_kind\": \"spectiary.source_collection_session.cache\",\n"
        "  \"schema_version\": 999,\n"
        "  \"active_source_index\": 0,\n"
        "  \"sources\": []\n"
        "}\n");

    const spectiary::SourceCollectionSessionStateCacheLoadResult loaded =
        spectiary::LoadSourceCollectionSessionStateCache(spectiary::RuntimePaths{},
            source_session_cache);
    Require(loaded.cache.sources.empty(), "unsupported source session cache schema should be ignored");
    Require(
        !loaded.cache.active_source_index,
        "unsupported source session cache schema should not restore an active source");
    Require(
        !loaded.warning.empty(),
        "unsupported source session cache should retain its non-blocking warning");
}

void TestSessionAggregatesCacheLoadWarningsWithoutBlockingSourceOpen()
{
    const std::filesystem::path source_session_cache =
        UniqueTempPath("_health_source_session.json");
    const std::filesystem::path navigation_cache =
        UniqueTempPath("_health_navigation.json");
    const std::filesystem::path labeling_cache =
        UniqueTempPath("_health_labeling.json");
    const std::filesystem::path workflow_cache =
        UniqueTempPath("_health_workflow.json");
    const std::filesystem::path source_path =
        UniqueTempPath("_health_source.npy");
    const std::filesystem::path second_source_path =
        UniqueTempPath("_health_second_source.npy");
    TouchFile(source_path);
    TouchFile(second_source_path);
    WriteTextFile(source_session_cache, "{ invalid json");
    WriteTextFile(
        navigation_cache,
        "{\n"
        "  \"format_kind\": \"spectiary.sample_navigation_state.cache\",\n"
        "  \"schema_version\": 999,\n"
        "  \"sources\": []\n"
        "}\n");
    const std::string corrupt_labeling_cache =
        "{ invalid json";
    WriteTextFile(
        labeling_cache,
        corrupt_labeling_cache);
    WriteTextFile(
        workflow_cache,
        "{\n"
        "  \"format_kind\": \"spectiary.sample_workflow_state.cache\",\n"
        "  \"schema_version\": 999,\n"
        "  \"sources\": []\n"
        "}\n");

    PreparedSession session(
        [source_path, second_source_path](
            const std::filesystem::path& path,
            std::size_t index) {
            Require(
                path == source_path ||
                    path == second_source_path,
                "health fixture should open one of its sources");
            return MakeSnapshot(path, 3, index);
        },
        source_session_cache,
        navigation_cache,
        labeling_cache,
        workflow_cache);
    const spectiary::SourceCollectionSessionResult opened =
        session.Open(source_path);
    Require(opened.loaded, "cache warnings must not block source opening");

    const spectiary::LocalUserStateHealthView& health =
        session.View().persistence;
    Require(
        health.kind ==
            spectiary::LocalUserStateHealthKind::Warning,
        "corrupt and unsupported caches should produce overall warning health");
    Require(
        health.messages.size() == 4,
        std::string{"all four independent cache warnings should be retained; got "} +
            std::to_string(health.messages.size()));
    Require(
        HasPersistenceMessage(
            health,
            spectiary::LocalUserStateArea::
                SourceSession,
            spectiary::
                LocalUserStateHealthMessageKind::
                    LoadWarning),
        "source-session warning should reach the session view");
    Require(
        HasPersistenceMessage(
            health,
            spectiary::LocalUserStateArea::
                SampleNavigation,
            spectiary::
                LocalUserStateHealthMessageKind::
                    LoadWarning),
        "navigation unsupported-schema warning should reach the session view");
    Require(
        HasPersistenceMessage(
            health,
            spectiary::LocalUserStateArea::
                SampleLabeling,
            spectiary::
                LocalUserStateHealthMessageKind::
                    LoadWarning),
        "labeling warning should remain part of overall health");
    Require(
        HasPersistenceMessage(
            health,
            spectiary::LocalUserStateArea::
                SampleWorkflow,
            spectiary::
                LocalUserStateHealthMessageKind::
                    LoadWarning),
        "workflow unsupported-schema warning should reach the session view");

    (void)Submit(
        session,
        StartOrResumeTemporaryLabelingTask());
    (void)session.SubmitAndService(
        MoveSampleNavigation(
            spectiary::SampleNavigationRequest::LocateRow(1)));
    (void)Submit(
        session,
        spectiary::SourceCollectionSessionIntent::ApplySampleFiltering(
            spectiary::SampleFilteringIntent::Clear()));
    const spectiary::SourceCollectionStateFlushResult flush =
        session.FlushStateCachesWithStatus();
    Require(
        flush.source_session_saved &&
            flush.navigation_saved &&
            !flush.labeling_saved &&
            flush.workflow_saved,
        "labeling should fail closed while the other independent cache owners repair their files");
    Require(
        !HasPersistenceMessage(
            session.View().persistence,
            spectiary::LocalUserStateArea::SourceSession,
            spectiary::LocalUserStateHealthMessageKind::LoadWarning),
        "a successful source-session flush should clear only its load warning");
    Require(
        ReadTextFile(labeling_cache) ==
            corrupt_labeling_cache,
        "fail-closed labeling persistence must preserve the corrupt source bytes");
    Require(
        HasPersistenceMessage(
            session.View().persistence,
            spectiary::LocalUserStateArea::
                SampleLabeling,
            spectiary::
                LocalUserStateHealthMessageKind::
                    SaveRetrying),
        "fail-closed labeling persistence should report a retrying save");
    Require(
        session.Open(second_source_path).loaded,
        "the repaired-cache fixture should open another source");
    Require(
        ReadTextFile(labeling_cache) ==
            corrupt_labeling_cache,
        "opening another source must not overwrite the untrusted labeling cache");
}

void TestRejectedAnnotationImportRefreshesDiagnosticProjection()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path annotation_path =
        UniqueTempPath("_wrong_roster.npy");
    SaveLabelResultFixture(
        annotation_path,
        "rejected-annotation",
        "Rejected annotation",
        {1, 2},
        spectiary::SampleLabelSet{},
        false);

    std::vector<std::size_t> loaded_indices;
    PreparedSession session =
        MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    const spectiary::SourceCollectionSessionView before =
        session.View();
    Require(
        before.navigation.current_annotations.empty() &&
            before.navigation.annotation_diagnostics.empty(),
        "rejected-import fixture should start without annotations or diagnostics");

    const spectiary::SourceCollectionSessionResult result =
        Submit(session, AddReadOnlyAnnotation(annotation_path));
    Require(
        !result.loaded,
        "an annotation whose value count does not match the active source must be rejected");

    const spectiary::SourceCollectionSessionView after =
        session.View();
    Require(
        after.navigation.current_annotations.empty(),
        "a rejected annotation import must not enter the annotation roster");
    Require(
        std::any_of(
            after.navigation.annotation_diagnostics.begin(),
            after.navigation.annotation_diagnostics.end(),
            [&annotation_path](
                const spectiary::SourceCollectionManifestDiagnostic&
                    diagnostic) {
                return diagnostic.kind ==
                           spectiary::
                               SourceCollectionManifestDiagnosticKind::
                                   AnnotationIgnored &&
                    diagnostic.path == annotation_path &&
                    !diagnostic.detail.empty();
            }),
        "a rejected annotation import must invalidate and refresh the structured diagnostic projection");
}

void TestRejectedAnnotationSwitchKeepsCurrentEditingTask()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_lease_switch_source.npy");
    std::filesystem::path first_annotation =
        UniqueTempPath("_lease_switch_first.npy");
    std::filesystem::path second_annotation =
        UniqueTempPath("_lease_switch_second.npy");
    const std::filesystem::path labeling_cache =
        UniqueTempPath("_lease_switch_labeling.json");
    TouchFile(source_path);
    spectiary::SampleLabelSet first_labels;
    first_labels.labels.push_back(
        spectiary::SampleLabelDefinition{
            5,
            "first",
            'f'});
    spectiary::SampleLabelSet second_labels;
    second_labels.labels.push_back(
        spectiary::SampleLabelDefinition{
            7,
            "second",
            's'});
    SaveLabelResultFixture(
        first_annotation,
        "first-task",
        "First task",
        {5, -1, -1},
        first_labels,
        true);
    SaveLabelResultFixture(
        second_annotation,
        "second-task",
        "Second task",
        {7, -1, -1},
        second_labels,
        true);

    const auto make_session = [&]() {
        return PreparedSession(
            [source_path](
                const std::filesystem::path& path,
                std::size_t spectrum_index) {
                Require(
                    path == source_path,
                    "lease-switch fixture should load its source");
                return MakeSnapshot(
                    source_path,
                    3,
                    spectrum_index);
            },
            {},
            UniqueTempPath("_lease_switch_navigation.json"),
            labeling_cache,
            UniqueTempPath("_lease_switch_workflow.json"));
    };

    std::string first_task_id;
    std::string second_task_id;
    {
        PreparedSession seed = make_session();
        Require(
            seed.Open(
                    source_path,
                    0,
                    {first_annotation, second_annotation})
                .loaded,
            "lease-switch seed should open the source");
        (void)Submit(
            seed,
            ActivateLabelingTaskFromAnnotation(
                first_annotation));
        Require(
            seed.View().labeling.has_active_task &&
                spectiary::IsCanonicalUuidV4(
                    seed.View().labeling.task_id),
            "lease-switch seed should register the first task");
        first_task_id = seed.View().labeling.task_id;
        first_annotation.replace_extension(".asdf");
        (void)Submit(seed, SetActiveLabelingOutputPath(first_annotation));
        Require(seed.View().labeling.output_path == first_annotation,
            "lease-switch fixture should establish a canonical owner");
        (void)Submit(
            seed,
            DeactivateActiveLabelingTask());
        Require(
            !seed.View().labeling.has_active_task,
            "lease-switch seed should deactivate the first task");
        (void)Submit(
            seed,
            ActivateLabelingTaskFromAnnotation(
                second_annotation));
        Require(
            seed.View().labeling.has_active_task &&
                spectiary::IsCanonicalUuidV4(
                    seed.View().labeling.task_id) &&
                seed.View().labeling.task_id != first_task_id,
            "lease-switch seed should register the second task");
        second_task_id = seed.View().labeling.task_id;
        second_annotation.replace_extension(".asdf");
        (void)Submit(seed, SetActiveLabelingOutputPath(second_annotation));
        Require(seed.View().labeling.output_path == second_annotation,
            "lease-switch fixture should establish a canonical owner");
        (void)Submit(
            seed,
            DeactivateActiveLabelingTask());
        Require(
            !seed.View().labeling.has_active_task,
            "lease-switch seed should deactivate the second task");
        Require(
            seed.FlushStateCaches(),
            "lease-switch seed should flush both task records");
    }

    PreparedSession first = make_session();
    PreparedSession second = make_session();
    Require(
        first.Open(
                source_path,
                0,
                {first_annotation, second_annotation})
            .loaded &&
            second.Open(
                source_path,
                0,
                {first_annotation, second_annotation})
                .loaded,
        "both lease-switch instances should load the source");
    (void)Submit(
        first,
        ActivateLabelingTaskFromAnnotation(
            first_annotation));
    Require(
        first.View().labeling.has_active_task &&
            first.View().labeling.task_id ==
                first_task_id,
        "first instance should activate the first task");
    (void)Submit(
        second,
        ActivateLabelingTaskFromAnnotation(
            second_annotation));
    Require(
        second.View().labeling.has_active_task &&
            second.View().labeling.task_id ==
                second_task_id,
        "second instance should activate the second task");

    const spectiary::SourceCollectionSessionResult rejected =
        Submit(
            first,
            ActivateLabelingTaskFromAnnotation(
                second_annotation));
    Require(
        !rejected.changed &&
            rejected.labeling_issue ==
                spectiary::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable &&
            rejected.message.empty(),
        "occupied annotation activation should report only the stable target lease issue");
    Require(
        first.View().labeling.has_active_task &&
            first.View().labeling.task_id ==
                first_task_id,
        "rejected target activation must keep the current editing selection");
}

void TestDirectPreparedWorkflowAdoptsCacheHealthAndNavigationBase()
{
    const std::filesystem::path warning_navigation_cache =
        UniqueTempPath("_direct_health_navigation.json");
    const std::filesystem::path warning_labeling_cache =
        UniqueTempPath("_direct_health_labeling.json");
    const std::filesystem::path warning_workflow_cache =
        UniqueTempPath("_direct_health_workflow.json");
    const std::filesystem::path warning_source =
        UniqueTempPath("_direct_health_source.npy");
    TouchFile(warning_source);
    WriteTextFile(
        warning_navigation_cache,
        "{\n"
        "  \"format_kind\": \"spectiary.sample_navigation_state.cache\",\n"
        "  \"schema_version\": 999,\n"
        "  \"sources\": []\n"
        "}\n");
    WriteTextFile(warning_labeling_cache, "{ invalid json");
    WriteTextFile(
        warning_workflow_cache,
        "{\n"
        "  \"format_kind\": \"spectiary.sample_workflow_state.cache\",\n"
        "  \"schema_version\": 999,\n"
        "  \"sources\": []\n"
        "}\n");

    const spectiary::SpectrumSnapshotHandle warning_snapshot =
        MakeSnapshot(warning_source, 3, 0);
    spectiary::SourceCollectionContext warning_context;
    warning_context.identity =
        spectiary::BuildSourceCollectionIdentity(*warning_snapshot);
    warning_context.manifest.sample_names = {"a", "b", "c"};
    spectiary::PreparedSampleWorkflowState warning_prepared =
        spectiary::PrepareSampleWorkflowState(
            *warning_snapshot,
            warning_context,
            0,
            {
                warning_labeling_cache,
                warning_workflow_cache,
                warning_navigation_cache,
            });
    Require(
        warning_prepared.preparation_cache != nullptr,
        "the direct preparation helper should retain its loaded cache bundle");

    spectiary::SourceCollectionSession warning_session(
        {},
        warning_navigation_cache,
        warning_labeling_cache,
        warning_workflow_cache);
    Require(
        warning_session.OpenPreparedSource(
                           warning_source,
                           0,
                           warning_snapshot,
                           std::move(warning_context),
                           std::move(warning_prepared))
            .loaded,
        "the direct prepared source should open despite cache warnings");
    const spectiary::LocalUserStateHealthView warning_health =
        warning_session.View().persistence;
    Require(
        warning_health.kind ==
            spectiary::LocalUserStateHealthKind::Warning,
        "direct preparation cache warnings should reach Session health");
    Require(
        HasPersistenceMessage(
            warning_health,
            spectiary::LocalUserStateArea::
                SampleNavigation) &&
            HasPersistenceMessage(
                warning_health,
                spectiary::LocalUserStateArea::
                    SampleLabeling) &&
            HasPersistenceMessage(
                warning_health,
                spectiary::LocalUserStateArea::
                    SampleWorkflow),
        "direct preparation should adopt every owner warning");

    const std::filesystem::path navigation_cache =
        UniqueTempPath("_direct_base_navigation.json");
    const std::filesystem::path labeling_cache =
        UniqueTempPath("_direct_base_labeling.json");
    const std::filesystem::path workflow_cache =
        UniqueTempPath("_direct_base_workflow.json");
    const std::filesystem::path source =
        UniqueTempPath("_direct_base_source.npy");
    TouchFile(source);
    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(source, 3, 0);
    spectiary::SourceCollectionContext context;
    context.identity =
        spectiary::BuildSourceCollectionIdentity(*snapshot);
    context.manifest.sample_names = {"a", "b", "c"};
    const spectiary::SourceCollectionIdentity identity = context.identity;

    spectiary::SampleNavigationStateCache navigation_fixture;
    navigation_fixture.last_indices_by_source_identity.emplace(
        "unrelated-source",
        2);
    Require(
        spectiary::SaveSampleNavigationStateCache(
            navigation_cache,
            navigation_fixture),
        "the direct navigation base fixture should save");
    spectiary::PreparedSampleWorkflowState prepared =
        spectiary::PrepareSampleWorkflowState(
            *snapshot,
            context,
            0,
            {
                labeling_cache,
                workflow_cache,
                navigation_cache,
            });

    spectiary::SourceCollectionSession session(
        {},
        navigation_cache,
        labeling_cache,
        workflow_cache);
    Require(
        session.OpenPreparedSource(
                   source,
                   0,
                   snapshot,
                   std::move(context),
                   std::move(prepared))
            .loaded,
        "the direct navigation-base source should open");
    const spectiary::SourceCollectionSessionResult pending =
        Submit(
            session,
            MoveSampleNavigation(
                spectiary::SampleNavigationRequest::LocateRow(1)));
    Require(
        pending.follow_up_spectrum_index == 1,
        "the direct navigation-base fixture should request row 1");
    Require(
        session.OpenPreparedSource(
                   source,
                   1,
                   MakeSnapshot(source, 3, 1),
                   spectiary::PreparedSourceCollectionReuse{identity})
            .loaded,
        "the direct navigation-base row should commit without another cache load");
    Require(
        session.FlushStateCachesWithStatus().all_saved(),
        "the direct navigation-base fixture should flush");

    const spectiary::SampleNavigationStateCache restored =
        spectiary::LoadSampleNavigationStateCache(navigation_cache).cache;
    Require(
        restored.last_indices_by_source_identity.at("unrelated-source") == 2,
        "direct preparation should preserve an unrelated navigation entry");
    Require(
        restored.last_indices_by_source_identity.at(identity.id) == 1,
        "direct preparation should merge the live navigation position");
}

void TestStalePreparedCacheWarningsDoNotReappearAfterRepair()
{
    const std::filesystem::path navigation_cache =
        UniqueTempPath("_stale_warning_navigation.json");
    const std::filesystem::path labeling_cache =
        UniqueTempPath("_stale_warning_labeling.json");
    const std::filesystem::path workflow_cache =
        UniqueTempPath("_stale_warning_workflow.json");
    const std::filesystem::path source_a =
        UniqueTempPath("_stale_warning_a.npy");
    const std::filesystem::path source_b =
        UniqueTempPath("_stale_warning_b.npy");
    const std::filesystem::path source_c =
        UniqueTempPath("_stale_warning_c.npy");
    TouchFile(source_a);
    TouchFile(source_b);
    TouchFile(source_c);

    const spectiary::SpectrumSnapshotHandle snapshot_a =
        MakeSnapshot(source_a, 3, 0);
    const spectiary::SpectrumSnapshotHandle snapshot_b =
        MakeSnapshot(source_b, 3, 0);
    const spectiary::SpectrumSnapshotHandle snapshot_c =
        MakeSnapshot(source_c, 3, 0);
    spectiary::SourceCollectionContext context_a;
    context_a.identity = {
        "stale-warning-a",
        "a",
        "a-source",
        "a-context",
        3,
    };
    context_a.manifest.sample_names = {"a0", "a1", "a2"};
    spectiary::SourceCollectionContext context_b;
    context_b.identity = {
        "stale-warning-b",
        "b",
        "b-source",
        "b-context",
        3,
    };
    context_b.manifest.sample_names = {"b0", "b1", "b2"};
    const spectiary::SourceCollectionIdentity identity_b =
        context_b.identity;
    spectiary::SourceCollectionContext context_c;
    context_c.identity = {
        "stale-warning-c",
        "c",
        "c-source",
        "c-context",
        3,
    };
    context_c.manifest.sample_names = {"c0", "c1", "c2"};

    auto shared_cache =
        std::make_shared<spectiary::SampleWorkflowPreparationCacheBundle>();
    shared_cache->navigation.warning =
        "stale navigation cache warning";
    shared_cache->labeling.warning =
        "stale labeling cache warning";
    shared_cache->workflow_warning =
        "stale workflow cache warning";
    auto distinct_stale_cache =
        std::make_shared<spectiary::SampleWorkflowPreparationCacheBundle>(
            *shared_cache);

    spectiary::PreparedSampleWorkflowState prepared_a =
        spectiary::PrepareSampleWorkflowStateFromCache(
            *snapshot_a,
            context_a,
            0,
            *shared_cache);
    prepared_a.preparation_cache = shared_cache;
    spectiary::PreparedSampleWorkflowState prepared_b =
        spectiary::PrepareSampleWorkflowStateFromCache(
            *snapshot_b,
            context_b,
            0,
            *shared_cache);
    prepared_b.preparation_cache = shared_cache;
    spectiary::PreparedSampleWorkflowState prepared_c =
        spectiary::PrepareSampleWorkflowStateFromCache(
            *snapshot_c,
            context_c,
            0,
            *distinct_stale_cache);
    prepared_c.preparation_cache = distinct_stale_cache;

    PreparedSession session(
        [source_a, source_b, source_c](
            const std::filesystem::path& path,
            std::size_t index) {
            Require(
                path == source_a ||
                    path == source_b ||
                    path == source_c,
                "the stale-warning fixture should open one of its sources");
            return MakeSnapshot(path, 3, index);
        },
        {},
        navigation_cache,
        labeling_cache,
        workflow_cache);
    Require(
        session.OpenPreparedSource(
                   source_a,
                   0,
                   snapshot_a,
                   std::move(context_a),
                   std::move(prepared_a))
            .loaded,
        "the first stale-warning source should open");
    Require(
        session.View().persistence.messages.size() == 3,
        "the first cache bundle should surface all three owner warnings");

    (void)Submit(
        session,
        StartOrResumeTemporaryLabelingTask());
    (void)Submit(
        session,
        spectiary::SourceCollectionSessionIntent::ApplySampleFiltering(
            spectiary::SampleFilteringIntent::Clear()));
    Require(
        session.FlushStateCachesWithStatus().all_saved(),
        "labeling and workflow repairs should save independently");
    const spectiary::LocalUserStateHealthView
        navigation_warning = session.View().persistence;
    Require(
        navigation_warning.kind ==
                spectiary::LocalUserStateHealthKind::Warning &&
            navigation_warning.messages.size() == 1 &&
            HasPersistenceMessage(
                navigation_warning,
                spectiary::LocalUserStateArea::
                    SampleNavigation),
        "only the navigation warning should remain before its first save");

    Require(
        session.OpenPreparedSource(
                   source_b,
                   0,
                   snapshot_b,
                   std::move(context_b),
                   std::move(prepared_b))
            .loaded,
        "the same-batch stale source should open");
    const spectiary::LocalUserStateHealthView
        same_batch_health = session.View().persistence;
    Require(
        same_batch_health.messages.size() == 1 &&
            HasPersistenceMessage(
                same_batch_health,
                spectiary::LocalUserStateArea::
                    SampleNavigation) &&
            !HasPersistenceMessage(
                same_batch_health,
                spectiary::LocalUserStateArea::
                    SampleLabeling) &&
            !HasPersistenceMessage(
                same_batch_health,
                spectiary::LocalUserStateArea::
                    SampleWorkflow),
        "same-batch activation must not resurrect repaired owner warnings");

    spectiary::SourceCollectionSession& base_session = session;
    const spectiary::SourceCollectionSessionResult moved =
        base_session.Submit(
            MoveSampleNavigation(
                spectiary::SampleNavigationRequest::LocateRow(1)));
    Require(
        moved.follow_up_spectrum_index == 1,
        "the navigation repair should request its prepared row");
    Require(
        session.OpenPreparedSource(
                   source_b,
                   1,
                   MakeSnapshot(source_b, 3, 1),
                   spectiary::PreparedSourceCollectionReuse{identity_b})
            .loaded,
        "the navigation repair should commit its prepared row");
    Require(
        session.FlushStateCachesWithStatus().all_saved(),
        "the navigation repair should save");
    Require(
        session.View().persistence.kind ==
            spectiary::LocalUserStateHealthKind::Healthy,
        "all repaired cache owners should become healthy");

    Require(
        session.OpenPreparedSource(
                   source_c,
                   0,
                   snapshot_c,
                   std::move(context_c),
                   std::move(prepared_c))
            .loaded,
        "the distinct stale-bundle source should open");
    Require(
        session.View().persistence.kind ==
                spectiary::LocalUserStateHealthKind::Healthy &&
            session.View().persistence.messages.empty(),
        "a distinct pre-repair bundle must not resurrect cleared warnings");
}

void TestSourceSessionSkipsMissingSourcePathsOnRestore()
{
    const std::filesystem::path source_session_cache = UniqueTempPath("_sources.json");
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    const std::filesystem::path missing_source_path = UniqueTempPath("_missing.npy");
    const std::filesystem::path existing_source_path = UniqueTempPath("_existing.npy");
    TouchFile(existing_source_path);

    spectiary::SourceCollectionSessionStateCache saved_state;
    saved_state.sources = {
        spectiary::SourceCollectionSavedSource{missing_source_path, 3},
        spectiary::SourceCollectionSavedSource{existing_source_path, 1},
    };
    saved_state.active_source_index = 1;
    Require(
        spectiary::SaveSourceCollectionSessionStateCache(spectiary::RuntimePaths{}, source_session_cache, saved_state),
        "source session fixture should save");

    std::vector<LoadedSourceSnapshot> restored_loads;
    PreparedSession restored = MakePersistentSession(
        restored_loads,
        source_session_cache,
        navigation_cache,
        labeling_cache,
        {SourceFixture{existing_source_path, 3}});

    const spectiary::SourceCollectionSessionView view = restored.View();
    Require(view.sources.size() == 1, "restore should skip missing source paths");
    Require(view.sources[0].path == existing_source_path, "existing source should remain after missing source skip");
    Require(view.current_source_index && *view.current_source_index == 0, "remaining source should become active");
    Require(view.snapshot->source.path == existing_source_path, "active snapshot should use the existing source");
    Require(view.snapshot->collection.current_index == 1, "existing source should restore its last row");
    Require(restored_loads.size() == 1, "missing source should not be loaded");
}

void TestSourceSessionRestoresAtMostThirtyTwoSources()
{
    const std::filesystem::path source_session_cache = UniqueTempPath("_sources.json");
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    std::vector<SourceFixture> fixtures;
    spectiary::SourceCollectionSessionStateCache saved_state;
    for (std::size_t index = 0; index < 35; ++index) {
        std::filesystem::path source_path = UniqueTempPath(std::string("_cap_") + std::to_string(index) + ".npy");
        TouchFile(source_path);
        fixtures.push_back(SourceFixture{source_path, 4});
        saved_state.sources.push_back(spectiary::SourceCollectionSavedSource{source_path, index % 4});
    }
    saved_state.active_source_index = 34;
    Require(
        spectiary::SaveSourceCollectionSessionStateCache(spectiary::RuntimePaths{}, source_session_cache, saved_state),
        "source session cap fixture should save");

    std::vector<LoadedSourceSnapshot> restored_loads;
    PreparedSession restored = MakePersistentSession(
        restored_loads,
        source_session_cache,
        navigation_cache,
        labeling_cache,
        fixtures);

    const spectiary::SourceCollectionSessionView view = restored.View();
    Require(view.sources.size() == 32, "restore should cap the source list at 32 entries");
    Require(restored_loads.size() == 32, "restore should load only 32 source snapshots");
    Require(view.sources.back().path == saved_state.sources[31].path, "last restored source should be the 32nd entry");
    Require(view.current_source_index && *view.current_source_index == 31, "out-of-cap active source should not restore");
    Require(view.snapshot->source.path == saved_state.sources[31].path, "last restored source should remain active");
}

void TestDeferredSourceSessionRestoreDoesNotInvokeLoaderOnConstruction()
{
    const std::filesystem::path source_session_cache = UniqueTempPath("_sources.json");
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    const std::filesystem::path workflow_cache = UniqueTempPath("_workflow.json");
    const std::filesystem::path source_path = UniqueTempPath("_deferred.npy");
    const std::filesystem::path unavailable_path = UniqueTempPath("_offline.npy");
    TouchFile(source_path);

    spectiary::SourceCollectionSessionStateCache saved_state;
    saved_state.sources = {
        spectiary::SourceCollectionSavedSource{source_path, 2},
        spectiary::SourceCollectionSavedSource{unavailable_path, 0},
    };
    saved_state.active_source_index = 0;
    Require(
        spectiary::SaveSourceCollectionSessionStateCache(spectiary::RuntimePaths{}, source_session_cache, saved_state),
        "deferred source session fixture should save");

    spectiary::SourceCollectionSession session(
        source_session_cache,
        navigation_cache,
        labeling_cache,
        workflow_cache);

    std::optional<spectiary::SourceCollectionDeferredRestorePlan> plan = session.TakeDeferredRestorePlan();
    Require(plan.has_value(), "deferred restore should publish a background-load plan");
    Require(
        plan->sources.size() == 2,
        "deferred restore planning should not synchronously probe unavailable saved paths");
    Require(plan->sources[0].path == source_path, "deferred restore plan should retain the source path");
    Require(plan->sources[0].last_spectrum_index == 2, "deferred restore plan should retain the saved row");
    Require(
        plan->sources[1].path == unavailable_path,
        "the background worker should decide whether a saved path is available");
    Require(plan->active_source_index == 0, "deferred restore plan should retain the active source");

    const spectiary::SpectrumSnapshotHandle snapshot = MakeSnapshot(source_path, 4, 2);
    spectiary::SourceCollectionContext context;
    context.identity = spectiary::BuildSourceCollectionIdentity(*snapshot);
    spectiary::PreparedSampleWorkflowState prepared_workflow =
        PrepareWorkflow(snapshot, context, 2, labeling_cache, workflow_cache);
    const spectiary::SourceCollectionSessionResult result = session.OpenPreparedSource(
        source_path,
        2,
        snapshot,
        std::move(context),
        std::move(prepared_workflow));
    Require(result.loaded, "prepared deferred source should commit");
    Require(
        session.View().snapshot->collection.current_index == 2,
        "prepared deferred commit should retain the saved row");
    session.FinishDeferredRestore();
    (void)Submit(
        session,
        spectiary::SourceCollectionSessionIntent::EditSourceCollection(
            spectiary::SourceCollectionIntent::Remove(0)));
    Require(session.FlushStateCaches(), "user mutation after deferred restore should persist");
    const spectiary::SourceCollectionSessionStateCache persisted =
        spectiary::LoadSourceCollectionSessionStateCache(spectiary::RuntimePaths{}, source_session_cache)
            .cache;
    Require(persisted.sources.size() == 1, "unresolved deferred source intent should remain persisted");
    Require(
        persisted.sources.front().path == unavailable_path,
        "removing a restored source should not silently delete a separate unresolved source intent");
}

void TestSupersededDeferredRestorePreservesPersistedSourceIntents()
{
    const std::filesystem::path source_session_cache = UniqueTempPath("_superseded_sources.json");
    const std::filesystem::path navigation_cache = UniqueTempPath("_superseded_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_superseded_labeling.json");
    const std::filesystem::path workflow_cache = UniqueTempPath("_superseded_workflow.json");
    const std::filesystem::path source_a = UniqueTempPath("_historical_a.npy");
    const std::filesystem::path source_b = UniqueTempPath("_historical_b.npy");
    const std::filesystem::path source_d = UniqueTempPath("_new_d.npy");
    const std::filesystem::path annotation_a = UniqueTempPath("_historical_a_annotation.npy");
    const std::filesystem::path annotation_b = UniqueTempPath("_historical_b_annotation.npy");

    spectiary::SourceCollectionSessionStateCache saved_state;
    saved_state.sources = {
        spectiary::SourceCollectionSavedSource{source_a, 3, {annotation_a}},
        spectiary::SourceCollectionSavedSource{source_b, 7, {annotation_b}},
    };
    saved_state.active_source_index = 1;
    Require(
        spectiary::SaveSourceCollectionSessionStateCache(spectiary::RuntimePaths{}, source_session_cache, saved_state),
        "superseded restore fixture should save");

    spectiary::SourceCollectionSession session(
        source_session_cache,
        navigation_cache,
        labeling_cache,
        workflow_cache);
    const std::optional<spectiary::SourceCollectionDeferredRestorePlan> plan =
        session.TakeDeferredRestorePlan();
    Require(plan && plan->sources.size() == 2, "fixture should produce two deferred restore tasks");

    // Mirrors ShellUi superseding the background restore before either A or B
    // commits, then accepting a newer explicit user source D.
    session.FinishDeferredRestore();
    const spectiary::SpectrumSnapshotHandle snapshot_d = MakeSnapshot(source_d, 1, 0);
    spectiary::SourceCollectionContext context_d;
    context_d.identity = spectiary::BuildSourceCollectionIdentity(*snapshot_d);
    spectiary::PreparedSampleWorkflowState prepared_workflow_d =
        PrepareWorkflow(snapshot_d, context_d, 0, labeling_cache, workflow_cache);
    Require(
        session.OpenPreparedSource(
            source_d,
            0,
            snapshot_d,
            std::move(context_d),
            std::move(prepared_workflow_d)).loaded,
        "new explicit source should commit");
    Require(session.FlushStateCaches(), "new explicit source should flush the source-session cache");

    const spectiary::SourceCollectionSessionStateCache persisted =
        spectiary::LoadSourceCollectionSessionStateCache(spectiary::RuntimePaths{}, source_session_cache)
            .cache;
    Require(persisted.sources.size() == 3, "superseding restore must preserve A and B while adding D");
    const auto find_source = [&persisted](const std::filesystem::path& path) {
        return std::find_if(persisted.sources.begin(), persisted.sources.end(), [&path](const auto& source) {
            return source.path == path;
        });
    };
    const auto saved_a = find_source(source_a);
    const auto saved_b = find_source(source_b);
    const auto saved_d = find_source(source_d);
    Require(saved_a != persisted.sources.end(), "historical source A should remain persisted");
    Require(saved_b != persisted.sources.end(), "historical source B should remain persisted");
    Require(saved_d != persisted.sources.end(), "new explicit source D should be persisted");
    Require(saved_a->last_spectrum_index == 3, "historical source A row should remain intact");
    Require(saved_b->last_spectrum_index == 7, "historical source B row should remain intact");
    Require(
        saved_a->annotation_paths == std::vector<std::filesystem::path>{annotation_a},
        "historical source A annotations should remain intact");
    Require(
        saved_b->annotation_paths == std::vector<std::filesystem::path>{annotation_b},
        "historical source B annotations should remain intact");
    Require(
        persisted.active_source_index &&
            persisted.sources[*persisted.active_source_index].path == source_d,
        "the newer explicit source should remain the persisted active source");
}

void TestForgettingUnavailableDeferredSourcePersistsDuringRestore()
{
    const std::filesystem::path source_cache = UniqueTempPath("_forget_unavailable_sources.json");
    const std::filesystem::path unavailable_a = UniqueTempPath("_unavailable_a.npy");
    const std::filesystem::path unavailable_b = UniqueTempPath("_unavailable_b.npy");
    spectiary::SourceCollectionSessionStateCache saved;
    saved.sources = {
        spectiary::SourceCollectionSavedSource{unavailable_a, 2, {}},
        spectiary::SourceCollectionSavedSource{unavailable_b, 4, {}},
    };
    saved.active_source_index = 0;
    Require(
        spectiary::SaveSourceCollectionSessionStateCache(spectiary::RuntimePaths{}, source_cache, saved),
        "unavailable source fixture should save");

    spectiary::SourceCollectionSession session(
        source_cache,
        std::filesystem::path{},
        std::filesystem::path{},
        std::filesystem::path{});
    const auto plan = session.TakeDeferredRestorePlan();
    Require(plan && plan->sources.size() == 2, "fixture should expose both unresolved restore intents");
    Require(session.HasUnresolvedSourceIntent(unavailable_a), "source A should initially be unresolved");
    Require(
        session.ForgetUnresolvedSourceIntent(unavailable_a),
        "terminal Files action should permanently forget unavailable source A");
    Require(
        !session.HasUnresolvedSourceIntent(unavailable_a) && session.HasUnresolvedSourceIntent(unavailable_b),
        "Forget must remove only the selected path identity");
    Require(
        session.FlushStateCaches(),
        "closing during deferred restore must flush an explicit Forget immediately");

    const spectiary::SourceCollectionSessionStateCache persisted =
        spectiary::LoadSourceCollectionSessionStateCache(spectiary::RuntimePaths{}, source_cache)
            .cache;
    Require(persisted.sources.size() == 1, "forgotten unavailable source must not return on restart");
    Require(persisted.sources.front().path == unavailable_b, "unrelated unresolved source B must remain persisted");
}

void TestPreparedRestoreDoesNotExposeSnapshotForAReconciledDifferentRow()
{
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    const std::filesystem::path workflow_cache = UniqueTempPath("_workflow.json");
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path annotation_path = UniqueTempPath("_quality.npy");
    const std::string annotation_source_id = AnnotationSourceId(annotation_path);
    TouchFile(source_path);
    SaveLabelResultFixture(
        annotation_path,
        "quality",
        "Quality",
        {1, 2, 2},
        spectiary::SampleLabelSet{},
        false);

    {
        std::vector<LoadedSourceSnapshot> loaded_snapshots;
        PreparedSession source = MakeWorkflowPersistentSession(
            loaded_snapshots,
            navigation_cache,
            labeling_cache,
            workflow_cache,
            source_path,
            3);
        (void)Submit(source, OpenSourceCollection(source_path, 0));
        Require(Submit(source, AddReadOnlyAnnotation(annotation_path)).loaded, "filter annotation should load");
        (void)Submit(source, AddSampleFilterSource(annotation_source_id));
        (void)Submit(source, SetFilterValueSelected(annotation_source_id, "1", true));
        Require(source.FlushStateCaches(), "workflow fixture should save");
    }

    spectiary::SourceCollectionSession restored(
        std::filesystem::path{},
        navigation_cache,
        labeling_cache,
        workflow_cache);
    const spectiary::SpectrumSnapshotHandle prepared_snapshot = MakeSnapshot(source_path, 3, 2);
    spectiary::SourceCollectionContext context;
    context.identity = spectiary::BuildSourceCollectionIdentity(*prepared_snapshot);
    std::string annotation_error;
    std::optional<spectiary::SampleAnnotationResult> annotation =
        spectiary::test_support::LegacyFixtureIo{}.Load(annotation_path, 3, &annotation_error);
    Require(annotation.has_value(), "prepared restore annotation fixture should load");
    context.manifest.annotations.push_back(std::move(*annotation));
    spectiary::PreparedSampleWorkflowState prepared_workflow =
        PrepareWorkflow(prepared_snapshot, context, 2, labeling_cache, workflow_cache);

    const spectiary::SourceCollectionSessionResult result = restored.OpenPreparedSource(
        source_path,
        2,
        prepared_snapshot,
        std::move(context),
        std::move(prepared_workflow));
    const spectiary::SourceCollectionSessionView view = restored.View();
    Require(view.navigation.current_index == 0, "restored filter should reconcile navigation to row 0");
    Require(
        restored.CurrentSampleSnapshot() == nullptr && view.current_sample_snapshot == nullptr,
        "plot must not receive the prepared row 2 snapshot while labeling points at row 0");
    Require(result.follow_up_spectrum_index == 0, "prepared restore should request a background row 0 load");
    Require(result.loaded, "prepared source should still be accepted while the corrected row is pending");

    const spectiary::SpectrumSnapshotHandle corrected_snapshot = MakeSnapshot(source_path, 3, 0);
    spectiary::SourceCollectionContext corrected_context;
    corrected_context.identity = spectiary::BuildSourceCollectionIdentity(*corrected_snapshot);
    std::optional<spectiary::SampleAnnotationResult> corrected_annotation =
        spectiary::test_support::LegacyFixtureIo{}.Load(annotation_path, 3, &annotation_error);
    Require(corrected_annotation.has_value(), "corrected prepared annotation fixture should load");
    corrected_context.manifest.annotations.push_back(std::move(*corrected_annotation));
    spectiary::PreparedSampleWorkflowState corrected_workflow =
        PrepareWorkflow(corrected_snapshot, corrected_context, 0, labeling_cache, workflow_cache);
    const spectiary::SourceCollectionSessionResult corrected = restored.OpenPreparedSource(
        source_path,
        0,
        corrected_snapshot,
        std::move(corrected_context),
        std::move(corrected_workflow));
    Require(!corrected.follow_up_spectrum_index, "corrected row should complete prepared restoration");
    Require(
        restored.CurrentSampleSnapshot() &&
            restored.CurrentSampleSnapshot()->collection.current_index == 0,
        "plot and labeling should converge on the reconciled row 0 snapshot");
    const auto& corrected_transition =
        restored.View().sample_transition;
    Require(
        corrected_transition &&
            corrected_transition->current_sample_index == 0 &&
            !corrected_transition->from_sample_index,
        "a prepared snapshot that was never presented must not become the transition source row");
}

void TestReturningToPresentedSampleClearsTentativeTransition()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_deferred_transition_return.npy");
    spectiary::SourceCollectionSession session({}, {}, {}, {});

    const spectiary::SpectrumSnapshotHandle initial_snapshot =
        MakeSnapshot(source_path, 3, 0);
    spectiary::SourceCollectionContext context;
    context.identity = {
        "deferred-transition-return",
        "source",
        "source-fingerprint",
        "context-fingerprint",
        3,
    };
    context.manifest.sample_names = {"alpha", "beta", "gamma"};
    spectiary::PreparedSampleWorkflowState prepared =
        PrepareWorkflow(initial_snapshot, context, 0, {}, {});
    Require(
        session.OpenPreparedSource(
                   source_path,
                   0,
                   initial_snapshot,
                   std::move(context),
                   std::move(prepared))
            .loaded,
        "deferred transition-return fixture should load");

    Require(
        Submit(
            session,
            MoveSampleNavigation(
                spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 1,
        "next should queue row 1 while row 0 stays presented");
    Require(
        !session.View().sample_transition,
        "a tentative transition should stay hidden while its target snapshot is pending");

    const spectiary::SourceCollectionSessionResult returned =
        Submit(
            session,
            MoveSampleNavigation(
                spectiary::SampleNavigationRequest::Previous()));
    Require(
        !returned.follow_up_spectrum_index &&
            session.CurrentSampleSnapshot() == initial_snapshot,
        "previous from pending row 1 should return to the already presented row 0");
    Require(
        !session.View().sample_transition,
        "returning to the already presented row must clear the tentative transition instead of publishing 1-to-0");
}

void TestDeferredTransitionUsesPresentedSampleAsSource()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_deferred_transition_source.npy");
    spectiary::SourceCollectionSession session({}, {}, {}, {});

    const spectiary::SpectrumSnapshotHandle initial_snapshot =
        MakeSnapshot(source_path, 3, 0);
    spectiary::SourceCollectionContext context;
    context.identity = {
        "deferred-transition-source",
        "source",
        "source-fingerprint",
        "context-fingerprint",
        3,
    };
    context.manifest.sample_names = {"alpha", "beta", "gamma"};
    const spectiary::SourceCollectionIdentity identity =
        context.identity;
    spectiary::PreparedSampleWorkflowState prepared =
        PrepareWorkflow(initial_snapshot, context, 0, {}, {});
    Require(
        session.OpenPreparedSource(
                   source_path,
                   0,
                   initial_snapshot,
                   std::move(context),
                   std::move(prepared))
            .loaded,
        "deferred transition-source fixture should load");

    Require(
        Submit(
            session,
            MoveSampleNavigation(
                spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 1,
        "first next should queue row 1");
    Require(
        Submit(
            session,
            MoveSampleNavigation(
                spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 2,
        "second next should advance the pending cursor to row 2");

    const spectiary::SpectrumSnapshotHandle final_snapshot =
        MakeSnapshot(source_path, 3, 2);
    Require(
        session.OpenPreparedSource(
                   source_path,
                   2,
                   final_snapshot,
                   spectiary::PreparedSourceCollectionReuse{identity})
            .loaded,
        "the final pending row should commit directly");
    const auto& transition =
        session.View().sample_transition;
    Require(
        transition &&
            transition->reason ==
                spectiary::SourceCollectionSampleTransitionReason::Next &&
            transition->from_sample_index == 0 &&
            transition->current_sample_index == 2,
        "a committed deferred transition must use the last presented row rather than an unseen pending cursor as its source");
}

void TestEmptyPreparedReconciliationClearsTentativeTransition()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_empty_prepared_reconciliation.npy");
    spectiary::SourceCollectionSession session({}, {}, {}, {});

    const spectiary::SpectrumSnapshotHandle initial_snapshot =
        MakeSnapshot(source_path, 3, 0);
    spectiary::SourceCollectionContext initial_context;
    initial_context.identity = {
        "empty-prepared-reconciliation",
        "source",
        "source-fingerprint",
        "initial-context",
        3,
    };
    initial_context.manifest.sample_names = {"alpha", "beta", "gamma"};
    spectiary::PreparedSampleWorkflowState initial_workflow =
        PrepareWorkflow(
            initial_snapshot,
            initial_context,
            0,
            {},
            {});
    Require(
        session.OpenPreparedSource(
                   source_path,
                   0,
                   initial_snapshot,
                   std::move(initial_context),
                   std::move(initial_workflow))
            .loaded,
        "empty prepared-reconciliation fixture should load");
    Require(
        Submit(
            session,
            MoveSampleNavigation(
                spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 1,
        "next should queue the intermediate row");

    const spectiary::SpectrumSnapshotHandle intermediate_snapshot =
        MakeSnapshot(source_path, 3, 1);
    spectiary::SourceCollectionContext changed_context;
    changed_context.identity = {
        "empty-prepared-reconciliation",
        "source",
        "source-fingerprint",
        "empty-context",
        3,
    };
    changed_context.manifest.sample_names = {"alpha", "beta", "gamma"};
    spectiary::PreparedSampleWorkflowState empty_workflow =
        PrepareWorkflow(
            intermediate_snapshot,
            changed_context,
            1,
            {},
            {});
    empty_workflow.current_index.reset();
    empty_workflow.navigation_sequence.empty = true;
    empty_workflow.navigation_sequence.current_source_row.reset();
    empty_workflow.navigation_sequence.current_sequence_position.reset();

    const spectiary::SourceCollectionSessionResult reconciled =
        session.OpenPreparedSource(
            source_path,
            1,
            intermediate_snapshot,
            std::move(changed_context),
            std::move(empty_workflow));
    Require(
        reconciled.load_error.kind ==
                spectiary::SourceCollectionLoadErrorKind::
                    PreparedNavigationUnavailable &&
            !reconciled.follow_up_spectrum_index,
        "empty reconciliation should cancel the pending target");
    Require(
        session.CurrentSampleSnapshot() == initial_snapshot &&
            session.View().navigation.current_index == 0,
        "empty reconciliation should retain the previous complete row 0 presentation");
    Require(
        !session.View().sample_transition,
        "empty reconciliation must clear the tentative transition instead of publishing a null target over row 0");
}

void TestDeferredNavigationKeepsPresentedSampleUntilPreparedSnapshotCommits()
{
    const std::filesystem::path source_path = UniqueTempPath("_deferred_navigation.npy");
    spectiary::SourceCollectionSession session({}, {}, {}, {});

    const spectiary::SpectrumSnapshotHandle initial_snapshot = MakeSnapshot(source_path, 3, 0);
    spectiary::SourceCollectionContext context;
    context.identity = {"deferred-navigation", "source", "source-fingerprint", "context-fingerprint", 3};
    context.manifest.sample_names = {"alpha", "beta", "gamma"};
    const spectiary::SourceCollectionIdentity identity = context.identity;
    spectiary::PreparedSampleWorkflowState prepared =
        PrepareWorkflow(initial_snapshot, context, 0, {}, {});
    Require(
        session.OpenPreparedSource(
            source_path,
            0,
            initial_snapshot,
            std::move(context),
            std::move(prepared)).loaded,
        "initial prepared source should commit");
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    const spectiary::SourceCollectionLabelingView initial_labeling = session.View().labeling;

    const spectiary::SourceCollectionSessionResult pending =
        Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::Next()));
    const spectiary::SourceCollectionSessionView pending_view = session.View();
    Require(pending.follow_up_spectrum_index == 1, "next should request row 1 in the background");
    Require(
        pending_view.navigation.current_index == 0,
        "navigation should keep presenting row 0 while row 1 is pending");
    Require(
        pending_view.current_sample_snapshot == initial_snapshot &&
            pending_view.current_sample_snapshot->collection.current_index == 0,
        "sample panels should keep presenting the complete row 0 snapshot while row 1 is pending");
    Require(
        pending_view.labeling.current_index == 0,
        "labeling should remain bound to the presented row while navigation is pending");
    Require(
        pending_view.labeling.remembered_position == initial_labeling.remembered_position,
        "labeling controls should not expose the pending row before its snapshot commits");

    const spectiary::SourceCollectionSessionResult reversed =
        Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::Previous()));
    Require(
        reversed.canceled_source_follow_up_path == source_path &&
            !reversed.follow_up_spectrum_index,
        "returning to the presented row should cancel the obsolete background follow-up");
    Require(
        session.View().navigation.current_index == 0 &&
            session.CurrentSampleSnapshot() == initial_snapshot,
        "canceling pending navigation should leave the committed presentation unchanged");

    Require(
        Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 1,
        "navigation should be able to request row 1 again after cancellation");
    Require(
        Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 2,
        "a repeated next should advance from the pending target instead of the presented row");
    const spectiary::SourceCollectionSessionView&
        row_two_pending_view = session.View();
    Require(
        row_two_pending_view.navigation.can_move_previous &&
            !row_two_pending_view.navigation.can_move_next,
        "navigation buttons should use pending row 2 while the visible presentation remains on row 0");
    (void)session.TakeViewRetirement();
    Require(
        !Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::Next()))
             .follow_up_spectrum_index,
        "repeating next at the pending sequence boundary should retain the existing row 2 ticket");
    Require(
        session.View().navigation.current_index == 0 &&
            session.CurrentSampleSnapshot() == initial_snapshot,
        "coalescing navigation should continue presenting the last complete sample");
    Require(
        session.CancelPendingSampleNavigation(source_path, 2),
        "a failed latest background load should cancel its matching pending navigation");
    const spectiary::SourceCollectionSessionView&
        canceled_pending_view = session.View();
    Require(
        &canceled_pending_view !=
                &row_two_pending_view &&
            canceled_pending_view.navigation.current_index ==
                0 &&
            !canceled_pending_view.navigation.can_move_previous &&
            canceled_pending_view.navigation.can_move_next &&
            session.CurrentSampleSnapshot() ==
                initial_snapshot,
        "a failed background load should retain the last complete presentation");
    Require(
        session.TakeViewRetirement().size() == 1 &&
            &session.View() == &canceled_pending_view,
        "pending cancellation should retire exactly one old projection");
    Require(
        Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 1,
        "navigation after a failed load should resume from the presented row");

    const spectiary::SpectrumSnapshotHandle next_snapshot = MakeSnapshot(source_path, 3, 1);
    const spectiary::SourceCollectionSessionResult committed = session.OpenPreparedSource(
        source_path,
        1,
        next_snapshot,
        spectiary::PreparedSourceCollectionReuse{identity});
    const spectiary::SourceCollectionSessionView committed_view = session.View();
    Require(committed.loaded, "prepared row 1 should commit");
    Require(!committed.follow_up_spectrum_index, "the committed row should need no corrective follow-up");
    Require(
        committed_view.navigation.current_index == 1,
        "navigation should switch to row 1 when its snapshot commits");
    Require(
        committed_view.current_sample_snapshot == next_snapshot &&
            committed_view.current_sample_snapshot->collection.current_index == 1,
        "sample panels should switch to the complete row 1 snapshot on commit");
    Require(
        committed_view.labeling.current_index == 1,
        "labeling should switch to row 1 in the same committed presentation");
}

void TestDeferredFilterRetargetsPendingNavigationWithoutChangingCommittedPresentation()
{
    const std::filesystem::path source_path = UniqueTempPath("_deferred_filter_retarget.npy");
    const std::filesystem::path annotation_path = UniqueTempPath("_deferred_filter_values.npy");
    SaveLabelResultFixture(
        annotation_path,
        "deferred-filter-values",
        "Deferred filter values",
        {0, 0, 1},
        spectiary::SampleLabelSet{},
        false);
    spectiary::SourceCollectionSession session({}, {}, {}, {});

    const spectiary::SpectrumSnapshotHandle initial_snapshot = MakeSnapshot(source_path, 3, 0);
    spectiary::SourceCollectionContext context;
    context.identity = {"deferred-filter-retarget", "source", "source-fingerprint", "context", 3};
    context.manifest.sample_names = {"alpha", "beta", "gamma"};
    const spectiary::SourceCollectionIdentity identity = context.identity;
    spectiary::PreparedSampleWorkflowState prepared =
        PrepareWorkflow(initial_snapshot, context, 0, {}, {});
    Require(
        session.OpenPreparedSource(
                   source_path,
                   0,
                   initial_snapshot,
                   std::move(context),
                   std::move(prepared))
            .loaded,
        "deferred filter fixture should load");
    Require(Submit(session, AddReadOnlyAnnotation(annotation_path)).loaded, "filter annotation should load");
    const std::string filter_source_id = AnnotationSourceId(annotation_path);
    (void)Submit(session, AddSampleFilterSource(filter_source_id));
    Require(
        Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 1,
        "manual next should initially queue row 1");

    const spectiary::SourceCollectionSessionResult filtered =
        Submit(session, SetFilterValueSelected(filter_source_id, "1", true));
    const spectiary::SourceCollectionSessionView pending_view = session.View();
    Require(filtered.follow_up_spectrum_index == 2, "filter reconciliation should replace row 1 with row 2");
    Require(
        pending_view.navigation.current_index == 0 &&
            pending_view.current_sample_snapshot == initial_snapshot &&
            pending_view.labeling.current_index == 0,
        "filter reconciliation should retain the complete committed row 0 presentation");

    const spectiary::SpectrumSnapshotHandle filtered_snapshot = MakeSnapshot(source_path, 3, 2);
    const spectiary::SourceCollectionSessionResult committed = session.OpenPreparedSource(
        source_path,
        2,
        filtered_snapshot,
        spectiary::PreparedSourceCollectionReuse{identity});
    const spectiary::SourceCollectionSessionView committed_view = session.View();
    Require(committed.loaded && !committed.follow_up_spectrum_index, "filtered row 2 should commit once");
    Require(
        committed_view.navigation.current_index == 2 &&
            committed_view.current_sample_snapshot == filtered_snapshot,
        "filter target and snapshot should become visible together");
}

void TestExplicitCommittedSequencePositionCancelsPendingNavigation()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_deferred_sequence_cancel.npy");
    spectiary::SourceCollectionSession session({}, {}, {}, {});

    const spectiary::SpectrumSnapshotHandle initial_snapshot =
        MakeSnapshot(source_path, 3, 0);
    spectiary::SourceCollectionContext context;
    context.identity = {
        "deferred-sequence-cancel",
        "source",
        "source-fingerprint",
        "context",
        3,
    };
    context.manifest.sample_names = {"alpha", "beta", "gamma"};
    spectiary::PreparedSampleWorkflowState prepared =
        PrepareWorkflow(initial_snapshot, context, 0, {}, {});
    Require(
        session.OpenPreparedSource(
                   source_path,
                   0,
                   initial_snapshot,
                   std::move(context),
                   std::move(prepared))
            .loaded,
        "deferred sequence-cancel fixture should load");

    (void)Submit(
        session,
        SetSampleSortSource("sample-name"));
    Require(
        session.View().navigation.sequence_active &&
            session.View().navigation.current_sequence_position == 0,
        "sample-name sorting should expose committed sequence position A");

    const spectiary::SourceCollectionSessionResult pending = Submit(
        session,
        MoveSampleNavigation(
            spectiary::SampleNavigationRequest::
                LocateSequencePosition(1)));
    Require(
        pending.follow_up_spectrum_index == 1 &&
            session.EffectiveSampleNavigationIndex() == 1,
        "explicit sequence position B should become the deferred target while A remains displayed");
    Require(
        session.View().navigation.current_index == 0 &&
            session.View().navigation.current_sequence_position == 0,
        "the pending B request must not replace the committed A presentation");

    const spectiary::SourceCollectionSessionResult canceled = Submit(
        session,
        MoveSampleNavigation(
            spectiary::SampleNavigationRequest::
                LocateSequencePosition(0)));
    Require(
        !canceled.follow_up_spectrum_index &&
            canceled.canceled_source_follow_up_path == source_path,
        "explicitly resubmitting committed A should cancel B's source-bound follow-up");
    Require(
        session.EffectiveSampleNavigationIndex() == 0 &&
            !session.CancelActivePendingSampleNavigation(),
        "latest intent A should clear the deferred B target");
}

void TestDeferredLabelAutoAdvanceUsesTheVisibleLabeledSampleAsItsBase()
{
    const std::filesystem::path source_path = UniqueTempPath("_deferred_label_advance.npy");
    spectiary::SourceCollectionSession session({}, {}, {}, {});

    const spectiary::SpectrumSnapshotHandle initial_snapshot = MakeSnapshot(source_path, 3, 0);
    spectiary::SourceCollectionContext context;
    context.identity = {"deferred-label-advance", "source", "source-fingerprint", "context", 3};
    context.manifest.sample_names = {"alpha", "beta", "gamma"};
    const spectiary::SourceCollectionIdentity identity = context.identity;
    spectiary::PreparedSampleWorkflowState prepared =
        PrepareWorkflow(initial_snapshot, context, 0, {}, {});
    Require(
        session.OpenPreparedSource(
                   source_path,
                   0,
                   initial_snapshot,
                   std::move(context),
                   std::move(prepared))
            .loaded,
        "deferred label fixture should load");
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{1, "accepted", 'a'})).changed,
        "deferred label fixture should add its label");
    (void)Submit(session, SetActiveLabelingAutoAdvance(true));
    Require(
        Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 1,
        "manual next should queue row 1 before labeling row 0");

    const spectiary::SourceCollectionSessionResult labeled =
        Submit(session, AssignActiveLabelToCurrentSample(1));
    const spectiary::SourceCollectionSessionView pending_view = session.View();
    Require(
        !labeled.follow_up_spectrum_index,
        "auto-advance from visible row 0 should retain the existing row 1 ticket instead of jumping to row 2");
    Require(
        pending_view.current_sample_snapshot == initial_snapshot &&
            pending_view.labeling.current_index == 0 &&
            pending_view.labeling.current_code == 1 &&
            !pending_view.sample_transition,
        "the label write should remain visibly attached to committed row 0 while row 1 loads");

    const spectiary::SpectrumSnapshotHandle next_snapshot = MakeSnapshot(source_path, 3, 1);
    Require(
        session.OpenPreparedSource(
                   source_path,
                   1,
                   next_snapshot,
                   spectiary::PreparedSourceCollectionReuse{identity})
            .loaded,
        "the retained row 1 ticket should still commit");
    Require(
        session.View().navigation.current_index == 1 &&
            session.View().current_sample_snapshot == next_snapshot,
        "auto-advance should land on row 1 after its complete snapshot arrives");
    const auto& committed_transition =
        session.View().sample_transition;
    Require(
        committed_transition &&
            committed_transition->reason ==
                spectiary::SourceCollectionSampleTransitionReason::
                    LabelingAutoAdvance &&
            committed_transition->from_sample_index == 0 &&
            committed_transition->current_sample_index == 1 &&
            committed_transition->accepted_label_value == 1,
        "auto-advance feedback should become presentation-visible only with the committed target sample");
}

void TestManualNavigationTakesOverMatchingAutoAdvanceTarget()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_manual_matching_auto_advance.npy");
    spectiary::SourceCollectionSession session({}, {}, {}, {});

    const spectiary::SpectrumSnapshotHandle initial_snapshot =
        MakeSnapshot(source_path, 2, 0);
    spectiary::SourceCollectionContext context;
    context.identity = {
        "manual-matching-auto-advance",
        "source",
        "source-fingerprint",
        "context",
        2,
    };
    context.manifest.sample_names = {"alpha", "beta"};
    const spectiary::SourceCollectionIdentity identity = context.identity;
    spectiary::PreparedSampleWorkflowState prepared =
        PrepareWorkflow(initial_snapshot, context, 0, {}, {});
    Require(
        session.OpenPreparedSource(
                   source_path,
                   0,
                   initial_snapshot,
                   std::move(context),
                   std::move(prepared))
            .loaded,
        "manual matching-target fixture should load");
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(
            session,
            UpsertActiveLabel(
                spectiary::SampleLabelDefinition{1, "accepted", 'a'}))
            .changed,
        "manual matching-target fixture should add its label");
    (void)Submit(session, SetActiveLabelingAutoAdvance(true));

    Require(
        Submit(session, AssignActiveLabelToCurrentSample(1))
                .follow_up_spectrum_index == 1,
        "labeling row 0 should queue auto-advance to row 1");
    Require(
        !session.View().sample_transition,
        "auto-advance feedback should stay hidden while row 1 is pending");

    const spectiary::SourceCollectionSessionResult located = Submit(
        session,
        MoveSampleNavigation(
            spectiary::SampleNavigationRequest::LocateRow(1)));
    Require(
        !located.follow_up_spectrum_index &&
            session.EffectiveSampleNavigationIndex() == 1,
        "explicitly locating the same pending row should retain its worker ticket");

    const spectiary::SpectrumSnapshotHandle next_snapshot =
        MakeSnapshot(source_path, 2, 1);
    Require(
        session.OpenPreparedSource(
                   source_path,
                   1,
                   next_snapshot,
                   spectiary::PreparedSourceCollectionReuse{identity})
            .loaded,
        "the manually claimed row 1 ticket should commit");
    const auto& transition = session.View().sample_transition;
    Require(
        transition &&
            transition->reason ==
                spectiary::SourceCollectionSampleTransitionReason::LocateRow &&
            transition->from_sample_index == 0 &&
            transition->current_sample_index == 1 &&
            !transition->accepted_label_value,
        "successful explicit navigation must replace matching pending auto-advance feedback");
}

void TestPendingNavigationCancellationClearsTentativeTransition()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_pending_transition_cancellation.npy");
    spectiary::SourceCollectionSession session({}, {}, {}, {});

    const spectiary::SpectrumSnapshotHandle initial_snapshot =
        MakeSnapshot(source_path, 2, 0);
    spectiary::SourceCollectionContext context;
    context.identity = {
        "pending-transition-cancellation",
        "source",
        "source-fingerprint",
        "context",
        2,
    };
    context.manifest.sample_names = {"alpha", "beta"};
    spectiary::PreparedSampleWorkflowState prepared =
        PrepareWorkflow(initial_snapshot, context, 0, {}, {});
    Require(
        session.OpenPreparedSource(
                   source_path,
                   0,
                   initial_snapshot,
                   std::move(context),
                   std::move(prepared))
            .loaded,
        "pending cancellation fixture should load");

    Require(
        Submit(session, MoveSampleNavigation(
                            spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 1,
        "path-bound cancellation fixture should queue row 1");
    const spectiary::SourceCollectionSessionView& path_pending_view =
        session.View();
    (void)session.TakeViewRetirement();
    Require(
        session.CancelPendingSampleNavigation(source_path, 1),
        "matching path-bound pending navigation should cancel");
    Require(
        &session.View() != &path_pending_view &&
            session.View().navigation.current_index == 0 &&
            !session.View().sample_transition,
        "path-bound cancellation must invalidate the view and clear tentative transition provenance");

    Require(
        Submit(session, MoveSampleNavigation(
                            spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 1,
        "active cancellation fixture should queue row 1 again");
    const spectiary::SourceCollectionSessionView& active_pending_view =
        session.View();
    (void)session.TakeViewRetirement();
    Require(
        session.CancelActivePendingSampleNavigation(),
        "active pending navigation should cancel");
    Require(
        &session.View() != &active_pending_view &&
            session.View().navigation.current_index == 0 &&
            !session.View().sample_transition,
        "active cancellation must invalidate the view and clear tentative transition provenance");
}

void TestDeferredLabelAutoAdvanceUpgradesMatchingFilterPendingPositionSemantics()
{
    const std::filesystem::path source_path = UniqueTempPath("_deferred_label_merge.npy");
    const std::filesystem::path annotation_path = UniqueTempPath("_deferred_label_merge_filter.npy");
    SaveLabelResultFixture(
        annotation_path,
        "deferred-label-merge-filter",
        "Deferred label merge filter",
        {0, 1, 0},
        spectiary::SampleLabelSet{},
        false);
    spectiary::SourceCollectionSession session({}, {}, {}, {});

    const spectiary::SpectrumSnapshotHandle initial_snapshot = MakeSnapshot(source_path, 3, 0);
    spectiary::SourceCollectionContext context;
    context.identity = {"deferred-label-merge", "source", "source-fingerprint", "context", 3};
    context.manifest.sample_names = {"alpha", "beta", "gamma"};
    const spectiary::SourceCollectionIdentity identity = context.identity;
    spectiary::PreparedSampleWorkflowState prepared =
        PrepareWorkflow(initial_snapshot, context, 0, {}, {});
    Require(
        session.OpenPreparedSource(
                   source_path,
                   0,
                   initial_snapshot,
                   std::move(context),
                   std::move(prepared))
            .loaded,
        "deferred label merge fixture should load");
    Require(Submit(session, AddReadOnlyAnnotation(annotation_path)).loaded, "filter annotation should load");
    const std::string filter_source_id = AnnotationSourceId(annotation_path);
    (void)Submit(session, AddSampleFilterSource(filter_source_id));
    Require(
        Submit(session, SetFilterValueSelected(filter_source_id, "1", true))
                .follow_up_spectrum_index == 1,
        "filter reconciliation should queue row 1 without labeling-position semantics");

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{1, "accepted", 'a'})).changed,
        "deferred label merge fixture should add its label");
    (void)Submit(session, SetActiveLabelingAutoAdvance(true));
    const spectiary::SourceCollectionSessionResult labeled =
        Submit(session, AssignActiveLabelToCurrentSample(1));
    Require(
        !labeled.follow_up_spectrum_index,
        "auto-advance to the same pending row should retain the filter's existing worker ticket");

    const spectiary::SpectrumSnapshotHandle next_snapshot = MakeSnapshot(source_path, 3, 1);
    Require(
        session.OpenPreparedSource(
                   source_path,
                   1,
                   next_snapshot,
                   spectiary::PreparedSourceCollectionReuse{identity})
            .loaded,
        "the merged row 1 request should commit through the retained ticket");
    Require(
        session.View().labeling.remembered_position == 1,
        "label auto-advance should upgrade the matching filter pending request to remember row 1");
}

void TestDeferredLabelAutoAdvancePreservesNewLocalFilterFollowUp()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_deferred_local_label_filter.npy");
    const std::filesystem::path output_path =
        UniqueTempPath("_deferred_local_label_filter_result.asdf");
    spectiary::SourceCollectionSession session({}, {}, {}, {});

    const spectiary::SpectrumSnapshotHandle initial_snapshot =
        MakeSnapshot(source_path, 3, 0);
    spectiary::SourceCollectionContext context;
    context.identity = {
        "deferred-local-label-filter",
        "source",
        "source-fingerprint",
        "context",
        3,
    };
    context.manifest.sample_names = {"alpha", "beta", "gamma"};
    const spectiary::SourceCollectionIdentity identity = context.identity;
    spectiary::PreparedSampleWorkflowState prepared =
        PrepareWorkflow(initial_snapshot, context, 0, {}, {});
    Require(
        session.OpenPreparedSource(
                   source_path,
                   0,
                   initial_snapshot,
                   std::move(context),
                   std::move(prepared))
            .loaded,
        "deferred local label filter fixture should load");

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(
            session,
            UpsertActiveLabel(
                spectiary::SampleLabelDefinition{1, "accepted", 'a'}))
            .changed,
        "deferred local label filter fixture should add its label");
    (void)Submit(session, SetActiveLabelingOutputPath(output_path));

    const std::string filter_source_id =
        "labeling:" + session.View().labeling.task_id;
    (void)Submit(session, AddSampleFilterSource(filter_source_id));
    const spectiary::SourceCollectionSessionResult filtered =
        Submit(
            session,
            SetFilterValueSelected(
                filter_source_id,
                std::to_string(
                    spectiary::kUnlabeledSampleLabelCode),
                true));
    Require(
        !filtered.follow_up_spectrum_index &&
            session.View().navigation.sequence_count == 3,
        "unlabeled filter should initially retain visible row 0");

    (void)Submit(session, SetActiveLabelingAutoAdvance(true));
    const spectiary::SourceCollectionSessionResult labeled =
        Submit(session, AssignActiveLabelToCurrentSample(1));
    Require(
        labeled.follow_up_spectrum_index == 1,
        "label filtering should preserve the newly required row 1 follow-up when auto-advance reuses it");
    Require(
        session.View().current_sample_snapshot == initial_snapshot &&
            session.View().labeling.current_index == 0 &&
            session.View().labeling.current_code == 1,
        "the committed row 0 presentation should remain complete while filtered row 1 loads");

    const spectiary::SpectrumSnapshotHandle next_snapshot =
        MakeSnapshot(source_path, 3, 1);
    Require(
        session.OpenPreparedSource(
                   source_path,
                   1,
                   next_snapshot,
                   spectiary::PreparedSourceCollectionReuse{
                       identity})
            .loaded,
        "the preserved row 1 follow-up should commit");
    Require(
        session.View().navigation.current_index == 1 &&
            session.View().current_sample_snapshot == next_snapshot,
        "the complete presentation should land on filtered row 1");
}

void TestDeferredLabelUndoClearsSupersededLocalFilterFollowUp()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_deferred_local_label_undo_filter.npy");
    const std::filesystem::path output_path =
        UniqueTempPath("_deferred_local_label_undo_filter_result.asdf");
    spectiary::SourceCollectionSession session({}, {}, {}, {});

    const spectiary::SpectrumSnapshotHandle row_zero_snapshot =
        MakeSnapshot(source_path, 3, 0);
    spectiary::SourceCollectionContext context;
    context.identity = {
        "deferred-local-label-undo-filter",
        "source",
        "source-fingerprint",
        "context",
        3,
    };
    context.manifest.sample_names = {"alpha", "beta", "gamma"};
    const spectiary::SourceCollectionIdentity identity = context.identity;
    spectiary::PreparedSampleWorkflowState prepared =
        PrepareWorkflow(row_zero_snapshot, context, 0, {}, {});
    Require(
        session.OpenPreparedSource(
                   source_path,
                   0,
                   row_zero_snapshot,
                   std::move(context),
                   std::move(prepared))
            .loaded,
        "deferred local label undo filter fixture should load");

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(
            session,
            UpsertActiveLabel(
                spectiary::SampleLabelDefinition{1, "accepted", 'a'}))
            .changed,
        "deferred local label undo filter fixture should add its label");

    Require(
        Submit(
            session,
            MoveSampleNavigation(
                spectiary::SampleNavigationRequest::LocateRow(1)))
                .follow_up_spectrum_index == 1,
        "undo fixture should request row 1");
    Require(
        session.OpenPreparedSource(
                   source_path,
                   1,
                   MakeSnapshot(source_path, 3, 1),
                   spectiary::PreparedSourceCollectionReuse{
                       identity})
            .loaded,
        "undo fixture should commit row 1");
    (void)Submit(session, AssignActiveLabelToCurrentSample(1));

    Require(
        Submit(
            session,
            MoveSampleNavigation(
                spectiary::SampleNavigationRequest::LocateRow(0)))
                .follow_up_spectrum_index == 0,
        "undo fixture should request row 0");
    Require(
        session.OpenPreparedSource(
                   source_path,
                   0,
                   row_zero_snapshot,
                   spectiary::PreparedSourceCollectionReuse{
                       identity})
            .loaded,
        "undo fixture should commit row 0");
    (void)Submit(session, AssignActiveLabelToCurrentSample(1));
    (void)Submit(session, SetActiveLabelingOutputPath(output_path));

    const std::string filter_source_id =
        "labeling:" + session.View().labeling.task_id;
    (void)Submit(session, AddSampleFilterSource(filter_source_id));
    const spectiary::SourceCollectionSessionResult filtered =
        Submit(
            session,
            SetFilterValueSelected(
                filter_source_id,
                "1",
                true));
    Require(
        !filtered.follow_up_spectrum_index &&
            session.View().navigation.sequence_count == 2,
        "label-one filter should initially retain visible row 0");

    const spectiary::SourceCollectionSessionResult undone =
        Submit(session, UndoLastLabelWrite());
    Require(
        !undone.follow_up_spectrum_index,
        "undo restore to the visible row should clear the superseded filter follow-up");
    Require(
        session.View().current_sample_snapshot ==
                row_zero_snapshot &&
            session.View().labeling.current_index == 0 &&
            session.View().labeling.current_code ==
                spectiary::kUnlabeledSampleLabelCode &&
            !session.View().navigation.current_sample_in_filter,
        "undo should keep the complete restored row 0 presentation outside the active filter");
    Require(
        !session.CancelActivePendingSampleNavigation(),
        "undo restore to the visible row should leave no pending navigation");
}

void TestPreparedPlanReconciliationKeepsPreviousCompletePresentationUntilFinalRow()
{
    const std::filesystem::path source_path = UniqueTempPath("_deferred_plan_reconcile.npy");
    spectiary::SourceCollectionSession session({}, {}, {}, {});

    const spectiary::SpectrumSnapshotHandle initial_snapshot = MakeSnapshot(source_path, 3, 0);
    spectiary::SourceCollectionContext initial_context;
    initial_context.identity = {
        "deferred-plan-reconcile",
        "source",
        "source-fingerprint",
        "initial-context",
        3,
    };
    initial_context.manifest.sample_names = {"alpha", "beta", "gamma"};
    spectiary::PreparedSampleWorkflowState initial_workflow =
        PrepareWorkflow(initial_snapshot, initial_context, 0, {}, {});
    Require(
        session.OpenPreparedSource(
                   source_path,
                   0,
                   initial_snapshot,
                   std::move(initial_context),
                   std::move(initial_workflow))
            .loaded,
        "initial prepared source should commit");
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    const std::optional<std::size_t> initial_remembered_position =
        session.View().labeling.remembered_position;
    Require(
        Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 1,
        "next should initially request row 1");

    const spectiary::SpectrumSnapshotHandle intermediate_snapshot = MakeSnapshot(source_path, 3, 1);
    spectiary::SourceCollectionContext changed_context;
    changed_context.identity = {
        "deferred-plan-reconcile",
        "source",
        "source-fingerprint",
        "changed-context",
        3,
    };
    changed_context.manifest.sample_names = {"alpha", "beta", "gamma"};
    spectiary::PreparedSampleWorkflowState reconciled_workflow =
        PrepareWorkflow(intermediate_snapshot, changed_context, 1, {}, {});
    reconciled_workflow.current_index = 2;
    reconciled_workflow.navigation_sequence.current_source_row = 2;

    const spectiary::SourceCollectionSessionResult reconciled = session.OpenPreparedSource(
        source_path,
        1,
        intermediate_snapshot,
        std::move(changed_context),
        std::move(reconciled_workflow));
    const spectiary::SourceCollectionSessionView pending_view = session.View();
    Require(reconciled.loaded, "the intermediate prepared plan should be accepted");
    Require(
        reconciled.follow_up_spectrum_index == 2,
        "the reconciled plan should request only its final row");
    Require(
        session.CurrentSourceSnapshot() == initial_snapshot &&
            session.CurrentSampleSnapshot() == initial_snapshot,
        "plan reconciliation must retain the previous complete snapshot until the final row arrives");
    Require(
        pending_view.navigation.current_index == 0 &&
            pending_view.labeling.current_index == 0,
        "plan reconciliation must retain the previous complete navigation and labeling presentation");
    Require(
        pending_view.labeling.remembered_position == initial_remembered_position,
        "an intermediate plan must not commit the pending labeling position");

    Require(
        session.CancelPendingSampleNavigation(source_path, 2),
        "failure of the reconciled final row should cancel the retargeted navigation");
    Require(
        session.CurrentSourceSnapshot() == initial_snapshot &&
            session.CurrentSampleSnapshot() == initial_snapshot &&
            session.View().navigation.current_index == 0,
        "failure of the reconciled final row must leave the old complete presentation recoverable");
    Require(
        session.View().labeling.remembered_position == initial_remembered_position,
        "failed plan reconciliation must not leak a labeling-position commit");

    Require(
        Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 1,
        "navigation should remain usable after the reconciled final row fails");
    const spectiary::SpectrumSnapshotHandle second_intermediate_snapshot =
        MakeSnapshot(source_path, 3, 1);
    spectiary::SourceCollectionContext second_changed_context;
    second_changed_context.identity = {
        "deferred-plan-reconcile",
        "source",
        "source-fingerprint",
        "changed-context",
        3,
    };
    second_changed_context.manifest.sample_names = {"alpha", "beta", "gamma"};
    spectiary::PreparedSampleWorkflowState second_reconciled_workflow =
        PrepareWorkflow(second_intermediate_snapshot, second_changed_context, 1, {}, {});
    second_reconciled_workflow.current_index = 2;
    second_reconciled_workflow.navigation_sequence.current_source_row = 2;
    Require(
        session.OpenPreparedSource(
                   source_path,
                   1,
                   second_intermediate_snapshot,
                   std::move(second_changed_context),
                   std::move(second_reconciled_workflow))
                .follow_up_spectrum_index == 2,
        "a retried intermediate plan should again request its reconciled final row");

    const spectiary::SpectrumSnapshotHandle final_snapshot = MakeSnapshot(source_path, 3, 2);
    spectiary::SourceCollectionContext final_context;
    final_context.identity = {
        "deferred-plan-reconcile",
        "source",
        "source-fingerprint",
        "changed-context",
        3,
    };
    final_context.manifest.sample_names = {"alpha", "beta", "gamma"};
    spectiary::PreparedSampleWorkflowState final_workflow =
        PrepareWorkflow(final_snapshot, final_context, 2, {}, {});
    const spectiary::SourceCollectionSessionResult final_result = session.OpenPreparedSource(
        source_path,
        2,
        final_snapshot,
        std::move(final_context),
        std::move(final_workflow));
    const spectiary::SourceCollectionSessionView final_view = session.View();
    Require(final_result.loaded && !final_result.follow_up_spectrum_index, "the final row should commit once");
    Require(
        final_view.snapshot == final_snapshot &&
            final_view.current_sample_snapshot == final_snapshot &&
            final_view.navigation.current_index == 2 &&
            final_view.labeling.current_index == 2,
        "the final snapshot, navigation, and labeling panels should publish together");
    Require(
        final_view.labeling.remembered_position == 2,
        "the source-bound pending navigation should remember labeling position only at final commit");
}

void TestPreparedPlanPreservesNewerLiveWorkflowWhenPendingTargetIsUnchanged()
{
    const std::filesystem::path source_path = UniqueTempPath("_stale_prepared_plan.npy");
    spectiary::SourceCollectionSession session({}, {}, {}, {});

    const spectiary::SpectrumSnapshotHandle initial_snapshot = MakeSnapshot(source_path, 3, 0);
    spectiary::SourceCollectionContext initial_context;
    initial_context.identity = {
        "stale-prepared-plan",
        "source",
        "stable-source-fingerprint",
        "context-v1",
        3,
    };
    initial_context.manifest.sample_names = {"alpha", "beta", "gamma"};
    spectiary::PreparedSampleWorkflowState initial_workflow =
        PrepareWorkflow(initial_snapshot, initial_context, 0, {}, {});
    Require(
        session.OpenPreparedSource(
                   source_path,
                   0,
                   initial_snapshot,
                   std::move(initial_context),
                   std::move(initial_workflow))
            .loaded,
        "stale prepared plan fixture should commit its initial source");
    Require(
        Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 1,
        "stale prepared plan fixture should queue row 1");
    const std::optional<spectiary::SourceCollectionLoadHint> load_hint =
        session.LoadHintForSource(source_path);
    Require(load_hint.has_value(), "known source navigation should expose its workflow revision");

    const spectiary::SpectrumSnapshotHandle prepared_snapshot = MakeSnapshot(source_path, 3, 1);
    spectiary::SourceCollectionContext changed_context;
    changed_context.identity = {
        "stale-prepared-plan",
        "source",
        "stable-source-fingerprint",
        "context-v2",
        3,
    };
    changed_context.manifest.sample_names = {"alpha", "beta", "gamma"};
    spectiary::PreparedSampleWorkflowState stale_workflow =
        PrepareWorkflow(prepared_snapshot, changed_context, 1, {}, {});

    (void)Submit(session, AddSampleSortSource("sample-name"));
    const spectiary::SourceCollectionSessionResult sorted =
        Submit(session, SetSampleSortSource("sample-name"));
    Require(
        !sorted.follow_up_spectrum_index && session.View().sorting.active,
        "new live sorting should retain the existing row 1 worker");

    const spectiary::SourceCollectionSessionResult committed = session.OpenPreparedSource(
        source_path,
        1,
        prepared_snapshot,
        spectiary::PreparedSourceCollectionPlan{
            std::move(changed_context),
            std::move(stale_workflow),
            load_hint->reuse.live_workflow_revision()});
    const spectiary::SourceCollectionSessionView view = session.View();
    Require(committed.loaded && !committed.follow_up_spectrum_index, "row 1 should commit once");
    Require(
        view.current_sample_snapshot == prepared_snapshot &&
            view.navigation.current_index == 1,
        "the prepared snapshot and pending row should commit atomically");
    Require(
        view.sorting.active && view.sorting.active_source_id == "sample-name",
        "an older full plan must not overwrite newer live sorting while its worker waits");
}

void TestLiveWorkflowContextReconciliationKeepsOldSnapshotWhenTargetChanges()
{
    const std::filesystem::path source_path = UniqueTempPath("_live_context_reconcile.npy");
    const std::filesystem::path annotation_path =
        UniqueTempPath("_live_context_reconcile_filter.npy");
    SaveLabelResultFixture(
        annotation_path,
        "live-context-values",
        "Live context values",
        {1, 1, 0},
        spectiary::SampleLabelSet{},
        false);
    spectiary::SourceCollectionSession session({}, {}, {}, {});

    const spectiary::SpectrumSnapshotHandle initial_snapshot = MakeSnapshot(source_path, 3, 0);
    spectiary::SourceCollectionContext initial_context;
    initial_context.identity = {
        "live-context-reconcile",
        "source",
        "stable-source-fingerprint",
        "context-v1",
        3,
    };
    initial_context.manifest.sample_names = {"alpha", "beta", "gamma"};
    std::string annotation_error;
    std::optional<spectiary::SampleAnnotationResult> initial_annotation =
        spectiary::test_support::LegacyFixtureIo{}.Load(annotation_path, 3, &annotation_error);
    Require(initial_annotation.has_value(), "initial context annotation should load");
    initial_context.manifest.annotations.push_back(std::move(*initial_annotation));
    spectiary::PreparedSampleWorkflowState initial_workflow =
        PrepareWorkflow(initial_snapshot, initial_context, 0, {}, {});
    Require(
        session.OpenPreparedSource(
                   source_path,
                   0,
                   initial_snapshot,
                   std::move(initial_context),
                   std::move(initial_workflow))
            .loaded,
        "live context fixture should commit its initial source");
    const std::string filter_source_id = AnnotationSourceId(annotation_path);
    (void)Submit(session, AddSampleFilterSource(filter_source_id));
    (void)Submit(session, SetFilterValueSelected(filter_source_id, "1", true));
    Require(
        Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 1,
        "the original context should queue row 1");
    const spectiary::SourceCollectionSessionView presentation_before_context_change =
        session.View();
    Require(
        presentation_before_context_change.filter.evaluation.included_count == 2 &&
            presentation_before_context_change.navigation.sequence_count == 2 &&
            presentation_before_context_change.navigation.current_sample_in_filter,
        "the committed presentation should still expose the original context projections");
    const std::optional<spectiary::SourceCollectionLoadHint> load_hint =
        session.LoadHintForSource(source_path);
    Require(load_hint.has_value(), "context reconciliation should capture a live revision");

    SaveLabelResultFixture(
        annotation_path,
        "live-context-values",
        "Live context values",
        {0, 0, 1},
        spectiary::SampleLabelSet{},
        false);
    const spectiary::SpectrumSnapshotHandle intermediate_snapshot =
        MakeSnapshot(source_path, 3, 1);
    spectiary::SourceCollectionContext changed_context;
    changed_context.identity = {
        "live-context-reconcile",
        "source",
        "stable-source-fingerprint",
        "context-v2",
        3,
    };
    const spectiary::SourceCollectionIdentity changed_identity = changed_context.identity;
    changed_context.manifest.sample_names = {"alpha", "beta", "gamma"};
    std::optional<spectiary::SampleAnnotationResult> changed_annotation =
        spectiary::test_support::LegacyFixtureIo{}.Load(annotation_path, 3, &annotation_error);
    Require(changed_annotation.has_value(), "changed context annotation should load");
    changed_context.manifest.annotations.push_back(std::move(*changed_annotation));
    spectiary::PreparedSampleWorkflowState stale_workflow =
        PrepareWorkflow(intermediate_snapshot, changed_context, 1, {}, {});

    const spectiary::SourceCollectionSessionResult reconciled = session.OpenPreparedSource(
        source_path,
        1,
        intermediate_snapshot,
        spectiary::PreparedSourceCollectionPlan{
            std::move(changed_context),
            std::move(stale_workflow),
            load_hint->reuse.live_workflow_revision()});
    Require(
        reconciled.loaded && reconciled.follow_up_spectrum_index == 2,
        "the live filter should retarget the changed context to row 2");
    const spectiary::SourceCollectionSessionView presentation_while_waiting = session.View();
    Require(
        session.CurrentSourceSnapshot() == initial_snapshot &&
            session.CurrentSampleSnapshot() == initial_snapshot &&
            presentation_while_waiting.navigation.current_index == 0,
        "context reconciliation must keep the old complete row until final row 2 is prepared");
    Require(
        presentation_while_waiting.filter.evaluation.included_count == 2 &&
            presentation_while_waiting.navigation.sequence_count == 2 &&
            presentation_while_waiting.navigation.current_sample_in_filter,
        "the old manifest, filter, and navigation projections must remain committed while final row 2 loads");

    const spectiary::SpectrumSnapshotHandle final_snapshot = MakeSnapshot(source_path, 3, 2);
    spectiary::SourceCollectionContext final_context;
    final_context.identity = changed_identity;
    final_context.manifest.sample_names = {"alpha", "beta", "gamma"};
    std::optional<spectiary::SampleAnnotationResult> final_annotation =
        spectiary::test_support::LegacyFixtureIo{}.Load(annotation_path, 3, &annotation_error);
    Require(final_annotation.has_value(), "final context annotation should load");
    final_context.manifest.annotations.push_back(std::move(*final_annotation));
    spectiary::PreparedSampleWorkflowState final_workflow =
        PrepareWorkflow(final_snapshot, final_context, 2, {}, {});
    Require(
        session.OpenPreparedSource(
                   source_path,
                   2,
                   final_snapshot,
                   spectiary::PreparedSourceCollectionPlan{
                       std::move(final_context),
                       std::move(final_workflow),
                       load_hint->reuse.live_workflow_revision()})
            .loaded,
        "the reconciled final row should atomically commit its full context plan");
    Require(
        session.CurrentSampleSnapshot() == final_snapshot &&
            session.View().navigation.current_index == 2,
        "the changed context should publish only with its final complete row");
    std::filesystem::remove(annotation_path);
}

void TestSwitchingAwayCancelsSourceBoundDeferredNavigation()
{
    const std::filesystem::path source_a = UniqueTempPath("_deferred_switch_a.npy");
    const std::filesystem::path source_b = UniqueTempPath("_deferred_switch_b.npy");
    spectiary::SourceCollectionSession session({}, {}, {}, {});

    auto open_prepared = [&session](
                             const std::filesystem::path& path,
                             std::string identity,
                             const spectiary::SpectrumSnapshotHandle& snapshot) {
        spectiary::SourceCollectionContext context;
        context.identity = {
            std::move(identity),
            "source",
            path.string(),
            path.string() + "-context",
            3,
        };
        context.manifest.sample_names = {"alpha", "beta", "gamma"};
        spectiary::PreparedSampleWorkflowState workflow =
            PrepareWorkflow(snapshot, context, 0, {}, {});
        return session.OpenPreparedSource(
            path,
            0,
            snapshot,
            std::move(context),
            std::move(workflow));
    };

    const spectiary::SpectrumSnapshotHandle snapshot_a = MakeSnapshot(source_a, 3, 0);
    const spectiary::SpectrumSnapshotHandle snapshot_b = MakeSnapshot(source_b, 3, 0);
    Require(open_prepared(source_a, "deferred-switch-a", snapshot_a).loaded, "source A should load");
    Require(open_prepared(source_b, "deferred-switch-b", snapshot_b).loaded, "source B should load");
    (void)Submit(session, SwitchSourceCollection(0));
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    const std::optional<std::size_t> remembered_a = session.View().labeling.remembered_position;
    Require(
        Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 1,
        "source A should queue row 1");

    const spectiary::SourceCollectionSessionResult switched_to_b =
        Submit(session, SwitchSourceCollection(1));
    Require(
        switched_to_b.canceled_source_follow_up_path == source_a,
        "switching to B should cancel A's pending source-follow-up ticket");
    const spectiary::SourceCollectionSessionResult switched_back_to_a =
        Submit(session, SwitchSourceCollection(0));
    const spectiary::SourceCollectionSessionView restored_a = session.View();
    Require(
        !switched_back_to_a.follow_up_spectrum_index,
        "switching back to A must not resurrect its canceled row 1 navigation");
    Require(
        restored_a.snapshot == snapshot_a &&
            restored_a.current_sample_snapshot == snapshot_a &&
            restored_a.navigation.current_index == 0,
        "switching back to A should restore its last committed row 0 presentation");
    Require(
        restored_a.labeling.remembered_position == remembered_a,
        "canceled navigation metadata must not leak across source switches");
}

void TestNonActiveRemovalAndCurrentReselectionPreserveDeferredNavigation()
{
    const std::filesystem::path source_a = UniqueTempPath("_preserve_pending_a.npy");
    const std::filesystem::path source_b = UniqueTempPath("_preserve_pending_b.npy");
    spectiary::SourceCollectionSession session({}, {}, {}, {});

    const spectiary::SpectrumSnapshotHandle snapshot_a = MakeSnapshot(source_a, 3, 0);
    spectiary::SourceCollectionContext context_a;
    context_a.identity = {"preserve-pending-a", "a", "a-source", "a-context", 3};
    context_a.manifest.sample_names = {"a0", "a1", "a2"};
    const spectiary::SourceCollectionIdentity identity_a = context_a.identity;
    spectiary::PreparedSampleWorkflowState workflow_a =
        PrepareWorkflow(snapshot_a, context_a, 0, {}, {});
    Require(
        session.OpenPreparedSource(
                   source_a,
                   0,
                   snapshot_a,
                   std::move(context_a),
                   std::move(workflow_a))
            .loaded,
        "source A should load");

    const spectiary::SpectrumSnapshotHandle snapshot_b = MakeSnapshot(source_b, 3, 0);
    spectiary::SourceCollectionContext context_b;
    context_b.identity = {"preserve-pending-b", "b", "b-source", "b-context", 3};
    context_b.manifest.sample_names = {"b0", "b1", "b2"};
    spectiary::PreparedSampleWorkflowState workflow_b =
        PrepareWorkflow(snapshot_b, context_b, 0, {}, {});
    Require(
        session.OpenPreparedSource(
                   source_b,
                   0,
                   snapshot_b,
                   std::move(context_b),
                   std::move(workflow_b))
            .loaded,
        "source B should load");
    (void)Submit(session, SwitchSourceCollection(0));
    Require(
        Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 1,
        "source A should queue row 1");

    const spectiary::SourceCollectionSessionResult removed_b =
        Submit(session, RemoveSourceCollection(1));
    Require(
        removed_b.canceled_source_follow_up_path == source_b &&
            !removed_b.follow_up_spectrum_index,
        "removing inactive source B should cancel only B's tickets and retain source A's pending ticket");
    const spectiary::SourceCollectionSessionResult reselected_a =
        Submit(session, SwitchSourceCollection(0));
    Require(
        !reselected_a.canceled_source_follow_up_path && !reselected_a.follow_up_spectrum_index,
        "reselecting active source A should retain its pending ticket");

    const spectiary::SpectrumSnapshotHandle row_one_snapshot = MakeSnapshot(source_a, 3, 1);
    Require(
        session.OpenPreparedSource(
                   source_a,
                   1,
                   row_one_snapshot,
                   spectiary::PreparedSourceCollectionReuse{identity_a})
            .loaded,
        "the retained source A ticket should still commit row 1");
    Require(
        session.CurrentSampleSnapshot() == row_one_snapshot &&
            session.View().navigation.current_index == 1,
        "non-active removal and current reselection must preserve source A navigation");
}

void TestSameIdentityPreparedReloadPreservesLiveWorkflowAndCurrentRow()
{
    const std::filesystem::path source_path = UniqueTempPath("_same_identity_reload.npy");
    const std::filesystem::path other_source_path = UniqueTempPath("_same_identity_reload_other.npy");
    const std::filesystem::path annotation_path = UniqueTempPath("_same_identity_reload_annotation.npy");
    SaveLabelResultFixture(
        annotation_path,
        "reload-values",
        "Reload values",
        {1, 2, 3},
        spectiary::SampleLabelSet{},
        false);
    std::string annotation_error;
    std::optional<spectiary::SampleAnnotationResult> annotation =
        spectiary::test_support::LegacyFixtureIo{}.Load(annotation_path, 3, &annotation_error);
    Require(annotation.has_value(), "same-identity reload annotation fixture should load");

    spectiary::SourceCollectionSession session({}, {}, {}, {});
    const spectiary::SpectrumSnapshotHandle initial_snapshot = MakeSnapshot(source_path, 3, 0);
    spectiary::SourceCollectionContext context;
    context.identity = spectiary::SourceCollectionIdentity{
        .id = "same-identity-reload",
        .source_name = "reload",
        .source_fingerprint = "same-source",
        .context_fingerprint = "same-context",
        .spectrum_count = 3,
    };
    context.manifest.sample_names = {"a", "b", "c"};
    context.manifest.annotations.push_back(std::move(*annotation));
    const spectiary::SourceCollectionIdentity identity = context.identity;
    spectiary::PreparedSampleWorkflowState prepared =
        PrepareWorkflow(initial_snapshot, context, 0, {}, {});
    Require(
        session.OpenPreparedSource(
                   source_path,
                   0,
                   initial_snapshot,
                   std::move(context),
                   std::move(prepared))
            .loaded,
        "initial prepared source should load");

    const std::string annotation_source_id = AnnotationSourceId(annotation_path);
    (void)Submit(session, AddSampleFilterSource(annotation_source_id));
    const spectiary::SourceCollectionSessionResult filter_result =
        Submit(session, SetFilterValueSelected(annotation_source_id, "2", true));
    Require(filter_result.follow_up_spectrum_index == 1, "live filter should move navigation to row 1");
    Require(
        session.OpenPreparedSource(
                   source_path,
                   1,
                   MakeSnapshot(source_path, 3, 1),
                   spectiary::PreparedSourceCollectionReuse{identity})
            .loaded,
        "the filtered row should commit only after its complete snapshot is prepared");
    (void)Submit(session, AddSampleSortSource(annotation_source_id));
    (void)Submit(session, SetSampleSortSource(annotation_source_id));
    (void)Submit(
        session,
        SetSampleSortDirection(spectiary::SampleNavigationSortDirection::Descending));
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(spectiary::SampleLabelDefinition{7, "live", 'l'})).changed,
        "live non-active labeling fixture should accept its label");
    (void)Submit(session, AssignActiveLabelToCurrentSample(7));

    const spectiary::SpectrumSnapshotHandle other_snapshot = MakeSnapshot(other_source_path, 3, 0);
    spectiary::SourceCollectionContext other_context;
    other_context.identity = {"same-identity-other", "other", "other-source", "other-context", 3};
    other_context.manifest.sample_names = {"x", "y", "z"};
    spectiary::PreparedSampleWorkflowState other_workflow =
        PrepareWorkflow(other_snapshot, other_context, 0, {}, {});
    (void)session.OpenPreparedSource(
        other_source_path,
        0,
        other_snapshot,
        std::move(other_context),
        std::move(other_workflow));

    const std::optional<spectiary::SourceCollectionLoadHint> hint =
        session.LoadHintForSource(source_path);
    Require(hint && hint->spectrum_index == 1, "non-active source reload should capture its live row");
    Require(
        hint->reuse.identity().id == identity.id,
        "non-active source reload should expose its stable generation");

    const spectiary::SpectrumSnapshotHandle reloaded_snapshot = MakeSnapshot(source_path, 3, 1);
    const spectiary::SourceCollectionSessionResult reload_result = session.OpenPreparedSource(
        source_path,
        1,
        reloaded_snapshot,
        spectiary::PreparedSourceCollectionReuse{identity});
    const spectiary::SourceCollectionSessionView view = session.View();
    Require(reload_result.loaded, "matching same-identity snapshot-only reload should commit");
    Require(
        reload_result.background_retirement.size() >= 1,
        "same-identity non-active reload should hand replaced snapshots to the background reclaimer");
    Require(!reload_result.follow_up_spectrum_index, "preserved row should already match the reloaded snapshot");
    Require(view.navigation.current_index == 1, "same-identity reload should preserve the live row");
    Require(view.navigation.filter_active, "same-identity reload should preserve the live filter");
    Require(view.sorting.active, "same-identity reload should preserve the live sort");
}

void TestPreparedCacheSnapshotPreventsUiCacheReload()
{
    std::size_t labeling_cache_loads = 0;
    std::size_t workflow_cache_loads = 0;
    spectiary::SampleWorkflowCoordinator coordinator(
        {},
        {},
        {},
        [&labeling_cache_loads](const std::filesystem::path&) {
            ++labeling_cache_loads;
            return spectiary::SampleLabelingStateCacheLoadResult{};
        },
        [&workflow_cache_loads](const std::filesystem::path&) {
            ++workflow_cache_loads;
            return spectiary::SampleWorkflowStateCacheLoadResult{};
        });

    const std::filesystem::path source_a = UniqueTempPath("_prepared_cache_a.npy");
    const std::filesystem::path source_b = UniqueTempPath("_prepared_cache_b.npy");
    const spectiary::SpectrumSnapshotHandle snapshot_a = MakeSnapshot(source_a, 3, 0);
    const spectiary::SpectrumSnapshotHandle snapshot_b = MakeSnapshot(source_b, 3, 0);
    spectiary::SourceCollectionContext context_a;
    context_a.identity = {"prepared-cache-a", "a", "a-source", "a-context", 3};
    context_a.manifest.sample_names = {"a0", "a1", "a2"};
    spectiary::SourceCollectionContext context_b;
    context_b.identity = {"prepared-cache-b", "b", "b-source", "b-context", 3};
    context_b.manifest.sample_names = {"b0", "b1", "b2"};

    auto cache = std::make_shared<spectiary::SampleWorkflowPreparationCacheBundle>();
    spectiary::SampleLabelingSourceState labeling_a;
    labeling_a.sample_count = 3;
    labeling_a.source_name = "a";
    cache->labeling.cache.sources.emplace(context_a.identity.id, labeling_a);
    spectiary::SampleWorkflowSourceState workflow_a_state;
    workflow_a_state.annotation_display_names.push_back({"annotation:cache", "Cached name"});
    cache->workflow.sources_by_identity.emplace(context_a.identity.id, std::move(workflow_a_state));

    spectiary::PreparedSampleWorkflowState prepared_a =
        spectiary::PrepareSampleWorkflowStateFromCache(*snapshot_a, context_a, 0, *cache);
    prepared_a.preparation_cache = cache;
    spectiary::PreparedSampleWorkflowActivationResult activation_a =
        coordinator.SyncPreparedActiveSource("prepared-cache-a-key", snapshot_a, context_a, std::move(prepared_a));

    spectiary::PreparedSampleWorkflowState prepared_b =
        spectiary::PrepareSampleWorkflowStateFromCache(*snapshot_b, context_b, 0, *cache);
    prepared_b.preparation_cache = cache;
    spectiary::PreparedSampleWorkflowActivationResult activation_b =
        coordinator.SyncPreparedActiveSource("prepared-cache-b-key", snapshot_b, context_b, std::move(prepared_b));
    (void)activation_a;
    (void)activation_b;

    const std::optional<spectiary::SampleWorkflowSourceState> workflow_a =
        coordinator.WorkflowStateForSourceIdentity(context_a.identity.id);
    const std::optional<spectiary::SampleLabelingSourceState> restored_labeling_a =
        coordinator.LabelingStateForSourceIdentity(context_a.identity.id);
    Require(workflow_a.has_value(), "prepared workflow cache state should remain available in memory");
    Require(
        restored_labeling_a && restored_labeling_a->sample_count == 3,
        "prepared labeling cache state should remain available in memory");
    Require(
        workflow_cache_loads == 0 && labeling_cache_loads == 0,
        "prepared commits and non-active state hints must not reopen either cache on the UI thread");
}

void TestKnownSourceSyncReusesTheLabelingSourceGeneration()
{
    spectiary::SampleWorkflowCoordinator coordinator({}, {}, {});
    const std::filesystem::path source_path =
        UniqueTempPath("_known_source_generation.npy");
    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(source_path, 3, 0);
    spectiary::SourceCollectionContext context;
    context.identity = {
        "known-source-generation",
        "known source",
        "known-source-fingerprint",
        "known-context-generation",
        3};
    context.manifest.sample_names = {"a", "b", "c"};
    spectiary::PreparedSampleWorkflowState prepared =
        PrepareWorkflow(snapshot, context, 0, {}, {});
    Require(
        coordinator
            .SyncPreparedActiveSource(
                "known-source-key",
                snapshot,
                context,
                std::move(prepared))
            .action.workflow_changed,
        "known-source generation fixture should activate");

    const spectiary::SampleWorkflowTransitionOutcome reused =
        coordinator.SyncKnownActiveSource(
            "known-source-key",
            snapshot);
    Require(
        !reused.invalidate_view &&
            !reused.action.workflow_changed,
        "same-generation known-source sync should reuse the labeling descriptor without reactivating the controller");
}

void TestPreparedProjectionsMoveIntoTheSessionView()
{
    const std::filesystem::path source_path = UniqueTempPath("_prepared_projection.npy");
    spectiary::SourceCollectionSession session({}, {}, {}, {});
    const spectiary::SpectrumSnapshotHandle snapshot = MakeSnapshot(source_path, 3, 0);
    spectiary::SourceCollectionContext context;
    context.identity = {"prepared-projection", "projection", "source", "context", 3};
    context.manifest.sample_names = {"a", "b", "c"};
    spectiary::PreparedSampleWorkflowState prepared =
        PrepareWorkflow(snapshot, context, 0, {}, {});
    prepared.filter_view.sources.push_back(
        spectiary::SourceCollectionFilterSourceView{.id = "filter-source", .name = "Filter source"});
    prepared.sorting_view.sources.push_back(
        spectiary::SourceCollectionSampleSortSourceView{.id = "sort-source", .name = "Sort source"});
    const auto* prepared_filter_storage = prepared.filter_view.sources.data();
    const auto* prepared_sorting_storage = prepared.sorting_view.sources.data();

    const spectiary::SourceCollectionSessionResult result = session.OpenPreparedSource(
        source_path,
        0,
        snapshot,
        std::move(context),
        std::move(prepared));
    Require(result.loaded, "prepared projection fixture should load");
    const spectiary::SourceCollectionSessionView& view = session.View();
    Require(
        view.filter.sources.data() == prepared_filter_storage,
        "prepared filter projection should move into the UI session view");
    Require(
        view.sorting.sources.data() == prepared_sorting_storage,
        "prepared sorting projection should move into the UI session view");
    const spectiary::SourceCollectionSessionView& repeated_view =
        session.View();
    Require(
        &repeated_view == &view &&
            repeated_view.filter.sources.data() ==
                prepared_filter_storage &&
            repeated_view.sorting.sources.data() ==
                prepared_sorting_storage,
        "repeated reads should retain the stable prepared projection");
}

void TestSessionOwnsStableViewInvalidationAndRetirement()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_session_view_revision.npy");
    spectiary::SourceCollectionSession session(
        UniqueTempPath("_session_view_sources.json"),
        UniqueTempPath("_session_view_navigation.json"),
        UniqueTempPath("_session_view_labeling.json"),
        UniqueTempPath("_session_view_workflow.json"));

    const spectiary::SourceCollectionSessionView& empty_view =
        session.View();
    Require(
        &session.View() == &empty_view,
        "unchanged session reads should reuse one projection");

    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(source_path, 3, 0);
    spectiary::SourceCollectionContext context;
    context.identity = {
        "session-view-revision",
        "revision",
        "source",
        "context",
        3};
    context.manifest.sample_names = {"a", "b", "c"};
    spectiary::PreparedSampleWorkflowState prepared =
        PrepareWorkflow(snapshot, context, 0, {}, {});
    const spectiary::SourceCollectionSessionResult load_result =
        session.OpenPreparedSource(
            source_path,
            0,
            snapshot,
            std::move(context),
            std::move(prepared));
    Require(
        load_result.loaded,
        "load completion fixture should load");
    Require(
        empty_view.sources.empty(),
        "session should retain the stale projection until its reader frame ends");
    const spectiary::SourceCollectionSessionView& loaded_view =
        session.View();
    Require(
        &loaded_view != &empty_view,
        "load completion should invalidate the projection exactly once");
    Require(
        &session.View() == &loaded_view,
        "repeated loaded-session reads should not rebuild");
    std::vector<spectiary::BackgroundRetirementHandle>
        retained_view_generations =
            session.TakeViewRetirement();
    Require(
        retained_view_generations.size() == 1,
        "load completion should expose one stale projection for end-of-frame retirement");

    const spectiary::SourceCollectionSessionResult command_result =
        session.Submit(
            spectiary::SourceCollectionSessionIntent::
                UpdateSampleNavigation(
                    spectiary::SampleNavigationIntent::
                        SetSampleNameQuery("b")));
    Require(
        command_result.action.navigation_inputs_changed,
        "sample-name query should report a projection mutation");
    Require(
        command_result.view_invalidated,
        "a mutating session transition should report view invalidation");
    Require(
        loaded_view.navigation.exact_sample_name.empty(),
        "session should retain the prior command projection until the frame ends");
    const spectiary::SourceCollectionSessionView& command_view =
        session.View();
    Require(
        &command_view != &loaded_view &&
            command_view.navigation.exact_sample_name_match &&
            *command_view.navigation.exact_sample_name_match == 1,
        "session command should invalidate once and expose its new state");
    Require(
        &session.View() == &command_view,
        "repeated command projection reads should not rebuild");
    std::vector<spectiary::BackgroundRetirementHandle>
        command_retirement =
            session.TakeViewRetirement();
    Require(
        command_retirement.size() == 1,
        "one mutating command should retire one projection generation");
    retained_view_generations.insert(
        retained_view_generations.end(),
        std::make_move_iterator(
            command_retirement.begin()),
        std::make_move_iterator(
            command_retirement.end()));

    const spectiary::SourceCollectionSessionResult
        repeated_query_result = session.Submit(
            spectiary::SourceCollectionSessionIntent::
                UpdateSampleNavigation(
                    spectiary::SampleNavigationIntent::
                        SetSampleNameQuery("b")));
    Require(
        !repeated_query_result.action.snapshot_changed &&
            !repeated_query_result.action.workflow_changed &&
            !repeated_query_result.action.navigation_inputs_changed &&
            !repeated_query_result.view_invalidated &&
            &session.View() == &command_view &&
            session.TakeViewRetirement().empty(),
        "a repeated query must not invalidate or rebuild the projection");

    const spectiary::SourceCollectionSessionResult
        boundary_navigation_result = session.Submit(
            spectiary::SourceCollectionSessionIntent::
                UpdateSampleNavigation(
                    spectiary::SampleNavigationIntent::Move(
                        spectiary::SampleNavigationRequest::
                            Previous())));
    Require(
        !boundary_navigation_result.navigation.moved &&
            !boundary_navigation_result.view_invalidated &&
            &session.View() == &command_view &&
            session.TakeViewRetirement().empty(),
        "boundary navigation must not invalidate or rebuild the projection");

    const spectiary::SourceCollectionSessionResult workflow_result =
        session.Submit(
            spectiary::SourceCollectionSessionIntent::
                ChangeActiveSampleWorkflow(
                    spectiary::ActiveSampleWorkflowIntent::
                        StartOrResumeTemporaryLabelingTask()));
    Require(
        workflow_result.action.workflow_changed &&
            workflow_result.view_invalidated,
        "maintenance fixture should create a temporary task");
    (void)session.View();
    std::vector<spectiary::BackgroundRetirementHandle>
        workflow_retirement =
            session.TakeViewRetirement();
    Require(
        workflow_retirement.size() == 1,
        "workflow mutation should retire one projection generation");
    retained_view_generations.insert(
        retained_view_generations.end(),
        std::make_move_iterator(
            workflow_retirement.begin()),
        std::make_move_iterator(
            workflow_retirement.end()));
    const spectiary::SourceCollectionSessionResult
        scheduled_labeling_result =
            Submit(
                session,
                UpsertActiveLabel(
                    spectiary::SampleLabelDefinition{
                        7,
                        "scheduled",
                        's'}));
    Require(
        scheduled_labeling_result.changed &&
            scheduled_labeling_result.view_invalidated,
        "maintenance fixture should schedule a draft task save");
    const spectiary::SourceCollectionSessionView*
        maintenance_view_before = &session.View();
    std::vector<spectiary::BackgroundRetirementHandle>
        scheduled_labeling_retirement =
            session.TakeViewRetirement();
    Require(
        scheduled_labeling_retirement.size() == 1,
        "scheduled draft mutation should retire one projection generation");
    retained_view_generations.insert(
        retained_view_generations.end(),
        std::make_move_iterator(
            scheduled_labeling_retirement.begin()),
        std::make_move_iterator(
            scheduled_labeling_retirement.end()));
    bool maintenance_changed_projection = false;
    std::vector<spectiary::BackgroundRetirementHandle>
        maintenance_retirement;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::optional<
            spectiary::LocalUserStateSaveScheduler::TimePoint>
            deadline = session.NextMaintenanceDeadline();
        Require(
            deadline.has_value(),
            "scheduled labeling state should expose a maintenance deadline");
        spectiary::SourceCollectionSessionResult
            maintenance =
                session.RunMaintenance(*deadline);
        maintenance_retirement.insert(
            maintenance_retirement.end(),
            std::make_move_iterator(
                maintenance.background_retirement.begin()),
            std::make_move_iterator(
                maintenance.background_retirement.end()));
        if (&session.View() !=
            maintenance_view_before) {
            maintenance_changed_projection = true;
            break;
        }
    }
    std::vector<spectiary::BackgroundRetirementHandle>
        view_retirement =
            session.TakeViewRetirement();
    Require(
        maintenance_changed_projection &&
            view_retirement.size() == 1,
        "view-visible maintenance should invalidate and retire exactly once");
    retained_view_generations.insert(
        retained_view_generations.end(),
        std::make_move_iterator(
            view_retirement.begin()),
        std::make_move_iterator(
            view_retirement.end()));
    const spectiary::SourceCollectionSessionView*
        maintenance_view = &session.View();
    Require(
        &session.View() == maintenance_view,
        "unchanged reads after maintenance should reuse one projection");
}

void TestRemovingInactiveSourceInvalidatesTheSessionView()
{
    const std::filesystem::path source_a =
        UniqueTempPath("_view_roster_a.npy");
    const std::filesystem::path source_b =
        UniqueTempPath("_view_roster_b.npy");
    std::vector<LoadedSourceSnapshot> loaded_snapshots;
    PreparedSession session =
        MakeMultiSourceSession(
            loaded_snapshots,
            source_a,
            3,
            source_b,
            3);
    (void)Submit(
        session,
        OpenSourceCollection(source_a));
    (void)Submit(
        session,
        OpenSourceCollection(source_b));

    const spectiary::SourceCollectionSessionView& before =
        session.View();
    Require(
        before.sources.size() == 2 &&
            before.current_source_index &&
            *before.current_source_index == 1,
        "inactive-source removal fixture should present source B");

    const spectiary::SourceCollectionSessionResult result =
        session.Submit(RemoveSourceCollection(0));
    Require(
        result.action.source_roster_changed,
        "removing an inactive source should report a roster mutation");
    const spectiary::SourceCollectionSessionView& after =
        session.View();
    Require(
        &after != &before &&
            after.sources.size() == 1 &&
            after.sources.front().path == source_b &&
            after.current_source_index &&
            *after.current_source_index == 0,
        "removing an inactive source should rebuild the roster projection");
    Require(
        session.TakeViewRetirement().size() == 1 &&
            &session.View() == &after,
        "inactive-source removal should retire exactly one generation");
}

void TestSourceSelectionSupersessionRequiresAnActualActivationChange()
{
    spectiary::SourceCollectionSession session(
        {},
        {},
        {},
        {});
    Require(
        !session.SupersedesPendingSourceActivation(SwitchSourceCollection(0)),
        "an unavailable source selection cannot supersede the active source");
    Require(
        !session.SupersedesPendingSourceActivation(RemoveSourceCollection(0)),
        "an unavailable source removal cannot supersede the active source");
    Require(
        !session.SupersedesPendingSourceActivation(
            spectiary::SourceCollectionSessionIntent::UpdateSampleNavigation(
                spectiary::SampleNavigationIntent::Move(
                    spectiary::SampleNavigationRequest::Next()))),
        "navigation should retain its own replacement follow-up");
}

void TestRemovedPreparedReuseTargetIsRejectedWithoutMutatingTheSession()
{
    const std::filesystem::path source_a = UniqueTempPath("_rejected_reuse_a.npy");
    const std::filesystem::path source_b = UniqueTempPath("_rejected_reuse_b.npy");
    spectiary::SourceCollectionSession session(
        {},
        {},
        {},
        {});

    const spectiary::SpectrumSnapshotHandle snapshot_a = MakeSnapshot(source_a, 3, 0);
    spectiary::SourceCollectionContext context_a;
    context_a.identity = {"reuse-a", "a", "a-source", "a-context", 3};
    context_a.manifest.sample_names = {"a", "b", "c"};
    spectiary::PreparedSampleWorkflowState workflow_a =
        PrepareWorkflow(snapshot_a, context_a, 0, {}, {});
    (void)session.OpenPreparedSource(
        source_a,
        0,
        snapshot_a,
        std::move(context_a),
        std::move(workflow_a));

    const spectiary::SpectrumSnapshotHandle snapshot_b = MakeSnapshot(source_b, 3, 0);
    spectiary::SourceCollectionContext context_b;
    context_b.identity = {"reuse-b", "b", "b-source", "b-context", 3};
    context_b.manifest.sample_names = {"x", "y", "z"};
    spectiary::PreparedSampleWorkflowState workflow_b =
        PrepareWorkflow(snapshot_b, context_b, 0, {}, {});
    (void)session.OpenPreparedSource(
        source_b,
        0,
        snapshot_b,
        std::move(context_b),
        std::move(workflow_b));

    const std::optional<spectiary::SourceCollectionLoadHint> stale_plan_hint =
        session.LoadHintForSource(source_a);
    Require(stale_plan_hint.has_value(), "known inactive source should expose its plan revision");

    const spectiary::SourceCollectionSessionResult removed = Submit(session, RemoveSourceCollection(0));
    Require(
        removed.background_retirement.size() >= 2,
        "removing a prepared source should hand its snapshot and workflow context to the reclaimer");
    Require(
        removed.canceled_source_follow_up_path == source_a,
        "removing an inactive source should cancel that source's non-explicit Shell tickets");

    const spectiary::SpectrumSnapshotHandle stale_snapshot = MakeSnapshot(source_a, 3, 1);
    const spectiary::SourceCollectionSessionResult rejected = session.OpenPreparedSource(
        source_a,
        1,
        stale_snapshot,
        spectiary::PreparedSourceCollectionReuse{{"reuse-a", "a", "a-source", "a-context", 3}});
    Require(!rejected.loaded, "reuse for a removed source must be rejected");
    Require(
        rejected.message.empty() &&
            rejected.load_error.kind ==
                spectiary::
                    SourceCollectionLoadErrorKind::
                        PreparedReuseTargetUnavailable,
        "prepared-source rejection should expose a stable semantic instead of application-authored English");
    Require(
        rejected.background_retirement.size() == 1,
        "a rejected reuse must hand its decoded snapshot to the background reclaimer");
    Require(
        session.CurrentSourceSnapshot() == snapshot_b,
        "rejecting reuse for a removed source must leave the newer active source untouched");

    const spectiary::SpectrumSnapshotHandle stale_plan_snapshot = MakeSnapshot(source_a, 3, 1);
    spectiary::SourceCollectionContext stale_context;
    stale_context.identity = {"reuse-a", "a", "a-source", "a-context-new", 3};
    stale_context.manifest.sample_names = {"a", "b", "c"};
    spectiary::PreparedSampleWorkflowState stale_workflow =
        PrepareWorkflow(stale_plan_snapshot, stale_context, 1, {}, {});
    const spectiary::SourceCollectionSessionResult rejected_plan = session.OpenPreparedSource(
        source_a,
        1,
        stale_plan_snapshot,
        spectiary::PreparedSourceCollectionPlan{
            std::move(stale_context),
            std::move(stale_workflow),
            stale_plan_hint->reuse.live_workflow_revision()});
    Require(!rejected_plan.loaded, "a late full plan for a removed source must be rejected");
    Require(
        rejected_plan.message.empty() &&
            rejected_plan.load_error.kind ==
                spectiary::
                    SourceCollectionLoadErrorKind::
                        PreparedKnownSourcePlanStale,
        "stale prepared-plan rejection should expose a stable semantic instead of English diagnostic text");
    Require(
        rejected_plan.background_retirement.size() >= 2,
        "a rejected full plan should retire its decoded snapshot and prepared payload");
    Require(
        session.CurrentSourceSnapshot() == snapshot_b && session.View().sources.size() == 1,
        "a late full plan must not reinsert or activate the removed source");
}

void TestReactivatedFilteredSourceQueuesFreshWorkWithoutDroppingCommittedSnapshot()
{
    const std::filesystem::path source_a = UniqueTempPath("_interrupted_follow_up_a.npy");
    const std::filesystem::path source_b = UniqueTempPath("_interrupted_follow_up_b.npy");
    const std::filesystem::path annotation_path = UniqueTempPath("_interrupted_follow_up.npy");
    const std::filesystem::path workflow_cache = UniqueTempPath("_interrupted_follow_up_workflow.json");
    SaveLabelResultFixture(
        annotation_path,
        "follow-up-values",
        "Follow-up values",
        {1, 2, 3},
        spectiary::SampleLabelSet{},
        false);
    std::string annotation_error;
    std::optional<spectiary::SampleAnnotationResult> annotation =
        spectiary::test_support::LegacyFixtureIo{}.Load(annotation_path, 3, &annotation_error);
    Require(annotation.has_value(), "interrupted follow-up annotation fixture should load");

    spectiary::SourceCollectionSession session(
        std::filesystem::path{},
        std::filesystem::path{},
        std::filesystem::path{},
        workflow_cache);

    const spectiary::SpectrumSnapshotHandle snapshot_a = MakeSnapshot(source_a, 3, 0);
    spectiary::SourceCollectionContext context_a;
    context_a.identity = {"interrupted-a", "a", "a-source", "a-context", 3};
    context_a.manifest.sample_names = {"a", "b", "c"};
    context_a.manifest.annotations.push_back(std::move(*annotation));
    spectiary::PreparedSampleWorkflowState workflow_a =
        PrepareWorkflow(snapshot_a, context_a, 0, {}, {});
    (void)session.OpenPreparedSource(
        source_a,
        0,
        snapshot_a,
        std::move(context_a),
        std::move(workflow_a));
    const std::string annotation_source_id = AnnotationSourceId(annotation_path);
    (void)Submit(session, AddSampleFilterSource(annotation_source_id));
    Require(
        Submit(session, SetFilterValueSelected(annotation_source_id, "2", true)).follow_up_spectrum_index == 1,
        "filter should request the unresolved row 1 follow-up");
    Require(
        !Submit(session, SetSampleNameQuery("b")).follow_up_spectrum_index,
        "a query that retains the pending target should keep the existing row 1 ticket");
    Require(
        session.CurrentSampleSnapshot() == snapshot_a,
        "the complete committed row 0 snapshot should remain visible while row 1 is pending");

    const spectiary::SpectrumSnapshotHandle snapshot_b = MakeSnapshot(source_b, 3, 0);
    spectiary::SourceCollectionContext context_b;
    context_b.identity = {"interrupted-b", "b", "b-source", "b-context", 3};
    context_b.manifest.sample_names = {"x", "y", "z"};
    spectiary::PreparedSampleWorkflowState workflow_b =
        PrepareWorkflow(snapshot_b, context_b, 0, {}, {});
    (void)session.OpenPreparedSource(
        source_b,
        0,
        snapshot_b,
        std::move(context_b),
        std::move(workflow_b));

    const spectiary::SourceCollectionSessionResult reactivated =
        Submit(session, SwitchSourceCollection(0));
    Require(
        reactivated.follow_up_spectrum_index == 1,
        "reactivating source A should create fresh row 1 work because its active filter excludes row 0");
    Require(
        session.CurrentSampleSnapshot() == snapshot_a,
        "reactivating source A should restore its last complete committed row 0 snapshot");
}

void TestSwitchingPreparedSourceReusesItsInMemoryContext()
{
    const std::filesystem::path source_a = UniqueTempPath("_prepared_a.npy");
    const std::filesystem::path source_b = UniqueTempPath("_prepared_b.npy");
    const std::filesystem::path annotation_path = UniqueTempPath("_prepared_filter.npy");
    const std::filesystem::path navigation_cache =
        UniqueTempPath("_prepared_navigation.json");
    const std::filesystem::path labeling_cache =
        UniqueTempPath("_prepared_labeling.json");
    const std::filesystem::path workflow_cache = UniqueTempPath("_prepared_workflow.json");
    const std::string annotation_source_id = AnnotationSourceId(annotation_path);
    SaveLabelResultFixture(
        annotation_path,
        "prepared-filter",
        "Prepared filter",
        {1, 2, 2},
        spectiary::SampleLabelSet{},
        false);
    TouchFile(source_a);
    TouchFile(source_b);

    std::vector<LoadedSourceSnapshot> loaded_snapshots;
    PreparedSession session(
        [&loaded_snapshots](
            const std::filesystem::path& path,
            std::size_t spectrum_index) {
            loaded_snapshots.push_back(
                {path, spectrum_index});
            return MakeSnapshot(path, 3, spectrum_index);
        },
        {},
        navigation_cache,
        labeling_cache,
        workflow_cache);
    (void)session.Open(
        source_a,
        0,
        {annotation_path});
    (void)Submit(session, AddSampleFilterSource(annotation_source_id));
    (void)Submit(session, SetFilterValueSelected(annotation_source_id, "2", true));
    Require(session.View().navigation.filter_active, "prepared source A should have an active filter");

    (void)session.Open(source_b);
    const std::size_t loads_before_reactivation =
        loaded_snapshots.size();
    (void)Submit(session, SwitchSourceCollection(0));

    const spectiary::SourceCollectionSessionView view = session.View();
    Require(view.filter.sources.size() == 1, "prepared source activation should retain its cached manifest");
    Require(view.navigation.filter_active, "prepared source activation should restore its workflow identity");
    Require(
        view.navigation.current_index && *view.navigation.current_index == 1,
        "prepared source activation should restore the retained filtered row");
    Require(
        loaded_snapshots.size() ==
            loads_before_reactivation,
        "prepared source activation should reuse its in-memory context and snapshot");
}

void TestPreparedSnapshotsBecomeBoundedRawRowResidency()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_resident_rows.npy");
    spectiary::SourceCollectionSession session(
        {},
        {},
        {},
        {});

    spectiary::SourceCollectionContext context;
    context.identity = {
        "resident-rows",
        "source",
        "source-fingerprint",
        "context-fingerprint",
        12,
    };
    const spectiary::SourceCollectionIdentity identity =
        context.identity;
    context.manifest.sample_names = {
        "00", "01", "02", "03", "04", "05",
        "06", "07", "08", "09", "10", "11",
    };
    const spectiary::SourceCollectionContextReuseProof proof{
        identity,
        {
            .source_stat_fingerprint = "stable-source",
            .companion_name_fingerprint = "stable-name",
            .companion_annotation_fingerprint =
                "stable-annotation",
        }};

    auto payload_destroyed_promise =
        std::make_shared<std::promise<std::thread::id>>();
    std::future<std::thread::id> payload_destroyed =
        payload_destroyed_promise->get_future();
    auto* payload = new std::vector<double>(1U << 20U, 1.0);
    spectiary::SpectrumValueVector payload_handle(
        payload,
        [payload_destroyed_promise](
            const std::vector<double>* value) {
            delete value;
            payload_destroyed_promise->set_value(
                std::this_thread::get_id());
        });
    auto first_mutable =
        std::make_shared<spectiary::SpectrumSnapshot>(
            *MakeSnapshot(source_path, 12, 0));
    first_mutable->current_spectrum.x_values =
        std::move(payload_handle);
    first_mutable->current_spectrum.point_count = 1U << 20U;
    spectiary::SpectrumSnapshotHandle first_snapshot =
        first_mutable;
    spectiary::PreparedSampleWorkflowState workflow =
        PrepareWorkflow(first_snapshot, context, 0, {}, {});
    spectiary::SourceCollectionSessionResult initial =
        session.OpenPreparedSource(
            source_path,
            0,
            first_snapshot,
            spectiary::PreparedSourceCollectionPlan{
                std::move(context),
                std::move(workflow)},
            {},
            proof);
    Require(initial.loaded, "resident fixture should load row 0");
    first_mutable.reset();
    first_snapshot.reset();

    const std::thread::id caller_thread =
        std::this_thread::get_id();
    spectiary::SourceCollectionLoadQueue retirement_queue;
    for (spectiary::BackgroundRetirementHandle& resource :
         initial.background_retirement) {
        retirement_queue.RetireResource(std::move(resource));
    }
    for (std::size_t row = 1; row <= 8; ++row) {
        const spectiary::SourceCollectionSessionResult navigation =
            Submit(
                session,
                MoveSampleNavigation(
                    spectiary::SampleNavigationRequest::Next()));
        Require(
            navigation.follow_up_spectrum_index == row,
            "resident history should request the next raw row");
        spectiary::SourceCollectionSessionResult loaded =
            session.OpenPreparedSource(
                source_path,
                row,
                MakeSnapshot(source_path, 12, row),
                spectiary::PreparedSourceCollectionReuse{
                    identity},
                {},
                proof);
        Require(loaded.loaded, "resident history row should load");
        for (spectiary::BackgroundRetirementHandle& resource :
             loaded.background_retirement) {
            retirement_queue.RetireResource(
                std::move(resource));
        }
    }
    const std::optional<spectiary::SourceCollectionLoadHint>
        row_seven_before_view_changes =
            session.LoadHintForSource(source_path, 7);
    Require(
        row_seven_before_view_changes &&
            row_seven_before_view_changes
                ->reuse.resident_snapshot() &&
            row_seven_before_view_changes
                ->reuse.resident_snapshot()
                ->snapshot,
        "the eight-entry history should retain raw row 7");

    (void)Submit(
        session,
        SetSampleNameQuery("0"));
    (void)Submit(
        session,
        AddSampleSortSource("sample-name"));
    (void)Submit(
        session,
        SetSampleSortSource("sample-name"));
    const std::optional<spectiary::SourceCollectionLoadHint>
        row_seven_after_view_changes =
            session.LoadHintForSource(source_path, 7);
    Require(
        row_seven_after_view_changes &&
            row_seven_after_view_changes
                ->reuse.resident_snapshot() &&
            row_seven_after_view_changes
                ->reuse.resident_snapshot()
                    ->snapshot ==
                row_seven_before_view_changes
                    ->reuse.resident_snapshot()
                    ->snapshot,
        "query and sorting should only change access order, not copy or invalidate resident snapshots");

    Require(
        Submit(
            session,
            MoveSampleNavigation(
                spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 9,
        "resident eviction should be driven by a committed row 9 navigation");
    spectiary::SourceCollectionSessionResult eviction =
        session.OpenPreparedSource(
            source_path,
            9,
            MakeSnapshot(source_path, 12, 9),
            spectiary::PreparedSourceCollectionReuse{identity},
            {},
            proof);
    Require(eviction.loaded, "row 9 should commit");
    const std::optional<spectiary::SourceCollectionLoadHint>
        row_zero_after_eviction =
            session.LoadHintForSource(source_path, 0);
    Require(
        row_zero_after_eviction &&
            !row_zero_after_eviction
                 ->reuse.resident_snapshot(),
        "the ninth non-current row should evict untouched raw row 0");
    for (spectiary::BackgroundRetirementHandle& resource :
         eviction.background_retirement) {
        retirement_queue.RetireResource(std::move(resource));
    }
    Require(
        payload_destroyed.wait_for(std::chrono::seconds(2)) ==
            std::future_status::ready,
        "evicted snapshot payload should be reclaimed promptly");
    Require(
        payload_destroyed.get() != caller_thread,
        "evicted snapshot payload must be destroyed by background retirement");

    spectiary::SourceCollectionContext changed_context;
    changed_context.identity = identity;
    changed_context.identity.context_fingerprint =
        "changed-context";
    changed_context.manifest.sample_names = {
        "00", "01", "02", "03", "04", "05",
        "06", "07", "08", "09", "10", "11",
    };
    const spectiary::SourceCollectionContextReuseProof
        changed_proof{
            changed_context.identity,
            proof.dependency_state};
    spectiary::PreparedSampleWorkflowState changed_workflow =
        PrepareWorkflow(
            MakeSnapshot(source_path, 12, 10),
            changed_context,
            10,
            {},
            {});
    Require(
        Submit(
            session,
            MoveSampleNavigation(
                spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 10,
        "context invalidation should be driven by row 10 navigation");
    spectiary::SourceCollectionSessionResult changed =
        session.OpenPreparedSource(
            source_path,
            10,
            MakeSnapshot(source_path, 12, 10),
            spectiary::PreparedSourceCollectionPlan{
                std::move(changed_context),
                std::move(changed_workflow)},
            {},
            changed_proof);
    Require(changed.loaded, "changed context row should load");
    const std::optional<spectiary::SourceCollectionLoadHint>
        invalidated_old_row =
            session.LoadHintForSource(source_path, 8);
    Require(
        invalidated_old_row &&
            !invalidated_old_row
                 ->reuse.resident_snapshot(),
        "context generation changes must invalidate old resident rows");
    for (spectiary::BackgroundRetirementHandle& resource :
         changed.background_retirement) {
        retirement_queue.RetireResource(std::move(resource));
    }
}

void TestPreparedOpenReturnsResidentInvalidationForBackgroundRetirement()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_prepared_resident_retirement.npy");
    TouchFile(source_path);

    auto payload_destroyed_promise =
        std::make_shared<std::promise<std::thread::id>>();
    std::future<std::thread::id> payload_destroyed =
        payload_destroyed_promise->get_future();
    spectiary::SpectrumValueVector payload(
        new std::vector<double>(1024, 1.0),
        [payload_destroyed_promise](
            const std::vector<double>* value) {
            delete value;
            payload_destroyed_promise->set_value(
                std::this_thread::get_id());
        });
    auto first_mutable =
        std::make_shared<spectiary::SpectrumSnapshot>(
            *MakeSnapshot(source_path, 3, 0));
    first_mutable->current_spectrum.x_values = std::move(payload);
    first_mutable->current_spectrum.point_count = 1024;
    spectiary::SpectrumSnapshotHandle first_snapshot =
        first_mutable;

    spectiary::SourceCollectionSession session(
        {},
        {},
        {},
        {});
    spectiary::SourceCollectionContext context;
    context.identity = {
        "prepared-resident-retirement",
        "source",
        "source-fingerprint",
        "context-fingerprint",
        3,
    };
    context.manifest.sample_names = {"0", "1", "2"};
    const spectiary::SourceCollectionIdentity identity =
        context.identity;
    const spectiary::SourceCollectionContextReuseProof proof{
        identity,
        {
            .source_stat_fingerprint = "stable-source",
            .companion_name_fingerprint = "stable-name",
            .companion_annotation_fingerprint =
                "stable-annotation",
        }};
    spectiary::PreparedSampleWorkflowState workflow =
        PrepareWorkflow(first_snapshot, context, 0, {}, {});
    spectiary::SourceCollectionLoadQueue retirement_queue;
    spectiary::SourceCollectionSessionResult initial =
        session.OpenPreparedSource(
            source_path,
            0,
            first_snapshot,
            spectiary::PreparedSourceCollectionPlan{
                std::move(context),
                std::move(workflow)},
            {},
            proof);
    for (spectiary::BackgroundRetirementHandle& resource :
         initial.background_retirement) {
        retirement_queue.RetireResource(std::move(resource));
    }
    Require(
        Submit(
            session,
            MoveSampleNavigation(
                spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 1,
        "prepared retirement fixture should prepare row 1");
    spectiary::SourceCollectionSessionResult second =
        session.OpenPreparedSource(
            source_path,
            1,
            MakeSnapshot(source_path, 3, 1),
            spectiary::PreparedSourceCollectionReuse{identity},
            {},
            proof);
    for (spectiary::BackgroundRetirementHandle& resource :
         second.background_retirement) {
        retirement_queue.RetireResource(std::move(resource));
    }
    first_mutable.reset();
    first_snapshot.reset();

    const std::thread::id caller_thread =
        std::this_thread::get_id();
    const spectiary::SpectrumSnapshotHandle changed_snapshot =
        MakeSnapshot(source_path, 3, 2);
    spectiary::SourceCollectionContext changed_context;
    changed_context.identity = {
        "prepared-resident-retirement-v2",
        "source",
        "source-fingerprint",
        "context-fingerprint-v2",
        3,
    };
    changed_context.manifest.sample_names = {"0", "1", "2"};
    spectiary::PreparedSampleWorkflowState changed_workflow =
        PrepareWorkflow(
            changed_snapshot,
            changed_context,
            2,
            {},
            {});
    const spectiary::SourceCollectionContextReuseProof
        changed_proof{
            changed_context.identity,
            proof.dependency_state,
        };
    spectiary::SourceCollectionSessionResult prepared =
        session.OpenPreparedSource(
            source_path,
            2,
            changed_snapshot,
            spectiary::PreparedSourceCollectionPlan{
                std::move(changed_context),
                std::move(changed_workflow)},
            {},
            changed_proof);
    Require(
        payload_destroyed.wait_for(std::chrono::milliseconds(0)) !=
            std::future_status::ready,
        "prepared invalidation must not destroy resident payloads on the caller thread");
    Require(
        !prepared.background_retirement.empty(),
        "prepared invalidation should return background-retirement ownership");
    for (spectiary::BackgroundRetirementHandle& resource :
         prepared.background_retirement) {
        retirement_queue.RetireResource(std::move(resource));
    }
    Require(
        payload_destroyed.wait_for(std::chrono::seconds(2)) ==
            std::future_status::ready,
        "prepared invalidated resident payload should be reclaimed promptly");
    Require(
        payload_destroyed.get() != caller_thread,
        "prepared invalidated resident payload must be destroyed by background retirement");
    std::filesystem::remove(source_path);
}

void TestResidentSnapshotByteCapEvictsBeforeCountCap()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_resident_byte_cap.npy");
    spectiary::SourceCollectionSession session(
        {},
        {},
        {},
        {});
    spectiary::SourceCollectionContext context;
    context.identity = {
        "resident-byte-cap",
        "source",
        "source-fingerprint",
        "context-fingerprint",
        3,
    };
    context.manifest.sample_names = {"0", "1", "2"};
    const spectiary::SourceCollectionIdentity identity =
        context.identity;
    const spectiary::SourceCollectionContextReuseProof proof{
        identity,
        {
            .source_stat_fingerprint = "stable-source",
            .companion_name_fingerprint = "stable-name",
            .companion_annotation_fingerprint =
                "stable-annotation",
        }};
    constexpr std::size_t kLargePayloadBytes =
        65U * 1024U * 1024U;
    const auto large_values =
        std::make_shared<const std::vector<double>>(
            kLargePayloadBytes / sizeof(double),
            1.0);
    const auto make_large_snapshot =
        [&source_path, &large_values](std::size_t row) {
            auto snapshot =
                std::make_shared<spectiary::SpectrumSnapshot>(
                    *MakeSnapshot(source_path, 3, row));
            snapshot->current_spectrum.x_values = large_values;
            snapshot->current_spectrum.point_count =
                large_values->size();
            return spectiary::SpectrumSnapshotHandle{
                std::move(snapshot)};
        };

    spectiary::SpectrumSnapshotHandle first_snapshot =
        make_large_snapshot(0);
    spectiary::PreparedSampleWorkflowState workflow =
        PrepareWorkflow(first_snapshot, context, 0, {}, {});
    spectiary::SourceCollectionLoadQueue retirement_queue;
    spectiary::SourceCollectionSessionResult initial =
        session.OpenPreparedSource(
            source_path,
            0,
            first_snapshot,
            spectiary::PreparedSourceCollectionPlan{
                std::move(context),
                std::move(workflow)},
            {},
            proof);
    for (spectiary::BackgroundRetirementHandle& resource :
         initial.background_retirement) {
        retirement_queue.RetireResource(std::move(resource));
    }
    first_snapshot.reset();

    for (std::size_t row = 1; row <= 2; ++row) {
        Require(
            Submit(
                session,
                MoveSampleNavigation(
                    spectiary::SampleNavigationRequest::Next()))
                    .follow_up_spectrum_index == row,
            "resident byte-cap fixture should request the next row");
        spectiary::SourceCollectionSessionResult loaded =
            session.OpenPreparedSource(
                source_path,
                row,
                make_large_snapshot(row),
                spectiary::PreparedSourceCollectionReuse{
                    identity},
                {},
                proof);
        for (spectiary::BackgroundRetirementHandle& resource :
             loaded.background_retirement) {
            retirement_queue.RetireResource(
                std::move(resource));
        }
    }

    const auto row_zero =
        session.LoadHintForSource(source_path, 0);
    const auto row_one =
        session.LoadHintForSource(source_path, 1);
    Require(
        row_zero &&
            !row_zero->reuse.resident_snapshot(),
        "two 65 MiB residents should evict the older row at the 128 MiB byte cap");
    Require(
        row_one &&
            row_one->reuse.resident_snapshot(),
        "byte-cap eviction should retain the newer row while below the eight-entry count cap");
}

void TestFolderListingGenerationFlowsIntoSubsequentLoadHint()
{
    const std::filesystem::path source_path = UniqueTempPath("_folder_listing_hint");
    spectiary::SourceCollectionSession session(
        std::filesystem::path{},
        std::filesystem::path{},
        std::filesystem::path{},
        std::filesystem::path{});

    const spectiary::SpectrumSnapshotHandle snapshot = MakeSnapshot(source_path, 2, 0);
    spectiary::SourceCollectionContext context;
    context.identity = {
        "folder-listing-hint",
        "folder",
        "source-fingerprint",
        "context-fingerprint",
        2,
    };
    const spectiary::SourceCollectionIdentity identity = context.identity;
    context.manifest.sample_names = {"a.csv", "b.csv"};
    spectiary::PreparedSampleWorkflowState workflow =
        PrepareWorkflow(snapshot, context, 0, {}, {});
    auto* listing_generation_value =
        new spectiary::SourceCollectionFolderListingGeneration();
    listing_generation_value->listing.spectra = {
        {source_path / "a.csv", "csv", "a-stat"},
        {source_path / "b.csv", "csv", "b-stat"},
    };
    auto listing_destroyed_promise =
        std::make_shared<std::promise<std::thread::id>>();
    std::future<std::thread::id> listing_destroyed =
        listing_destroyed_promise->get_future();
    spectiary::SourceCollectionFolderListingGenerationHandle verified_generation(
        listing_generation_value,
        [listing_destroyed_promise](
            const spectiary::SourceCollectionFolderListingGeneration* value) {
            delete value;
            listing_destroyed_promise->set_value(std::this_thread::get_id());
        });
    const spectiary::SourceCollectionContextReuseProof reuse_proof{
        identity,
        {
            .source_stat_fingerprint = "folder-stat",
            .companion_name_fingerprint = "none",
            .companion_annotation_fingerprint = "none",
        }};

    const spectiary::SourceCollectionSessionResult result = session.OpenPreparedSource(
        source_path,
        0,
        snapshot,
        spectiary::PreparedSourceCollectionPlan{
            std::move(context),
            std::move(workflow)},
        verified_generation,
        reuse_proof);
    Require(result.loaded, "prepared folder generation should load");
    std::optional<spectiary::SourceCollectionLoadHint> hint =
        session.LoadHintForSource(source_path);
    Require(hint.has_value(), "known folder source should expose a subsequent load hint");
    Require(
        hint->reuse.folder_listing_generation() ==
            verified_generation,
        "subsequent navigation should reuse the exact immutable listing generation");
    Require(
        hint->reuse.context_reuse_proof().has_value() &&
            hint->reuse.context_reuse_proof()->identity.id ==
                identity.id &&
            hint->reuse.context_reuse_proof()
                    ->dependency_state ==
                reuse_proof.dependency_state,
        "subsequent navigation should carry the proof accepted with that generation");

    const std::weak_ptr<const spectiary::SourceCollectionFolderListingGeneration>
        retired_listing_generation = verified_generation;
    hint.reset();
    verified_generation.reset();
    Require(
        !retired_listing_generation.expired(),
        "the roster should own the active listing generation");
    Require(
        Submit(session, MoveSampleNavigation(spectiary::SampleNavigationRequest::Next()))
                .follow_up_spectrum_index == 1,
        "the listing retirement fixture should prepare a row replacement");

    spectiary::SourceCollectionSessionResult replacement = session.OpenPreparedSource(
        source_path,
        1,
        MakeSnapshot(source_path, 2, 1),
        spectiary::PreparedSourceCollectionReuse{identity},
        std::make_shared<const spectiary::SourceCollectionFolderListingGeneration>());
    Require(replacement.loaded, "the replacement folder generation should load");
    Require(
        !retired_listing_generation.expired(),
        "the replaced listing generation should remain owned until background retirement");
    const std::thread::id caller_thread = std::this_thread::get_id();
    spectiary::SourceCollectionLoadQueue retirement_queue;
    for (spectiary::BackgroundRetirementHandle& resource :
         replacement.background_retirement) {
        retirement_queue.RetireResource(std::move(resource));
    }
    Require(
        listing_destroyed.wait_for(std::chrono::seconds(2)) ==
            std::future_status::ready,
        "the replaced listing should be reclaimed promptly");
    Require(
        retired_listing_generation.expired(),
        "background retirement should release the replaced listing generation");
    Require(
        listing_destroyed.get() != caller_thread,
        "the replaced listing should be reclaimed off the UI caller thread");
}

void TestSourceSessionFlushFailureKeepsDirtyState()
{
    const std::filesystem::path blocker = UniqueTempPath("_blocked");
    const std::filesystem::path source_session_cache = blocker / "source-session.json";
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    const std::filesystem::path source_path = UniqueTempPath("_source.npy");
    TouchFile(blocker);
    TouchFile(source_path);

    std::vector<LoadedSourceSnapshot> loaded_snapshots;
    PreparedSession session = MakePersistentSession(
        loaded_snapshots,
        source_session_cache,
        navigation_cache,
        labeling_cache,
        {SourceFixture{source_path, 3}});
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());

    const spectiary::SourceCollectionStateFlushResult failed_flush =
        session.FlushStateCachesWithStatus();
    Require(
        !failed_flush.source_session_saved,
        "blocked source-session path should report its exact failed owner");
    Require(
        failed_flush.navigation_saved &&
            failed_flush.labeling_saved &&
            failed_flush.workflow_saved,
        "source-session failure must not block the other three cache flushes");
    const spectiary::LocalUserStateHealthView failed_health =
        session.View().persistence;
    Require(
        failed_health.kind ==
            spectiary::LocalUserStateHealthKind::Retrying,
        "a save failure should expose retrying overall health");
    Require(
        HasPersistenceMessage(
            failed_health,
            spectiary::LocalUserStateArea::
                SourceSession,
            spectiary::
                LocalUserStateHealthMessageKind::
                    SaveRetrying),
        "retrying health should identify the failed cache owner");
    Require(
        session.NextMaintenanceDeadline().has_value(),
        "a failed source-session flush should retain a retry maintenance deadline");
    {
        std::vector<LoadedSourceSnapshot> reloaded_snapshots;
        PreparedSession reloaded(
            [&reloaded_snapshots, source_path](const std::filesystem::path& path, std::size_t spectrum_index) {
                Require(path == source_path, "labeling cache reload should use the source fixture");
                reloaded_snapshots.push_back(LoadedSourceSnapshot{path, spectrum_index});
                return MakeSnapshot(source_path, 3, spectrum_index);
            },
            std::filesystem::path{},
            navigation_cache,
            labeling_cache,
            UniqueTempPath("_workflow.json"));
        (void)Submit(reloaded, OpenSourceCollection(source_path, 0));
        Require(
            !reloaded.View().labeling.has_active_task &&
                reloaded.View().labeling.has_temporary_task,
            "workflow flush should save a read-only labeling task while the original instance holds its lease");
    }

    std::filesystem::remove(blocker);
    std::filesystem::create_directories(blocker);
    const spectiary::SourceCollectionStateFlushResult recovered_flush =
        session.FlushStateCachesWithStatus();
    Require(
        recovered_flush.all_saved(),
        "flush should retry dirty source state after the path is fixed");
    Require(
        session.View().persistence.kind ==
            spectiary::LocalUserStateHealthKind::Recovered,
        "the first successful retry should expose recovered health");

    const spectiary::SourceCollectionSessionStateCache restored_state =
        spectiary::LoadSourceCollectionSessionStateCache(spectiary::RuntimePaths{}, source_session_cache)
            .cache;
    Require(restored_state.sources.size() == 1, "retry flush should write the source session cache");
    Require(restored_state.sources[0].path == source_path, "retry flush should persist the source path");

    (void)Submit(session, RemoveSourceCollection(0));
    Require(
        session.View().persistence.kind ==
            spectiary::LocalUserStateHealthKind::Healthy,
        "the next source-session mutation should clear recovered health");
}

struct LabelingProjectionHandoffFixture {
    std::filesystem::path source_path;
    std::filesystem::path output_path;
    std::filesystem::path navigation_cache;
    std::filesystem::path labeling_cache;
    std::filesystem::path workflow_cache;
    spectiary::SourceCollectionContext context;
    std::string task_id;
};

LabelingProjectionHandoffFixture SeedLabelingProjectionHandoffFixture(
    std::string_view suffix,
    bool select_first_label)
{
    LabelingProjectionHandoffFixture fixture;
    fixture.source_path =
        UniqueTempPath(std::string(suffix) + "_source.npy");
    fixture.output_path =
        UniqueTempPath(std::string(suffix) + "_labels.asdf");
    fixture.navigation_cache =
        UniqueTempPath(std::string(suffix) + "_navigation.json");
    fixture.labeling_cache =
        UniqueTempPath(std::string(suffix) + "_labeling.json");
    fixture.workflow_cache =
        UniqueTempPath(std::string(suffix) + "_workflow.json");
    TouchFile(fixture.source_path);
    fixture.context.identity = {
        std::string(suffix) + "-identity",
        "source",
        "source-fingerprint",
        "context-fingerprint",
        3,
    };
    fixture.context.manifest.sample_names = {
        "alpha",
        "beta",
        "gamma",
    };

    {
        spectiary::SourceCollectionSession seed(
            {},
            fixture.navigation_cache,
            fixture.labeling_cache,
            fixture.workflow_cache);
        const spectiary::SpectrumSnapshotHandle row_zero =
            MakeSnapshot(fixture.source_path, 3, 0);
        spectiary::PreparedSampleWorkflowState prepared =
            PrepareWorkflow(
                row_zero,
                fixture.context,
                0,
                fixture.labeling_cache,
                fixture.workflow_cache);
        Require(
            seed.OpenPreparedSource(
                    fixture.source_path,
                    0,
                    row_zero,
                    fixture.context,
                    std::move(prepared))
                .loaded,
            "labeling projection handoff fixture should open its source");
        (void)Submit(seed, StartOrResumeTemporaryLabelingTask());
        fixture.task_id = seed.View().labeling.task_id;
        Require(
            spectiary::IsCanonicalUuidV4(fixture.task_id),
            "labeling projection handoff fixture should create a canonical task id");
        Require(
            Submit(
                seed,
                UpsertActiveLabel(
                    spectiary::SampleLabelDefinition{
                        1,
                        "first",
                        'f'}))
                .changed &&
                Submit(
                    seed,
                    UpsertActiveLabel(
                        spectiary::SampleLabelDefinition{
                            2,
                            "second",
                            's'}))
                    .changed,
            "labeling projection handoff fixture should define both labels");
        (void)Submit(seed, AssignActiveLabelToCurrentSample(1));
        Require(
            Submit(
                seed,
                MoveSampleNavigation(
                    spectiary::SampleNavigationRequest::LocateRow(1)))
                    .follow_up_spectrum_index == 1,
            "labeling projection handoff fixture should request row 1");
        Require(
            seed.OpenPreparedSource(
                    fixture.source_path,
                    1,
                    MakeSnapshot(fixture.source_path, 3, 1),
                    spectiary::PreparedSourceCollectionReuse{
                        fixture.context.identity})
                .loaded,
            "labeling projection handoff fixture should commit row 1");
        (void)Submit(seed, AssignActiveLabelToCurrentSample(2));
        (void)Submit(
            seed,
            SetActiveLabelingOutputPath(
                fixture.output_path));
        Require(
            seed.View().labeling.output_path ==
                std::optional<std::filesystem::path>{
                    fixture.output_path},
            "labeling projection handoff fixture should formalize its task");

        const std::string source_id =
            "labeling:" + fixture.task_id;
        (void)Submit(seed, AddSampleFilterSource(source_id));
        (void)Submit(
            seed,
            SetFilterValueSelected(
                source_id,
                "2",
                true));
        if (select_first_label) {
            (void)Submit(
                seed,
                SetFilterValueSelected(
                    source_id,
                    "1",
                    true));
        }
        (void)Submit(seed, SetSampleSortSource("sample-name"));
        const spectiary::SourceCollectionSessionView seeded_view =
            seed.View();
        Require(
            seeded_view.filter.evaluation.included_count ==
                    (select_first_label ? 2 : 1) &&
                seeded_view.sorting.active &&
                seeded_view.navigation.current_sequence_position ==
                    (select_first_label ? 1 : 0),
            std::string(
                "labeling projection handoff fixture should persist its old filter and sort projection: included=") +
                std::to_string(
                    seeded_view.filter.evaluation.included_count) +
                ", sorting=" +
                (seeded_view.sorting.active ? "true" : "false") +
                ", position=" +
                (seeded_view.navigation.current_sequence_position
                     ? std::to_string(
                           *seeded_view.navigation.current_sequence_position)
                     : "none"));
        Require(
            seed.FlushStateCaches(),
            "labeling projection handoff fixture should persist its caches");
    }
    std::string annotation_error;
    Require(
        IngestReadOnlySampleAnnotation(
            fixture.context.manifest,
            fixture.output_path,
            spectiary::SampleAnnotationSourceCompatibility{
                .base_identity = fixture.context.identity.id,
                .source_kind = "test",
                .source_name = fixture.context.identity.source_name,
                .source_fingerprint =
                    fixture.context.identity.source_fingerprint,
                .sample_count =
                    fixture.context.identity.spectrum_count,
                .sample_names =
                    fixture.context.manifest.sample_names,
            },
            &annotation_error),
        annotation_error.empty()
            ? "labeling projection handoff fixture should attach its canonical owner"
            : annotation_error);
    return fixture;
}

void WriteLatestLabelingProjection(
    const LabelingProjectionHandoffFixture& fixture)
{
    spectiary::SampleLabelingController editor(
        fixture.labeling_cache);
    ActivateCanonicalFixtureSource(
        editor,
        fixture.source_path,
        fixture.context);
    if (editor.View().active_task == nullptr) {
        Require(
            editor.ActivateTask(fixture.task_id)
                .accepted,
            "projection handoff editor should acquire the task");
    }
    Require(
        editor.AssignLabel(0, 2).operation.output_saved &&
            editor.AssignLabel(1, 1).operation.output_saved &&
            editor.AssignLabel(2, 1).operation.output_saved,
        "projection handoff editor should persist the latest values");
    Require(
        editor.DeactivateActiveTask().state_saved,
        "projection handoff editor should release the latest task");
}

struct TemporaryDraftNavigationRefreshFixture {
    std::filesystem::path source_path;
    std::filesystem::path formal_output_path;
    std::filesystem::path draft_output_path;
    std::filesystem::path navigation_cache;
    std::filesystem::path labeling_cache;
    std::filesystem::path workflow_cache;
    spectiary::SourceCollectionContext context;
    std::string formal_task_id;
    std::string draft_task_id;
};

TemporaryDraftNavigationRefreshFixture
SeedTemporaryDraftNavigationRefreshFixture(std::string_view suffix)
{
    TemporaryDraftNavigationRefreshFixture fixture;
    fixture.source_path = UniqueTempPath(std::string(suffix) + "_source.npy");
    fixture.formal_output_path = UniqueTempPath(std::string(suffix) + "_formal.asdf");
    fixture.draft_output_path = UniqueTempPath(std::string(suffix) + "_draft.asdf");
    fixture.navigation_cache = UniqueTempPath(std::string(suffix) + "_navigation.json");
    fixture.labeling_cache = UniqueTempPath(std::string(suffix) + "_labeling.json");
    fixture.workflow_cache = UniqueTempPath(std::string(suffix) + "_workflow.json");
    TouchFile(fixture.source_path);
    fixture.context.identity = {
        std::string(suffix) + "-identity",
        "source",
        "source-fingerprint",
        "context-fingerprint",
        3,
    };
    fixture.context.manifest.sample_names = {
        "alpha",
        "beta",
        "gamma",
    };

    {
        spectiary::SampleLabelingController seed(fixture.labeling_cache);
        ActivateCanonicalFixtureSource(
            seed,
            fixture.source_path,
            fixture.context);
        Require(
            seed.CreateTask("Formal task").accepted &&
                seed.UpsertActiveLabel(
                       spectiary::SampleLabelDefinition{1, "one", 'o'})
                    .changed,
            "navigation refresh fixture should create the unrelated formal task");
        Require(
            seed.View().active_task != nullptr,
            "navigation refresh fixture should expose the formal task");
        fixture.formal_task_id = seed.View().active_task->task_id;
        Require(
            seed.AssignLabel(0, 1).write.changed,
            "navigation refresh fixture should assign the formal task label");
        const auto saved = seed.SaveActiveTemporaryTaskToOutput(
            fixture.formal_output_path);
        Require(
            saved.output_saved,
            "navigation refresh fixture should save the formal task: " +
                saved.diagnostic);
        const auto deactivated = seed.DeactivateActiveTask();
        Require(
            deactivated.state_saved,
            "navigation refresh fixture should persist formal task deactivation: " +
                deactivated.diagnostic);

        Require(
            seed.StartOrResumeTemporaryTask().accepted,
            "navigation refresh fixture should create the paused draft");
        Require(
            seed.View().active_task != nullptr,
            "navigation refresh fixture should expose the draft task");
        fixture.draft_task_id = seed.View().active_task->task_id;
        Require(
            seed.UpsertActiveLabel(
                       spectiary::SampleLabelDefinition{1, "one", 'o'})
                    .changed &&
                seed.UpsertActiveLabel(
                       spectiary::SampleLabelDefinition{2, "two", 't'})
                    .changed &&
                seed.AssignLabel(0, 1).write.changed &&
                seed.AssignLabel(1, 2).write.changed &&
                seed.AssignLabel(2, 1).write.changed &&
                seed.SaveActiveTemporaryTaskToOutput(
                       fixture.draft_output_path)
                    .output_saved &&
                seed.DeactivateActiveTask().state_saved &&
                seed.ActivateTask(fixture.formal_task_id).accepted,
            "navigation refresh fixture should persist and select the formal task");
    }

    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(fixture.source_path, 3, 0);
    {
        spectiary::SourceCollectionSession configured(
            {},
            fixture.navigation_cache,
            fixture.labeling_cache,
            fixture.workflow_cache);
        const spectiary::PreparedSampleWorkflowState prepared =
            PrepareWorkflow(
                snapshot,
                fixture.context,
                0,
                fixture.labeling_cache,
                fixture.workflow_cache);
        Require(
            configured.OpenPreparedSource(
                    fixture.source_path,
                    0,
                    snapshot,
                    fixture.context,
                    prepared)
                .loaded,
            "navigation refresh fixture should open the formalized projection");

        const std::string filter_source_id =
            "labeling:" + fixture.draft_task_id;
        Require(
            Submit(configured, AddSampleFilterSource(filter_source_id))
                .action.workflow_changed,
            "navigation refresh fixture should add the draft filter source");
        Require(
            Submit(
                configured,
                SetFilterValueSelected(filter_source_id, "2", true))
                .action.navigation_inputs_changed &&
                configured.View().filter.evaluation.included_count == 1,
            "navigation refresh fixture should select one draft filter value");
        Require(
            Submit(configured, SetSampleSortSource("sample-name"))
                .action.navigation_inputs_changed &&
                configured.View().sorting.active,
            "navigation refresh fixture should activate sample-name sorting");
        Require(
            configured.FlushStateCaches(),
            "navigation refresh fixture should persist its workflow caches");
    }

    spectiary::SampleLabelingStateCacheLoadResult cache =
        spectiary::LoadSampleLabelingStateCache(spectiary::RuntimePaths{}, fixture.labeling_cache);
    auto source = cache.cache.sources.find(fixture.context.identity.id);
    Require(
        source != cache.cache.sources.end(),
        "navigation refresh fixture should reload its source state");
    auto draft = std::find_if(
        source->second.tasks.begin(),
        source->second.tasks.end(),
        [&fixture](const spectiary::SampleLabelingTask& task) {
            return task.task_id == fixture.draft_task_id;
        });
    Require(
        draft != source->second.tasks.end() && draft->persistence.output_path,
        "navigation refresh fixture should find its formalized draft");
    const spectiary::SampleLabelingAsdfReadResult draft_document =
        spectiary::ReadSampleLabelingAsdfDocument(
            fixture.draft_output_path);
    Require(
        draft_document.succeeded(),
        draft_document.error.message.empty()
            ? "navigation refresh fixture should reopen its canonical draft values"
            : draft_document.error.message);
    auto projected_draft = spectiary::ProjectSampleLabelingDocumentTask(
        *draft_document.document, *draft);
    Require(projected_draft.has_value(),
        "navigation refresh fixture should project complete canonical values");
    *draft = std::move(*projected_draft);

    spectiary::RebuildSampleLabelingTaskStatistics(*draft);
    draft->persistence.output_path.reset();
    draft->persistence.output_format =
        spectiary::SampleLabelingOutputArtifactFormat::None;
    Require(
        spectiary::SaveSampleLabelingStateCache(spectiary::RuntimePaths{},
            fixture.labeling_cache,
            cache.cache),
        "navigation refresh fixture should restore the draft-only projection");

    Require(
        fixture.context.manifest.annotations.empty(),
        "navigation refresh fixture should retain a pure recovery draft with no pre-existing annotation attachment");
    return fixture;
}

void FormalizeTemporaryDraftFromAnotherInstance(
    const TemporaryDraftNavigationRefreshFixture& fixture)
{
    spectiary::SampleLabelingController formalizer(fixture.labeling_cache);
    ActivateCanonicalFixtureSource(
        formalizer,
        fixture.source_path,
        fixture.context);
    Require(
        formalizer.ActivateTask(fixture.draft_task_id).accepted &&
            formalizer
                .SaveActiveTemporaryTaskToOutput(
                    fixture.draft_output_path)
                .output_saved &&
            formalizer.DeactivateActiveTask().state_saved,
        "the external instance should formalize the recovery draft");
}

void AssertTemporaryDraftProjectionRefreshPreservesActiveUndo(
    bool delete_temporary_draft)
{
    const TemporaryDraftNavigationRefreshFixture fixture =
        SeedTemporaryDraftNavigationRefreshFixture(
            delete_temporary_draft
                ? "_coordinator_delete_formalized_draft"
                : "_coordinator_recover_formalized_draft");
    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(fixture.source_path, 3, 0);
    spectiary::SampleWorkflowCoordinator coordinator(
        fixture.navigation_cache,
        fixture.labeling_cache,
        fixture.workflow_cache);
    spectiary::PreparedSampleWorkflowState prepared =
        PrepareWorkflow(
            snapshot,
            fixture.context,
            0,
            fixture.labeling_cache,
            fixture.workflow_cache);
    Require(
        coordinator
                .SyncPreparedActiveSource(
                    std::string{"source"},
                    snapshot,
                    fixture.context,
                    std::move(prepared))
                .action.workflow_changed,
        "coordinator recovery fixture should open the paused-draft projection");
    const spectiary::SourceCollectionFilterView initial_filter =
        coordinator.BuildFilterView(snapshot);
    const spectiary::SourceCollectionSampleSortingView initial_sorting =
        coordinator.BuildSortingView(snapshot);
    Require(
        coordinator.LabelingView(snapshot).has_active_task &&
            coordinator.LabelingView(snapshot).task_id ==
                fixture.formal_task_id &&
            !coordinator.LabelingView(snapshot).active_task_is_temporary,
        "the unrelated formal task should remain active");
    (void)initial_filter;
    (void)initial_sorting;
    Require(
        coordinator.NavigationView(snapshot).current_index.has_value(),
        "the paused-draft projection should retain a current sample for active editing");
    const int active_code_before_edit =
        coordinator.LabelingView(snapshot).current_code;
    Require(
        coordinator
                .Apply(
                    spectiary::ActiveSampleWorkflowIntent::UpsertActiveLabel(
                        spectiary::SampleLabelDefinition{7, "seven", 's'}),
                    snapshot)
                .changed,
        "the active formal task should accept the undo test label");
    const spectiary::SampleWorkflowTransitionOutcome edit_result =
        coordinator.Apply(
            spectiary::ActiveSampleWorkflowIntent::AssignActiveLabelToCurrentSample(7),
            snapshot);
    Require(
        edit_result.label_write && edit_result.label_write->write.changed,
        "the active formal task should have undoable local editing");

    FormalizeTemporaryDraftFromAnotherInstance(fixture);
    const spectiary::SampleWorkflowTransitionOutcome result =
        coordinator.Apply(
            delete_temporary_draft
                ? spectiary::ActiveSampleWorkflowIntent::DeleteTemporaryLabelingTask(
                      fixture.context.identity.id,
                      fixture.draft_task_id)
                : spectiary::ActiveSampleWorkflowIntent::RecoverTemporaryLabelingTask(
                      fixture.context.identity.id,
                      fixture.draft_task_id),
            snapshot);
    Require(
        result.labeling_issue ==
            spectiary::SampleLabelingOperationResult::Issue::EditTargetChanged,
        "external formalization should reject the stale recovery command");
    Require(
        result.action.navigation_inputs_changed &&
            result.action.annotation_roster_changed &&
            !result.action.workflow_changed,
        "task projection convergence should reconcile filter, sort, and navigation inputs");

    const spectiary::SourceCollectionFilterView refreshed_filter =
        coordinator.BuildFilterView(snapshot);
    const spectiary::SourceCollectionSampleSortingView refreshed_sorting =
        coordinator.BuildSortingView(snapshot);
    const spectiary::SourceCollectionNavigationView refreshed_navigation =
        coordinator.NavigationView(snapshot);
    const auto restored_annotation = std::find_if(
        refreshed_navigation.current_annotations.begin(),
        refreshed_navigation.current_annotations.end(),
        [&fixture](
            const spectiary::SourceCollectionAnnotationValueView&
                annotation) {
            return annotation.path ==
                fixture.draft_output_path;
        });
    Require(
        restored_annotation !=
                refreshed_navigation.current_annotations.end() &&
            restored_annotation->relationship ==
                spectiary::SampleAnnotationWorkflowRelationship::
                    LocalLabelingTask,
        "task projection convergence must attach the canonical owner created by another instance");
    const std::string refreshed_source_id =
        "labeling:" + fixture.draft_task_id;
    Require(
        std::any_of(
            refreshed_filter.sources.begin(),
            refreshed_filter.sources.end(),
            [&refreshed_source_id](
                const spectiary::SourceCollectionFilterSourceView& source) {
                return source.id ==
                    refreshed_source_id;
            }),
        "the externally formalized canonical owner must remain available in the labeling selector and filter projection");
    Require(
        refreshed_filter.evaluation.included_count == 1,
        "task projection convergence should publish the latest filter values");
    Require(
        refreshed_sorting.active,
        "task projection convergence should retain the selected sorting input");
    Require(
        refreshed_navigation.sequence_count == 1,
        "task projection convergence should publish the final sequence count");
    Require(
        result.snapshot_index_to_load == 1,
        "task projection convergence should request the filtered sample row");
    const spectiary::SampleWorkflowTransitionOutcome undone =
        coordinator.Apply(
            spectiary::ActiveSampleWorkflowIntent::UndoLastLabelWrite(),
            snapshot);
    Require(
        undone.label_write && undone.label_write->write.changed &&
            coordinator.LabelingView(snapshot).current_code ==
                active_code_before_edit,
        "recovering or deleting an unrelated draft must preserve active-task undo history");
}

void TestRecoveringFormalizedTemporaryDraftReconcilesNavigation()
{
    AssertTemporaryDraftProjectionRefreshPreservesActiveUndo(false);
}

void TestDeletingFormalizedTemporaryDraftReconcilesNavigation()
{
    AssertTemporaryDraftProjectionRefreshPreservesActiveUndo(true);
}

void TestPreparedLeaseHandoffRebuildsLatestLabelingProjections()
{
    const LabelingProjectionHandoffFixture fixture =
        SeedLabelingProjectionHandoffFixture(
            "_prepared_labeling_handoff",
            true);
    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(fixture.source_path, 3, 1);
    spectiary::PreparedSampleWorkflowState stale_workflow =
        PrepareWorkflow(
            snapshot,
            fixture.context,
            1,
            fixture.labeling_cache,
            fixture.workflow_cache);

    WriteLatestLabelingProjection(fixture);

    spectiary::SourceCollectionSession session(
        {},
        fixture.navigation_cache,
        fixture.labeling_cache,
        fixture.workflow_cache);
    Require(
        session.OpenPreparedSource(
                fixture.source_path,
                1,
                snapshot,
                fixture.context,
                std::move(stale_workflow))
            .loaded,
        "prepared labeling handoff should commit the source");
    const spectiary::SourceCollectionSessionView view =
        session.View();
    Require(
        view.labeling.current_code == 1,
        "lease handoff should expose the latest task values");
    Require(
        view.filter.evaluation.included_count == 3 &&
            view.navigation.sequence_count == 3 &&
            view.navigation.current_sequence_position == 1,
        "prepared filter, sorting, and navigation projections must be rebuilt from the lease-refreshed task");
    const spectiary::SourceCollectionSessionResult next =
        Submit(
            session,
            MoveSampleNavigation(
                spectiary::SampleNavigationRequest::Next()));
    Require(
        next.follow_up_spectrum_index == 2,
        "prepared handoff navigation must follow the latest labeling sort order");
}

void TestRejectedStaleTaskActivationReconcilesNavigation()
{
    const LabelingProjectionHandoffFixture fixture =
        SeedLabelingProjectionHandoffFixture(
            "_deleted_labeling_handoff",
            false);
    spectiary::SampleLabelingStateCacheLoadResult cache =
        spectiary::LoadSampleLabelingStateCache(spectiary::RuntimePaths{},
            fixture.labeling_cache);
    auto source = cache.cache.sources.find(
        fixture.context.identity.id);
    Require(
        source != cache.cache.sources.end(),
        "stale deletion fixture should load its source");
    source->second.active_task_id.reset();
    Require(
        spectiary::SaveSampleLabelingStateCache(spectiary::RuntimePaths{},
            fixture.labeling_cache,
            cache.cache),
        "stale deletion fixture should leave the task inactive");

    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(fixture.source_path, 3, 1);
    spectiary::PreparedSampleWorkflowState stale_workflow =
        PrepareWorkflow(
            snapshot,
            fixture.context,
            1,
            fixture.labeling_cache,
            fixture.workflow_cache);
    spectiary::SourceCollectionSession stale(
        {},
        fixture.navigation_cache,
        fixture.labeling_cache,
        fixture.workflow_cache);
    Require(
        stale.OpenPreparedSource(
                 fixture.source_path,
                 1,
                 snapshot,
                 fixture.context,
                 std::move(stale_workflow))
            .loaded,
        "stale deletion fixture should open its old task projection");
    Require(
        stale.View().filter.evaluation.included_count == 1 &&
            stale.View().navigation.sequence_count == 1,
        "stale deletion fixture should preheat the old filtered sequence");

    spectiary::SampleLabelingController deleting(
        fixture.labeling_cache);
    ActivateCanonicalFixtureSource(
        deleting,
        fixture.source_path,
        fixture.context);
    Require(
        deleting.ActivateTask(fixture.task_id)
            .accepted,
        "deleting editor should acquire the inactive task");
    Require(
        deleting.DeleteActiveTask().state_saved,
        "deleting editor should commit the task tombstone");

    const spectiary::SourceCollectionSessionResult rejected =
        Submit(
            stale,
            ActivateLabelingTaskFromAnnotation(
                fixture.output_path));
    Require(
        rejected.labeling_issue ==
            spectiary::SampleLabelingOperationResult::Issue::
                EditTargetChanged,
        "stale activation should report that the deleted target changed");
    const spectiary::SourceCollectionSessionView reconciled =
        stale.View();
    Require(
        rejected.action.navigation_inputs_changed &&
            reconciled.filter.evaluation.included_count == 3 &&
            reconciled.navigation.sequence_count == 3 &&
            reconciled.navigation.current_sequence_position == 1,
        "rejected stale activation must remove the ghost projection and rebuild navigation");
}

void TestReloadedFormalOwnerDoesNotReplayPendingValues()
{
    const LabelingProjectionHandoffFixture fixture =
        SeedLabelingProjectionHandoffFixture(
            "_retry_labeling_handoff",
            false);
    spectiary::SampleLabelingStateCacheLoadResult pending_cache =
        spectiary::LoadSampleLabelingStateCache(spectiary::RuntimePaths{},
            fixture.labeling_cache);
    auto source = pending_cache.cache.sources.find(
        fixture.context.identity.id);
    Require(
        source != pending_cache.cache.sources.end() &&
            source->second.tasks.size() == 1,
        "retry projection fixture should load its task");
    source->second.active_task_id.reset();
    source->second.tasks[0].persistence.pending_sample_indices.insert(0);
    source->second.tasks[0].values.SetPendingValue(0, -1);
    source->second.tasks[0].persistence.save_state.kind =
        spectiary::SampleLabelSaveStateKind::Pending;
    source->second.tasks[0].persistence.save_state.pending_count = 1;
    Require(
        spectiary::SaveSampleLabelingStateCache(spectiary::RuntimePaths{},
            fixture.labeling_cache,
            pending_cache.cache),
        "retry projection fixture should persist its pending task");

    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(fixture.source_path, 3, 1);
    spectiary::PreparedSampleWorkflowState workflow =
        PrepareWorkflow(
            snapshot,
            fixture.context,
            1,
            fixture.labeling_cache,
            fixture.workflow_cache);
    spectiary::SourceCollectionSession session(
        {},
        fixture.navigation_cache,
        fixture.labeling_cache,
        fixture.workflow_cache);
    Require(
        session.OpenPreparedSource(
                fixture.source_path,
                1,
                snapshot,
                fixture.context,
                std::move(workflow))
            .loaded,
        "retry projection fixture should commit its stale source");
    Require(
        session.View().filter.evaluation.included_count == 1 &&
            session.View().navigation.current_sequence_position == 0,
        "retry projection fixture should preheat the stale derived state");

    WriteLatestLabelingProjection(fixture);
    const auto latest_bytes = ReadBinaryFile(fixture.output_path);
    Require(!session.NextMaintenanceDeadline().has_value(),
        "ordinary registration must never restore a formal pending retry");
    (void)session.RunMaintenance(spectiary::LocalUserStateSaveScheduler::Clock::now());
    Require(ReadBinaryFile(fixture.output_path) == latest_bytes,
        "restarting must not replay application-managed values over the latest ASDF");
}

}  // namespace

void RunAllTests()
{
    TestNavigationReloadsSnapshotAndRemembersLabelingPosition();
    TestTaskRenameIntentPreservesExactNameAndRejectsStaleTarget();
    TestAssigningLabelAutoAdvancesInsideSession();
    TestLabelAutoAdvanceExposesNonAdjacentFilteredTransition();
    TestLabelUndoRestoresValueAndAutoAdvancePosition();
    TestLabelUndoRestoresExistingValueAfterOverwriteAndClear();
    TestLabelUndoRestoresSampleOutsideActiveSampleNavigationSequence();
    TestLabelUndoHistoryInvalidatesWithTaskAndLabelDefinitions();
    TestLabelUndoHistoryIsBoundedToTwoHundredFiftySixWrites();
    TestNoOpLabelUpsertKeepsLabelUndoHistory();
    TestSavingCanonicalOwnerKeepsLabelUndoHistory();
    TestAnnotationFilterSelectionAppliesToNavigation();
    TestResolvedSequencePositionTracksFinalNavigationSequence();
    TestLocalLabelingAnnotationCanBeSampleFilterSource();
    TestRemovingLabelSelectedBySampleFilterReloadsReconciledSnapshot();
    TestRemovingLabelPrunesItsSampleFilterValue();
    TestChangingUsedLabelCodeMigratesValuesAndSampleFilter();
    TestLabelingViewCodeConflictIncludesUndefinedSampleValues();
    TestResumeLocateRespectsActiveFilterSequence();
    TestSourceOrderNavigationViewDoesNotMaterializeSequenceRows();
    TestRememberedPositionResumableTracksActiveSequence();
    TestSampleSortingIntentAppliesNavigationSequence();
    TestSampleSortingSourcesRequireExplicitAddition();
    TestSampleWorkflowStateRestoresFiltersAndSorting();
    TestAnnotationDisplayNameCustomizesWorkflowSurfacesAndPersists();
    TestAnnotationSortingSourcesRequireComparablePlainValues();
    TestSourceSessionRestoresAnnotationSortingState();
    TestEmptyFilterSequenceDoesNotLoadFallbackSnapshot();
    TestDeactivatingLabelingTaskKeepsAnnotationFilter();
    TestTemporaryLabelingTaskUsesDefaultNameAndResumes();
    TestLabelExportsIgnoreNavigationSequenceAndPreserveCanonicalState();
    TestFolderSessionExportsDefaultCsvAndNpyOverrideArtifacts();
    TestAttachedCsvPreservesUnlabeledSemantics();
    TestExportingLabelValuesDoesNotFormalizeOrAttachTask();
    TestTemporaryDraftRecoveryViewRestoresAfterRestart();
    TestTemporaryDraftRecoveryViewReportsLeaseConflict();
    TestTemporaryDraftRecoveryViewReportsUntrustedStaleDrafts();
    TestTemporaryDraftRecoveryViewReportsFormalTaskIdentityConflict();
    TestLabelingViewAndIntentClearValuesWhenRemovingUsedLabel();
    TestDiscardingTemporaryLabelingTaskAllowsFreshStart();
    TestSavingTemporaryTaskCreatesNamedAnnotationAndAllowsFreshTemporaryTask();
    TestFormalizedCanonicalAttachmentPersistsAcrossRestart();
    TestCanonicalOwnerRepairsMissingPreparedAttachmentAfterCrash();
    TestCanonicalRegistrationRestoreChecksReservedStorage();
    TestUnavailableCanonicalOwnersRemainVisibleAfterRestart();
    TestFailedFirstOutputSaveKeepsRecoverableTemporaryTask();
    TestMalformedExistingAsdfKeepsRecoverableTemporaryTask();
    TestActivatingExternalAnnotationResultCreatesLocalLabelingTask();
    TestStandaloneCanonicalAsdfAnnotationAdoptsExactTask();
    TestStandaloneCanonicalAsdfAdoptionReopensCurrentGeneration();
    TestCanonicalAsdfAnnotationActivatesPersistedOwner();
    TestCanonicalAsdfDeactivationRetainsHydratedAttachmentGeneration();
    TestInactiveCanonicalOwnerRepairsAttachmentProjection();
    TestAnnotationActivationRequiresCurrentTaskToBeClosed();
    TestRejectedAnnotationImportRefreshesDiagnosticProjection();
    TestRejectedAnnotationSwitchKeepsCurrentEditingTask();
    TestActivatingPlainIntegerAnnotationCreatesMetadataSidecar();
    TestLegacyImportCanSaveAsCanonicalAsdf();
    TestImportedDraftDoesNotRequireMetadataSidecar();
    TestAnnotationLocalMatchRequiresSidecarTaskId();
    TestSwitchingSourceCollectionRestoresWorkflowAndClearsFilters();
    TestSameIdentitySourceActivationReplacesAutoAdvanceFeedback();
    TestNavigationViewSeparatesSampleNameFromDisplayName();
    TestNavigationViewExposesSourceProvidedSampleName();
    TestRemovingActiveSourceActivatesNextSourceWorkflow();
    TestSourceSessionRestoresSourcesAndActiveIndex();
    TestSourceSessionStateCacheRoundTrip();
    TestSourceSessionStateCacheIgnoresCorruptJson();
    TestMissingPersistenceCachesAreHealthyDefaults();
    TestSourceSessionStateCacheIgnoresUnsupportedSchema();
    TestSessionAggregatesCacheLoadWarningsWithoutBlockingSourceOpen();
    TestDirectPreparedWorkflowAdoptsCacheHealthAndNavigationBase();
    TestStalePreparedCacheWarningsDoNotReappearAfterRepair();
    TestSourceSessionSkipsMissingSourcePathsOnRestore();
    TestSourceSessionRestoresAtMostThirtyTwoSources();
    TestDeferredSourceSessionRestoreDoesNotInvokeLoaderOnConstruction();
    TestSupersededDeferredRestorePreservesPersistedSourceIntents();
    TestForgettingUnavailableDeferredSourcePersistsDuringRestore();
    TestPreparedRestoreDoesNotExposeSnapshotForAReconciledDifferentRow();
    TestReturningToPresentedSampleClearsTentativeTransition();
    TestDeferredTransitionUsesPresentedSampleAsSource();
    TestEmptyPreparedReconciliationClearsTentativeTransition();
    TestDeferredLabelAutoAdvanceUsesTheVisibleLabeledSampleAsItsBase();
    TestManualNavigationTakesOverMatchingAutoAdvanceTarget();
    TestPendingNavigationCancellationClearsTentativeTransition();
    TestDeferredLabelAutoAdvanceUpgradesMatchingFilterPendingPositionSemantics();
    TestDeferredLabelAutoAdvancePreservesNewLocalFilterFollowUp();
    TestDeferredLabelUndoClearsSupersededLocalFilterFollowUp();
    TestDeferredFilterRetargetsPendingNavigationWithoutChangingCommittedPresentation();
    TestExplicitCommittedSequencePositionCancelsPendingNavigation();
    TestNonActiveRemovalAndCurrentReselectionPreserveDeferredNavigation();
    TestDeferredNavigationKeepsPresentedSampleUntilPreparedSnapshotCommits();
    TestSwitchingAwayCancelsSourceBoundDeferredNavigation();
    TestPreparedPlanReconciliationKeepsPreviousCompletePresentationUntilFinalRow();
    TestPreparedPlanPreservesNewerLiveWorkflowWhenPendingTargetIsUnchanged();
    TestLiveWorkflowContextReconciliationKeepsOldSnapshotWhenTargetChanges();
    TestSameIdentityPreparedReloadPreservesLiveWorkflowAndCurrentRow();
    TestPreparedCacheSnapshotPreventsUiCacheReload();
    TestKnownSourceSyncReusesTheLabelingSourceGeneration();
    TestPreparedProjectionsMoveIntoTheSessionView();
    TestSessionOwnsStableViewInvalidationAndRetirement();
    TestRemovingInactiveSourceInvalidatesTheSessionView();
    TestSourceSelectionSupersessionRequiresAnActualActivationChange();
    TestRemovedPreparedReuseTargetIsRejectedWithoutMutatingTheSession();
    TestReactivatedFilteredSourceQueuesFreshWorkWithoutDroppingCommittedSnapshot();
    TestSwitchingPreparedSourceReusesItsInMemoryContext();
    TestPreparedSnapshotsBecomeBoundedRawRowResidency();
    TestPreparedOpenReturnsResidentInvalidationForBackgroundRetirement();
    TestResidentSnapshotByteCapEvictsBeforeCountCap();
    TestFolderListingGenerationFlowsIntoSubsequentLoadHint();
    TestSourceSessionFlushFailureKeepsDirtyState();
    TestPreparedLeaseHandoffRebuildsLatestLabelingProjections();
    TestRejectedStaleTaskActivationReconcilesNavigation();
    TestReloadedFormalOwnerDoesNotReplayPendingValues();
    TestRecoveringFormalizedTemporaryDraftReconcilesNavigation();
    TestDeletingFormalizedTemporaryDraftReconcilesNavigation();
}

int main()
{
    try {
        const spectiary::test_support::TemporaryDirectory temporary;
        test_root = temporary.path();
        TestCanonicalSaveRejectsReservedStorage();
        RunAllTests();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
