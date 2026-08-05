#include "domain/source_collection_manifest.h"
#include "ui/source_collection_preparation_internal.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

namespace {

using namespace std::chrono_literals;

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::filesystem::path UniqueTempPath(std::string_view suffix)
{
    static std::atomic_uint64_t next_id = 1;
    return std::filesystem::temp_directory_path() /
           ("specforge_source_preparation_" +
            std::to_string(next_id.fetch_add(1)) +
            std::string(suffix));
}

void WriteFixture(
    const std::filesystem::path& path,
    std::string_view contents = "fixture")
{
    std::ofstream stream(
        path,
        std::ios::binary | std::ios::trunc);
    Require(
        stream.good(),
        "could not create source preparation fixture");
    stream << contents;
}

void TestPartialWorkflowCachePathsAreRejected()
{
    specforge::SourceCollectionPreparationAdapters adapters;
    adapters.workflow_cache_paths.labeling_state_cache_path =
        "labeling-state.json";

    bool rejected = false;
    try {
        (void)specforge::SourceCollectionPreparation(
            std::move(adapters));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }

    Require(
        rejected,
        "partial workflow cache paths must not mix explicit "
        "and process-default state roots");
}

specforge::SpectrumSnapshotHandle MakeSnapshot(
    const std::filesystem::path& path,
    std::size_t spectrum_index = 0,
    std::size_t spectrum_count = 3)
{
    auto snapshot =
        std::make_shared<specforge::SpectrumSnapshot>();
    snapshot->source.id = "fixture";
    snapshot->source.display_name = "fixture";
    snapshot->source.path = path;
    snapshot->collection.spectrum_count = spectrum_count;
    snapshot->collection.current_index = spectrum_index;
    snapshot->collection.can_move_previous =
        spectrum_index > 0;
    snapshot->collection.can_move_next =
        spectrum_index + 1 < spectrum_count;
    snapshot->capabilities.can_plot_current_spectrum = true;
    snapshot->capabilities.can_switch_spectrum = true;
    return snapshot;
}

class MutableDirectoryChangeGeneration final
    : public specforge::DirectoryChangeGeneration {
public:
    [[nodiscard]] bool IsCurrent() const noexcept override
    {
        return current_.load(std::memory_order_relaxed);
    }

    void Invalidate() noexcept
    {
        current_.store(false, std::memory_order_relaxed);
    }

private:
    void Close() noexcept override
    {
        Invalidate();
    }

    std::atomic_bool current_ = true;
};

specforge::SourceCollectionPreparationAdapters Adapters(
    specforge::SourceCollectionPreparationAdapters::SnapshotLoader
        loader)
{
    specforge::SourceCollectionPreparationAdapters adapters;
    adapters.snapshot_loader = std::move(loader);
    adapters.workflow_cache_loader =
        [](const auto&,
           const std::function<void()>& checkpoint) {
            checkpoint();
            return specforge::
                SampleWorkflowPreparationCacheBundle{};
        };
    adapters.workflow_cache_paths = {{}, {}};
    return adapters;
}

specforge::PreparedSourceCollection Prepare(
    specforge::SourceCollectionPreparation& preparation,
    std::uint64_t task_id,
    const specforge::SourceCollectionLoadRequest& request)
{
    return preparation.Prepare(
        task_id,
        request,
        []() {});
}

void TestKnownFileReusesVerifiedContext()
{
    const std::filesystem::path path =
        UniqueTempPath("_known_file.csv");
    WriteFixture(path);
    std::atomic_int context_builds = 0;
    specforge::SourceCollectionPreparationAdapters adapters =
        Adapters(
            [](const auto& source,
               std::size_t index,
               const auto&) {
                return MakeSnapshot(source, index);
            });
    adapters.file_context_builder =
        [&context_builds](
            const specforge::SpectrumSnapshot& snapshot,
            const specforge::SourceCollectionSingleFileState&
                state,
            const auto& checkpoint) {
            ++context_builds;
            return specforge::
                LoadSourceCollectionContextCancelable(
                    snapshot,
                    state,
                    checkpoint);
        };
    specforge::SourceCollectionPreparation preparation(
        std::move(adapters));

    const specforge::PreparedSourceCollection first =
        Prepare(
            preparation,
            1,
            {.path = path, .spectrum_index = 1});
    Require(
        first.context_reuse_proof.has_value(),
        "initial preparation should publish a reuse proof");
    Require(
        context_builds.load() == 1,
        "initial preparation should build one context");

    const specforge::PreparedSourceCollection reused =
        Prepare(
            preparation,
            2,
            {
                .path = path,
                .spectrum_index = 1,
                .reuse =
                    specforge::SourceCollectionReuseCandidate::
                        Verified(
                            *first.context_reuse_proof,
                            17),
            });
    Require(
        std::holds_alternative<
            specforge::PreparedSourceCollectionReuse>(
            reused.payload),
        "verified unchanged file should reuse its context");
    Require(
        context_builds.load() == 1,
        "verified reuse should skip the context builder");
    std::filesystem::remove(path);
}

void TestVerifiedResidentSnapshotSkipsDecode()
{
    const std::filesystem::path path =
        UniqueTempPath("_resident.csv");
    WriteFixture(path);
    std::atomic_int decoder_calls = 0;
    specforge::SourceCollectionPreparation preparation(
        Adapters(
            [&decoder_calls](
                const auto& source,
                std::size_t index,
                const auto&) {
                ++decoder_calls;
                return MakeSnapshot(source, index);
            }));

    const specforge::PreparedSourceCollection first =
        Prepare(
            preparation,
            1,
            {.path = path, .spectrum_index = 1});
    const specforge::SourceCollectionResidentSnapshot resident{
        1,
        first.snapshot,
        *first.context_reuse_proof,
        first.folder_listing_generation,
    };
    const specforge::PreparedSourceCollection reused =
        Prepare(
            preparation,
            2,
            {
                .path = path,
                .spectrum_index = 1,
                .reuse =
                    specforge::SourceCollectionReuseCandidate::
                        Verified(
                            *first.context_reuse_proof,
                            0,
                            first.folder_listing_generation,
                            resident),
            });
    Require(
        decoder_calls.load() == 1,
        "verified resident snapshot should skip decoding");
    Require(
        reused.snapshot_cache_hit &&
            reused.snapshot == first.snapshot,
        "resident reuse should publish the retained snapshot");
    std::filesystem::remove(path);
}

void TestStaleResidentSnapshotFallsBackToDecode()
{
    const std::filesystem::path path =
        UniqueTempPath("_stale_resident.csv");
    WriteFixture(path);
    std::atomic_int decoder_calls = 0;
    specforge::SourceCollectionPreparation preparation(
        Adapters(
            [&decoder_calls](
                const auto& source,
                std::size_t index,
                const auto&) {
                ++decoder_calls;
                return MakeSnapshot(source, index);
            }));
    const specforge::PreparedSourceCollection first =
        Prepare(
            preparation,
            1,
            {.path = path, .spectrum_index = 1});
    const specforge::SourceCollectionResidentSnapshot resident{
        1,
        first.snapshot,
        *first.context_reuse_proof,
        first.folder_listing_generation,
    };
    {
        std::ofstream stream(
            path,
            std::ios::binary | std::ios::app);
        stream << "-changed";
    }

    const specforge::PreparedSourceCollection refreshed =
        Prepare(
            preparation,
            2,
            {
                .path = path,
                .spectrum_index = 1,
                .reuse =
                    specforge::SourceCollectionReuseCandidate::
                        Verified(
                            *first.context_reuse_proof,
                            0,
                            first.folder_listing_generation,
                            resident),
            });
    Require(
        decoder_calls.load() == 2,
        "stale resident state must invoke the decoder");
    Require(
        !refreshed.snapshot_cache_hit,
        "stale resident state must publish a cache miss");
    std::filesystem::remove(path);
}

void TestCompanionAndAnnotationChangesMaterializeContext()
{
    const auto run_case =
        [](const std::filesystem::path& source,
           const std::vector<std::filesystem::path>&
               annotation_paths,
           const std::filesystem::path& dependency) {
            WriteFixture(source);
            WriteFixture(dependency);
            std::atomic_int context_builds = 0;
            specforge::SourceCollectionPreparationAdapters
                adapters = Adapters(
                    [](const auto& path,
                       std::size_t index,
                       const auto&) {
                        return MakeSnapshot(path, index);
                    });
            adapters.file_context_builder =
                [&context_builds](
                    const specforge::SpectrumSnapshot&
                        snapshot,
                    const specforge::
                        SourceCollectionSingleFileState&
                            state,
                    const auto& checkpoint) {
                    ++context_builds;
                    return specforge::
                        LoadSourceCollectionContextCancelable(
                            snapshot,
                            state,
                            checkpoint);
                };
            specforge::SourceCollectionPreparation preparation(
                std::move(adapters));
            const specforge::PreparedSourceCollection first =
                Prepare(
                    preparation,
                    1,
                    {
                        .path = source,
                        .annotation_paths =
                            annotation_paths,
                    });
            context_builds.store(0);
            {
                std::ofstream stream(
                    dependency,
                    std::ios::binary | std::ios::app);
                stream << "-changed";
            }
            const specforge::PreparedSourceCollection changed =
                Prepare(
                    preparation,
                    2,
                    {
                        .path = source,
                        .annotation_paths =
                            annotation_paths,
                        .reuse =
                            specforge::
                                SourceCollectionReuseCandidate::
                                    Verified(
                                        *first
                                             .context_reuse_proof,
                                        0),
                    });
            Require(
                context_builds.load() == 1 &&
                    std::holds_alternative<
                        specforge::
                            PreparedSourceCollectionPlan>(
                        changed.payload),
                "changed dependency must materialize a full context");
            std::filesystem::remove(dependency);
            std::filesystem::remove(source);
        };

    const std::filesystem::path companion_source =
        UniqueTempPath("_companion.npy");
    const std::optional<std::filesystem::path> companion =
        specforge::SourceCollectionCompanionNamePath(
            companion_source);
    Require(
        companion.has_value(),
        "NPY source should expose a companion-name path");
    run_case(
        companion_source,
        {},
        *companion);

    const std::filesystem::path annotation_source =
        UniqueTempPath("_annotation.csv");
    const std::filesystem::path annotation =
        UniqueTempPath("_annotation.npy");
    run_case(
        annotation_source,
        {annotation},
        annotation);
}

void TestReuseCandidateRejectsMismatchedResident()
{
    const std::filesystem::path path =
        UniqueTempPath("_mismatched_resident.csv");
    WriteFixture(path);
    const specforge::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(path);
    const specforge::SourceCollectionSingleFileState state =
        specforge::CaptureSourceCollectionSingleFileState(
            path,
            {});
    specforge::SourceCollectionContextReuseProof resident_proof{
        specforge::BuildSourceCollectionIdentity(
            *snapshot,
            state),
        state,
    };
    specforge::SourceCollectionContextReuseProof requested_proof =
        resident_proof;
    requested_proof.identity.id = "different-context";
    const specforge::SourceCollectionResidentSnapshot resident{
        0,
        snapshot,
        resident_proof,
        {},
    };

    bool rejected = false;
    try {
        (void)specforge::SourceCollectionReuseCandidate::
            Verified(
                std::move(requested_proof),
                0,
                {},
                resident);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    Require(
        rejected,
        "factory must reject a resident snapshot from another proof");
    std::filesystem::remove(path);
}

void TestChangedContextPlanCarriesLiveRevision()
{
    const std::filesystem::path path =
        UniqueTempPath("_revision.csv");
    WriteFixture(path);
    const specforge::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(path, 1);
    const specforge::SourceCollectionSingleFileState state =
        specforge::CaptureSourceCollectionSingleFileState(
            path,
            {});
    specforge::SourceCollectionIdentity previous_identity =
        specforge::BuildSourceCollectionIdentity(
            *snapshot,
            state);
    previous_identity.context_fingerprint =
        "previous-context";
    specforge::SourceCollectionPreparation preparation(
        Adapters(
            [snapshot](
                const auto&,
                std::size_t,
                const auto&) {
                return snapshot;
            }));

    const specforge::PreparedSourceCollection prepared =
        Prepare(
            preparation,
            1,
            {
                .path = path,
                .spectrum_index = 1,
                .reuse =
                    specforge::SourceCollectionReuseCandidate::
                        Known(
                            std::move(previous_identity),
                            42),
            });
    const auto* plan = std::get_if<
        specforge::PreparedSourceCollectionPlan>(
        &prepared.payload);
    Require(
        plan &&
            plan->base_live_workflow_revision ==
                std::optional<std::uint64_t>{42},
        "changed-context plan must retain the captured live revision");
    std::filesystem::remove(path);
}

void TestChangedFileRetriesOneStableGeneration()
{
    const std::filesystem::path path =
        UniqueTempPath("_toctou.csv");
    WriteFixture(path, "first");
    std::atomic_int decoder_calls = 0;
    specforge::SourceCollectionPreparation preparation(
        Adapters(
            [&decoder_calls](
                const auto& source,
                std::size_t index,
                const auto&) {
                if (++decoder_calls == 1) {
                    std::ofstream stream(
                        source,
                        std::ios::binary | std::ios::app);
                    stream << "-changed";
                }
                return MakeSnapshot(source, index);
            }));

    const specforge::PreparedSourceCollection prepared =
        Prepare(
            preparation,
            1,
            {.path = path});
    Require(
        decoder_calls.load() == 2,
        "one TOCTOU change should retry exactly once");
    Require(
        prepared.context_reuse_proof.has_value(),
        "stable retry should publish a verified result");
    Require(
        prepared.context_reuse_proof->dependency_state ==
            specforge::CaptureSourceCollectionSingleFileState(
                path,
                {}),
        "published proof should describe the accepted generation");
    std::filesystem::remove(path);
}

void TestFolderGenerationReuseAndStalePrefetch()
{
    const std::filesystem::path folder =
        UniqueTempPath("_folder");
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "sample.csv");
    auto generation =
        std::make_shared<MutableDirectoryChangeGeneration>();
    std::atomic_int scans = 0;
    specforge::SourceCollectionPreparationAdapters adapters =
        Adapters(
            [](const auto& source,
               std::size_t index,
               const auto&) {
                return MakeSnapshot(source, index, 1);
            });
    adapters.folder_scanner =
        [&scans](const auto& path, const auto& checkpoint) {
            ++scans;
            return specforge::ScanSourceCollectionFolder(
                path,
                {},
                checkpoint);
        };
    adapters.folder_change_generation_factory =
        [generation](const auto&, const auto& checkpoint) {
            checkpoint();
            return generation;
        };
    adapters.folder_snapshot_loader =
        [](const auto& path,
           std::size_t index,
           const auto& listing,
           const auto&) {
            return MakeSnapshot(
                path,
                index,
                listing.spectra.size());
        };
    specforge::SourceCollectionPreparation preparation(
        std::move(adapters));

    const specforge::PreparedSourceCollection first =
        Prepare(
            preparation,
            1,
            {.path = folder});
    Require(
        scans.load() == 1 &&
            first.folder_listing_generation,
        "initial folder preparation should publish one generation");

    const specforge::SourceCollectionReuseCandidate reuse =
        specforge::SourceCollectionReuseCandidate::Verified(
            *first.context_reuse_proof,
            0,
            first.folder_listing_generation);
    const specforge::PreparedSourceCollection second =
        Prepare(
            preparation,
            2,
            {
                .path = folder,
                .reuse = reuse,
            });
    Require(
        scans.load() == 1,
        "current folder generation should avoid rescanning");
    Require(
        std::holds_alternative<
            specforge::PreparedSourceCollectionReuse>(
            second.payload),
        "current folder generation should reuse its context");

    generation->Invalidate();
    bool stale = false;
    try {
        (void)Prepare(
            preparation,
            3,
            {
                .path = folder,
                .reuse = reuse,
                .snapshot_only = true,
            });
    } catch (
        const specforge::SourceCollectionPreparationStale&) {
        stale = true;
    }
    Require(
        stale,
        "snapshot-only preparation must reject a stale folder generation");
    Require(
        scans.load() == 1,
        "stale speculative preparation must not rescan");
    std::filesystem::remove_all(folder);
}

void TestInvalidatedFolderGenerationRefreshes()
{
    const std::filesystem::path folder =
        UniqueTempPath("_invalidated_folder");
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "sample.csv");
    auto initial_generation =
        std::make_shared<MutableDirectoryChangeGeneration>();
    auto replacement_generation =
        std::make_shared<MutableDirectoryChangeGeneration>();
    std::atomic_int generation_requests = 0;
    std::atomic_int scans = 0;
    std::atomic_int context_builds = 0;
    specforge::SourceCollectionPreparationAdapters adapters =
        Adapters(
            [](const auto& source,
               std::size_t index,
               const auto&) {
                return MakeSnapshot(source, index, 1);
            });
    adapters.folder_scanner =
        [&scans](const auto& path, const auto& checkpoint) {
            ++scans;
            return specforge::ScanSourceCollectionFolder(
                path,
                {},
                checkpoint);
        };
    adapters.folder_change_generation_factory =
        [&](const auto&, const auto& checkpoint) {
            checkpoint();
            return generation_requests.fetch_add(1) == 0
                ? initial_generation
                : replacement_generation;
        };
    adapters.folder_snapshot_loader =
        [](const auto& path,
           std::size_t index,
           const auto& listing,
           const auto&) {
            return MakeSnapshot(
                path,
                index,
                listing.spectra.size());
        };
    adapters.folder_context_builder =
        [&context_builds](
            const auto& snapshot,
            const auto& listing,
            const auto& checkpoint) {
            ++context_builds;
            return specforge::
                BuildFolderSourceCollectionContextCancelable(
                    snapshot,
                    listing,
                    checkpoint);
        };
    specforge::SourceCollectionPreparation preparation(
        std::move(adapters));
    const specforge::PreparedSourceCollection first =
        Prepare(
            preparation,
            1,
            {.path = folder});
    initial_generation->Invalidate();

    const specforge::PreparedSourceCollection refreshed =
        Prepare(
            preparation,
            2,
            {
                .path = folder,
                .reuse =
                    specforge::SourceCollectionReuseCandidate::
                        Verified(
                            *first.context_reuse_proof,
                            0,
                            first.folder_listing_generation),
            });
    Require(
        scans.load() == 2 &&
            context_builds.load() == 2,
        "invalidated generation must rescan and rebuild context");
    Require(
        refreshed.folder_listing_generation &&
            refreshed.folder_listing_generation->IsCurrent() &&
            refreshed.folder_listing_generation !=
                first.folder_listing_generation,
        "refresh must publish the replacement generation");
    std::filesystem::remove_all(folder);
}

void TestUnavailableFolderGenerationUsesFallbackScan()
{
    const std::filesystem::path folder =
        UniqueTempPath("_unavailable_generation");
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "sample.csv");
    const specforge::SourceCollectionFolderListing listing =
        specforge::ScanSourceCollectionFolder(folder);
    const auto hint = std::make_shared<
        const specforge::
            SourceCollectionFolderListingGeneration>(
        specforge::SourceCollectionFolderListingGeneration{
            listing,
            {},
        });
    std::atomic_int scans = 0;
    specforge::SourceCollectionPreparationAdapters adapters =
        Adapters(
            [](const auto& source,
               std::size_t index,
               const auto&) {
                return MakeSnapshot(source, index, 1);
            });
    adapters.folder_scanner =
        [&scans](const auto& path, const auto& checkpoint) {
            ++scans;
            return specforge::ScanSourceCollectionFolder(
                path,
                {},
                checkpoint);
        };
    adapters.folder_change_generation_factory =
        [](const auto&, const auto& checkpoint)
            -> specforge::DirectoryChangeGenerationHandle {
            checkpoint();
            return {};
        };
    adapters.folder_snapshot_loader =
        [](const auto& path,
           std::size_t index,
           const auto& current_listing,
           const auto&) {
            return MakeSnapshot(
                path,
                index,
                current_listing.spectra.size());
        };
    specforge::SourceCollectionIdentity unknown_identity;
    unknown_identity.id = "unknown";
    specforge::SourceCollectionPreparation preparation(
        std::move(adapters));
    const specforge::PreparedSourceCollection prepared =
        Prepare(
            preparation,
            1,
            {
                .path = folder,
                .reuse =
                    specforge::SourceCollectionReuseCandidate::
                        Known(
                            std::move(unknown_identity),
                            0,
                            hint),
            });
    Require(
        scans.load() == 1 &&
            prepared.folder_listing_generation,
        "unavailable monitor must use one post-decode fallback scan");
    std::filesystem::remove_all(folder);
}

void TestStaleFolderListingRefreshesBeforeDecode()
{
    const std::filesystem::path folder =
        UniqueTempPath("_stale_listing");
    std::filesystem::create_directory(folder);
    const std::filesystem::path sample =
        folder / "sample.csv";
    WriteFixture(sample);
    auto stale_generation =
        std::make_shared<MutableDirectoryChangeGeneration>();
    const auto stale_hint = std::make_shared<
        const specforge::
            SourceCollectionFolderListingGeneration>(
        specforge::SourceCollectionFolderListingGeneration{
            specforge::ScanSourceCollectionFolder(folder),
            stale_generation,
        });
    {
        std::ofstream stream(
            sample,
            std::ios::binary | std::ios::app);
        stream << "-changed";
    }
    std::atomic_int scans = 0;
    std::atomic_int decodes = 0;
    specforge::SourceCollectionPreparationAdapters adapters =
        Adapters(
            [](const auto& source,
               std::size_t index,
               const auto&) {
                return MakeSnapshot(source, index, 1);
            });
    adapters.folder_scanner =
        [&scans](const auto& path, const auto& checkpoint) {
            ++scans;
            return specforge::ScanSourceCollectionFolder(
                path,
                {},
                checkpoint);
        };
    adapters.folder_change_generation_factory =
        [](const auto&, const auto& checkpoint) {
            checkpoint();
            return std::make_shared<
                MutableDirectoryChangeGeneration>();
        };
    adapters.folder_snapshot_loader =
        [&](const auto& path,
            std::size_t index,
            const auto& listing,
            const auto&) {
            ++decodes;
            Require(
                listing.spectra.front().stat_fingerprint !=
                    stale_hint->listing.spectra.front()
                        .stat_fingerprint,
                "decoder must receive the refreshed listing");
            return MakeSnapshot(
                path,
                index,
                listing.spectra.size());
        };
    specforge::SourceCollectionIdentity unknown_identity;
    unknown_identity.id = "unknown";
    specforge::SourceCollectionPreparation preparation(
        std::move(adapters));
    (void)Prepare(
        preparation,
        1,
        {
            .path = folder,
            .reuse =
                specforge::SourceCollectionReuseCandidate::
                    Known(
                        std::move(unknown_identity),
                        0,
                        stale_hint),
        });
    Require(
        scans.load() == 1 &&
            decodes.load() == 1,
        "stale target must refresh once before its only decode");
    std::filesystem::remove_all(folder);
}

void TestPreferredFolderMemberResolvesAfterFirstLevelScan()
{
    const std::filesystem::path folder =
        UniqueTempPath("_preferred_member");
    std::error_code cleanup_error;
    std::filesystem::remove_all(folder, cleanup_error);
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "first.csv");
    const std::filesystem::path preferred =
        folder / "selected.fits";
    WriteFixture(preferred);
    WriteFixture(folder / "zzz.csv");

    std::atomic_size_t decoded_index =
        std::numeric_limits<std::size_t>::max();
    specforge::SourceCollectionPreparationAdapters adapters =
        Adapters(
            [](const auto& source,
               std::size_t index,
               const auto&) {
                return MakeSnapshot(source, index, 3);
            });
    adapters.folder_snapshot_loader =
        [&decoded_index](
            const auto& path,
            std::size_t index,
            const auto& listing,
            const auto&) {
            decoded_index.store(
                index,
                std::memory_order_relaxed);
            return MakeSnapshot(
                path,
                index,
                listing.spectra.size());
        };
    adapters.folder_scanner =
        [](const auto& path, const auto& checkpoint) {
            const auto listing = specforge::ScanSourceCollectionFolder(
                path,
                {},
                checkpoint);
            return listing;
        };
    adapters.folder_change_generation_factory =
        [](const auto&, const auto& checkpoint) {
            checkpoint();
            return std::make_shared<
                MutableDirectoryChangeGeneration>();
        };
    adapters.folder_context_builder =
        [](const auto& snapshot,
           const auto& listing,
           const auto& checkpoint) {
            checkpoint();
            return specforge::BuildFolderSourceCollectionContext(
                snapshot,
                listing);
        };

    specforge::SourceCollectionPreparation preparation(
        std::move(adapters));
    const specforge::PreparedSourceCollection prepared =
        Prepare(
            preparation,
            1,
            {
                .path = folder,
                .preferred_member_path = preferred,
            });
    Require(
        decoded_index.load(std::memory_order_relaxed) == 1 &&
            prepared.spectrum_index == 1 &&
            prepared.snapshot &&
            prepared.snapshot->collection.current_index == 1,
        "preferred folder member should select its stable first-level listing index");
    std::filesystem::remove_all(folder);
}

void TestPreferredFolderMemberMissingAfterScanFailsWithDiagnostic()
{
    const std::filesystem::path folder =
        UniqueTempPath("_missing_preferred_member");
    std::error_code cleanup_error;
    std::filesystem::remove_all(folder, cleanup_error);
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "first.csv");
    const std::filesystem::path preferred =
        folder / "missing.fits";

    specforge::SourceCollectionPreparationAdapters adapters =
        Adapters(
            [](const auto& source,
               std::size_t index,
               const auto&) {
                return MakeSnapshot(source, index, 1);
            });
    adapters.folder_snapshot_loader =
        [](const auto& path,
           std::size_t index,
           const auto& listing,
           const auto&) {
            return MakeSnapshot(
                path,
                index,
                listing.spectra.size());
        };
    adapters.folder_change_generation_factory =
        [](const auto&, const auto& checkpoint) {
            checkpoint();
            return std::make_shared<
                MutableDirectoryChangeGeneration>();
        };
    adapters.folder_context_builder =
        [](const auto& snapshot,
           const auto& listing,
           const auto& checkpoint) {
            checkpoint();
            return specforge::BuildFolderSourceCollectionContext(
                snapshot,
                listing);
        };

    specforge::SourceCollectionPreparation preparation(
        std::move(adapters));
    bool failed = false;
    std::string diagnostic;
    try {
        (void)Prepare(
            preparation,
            1,
            {
                .path = folder,
                .preferred_member_path = preferred,
            });
    } catch (const std::runtime_error& error) {
        failed = true;
        diagnostic = error.what();
    }

    Require(
        failed &&
            diagnostic.find("requested external source member") !=
                std::string::npos &&
            diagnostic.find("missing.fits") !=
                std::string::npos,
        "a preferred member missing from the first-level scan should fail with its diagnostic");
    std::filesystem::remove_all(folder);
}

void TestUnreadableFolderListingPreservesEnumerationDiagnostic()
{
    const std::filesystem::path folder =
        UniqueTempPath("_unreadable_preferred_member");
    std::error_code cleanup_error;
    std::filesystem::remove_all(folder, cleanup_error);
    std::filesystem::create_directory(folder);
    const std::filesystem::path preferred =
        folder / "selected.fits";

    specforge::SourceCollectionPreparationAdapters adapters =
        Adapters(
            [](const auto& source,
               std::size_t index,
               const auto&) {
                return MakeSnapshot(source, index, 1);
            });
    adapters.folder_scanner =
        [](const auto&, const auto& checkpoint) {
            checkpoint();
            specforge::SourceCollectionFolderListing listing;
            listing.readable = false;
            listing.error_message =
                "Could not enumerate the input folder: injected ACL failure.";
            return listing;
        };
    adapters.folder_change_generation_factory =
        [](const auto&, const auto& checkpoint) {
            checkpoint();
            return std::make_shared<MutableDirectoryChangeGeneration>();
        };

    specforge::SourceCollectionPreparation preparation(
        std::move(adapters));
    bool failed = false;
    std::string diagnostic;
    try {
        (void)Prepare(
            preparation,
            1,
            {
                .path = folder,
                .preferred_member_path = preferred,
            });
    } catch (const std::runtime_error& error) {
        failed = true;
        diagnostic = error.what();
    }

    Require(
        failed &&
            diagnostic.find(
                "Could not enumerate the input folder: injected ACL failure.") !=
                std::string::npos &&
            diagnostic.find("requested external source member") ==
                std::string::npos,
        "an unreadable folder listing should preserve its enumeration diagnostic");
    std::filesystem::remove_all(folder);
}

void TestChangedFolderRetriesOneStableGeneration()
{
    const std::filesystem::path folder =
        UniqueTempPath("_folder_toctou");
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "sample.csv");
    auto initial_generation =
        std::make_shared<MutableDirectoryChangeGeneration>();
    std::atomic_int generation_requests = 0;
    std::atomic_int scans = 0;
    std::atomic_int decodes = 0;
    bool mutate_during_decode = false;
    specforge::SourceCollectionPreparationAdapters adapters =
        Adapters(
            [](const auto& source,
               std::size_t index,
               const auto&) {
                return MakeSnapshot(source, index, 1);
            });
    adapters.folder_scanner =
        [&scans](const auto& path, const auto& checkpoint) {
            ++scans;
            return specforge::ScanSourceCollectionFolder(
                path,
                {},
                checkpoint);
        };
    adapters.folder_change_generation_factory =
        [&](const auto&, const auto& checkpoint) {
            checkpoint();
            if (generation_requests.fetch_add(1) == 0) {
                return initial_generation;
            }
            return std::make_shared<
                MutableDirectoryChangeGeneration>();
        };
    adapters.folder_snapshot_loader =
        [&](const auto& path,
            std::size_t index,
            const auto& listing,
            const auto&) {
            ++decodes;
            if (mutate_during_decode &&
                decodes.load() == 2) {
                WriteFixture(path / "added.csv");
                initial_generation->Invalidate();
            }
            return MakeSnapshot(
                path,
                index,
                listing.spectra.size());
        };
    specforge::SourceCollectionPreparation preparation(
        std::move(adapters));
    const specforge::PreparedSourceCollection first =
        Prepare(
            preparation,
            1,
            {.path = folder});
    mutate_during_decode = true;
    const specforge::PreparedSourceCollection retried =
        Prepare(
            preparation,
            2,
            {
                .path = folder,
                .reuse =
                    specforge::SourceCollectionReuseCandidate::
                        Verified(
                            *first.context_reuse_proof,
                            0,
                            first.folder_listing_generation),
            });
    Require(
        decodes.load() == 3,
        "changed folder should decode exactly one retry");
    Require(
        scans.load() == 2 &&
            retried.folder_listing_generation &&
            retried.folder_listing_generation->listing
                    .spectra.size() == 2,
        "retry must publish the newly verified listing generation");
    std::filesystem::remove_all(folder);
}

void TestPublishedGenerationInvalidatesOnPreparationDestruction()
{
    const std::filesystem::path folder =
        UniqueTempPath("_monitor_shutdown");
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "sample.csv");
    auto registration =
        std::make_shared<MutableDirectoryChangeGeneration>();
    specforge::SourceCollectionFolderListingGenerationHandle
        published;
    {
        specforge::SourceCollectionPreparationAdapters adapters =
            Adapters(
                [](const auto& source,
                   std::size_t index,
                   const auto&) {
                    return MakeSnapshot(source, index, 1);
                });
        adapters
            .folder_change_generation_registration_factory =
            [registration](
                const std::filesystem::path&,
                std::stop_token) {
                return registration;
            };
        adapters.folder_snapshot_loader =
            [](const auto& path,
               std::size_t index,
               const auto& listing,
               const auto&) {
                return MakeSnapshot(
                    path,
                    index,
                    listing.spectra.size());
            };
        specforge::SourceCollectionPreparation preparation(
            std::move(adapters));
        published =
            Prepare(
                preparation,
                1,
                {.path = folder})
                .folder_listing_generation;
        Require(
            published && published->IsCurrent(),
            "published generation should start current");
    }
    Require(
        !published->IsCurrent(),
        "preparation destruction must invalidate published leases");
    std::filesystem::remove_all(folder);
}

void TestCanceledBlockedRegistrationStopsOnDestruction()
{
    const std::filesystem::path folder =
        UniqueTempPath("_registration_cancel");
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "sample.csv");
    std::promise<void> registration_entered_promise;
    std::shared_future<void> registration_entered =
        registration_entered_promise.get_future().share();
    std::promise<void> registration_stopped_promise;
    std::future<void> registration_stopped =
        registration_stopped_promise.get_future();
    std::mutex registration_mutex;
    std::condition_variable_any registration_condition;
    std::atomic_bool canceled = false;
    auto preparation =
        std::make_unique<
            specforge::SourceCollectionPreparation>(
            [&]() {
                specforge::SourceCollectionPreparationAdapters
                    adapters = Adapters(
                        [](const auto& source,
                           std::size_t index,
                           const auto&) {
                            return MakeSnapshot(
                                source,
                                index,
                                1);
                        });
                adapters
                    .folder_change_generation_registration_factory =
                    [&](const std::filesystem::path&,
                        std::stop_token stop_token) {
                        registration_entered_promise.set_value();
                        std::unique_lock lock(
                            registration_mutex);
                        const bool released =
                            registration_condition.wait_for(
                                lock,
                                stop_token,
                                2s,
                                []() {
                                    return false;
                                });
                        Require(
                            !released &&
                                stop_token.stop_requested(),
                            "monitor shutdown should stop blocked registration");
                        registration_stopped_promise.set_value();
                        return std::make_shared<
                            MutableDirectoryChangeGeneration>();
                    };
                adapters.folder_snapshot_loader =
                    [](const auto& path,
                       std::size_t index,
                       const auto& listing,
                       const auto&) {
                        return MakeSnapshot(
                            path,
                            index,
                            listing.spectra.size());
                    };
                return adapters;
            }());
    std::promise<void> canceled_promise;
    std::future<void> canceled_result =
        canceled_promise.get_future();
    std::jthread worker(
        [&] {
            try {
                (void)preparation->Prepare(
                    1,
                    {.path = folder},
                    [&] {
                        if (canceled.load(
                                std::memory_order_relaxed)) {
                            throw specforge::
                                SourceCollectionPreparationCanceled();
                        }
                    });
            } catch (
                const specforge::
                    SourceCollectionPreparationCanceled&) {
                canceled_promise.set_value();
            }
        });
    Require(
        registration_entered.wait_for(2s) ==
            std::future_status::ready,
        "registration should enter its blocking operation");
    canceled.store(true, std::memory_order_relaxed);
    Require(
        canceled_result.wait_for(2s) ==
            std::future_status::ready,
        "cancellation checkpoint should release preparation caller");
    worker.join();
    preparation.reset();
    Require(
        registration_stopped.wait_for(2s) ==
            std::future_status::ready,
        "preparation destruction should stop and join monitor registration");
    std::filesystem::remove_all(folder);
}

}  // namespace

int main()
{
    TestPartialWorkflowCachePathsAreRejected();
    TestKnownFileReusesVerifiedContext();
    TestVerifiedResidentSnapshotSkipsDecode();
    TestStaleResidentSnapshotFallsBackToDecode();
    TestCompanionAndAnnotationChangesMaterializeContext();
    TestReuseCandidateRejectsMismatchedResident();
    TestChangedContextPlanCarriesLiveRevision();
    TestChangedFileRetriesOneStableGeneration();
    TestFolderGenerationReuseAndStalePrefetch();
    TestInvalidatedFolderGenerationRefreshes();
    TestUnavailableFolderGenerationUsesFallbackScan();
    TestStaleFolderListingRefreshesBeforeDecode();
    TestPreferredFolderMemberResolvesAfterFirstLevelScan();
    TestPreferredFolderMemberMissingAfterScanFailsWithDiagnostic();
    TestUnreadableFolderListingPreservesEnumerationDiagnostic();
    TestChangedFolderRetriesOneStableGeneration();
    TestPublishedGenerationInvalidatesOnPreparationDestruction();
    TestCanceledBlockedRegistrationStopsOnDestruction();
    return 0;
}
