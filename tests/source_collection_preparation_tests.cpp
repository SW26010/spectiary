#include "helpers/source_load_test_support.h"
#include "domain/source_collection_manifest.h"
#include "ui/source_collection_load_queue_internal.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
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


// Drives the production queue; profiling observations replace injected step counters.
class LoadingHarness {
public:
    explicit LoadingHarness(specforge::SourceCollectionLoadDependencies adapters)
        : queue_(specforge::MakeSourceCollectionLoadQueueForTesting(std::move(adapters))) {}

    specforge::PreparedSourceCollection Load(
        specforge::SourceCollectionLoadRequest request)
    {
        request.latency_attempt = trace_.Begin(request.spectrum_index, specforge::LoadLatencyClock::now());
        const auto id = queue_.Enqueue(std::move(request));
        auto completion = specforge::test_support::WaitForSourceCompletion(queue_, id);
        if (completion.stale) throw specforge::SourceCollectionPreparationStale();
        if (!completion.prepared) throw std::runtime_error(completion.error_message);
        return std::move(*completion.prepared);
    }
    std::size_t Scans() const {
        std::size_t count = 0;
        for (const auto& attempt : trace_.Reports())
            for (const auto& round : attempt.preparation_rounds)
                count += round.listing_scan_performed;
        return count;
    }
    std::size_t ContextBuilds() const {
        std::size_t count = 0;
        for (const auto& attempt : trace_.Reports())
            for (const auto& round : attempt.preparation_rounds)
                count += round.context_prepared_ns != 0 && !round.context_reused;
        return count;
    }
private:
    specforge::LoadLatencyAttemptLifecycle trace_;
    specforge::SourceCollectionLoadQueue queue_;
};

void TestPartialWorkflowCachePathsAreRejected()
{
    specforge::SourceCollectionLoadDependencies adapters;
    adapters.workflow_cache_paths = {};
    adapters.workflow_cache_paths.labeling_state_cache_path =
        "labeling-state.json";

    bool rejected = false;
    try {
        (void)LoadingHarness(
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

void TestReservedStorageSourceAdmission()
{
    using namespace specforge;
    const auto base = UniqueTempPath("_reserved_paths");
    for (const auto profile : {StorageProfile::Portable, StorageProfile::LocalAppData}) {
        const auto root = base / (profile == StorageProfile::Portable ? "portable" : "local");
        const auto paths = RuntimePathsForDeployment({.storage_profile = profile}, {
            .executable_path = root / "app.exe",
            .local_app_data_user_state_root = root,
        });
        std::atomic<int> decoder_calls = 0;
        SourceCollectionLoadDependencies adapters;
        adapters.workflow_cache_paths.runtime_paths = paths;
        adapters.snapshot_loader = [&](const auto& path, std::size_t index, const auto&) {
            ++decoder_calls;
            return MakeSnapshot(path, index);
        };
        LoadingHarness preparation(std::move(adapters));
        for (const auto* role : {"config", "state", "logs", "unsaved"}) {
            const auto path = root / role / "source.csv";
            std::filesystem::create_directories(path.parent_path());
            WriteFixture(path);
            bool rejected = false;
            try { (void)preparation.Load({.path = path}); }
            catch (const std::runtime_error& error) {
                rejected = std::string(error.what()).find("outside config") != std::string::npos;
            }
            Require(rejected && decoder_calls == 0, "reserved source must fail before decoder access");
        }
        for (const auto& path : {root / "source.csv", root / "my-work/source.csv"}) {
            std::filesystem::create_directories(path.parent_path());
            WriteFixture(path);
            (void)preparation.Load({.path = path});
        }
        Require(decoder_calls == 2, "user files at root and outside reserved children remain loadable");
        bool annotation_rejected = false;
        try { (void)preparation.Load({.path = root / "source.csv", .annotation_paths = {root / "state/task.asdf"}}); }
        catch (const std::runtime_error& error) {
            annotation_rejected = std::string(error.what()).find("outside config") != std::string::npos;
        }
        Require(annotation_rejected && decoder_calls == 2, "restored reserved annotation is rejected before decoding");
    }
    std::filesystem::remove_all(base);
}

void TestReservedStorageCompanionAdmission()
{
    using namespace specforge;
    const auto base = UniqueTempPath("_companion_admission_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    bool symlink_unavailable_reported = false;
    const auto create_alias = [&](const auto& target, const auto& alias) {
        std::error_code error;
        std::filesystem::create_symlink(target, alias, error);
#ifdef _WIN32
        // Windows without Developer Mode / SeCreateSymbolicLinkPrivilege
        // cannot create this fixture; do not require elevated test accounts.
        if (error.value() == 1314) { // ERROR_PRIVILEGE_NOT_HELD
            if (!symlink_unavailable_reported) {
                std::cout << "SKIP: companion symlink cases require Windows symbolic-link privilege\n";
                symlink_unavailable_reported = true;
            }
            return false;
        }
#endif
        Require(!error, "could not create companion alias fixture");
        return true;
    };
    for (const auto profile : {StorageProfile::Portable, StorageProfile::LocalAppData}) {
        const auto root = base / (profile == StorageProfile::Portable ? "portable" : "local");
        const auto paths = RuntimePathsForDeployment({.storage_profile = profile}, {
            .executable_path = root / "app.exe",
            .local_app_data_user_state_root = root,
        });
        std::filesystem::create_directories(root / "my-work");
        const auto source = root / "my-work/source_x.npy";
        WriteFixture(source);
        std::atomic<int> decoder_calls = 0;
        SourceCollectionLoadDependencies adapters;
        adapters.workflow_cache_paths.runtime_paths = paths;
        adapters.snapshot_loader = [&](const auto& path, std::size_t index, const auto&) {
            ++decoder_calls;
            return MakeSnapshot(path, index);
        };
        LoadingHarness preparation(std::move(adapters));
        // Missing companions remain optional.
        (void)preparation.Load({.path = source});
        for (const auto& companion : {*SourceCollectionCompanionNamePath(source),
                 *SourceCollectionCompanionAnnotationPath(source)}) {
            WriteFixture(companion);
            (void)preparation.Load({.path = source});
            std::filesystem::remove(companion);
            for (const auto* role : {"config", "state", "logs", "unsaved"}) {
                const auto target = root / role / "companion.npy";
                std::filesystem::create_directories(target.parent_path());
                WriteFixture(target);
                if (!create_alias(target, companion)) continue;
                const auto calls_before = decoder_calls.load();
                bool rejected = false;
                try { (void)preparation.Load({.path = source}); }
                catch (const std::runtime_error& error) {
                    rejected = std::string(error.what()).find("outside config") != std::string::npos;
                }
                Require(rejected && decoder_calls == calls_before,
                    "reserved companion alias must be rejected before decoder or manifest access");
                std::filesystem::remove(companion);
            }
            const auto allowed = root / "my-work/user-companion.npy";
            WriteFixture(allowed);
            if (!create_alias(allowed, companion)) continue;
            const auto calls_before = decoder_calls.load();
            (void)preparation.Load({.path = source});
            Require(decoder_calls == calls_before + 1,
                "companion aliases outside reserved storage remain admissible");
            std::filesystem::remove(companion);
        }
    }
    std::filesystem::remove_all(base);
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

specforge::SourceCollectionLoadDependencies Adapters(
    specforge::SourceCollectionLoadDependencies::SnapshotLoader
        loader)
{
    specforge::SourceCollectionLoadDependencies adapters;
    adapters.workflow_cache_paths = specforge::test_support::EmptyWorkflowCachePaths();
    adapters.snapshot_loader = std::move(loader);
    return adapters;
}


void TestKnownFileReusesVerifiedContext()
{
    const std::filesystem::path path =
        UniqueTempPath("_known_file.csv");
    WriteFixture(path);

    specforge::SourceCollectionLoadDependencies adapters =
        Adapters(
            [](const auto& source,
               std::size_t index,
               const auto&) {
                return MakeSnapshot(source, index);
            });
    LoadingHarness preparation(
        std::move(adapters));

    const specforge::PreparedSourceCollection first =
        preparation.Load(
            {.path = path, .spectrum_index = 1});
    Require(
        first.context_reuse_proof.has_value(),
        "initial preparation should publish a reuse proof");
    Require(
        preparation.ContextBuilds() == 1,
        "initial preparation should build one context");

    const specforge::PreparedSourceCollection reused =
        preparation.Load(
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
        preparation.ContextBuilds() == 1,
        "verified reuse should skip the context builder");
    std::filesystem::remove(path);
}

void TestVerifiedResidentSnapshotSkipsDecode()
{
    const std::filesystem::path path =
        UniqueTempPath("_resident.csv");
    WriteFixture(path);
    std::atomic_int decoder_calls = 0;
    LoadingHarness preparation(
        Adapters(
            [&decoder_calls](
                const auto& source,
                std::size_t index,
                const auto&) {
                ++decoder_calls;
                return MakeSnapshot(source, index);
            }));

    const specforge::PreparedSourceCollection first =
        preparation.Load(
            {.path = path, .spectrum_index = 1});
    const specforge::SourceCollectionResidentSnapshot resident{
        1,
        first.snapshot,
        *first.context_reuse_proof,
        first.folder_listing_generation,
    };
    const specforge::PreparedSourceCollection reused =
        preparation.Load(
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
    LoadingHarness preparation(
        Adapters(
            [&decoder_calls](
                const auto& source,
                std::size_t index,
                const auto&) {
                ++decoder_calls;
                return MakeSnapshot(source, index);
            }));
    const specforge::PreparedSourceCollection first =
        preparation.Load(
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
        preparation.Load(
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

            specforge::SourceCollectionLoadDependencies
                adapters = Adapters(
                    [](const auto& path,
                       std::size_t index,
                       const auto&) {
                        return MakeSnapshot(path, index);
                    });
            LoadingHarness preparation(
                std::move(adapters));
            const specforge::PreparedSourceCollection first =
                preparation.Load(
            {
                        .path = source,
                        .annotation_paths =
                            annotation_paths,
                    });
            const auto previous_builds = preparation.ContextBuilds();
            {
                std::ofstream stream(
                    dependency,
                    std::ios::binary | std::ios::app);
                stream << "-changed";
            }
            const specforge::PreparedSourceCollection changed =
                preparation.Load(
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
                preparation.ContextBuilds() == previous_builds + 1 &&
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
    LoadingHarness preparation(
        Adapters(
            [snapshot](
                const auto&,
                std::size_t,
                const auto&) {
                return snapshot;
            }));

    const specforge::PreparedSourceCollection prepared =
        preparation.Load(
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
    LoadingHarness preparation(
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
        preparation.Load(
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

    specforge::SourceCollectionLoadDependencies adapters =
        Adapters(
            [](const auto& source,
               std::size_t index,
               const auto&) {
                return MakeSnapshot(source, index, 1);
            });
    adapters.folder_change_generation_registration_factory =
        [generation](const auto&, std::stop_token) {
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
    LoadingHarness preparation(
        std::move(adapters));

    const specforge::PreparedSourceCollection first =
        preparation.Load(
            {.path = folder});
    Require(
        preparation.Scans() == 1 &&
            first.folder_listing_generation,
        "initial folder preparation should publish one generation");

    const specforge::SourceCollectionReuseCandidate reuse =
        specforge::SourceCollectionReuseCandidate::Verified(
            *first.context_reuse_proof,
            0,
            first.folder_listing_generation);
    const specforge::PreparedSourceCollection second =
        preparation.Load(
            {
                .path = folder,
                .reuse = reuse,
            });
    Require(
        preparation.Scans() == 1,
        "current folder generation should avoid rescanning");
    Require(
        std::holds_alternative<
            specforge::PreparedSourceCollectionReuse>(
            second.payload),
        "current folder generation should reuse its context");

    generation->Invalidate();
    bool stale = false;
    try {
        (void)preparation.Load(
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
        preparation.Scans() == 1,
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


    specforge::SourceCollectionLoadDependencies adapters =
        Adapters(
            [](const auto& source,
               std::size_t index,
               const auto&) {
                return MakeSnapshot(source, index, 1);
            });
    adapters.folder_change_generation_registration_factory =
        [&](const auto&, std::stop_token) {
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
    LoadingHarness preparation(
        std::move(adapters));
    const specforge::PreparedSourceCollection first =
        preparation.Load(
            {.path = folder});
    initial_generation->Invalidate();

    const specforge::PreparedSourceCollection refreshed =
        preparation.Load(
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
        preparation.Scans() == 2 &&
            preparation.ContextBuilds() == 2,
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

    specforge::SourceCollectionLoadDependencies adapters =
        Adapters(
            [](const auto& source,
               std::size_t index,
               const auto&) {
                return MakeSnapshot(source, index, 1);
            });
    adapters.folder_change_generation_registration_factory =
        [](const auto&, std::stop_token)
            -> std::shared_ptr<specforge::DirectoryChangeGeneration> {
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
    LoadingHarness preparation(
        std::move(adapters));
    const specforge::PreparedSourceCollection prepared =
        preparation.Load(
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
        preparation.Scans() == 1 &&
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

    std::atomic_int decodes = 0;
    specforge::SourceCollectionLoadDependencies adapters =
        Adapters(
            [](const auto& source,
               std::size_t index,
               const auto&) {
                return MakeSnapshot(source, index, 1);
            });
    adapters.folder_change_generation_registration_factory =
        [](const auto&, std::stop_token) {
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
    LoadingHarness preparation(
        std::move(adapters));
    (void)preparation.Load(
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
        preparation.Scans() == 1 &&
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
    specforge::SourceCollectionLoadDependencies adapters =
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
    adapters.folder_change_generation_registration_factory =
        [](const auto&, std::stop_token) {
            return std::make_shared<
                MutableDirectoryChangeGeneration>();
        };
    LoadingHarness preparation(
        std::move(adapters));
    const specforge::PreparedSourceCollection prepared =
        preparation.Load(
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

    specforge::SourceCollectionLoadDependencies adapters =
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
    adapters.folder_change_generation_registration_factory =
        [](const auto&, std::stop_token) {
            return std::make_shared<
                MutableDirectoryChangeGeneration>();
        };
    LoadingHarness preparation(
        std::move(adapters));
    bool failed = false;
    std::string diagnostic;
    try {
        (void)preparation.Load(
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

    specforge::SourceCollectionLoadDependencies adapters =
        Adapters(
            [](const auto& source,
               std::size_t index,
               const auto&) {
                return MakeSnapshot(source, index, 1);
            });
    adapters.folder_change_generation_registration_factory =
        [&folder](const auto&, std::stop_token) {
            std::filesystem::remove(folder);
            return std::make_shared<MutableDirectoryChangeGeneration>();
        };

    LoadingHarness preparation(
        std::move(adapters));
    bool failed = false;
    std::string diagnostic;
    try {
        (void)preparation.Load(
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
                "Could not enumerate the input folder.") !=
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

    std::atomic_int decodes = 0;
    bool mutate_during_decode = false;
    specforge::SourceCollectionLoadDependencies adapters =
        Adapters(
            [](const auto& source,
               std::size_t index,
               const auto&) {
                return MakeSnapshot(source, index, 1);
            });
    adapters.folder_change_generation_registration_factory =
        [&](const auto&, std::stop_token) {
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
    LoadingHarness preparation(
        std::move(adapters));
    const specforge::PreparedSourceCollection first =
        preparation.Load(
            {.path = folder});
    mutate_during_decode = true;
    const specforge::PreparedSourceCollection retried =
        preparation.Load(
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
        preparation.Scans() == 2 &&
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
        specforge::SourceCollectionLoadDependencies adapters =
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
        LoadingHarness preparation(
            std::move(adapters));
        published =
            preparation.Load(
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
    const auto folder = UniqueTempPath("_registration_cancel");
    std::filesystem::create_directory(folder);
    std::promise<void> entered_promise;
    auto entered = entered_promise.get_future();
    std::promise<void> stopped_promise;
    auto stopped = stopped_promise.get_future();
    std::mutex mutex;
    std::condition_variable_any condition;
    specforge::SourceCollectionLoadDependencies dependencies;
    dependencies.workflow_cache_paths = specforge::test_support::EmptyWorkflowCachePaths();
    dependencies.folder_change_generation_registration_factory =
        [&](const auto&, std::stop_token stop) {
            entered_promise.set_value();
            std::unique_lock lock(mutex);
            condition.wait(lock, stop, [] { return false; });
            stopped_promise.set_value();
            return std::make_shared<MutableDirectoryChangeGeneration>();
        };
    {
        auto queue = specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies));
        (void)queue.Enqueue({.path = folder});
        Require(entered.wait_for(2s) == std::future_status::ready, "registration must start before shutdown");
        // Destruction must cancel the caller and then stop and join registration.
    }
    Require(stopped.wait_for(0s) == std::future_status::ready, "queue destruction must stop the monitor");
    std::filesystem::remove_all(folder);
}

void TestRegistrationWaitTimesOutWithoutPollingAndClosesLateResult()
{
    std::mutex mutex;
    std::condition_variable_any condition;
    bool release = false;
    auto late = std::make_shared<MutableDirectoryChangeGeneration>();
    specforge::DirectoryChangeGenerationMonitor monitor(
        [&](const auto& path, std::stop_token stop) {
            if (path == "blocked") {
                std::unique_lock lock(mutex);
                condition.wait(lock, stop, [&] { return release; });
                return late;
            }
            return std::make_shared<MutableDirectoryChangeGeneration>();
        });
    int checkpoints = 0;
    const auto start = std::chrono::steady_clock::now();
    const auto result = monitor.Begin("blocked", [&] { ++checkpoints; });
    const auto elapsed = std::chrono::steady_clock::now() - start;
    Require(!result, "timed-out registration must use the unavailable fallback");
    Require(elapsed >= 5s && elapsed < 8s, "registration wait must retain its five-second deadline");
    Require(checkpoints == 2, "idle registration must not periodically poll cancellation");
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    Require(monitor.Begin("next", [] {}) != nullptr, "monitor must accept work after late completion");
    Require(!late->IsCurrent(), "abandoned registration must close even when another owner retains it");
}

void TestRegistrationFailureAndPreCanceledRequest()
{
    int calls = 0;
    specforge::DirectoryChangeGenerationMonitor monitor(
        [&](const auto& path, std::stop_token) -> std::shared_ptr<specforge::DirectoryChangeGeneration> {
            ++calls;
            if (path == "throw") {
                throw std::runtime_error("registration failed");
            }
            return {};
        });
    Require(!monitor.Begin("unavailable", [] {}), "registration failure must remain unavailable");
    bool threw = false;
    try {
        (void)monitor.Begin("throw", [] {});
    } catch (const std::runtime_error& error) {
        threw = std::string_view(error.what()) == "registration failed";
    }
    Require(threw, "factory exceptions must reach the caller");
    std::stop_source canceled;
    canceled.request_stop();
    Require(!monitor.Begin("canceled", [] {}, canceled.get_token()), "pre-canceled registration must not publish a generation");
    Require(calls == 2, "pre-canceled registration must not call the factory");
}

void TestCanceledQueuedRegistrationIsSkipped()
{
    std::promise<void> entered_promise;
    auto entered = entered_promise.get_future();
    std::mutex mutex;
    std::condition_variable_any condition;
    bool release = false;
    std::atomic_int unwanted_calls = 0;
    specforge::DirectoryChangeGenerationMonitor monitor(
        [&](const auto& path, std::stop_token stop) {
            if (path == "first") {
                entered_promise.set_value();
                std::unique_lock lock(mutex);
                condition.wait_for(lock, stop, 3s, [&] { return release; });
            } else if (path == "canceled") {
                ++unwanted_calls;
            }
            return std::make_shared<MutableDirectoryChangeGeneration>();
        });
    auto first = std::async(std::launch::async, [&] { return monitor.Begin("first", [] {}); });
    Require(entered.wait_for(2s) == std::future_status::ready, "first registration must occupy the worker");
    std::stop_source canceled;
    auto queued = std::async(std::launch::async, [&] {
        return monitor.Begin("canceled", [] {}, canceled.get_token());
    });
    Require(queued.wait_for(50ms) == std::future_status::timeout, "second registration must wait behind the first");
    canceled.request_stop();
    Require(queued.wait_for(1s) == std::future_status::ready, "queued cancellation must wake without waiting for the active factory");
    Require(!queued.get(), "canceled queued registration must not publish a result");
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    Require(first.get() != nullptr, "canceling queued work must not cancel active registration");
    Require(monitor.Begin("barrier", [] {}) != nullptr, "worker must drain the abandoned request");
    Require(unwanted_calls == 0, "abandoned queued work must never call the factory");
}

void TestNativeGenerationSurvivesPreparationCallerThread()
{
    const auto folder = UniqueTempPath("_native_monitor_lifetime");
    std::filesystem::create_directory(folder);
    specforge::DirectoryChangeGenerationHandle generation;
    {
        specforge::DirectoryChangeGenerationMonitor monitor;
        std::jthread caller([&] { generation = monitor.Begin(folder, [] {}); });
        caller.join();
        Require(generation && generation->IsCurrent(), "native generation must remain current after the requesting thread exits");
        WriteFixture(folder / "added.csv");
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (generation->IsCurrent() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        Require(!generation->IsCurrent(), "native generation must detect a real directory change");
        generation = monitor.Begin(folder, [] {});
        Require(generation && generation->IsCurrent(), "replacement generation must start current");
    }
    Require(!generation->IsCurrent(), "monitor shutdown must invalidate surviving native leases");
    std::filesystem::remove_all(folder);
}

void TestUncooperativeRegistrationBoundsWaitButDelaysShutdown()
{
    std::promise<void> entered_promise;
    auto entered = entered_promise.get_future();
    std::mutex mutex;
    std::condition_variable condition;
    bool release = false;
    std::atomic_int later_calls = 0;
    auto late = std::make_shared<MutableDirectoryChangeGeneration>();
    auto monitor = std::make_unique<specforge::DirectoryChangeGenerationMonitor>(
        [&](const auto& path, std::stop_token) {
            if (path == "uncooperative") {
                entered_promise.set_value();
                std::unique_lock lock(mutex);
                // Deliberately ignore stop_token, like the production Win32 call.
                // A finite escape keeps failed assertions from hanging the suite.
                condition.wait_for(lock, 15s, [&] { return release; });
                return late;
            }
            ++later_calls;
            return std::make_shared<MutableDirectoryChangeGeneration>();
        });
    std::stop_source canceled;
    auto caller = std::async(std::launch::async, [&] {
        return monitor->Begin("uncooperative", [] {}, canceled.get_token());
    });
    Require(entered.wait_for(2s) == std::future_status::ready, "uncooperative registration must start");
    canceled.request_stop();
    Require(caller.wait_for(1s) == std::future_status::ready, "caller cancellation must not depend on native cooperation");
    Require(!caller.get(), "canceled caller must receive no generation");

    const auto start = std::chrono::steady_clock::now();
    Require(!monitor->Begin("queued", [] {}), "later requests must time out behind a stuck registration");
    const auto elapsed = std::chrono::steady_clock::now() - start;
    Require(elapsed >= 5s && elapsed < 8s, "later caller must retain the five-second bound");
    Require(later_calls == 0, "a single registration worker cannot bypass an in-flight call");

    std::promise<void> shutdown_entered_promise;
    auto shutdown_entered = shutdown_entered_promise.get_future();
    auto shutdown = std::async(std::launch::async, [&] {
        shutdown_entered_promise.set_value();
        monitor.reset();
    });
    shutdown_entered.wait();
    const bool shutdown_waited = shutdown.wait_for(100ms) == std::future_status::timeout;
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    Require(shutdown.wait_for(2s) == std::future_status::ready, "shutdown must finish once registration returns");
    shutdown.get();
    Require(shutdown_waited, "documented shutdown limitation must remain explicit");
    Require(!late->IsCurrent(), "late result must close before shutdown completes");
    Require(later_calls == 0, "timed-out queued work must not run during shutdown");
}

void TestProductionQueueLoadsRealSourcesAndReusesGenerations()
{
    const auto folder = UniqueTempPath("_production_queue");
    std::filesystem::create_directory(folder);
    const auto first_path = folder / "a.csv";
    const auto second_path = folder / "b.csv";
    constexpr auto csv = "wav,flux\n5000,1\n5001,2\n5002,3\n";
    WriteFixture(first_path, csv);
    WriteFixture(second_path, csv);
    {
        specforge::SourceCollectionLoadQueue queue(
            specforge::test_support::EmptyWorkflowCachePaths());
        const auto load = [&](specforge::SourceCollectionLoadRequest request) {
            auto completion = specforge::test_support::WaitForSourceCompletion(
                queue, queue.Enqueue(std::move(request)));
            Require(completion.prepared.has_value(), completion.error_message);
            return std::move(*completion.prepared);
        };
        auto file = load({.path = first_path});
        Require(file.snapshot->current_spectrum.point_count == 3 && file.context_reuse_proof,
            "production decoder and preparation must load a real CSV");
        auto reused = load({.path = first_path,
            .reuse = specforge::SourceCollectionReuseCandidate::Verified(*file.context_reuse_proof, 0)});
        Require(std::holds_alternative<specforge::PreparedSourceCollectionReuse>(reused.payload),
            "unchanged real file must reuse its context");
        auto initial = load({.path = folder});
        Require(initial.snapshot->collection.spectrum_count == 2 && initial.folder_listing_generation,
            "production folder loader must discover both real spectra");
        auto warm = load({.path = folder, .spectrum_index = 1,
            .reuse = specforge::SourceCollectionReuseCandidate::Verified(
                *initial.context_reuse_proof, 0, initial.folder_listing_generation)});
        Require(warm.snapshot->collection.current_index == 1 &&
            warm.folder_listing_generation == initial.folder_listing_generation &&
            std::holds_alternative<specforge::PreparedSourceCollectionReuse>(warm.payload),
            "real folder navigation must reuse the unchanged listing and context");
        const auto prefetch_id = queue.EnqueuePrefetch({.path = folder,
            .reuse = specforge::SourceCollectionReuseCandidate::Verified(
                *warm.context_reuse_proof, 0, warm.folder_listing_generation)});
        Require(prefetch_id != 0, "real folder prefetch must be admitted");
        auto prefetch = specforge::test_support::WaitForSourceCompletion(queue, prefetch_id);
        Require(prefetch.prepared && prefetch.prepared->snapshot->collection.current_index == 0,
            "production prefetch must return the requested real spectrum");
        WriteFixture(folder / "c.csv", csv);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (initial.folder_listing_generation->IsCurrent() &&
            std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        Require(!initial.folder_listing_generation->IsCurrent(), "native folder notification must invalidate the cached listing");
        auto refreshed = load({.path = folder,
            .reuse = specforge::SourceCollectionReuseCandidate::Verified(
                *initial.context_reuse_proof, 0, initial.folder_listing_generation)});
        Require(refreshed.snapshot->collection.spectrum_count == 3 &&
            refreshed.folder_listing_generation != initial.folder_listing_generation,
            "real folder refresh must include the newly added spectrum");
        auto external = load({.source_open_request = specforge::SourceOpenRequest{
            .source_path = second_path,
            .origin = specforge::SourceOpenOrigin::ExternalStartup,
            .open_external_source_as_folder = true}});
        Require(external.path == folder && external.spectrum_index == 1,
            "production source-open probing must resolve the external preferred member");
        auto missing = specforge::test_support::WaitForSourceCompletion(
            queue, queue.Enqueue({.path = folder / "missing.csv"}));
        Require(!missing.prepared && !missing.error_message.empty(),
            "production source failures must publish an error instead of a prepared source");
    }
    std::filesystem::remove_all(folder);
}

}  // namespace

int main()
{
    try {
        TestProductionQueueLoadsRealSourcesAndReusesGenerations();
        TestPartialWorkflowCachePathsAreRejected();
        TestReservedStorageSourceAdmission();
        TestKnownFileReusesVerifiedContext();
        TestVerifiedResidentSnapshotSkipsDecode();
        TestStaleResidentSnapshotFallsBackToDecode();
        TestCompanionAndAnnotationChangesMaterializeContext();
        TestReservedStorageCompanionAdmission();
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
        TestRegistrationWaitTimesOutWithoutPollingAndClosesLateResult();
        TestRegistrationFailureAndPreCanceledRequest();
        TestCanceledQueuedRegistrationIsSkipped();
        TestNativeGenerationSurvivesPreparationCallerThread();
        TestUncooperativeRegistrationBoundsWaitButDelaysShutdown();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
